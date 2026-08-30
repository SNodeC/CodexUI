// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_THREADPANE_H
#define CODEXUI_CODEX_MIDDLE_THREADPANE_H

#include "codex/ui/UiViewState.h"

#include <QFrame>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QListWidget;
class QListWidgetItem;
class QMenu;
class QToolButton;
class QTimer;

namespace codexui::codex {
namespace middle {

class ThreadPane final : public QFrame {
public:
  enum class SortCriterion { Alphanumeric, Created, LastChanged, Recency };

  struct Actions {
    std::function<void()> newThread;
    std::function<void()> refresh;
    std::function<void()> hide;
    std::function<void(const std::string &)> select;
    std::function<void(const std::string &)> reload;
    std::function<void(const std::string &)> rename;
    std::function<void(const std::string &)> fork;
    std::function<void(const std::string &)> toggleArchive;
    std::function<void(const std::string &)> remove;
  };

  explicit ThreadPane(QWidget *parent = nullptr);

  void setActions(Actions actions);
  void refresh(const ui::ThreadListSnapshot &snapshot);
  void beginOptimisticThread(std::string id, std::string title,
                             std::string cwd);
  void promoteOptimisticThread(const std::string &draftId,
                               const std::string &authoritativeId);
  void confirmOptimisticThread(const std::string &threadId);
  void failOptimisticThread(const std::string &threadId);
  [[nodiscard]] bool isOptimisticThread(const std::string &threadId) const;
  void promotePromptedThread(const std::string &threadId);
  void setSortCriterion(SortCriterion criterion);
  [[nodiscard]] SortCriterion currentSortCriterion() const noexcept;
  [[nodiscard]] std::string visiblySelectedThreadId() const;

private:
  struct RenderedThreadRow {
    std::string id;
    std::string title;
    std::string cwd;
    std::string status;
    std::string parentId;
    std::size_t pending = 0;
    std::size_t depth = 0;
    bool hasChildren = false;
    bool expanded = false;
    bool optimistic = false;
    bool optimisticFailed = false;

    bool operator==(const RenderedThreadRow &) const = default;
  };
  struct RenderedThreadList {
    std::string selectedThreadId;
    SortCriterion sortCriterion = SortCriterion::Recency;
    std::vector<RenderedThreadRow> rows;

    bool operator==(const RenderedThreadList &) const = default;
  };
  struct OptimisticThread {
    std::string id;
    std::string title;
    std::string cwd;
    bool failed = false;
  };
  struct PromptPromotion {
    std::string rootThreadId;
    std::optional<std::int64_t> updatedAt;
    std::optional<std::int64_t> recencyAt;
  };

  void updateSortButton();
  void sortRootThreads(std::vector<ui::ThreadListRow> &rows) const;
  void appendVisibleThread(RenderedThreadList &snapshot,
                           const ui::ThreadListRow &thread,
                           const std::string &parentId, std::size_t depth,
                           std::unordered_set<std::string> &visited) const;
  void toggleExpanded(const std::string &threadId);
  void navigateHierarchy(int key);
  void setContextHighlight(const std::string &threadId, bool highlighted);
  void showContextMenu(const QPoint &position);

  std::optional<ui::ThreadListSnapshot> currentSnapshot;
  Actions actions;
  SortCriterion sortCriterion = SortCriterion::Recency;
  QToolButton *sortButton = nullptr;
  QListWidget *list = nullptr;
  std::unordered_map<std::string, QListWidgetItem *> rows;
  std::unordered_set<std::string> expandedThreads;
  std::string projectedSelectedThreadId;
  std::string contextThreadId;
  QMenu *contextMenu = nullptr;
  QTimer *optimisticAnimation = nullptr;
  std::vector<OptimisticThread> optimisticThreads;
  std::optional<PromptPromotion> promptPromotion;
  std::optional<RenderedThreadList> visibleSnapshot;
};

} // namespace middle
} // namespace codexui::codex

#endif
