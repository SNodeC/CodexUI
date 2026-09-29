// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_THREADPANE_H
#define CODEXUI_CODEX_MIDDLE_THREADPANE_H

#include "codex/nodegraph/Messages.h"
#include "codex/ui/UiViewState.h"

#include <QFrame>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

class QMenu;
class QAction;
class QToolButton;
class QTimer;

namespace codexui::codex {
namespace middle {

class ThreadTreeItem;
class ThreadTreeWidget;

class ThreadPane final : public QFrame {
public:
  enum class SortCriterion { Alphanumeric, Created, Recency, SectionOrder };

  using ThreadAction = std::function<void(const nodegraph::NodeRef &)>;

  struct VisibleThread {
    std::string id;
    std::string presentationKey;
    nodegraph::NodeRef target;
  };

  struct Actions {
    std::function<void()> newThread;
    std::function<void()> loadMore;
    std::function<void()> hide;
    ThreadAction select;
    ThreadAction reload;
    ThreadAction rename;
    ThreadAction fork;
    ThreadAction forkWithOptions;
    ThreadAction toggleArchive;
    ThreadAction remove;
    ThreadAction timing;
    std::function<void()> browserChanged;
    std::function<void()> refreshGroups;
    std::function<void(nlohmann::json)> browse;
    std::function<void(nodegraph::RuntimeActionKind, nlohmann::json)> manage;
    std::function<void(std::string, std::string, std::string)> newGroupedThread;
  };

  explicit ThreadPane(QWidget *parent = nullptr);
  ~ThreadPane() override;

  void setActions(Actions actions);
  void refresh(const ui::ThreadListSnapshot &snapshot);
  [[nodiscard]] bool applyRowPresentation(const ui::ThreadListRow &row);
  void beginOptimisticThread(std::string id, std::string presentationKey,
                             std::string title, std::string cwd);
  void discardOptimisticThread(const std::string &threadId);
  void setSortCriterion(SortCriterion criterion);
  [[nodiscard]] SortCriterion currentSortCriterion() const noexcept;
  [[nodiscard]] const ui::ThreadBrowserOptions &
  browserOptions() const noexcept {
    return browser;
  }
  [[nodiscard]] std::optional<VisibleThread> visiblySelectedThread() const;
  [[nodiscard]] bool
  retainsTarget(const nodegraph::NodeRef &target) const noexcept;

private:
  struct SortAction {
    SortCriterion criterion;
    QAction *action;
  };

  bool applyItemPresentation(ThreadTreeItem *item, const ui::ThreadListRow &row,
                             bool draft = false,
                             std::optional<std::int64_t> draftStartedAt = {},
                             bool publishAccessibility = true);
  void retireOptimisticThread();
  void updateSortButton();
  void sortRootItems();
  [[nodiscard]] bool repositionRootItem(ThreadTreeItem *item);
  void updateAnimationTimer(bool repaint = false);
  void requestMoreNearListEnd();
  void updateContextRow(const std::string &presentationKey);
  void showContextMenu(const QPoint &position);
  void showGroupMenu(QMenu *menu, ThreadTreeItem *item);
  void editGroup(bool project, const std::string &id = {},
                 bool readOnly = false);
  void changeBrowser();
  void activateBrowserRow(ThreadTreeItem *item);
  [[nodiscard]] QString settingsKey() const;

  friend class ThreadTreeWidget;

  Actions actions;
  SortCriterion sortCriterion = SortCriterion::Recency;
  std::array<SortAction, 4> sortActions;
  QToolButton *sortButton = nullptr;
  ThreadTreeWidget *tree = nullptr;
  std::unordered_map<std::string, ThreadTreeItem *> rows;
  ThreadTreeItem *draftItem = nullptr;
  bool providerReady = false;
  bool canControl = false;
  std::string contextPresentationKey;
  nodegraph::NodeRef contextTarget;
  bool contextArchived = false;
  QMenu *contextMenu = nullptr;
  QTimer *optimisticAnimation = nullptr;
  bool selectionDispatchPending = false;
  ui::ThreadBrowserOptions browser;
  std::vector<ui::ThreadGroup> projects;
  std::vector<ui::ThreadGroup> sections;
  std::string serverIdentity;
  bool groupingAvailable = false;
  QToolButton *groupButton = nullptr;
};

} // namespace middle
} // namespace codexui::codex

#endif
