// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_MIDDLEREGIONWIDGET_H
#define CODEXUI_CODEX_MIDDLE_MIDDLEREGIONWIDGET_H

#include <QWidget>

#include <functional>

class QEvent;
class QFrame;
class QLabel;
class QSplitter;

namespace codexui::codex::middle {

class ComposerPane;
class ConversationView;
class InspectorPane;
class ThreadPane;

// The sole geometry owner for the three-pane workspace.  Protocol and domain
// decisions remain in ShellWidget; this class owns only visible layout and
// wheel routing across the complete center strip.
class MiddleRegionWidget final : public QWidget {
public:
  explicit MiddleRegionWidget(QWidget *parent = nullptr);

  [[nodiscard]] ThreadPane &threads() const noexcept;
  [[nodiscard]] ConversationView &conversation() const noexcept;
  [[nodiscard]] ComposerPane &composer() const noexcept;
  [[nodiscard]] InspectorPane &inspector() const noexcept;
  [[nodiscard]] QSplitter *splitterWidget() const noexcept;

  void setThreadHeading(QString title, QString metadata);
  void showNotice(QString message, bool error = true);
  void showSidebar(bool visible);
  void showInspector(bool visible);
  [[nodiscard]] bool sidebarVisible() const noexcept;
  [[nodiscard]] bool inspectorVisible() const noexcept;
  void setPaneVisibilityAction(
      std::function<void(bool sidebarVisible, bool inspectorVisible)> action);

  // Called by ShellWidget's application event filter. Returns true only when
  // a wheel/touchpad event was consumed by the conversation.
  bool routeScrollEvent(QObject *watched, QEvent *event);

private:
  QSplitter *splitter = nullptr;
  ThreadPane *threadPane = nullptr;
  QFrame *conversationRegion = nullptr;
  QLabel *conversationTitle = nullptr;
  QLabel *conversationMetadata = nullptr;
  QFrame *noticeBar = nullptr;
  QLabel *noticeLabel = nullptr;
  ConversationView *conversationView = nullptr;
  ComposerPane *composerPane = nullptr;
  InspectorPane *inspectorPane = nullptr;
  std::function<void(bool, bool)> paneVisibilityAction;
};

} // namespace codexui::codex::middle

#endif
