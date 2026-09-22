// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "AccessibilityEventProbe.h"
#include "TimingPolicy.h"
#include "codex/ForkNaming.h"
#include "codex/middle/ThreadPane.h"
#include "codex/ui/NodeGraphUiAdapter.h"
#include "codex/ui/UiStyle.h"

#include <QAbstractItemView>
#include <QAccessible>
#include <QAccessibleActionInterface>
#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QProxyStyle>
#include <QScrollBar>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <ranges>
#include <sstream>

namespace codexui::codex {
namespace {

class AnimationDurationStyle final : public QProxyStyle {
public:
  explicit AnimationDurationStyle(int duration) : duration_(duration) {}
  AnimationDurationStyle(QStyle *base, int duration)
      : QProxyStyle(base), duration_(duration) {}

  int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
                const QWidget *widget = nullptr,
                QStyleHintReturn *returnData = nullptr) const override {
    return hint == SH_Widget_Animation_Duration
               ? duration_
               : QProxyStyle::styleHint(hint, option, widget, returnData);
  }

private:
  int duration_;
};

void installAnimationDurationStyle(QWidget &widget, int duration) {
  QStyle *base = widget.style();
  if (base && base->parent() == &widget)
    base->setParent(nullptr);
  else
    base = nullptr;
  auto *style = new AnimationDurationStyle(base, duration);
  style->setParent(&widget);
  widget.setStyle(style);
}

bool require(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << message << '\n';
  return false;
}

QTreeWidget *threadTree(QWidget &owner) {
  return owner.findChild<QTreeWidget *>(QStringLiteral("threadList"));
}

void click(QWidget *widget, QPoint position) {
  QMouseEvent press(QEvent::MouseButtonPress, QPointF(position),
                    QPointF(position), widget->mapToGlobal(position),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(widget, &press);
  QMouseEvent release(QEvent::MouseButtonRelease, QPointF(position),
                      QPointF(position), widget->mapToGlobal(position),
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(widget, &release);
  QApplication::processEvents();
}

QTreeWidgetItem *threadItem(QTreeWidget *tree, std::string_view id,
                            QTreeWidgetItem *parent = nullptr) {
  const int count = parent ? parent->childCount()
                    : tree ? tree->topLevelItemCount()
                           : 0;
  for (int index = 0; index < count; ++index) {
    QTreeWidgetItem *item =
        parent ? parent->child(index) : tree->topLevelItem(index);
    if (item->data(0, Qt::UserRole).toString() ==
        QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size())))
      return item;
    if (QTreeWidgetItem *found = threadItem(tree, id, item))
      return found;
  }
  return nullptr;
}

QAccessibleInterface *threadAccessible(QTreeWidget *tree,
                                       QTreeWidgetItem *item) {
  if (!tree || !item)
    return nullptr;
  std::vector<int> path;
  for (QTreeWidgetItem *current = item; current; current = current->parent())
    path.push_back(current->parent() ? current->parent()->indexOfChild(current)
                                     : tree->indexOfTopLevelItem(current));
  QAccessibleInterface *accessible =
      QAccessible::queryAccessibleInterface(tree);
  for (auto index = path.rbegin(); accessible && index != path.rend(); ++index)
    accessible = accessible->child(*index);
  return accessible;
}

bool forkNamesPreserveTheirLineage() {
  const std::vector<std::string> titles{"Original",
                                        "Original (fork 1)",
                                        "Original (fork 2)",
                                        "Original (fork 1.1)",
                                        "Original (fork 1.3)",
                                        "Original (fork 1.1.1)"};
  return require(suggestForkName("Original", titles) == "Original (fork 3)",
                 "a repeated root fork did not use the next root number") &&
         require(suggestForkName("Original (fork 1)", titles) ==
                     "Original (fork 1.2)",
                 "a fork of a fork lost its parent lineage") &&
         require(suggestForkName("Original (fork 1.1)", titles) ==
                     "Original (fork 1.1.2)",
                 "a nested fork did not use its own next child number") &&
         require(suggestForkName("Separate", titles) == "Separate (fork 1)",
                 "an unrelated thread did not start at fork 1");
}

bool selectedChildRetainsRootAndCanonicalIdentity() {
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
  int selectionCount = 0;
  middle::ThreadPane::Actions actions;
  actions.select = [&](const nodegraph::NodeRef &target) {
    selected = target;
    ++selectionCount;
  };
  pane.setActions(std::move(actions));
  pane.refresh(*snapshot);
  pane.show();
  QApplication::processEvents();

  QTreeWidget *tree = threadTree(pane);
  QTreeWidgetItem *rootItem = threadItem(tree, "root");
  QTreeWidgetItem *childItem = threadItem(tree, "child");
  if (!require(tree && tree->topLevelItemCount() == 1 && rootItem && childItem,
               "selected child did not retain its tree hierarchy") ||
      !require(childItem->parent() == rootItem && rootItem->isExpanded(),
               "selected child is not nested below its expanded root"))
    return false;
  tree->setCurrentItem(rootItem);
  QApplication::processEvents();
  selected.reset();
  tree->setCurrentItem(childItem);
  QApplication::processEvents();
  if (!require(selected == child,
               "thread UI changed the exact selection identity"))
    return false;
  selected.reset();
  selectionCount = 0;
  tree->setCurrentItem(rootItem);
  tree->setCurrentItem(childItem);
  tree->setCurrentItem(rootItem);
  QApplication::processEvents();
  return require(selected == root && selectionCount == 1,
                 "queued selection dispatched a stale intermediate target") &&
         require(tree->topLevelItem(0) == rootItem,
                 "selecting a child removed its root row");
}

bool accessibilityFollowsTheNativeHierarchy() {
#if !QT_CONFIG(accessibility)
  return true;
#else
  ui::ThreadListRow root;
  root.id = "accessible-root";
  root.presentationKey = root.id;
  root.title = "Accessible root";
  root.cwd = "/workspace";
  root.status = nodegraph::NodeStatus::Running;
  ui::ThreadListRow child;
  child.id = "accessible-child";
  child.presentationKey = child.id;
  child.title = "Accessible child";
  root.children.push_back(child);
  ui::ThreadListSnapshot snapshot;
  snapshot.selectedThreadId = root.id;
  snapshot.roots.push_back(root);

  middle::ThreadPane pane;
  pane.resize(320, 420);
  pane.refresh(snapshot);
  pane.show();
  QApplication::processEvents();
  QTreeWidget *tree = threadTree(pane);
  QTreeWidgetItem *rootItem = threadItem(tree, root.id);
  QTreeWidgetItem *childItem = threadItem(tree, child.id);
  QAccessibleInterface *treeAccessible =
      QAccessible::queryAccessibleInterface(tree);
  QAccessibleInterface *rootAccessible = threadAccessible(tree, rootItem);
  QAccessibleInterface *childAccessible = threadAccessible(tree, childItem);
  QAccessibleActionInterface *rootActions =
      rootAccessible ? rootAccessible->actionInterface() : nullptr;
  QAccessibleActionInterface *childActions =
      childAccessible ? childAccessible->actionInterface() : nullptr;
  QAccessibleSelectionInterface *selection =
      treeAccessible ? treeAccessible->selectionInterface() : nullptr;
  if (!require(tree && rootItem && childItem && treeAccessible &&
                   rootAccessible && childAccessible && rootActions &&
                   childActions && selection,
               "thread hierarchy did not expose semantic accessibility"))
    return false;

  tests::AccessibilityEventProbe events;
  const QAccessible::Id treeId = QAccessible::uniqueId(treeAccessible);
  const QAccessible::Id rootId = QAccessible::uniqueId(rootAccessible);
  const QAccessible::Id childId = QAccessible::uniqueId(childAccessible);

  bool passed = true;
  passed &= require(treeAccessible->role() == QAccessible::Tree &&
                        treeAccessible->text(QAccessible::Name) ==
                            QStringLiteral("Threads") &&
                        rootAccessible->role() == QAccessible::TreeItem &&
                        childAccessible->role() == QAccessible::TreeItem &&
                        treeAccessible->indexOfChild(rootAccessible) == 0 &&
                        treeAccessible->indexOfChild(childAccessible) == -1 &&
                        rootAccessible->parent() == treeAccessible &&
                        rootAccessible->indexOfChild(childAccessible) == 0 &&
                        childAccessible->parent() == rootAccessible &&
                        selection->selectedItemCount() == 1 &&
                        selection->selectedItems().front() == rootAccessible,
                    "accessible parentage does not match the native tree");
  passed &=
      require(rootAccessible->text(QAccessible::Name) ==
                      QStringLiteral("Accessible root") &&
                  childAccessible->text(QAccessible::Name) ==
                      QStringLiteral("Accessible child") &&
                  rootAccessible->text(QAccessible::Description)
                      .contains(QStringLiteral("Status: running")) &&
                  rootAccessible->text(QAccessible::Description)
                      .contains(QStringLiteral("Level 1")) &&
                  childAccessible->text(QAccessible::Description)
                      .contains(QStringLiteral("Level 2")) &&
                  childAccessible->text(QAccessible::Description)
                      .contains(QStringLiteral("Parent: Accessible root")),
              "accessible names or descriptions diverged from presentation");
  passed &= require(rootAccessible->state().collapsed &&
                        childAccessible->state().invisible &&
                        !childAccessible->state().focusable &&
                        !childAccessible->state().selectable &&
                        childAccessible->rect().isEmpty(),
                    "collapsed descendants remained visibly accessible");
  childActions->doAction(QAccessibleActionInterface::setFocusAction());
  passed &=
      require(childActions->actionNames().isEmpty() &&
                  !selection->select(childAccessible) &&
                  tree->currentItem() == rootItem && !childItem->isSelected(),
              "collapsed descendant accepted an accessibility action");

  events.clear();
  rootActions->doAction(QAccessibleActionInterface::toggleAction());
  QApplication::processEvents();
  const QRect rootRect = rootAccessible->rect();
  const QRect childRect = childAccessible->rect();
  passed &= require(
      rootItem->isExpanded() && rootAccessible->state().expanded &&
          !childAccessible->state().invisible && !childRect.isEmpty() &&
          rootRect.width() == tree->viewport()->width() &&
          childRect.width() == tree->viewport()->width() &&
          rootAccessible->childAt(childRect.center().x(),
                                  childRect.center().y()) == childAccessible &&
          treeAccessible->childAt(childRect.center().x(),
                                  childRect.center().y()) == rootAccessible,
      "accessible expansion, bounds, or hit hierarchy is incorrect");
  const auto expandedEvents =
      events.events(rootId, QAccessible::StateChanged);
  passed &= require(
      expandedEvents.size() == 1 &&
          expandedEvents.front().changedStates.expanded &&
          expandedEvents.front().changedStates.collapsed &&
          expandedEvents.front().state.expanded,
      "thread expansion did not emit once on its semantic TreeItem");
  events.clear();
  rootActions->doAction(QAccessibleActionInterface::toggleAction());
  QApplication::processEvents();
  const auto collapsedEvents =
      events.events(rootId, QAccessible::StateChanged);
  passed &= require(
      collapsedEvents.size() == 1 &&
          collapsedEvents.front().changedStates.expanded &&
          collapsedEvents.front().changedStates.collapsed &&
          collapsedEvents.front().state.collapsed,
      "thread collapse did not emit once on its semantic TreeItem");
  events.clear();
  rootItem->setExpanded(false);
  QApplication::processEvents();
  passed &= require(events.events(rootId, QAccessible::StateChanged).isEmpty(),
                    "an identical collapsed state emitted accessibility work");
  rootActions->doAction(QAccessibleActionInterface::toggleAction());
  QApplication::processEvents();
  events.clear();
  rootItem->setExpanded(true);
  QApplication::processEvents();
  passed &= require(events.events(rootId, QAccessible::StateChanged).isEmpty(),
                    "an identical expanded state emitted accessibility work");

  tree->setFocus(Qt::OtherFocusReason);
  QApplication::processEvents();
  events.clear();
  passed &= require(!selection->select(childAccessible),
                    "the root selection interface accepted a nested item");
  childActions->doAction(QAccessibleActionInterface::pressAction());
  QApplication::processEvents();
  const auto selectedChild =
      events.events(childId, QAccessible::SelectionAdd);
  const auto deselectedRoot =
      events.events(rootId, QAccessible::SelectionRemove);
  int addOrder = -1;
  int removeOrder = -1;
  for (qsizetype event = 0; event < events.all().size(); ++event) {
    const auto &record = events.all().at(event);
    if (record.target == childId && record.type == QAccessible::SelectionAdd)
      addOrder = static_cast<int>(event);
    if (record.target == rootId &&
        record.type == QAccessible::SelectionRemove)
      removeOrder = static_cast<int>(event);
  }
  passed &= require(tree->currentItem() == childItem &&
                        childAccessible->state().selected &&
                        childAccessible->state().focused &&
                        selection->selectedItemCount() == 0 &&
                        selection->selectedItems().isEmpty() &&
                        selectedChild.size() == 1 &&
                        deselectedRoot.size() == 1 &&
                        addOrder >= 0 && removeOrder > addOrder &&
                        events.events(childId, QAccessible::Focus).size() == 1 &&
                        std::ranges::none_of(events.all(), [](const auto &event) {
                          return event.target == 0 &&
                                 (event.type == QAccessible::SelectionAdd ||
                                  event.type == QAccessible::SelectionRemove ||
                                  event.type == QAccessible::Focus);
                        }),
                    "accessible focus and selection did not emit once on the "
                    "semantic items");

  events.clear();
  childActions->doAction(QAccessibleActionInterface::pressAction());
  QApplication::processEvents();
  passed &= require(events.events(childId).isEmpty(),
      "reselecting the current Thread item emitted a semantic event");

  events.clear();
  childItem->setSelected(false);
  passed &= require(tree->currentItem() == childItem &&
                        !childAccessible->state().selected &&
                        childAccessible->state().focused,
                    "native unselect discarded or hid keyboard focus");
  QApplication::processEvents();
  const auto unselectedChild =
      events.events(childId, QAccessible::SelectionRemove);
  passed &= require(unselectedChild.size() == 1 &&
                        !unselectedChild.front().state.selected,
                    "accessible unselect did not notify its semantic item");
  const QRect focusedRow = tree->visualItemRect(childItem);
  const auto closestFocusColor = [&focusedRow](const QImage &image) {
    const QColor expected(QString::fromLatin1(UiStyle::blueBorder));
    int result = 3 * 255;
    for (int x = 0; x <= 2; ++x)
      for (int y = focusedRow.top() + 12; y <= focusedRow.bottom() - 12; ++y) {
        const qreal dpr = image.devicePixelRatio();
        const QColor actual =
            image.pixelColor(qRound(x * dpr), qRound(y * dpr));
        result =
            std::min(result, std::abs(actual.red() - expected.red()) +
                                 std::abs(actual.green() - expected.green()) +
                                 std::abs(actual.blue() - expected.blue()));
      }
    return result;
  };
  const QImage focused = tree->viewport()->grab().toImage();
  tree->clearFocus();
  QApplication::processEvents();
  const QImage unfocused = tree->viewport()->grab().toImage();
  const int focusedDistance = closestFocusColor(focused);
  const int unfocusedDistance = closestFocusColor(unfocused);
  passed &=
      require(focusedDistance <= 20 && focusedDistance + 20 < unfocusedDistance,
              "unselected keyboard focus lacks its tokenized outline");
  events.clear();
  childActions->doAction(QAccessibleActionInterface::setFocusAction());
  QApplication::processEvents();
  passed &= require(
      tree->hasFocus() && childItem->isSelected() &&
          events.events(treeId, QAccessible::Focus).size() == 1 &&
          events.events(childId, QAccessible::Focus).isEmpty() &&
          treeAccessible->focusChild() == childAccessible &&
          events.events(childId, QAccessible::SelectionAdd).size() == 1,
      "native tree focus did not resolve to its unchanged current item");
  events.clear();
  childActions->doAction(QAccessibleActionInterface::setFocusAction());
  QApplication::processEvents();
  passed &= require(events.events(treeId).isEmpty() &&
                        events.events(childId).isEmpty(),
                    "refocusing the current Thread item emitted semantic work");

  events.clear();
  snapshot.selectedThreadId = child.id;
  snapshot.roots.front().title = "Renamed accessible root";
  snapshot.roots.front().cwd = "/renamed/workspace";
  pane.refresh(snapshot);
  QApplication::processEvents();
  passed &= require(
      events.events(rootId, QAccessible::NameChanged).size() == 1 &&
          events.events(rootId, QAccessible::DescriptionChanged).size() == 1 &&
          events.events(childId, QAccessible::DescriptionChanged).size() == 1 &&
          rootAccessible->text(QAccessible::Name) ==
              QStringLiteral("Renamed accessible root"),
      "Thread presentation mutation did not notify its retained semantic item");
  events.clear();
  pane.refresh(snapshot);
  QApplication::processEvents();
  passed &= require(
      events.events(rootId, QAccessible::NameChanged).isEmpty() &&
          events.events(rootId, QAccessible::DescriptionChanged).isEmpty() &&
          events.events(childId, QAccessible::DescriptionChanged).isEmpty(),
      "an identical Thread snapshot emitted accessibility changes");
  events.clear();
  snapshot.roots.front().pending = 1;
  pane.refresh(snapshot);
  QApplication::processEvents();
  passed &= require(
      events.events(rootId, QAccessible::NameChanged).size() == 1 &&
          events.events(rootId, QAccessible::DescriptionChanged).size() == 1 &&
          events.events(childId, QAccessible::DescriptionChanged).size() == 1,
      "a pending-derived parent name did not notify its retained subtree once");
  events.clear();
  pane.refresh(snapshot);
  QApplication::processEvents();
  passed &= require(
      events.events(rootId, QAccessible::NameChanged).isEmpty() &&
          events.events(rootId, QAccessible::DescriptionChanged).isEmpty() &&
          events.events(childId, QAccessible::DescriptionChanged).isEmpty(),
      "an identical pending Thread snapshot emitted accessibility changes");

  events.clear();
  snapshot.selectedThreadId = root.id;
  pane.refresh(snapshot);
  QApplication::processEvents();
  const auto rootSelection = events.events(rootId, QAccessible::SelectionAdd);
  const auto childDeselection =
      events.events(childId, QAccessible::SelectionRemove);
  passed &= require(
      rootSelection.size() == 1 && childDeselection.size() == 1 &&
          events.events(rootId, QAccessible::Focus).size() == 1 &&
          rootSelection.front().state.selected &&
          !childDeselection.front().state.selected,
      "snapshot selection did not publish the final semantic replacement");
  events.clear();
  pane.refresh(snapshot);
  QApplication::processEvents();
  passed &= require(events.events(rootId).isEmpty() &&
                        events.events(childId).isEmpty(),
                    "an identical snapshot selection emitted semantic work");
  events.clear();
  snapshot.selectedThreadId = child.id;
  pane.refresh(snapshot);
  QApplication::processEvents();
  passed &= require(
      events.events(childId, QAccessible::SelectionAdd).size() == 1 &&
          events.events(rootId, QAccessible::SelectionRemove).size() == 1 &&
          events.events(childId, QAccessible::Focus).size() == 1,
      "reverse snapshot selection did not publish the semantic replacement");

  tree->setEnabled(false);
  rootActions->doAction(QAccessibleActionInterface::toggleAction());
  passed &=
      require(rootItem->isExpanded() && !selection->unselect(childAccessible) &&
                  childItem->isSelected(),
              "disabled accessibility actions mutated the tree");
  tree->setEnabled(true);
  tree->clearSelection();
  pane.hide();
  QApplication::processEvents();
  passed &= require(
      rootAccessible->state().invisible && childAccessible->state().invisible &&
          !childAccessible->state().focused &&
          childActions->actionNames().isEmpty() &&
          treeAccessible->focusChild() == nullptr &&
          rootAccessible->rect().isEmpty() && childAccessible->rect().isEmpty(),
      "hidden thread items retained visible accessibility bounds");
  passed &= require(!selection->unselect(childAccessible) &&
                        !selection->select(childAccessible),
                    "a hidden nested thread entered root selection semantics");
  childActions->doAction(QAccessibleActionInterface::pressAction());
  passed &= require(!childItem->isSelected(),
                    "a hidden thread accepted an accessible press");

  events.clear();
  ui::ThreadListRow reparentedChild = snapshot.roots.front().children.front();
  reparentedChild.title = "Reparented accessible child";
  reparentedChild.cwd = "/reparented/workspace";
  reparentedChild.status = nodegraph::NodeStatus::Completed;
  snapshot.roots.front().children.clear();
  snapshot.roots.push_back(reparentedChild);
  pane.refresh(snapshot);
  QApplication::processEvents();
  passed &= require(
      QAccessible::accessibleInterface(childId) == childAccessible &&
          childAccessible->parent() == treeAccessible &&
          events.events(childId, QAccessible::ParentChanged).size() == 1 &&
          events.events(childId, QAccessible::NameChanged).size() == 1 &&
          events.events(childId, QAccessible::DescriptionChanged).size() == 1 &&
          events.events(rootId, QAccessible::StateChanged).size() == 1 &&
          events.events(rootId, QAccessible::StateChanged)
              .front()
              .changedStates.expandable &&
          events.events(rootId, QAccessible::StateChanged)
              .front()
              .changedStates.expanded &&
          !events.events(rootId, QAccessible::StateChanged)
               .front()
               .changedStates.collapsed &&
          !rootAccessible->state().expandable &&
          !rootAccessible->state().expanded &&
          !rootAccessible->state().collapsed,
      "Thread reparenting did not preserve and notify the semantic item");
  events.clear();
  snapshot.selectedThreadId = root.id;
  snapshot.roots.pop_back();
  pane.refresh(snapshot);
  QApplication::processEvents();
  passed &= require(
      events.events(childId, QAccessible::ObjectDestroyed).size() == 1 &&
          QAccessible::accessibleInterface(childId) == nullptr,
      "Thread retirement did not destroy the exact semantic interface once");
  events.clear();
  pane.refresh(snapshot);
  QApplication::processEvents();
  passed &= require(
      events.events(childId, QAccessible::ObjectDestroyed).isEmpty(),
      "an identical retired Thread snapshot repeated destruction");
  return passed;
#endif
}

bool presentationLifetimeFollowsExactThreadIncarnation() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef runtime;
  nodegraph::NodeRef firstRoot;
  nodegraph::NodeRef firstChild;
  {
    auto write = graph.write();
    runtime = write.upsert({nodegraph::NodeKind::Runtime, "runtime"});
    nodegraph::NodeState connectionState;
    connectionState.status = nodegraph::NodeStatus::Connected;
    connectionState.fields = {{"providerState", "ready"},
                              {"role", "controller"}};
    static_cast<void>(
        write.upsert({nodegraph::NodeKind::Connection, "connection"},
                     std::move(connectionState)));
    nodegraph::NodeState rootState;
    rootState.fields.emplace("name", "Stable root");
    firstRoot = write.upsert({nodegraph::NodeKind::Thread, "stable-root"},
                             std::move(rootState));
    nodegraph::NodeState childState;
    childState.fields.emplace("name", "Stable child");
    firstChild = write.upsert({nodegraph::NodeKind::Thread, "stable-child"},
                              std::move(childState));
    write.relate(runtime, nodegraph::RelationKind::RootThread, firstRoot);
    write.relate(firstRoot, nodegraph::RelationKind::AgentChildThread,
                 firstChild);
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  auto first = adapter.threads(firstRoot);
  if (!require(
          first && first->roots.size() == 1 &&
              first->roots.front().children.size() == 1 &&
              first->roots.front().target == firstRoot &&
              first->roots.front().children.front().target == firstChild &&
              !first->roots.front().presentationKey.empty() &&
              !first->roots.front().children.front().presentationKey.empty(),
          "the first exact thread incarnation was not projected"))
    return false;
  const std::string firstRootKey = first->roots.front().presentationKey;
  const std::string firstChildKey =
      first->roots.front().children.front().presentationKey;

  middle::ThreadPane pane;
  pane.resize(320, 500);
  pane.setSortCriterion(middle::ThreadPane::SortCriterion::Alphanumeric);
  nodegraph::NodeRef reloaded;
  nodegraph::NodeRef selected;
  middle::ThreadPane::Actions actions;
  actions.select = [&](const nodegraph::NodeRef &target) { selected = target; };
  actions.reload = [&](const nodegraph::NodeRef &target) { reloaded = target; };
  pane.setActions(std::move(actions));
  pane.refresh(*first);
  pane.show();
  QApplication::processEvents();

  QTreeWidget *tree = threadTree(pane);
  QTreeWidgetItem *firstRootItem = threadItem(tree, "stable-root");
  QTreeWidgetItem *firstChildItem = threadItem(tree, "stable-child");
  if (!require(tree && tree->topLevelItemCount() == 1 && firstRootItem &&
                   firstRootItem->childCount() == 1 && firstChildItem &&
                   firstChildItem->parent() == firstRootItem &&
                   !firstRootItem->isExpanded(),
               "the first incarnation did not render as one collapsed tree"))
    return false;
  tree->setCurrentItem(firstRootItem);
  QKeyEvent expand(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
  QApplication::sendEvent(tree, &expand);
  QApplication::processEvents();
  if (!require(firstRootItem->isExpanded(),
               "native tree navigation did not expand the hierarchy"))
    return false;
  const QPersistentModelIndex firstRootIndex(
      tree->indexFromItem(firstRootItem));
  QAccessibleInterface *treeAccessible =
      QAccessible::queryAccessibleInterface(tree);
  QAccessibleInterface *firstAccessible =
      treeAccessible ? treeAccessible->child(0) : nullptr;
  const QAccessible::Id firstAccessibleId =
      firstAccessible ? QAccessible::uniqueId(firstAccessible) : 0;

  const auto openContextMenu = [&]() -> QMenu * {
    const QPoint position =
        tree->visualItemRect(tree->topLevelItem(0)).center();
    QMetaObject::invokeMethod(tree, "customContextMenuRequested",
                              Qt::DirectConnection, Q_ARG(QPoint, position));
    QApplication::processEvents();
    for (QMenu *menu : pane.findChildren<QMenu *>()) {
      if (!menu->isVisible())
        continue;
      for (QAction *action : menu->actions())
        if (action && action->text() == QStringLiteral("Reload"))
          return menu;
    }
    return nullptr;
  };
  QPointer<QMenu> firstMenu = openContextMenu();
  QPointer<QAction> firstReload;
  if (firstMenu)
    for (QAction *action : firstMenu->actions())
      if (action && action->text() == QStringLiteral("Reload")) {
        firstReload = action;
        break;
      }
  if (!require(firstMenu && firstMenu->isVisible() && firstReload &&
                   firstReload->isEnabled(),
               "the first incarnation did not own its visible Reload menu"))
    return false;

  ui::ThreadListSnapshot unavailable = *first;
  unavailable.providerReady = false;
  unavailable.canControl = false;
  pane.refresh(unavailable);
  QApplication::processEvents();
  if (!require(!firstMenu || !firstMenu->isVisible(),
               "a capability transition left stale enabled menu actions "
               "visible"))
    return false;
  pane.refresh(*first);
  QApplication::processEvents();
  firstMenu = openContextMenu();
  firstReload = nullptr;
  if (firstMenu)
    for (QAction *action : firstMenu->actions())
      if (action && action->text() == QStringLiteral("Reload")) {
        firstReload = action;
        break;
      }
  if (!require(firstMenu && firstReload && firstReload->isEnabled(),
               "the restored capability snapshot did not produce a fresh "
               "exact menu"))
    return false;

  {
    const std::array<nodegraph::NodeRef, 2> oldThreads{firstRoot, firstChild};
    auto write = graph.write();
    write.removeMany(oldThreads);
    static_cast<void>(write.finish());
  }
  nodegraph::NodeRef secondRoot;
  nodegraph::NodeRef secondChild;
  {
    auto write = graph.write();
    nodegraph::NodeState rootState;
    rootState.fields.emplace("name", "Stable root");
    secondRoot = write.upsert({nodegraph::NodeKind::Thread, "stable-root"},
                              std::move(rootState));
    nodegraph::NodeState childState;
    childState.fields.emplace("name", "Stable child");
    secondChild = write.upsert({nodegraph::NodeKind::Thread, "stable-child"},
                               std::move(childState));
    write.relate(runtime, nodegraph::RelationKind::RootThread, secondRoot);
    write.relate(secondRoot, nodegraph::RelationKind::AgentChildThread,
                 secondChild);
    static_cast<void>(write.finish());
  }
  auto second = adapter.threads(secondRoot);
  if (!require(second && second->roots.size() == 1 &&
                   second->roots.front().children.size() == 1 &&
                   second->roots.front().target == secondRoot &&
                   second->roots.front().children.front().target ==
                       secondChild &&
                   second->roots.front().presentationKey != firstRootKey &&
                   second->roots.front().children.front().presentationKey !=
                       firstChildKey,
               "same canonical IDs reused the retired presentation lifetime"))
    return false;

  tree->setCurrentItem(firstChildItem);
  QApplication::processEvents();
  if (!require(selected == firstChild && selected != secondChild &&
                   pane.visiblySelectedThread() &&
                   pane.visiblySelectedThread()->target == firstChild,
               "a stale visible selection retargeted the replacement child"))
    return false;
  tree->setCurrentItem(firstRootItem);
  QApplication::processEvents();
  if (!require(pane.visiblySelectedThread() &&
                   pane.visiblySelectedThread()->target == firstRoot,
               "the visible row lost its exact pre-refresh target"))
    return false;
  firstReload->trigger();
  if (!require(reloaded == firstRoot && reloaded != secondRoot,
               "a delayed row action retargeted the replacement incarnation"))
    return false;
  QPointer<QMenu> staleMenu = openContextMenu();
  ui::ThreadListRow forgedStaleDelta = first->roots.front();
  forgedStaleDelta.target = secondRoot;
  if (!require(!pane.applyRowPresentation(forgedStaleDelta),
               "an incremental row update accepted a stale exact target"))
    return false;

  pane.refresh(*second);
  QApplication::processEvents();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QApplication::processEvents();
  QTreeWidgetItem *secondItem = threadItem(tree, "stable-root");
  QAccessibleInterface *secondAccessible =
      treeAccessible ? treeAccessible->child(0) : nullptr;
  bool result =
      require(tree->topLevelItemCount() == 1 && secondItem &&
                  secondItem->childCount() == 1 && !secondItem->isExpanded(),
              "the replacement incarnation inherited hierarchy expansion") &&
      require(!firstRootIndex.isValid() &&
                  (!firstAccessibleId ||
                   !QAccessible::accessibleInterface(firstAccessibleId)),
              "the replacement incarnation retained retired model or "
              "accessibility identity") &&
      require(
          staleMenu.isNull() || !staleMenu->isVisible(),
          "the replacement incarnation retained the retired context menu") &&
      require(pane.visiblySelectedThread() &&
                  pane.visiblySelectedThread()->target == secondRoot &&
                  pane.currentSortCriterion() ==
                      middle::ThreadPane::SortCriterion::Alphanumeric,
              "reincarnation lost canonical navigation or global sorting") &&
      require(secondAccessible &&
                  secondAccessible->role() == QAccessible::TreeItem &&
                  secondAccessible->parent() == treeAccessible &&
                  secondAccessible->state().expandable &&
                  secondAccessible->state().collapsed,
              "the fresh hierarchy did not expose nested collapsed "
              "accessibility");

  const auto selectedSecond = adapter.threads(secondChild);
  if (!require(selectedSecond.has_value(),
               "the selected replacement child was not projected"))
    return false;
  pane.refresh(*selectedSecond);
  QApplication::processEvents();
  result &=
      require(secondItem->isExpanded() && threadItem(tree, "stable-child") &&
                  pane.visiblySelectedThread() &&
                  pane.visiblySelectedThread()->target == secondChild,
              "selecting a child did not expand its current ancestors");

  {
    const std::array<nodegraph::NodeRef, 2> oldThreads{secondRoot, secondChild};
    auto write = graph.write();
    write.removeMany(oldThreads);
    static_cast<void>(write.finish());
  }
  nodegraph::NodeRef thirdRoot;
  nodegraph::NodeRef thirdChild;
  {
    auto write = graph.write();
    nodegraph::NodeState rootState;
    rootState.fields.emplace("name", "Stable root");
    thirdRoot = write.upsert({nodegraph::NodeKind::Thread, "stable-root"},
                             std::move(rootState));
    nodegraph::NodeState childState;
    childState.fields.emplace("name", "Stable child");
    thirdChild = write.upsert({nodegraph::NodeKind::Thread, "stable-child"},
                              std::move(childState));
    write.relate(runtime, nodegraph::RelationKind::RootThread, thirdRoot);
    write.relate(thirdRoot, nodegraph::RelationKind::AgentChildThread,
                 thirdChild);
    static_cast<void>(write.finish());
  }
  const auto third = adapter.threads(thirdChild);
  if (!require(third && third->roots.size() == 1 &&
                   third->roots.front().children.size() == 1,
               "the second replacement tree was not projected"))
    return false;
  pane.refresh(*third);
  QApplication::processEvents();
  result &= require(
      third &&
          third->roots.front().presentationKey !=
              second->roots.front().presentationKey &&
          third->roots.front().children.front().presentationKey !=
              second->roots.front().children.front().presentationKey &&
          tree->topLevelItemCount() == 1 &&
          tree->topLevelItem(0)->isExpanded() &&
          tree->topLevelItem(0)->childCount() == 1 &&
          pane.visiblySelectedThread() &&
          pane.visiblySelectedThread()->target == thirdChild,
      "a reincarnated selected child did not expand only its new ancestor");
  return result;
}

bool sortingAndPromptAnimationAreFixed() {
  AnimationDurationStyle normalMotion(100);
  middle::ThreadPane pane;
  pane.resize(300, 500);
  ui::ThreadListSnapshot snapshot;
  ui::ThreadListRow older;
  older.id = "older";
  older.presentationKey = older.id;
  older.title = "20 tasks";
  older.createdAt = 500;
  older.updatedAt = 900;
  older.recencyAt = 10;
  ui::ThreadListRow recent;
  recent.id = "recent";
  recent.presentationKey = recent.id;
  recent.title = "Alpha";
  recent.createdAt = 1;
  recent.updatedAt = 2;
  recent.recencyAt = 30;
  ui::ThreadListRow missing;
  missing.id = "missing";
  missing.presentationKey = missing.id;
  missing.title = "2 tasks";
  snapshot.roots = {older, recent, missing};

  pane.refresh(snapshot);
  pane.show();
  QApplication::processEvents();
  QTreeWidget *tree = threadTree(pane);
  if (tree)
    tree->setStyle(&normalMotion);
  auto *sortButton =
      pane.findChild<QToolButton *>(QStringLiteral("threadSortButton"));
  if (!require(tree && tree->topLevelItemCount() == 3, "thread tree missing") ||
      !require(sortButton && sortButton->menu(),
               "thread sort control missing") ||
      !require(tree->topLevelItem(0)->data(0, Qt::UserRole).toString() ==
                   QStringLiteral("recent"),
               "thread rows are not ordered by recencyAt newest first"))
    return false;
  if (!require(tree->topLevelItem(0)->toolTip(0).contains(
                   QStringLiteral("Recent turn:")) &&
                   tree->topLevelItem(0)->toolTip(0).contains(
                       QStringLiteral("Created:")),
               "thread hover omits provider recent-turn or creation time"))
    return false;

  QStringList sortLabels;
  for (QAction *action : sortButton->menu()->actions())
    sortLabels.push_back(action->text());
  if (!require(sortLabels == QStringList{QStringLiteral("Alphanumeric"),
                                         QStringLiteral("Created"),
                                         QStringLiteral("Recent")},
               "thread sort menu does not expose exactly the three contracts"))
    return false;

  pane.setSortCriterion(middle::ThreadPane::SortCriterion::Alphanumeric);
  QApplication::processEvents();
  if (!require(tree->topLevelItem(0)->data(0, Qt::UserRole).toString() ==
                   QStringLiteral("missing"),
               "alphanumeric order is not numeric-aware") ||
      !require(tree->topLevelItem(1)->data(0, Qt::UserRole).toString() ==
                   QStringLiteral("older"),
               "alphanumeric order did not place 20 after 2") ||
      !require(tree->topLevelItem(2)->data(0, Qt::UserRole).toString() ==
                   QStringLiteral("recent"),
               "alphanumeric order did not place letters after numeric titles"))
    return false;

  pane.setSortCriterion(middle::ThreadPane::SortCriterion::Created);
  QApplication::processEvents();
  if (!require(tree->topLevelItem(0)->data(0, Qt::UserRole).toString() ==
                   QStringLiteral("older"),
               "Created order is not newest first") ||
      !require(tree->topLevelItem(1)->data(0, Qt::UserRole).toString() ==
                   QStringLiteral("recent"),
               "Created order did not keep older dated rows before missing") ||
      !require(tree->topLevelItem(2)->data(0, Qt::UserRole).toString() ==
                   QStringLiteral("missing"),
               "Created order did not put missing timestamps last"))
    return false;

  pane.setSortCriterion(middle::ThreadPane::SortCriterion::Recency);
  QApplication::processEvents();

  snapshot.roots[0].recencyAt = 31;
  snapshot.roots[0].awaitingPromptAcknowledgement = true;
  snapshot.roots[0].pendingPromptAdmittedAtMs =
      QDateTime::currentMSecsSinceEpoch() - 1500;
  pane.refresh(snapshot);
  QApplication::processEvents();
  auto *animation =
      pane.findChild<QTimer *>(QStringLiteral("optimisticThreadAnimation"));
  QTreeWidgetItem *promoted = tree->topLevelItem(0);
  QAccessibleInterface *promotedAccessible = threadAccessible(tree, promoted);
  if (!require(promoted->data(0, Qt::UserRole).toString() ==
                   QStringLiteral("older"),
               "new turn admission did not promote its thread") ||
      !require(promotedAccessible &&
                   promotedAccessible->text(QAccessible::Name) ==
                       QStringLiteral("20 tasks"),
               "promotion substituted a canonical ID for the chosen name") ||
      !require(animation && animation->isActive(),
               "thread-card animation did not follow the pending prompt"))
    return false;

  snapshot.roots[1].status = nodegraph::NodeStatus::Running;
  pane.refresh(snapshot);
  QApplication::processEvents();
  if (!require(animation->isActive(),
               "unrelated thread traffic stopped pending prompt animation"))
    return false;

  pane.hide();
  if (!require(!animation->isActive(),
               "hiding the thread pane retained animation work"))
    return false;
  pane.show();
  QApplication::processEvents();
  if (!require(animation->isActive(),
               "showing the thread pane did not resume visible animation"))
    return false;

  snapshot.roots[0].awaitingPromptAcknowledgement = false;
  snapshot.roots[0].pendingPromptAdmittedAtMs.reset();
  pane.refresh(snapshot);
  QApplication::processEvents();
  return require(!animation->isActive(),
                 "prompt acknowledgement did not stop thread-card animation") &&
         require(tree->topLevelItem(0) == promoted && promotedAccessible &&
                     promotedAccessible->isValid() &&
                     promotedAccessible->text(QAccessible::Name) ==
                         QStringLiteral("20 tasks"),
                 "prompt acknowledgement changed the chosen name");
}

bool reducedMotionStopsThreadFeedbackAtTheStyleBoundary() {
  ui::ThreadListRow row;
  row.id = "reduced-motion-thread";
  row.presentationKey = row.id;
  row.title = "Reduced motion";
  row.cwd = "/workspace";
  row.awaitingPromptAcknowledgement = true;
  row.pendingPromptAdmittedAtMs = QDateTime::currentMSecsSinceEpoch() - 1500;
  ui::ThreadListSnapshot snapshot;
  snapshot.roots.push_back(row);

  AnimationDurationStyle normalMotion(100);
  AnimationDurationStyle reducedMotion(0);
  middle::ThreadPane pane;
  pane.resize(320, 240);
  if (QTreeWidget *tree = threadTree(pane))
    tree->setStyle(&normalMotion);
  pane.refresh(snapshot);
  pane.show();
  QApplication::processEvents();
  QTreeWidget *tree = threadTree(pane);
  auto *animation =
      pane.findChild<QTimer *>(QStringLiteral("optimisticThreadAnimation"));
  bool result = require(animation && animation->isActive(),
                        "normal motion did not start visible thread feedback");

  if (tree)
    tree->setStyle(&reducedMotion);
  QElapsedTimer stopped;
  stopped.start();
  while (animation && animation->isActive() && stopped.elapsed() < 100) {
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  }
  const QImage staticFrame =
      tree ? tree->viewport()->grab().toImage() : QImage{};
  QElapsedTimer stable;
  stable.start();
  while (stable.elapsed() < 110) {
    QApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(1);
  }
  result &= require(
      tree && animation && !animation->isActive() &&
          tree->viewport()->grab().toImage() == staticFrame,
      "reduced motion did not stop thread feedback at the style boundary");

  if (tree)
    tree->setStyle(&normalMotion);
  QApplication::processEvents();
  result &= require(animation && animation->isActive(),
                    "restoring motion did not resume visible thread feedback");
  return result;
}

int colorDistance(const QColor &left, const QColor &right) {
  return std::abs(left.red() - right.red()) +
         std::abs(left.green() - right.green()) +
         std::abs(left.blue() - right.blue());
}

QColor logicalPixel(const QImage &image, QPoint point) {
  const qreal dpr = image.devicePixelRatio();
  point = QPoint(qRound(point.x() * dpr), qRound(point.y() * dpr));
  return image.rect().contains(point) ? image.pixelColor(point) : QColor{};
}

bool threadRowsConsumeCanonicalStatusTone() {
  middle::ThreadPane pane;
  pane.resize(300, 300);
  pane.show();
  ui::ThreadListSnapshot snapshot;
  ui::ThreadListRow row;
  row.id = "status-thread";
  row.presentationKey = row.id;
  row.title = "Status thread";
  ui::ThreadListRow child;
  child.id = "status-child";
  child.presentationKey = child.id;
  ui::ThreadListRow grandchild;
  grandchild.id = "status-grandchild";
  grandchild.presentationKey = grandchild.id;
  child.children.push_back(grandchild);
  row.children.push_back(child);
  snapshot.roots.push_back(row);
  const std::array cases{
      std::pair{nodegraph::NodeStatus::Running, UiStyle::blue},
      std::pair{nodegraph::NodeStatus::Completed, UiStyle::green},
      std::pair{nodegraph::NodeStatus::Connected, UiStyle::green},
      std::pair{nodegraph::NodeStatus::Interrupted, UiStyle::orange},
      std::pair{nodegraph::NodeStatus::Failed, UiStyle::red},
      std::pair{nodegraph::NodeStatus::Disconnected, UiStyle::red}};
  bool passed = true;
  for (const auto &[status, color] : cases) {
    snapshot.roots.front().status = status;
    pane.refresh(snapshot);
    QApplication::processEvents();
    QTreeWidget *tree = threadTree(pane);
    QTreeWidgetItem *item = tree ? tree->topLevelItem(0) : nullptr;
    const QRect rect = item ? tree->visualItemRect(item) : QRect{};
    const QColor actual =
        rect.isValid()
            ? logicalPixel(tree->viewport()->grab().toImage(),
                           QPoint(rect.left() + 14, rect.center().y()))
            : QColor{};
    const QColor chevron =
        rect.isValid()
            ? logicalPixel(tree->viewport()->grab().toImage(),
                           QPoint(rect.left() - 1, rect.center().y()))
            : QColor{};
    passed &= require(
        item && colorDistance(actual, QColor(QString::fromLatin1(color))) <= 6,
        "thread delegate bypassed the canonical status tone");
    passed &=
        require(item && colorDistance(chevron, QColor(QString::fromLatin1(
                                                   UiStyle::secondary))) <= 30,
                "thread disclosure drifted from the status indicator");
  }
  QTreeWidget *tree = threadTree(pane);
  QTreeWidgetItem *item = tree ? tree->topLevelItem(0) : nullptr;
  const QRect rect = item ? tree->visualItemRect(item) : QRect{};
  const QPoint disclosureEdge(rect.left(), rect.center().y());
  const QColor edgePixel =
      rect.isValid()
          ? logicalPixel(tree->viewport()->grab().toImage(), disclosureEdge)
          : QColor{};
  passed &= require(
      item && !item->isExpanded() &&
          colorDistance(edgePixel,
                        QColor(QString::fromLatin1(UiStyle::panel))) > 50,
      "disclosure mouse fixture did not target a visible chevron pixel");
  if (tree && item) {
    click(tree->viewport(), QPoint(rect.left() - 12, rect.center().y()));
    passed &= require(!item->isExpanded(),
                      "mouse disclosure target extended before its legacy "
                      "leading edge");
    click(tree->viewport(), QPoint(rect.left() - 11, rect.center().y()));
    passed &= require(item->isExpanded(),
                      "mouse disclosure target omitted its legacy leading "
                      "edge");
    click(tree->viewport(), QPoint(rect.left() + 13, rect.center().y()));
    passed &= require(item->isExpanded(),
                      "mouse disclosure target extended after its legacy "
                      "trailing edge");
    click(tree->viewport(), QPoint(rect.left() + 12, rect.center().y()));
    passed &= require(!item->isExpanded(),
                      "mouse disclosure target omitted its legacy trailing "
                      "edge");
    click(tree->viewport(), QPoint(rect.left(), rect.top() + 7));
    passed &= require(!item->isExpanded(),
                      "mouse disclosure target extended above its legacy "
                      "leading edge");
    click(tree->viewport(), QPoint(rect.left(), rect.top() + 8));
    passed &= require(item->isExpanded(),
                      "mouse disclosure target omitted its legacy top edge");
    click(tree->viewport(), QPoint(rect.left(), rect.bottom() - 7));
    passed &= require(item->isExpanded(),
                      "mouse disclosure target extended below its legacy "
                      "trailing edge");
    click(tree->viewport(), QPoint(rect.left(), rect.bottom() - 8));
    passed &= require(!item->isExpanded(),
                      "mouse disclosure target omitted its legacy bottom edge");
    click(tree->viewport(), disclosureEdge);
    QTreeWidgetItem *nested = item->child(0);
    const QRect nestedRect = tree->visualItemRect(nested);
    click(tree->viewport(),
          QPoint(nestedRect.left() - 11, nestedRect.center().y()));
    passed &= require(nested && nested->isExpanded(),
                      "nested disclosure did not share the native target");
    click(tree->viewport(),
          QPoint(nestedRect.left() + 12, nestedRect.center().y()));
    passed &= require(!nested->isExpanded(),
                      "nested disclosure trailing edge did not collapse");
  }
  return passed;
}

bool rowDotUsesColor(QTreeWidget *tree, QTreeWidgetItem *item,
                     const QColor &expected) {
  if (!tree || !item)
    return false;
  const QRect rect = tree->visualItemRect(item);
  return rect.isValid() &&
         colorDistance(
             logicalPixel(tree->viewport()->grab().toImage(),
                          QPoint(rect.left() + 14, rect.center().y())),
             expected) <= 6;
}

bool optimisticCreationHandsOneRowToGraphAuthority() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef localTarget;
  nodegraph::NodeRef canonicalTarget;
  {
    auto write = graph.write();
    localTarget = write.upsert({nodegraph::NodeKind::Thread, "local-thread:1"});
    canonicalTarget =
        write.upsert({nodegraph::NodeKind::Thread, "created-thread"});
    static_cast<void>(write.finish());
  }
  AnimationDurationStyle normalMotion(100);
  middle::ThreadPane pane;
  pane.resize(320, 420);
  if (QTreeWidget *tree = threadTree(pane))
    tree->setStyle(&normalMotion);
  pane.beginOptimisticThread("draft:new-thread", "creation:test", "Chosen name",
                             "/workspace");
  ui::ThreadListSnapshot empty;
  empty.selectedThreadId = "draft:new-thread";
  pane.refresh(empty);
  pane.show();
  QApplication::processEvents();

  QTreeWidget *tree = threadTree(pane);
  auto *animation =
      pane.findChild<QTimer *>(QStringLiteral("optimisticThreadAnimation"));
  QTreeWidgetItem *draft =
      tree && tree->topLevelItemCount() == 1 ? tree->topLevelItem(0) : nullptr;
#if QT_CONFIG(accessibility)
  QAccessibleInterface *draftAccessible = threadAccessible(tree, draft);
  const QAccessible::Id draftAccessibleId =
      draftAccessible ? QAccessible::uniqueId(draftAccessible) : 0;
  tests::AccessibilityEventProbe events;
#endif
  const QColor orange(QString::fromLatin1(UiStyle::orange));
  if (!require(draft && animation && animation->isActive(),
               "dialog Continue did not immediately animate its draft row") ||
      !require(rowDotUsesColor(tree, draft, orange),
               "the initial optimistic row is not orange"))
    return false;

  ui::ThreadListSnapshot admitted;
  admitted.selectedThreadId = "local-thread:1";
  ui::ThreadListRow canonical;
  canonical.id = "local-thread:1";
  canonical.presentationKey = "creation:test";
  canonical.target = localTarget;
  canonical.title = "Chosen name";
  canonical.status = nodegraph::NodeStatus::Running;
  canonical.awaitingPromptAcknowledgement = true;
  canonical.pendingPromptAdmittedAtMs =
      QDateTime::currentMSecsSinceEpoch() - 1500;
  admitted.roots.push_back(canonical);
#if QT_CONFIG(accessibility)
  events.clear();
#endif
  pane.refresh(admitted);
  QApplication::processEvents();
  QTreeWidgetItem *promoted = tree->topLevelItem(0);
  const QColor blue(QString::fromLatin1(UiStyle::blue));
  if (!require(promoted == draft && animation->isActive(),
               "graph admission replaced or stopped the draft row") ||
      !require(promoted->data(0, Qt::UserRole).toString() ==
                   QStringLiteral("local-thread:1"),
               "graph admission did not update the row's action identity") ||
      !require(rowDotUsesColor(tree, promoted, blue),
               "graph admission did not take ownership of pending feedback"))
    return false;
#if QT_CONFIG(accessibility)
  if (!require(QAccessible::accessibleInterface(draftAccessibleId) ==
                       draftAccessible &&
                   events.events(draftAccessibleId, QAccessible::SelectionAdd)
                       .isEmpty() &&
                   events.events(draftAccessibleId,
                                 QAccessible::SelectionRemove)
                       .isEmpty() &&
                   events.events(draftAccessibleId, QAccessible::Focus)
                       .isEmpty(),
               "optimistic admission replaced or refocused its semantic item"))
    return false;
  events.clear();
#endif

  admitted.selectedThreadId = "created-thread";
  admitted.roots.front().id = "created-thread";
  admitted.roots.front().target = canonicalTarget;
  pane.refresh(admitted);
  QApplication::processEvents();
  if (!require(tree->topLevelItem(0) == draft &&
                   draft->data(0, Qt::UserRole).toString() ==
                       QStringLiteral("created-thread"),
               "canonical attachment replaced the graph-owned row"))
    return false;
#if QT_CONFIG(accessibility)
  if (!require(QAccessible::accessibleInterface(draftAccessibleId) ==
                       draftAccessible &&
                   events.events(draftAccessibleId, QAccessible::SelectionAdd)
                       .isEmpty() &&
                   events.events(draftAccessibleId,
                                 QAccessible::SelectionRemove)
                       .isEmpty() &&
                   events.events(draftAccessibleId, QAccessible::Focus)
                       .isEmpty(),
               "canonical promotion replaced or refocused its semantic item"))
    return false;
#endif

  admitted.roots.front().awaitingPromptAcknowledgement = false;
  admitted.roots.front().pendingPromptAdmittedAtMs.reset();
  pane.refresh(admitted);
  QApplication::processEvents();
  return require(tree->topLevelItem(0) == draft && !animation->isActive(),
                 "authoritative acknowledgement did not end the same row's "
                 "pending animation") &&
         require(!rowDotUsesColor(tree, draft, orange),
                 "acknowledged thread row did not return to its native color");
}

bool optimisticDiscardRetiresAuthorityBeforeSelectionReentry() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef existingTarget;
  {
    auto write = graph.write();
    existingTarget =
        write.upsert({nodegraph::NodeKind::Thread, "existing-thread"});
    static_cast<void>(write.finish());
  }

  middle::ThreadPane pane;
  ui::ThreadListSnapshot snapshot;
  snapshot.selectedThreadId = "existing-thread";
  ui::ThreadListRow existing;
  existing.id = "existing-thread";
  existing.presentationKey = existing.id;
  existing.target = existingTarget;
  snapshot.roots.push_back(existing);
  pane.refresh(snapshot);

  int selections = 0;
  middle::ThreadPane::Actions actions;
  actions.select = [&](const nodegraph::NodeRef &target) {
    if (target != existingTarget)
      return;
    ++selections;
    pane.discardOptimisticThread("draft:new-thread");
  };
  pane.setActions(std::move(actions));
  pane.beginOptimisticThread("draft:new-thread", "creation:first", "First",
                             "/workspace");
  pane.show();
  QApplication::processEvents();

  QTreeWidget *tree = threadTree(pane);
  QTreeWidgetItem *existingItem = threadItem(tree, "existing-thread");
  if (!require(tree && existingItem,
               "reentrant draft-retirement fixture lacks its real row"))
    return false;
  tree->setCurrentItem(existingItem);
  QApplication::processEvents();
  if (!require(selections == 1 && !threadItem(tree, "draft:new-thread") &&
                   tree->currentItem() == existingItem &&
                   existingItem->isSelected(),
               "draft retirement repeated or disturbed the user selection"))
    return false;

  pane.beginOptimisticThread("draft:new-thread", "creation:second", "Second",
                             "/workspace");
  QApplication::processEvents();
  return require(threadItem(tree, "draft:new-thread") == tree->currentItem(),
                 "draft retirement prevented a later creation from starting");
}

bool pagingFollowsTheVisibleListEnd() {
  middle::ThreadPane shortPane;
  shortPane.resize(300, 500);
  int shortPageRequests = 0;
  middle::ThreadPane::Actions shortActions;
  shortActions.loadMore = [&] { ++shortPageRequests; };
  shortPane.setActions(std::move(shortActions));
  ui::ThreadListSnapshot shortSnapshot;
  for (int index = 0; index < 2; ++index) {
    ui::ThreadListRow row;
    row.id = "short-" + std::to_string(index);
    row.presentationKey = row.id;
    shortSnapshot.roots.push_back(std::move(row));
  }
  shortPane.refresh(shortSnapshot);
  shortPane.show();
  QApplication::processEvents();
  if (!require(shortPageRequests == 1,
               "an initially short thread page did not request more rows"))
    return false;
  shortPageRequests = 0;
  ui::ThreadListRow appended;
  appended.id = "short-2";
  appended.presentationKey = appended.id;
  shortSnapshot.roots.push_back(std::move(appended));
  shortPane.refresh(shortSnapshot);
  QApplication::processEvents();
  if (!require(shortPageRequests == 1,
               "a still-short appended page did not continue pagination"))
    return false;

  middle::ThreadPane pane;
  pane.resize(300, 500);
  int loadMoreRequests = 0;
  middle::ThreadPane::Actions actions;
  actions.loadMore = [&] { ++loadMoreRequests; };
  pane.setActions(std::move(actions));

  ui::ThreadListSnapshot longSnapshot;
  for (int index = 0; index < 80; ++index) {
    ui::ThreadListRow row;
    row.id = "long-" + std::to_string(index);
    row.presentationKey = row.id;
    row.title = row.id;
    row.recencyAt = 1000 - index;
    longSnapshot.roots.push_back(std::move(row));
  }
  pane.refresh(longSnapshot);
  pane.show();
  QApplication::processEvents();
  QTreeWidget *tree = threadTree(pane);
  if (!require(tree && tree->verticalScrollBar()->maximum() > 0,
               "long thread page did not produce a scroll range"))
    return false;
  loadMoreRequests = 0;
  tree->verticalScrollBar()->setValue(tree->verticalScrollBar()->maximum());
  QApplication::processEvents();
  if (!require(loadMoreRequests == 1,
               "scrolling to the visible list end did not request exactly one "
               "thread page"))
    return false;
  loadMoreRequests = 0;
  ui::ThreadListRow newest;
  newest.id = "long-newest";
  newest.presentationKey = newest.id;
  newest.title = newest.id;
  newest.recencyAt = 2000;
  longSnapshot.roots.push_back(std::move(newest));
  pane.refresh(longSnapshot);
  QApplication::processEvents();
  return require(loadMoreRequests == 1,
                 "anchor restoration requested the next thread page more than "
                 "once");
}

bool structuralInsertionPreservesTheViewportAnchor() {
  middle::ThreadPane pane;
  pane.resize(300, 500);
  pane.setSortCriterion(middle::ThreadPane::SortCriterion::Alphanumeric);
  ui::ThreadListSnapshot snapshot;
  for (int index = 0; index < 100; ++index) {
    ui::ThreadListRow row;
    row.id = "b-" + std::to_string(index);
    row.presentationKey = row.id;
    row.title = "B " + std::to_string(index);
    snapshot.roots.push_back(std::move(row));
  }
  pane.refresh(snapshot);
  pane.show();
  QApplication::processEvents();
  QTreeWidget *tree = threadTree(pane);
  QTreeWidgetItem *anchor = threadItem(tree, "b-50");
  if (!require(tree && anchor && tree->verticalScrollBar()->maximum() > 0,
               "anchor fixture did not produce a scrollable tree"))
    return false;
  tree->scrollToItem(anchor, QAbstractItemView::PositionAtTop);
  QApplication::processEvents();
  const QPoint probe(tree->viewport()->width() / 2, 0);
  const QModelIndex before = tree->indexAt(probe);
  const QString beforeId = before.data(Qt::UserRole).toString();
  const int beforeY = tree->visualRect(before).top();
  const int beforeScroll = tree->verticalScrollBar()->value();

  for (int index = 0; index < 10; ++index) {
    ui::ThreadListRow row;
    row.id = "a-" + std::to_string(index);
    row.presentationKey = row.id;
    row.title = "A " + std::to_string(index);
    snapshot.roots.push_back(std::move(row));
  }
  pane.refresh(snapshot);
  QApplication::processEvents();
  const QModelIndex after = tree->indexAt(probe);
  return require(beforeId == QStringLiteral("b-50") &&
                     after.data(Qt::UserRole).toString() == beforeId &&
                     tree->visualRect(after).top() == beforeY &&
                     tree->verticalScrollBar()->value() > beforeScroll,
                 "structural insertion moved the stable viewport anchor");
}

bool authoritativeSelectionWinsOverThePriorViewportAnchor() {
  middle::ThreadPane pane;
  pane.resize(300, 500);
  pane.setSortCriterion(middle::ThreadPane::SortCriterion::Alphanumeric);
  ui::ThreadListSnapshot snapshot;
  snapshot.selectedThreadId = "b-50";
  for (int index = 0; index < 100; ++index) {
    ui::ThreadListRow row;
    row.id = "b-" + std::to_string(index);
    row.presentationKey = row.id;
    row.title = "B " + std::to_string(index);
    snapshot.roots.push_back(std::move(row));
  }
  pane.refresh(snapshot);
  pane.show();
  QApplication::processEvents();
  QTreeWidget *tree = threadTree(pane);
  QTreeWidgetItem *anchor = threadItem(tree, "b-50");
  QTreeWidgetItem *nextSelection = threadItem(tree, "b-99");
  if (!require(tree && anchor && nextSelection,
               "selection-priority fixture did not populate the tree"))
    return false;
  tree->scrollToItem(anchor, QAbstractItemView::PositionAtTop);
  QApplication::processEvents();
  for (int index = 0; index < 10; ++index) {
    ui::ThreadListRow row;
    row.id = "a-" + std::to_string(index);
    row.presentationKey = row.id;
    row.title = "A " + std::to_string(index);
    snapshot.roots.push_back(std::move(row));
  }
  snapshot.selectedThreadId = "b-99";
  pane.refresh(snapshot);
  QApplication::processEvents();
  return require(tree->currentItem() == nextSelection &&
                     nextSelection->isSelected() &&
                     tree->visualItemRect(nextSelection)
                         .intersects(tree->viewport()->rect()),
                 "the prior viewport anchor hid the new authoritative "
                 "selection");
}

bool retainedSelectionRemainsVisibleAfterReparenting() {
  ui::ThreadListRow moving;
  moving.id = "moving";
  moving.presentationKey = moving.id;
  moving.title = "Moving";
  ui::ThreadListRow parent;
  parent.id = "parent";
  parent.presentationKey = parent.id;
  parent.title = "Parent";
  ui::ThreadListSnapshot snapshot;
  snapshot.selectedThreadId = moving.id;
  snapshot.roots = {moving, parent};
  middle::ThreadPane pane;
  pane.resize(300, 500);
  pane.refresh(snapshot);
  pane.show();
  QApplication::processEvents();
  QTreeWidget *tree = threadTree(pane);
  QTreeWidgetItem *movingItem = threadItem(tree, moving.id);
  if (!require(tree && movingItem && tree->currentItem() == movingItem,
               "reparenting fixture did not select the original row"))
    return false;

  snapshot.selectedThreadId.clear();
  parent.children.push_back(moving);
  snapshot.roots = {parent};
  pane.refresh(snapshot);
  QApplication::processEvents();
  QTreeWidgetItem *parentItem = threadItem(tree, parent.id);
  return require(
      threadItem(tree, moving.id) == movingItem && parentItem &&
          movingItem->parent() == parentItem && parentItem->isExpanded() &&
          tree->currentItem() == movingItem &&
          tree->visualItemRect(movingItem).intersects(tree->viewport()->rect()),
      "reparenting hid or replaced the retained current thread");
}

bool incrementalRootSortPreservesTheViewportAnchor() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef selected;
  {
    auto write = graph.write();
    const nodegraph::NodeRef runtime =
        write.upsert({nodegraph::NodeKind::Runtime, "sort-runtime"});
    for (int index = 0; index < 100; ++index) {
      const std::string id = "sort-" + std::to_string(index);
      nodegraph::NodeState state;
      state.fields.emplace("name", "Sort " + std::to_string(index));
      state.fields.emplace("recencyAt", static_cast<std::int64_t>(index));
      const nodegraph::NodeRef thread =
          write.upsert({nodegraph::NodeKind::Thread, id}, std::move(state));
      write.relate(runtime, nodegraph::RelationKind::RootThread, thread);
      if (index == 50)
        selected = thread;
    }
    static_cast<void>(write.finish());
  }
  ui::NodeGraphUiAdapter adapter(graph);
  const auto snapshot = adapter.threads(selected);
  middle::ThreadPane pane;
  pane.resize(300, 500);
  if (!require(snapshot && snapshot->roots.size() == 100,
               "incremental-sort fixture did not project every thread"))
    return false;
  pane.refresh(*snapshot);
  pane.show();
  QApplication::processEvents();
  QTreeWidget *tree = threadTree(pane);
  QTreeWidgetItem *anchor = threadItem(tree, "sort-50");
  if (!require(tree && anchor,
               "incremental-sort fixture did not retain its selected row"))
    return false;
  tree->scrollToItem(anchor, QAbstractItemView::PositionAtTop);
  QApplication::processEvents();
  const QPoint probe(tree->viewport()->width() / 2, 0);
  const QModelIndex before = tree->indexAt(probe);
  const QString beforeId = before.data(Qt::UserRole).toString();
  const int beforeY = tree->visualRect(before).top();
  const auto promoted = std::ranges::find(
      snapshot->roots, std::string("sort-0"), &ui::ThreadListRow::id);
  if (!require(promoted != snapshot->roots.end(),
               "incremental-sort fixture omitted the promoted row"))
    return false;
  ui::ThreadListRow row = *promoted;
  row.recencyAt = 1000;
  if (!require(pane.applyRowPresentation(row),
               "incremental root presentation update was rejected"))
    return false;
  QApplication::processEvents();
  const QModelIndex after = tree->indexAt(probe);
  return require(beforeId == QStringLiteral("sort-50") &&
                     after.data(Qt::UserRole).toString() == beforeId &&
                     tree->visualRect(after).top() == beforeY,
                 "incremental root sorting moved the stable viewport anchor");
}

class ViewportPaintProbe final : public QObject {
public:
  void reset(QAbstractItemView *nextView, QRect nextAllowed = {}) {
    view = nextView;
    allowed = nextAllowed;
    paints = 0;
    bounded = true;
  }

  int paints = 0;
  bool bounded = true;

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (view && watched == view->viewport() && event->type() == QEvent::Paint) {
      ++paints;
      if (!allowed.isNull()) {
        const QRect dirty =
            static_cast<QPaintEvent *>(event)->region().boundingRect();
        bounded = bounded && allowed.adjusted(-2, -2, 2, 2).contains(dirty);
      }
    }
    return false;
  }

private:
  QPointer<QAbstractItemView> view;
  QRect allowed;
};

void settleThreadPane() {
  for (int pass = 0; pass < 4; ++pass) {
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents(QEventLoop::AllEvents, 10);
  }
}

void waitThreadPane(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
  settleThreadPane();
}

quint64 imageChecksum(const QImage &image) {
  quint64 result = 1469598103934665603ULL;
  const uchar *bits = image.constBits();
  for (qsizetype offset = 0; offset < image.sizeInBytes(); offset += 97) {
    result ^= bits[offset];
    result *= 1099511628211ULL;
  }
  return result ^ static_cast<quint64>(image.width()) ^
         (static_cast<quint64>(image.height()) << 32U);
}

std::string benchmarkThreadId(std::size_t index) {
  std::ostringstream value;
  value << "thread-" << std::setw(6) << std::setfill('0') << index;
  return value.str();
}

#if defined(__SANITIZE_ADDRESS__)
constexpr bool InstrumentedBuild = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool InstrumentedBuild = true;
#else
constexpr bool InstrumentedBuild = false;
#endif
#else
constexpr bool InstrumentedBuild = false;
#endif

bool threadPerformanceProfile(std::size_t count, qreal expectedDpr,
                              bool enforceGates) {
  QStringList failures;
  const auto gate = [&failures](bool condition, QString message) {
    if (condition)
      return;
    failures.push_back(std::move(message));
    std::cerr << "THREAD PERFORMANCE GATE FAILED: "
              << failures.back().toStdString() << '\n';
  };

  nodegraph::NodeGraph graph;
  nodegraph::NodeRef selected;
  {
    auto write = graph.write();
    const nodegraph::NodeRef runtime =
        write.upsert({nodegraph::NodeKind::Runtime, "runtime"});
    for (std::size_t index = 0; index < count; ++index) {
      nodegraph::NodeState state;
      const std::string id = benchmarkThreadId(index);
      state.fields.emplace("name", "Thread " + id);
      state.fields.emplace("cwd", "/workspace/" + id);
      state.fields.emplace("createdAt", static_cast<std::int64_t>(index + 1));
      state.fields.emplace("recencyAt", static_cast<std::int64_t>(index + 1));
      const nodegraph::NodeRef thread =
          write.upsert({nodegraph::NodeKind::Thread, id}, std::move(state));
      write.relate(runtime, nodegraph::RelationKind::RootThread, thread);
      if (index == count / 2)
        selected = thread;
    }
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  QElapsedTimer adapterTimer;
  adapterTimer.start();
  const std::optional<ui::ThreadListSnapshot> snapshot =
      adapter.threads(selected);
  const qint64 adapterMicros = adapterTimer.nsecsElapsed() / 1000;
  gate(snapshot && snapshot->roots.size() == count,
       QStringLiteral("actual graph projection lost thread rows"));
  if (!snapshot)
    return false;

  middle::ThreadPane pane;
  pane.resize(320, 520);
  const int fixedWidgets = pane.findChildren<QWidget *>().size();
  const int fixedDocuments = pane.findChildren<QTextDocument *>().size();
  auto *view =
      pane.findChild<QAbstractItemView *>(QStringLiteral("threadList"));
  gate(view != nullptr, QStringLiteral("thread item view missing"));
  if (!view)
    return false;
  installAnimationDurationStyle(*view, 100);

  ViewportPaintProbe paints;
  qApp->installEventFilter(&paints);
  paints.reset(view);
  QElapsedTimer populationTimer;
  populationTimer.start();
  pane.refresh(*snapshot);
  pane.show();
  settleThreadPane();
  const qint64 populationMicros = populationTimer.nsecsElapsed() / 1000;
  const qreal actualDpr = pane.devicePixelRatioF();
  const int populatedWidgets = pane.findChildren<QWidget *>().size();
  const int documents = pane.findChildren<QTextDocument *>().size();
  const int rootRows = view->model()->rowCount();
  const std::function<int(const QModelIndex &)> countIndexWidgets =
      [&](const QModelIndex &parent) {
        int result = 0;
        for (int row = 0; row < view->model()->rowCount(parent); ++row) {
          const QModelIndex index = view->model()->index(row, 0, parent);
          result += view->indexWidget(index) != nullptr;
          result += countIndexWidgets(index);
        }
        return result;
      };
  const int indexWidgets = countIndexWidgets({});

  QTreeWidget *tree = qobject_cast<QTreeWidget *>(view);
  QTreeWidgetItem *selectedItem =
      tree ? threadItem(tree, snapshot->selectedThreadId) : nullptr;
  const QModelIndex selectedIndex =
      selectedItem ? tree->indexFromItem(selectedItem) : QModelIndex{};
  gate(selectedIndex.isValid(),
       QStringLiteral("authoritative selection is absent from the tree"));
  view->setCurrentIndex(selectedIndex);
  view->scrollTo(selectedIndex, QAbstractItemView::PositionAtCenter);
  view->setFocus();
  settleThreadPane();
  const int anchorX = view->viewport()->width() / 2;
  const QModelIndex anchorBefore = view->indexAt(QPoint(anchorX, 1));
  const QString anchorIdBefore = anchorBefore.data(Qt::UserRole).toString();
  const int anchorYBefore =
      anchorBefore.isValid() ? view->visualRect(anchorBefore).top() : 0;
  const int scrollBefore = view->verticalScrollBar()->value();
  const QPersistentModelIndex persistentSelected(selectedIndex);
  const quint64 pixelsBefore =
      imageChecksum(view->viewport()->grab().toImage());

  int modelMutations = 0;
  const auto mutation = [&modelMutations] { ++modelMutations; };
  QObject::connect(view->model(), &QAbstractItemModel::modelReset, &pane,
                   mutation);
  QObject::connect(view->model(), &QAbstractItemModel::layoutChanged, &pane,
                   mutation);
  QObject::connect(view->model(), &QAbstractItemModel::rowsInserted, &pane,
                   [&modelMutations] { ++modelMutations; });
  QObject::connect(view->model(), &QAbstractItemModel::rowsRemoved, &pane,
                   [&modelMutations] { ++modelMutations; });
  QObject::connect(view->model(), &QAbstractItemModel::rowsMoved, &pane,
                   [&modelMutations] { ++modelMutations; });
  QObject::connect(view->model(), &QAbstractItemModel::dataChanged, &pane,
                   [&modelMutations] { ++modelMutations; });

  paints.reset(view);
  QElapsedTimer noOpTimer;
  noOpTimer.start();
  pane.refresh(*snapshot);
  settleThreadPane();
  const qint64 noOpMicros = noOpTimer.nsecsElapsed() / 1000;
  const int noOpPaints = paints.paints;
  const QModelIndex anchorAfter = view->indexAt(QPoint(anchorX, 1));
  const bool noOpStable =
      modelMutations == 0 && persistentSelected.isValid() &&
      persistentSelected == view->currentIndex() &&
      view->verticalScrollBar()->value() == scrollBefore &&
      anchorAfter.data(Qt::UserRole).toString() == anchorIdBefore &&
      (!anchorAfter.isValid() ||
       view->visualRect(anchorAfter).top() == anchorYBefore) &&
      imageChecksum(view->viewport()->grab().toImage()) == pixelsBefore;

  ui::ThreadListRow offscreen = snapshot->roots.back();
  offscreen.awaitingPromptAcknowledgement = true;
  offscreen.pendingPromptAdmittedAtMs =
      QDateTime::currentMSecsSinceEpoch() - 1500;
  paints.reset(view);
  const bool offscreenApplied = pane.applyRowPresentation(offscreen);
  settleThreadPane();
  paints.reset(view);
  waitThreadPane(170);
  const int offscreenAnimationPaints = paints.paints;
  auto *animation =
      pane.findChild<QTimer *>(QStringLiteral("optimisticThreadAnimation"));
  const bool offscreenTimerActive = animation && animation->isActive();
  offscreen.awaitingPromptAcknowledgement = false;
  offscreen.pendingPromptAdmittedAtMs.reset();
  static_cast<void>(pane.applyRowPresentation(offscreen));
  settleThreadPane();

  const auto selectedRow = std::ranges::find(
      snapshot->roots, snapshot->selectedThreadId, &ui::ThreadListRow::id);
  gate(selectedRow != snapshot->roots.end(),
       QStringLiteral("selected row is absent from the snapshot"));
  ui::ThreadListRow visible = selectedRow != snapshot->roots.end()
                                  ? *selectedRow
                                  : snapshot->roots.front();
  visible.awaitingPromptAcknowledgement = true;
  visible.pendingPromptAdmittedAtMs =
      QDateTime::currentMSecsSinceEpoch() - 1500;
  QRect visibleRect = view->visualRect(view->currentIndex());
  visibleRect.setLeft(view->viewport()->rect().left());
  visibleRect.setRight(view->viewport()->rect().right());
  const bool visibleApplied = pane.applyRowPresentation(visible);
  settleThreadPane();
  paints.reset(view, visibleRect);
  waitThreadPane(170);
  const int visibleAnimationPaints = paints.paints;
  const bool visiblePaintsBounded = paints.bounded;
  visible.awaitingPromptAcknowledgement = false;
  visible.pendingPromptAdmittedAtMs.reset();
  static_cast<void>(pane.applyRowPresentation(visible));
  settleThreadPane();

  std::cout << "THREAD_PROFILE rows=" << count
            << " expected_dpr=" << expectedDpr << " actual_dpr=" << actualDpr
            << " adapter_us=" << adapterMicros
            << " population_us=" << populationMicros
            << " fixed_widgets=" << fixedWidgets
            << " populated_widgets=" << populatedWidgets
            << " index_widgets=" << indexWidgets << " documents=" << documents
            << " no_op_us=" << noOpMicros << " no_op_paints=" << noOpPaints
            << " no_op_stable=" << noOpStable
            << " offscreen_animation_paints=" << offscreenAnimationPaints
            << " offscreen_timer_active=" << offscreenTimerActive
            << " visible_animation_paints=" << visibleAnimationPaints
            << " visible_paints_bounded=" << visiblePaintsBounded << '\n';

  gate(std::abs(actualDpr - expectedDpr) < 0.02,
       QStringLiteral("actual DPR did not match the requested profile"));
  gate(rootRows == static_cast<int>(count),
       QStringLiteral("thread model cardinality is not exact"));
  gate(offscreenApplied && visibleApplied,
       QStringLiteral("exact row update was rejected"));
  if (enforceGates) {
    const qint64 scaleAllowance = InstrumentedBuild ? 4 : 1;
    gate(indexWidgets == 0,
         QStringLiteral("thread rows retained index widgets"));
    gate(populatedWidgets <= fixedWidgets + 4,
         QStringLiteral("QWidget residency grew with thread count"));
    gate(documents == fixedDocuments,
         QStringLiteral("thread rows retained QTextDocuments"));
    gate(noOpStable && noOpPaints == 0,
         QStringLiteral(
             "semantic no-op changed model, pixels, focus, or anchor"));
    gate(codexui::testing::timingLimit(noOpMicros <= 100000 * scaleAllowance),
         QStringLiteral("10k semantic no-op exceeded the work budget"));
    gate(
        codexui::testing::timingLimit(adapterMicros <= 500000 * scaleAllowance),
        QStringLiteral("thread projection exceeded the quantitative budget"));
    gate(codexui::testing::timingLimit(populationMicros <=
                                       2000000 * scaleAllowance),
         QStringLiteral("thread population exceeded the quantitative budget"));
    gate(!offscreenTimerActive && offscreenAnimationPaints == 0,
         QStringLiteral("offscreen animation retained timer or paint work"));
    gate(visibleAnimationPaints >= 2 && visibleAnimationPaints <= 8 &&
             visiblePaintsBounded,
         QStringLiteral("visible animation did not remain row-local"));
  }
  qApp->removeEventFilter(&paints);
  return failures.isEmpty();
}

} // namespace
} // namespace codexui::codex

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  if (argc >= 3) {
    const std::size_t count =
        std::max<std::size_t>(1, std::strtoull(argv[1], nullptr, 10));
    const qreal expectedDpr = std::max(0.1, std::strtod(argv[2], nullptr));
    const bool enforceGates =
        argc < 4 || std::string_view(argv[3]) != "baseline";
    return codexui::codex::threadPerformanceProfile(count, expectedDpr,
                                                    enforceGates)
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
  }
  bool passed = true;
  const QString expectedDprValue = qEnvironmentVariable("CODEXUI_EXPECTED_DPR");
  if (!expectedDprValue.isEmpty()) {
    bool validDpr = false;
    const qreal expectedDpr = expectedDprValue.toDouble(&validDpr);
    QWidget probe;
    probe.show();
    QApplication::processEvents();
    passed &= codexui::codex::require(
        validDpr && std::abs(probe.devicePixelRatioF() - expectedDpr) < 0.02,
        "ThreadPane visual test did not run at its requested DPR");
  }
  passed &= codexui::codex::forkNamesPreserveTheirLineage();
  passed &= codexui::codex::selectedChildRetainsRootAndCanonicalIdentity();
  passed &= codexui::codex::accessibilityFollowsTheNativeHierarchy();
  passed &= codexui::codex::presentationLifetimeFollowsExactThreadIncarnation();
  passed &= codexui::codex::sortingAndPromptAnimationAreFixed();
  passed &=
      codexui::codex::reducedMotionStopsThreadFeedbackAtTheStyleBoundary();
  passed &= codexui::codex::threadRowsConsumeCanonicalStatusTone();
  passed &= codexui::codex::optimisticCreationHandsOneRowToGraphAuthority();
  passed &=
      codexui::codex::optimisticDiscardRetiresAuthorityBeforeSelectionReentry();
  passed &= codexui::codex::pagingFollowsTheVisibleListEnd();
  passed &= codexui::codex::structuralInsertionPreservesTheViewportAnchor();
  passed &=
      codexui::codex::authoritativeSelectionWinsOverThePriorViewportAnchor();
  passed &= codexui::codex::retainedSelectionRemainsVisibleAfterReparenting();
  passed &= codexui::codex::incrementalRootSortPreservesTheViewportAnchor();
  if (passed)
    std::cout << "NodeGraph ThreadPane UI tests passed\n";
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
