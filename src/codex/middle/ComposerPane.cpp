// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ComposerPane.h"

#include "codex/TurnSettingsWidget.h"
#include "codex/middle/ConversationPresentation.h"
#include "codex/ui/ExpandingPromptEditor.h"
#include "codex/ui/UiStyle.h"

#include <QDir>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <string_view>
#include <utility>

namespace codexui::codex::middle {
namespace {

constexpr int ControlHeight = 32;
constexpr int DividerOutset = 10;
constexpr int AttachmentRowHeight = 28;
constexpr int MaximumVisibleAttachments = 4;

using UiStyle::makeLabel;

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
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

ComposerPane::ComposerPane(QWidget *parent) : QWidget(parent) {
  setObjectName(QStringLiteral("composerOverlay"));
  setAttribute(Qt::WA_StyledBackground, true);
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

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
  attentionLayout->setSpacing(8);
  auto *attentionText = new QWidget(attention_);
  auto *attentionTextLayout = new QVBoxLayout(attentionText);
  attentionTextLayout->setContentsMargins(0, 0, 0, 0);
  attentionTextLayout->setSpacing(2);
  attentionTitle_ = makeLabel(QStringLiteral("A Codex request needs attention"),
                              "attentionSection");
  attentionDetail_ =
      makeLabel(QStringLiteral("Review the pending request."), "meta");
  attentionTextLayout->addWidget(attentionTitle_);
  attentionTextLayout->addWidget(attentionDetail_);
  attentionLayout->addWidget(attentionText, 1);
  attentionLayout->addStretch();
  attentionRejectButton_ =
      new QPushButton(QStringLiteral("Reject"), attention_);
  attentionAcceptButton_ =
      new QPushButton(QStringLiteral("Accept"), attention_);
  attentionReviewButton_ =
      new QPushButton(QStringLiteral("Review"), attention_);
  attentionRejectButton_->setObjectName(
      QStringLiteral("pendingRequestRejectButton"));
  attentionAcceptButton_->setObjectName(
      QStringLiteral("pendingRequestAcceptButton"));
  attentionReviewButton_->setObjectName(
      QStringLiteral("pendingRequestReviewButton"));
  attentionRejectButton_->setProperty("kind", "destructive");
  attentionAcceptButton_->setProperty("kind", "request");
  attentionReviewButton_->setProperty("kind", "request");
  attentionLayout->addWidget(attentionRejectButton_);
  attentionLayout->addWidget(attentionAcceptButton_);
  attentionLayout->addWidget(attentionReviewButton_);
  connect(attentionRejectButton_, &QPushButton::clicked, this, [this] {
    if (actions_.deny)
      actions_.deny();
  });
  connect(attentionAcceptButton_, &QPushButton::clicked, this, [this] {
    if (actions_.accept)
      actions_.accept();
  });
  connect(attentionReviewButton_, &QPushButton::clicked, this, [this] {
    if (actions_.review)
      actions_.review();
  });
  attentionReviewButton_->hide();
  attention_->hide();
  surfacesLayout->addWidget(attention_);

  turnSettings_ = new TurnSettingsWidget(turnSettingsPolicy_, surfaces);
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
  sendButton_->setObjectName(QStringLiteral("composerSendButton"));
  sendButton_->setProperty("kind", "primary");
  sendButton_->setToolTip(QStringLiteral("Send prompt (Enter)"));
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
    refreshSubmissionEnabled();
    refreshAdaptiveLayout();
  });
  connect(promptEditor_, &codexui::ExpandingPromptEditor::focusStateChanged,
          this, [this](bool focused) {
            composer_->setProperty("focused", focused);
            repolish(composer_);
          });
  connect(promptEditor_, &codexui::ExpandingPromptEditor::editorHeightChanged,
          this, [this](int) {
            refreshAdaptiveLayout();
            updateGeometry();
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
  refreshSubmissionEnabled();
}

ComposerPane::~ComposerPane() { delete turnSettings_; }

void ComposerPane::setActions(Actions actions) {
  actions_ = std::move(actions);
}

void ComposerPane::setAttachments(std::vector<AttachmentDraft> attachments) {
  attachments_ = std::move(attachments);
  refreshAttachments();
  updateGeometry();
}

const std::vector<AttachmentDraft> &ComposerPane::attachments() const noexcept {
  return attachments_;
}

void ComposerPane::setAttentionVisible(bool visible) {
  if (attention_->isHidden() != visible)
    return;
  const bool transferFocus = !visible && (attentionRejectButton_->hasFocus() ||
                                          attentionAcceptButton_->hasFocus() ||
                                          attentionReviewButton_->hasFocus());
  attention_->setVisible(visible);
  if (transferFocus)
    promptEditor_->setFocus();
  updateGeometry();
}

void ComposerPane::setAttentionRequest(QString title, QString detail,
                                       bool directAccept, QString acceptLabel,
                                       bool directReject, QString rejectLabel,
                                       bool replacesTarget) {
  if (title.isEmpty())
    title = QStringLiteral("A Codex request needs attention");
  if (detail.isEmpty())
    detail = QStringLiteral("Review the pending request.");
  if (acceptLabel.isEmpty())
    acceptLabel = QStringLiteral("Accept");
  if (rejectLabel.isEmpty())
    rejectLabel = QStringLiteral("Decline");
  const bool targetHadFocus =
      replacesTarget && (attentionRejectButton_->hasFocus() ||
                         attentionAcceptButton_->hasFocus() ||
                         attentionReviewButton_->hasFocus());
  const bool transferToReview =
      (attentionRejectButton_->hasFocus() && !directReject) ||
      (attentionAcceptButton_->hasFocus() && !directAccept);
  const bool unchanged = !replacesTarget && attentionTitle_->text() == title &&
                         attentionDetail_->text() == detail &&
                         !attentionRejectButton_->isHidden() == directReject &&
                         !attentionAcceptButton_->isHidden() == directAccept &&
                         !attentionReviewButton_->isHidden() &&
                         attentionAcceptButton_->text() == acceptLabel &&
                         attentionRejectButton_->text() == rejectLabel;
  if (unchanged)
    return;
  presentation::setAccessibleNameIfChanged(*attention_, title);
  presentation::setAccessibleDescriptionIfChanged(*attention_, detail);
  attentionTitle_->setText(std::move(title));
  attentionDetail_->setText(std::move(detail));
  attentionRejectButton_->setText(std::move(rejectLabel));
  attentionRejectButton_->setVisible(directReject);
  attentionAcceptButton_->setText(std::move(acceptLabel));
  attentionAcceptButton_->setVisible(directAccept);
  attentionReviewButton_->setVisible(true);
  if (targetHadFocus)
    promptEditor_->setFocus();
  else if (transferToReview)
    attentionReviewButton_->setFocus();
  updateGeometry();
}

void ComposerPane::setAttentionActionEnabled(bool acceptEnabled,
                                             bool rejectEnabled,
                                             bool reviewEnabled) {
  const bool transferFocus =
      (attentionRejectButton_->hasFocus() && !rejectEnabled) ||
      (attentionAcceptButton_->hasFocus() && !acceptEnabled) ||
      (attentionReviewButton_->hasFocus() && !reviewEnabled);
  attentionRejectButton_->setEnabled(rejectEnabled);
  attentionAcceptButton_->setEnabled(acceptEnabled);
  attentionReviewButton_->setEnabled(reviewEnabled);
  if (transferFocus) {
    if (reviewEnabled)
      attentionReviewButton_->setFocus();
    else
      promptEditor_->setFocus();
  }
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
  updateGeometry();
}

void ComposerPane::setCanSubmit(bool canSubmit) {
  // Admission never locks or greys the editor; independent prompts may be
  // entered while earlier submissions await their real app-server callback.
  promptEditor_->setEnabled(true);
  canSubmit_ = canSubmit;
  refreshSubmissionEnabled();
  attachmentButton_->setEnabled(canSubmit);
  for (QPushButton *button : attachmentPanel_->findChildren<QPushButton *>())
    button->setEnabled(true);
}

void ComposerPane::setSettingsEnabled(bool enabled) {
  turnSettings_->setEnabled(enabled);
  turnSettings_->setToolTip(
      enabled ? QString{}
              : QStringLiteral("Settings apply when starting a turn"));
}

void ComposerPane::setTurnSettingsContext(TurnSettingsContext context) {
  turnSettings_->setContext(std::move(context));
}

void ComposerPane::clearDraft() {
  promptEditor_->clear();
  attachments_.clear();
  refreshAttachments();
  updateGeometry();
}

void ComposerPane::submitDraft() {
  const QString prompt = promptEditor_->toPlainText();
  if (prompt.trimmed().isEmpty() || !sendButton_->isEnabled() ||
      !actions_.submit)
    return;
  std::vector<AttachmentDraft> attachments = attachments_;
  if (actions_.submit(prompt, std::move(attachments)))
    clearDraft();
}

void ComposerPane::refreshAttachments() {
  clearLayout(attachmentListLayout_);
  if (attachments_.empty()) {
    refreshAttachmentGeometry();
    return;
  }

  for (const AttachmentDraft &attachment : attachments_) {
    const std::string attachmentPath = attachment.path;
    const QString attachmentName = text(attachment.name);
    auto *row = new QWidget;
    row->setFixedHeight(AttachmentRowHeight);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 2, 0, 2);
    rowLayout->setSpacing(5);

    auto *remove = new QPushButton(QStringLiteral("X"), row);
    remove->setAccessibleName(
        QStringLiteral("Remove %1").arg(attachmentName));
    remove->setToolTip(QStringLiteral("Remove attachment"));
    remove->setFixedSize(18, 18);
    remove->setProperty("kind", "destructiveCompact");
    connect(remove, &QPushButton::clicked, this,
            [this, row, attachmentPath, attachmentName] {
      const auto attachment = std::ranges::find(
          attachments_, attachmentPath, &AttachmentDraft::path);
      if (attachment == attachments_.end())
        return;
      attachments_.erase(attachment);
      const int rowIndex = attachmentListLayout_->indexOf(row);
      if (QLayoutItem *item = attachmentListLayout_->takeAt(rowIndex))
        delete item;
      row->hide();
      row->deleteLater();
      refreshAttachmentGeometry();
      QWidget *focusTarget = nullptr;
      if (rowIndex >= 0 && rowIndex < attachmentListLayout_->count())
        focusTarget = attachmentListLayout_->itemAt(rowIndex)->widget();
      else if (rowIndex > 0)
        focusTarget = attachmentListLayout_->itemAt(rowIndex - 1)->widget();
      if (focusTarget)
        focusTarget = focusTarget->findChild<QPushButton *>();
      (focusTarget ? focusTarget : attachmentButton_)->setFocus();
      presentation::announce(
          *attachmentButton_,
          QStringLiteral("Removed attachment %1").arg(attachmentName));
      updateGeometry();
    });

    auto *fileBox = new QFrame(row);
    fileBox->setObjectName(QStringLiteral("attachmentFileBox"));
    auto *fileLayout = new QHBoxLayout(fileBox);
    fileLayout->setContentsMargins(8, 1, 8, 1);
    auto *name = makeLabel(text(attachment.name), "meta");
    name->setToolTip(QDir::toNativeSeparators(text(attachment.path)));
    fileLayout->addWidget(name);
    rowLayout->addWidget(fileBox, 1);
    rowLayout->addWidget(remove, 0, Qt::AlignVCenter);
    attachmentListLayout_->addWidget(row);
  }

  refreshAttachmentGeometry();
}

void ComposerPane::refreshAttachmentGeometry() {
  const bool hasAttachments = !attachments_.empty();
  attachmentPanel_->setVisible(hasAttachments);
  if (!hasAttachments) {
    attachmentListScroll_->setFixedHeight(0);
    return;
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

void ComposerPane::refreshSubmissionEnabled() {
  static const QRegularExpression NonWhitespace(QStringLiteral("\\S"));
  sendButton_->setEnabled(
      canSubmit_ && !promptEditor_->document()->find(NonWhitespace).isNull());
}

} // namespace codexui::codex::middle
