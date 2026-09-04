// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ThreadPane.h"
#include "codex/ui/NodeGraphUiAdapter.h"

#include <QApplication>
#include <QListWidget>

#include <cstdlib>
#include <iostream>

namespace codexui::codex {
namespace {

bool require(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << message << '\n';
  return false;
}

bool selectedChildRetainsRootAndExactTarget() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef root;
  nodegraph::NodeRef child;
  {
    auto write = graph.write();
    nodegraph::NodeRef runtime =
        write.upsert({nodegraph::NodeKind::Runtime, "runtime"});
    nodegraph::NodeState rootState;
    rootState.fields.emplace("name", "Root");
    root = write.upsert({nodegraph::NodeKind::Thread, "root"},
                        std::move(rootState));
    nodegraph::NodeState childState;
    childState.fields.emplace("name", "Child");
    child = write.upsert({nodegraph::NodeKind::Thread, "child"},
                         std::move(childState));
    write.relate(runtime, nodegraph::RelationKind::RootThread, root);
    write.relate(root, nodegraph::RelationKind::AgentChildThread, child);
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  const auto snapshot = adapter.threads(child);
  if (!require(snapshot.has_value(), "thread adapter read failed"))
    return false;

  middle::ThreadPane pane;
  pane.resize(300, 500);
  nodegraph::NodeRef selected;
  middle::ThreadPane::NodeActions actions;
  actions.select = [&](const nodegraph::NodeRef &target) { selected = target; };
  pane.setNodeActions(std::move(actions));
  pane.refresh(*snapshot);
  pane.show();
  QApplication::processEvents();

  auto *list = pane.findChild<QListWidget *>(QStringLiteral("threadList"));
  if (!require(list != nullptr, "thread list widget missing") ||
      !require(list->count() == 2,
               "selected child did not retain its visible root"))
    return false;
  list->setCurrentRow(0);
  QApplication::processEvents();
  selected.reset();
  list->setCurrentRow(1);
  QApplication::processEvents();
  return require(selected == child,
                 "thread UI reconstructed or discarded the exact NodeRef") &&
         require(list->item(0)->data(Qt::UserRole).toString() ==
                     QStringLiteral("root"),
                 "selecting a child removed its root row");
}

} // namespace
} // namespace codexui::codex

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  if (!codexui::codex::selectedChildRetainsRootAndExactTarget())
    return EXIT_FAILURE;
  std::cout << "NodeGraph ThreadPane UI tests passed\n";
  return EXIT_SUCCESS;
}
