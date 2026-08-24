// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_GREENFIELD_CODEX_MIDDLE_THREADPANE_H
#define CODEXUI_GREENFIELD_CODEX_MIDDLE_THREADPANE_H

#include <QByteArray>
#include <QFrame>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class QListWidget;
class QListWidgetItem;

namespace codexui::codex {
class PresentationModel;

namespace middle {

class ThreadPane final : public QFrame {
public:
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
  [[nodiscard]] std::string visiblySelectedThreadId() const;

private:
  void showContextMenu(const QPoint &position);

  const PresentationModel *currentModel = nullptr;
  Actions actions;
  QListWidget *list = nullptr;
  std::unordered_map<std::string, QListWidgetItem *> rows;
  std::vector<std::string> retainedVisibleThreads;
  QByteArray visibleSnapshot;
};

} // namespace middle
} // namespace codexui::codex

#endif
