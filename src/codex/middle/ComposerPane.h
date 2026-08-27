// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_COMPOSERPANE_H
#define CODEXUI_CODEX_MIDDLE_COMPOSERPANE_H

#include "codex/FileSelectionDialog.h"

#include <QWidget>

#include <cstddef>
#include <functional>
#include <vector>

class QEvent;
class QFrame;
class QGridLayout;
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

// A bottom-aligned overlay.  Only canonicalReserve() participates in the
// center layout; all height above that reserve is reported as trailing
// conversation space.
class ComposerPane final : public QWidget {
public:
  struct Actions {
    std::function<bool(QString, std::vector<AttachmentDraft>)> submit;
    std::function<void()> stop;
    std::function<void()> attach;
    std::function<void()> review;
    std::function<void()> deny;
  };

  explicit ComposerPane(QWidget *anchor);

  void setActions(Actions actions);
  void setExtraOverlayHeightAction(std::function<void(int)> action);
  void setAttachments(std::vector<AttachmentDraft> attachments);
  [[nodiscard]] const std::vector<AttachmentDraft> &
  attachments() const noexcept;
  void setAttentionVisible(bool visible);
  void setActiveTurn(bool active);
  void setCanSubmit(bool canSubmit);
  void setSettingsEnabled(bool enabled);
  void clearDraft();
  void synchronizeGeometry();

  [[nodiscard]] QWidget *canonicalReserve() const noexcept { return reserve_; }
  [[nodiscard]] int canonicalReserveHeight() const noexcept {
    return canonicalHeight_;
  }
  [[nodiscard]] int extraOverlayHeight() const noexcept { return extraHeight_; }
  [[nodiscard]] codexui::ExpandingPromptEditor *promptEditor() const noexcept {
    return promptEditor_;
  }
  [[nodiscard]] TurnSettingsWidget *turnSettings() const noexcept {
    return turnSettings_;
  }

protected:
  bool event(QEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  void submitDraft();
  void refreshAttachments();
  void refreshAdaptiveLayout();
  void refreshActionStyle();

  QWidget *anchor_ = nullptr;
  QWidget *reserve_ = nullptr;
  QFrame *attention_ = nullptr;
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
  std::function<void(int)> extraOverlayHeightAction_;
  std::vector<AttachmentDraft> attachments_;
  int canonicalHeight_ = 0;
  int extraHeight_ = 0;
  bool activeTurn_ = false;
  bool expanded_ = false;
  bool synchronizing_ = false;
  bool canonicalCaptureEnabled_ = false;
};

} // namespace middle
} // namespace codexui::codex

#endif // CODEXUI_CODEX_MIDDLE_COMPOSERPANE_H
