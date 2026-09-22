// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/middle/ConversationPresentation.h"
#include "codex/middle/InspectorPane.h"
#include "codex/nodegraph/NodeGraph.h"
#include "codex/ui/NodeGraphUiAdapter.h"
#include "codex/ui/UiStyle.h"
#include "AccessibilityEventProbe.h"

#include <QAbstractTextDocumentLayout>
#include <QAbstractScrollArea>
#include <QAccessibleInterface>
#include <QApplication>
#include <QChildEvent>
#include <QClipboard>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include <git2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace codexui::codex {
namespace {

bool expect(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAILED: " << message << '\n';
  return false;
}

template <typename T>
const T *pageValue(const ui::InspectorPageSnapshot &page,
                   std::size_t valueIndex = 0) {
  for (const ui::InspectorRow &row : page.rows)
    if (const auto *value = std::get_if<T>(&row.value)) {
      if (valueIndex == 0)
        return value;
      --valueIndex;
    }
  return nullptr;
}

template <typename T>
T *pageValue(ui::InspectorPageSnapshot &page,
             std::size_t valueIndex = 0) {
  for (ui::InspectorRow &row : page.rows)
    if (auto *value = std::get_if<T>(&row.value)) {
      if (valueIndex == 0)
        return value;
      --valueIndex;
    }
  return nullptr;
}

template <typename T>
std::size_t pageValueCount(const ui::InspectorPageSnapshot &page) {
  return static_cast<std::size_t>(std::ranges::count_if(
      page.rows, [](const ui::InspectorRow &row) {
        return std::holds_alternative<T>(row.value);
      }));
}

ui::InspectorRow agentRow(std::string id, ui::InspectorAgentRow agent) {
  return {"agent:" + std::move(id), std::move(agent)};
}

ui::InspectorRow requestRow(PendingRequestDescriptor request) {
  return {request.target
              ? "request:" + std::to_string(request.target->incarnation())
              : std::string{},
          std::move(request)};
}

void setPageRows(ui::InspectorPageSnapshot &page,
                 std::vector<ui::InspectorRow> rows,
                 std::uint64_t orderRevision = 1) {
  page = {};
  page.total = rows.size();
  page.orderRevision = orderRevision;
  page.rows = std::move(rows);
}

struct WindowedInspectorData final {
  std::uint64_t threadIncarnation = 0;
  std::uint64_t planOrderRevision = 1;
  std::uint64_t agentOrderRevision = 1;
  std::uint64_t requestOrderRevision = 1;
  std::vector<ui::InspectorRow> plan;
  std::vector<ui::InspectorRow> agents;
  std::vector<ui::InspectorRow> requests;
  std::size_t responseCount = 0;
  std::size_t maximumRowsReturned = 0;

  ui::InspectorPageSnapshot
  page(ui::InspectorProjection projection,
       const ui::InspectorRowRequest &request) {
    const std::vector<ui::InspectorRow> *source = nullptr;
    std::uint64_t revision = 0;
    std::string emptyMessage;
    switch (projection) {
    case ui::InspectorProjection::Plan:
      source = &plan;
      revision = planOrderRevision;
      break;
    case ui::InspectorProjection::Agents:
      source = &agents;
      revision = agentOrderRevision;
      emptyMessage = "No agent activity for this thread.";
      break;
    case ui::InspectorProjection::Requests:
      source = &requests;
      revision = requestOrderRevision;
      emptyMessage = "No pending requests.";
      break;
    case ui::InspectorProjection::Changes:
    case ui::InspectorProjection::State:
    case ui::InspectorProjection::Protocol:
      return {};
    }
    ui::InspectorPageSnapshot result;
    result.total = source->size();
    result.orderRevision = revision;
    result.first = std::min(request.first, result.total);
    const auto find = [&](std::string_view key) -> std::optional<std::size_t> {
      const auto found =
          std::ranges::find(*source, key, &ui::InspectorRow::key);
      return found == source->end()
                 ? std::nullopt
                 : std::optional<std::size_t>{static_cast<std::size_t>(
                       std::distance(source->begin(), found))};
    };
    if (projection == ui::InspectorProjection::Agents) {
      result.validRetainedKeys.emplace();
      const std::size_t retainedCount =
          std::min(request.retainedKeys.size(), ui::MaximumInspectorRows);
      for (std::size_t index = 0; index < retainedCount; ++index) {
        const std::string &key = request.retainedKeys[index];
        if (find(key) &&
            std::ranges::find(*result.validRetainedKeys, key) ==
                result.validRetainedKeys->end())
          result.validRetainedKeys->push_back(key);
      }
    }
    result.anchorIndex = find(request.anchorKey);
    result.focusedIndex = find(request.focusedKey);
    const std::size_t count = std::min(request.count, ui::MaximumInspectorRows);
    if (count != 0 && result.anchorIndex &&
        (*result.anchorIndex < result.first ||
         *result.anchorIndex - result.first >= count))
      result.first = *result.anchorIndex;
    const std::size_t end = std::min(result.total, result.first + count);
    result.rows.assign(source->begin() + static_cast<std::ptrdiff_t>(result.first),
                       source->begin() + static_cast<std::ptrdiff_t>(end));
    if (result.focusedIndex &&
        (*result.focusedIndex < result.first || *result.focusedIndex >= end))
      result.focused = source->at(*result.focusedIndex);
    if (result.total == 0)
      result.emptyMessage = std::move(emptyMessage);
    ++responseCount;
    maximumRowsReturned = std::max(maximumRowsReturned, result.rows.size());
    return result;
  }

  void present(middle::InspectorPane &pane,
               ui::InspectorProjection projection) {
    ui::InspectorSnapshot snapshot;
    snapshot.threadIncarnation = threadIncarnation;
    ui::InspectorPageSnapshot projected =
        page(projection, pane.rowRequest(projection));
    switch (projection) {
    case ui::InspectorProjection::Plan:
      snapshot.plan = std::move(projected);
      break;
    case ui::InspectorProjection::Agents:
      snapshot.agents = std::move(projected);
      break;
    case ui::InspectorProjection::Requests:
      snapshot.requests = std::move(projected);
      break;
    case ui::InspectorProjection::Changes:
    case ui::InspectorProjection::State:
    case ui::InspectorProjection::Protocol:
      break;
    }
    pane.refresh(snapshot, projection);
  }

  void bind(middle::InspectorPane &pane) {
    pane.setRefreshRequestedAction([this, &pane] {
      std::optional<ui::InspectorProjection> projection;
      switch (pane.tabs()->currentIndex()) {
      case 0:
        projection = ui::InspectorProjection::Plan;
        break;
      case 1:
        projection = ui::InspectorProjection::Agents;
        break;
      case 3:
        projection = ui::InspectorProjection::Requests;
        break;
      default:
        break;
      }
      if (projection)
        QTimer::singleShot(0, &pane, [this, &pane, projection] {
          present(pane, *projection);
        });
    });
    present(pane, ui::InspectorProjection::Plan);
    present(pane, ui::InspectorProjection::Agents);
    present(pane, ui::InspectorProjection::Requests);
  }
};

struct AdapterInspectorData final {
  AdapterInspectorData(const nodegraph::NodeGraph &graph,
                       nodegraph::NodeRef selectedThread,
                       std::array<std::size_t, 3> totals)
      : adapter(graph), thread(std::move(selectedThread)),
        expectedTotals(totals) {}

  static std::optional<std::size_t>
  pageIndex(ui::InspectorProjection projection) {
    switch (projection) {
    case ui::InspectorProjection::Plan:
      return 0;
    case ui::InspectorProjection::Agents:
      return 1;
    case ui::InspectorProjection::Requests:
      return 2;
    case ui::InspectorProjection::Changes:
    case ui::InspectorProjection::State:
    case ui::InspectorProjection::Protocol:
      return std::nullopt;
    }
    return std::nullopt;
  }

  std::optional<ui::InspectorPageSnapshot>
  page(ui::InspectorProjection projection,
       const ui::InspectorRowRequest &request) {
    const std::optional<std::size_t> page = pageIndex(projection);
    if (!page)
      return std::nullopt;
    QElapsedTimer timer;
    timer.start();
    const auto snapshot = adapter.inspector(thread, projection, request);
    const qint64 micros = timer.nsecsElapsed() / 1000;
    if (!snapshot) {
      valid = false;
      return std::nullopt;
    }
    const ui::InspectorPageSnapshot *result = nullptr;
    switch (projection) {
    case ui::InspectorProjection::Plan:
      result = &snapshot->plan;
      break;
    case ui::InspectorProjection::Agents:
      result = &snapshot->agents;
      break;
    case ui::InspectorProjection::Requests:
      result = &snapshot->requests;
      break;
    case ui::InspectorProjection::Changes:
    case ui::InspectorProjection::State:
    case ui::InspectorProjection::Protocol:
      break;
    }
    valid &= result && result->total == expectedTotals[*page] &&
             result->rows.size() <= ui::MaximumInspectorRows &&
             result->rows.size() + (result->focused ? 1 : 0) <=
                 ui::MaximumInspectorRows + 1;
    maximumRowsReturned =
        std::max(maximumRowsReturned, result ? result->rows.size() : 0U);
    maximumValuesReturned = std::max(
        maximumValuesReturned,
        result ? result->rows.size() + (result->focused ? 1U : 0U) : 0U);
    maximumResponseMicros = std::max(maximumResponseMicros, micros);
    projectionMicros[*page] += micros;
    ++projectionResponses[*page];
    ++responseCount;
    return result ? std::optional{*result} : std::nullopt;
  }

  void present(middle::InspectorPane &pane,
               ui::InspectorProjection projection) {
    const auto projected = page(projection, pane.rowRequest(projection));
    if (!projected)
      return;
    ui::InspectorSnapshot snapshot;
    snapshot.threadIncarnation = thread->incarnation();
    switch (projection) {
    case ui::InspectorProjection::Plan:
      snapshot.plan = *projected;
      break;
    case ui::InspectorProjection::Agents:
      snapshot.agents = *projected;
      break;
    case ui::InspectorProjection::Requests:
      snapshot.requests = *projected;
      break;
    case ui::InspectorProjection::Changes:
    case ui::InspectorProjection::State:
    case ui::InspectorProjection::Protocol:
      return;
    }
    pane.refresh(snapshot, projection);
  }

  void bind(middle::InspectorPane &pane) {
    pane.setRefreshRequestedAction([this, &pane] {
      const std::optional<ui::InspectorProjection> projection =
          pane.currentProjection();
      if (!projection || !pageIndex(*projection))
        return;
      QTimer::singleShot(0, &pane,
                         [this, &pane, projection] { present(pane, *projection); });
    });
    present(pane, ui::InspectorProjection::Plan);
    present(pane, ui::InspectorProjection::Agents);
    present(pane, ui::InspectorProjection::Requests);
  }

  ui::NodeGraphUiAdapter adapter;
  nodegraph::NodeRef thread;
  std::array<std::size_t, 3> expectedTotals;
  std::array<qint64, 3> projectionMicros{};
  std::array<std::size_t, 3> projectionResponses{};
  std::size_t responseCount = 0;
  std::size_t maximumRowsReturned = 0;
  std::size_t maximumValuesReturned = 0;
  qint64 maximumResponseMicros = 0;
  bool valid = true;
};

class InspectorWorkProbe final : public QObject {
public:
  void watch(middle::InspectorPane &pane) {
    root_ = &pane;
    constexpr std::array names{"inspectorPlanRows", "inspectorAgentRows",
                               "inspectorRequestRows"};
    for (std::size_t index = 0; index < names.size(); ++index)
      if (auto *view = pane.findChild<QAbstractScrollArea *>(
              QString::fromLatin1(names[index])))
        viewports_[index] = view->viewport();
  }

  void resetSample() noexcept {
    sampleConstructions = 0;
    sampleRetirements = 0;
    sampleRowResizes = 0;
    sampleLayoutRequests = 0;
    samplePaints = 0;
  }

  void finishSample() noexcept {
    maximumConstructions =
        std::max(maximumConstructions, sampleConstructions);
    maximumRetirements = std::max(maximumRetirements, sampleRetirements);
    maximumRowResizes = std::max(maximumRowResizes, sampleRowResizes);
    maximumLayoutRequests =
        std::max(maximumLayoutRequests, sampleLayoutRequests);
    maximumPaints = std::max(maximumPaints, samplePaints);
  }

  std::size_t constructions = 0;
  std::size_t retirements = 0;
  std::size_t rowResizes = 0;
  std::size_t layoutRequests = 0;
  std::size_t paints = 0;
  std::size_t peakRows = 0;
  std::size_t sampleConstructions = 0;
  std::size_t sampleRetirements = 0;
  std::size_t sampleRowResizes = 0;
  std::size_t sampleLayoutRequests = 0;
  std::size_t samplePaints = 0;
  std::size_t maximumConstructions = 0;
  std::size_t maximumRetirements = 0;
  std::size_t maximumRowResizes = 0;
  std::size_t maximumLayoutRequests = 0;
  std::size_t maximumPaints = 0;
  std::size_t maximumFramePaints = 0;
  std::size_t paintFrames = 0;

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    // One backing-store update delivers the child paints for a frame. A
    // residency sample can span many such frames while admission yields.
    if (watched == root_ && event->type() == QEvent::UpdateRequest) {
      framePaints_ = 0;
      ++paintFrames;
    }
    const bool viewport =
        std::ranges::find(viewports_, watched) != viewports_.end();
    if (viewport && event->type() == QEvent::ChildAdded) {
      QObject *child = static_cast<QChildEvent *>(event)->child();
      if (qobject_cast<QWidget *>(child) && rows_.insert(child).second) {
        ++constructions;
        ++sampleConstructions;
        peakRows = std::max(peakRows, rows_.size());
      }
    } else if (viewport && event->type() == QEvent::ChildRemoved) {
      QObject *child = static_cast<QChildEvent *>(event)->child();
      if (rows_.erase(child) != 0) {
        ++retirements;
        ++sampleRetirements;
      }
    } else if (event->type() == QEvent::Resize &&
               rows_.contains(watched)) {
      ++rowResizes;
      ++sampleRowResizes;
    }

    auto *widget = qobject_cast<QWidget *>(watched);
    if (!root_ || !widget ||
        (widget != root_ && !root_->isAncestorOf(widget)))
      return false;
    if (event->type() == QEvent::LayoutRequest) {
      ++layoutRequests;
      ++sampleLayoutRequests;
    } else if (event->type() == QEvent::Paint) {
      ++paints;
      ++samplePaints;
      maximumFramePaints = std::max(maximumFramePaints, ++framePaints_);
    }
    return false;
  }

private:
  std::size_t framePaints_ = 0;
  QPointer<QWidget> root_;
  std::array<QObject *, 3> viewports_{};
  std::unordered_set<QObject *> rows_;
};

class InspectorDocumentTracker final : public QObject {
public:
  void observe(const middle::InspectorPane &pane) {
    for (middle::MarkdownTextView *view :
         pane.findChildren<middle::MarkdownTextView *>()) {
      QTextDocument *document = view->document();
      const auto [found, inserted] =
          sources_.try_emplace(document, view->markdownSource());
      if (!inserted) {
        if (found->second != view->markdownSource()) {
          found->second = view->markdownSource();
          ++parseInputs;
        }
        continue;
      }
      ++constructions;
      if (!view->markdownSource().isEmpty())
        ++parseInputs;
      connect(document, &QTextDocument::contentsChanged, this,
              [this] { ++mutations; });
      connect(document->documentLayout(),
              &QAbstractTextDocumentLayout::documentSizeChanged, this,
              [this](const QSizeF &) { ++measurements; });
      connect(document, &QObject::destroyed, this, [this, document] {
        sources_.erase(document);
        ++retirements;
      });
    }
    peakDocuments = std::max(peakDocuments, sources_.size());
  }

  std::size_t constructions = 0;
  std::size_t retirements = 0;
  std::size_t parseInputs = 0;
  std::size_t mutations = 0;
  std::size_t measurements = 0;
  std::size_t peakDocuments = 0;

private:
  std::unordered_map<QTextDocument *, QString> sources_;
};

int accessibleButtonCount(QAccessibleInterface *interface, const QString &name) {
  if (!interface)
    return 0;
  int count = 0;
  if (interface->role() == QAccessible::Button &&
      interface->text(QAccessible::Name) == name)
    ++count;
  for (int index = 0; index < interface->childCount(); ++index)
    count += accessibleButtonCount(interface->child(index), name);
  return count;
}

int accessibleObjectCount(QAccessibleInterface *interface,
                          const QObject *object) {
  if (!interface)
    return 0;
  int count = interface->object() == object ? 1 : 0;
  for (int index = 0; index < interface->childCount(); ++index)
    count += accessibleObjectCount(interface->child(index), object);
  return count;
}

bool accessibleRowsMatch(QAbstractScrollArea *view) {
  if (!view)
    return false;
  QAccessibleInterface *viewport =
      QAccessible::queryAccessibleInterface(view->viewport());
  const auto rows = view->viewport()->findChildren<QWidget *>(
      QString{}, Qt::FindDirectChildrenOnly);
  if (!viewport || viewport->childCount() != rows.size())
    return false;
  std::unordered_set<QObject *> represented;
  for (int index = 0; index < viewport->childCount(); ++index) {
    QAccessibleInterface *child = viewport->child(index);
    QWidget *row = child ? qobject_cast<QWidget *>(child->object()) : nullptr;
    if (!row || !rows.contains(row) || !represented.insert(row).second)
      return false;
  }
  return represented.size() == static_cast<std::size_t>(rows.size());
}

#if QT_CONFIG(accessibility)
QAccessible::Id accessibleId(QObject *object) {
  QAccessibleInterface *interface =
      object ? QAccessible::queryAccessibleInterface(object) : nullptr;
  return interface ? QAccessible::uniqueId(interface) : 0;
}
#endif

bool requestedDprIsActive(qreal expected) {
  QWidget probe;
  probe.resize(16, 16);
  probe.show();
  QCoreApplication::processEvents();
  const qreal actual = probe.devicePixelRatioF();
  return expect(std::abs(actual - expected) <= 0.02,
                "the Inspector test runs at its requested device-pixel ratio");
}

bool labelUsesColor(QLabel *label, const char *color) {
  if (!label)
    return false;
  label->ensurePolished();
  return label->palette().color(QPalette::WindowText) ==
         QColor(QString::fromLatin1(color));
}

template <typename Predicate>
bool waitFor(Predicate &&predicate, int timeoutMilliseconds = 3000) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!predicate() && elapsed.elapsed() < timeoutMilliseconds) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(2);
  }
  return predicate();
}

nodegraph::NodeRef addActivity(nodegraph::NodeGraph::WriteAccess &write,
                               const nodegraph::NodeRef &turn, std::string id,
                               std::string kind, std::string childId,
                               std::string status, std::string tool = {}) {
  nodegraph::NodeState state;
  state.status = status == "completed"     ? nodegraph::NodeStatus::Completed
                 : status == "interrupted" ? nodegraph::NodeStatus::Interrupted
                                           : nodegraph::NodeStatus::Running;
  state.fields = {{"type", nodegraph::Value("subAgentActivity")},
                  {"kind", nodegraph::Value(std::move(kind))},
                  {"agentThreadId", nodegraph::Value(std::move(childId))},
                  {"status", nodegraph::Value(std::move(status))},
                  {"tool", nodegraph::Value(std::move(tool))}};
  const nodegraph::NodeRef item = write.upsert(
      {nodegraph::NodeKind::Item, std::move(id)}, std::move(state));
  write.setParent(turn, item);
  return item;
}

PendingRequestDescriptor requestDescriptor(nodegraph::NodeRef target) {
  PendingRequestDescriptor request;
  request.target = std::move(target);
  request.method = "item/commandExecution/requestApproval";
  request.kind = PendingRequestKind::CommandApproval;
  request.threadId = "stable-inspector";
  request.threadTitle = "Stable inspector";
  request.raw = {{"command", "make test"}};
  request.availability = PendingRequestAvailability::Actionable;
  return request;
}

bool logicalAgentsRemainDeduplicated() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef owner;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef childOne;
  nodegraph::NodeRef malformedReceivers;
  {
    auto write = graph.write();
    owner = write.upsert({nodegraph::NodeKind::Thread, "owner"});
    turn = write.upsert({nodegraph::NodeKind::Turn, "turn"});
    write.setParent(owner, turn);
    childOne = write.upsert({nodegraph::NodeKind::Thread, "child-one"},
                            {nodegraph::NodeStatus::Running, {}});
    const nodegraph::NodeRef started =
        addActivity(write, turn, "start", "started", "child-one", "inProgress",
                    "spawn_agent");
    write.setField(
        started, "receiverThreadIds",
        nodegraph::Value::Array{nodegraph::Value("child-one"),
                                nodegraph::Value("unrelated-fallback")});
    write.setField(
        started, "agentsStates",
        nodegraph::Value::Object{
            {"unrelated-state",
             nodegraph::Value::Object{{"status", "completed"}}}});
    write.relate(started, nodegraph::RelationKind::AgentChildThread, childOne);
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  auto snapshot = adapter.inspector(owner, ui::InspectorProjection::Agents);
  const auto agentAt = [](const auto &value, std::size_t index = 0) {
    return value ? pageValue<ui::InspectorAgentRow>(value->agents, index)
                 : nullptr;
  };
  const ui::InspectorAgentRow *agent = agentAt(snapshot);
  bool result =
      expect(snapshot && snapshot->agents.total == 1 && agent &&
                 agent->childThreadId == "child-one",
             "an exact child relation is the complete Agent identity set");

  {
    auto write = graph.write();
    for (const nodegraph::NodeRef &activity : {
             addActivity(write, turn, "progress", "progress", "child-one",
                         "inProgress"),
             addActivity(write, turn, "replay", "started", "child-one",
                         "inProgress", "spawn_agent"),
             addActivity(write, turn, "complete", "completed", "child-one",
                         "completed")})
      write.relate(activity, nodegraph::RelationKind::AgentChildThread,
                   childOne);
    malformedReceivers = addActivity(write, turn, "malformed-receivers",
                                     "progress", "child-one", "inProgress");
    write.setField(malformedReceivers, "receiverThreadIds",
                   nodegraph::Value::Array{
                       nodegraph::Value(std::uint64_t{7})});
    write.relate(malformedReceivers,
                 nodegraph::RelationKind::AgentChildThread, childOne);
    write.setStatus(childOne, nodegraph::NodeStatus::Completed);
    write.setField(childOne, "status", "completed");
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(owner, ui::InspectorProjection::Agents);
  agent = agentAt(snapshot);
  result &= expect(snapshot && snapshot->agents.total == 1 && agent &&
                       agent->status.semantic ==
                           nodegraph::NodeStatus::Completed &&
                       agent->receiverThreadIds ==
                           std::vector<std::string>{"child-one",
                                                    "unrelated-fallback"},
                   "start, progress, replay and completion remain one row, "
                   "and a numeric receiver cannot hide valid receiver metadata");

  {
    auto write = graph.write();
    write.setField(malformedReceivers, "receiverThreadIds",
                   nodegraph::Value::Array{nodegraph::Value("")});
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(owner, ui::InspectorProjection::Agents);
  agent = agentAt(snapshot);
  result &= expect(
      agent && agent->receiverThreadIds ==
                   std::vector<std::string>{"child-one",
                                            "unrelated-fallback"},
      "an empty receiver cannot hide valid receiver metadata");

  {
    auto write = graph.write();
    write.setField(
        malformedReceivers, "receiverThreadIds",
        nodegraph::Value::Array{nodegraph::Value(""),
                                nodegraph::Value("replacement")});
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(owner, ui::InspectorProjection::Agents);
  agent = agentAt(snapshot);
  result &= expect(
      agent && agent->receiverThreadIds ==
                   std::vector<std::string>{"replacement"},
      "empty receivers are absent from otherwise valid receiver metadata");

  {
    auto write = graph.write();
    const nodegraph::NodeRef stale = addActivity(
        write, turn, "stale", "progress", "child-one", "inProgress");
    write.relate(stale, nodegraph::RelationKind::AgentChildThread, childOne);
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(owner, ui::InspectorProjection::Agents);
  agent = agentAt(snapshot);
  result &=
      expect(agent && agent->status.semantic == nodegraph::NodeStatus::Completed,
             "a stale active update cannot overwrite terminal status");

  {
    auto write = graph.write();
    nodegraph::NodeState ordinary;
    ordinary.fields = {
        {"type", nodegraph::Value("collabAgentToolCall")},
        {"tool", nodegraph::Value("send_message")},
        {"receiverThreadIds", nodegraph::Value(nodegraph::Value::Array{
                                  nodegraph::Value("not-a-spawn")})}};
    const nodegraph::NodeRef item =
        write.upsert({nodegraph::NodeKind::Item, "ordinary-collaboration"},
                     std::move(ordinary));
    write.setParent(turn, item);
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(owner, ui::InspectorProjection::Agents);
  result &= expect(snapshot && snapshot->agents.total == 1,
                   "a non-spawn collaboration call creates no Agent row");

  {
    auto write = graph.write();
    nodegraph::NodeState spawn;
    spawn.status = nodegraph::NodeStatus::Running;
    spawn.fields = {
        {"type", nodegraph::Value("collabAgentToolCall")},
        {"tool", nodegraph::Value("spawn_agents_on_csv")},
        {"receiverThreadIds",
         nodegraph::Value(nodegraph::Value::Array{
             nodegraph::Value("child-two"), nodegraph::Value("child-two")})}};
    const nodegraph::NodeRef item = write.upsert(
        {nodegraph::NodeKind::Item, "multi-spawn"}, std::move(spawn));
    write.setParent(turn, item);
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(owner, ui::InspectorProjection::Agents);
  const ui::InspectorAgentRow *first = agentAt(snapshot, 0);
  const ui::InspectorAgentRow *second = agentAt(snapshot, 1);
  result &=
      expect(snapshot && snapshot->agents.total == 2 && first && second &&
                 first->childThreadId == "child-one" &&
                 second->childThreadId == "child-two",
             "two distinct children produce two stable ordered rows");

  {
    auto write = graph.write();
    const nodegraph::NodeRef interrupted = addActivity(
        write, turn, "interrupt", "interrupted", "child-one", "interrupted");
    write.relate(interrupted, nodegraph::RelationKind::AgentChildThread,
                 childOne);
    write.setStatus(childOne, nodegraph::NodeStatus::Interrupted);
    write.setField(childOne, "status", "interrupted");
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(owner, ui::InspectorProjection::Agents);
  first = agentAt(snapshot, 0);
  second = agentAt(snapshot, 1);
  result &=
      expect(snapshot && snapshot->agents.total == 2 && first && second &&
                 first->childThreadId == "child-one" &&
                 first->status.semantic ==
                     nodegraph::NodeStatus::Interrupted &&
                 second->childThreadId == "child-two",
             "interruption patches the existing row without reordering");
  const auto agentsOnly =
      adapter.inspector(owner, ui::InspectorProjection::Agents);
  result &=
      expect(agentsOnly && agentsOnly->agents.total == 2 &&
                 agentsOnly->plan.total == 0 &&
                 agentsOnly->state.state.empty() &&
                 agentsOnly->requests.total == 0,
             "the active Agents projection does not construct State, Plan, or "
             "Requests presentation data");

  const auto reportedStates = [](std::string status = {}) {
    nodegraph::Value::Object states{
        {"B", nodegraph::Value::Object{{"status", "inProgress"}}},
        {"D", nodegraph::Value::Object{{"status", "completed"}}}};
    if (!status.empty())
      states.emplace("A", nodegraph::Value::Object{{"status", status}});
    return nodegraph::Value(std::move(states));
  };
  nodegraph::NodeGraph fallbackGraph;
  nodegraph::NodeRef fallbackOwner;
  nodegraph::NodeRef fallbackTurn;
  nodegraph::NodeRef fallbackActivity;
  {
    auto write = fallbackGraph.write();
    fallbackOwner =
        write.upsert({nodegraph::NodeKind::Thread, "fallback-order-owner"});
    fallbackTurn =
        write.upsert({nodegraph::NodeKind::Turn, "fallback-order-turn"});
    write.setParent(fallbackOwner, fallbackTurn);
    nodegraph::NodeState source;
    source.status = nodegraph::NodeStatus::Running;
    source.fields = {
        {"type", "collabAgentToolCall"},
        {"tool", "spawn_agent"},
        {"agentThreadId", "A"},
        {"receiverThreadIds",
         nodegraph::Value::Array{nodegraph::Value("B"), nodegraph::Value("A"),
                                 nodegraph::Value("C"),
                                 nodegraph::Value(std::uint64_t{7}),
                                 nodegraph::Value("")}},
        {"agentsStates", reportedStates()}};
    fallbackActivity = write.upsert(
        {nodegraph::NodeKind::Item, "fallback-order-source"},
        std::move(source));
    write.setParent(fallbackTurn, fallbackActivity);
    static_cast<void>(write.finish());
  }
  ui::NodeGraphUiAdapter fallbackAdapter(fallbackGraph);
  const auto fallback = fallbackAdapter.inspector(
      fallbackOwner, ui::InspectorProjection::Agents);
  std::vector<std::string> fallbackIds;
  if (fallback)
    for (const ui::InspectorRow &row : fallback->agents.rows)
      if (const auto *agent = std::get_if<ui::InspectorAgentRow>(&row.value))
        fallbackIds.push_back(agent->childThreadId);
  result &= expect(fallbackIds ==
                       std::vector<std::string>{"A", "B", "C", "D"},
                   "fallback Agent identities preserve primary, receiver, "
                   "and reported order while deduplicating collisions");
  const auto fallbackStatus = [&] {
    const auto projected = fallbackAdapter.inspector(
        fallbackOwner, ui::InspectorProjection::Agents);
    const auto *row =
        projected ? pageValue<ui::InspectorAgentRow>(projected->agents)
                  : nullptr;
    return row ? row->status.semantic : nodegraph::NodeStatus::Unknown;
  };
  result &= expect(fallbackStatus() == nodegraph::NodeStatus::Running,
                   "a started fallback Agent projects Running");
  {
    auto write = fallbackGraph.write();
    write.setField(fallbackActivity, "agentsStates",
                   reportedStates("inProgress"));
    static_cast<void>(write.finish());
  }
  result &= expect(fallbackStatus() == nodegraph::NodeStatus::Running,
                   "reported Agent progress remains Running");
  {
    auto write = fallbackGraph.write();
    write.setField(fallbackActivity, "agentsStates",
                   reportedStates("completed"));
    static_cast<void>(write.finish());
  }
  result &= expect(fallbackStatus() == nodegraph::NodeStatus::Completed,
                   "reported Agent completion projects Completed");
  {
    auto write = fallbackGraph.write();
    write.setField(fallbackActivity, "agentsStates", reportedStates());
    write.setStatus(fallbackTurn, nodegraph::NodeStatus::Completed);
    static_cast<void>(write.finish());
  }
  result &= expect(
      fallbackStatus() == nodegraph::NodeStatus::NotLoaded,
      "a Running Agent missing reported child state in a terminal Turn "
      "projects NotLoaded");
  return result;
}

bool agentPresentationIdentityFollowsExactGraphLifetime() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef owner;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef activity;
  nodegraph::NodeRef firstChild;
  nodegraph::NodeRef childTurn;
  nodegraph::NodeRef previousMessage;
  nodegraph::NodeRef childMessage;
  nodegraph::NodeRef sourceActivity;
  {
    auto write = graph.write();
    owner = write.upsert({nodegraph::NodeKind::Thread, "identity-owner"});
    turn = write.upsert({nodegraph::NodeKind::Turn, "identity-turn"});
    write.setParent(owner, turn);
    firstChild = write.upsert(
        {nodegraph::NodeKind::Thread, "shared-child"},
        {nodegraph::NodeStatus::Completed, {{"status", "completed"}}});
    childTurn = write.upsert({nodegraph::NodeKind::Turn, "child-turn"});
    nodegraph::NodeState previousMessageState;
    previousMessageState.fields = {{"type", "agentMessage"},
                                   {"text", "previous child result"}};
    previousMessage = write.upsert(
        {nodegraph::NodeKind::Item, "previous-child-message"},
        std::move(previousMessageState));
    nodegraph::NodeState childMessageState;
    childMessageState.fields = {{"type", "agentMessage"},
                                {"text", "exact child result"}};
    childMessage = write.upsert(
        {nodegraph::NodeKind::Item, "child-message"},
        std::move(childMessageState));
    write.setParent(firstChild, childTurn);
    write.setParent(childTurn, previousMessage);
    write.setParent(childTurn, childMessage);
    activity = addActivity(write, turn, "exact-source", "started",
                           "shared-child", "inProgress", "spawn_agent");
    write.setField(activity, "prompt", "exact relation");
    write.relate(activity, nodegraph::RelationKind::AgentChildThread,
                 firstChild);
    nodegraph::NodeState sourceState;
    sourceState.status = nodegraph::NodeStatus::Running;
    sourceState.fields = {
        {"type", "collabAgentToolCall"},
        {"tool", "spawn_agent"},
        {"prompt", "source only"},
        {"receiverThreadIds",
         nodegraph::Value(nodegraph::Value::Array{
             nodegraph::Value("shared-child")})}};
    sourceActivity = write.upsert({nodegraph::NodeKind::Item, "source-only"},
                                  std::move(sourceState));
    write.setParent(turn, sourceActivity);
    static_cast<void>(write.finish());
  }

  const auto rowForPrompt = [](const ui::InspectorSnapshot &snapshot,
                               std::string_view prompt)
      -> const ui::InspectorAgentRow * {
    for (const ui::InspectorRow &row : snapshot.agents.rows)
      if (const auto *agent = std::get_if<ui::InspectorAgentRow>(&row.value);
          agent && agent->prompt == prompt)
        return agent;
    return nullptr;
  };
  const auto keyForPrompt = [](const ui::InspectorSnapshot &snapshot,
                               std::string_view prompt) {
    for (const ui::InspectorRow &row : snapshot.agents.rows)
      if (const auto *agent = std::get_if<ui::InspectorAgentRow>(&row.value);
          agent && agent->prompt == prompt)
        return row.key;
    return std::string{};
  };
  ui::NodeGraphUiAdapter adapter(graph);
  const auto initial = adapter.inspector(owner, ui::InspectorProjection::Agents);
  const ui::InspectorAgentRow *initialExact =
      initial ? rowForPrompt(*initial, "exact relation") : nullptr;
  const ui::InspectorAgentRow *initialSource =
      initial ? rowForPrompt(*initial, "source only") : nullptr;
  const std::string firstExactKey =
      initial ? keyForPrompt(*initial, "exact relation") : "";
  const std::string firstSourceKey =
      initial ? keyForPrompt(*initial, "source only") : "";
  bool result = expect(
      initialExact && initialSource && initial->agents.total == 2 &&
          initialExact->childThreadId == "shared-child" &&
          initialSource->childThreadId == "shared-child" &&
          initialExact->status.semantic == nodegraph::NodeStatus::Completed &&
          initialExact->resultText == "exact child result" &&
          initialSource->status.semantic == nodegraph::NodeStatus::Running &&
          initialSource->resultText.empty() &&
          firstExactKey != firstSourceKey && firstExactKey != "shared-child" &&
          firstSourceKey != "shared-child",
      "only an exact child relation imports child status and result into its "
      "distinct presentation identity");

  {
    auto write = graph.write();
    const std::array reordered{childMessage, previousMessage};
    write.replaceChildren(childTurn, reordered);
    static_cast<void>(write.finish());
  }
  const auto reorderedMessages =
      adapter.inspector(owner, ui::InspectorProjection::Agents);
  const ui::InspectorAgentRow *reorderedMessageRow =
      reorderedMessages ? rowForPrompt(*reorderedMessages, "exact relation")
                        : nullptr;
  result &= expect(
      reorderedMessageRow &&
          reorderedMessageRow->resultText == "previous child result",
      "an exact child result follows authoritative Item order, not creation order");
  {
    auto write = graph.write();
    const std::array restoredOrder{previousMessage, childMessage};
    write.replaceChildren(childTurn, restoredOrder);
    static_cast<void>(write.finish());
  }

  nodegraph::NodeRef laterChildTurn;
  {
    auto write = graph.write();
    laterChildTurn =
        write.upsert({nodegraph::NodeKind::Turn, "later-child-turn"});
    nodegraph::NodeState laterMessageState;
    laterMessageState.fields = {{"type", "agentMessage"},
                                {"text", "later turn result"}};
    const nodegraph::NodeRef laterMessage = write.upsert(
        {nodegraph::NodeKind::Item, "later-child-message"},
        std::move(laterMessageState));
    write.setParent(firstChild, laterChildTurn);
    write.setParent(laterChildTurn, laterMessage);
    static_cast<void>(write.finish());
  }
  const auto laterTurn =
      adapter.inspector(owner, ui::InspectorProjection::Agents);
  const ui::InspectorAgentRow *laterTurnRow =
      laterTurn ? rowForPrompt(*laterTurn, "exact relation") : nullptr;
  result &= expect(laterTurnRow &&
                       laterTurnRow->resultText == "later turn result",
                   "an exact child result follows the last current Turn");
  {
    auto write = graph.write();
    const std::array reordered{laterChildTurn, childTurn};
    write.replaceChildren(firstChild, reordered);
    static_cast<void>(write.finish());
  }
  const auto reorderedTurns =
      adapter.inspector(owner, ui::InspectorProjection::Agents);
  const ui::InspectorAgentRow *reorderedTurnRow =
      reorderedTurns ? rowForPrompt(*reorderedTurns, "exact relation")
                     : nullptr;
  result &= expect(
      reorderedTurnRow && reorderedTurnRow->resultText == "exact child result",
      "an exact child result follows authoritative Turn order, not creation order");
  {
    auto write = graph.write();
    const std::array restoredTurns{childTurn};
    write.replaceChildren(firstChild, restoredTurns);
    static_cast<void>(write.finish());
  }

  {
    auto write = graph.write();
    write.eraseField(childMessage, "text");
    static_cast<void>(write.finish());
  }
  const auto predecessor =
      adapter.inspector(owner, ui::InspectorProjection::Agents);
  const ui::InspectorAgentRow *predecessorRow =
      predecessor ? rowForPrompt(*predecessor, "exact relation") : nullptr;
  result &= expect(
      predecessorRow &&
          predecessorRow->resultText == "previous child result",
      "clearing the latest child result exposes its indexed predecessor");
  {
    auto write = graph.write();
    write.setField(childMessage, "text", "updated child result");
    static_cast<void>(write.finish());
  }
  const auto restored =
      adapter.inspector(owner, ui::InspectorProjection::Agents);
  const ui::InspectorAgentRow *restoredRow =
      restored ? rowForPrompt(*restored, "exact relation") : nullptr;
  result &= expect(restoredRow &&
                       restoredRow->resultText == "updated child result",
                   "updating the latest child result refreshes its exact parent row");

  {
    auto write = graph.write();
    write.remove(firstChild);
    static_cast<void>(write.finish());
  }
  nodegraph::NodeRef replacementChild;
  {
    auto write = graph.write();
    replacementChild = write.upsert(
        {nodegraph::NodeKind::Thread, "shared-child"},
        {nodegraph::NodeStatus::Completed, {{"status", "completed"}}});
    static_cast<void>(write.finish());
  }
  const auto beforeRebind =
      adapter.inspector(owner, ui::InspectorProjection::Agents);
  const ui::InspectorAgentRow *unbound =
      beforeRebind ? rowForPrompt(*beforeRebind, "exact relation") : nullptr;
  const std::string unboundKey =
      beforeRebind ? keyForPrompt(*beforeRebind, "exact relation") : "";
  result &= expect(
      unbound && unboundKey != firstExactKey &&
          unbound->status.semantic == nodegraph::NodeStatus::Running &&
          unbound->resultText.empty(),
      "retired Agent activity did not retarget a same-ID replacement child");

  {
    auto write = graph.write();
    write.relate(activity, nodegraph::RelationKind::AgentChildThread,
                 replacementChild);
    static_cast<void>(write.finish());
  }
  const auto rebound =
      adapter.inspector(owner, ui::InspectorProjection::Agents);
  const ui::InspectorAgentRow *reboundExact =
      rebound ? rowForPrompt(*rebound, "exact relation") : nullptr;
  const std::string reboundKey =
      rebound ? keyForPrompt(*rebound, "exact relation") : "";
  result &= expect(
      reboundExact && reboundKey != firstExactKey &&
          reboundKey != unboundKey &&
          reboundExact->childThreadId == "shared-child" &&
          reboundExact->status.semantic == nodegraph::NodeStatus::Completed,
      "an exact replacement relation acquired a fresh Agent identity and "
      "only then contributed child state");

  {
    auto write = graph.write();
    write.remove(sourceActivity);
    static_cast<void>(write.finish());
  }
  {
    auto write = graph.write();
    nodegraph::NodeState sourceState;
    sourceState.status = nodegraph::NodeStatus::Running;
    sourceState.fields = {
        {"type", "collabAgentToolCall"},
        {"tool", "spawn_agent"},
        {"prompt", "source only"},
        {"receiverThreadIds",
         nodegraph::Value(nodegraph::Value::Array{
             nodegraph::Value("shared-child")})}};
    sourceActivity = write.upsert({nodegraph::NodeKind::Item, "source-only"},
                                  std::move(sourceState));
    write.setParent(turn, sourceActivity);
    static_cast<void>(write.finish());
  }
  const auto replacedSource =
      adapter.inspector(owner, ui::InspectorProjection::Agents);
  const ui::InspectorAgentRow *currentSource =
      replacedSource ? rowForPrompt(*replacedSource, "source only") : nullptr;
  result &= expect(
      currentSource &&
          keyForPrompt(*replacedSource, "source only") != firstSourceKey,
      "source-only Agent identity follows the exact source Item incarnation");
  return result;
}

bool sharedChildFactsFanOutToEveryExactParent() {
  nodegraph::NodeGraph graph;
  std::array<nodegraph::NodeRef, 2> owners;
  nodegraph::NodeRef child;
  nodegraph::NodeRef childTurn;
  {
    auto write = graph.write();
    child = write.upsert(
        {nodegraph::NodeKind::Thread, "fanout-shared-child"},
        {nodegraph::NodeStatus::Running, {{"status", "inProgress"}}});
    childTurn =
        write.upsert({nodegraph::NodeKind::Turn, "fanout-child-turn"});
    write.setParent(child, childTurn);
    nodegraph::NodeState message;
    message.fields =
        {{"type", "agentMessage"}, {"text", "base child result"}};
    const nodegraph::NodeRef predecessor = write.upsert(
        {nodegraph::NodeKind::Item, "fanout-child-base"},
        std::move(message));
    write.setParent(childTurn, predecessor);
    for (std::size_t index = 0; index < owners.size(); ++index) {
      owners[index] = write.upsert(
          {nodegraph::NodeKind::Thread,
           "fanout-parent-" + std::to_string(index)});
      const nodegraph::NodeRef turn = write.upsert(
          {nodegraph::NodeKind::Turn,
           "fanout-parent-turn-" + std::to_string(index)});
      write.setParent(owners[index], turn);
      const nodegraph::NodeRef source = addActivity(
          write, turn, "fanout-parent-source-" + std::to_string(index),
          "started", "fanout-shared-child", "inProgress", "spawn_agent");
      write.setField(source, "prompt",
                     "parent " + std::to_string(index));
      write.relate(source, nodegraph::RelationKind::AgentChildThread, child);
    }
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  const auto rowsMatch = [&](nodegraph::NodeStatus status,
                             std::string_view resultText) {
    for (const nodegraph::NodeRef &owner : owners) {
      const auto snapshot =
          adapter.inspector(owner, ui::InspectorProjection::Agents);
      const auto *agent =
          snapshot ? pageValue<ui::InspectorAgentRow>(snapshot->agents)
                   : nullptr;
      if (!snapshot || snapshot->agents.total != 1 || !agent ||
          agent->status.semantic != status ||
          agent->resultText != resultText)
        return false;
    }
    return true;
  };
  const auto revisionsMatch = [&](std::uint64_t parentRevision,
                                  std::uint64_t childRevision) {
    auto read = graph.tryRead();
    const auto *childIndex = read ? read->inspectorIndex(child) : nullptr;
    if (!childIndex || childIndex->agentChangedRevision != childRevision)
      return false;
    return std::ranges::all_of(owners, [&](const nodegraph::NodeRef &owner) {
      const auto *index = read->inspectorIndex(owner);
      return index && index->agentChangedRevision == parentRevision;
    });
  };
  auto read = graph.tryRead();
  const auto *childIndex = read ? read->inspectorIndex(child) : nullptr;
  const std::uint64_t childRevision =
      childIndex ? childIndex->agentChangedRevision : 0;
  read.reset();
  bool result = expect(
      rowsMatch(nodegraph::NodeStatus::Running, "base child result"),
      "one shared child's initial status and result project into every exact "
      "parent row");

  std::uint64_t revision = 0;
  {
    auto write = graph.write();
    write.setStatus(child, nodegraph::NodeStatus::Completed);
    write.setField(child, "status", "completed");
    revision = write.finish().revision;
  }
  result &= expect(
      revisionsMatch(revision, childRevision) &&
          rowsMatch(nodegraph::NodeStatus::Completed, "base child result"),
      "one child status mutation invalidates every exact parent and not the "
      "child's empty Agents page");

  nodegraph::NodeRef latest;
  {
    auto write = graph.write();
    for (std::size_t index = 0; index < 3; ++index) {
      nodegraph::NodeState message;
      message.fields = {{"type", "agentMessage"}};
      if (index != 2)
        message.fields.emplace("text", index == 0 ? "first tail result"
                                                   : "second tail result");
      nodegraph::NodeRef item = write.upsert(
          {nodegraph::NodeKind::Item,
           "fanout-child-tail-" + std::to_string(index)},
          std::move(message));
      write.setParent(childTurn, item);
      latest = std::move(item);
    }
    revision = write.finish().revision;
  }
  result &= expect(
      revisionsMatch(revision, childRevision) &&
          rowsMatch(nodegraph::NodeStatus::Completed, "second tail result"),
      "one transaction indexes multiple message tails, ignores its empty "
      "tail, and invalidates every exact parent once");

  {
    auto write = graph.write();
    write.setField(latest, "text", "promoted empty tail");
    revision = write.finish().revision;
  }
  result &= expect(
      revisionsMatch(revision, childRevision) &&
          rowsMatch(nodegraph::NodeStatus::Completed, "promoted empty tail"),
      "giving an indexed empty tail text promotes it for every exact parent");
  {
    auto write = graph.write();
    write.setField(latest, "type", "plain");
    revision = write.finish().revision;
  }
  result &= expect(
      revisionsMatch(revision, childRevision) &&
          rowsMatch(nodegraph::NodeStatus::Completed, "second tail result"),
      "changing the latest message type exposes its predecessor for every "
      "exact parent");
  {
    auto write = graph.write();
    write.setField(latest, "type", "agentMessage");
    revision = write.finish().revision;
  }
  result &= expect(
      revisionsMatch(revision, childRevision) &&
          rowsMatch(nodegraph::NodeStatus::Completed, "promoted empty tail"),
      "restoring the latest message type restores it for every exact parent");
  {
    auto write = graph.write();
    write.remove(latest);
    revision = write.finish().revision;
  }
  result &= expect(
      revisionsMatch(revision, childRevision) &&
          rowsMatch(nodegraph::NodeStatus::Completed, "second tail result"),
      "removing the latest message exposes its predecessor for every exact "
      "parent without changing the child's empty Agents page");
  return result;
}

bool agentIndexTracksAuthorityInsteadOfUpdateHistory() {
  const auto agentAt = [](const std::optional<ui::InspectorSnapshot> &snapshot,
                          std::size_t row) {
    return snapshot && row < snapshot->agents.rows.size()
               ? std::get_if<ui::InspectorAgentRow>(
                     &snapshot->agents.rows[row].value)
               : nullptr;
  };
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef owner;
  nodegraph::NodeRef turn;
  nodegraph::NodeRef sourceA;
  nodegraph::NodeRef sourceB;
  {
    auto write = graph.write();
    owner = write.upsert({nodegraph::NodeKind::Thread, "indexed-owner"});
    turn = write.upsert({nodegraph::NodeKind::Turn, "indexed-turn"});
    const auto childA =
        write.upsert({nodegraph::NodeKind::Thread, "indexed-child-a"});
    const auto childB =
        write.upsert({nodegraph::NodeKind::Thread, "indexed-child-b"});
    write.setParent(owner, turn);
    sourceA = addActivity(write, turn, "indexed-source-a", "started",
                          "indexed-child-a", "inProgress", "spawn_agent");
    sourceB = addActivity(write, turn, "indexed-source-b", "started",
                          "indexed-child-b", "inProgress", "spawn_agent");
    write.relate(sourceA, nodegraph::RelationKind::AgentChildThread, childA);
    write.relate(sourceB, nodegraph::RelationKind::AgentChildThread, childB);
    static_cast<void>(write.finish());
  }
  ui::NodeGraphUiAdapter adapter(graph);
  auto snapshot = adapter.inspector(owner, ui::InspectorProjection::Agents);
  const std::uint64_t initialOrder = snapshot ? snapshot->agents.orderRevision
                                               : 0;

  nodegraph::NodeRef plain;
  {
    auto write = graph.write();
    plain = write.upsert({nodegraph::NodeKind::Item, "indexed-plain"});
    const std::array reordered{sourceB, sourceA, plain};
    write.replaceChildren(turn, reordered);
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(owner, ui::InspectorProjection::Agents);
  const auto *first = agentAt(snapshot, 0);
  const auto *second = agentAt(snapshot, 1);
  bool result = expect(
      snapshot && snapshot->agents.total == 2 && first && second &&
          first->childThreadId == "indexed-child-b" &&
          second->childThreadId == "indexed-child-a" &&
          snapshot->agents.orderRevision > initialOrder,
      "an authoritative child reorder cannot be mistaken for an Agent tail append");

  nodegraph::NodeRef sourceC;
  {
    auto write = graph.write();
    const auto childC =
        write.upsert({nodegraph::NodeKind::Thread, "indexed-child-c"});
    sourceC = addActivity(write, turn, "indexed-source-c", "started",
                          "indexed-child-c", "inProgress", "spawn_agent");
    write.relate(sourceC, nodegraph::RelationKind::AgentChildThread, childC);
    const auto newPlain =
        write.upsert({nodegraph::NodeKind::Item, "indexed-new-plain"});
    const std::array reordered{sourceB, sourceC, sourceA, plain, newPlain};
    write.replaceChildren(turn, reordered);
    static_cast<void>(write.finish());
  }
  snapshot = adapter.inspector(owner, ui::InspectorProjection::Agents);
  first = agentAt(snapshot, 0);
  second = agentAt(snapshot, 1);
  const auto *third = agentAt(snapshot, 2);
  result &= expect(
      snapshot && snapshot->agents.total == 3 && first && second && third &&
          first->childThreadId == "indexed-child-b" &&
          second->childThreadId == "indexed-child-c" &&
          third->childThreadId == "indexed-child-a",
      "a non-tail Agent insertion is indexed even when a new plain item is last");

  nodegraph::NodeGraph fanoutGraph;
  nodegraph::NodeRef fanoutOwner;
  nodegraph::NodeRef fanoutTurn;
  nodegraph::NodeRef progress;
  {
    auto write = fanoutGraph.write();
    fanoutOwner =
        write.upsert({nodegraph::NodeKind::Thread, "fanout-owner"});
    fanoutTurn = write.upsert({nodegraph::NodeKind::Turn, "fanout-turn"});
    const auto childA =
        write.upsert({nodegraph::NodeKind::Thread, "fanout-child-a"});
    const auto childB =
        write.upsert({nodegraph::NodeKind::Thread, "fanout-child-b"});
    write.setParent(fanoutOwner, fanoutTurn);
    const auto starter = addActivity(write, fanoutTurn, "fanout-starter",
                                     "started", "fanout-child-b",
                                     "inProgress", "spawn_agent");
    progress = addActivity(write, fanoutTurn, "fanout-progress", "progress",
                           "fanout-child-b", "inProgress");
    write.setField(progress, "prompt", "progress prompt");
    write.relate(starter, nodegraph::RelationKind::AgentChildThread, childB);
    write.relate(progress, nodegraph::RelationKind::AgentChildThread, childA);
    write.relate(progress, nodegraph::RelationKind::AgentChildThread, childB);
    static_cast<void>(write.finish());
  }
  ui::NodeGraphUiAdapter fanoutAdapter(fanoutGraph);
  snapshot = fanoutAdapter.inspector(fanoutOwner,
                                     ui::InspectorProjection::Agents);
  result &= expect(snapshot && snapshot->agents.total == 1,
                   "a non-creating multi-target update does not invent a row");
  {
    auto write = fanoutGraph.write();
    write.setField(progress, "kind", "started");
    static_cast<void>(write.finish());
  }
  snapshot = fanoutAdapter.inspector(fanoutOwner,
                                     ui::InspectorProjection::Agents);
  first = agentAt(snapshot, 0);
  second = agentAt(snapshot, 1);
  result &= expect(
      snapshot && snapshot->agents.total == 2 && first && second &&
          first->childThreadId == "fanout-child-b" &&
          second->childThreadId == "fanout-child-a",
      "an eligibility change rebuilds every exact target, including a previously missing row");

  nodegraph::NodeRef latest;
  {
    auto write = fanoutGraph.write();
    const auto childB =
        write.find({nodegraph::NodeKind::Thread, "fanout-child-b"});
    latest = addActivity(write, fanoutTurn, "fanout-latest", "progress",
                         "fanout-child-b", "inProgress");
    write.setField(latest, "prompt", "latest prompt");
    write.relate(latest, nodegraph::RelationKind::AgentChildThread, childB);
    static_cast<void>(write.finish());
  }
  {
    auto write = fanoutGraph.write();
    write.eraseField(latest, "prompt");
    static_cast<void>(write.finish());
  }
  snapshot = fanoutAdapter.inspector(fanoutOwner,
                                     ui::InspectorProjection::Agents);
  first = agentAt(snapshot, 0);
  result &= expect(first && first->prompt == "progress prompt",
                   "clearing the newest Agent field exposes its indexed predecessor");
  {
    auto write = fanoutGraph.write();
    write.setField(progress, "prompt", "updated predecessor");
    static_cast<void>(write.finish());
  }
  snapshot = fanoutAdapter.inspector(fanoutOwner,
                                     ui::InspectorProjection::Agents);
  first = agentAt(snapshot, 0);
  result &= expect(first && first->prompt == "updated predecessor",
                   "a historical winning contributor updates without scanning history");
  return result;
}

bool establishedAgentsWidgetContractIsRetained() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef owner;
  {
    auto write = graph.write();
    owner = write.upsert({nodegraph::NodeKind::Thread, "widget-owner"});
    const nodegraph::NodeRef turn =
        write.upsert({nodegraph::NodeKind::Turn, "widget-turn"});
    write.setParent(owner, turn);
    static_cast<void>(addActivity(write, turn, "widget-agent", "started",
                                  "widget-child", "inProgress", "spawn_agent"));
    static_cast<void>(write.finish());
  }
  ui::NodeGraphUiAdapter adapter(graph);
  const auto snapshot =
      adapter.inspector(owner, ui::InspectorProjection::Agents);
  ui::InspectorSnapshot initial = *snapshot;
  ui::InspectorAgentRow *initialAgent =
      pageValue<ui::InspectorAgentRow>(initial.agents);
  initialAgent->prompt = "Inspect the stable row";
  initialAgent->resultText =
      "## Result\n\nRetain [this link](https://example.com).";
  middle::InspectorPane pane;
  pane.resize(440, 700);
  pane.show();
  pane.refresh(initial, ui::InspectorProjection::Agents);
  QCoreApplication::processEvents();

  bool result =
      expect(pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"))
                 .empty(),
             "the hidden Agents tab constructs no Agent row widgets");
  pane.tabs()->setCurrentIndex(1);
  QCoreApplication::processEvents();
  const auto frames =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"));
  result &= expect(frames.size() == 1,
                   "activating Agents materializes the current logical row");
  if (!frames.empty()) {
    QPointer<QFrame> stableFrame = frames.front();
    auto *content = frames.front()->findChild<QWidget *>(
        QStringLiteral("agentCardContent"));
    auto *disclosure = frames.front()->findChild<QToolButton *>(
        QStringLiteral("agentDisclosureButton"));
    auto *copy = frames.front()->findChild<QToolButton *>(
        QStringLiteral("agentCopyButton"));
    auto *status =
        frames.front()->findChild<QLabel *>(QStringLiteral("agentStatus"));
    auto *resultView = frames.front()->findChild<middle::MarkdownTextView *>();
    QTextDocument *document = resultView ? resultView->document() : nullptr;
    int documentMutations = 0;
    if (document)
      QObject::connect(
          document, &QTextDocument::contentsChange, &pane,
          [&documentMutations](int, int, int) { ++documentMutations; });
    result &= expect(
        content && disclosure && copy && status && resultView && document &&
            !content->isVisible() &&
            disclosure->accessibleName() == QStringLiteral("Expand agent") &&
            disclosure->toolTip() == disclosure->accessibleName() &&
            copy->accessibleName() == QStringLiteral("Copy agent content") &&
            status->text() == QStringLiteral("running") &&
            labelUsesColor(status, UiStyle::blueText) &&
            resultView->markdownSource().contains(QStringLiteral("this link")),
        "the collapsed Agent row uses the shared accessible controls and "
        "native Markdown view");
    if (disclosure)
      disclosure->click();
    QCoreApplication::processEvents();
    if (resultView) {
      QTextCursor selection(resultView->document());
      selection.setPosition(0);
      selection.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor);
      resultView->setTextCursor(selection);
      pane.activateWindow();
      resultView->setFocus(Qt::TabFocusReason);
      QCoreApplication::processEvents();
    }
    const int selectionStart =
        resultView ? resultView->textCursor().selectionStart() : -1;
    const int selectionEnd =
        resultView ? resultView->textCursor().selectionEnd() : -1;
    result &= expect(resultView && resultView->hasFocus(),
                     "the expanded Agent Markdown view accepts keyboard focus");
    result &= expect(
        content && content->isVisible() &&
            disclosure->accessibleName() == QStringLiteral("Collapse agent"),
        "the shared disclosure expands the established Agent content");
    ui::InspectorSnapshot updated = initial;
    pageValue<ui::InspectorAgentRow>(updated.agents)->status =
        nodegraph::NodeStatus::Completed;
    pane.refresh(updated, ui::InspectorProjection::Agents);
    QCoreApplication::processEvents();
    const auto updatedFrames =
        pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"));
    auto *updatedContent = stableFrame ? stableFrame->findChild<QWidget *>(
                                             QStringLiteral("agentCardContent"))
                                       : nullptr;
    auto *updatedDisclosure = stableFrame
                                  ? stableFrame->findChild<QToolButton *>(
                                        QStringLiteral("agentDisclosureButton"))
                                  : nullptr;
    auto *updatedCopy = stableFrame ? stableFrame->findChild<QToolButton *>(
                                          QStringLiteral("agentCopyButton"))
                                    : nullptr;
    auto *updatedResult =
        stableFrame ? stableFrame->findChild<middle::MarkdownTextView *>()
                    : nullptr;
    const bool objectsRetained =
        updatedFrames.size() == 1 && updatedFrames.front() == stableFrame &&
        updatedContent == content && updatedContent->isVisible() &&
        updatedDisclosure == disclosure && updatedCopy == copy &&
        updatedResult == resultView && updatedResult->document() == document &&
        stableFrame->findChild<QLabel *>(QStringLiteral("agentStatus")) ==
            status &&
        status->text() == QStringLiteral("completed") &&
        labelUsesColor(status, UiStyle::greenText);
    result &= expect(objectsRetained,
                     "an Agent status change retains the exact expanded "
                     "control and Markdown document objects");
    result &= expect(
        objectsRetained &&
            updatedResult->textCursor().selectionStart() == selectionStart &&
            updatedResult->textCursor().selectionEnd() == selectionEnd,
        "an Agent status change retains the Markdown selection");
    result &= expect(objectsRetained && updatedResult->hasFocus(),
                     "an Agent status change retains Markdown keyboard focus");
    result &= expect(documentMutations == 0,
                     "an Agent status-only update performs no Markdown work");
    if (copy)
      copy->click();
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    result &= expect(
        mime && mime->hasText() && mime->hasFormat("text/markdown") &&
            mime->text().contains(QStringLiteral("Status: completed")) &&
            mime->text().contains(QStringLiteral("Retain [this link]")),
        "Agent Copy reads the latest retained row and preserves rich MIME");

#if QT_CONFIG(accessibility)
    const std::array noOpAccessibleIds{
        accessibleId(stableFrame), accessibleId(disclosure), accessibleId(copy),
        accessibleId(resultView)};
#endif
    const QRect noOpGeometry = stableFrame ? stableFrame->geometry() : QRect{};
    auto *agentRows = pane.findChild<QAbstractScrollArea *>(
        QStringLiteral("inspectorAgentRows"));
    const int noOpScroll =
        agentRows ? agentRows->verticalScrollBar()->value() : -1;
    pane.refresh(updated, ui::InspectorProjection::Agents);
    QCoreApplication::processEvents();
    bool noOpAccessibleIdsRetained = true;
#if QT_CONFIG(accessibility)
    const std::array retainedAccessibleIds{
        accessibleId(stableFrame), accessibleId(disclosure), accessibleId(copy),
        accessibleId(resultView)};
    noOpAccessibleIdsRetained =
        std::ranges::none_of(noOpAccessibleIds,
                            [](QAccessible::Id id) { return id == 0; }) &&
        retainedAccessibleIds == noOpAccessibleIds;
#endif
    result &= expect(stableFrame &&
                         stableFrame->findChild<middle::MarkdownTextView *>() ==
                             resultView &&
                         resultView->document() == document &&
                         stableFrame->geometry() == noOpGeometry &&
                         agentRows->verticalScrollBar()->value() == noOpScroll &&
                         noOpAccessibleIdsRetained,
                     "repeating identical Agent state replaces and moves nothing");

    QPointer<QToolButton> retiredCopy = copy;
    QPointer<middle::MarkdownTextView> retiredResult = resultView;
    ui::InspectorSnapshot reincarnated = updated;
    reincarnated.agents.rows.front().key += "-replacement";
    ++reincarnated.agents.orderRevision;
    pane.refresh(reincarnated, ui::InspectorProjection::Agents);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const auto replacementFrames =
        pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"));
    QPointer<QFrame> replacementFrame =
        replacementFrames.size() == 1 ? replacementFrames.front() : nullptr;
    auto *replacementContent =
        replacementFrame
            ? replacementFrame->findChild<QWidget *>(
                  QStringLiteral("agentCardContent"))
            : nullptr;
    auto *replacementDisclosure =
        replacementFrame
            ? replacementFrame->findChild<QToolButton *>(
                  QStringLiteral("agentDisclosureButton"))
            : nullptr;
    auto *replacementResult =
        replacementFrame
            ? replacementFrame->findChild<middle::MarkdownTextView *>()
            : nullptr;
    result &= expect(
        stableFrame.isNull() && retiredCopy.isNull() &&
            retiredResult.isNull() && replacementFrame &&
            replacementContent && !replacementContent->isVisible() &&
            replacementDisclosure &&
            replacementDisclosure->accessibleName() ==
                QStringLiteral("Expand agent") &&
            replacementResult &&
            !replacementResult->textCursor().hasSelection() &&
            !replacementResult->hasFocus() && pane.tabs()->currentIndex() == 1,
        "same-canonical Agent replacement did not retire Agent controls, "
        "documents, interaction state, and feedback while retaining the tab");

    if (replacementDisclosure)
      replacementDisclosure->click();
    QCoreApplication::processEvents();
    result &= expect(replacementContent && replacementContent->isVisible(),
                     "the replacement Agent can own expansion state");

    ui::InspectorSnapshot removed = reincarnated;
    setPageRows(removed.agents, {}, reincarnated.agents.orderRevision + 1);
    removed.agents.emptyMessage = "No agent activity for this thread.";
    removed.agents.validRetainedKeys.emplace();
    pane.refresh(removed, ui::InspectorProjection::Agents);
    QCoreApplication::processEvents();
    result &= expect(replacementFrame.isNull(),
                     "removing an Agent retires its retained object tree");
    pane.refresh(reincarnated, ui::InspectorProjection::Agents);
    QCoreApplication::processEvents();
    const auto restored =
        pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"));
    auto *restoredContent = restored.empty()
                                ? nullptr
                                : restored.front()->findChild<QWidget *>(
                                      QStringLiteral("agentCardContent"));
    result &= expect(restored.size() == 1 && restoredContent &&
                         !restoredContent->isVisible(),
                     "removed Agent expansion state is pruned before reuse");
  }
  return result;
}

bool planAndRequestUpdatesRetainUnaffectedRows() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef firstRequest;
  nodegraph::NodeRef secondRequest;
  {
    auto write = graph.write();
    firstRequest =
        write.upsert({nodegraph::NodeKind::Interaction, "request-one"});
    secondRequest =
        write.upsert({nodegraph::NodeKind::Interaction, "request-two"});
    static_cast<void>(write.finish());
  }
  ui::InspectorSnapshot snapshot;
  snapshot.threadIncarnation = 1;
  setPageRows(
      snapshot.plan,
      {{"plan:explanation",
        ui::InspectorMarkdownRow{"Stable explanation", "Plan explanation"}},
       {"plan:step:0",
        middle::PlanStepData{"first stable step",
                             nodegraph::NodeStatus::Running}},
       {"plan:step:1",
        middle::PlanStepData{"second stable step",
                             nodegraph::NodeStatus::Pending}}});
  PendingRequestDescriptor second = requestDescriptor(secondRequest);
  second.raw["command"] = "make second";
  setPageRows(snapshot.requests,
              {requestRow(requestDescriptor(firstRequest)),
               requestRow(std::move(second))});

  middle::InspectorPane pane;
  nodegraph::NodeRef reviewedRequest;
  pane.setRequestActions(
      {}, [&](const nodegraph::NodeRef &target) { reviewedRequest = target; },
      {});
  pane.resize(440, 700);
  pane.show();
  pane.refresh(snapshot, ui::InspectorProjection::Plan);
  pane.refresh(snapshot, ui::InspectorProjection::Requests);
  static_cast<void>(waitFor([&] {
    return pane
                   .findChildren<QFrame *>(
                       QStringLiteral("inspectorPlanStepFrame"))
                   .size() == 2 &&
           std::ranges::any_of(
               pane.findChildren<middle::MarkdownTextView *>(),
               [](const middle::MarkdownTextView *view) {
                 return view->accessibleName() ==
                        QStringLiteral("Plan explanation");
               });
  }));
  auto planFrames =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorPlanStepFrame"));
  QPointer<middle::MarkdownTextView> planExplanation;
  for (middle::MarkdownTextView *view :
       pane.findChildren<middle::MarkdownTextView *>())
    if (view->accessibleName() == QStringLiteral("Plan explanation"))
      planExplanation = view;
  QTextDocument *planDocument =
      planExplanation ? planExplanation->document() : nullptr;
  QFrame *firstPlan = nullptr;
  QFrame *secondPlan = nullptr;
  for (QFrame *frame : planFrames) {
    QLabel *description =
        frame->findChild<QLabel *>(QStringLiteral("planStepDescription"));
    if (description &&
        description->text() == QStringLiteral("first stable step"))
      firstPlan = frame;
    if (description &&
        description->text() == QStringLiteral("second stable step"))
      secondPlan = frame;
  }
  QLabel *firstDescription = firstPlan
                                 ? firstPlan->findChild<QLabel *>(
                                       QStringLiteral("planStepDescription"))
                                 : nullptr;
  QLabel *firstStatus =
      firstPlan
          ? firstPlan->findChild<QLabel *>(QStringLiteral("planStepStatus"))
          : nullptr;
  QLabel *secondDescription = secondPlan
                                  ? secondPlan->findChild<QLabel *>(
                                        QStringLiteral("planStepDescription"))
                                  : nullptr;
  pageValue<middle::PlanStepData>(snapshot.plan)->status =
      nodegraph::NodeStatus::Completed;
  pane.refresh(snapshot, ui::InspectorProjection::Plan);
  static_cast<void>(waitFor([&] {
    return firstStatus && firstStatus->text() == QStringLiteral("completed");
  }));
  planFrames =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorPlanStepFrame"));
  bool result = expect(
      firstDescription && firstStatus && secondDescription && planExplanation &&
          planExplanation->document() == planDocument &&
          planFrames.contains(firstPlan) && planFrames.contains(secondPlan) &&
          firstPlan->findChild<QLabel *>(
              QStringLiteral("planStepDescription")) == firstDescription &&
          firstPlan->findChild<QLabel *>(QStringLiteral("planStepStatus")) ==
              firstStatus &&
          secondPlan->findChild<QLabel *>(
              QStringLiteral("planStepDescription")) == secondDescription &&
          firstStatus->text() == QStringLiteral("completed"),
      "a Plan status update retains both rows and every stable child object");
  auto *planRows = pane.findChild<QAbstractScrollArea *>(
      QStringLiteral("inspectorPlanRows"));
  result &= expect(accessibleRowsMatch(planRows),
                   "every resident Plan row has exactly one accessibility representation");

  if (planExplanation) {
    QTextCursor selection(planExplanation->document());
    selection.setPosition(0);
    selection.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor);
    planExplanation->setTextCursor(selection);
    pane.activateWindow();
    planExplanation->setFocus(Qt::TabFocusReason);
  }
  const int selectedPlanTab = pane.tabs()->currentIndex();
  QPointer<middle::MarkdownTextView> retiredExplanation = planExplanation;
  ui::InspectorSnapshot switched = snapshot;
  switched.threadIncarnation = 2;
  pageValue<ui::InspectorMarkdownRow>(switched.plan)->text =
      "Different thread explanation";
  pane.refresh(switched, ui::InspectorProjection::Plan);
  static_cast<void>(waitFor([&] {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return retiredExplanation.isNull();
  }));
  middle::MarkdownTextView *replacementExplanation = nullptr;
  for (middle::MarkdownTextView *view :
       pane.findChildren<middle::MarkdownTextView *>())
    if (view->accessibleName() == QStringLiteral("Plan explanation"))
      replacementExplanation = view;
  result &= expect(
      retiredExplanation.isNull() && replacementExplanation &&
          replacementExplanation->document() != planDocument &&
          replacementExplanation->markdownSource() ==
              QStringLiteral("Different thread explanation") &&
          !replacementExplanation->textCursor().hasSelection() &&
          !replacementExplanation->hasFocus() &&
          pane.tabs()->currentIndex() == selectedPlanTab,
      "same-canonical thread replacement retires Plan content identity and "
      "interaction state while retaining the selected tab");
  pane.refresh(snapshot, ui::InspectorProjection::Plan);
  QCoreApplication::processEvents();

  setPageRows(snapshot.plan, {}, snapshot.plan.orderRevision + 1);
  pane.refresh(snapshot, ui::InspectorProjection::Plan);
  QCoreApplication::processEvents();
  bool preparingMessage = false;
  for (QLabel *label : pane.findChildren<QLabel *>())
    preparingMessage |=
        label->text() == QStringLiteral("Plan is being prepared.");
  result &= expect(
      pane.findChildren<QFrame *>(QStringLiteral("inspectorPlanStepFrame"))
              .isEmpty() &&
          !preparingMessage,
      "an explicitly empty structured Plan remains present and blank");

  pane.tabs()->setCurrentIndex(3);
  QCoreApplication::processEvents();
  auto requestFrames =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorRequestFrame"));
  QFrame *requestOne = nullptr;
  QFrame *requestTwo = nullptr;
  for (QFrame *frame : requestFrames) {
    QLabel *detail =
        frame->findChild<QLabel *>(QStringLiteral("pendingRequestDetail"));
    if (detail && detail->text().contains(QStringLiteral("make test")))
      requestOne = frame;
    if (detail && detail->text().contains(QStringLiteral("make second")))
      requestTwo = frame;
  }
  auto *stableAccept = requestOne ? requestOne->findChild<QPushButton *>(
                                        QStringLiteral("pendingRequestAccept"))
                                  : nullptr;
  auto *stableStatus = requestOne ? requestOne->findChild<QLabel *>(
                                        QStringLiteral("pendingRequestStatus"))
                                  : nullptr;
  QAccessibleInterface *statusAccessible =
      stableStatus ? QAccessible::queryAccessibleInterface(stableStatus)
                   : nullptr;
  const QAccessible::Id statusId =
      statusAccessible ? QAccessible::uniqueId(statusAccessible) : 0;
  QAccessibleInterface *requestAccessible =
      requestOne ? QAccessible::queryAccessibleInterface(requestOne) : nullptr;
  const QAccessible::Id requestId =
      requestAccessible ? QAccessible::uniqueId(requestAccessible) : 0;
  QAccessibleInterface *acceptAccessible =
      stableAccept ? QAccessible::queryAccessibleInterface(stableAccept)
                   : nullptr;
  const QAccessible::Id acceptId =
      acceptAccessible ? QAccessible::uniqueId(acceptAccessible) : 0;
  tests::AccessibilityEventProbe events;
  auto *requestRows = pane.findChild<QAbstractScrollArea *>(
      QStringLiteral("inspectorRequestRows"));
  bool requestAccessibility = accessibleRowsMatch(requestRows);
  for (QFrame *frame : requestFrames) {
    QAccessibleInterface *frameInterface =
        QAccessible::queryAccessibleInterface(frame);
    for (QPushButton *button : frame->findChildren<QPushButton *>())
      if (button->isVisible())
        requestAccessibility &=
            accessibleObjectCount(frameInterface, button) == 1;
  }
  result &= expect(
      requestAccessibility,
      "every resident Request row and visible action has exactly one accessibility representation");
  const QRect requestNoOpGeometry = requestOne ? requestOne->geometry() : QRect{};
  const int requestNoOpScroll =
      requestRows ? requestRows->verticalScrollBar()->value() : -1;
  events.clear();
  pageValue<PendingRequestDescriptor>(snapshot.requests)->responseRevision++;
  pane.refresh(snapshot, ui::InspectorProjection::Requests);
  QCoreApplication::processEvents();
  result &= expect(requestOne && requestOne->geometry() == requestNoOpGeometry &&
                       requestRows->verticalScrollBar()->value() ==
                           requestNoOpScroll &&
                       requestOne->findChild<QPushButton *>(
                           QStringLiteral("pendingRequestAccept")) ==
                           stableAccept &&
                       events.events(statusId).isEmpty() &&
                       events.events(requestId).isEmpty() &&
                       events.events(acceptId).isEmpty(),
                   "a nonvisual Request revision moves and replaces nothing");
  events.clear();
  pageValue<PendingRequestDescriptor>(snapshot.requests)->availability =
      PendingRequestAvailability::Unavailable;
  pane.refresh(snapshot, ui::InspectorProjection::Requests);
  QCoreApplication::processEvents();
  requestFrames =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorRequestFrame"));
  result &= expect(
      requestFrames.contains(requestOne) &&
          requestFrames.contains(requestTwo) &&
          requestOne->findChild<QPushButton *>(
              QStringLiteral("pendingRequestAccept")) == stableAccept &&
          stableAccept && !stableAccept->isEnabled() && statusAccessible &&
          events.events(statusId, QAccessible::NameChanged).size() == 1 &&
          events.events(requestId, QAccessible::DescriptionChanged).size() ==
              1 &&
          events.events(acceptId, QAccessible::StateChanged).size() == 1 &&
          events.events(acceptId, QAccessible::StateChanged)
              .front()
              .changedStates.disabled &&
          events.events(acceptId, QAccessible::StateChanged)
              .front()
              .state.disabled &&
          statusAccessible->text(QAccessible::Name) == stableStatus->text(),
      "a Request state update retains both stable request rows without a "
      "whole-tab rebuild and emits one final-name event");
  events.clear();
  pageValue<PendingRequestDescriptor>(snapshot.requests)->availability =
      PendingRequestAvailability::Actionable;
  pane.refresh(snapshot, ui::InspectorProjection::Requests);
  QCoreApplication::processEvents();
  const auto acceptEnabled =
      events.events(acceptId, QAccessible::StateChanged);
  result &= expect(
      requestFrames.contains(requestOne) && stableAccept &&
          stableAccept->isEnabled() &&
          events.events(statusId, QAccessible::NameChanged).size() == 1 &&
          events.events(requestId, QAccessible::DescriptionChanged).size() ==
              1 &&
          acceptEnabled.size() == 1 &&
          acceptEnabled.front().changedStates.disabled &&
          !acceptEnabled.front().state.disabled,
      "restoring Request availability emits one reverse semantic transition "
      "without replacing the row or action");

  std::reverse(snapshot.requests.rows.begin(), snapshot.requests.rows.end());
  ++snapshot.requests.orderRevision;
  pane.refresh(snapshot, ui::InspectorProjection::Requests);
  QCoreApplication::processEvents();
  result &=
      expect(requestOne && requestTwo && requestOne->parentWidget() &&
                 requestOne->parentWidget() == requestTwo->parentWidget() &&
                 requestTwo->geometry().top() < requestOne->geometry().top(),
             "a Request reorder moves the stable rows into snapshot order");
  std::reverse(snapshot.requests.rows.begin(), snapshot.requests.rows.end());
  ++snapshot.requests.orderRevision;

  nodegraph::NodeRef replacement;
  {
    auto write = graph.write();
    write.remove(firstRequest);
    static_cast<void>(write.finish());
  }
  {
    auto write = graph.write();
    replacement =
        write.upsert({nodegraph::NodeKind::Interaction, "request-one"});
    static_cast<void>(write.finish());
  }
  snapshot.requests.rows.front() = requestRow(requestDescriptor(replacement));
  ++snapshot.requests.orderRevision;
  QPointer<QFrame> retiredRequest = requestOne;
  pane.refresh(snapshot, ui::InspectorProjection::Requests);
  QCoreApplication::processEvents();
  QFrame *replacementFrame = nullptr;
  for (QFrame *frame :
       pane.findChildren<QFrame *>(QStringLiteral("inspectorRequestFrame")))
    if (QLabel *detail =
            frame->findChild<QLabel *>(QStringLiteral("pendingRequestDetail"));
        detail && detail->text().contains(QStringLiteral("make test")))
      replacementFrame = frame;
  auto *accept = replacementFrame ? replacementFrame->findChild<QPushButton *>(
                                        QStringLiteral("pendingRequestAccept"))
                                  : nullptr;
  if (accept)
    accept->click();
  result &= expect(
      replacement != firstRequest && retiredRequest.isNull() &&
          replacementFrame && reviewedRequest == replacement,
      "a reused canonical request identity rebuilds the row and targets the "
      "exact new Interaction");
  return result;
}

bool partialProjectionRetiresTheWholeThreadPresentation() {
  ui::InspectorSnapshot first;
  first.threadIncarnation = 41;
  setPageRows(first.plan,
              {{"plan:first:explanation",
                ui::InspectorMarkdownRow{"First explanation",
                                         "Plan explanation"}},
               {"plan:first:step",
                middle::PlanStepData{"first step",
                                     nodegraph::NodeStatus::Running}}});
  ui::InspectorAgentRow firstAgent;
  firstAgent.agentPath = "root/first-agent";
  firstAgent.resultText = "First result";
  setPageRows(first.agents, {agentRow("stable-agent", firstAgent)});

  middle::InspectorPane pane;
  pane.resize(440, 700);
  pane.show();
  pane.refresh(first, ui::InspectorProjection::Plan);
  pane.refresh(first, ui::InspectorProjection::Agents);
  QCoreApplication::processEvents();
  QPointer<middle::MarkdownTextView> firstPlan;
  for (middle::MarkdownTextView *view :
       pane.findChildren<middle::MarkdownTextView *>())
    if (view->accessibleName() == QStringLiteral("Plan explanation"))
      firstPlan = view;
  const bool firstPlanMaterialized = firstPlan;
  pane.tabs()->setCurrentIndex(1);
  QCoreApplication::processEvents();
  const auto firstAgentFrames =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"));
  QPointer<QFrame> firstAgentFrame =
      firstAgentFrames.size() == 1 ? firstAgentFrames.front() : nullptr;
  const bool firstPagesMaterialized =
      firstPlanMaterialized && firstPlan.isNull() && firstAgentFrame;
  pane.tabs()->setCurrentIndex(0);
  QCoreApplication::processEvents();
  QPointer<middle::MarkdownTextView> rematerializedPlan;
  for (middle::MarkdownTextView *view :
       pane.findChildren<middle::MarkdownTextView *>())
    if (view->accessibleName() == QStringLiteral("Plan explanation"))
      rematerializedPlan = view;

  ui::InspectorSnapshot secondAgents;
  secondAgents.threadIncarnation = 42;
  ui::InspectorAgentRow secondAgent = firstAgent;
  secondAgent.agentPath = "root/second-agent";
  secondAgent.resultText = "Second result";
  setPageRows(secondAgents.agents,
              {agentRow("stable-agent", std::move(secondAgent))});
  pane.refresh(secondAgents, ui::InspectorProjection::Agents);
  QCoreApplication::processEvents();
  const bool selectedTabRetained = pane.tabs()->currentIndex() == 0;
  const bool replacementStayedLazy =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"))
          .isEmpty();
  pane.tabs()->setCurrentIndex(1);
  QCoreApplication::processEvents();
  const auto replacementFrames =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"));
  QFrame *replacement =
      replacementFrames.size() == 1 ? replacementFrames.front() : nullptr;
  auto *replacementContent =
      replacement
          ? replacement->findChild<QWidget *>(QStringLiteral("agentCardContent"))
          : nullptr;
  return expect(firstPagesMaterialized,
                "both first-incarnation Inspector pages materialized") &&
         expect(rematerializedPlan.isNull() && firstAgentFrame.isNull(),
                "an Agents-only replacement retired the hidden Plan and "
                "Agent object trees together") &&
         expect(selectedTabRetained && replacementStayedLazy,
                "the hidden replacement stayed lazy without changing the "
                "selected Inspector tab") &&
         expect(replacement && replacementContent &&
                    !replacementContent->isVisible(),
                "activating Agents materialized only the fresh collapsed "
                "replacement");
}

bool hiddenRequestProjectionRetiresExactTargets() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef first;
  {
    auto write = graph.write();
    first = write.upsert({nodegraph::NodeKind::Interaction, "stable-request"});
    static_cast<void>(write.finish());
  }
  middle::InspectorPane pane;
  nodegraph::NodeRef accepted;
  pane.setRequestActions(
      {}, [&](const nodegraph::NodeRef &target) { accepted = target; }, {});
  pane.resize(440, 700);
  pane.show();
  ui::InspectorSnapshot firstRequests;
  firstRequests.threadIncarnation = 7;
  setPageRows(firstRequests.requests,
              {requestRow(requestDescriptor(first))});
  pane.refresh(firstRequests, ui::InspectorProjection::Requests);
  pane.tabs()->setCurrentIndex(3);
  QCoreApplication::processEvents();
  const auto firstFrames =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorRequestFrame"));
  QPointer<QFrame> firstFrame =
      firstFrames.size() == 1 ? firstFrames.front() : nullptr;
  const bool firstRequestMaterialized = firstFrame;
  pane.tabs()->setCurrentIndex(0);
  QCoreApplication::processEvents();

  {
    auto write = graph.write();
    write.remove(first);
    static_cast<void>(write.finish());
  }
  nodegraph::NodeRef second;
  {
    auto write = graph.write();
    second =
        write.upsert({nodegraph::NodeKind::Interaction, "stable-request"});
    static_cast<void>(write.finish());
  }
  ui::InspectorSnapshot secondRequests;
  secondRequests.threadIncarnation = 7;
  setPageRows(secondRequests.requests,
              {requestRow(requestDescriptor(second))}, 2);
  pane.refresh(secondRequests, ui::InspectorProjection::Requests);
  QCoreApplication::processEvents();
  const auto hiddenFrames =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorRequestFrame"));
  const bool hiddenReplacementStayedLazy = hiddenFrames.empty();
  pane.tabs()->setCurrentIndex(3);
  QCoreApplication::processEvents();
  const auto secondFrames =
      pane.findChildren<QFrame *>(QStringLiteral("inspectorRequestFrame"));
  QFrame *secondFrame =
      secondFrames.size() == 1 ? secondFrames.front() : nullptr;
  auto *accept = secondFrame
                     ? secondFrame->findChild<QPushButton *>(
                           QStringLiteral("pendingRequestAccept"))
                     : nullptr;
  if (accept)
    accept->click();
  return expect(firstRequestMaterialized && first != second &&
                    firstFrame.isNull() && hiddenReplacementStayedLazy,
                "a hidden Request refresh retired the exact old object tree") &&
         expect(secondFrame,
                "activating Requests materialized only its latest snapshot") &&
         expect(accepted == second,
                "the hidden replacement Request action targets only its exact "
                "Interaction");
}

bool changesTabShowsCanonicalThreadRepositoryChanges() {
  QTemporaryDir repositoryDirectory;
  if (!expect(repositoryDirectory.isValid(),
              "creates a repository for the Inspector Changes proof"))
    return false;

  git_repository *repository = nullptr;
  if (!expect(git_repository_init(
                  &repository, repositoryDirectory.path().toUtf8().constData(),
                  0) == 0,
              "initializes the Inspector Changes proof repository"))
    return false;

  const QString changedPath =
      repositoryDirectory.filePath(QStringLiteral("changed.txt"));
  QFile changedFile(changedPath);
  const QByteArray contents("visible Inspector change\n");
  const bool wrote = changedFile.open(QIODevice::WriteOnly) &&
                     changedFile.write(contents) == contents.size();
  changedFile.close();
  if (!expect(wrote, "creates the real untracked Inspector change")) {
    git_repository_free(repository);
    return false;
  }

  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  const QString parentWorkspace =
      QFileInfo(repositoryDirectory.path()).absolutePath();
  {
    auto write = graph.write();
    nodegraph::NodeState threadState;
    threadState.fields = {
        {"id", nodegraph::Value("changes-thread")},
        {"cwd", nodegraph::Value(parentWorkspace.toStdString())}};
    thread = write.upsert({nodegraph::NodeKind::Thread, "changes-thread"},
                          std::move(threadState));
    const nodegraph::NodeRef turn =
        write.upsert({nodegraph::NodeKind::Turn, "changes-turn"});
    nodegraph::NodeState fileChanges;
    fileChanges.fields = {
        {"type", nodegraph::Value("fileChange")},
        {"cwd", nodegraph::Value(repositoryDirectory.path().toStdString())},
        {"changes",
         nodegraph::Value(nodegraph::Value::Array{nodegraph::Value(
             nodegraph::Value::Object{{"path", nodegraph::Value("changed.txt")},
                                      {"kind", nodegraph::Value("add")}})})}};
    const nodegraph::NodeRef item = write.upsert(
        {nodegraph::NodeKind::Item, "changes-item"}, std::move(fileChanges));
    write.setParent(thread, turn);
    write.setParent(turn, item);
    static_cast<void>(write.finish());
  }

  ui::NodeGraphUiAdapter adapter(graph);
  const auto snapshot =
      adapter.inspector(thread, ui::InspectorProjection::Changes);
  bool result = expect(
      snapshot && snapshot->changes.cwd == parentWorkspace.toStdString() &&
          snapshot->changes.commandCwds ==
              std::vector<std::string>{
                  repositoryDirectory.path().toStdString()} &&
          snapshot->changes.changedPaths ==
              std::vector<std::string>{"changed.txt"},
      "Changes projection retains the parent workspace, item repository, and "
      "changed path");

  middle::InspectorPane pane;
  pane.resize(520, 720);
  pane.show();
  pane.tabs()->setCurrentIndex(2);
  if (snapshot)
    pane.refresh(*snapshot, ui::InspectorProjection::Changes);
  auto *files = pane.findChild<QListWidget *>(QStringLiteral("codexDiffFiles"));
  const bool visible = waitFor([&] {
    return files && files->count() == 1 &&
           files->item(0)->text().contains(QStringLiteral("changed.txt"));
  });
  result &=
      expect(visible,
             "the visible Inspector Changes tab renders the real changed file");
  git_repository_free(repository);
  return result;
}

bool stateAndProtocolRemainUsefulBoundedAndRedacted() {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef thread;
  {
    auto write = graph.write();
    nodegraph::NodeState state;
    state.status = nodegraph::NodeStatus::Running;
    state.fields = {{"name", nodegraph::Value("Visible thread")},
                    {"prompt", nodegraph::Value("must-not-leak")},
                    {"status", nodegraph::Value("running")}};
    thread = write.upsert({nodegraph::NodeKind::Thread, "state-thread"},
                          std::move(state));
    static_cast<void>(write.finish());
  }
  ui::NodeGraphUiAdapter adapter(graph);
  const auto snapshot =
      adapter.inspector(thread, ui::InspectorProjection::State);
  bool result = expect(snapshot &&
                           snapshot->state.state.dump().find("must-not-leak") ==
                               std::string::npos &&
                           snapshot->state.state.dump().find("<redacted>") !=
                               std::string::npos,
                       "State preserves useful graph metadata without secrets");

  middle::InspectorPane pane;
  pane.resize(440, 700);
  pane.refresh(*snapshot, ui::InspectorProjection::State);
  nodegraph::ProtocolDiagnostic diagnostic;
  diagnostic.details = {{"sequence", nodegraph::Value(std::uint64_t{7})},
                        {"direction", nodegraph::Value("server notification")},
                        {"subject", nodegraph::Value("item/completed")},
                        {"authority", nodegraph::Value("merge")},
                        {"threadId", nodegraph::Value("state-thread")},
                        {"correlation", nodegraph::Value("corr-7")},
                        {"error", nodegraph::Value("Bearer secret-value")}};
  static_cast<void>(pane.appendProtocolDiagnostic(diagnostic));
  auto *protocol =
      pane.findChild<QPlainTextEdit *>(QStringLiteral("protocolInfoLog"));
  result &= expect(protocol && protocol->toPlainText().isEmpty(),
                   "a hidden Inspector diagnostic performs no QWidget update");

  pane.show();
  pane.tabs()->setCurrentIndex(4);
  auto *protocolChoice =
      pane.findChild<QPushButton *>(QStringLiteral("protocolInfoChoice"));
  if (protocolChoice)
    protocolChoice->click();
  QCoreApplication::processEvents();
  result &= expect(
      pane.currentProjection() == ui::InspectorProjection::Protocol &&
          protocol &&
          protocol->toPlainText().contains(QStringLiteral("item/completed")) &&
          protocol->toPlainText().contains(QStringLiteral("corr-7")) &&
          protocol->toPlainText().contains(
              QStringLiteral("<redacted sensitive text>")) &&
          !protocol->toPlainText().contains(QStringLiteral("secret-value")),
      "Protocol retains bounded metadata and redacts sensitive text");

  auto *stateChoice =
      pane.findChild<QPushButton *>(QStringLiteral("stateInfoChoice"));
  QPushButton *protocolBack = nullptr;
  for (QPushButton *button : pane.findChildren<QPushButton *>())
    if (button->text().contains(QStringLiteral("Info"))) {
      protocolBack = button;
      break;
    }
  if (protocolBack)
    protocolBack->click();
  if (stateChoice)
    stateChoice->click();
  QCoreApplication::processEvents();
  auto *state =
      pane.findChild<QPlainTextEdit *>(QStringLiteral("stateInfoView"));
  result &= expect(
      pane.currentProjection() == ui::InspectorProjection::State && state &&
          state->toPlainText().contains(QStringLiteral("sharedNodeGraph")) &&
          state->toPlainText().contains(QStringLiteral("Visible thread")) &&
          !state->toPlainText().contains(QStringLiteral("must-not-leak")),
      "the visible State page retains graph inspection content");
  return result;
}

bool inspectorResidencyPerformance(std::size_t rowCount) {
  nodegraph::NodeGraph graph;
  nodegraph::NodeRef connection;
  nodegraph::NodeRef thread;
  std::vector<nodegraph::NodeRef> agentItems;
  agentItems.reserve(rowCount);
  {
    auto write = graph.write();
    nodegraph::NodeState connectionState;
    connectionState.status = nodegraph::NodeStatus::Connected;
    connectionState.fields = {{"role", "controller"},
                              {"providerState", "ready"}};
    connection = write.upsert({nodegraph::NodeKind::Connection, "connection"},
                              std::move(connectionState));
    const nodegraph::NodeRef runtime =
        write.upsert({nodegraph::NodeKind::Runtime, "runtime"});
    thread = write.upsert({nodegraph::NodeKind::Thread, "residency-thread"});
    nodegraph::Value::Array steps;
    steps.reserve(rowCount);
    for (std::size_t index = 0; index < rowCount; ++index)
      steps.emplace_back(nodegraph::Value::Object{
          {"step", "plan step " + std::to_string(index)},
          {"status", index % 3 == 0 ? "completed" : "pending"}});
    nodegraph::NodeState turnState;
    turnState.status = nodegraph::NodeStatus::Running;
    turnState.fields = {
        {"plan", std::move(steps)},
        {"planExplanation",
         "## Residency\n\nThe authoritative Plan renderer stays interactive."}};
    const nodegraph::NodeRef turn = write.upsert(
        {nodegraph::NodeKind::Turn, "residency-turn"}, std::move(turnState));
    write.setParent(thread, turn);

    std::vector<nodegraph::NodeRef> requests;
    requests.reserve(rowCount);
    for (std::size_t index = 0; index < rowCount; ++index) {
      nodegraph::NodeState agent;
      agent.status = nodegraph::NodeStatus::Running;
      agent.fields = {
          {"type", "subAgentActivity"},
          {"kind", "started"},
          {"agentThreadId", "residency-agent-" + std::to_string(index)},
          {"agentPath", "root/agent-" + std::to_string(index)},
          {"prompt", "Inspect row " + std::to_string(index)},
          {"resultText", "**Result " + std::to_string(index) + "**"}};
      nodegraph::NodeRef item = write.upsert(
          {nodegraph::NodeKind::Item,
           "residency-agent-item-" + std::to_string(index)},
          std::move(agent));
      write.setParent(turn, item);
      agentItems.push_back(std::move(item));

      nodegraph::NodeState request;
      request.status = nodegraph::NodeStatus::Pending;
      request.fields = {
          {"method", "item/commandExecution/requestApproval"},
          {"payload",
           nodegraph::Value::Object{
               {"threadId", "residency-thread"},
               {"command", "command-" + std::to_string(index)}}}};
      nodegraph::NodeRef interaction = write.upsert(
          {nodegraph::NodeKind::Interaction,
           "residency-request-" + std::to_string(index)},
          std::move(request));
      write.relate(interaction, nodegraph::RelationKind::InteractionTarget,
                   thread);
      requests.push_back(std::move(interaction));
    }
    write.replaceRelated(runtime, nodegraph::RelationKind::PendingInteraction,
                         std::move(requests));
    static_cast<void>(write.finish());
  }

  AdapterInspectorData data(graph, thread,
                            {rowCount + 1, rowCount, rowCount});

  middle::InspectorPane pane;
  InspectorWorkProbe work;
  InspectorDocumentTracker documents;
  pane.resize(440, 720);
  work.watch(pane);
  qApp->installEventFilter(&work);
  work.resetSample();
  pane.show();
  QElapsedTimer timer;
  timer.start();
  data.bind(pane);
  QCoreApplication::processEvents();
  const qint64 planMicros = timer.nsecsElapsed() / 1000;

  auto *plan = pane.findChild<QAbstractScrollArea *>(
      QStringLiteral("inspectorPlanRows"));
  auto *agents = pane.findChild<QAbstractScrollArea *>(
      QStringLiteral("inspectorAgentRows"));
  auto *requests = pane.findChild<QAbstractScrollArea *>(
      QStringLiteral("inspectorRequestRows"));
  const auto rowCountFor = [&pane](const QString &name) {
    return pane.findChildren<QFrame *>(name).size();
  };
  const auto residentCount = [](QAbstractScrollArea *view) {
    return view ? view->viewport()
                      ->findChildren<QWidget *>(QString{},
                                                Qt::FindDirectChildrenOnly)
                      .size()
                : qsizetype{0};
  };
  qsizetype planPeak = 0;
  qsizetype agentPeak = 0;
  qsizetype requestPeak = 0;
  qsizetype documentPeak = 0;
  qsizetype widgetPeak = 0;
  const auto recordPeaks = [&] {
    planPeak = std::max(planPeak, residentCount(plan));
    agentPeak = std::max(agentPeak, residentCount(agents));
    requestPeak = std::max(requestPeak, residentCount(requests));
    documentPeak = std::max(
        documentPeak,
        pane.findChildren<middle::MarkdownTextView *>().size());
    widgetPeak = std::max(widgetPeak, pane.findChildren<QWidget *>().size());
  };
  std::size_t maximumPageDemands = 0;
  std::size_t responsesAtSampleStart = 0;
  const auto beginWork = [&] {
    responsesAtSampleStart = data.responseCount;
    work.resetSample();
  };
  const auto recordWork = [&] {
    recordPeaks();
    documents.observe(pane);
    work.finishSample();
    maximumPageDemands =
        std::max(maximumPageDemands,
                 data.responseCount - responsesAtSampleStart);
  };
  const auto reachesBottom = [](QAbstractScrollArea *view,
                                const QString &rowName,
                                const QString &labelName,
                                const QString &expected) {
    if (!view)
      return false;
    view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
    return waitFor([&] {
      for (QFrame *row : view->findChildren<QFrame *>(rowName)) {
        QLabel *label = row->findChild<QLabel *>(labelName);
        if (label && label->text().contains(expected) && !row->isHidden() &&
            row->geometry().intersects(view->viewport()->rect()))
          return row->geometry().bottom() >= view->viewport()->height() - 16;
      }
      return false;
    });
  };
  const auto settleResidency = [&] {
    std::size_t stablePasses = 0;
    std::size_t responses = data.responseCount;
    std::size_t constructions = work.constructions;
    std::size_t retirements = work.retirements;
    QElapsedTimer elapsed;
    elapsed.start();
    while (stablePasses < 3 && elapsed.elapsed() < 3000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      if (responses == data.responseCount &&
          constructions == work.constructions &&
          retirements == work.retirements) {
        ++stablePasses;
      } else {
        responses = data.responseCount;
        constructions = work.constructions;
        retirements = work.retirements;
        stablePasses = 0;
      }
    }
    return stablePasses == 3;
  };

  bool result = expect(plan && agents && requests,
                       "the three Inspector projections share row viewports");
  recordWork();
  result &= expect(planPeak <= ui::MaximumInspectorRows &&
                       rowCountFor(QStringLiteral("inspectorAgentFrame")) == 0 &&
                       rowCountFor(QStringLiteral("inspectorRequestFrame")) == 0,
                   "only the active Plan viewport owns a bounded widget set");
  result &= expect(
      QAccessible::queryAccessibleInterface(plan) &&
          plan->accessibleName() == QStringLiteral("Plan") &&
          plan->viewport()->accessibleName().isEmpty(),
      "the active virtualized Plan viewport has one accessible representation");
  beginWork();
  timer.restart();
  const bool planBottom = reachesBottom(
      plan, QStringLiteral("inspectorPlanStepFrame"),
      QStringLiteral("planStepDescription"),
      QStringLiteral("plan step %1").arg(rowCount - 1));
  const qint64 planScrollMicros = timer.nsecsElapsed() / 1000;
  recordWork();
  beginWork();
  pane.resize(440, 2300);
  QCoreApplication::processEvents();
  recordWork();
  result &= expect(
      plan && plan->viewport()->height() >= 2160 && planPeak <= 50 &&
          reachesBottom(plan, QStringLiteral("inspectorPlanStepFrame"),
                        QStringLiteral("planStepDescription"),
                        QStringLiteral("plan step %1").arg(rowCount - 1)),
      "a 2160-logical-pixel viewport remains covered by at most 50 Plan renderers");

  beginWork();
  timer.restart();
  pane.tabs()->setCurrentIndex(1);
  QCoreApplication::processEvents();
  const qint64 agentMicros = timer.nsecsElapsed() / 1000;
  recordWork();
  result &= expect(rowCountFor(QStringLiteral("inspectorPlanStepFrame")) == 0 &&
                       rowCountFor(QStringLiteral("inspectorAgentFrame")) <= 50 &&
                       pane.findChildren<middle::MarkdownTextView *>().size() <=
                           50,
                   "Agents release the hidden Plan and bound widgets and documents");
  beginWork();
  timer.restart();
  const bool agentBottom = reachesBottom(
      agents, QStringLiteral("inspectorAgentFrame"),
      QStringLiteral("agentName"),
      QStringLiteral("agent-%1").arg(rowCount - 1));
  const qint64 agentScrollMicros = timer.nsecsElapsed() / 1000;
  recordWork();
  const bool agentResidencySettled = settleResidency();
  recordWork();
  result &= expect(agentResidencySettled,
                   "Agent residency reaches an observable quiescent boundary");

  QFrame *lastAgentFrame = nullptr;
  for (QFrame *frame :
       pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"))) {
    QLabel *name = frame->findChild<QLabel *>(QStringLiteral("agentName"));
    if (name && name->text() ==
                    QStringLiteral("agent-%1").arg(rowCount - 1)) {
      lastAgentFrame = frame;
      break;
    }
  }
  auto *lastAgentMarkdown =
      lastAgentFrame
          ? lastAgentFrame->findChild<middle::MarkdownTextView *>()
          : nullptr;
  QTextDocument *lastAgentDocument =
      lastAgentMarkdown ? lastAgentMarkdown->document() : nullptr;
  const std::size_t constructionsBeforeUpdate = work.constructions;
  const std::size_t documentsBeforeUpdate = documents.constructions;
  const std::size_t parsesBeforeUpdate = documents.parseInputs;
  const std::size_t mutationsBeforeUpdate = documents.mutations;
  const std::size_t measurementsBeforeUpdate = documents.measurements;
  beginWork();
  if (!agentItems.empty()) {
    auto write = graph.write();
    write.setField(agentItems.back(), "resultText",
                   "**Updated result " + std::to_string(rowCount - 1) + "**");
    static_cast<void>(write.finish());
    data.present(pane, ui::InspectorProjection::Agents);
    QCoreApplication::processEvents();
  }
  recordWork();
  const std::size_t updateRowConstructions =
      work.constructions - constructionsBeforeUpdate;
  const std::size_t updateDocumentConstructions =
      documents.constructions - documentsBeforeUpdate;
  const std::size_t updateParseInputs =
      documents.parseInputs - parsesBeforeUpdate;
  const std::size_t updateDocumentMutations =
      documents.mutations - mutationsBeforeUpdate;
  const std::size_t updateDocumentMeasurements =
      documents.measurements - measurementsBeforeUpdate;
  const std::size_t updateLayoutRequests = work.sampleLayoutRequests;
  const std::size_t updatePaints = work.samplePaints;
  const bool retainedAgentRenderer =
      lastAgentFrame && lastAgentMarkdown && agents &&
      lastAgentFrame->parentWidget() == agents->viewport() &&
      !lastAgentFrame->isHidden() &&
      lastAgentFrame->findChild<middle::MarkdownTextView *>() ==
          lastAgentMarkdown &&
      lastAgentMarkdown->document() == lastAgentDocument;
  result &= expect(
      retainedAgentRenderer &&
          updateRowConstructions == 0 && updateDocumentConstructions == 0,
      "one visible Agent result update retains its renderer and document");
  result &= expect(
      updateParseInputs == 1 && updateDocumentMutations > 0 &&
          updateDocumentMutations <= 16 && updateDocumentMeasurements <= 16 &&
          updateLayoutRequests <= 64 && updatePaints <= 64,
      "one visible Agent result update performs bounded Markdown and frame work");

  beginWork();
  agents->verticalScrollBar()->setValue(agents->verticalScrollBar()->maximum() /
                                        2);
  QCoreApplication::processEvents();
  recordWork();
  const int agentAnchor = agents->verticalScrollBar()->value();

  beginWork();
  timer.restart();
  pane.tabs()->setCurrentIndex(3);
  QCoreApplication::processEvents();
  const qint64 requestMicros = timer.nsecsElapsed() / 1000;
  recordWork();
  result &= expect(rowCountFor(QStringLiteral("inspectorAgentFrame")) == 0 &&
                       rowCountFor(QStringLiteral("inspectorRequestFrame")) <=
                           50 &&
                       pane.findChildren<middle::MarkdownTextView *>().empty(),
                   "Requests release hidden Agent widgets and Markdown documents");
  beginWork();
  timer.restart();
  const bool requestBottom = reachesBottom(
      requests, QStringLiteral("inspectorRequestFrame"),
      QStringLiteral("pendingRequestDetail"),
      QStringLiteral("command-%1").arg(rowCount - 1));
  const qint64 requestScrollMicros = timer.nsecsElapsed() / 1000;
  recordWork();
  beginWork();
  {
    auto write = graph.write();
    write.setField(connection, "role", "observer");
    static_cast<void>(write.finish());
  }
  std::size_t keyboardDemands = 0;
  pane.setRefreshRequestedAction([&] { ++keyboardDemands; });
  requests->verticalScrollBar()->setValue(0);
  data.present(pane, ui::InspectorProjection::Requests);
  keyboardDemands = 0;
  requests->setFocus(Qt::TabFocusReason);
  QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
  const bool tabDelivered = QApplication::sendEvent(requests, &tab);
  QCoreApplication::processEvents();
  recordWork();
  result &= expect(
      tabDelivered && keyboardDemands == 0 &&
          requests->verticalScrollBar()->value() == 0 &&
          QApplication::focusWidget() != requests &&
          !requests->isAncestorOf(QApplication::focusWidget()),
      "Tab skips an unavailable Request page without scrolling or paging");
  beginWork();
  data.bind(pane);
  pane.tabs()->setCurrentIndex(1);
  QCoreApplication::processEvents();
  recordWork();
  result &= expect(agents->verticalScrollBar()->value() == agentAnchor,
                   "an inactive Inspector tab retains its semantic scroll anchor");
  result &= expect(planBottom && agentBottom && requestBottom,
                   "Plan, Agent, and Request final rows remain reachable without a blank viewport");

  const std::size_t uiResponseCount = data.responseCount;
  constexpr std::array projections{ui::InspectorProjection::Plan,
                                   ui::InspectorProjection::Agents,
                                   ui::InspectorProjection::Requests};
  std::array<qint64, 3> directBatchMicros{};
  bool directPagesExact = true;
  for (std::size_t projection = 0; projection < projections.size();
       ++projection) {
    const std::size_t total =
        projection == 0 ? rowCount + 1 : rowCount;
    QElapsedTimer batch;
    batch.start();
    for (std::size_t sample = 0; sample < 32; ++sample) {
      const std::size_t first =
          total > ui::MaximumInspectorRows
              ? (total - ui::MaximumInspectorRows) * sample / 31
              : 0;
      ui::InspectorRowRequest request;
      request.first = first;
      const auto page = data.page(projections[projection], request);
      directPagesExact &=
          page && page->total == total && page->first == first &&
          page->rows.size() ==
              std::min(ui::MaximumInspectorRows, total - first) &&
          std::ranges::all_of(page->rows, [](const ui::InspectorRow &row) {
            return !row.key.empty();
          });
    }
    directBatchMicros[projection] = batch.nsecsElapsed() / 1000;
  }
  QElapsedTimer pendingBatch;
  pendingBatch.start();
  bool pendingPagesExact = true;
  for (std::size_t sample = 0; sample < 32; ++sample) {
    ui::InspectorRowRequest request;
    request.first =
        rowCount > ui::MaximumInspectorRows
            ? (rowCount - ui::MaximumInspectorRows) * sample / 31
            : 0;
    const auto page = data.adapter.pendingRequests(request);
    pendingPagesExact &=
        page && page->total == rowCount && page->first == request.first &&
        page->rows.size() ==
            std::min(ui::MaximumInspectorRows, rowCount - request.first);
  }
  const qint64 pendingBatchMicros = pendingBatch.nsecsElapsed() / 1000;

  constexpr qint64 MaximumInitialMicros = 2'000'000;
  constexpr qint64 MaximumSwitchMicros = 1'000'000;
  constexpr qint64 MaximumScrollMicros = 750'000;
  constexpr qint64 MaximumProjectionResponseMicros = 200'000;
  constexpr qint64 MaximumProjectionBatchMicros = 1'000'000;
  result &= expect(planMicros <= MaximumInitialMicros &&
                       agentMicros <= MaximumSwitchMicros &&
                       requestMicros <= MaximumSwitchMicros &&
                       planScrollMicros <= MaximumScrollMicros &&
                       agentScrollMicros <= MaximumScrollMicros &&
                       requestScrollMicros <= MaximumScrollMicros,
                   "Inspector residency work stays within quantitative time gates");
  result &= expect(
      data.valid && directPagesExact && pendingPagesExact &&
          data.maximumRowsReturned <= ui::MaximumInspectorRows &&
          data.maximumValuesReturned <= ui::MaximumInspectorRows + 1 &&
          data.maximumResponseMicros <= MaximumProjectionResponseMicros &&
          std::ranges::all_of(directBatchMicros, [](qint64 micros) {
            return micros <= MaximumProjectionBatchMicros;
          }) &&
          pendingBatchMicros <= MaximumProjectionBatchMicros,
      "real graph-backed Inspector page projections are exact and quantitatively bounded");
  result &= expect(
      uiResponseCount >= 6 && uiResponseCount <= 24 &&
          maximumPageDemands <= 4 &&
          planPeak <= ui::MaximumInspectorRows &&
          agentPeak <= ui::MaximumInspectorRows &&
          requestPeak <= ui::MaximumInspectorRows &&
          documentPeak <= ui::MaximumInspectorRows &&
          work.peakRows <= ui::MaximumInspectorRows + 1 &&
          widgetPeak <= 1328 && documents.peakDocuments <= 50,
      "Inspector residency, page demand, widget, and document peaks stay bounded");
  result &= expect(
      work.maximumConstructions <= 100 && work.maximumRetirements <= 100 &&
          work.constructions <= (uiResponseCount + 2) * 100 &&
          work.retirements <= work.constructions &&
          documents.constructions <= work.constructions &&
          documents.parseInputs <= documents.constructions + 16 &&
          work.rowResizes <= work.constructions * 4 + 256 &&
          documents.measurements <= documents.constructions * 8 + 64 &&
          work.layoutRequests <= work.constructions * 64 + 512 &&
          work.paints <= work.constructions * 32 + 512 &&
          work.maximumRowResizes <= 256 &&
          work.maximumLayoutRequests <= 4096 &&
          work.paintFrames > 0 && work.maximumFramePaints <= 2048,
      "Inspector construction, churn, parsing, measurement, and per-frame work stay bounded");
  std::cout << "Inspector performance rows=" << rowCount
            << " plan_us=" << planMicros
            << " plan_scroll_us=" << planScrollMicros
            << " agent_us=" << agentMicros
            << " agent_scroll_us=" << agentScrollMicros
            << " request_us=" << requestMicros
            << " request_scroll_us=" << requestScrollMicros
            << " plan_peak=" << planPeak << " agent_peak=" << agentPeak
            << " request_peak=" << requestPeak
            << " markdown_peak=" << documentPeak
            << " widget_peak=" << widgetPeak
            << " ui_pages=" << uiResponseCount
            << " max_page_burst=" << maximumPageDemands
            << " max_projection_us=" << data.maximumResponseMicros
            << " plan_batch_us=" << directBatchMicros[0]
            << " agent_batch_us=" << directBatchMicros[1]
            << " request_batch_us=" << directBatchMicros[2]
            << " pending_batch_us=" << pendingBatchMicros
            << " row_constructions=" << work.constructions
            << " row_retirements=" << work.retirements
            << " row_resize_events=" << work.rowResizes
            << " layout_requests=" << work.layoutRequests
            << " paints=" << work.paints
            << " max_constructions=" << work.maximumConstructions
            << " max_retirements=" << work.maximumRetirements
            << " max_resizes=" << work.maximumRowResizes
            << " max_layouts=" << work.maximumLayoutRequests
            << " max_paints=" << work.maximumPaints
            << " max_frame_paints=" << work.maximumFramePaints
            << " paint_frames=" << work.paintFrames
            << " document_constructions=" << documents.constructions
            << " document_retirements=" << documents.retirements
            << " parse_inputs=" << documents.parseInputs
            << " document_mutations=" << documents.mutations
            << " document_measurements=" << documents.measurements
            << " update_renderer_retained=" << retainedAgentRenderer
            << " update_row_constructions=" << updateRowConstructions
            << " update_document_constructions="
            << updateDocumentConstructions
            << " update_parse_inputs=" << updateParseInputs
            << " update_document_mutations=" << updateDocumentMutations
            << " update_document_measurements=" << updateDocumentMeasurements
            << " update_layout_requests=" << updateLayoutRequests
            << " update_paints=" << updatePaints << '\n';
  qApp->removeEventFilter(&work);
  return result;
}

bool focusedAgentRendererRemainsAuthoritative() {
  WindowedInspectorData data;
  data.threadIncarnation = 88;
  data.agents.reserve(200);
  for (int index = 0; index < 200; ++index) {
    ui::InspectorAgentRow agent;
    const std::string agentId = "focus-agent-" + std::to_string(index);
    agent.agentPath = "root/focus-agent-" + std::to_string(index);
    agent.status = nodegraph::NodeStatus::Running;
    if (index != 0)
      agent.resultText = "## Result " + std::to_string(index) +
                         "\n\nSelectable Markdown content.";
    data.agents.push_back(agentRow(agentId, std::move(agent)));
  }

  middle::InspectorPane pane;
  pane.resize(440, 720);
  pane.show();
  data.bind(pane);
  pane.tabs()->setCurrentIndex(1);
  QCoreApplication::processEvents();
  auto *view = pane.findChild<QAbstractScrollArea *>(
      QStringLiteral("inspectorAgentRows"));
  const auto sendTab = [](bool backward = false) {
    QWidget *target = QApplication::focusWidget();
    QKeyEvent event(QEvent::KeyPress,
                    backward ? Qt::Key_Backtab : Qt::Key_Tab,
                    backward ? Qt::ShiftModifier : Qt::NoModifier);
    return target && QApplication::sendEvent(target, &event);
  };
  const auto focusedFrame = [&pane] {
    QWidget *widget = QApplication::focusWidget();
    while (widget && widget != &pane) {
      if (widget->objectName() == QStringLiteral("inspectorAgentFrame"))
        return qobject_cast<QFrame *>(widget);
      widget = widget->parentWidget();
    }
    return static_cast<QFrame *>(nullptr);
  };
  QFrame *first = nullptr;
  for (QFrame *frame :
       pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"))) {
    QLabel *name = frame->findChild<QLabel *>(QStringLiteral("agentName"));
    if (name && name->text() == QStringLiteral("focus-agent-0")) {
      first = frame;
      break;
    }
  }
  auto *disclosure = first ? first->findChild<QToolButton *>(
                                 QStringLiteral("agentDisclosureButton"))
                           : nullptr;
  auto *copy = first ? first->findChild<QToolButton *>(
                           QStringLiteral("agentCopyButton"))
                     : nullptr;
  ui::InspectorAgentRow *firstAgent =
      std::get_if<ui::InspectorAgentRow>(&data.agents.front().value);
  firstAgent->resultText = "## Result 0\n\n";
  for (int line = 0; line < 600; ++line)
    firstAgent->resultText +=
        "Selectable Markdown content " + std::to_string(line) + ".\n\n";
  data.present(pane, ui::InspectorProjection::Agents);
  QCoreApplication::processEvents();
  if (disclosure)
    disclosure->click();
  if (copy && view) {
    copy->setFocus(Qt::TabFocusReason);
    view->verticalScrollBar()->setValue(300);
  }
  auto *markdown = first ? first->findChild<middle::MarkdownTextView *>()
                         : nullptr;
  const bool oversizedForward =
      sendTab() && disclosure && disclosure->hasFocus() && view &&
      QRect(disclosure->mapTo(view->viewport(), QPoint{}), disclosure->size())
          .intersects(view->viewport()->rect());
  const bool oversizedMarkdown =
      sendTab() && markdown && markdown->hasFocus() && view &&
      markdown->mapTo(view->viewport(), QPoint{}).y() >= 0;
  const bool oversizedBackward =
      sendTab(true) && disclosure && disclosure->hasFocus() && view &&
      QRect(disclosure->mapTo(view->viewport(), QPoint{}), disclosure->size())
          .intersects(view->viewport()->rect());
  if (markdown) {
    QTextCursor cursor(markdown->document());
    cursor.setPosition(0);
    cursor.movePosition(QTextCursor::NextWord, QTextCursor::KeepAnchor);
    markdown->setTextCursor(cursor);
    pane.activateWindow();
    markdown->setFocus(Qt::TabFocusReason);
  }
  QCoreApplication::processEvents();
  QPointer<QFrame> pinned = first;
  QPointer<middle::MarkdownTextView> pinnedMarkdown = markdown;
  QTextDocument *document = markdown ? markdown->document() : nullptr;
  const int selectionStart =
      markdown ? markdown->textCursor().selectionStart() : -1;
  const int selectionEnd = markdown ? markdown->textCursor().selectionEnd() : -1;

  bool result = expect(
      view && pinned && pinnedMarkdown && pinnedMarkdown->hasFocus() &&
          QAccessible::queryAccessibleInterface(pinnedMarkdown) &&
          !pinnedMarkdown->accessibleName().isEmpty(),
      "the real Agent Markdown renderer owns focus and accessibility");
  result &= expect(oversizedForward && oversizedMarkdown && oversizedBackward,
                   "Tab reveals the exact control within an Agent row taller than the viewport");
  if (view && markdown) {
    const int beforeWheel = view->verticalScrollBar()->value();
    const QPoint local(8, 8);
    QWheelEvent wheel(QPointF(local),
                      QPointF(markdown->viewport()->mapToGlobal(local)),
                      QPoint{}, QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(markdown->viewport(), &wheel);
    QCoreApplication::processEvents();
    result &= expect(view->verticalScrollBar()->value() > beforeWheel,
                     "wheel input over Markdown scrolls the owning viewport");
    view->verticalScrollBar()->setValue(0);
    QCoreApplication::processEvents();
    markdown->setFocus(Qt::TabFocusReason);
    view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
    QCoreApplication::processEvents();
  }
  result &= expect(
      pinned && pinnedMarkdown && pinnedMarkdown->document() == document &&
          pinnedMarkdown->hasFocus() &&
          pinnedMarkdown->textCursor().selectionStart() == selectionStart &&
          pinnedMarkdown->textCursor().selectionEnd() == selectionEnd &&
          !pinned->isHidden() &&
          pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"))
                  .size() <= 51,
      "scrolling pins the exact focused Agent renderer and selection at a bounded cost");

  QFrame *anchorFrame = nullptr;
  if (view) {
    for (QFrame *frame :
         pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"))) {
      if (frame == pinned || frame->isHidden())
        continue;
      if (!anchorFrame || frame->geometry().top() < anchorFrame->geometry().top())
        anchorFrame = frame;
    }
  }
  QPointer<QFrame> retainedAnchor = anchorFrame;
  const int anchorTop = anchorFrame ? anchorFrame->geometry().top() : 0;
  QFont changedFont = pane.font();
  changedFont.setPointSizeF(changedFont.pointSizeF() + 1.0);
  pane.setFont(changedFont);
  const bool fontReflowStable = waitFor([&] {
    return
      retainedAnchor && retainedAnchor->geometry().top() == anchorTop && pinned &&
          pinnedMarkdown && pinnedMarkdown->document() == document &&
          pinnedMarkdown->hasFocus() &&
          pinnedMarkdown->textCursor().selectionStart() == selectionStart &&
          pinnedMarkdown->textCursor().selectionEnd() == selectionEnd;
  });
  result &= expect(
      fontReflowStable,
      "font reflow preserves the semantic anchor and focused Agent renderer");
  const int styleAnchorTop = retainedAnchor ? retainedAnchor->geometry().top() : 0;
  pane.setStyleSheet(QStringLiteral(
      "QFrame#inspectorAgentFrame { border: 1px solid transparent; }"));
  const bool styleReflowStable = waitFor([&] {
    return
      retainedAnchor && retainedAnchor->geometry().top() == styleAnchorTop &&
          pinned && pinnedMarkdown && pinnedMarkdown->document() == document &&
          pinnedMarkdown->hasFocus();
  });
  result &= expect(
      styleReflowStable,
      "style reflow preserves the semantic anchor and focused Agent renderer");

  pane.resize(360, 760);
  QCoreApplication::processEvents();
  result &= expect(pinned && pinnedMarkdown && pinnedMarkdown->document() ==
                                                document &&
                       pinnedMarkdown->hasFocus(),
                   "width reflow retains the focused Agent renderer and document");
  const bool nextDelivered = sendTab();
  const bool nextFocused = waitFor([&] {
    QFrame *frame = focusedFrame();
    QLabel *name = frame ? frame->findChild<QLabel *>(
                               QStringLiteral("agentName"))
                         : nullptr;
    return name && name->text() == QStringLiteral("focus-agent-1") && view &&
           frame->geometry().intersects(view->viewport()->rect());
  });
  result &= expect(nextDelivered && nextFocused && pinned &&
                       pane.findChildren<QFrame *>(
                               QStringLiteral("inspectorAgentFrame"))
                               .size() <= 51,
                   "Tab reveals and enters the next semantic Agent row while the source renderer remains valid");
  const bool backtabDelivered = sendTab(true);
  const bool previousFocused = waitFor(
      [&] { return focusedFrame() == pinned && pinnedMarkdown->hasFocus(); });
  result &= expect(backtabDelivered && previousFocused,
                   "Backtab returns to the preceding semantic Agent control without replacement");
  if (view && pinnedMarkdown) {
    pinnedMarkdown->setFocus(Qt::OtherFocusReason);
    view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
    QCoreApplication::processEvents();
    result &= expect(pinned && pinnedMarkdown->hasFocus() &&
                         !pinned->geometry().intersects(
                             view->viewport()->rect()),
                     "focus alone pins an offscreen Agent renderer");
    view->setFocus(Qt::OtherFocusReason);
    result &= expect(pinned && pinned->parentWidget() == nullptr &&
                         pinned->isHidden() && view->hasFocus(),
                     "focus transfer detaches but does not destroy its active signal receiver");
  }
  QCoreApplication::processEvents();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  result &= expect(
      pinned.isNull() &&
          pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"))
                  .size() <= 50,
      "an offscreen Agent renderer retires after its real focus leaves");

  if (view) {
    const std::size_t responses = data.responseCount;
    view->verticalScrollBar()->setValue(520);
    view->verticalScrollBar()->setValue(260);
    static_cast<void>(waitFor(
        [&] { return data.responseCount > responses; }));
  }
  std::vector<QFrame *> visibleFrames;
  for (QFrame *frame :
       pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame")))
    if (!frame->isHidden() && view &&
        frame->geometry().intersects(view->viewport()->rect()))
      visibleFrames.push_back(frame);
  std::ranges::sort(visibleFrames, {},
                    [](QFrame *frame) { return frame->geometry().top(); });
  QAccessibleInterface *outerInterface =
      QAccessible::queryAccessibleInterface(view);
  QAccessibleInterface *viewportInterface =
      view ? QAccessible::queryAccessibleInterface(view->viewport()) : nullptr;
  bool accessibleOrder = outerInterface && viewportInterface &&
                         outerInterface->text(QAccessible::Name) ==
                             QStringLiteral("Agents") &&
                         viewportInterface->text(QAccessible::Name).isEmpty() &&
                         outerInterface->indexOfChild(viewportInterface) >= 0;
  int previousAccessibleRow = -1;
  if (viewportInterface) {
    for (int index = 0; index < viewportInterface->childCount(); ++index) {
      QAccessibleInterface *child = viewportInterface->child(index);
      auto *frame = child ? qobject_cast<QFrame *>(child->object()) : nullptr;
      if (!frame ||
          frame->objectName() != QStringLiteral("inspectorAgentFrame"))
        continue;
      QLabel *name =
          frame->findChild<QLabel *>(QStringLiteral("agentName"));
      const int row = name ? name->text().section('-', -1).toInt() : -1;
      accessibleOrder &= row > previousAccessibleRow &&
                         child->state().invisible == frame->isHidden();
      if (!frame->isHidden()) {
        const QRect global(view->viewport()->mapToGlobal(QPoint{}),
                           view->viewport()->size());
        accessibleOrder &=
            global.intersects(child->rect()) &&
            accessibleButtonCount(child,
                                  QStringLiteral("Copy agent content")) == 1 &&
            accessibleButtonCount(child, QStringLiteral("Expand agent")) +
                    accessibleButtonCount(
                        child, QStringLiteral("Collapse agent")) ==
                1;
      }
      previousAccessibleRow = row;
    }
  }
  result &= expect(accessibleOrder && previousAccessibleRow >= 0 &&
                       accessibleRowsMatch(view),
                   "the standard accessibility tree exposes ordered semantic rows and named controls only once");
  if (view)
    view->setFocus(Qt::TabFocusReason);
  result &= expect(
      !visibleFrames.empty() && sendTab() &&
          focusedFrame() == visibleFrames.front(),
      "Tab enters the first visible semantic Agent after reverse scrolling");
  if (view) {
    view->verticalScrollBar()->setValue(0);
    view->setFocus(Qt::TabFocusReason);
    static_cast<void>(waitFor([&] {
      return std::ranges::any_of(
          pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame")),
          [view](QFrame *frame) {
            QLabel *name = frame->findChild<QLabel *>(
                QStringLiteral("agentName"));
            return name && name->text() == QStringLiteral("focus-agent-0") &&
                   !frame->isHidden() &&
                   frame->geometry().intersects(view->viewport()->rect());
          });
    }));
  }
  const bool enteredFirstRow = sendTab() && focusedFrame();
  const bool returnedToViewport = sendTab(true) && view && view->hasFocus();
  const bool exitedViewport = sendTab(true) && view &&
                              QApplication::focusWidget() != view &&
                              !view->isAncestorOf(QApplication::focusWidget());
  result &= expect(enteredFirstRow && returnedToViewport && exitedViewport,
                   "Backtab exits before the first semantic Agent row without trapping focus");
  return result;
}

bool expandedAgentStateIsBoundedAndExact() {
  WindowedInspectorData data;
  data.threadIncarnation = 89;
  for (int index = 0; index < 80; ++index) {
    ui::InspectorAgentRow agent;
    const std::string id = "retained-agent-" + std::to_string(index);
    agent.agentPath = "root/" + id;
    agent.status = nodegraph::NodeStatus::Running;
    agent.resultText = "Result " + std::to_string(index);
    data.agents.push_back(agentRow(id, std::move(agent)));
  }

  middle::InspectorPane pane;
  pane.resize(440, 10000);
  pane.show();
  data.bind(pane);
  pane.tabs()->setCurrentIndex(1);
  const bool firstPageReady = waitFor([&] {
    return pane
               .findChildren<QFrame *>(
                   QStringLiteral("inspectorAgentFrame"))
               .size() == ui::MaximumInspectorRows;
  });
  auto frames = pane.findChildren<QFrame *>(
      QStringLiteral("inspectorAgentFrame"));
  std::ranges::sort(frames, {},
                    [](QFrame *frame) { return frame->geometry().top(); });
  for (QFrame *frame : frames)
    if (QToolButton *disclosure = frame->findChild<QToolButton *>(
            QStringLiteral("agentDisclosureButton")))
      disclosure->click();
  QCoreApplication::processEvents();

  const std::string oldestKey = "agent:retained-agent-0";
  const ui::InspectorRowRequest filled =
      pane.rowRequest(ui::InspectorProjection::Agents);
  QPointer<QFrame> oldest = frames.empty() ? nullptr : frames.front();
  auto *oldestContent = oldest ? oldest->findChild<QWidget *>(
                                    QStringLiteral("agentCardContent"))
                               : nullptr;
  auto *oldestDisclosure = oldest ? oldest->findChild<QToolButton *>(
                                       QStringLiteral("agentDisclosureButton"))
                                  : nullptr;
  bool result = expect(
      firstPageReady && filled.retainedKeys.size() ==
                            ui::MaximumInspectorRows &&
          filled.retainedKeys.front() == oldestKey && oldestContent &&
          oldestContent->isVisible() && oldestDisclosure,
      "Agent expansion retention is bounded by the paging contract");

  pane.resize(440, 720);
  if (oldestDisclosure)
    oldestDisclosure->setFocus(Qt::TabFocusReason);
  auto *view = pane.findChild<QAbstractScrollArea *>(
      QStringLiteral("inspectorAgentRows"));
  if (view)
    view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
  QFrame *newFrame = nullptr;
  const bool lastPageReady = waitFor([&] {
    const auto retained =
        pane.rowRequest(ui::InspectorProjection::Agents).retainedKeys;
    for (QFrame *frame : pane.findChildren<QFrame *>(
             QStringLiteral("inspectorAgentFrame"))) {
      QLabel *name =
          frame->findChild<QLabel *>(QStringLiteral("agentName"));
      const std::string key =
          name ? "agent:" + name->text().toStdString() : std::string{};
      if (!key.empty() &&
          std::ranges::find(retained, key) == retained.end() && !frame->isHidden()) {
        newFrame = frame;
        return true;
      }
    }
    return false;
  });
  auto *newDisclosure = newFrame ? newFrame->findChild<QToolButton *>(
                                      QStringLiteral("agentDisclosureButton"))
                                 : nullptr;
  const std::string newKey =
      newFrame
          ? "agent:" + newFrame->findChild<QLabel *>(
                           QStringLiteral("agentName"))
                           ->text()
                           .toStdString()
          : std::string{};
  if (newDisclosure)
    newDisclosure->click();
  QCoreApplication::processEvents();
  const ui::InspectorRowRequest evicted =
      pane.rowRequest(ui::InspectorProjection::Agents);
  result &= expect(
      lastPageReady && oldest && oldestContent && !oldestContent->isVisible() &&
          evicted.retainedKeys.size() == ui::MaximumInspectorRows &&
          std::ranges::find(evicted.retainedKeys, oldestKey) ==
              evicted.retainedKeys.end() &&
          std::ranges::find(evicted.retainedKeys, newKey) !=
              evicted.retainedKeys.end() &&
          pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"))
                  .size() <=
              ui::MaximumInspectorRows + 1,
      "evicting retained expansion collapses its focus-pinned renderer through the geometry authority");
  return result;
}

bool rowViewportRetiresOnlyAfterWheelDispatch() {
  WindowedInspectorData data;
  data.threadIncarnation = 89;
  data.agents.reserve(200);
  for (int index = 0; index < 200; ++index) {
    ui::InspectorAgentRow agent;
    const std::string agentId = "wheel-agent-" + std::to_string(index);
    agent.agentPath = "root/wheel-agent-" + std::to_string(index);
    agent.status = nodegraph::NodeStatus::Running;
    agent.resultText = "Selectable Markdown " + std::to_string(index) + ".";
    data.agents.push_back(agentRow(agentId, std::move(agent)));
  }

  middle::InspectorPane pane;
  pane.resize(440, 720);
  pane.show();
  data.bind(pane);
  pane.tabs()->setCurrentIndex(1);
  pane.setRefreshRequestedAction(
      [&] { data.present(pane, ui::InspectorProjection::Agents); });
  auto *view = pane.findChild<QAbstractScrollArea *>(
      QStringLiteral("inspectorAgentRows"));
  QFrame *source = nullptr;
  static_cast<void>(waitFor([&] {
    source = nullptr;
    for (QFrame *frame :
         pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame")))
      if (view && !frame->isHidden() &&
          frame->geometry().intersects(view->viewport()->rect()) &&
          frame->findChild<middle::MarkdownTextView *>()) {
        source = frame;
        break;
      }
    return source != nullptr;
  }));
  if (view)
    view->setFocus(Qt::OtherFocusReason);
  auto *markdown =
      source ? source->findChild<middle::MarkdownTextView *>() : nullptr;
  auto *disclosure = source ? source->findChild<QToolButton *>(
                                  QStringLiteral("agentDisclosureButton"))
                            : nullptr;
  if (disclosure) {
    disclosure->click();
    QCoreApplication::processEvents();
  }
  if (view)
    view->setFocus(Qt::OtherFocusReason);
  QPointer<QFrame> retainedSource = source;
  QPointer<middle::MarkdownTextView> retainedMarkdown = markdown;
  QPointer<QWidget> retainedReceiver = markdown ? markdown->viewport() : nullptr;

  bool result = expect(view && retainedSource && retainedMarkdown &&
                           retainedReceiver &&
                           retainedMarkdown->isVisibleTo(view->viewport()) &&
                           view->verticalScrollBar()->maximum() > 0,
                       "the wheel lifetime fixture has a materialized Markdown receiver");
  if (view && retainedReceiver) {
    const QPoint local(8, 8);
    QWheelEvent wheel(
        QPointF(local), QPointF(retainedReceiver->mapToGlobal(local)),
        QPoint(0, -view->verticalScrollBar()->maximum() - 1000), QPoint{},
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    const bool delivered = QApplication::sendEvent(retainedReceiver, &wheel);
    result &= expect(delivered && wheel.isAccepted() && retainedSource &&
                         retainedMarkdown && retainedReceiver &&
                         retainedSource->parentWidget() == nullptr &&
                         retainedSource->isHidden() &&
                         view->verticalScrollBar()->value() ==
                             view->verticalScrollBar()->maximum(),
                     "wheel delivery keeps its retired renderer alive through sendEvent");
  }
  QCoreApplication::processEvents();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  result &= expect(
      retainedSource.isNull() && retainedMarkdown.isNull() &&
          retainedReceiver.isNull() &&
          pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"))
                  .size() <= 50,
      "the offscreen wheel receiver retires after event delivery at bounded residency");
  return result;
}

bool threadReplacementReanchorsBeforePaging() {
  WindowedInspectorData data;
  data.threadIncarnation = 90;
  const auto populate = [&data](std::string_view prefix) {
    data.agents.clear();
    data.agents.reserve(400);
    for (int index = 0; index < 400; ++index) {
      ui::InspectorAgentRow agent;
      const std::string agentId =
          std::string(prefix) + std::to_string(index);
      agent.agentPath = "root/" + agentId;
      agent.status = nodegraph::NodeStatus::Running;
      data.agents.push_back(agentRow(agentId, std::move(agent)));
    }
  };
  populate("thread-a-");

  middle::InspectorPane pane;
  pane.resize(440, 720);
  pane.show();
  data.bind(pane);
  pane.tabs()->setCurrentIndex(1);
  QCoreApplication::processEvents();
  auto *view = pane.findChild<QAbstractScrollArea *>(
      QStringLiteral("inspectorAgentRows"));
  if (view) {
    pane.setRefreshRequestedAction(
        [&] { data.present(pane, ui::InspectorProjection::Agents); });
    view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum() /
                                        2);
    static_cast<void>(waitFor([&] {
      return view->verticalScrollBar()->value() > 0 &&
             pane.rowRequest(ui::InspectorProjection::Agents).first > 0;
    }));
  }
  const std::size_t responses = data.responseCount;
  ++data.threadIncarnation;
  populate("thread-b-");
  data.present(pane, ui::InspectorProjection::Agents);
  const bool firstRowVisible = waitFor([&] {
    for (QFrame *frame :
         pane.findChildren<QFrame *>(QStringLiteral("inspectorAgentFrame"))) {
      QLabel *name = frame->findChild<QLabel *>(QStringLiteral("agentName"));
      if (name && name->text() == QStringLiteral("thread-b-0") &&
          !frame->isHidden() && view &&
          frame->geometry().intersects(view->viewport()->rect()))
        return true;
    }
    return false;
  });
  return expect(view && view->verticalScrollBar()->value() == 0 &&
                    data.responseCount == responses + 2 && firstRowVisible,
                "thread replacement resets its viewport before requesting the new first page");
}

bool requestResidencyPreservesIdentityAndSafeLifetime() {
  nodegraph::NodeGraph graph;
  WindowedInspectorData data;
  data.threadIncarnation = 99;
  {
    auto write = graph.write();
    for (int index = 0; index < 120; ++index) {
      nodegraph::NodeRef target = write.upsert(
          {nodegraph::NodeKind::Interaction,
           "window-request-" + std::to_string(index)});
      PendingRequestDescriptor request = requestDescriptor(target);
      request.raw["command"] = "window-command-" + std::to_string(index);
      data.requests.push_back(requestRow(std::move(request)));
    }
    static_cast<void>(write.finish());
  }

  middle::InspectorPane pane;
  pane.resize(440, 720);
  pane.show();
  data.bind(pane);
  pane.tabs()->setCurrentIndex(3);
  auto *view = pane.findChild<QAbstractScrollArea *>(
      QStringLiteral("inspectorRequestRows"));
  static_cast<void>(waitFor([&] {
    return std::ranges::any_of(
        pane.findChildren<QFrame *>(QStringLiteral("inspectorRequestFrame")),
        [view](const QFrame *frame) {
          return view && !frame->isHidden() &&
                 frame->geometry().intersects(view->viewport()->rect()) &&
                 frame->findChild<QPushButton *>(
                     QStringLiteral("pendingRequestAccept"));
        });
  }));
  if (view) {
    view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum() /
                                        2);
    static_cast<void>(waitFor([view] {
      return view->verticalScrollBar()->value() ==
             view->verticalScrollBar()->maximum() / 2;
    }));
  }
  QFrame *focusedRow = nullptr;
  static_cast<void>(waitFor([&] {
    focusedRow = nullptr;
    for (QFrame *frame :
         pane.findChildren<QFrame *>(QStringLiteral("inspectorRequestFrame")))
      if (!frame->isHidden() && view &&
          frame->geometry().intersects(view->viewport()->rect())) {
        focusedRow = frame;
        break;
      }
    return focusedRow && focusedRow->findChild<QPushButton *>(
                             QStringLiteral("pendingRequestAccept"));
  }));
  auto *accept = focusedRow ? focusedRow->findChild<QPushButton *>(
                                  QStringLiteral("pendingRequestAccept"))
                            : nullptr;
  const QString focusedDetail =
      focusedRow ? focusedRow->findChild<QLabel *>(
                           QStringLiteral("pendingRequestDetail"))
                           ->text()
                 : QString{};
  nodegraph::NodeRef focusedTarget;
  if (focusedRow) {
    for (const ui::InspectorRow &row : data.requests)
      if (const auto *request =
              std::get_if<PendingRequestDescriptor>(&row.value);
          request &&
          QString::fromStdString(PendingRequestPolicy::detail(*request)) ==
              focusedDetail) {
        focusedTarget = request->target;
        break;
      }
  }
  if (accept) {
    pane.activateWindow();
    accept->setFocus(Qt::TabFocusReason);
  }
  QCoreApplication::processEvents();
  const int focusedTop = focusedRow ? focusedRow->geometry().top() : 0;
  QPointer<QFrame> stableRow = focusedRow;
  QPointer<QPushButton> stableAccept = accept;

  nodegraph::NodeRef insertedTarget;
  {
    auto write = graph.write();
    insertedTarget = write.upsert(
        {nodegraph::NodeKind::Interaction, "inserted-window-request"});
    static_cast<void>(write.finish());
  }
  PendingRequestDescriptor inserted = requestDescriptor(insertedTarget);
  inserted.raw["command"] = "inserted-window-command";
  data.requests.insert(data.requests.begin(), requestRow(std::move(inserted)));
  ++data.requestOrderRevision;
  data.present(pane, ui::InspectorProjection::Requests);
  QCoreApplication::processEvents();
  bool result = expect(
      stableRow && stableAccept && stableAccept->hasFocus() &&
          stableRow->geometry().top() == focusedTop &&
          stableRow->findChild<QPushButton *>(
              QStringLiteral("pendingRequestAccept")) == stableAccept,
      "insertion above a focused Request retains its renderer, control, and pixel anchor");

  data.requests.erase(data.requests.begin());
  ++data.requestOrderRevision;
  data.present(pane, ui::InspectorProjection::Requests);
  QCoreApplication::processEvents();
  result &= expect(
      stableRow && stableAccept && stableAccept->hasFocus() &&
          stableRow->geometry().top() == focusedTop &&
          stableRow->findChild<QPushButton *>(
              QStringLiteral("pendingRequestAccept")) == stableAccept,
      "removal above a focused Request retains its renderer, control, and pixel anchor");

  nodegraph::NodeRef acceptedTarget;
  pane.setRequestActions(
      {},
      [&](const nodegraph::NodeRef &target) {
        acceptedTarget = target;
        std::erase_if(data.requests, [&target](const ui::InspectorRow &row) {
          const auto *request =
              std::get_if<PendingRequestDescriptor>(&row.value);
          return request && request->target == target;
        });
        ++data.requestOrderRevision;
        data.present(pane, ui::InspectorProjection::Requests);
      },
      {});
  if (stableAccept)
    stableAccept->click();
  QCoreApplication::processEvents();
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  result &= expect(
      focusedTarget && acceptedTarget == focusedTarget && stableRow.isNull() &&
          stableAccept.isNull() && view && view->hasFocus() &&
          pane.findChildren<QFrame *>(QStringLiteral("inspectorRequestFrame"))
                  .size() <= 50,
      "a Request action may synchronously remove its own renderer without stale lifetime or focus");
  return result;
}

} // namespace
} // namespace codexui::codex

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  if (argc == 4 && std::string_view(argv[1]) == "--performance") {
    bool passed = codexui::codex::requestedDprIsActive(std::stod(argv[3]));
    passed &= codexui::codex::inspectorResidencyPerformance(
        std::stoull(argv[2]));
    return passed ? 0 : 1;
  }
  if (argc == 3 && std::string_view(argv[1]) == "--geometry") {
    bool passed = codexui::codex::requestedDprIsActive(std::stod(argv[2]));
    passed &= codexui::codex::focusedAgentRendererRemainsAuthoritative();
    passed &= codexui::codex::rowViewportRetiresOnlyAfterWheelDispatch();
    passed &= codexui::codex::threadReplacementReanchorsBeforePaging();
    passed &=
        codexui::codex::requestResidencyPreservesIdentityAndSafeLifetime();
    return passed ? 0 : 1;
  }
  bool passed = true;
  passed &= codexui::codex::logicalAgentsRemainDeduplicated();
  passed &=
      codexui::codex::agentPresentationIdentityFollowsExactGraphLifetime();
  passed &= codexui::codex::sharedChildFactsFanOutToEveryExactParent();
  passed &= codexui::codex::agentIndexTracksAuthorityInsteadOfUpdateHistory();
  passed &= codexui::codex::establishedAgentsWidgetContractIsRetained();
  passed &= codexui::codex::planAndRequestUpdatesRetainUnaffectedRows();
  passed &=
      codexui::codex::partialProjectionRetiresTheWholeThreadPresentation();
  passed &= codexui::codex::hiddenRequestProjectionRetiresExactTargets();
  passed &= codexui::codex::changesTabShowsCanonicalThreadRepositoryChanges();
  passed &= codexui::codex::stateAndProtocolRemainUsefulBoundedAndRedacted();
  passed &= codexui::codex::focusedAgentRendererRemainsAuthoritative();
  passed &= codexui::codex::expandedAgentStateIsBoundedAndExact();
  passed &= codexui::codex::rowViewportRetiresOnlyAfterWheelDispatch();
  passed &= codexui::codex::threadReplacementReanchorsBeforePaging();
  passed &= codexui::codex::requestResidencyPreservesIdentityAndSafeLifetime();
  if (passed)
    std::cout << "NodeGraph Inspector UI adapter tests passed\n";
  return passed ? 0 : 1;
}
