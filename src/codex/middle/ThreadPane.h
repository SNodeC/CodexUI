// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX_MIDDLE_THREADPANE_H
#define CODEXUI_CODEX_MIDDLE_THREADPANE_H

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

namespace codexui::codex {
class PresentationModel;

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
  void refresh(const PresentationModel &model,
               const std::string &selectedThreadId);
  void setSortCriterion(SortCriterion criterion);
  [[nodiscard]] SortCriterion currentSortCriterion() const noexcept;
  [[nodiscard]] std::string visiblySelectedThreadId() const;

private:
  struct ThreadRowSnapshot {
    std::string id;
    std::string title;
    std::string cwd;
    std::string status;
    std::string parentId;
    std::size_t pending = 0;
    std::size_t depth = 0;
    bool hasChildren = false;
    bool expanded = false;

    bool operator==(const ThreadRowSnapshot &) const = default;
  };
  struct ThreadPaneSnapshot {
    std::string selectedThreadId;
    SortCriterion sortCriterion = SortCriterion::Recency;
    std::vector<ThreadRowSnapshot> rows;

    bool operator==(const ThreadPaneSnapshot &) const = default;
  };

  void updateSortButton();
  void sortRootThreads(std::vector<std::string> &ids,
                       const PresentationModel &model) const;
  void appendVisibleThread(
      ThreadPaneSnapshot &snapshot, const PresentationModel &model,
      const std::unordered_map<std::string, std::size_t> &pendingByThread,
      const std::string &threadId, const std::string &parentId,
      std::size_t depth, std::unordered_set<std::string> &visited) const;
  void toggleExpanded(const std::string &threadId);
  void navigateHierarchy(int key);
  void setContextHighlight(const std::string &threadId, bool highlighted);
  void showContextMenu(const QPoint &position);

  const PresentationModel *currentModel = nullptr;
  Actions actions;
  SortCriterion sortCriterion = SortCriterion::Recency;
  QToolButton *sortButton = nullptr;
  QListWidget *list = nullptr;
  std::unordered_map<std::string, QListWidgetItem *> rows;
  std::unordered_set<std::string> collapsedThreads;
  std::string projectedSelectedThreadId;
  std::string contextThreadId;
  QMenu *contextMenu = nullptr;
  std::optional<ThreadPaneSnapshot> visibleSnapshot;
};

} // namespace middle
} // namespace codexui::codex

#endif
