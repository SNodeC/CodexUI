// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_THREADPANE_H
#define CODEXUI_CODEX_MIDDLE_THREADPANE_H

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
  enum class SortCriterion { Alphanumeric, Created, Recency };

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
  [[nodiscard]] std::optional<VisibleThread> visiblySelectedThread() const;

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
  void updateAnimationTimer(bool repaint = false);
  void requestMoreNearListEnd();
  void updateContextRow(const std::string &presentationKey);
  void showContextMenu(const QPoint &position);

  friend class ThreadTreeWidget;

  Actions actions;
  SortCriterion sortCriterion = SortCriterion::Recency;
  std::array<SortAction, 3> sortActions;
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
};

} // namespace middle
} // namespace codexui::codex

#endif
