// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationView.h"
#include "codex/middle/ConversationPresentation.h"
#include "codex/ui/UiStyle.h"

#include <QAbstractSlider>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCoreApplication>
#include <QEasingCurve>
#include <QEvent>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionFocusRect>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QTimer>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace codexui::codex::middle {

class ConversationLoadingOverlay final : public QWidget {
public:
  static constexpr int SpinnerDelayMilliseconds = 500;
  static constexpr int SpinnerAnimationMilliseconds = 33;
  static constexpr int SpinnerDiameter = 30;
  static constexpr int SpinnerStrokeWidth = 3;

  explicit ConversationLoadingOverlay(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("conversationStagingOverlay"));
    setAccessibleName(QStringLiteral("Loading conversation"));
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);

    spinnerDelay_.setSingleShot(true);
    spinnerDelay_.setTimerType(Qt::PreciseTimer);
    spinnerDelay_.setInterval(SpinnerDelayMilliseconds);
    connect(&spinnerDelay_, &QTimer::timeout, this, [this] {
      if (!isVisible())
        return;
      spinnerVisible_ = true;
      setProperty("spinnerVisible", true);
      spinnerAnimation_.start();
      setProperty("spinnerAnimationActive", true);
      update(spinnerRect().adjusted(-2, -2, 2, 2).toAlignedRect());
    });

    spinnerAnimation_.setTimerType(Qt::PreciseTimer);
    spinnerAnimation_.setInterval(SpinnerAnimationMilliseconds);
    connect(&spinnerAnimation_, &QTimer::timeout, this, [this] {
      phaseDegrees_ = (phaseDegrees_ - 18 + 360) % 360;
      setProperty("spinnerAnimationTick",
                  property("spinnerAnimationTick").toULongLong() + 1);
      update(spinnerRect().adjusted(-2, -2, 2, 2).toAlignedRect());
    });

    setProperty("spinnerDelayMilliseconds", SpinnerDelayMilliseconds);
    setProperty("spinnerDiameter", SpinnerDiameter);
    setProperty("spinnerStrokeWidth", SpinnerStrokeWidth);
    setProperty("spinnerVisible", false);
    setProperty("spinnerAnimationActive", false);
    setProperty("spinnerAnimationTick", qulonglong{0});
    hide();
  }

  void begin() {
    spinnerDelay_.stop();
    spinnerAnimation_.stop();
    spinnerVisible_ = false;
    phaseDegrees_ = 90;
    setProperty("spinnerVisible", false);
    setProperty("spinnerAnimationActive", false);
    setProperty("spinnerAnimationTick", qulonglong{0});
    show();
    raise();
    update();
    spinnerDelay_.start();
  }

  void finish() {
    spinnerDelay_.stop();
    spinnerAnimation_.stop();
    spinnerVisible_ = false;
    setProperty("spinnerVisible", false);
    setProperty("spinnerAnimationActive", false);
    hide();
  }

protected:
  void paintEvent(QPaintEvent *event) override {
    QPainter painter(this);
    painter.setClipRegion(event->region());
    painter.fillRect(rect(), QColor(QString::fromLatin1(UiStyle::appBackground)));
    if (!spinnerVisible_)
      return;

    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF ring = spinnerRect();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(QString::fromLatin1(UiStyle::divider)),
                        SpinnerStrokeWidth,
                        Qt::SolidLine, Qt::RoundCap));
    painter.drawEllipse(ring);
    painter.setPen(QPen(QColor(QString::fromLatin1(UiStyle::secondary)),
                        SpinnerStrokeWidth,
                        Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(ring, phaseDegrees_ * 16, 105 * 16);
  }

private:
  [[nodiscard]] QRectF spinnerRect() const {
    const int paintedCenterlineDiameter =
        SpinnerDiameter - SpinnerStrokeWidth;
    QRectF ring(0.0, 0.0, paintedCenterlineDiameter,
                paintedCenterlineDiameter);
    ring.moveCenter(QRectF(rect()).center());
    return ring;
  }

  QTimer spinnerDelay_;
  QTimer spinnerAnimation_;
  bool spinnerVisible_ = false;
  int phaseDegrees_ = 90;
};

namespace {

constexpr int CardSpacing = 8;
constexpr int HistoryButtonHeight = 32;
constexpr int NestedCardIndent = 12;
constexpr int NativeScrollLineStep = 20;
constexpr int EstimatedCardHeight = 112;
constexpr int MinimumMaterializationRows = 8;

bool passiveWhenCollapsed(CardKind kind) noexcept {
  return kind == CardKind::AgentActivity || kind == CardKind::Reasoning ||
         kind == CardKind::Plan || kind == CardKind::GenericActivity;
}

bool eligibleForPassivePresentation(const VisibleCardData &card,
                                    bool collapsed) noexcept {
  if (card.kind == CardKind::LocalPrompt)
    return false;
  if (collapsed)
    return true;
  if (card.kind == CardKind::AgentMessage ||
      card.kind == CardKind::AgentActivity ||
      card.kind == CardKind::Reasoning || card.kind == CardKind::Plan ||
      card.kind == CardKind::GenericActivity)
    return true;
  if (card.kind == CardKind::UserMessage) {
    const auto *message = std::get_if<UserMessageData>(&card.payload);
    return !message || message->imagePaths.empty();
  }
  return false;
}

QLabel *makeEmptyLabel(QWidget *parent) {
  auto *label =
      new QLabel(QStringLiteral("Conversation activity appears here."), parent);
  label->setProperty("kind", "muted");
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  return label;
}

void incrementProperty(QObject *object, const char *name) {
  object->setProperty(name, object->property(name).toULongLong() + 1);
}

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

struct PassiveBlock {
  QString text;
  bool markdown = false;
  bool metadata = false;
};

struct PassivePresentation {
  QString title;
  QString status;
  QColor background = QColor(QStringLiteral("#ffffff"));
  QColor border = QColor(QStringLiteral("#d7dee8"));
  QColor titleColor = QColor(QStringLiteral("#1d2633"));
  std::vector<PassiveBlock> blocks;
  int verticalMargin = 10;
};

PassivePresentation passivePresentation(const VisibleCardData &card) {
  PassivePresentation result;
  std::visit(
      [&](const auto &payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, UserMessageData>) {
          result.title = QStringLiteral("You");
          result.background = QColor(QStringLiteral("#eff5fe"));
          result.border = QColor(QStringLiteral("#b7cff9"));
          result.titleColor = QColor(QStringLiteral("#415882"));
          result.blocks.push_back({text(payload.text), true, false});
        } else if constexpr (std::is_same_v<Payload, AgentMessageData>) {
          result.title = QStringLiteral("Codex");
          result.status = payload.finalAnswer ? QStringLiteral("final answer")
                                              : QStringLiteral("update");
          result.background =
              QColor(payload.finalAnswer ? QStringLiteral("#f4f3fd")
                                         : QStringLiteral("#f9f4ea"));
          result.border =
              QColor(payload.finalAnswer ? QStringLiteral("#cec7f6")
                                         : QStringLiteral("#e1cb9d"));
          result.titleColor =
              QColor(payload.finalAnswer ? QStringLiteral("#59507f")
                                         : QStringLiteral("#6b5521"));
          result.verticalMargin = payload.finalAnswer ? 10 : 8;
          result.blocks.push_back({text(payload.text), true, false});
        } else if constexpr (std::is_same_v<Payload, CommandExecutionData>) {
          result.title = QStringLiteral("Command execution");
          result.status = presentation::statusLabel(payload.status);
          result.blocks.push_back({text(payload.command), false, false});
          result.blocks.push_back({text(payload.output), false, false});
        } else if constexpr (std::is_same_v<Payload, AgentActivityData>) {
          result.title = QStringLiteral("Agent activity");
          result.status = presentation::statusLabel(payload.status);
          result.blocks.push_back(
              {presentation::agentMetadata(payload), false, true});
          result.blocks.push_back({text(payload.prompt), false, false});
          result.blocks.push_back({text(payload.resultText), true, false});
        } else if constexpr (std::is_same_v<Payload, ReasoningData>) {
          result.title = QStringLiteral("Reasoning");
          result.blocks.push_back({text(payload.summary), true, false});
        } else if constexpr (std::is_same_v<Payload, FileChangesData>) {
          result.title = QStringLiteral("File changes");
          result.status = presentation::statusLabel(payload.status);
          result.blocks.push_back(
              {presentation::fileChangesText(payload), false, false});
        } else if constexpr (std::is_same_v<Payload, PlanData>) {
          result.title = QStringLiteral("Plan");
          result.blocks.push_back(
              {presentation::planMarkdown(payload), true, false});
        } else if constexpr (std::is_same_v<Payload, ImageGenerationData>) {
          result.title = payload.status.empty() && payload.revisedPrompt.empty()
                             ? QStringLiteral("Image")
                             : QStringLiteral("Generated image");
          result.status = presentation::statusLabel(payload.status);
          result.blocks.push_back({text(payload.revisedPrompt), false, false});
        } else if constexpr (std::is_same_v<Payload, GenericActivityData>) {
          result.title = presentation::genericActivityTitle(payload);
          result.status = presentation::statusLabel(payload.status);
          result.blocks.push_back(
              {presentation::boundedGenericActivityDetail(payload), false,
               true});
        } else if constexpr (std::is_same_v<Payload, LocalPromptData>) {
          result.title = QStringLiteral("You");
          result.blocks.push_back({text(payload.prompt), true, false});
        }
      },
      card.payload);
  std::erase_if(result.blocks,
                [](const PassiveBlock &block) { return block.text.isEmpty(); });
  return result;
}

class ConversationPassiveDelegate final : public QStyledItemDelegate {
public:
  explicit ConversationPassiveDelegate(QObject *parent)
      : QStyledItemDelegate(parent) {}

  QSize sizeHint(const QStyleOptionViewItem &option,
                 const QModelIndex &index) const override {
    return cardSize(option, index, true);
  }

  QSize cardSize(const QStyleOptionViewItem &option, const QModelIndex &index,
                 bool collapsed) const {
    const auto *conversation =
        qobject_cast<const ConversationItemModel *>(index.model());
    const ConversationItemModel::Row *row =
        conversation ? conversation->row(index.row()) : nullptr;
    if (!row)
      return {};
    const PassivePresentation presentation = passivePresentation(row->card);
    int height = 24 + 2 * presentation.verticalMargin;
    if (!collapsed) {
      const int bodyWidth = std::max(1, option.rect.width() - 24);
      bool first = true;
      for (std::size_t block = 0; block < presentation.blocks.size(); ++block) {
        const PassiveBlock &value = presentation.blocks[block];
        QFont font = option.font;
        if (value.metadata)
          font.setPointSizeF(std::max(7.0, font.pointSizeF() - 1.0));
        height += (first ? 6 : 6) +
                  documentHeight(row->stableKey, block, value, bodyWidth, font);
        first = false;
      }
    }
    return {std::max(0, option.rect.width()), std::max(44, height)};
  }

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    paintCard(painter, option, index, true);
  }

  void paintCard(QPainter *painter, const QStyleOptionViewItem &option,
                 const QModelIndex &index, bool collapsed) const {
    const auto *conversation =
        qobject_cast<const ConversationItemModel *>(index.model());
    const ConversationItemModel::Row *row =
        conversation ? conversation->row(index.row()) : nullptr;
    if (!painter || !row)
      return;

    const PassivePresentation presentation = passivePresentation(row->card);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRectF bounds = QRectF(option.rect).adjusted(0.5, 0.5, -0.5, -0.5);
    if (option.viewItemPosition != QStyleOptionViewItem::Beginning) {
      painter->setBrush(presentation.background);
      painter->setPen(QPen(presentation.border, 1.0));
      painter->drawRoundedRect(bounds, 10.0, 10.0);
    }

    QFont titleFont = option.font;
    titleFont.setWeight(QFont::DemiBold);
    painter->setFont(titleFont);
    painter->setPen(presentation.titleColor);
    const int top = option.rect.top() + presentation.verticalMargin;
    const QRect titleRect(option.rect.left() + 12, top,
                          std::max(0, option.rect.width() - 88), 24);
    painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                      option.fontMetrics.elidedText(presentation.title,
                                                    Qt::ElideRight,
                                                    titleRect.width()));

    if (!presentation.status.isEmpty()) {
      QFont statusFont = option.font;
      statusFont.setPointSizeF(std::max(7.0, statusFont.pointSizeF() - 1.0));
      painter->setFont(statusFont);
      painter->setPen(QColor(QStringLiteral("#667085")));
      const QRect statusRect(option.rect.right() - 205, top, 145, 24);
      painter->drawText(statusRect, Qt::AlignRight | Qt::AlignVCenter,
                        option.fontMetrics.elidedText(presentation.status,
                                                      Qt::ElideRight,
                                                      statusRect.width()));
    }

    if (!collapsed) {
      int blockTop = top + 30;
      const int bodyWidth = std::max(1, option.rect.width() - 24);
      for (std::size_t block = 0; block < presentation.blocks.size(); ++block) {
        const PassiveBlock &value = presentation.blocks[block];
        QFont font = option.font;
        if (value.metadata)
          font.setPointSizeF(std::max(7.0, font.pointSizeF() - 1.0));
        const int height =
            documentHeight(row->stableKey, block, value, bodyWidth, font);
        paintDocument(
            painter, row->stableKey, block, value,
            QRect(option.rect.left() + 12, blockTop, bodyWidth, height), font);
        blockTop += height + 6;
      }
    }

    painter->setPen(QPen(QColor(QStringLiteral("#667085")), 1.3));
    painter->setBrush(Qt::NoBrush);
    const qreal copyLeft = option.rect.right() - 43.0;
    painter->drawRoundedRect(
        QRectF(copyLeft, option.rect.top() + 14.0, 8.0, 9.0), 1.0, 1.0);
    painter->drawRoundedRect(
        QRectF(copyLeft + 3.0, option.rect.top() + 17.0, 8.0, 9.0), 1.0, 1.0);
    QPainterPath chevron;
    if (collapsed) {
      chevron.moveTo(option.rect.right() - 15.0, top + 7.0);
      chevron.lineTo(option.rect.right() - 19.0, top + 12.0);
      chevron.lineTo(option.rect.right() - 15.0, top + 17.0);
    } else {
      chevron.moveTo(option.rect.right() - 20.0, top + 9.0);
      chevron.lineTo(option.rect.right() - 15.0, top + 14.0);
      chevron.lineTo(option.rect.right() - 10.0, top + 9.0);
    }
    painter->drawPath(chevron);
    if (row->card.activeWork.value_or(false)) {
      painter->setPen(QPen(QColor(QStringLiteral("#98a2b3")), 2.0));
      painter->drawRoundedRect(bounds.adjusted(0.5, 0.5, -0.5, -0.5), 9.0, 9.0);
    }
    painter->restore();
  }

private:
  struct DocumentRecord {
    QString text;
    int width = 0;
    bool markdown = false;
    QFont font;
    std::unique_ptr<QTextDocument> document;
    std::uint64_t used = 0;
  };

  QTextDocument *document(const std::string &stableKey, std::size_t block,
                          const PassiveBlock &value, int width,
                          const QFont &font) const {
    const std::string key = stableKey + ':' + std::to_string(block);
    auto found = documents_.find(key);
    if (found == documents_.end() || found->second.text != value.text ||
        found->second.width != width ||
        found->second.markdown != value.markdown ||
        found->second.font != font) {
      if (found != documents_.end())
        documents_.erase(found);
      if (documents_.size() >= 128) {
        const auto oldest =
            std::ranges::min_element(documents_, {}, [](const auto &entry) {
              return entry.second.used;
            });
        if (oldest != documents_.end())
          documents_.erase(oldest);
      }
      DocumentRecord record;
      record.text = value.text;
      record.width = width;
      record.markdown = value.markdown;
      record.font = font;
      record.document = std::make_unique<QTextDocument>();
      record.document->setDocumentMargin(0);
      record.document->setDefaultFont(font);
      record.document->setDefaultStyleSheet(
          QStringLiteral("a{color:#5471a6;text-decoration:none;}"));
      if (value.markdown)
        record.document->setMarkdown(value.text,
                                     QTextDocument::MarkdownFeatures(
                                         QTextDocument::MarkdownDialectGitHub) |
                                         QTextDocument::MarkdownNoHTML);
      else
        record.document->setPlainText(value.text);
      record.document->setTextWidth(width);
      found = documents_.emplace(key, std::move(record)).first;
    }
    found->second.used = ++documentUse_;
    return found->second.document.get();
  }

  int documentHeight(const std::string &stableKey, std::size_t block,
                     const PassiveBlock &value, int width,
                     const QFont &font) const {
    return std::max(
        1,
        static_cast<int>(std::ceil(
            document(stableKey, block, value, width, font)->size().height())) +
            (value.markdown ? 4 : 0));
  }

  void paintDocument(QPainter *painter, const std::string &stableKey,
                     std::size_t block, const PassiveBlock &value,
                     const QRect &rect, const QFont &font) const {
    QTextDocument *valueDocument =
        document(stableKey, block, value, rect.width(), font);
    QAbstractTextDocumentLayout::PaintContext context;
    context.palette.setColor(
        QPalette::Text, value.metadata ? QColor(QStringLiteral("#667085"))
                                       : QColor(QStringLiteral("#1d2633")));
    context.clip = QRect(QPoint{}, rect.size());
    painter->save();
    painter->translate(rect.topLeft());
    valueDocument->documentLayout()->draw(painter, context);
    painter->restore();
  }

  mutable std::unordered_map<std::string, DocumentRecord> documents_;
  mutable std::uint64_t documentUse_ = 0;
};

} // namespace

ConversationView::ConversationView(QWidget *parent)
    : QAbstractItemView(parent), model_(new ConversationItemModel(this)) {
  setObjectName(QStringLiteral("conversationScroll"));
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
  setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  setSelectionMode(QAbstractItemView::SingleSelection);
  setSelectionBehavior(QAbstractItemView::SelectRows);
  setTabKeyNavigation(true);
  setModel(model_);
  setItemDelegate(new ConversationPassiveDelegate(this));
  setMouseTracking(true);
  verticalScrollBar()->setSingleStep(NativeScrollLineStep);
  viewport()->setAutoFillBackground(false);

  loadMore_ =
      new QPushButton(QStringLiteral("Load more activities"), viewport());
  loadMore_->setObjectName(QStringLiteral("conversationLoadMore"));
  loadMore_->setProperty("kind", "history");
  loadMore_->setFixedHeight(HistoryButtonHeight);
  loadMore_->hide();
  connect(loadMore_, &QPushButton::clicked, this, [this] {
    if (loadMoreAction_)
      loadMoreAction_();
  });

  empty_ = makeEmptyLabel(viewport());
  emptyMessage_ = empty_->text();

  // Rich rows prepared for an atomic thread/paging reveal are never parented
  // into the visible or accessible viewport until their final geometry is
  // known.
  stagingHost_ = new QWidget;
  stagingHost_->setObjectName(QStringLiteral("conversationStagingHost"));
  stagingHost_->hide();

  stagingOverlay_ = new ConversationLoadingOverlay(viewport());

  followAnimation_ = new QVariantAnimation(this);
  followAnimation_->setEasingCurve(QEasingCurve::OutCubic);
  connect(followAnimation_, &QVariantAnimation::valueChanged, this,
          [this](const QVariant &value) {
            if (mode_ != Mode::Following || applying_) {
              followAnimation_->stop();
              return;
            }
            setScrollValue(
                std::max(verticalScrollBar()->value(), value.toInt()));
          });
  connect(followAnimation_, &QVariantAnimation::finished, this, [this] {
    if (mode_ == Mode::Following)
      setScrollValue(verticalScrollBar()->maximum());
  });

  connect(verticalScrollBar(), &QScrollBar::sliderPressed, this, [this] {
    sliderDown_ = true;
    pausedByComposerGrowth_ = false;
    stopFollowingAnimation();
  });
  connect(verticalScrollBar(), &QScrollBar::sliderReleased, this, [this] {
    sliderDown_ = false;
    handleUserScrollValue(verticalScrollBar()->value());
  });
  connect(verticalScrollBar(), &QScrollBar::actionTriggered, this,
          [this](int action) {
            userActionPending_ = true;
            pausedByComposerGrowth_ = false;
            stopFollowingAnimation();
            if (action == QAbstractSlider::SliderSingleStepSub ||
                action == QAbstractSlider::SliderPageStepSub ||
                action == QAbstractSlider::SliderToMinimum)
              mode_ = Mode::Paused;
          });
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            if (!programmaticScroll_ && !applying_ &&
                (sliderDown_ || userActionPending_))
              handleUserScrollValue(value);
            userActionPending_ = false;
          });

  rebuildHeightIndex();
  updateScrollRange();
}

ConversationView::~ConversationView() {
  cancelStructuralStaging();
  releaseAllCards();
  delete stagingHost_;
}

void ConversationView::setLoadMoreAction(std::function<void()> action) {
  loadMoreAction_ = std::move(action);
}

void ConversationView::setPromptMaterializedAction(
    std::function<bool(nodegraph::NodeRef)> action) {
  promptMaterializedAction_ = std::move(action);
}

void ConversationView::setPromptRecoveryAction(
    std::function<void(nodegraph::NodeRef)> action) {
  promptRecoveryAction_ = std::move(action);
}

void ConversationView::setPresentationCommittedAction(
    std::function<void(const std::string &)> action) {
  presentationCommittedAction_ = std::move(action);
}

void ConversationView::setEmptyMessage(QString message) {
  if (message == emptyMessage_)
    return;
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  emptyMessage_ = std::move(message);
  empty_->setText(emptyMessage_);
  updateScrollRange();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  layoutMaterializedCards();
  viewport()->update();
}

void ConversationView::setPresentationOptions(PresentationOptions options) {
  if (presentationOptions_ == options)
    return;
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  presentationOptions_ = options;
  const bool visibilityChanged =
      model_->setVisibility({options.showReasoning, options.showCodexUpdates});
  if (!visibilityChanged)
    return;
  rebuildSectionRanges();
  rebuildHeightIndex();
  updateScrollRange();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  updateMaterialization(false);
  viewport()->update();
}

bool ConversationView::cardVisible(const VisibleCardData &card) const noexcept {
  if (card.kind == CardKind::Reasoning)
    return presentationOptions_.showReasoning;
  if (card.kind != CardKind::AgentMessage)
    return true;
  const auto *message = std::get_if<AgentMessageData>(&card.payload);
  return !message || message->finalAnswer ||
         presentationOptions_.showCodexUpdates;
}

void ConversationView::storeCurrentThreadState() {
  if (threadId_.empty())
    return;
  threadStates_[threadId_] = {mode_, captureAnchor(), pausedByComposerGrowth_};
}

void ConversationView::setThread(const std::string &threadId) {
  if (threadId == threadId_)
    return;
  storeCurrentThreadState();
  stopFollowingAnimation();
  threadId_ = threadId;
  const auto saved = threadStates_.find(threadId_);
  mode_ = saved == threadStates_.end() ? Mode::Following : saved->second.mode;
  pausedByComposerGrowth_ =
      saved != threadStates_.end() && saved->second.pausedByComposerGrowth;
}

bool ConversationView::reconcile(const ConversationSnapshot &snapshot) {
  if (!committingStructuralStage_ && pendingStructuralSnapshot_)
    cancelStructuralStaging();
  return reconcileOwned(ConversationSnapshot(snapshot));
}

void ConversationView::beginThreadSelection(const std::string &threadId) {
  if (threadId.empty() ||
      (loadingThreadId_ == threadId && stagingOverlay_->isVisible()))
    return;
  cancelStructuralStaging();
  loadingThreadId_ = threadId;
  stagingOverlay_->setGeometry(viewport()->rect());
  stagingOverlay_->begin();
  incrementProperty(this, "threadSelectionLoadsStarted");
}

bool ConversationView::reconcileOwned(ConversationSnapshot snapshot) {
  const std::string targetThreadId = snapshot.threadId;
  const bool switchedThread = snapshot.threadId != threadId_;
  const Anchor currentAnchor = captureAnchor();
  setThread(snapshot.threadId);
  Anchor targetAnchor = currentAnchor;
  if (switchedThread) {
    const auto saved = threadStates_.find(snapshot.threadId);
    targetAnchor =
        saved == threadStates_.end() ? Anchor{} : saved->second.anchor;
  }
  const bool follow = mode_ == Mode::Following;

  std::unordered_map<std::string, CardKind> previousKinds;
  previousKinds.reserve(static_cast<std::size_t>(model_->rowCount()));
  for (int row = 0; row < model_->rowCount(); ++row) {
    if (const auto *value = model_->row(row))
      previousKinds.emplace(value->stableKey, value->card.kind);
  }

  const QScopedValueRollback applying(applying_, true);
  const QSignalBlocker scrollSignals(verticalScrollBar());
  viewport()->setUpdatesEnabled(false);
  stopFollowingAnimation();

  const bool changed = model_->reconcile(std::move(snapshot));
  if (!targetThreadId.empty()) {
    HistoryWindow &history = historyWindows_[targetThreadId];
    const std::size_t represented = model_->historyActivityCount();
    if (represented > history.effective) {
      history.requested = represented;
      history.effective = represented;
    }
    history.lastAuthoritativeCount =
        std::max(history.lastAuthoritativeCount,
                 represented + model_->hiddenAuthoritativeItemCount());
  }
  rebuildSectionRanges();
  loadMore_->setVisible(model_->hasMore());
  if (model_->hasMore()) {
    const std::size_t page =
        model_->hiddenAuthoritativeItemCount() == 0
            ? AuthoritativeHistoryPageSize
            : std::min(AuthoritativeHistoryPageSize,
                       model_->hiddenAuthoritativeItemCount());
    loadMore_->setText(QStringLiteral("Load %1 more activities")
                           .arg(static_cast<qulonglong>(page)));
    loadMore_->setToolTip(
        model_->hiddenAuthoritativeItemCount() == 0
            ? QStringLiteral("Earlier activities are available")
            : QStringLiteral("%1 earlier activities are retained")
                  .arg(static_cast<qulonglong>(
                      model_->hiddenAuthoritativeItemCount())));
  }
  empty_->setVisible(model_->rowCount() == 0);

  std::vector<std::string> removeKeys;
  removeKeys.reserve(materializedCards_.size());
  for (auto &[key, card] : materializedCards_) {
    const QModelIndex index = model_->indexForStableKey(key);
    const ConversationItemModel::Row *row = model_->row(index.row());
    if (!index.isValid() || !row || !rowPresented(index.row()) ||
        !card->canApply(row->card)) {
      removeKeys.push_back(key);
      continue;
    }
    if (card->data() != row->card)
      static_cast<void>(card->applyPresentation(row->card));
    configureCardForRow(card, *row);
    const int measuredHeight = measureCard(card, rowWidth(*row));
    heightCache_.insert_or_assign(
        key, HeightRecord{rowWidth(*row), measuredHeight});
  }
  for (const std::string &key : removeKeys) {
    const auto found = materializedCards_.find(key);
    if (found == materializedCards_.end())
      continue;
    ConversationCard *card = found->second;
    materializedCards_.erase(found);
    releaseCard(key, card);
  }

  rebuildHeightIndex();
  updateScrollRange();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(targetAnchor);
  updateMaterialization(false);

  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(targetAnchor);
  layoutMaterializedCards();
  storeCurrentThreadState();

  std::vector<nodegraph::NodeRef> acknowledged;
  for (int rowIndex = 0; rowIndex < model_->rowCount(); ++rowIndex) {
    const auto *row = model_->row(rowIndex);
    if (!row || row->card.kind != CardKind::UserMessage || !row->card.target)
      continue;
    const auto before = previousKinds.find(row->stableKey);
    if (before != previousKinds.end() &&
        before->second == CardKind::LocalPrompt)
      acknowledged.push_back(row->card.target);
  }

  viewport()->setUpdatesEnabled(true);
  finishThreadSelection(targetThreadId);
  viewport()->update();
  if (changed)
    incrementProperty(this, "graphRefreshPasses");
  for (nodegraph::NodeRef &target : acknowledged)
    if (promptMaterializedAction_ &&
        !promptMaterializedAction_(std::move(target)))
      break;
  return changed;
}

void ConversationView::reconcileStaged(ConversationSnapshot snapshot) {
  if (!loadingThreadId_.empty() &&
      snapshot.threadId != loadingThreadId_) {
    incrementProperty(this, "staleThreadStagesIgnored");
    return;
  }
  if (snapshot.threadId != threadId_ && loadingThreadId_.empty())
    beginThreadSelection(snapshot.threadId);
  else
    cancelStructuralStaging();
  pendingStructuralSnapshot_ = std::move(snapshot);
  buildPendingLocations();
  choosePendingStageRows();
  pendingStructuralCardIndex_ = 0;
  stagingHost_->resize(std::max(0, viewport()->width()),
                       std::max(0, viewport()->height()));
  incrementProperty(this, "structuralStageStarts");
  if (pendingStructuralCardKeys_.empty()) {
    runStructuralStagePass();
    return;
  }
  scheduleStructuralStagePass();
}

void ConversationView::buildPendingLocations() {
  pendingLocations_.clear();
  if (!pendingStructuralSnapshot_)
    return;
  for (std::size_t sectionIndex = 0;
       sectionIndex < pendingStructuralSnapshot_->sections.size();
       ++sectionIndex) {
    TurnSection &section = pendingStructuralSnapshot_->sections[sectionIndex];
    std::optional<std::string> root;
    if (section.rootCardKey)
      root = stableKey(*section.rootCardKey);
    const bool representedRoot =
        root && std::ranges::any_of(section.cards, [&](const auto &card) {
          return stableKey(card.key) == *root;
        });
    for (std::size_t cardIndex = 0; cardIndex < section.cards.size();
         ++cardIndex) {
      const std::string key = stableKey(section.cards[cardIndex].key);
      const bool isRoot = representedRoot && key == *root;
      pendingLocations_.emplace(
          key,
          PendingLocation{
              sectionIndex, cardIndex, representedRoot && !isRoot, isRoot,
              isRoot && pendingStructuralSnapshot_->activeTurnId &&
                  section.turnId == *pendingStructuralSnapshot_->activeTurnId});
    }
  }
}

void ConversationView::choosePendingStageRows() {
  pendingStructuralCardKeys_.clear();
  if (!pendingStructuralSnapshot_ || pendingLocations_.empty())
    return;

  std::vector<std::string> keys;
  keys.reserve(pendingLocations_.size());
  for (const TurnSection &section : pendingStructuralSnapshot_->sections)
    for (const VisibleCardData &card : section.cards)
      keys.push_back(stableKey(card.key));

  const int viewportRows =
      std::max(MinimumMaterializationRows,
               std::max(1, viewport()->height()) / EstimatedCardHeight + 2);
  const std::size_t budget = static_cast<std::size_t>(viewportRows * 3);
  std::size_t center = keys.empty() ? 0 : keys.size() - 1;
  if (pendingStructuralSnapshot_->threadId == threadId_ &&
      mode_ == Mode::Paused) {
    const Anchor anchor = captureAnchor();
    const auto found = std::ranges::find(keys, anchor.stableKey);
    if (found != keys.end())
      center = static_cast<std::size_t>(std::distance(keys.begin(), found));
  } else if (const auto saved =
                 threadStates_.find(pendingStructuralSnapshot_->threadId);
             saved != threadStates_.end() &&
             saved->second.mode == Mode::Paused) {
    const auto found = std::ranges::find(keys, saved->second.anchor.stableKey);
    if (found != keys.end())
      center = static_cast<std::size_t>(std::distance(keys.begin(), found));
  }
  const std::size_t first = center > budget / 2 ? center - budget / 2 : 0;
  const std::size_t last = std::min(keys.size(), first + budget);
  for (std::size_t index = first; index < last; ++index) {
    const std::string &key = keys[index];
    VisibleCardData *card = pendingCard(key);
    if (!card || !cardVisible(*card))
      continue;
    bool collapsed = false;
    if (const auto retained = cardCollapsedStates_.find(key);
        retained != cardCollapsedStates_.end()) {
      collapsed = retained->second;
    } else if (card->kind == CardKind::CommandExecution) {
      collapsed = !presentationOptions_.commandsInitiallyExpanded;
    } else if (card->kind == CardKind::ImageGeneration) {
      collapsed = !presentationOptions_.imagesInitiallyExpanded;
    } else if (card->kind == CardKind::FileChanges) {
      collapsed = !presentationOptions_.fileChangesInitiallyExpanded;
    } else {
      collapsed = card->kind != CardKind::UserMessage &&
                  card->kind != CardKind::AgentMessage &&
                  card->kind != CardKind::LocalPrompt;
    }
    if (eligibleForPassivePresentation(*card, collapsed))
      continue;
    const auto retained = materializedCards_.find(key);
    if (retained != materializedCards_.end() &&
        retained->second->canApply(*card))
      continue;
    pendingStructuralCardKeys_.push_back(key);
  }
}

VisibleCardData *ConversationView::pendingCard(const std::string &key) {
  if (!pendingStructuralSnapshot_)
    return nullptr;
  const auto found = pendingLocations_.find(key);
  if (found == pendingLocations_.end())
    return nullptr;
  const PendingLocation &location = found->second;
  if (location.section >= pendingStructuralSnapshot_->sections.size())
    return nullptr;
  TurnSection &section = pendingStructuralSnapshot_->sections[location.section];
  return location.card < section.cards.size() ? &section.cards[location.card]
                                              : nullptr;
}

const ConversationView::PendingLocation *
ConversationView::pendingLocation(const std::string &key) const {
  const auto found = pendingLocations_.find(key);
  return found == pendingLocations_.end() ? nullptr : &found->second;
}

void ConversationView::scheduleStructuralStagePass() {
  if (structuralStagePassScheduled_ || !pendingStructuralSnapshot_)
    return;
  structuralStagePassScheduled_ = true;
  QTimer::singleShot(1, Qt::PreciseTimer, this, [this] {
    structuralStagePassScheduled_ = false;
    runStructuralStagePass();
  });
}

void ConversationView::runStructuralStagePass() {
  if (!pendingStructuralSnapshot_)
    return;
  while (pendingStructuralCardIndex_ < pendingStructuralCardKeys_.size()) {
    const std::string key =
        pendingStructuralCardKeys_[pendingStructuralCardIndex_++];
    VisibleCardData *data = pendingCard(key);
    const PendingLocation *location = pendingLocation(key);
    if (!data || !location)
      continue;
    ConversationCard *card = createCard(*data, stagingHost_, key);
    card->setNestedPresentation(location->nested);
    card->setAuthoritativeTurnActive(location->activeTurn);
    const int width = std::max(
        0, viewport()->width() - (location->nested ? 2 * NestedCardIndent : 0));
    const int height = measureCard(card, width);
    card->hide();
    stagedCards_.insert_or_assign(key, card);
    stagedHeights_.insert_or_assign(key, height);
    incrementProperty(this, "structuralStageCardPasses");
    scheduleStructuralStagePass();
    return;
  }

  ConversationSnapshot completed = std::move(*pendingStructuralSnapshot_);
  pendingStructuralSnapshot_.reset();
  pendingStructuralCardKeys_.clear();
  pendingStructuralCardIndex_ = 0;
  pendingLocations_.clear();
  const QScopedValueRollback committing(committingStructuralStage_, true);
  static_cast<void>(reconcileOwned(std::move(completed)));
  for (auto &[key, card] : stagedCards_) {
    static_cast<void>(key);
    delete card;
  }
  stagedCards_.clear();
  stagedHeights_.clear();
  incrementProperty(this, "structuralStageCommits");
}

void ConversationView::cancelStructuralStaging() {
  pendingStructuralSnapshot_.reset();
  pendingLocations_.clear();
  pendingStructuralCardKeys_.clear();
  pendingStructuralCardIndex_ = 0;
  for (auto &[key, card] : stagedCards_) {
    static_cast<void>(key);
    delete card;
  }
  stagedCards_.clear();
  stagedHeights_.clear();
}

void ConversationView::finishThreadSelection(const std::string &threadId) {
  if (!loadingThreadId_.empty() && !threadId.empty() &&
      loadingThreadId_ != threadId)
    return;
  if (loadingThreadId_.empty() && !stagingOverlay_->isVisible())
    return;
  loadingThreadId_.clear();
  stagingOverlay_->finish();
  incrementProperty(this, "threadSelectionLoadsFinished");
  if (presentationCommittedAction_)
    presentationCommittedAction_(threadId);
}

std::optional<PresentationImpact>
ConversationView::applyCardPresentation(const VisibleCardData &card) {
  return applyCardPresentationOwned(VisibleCardData(card));
}

std::optional<PresentationImpact>
ConversationView::applyCardPresentation(VisibleCardData &&card) {
  return applyCardPresentationOwned(std::move(card));
}

std::optional<PresentationImpact> ConversationView::applyPromptMaterialization(
    PromptMaterialization materialization) {
  if (!materialization.prompt)
    return std::nullopt;
  return applyCardPresentationOwned(std::move(materialization.card),
                                    std::move(materialization.prompt));
}

bool ConversationView::applyRowChange(ConversationRowChange change) {
  if (pendingStructuralSnapshot_ ||
      change.placement.card.threadId != threadId_)
    return false;
  const std::string key = stableKey(change.placement.card.key);
  if (key.empty())
    return false;

  QModelIndex source =
      model_->indexForTarget(change.placement.card.target);
  const QModelIndex stableSource = model_->indexForStableKey(key);
  if (!source.isValid() && stableSource.isValid()) {
    const ConversationItemModel::Row *row = model_->row(stableSource.row());
    if (!row)
      return false;
    const auto retargeted = model_->updateCard(change.placement.card);
    if (retargeted == ConversationItemModel::CardUpdateResult::Missing ||
        retargeted == ConversationItemModel::CardUpdateResult::Incompatible)
      return false;
    source = model_->indexForTarget(change.placement.card.target);
  }

  const int sourceRow = source.isValid() ? source.row() : -1;
  const std::string oldSection =
      sourceRow >= 0 ? model_->row(sourceRow)->sectionKey : std::string{};
  const std::string newSection = change.placement.sectionKey;
  int destinationRow = -1;
  if (change.previousCardKey) {
    const QModelIndex previous =
        model_->indexForStableKey(stableKey(*change.previousCardKey));
    if (previous.isValid() && previous.row() != sourceRow) {
      destinationRow = previous.row() + 1;
      if (sourceRow >= 0 && sourceRow < destinationRow)
        --destinationRow;
    }
  } else {
    // The adapter saw the canonical beginning, rather than merely failing to
    // resolve a predecessor. Applying a coalesced reorder from front to back
    // therefore leaves the already-correct prefix stable.
    destinationRow = 0;
  }
  if (destinationRow < 0 && change.nextCardKey) {
    const QModelIndex next =
        model_->indexForStableKey(stableKey(*change.nextCardKey));
    if (next.isValid() && next.row() != sourceRow) {
      destinationRow = next.row();
      if (sourceRow >= 0 && sourceRow < destinationRow)
        --destinationRow;
    }
  }
  if (destinationRow < 0) {
    if (sourceRow >= 0)
      destinationRow = sourceRow;
    else if (model_->rowCount() == 0)
      destinationRow = 0;
    else
      return false;
  }

  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  const QScopedValueRollback applying(applying_, true);
  const QSignalBlocker scrollSignals(verticalScrollBar());
  stopFollowingAnimation();

  ConversationItemModel::StructuralChangeResult result;
  if (sourceRow >= 0) {
    nodegraph::NodeRef target = change.placement.card.target;
    result = model_->moveTarget(target, destinationRow,
                                std::move(change.placement));
  } else {
    result =
        model_->insertCard(destinationRow, std::move(change.placement));
  }
  if (result == ConversationItemModel::StructuralChangeResult::Missing ||
      result == ConversationItemModel::StructuralChangeResult::Invalid ||
      result == ConversationItemModel::StructuralChangeResult::Duplicate)
    return false;
  if (result == ConversationItemModel::StructuralChangeResult::Unchanged)
    return true;

  finishExactStructureChange(anchor, follow, sourceRow, destinationRow, key,
                             oldSection, newSection);
  incrementProperty(this, "targetedStructuralRowChanges");
  return true;
}

bool ConversationView::removeCardTarget(const nodegraph::NodeRef &target) {
  if (pendingStructuralSnapshot_)
    return false;
  const QModelIndex index = model_->indexForTarget(target);
  const ConversationItemModel::Row *row = model_->row(index.row());
  if (!index.isValid() || !row)
    return false;
  const std::string key = row->stableKey;
  const std::string oldSection = row->sectionKey;
  const int removedRow = index.row();
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  const QScopedValueRollback applying(applying_, true);
  const QSignalBlocker scrollSignals(verticalScrollBar());
  stopFollowingAnimation();

  if (model_->removeTarget(target) !=
      ConversationItemModel::StructuralChangeResult::Changed)
    return false;
  if (const auto found = materializedCards_.find(key);
      found != materializedCards_.end()) {
    ConversationCard *card = found->second;
    materializedCards_.erase(found);
    releaseCard(key, card);
  }
  heightCache_.erase(key);
  cardCollapsedStates_.erase(key);
  cardInteractionStates_.erase(key);
  finishExactStructureChange(anchor, follow, removedRow, -1, key, oldSection,
                             {});
  incrementProperty(this, "targetedStructuralRemovals");
  return true;
}

void ConversationView::finishExactStructureChange(const Anchor &anchor,
                                                  bool follow, int sourceRow,
                                                  int destinationRow,
                                                  std::string changedKey,
                                                  std::string oldSection,
                                                  std::string newSection) {
  std::unordered_map<std::string, SectionRange> previousSections;
  for (const std::string *section : {&oldSection, &newSection}) {
    if (section->empty() || previousSections.contains(*section))
      continue;
    if (const auto found = sectionRanges_.find(*section);
        found != sectionRanges_.end())
      previousSections.emplace(*section, found->second);
  }

  if (sourceRow < 0) {
    const std::array<int, 1> inserted{0};
    heights_.insert(static_cast<std::size_t>(destinationRow), inserted);
  } else if (destinationRow < 0) {
    heights_.remove(static_cast<std::size_t>(sourceRow), 1);
  } else if (sourceRow != destinationRow) {
    heights_.move(static_cast<std::size_t>(sourceRow), 1,
                  static_cast<std::size_t>(destinationRow));
  }

  const auto nearSectionRow = [this](const std::string &section,
                                     int preferred) {
    if (preferred >= 0 && preferred < model_->rowCount()) {
      const ConversationItemModel::Row *candidate = model_->row(preferred);
      if (candidate && candidate->sectionKey == section)
        return preferred;
    }
    for (const int neighbor : {preferred - 1, preferred + 1}) {
      if (neighbor < 0 || neighbor >= model_->rowCount())
        continue;
      const ConversationItemModel::Row *candidate = model_->row(neighbor);
      if (candidate && candidate->sectionKey == section)
        return neighbor;
    }
    return -1;
  };
  const auto updateSection = [&](const std::string &section,
                                 int preferred) {
    if (section.empty())
      return;
    const auto previous = previousSections.find(section);
    SectionRange replacement = previous == previousSections.end()
                                   ? SectionRange{}
                                   : previous->second;
    bool rescan = false;
    const auto validRow = [this, &section](const std::string &key,
                                          bool requireRoot) {
      const std::optional<int> position = modelSectionRow(key);
      const ConversationItemModel::Row *row =
          position ? model_->row(*position) : nullptr;
      return row && row->sectionKey == section &&
             (!requireRoot || row->turnRoot);
    };
    if (!replacement.root.empty() && !validRow(replacement.root, true))
      replacement.root.clear();
    if (!replacement.first.empty() &&
        (!validRow(replacement.first, false) ||
         !rowPresented(*modelSectionRow(replacement.first)))) {
      replacement.first.clear();
      rescan = true;
    }
    if (!replacement.last.empty() &&
        (!validRow(replacement.last, false) ||
         !rowPresented(*modelSectionRow(replacement.last)))) {
      replacement.last.clear();
      rescan = true;
    }

    const QModelIndex changedIndex = model_->indexForStableKey(changedKey);
    const ConversationItemModel::Row *changed =
        model_->row(changedIndex.row());
    const bool changedInSection =
        changedIndex.isValid() && changed && changed->sectionKey == section;
    if (changedInSection && changed->turnRoot)
      replacement.root = changedKey;
    const std::string oldRoot = previous == previousSections.end()
                                    ? std::string{}
                                    : previous->second.root;
    if (replacement.root != oldRoot)
      rescan = true;

    if (sourceRow >= 0 && destinationRow >= 0 && sourceRow != destinationRow &&
        previous != previousSections.end() &&
        (previous->second.first == changedKey ||
         previous->second.last == changedKey))
      rescan = true;
    if (changedInSection && rowPresented(changedIndex.row())) {
      const std::optional<int> first = modelSectionRow(replacement.first);
      const std::optional<int> last = modelSectionRow(replacement.last);
      if (!first || changedIndex.row() < *first)
        replacement.first = changedKey;
      if (!last || changedIndex.row() > *last)
        replacement.last = changedKey;
    } else if (previous != previousSections.end() &&
               (previous->second.first == changedKey ||
                previous->second.last == changedKey)) {
      rescan = true;
    }

    if (rescan) {
      rebuildSectionRange(section, nearSectionRow(section, preferred));
      return;
    }
    const std::optional<int> root = modelSectionRow(replacement.root);
    const ConversationItemModel::Row *rootRow =
        root ? model_->row(*root) : nullptr;
    replacement.active = rootRow && rootRow->activeTurn;
    if (replacement.first.empty() && replacement.root.empty()) {
      sectionRanges_.erase(section);
      if (activeSectionKey_ == section)
        activeSectionKey_.clear();
    } else {
      if (replacement.active)
        activeSectionKey_ = section;
      else if (activeSectionKey_ == section)
        activeSectionKey_.clear();
      sectionRanges_.insert_or_assign(section, std::move(replacement));
    }
    incrementProperty(this, "conversationSectionRangeLocalUpdates");
  };
  updateSection(oldSection, std::max(0, sourceRow));
  if (newSection != oldSection)
    updateSection(newSection, destinationRow);

  std::unordered_set<std::string> affectedKeys{changedKey};
  bool rootStructureChanged = false;
  for (const std::string *section : {&oldSection, &newSection}) {
    if (section->empty())
      continue;
    const auto before = previousSections.find(*section);
    const auto after = sectionRanges_.find(*section);
    if (before != previousSections.end()) {
      affectedKeys.insert(before->second.first);
      affectedKeys.insert(before->second.last);
      affectedKeys.insert(before->second.root);
    }
    if (after != sectionRanges_.end()) {
      affectedKeys.insert(after->second.first);
      affectedKeys.insert(after->second.last);
      affectedKeys.insert(after->second.root);
    }
    const std::string beforeRoot = before == previousSections.end()
                                       ? std::string{}
                                       : before->second.root;
    const std::string afterRoot = after == sectionRanges_.end()
                                      ? std::string{}
                                      : after->second.root;
    rootStructureChanged = rootStructureChanged || beforeRoot != afterRoot;
  }

  if (rootStructureChanged) {
    for (const std::string *section : {&oldSection, &newSection}) {
      if (section->empty())
        continue;
      const auto range = sectionRanges_.find(*section);
      const std::optional<int> member =
          range == sectionRanges_.end()
              ? std::nullopt
              : modelSectionRow(!range->second.first.empty()
                                    ? range->second.first
                                    : range->second.root);
      if (!member)
        continue;
      int first = *member;
      while (first > 0 && model_->row(first - 1)->sectionKey == *section)
        --first;
      for (int rowIndex = first; rowIndex < model_->rowCount(); ++rowIndex) {
        const ConversationItemModel::Row *row = model_->row(rowIndex);
        if (!row || row->sectionKey != *section)
          break;
        affectedKeys.insert(row->stableKey);
      }
    }
  }

  const auto refreshExtent = [this](const std::string &key) {
    if (key.empty())
      return;
    const QModelIndex index = model_->indexForStableKey(key);
    const ConversationItemModel::Row *row = model_->row(index.row());
    if (!index.isValid() || !row)
      return;
    if (!rowPresented(index.row())) {
      static_cast<void>(
          heights_.setHeight(static_cast<std::size_t>(index.row()), 0));
      return;
    }
    int cardHeight = estimatedCardHeight(row->card);
    if (const auto cached = heightCache_.find(key);
        cached != heightCache_.end() &&
        cached->second.width == rowWidth(*row))
      cardHeight = cached->second.height;
    static_cast<void>(heights_.setHeight(
        static_cast<std::size_t>(index.row()),
        std::max(1, cardHeight) + rowSpacing(index.row())));
  };
  for (const std::string &key : affectedKeys)
    refreshExtent(key);

  std::vector<std::string> released;
  for (auto &[key, card] : materializedCards_) {
    const QModelIndex index = model_->indexForStableKey(key);
    const ConversationItemModel::Row *row = model_->row(index.row());
    if (!index.isValid() || !row || !rowPresented(index.row()) ||
        !card->canApply(row->card)) {
      released.push_back(key);
      continue;
    }
    if (!affectedKeys.contains(key) && row->sectionKey != oldSection &&
        row->sectionKey != newSection)
      continue;
    if (card->data() != row->card) {
      captureCardInteractionState(key, card, false);
      static_cast<void>(card->applyPresentation(row->card));
      restoreCardInteractionState(key, card);
    }
    configureCardForRow(card, *row);
    const int height = measureCard(card, rowWidth(*row));
    heightCache_.insert_or_assign(
        key, HeightRecord{rowWidth(*row), height});
    static_cast<void>(heights_.setHeight(
        static_cast<std::size_t>(index.row()), height + rowSpacing(index.row())));
  }
  for (const std::string &key : released) {
    const auto found = materializedCards_.find(key);
    if (found == materializedCards_.end())
      continue;
    ConversationCard *card = found->second;
    materializedCards_.erase(found);
    releaseCard(key, card);
  }

  empty_->setVisible(model_->rowCount() == 0);
  setProperty("conversationHeightIndexUpdateSteps",
              static_cast<qulonglong>(heights_.lastUpdateSteps()));
  updateScrollRange();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  updateMaterialization(false);
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  layoutMaterializedCards();

  QRect damage;
  const QRect viewportBounds = viewport()->rect();
  const auto addPresentedRow = [&](int rowIndex) {
    const QRect row = rowRect(rowIndex).intersected(viewportBounds);
    if (!row.isEmpty())
      damage = damage.united(row);
  };
  for (const std::string &key : affectedKeys) {
    const QModelIndex index = model_->indexForStableKey(key);
    if (index.isValid())
      addPresentedRow(index.row());
  }

  const auto firstPresentedAtOrAfter = [this](int rowIndex) {
    if (heights_.totalHeight() <= 0 || rowIndex >= model_->rowCount())
      return -1;
    rowIndex = std::max(0, rowIndex);
    const qint64 top = heights_.top(static_cast<std::size_t>(rowIndex));
    if (top >= heights_.totalHeight())
      return -1;
    const int candidate = static_cast<int>(heights_.rowAt(top));
    return candidate >= rowIndex && rowPresented(candidate) ? candidate : -1;
  };
  const auto lastPresentedAtOrBefore = [this](int rowIndex) {
    if (heights_.totalHeight() <= 0 || rowIndex < 0)
      return -1;
    rowIndex = std::min(rowIndex, model_->rowCount() - 1);
    const qint64 bottom = heights_.bottom(static_cast<std::size_t>(rowIndex));
    if (bottom <= 0)
      return -1;
    const int candidate = static_cast<int>(heights_.rowAt(bottom - 1));
    return candidate <= rowIndex && rowPresented(candidate) ? candidate : -1;
  };
  const auto addInterval = [&](int firstRow, int lastRow) {
    const int first = firstPresentedAtOrAfter(firstRow);
    const int last = lastPresentedAtOrBefore(lastRow);
    if (first >= 0 && last >= first) {
      const QRect interval =
          rowRect(first).united(rowRect(last)).intersected(viewportBounds);
      if (!interval.isEmpty())
        damage = damage.united(
            QRect(0, interval.top(), viewport()->width(), interval.height()));
    }
  };
  const auto addFromRowToBottom = [&](int changedRow) {
    const int first = firstPresentedAtOrAfter(changedRow);
    if (first >= 0) {
      const QRect firstRect = rowRect(first);
      if (firstRect.bottom() >= viewportBounds.top() &&
          firstRect.top() <= viewportBounds.bottom()) {
        const int top = std::max(viewportBounds.top(), firstRect.top());
        damage = damage.united(
            QRect(0, top, viewport()->width(),
                  viewportBounds.bottom() - top + 1));
      }
    }
  };

  const QModelIndex anchorIndex =
      model_->indexForStableKey(anchor.stableKey);
  const int finalAnchorRow = anchorIndex.isValid() ? anchorIndex.row() : -1;
  const bool changedAnchor = !anchor.stableKey.empty() &&
                             anchor.stableKey == changedKey;
  if (changedAnchor) {
    damage = damage.united(viewportBounds);
  } else if (sourceRow >= 0 && destinationRow >= 0 &&
             sourceRow != destinationRow) {
    int oldAnchorRow = finalAnchorRow;
    if (finalAnchorRow >= 0 && sourceRow < destinationRow &&
        finalAnchorRow >= sourceRow && finalAnchorRow < destinationRow) {
      ++oldAnchorRow;
    } else if (finalAnchorRow >= 0 && sourceRow > destinationRow &&
               finalAnchorRow > destinationRow &&
               finalAnchorRow <= sourceRow) {
      --oldAnchorRow;
    }
    const bool sourceAboveAnchor =
        oldAnchorRow >= 0 && sourceRow < oldAnchorRow;
    const bool destinationAboveAnchor =
        finalAnchorRow >= 0 && destinationRow < finalAnchorRow;
    if (sourceAboveAnchor && destinationAboveAnchor) {
      // Restoring the stable anchor compensates the complete moved interval.
    } else if (sourceAboveAnchor) {
      addFromRowToBottom(destinationRow);
    } else if (destinationAboveAnchor) {
      addFromRowToBottom(sourceRow);
    } else {
      addInterval(std::min(sourceRow, destinationRow),
                  std::max(sourceRow, destinationRow));
    }
  } else if (sourceRow < 0) {
    if (finalAnchorRow < 0 || destinationRow >= finalAnchorRow)
      addFromRowToBottom(destinationRow);
  } else {
    const bool removedAboveAnchor =
        finalAnchorRow >= 0 && sourceRow <= finalAnchorRow;
    if (!removedAboveAnchor)
      addFromRowToBottom(sourceRow);
    if (!removedAboveAnchor &&
        firstPresentedAtOrAfter(sourceRow) < 0) {
      const qint64 contentBottom = static_cast<qint64>(leadingChromeHeight()) +
                                   heights_.totalHeight() -
                                   verticalScrollBar()->value();
      if (contentBottom >= viewportBounds.top() &&
          contentBottom <= viewportBounds.bottom()) {
        const int top = static_cast<int>(contentBottom);
        damage = damage.united(
            QRect(0, top, viewport()->width(),
                  viewportBounds.bottom() - top + 1));
      }
    }
  }
  damage = damage.intersected(viewportBounds);
  if (!damage.isEmpty()) {
    viewport()->update(damage);
    incrementProperty(this, "targetedStructuralRepaints");
    setProperty("lastTargetedStructuralRepaintHeight", damage.height());
  } else {
    incrementProperty(this, "targetedStructuralOffscreenRepaintsAvoided");
    setProperty("lastTargetedStructuralRepaintHeight", 0);
  }
  incrementProperty(this, "graphRefreshPasses");
  updateMaterializationProperties();
  storeCurrentThreadState();
}

std::size_t ConversationView::historyLimitForThread(
    const std::string &threadId, std::size_t authoritativeItemCount) {
  HistoryWindow &history = historyWindows_[threadId];
  const bool following = modeForThread(threadId) == Mode::Following;
  if (!following &&
      authoritativeItemCount > history.lastAuthoritativeCount) {
    history.effective +=
        authoritativeItemCount - history.lastAuthoritativeCount;
  } else if (following) {
    history.effective = history.requested;
  }
  history.lastAuthoritativeCount = authoritativeItemCount;
  return history.effective;
}

ConversationView::HistoryPageRequest ConversationView::requestNextHistoryPage(
    const std::string &threadId, std::size_t authoritativeItemCount,
    bool providerHasMore) {
  HistoryWindow &history = historyWindows_[threadId];
  const bool retainedHistoryAvailable =
      history.effective < authoritativeItemCount;
  history.requested += AuthoritativeHistoryPageSize;
  history.effective += AuthoritativeHistoryPageSize;
  return {history.effective,
          !retainedHistoryAvailable && providerHasMore};
}

void ConversationView::forgetThreadPresentation(const std::string &threadId) {
  historyWindows_.erase(threadId);
  threadStates_.erase(threadId);
}

bool ConversationView::appendTailCard(ConversationTailCard tail) {
  const std::string threadId = tail.card.threadId;
  HistoryWindow nextHistory = historyWindows_[threadId];
  std::size_t authoritativeItemCount = tail.authoritativeItemCount;
  if (authoritativeItemCount == 0) {
    authoritativeItemCount = std::max(
        nextHistory.lastAuthoritativeCount + 1,
        model_->historyActivityCount() +
            model_->hiddenAuthoritativeItemCount() + 1);
  }
  const bool following = modeForThread(threadId) == Mode::Following;
  if ((!following || nextHistory.effective > nextHistory.requested) &&
      authoritativeItemCount > nextHistory.lastAuthoritativeCount) {
    nextHistory.effective +=
        authoritativeItemCount - nextHistory.lastAuthoritativeCount;
  } else if (following) {
    nextHistory.effective = nextHistory.requested;
  }
  nextHistory.lastAuthoritativeCount = authoritativeItemCount;
  const std::size_t historyActivityLimit = nextHistory.effective;
  if (pendingStructuralSnapshot_ || tail.card.threadId != threadId_ ||
      historyActivityLimit == 0)
    return false;

  const std::string appendedKey = stableKey(tail.card.key);
  if (appendedKey.empty() || model_->indexForStableKey(appendedKey).isValid())
    return false;

  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  const std::size_t hiddenBefore = model_->hiddenAuthoritativeItemCount();
  const bool providerHasMore = tail.providerHasMore;
  const int oldLast = model_->rowCount() - 1;
  std::string oldLastKey;
  std::optional<SectionRange> oldLastSection;
  int oldLastCardHeight = 0;
  bool fragmentsMaterializedRoot = false;
  if (const ConversationItemModel::Row *row = model_->row(oldLast)) {
    oldLastKey = row->stableKey;
    fragmentsMaterializedRoot =
        tail.nested && row->turnRoot && row->sectionKey == tail.sectionKey;
    if (const auto found = sectionRanges_.find(row->sectionKey);
        found != sectionRanges_.end())
      oldLastSection = found->second;
    const int oldExtent = heights_.height(static_cast<std::size_t>(oldLast));
    if (oldExtent > 0)
      oldLastCardHeight = std::max(
          1, oldExtent - rowSpacing(oldLast, oldLastSection ? &*oldLastSection
                                                            : nullptr));
  }

  const bool startsActiveSection = tail.turnRoot && tail.activeTurn;
  const std::string appendedSection = tail.sectionKey;
  const QScopedValueRollback applying(applying_, true);
  const QSignalBlocker scrollSignals(verticalScrollBar());
  stopFollowingAnimation();

  if (!model_->appendTail(std::move(tail)))
    return false;
  historyWindows_.insert_or_assign(threadId, nextHistory);
  const int appendedRow = model_->rowCount() - 1;
  const ConversationItemModel::Row *appended = model_->row(appendedRow);
  if (!appended)
    return false;

  QRect damage;
  if (startsActiveSection && !activeSectionKey_.empty() &&
      activeSectionKey_ != appendedSection) {
    const auto oldActive = sectionRanges_.find(activeSectionKey_);
    if (oldActive != sectionRanges_.end()) {
      oldActive->second.active = false;
      if (const std::optional<int> root =
              modelSectionRow(oldActive->second.root)) {
        static_cast<void>(model_->setActiveTurn(*root, false));
        damage = damage.united(rowRect(*root));
        if (const ConversationItemModel::Row *rootRow = model_->row(*root))
          if (ConversationCard *rootCard = cardForStableKey(rootRow->stableKey))
            configureCardForRow(rootCard, *rootRow);
      }
    }
  }

  if (appended->turnRoot)
    sectionRanges_[appended->sectionKey].root = appended->stableKey;
  if (rowPresented(appendedRow)) {
    SectionRange &range = sectionRanges_[appended->sectionKey];
    if (range.first.empty())
      range.first = appended->stableKey;
    range.last = appended->stableKey;
    range.active = range.active || appended->activeTurn;
  }
  if (startsActiveSection)
    activeSectionKey_ = appendedSection;

  // The first child changes the retained prompt from a complete card into the
  // transparent root fragment of the section-wide Turn surface. Direct-tail
  // insertion must apply that transition to the existing editor before the
  // completed frame is exposed; a later full reconcile may never be needed.
  if (fragmentsMaterializedRoot) {
    const ConversationItemModel::Row *rootRow = model_->row(oldLast);
    ConversationCard *rootCard =
        rootRow ? cardForStableKey(rootRow->stableKey) : nullptr;
    if (rootRow && rootCard) {
      configureCardForRow(rootCard, *rootRow);
      oldLastCardHeight = measureCard(rootCard, rowWidth(*rootRow));
      heightCache_.insert_or_assign(
          rootRow->stableKey,
          HeightRecord{rowWidth(*rootRow), oldLastCardHeight});
      damage = damage.united(rowRect(oldLast));
    }
  }

  // Appending inside a represented turn changes only the previous tail's
  // section edge spacing. Preserve its measured card height exactly, after
  // accounting for the root-fragment margin transition above.
  if (oldLast >= 0 && oldLastCardHeight > 0)
    static_cast<void>(
        heights_.setHeight(static_cast<std::size_t>(oldLast),
                           oldLastCardHeight + rowSpacing(oldLast)));

  int appendedExtent = 0;
  if (rowPresented(appendedRow)) {
    int cardHeight = estimatedCardHeight(appended->card);
    if (rowUsesPassiveDelegate(*appended)) {
      QStyleOptionViewItem option;
      option.initFrom(this);
      option.rect = QRect(0, 0, rowWidth(*appended), 0);
      const auto *delegate =
          static_cast<const ConversationPassiveDelegate *>(itemDelegate());
      cardHeight = delegate
                       ->cardSize(option, model_->index(appendedRow),
                                  rowCollapsed(*appended))
                       .height();
      heightCache_.insert_or_assign(
          appendedKey, HeightRecord{rowWidth(*appended), cardHeight});
    }
    appendedExtent = std::max(1, cardHeight) + rowSpacing(appendedRow);
  }
  const std::array<int, 1> appendedHeights{appendedExtent};
  heights_.insert(heights_.size(), appendedHeights);

  std::size_t hiddenIncrement = 0;
  while (true) {
    const ConversationItemModel::HistoryTrim trim =
        model_->trimHistoryTo(historyActivityLimit);
    hiddenIncrement += trim.hiddenIncrement;
    if (trim.pinnedRoot)
      continue;
    if (trim.count == 0)
      break;

    for (const std::string &key : trim.removedStableKeys) {
      const auto materialized = materializedCards_.find(key);
      if (materialized == materializedCards_.end())
        continue;
      ConversationCard *card = materialized->second;
      materializedCards_.erase(materialized);
      releaseCard(key, card);
    }
    heights_.remove(static_cast<std::size_t>(trim.row),
                    static_cast<std::size_t>(trim.count));

    const ConversationItemModel::Row *newFirst = model_->row(0);
    if (trim.row == 1 && newFirst &&
        newFirst->sectionKey == trim.sectionKey) {
      // The pinned root and the presented section boundaries stay unchanged;
      // only one nested prefix extent was removed.
    } else if (newFirst && newFirst->sectionKey == trim.sectionKey) {
      rebuildSectionRange(trim.sectionKey, 0);
    } else {
      sectionRanges_.erase(trim.sectionKey);
      if (activeSectionKey_ == trim.sectionKey)
        activeSectionKey_.clear();
    }
  }

  model_->setHistoryChrome(hiddenBefore + hiddenIncrement, providerHasMore);
  loadMore_->setVisible(model_->hasMore());
  if (model_->hasMore()) {
    const std::size_t page =
        model_->hiddenAuthoritativeItemCount() == 0
            ? AuthoritativeHistoryPageSize
            : std::min(AuthoritativeHistoryPageSize,
                       model_->hiddenAuthoritativeItemCount());
    loadMore_->setText(QStringLiteral("Load %1 more activities")
                           .arg(static_cast<qulonglong>(page)));
    loadMore_->setToolTip(
        model_->hiddenAuthoritativeItemCount() == 0
            ? QStringLiteral("Earlier activities are available")
            : QStringLiteral("%1 earlier activities are retained")
                  .arg(static_cast<qulonglong>(
                      model_->hiddenAuthoritativeItemCount())));
  }
  empty_->setVisible(model_->rowCount() == 0);

  updateScrollRange();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  updateMaterialization(false);
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  layoutMaterializedCards();

  if (!oldLastKey.empty()) {
    const QModelIndex index = model_->indexForStableKey(oldLastKey);
    if (index.isValid())
      damage = damage.united(rowRect(index.row()));
  }
  const QModelIndex appendedIndex = model_->indexForStableKey(appendedKey);
  if (appendedIndex.isValid())
    damage = damage.united(rowRect(appendedIndex.row()));
  if (!damage.isEmpty())
    viewport()->update(damage.intersected(viewport()->rect()));

  incrementProperty(this, "graphRefreshPasses");
  incrementProperty(this, "targetedStructuralAppends");
  setProperty("conversationHeightIndexUpdateSteps",
              static_cast<qulonglong>(heights_.lastUpdateSteps()));
  updateMaterializationProperties();
  storeCurrentThreadState();
  return true;
}

std::optional<PresentationImpact>
ConversationView::applyCardPresentationOwned(
    VisibleCardData card, nodegraph::NodeRef materializedPrompt) {
  const std::string key = stableKey(card.key);
  if (VisibleCardData *pending = pendingCard(key);
      pending && pendingStructuralSnapshot_ &&
      card.threadId == pendingStructuralSnapshot_->threadId) {
    const auto staged = stagedCards_.find(key);
    if (staged != stagedCards_.end()) {
      if (staged->second->canApply(card)) {
        const PresentationImpact impact =
            staged->second->applyPresentation(card);
        if (impact == PresentationImpact::GeometryChanged) {
          const PendingLocation *location = pendingLocation(key);
          const int width = std::max(
              0, viewport()->width() -
                     (location && location->nested ? 2 * NestedCardIndent : 0));
          stagedHeights_.insert_or_assign(key,
                                          measureCard(staged->second, width));
        }
      } else {
        delete staged->second;
        stagedCards_.erase(staged);
        stagedHeights_.erase(key);
      }
    }
    *pending = card;
    if (!model_->indexForStableKey(key).isValid())
      return PresentationImpact::None;
  }

  if (card.threadId != threadId_)
    return std::nullopt;
  const QModelIndex index = model_->indexForStableKey(key);
  const ConversationItemModel::Row *before = model_->row(index.row());
  if (!index.isValid() || !before)
    return std::nullopt;
  if (before->card == card)
    return PresentationImpact::None;

  const bool wasPresented = rowPresented(index.row());
  const Anchor presentationAnchor = captureAnchor();
  const bool followedBefore = mode_ == Mode::Following;
  const std::string sectionKey = before->sectionKey;
  std::optional<SectionRange> oldSection;
  if (const auto found = sectionRanges_.find(sectionKey);
      found != sectionRanges_.end())
    oldSection = found->second;
  QRect presentationDamage = rowRect(index.row());
  if (oldSection) {
    const std::optional<int> root = modelSectionRow(oldSection->root);
    const std::optional<int> last = modelSectionRow(oldSection->last);
    if (root && last)
      presentationDamage =
          presentationDamage.united(rowRect(*root)).united(rowRect(*last));
  }
  const bool becomingAuthoritative =
      before->card.kind == CardKind::LocalPrompt &&
      card.kind == CardKind::UserMessage && materializedPrompt;
  const bool paintedInViewport =
      wasPresented && rowRect(index.row()).intersects(viewport()->rect());
  nodegraph::NodeRef acknowledgementTarget =
      becomingAuthoritative ? std::move(materializedPrompt)
                            : nodegraph::NodeRef{};
  ConversationCard *visibleCard = cardForStableKey(key);
  PresentationImpact impact = PresentationImpact::None;
  if (visibleCard) {
    if (!visibleCard->canApply(card))
      return std::nullopt;
    captureCardInteractionState(key, visibleCard, false);
    impact = visibleCard->applyPresentation(card);
    restoreCardInteractionState(key, visibleCard);
  }

  const ConversationItemModel::CardUpdateResult result =
      model_->updateCard(std::move(card));
  if (result == ConversationItemModel::CardUpdateResult::Missing ||
      result == ConversationItemModel::CardUpdateResult::Incompatible)
    return std::nullopt;
  if (result == ConversationItemModel::CardUpdateResult::Unchanged)
    return PresentationImpact::None;

  const ConversationItemModel::Row *after = model_->row(index.row());
  const bool presentationChanged =
      after && rowPresented(index.row()) != wasPresented;
  if (presentationChanged) {
    heightCache_.erase(key);
    updateSectionRangeForPresentationChange(index.row(), wasPresented);
    const auto nextSectionFound = sectionRanges_.find(sectionKey);
    const SectionRange *nextSection = nextSectionFound == sectionRanges_.end()
                                          ? nullptr
                                          : &nextSectionFound->second;

    std::unordered_set<int> affectedRows{index.row()};
    if (oldSection) {
      if (const std::optional<int> row = modelSectionRow(oldSection->root))
        affectedRows.insert(*row);
      if (const std::optional<int> row = modelSectionRow(oldSection->last))
        affectedRows.insert(*row);
    }
    if (nextSection) {
      if (const std::optional<int> row = modelSectionRow(nextSection->root))
        affectedRows.insert(*row);
      if (const std::optional<int> row = modelSectionRow(nextSection->last))
        affectedRows.insert(*row);
    }

    for (const int affectedRow : affectedRows) {
      const ConversationItemModel::Row *affected = model_->row(affectedRow);
      if (!affected || affected->sectionKey != sectionKey)
        continue;
      if (!rowPresented(affectedRow)) {
        static_cast<void>(
            heights_.setHeight(static_cast<std::size_t>(affectedRow), 0));
        continue;
      }

      const int previousExtent =
          heights_.height(static_cast<std::size_t>(affectedRow));
      int cardHeight = 0;
      if (previousExtent > 0) {
        cardHeight =
            std::max(1, previousExtent -
                            rowSpacing(affectedRow,
                                       oldSection ? &*oldSection : nullptr));
      } else if (const auto cached = heightCache_.find(affected->stableKey);
                 cached != heightCache_.end() &&
                 cached->second.width == rowWidth(*affected)) {
        cardHeight = cached->second.height;
      } else {
        cardHeight = estimatedCardHeight(affected->card);
      }
      static_cast<void>(heights_.setHeight(
          static_cast<std::size_t>(affectedRow),
          std::max(1, cardHeight) + rowSpacing(affectedRow, nextSection)));
    }

    if (!rowPresented(index.row()) && visibleCard) {
      materializedCards_.erase(key);
      releaseCard(key, visibleCard);
      visibleCard = nullptr;
    }

    for (const int affectedRow : affectedRows) {
      const ConversationItemModel::Row *affected = model_->row(affectedRow);
      if (!affected || !rowPresented(affectedRow) || !affected->turnRoot)
        continue;
      ConversationCard *rootCard = cardForStableKey(affected->stableKey);
      if (!rootCard)
        continue;
      configureCardForRow(rootCard, *affected);
      const int height = measureCard(rootCard, rowWidth(*affected));
      heightCache_.insert_or_assign(affected->stableKey,
                                    HeightRecord{rowWidth(*affected), height});
      static_cast<void>(
          heights_.setHeight(static_cast<std::size_t>(affectedRow),
                             height + rowSpacing(affectedRow, nextSection)));
    }

    impact = PresentationImpact::GeometryChanged;
    incrementProperty(this, "conversationLocalGeometryPasses");
    setProperty("conversationHeightIndexUpdateSteps",
                static_cast<qulonglong>(heights_.lastUpdateSteps()));
    updateScrollRange();
    if (followedBefore)
      setScrollValue(verticalScrollBar()->maximum());
    else
      restoreAnchor(presentationAnchor);
    updateMaterialization(false);
    if (followedBefore)
      setScrollValue(verticalScrollBar()->maximum());
    else
      restoreAnchor(presentationAnchor);
    if (nextSection) {
      const std::optional<int> root = modelSectionRow(nextSection->root);
      const std::optional<int> last = modelSectionRow(nextSection->last);
      if (root && last)
        presentationDamage =
            presentationDamage.united(rowRect(*root)).united(rowRect(*last));
    }
    const int damageTop =
        std::clamp(presentationDamage.top(), 0, viewport()->height());
    viewport()->update(QRect(0, damageTop, viewport()->width(),
                             viewport()->height() - damageTop));
  } else if (visibleCard && impact == PresentationImpact::GeometryChanged) {
    const int height = measureCard(visibleCard, rowWidth(*after));
    static_cast<void>(updateMeasuredHeight(index.row(), height, true));
  } else if (visibleCard && impact == PresentationImpact::PaintOnly) {
    visibleCard->update();
  } else if (after && paintedInViewport && rowUsesPassiveDelegate(*after)) {
    heightCache_.erase(key);
    QStyleOptionViewItem option;
    option.initFrom(this);
    option.rect = QRect(0, 0, rowWidth(*after), 0);
    const auto *delegate =
        static_cast<const ConversationPassiveDelegate *>(itemDelegate());
    const int height =
        delegate->cardSize(option, index, rowCollapsed(*after)).height();
    impact = updateMeasuredHeight(index.row(), height, true)
                 ? PresentationImpact::GeometryChanged
                 : PresentationImpact::PaintOnly;
    viewport()->update(rowRect(index.row()));
  }

  if (visibleCard || paintedInViewport)
    incrementProperty(this, "targetedVisibleCardUpdates");
  else
    incrementProperty(this, "targetedOffscreenCardUpdates");
  incrementProperty(this, "graphRefreshPasses");
  incrementProperty(this, "targetedCardCommits");
  storeCurrentThreadState();
  if (becomingAuthoritative && promptMaterializedAction_)
    static_cast<void>(
        promptMaterializedAction_(std::move(acknowledgementTarget)));
  return impact;
}

int ConversationView::estimatedCardHeight(const VisibleCardData &card) const {
  const std::string key = stableKey(card.key);
  bool collapsed = false;
  if (const auto retained = cardCollapsedStates_.find(key);
      retained != cardCollapsedStates_.end()) {
    collapsed = retained->second;
  } else if (card.kind == CardKind::CommandExecution) {
    collapsed = !presentationOptions_.commandsInitiallyExpanded;
  } else if (card.kind == CardKind::ImageGeneration) {
    collapsed = !presentationOptions_.imagesInitiallyExpanded;
  } else if (card.kind == CardKind::FileChanges) {
    collapsed = !presentationOptions_.fileChangesInitiallyExpanded;
  } else {
    collapsed = passiveWhenCollapsed(card.kind);
  }
  if (collapsed && card.kind != CardKind::LocalPrompt)
    return 44;
  switch (card.kind) {
  case CardKind::CommandExecution:
    return 156;
  case CardKind::FileChanges:
  case CardKind::ImageGeneration:
  case CardKind::Plan:
    return 136;
  case CardKind::UserMessage:
  case CardKind::LocalPrompt:
    return 92;
  default:
    return EstimatedCardHeight;
  }
}

int ConversationView::rowWidth(const ConversationItemModel::Row &row) const {
  return std::max(0, viewport()->width() -
                         (row.nested ? 2 * NestedCardIndent : 0));
}

bool ConversationView::rowUsesPassiveDelegate(
    const ConversationItemModel::Row &row) const {
  return eligibleForPassivePresentation(row.card, rowCollapsed(row));
}

bool ConversationView::rowCollapsed(
    const ConversationItemModel::Row &row) const {
  if (const auto retained = cardCollapsedStates_.find(row.stableKey);
      retained != cardCollapsedStates_.end())
    return retained->second;
  if (row.card.kind == CardKind::CommandExecution)
    return !presentationOptions_.commandsInitiallyExpanded;
  if (row.card.kind == CardKind::ImageGeneration)
    return !presentationOptions_.imagesInitiallyExpanded;
  if (row.card.kind == CardKind::FileChanges)
    return !presentationOptions_.fileChangesInitiallyExpanded;
  return row.card.kind != CardKind::UserMessage &&
         row.card.kind != CardKind::AgentMessage &&
         row.card.kind != CardKind::LocalPrompt;
}

bool ConversationView::rowPresented(int rowIndex) const {
  const ConversationItemModel::Row *row = model_->row(rowIndex);
  if (!row || !row->presented)
    return false;
  if (!row->nested)
    return true;
  const auto section = sectionRanges_.find(row->sectionKey);
  if (section == sectionRanges_.end() || section->second.root.empty())
    return true;
  const std::optional<int> rootIndex = modelSectionRow(section->second.root);
  const ConversationItemModel::Row *rootRow =
      rootIndex ? model_->row(*rootIndex) : nullptr;
  return !rootRow || !rowCollapsed(*rootRow);
}

void ConversationView::rebuildSectionRanges() {
  activeSectionKey_.clear();
  sectionRanges_.clear();
  sectionRanges_.reserve(static_cast<std::size_t>(model_->rowCount()));
  for (int rowIndex = 0; rowIndex < model_->rowCount(); ++rowIndex) {
    const ConversationItemModel::Row *row = model_->row(rowIndex);
    if (!row)
      continue;
    if (row->turnRoot)
      sectionRanges_[row->sectionKey].root = row->stableKey;
  }
  for (int rowIndex = 0; rowIndex < model_->rowCount(); ++rowIndex) {
    const ConversationItemModel::Row *row = model_->row(rowIndex);
    if (!row || !rowPresented(rowIndex))
      continue;
    SectionRange &range = sectionRanges_[row->sectionKey];
    if (range.first.empty())
      range.first = row->stableKey;
    range.last = row->stableKey;
    range.active = range.active || (row->turnRoot && row->activeTurn);
    if (row->turnRoot && row->activeTurn)
      activeSectionKey_ = row->sectionKey;
  }
  incrementProperty(this, "conversationSectionRangeRebuilds");
}

void ConversationView::rebuildSectionRange(const std::string &sectionKey,
                                           int nearRow) {
  if (sectionKey.empty())
    return;
  if (nearRow < 0 || nearRow >= model_->rowCount() ||
      model_->row(nearRow)->sectionKey != sectionKey) {
    const auto retained = sectionRanges_.find(sectionKey);
    const std::optional<int> retainedRow =
        retained == sectionRanges_.end()
            ? std::nullopt
            : modelSectionRow(!retained->second.first.empty()
                                  ? retained->second.first
                                  : retained->second.root);
    if (!retainedRow) {
      sectionRanges_.erase(sectionKey);
      if (activeSectionKey_ == sectionKey)
        activeSectionKey_.clear();
      return;
    }
    nearRow = *retainedRow;
  }

  int first = nearRow;
  while (first > 0) {
    const ConversationItemModel::Row *candidate = model_->row(first - 1);
    if (!candidate || candidate->sectionKey != sectionKey)
      break;
    --first;
  }
  SectionRange replacement;
  int end = first;
  for (; end < model_->rowCount(); ++end) {
    const ConversationItemModel::Row *candidate = model_->row(end);
    if (!candidate || candidate->sectionKey != sectionKey)
      break;
    if (candidate->turnRoot)
      replacement.root = candidate->stableKey;
  }
  const std::optional<int> rootRow = modelSectionRow(replacement.root);
  const ConversationItemModel::Row *root =
      rootRow ? model_->row(*rootRow) : nullptr;
  const bool childrenPresented = !root || !rowCollapsed(*root);
  for (int candidateIndex = first; candidateIndex < end; ++candidateIndex) {
    const ConversationItemModel::Row *candidate = model_->row(candidateIndex);
    if (!candidate->presented || (candidate->nested && !childrenPresented))
      continue;
    if (replacement.first.empty())
      replacement.first = candidate->stableKey;
    replacement.last = candidate->stableKey;
    replacement.active =
        replacement.active || (candidate->turnRoot && candidate->activeTurn);
  }
  if (replacement.first.empty() && replacement.root.empty()) {
    sectionRanges_.erase(sectionKey);
    if (activeSectionKey_ == sectionKey)
      activeSectionKey_.clear();
  } else {
    if (replacement.active)
      activeSectionKey_ = sectionKey;
    else if (activeSectionKey_ == sectionKey)
      activeSectionKey_.clear();
    sectionRanges_.insert_or_assign(sectionKey, std::move(replacement));
  }
  incrementProperty(this, "conversationSectionRangeLocalUpdates");
}

void ConversationView::updateSectionRangeForPresentationChange(
    int rowIndex, bool wasPresented) {
  const ConversationItemModel::Row *changed = model_->row(rowIndex);
  if (!changed || rowPresented(rowIndex) == wasPresented)
    return;

  if (rowPresented(rowIndex)) {
    SectionRange &range = sectionRanges_[changed->sectionKey];
    const std::optional<int> first = modelSectionRow(range.first);
    const std::optional<int> last = modelSectionRow(range.last);
    if (!first || rowIndex < *first)
      range.first = changed->stableKey;
    if (!last || rowIndex > *last)
      range.last = changed->stableKey;
    if (changed->turnRoot)
      range.root = changed->stableKey;
    range.active = range.active || (changed->turnRoot && changed->activeTurn);
    return;
  }

  // Hiding a targeted row is uncommon (global visibility changes use the
  // structural rebuild path). Recompute only its canonical turn, never the
  // loaded conversation.
  rebuildSectionRange(changed->sectionKey, rowIndex);
}

int ConversationView::rowSpacing(int rowIndex) const {
  const ConversationItemModel::Row *row = model_->row(rowIndex);
  if (!row)
    return CardSpacing;
  const auto found = sectionRanges_.find(row->sectionKey);
  return rowSpacing(rowIndex,
                    found == sectionRanges_.end() ? nullptr : &found->second);
}

int ConversationView::rowSpacing(int rowIndex,
                                 const SectionRange *section) const {
  if (!section || section->root.empty() || section->last.empty())
    return CardSpacing;
  const std::optional<int> root = modelSectionRow(section->root);
  const std::optional<int> last = modelSectionRow(section->last);
  if (!root || !last || *last <= *root)
    return CardSpacing;
  if (rowIndex == *root)
    return 14;
  if (rowIndex == *last)
    return CardSpacing + 10;
  return CardSpacing;
}

std::optional<int>
ConversationView::modelSectionRow(const std::string &stableKey) const {
  if (stableKey.empty())
    return std::nullopt;
  const QModelIndex index = model_->indexForStableKey(stableKey);
  return index.isValid() ? std::optional<int>(index.row()) : std::nullopt;
}

void ConversationView::rebuildHeightIndex() {
  std::vector<int> extents;
  extents.reserve(static_cast<std::size_t>(model_->rowCount()));
  for (int rowIndex = 0; rowIndex < model_->rowCount(); ++rowIndex) {
    const ConversationItemModel::Row *row = model_->row(rowIndex);
    if (!row || !rowPresented(rowIndex)) {
      extents.push_back(0);
      continue;
    }
    const int width = rowWidth(*row);
    int height = 0;
    if (const auto staged = stagedHeights_.find(row->stableKey);
        staged != stagedHeights_.end()) {
      height = staged->second;
      heightCache_.insert_or_assign(row->stableKey,
                                    HeightRecord{width, height});
    } else if (const auto cached = heightCache_.find(row->stableKey);
               cached != heightCache_.end() && cached->second.width == width) {
      height = cached->second.height;
    } else {
      height = estimatedCardHeight(row->card);
    }
    extents.push_back(std::max(1, height) + rowSpacing(rowIndex));
  }
  heights_.assign(extents);
  setProperty("conversationHeightIndexRebuilds",
              static_cast<qulonglong>(heights_.rebuildCount()));
}

int ConversationView::leadingChromeHeight() const noexcept {
  if (!model_)
    return 0;
  if (model_->hasMore())
    return HistoryButtonHeight + CardSpacing;
  if (model_->rowCount() == 0 && empty_)
    return std::max(28, empty_->sizeHint().height()) + CardSpacing;
  return 0;
}

qint64 ConversationView::naturalContentHeight() const noexcept {
  return static_cast<qint64>(leadingChromeHeight()) + heights_.totalHeight() +
         trailingSpaceHeight_;
}

void ConversationView::updateScrollRange() {
  if (!model_ || !viewport())
    return;
  const int viewportHeight = std::max(0, viewport()->height());
  const qint64 maximum64 =
      std::max<qint64>(0, naturalContentHeight() - viewportHeight);
  const int maximum = static_cast<int>(std::min<qint64>(INT_MAX, maximum64));
  verticalScrollBar()->setPageStep(viewportHeight);
  verticalScrollBar()->setRange(0, maximum);
  horizontalScrollBar()->setPageStep(viewport()->width());
  horizontalScrollBar()->setRange(0, 0);

  const int leading = leadingChromeHeight();
  if (model_->hasMore() && loadMore_) {
    const int width = std::min(std::max(180, loadMore_->sizeHint().width()),
                               std::max(0, viewport()->width()));
    loadMore_->setGeometry(
        (viewport()->width() - width) / 2 - horizontalScrollBar()->value(),
        -verticalScrollBar()->value(), width, HistoryButtonHeight);
  }
  if (model_->rowCount() == 0 && empty_) {
    empty_->setGeometry(0, -verticalScrollBar()->value(),
                        std::max(0, viewport()->width()),
                        std::max(0, leading - CardSpacing));
  }
  if (stagingOverlay_)
    stagingOverlay_->setGeometry(viewport()->rect());
}

QRect ConversationView::rowRect(int rowIndex) const {
  const ConversationItemModel::Row *row = model_->row(rowIndex);
  if (!row || !rowPresented(rowIndex) || rowIndex < 0 ||
      static_cast<std::size_t>(rowIndex) >= heights_.size())
    return {};
  const int extent = heights_.height(static_cast<std::size_t>(rowIndex));
  if (extent <= 0)
    return {};
  const qint64 contentTop = static_cast<qint64>(leadingChromeHeight()) +
                            heights_.top(static_cast<std::size_t>(rowIndex));
  const qint64 viewportTop = contentTop - verticalScrollBar()->value();
  const int x = row->nested ? NestedCardIndent : 0;
  return {x - horizontalScrollBar()->value(),
          static_cast<int>(std::clamp<qint64>(viewportTop, INT_MIN, INT_MAX)),
          rowWidth(*row), std::max(1, extent - rowSpacing(rowIndex))};
}

int ConversationView::measureCard(ConversationCard *card, int width) const {
  if (!card || !card->layout())
    return 0;
  width = std::max(1, width);
  card->setMinimumHeight(0);
  card->setMaximumHeight(QWIDGETSIZE_MAX);
  card->resize(width, std::max(1, card->height()));
  if (QWidget *content =
          card->findChild<QWidget *>(QStringLiteral("conversationCardContent"),
                                     Qt::FindDirectChildrenOnly);
      content && content->layout()) {
    content->layout()->invalidate();
    content->layout()->setGeometry(content->contentsRect());
    content->layout()->activate();
  }
  card->layout()->invalidate();
  card->layout()->setGeometry(card->contentsRect());
  card->layout()->activate();
  const int height =
      card->layout()->hasHeightForWidth()
          ? card->layout()->heightForWidth(width) + 2 * card->frameWidth()
          : card->sizeHint().height();
  card->setFixedHeight(std::max(1, height));
  card->resize(width, std::max(1, height));
  card->layout()->setGeometry(card->contentsRect());
  card->layout()->activate();
  QCoreApplication::removePostedEvents(card, QEvent::LayoutRequest);
  return std::max(1, height);
}

bool ConversationView::updateMeasuredHeight(int rowIndex, int cardHeight,
                                            bool preserveAnchor) {
  const ConversationItemModel::Row *row = model_->row(rowIndex);
  if (!row || !rowPresented(rowIndex))
    return false;
  const Anchor anchor = preserveAnchor ? captureAnchor() : Anchor{};
  const bool follow = mode_ == Mode::Following;
  heightCache_.insert_or_assign(row->stableKey,
                                HeightRecord{rowWidth(*row), cardHeight});
  if (!heights_.setHeight(static_cast<std::size_t>(rowIndex),
                          std::max(1, cardHeight) + rowSpacing(rowIndex)))
    return false;
  incrementProperty(this, "conversationLocalGeometryPasses");
  setProperty("conversationHeightIndexUpdateSteps",
              static_cast<qulonglong>(heights_.lastUpdateSteps()));
  updateScrollRange();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else if (preserveAnchor)
    restoreAnchor(anchor);
  layoutMaterializedCards();
  return true;
}

std::pair<int, int> ConversationView::materializationRows() const {
  if (model_->rowCount() == 0 || heights_.empty() ||
      heights_.totalHeight() <= 0)
    return {-1, -1};
  const qint64 scroll = verticalScrollBar()->value();
  const qint64 viewportHeight = std::max(1, viewport()->height());
  const qint64 contentStart =
      std::max<qint64>(0, scroll - leadingChromeHeight() - viewportHeight);
  const qint64 contentEnd =
      std::min<qint64>(heights_.totalHeight() - 1,
                       scroll - leadingChromeHeight() + 2 * viewportHeight);
  if (contentEnd < contentStart)
    return {-1, -1};
  const int first = static_cast<int>(heights_.rowAt(contentStart));
  const int last = static_cast<int>(heights_.rowAt(contentEnd));
  return {std::max(0, first), std::min(model_->rowCount() - 1, last)};
}

ConversationCard *ConversationView::createCard(const VisibleCardData &data,
                                               QWidget *parent,
                                               const std::string &key) {
  ConversationCard *card = createConversationCard(
      data, parent, !presentationOptions_.commandsInitiallyExpanded,
      !presentationOptions_.imagesInitiallyExpanded,
      !presentationOptions_.fileChangesInitiallyExpanded);
  card->setProperty("conversationAnchorKey", QString::fromStdString(key));
  if (const auto collapsed = cardCollapsedStates_.find(key);
      collapsed != cardCollapsedStates_.end())
    card->setCollapsed(collapsed->second);
  if (const auto output = commandOutputStates_.find(key);
      output != commandOutputStates_.end())
    card->restoreCommandOutputScrollState(output->second);
  restoreCardInteractionState(key, card);
  card->installEventFilter(this);
  for (QWidget *child : card->findChildren<QWidget *>())
    child->installEventFilter(this);
  connect(card, &ConversationCard::foldRequested, this,
          [this, key, card](bool collapsed) {
            const auto retained = materializedCards_.find(key);
            if (retained != materializedCards_.end() &&
                retained->second == card)
              setCardCollapsed(key, card, collapsed);
          });
  connect(card, &ConversationCard::recoveryRequested, this, [this, key, card] {
    const auto retained = materializedCards_.find(key);
    if (retained == materializedCards_.end() || retained->second != card ||
        !promptRecoveryAction_ || !card->data().target)
      return;
    promptRecoveryAction_(card->data().target);
  });
  return card;
}

void ConversationView::configureCardForRow(
    ConversationCard *card, const ConversationItemModel::Row &row) {
  if (!card)
    return;
  const auto section = sectionRanges_.find(row.sectionKey);
  const std::optional<int> root =
      section == sectionRanges_.end()
          ? std::nullopt
          : modelSectionRow(section->second.root);
  const std::optional<int> last =
      section == sectionRanges_.end()
          ? std::nullopt
          : modelSectionRow(section->second.last);
  const bool fragmentedRoot =
      row.turnRoot && root && last && *last > *root;
  card->setProperty("turnContainer", row.turnRoot);
  card->setNestedPresentation(row.nested);
  card->setVirtualTurnRootPresentation(fragmentedRoot);
  card->setAuthoritativeTurnActive(row.turnRoot && row.activeTurn &&
                                   !fragmentedRoot);
}

ConversationCard *ConversationView::materializeRow(int rowIndex,
                                                   bool forInteraction) {
  const ConversationItemModel::Row *row = model_->row(rowIndex);
  if (!row || !rowPresented(rowIndex))
    return nullptr;
  if (ConversationCard *retained = cardForStableKey(row->stableKey))
    return retained;

  if (!forInteraction && rowUsesPassiveDelegate(*row)) {
    QStyleOptionViewItem option;
    option.initFrom(this);
    option.rect = QRect(0, 0, rowWidth(*row), 0);
    const auto *delegate =
        static_cast<const ConversationPassiveDelegate *>(itemDelegate());
    const int height =
        delegate->cardSize(option, model_->index(rowIndex), rowCollapsed(*row))
            .height();
    heightCache_.insert_or_assign(row->stableKey,
                                  HeightRecord{rowWidth(*row), height});
    static_cast<void>(heights_.setHeight(static_cast<std::size_t>(rowIndex),
                                         height + rowSpacing(rowIndex)));
    return nullptr;
  }

  ConversationCard *card = nullptr;
  if (const auto staged = stagedCards_.find(row->stableKey);
      staged != stagedCards_.end() && staged->second->canApply(row->card)) {
    card = staged->second;
    stagedCards_.erase(staged);
    stagedHeights_.erase(row->stableKey);
  } else {
    card = createCard(row->card, stagingHost_, row->stableKey);
    incrementProperty(this, "conversationCardConstructions");
  }
  card->hide();
  card->setParent(viewport());
  configureCardForRow(card, *row);
  const int height = measureCard(card, rowWidth(*row));
  heightCache_.insert_or_assign(row->stableKey,
                                HeightRecord{rowWidth(*row), height});
  static_cast<void>(heights_.setHeight(static_cast<std::size_t>(rowIndex),
                                       height + rowSpacing(rowIndex)));
  materializedCards_.emplace(row->stableKey, card);
  card->setGeometry(rowRect(rowIndex));
  card->show();
  if (const auto output = commandOutputStates_.find(row->stableKey);
      output != commandOutputStates_.end())
    card->restoreCommandOutputScrollState(output->second);
  restoreCardInteractionState(row->stableKey, card);
  incrementProperty(this, "conversationRowsMaterialized");
  return card;
}

void ConversationView::releaseCard(const std::string &key,
                                   ConversationCard *card) {
  if (!card)
    return;
  captureCardInteractionState(key, card, true);
  cardCollapsedStates_.insert_or_assign(key, card->isCollapsed());
  if (const auto state = card->commandOutputScrollState())
    commandOutputStates_.insert_or_assign(key, *state);
  card->setViewportVisible(false);
  delete card;
  incrementProperty(this, "conversationRowsReleased");
}

void ConversationView::captureCardInteractionState(
    const std::string &key, ConversationCard *card,
    bool preserveExistingWhenEmpty) {
  if (!card)
    return;
  CardInteractionState state;
  const auto labels = card->findChildren<QLabel *>();
  for (int ordinal = 0; ordinal < labels.size(); ++ordinal) {
    QLabel *label = labels.at(ordinal);
    if (!label->hasSelectedText())
      continue;
    state.labels.push_back({ordinal, label->selectionStart(),
                            static_cast<int>(label->selectedText().size())});
  }
  const auto edits = card->findChildren<QTextEdit *>();
  for (int ordinal = 0; ordinal < edits.size(); ++ordinal) {
    const QTextCursor cursor = edits.at(ordinal)->textCursor();
    if (!cursor.hasSelection())
      continue;
    state.edits.push_back({ordinal, cursor.position(), cursor.anchor()});
  }
  if (!state.labels.empty() || !state.edits.empty())
    cardInteractionStates_.insert_or_assign(key, std::move(state));
  else if (!preserveExistingWhenEmpty)
    cardInteractionStates_.erase(key);
}

void ConversationView::restoreCardInteractionState(const std::string &key,
                                                   ConversationCard *card) {
  if (!card)
    return;
  const auto retained = cardInteractionStates_.find(key);
  if (retained == cardInteractionStates_.end())
    return;
  const auto labels = card->findChildren<QLabel *>();
  for (const LabelSelection &selection : retained->second.labels) {
    if (selection.ordinal < 0 || selection.ordinal >= labels.size())
      continue;
    labels.at(selection.ordinal)
        ->setSelection(selection.start, selection.length);
  }
  const auto edits = card->findChildren<QTextEdit *>();
  for (const EditSelection &selection : retained->second.edits) {
    if (selection.ordinal < 0 || selection.ordinal >= edits.size())
      continue;
    QTextEdit *edit = edits.at(selection.ordinal);
    const int maximum = std::max(0, edit->document()->characterCount() - 1);
    QTextCursor cursor(edit->document());
    cursor.setPosition(std::clamp(selection.anchor, 0, maximum));
    cursor.setPosition(std::clamp(selection.position, 0, maximum),
                       QTextCursor::KeepAnchor);
    edit->setTextCursor(cursor);
  }
}

void ConversationView::releaseUnneededCards(int firstRow, int lastRow) {
  std::unordered_set<std::string> retainedKeys;
  if (firstRow >= 0 && lastRow >= firstRow) {
    retainedKeys.reserve(static_cast<std::size_t>(lastRow - firstRow + 1));
    for (int rowIndex = firstRow; rowIndex <= lastRow; ++rowIndex)
      if (const auto *row = model_->row(rowIndex);
          row && rowPresented(rowIndex))
        retainedKeys.insert(row->stableKey);
  }

  QWidget *focused = QApplication::focusWidget();
  std::vector<std::string> removeKeys;
  for (const auto &[key, card] : materializedCards_) {
    const bool ownsFocus =
        focused && (focused == card || card->isAncestorOf(focused));
    if (!retainedKeys.contains(key) && !ownsFocus)
      removeKeys.push_back(key);
  }
  for (const std::string &key : removeKeys) {
    const auto found = materializedCards_.find(key);
    if (found == materializedCards_.end())
      continue;
    ConversationCard *card = found->second;
    materializedCards_.erase(found);
    releaseCard(key, card);
  }
}

void ConversationView::releaseAllCards() {
  for (auto &[key, card] : materializedCards_)
    releaseCard(key, card);
  materializedCards_.clear();
  updateMaterializationProperties();
}

void ConversationView::updateMaterialization(bool preserveAnchor) {
  if (materializing_)
    return;
  const QScopedValueRollback materializing(materializing_, true);
  const Anchor anchor = preserveAnchor ? captureAnchor() : Anchor{};
  const bool follow = mode_ == Mode::Following;

  for (int pass = 0; pass < 2; ++pass) {
    const auto [first, last] = materializationRows();
    const qint64 totalBefore = heights_.totalHeight();
    if (first >= 0)
      for (int rowIndex = first; rowIndex <= last; ++rowIndex)
        static_cast<void>(materializeRow(rowIndex));
    if (heights_.totalHeight() == totalBefore)
      break;
    updateScrollRange();
    if (follow)
      setScrollValue(verticalScrollBar()->maximum());
    else if (preserveAnchor)
      restoreAnchor(anchor);
  }
  const auto [first, last] = materializationRows();
  releaseUnneededCards(first, last);
  updateScrollRange();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else if (preserveAnchor)
    restoreAnchor(anchor);
  layoutMaterializedCards();
  updateMaterializationProperties();
}

void ConversationView::layoutMaterializedCards() {
  const QRect visibleRect = viewport()->rect();
  for (auto &[key, card] : materializedCards_) {
    const QModelIndex index = model_->indexForStableKey(key);
    if (!index.isValid())
      continue;
    const QRect geometry = rowRect(index.row());
    card->setGeometry(geometry);
    card->setViewportVisible(geometry.intersects(visibleRect));
  }
}

void ConversationView::updateMaterializationProperties() {
  const qulonglong count = static_cast<qulonglong>(materializedCards_.size());
  setProperty("conversationMaterializedCardCount", count);
  setProperty(
      "conversationMaterializedCardPeak",
      std::max(property("conversationMaterializedCardPeak").toULongLong(),
               count));
}

ConversationCard *
ConversationView::cardForStableKey(const std::string &key) const {
  const auto found = materializedCards_.find(key);
  return found == materializedCards_.end() ? nullptr : found->second;
}

void ConversationView::setCardCollapsed(const std::string &key,
                                        ConversationCard *card,
                                        bool collapsed) {
  if (!card || card->isCollapsed() == collapsed)
    return;
  const QModelIndex index = model_->indexForStableKey(key);
  if (!index.isValid())
    return;
  Anchor anchor = captureAnchor();
  anchor.stableKey = key;
  anchor.pixelOffset = rowRect(index.row()).top();
  mode_ = Mode::Paused;
  pausedByComposerGrowth_ = false;
  stopFollowingAnimation();
  cardCollapsedStates_.insert_or_assign(key, collapsed);
  card->setCollapsed(collapsed);
  const ConversationItemModel::Row *row = model_->row(index.row());
  if (!row)
    return;

  if (row->turnRoot) {
    SectionRange replacement;
    int first = index.row();
    while (first > 0) {
      const ConversationItemModel::Row *candidate = model_->row(first - 1);
      if (!candidate || candidate->sectionKey != row->sectionKey)
        break;
      --first;
    }
    int last = first;
    for (; last < model_->rowCount(); ++last) {
      const ConversationItemModel::Row *candidate = model_->row(last);
      if (!candidate || candidate->sectionKey != row->sectionKey)
        break;
      if (!rowPresented(last))
        continue;
      if (replacement.first.empty())
        replacement.first = candidate->stableKey;
      replacement.last = candidate->stableKey;
      if (candidate->turnRoot)
        replacement.root = candidate->stableKey;
      replacement.active = replacement.active ||
                           (candidate->turnRoot && candidate->activeTurn);
    }
    if (replacement.first.empty() && replacement.root.empty())
      sectionRanges_.erase(row->sectionKey);
    else
      sectionRanges_.insert_or_assign(row->sectionKey, replacement);

    const SectionRange *section = nullptr;
    if (const auto found = sectionRanges_.find(row->sectionKey);
        found != sectionRanges_.end())
      section = &found->second;
    for (int affectedRow = first; affectedRow < last; ++affectedRow) {
      const ConversationItemModel::Row *affected = model_->row(affectedRow);
      if (!affected)
        continue;
      if (!rowPresented(affectedRow)) {
        static_cast<void>(
            heights_.setHeight(static_cast<std::size_t>(affectedRow), 0));
        continue;
      }
      int cardHeight = estimatedCardHeight(affected->card);
      if (const auto cached = heightCache_.find(affected->stableKey);
          cached != heightCache_.end() &&
          cached->second.width == rowWidth(*affected))
        cardHeight = cached->second.height;
      static_cast<void>(heights_.setHeight(
          static_cast<std::size_t>(affectedRow),
          std::max(1, cardHeight) + rowSpacing(affectedRow, section)));
    }
    configureCardForRow(card, *row);
    const int rootHeight = measureCard(card, rowWidth(*row));
    heightCache_.insert_or_assign(
        key, HeightRecord{rowWidth(*row), rootHeight});
    static_cast<void>(heights_.setHeight(
        static_cast<std::size_t>(index.row()),
        rootHeight + rowSpacing(index.row(), section)));
    incrementProperty(this, "conversationLocalGeometryPasses");
    setProperty("conversationHeightIndexUpdateSteps",
                static_cast<qulonglong>(heights_.lastUpdateSteps()));
    updateScrollRange();
    restoreAnchor(anchor);
    updateMaterialization(false);
    restoreAnchor(anchor);
    layoutMaterializedCards();
    viewport()->update();
  } else {
    const int height = measureCard(card, rowWidth(*row));
    static_cast<void>(updateMeasuredHeight(index.row(), height, false));
  }
  restoreAnchor(anchor);
  if (!collapsed) {
    const QRect expanded = rowRect(index.row());
    const int availableBottom =
        std::max(0, viewport()->height() - trailingSpaceHeight_ - 1);
    if (!expanded.isEmpty() && expanded.bottom() > availableBottom)
      setScrollValue(verticalScrollBar()->value() + expanded.bottom() -
                     availableBottom);
  }
  layoutMaterializedCards();
  storeCurrentThreadState();
}

void ConversationView::setTrailingSpaceHeight(int height) {
  height = std::max(0, height);
  if (height == trailingSpaceHeight_)
    return;
  const bool grew = height > trailingSpaceHeight_;
  const Anchor anchor = captureAnchor();
  const int previousValue = verticalScrollBar()->value();
  stopFollowingAnimation();
  if (grew) {
    pausedByComposerGrowth_ =
        pausedByComposerGrowth_ || mode_ == Mode::Following;
    mode_ = Mode::Paused;
  }
  trailingSpaceHeight_ = height;
  verticalScrollBar()->setProperty("composerBottomInset", height);
  verticalScrollBar()->setStyleSheet(
      height == 0
          ? QString{}
          : QStringLiteral("QScrollBar:vertical{margin:2px 2px %1px 2px;}")
                .arg(height + 2));
  updateScrollRange();
  if (mode_ == Mode::Following)
    setScrollValue(verticalScrollBar()->maximum());
  else if (grew && anchor.stableKey.empty())
    setScrollValue(std::min(previousValue, verticalScrollBar()->maximum()));
  else
    restoreAnchor(anchor);
  if (!grew && isAtBottom()) {
    mode_ = Mode::Following;
    pausedByComposerGrowth_ = false;
  }
  layoutMaterializedCards();
  storeCurrentThreadState();
}

void ConversationView::prepareForLocalPromptAdmission() {
  if (mode_ != Mode::Paused || !pausedByComposerGrowth_)
    return;
  mode_ = Mode::Following;
  pausedByComposerGrowth_ = false;
  storeCurrentThreadState();
}

bool ConversationView::forwardWheelEvent(QWheelEvent *event) {
  return event && applyWheel(event);
}

bool ConversationView::isAtBottom() const noexcept {
  return verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 1;
}

ConversationView::Mode
ConversationView::modeForThread(const std::string &threadId) const noexcept {
  if (threadId == threadId_)
    return mode_;
  const auto saved = threadStates_.find(threadId);
  return saved == threadStates_.end() ? Mode::Following : saved->second.mode;
}

ConversationView::Anchor ConversationView::captureAnchor() const {
  Anchor anchor;
  anchor.absoluteValue = verticalScrollBar()->value();
  anchor.horizontalValue = horizontalScrollBar()->value();
  if (model_->rowCount() == 0 || heights_.empty() ||
      heights_.totalHeight() <= 0)
    return anchor;
  const qint64 contentY =
      std::max<qint64>(0, static_cast<qint64>(verticalScrollBar()->value()) -
                              leadingChromeHeight());
  std::size_t rowIndex = heights_.rowAt(contentY);
  while (rowIndex < heights_.size() && heights_.height(rowIndex) == 0)
    ++rowIndex;
  if (rowIndex >= heights_.size())
    return anchor;
  const ConversationItemModel::Row *row =
      model_->row(static_cast<int>(rowIndex));
  if (!row)
    return anchor;
  anchor.stableKey = row->stableKey;
  anchor.pixelOffset = rowRect(static_cast<int>(rowIndex)).top();
  return anchor;
}

void ConversationView::restoreAnchor(const Anchor &anchor) {
  int value = anchor.absoluteValue;
  if (!anchor.stableKey.empty()) {
    const QModelIndex index = model_->indexForStableKey(anchor.stableKey);
    if (index.isValid()) {
      const qint64 top = static_cast<qint64>(leadingChromeHeight()) +
                         heights_.top(static_cast<std::size_t>(index.row()));
      value = static_cast<int>(std::clamp<qint64>(
          top - anchor.pixelOffset, verticalScrollBar()->minimum(),
          verticalScrollBar()->maximum()));
    }
  }
  setScrollValue(value);
  horizontalScrollBar()->setValue(std::clamp(anchor.horizontalValue,
                                             horizontalScrollBar()->minimum(),
                                             horizontalScrollBar()->maximum()));
}

void ConversationView::setScrollValue(int value) {
  value = std::clamp(value, verticalScrollBar()->minimum(),
                     verticalScrollBar()->maximum());
  const QScopedValueRollback programmatic(programmaticScroll_, true);
  verticalScrollBar()->setValue(value);
  layoutMaterializedCards();
}

void ConversationView::stopFollowingAnimation() {
  if (followAnimation_->state() != QAbstractAnimation::Stopped)
    followAnimation_->stop();
}

void ConversationView::animateToBottom(int previousValue) {
  if (mode_ != Mode::Following)
    return;
  const int destination = verticalScrollBar()->maximum();
  const int start =
      std::clamp(std::max(verticalScrollBar()->value(), previousValue),
                 verticalScrollBar()->minimum(), destination);
  const int distance = destination - start;
  stopFollowingAnimation();
  if (distance <= 3) {
    setScrollValue(destination);
    return;
  }
  setScrollValue(start);
  followAnimation_->setDuration(std::clamp(110 + distance / 3, 130, 260));
  followAnimation_->setStartValue(start);
  followAnimation_->setEndValue(destination);
  followAnimation_->start();
}

void ConversationView::handleUserScrollValue(int value) {
  stopFollowingAnimation();
  pausedByComposerGrowth_ = false;
  mode_ = value >= verticalScrollBar()->maximum() - 1 ? Mode::Following
                                                      : Mode::Paused;
  storeCurrentThreadState();
}

bool ConversationView::applyWheel(QWheelEvent *event) {
  if (!event)
    return false;
  const int intent = !event->pixelDelta().isNull() ? event->pixelDelta().y()
                                                   : event->angleDelta().y();
  if (intent == 0)
    return false;
  pausedByComposerGrowth_ = false;
  const int oldValue = verticalScrollBar()->value();
  if (intent > 0) {
    stopFollowingAnimation();
    mode_ = Mode::Paused;
  }
  QScrollBar *bar = verticalScrollBar();
  const QPointF local = bar->mapFromGlobal(event->globalPosition().toPoint());
  QWheelEvent forwarded(local, event->globalPosition(), event->pixelDelta(),
                        event->angleDelta(), event->buttons(),
                        event->modifiers(), event->phase(), event->inverted());
  const QScopedValueRollback nativeDispatch(dispatchingNativeWheel_, true);
  QApplication::sendEvent(bar, &forwarded);
  if (verticalScrollBar()->value() < oldValue)
    mode_ = Mode::Paused;
  if (isAtBottom())
    mode_ = Mode::Following;
  updateMaterialization(false);
  storeCurrentThreadState();
  event->accept();
  return true;
}

QRect ConversationView::visualRect(const QModelIndex &index) const {
  if (!index.isValid() || index.model() != model_ || index.column() != 0)
    return {};
  return rowRect(index.row());
}

void ConversationView::scrollTo(const QModelIndex &index, ScrollHint hint) {
  const QRect geometry = visualRect(index);
  if (geometry.isEmpty())
    return;
  int target = verticalScrollBar()->value();
  if (hint == PositionAtTop)
    target += geometry.top();
  else if (hint == PositionAtBottom)
    target += geometry.bottom() - viewport()->height() + 1;
  else if (hint == PositionAtCenter)
    target += geometry.center().y() - viewport()->height() / 2;
  else if (geometry.top() < 0)
    target += geometry.top();
  else if (geometry.bottom() >= viewport()->height())
    target += geometry.bottom() - viewport()->height() + 1;
  setScrollValue(target);
  handleUserScrollValue(verticalScrollBar()->value());
  updateMaterialization(false);
}

QModelIndex ConversationView::indexAt(const QPoint &point) const {
  if (!viewport()->rect().contains(point) || heights_.empty())
    return {};
  const qint64 contentY = static_cast<qint64>(point.y()) +
                          verticalScrollBar()->value() - leadingChromeHeight();
  if (contentY < 0 || contentY >= heights_.totalHeight())
    return {};
  const int rowIndex = static_cast<int>(heights_.rowAt(contentY));
  const QRect geometry = rowRect(rowIndex);
  return geometry.contains(point) ? model_->index(rowIndex) : QModelIndex{};
}

QModelIndex ConversationView::moveCursor(CursorAction cursorAction,
                                         Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  int row = currentIndex().isValid() ? currentIndex().row() : -1;
  const int count = model_->rowCount();
  int direction = 0;
  switch (cursorAction) {
  case MoveUp:
  case MovePrevious:
    direction = -1;
    break;
  case MoveDown:
  case MoveNext:
    direction = 1;
    break;
  case MoveHome:
    row = 0;
    direction = 1;
    break;
  case MoveEnd:
    row = count - 1;
    direction = -1;
    break;
  case MovePageUp:
  case MovePageDown: {
    const int y = cursorAction == MovePageUp ? 0 : viewport()->height() - 1;
    const QModelIndex page = indexAt(QPoint(viewport()->width() / 2, y));
    if (page.isValid())
      row = page.row();
    direction = cursorAction == MovePageUp ? -1 : 1;
    break;
  }
  default:
    return currentIndex();
  }
  if (row < 0)
    row = direction < 0 ? count - 1 : 0;
  while (row >= 0 && row < count) {
    if (!isIndexHidden(model_->index(row)))
      return model_->index(row);
    row += direction;
  }
  return currentIndex();
}

int ConversationView::horizontalOffset() const {
  return horizontalScrollBar()->value();
}

int ConversationView::verticalOffset() const {
  return verticalScrollBar()->value();
}

bool ConversationView::isIndexHidden(const QModelIndex &index) const {
  const ConversationItemModel::Row *row = model_->row(index.row());
  return !row || !rowPresented(index.row());
}

void ConversationView::setSelection(
    const QRect &rect, QItemSelectionModel::SelectionFlags command) {
  if (!selectionModel())
    return;
  const QModelIndex topLeft = indexAt(rect.topLeft());
  const QModelIndex bottomRight = indexAt(rect.bottomRight());
  if (!topLeft.isValid() || !bottomRight.isValid())
    return;
  selectionModel()->select(
      QItemSelection(model_->index(std::min(topLeft.row(), bottomRight.row())),
                     model_->index(std::max(topLeft.row(), bottomRight.row()))),
      command);
}

QRegion ConversationView::visualRegionForSelection(
    const QItemSelection &selection) const {
  QRegion region;
  for (const QItemSelectionRange &range : selection)
    for (int row = range.top(); row <= range.bottom(); ++row)
      region += visualRect(model_->index(row));
  return region;
}

void ConversationView::updateGeometries() {
  if (applying_)
    return;
  updateScrollRange();
  layoutMaterializedCards();
}

void ConversationView::scrollContentsBy(int dx, int dy) {
  static_cast<void>(dx);
  static_cast<void>(dy);
  if (!materializing_)
    updateMaterialization(false);
  layoutMaterializedCards();
  viewport()->update();
}

bool ConversationView::eventFilter(QObject *watched, QEvent *event) {
  auto *widget = qobject_cast<QWidget *>(watched);
  ConversationCard *card = nullptr;
  for (QWidget *candidate = widget; candidate && candidate != viewport();
       candidate = candidate->parentWidget()) {
    if ((card = qobject_cast<ConversationCard *>(candidate)))
      break;
  }
  if (card && event->type() == QEvent::FocusIn) {
    const std::string key =
        card->property("conversationAnchorKey").toString().toStdString();
    const QModelIndex index = model_->indexForStableKey(key);
    if (index.isValid())
      setCurrentIndex(index);
  }
  if (card && event->type() == QEvent::FocusOut) {
    const std::string key =
        card->property("conversationAnchorKey").toString().toStdString();
    captureCardInteractionState(key, card, false);
  }
  // A row's root card is the view's geometry boundary. Measuring that root
  // for a descendant QLabel request can change its QTextDocument width and
  // post the same descendant request again, keeping an idle view busy.
  if (card && widget == card && event->type() == QEvent::LayoutRequest &&
      !applying_ && !materializing_) {
    const std::string key =
        card->property("conversationAnchorKey").toString().toStdString();
    const QModelIndex index = model_->indexForStableKey(key);
    const ConversationItemModel::Row *row = model_->row(index.row());
    if (index.isValid() && row && cardForStableKey(key) == card) {
      const int height = measureCard(card, rowWidth(*row));
      static_cast<void>(updateMeasuredHeight(index.row(), height, true));
      return true;
    }
  }
  return QAbstractItemView::eventFilter(watched, event);
}

void ConversationView::currentChanged(const QModelIndex &current,
                                      const QModelIndex &previous) {
  QAbstractItemView::currentChanged(current, previous);
  if (!current.isValid() || current.model() != model_)
    return;
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  const qint64 before = heights_.totalHeight();
  static_cast<void>(materializeRow(current.row(), true));
  if (before != heights_.totalHeight()) {
    updateScrollRange();
    if (follow)
      setScrollValue(verticalScrollBar()->maximum());
    else
      restoreAnchor(anchor);
  }
  layoutMaterializedCards();
  updateMaterializationProperties();
}

void ConversationView::mouseMoveEvent(QMouseEvent *event) {
  if (forwardingMouseEvent_) {
    event->accept();
    return;
  }
  if (forwardedMouseTarget_ && event->buttons() != Qt::NoButton) {
    const QPointer<QWidget> target = forwardedMouseTarget_;
    const QPoint viewportPosition = event->position().toPoint();
    const QPoint localPosition = target->mapFrom(viewport(), viewportPosition);
    QMouseEvent forwarded(event->type(), QPointF(localPosition),
                          event->scenePosition(), event->globalPosition(),
                          event->button(), event->buttons(), event->modifiers(),
                          event->pointingDevice());
    const QScopedValueRollback forwarding(forwardingMouseEvent_, true);
    QApplication::sendEvent(target, &forwarded);
    event->setAccepted(forwarded.isAccepted());
    return;
  }
  const QModelIndex index = indexAt(event->position().toPoint());
  if (index.isValid() &&
      !cardForStableKey(index.data(ConversationItemModel::StableKeyRole)
                            .toString()
                            .toStdString())) {
    const Anchor anchor = captureAnchor();
    const bool follow = mode_ == Mode::Following;
    const qint64 before = heights_.totalHeight();
    static_cast<void>(materializeRow(index.row(), true));
    if (before != heights_.totalHeight()) {
      updateScrollRange();
      if (follow)
        setScrollValue(verticalScrollBar()->maximum());
      else
        restoreAnchor(anchor);
    }
    layoutMaterializedCards();
    updateMaterializationProperties();
  }
  QAbstractItemView::mouseMoveEvent(event);
}

void ConversationView::mousePressEvent(QMouseEvent *event) {
  if (forwardingMouseEvent_) {
    event->accept();
    return;
  }
  const QPoint viewportPosition = event->position().toPoint();
  const QModelIndex index = indexAt(viewportPosition);
  if (!index.isValid()) {
    QAbstractItemView::mousePressEvent(event);
    return;
  }
  setCurrentIndex(index);
  const ConversationItemModel::Row *row = model_->row(index.row());
  ConversationCard *card = row ? cardForStableKey(row->stableKey) : nullptr;
  if (!card) {
    const Anchor anchor = captureAnchor();
    const bool follow = mode_ == Mode::Following;
    const qint64 before = heights_.totalHeight();
    card = materializeRow(index.row(), true);
    if (before != heights_.totalHeight()) {
      updateScrollRange();
      if (follow)
        setScrollValue(verticalScrollBar()->maximum());
      else
        restoreAnchor(anchor);
    }
    layoutMaterializedCards();
    updateMaterializationProperties();
  }
  if (!card) {
    QAbstractItemView::mousePressEvent(event);
    return;
  }

  const QPoint cardPosition = card->mapFrom(viewport(), viewportPosition);
  QWidget *target = card->childAt(cardPosition);
  if (!target)
    target = card;
  const QPoint localPosition = target->mapFrom(viewport(), viewportPosition);
  QMouseEvent forwarded(event->type(), QPointF(localPosition),
                        event->scenePosition(), event->globalPosition(),
                        event->button(), event->buttons(), event->modifiers(),
                        event->pointingDevice());
  forwardedMouseTarget_ = target;
  const QScopedValueRollback forwarding(forwardingMouseEvent_, true);
  QApplication::sendEvent(target, &forwarded);
  event->setAccepted(forwarded.isAccepted());
}

void ConversationView::mouseReleaseEvent(QMouseEvent *event) {
  if (forwardingMouseEvent_) {
    event->accept();
    return;
  }
  if (!forwardedMouseTarget_) {
    QAbstractItemView::mouseReleaseEvent(event);
    return;
  }
  const QPointer<QWidget> target = forwardedMouseTarget_;
  forwardedMouseTarget_.clear();
  const QPoint viewportPosition = event->position().toPoint();
  const QPoint localPosition = target->mapFrom(viewport(), viewportPosition);
  QMouseEvent forwarded(event->type(), QPointF(localPosition),
                        event->scenePosition(), event->globalPosition(),
                        event->button(), event->buttons(), event->modifiers(),
                        event->pointingDevice());
  const QScopedValueRollback forwarding(forwardingMouseEvent_, true);
  QApplication::sendEvent(target, &forwarded);
  event->setAccepted(forwarded.isAccepted());
}

void ConversationView::paintEvent(QPaintEvent *event) {
  QPainter painter(viewport());
  painter.setClipRegion(event->region());
  if (!heights_.empty() && heights_.totalHeight() > 0) {
    const qint64 firstY = std::max<qint64>(0, verticalScrollBar()->value() -
                                                  leadingChromeHeight());
    const qint64 lastY =
        std::min<qint64>(heights_.totalHeight() - 1,
                         verticalScrollBar()->value() - leadingChromeHeight() +
                             std::max(0, viewport()->height() - 1));
    if (lastY >= firstY) {
      const int first = static_cast<int>(heights_.rowAt(firstY));
      const int last = static_cast<int>(heights_.rowAt(lastY));
      std::unordered_set<std::string> paintedSections;
      for (int rowIndex = first; rowIndex <= last; ++rowIndex) {
        const ConversationItemModel::Row *row = model_->row(rowIndex);
        if (!row || !rowPresented(rowIndex) ||
            !paintedSections.insert(row->sectionKey).second)
          continue;
        const auto section = sectionRanges_.find(row->sectionKey);
        if (section == sectionRanges_.end() ||
            section->second.root.empty() || section->second.last.empty())
          continue;
        const SectionRange &range = section->second;
        const std::optional<int> root = modelSectionRow(range.root);
        const std::optional<int> sectionLast = modelSectionRow(range.last);
        if (!root || !sectionLast || *sectionLast <= *root)
          continue;
        const qreal top =
            static_cast<qreal>(leadingChromeHeight()) +
            static_cast<qreal>(heights_.top(static_cast<std::size_t>(*root))) -
            verticalScrollBar()->value();
        const qreal bottom =
            static_cast<qreal>(leadingChromeHeight()) +
            static_cast<qreal>(
                heights_.top(static_cast<std::size_t>(*sectionLast))) +
            heights_.height(static_cast<std::size_t>(*sectionLast)) -
            rowSpacing(*sectionLast) + 10 - verticalScrollBar()->value();
        const QRectF surface(0.5, top + 0.5,
                             std::max(0, viewport()->width()) - 1.0,
                             std::max<qreal>(1.0, bottom - top - 1.0));
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(QColor(QStringLiteral("#eff5fe")));
        painter.setPen(QPen(QColor(range.active ? QStringLiteral("#6f98e8")
                                                : QStringLiteral("#b7cff9")),
                            range.active ? 2.0 : 1.0));
        painter.drawRoundedRect(surface, 8.0, 8.0);
      }
      for (int rowIndex = first; rowIndex <= last; ++rowIndex) {
        const ConversationItemModel::Row *row = model_->row(rowIndex);
        if (!row || !rowPresented(rowIndex) ||
            !rowUsesPassiveDelegate(*row) ||
            cardForStableKey(row->stableKey))
          continue;
        QStyleOptionViewItem option;
        option.initFrom(this);
        option.rect = rowRect(rowIndex);
        const auto section = sectionRanges_.find(row->sectionKey);
        if (section != sectionRanges_.end()) {
          const std::optional<int> root =
              modelSectionRow(section->second.root);
          const std::optional<int> last =
              modelSectionRow(section->second.last);
          if (root && last && *root == rowIndex && *last > *root)
            option.viewItemPosition = QStyleOptionViewItem::Beginning;
        }
        if (!option.rect.intersects(event->rect()))
          continue;
        if (selectionModel() &&
            selectionModel()->isSelected(model_->index(rowIndex)))
          option.state |= QStyle::State_Selected;
        static_cast<ConversationPassiveDelegate *>(itemDelegate())
            ->paintCard(&painter, option, model_->index(rowIndex),
                        rowCollapsed(*row));
      }
    }
  }
  if (hasFocus() && currentIndex().isValid()) {
    QStyleOptionFocusRect option;
    option.initFrom(this);
    option.rect = visualRect(currentIndex()).adjusted(1, 1, -1, -1);
    option.state |= QStyle::State_KeyboardFocusChange;
    style()->drawPrimitive(QStyle::PE_FrameFocusRect, &option, &painter, this);
  }
}

void ConversationView::resizeEvent(QResizeEvent *event) {
  const Anchor anchor = captureAnchor();
  const bool follow = mode_ == Mode::Following;
  stopFollowingAnimation();
  QAbstractItemView::resizeEvent(event);
  stagingHost_->resize(viewport()->size());
  stagingOverlay_->setGeometry(viewport()->rect());
  rebuildHeightIndex();
  for (auto &[key, card] : materializedCards_) {
    const QModelIndex index = model_->indexForStableKey(key);
    const ConversationItemModel::Row *row = model_->row(index.row());
    if (!index.isValid() || !row)
      continue;
    const int height = measureCard(card, rowWidth(*row));
    heightCache_.insert_or_assign(key, HeightRecord{rowWidth(*row), height});
    static_cast<void>(heights_.setHeight(static_cast<std::size_t>(index.row()),
                                         height + rowSpacing(index.row())));
  }
  updateScrollRange();
  if (follow)
    setScrollValue(verticalScrollBar()->maximum());
  else
    restoreAnchor(anchor);
  updateMaterialization(false);
  viewport()->update();
  storeCurrentThreadState();
}

void ConversationView::wheelEvent(QWheelEvent *event) {
  if (!applyWheel(event))
    QAbstractItemView::wheelEvent(event);
}

} // namespace codexui::codex::middle
