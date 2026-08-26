// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ComposerPane.h"

#include "codex/TurnSettingsWidget.h"
#include "codex/ui/ExpandingPromptEditor.h"

#include <QDir>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace codexui::codex::middle {
namespace {

constexpr int ControlHeight = 32;
constexpr int HorizontalInset = 24;
constexpr int DividerOutset = 10;
constexpr int BottomInset = 12;
constexpr int AttachmentRowHeight = 28;
constexpr int MaximumVisibleAttachments = 4;

QLabel *makeLabel(QString value, const char *kind) {
  auto *label = new QLabel(std::move(value));
  label->setProperty("kind", kind);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  return label;
}

void clearLayout(QLayout *layout) {
  while (QLayoutItem *item = layout->takeAt(0)) {
    delete item->widget();
    delete item;
  }
}

void repolish(QWidget *widget) {
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
  widget->update();
}

} // namespace

ComposerPane::ComposerPane(QWidget *anchor)
    : QWidget(anchor), anchor_(anchor), reserve_(new QWidget(anchor)) {
  Q_ASSERT(anchor_);
  setObjectName(QStringLiteral("composerOverlay"));
  setAttribute(Qt::WA_StyledBackground, true);
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  anchor_->installEventFilter(this);

  reserve_->setObjectName(QStringLiteral("composerCanonicalReserve"));
  reserve_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  reserve_->setFixedHeight(0);

  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(0, 8, 0, 0);
  root->setSpacing(8);

  auto *boundary = new QFrame(this);
  boundary->setProperty("kind", "standardDivider");
  boundary->setFixedHeight(1);
  root->addWidget(boundary);

  auto *surfaces = new QWidget(this);
  auto *surfacesLayout = new QVBoxLayout(surfaces);
  surfacesLayout->setContentsMargins(DividerOutset, 0, DividerOutset, 0);
  surfacesLayout->setSpacing(8);
  root->addWidget(surfaces);

  attention_ = new QFrame(surfaces);
  attention_->setProperty("kind", "orangeBadge");
  auto *attentionLayout = new QHBoxLayout(attention_);
  attentionLayout->setContentsMargins(10, 6, 10, 6);
  attentionLayout->addWidget(makeLabel(
      QStringLiteral("A Codex request needs attention"), "attentionSection"));
  attentionLayout->addStretch();
  auto *deny = new QPushButton(QStringLiteral("Deny"), attention_);
  auto *review = new QPushButton(QStringLiteral("Review"), attention_);
  deny->setProperty("kind", "destructive");
  review->setProperty("kind", "request");
  attentionLayout->addWidget(deny);
  attentionLayout->addWidget(review);
  connect(deny, &QPushButton::clicked, this, [this] {
    if (actions_.deny)
      actions_.deny();
  });
  connect(review, &QPushButton::clicked, this, [this] {
    if (actions_.review)
      actions_.review();
  });
  attention_->hide();
  surfacesLayout->addWidget(attention_);

  turnSettings_ = new TurnSettingsWidget(surfaces);
  surfacesLayout->addWidget(turnSettings_);

  composer_ = new QFrame(surfaces);
  composer_->setProperty("kind", "composer");
  auto *composerLayout = new QVBoxLayout(composer_);
  composerLayout->setContentsMargins(10, 8, 8, 8);
  composerLayout->setSpacing(6);

  attachmentPanel_ = new QFrame(composer_);
  attachmentPanel_->setProperty("kind", "summary");
  auto *attachmentPanelLayout = new QVBoxLayout(attachmentPanel_);
  attachmentPanelLayout->setContentsMargins(6, 6, 6, 6);
  attachmentPanelLayout->setSpacing(4);
  attachmentListScroll_ = new QScrollArea(attachmentPanel_);
  attachmentListScroll_->setWidgetResizable(true);
  attachmentListScroll_->setFrameShape(QFrame::NoFrame);
  attachmentListScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto *attachmentContent = new QWidget;
  attachmentListLayout_ = new QVBoxLayout(attachmentContent);
  attachmentListLayout_->setContentsMargins(0, 0, 0, 0);
  attachmentListLayout_->setSpacing(4);
  attachmentListScroll_->setWidget(attachmentContent);
  attachmentPanelLayout->addWidget(attachmentListScroll_);
  attachmentPanel_->hide();
  composerLayout->addWidget(attachmentPanel_);

  composerBody_ = new QWidget(composer_);
  composerBody_->installEventFilter(this);
  composerGrid_ = new QGridLayout(composerBody_);
  composerGrid_->setContentsMargins(0, 0, 0, 0);
  composerGrid_->setHorizontalSpacing(8);
  composerGrid_->setVerticalSpacing(6);
  composerGrid_->setColumnStretch(1, 1);

  attachmentButton_ = new QToolButton(composerBody_);
  attachmentButton_->setProperty("kind", "composerAction");
  attachmentButton_->setIcon(
      QIcon::fromTheme(QIcon::ThemeIcon::MailAttachment));
  attachmentButton_->setIconSize(QSize(16, 16));
  attachmentButton_->setToolTip(QStringLiteral("Attach files"));
  attachmentButton_->setAccessibleName(QStringLiteral("Attach files"));
  attachmentButton_->setFixedSize(ControlHeight, ControlHeight);

  promptEditor_ = new codexui::ExpandingPromptEditor(composerBody_);
  sendButton_ = new QPushButton(QStringLiteral("Send"), composerBody_);
  sendButton_->setProperty("kind", "primary");
  sendButton_->setFixedSize(62, ControlHeight);
  stopButton_ = new QPushButton(QStringLiteral("Stop"), composerBody_);
  stopButton_->setProperty("kind", "stop");
  stopButton_->setFixedSize(54, ControlHeight);
  stopButton_->hide();

  composerGrid_->addWidget(attachmentButton_, 0, 0);
  composerGrid_->addWidget(promptEditor_, 0, 1);
  composerGrid_->addWidget(sendButton_, 0, 2);
  composerLayout->addWidget(composerBody_);
  surfacesLayout->addWidget(composer_);

  connect(sendButton_, &QPushButton::clicked, this, [this] { submitDraft(); });
  connect(promptEditor_, &codexui::ExpandingPromptEditor::submitRequested, this,
          [this] { submitDraft(); });
  connect(promptEditor_, &QPlainTextEdit::textChanged, this, [this] {
    refreshAdaptiveLayout();
    synchronizeGeometry();
  });
  connect(promptEditor_, &codexui::ExpandingPromptEditor::editorHeightChanged,
          this, [this](int) {
            refreshAdaptiveLayout();
            synchronizeGeometry();
          });
  connect(stopButton_, &QPushButton::clicked, this, [this] {
    if (actions_.stop)
      actions_.stop();
  });
  connect(attachmentButton_, &QToolButton::clicked, this, [this] {
    if (actions_.attach)
      actions_.attach();
  });

  refreshAttachments();
  synchronizeGeometry();
  QTimer::singleShot(0, this, [this] {
    // The compact reserve is measured only after the splitter has assigned
    // the center pane its real width. Until then the overlay may be placed,
    // but its construction-time size must not become canonical.
    canonicalCaptureEnabled_ = true;
    synchronizeGeometry();
  });
  raise();
}

void ComposerPane::setActions(Actions actions) {
  actions_ = std::move(actions);
}

void ComposerPane::setExtraOverlayHeightAction(
    std::function<void(int)> action) {
  extraOverlayHeightAction_ = std::move(action);
  if (extraOverlayHeightAction_)
    extraOverlayHeightAction_(extraHeight_);
}

void ComposerPane::setAttachments(std::vector<AttachmentDraft> attachments) {
  attachments_ = std::move(attachments);
  refreshAttachments();
  synchronizeGeometry();
}

const std::vector<AttachmentDraft> &ComposerPane::attachments() const noexcept {
  return attachments_;
}

void ComposerPane::setAttentionVisible(bool visible) {
  if (attention_->isVisible() == visible)
    return;
  attention_->setVisible(visible);
  synchronizeGeometry();
}

void ComposerPane::setActiveTurn(bool active) {
  if (activeTurn_ == active)
    return;
  activeTurn_ = active;
  stopButton_->setVisible(active);
  sendButton_->setText(active ? QStringLiteral("Steer")
                              : QStringLiteral("Send"));
  refreshActionStyle();
  refreshAdaptiveLayout();
  synchronizeGeometry();
}

void ComposerPane::setCanSubmit(bool canSubmit) {
  // Admission never locks or greys the editor; independent prompts may be
  // entered while earlier submissions await their real app-server callback.
  promptEditor_->setEnabled(true);
  sendButton_->setEnabled(canSubmit);
  attachmentButton_->setEnabled(canSubmit);
  for (QPushButton *button : attachmentPanel_->findChildren<QPushButton *>())
    button->setEnabled(true);
}

void ComposerPane::setSettingsEnabled(bool enabled) {
  turnSettings_->setControlsEnabled(enabled);
}

void ComposerPane::clearDraft() {
  promptEditor_->clear();
  attachments_.clear();
  refreshAttachments();
  synchronizeGeometry();
}

void ComposerPane::synchronizeGeometry() {
  if (synchronizing_ || !anchor_ || !layout())
    return;
  synchronizing_ = true;

  const int overlayInset = HorizontalInset - DividerOutset;
  const int width = std::max(0, anchor_->width() - 2 * overlayInset);
  if (this->width() != width)
    resize(width, std::max(0, height()));
  layout()->activate();
  refreshAdaptiveLayout();
  layout()->activate();

  const int availableHeight = std::max(0, anchor_->height() - BottomInset);
  const int naturalHeight = sizeHint().height();
  const int wantedHeight = std::min(naturalHeight, availableHeight);
  if (canonicalCaptureEnabled_ && canonicalHeight_ == 0 && naturalHeight > 0) {
    // The reserve describes the compact surface, not the amount which happened
    // to fit into an unlaid-out parent during construction.
    canonicalHeight_ = naturalHeight;
    reserve_->setFixedHeight(canonicalHeight_);
  }
  const int wantedExtra = canonicalCaptureEnabled_ && canonicalHeight_ > 0
                              ? std::max(0, wantedHeight - canonicalHeight_)
                              : 0;
  const QRect wantedGeometry(overlayInset, availableHeight - wantedHeight,
                             width, wantedHeight);
  const bool geometryChanged = geometry() != wantedGeometry;
  if (geometryChanged)
    setGeometry(wantedGeometry);
  // The natural height was calculated after the prompt layout changed, while
  // the child layout still had the previous overlay geometry. Lay out the
  // children once against the final rectangle so fixed surfaces cannot remain
  // compressed and leave a false gap during upward growth.
  if (geometryChanged) {
    layout()->invalidate();
    layout()->activate();
  }
  raise();

  if (wantedExtra != extraHeight_) {
    extraHeight_ = wantedExtra;
    if (extraOverlayHeightAction_)
      extraOverlayHeightAction_(extraHeight_);
  }
  synchronizing_ = false;
}

bool ComposerPane::event(QEvent *event) {
  const bool result = QWidget::event(event);
  if ((event->type() == QEvent::LayoutRequest ||
       event->type() == QEvent::Show) &&
      !synchronizing_)
    synchronizeGeometry();
  return result;
}

bool ComposerPane::eventFilter(QObject *watched, QEvent *event) {
  if (watched == anchor_ &&
      (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
       event->type() == QEvent::LayoutRequest))
    synchronizeGeometry();
  if (watched == composerBody_ &&
      (event->type() == QEvent::Resize || event->type() == QEvent::Show ||
       event->type() == QEvent::LayoutRequest)) {
    refreshAdaptiveLayout();
    synchronizeGeometry();
  }
  return QWidget::eventFilter(watched, event);
}

void ComposerPane::submitDraft() {
  const QString prompt = promptEditor_->toPlainText().trimmed();
  if (prompt.isEmpty() || !sendButton_->isEnabled() || !actions_.submit)
    return;
  std::vector<AttachmentDraft> attachments = attachments_;
  if (actions_.submit(prompt, std::move(attachments)))
    clearDraft();
}

void ComposerPane::refreshAttachments() {
  clearLayout(attachmentListLayout_);
  const bool hasAttachments = !attachments_.empty();
  attachmentPanel_->setVisible(hasAttachments);
  if (!hasAttachments) {
    attachmentListScroll_->setFixedHeight(0);
    return;
  }

  for (std::size_t index = 0; index < attachments_.size(); ++index) {
    const AttachmentDraft &attachment = attachments_[index];
    auto *row = new QWidget;
    row->setFixedHeight(AttachmentRowHeight);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 2, 0, 2);
    rowLayout->setSpacing(5);

    auto *remove = new QPushButton(QStringLiteral("X"), row);
    remove->setAccessibleName(QStringLiteral("Remove %1").arg(attachment.name));
    remove->setToolTip(QStringLiteral("Remove attachment"));
    remove->setFixedSize(18, 18);
    remove->setProperty("kind", "destructiveCompact");
    connect(remove, &QPushButton::clicked, this, [this, index] {
      if (index >= attachments_.size())
        return;
      attachments_.erase(attachments_.begin() +
                         static_cast<std::ptrdiff_t>(index));
      refreshAttachments();
      synchronizeGeometry();
    });

    auto *fileBox = new QFrame(row);
    fileBox->setObjectName(QStringLiteral("attachmentFileBox"));
    fileBox->setStyleSheet(
        QStringLiteral("QFrame#attachmentFileBox{background:#ffffff;"
                       "border:1px solid #d7dee8;border-radius:6px;}"));
    auto *fileLayout = new QHBoxLayout(fileBox);
    fileLayout->setContentsMargins(8, 1, 8, 1);
    auto *name = makeLabel(attachment.name, "meta");
    name->setToolTip(QDir::toNativeSeparators(attachment.path));
    fileLayout->addWidget(name);
    rowLayout->addWidget(fileBox, 1);
    rowLayout->addWidget(remove, 0, Qt::AlignVCenter);
    attachmentListLayout_->addWidget(row);
  }

  const int visibleRows = std::min<int>(static_cast<int>(attachments_.size()),
                                        MaximumVisibleAttachments);
  attachmentListScroll_->setFixedHeight(visibleRows * AttachmentRowHeight +
                                        (visibleRows - 1) * 4);
}

void ComposerPane::refreshAdaptiveLayout() {
  if (!composerBody_ || composerBody_->width() <= 0)
    return;
  const int visibleControls = activeTurn_ ? 3 : 2;
  const int controlsWidth = attachmentButton_->width() + sendButton_->width() +
                            (activeTurn_ ? stopButton_->width() : 0);
  const int compactEditorWidth =
      composerBody_->contentsRect().width() - controlsWidth -
      visibleControls * composerGrid_->horizontalSpacing();
  const bool shouldExpand =
      promptEditor_->requiresExpandedLayout(compactEditorWidth);
  if (shouldExpand == expanded_ &&
      composerGrid_->indexOf(stopButton_) == (activeTurn_ ? 3 : -1))
    return;

  expanded_ = shouldExpand;
  composerGrid_->removeWidget(attachmentButton_);
  composerGrid_->removeWidget(promptEditor_);
  composerGrid_->removeWidget(sendButton_);
  composerGrid_->removeWidget(stopButton_);
  if (expanded_) {
    composerGrid_->addWidget(promptEditor_, 0, 0, 1, 4);
    composerGrid_->addWidget(attachmentButton_, 1, 0);
    composerGrid_->addWidget(sendButton_, 1, 2);
    if (activeTurn_)
      composerGrid_->addWidget(stopButton_, 1, 3);
  } else {
    composerGrid_->addWidget(attachmentButton_, 0, 0);
    composerGrid_->addWidget(promptEditor_, 0, 1);
    composerGrid_->addWidget(sendButton_, 0, 2);
    if (activeTurn_)
      composerGrid_->addWidget(stopButton_, 0, 3);
  }
  composerGrid_->invalidate();
  composerGrid_->activate();
}

void ComposerPane::refreshActionStyle() {
  const QString kind =
      activeTurn_ ? QStringLiteral("steer") : QStringLiteral("primary");
  if (sendButton_->property("kind").toString() == kind)
    return;
  sendButton_->setProperty("kind", kind);
  repolish(sendButton_);
}

} // namespace codexui::codex::middle
