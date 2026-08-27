// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationCards.h"

#include "codex/PresentationStatus.h"
#include "codex/ui/UiStyle.h"

#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStyle>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace codexui::codex::middle {
namespace {

constexpr int MaximumCommandOutputHeight = 220;
constexpr int MaximumCommandTextHeight = 90;
constexpr int CommandTextPadding = 7;
constexpr int PendingAnimationIntervalMilliseconds = 32;
constexpr qint64 PendingHalfCycleMilliseconds = 850;
constexpr int ThumbnailMaximumWidth = 280;
constexpr int ThumbnailMaximumHeight = 180;
constexpr int ViewerMaximumImageExtent = 4096;
constexpr qsizetype MaximumGenericActivityCharacters = 4096;

bool initiallyCollapsed(CardKind kind) {
  return kind != CardKind::UserMessage && kind != CardKind::AgentMessage &&
         kind != CardKind::LocalPrompt;
}

class CardDisclosureButton final : public QToolButton {
public:
  explicit CardDisclosureButton(QWidget *parent = nullptr)
      : QToolButton(parent) {
    setObjectName(QStringLiteral("cardDisclosureButton"));
    setProperty("kind", "subtle");
    setFixedSize(24, 24);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QStringLiteral("Expand card"));
    setToolTip(accessibleName());
    setProperty("chevronDirection", "left");
  }

  void setExpanded(bool expanded) {
    if (expanded_ == expanded)
      return;
    expanded_ = expanded;
    setAccessibleName(expanded ? QStringLiteral("Collapse card")
                               : QStringLiteral("Expand card"));
    setToolTip(accessibleName());
    setProperty("chevronDirection", expanded ? "down" : "left");
    update();
  }

protected:
  void paintEvent(QPaintEvent *event) override {
    static_cast<void>(event);
    QRect indicator = rect().adjusted(3, 3, -3, -3);
    // Keep the full 24 px hit target while aligning the visible stroke with
    // the card's canonical right inset. The narrower left glyph needs two
    // pixels more optical compensation than the down glyph.
    indicator.translate(expanded_ ? 7 : 9, 0);
    UiStyle::drawChevron(this, indicator, isEnabled(),
                         underMouse() || hasFocus(),
                         expanded_ ? UiStyle::ChevronDirection::Down
                                   : UiStyle::ChevronDirection::Left);
  }

private:
  bool expanded_ = false;
};

void openImageViewer(const QString &path);

class ImageThumbnail final : public QLabel {
public:
  ImageThumbnail(QString path, QWidget *parent)
      : QLabel(parent), path_(std::move(path)) {
    setObjectName(QStringLiteral("messageImageThumbnail"));
    setProperty("kind", "imageThumbnail");
    setCursor(Qt::PointingHandCursor);
    setToolTip(QDir::toNativeSeparators(path_));
    setAlignment(Qt::AlignCenter);
    setMinimumSize(72, 48);
    setMaximumSize(ThumbnailMaximumWidth, ThumbnailMaximumHeight);

    QImageReader reader(path_);
    reader.setAutoTransform(true);
    const QSize source = reader.size();
    if (source.isValid())
      reader.setScaledSize(source.scaled(ThumbnailMaximumWidth - 8,
                                         ThumbnailMaximumHeight - 8,
                                         Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull()) {
      setText(QStringLiteral("Image unavailable\n%1")
                  .arg(QFileInfo(path_).fileName()));
      setProperty("imageAvailable", false);
      unsetCursor();
      return;
    }
    setProperty("imageAvailable", true);
    setPixmap(QPixmap::fromImage(image));
    setFixedSize(image.size() + QSize(8, 8));
  }

  [[nodiscard]] bool represents(const QString &path) const {
    return path_ == path &&
           property("imageAvailable").toBool() == QFileInfo(path).isFile();
  }

protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton &&
        property("imageAvailable").toBool()) {
      openImageViewer(path_);
      event->accept();
      return;
    }
    QLabel::mousePressEvent(event);
  }

private:
  QString path_;
};

class ImageViewer final : public QDialog {
public:
  explicit ImageViewer(const QString &path) : QDialog(nullptr, Qt::Window) {
    setObjectName(QStringLiteral("messageImageViewer"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::NonModal);
    setWindowTitle(QFileInfo(path).fileName());
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    imageLabel_ = new QLabel(scroll_);
    imageLabel_->setObjectName(QStringLiteral("messageImageViewerImage"));
    imageLabel_->setAlignment(Qt::AlignCenter);

    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize source = reader.size();
    if (source.isValid() && (source.width() > ViewerMaximumImageExtent ||
                             source.height() > ViewerMaximumImageExtent))
      reader.setScaledSize(source.scaled(ViewerMaximumImageExtent,
                                         ViewerMaximumImageExtent,
                                         Qt::KeepAspectRatio));
    image_ = reader.read();
    if (image_.isNull())
      imageLabel_->setText(QStringLiteral("Image unavailable"));
    scroll_->setWidget(imageLabel_);
    layout->addWidget(scroll_);
    resize(900, 650);
    updatePixmap();
  }

protected:
  void showEvent(QShowEvent *event) override {
    QDialog::showEvent(event);
    updatePixmap();
  }

  void resizeEvent(QResizeEvent *event) override {
    QDialog::resizeEvent(event);
    updatePixmap();
  }

private:
  void updatePixmap() {
    if (image_.isNull() || !scroll_)
      return;
    const QSize available = scroll_->viewport()->size() - QSize(8, 8);
    if (available.isEmpty())
      return;
    imageLabel_->setPixmap(QPixmap::fromImage(image_.scaled(
        available, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
  }

  QImage image_;
  QScrollArea *scroll_ = nullptr;
  QLabel *imageLabel_ = nullptr;
};

void openImageViewer(const QString &path) {
  auto *viewer = new ImageViewer(path);
  viewer->show();
}

QLabel *makeLabel(const QString &value, const char *kind = "body",
                  QWidget *parent = nullptr) {
  auto *label = new QLabel(value, parent);
  label->setProperty("kind", kind);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

QString markdownHtml(const QString &markdown) {
  QTextDocument document;
  document.setMarkdown(markdown, QTextDocument::MarkdownFeatures(
                                     QTextDocument::MarkdownDialectGitHub) |
                                     QTextDocument::MarkdownNoHTML);
  return document.toHtml();
}

QLabel *makeMarkdownLabel(const QString &value, QWidget *parent = nullptr) {
  auto *label = new QLabel(parent);
  label->setProperty("kind", "body");
  label->setTextFormat(Qt::RichText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setOpenExternalLinks(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                 Qt::LinksAccessibleByMouse);
  label->setProperty("markdownSource", value);
  label->setText(markdownHtml(value));
  return label;
}

bool setVisibleText(QLabel *label, const QString &text) {
  const bool visible = !text.isEmpty();
  const bool explicitlyVisible = !label->isHidden();
  const bool changed = label->text() != text || explicitlyVisible != visible;
  if (label->text() != text)
    label->setText(text);
  if (explicitlyVisible != visible)
    label->setVisible(visible);
  return changed;
}

bool setVisibleMarkdown(QLabel *label, const QString &markdown) {
  const bool visible = !markdown.isEmpty();
  const bool contentChanged =
      label->property("markdownSource").toString() != markdown;
  const bool explicitlyVisible = !label->isHidden();
  const bool changed = contentChanged || explicitlyVisible != visible;
  if (contentChanged) {
    label->setProperty("markdownSource", markdown);
    label->setText(markdownHtml(markdown));
  }
  if (explicitlyVisible != visible)
    label->setVisible(visible);
  return changed;
}

QString displayStatus(const QString &status) {
  const QByteArray encoded = status.toUtf8();
  const PresentationStatus classified = classifyStatus(std::string_view(
      encoded.constData(), static_cast<std::size_t>(encoded.size())));
  const QString display = QString::fromUtf8(
      classified.text.data(), static_cast<qsizetype>(classified.text.size()));
  return classified.kind == StatusKind::Unknown
             ? UiStyle::humanizeLabel(display)
             : display;
}

QString statusTone(const QString &status) {
  const QByteArray encoded = status.toUtf8();
  const std::string_view tone =
      classifyStatus(std::string_view(encoded.constData(),
                                      static_cast<std::size_t>(encoded.size())))
          .tone;
  return QString::fromLatin1(tone.data(), static_cast<qsizetype>(tone.size()));
}

void setStatusTone(QLabel *label, const QString &status) {
  const QString tone = statusTone(status);
  if (label->property("tone").toString() == tone)
    return;
  label->setProperty("tone", tone);
  label->style()->unpolish(label);
  label->style()->polish(label);
  label->update();
}

QString commandMetadata(const CommandExecutionData &command) {
  QStringList metadata{displayStatus(command.status)};
  if (command.exitCode)
    metadata << QStringLiteral("exit %1").arg(*command.exitCode);
  if (!command.cwd.isEmpty())
    metadata << command.cwd;
  if (command.durationMilliseconds) {
    const qreal seconds = qreal(*command.durationMilliseconds) / 1000.0;
    metadata << QStringLiteral("%1 s").arg(seconds, 0, 'f',
                                           seconds < 10.0 ? 1 : 0);
  }
  return metadata.join(QStringLiteral("  |  "));
}

QString agentMetadata(const AgentActivityData &activity) {
  QStringList metadata;
  if (!activity.tool.isEmpty())
    metadata << activity.tool;
  metadata << displayStatus(activity.status.isEmpty() ? activity.kind
                                                      : activity.status);
  if (!activity.receivers.isEmpty())
    metadata << activity.receivers.join(QStringLiteral(", "));
  if (!activity.model.isEmpty())
    metadata << activity.model;
  if (!activity.reasoningEffort.isEmpty())
    metadata << activity.reasoningEffort;
  if (!activity.childThreadId.isEmpty())
    metadata << QStringLiteral("thread %1").arg(activity.childThreadId);
  if (!activity.agentPath.isEmpty())
    metadata << activity.agentPath;
  if (!activity.senderThreadId.isEmpty())
    metadata << QStringLiteral("sender %1").arg(activity.senderThreadId);
  return metadata.join(QStringLiteral("  |  "));
}

QString displayChangeKind(const QString &kind) {
  if (kind.isEmpty())
    return QStringLiteral("Changed");
  return UiStyle::humanizeLabel(kind);
}

struct DiffCounts {
  int additions = 0;
  int deletions = 0;
};

QString fileChangesText(const FileChangesData &data) {
  QStringList rows;
  for (const FileChangeData &change : data.changes) {
    if (change.path.isEmpty())
      continue;
    QString row = QStringLiteral("%1  ·  %2")
                      .arg(change.path, displayChangeKind(change.kind));
    if (change.additions && change.deletions)
      row += QStringLiteral("  +%1 −%2")
                 .arg(*change.additions)
                 .arg(*change.deletions);
    rows << row;
  }
  return rows.join(QLatin1Char('\n'));
}

std::optional<DiffCounts> totalDiffCounts(const FileChangesData &data) {
  DiffCounts total;
  bool available = false;
  for (const FileChangeData &change : data.changes) {
    if (!change.additions || !change.deletions)
      continue;
    available = true;
    total.additions += *change.additions;
    total.deletions += *change.deletions;
  }
  return available ? std::optional<DiffCounts>{total} : std::nullopt;
}

QString planMarkdown(const PlanData &plan) {
  if (!plan.legacyText.isEmpty())
    return plan.legacyText;
  QStringList rows;
  if (!plan.explanation.isEmpty())
    rows << plan.explanation;
  if (!plan.steps.empty() && !rows.empty())
    rows << QString{};
  for (const PlanStepData &step : plan.steps) {
    const QString marker =
        step.status == QStringLiteral("completed")    ? QStringLiteral("✓")
        : step.status == QStringLiteral("inProgress") ? QStringLiteral("◉")
                                                      : QStringLiteral("○");
    rows << QStringLiteral("%1 %2  ").arg(marker, step.text);
  }
  return rows.join(QLatin1Char('\n'));
}

QString boundedGenericActivity(const nlohmann::json &raw) {
  QString rendered = QString::fromStdString(raw.dump(2));
  if (rendered.size() <= MaximumGenericActivityCharacters)
    return rendered;
  rendered.truncate(MaximumGenericActivityCharacters);
  return rendered + QStringLiteral("\n\n[Activity details truncated]");
}

bool acceptedTransitionActive(const LocalPromptData &prompt, qint64 now) {
  return prompt.acceptedTransitionActive(now);
}

bool presentationEquals(const VisibleCardData &left,
                        const VisibleCardData &right) {
  if (left.kind != right.kind)
    return false;
  const auto *first = std::get_if<LocalPromptData>(&left.payload);
  const auto *second = std::get_if<LocalPromptData>(&right.payload);
  if (first && second) {
    return first->prompt == second->prompt &&
           first->imagePaths == second->imagePaths &&
           first->state == second->state &&
           first->acceptedAtMilliseconds == second->acceptedAtMilliseconds &&
           first->error == second->error;
  }
  return left.payload == right.payload;
}

} // namespace

ContentSizedTextView::ContentSizedTextView(int maximumContentHeight,
                                           QWidget *parent)
    : QTextEdit(parent) {
  setReadOnly(true);
  setAcceptRichText(false);
  setMinimumHeight(0);
  setMaximumHeight(maximumContentHeight);
  setLineWrapMode(QTextEdit::WidgetWidth);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  document()->setDocumentMargin(CommandTextPadding);
}

bool ContentSizedTextView::setContent(const QString &content) {
  if (toPlainText() == content)
    return false;
  setPlainText(content);
  measureAtCurrentWidth(true);
  return true;
}

QSize ContentSizedTextView::sizeHint() const {
  QSize result = QTextEdit::sizeHint();
  result.setHeight(preferredHeight_);
  return result;
}

QSize ContentSizedTextView::minimumSizeHint() const {
  QSize result = QTextEdit::minimumSizeHint();
  result.setHeight(0);
  return result;
}

void ContentSizedTextView::resizeEvent(QResizeEvent *event) {
  QTextEdit::resizeEvent(event);
  // Wrapping is authoritative only after QTextEdit has assigned its
  // viewport width. Propagate a changed hint immediately so a multiline view
  // cannot remain at an earlier one-line height with a premature scrollbar.
  measureAtCurrentWidth(true);
}

void ContentSizedTextView::measureAtCurrentWidth(bool notifyParent) {
  const QString content = toPlainText();
  int wantedHeight = 0;
  if (!content.isEmpty()) {
    const int frame = 2 * frameWidth();
    document()->setTextWidth(std::max(1, viewport()->width()));
    wantedHeight =
        frame + static_cast<int>(std::ceil(document()->size().height()));
  }
  wantedHeight = std::clamp(wantedHeight, 0, maximumHeight());
  if (wantedHeight == preferredHeight_)
    return;
  preferredHeight_ = wantedHeight;
  if (notifyParent)
    updateGeometry();
}

CommandOutputView::CommandOutputView(const QString &output, QWidget *parent)
    : ContentSizedTextView(MaximumCommandOutputHeight, parent) {
  setProperty("kind", "code");
  setObjectName(QStringLiteral("commandOutputView"));
  setStyleSheet(QStringLiteral(
      "QTextEdit#commandOutputView{background:#111827;color:#e5e7eb;"
      "border-radius:6px;}"));

  connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
          [this](int value) {
            if (programmaticScroll_)
              return;
            preservedScrollValue_ = value;
            followsLatest_ = isAtBottom();
          });
  connect(verticalScrollBar(), &QScrollBar::rangeChanged, this,
          [this](int, int) {
            if (!programmaticScroll_)
              settleScroll();
          });

  setOutput(output);
  measureAtCurrentWidth(false);
  settleScroll();
}

CommandOutputView::ScrollState CommandOutputView::scrollState() const {
  return {followsLatest_, preservedScrollValue_};
}

bool CommandOutputView::followsLatest() const noexcept {
  return followsLatest_;
}

bool CommandOutputView::setOutput(const QString &output) {
  const QString displayOutput = trimTrailingEmptyLines(output);
  if (currentOutput_ == displayOutput)
    return false;

  const bool retainedFollow = followsLatest_;
  const int retainedValue = preservedScrollValue_;
  const bool appendOnly =
      !currentOutput_.isEmpty() && displayOutput.startsWith(currentOutput_);
  programmaticScroll_ = true;
  if (appendOnly) {
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(displayOutput.sliced(currentOutput_.size()));
  } else {
    setPlainText(displayOutput);
  }
  currentOutput_ = displayOutput;
  followsLatest_ = retainedFollow;
  preservedScrollValue_ = retainedValue;
  programmaticScroll_ = false;
  // Asking the document layout for its size here completes wrapping at the
  // already assigned viewport width.  The enclosing conversation can then
  // account for the final card height in the same reconciliation transaction.
  measureAtCurrentWidth(true);
  settleScroll();
  return true;
}

void CommandOutputView::restoreScrollState(const ScrollState &state) {
  followsLatest_ = state.followsLatest;
  preservedScrollValue_ = std::max(0, state.value);
  settleScroll();
}

void CommandOutputView::wheelEvent(QWheelEvent *event) {
  QScrollBar *bar = verticalScrollBar();
  const int delta = !event->pixelDelta().isNull() ? event->pixelDelta().y()
                                                  : event->angleDelta().y();
  if (bar->maximum() <= bar->minimum() ||
      (delta > 0 && bar->value() <= bar->minimum()) ||
      (delta < 0 && bar->value() >= bar->maximum())) {
    event->accept();
    return;
  }

  if (delta > 0)
    followsLatest_ = false;
  QTextEdit::wheelEvent(event);
  preservedScrollValue_ = bar->value();
  followsLatest_ = isAtBottom();
}

void CommandOutputView::settleScroll() {
  if (settlingScroll_)
    return;
  settlingScroll_ = true;
  QScrollBar *bar = verticalScrollBar();
  const bool wasProgrammatic = programmaticScroll_;
  programmaticScroll_ = true;
  if (followsLatest_) {
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    setTextCursor(cursor);
    ensureCursorVisible();
  }
  const int target =
      followsLatest_
          ? bar->maximum()
          : std::clamp(preservedScrollValue_, bar->minimum(), bar->maximum());
  bar->setValue(target);
  preservedScrollValue_ = target;
  programmaticScroll_ = wasProgrammatic;
  settlingScroll_ = false;
}

bool CommandOutputView::isAtBottom() const {
  return verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 1;
}

class ConversationCard::Impl final {
public:
  Impl(ConversationCard *owner, const VisibleCardData &initial)
      : owner(owner), current(initial),
        collapsed(initiallyCollapsed(initial.kind)) {
    owner->setObjectName(QStringLiteral("conversationCard"));
    owner->setProperty("conversationCardKey",
                       QString::fromStdString(stableKey(initial.key)));
    owner->setProperty("conversationCardKind", static_cast<int>(initial.kind));
    layout = new QVBoxLayout(owner);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);

    header = new QWidget(owner);
    header->setObjectName(QStringLiteral("conversationCardHeader"));
    headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(6);
    title = makeLabel({}, "title", header);
    title->setWordWrap(false);
    disclosure = new CardDisclosureButton(header);
    headerLayout->addWidget(title, 1);
    headerLayout->addWidget(disclosure, 0, Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(header);

    content = new QWidget(owner);
    content->setObjectName(QStringLiteral("conversationCardContent"));
    contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(6);
    layout->addWidget(content);

    QObject::connect(disclosure, &QToolButton::clicked, owner,
                     [this] { emit this->owner->foldRequested(!collapsed); });
    owner->setProperty("kind", "raised");
    std::visit([this](const auto &payload) { createComposition(payload); },
               initial.payload);
    refreshFoldPresentation();
  }

  [[nodiscard]] bool canApply(const VisibleCardData &next) const noexcept {
    return current.key == next.key && (current.kind == next.kind ||
                                       (current.kind == CardKind::LocalPrompt &&
                                        next.kind == CardKind::UserMessage));
  }

  bool apply(const VisibleCardData &next) {
    if (!canApply(next)) {
      Q_ASSERT_X(false, "ConversationCard::apply",
                 "a persistent conversation card received an incompatible "
                 "key or kind");
      return false;
    }
    const bool becomingAuthoritative = current.kind == CardKind::LocalPrompt &&
                                       next.kind == CardKind::UserMessage;
    const bool presentationChanged =
        becomingAuthoritative || !presentationEquals(current, next);
    if (becomingAuthoritative)
      promoteToAuthoritativeUserMessage();
    current = next;
    if (!presentationChanged)
      return false;
    std::visit([this](const auto &payload) { updateComposition(payload); },
               next.payload);
    refreshFoldPresentation();
    owner->updateGeometry();
    owner->update();
    return true;
  }

  void promoteToAuthoritativeUserMessage() {
    if (animationTimer)
      animationTimer->stop();
    owner->setObjectName(QStringLiteral("conversationCard"));
    owner->setProperty("conversationCardKind",
                       static_cast<int>(CardKind::UserMessage));
    owner->setProperty("messageRole", "user");
    owner->setStyleSheet(QString{});
    for (QLabel *label : {title, body, metadata})
      if (label)
        label->setStyleSheet(QString{});
    if (metadata) {
      metadata->clear();
      metadata->hide();
    }
  }

  void setCollapsed(bool next) {
    if (collapsed == next)
      return;
    collapsed = next;
    refreshFoldPresentation();
    owner->updateGeometry();
    owner->update();
  }

  [[nodiscard]] bool hasVisibleContent() const {
    for (int index = 0; index < contentLayout->count(); ++index) {
      if (QWidget *widget = contentLayout->itemAt(index)->widget();
          widget && !widget->isHidden())
        return true;
    }
    return false;
  }

  void refreshFoldPresentation() {
    const bool expandable = hasVisibleContent();
    disclosure->setExpanded(!collapsed);
    disclosure->setVisible(expandable);
    content->setVisible(expandable && !collapsed);
  }

  void createImageContainer() {
    images = new QWidget(content);
    images->setObjectName(QStringLiteral("messageImages"));
    imageLayout = new QVBoxLayout(images);
    imageLayout->setContentsMargins(0, 0, 0, 0);
    imageLayout->setSpacing(8);
    imageLayout->setAlignment(Qt::AlignLeft);
    images->hide();
    contentLayout->addWidget(images);
  }

  void setImages(const QStringList &paths, bool forceRebuild = false) {
    bool matches = !forceRebuild && imageLayout->count() == paths.size();
    for (qsizetype index = 0; matches && index < paths.size(); ++index) {
      const auto *thumbnail = dynamic_cast<ImageThumbnail *>(
          imageLayout->itemAt(static_cast<int>(index))->widget());
      matches = thumbnail && thumbnail->represents(paths.at(index));
    }
    if (matches) {
      images->setVisible(!paths.isEmpty());
      return;
    }
    while (QLayoutItem *item = imageLayout->takeAt(0)) {
      delete item->widget();
      delete item;
    }
    for (const QString &path : paths) {
      auto *thumbnail = new ImageThumbnail(path, images);
      imageLayout->addWidget(thumbnail, 0, Qt::AlignLeft);
    }
    images->setVisible(!paths.isEmpty());
  }

  void createComposition(const UserMessageData &message) {
    owner->setProperty("messageRole", "user");
    title->setText(QStringLiteral("You"));
    body = makeMarkdownLabel({}, content);
    contentLayout->addWidget(body);
    createImageContainer();
    updateComposition(message);
  }

  void updateComposition(const UserMessageData &message) {
    setVisibleMarkdown(body, message.text);
    setImages(message.imagePaths);
  }

  void createComposition(const AgentMessageData &message) {
    owner->setProperty("messageRole", "agent");
    title->setText(QStringLiteral("Codex"));
    title->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    phaseSeparator = makeLabel(QStringLiteral("•"), "messagePhase", header);
    phaseSeparator->setObjectName(QStringLiteral("agentMessagePhaseSeparator"));
    phaseSeparator->setWordWrap(false);
    phaseSeparator->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    phase = makeLabel({}, "messagePhase", header);
    phase->setObjectName(QStringLiteral("agentMessagePhase"));
    phase->setWordWrap(false);
    phase->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    headerLayout->setStretch(0, 0);
    headerLayout->insertWidget(1, phaseSeparator, 0, Qt::AlignVCenter);
    headerLayout->insertWidget(2, phase, 0, Qt::AlignVCenter);
    headerLayout->insertStretch(3, 1);
    body = makeMarkdownLabel({}, content);
    contentLayout->addWidget(body);
    updateComposition(message);
  }

  void updateComposition(const AgentMessageData &message) {
    phase->setText(message.finalAnswer ? QStringLiteral("final answer")
                                       : QStringLiteral("update"));
    const QString phaseStatus = message.finalAnswer
                                    ? QStringLiteral("completed")
                                    : QStringLiteral("inProgress");
    setStatusTone(phaseSeparator, phaseStatus);
    setStatusTone(phase, phaseStatus);
    layout->setContentsMargins(12, message.finalAnswer ? 10 : 8, 12,
                               message.finalAnswer ? 10 : 8);
    setVisibleMarkdown(body, message.text);
  }

  void createComposition(const CommandExecutionData &execution) {
    title->setText(QStringLiteral("Command execution"));
    command = new ContentSizedTextView(MaximumCommandTextHeight, content);
    command->setProperty("kind", "command");
    command->setObjectName(QStringLiteral("commandTextView"));
    command->setStyleSheet(
        QStringLiteral("QTextEdit#commandTextView{background:#f8fafc;"
                       "border:1px solid #d7dee8;border-radius:6px;}"));
    output = new CommandOutputView({}, content);
    output->hide();
    metadata = makeLabel({}, "meta", content);
    metadata->setObjectName(QStringLiteral("commandMetadata"));
    contentLayout->addWidget(command);
    contentLayout->addWidget(output);
    contentLayout->addWidget(metadata);
    updateComposition(execution);
  }

  void updateComposition(const CommandExecutionData &execution) {
    const QString displayCommand = trimTrailingEmptyLines(execution.command);
    command->setContent(displayCommand);
    command->setVisible(!displayCommand.isEmpty());
    const QString displayOutput = trimTrailingEmptyLines(execution.output);
    const bool visibleOutput = terminalOutputHasVisibleText(displayOutput);
    if (visibleOutput) {
      output->setOutput(displayOutput);
      output->show();
    } else {
      output->hide();
      output->setOutput({});
      // Once the surface ceases to exist there is no user-owned paused
      // position to retain. If visible output appears later it starts in
      // the documented follow-latest state.
      output->restoreScrollState({true, 0});
    }
    metadata->setText(commandMetadata(execution));
    setStatusTone(metadata, execution.status);
    metadata->show();
  }

  void createComposition(const AgentActivityData &activity) {
    title->setText(QStringLiteral("Agent activity"));
    metadata = makeLabel({}, "meta", content);
    body = makeLabel({}, "body", content);
    detail = makeMarkdownLabel({}, content);
    contentLayout->addWidget(metadata);
    contentLayout->addWidget(body);
    contentLayout->addWidget(detail);
    updateComposition(activity);
  }

  void updateComposition(const AgentActivityData &activity) {
    metadata->setText(agentMetadata(activity));
    setStatusTone(metadata,
                  activity.status.isEmpty() ? activity.kind : activity.status);
    metadata->show();
    setVisibleText(body, activity.prompt);
    setVisibleMarkdown(detail, activity.resultText);
  }

  void createComposition(const ReasoningData &reasoning) {
    title->setText(QStringLiteral("Reasoning"));
    body = makeMarkdownLabel({}, content);
    contentLayout->addWidget(body);
    updateComposition(reasoning);
  }

  void updateComposition(const ReasoningData &reasoning) {
    setVisibleMarkdown(body, reasoning.summary);
  }

  void createComposition(const FileChangesData &changes) {
    title->setText(QStringLiteral("File changes"));
    metadata = makeLabel({}, "meta", content);
    body = makeLabel({}, "body", content);
    contentLayout->addWidget(body);
    contentLayout->addWidget(metadata);
    updateComposition(changes);
  }

  void updateComposition(const FileChangesData &changes) {
    setVisibleText(body, fileChangesText(changes));
    QStringList values{displayStatus(changes.status)};
    values << QStringLiteral("%1 paths").arg(changes.changes.size());
    if (const auto counts = totalDiffCounts(changes))
      values << QStringLiteral("+%1 −%2")
                    .arg(counts->additions)
                    .arg(counts->deletions);
    metadata->setText(values.join(QStringLiteral("  |  ")));
    setStatusTone(metadata, changes.status);
    metadata->show();
  }

  void createComposition(const PlanData &plan) {
    title->setText(QStringLiteral("Plan"));
    body = makeMarkdownLabel({}, content);
    contentLayout->addWidget(body);
    updateComposition(plan);
  }

  void updateComposition(const PlanData &plan) {
    setVisibleMarkdown(body, planMarkdown(plan));
  }

  void createComposition(const ImageGenerationData &image) {
    title->setText(QStringLiteral("Generated image"));
    metadata = makeLabel({}, "meta", content);
    body = makeLabel({}, "body", content);
    contentLayout->addWidget(metadata);
    contentLayout->addWidget(body);
    createImageContainer();
    updateComposition(image);
  }

  void updateComposition(const ImageGenerationData &image) {
    const bool generated =
        !image.status.isEmpty() || !image.revisedPrompt.isEmpty();
    title->setText(generated ? QStringLiteral("Generated image")
                             : QStringLiteral("Image"));
    setVisibleText(metadata, displayStatus(image.status));
    setStatusTone(metadata, image.status);
    setVisibleText(body, image.revisedPrompt);
    // A generated image can become readable at the same path as its status
    // advances, so its update remains the authoritative reload boundary.
    setImages(image.path.isEmpty() ? QStringList{} : QStringList{image.path},
              true);
  }

  void createComposition(const GenericActivityData &activity) {
    metadata = makeLabel({}, "meta", content);
    contentLayout->addWidget(metadata);
    updateComposition(activity);
  }

  void updateComposition(const GenericActivityData &activity) {
    title->setText(activity.type.isEmpty()
                       ? QStringLiteral("Activity")
                       : UiStyle::humanizeLabel(activity.type));
    metadata->setText(boundedGenericActivity(activity.raw));
    metadata->setObjectName(QStringLiteral("genericActivityMetadata"));
    metadata->show();
  }

  void createComposition(const LocalPromptData &prompt) {
    owner->setObjectName(QStringLiteral("pendingPromptCard"));
    owner->setStyleSheet(
        QStringLiteral("QFrame#pendingPromptCard{background:transparent;"
                       "border:1px solid transparent;border-radius:8px;}"));
    title->setText(QStringLiteral("You"));
    body = makeMarkdownLabel({}, content);
    metadata = makeLabel({}, "meta", content);
    contentLayout->addWidget(body);
    contentLayout->addWidget(metadata);
    createImageContainer();
    animationTimer = new QTimer(owner);
    animationTimer->setInterval(PendingAnimationIntervalMilliseconds);
    QObject::connect(animationTimer, &QTimer::timeout, owner, [this] {
      if (refreshPendingPresentation())
        owner->updateGeometry();
      owner->update();
    });
    updateComposition(prompt);
  }

  void updateComposition(const LocalPromptData &prompt) {
    setVisibleMarkdown(body, prompt.prompt);
    setImages(prompt.imagePaths);
    refreshPendingPresentation();
  }

  bool refreshPendingPresentation() {
    const auto *prompt = std::get_if<LocalPromptData>(&current.payload);
    if (!prompt)
      return false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool transitioning = acceptedTransitionActive(*prompt, now);
    const bool waiting = prompt->state == PromptState::Queued ||
                         prompt->state == PromptState::InFlight;
    const bool failed = prompt->state == PromptState::Failed;
    const QString foreground = waiting || transitioning
                                   ? QStringLiteral("#536b8f")
                               : failed ? QStringLiteral("#982f3d")
                                        : QStringLiteral("#1d2633");
    const QString style =
        QStringLiteral("background:transparent;color:%1;").arg(foreground);
    bool changed = false;
    for (QLabel *label : {title, body, metadata}) {
      if (label->styleSheet() != style) {
        label->setStyleSheet(style);
        changed = true;
      }
    }

    QString status;
    if (failed)
      status = prompt->error.isEmpty()
                   ? QStringLiteral("Not sent")
                   : QStringLiteral("Not sent: %1").arg(prompt->error);

    changed = setVisibleText(metadata, status) || changed;

    if (waiting || transitioning) {
      if (!animationTimer->isActive())
        animationTimer->start();
    } else {
      animationTimer->stop();
    }
    return changed;
  }

  ConversationCard *owner = nullptr;
  VisibleCardData current;
  bool collapsed = false;
  QVBoxLayout *layout = nullptr;
  QWidget *header = nullptr;
  QHBoxLayout *headerLayout = nullptr;
  QLabel *title = nullptr;
  QLabel *phaseSeparator = nullptr;
  QLabel *phase = nullptr;
  CardDisclosureButton *disclosure = nullptr;
  QWidget *content = nullptr;
  QVBoxLayout *contentLayout = nullptr;
  QLabel *body = nullptr;
  QLabel *metadata = nullptr;
  QLabel *detail = nullptr;
  ContentSizedTextView *command = nullptr;
  CommandOutputView *output = nullptr;
  QTimer *animationTimer = nullptr;
  QWidget *images = nullptr;
  QVBoxLayout *imageLayout = nullptr;
};

ConversationCard::ConversationCard(const VisibleCardData &data, QWidget *parent)
    : QFrame(parent), impl_(std::make_unique<Impl>(this, data)) {}

ConversationCard::~ConversationCard() = default;

CardKind ConversationCard::cardKind() const noexcept {
  return impl_->current.kind;
}

const VisibleCardData &ConversationCard::data() const noexcept {
  return impl_->current;
}

bool ConversationCard::isCollapsed() const noexcept { return impl_->collapsed; }

void ConversationCard::setCollapsed(bool collapsed) {
  impl_->setCollapsed(collapsed);
}

std::optional<CommandOutputView::ScrollState>
ConversationCard::commandOutputScrollState() const {
  if (!impl_->output)
    return std::nullopt;
  return impl_->output->scrollState();
}

void ConversationCard::restoreCommandOutputScrollState(
    const CommandOutputView::ScrollState &state) {
  if (impl_->output)
    impl_->output->restoreScrollState(state);
}

bool ConversationCard::apply(const VisibleCardData &data) {
  return impl_->apply(data);
}

bool ConversationCard::canApply(const VisibleCardData &data) const noexcept {
  return impl_->canApply(data);
}

void ConversationCard::paintEvent(QPaintEvent *event) {
  QFrame::paintEvent(event);
  if (impl_->current.kind != CardKind::LocalPrompt)
    return;
  const auto *prompt = std::get_if<LocalPromptData>(&impl_->current.payload);
  if (!prompt)
    return;

  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QRectF bounds = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const bool waiting = prompt->state == PromptState::Queued ||
                       prompt->state == PromptState::InFlight;
  const bool transitioning = acceptedTransitionActive(*prompt, now);
  const bool failed = prompt->state == PromptState::Failed;
  const QColor background = waiting || transitioning
                                ? QColor(QStringLiteral("#dbe7f8"))
                            : failed ? QColor(QStringLiteral("#fff0f2"))
                                     : QColor(QStringLiteral("#eaf2ff"));
  const QColor border = waiting || transitioning
                            ? QColor(QStringLiteral("#9eb9df"))
                        : failed ? QColor(QStringLiteral("#efb8c0"))
                                 : QColor(QStringLiteral("#bfd3f9"));
  painter.setBrush(background);
  painter.setPen(QPen(border, 1.0));
  painter.drawRoundedRect(bounds, 8.0, 8.0);

  if (!waiting && !transitioning)
    return;

  const qint64 phase = now % (2 * PendingHalfCycleMilliseconds);
  const qreal position =
      waiting ? phase <= PendingHalfCycleMilliseconds
                    ? qreal(phase) / PendingHalfCycleMilliseconds
                    : qreal(2 * PendingHalfCycleMilliseconds - phase) /
                          PendingHalfCycleMilliseconds
              : std::clamp(qreal(now - prompt->acceptedAtMilliseconds) /
                               AcknowledgementTransitionMilliseconds,
                           0.0, 1.0);
  const qreal center = bounds.left() + position * bounds.width();
  const qreal radius = std::max(28.0, bounds.width() * 0.24);
  QLinearGradient sweep(center - radius, 0.0, center + radius, 0.0);
  sweep.setColorAt(0.0, QColor(47, 111, 235, 0));
  sweep.setColorAt(0.5, QColor(117, 160, 239, 105));
  sweep.setColorAt(1.0, QColor(47, 111, 235, 0));
  QPainterPath clip;
  clip.addRoundedRect(bounds, 8.0, 8.0);
  painter.save();
  painter.setClipPath(clip);
  painter.fillRect(bounds, sweep);
  painter.restore();

  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(QColor(QStringLiteral("#79a0d7")), 1.5));
  painter.drawRoundedRect(bounds, 8.0, 8.0);
}

ConversationCard *createConversationCard(const VisibleCardData &data,
                                         QWidget *parent) {
  return new ConversationCard(data, parent);
}

} // namespace codexui::codex::middle
