// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_COMPOSERPANE_H
#define CODEXUI_CODEX_MIDDLE_COMPOSERPANE_H

#include "codex/AttachmentDraft.h"
#include "codex/TurnSettingsPolicy.h"

#include <QWidget>

#include <cstddef>
#include <functional>
#include <vector>

class QFrame;
class QGridLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

namespace codexui {
class ExpandingPromptEditor;
}

namespace codexui::codex {
class TurnSettingsWidget;

namespace middle {

// The center layout is the sole composer geometry owner.
class ComposerPane final : public QWidget {
public:
  struct Actions {
    std::function<bool(QString, std::vector<AttachmentDraft>)> submit;
    std::function<void()> stop;
    std::function<void()> attach;
    std::function<void()> accept;
    std::function<void()> review;
    std::function<void()> deny;
  };

  explicit ComposerPane(QWidget *parent = nullptr);
  ~ComposerPane() override;

  void setActions(Actions actions);
  void setAttachments(std::vector<AttachmentDraft> attachments);
  [[nodiscard]] const std::vector<AttachmentDraft> &
  attachments() const noexcept;
  void setAttentionVisible(bool visible);
  void setAttentionRequest(QString title, QString detail, bool directAccept,
                           QString acceptLabel, bool directReject,
                           QString rejectLabel, bool replacesTarget);
  void setAttentionActionEnabled(bool acceptEnabled, bool rejectEnabled,
                                 bool reviewEnabled);
  void setActiveTurn(bool active);
  void setCanSubmit(bool canSubmit);
  void setSettingsEnabled(bool enabled);
  void setTurnSettingsContext(TurnSettingsContext context);
  void clearDraft();
  [[nodiscard]] codexui::ExpandingPromptEditor *promptEditor() const noexcept {
    return promptEditor_;
  }
  [[nodiscard]] TurnSettingsPolicy &turnSettings() noexcept {
    return turnSettingsPolicy_;
  }

private:
  void submitDraft();
  void refreshAttachments();
  void refreshAttachmentGeometry();
  void refreshAdaptiveLayout();
  void refreshActionStyle();
  void refreshSubmissionEnabled();

  QFrame *attention_ = nullptr;
  QLabel *attentionTitle_ = nullptr;
  QLabel *attentionDetail_ = nullptr;
  QPushButton *attentionRejectButton_ = nullptr;
  QPushButton *attentionAcceptButton_ = nullptr;
  QPushButton *attentionReviewButton_ = nullptr;
  TurnSettingsPolicy turnSettingsPolicy_;
  TurnSettingsWidget *turnSettings_ = nullptr;
  QFrame *composer_ = nullptr;
  QFrame *attachmentPanel_ = nullptr;
  QScrollArea *attachmentListScroll_ = nullptr;
  QVBoxLayout *attachmentListLayout_ = nullptr;
  QWidget *composerBody_ = nullptr;
  QGridLayout *composerGrid_ = nullptr;
  QToolButton *attachmentButton_ = nullptr;
  codexui::ExpandingPromptEditor *promptEditor_ = nullptr;
  QPushButton *sendButton_ = nullptr;
  QPushButton *stopButton_ = nullptr;

  Actions actions_;
  std::vector<AttachmentDraft> attachments_;
  bool activeTurn_ = false;
  bool canSubmit_ = false;
  bool expanded_ = false;
};

} // namespace middle
} // namespace codexui::codex

#endif // CODEXUI_CODEX_MIDDLE_COMPOSERPANE_H
