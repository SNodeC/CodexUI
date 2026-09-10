// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/ShellWidget.h"

#include "codex/ConnectionDialog.h"
#include "codex/FileSelectionDialog.h"
#include "codex/FrontendSession.h"
#include "codex/NewThreadDialog.h"
#include "codex/PendingRequestDialog.h"
#include "codex/PendingRequestPolicy.h"
#include "codex/TurnSettingsWidget.h"
#include "codex/middle/ComposerPane.h"
#include "codex/middle/ConversationView.h"
#include "codex/middle/InspectorPane.h"
#include "codex/middle/MiddleRegionWidget.h"
#include "codex/middle/ThreadPane.h"
#include "codex/ui/BrandMark.h"
#include "codex/ui/ExpandingPromptEditor.h"
#include "codex/ui/NodeGraphUiAdapter.h"
#include "codex/ui/UiStyle.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStringList>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

namespace codexui::codex {
namespace {

constexpr auto DraftThreadId = "draft:new-thread";
constexpr int GraphRetryDelayMilliseconds = 8;
constexpr std::size_t ConversationPresentationRowsPerPass = 8;

bool containsKind(const nodegraph::GraphChanged &change,
                  std::initializer_list<nodegraph::NodeKind> kinds) {
  const auto matches = [kinds](const nodegraph::NodeRef &node) {
    return node && std::ranges::find(kinds, node->id().kind) != kinds.end();
  };
  return std::ranges::any_of(change.affected, matches) ||
         std::ranges::any_of(change.removed, matches);
}

QString text(std::string_view value) {
  return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

std::string utf8(const QString &value) { return value.toUtf8().toStdString(); }

QString lastActivityText(std::int64_t timestamp) {
  const QDateTime activity =
      QDateTime::fromSecsSinceEpoch(timestamp).toLocalTime();
  const QDateTime now = QDateTime::currentDateTime();
  const QString formatted =
      activity.date() == now.date()
          ? activity.toString(QStringLiteral("HH:mm:ss"))
          : activity.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
  return QStringLiteral("Last activity: %1").arg(formatted);
}

const nodegraph::Value *graphField(const nodegraph::NodeState &state,
                                   std::string_view name) {
  const auto found = state.fields.find(name);
  return found == state.fields.end() ? nullptr : &found->second;
}

const nodegraph::Value *graphField(const nodegraph::Value::Object &object,
                                   std::string_view name) {
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

std::string graphString(const nodegraph::Value *value) {
  return value && value->asString() ? *value->asString() : std::string{};
}

bool fieldChanged(const nodegraph::NodeGraph::ReadAccess &read,
                  const nodegraph::NodeRef &node, std::string_view field,
                  std::uint64_t revision) {
  return node && read.contains(node) &&
         read.fieldChangedRevision(node, field) == revision;
}

struct ThreadPaneRoute {
  bool affected = false;
  bool structural = false;
  std::vector<nodegraph::NodeRef> rows;
};

ThreadPaneRoute
threadPaneRoute(const nodegraph::GraphChanged &change,
                const nodegraph::NodeGraph &graph,
                middle::ThreadPane::SortCriterion sortCriterion) {
  if (change.rescanRequired ||
      containsKind(change, {nodegraph::NodeKind::Interaction}) ||
      std::ranges::any_of(change.removed, [](const auto &node) {
        return node && (node->id().kind == nodegraph::NodeKind::Runtime ||
                        node->id().kind == nodegraph::NodeKind::Thread);
      }))
    return {true, true, {}};
  const auto read = graph.tryRead();
  if (!read)
    return containsKind(change, {nodegraph::NodeKind::Runtime,
                                 nodegraph::NodeKind::Thread})
               ? ThreadPaneRoute{true, true, {}}
               : ThreadPaneRoute{};
  constexpr std::array<std::string_view, 14> Fields{"name",
                                                    "title",
                                                    "cwd",
                                                    "workspace",
                                                    "status",
                                                    "createdAt",
                                                    "updatedAt",
                                                    "recencyAt",
                                                    "lastActivityAt",
                                                    "localActivityAt",
                                                    "localPromptActivityAt",
                                                    "pendingInteractionCount",
                                                    "hydrationState",
                                                    "archived"};
  ThreadPaneRoute route;
  for (const nodegraph::NodeRef &node : change.affected) {
    if (!node || !read->contains(node))
      continue;
    if (node->id().kind == nodegraph::NodeKind::Runtime) {
      if (read->structureChangedRevision(node) == change.revision)
        return {true, true, {}};
      continue;
    }
    if (node->id().kind != nodegraph::NodeKind::Thread)
      continue;
    const bool presentationChanged =
        read->statusChangedRevision(node) == change.revision ||
        std::ranges::any_of(Fields, [&](std::string_view field) {
          return fieldChanged(*read, node, field, change.revision);
        });
    if (!presentationChanged &&
        read->structureChangedRevision(node) != change.revision)
      continue;
    const bool sortChanged =
        (sortCriterion == middle::ThreadPane::SortCriterion::Alphanumeric &&
         (fieldChanged(*read, node, "name", change.revision) ||
          fieldChanged(*read, node, "title", change.revision))) ||
        (sortCriterion == middle::ThreadPane::SortCriterion::Created &&
         fieldChanged(*read, node, "createdAt", change.revision)) ||
        (sortCriterion == middle::ThreadPane::SortCriterion::LastChanged &&
         fieldChanged(*read, node, "updatedAt", change.revision)) ||
        (sortCriterion == middle::ThreadPane::SortCriterion::Recency &&
         fieldChanged(*read, node, "recencyAt", change.revision));
    if (read->structureChangedRevision(node) == change.revision ||
        sortChanged || fieldChanged(*read, node, "archived", change.revision))
      return {true, true, {}};
    route.affected = true;
    if (std::ranges::find(route.rows, node) == route.rows.end())
      route.rows.push_back(node);
  }
  return route;
}

std::vector<AttachmentDraft>
graphAttachmentDrafts(const nodegraph::NodeState &state) {
  std::vector<AttachmentDraft> result;
  const nodegraph::Value *attachments = graphField(state, "attachments");
  const nodegraph::Value::Array *array =
      attachments ? attachments->asArray() : nullptr;
  if (!array)
    return result;
  result.reserve(array->size());
  for (const nodegraph::Value &entry : *array) {
    const nodegraph::Value::Object *object = entry.asObject();
    if (!object)
      continue;
    AttachmentDraft attachment;
    attachment.path = graphString(graphField(*object, "path"));
    attachment.name = graphString(graphField(*object, "displayName"));
    attachment.mimeType = graphString(graphField(*object, "mimeType"));
    if (!attachment.path.empty())
      result.emplace_back(std::move(attachment));
  }
  return result;
}

struct ConversationRoute {
  bool affected = false;
  bool structural = false;
  std::vector<nodegraph::NodeRef> items;
};

ConversationRoute conversationRoute(const nodegraph::GraphChanged &change,
                                    const nodegraph::NodeGraph &graph,
                                    const nodegraph::NodeRef &selectedThread) {
  if (change.rescanRequired)
    return {true, true, {}};
  if (!selectedThread)
    return {};
  constexpr std::size_t MaximumFilteredNodes = 64;
  if (change.affected.size() + change.removed.size() > MaximumFilteredNodes)
    return {true, true, {}};

  const std::optional<nodegraph::NodeGraph::ReadAccess> read = graph.tryRead();
  if (!read)
    return {true, true, {}};

  const std::string &selectedId = selectedThread->id().canonical;
  ConversationRoute route;
  const auto addItem = [&](const nodegraph::NodeRef &item) {
    if (item && std::ranges::find(route.items, item) == route.items.end())
      route.items.push_back(item);
  };
  const auto routeNode = [&](const nodegraph::NodeRef &node) {
    if (!node)
      return;
    if (node == selectedThread) {
      if (!read->contains(node)) {
        route = {true, true, {}};
        return;
      }
      constexpr std::array<std::string_view, 5> Fields{
          "historyLoadedItemCount", "historyTotalItemCount", "historyHasMore",
          "hasMore", "hydrationState"};
      if (read->structureChangedRevision(node) == change.revision ||
          std::ranges::any_of(Fields, [&](std::string_view field) {
            return fieldChanged(*read, node, field, change.revision);
          })) {
        route.affected = true;
        route.structural = true;
      }
      return;
    }
    if (node->id().kind == nodegraph::NodeKind::Thread)
      return;
    if (node->id().kind != nodegraph::NodeKind::Turn &&
        node->id().kind != nodegraph::NodeKind::Item)
      return;

    try {
      bool belongs = false;
      nodegraph::NodeRef ancestor = node;
      while ((ancestor = read->parent(ancestor))) {
        if (ancestor == selectedThread) {
          belongs = true;
          break;
        }
        if (ancestor->id().kind == nodegraph::NodeKind::Thread)
          break;
      }

      if (!belongs) {
        const std::shared_ptr<const nodegraph::NodeState> state =
            read->state(node);
        for (const std::string_view field :
             {std::string_view("protocolThreadId"),
              std::string_view("threadId")}) {
          if (graphString(graphField(*state, field)) == selectedId) {
            belongs = true;
            break;
          }
        }
      }
      if (!belongs) {
        if (node->id().kind == nodegraph::NodeKind::Item &&
            read->structureChangedRevision(node) == change.revision) {
          // A row moved out of the selected thread has already lost its old
          // ancestry. The Qt model's exact NodeRef index determines whether
          // there is a selected row to remove.
          route.affected = true;
          route.structural = true;
          addItem(node);
        }
        return;
      }
      route.affected = true;
      if (node->id().kind == nodegraph::NodeKind::Turn) {
        route.structural = true;
        const auto roots =
            read->related(node, nodegraph::RelationKind::TurnRootItem);
        if (!roots.empty())
          addItem(roots.front());
        return;
      }
      if (read->structureChangedRevision(node) == change.revision)
        route.structural = true;
      addItem(node);
    } catch (const std::invalid_argument &) {
      // A queued NodeRef may have been retired by a later graph transaction.
      // Conservatively refresh rather than risk missing a selected update.
      route = {true, true, {}};
    }
  };

  for (const nodegraph::NodeRef &node : change.affected)
    routeNode(node);
  for (const nodegraph::NodeRef &node : change.removed) {
    if (!node)
      continue;
    if (node == selectedThread)
      return {true, true, {}};
    if (node->id().kind == nodegraph::NodeKind::Item) {
      route.affected = true;
      route.structural = true;
      addItem(node);
    } else if (node->id().kind == nodegraph::NodeKind::Turn) {
      // Its descendants are separately affected or removed by NodeGraph.
      // The exact Item identities below decide whether selected rows exist.
      route.affected = true;
      route.structural = true;
    }
  }
  return route;
}

enum class InspectorDependency {
  None,
  Plan,
  Agents,
  Changes,
  Requests,
  State,
  Protocol,
};

bool inspectorAffected(const nodegraph::GraphChanged &change,
                       const nodegraph::NodeGraph &graph,
                       const nodegraph::NodeRef &selectedThread,
                       InspectorDependency dependency) {
  if (dependency == InspectorDependency::None ||
      dependency == InspectorDependency::Protocol)
    return false;
  if (change.rescanRequired)
    return true;
  if (dependency == InspectorDependency::Requests)
    return containsKind(change, {nodegraph::NodeKind::Interaction});
  // State explicitly presents a bounded cross-domain graph summary. It is the
  // sole Inspector page whose visible data can depend on any graph node.
  if (dependency == InspectorDependency::State)
    return !change.affected.empty() || !change.removed.empty();
  if (!selectedThread)
    return false;
  if (change.affected.size() + change.removed.size() > 64)
    return true;
  const auto read = graph.tryRead();
  if (!read)
    return true;
  if (!read->contains(selectedThread) || read->removed(selectedThread))
    return true;
  const std::vector<nodegraph::NodeRef> agentChildren =
      read->related(selectedThread, nodegraph::RelationKind::AgentChildThread);
  const auto relevant = [&](const nodegraph::NodeRef &node) {
    if (!node)
      return false;
    if (!read->contains(node))
      return node->id().kind == nodegraph::NodeKind::Thread ||
             node->id().kind == nodegraph::NodeKind::Turn ||
             node->id().kind == nodegraph::NodeKind::Item;
    if (node == selectedThread) {
      if (dependency == InspectorDependency::Changes)
        return fieldChanged(*read, node, "cwd", change.revision) ||
               fieldChanged(*read, node, "workspace", change.revision);
      return (dependency == InspectorDependency::Agents &&
              read->structureChangedRevision(node) == change.revision) ||
             fieldChanged(*read, node, "hydrationState", change.revision);
    }
    if (node->id().kind == nodegraph::NodeKind::Thread)
      return dependency == InspectorDependency::Agents &&
             std::ranges::find(agentChildren, node) != agentChildren.end();
    if (node->id().kind != nodegraph::NodeKind::Turn &&
        node->id().kind != nodegraph::NodeKind::Item)
      return false;

    nodegraph::NodeRef containingThread = node;
    while (containingThread &&
           containingThread->id().kind != nodegraph::NodeKind::Thread)
      containingThread = read->parent(containingThread);
    if (containingThread != selectedThread &&
        std::ranges::find(agentChildren, containingThread) ==
            agentChildren.end())
      return false;
    if (node->id().kind == nodegraph::NodeKind::Turn)
      return dependency == InspectorDependency::Plan &&
             (fieldChanged(*read, node, "plan", change.revision) ||
              fieldChanged(*read, node, "planExplanation", change.revision));
    const auto state = read->state(node);
    const std::string type = graphString(graphField(*state, "type"));
    if (dependency == InspectorDependency::Plan)
      return type == "plan";
    if (dependency == InspectorDependency::Agents)
      return type == "subAgentActivity" || type == "collabAgentToolCall" ||
             (type == "agentMessage" && containingThread != selectedThread);
    if (dependency == InspectorDependency::Changes) {
      if (type == "fileChange")
        return true;
      return type == "commandExecution" &&
             fieldChanged(*read, node, "cwd", change.revision);
    }
    return false;
  };
  return std::ranges::any_of(change.affected, relevant) ||
         std::ranges::any_of(change.removed, relevant);
}

bool shellChromeAffected(const nodegraph::GraphChanged &change,
                         const nodegraph::NodeGraph &graph,
                         const nodegraph::NodeRef &selectedThread) {
  if (change.rescanRequired)
    return true;
  constexpr std::size_t MaximumFilteredNodes = 64;
  if (change.affected.size() + change.removed.size() > MaximumFilteredNodes)
    return true;

  const auto read = graph.tryRead();
  if (!read)
    return true;
  const auto directlyAffectsChrome = [&](const auto &node) {
    if (!node)
      return false;
    if (!read->contains(node))
      return true;
    switch (node->id().kind) {
    case nodegraph::NodeKind::Connection:
      return true;
    case nodegraph::NodeKind::Runtime:
      return read->structureChangedRevision(node) == change.revision ||
             fieldChanged(*read, node, "controller", change.revision);
    case nodegraph::NodeKind::Catalog:
      return node->id().canonical == "model" ||
             node->id().canonical == "permissionProfile";
    case nodegraph::NodeKind::Interaction:
      return true;
    case nodegraph::NodeKind::Thread: {
      if (!selectedThread || node != selectedThread)
        return false;
      constexpr std::array<std::string_view, 15> Fields{"name",
                                                        "title",
                                                        "cwd",
                                                        "workspace",
                                                        "status",
                                                        "hydrationState",
                                                        "recoveryOnly",
                                                        "lastActivityAt",
                                                        "recencyAt",
                                                        "updatedAt",
                                                        "localActivityAt",
                                                        "localPromptActivityAt",
                                                        "settingsRevision",
                                                        "settings",
                                                        "latestSettingsUpdate"};
      return read->statusChangedRevision(node) == change.revision ||
             read->structureChangedRevision(node) == change.revision ||
             std::ranges::any_of(Fields, [&](std::string_view field) {
               return fieldChanged(*read, node, field, change.revision);
             });
    }
    default:
      return false;
    }
  };
  if (std::ranges::any_of(change.affected, directlyAffectsChrome) ||
      std::ranges::any_of(change.removed, directlyAffectsChrome))
    return true;
  if (!selectedThread)
    return false;

  const auto turnBelongsToSelection = [&](const auto &node) {
    if (!node || node->id().kind != nodegraph::NodeKind::Turn)
      return false;
    try {
      if (!read->contains(node))
        return true;
      if (read->statusChangedRevision(node) != change.revision &&
          read->structureChangedRevision(node) != change.revision)
        return false;
      if (read->parent(node) == selectedThread)
        return true;
      return graphString(graphField(*read->state(node), "protocolThreadId")) ==
             selectedThread->id().canonical;
    } catch (const std::invalid_argument &) {
      return true;
    }
  };
  return std::ranges::any_of(change.affected, turnBelongsToSelection) ||
         std::ranges::any_of(change.removed, turnBelongsToSelection);
}

bool graphUiFallbackAffected(const nodegraph::GraphChanged &change,
                             const nodegraph::NodeGraph &graph) {
  if (change.rescanRequired ||
      containsKind(change, {nodegraph::NodeKind::Notice}))
    return true;
  const auto read = graph.tryRead();
  if (!read)
    return containsKind(change, {nodegraph::NodeKind::Runtime});
  return std::ranges::any_of(change.affected, [&](const auto &node) {
    return node && read->contains(node) &&
           node->id().kind == nodegraph::NodeKind::Runtime &&
           (read->structureChangedRevision(node) == change.revision ||
            fieldChanged(*read, node, "uiSelectionSerial", change.revision));
  });
}

bool graphBool(const nodegraph::Value *value, bool fallback = false) {
  return value && value->asBool() ? *value->asBool() : fallback;
}

std::optional<std::int64_t> graphInteger(const nodegraph::Value *value) {
  if (!value)
    return std::nullopt;
  if (const auto *number = value->asInt64())
    return *number;
  if (const auto *number = value->asUInt64()) {
    if (*number <=
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return static_cast<std::int64_t>(*number);
  }
  return std::nullopt;
}

std::string graphStatus(const nodegraph::NodeState &state) {
  if (const nodegraph::Value *value = graphField(state, "status")) {
    if (const nodegraph::Value::Object *object = value->asObject())
      return graphString(graphField(*object, "type"));
    if (std::string status = graphString(value); !status.empty())
      return status;
  }
  switch (state.status) {
  case nodegraph::NodeStatus::Pending:
    return "pending";
  case nodegraph::NodeStatus::Running:
    return "running";
  case nodegraph::NodeStatus::Completed:
    return "completed";
  case nodegraph::NodeStatus::Failed:
    return "failed";
  case nodegraph::NodeStatus::Interrupted:
    return "interrupted";
  case nodegraph::NodeStatus::NotLoaded:
    return "notLoaded";
  case nodegraph::NodeStatus::Connected:
    return "connected";
  case nodegraph::NodeStatus::Disconnected:
    return "disconnected";
  case nodegraph::NodeStatus::Unknown:
    return {};
  }
  return {};
}

bool activeStatus(const nodegraph::NodeState &state) {
  const std::string status = graphStatus(state);
  return state.status == nodegraph::NodeStatus::Running || status == "active" ||
         status == "inProgress" || status == "running";
}

// These conversions never parse or encode app-server JSON. They adapt the
// graph's already-decoded values to existing local-only dialog/widget APIs.
nlohmann::json widgetJson(const nodegraph::Value &value) {
  if (value.isNull())
    return nullptr;
  if (const auto *boolean = value.asBool())
    return *boolean;
  if (const auto *number = value.asInt64())
    return *number;
  if (const auto *number = value.asUInt64())
    return *number;
  if (const auto *number = value.asDouble())
    return *number;
  if (const auto *string = value.asString())
    return *string;
  if (const auto *array = value.asArray()) {
    nlohmann::json result = nlohmann::json::array();
    for (const nodegraph::Value &entry : *array)
      result.push_back(widgetJson(entry));
    return result;
  }
  nlohmann::json result = nlohmann::json::object();
  if (const auto *object = value.asObject())
    for (const auto &[key, entry] : *object)
      result[key] = widgetJson(entry);
  return result;
}

nlohmann::json widgetSettingsJson(const nodegraph::NodeState &state) {
  nlohmann::json result = nlohmann::json::object();
  for (const std::string_view key : {
           std::string_view("model"),
           std::string_view("effort"),
           std::string_view("reasoningEffort"),
           std::string_view("personality"),
           std::string_view("sandbox"),
           std::string_view("sandboxPolicy"),
           std::string_view("approvalPolicy"),
           std::string_view("approvalsReviewer"),
           std::string_view("cwd"),
           std::string_view("activePermissionProfile"),
           std::string_view("serviceTier"),
           std::string_view("summary"),
           std::string_view("collaborationMode"),
       }) {
    if (const nodegraph::Value *value = graphField(state, key))
      result[std::string(key)] = widgetJson(*value);
  }
  return result;
}

nlohmann::json
catalogArray(const std::shared_ptr<const nodegraph::NodeState> &state,
             std::initializer_list<const char *> keys) {
  if (!state)
    return nlohmann::json::array();
  for (const char *key : keys) {
    const nodegraph::Value *value = graphField(*state, key);
    if (value && value->asArray())
      return widgetJson(*value);
  }
  return nlohmann::json::array();
}

nodegraph::Value actionValue(const nlohmann::json &value) {
  if (value.is_null())
    return nullptr;
  if (value.is_boolean())
    return value.get<bool>();
  if (value.is_number_unsigned())
    return value.get<std::uint64_t>();
  if (value.is_number_integer())
    return value.get<std::int64_t>();
  if (value.is_number_float())
    return value.get<double>();
  if (value.is_string())
    return value.get<std::string>();
  if (value.is_array()) {
    nodegraph::Value::Array result;
    result.reserve(value.size());
    for (const auto &entry : value)
      result.emplace_back(actionValue(entry));
    return result;
  }
  nodegraph::Value::Object result;
  if (value.is_object())
    for (auto entry = value.cbegin(); entry != value.cend(); ++entry)
      result.emplace(entry.key(), actionValue(entry.value()));
  return result;
}

nodegraph::Value::Object actionObject(const nlohmann::json &value) {
  nodegraph::Value converted = actionValue(value);
  return converted.asObject() ? std::move(*converted.asObject())
                              : nodegraph::Value::Object{};
}

bool connectionSettingsContainSelection(const nlohmann::json &settings,
                                        const nlohmann::json &selection) {
  if (!settings.is_object() || !selection.is_object())
    return false;
  const std::string transport = selection.value("transport", std::string{});
  if (transport.empty() ||
      settings.value("selected", std::string{}) != transport)
    return false;
  const nlohmann::json available =
      settings.value("available", nlohmann::json::array());
  for (const auto &entry : available) {
    if (!entry.is_object() || entry.value("key", std::string{}) != transport)
      continue;
    for (auto field = selection.cbegin(); field != selection.cend(); ++field) {
      if (field.key() != "transport" &&
          (!entry.contains(field.key()) || entry[field.key()] != field.value()))
        return false;
    }
    return true;
  }
  return false;
}

void applyConnectionSelection(nlohmann::json &settings,
                              const nlohmann::json &selection) {
  if (!settings.is_object() || !selection.is_object())
    return;
  const std::string transport = selection.value("transport", std::string{});
  if (transport.empty())
    return;
  settings["selected"] = transport;
  auto available = settings.find("available");
  if (available == settings.end() || !available->is_array())
    return;
  for (auto &entry : *available) {
    if (!entry.is_object() || entry.value("key", std::string{}) != transport)
      continue;
    for (auto field = selection.cbegin(); field != selection.cend(); ++field)
      if (field.key() != "transport")
        entry[field.key()] = field.value();
    return;
  }
}

std::string requestKind(std::string_view method) {
  if (method == "item/commandExecution/requestApproval")
    return "command-approval";
  if (method == "item/fileChange/requestApproval")
    return "file-change-approval";
  if (method == "item/tool/requestUserInput")
    return "user-input";
  if (method == "mcpServer/elicitation/request")
    return "mcp-elicitation";
  if (method == "item/permissions/requestApproval")
    return "permissions-approval";
  if (method == "item/tool/call")
    return "dynamic-tool-call";
  if (method == "account/chatgptAuthTokens/refresh")
    return "authentication-refresh";
  if (method == "attestation/generate")
    return "attestation";
  if (method == "applyPatchApproval")
    return "legacy-patch-approval";
  if (method == "execCommandApproval")
    return "legacy-command-approval";
  return "unsupported";
}

struct PendingGraphRequest final {
  nodegraph::NodeRef node;
  std::string id;
  std::string displayId;
  std::string kind;
  std::string threadId;
  nodegraph::Value::Object payload;
  std::optional<nodegraph::Value::Object> retainedResponsePayload;
  bool recoveryOnly = false;
  bool recoverable = false;
  bool actionable = false;

  bool operator==(const PendingGraphRequest &) const = default;
};

struct PendingGraphRequestSnapshot final {
  nodegraph::NodeRef node;
  std::shared_ptr<const nodegraph::NodeState> state;
  std::string threadId;
};

std::optional<PendingGraphRequestSnapshot>
readPendingRequest(nodegraph::NodeGraph::ReadAccess &read,
                   const nodegraph::NodeRef &selectedThread,
                   const std::string &requestKey) {
  nodegraph::NodeRef candidate;
  if (!requestKey.empty()) {
    candidate = read.find({nodegraph::NodeKind::Interaction, requestKey});
  } else if (selectedThread && !read.removed(selectedThread)) {
    candidate = read.relatedAt(selectedThread,
                               nodegraph::RelationKind::PendingInteraction, 0);
  }
  if (!candidate && requestKey.empty()) {
    const nodegraph::NodeRef runtime =
        read.find({nodegraph::NodeKind::Runtime, "runtime"});
    candidate =
        read.relatedAt(runtime, nodegraph::RelationKind::PendingInteraction, 0);
  }
  if (!candidate || candidate->id().kind != nodegraph::NodeKind::Interaction)
    return std::nullopt;

  std::shared_ptr<const nodegraph::NodeState> state = read.state(candidate);
  if (state->status != nodegraph::NodeStatus::Pending &&
      state->status != nodegraph::NodeStatus::Failed)
    return std::nullopt;

  nodegraph::NodeRef target =
      read.relatedAt(candidate, nodegraph::RelationKind::InteractionTarget, 0);
  while (target && target->id().kind != nodegraph::NodeKind::Thread)
    target = read.parent(target);
  return PendingGraphRequestSnapshot{std::move(candidate), std::move(state),
                                     target ? target->id().canonical
                                            : std::string{}};
}

PendingGraphRequest
materializePendingRequest(PendingGraphRequestSnapshot snapshot,
                          bool canControl) {
  PendingGraphRequest request;
  request.node = std::move(snapshot.node);
  request.id = request.node->id().canonical;
  request.displayId = graphString(graphField(*snapshot.state, "requestId"));
  if (request.displayId.empty())
    request.displayId = request.id;
  request.kind =
      requestKind(graphString(graphField(*snapshot.state, "method")));
  request.recoveryOnly = graphBool(graphField(*snapshot.state, "recoveryOnly"));
  request.actionable = canControl && !request.recoveryOnly;
  if (const nodegraph::Value *payload = graphField(*snapshot.state, "payload");
      payload && payload->asObject())
    request.payload = *payload->asObject();
  if (const nodegraph::Value *retained =
          graphField(*snapshot.state, "retainedResponsePayload");
      retained && retained->asObject())
    request.retainedResponsePayload = *retained->asObject();
  request.recoverable =
      request.recoveryOnly && request.retainedResponsePayload.has_value();
  request.threadId = snapshot.threadId.empty()
                         ? graphString(graphField(request.payload, "threadId"))
                         : std::move(snapshot.threadId);
  return request;
}

struct ShellChromeValues final {
  bool connected = false;
  bool retrying = false;
  bool canControl = false;
  bool activeTurn = false;
  bool threadAdmissionReady = true;
  bool conversationReadyForDisplay = true;
  bool hydrationFailed = false;
  bool recoveryOnly = false;
  std::string role;
  std::string providerState;
  std::string selectedTransport;
  std::string workspace;
  std::string title;
  std::string status;
  std::optional<std::int64_t> lastActivityAt;
  std::size_t totalPending = 0;
  nlohmann::json canonicalSettings = nlohmann::json::object();
  nlohmann::json settingsUpdate = nlohmann::json::object();
  std::uint64_t settingsRevision = 0;
  std::optional<PendingGraphRequest> attention;

  bool operator==(const ShellChromeValues &) const = default;
};

nodegraph::Value::Object
authoredResponsePayload(std::string_view kind,
                        const PendingRequestResponse &response) {
  nodegraph::Value::Object result;
  if (kind == "permissions-approval") {
    if (response.error.is_null()) {
      const auto scope = response.result.find("scope");
      if (scope != response.result.end())
        result.emplace("scope", actionValue(*scope));
    } else {
      result.emplace("scope", "decline");
    }
    return result;
  }
  if (kind == "user-input") {
    const auto answers = response.result.find("answers");
    if (answers != response.result.end())
      result.emplace("answers", actionValue(*answers));
    return result;
  }
  if (kind == "mcp-elicitation") {
    for (const char *field : {"action", "content", "_meta"}) {
      const auto found = response.result.find(field);
      if (found != response.result.end())
        result.emplace(field, actionValue(*found));
    }
    return result;
  }
  if (kind == "dynamic-tool-call") {
    for (const char *field : {"success", "contentItems", "message"}) {
      const auto found = response.result.find(field);
      if (found != response.result.end())
        result.emplace(field, actionValue(*found));
    }
    return result;
  }
  const auto decision = response.result.find("decision");
  if (decision != response.result.end())
    result.emplace("decision", actionValue(*decision));
  return result;
}

PendingRequestResponse
retainedResponse(std::string_view kind,
                 const nodegraph::Value::Object &payload) {
  PendingRequestResponse response;
  nlohmann::json result = nlohmann::json::object();
  const auto copy = [&](std::string_view key) {
    if (const nodegraph::Value *value = graphField(payload, key))
      result[std::string(key)] = widgetJson(*value);
  };
  if (kind == "user-input") {
    copy("answers");
  } else if (kind == "mcp-elicitation") {
    copy("action");
    copy("content");
    copy("_meta");
  } else if (kind == "permissions-approval") {
    copy("scope");
  } else if (kind == "dynamic-tool-call") {
    copy("success");
    copy("contentItems");
    copy("message");
  } else {
    copy("decision");
  }
  response.result = std::move(result);
  return response;
}

QLabel *makeLabel(QString value, const char *kind = "body") {
  auto *label = new QLabel(std::move(value));
  label->setProperty("kind", kind);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setMinimumWidth(0);
  label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}

QLabel *makeStatusLabel(QString value, QString objectName, int maximumWidth,
                        const char *kind = "meta") {
  auto *label = makeLabel({}, kind);
  label->setObjectName(std::move(objectName));
  label->setWordWrap(false);
  label->setMaximumWidth(maximumWidth);
  label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  label->setToolTip(value);
  label->setText(label->fontMetrics().elidedText(value, Qt::ElideMiddle,
                                                 label->maximumWidth()));
  return label;
}

void setStatusLabelText(QLabel *label, QString value) {
  const QString displayed = label->fontMetrics().elidedText(
      value, Qt::ElideMiddle, label->maximumWidth());
  if (label->toolTip() != value)
    label->setToolTip(value);
  if (label->text() != displayed)
    label->setText(displayed);
}

QString statusToneColor(QStringView tone) {
  if (tone == QStringLiteral("active"))
    return QString::fromLatin1(UiStyle::blue);
  if (tone == QStringLiteral("success"))
    return QString::fromLatin1(UiStyle::green);
  if (tone == QStringLiteral("warning"))
    return QString::fromLatin1(UiStyle::orange);
  if (tone == QStringLiteral("danger"))
    return QString::fromLatin1(UiStyle::red);
  return QStringLiteral("#98a2b3");
}

void setStatusTone(QFrame *dot, QLabel *label, const QString &tone) {
  if (dot->property("tone").toString() != tone) {
    dot->setProperty("tone", tone);
    dot->setStyleSheet(QStringLiteral("background:%1;border-radius:5px;")
                           .arg(statusToneColor(tone)));
  }
  if (label->property("tone").toString() == tone)
    return;
  label->setProperty("tone", tone);
  label->style()->unpolish(label);
  label->style()->polish(label);
  label->update();
}

QFrame *statusDot() {
  auto *dot = new QFrame;
  dot->setFixedSize(10, 10);
  dot->setStyleSheet(QStringLiteral("background:#98a2b3;border-radius:5px;"));
  return dot;
}

} // namespace

struct ShellWidget::Impl final {
  Impl(ShellWidget *owner, FrontendSession &session)
      : owner(owner), session(session), uiAdapter(session.nodeGraph()),
        alive(std::make_shared<bool>(true)) {
    owner->setProperty("conversationPresentationRowsPerPassBudget",
                       static_cast<qulonglong>(
                           ConversationPresentationRowsPerPass));
    buildUi();
    connectUi();
    const auto token = alive;
    session.setGraphChangedHandler(
        [this, token](const nodegraph::GraphChanged &change) {
          if (*token)
            handleGraphChanged(change);
        });
    session.setGraphUiEffectHandler(
        [this, token](const nodegraph::UiEffect &effect) {
          if (*token)
            handleUiEffect(effect);
        });
    session.setRuntimeStoppedHandler([this, token] {
      if (*token) {
        showNotice(QStringLiteral("Codex worker stopped."));
        scheduleRender();
      }
    });
    bindGraphPanes({});
    render();
  }

  ~Impl() {
    *alive = false;
    session.setGraphChangedHandler({});
    session.setGraphUiEffectHandler({});
    session.setRuntimeStoppedHandler({});
    if (qApp)
      qApp->removeEventFilter(owner);
  }

  void buildUi();
  void connectUi();
  void scheduleRender();
  void scheduleGraphBinding(bool immediate = true);
  void runGraphBinding();
  void bindGraphPanes(nodegraph::NodeRef selectedThread);
  [[nodiscard]] bool refreshConversation();
  [[nodiscard]] bool refreshInspector();
  void schedulePaneCommit(bool immediate = false);
  void commitPendingPanes();
  void handleGraphChanged(const nodegraph::GraphChanged &change);
  void handleUiEffect(const nodegraph::UiEffect &effect);
  void reconcileOptimisticCreation(const nodegraph::GraphChanged &change);
  void reconcileGraphUiFallback();
  void selectGraphThread(nodegraph::NodeRef thread);
  void scheduleDraftSelection(bool replenishRetry = true);
  void render();
  void renderStatus(const ShellChromeValues &values,
                    bool updateWorkspace = true);
  void showNotice(QString message, bool error = true);
  [[nodiscard]] bool sendNodeAction(nodegraph::NodeAction action,
                                    QString rejection);
  [[nodiscard]] bool sendRuntimeAction(nodegraph::RuntimeAction action,
                                       QString rejection);
  [[nodiscard]] nodegraph::NodeRef activeTurn() const;
  [[nodiscard]] nodegraph::NodeRef threadById(const std::string &id,
                                              bool *busy = nullptr) const;
  [[nodiscard]] std::optional<PendingGraphRequest>
  pendingRequest(const std::string &requestKey = {}, bool *busy = nullptr);
  void hydrateSelectedThreadIfNeeded(nodegraph::NodeRef thread);
  void beginNewThreadDialog();
  void renameThreadDialog(const nodegraph::NodeRef &thread);
  void confirmDeleteThread(const nodegraph::NodeRef &thread);
  [[nodiscard]] bool submitPrompt(QString prompt,
                                  std::vector<AttachmentDraft> attachments);
  void chooseAttachments();
  void recoverPrompt(const nodegraph::NodeRef &prompt);
  void reviewPending(const std::string &requestKey);
  void acceptPending(const std::string &requestKey);
  void rejectPending(const std::string &requestKey);
  void respondToFirstPending(bool approve);

  ShellWidget *owner = nullptr;
  FrontendSession &session;
  ui::NodeGraphUiAdapter uiAdapter;
  std::shared_ptr<bool> alive;
  std::optional<NewThreadDraft> newThreadDraft;
  std::string creationDraftCorrelation;
  std::uint64_t nextCreationDraftSerial = 1;
  std::string optimisticCreationThreadId;
  bool creationInFlight = false;
  std::string selectedGraphThreadId;
  nodegraph::NodeRef boundGraphThread;
  nodegraph::NodeRef attentionInteraction;
  std::map<const nodegraph::Node *, std::pair<nodegraph::NodeRef, QString>>
      retainedRenames;
  std::map<std::string, PendingRequestResponse> retainedInteractionResponses;
  std::optional<nlohmann::json> retainedConnectionSelection;
  bool renderScheduled = false;
  bool graphPanesBound = false;
  bool graphBindingScheduled = false;
  bool paneCommitScheduled = false;
  bool pendingThreadPane = false;
  std::vector<nodegraph::NodeRef> pendingThreadRows;
  bool pendingConversation = false;
  std::deque<nodegraph::NodeRef> pendingConversationItems;
  bool pendingInspector = false;
  bool pendingChrome = false;
  bool draftSelectionScheduled = false;
  bool graphFallbackScheduled = false;
  unsigned draftSelectionRetriesRemaining = 0;
  std::uint64_t lastNoticeSerial = 0;
  std::uint64_t lastSelectionSerial = 0;
  std::uint64_t modelCatalogRevision = 0;
  std::uint64_t permissionProfileCatalogRevision = 0;
  bool modelCatalogInitialized = false;
  bool modelCatalogPresent = false;
  bool permissionProfileCatalogInitialized = false;
  bool permissionProfileCatalogPresent = false;
  std::optional<ShellChromeValues> renderedChrome;
  std::uint64_t shellRenderCommits = 0;
  std::uint64_t threadPaneRoutes = 0;
  std::uint64_t conversationRoutes = 0;
  std::uint64_t inspectorRoutes = 0;

  middle::MiddleRegionWidget *middleRegion = nullptr;
  QPushButton *restoreSidebarButton = nullptr;
  QPushButton *restoreInspectorButton = nullptr;
  QLabel *workspaceBreadcrumb = nullptr;
  QPushButton *requestButton = nullptr;
  QFrame *connectionStatusDot = nullptr;
  QToolButton *connectionButton = nullptr;
  QAction *connectAction = nullptr;
  QAction *disconnectAction = nullptr;
  QAction *reconnectAction = nullptr;
  QPushButton *controllerButton = nullptr;
  QFrame *globalStatusDot = nullptr;
  QLabel *globalStatusLabel = nullptr;
};

void ShellWidget::Impl::buildUi() {
  owner->setObjectName(QStringLiteral("applicationShell"));
  auto *root = new QVBoxLayout(owner);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto *top = new QFrame;
  top->setObjectName(QStringLiteral("topBar"));
  top->setStyleSheet(QStringLiteral(
      "QFrame#topBar{background:#ffffff;border-bottom:1px solid #d7dee8;}"));
  top->setFixedHeight(64);
  auto *topLayout = new QHBoxLayout(top);
  topLayout->setContentsMargins(18, 0, 18, 0);
  topLayout->setSpacing(12);
  topLayout->addWidget(codexui::BrandMark::createLockup());

  restoreSidebarButton = new QPushButton(QStringLiteral("Show threads"));
  restoreSidebarButton->setProperty("kind", "subtle");
  restoreSidebarButton->setFixedHeight(32);
  restoreSidebarButton->hide();
  topLayout->addSpacing(12);
  topLayout->addWidget(restoreSidebarButton);
  topLayout->addSpacing(18);
  workspaceBreadcrumb = makeLabel(QStringLiteral("No workspace"), "muted");
  workspaceBreadcrumb->setWordWrap(false);
  workspaceBreadcrumb->setMaximumWidth(280);
  workspaceBreadcrumb->setStyleSheet(
      QStringLiteral("color:#667085;font-weight:500;"));
  topLayout->addWidget(workspaceBreadcrumb);
  topLayout->addStretch();

  restoreInspectorButton = new QPushButton(QStringLiteral("Show inspector"));
  restoreInspectorButton->setProperty("kind", "subtle");
  restoreInspectorButton->setFixedHeight(32);
  restoreInspectorButton->hide();
  requestButton = new QPushButton;
  requestButton->setProperty("kind", "request");
  requestButton->setFixedHeight(32);
  requestButton->hide();
  controllerButton = new QPushButton(QStringLiteral("Claim control"));
  controllerButton->setFixedHeight(32);
  topLayout->addWidget(restoreInspectorButton);
  topLayout->addWidget(requestButton);
  topLayout->addWidget(controllerButton);

  connectionStatusDot = statusDot();
  connectionStatusDot->setToolTip(QStringLiteral("Not connected"));
  connectionButton = new UiStyle::ChevronToolButton;
  connectionButton->setObjectName(QStringLiteral("transportButton"));
  connectionButton->setText(QStringLiteral("Connection"));
  connectionButton->setProperty("kind", "subtle");
  connectionButton->setProperty("codexChevron", true);
  connectionButton->setPopupMode(QToolButton::InstantPopup);
  connectionButton->setFixedHeight(32);
  auto *connectionMenu = new QMenu(connectionButton);
  connectionMenu->addAction(QStringLiteral("Configure..."), owner, [this] {
    nlohmann::json settings = nlohmann::json::object();
    {
      auto read = session.nodeGraph().tryRead();
      if (!read) {
        scheduleRender();
        showNotice(QStringLiteral("Connection state is busy; try again."));
        return;
      }
      const nodegraph::NodeRef connection =
          read->find({nodegraph::NodeKind::Connection, "connection"});
      if (connection) {
        const auto state = read->state(connection);
        if (const nodegraph::Value *value = graphField(*state, "settings"))
          settings = widgetJson(*value);
      }
    }
    if (!settings.is_object() || settings.empty()) {
      showNotice(QStringLiteral("Connection settings are not available yet."));
      return;
    }
    if (retainedConnectionSelection &&
        connectionSettingsContainSelection(settings,
                                           *retainedConnectionSelection))
      retainedConnectionSelection.reset();
    if (retainedConnectionSelection)
      applyConnectionSelection(settings, *retainedConnectionSelection);
    ConnectionDialog dialog(std::move(settings), owner);
    if (dialog.exec() != QDialog::Accepted)
      return;
    retainedConnectionSelection = dialog.selection();
    nodegraph::RuntimeAction action;
    action.kind = nodegraph::RuntimeActionKind::ConfigureConnection;
    action.payload = actionObject(*retainedConnectionSelection);
    static_cast<void>(sendRuntimeAction(
        std::move(action),
        QStringLiteral("Connection settings were not sent; try again.")));
  });
  connectionMenu->addSeparator();
  connectAction =
      connectionMenu->addAction(QStringLiteral("Connect"), owner, [this] {
        static_cast<void>(sendRuntimeAction(
            {nodegraph::RuntimeActionKind::Connect},
            QStringLiteral("Connect request was not admitted; try again.")));
      });
  disconnectAction =
      connectionMenu->addAction(QStringLiteral("Disconnect"), owner, [this] {
        static_cast<void>(sendRuntimeAction(
            {nodegraph::RuntimeActionKind::Disconnect},
            QStringLiteral("Disconnect request was not admitted; try again.")));
      });
  reconnectAction =
      connectionMenu->addAction(QStringLiteral("Reconnect"), owner, [this] {
        static_cast<void>(sendRuntimeAction(
            {nodegraph::RuntimeActionKind::Reconnect},
            QStringLiteral("Reconnect request was not admitted; try again.")));
      });
  connectionButton->setMenu(connectionMenu);
  auto *connectionControl = new QWidget;
  auto *connectionLayout = new QHBoxLayout(connectionControl);
  connectionLayout->setContentsMargins(0, 0, 0, 0);
  connectionLayout->setSpacing(6);
  connectionLayout->addWidget(connectionButton);
  connectionLayout->addWidget(connectionStatusDot);
  topLayout->addWidget(connectionControl);
  root->addWidget(top);

  middleRegion = new middle::MiddleRegionWidget;
  root->addWidget(middleRegion, 1);

  auto *statusBar = new QFrame;
  statusBar->setObjectName(QStringLiteral("customStatusBar"));
  statusBar->setStyleSheet(QStringLiteral(
      "QFrame#customStatusBar{background:#f8fafc;border-top:1px solid "
      "#d7dee8;}"));
  statusBar->setFixedHeight(40);
  auto *statusLayout = new QHBoxLayout(statusBar);
  statusLayout->setContentsMargins(18, 0, 24, 0);
  statusLayout->setSpacing(10);
  auto *attribution = new QLabel(QStringLiteral(
      "<span style=\"color:#344054;font-weight:600\">"
      "© Volker Christian &amp; Codex</span>  |  "
      "<a style=\"color:#344054;text-decoration:none;font-weight:600\" "
      "href=\"https://github.com/SNodeC/CodexUI\">CodexUI</a>  •  "
      "<a style=\"color:#344054;text-decoration:none;font-weight:600\" "
      "href=\"https://github.com/SNodeC/AISuite\">AISuite</a>  •  "
      "<span style=\"color:#98a2b3;font-size:8pt\">Powered by</span> "
      "<a style=\"color:#344054;text-decoration:none;font-weight:600\" "
      "href=\"https://github.com/SNodeC/snode.c\">SNode.C</a>"));
  attribution->setObjectName(QStringLiteral("statusAttribution"));
  attribution->setProperty("kind", "meta");
  attribution->setTextFormat(Qt::RichText);
  attribution->setOpenExternalLinks(true);
  attribution->setTextInteractionFlags(Qt::LinksAccessibleByMouse |
                                       Qt::LinksAccessibleByKeyboard);
  attribution->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
  statusLayout->addWidget(attribution);
  statusLayout->addStretch();
  auto *statusCaption = makeStatusLabel(
      QStringLiteral("Status:"), QStringLiteral("globalStatusCaption"), 60);
  statusLayout->addWidget(statusCaption);
  globalStatusDot = statusDot();
  globalStatusDot->setObjectName(QStringLiteral("shellGlobalStatusDot"));
  statusLayout->addWidget(globalStatusDot);
  globalStatusLabel =
      makeStatusLabel(QStringLiteral("Offline"),
                      QStringLiteral("globalStatusLabel"), 160, "settingLabel");
  statusLayout->addWidget(globalStatusLabel);
  root->addWidget(statusBar);
}

void ShellWidget::Impl::connectUi() {
  middle::ThreadPane::Actions threadActions;
  threadActions.newThread = [this] { beginNewThreadDialog(); };
  threadActions.refresh = [this] {
    static_cast<void>(sendRuntimeAction(
        {nodegraph::RuntimeActionKind::RefreshThreads},
        QStringLiteral("Thread refresh was not admitted; try again.")));
  };
  threadActions.hide = [this] { middleRegion->showSidebar(false); };
  threadActions.select = [this](const std::string &id) {
    if (id == DraftThreadId && newThreadDraft) {
      render();
      return;
    }
    const nodegraph::NodeRef thread = threadById(id);
    if (!thread)
      return;
    if (!creationInFlight && !optimisticCreationThreadId.empty()) {
      middleRegion->threads().confirmOptimisticThread(
          optimisticCreationThreadId);
      optimisticCreationThreadId.clear();
      creationDraftCorrelation.clear();
    }
    selectedGraphThreadId = thread->id().canonical;
    newThreadDraft.reset();
    bindGraphPanes(thread);
    hydrateSelectedThreadIfNeeded(thread);
    render();
  };
  threadActions.reload = [this](const std::string &id) {
    const nodegraph::NodeRef thread = threadById(id);
    if (!thread)
      return;
    nodegraph::NodeAction action{thread, nodegraph::NodeActionKind::Reload};
    static_cast<void>(sendNodeAction(
        std::move(action),
        QStringLiteral("Thread reload was not admitted; try again.")));
  };
  threadActions.rename = [this](const std::string &id) {
    if (const nodegraph::NodeRef thread = threadById(id))
      renameThreadDialog(thread);
  };
  threadActions.fork = [this](const std::string &id) {
    const nodegraph::NodeRef thread = threadById(id);
    if (!thread)
      return;
    nodegraph::NodeAction action{thread, nodegraph::NodeActionKind::Fork};
    static_cast<void>(sendNodeAction(
        std::move(action),
        QStringLiteral("Thread fork was not admitted; try again.")));
  };
  threadActions.toggleArchive = [this](const std::string &id) {
    const nodegraph::NodeRef thread = threadById(id);
    if (!thread)
      return;
    bool archived = false;
    if (auto read = session.nodeGraph().tryRead())
      archived = graphBool(graphField(*read->state(thread), "archived"));
    else {
      showNotice(
          QStringLiteral("Thread state is busy; no archive action was sent."));
      return;
    }
    nodegraph::NodeAction action{thread,
                                 archived ? nodegraph::NodeActionKind::Unarchive
                                          : nodegraph::NodeActionKind::Archive};
    static_cast<void>(sendNodeAction(
        std::move(action),
        QStringLiteral("Archive request was not admitted; try again.")));
  };
  threadActions.remove = [this](const std::string &id) {
    if (const nodegraph::NodeRef thread = threadById(id))
      confirmDeleteThread(thread);
  };
  middleRegion->threads().setActions(std::move(threadActions));

  middle::ComposerPane::Actions composerActions;
  composerActions.submit = [this](QString prompt,
                                  std::vector<AttachmentDraft> attachments) {
    return submitPrompt(std::move(prompt), std::move(attachments));
  };
  composerActions.stop = [this] {
    nodegraph::NodeRef turn = activeTurn();
    if (!turn) {
      showNotice(QStringLiteral("No active turn is available to stop."));
      return;
    }
    nodegraph::NodeAction action{turn,
                                 nodegraph::NodeActionKind::InterruptTurn};
    static_cast<void>(sendNodeAction(
        std::move(action),
        QStringLiteral("Stop request was not admitted; try again.")));
  };
  composerActions.attach = [this] { chooseAttachments(); };
  composerActions.accept = [this] { respondToFirstPending(true); };
  composerActions.review = [this] { respondToFirstPending(true); };
  composerActions.deny = [this] { respondToFirstPending(false); };
  middleRegion->composer().setActions(std::move(composerActions));

  middleRegion->conversation().setLoadMoreAction([this] {
    if (!boundGraphThread)
      return;
    const auto info = uiAdapter.conversationInfo(boundGraphThread);
    if (!info) {
      showNotice(QStringLiteral(
          "Conversation state is busy; no history request was sent."));
      return;
    }
    const middle::ConversationView::HistoryPageRequest request =
        middleRegion->conversation().requestNextHistoryPage(
            boundGraphThread->id().canonical,
            info->authoritativeItemCount, info->providerHasMore);
    pendingConversation = true;
    pendingConversationItems.clear();
    commitPendingPanes();
    if (!request.requestProvider)
      return;
    nodegraph::NodeAction action{boundGraphThread,
                                 nodegraph::NodeActionKind::LoadHistory};
    static_cast<void>(sendNodeAction(
        std::move(action),
        QStringLiteral("History request was not admitted; try again.")));
  });
  middleRegion->conversation().setPromptMaterializedAction(
      [this](nodegraph::NodeRef localPrompt) {
        nodegraph::NodeAction action{
            std::move(localPrompt),
            nodegraph::NodeActionKind::PromptMaterialized};
        return sendNodeAction(std::move(action), {});
      });
  middleRegion->conversation().setPromptRecoveryAction(
      [this](nodegraph::NodeRef prompt) { recoverPrompt(prompt); });
  middleRegion->conversation().setPresentationCommittedAction(
      [this](const std::string &threadId) {
        if (!boundGraphThread || boundGraphThread->id().canonical != threadId)
          return;
        renderedChrome.reset();
        pendingInspector = true;
        pendingChrome = true;
        schedulePaneCommit();
      });
  middleRegion->inspector().setRequestActions(
      [this](const std::string &id) { reviewPending(id); },
      [this](const std::string &id) { acceptPending(id); },
      [this](const std::string &id) { rejectPending(id); });
  middleRegion->inspector().setRefreshRequestedAction([this] {
    pendingInspector = true;
    schedulePaneCommit();
  });
  middleRegion->setPaneVisibilityAction(
      [this](bool sidebarVisible, bool inspectorVisible) {
        restoreSidebarButton->setVisible(!sidebarVisible);
        restoreInspectorButton->setVisible(!inspectorVisible);
      });

  connect(restoreSidebarButton, &QPushButton::clicked, owner,
          [this] { middleRegion->showSidebar(true); });
  connect(restoreInspectorButton, &QPushButton::clicked, owner, [this] {
    middleRegion->showInspector(true);
    pendingInspector = true;
    schedulePaneCommit();
  });
  connect(requestButton, &QPushButton::clicked, owner, [this] {
    middleRegion->showInspector(true);
    middleRegion->inspector().tabs()->setCurrentIndex(3);
    pendingInspector = true;
    schedulePaneCommit();
  });
  connect(controllerButton, &QPushButton::clicked, owner, [this] {
    bool controller = false;
    auto read = session.nodeGraph().tryRead();
    if (!read) {
      showNotice(QStringLiteral(
          "Connection state is busy; no controller request was sent."));
      return;
    }
    const nodegraph::NodeRef connection =
        read->find({nodegraph::NodeKind::Connection, "connection"});
    if (connection)
      controller = graphString(graphField(*read->state(connection), "role")) ==
                   "controller";
    read.reset();
    static_cast<void>(sendRuntimeAction(
        {controller ? nodegraph::RuntimeActionKind::ReleaseController
                    : nodegraph::RuntimeActionKind::ClaimController},
        QStringLiteral("Controller request was not admitted; try again.")));
  });
  qApp->installEventFilter(owner);
}

void ShellWidget::Impl::showNotice(QString message, bool error) {
  middleRegion->showNotice(std::move(message), error);
}

void ShellWidget::Impl::scheduleRender() {
  if (renderScheduled)
    return;
  renderScheduled = true;
  const auto token = alive;
  // Stream deltas can arrive in bursts. Reconcile cheap shell chrome once per
  // display interval while graph-bound panes schedule their own bounded work.
  QTimer::singleShot(16, Qt::PreciseTimer, owner, [this, token] {
    if (!*token)
      return;
    renderScheduled = false;
    render();
  });
}

void ShellWidget::Impl::scheduleGraphBinding(bool immediate) {
  if (graphBindingScheduled)
    return;
  graphBindingScheduled = true;
  const auto token = alive;
  const int delay = immediate ? 0 : GraphRetryDelayMilliseconds;
  QTimer::singleShot(delay, owner, [this, token] {
    if (!*token)
      return;
    graphBindingScheduled = false;
    runGraphBinding();
  });
}

void ShellWidget::Impl::runGraphBinding() {
  nodegraph::NodeRef selectedThread;
  if (!selectedGraphThreadId.empty()) {
    {
      auto graphRead = session.nodeGraph().tryRead();
      if (!graphRead) {
        scheduleGraphBinding(false);
        return;
      }
      selectedThread =
          graphRead->find({nodegraph::NodeKind::Thread, selectedGraphThreadId});
    }
  }

  const bool reboundSelectedThread =
      selectedThread && selectedThread != boundGraphThread;
  bindGraphPanes(std::move(selectedThread));
  if (reboundSelectedThread) {
    nodegraph::NodeAction action{boundGraphThread,
                                 nodegraph::NodeActionKind::Hydrate};
    static_cast<void>(sendNodeAction(
        std::move(action),
        QStringLiteral(
            "Thread loading was not admitted; select Reload to retry.")));
  }

  if (newThreadDraft)
    scheduleDraftSelection();
}

void ShellWidget::Impl::bindGraphPanes(nodegraph::NodeRef selectedThread) {
  if (graphPanesBound && boundGraphThread == selectedThread)
    return;
  if (selectedThread && boundGraphThread != selectedThread)
    middleRegion->conversation().beginThreadSelection(
        selectedThread->id().canonical);
  boundGraphThread = std::move(selectedThread);
  graphPanesBound = true;
  if (auto threads = uiAdapter.threads(boundGraphThread))
    middleRegion->threads().refresh(*threads);
  const bool conversationReady = refreshConversation();
  const bool inspectorReady = refreshInspector();
  // The immediate atomic bind already represents the newest graph state.
  // Any frame-coalesced work queued for the previous selection is obsolete.
  pendingThreadPane = false;
  pendingThreadRows.clear();
  pendingConversation = !conversationReady;
  pendingConversationItems.clear();
  pendingInspector = !inspectorReady;
  if (pendingConversation || pendingInspector)
    schedulePaneCommit();
}

bool ShellWidget::Impl::refreshConversation() {
  if (!boundGraphThread) {
    static_cast<void>(middleRegion->conversation().reconcile({}));
    return true;
  }

  const auto info = uiAdapter.conversationInfo(boundGraphThread);
  if (!info)
    return false;
  const std::string &threadId = boundGraphThread->id().canonical;
  const std::size_t historyLimit =
      middleRegion->conversation().historyLimitForThread(
          threadId, info->authoritativeItemCount);

  if (!info->readyForDisplay) {
    if (!info->hydrationFailed) {
      middleRegion->conversation().beginThreadSelection(threadId);
      return true;
    }
    middleRegion->conversation().setEmptyMessage(
        QStringLiteral("Thread loading failed. Select Reload to retry."));
    middle::ConversationSnapshot failed;
    failed.threadId = threadId;
    static_cast<void>(middleRegion->conversation().reconcile(failed));
    return true;
  }

  auto snapshot = uiAdapter.conversation(boundGraphThread, historyLimit);
  if (!snapshot)
    return false;
  middleRegion->conversation().setEmptyMessage(
      QStringLiteral("No materialized activity."));
  middleRegion->conversation().reconcileStaged(std::move(*snapshot));
  return true;
}

bool ShellWidget::Impl::refreshInspector() {
  nodegraph::NodeRef inspectorThread = boundGraphThread;
  if (inspectorThread) {
    const auto info = uiAdapter.conversationInfo(inspectorThread);
    if (!info)
      return false;
    const std::string &presentedThreadId =
        middleRegion->conversation().presentedThreadId();
    if (!presentedThreadId.empty() &&
        presentedThreadId != inspectorThread->id().canonical &&
        !info->hydrationFailed)
      return true;
    if (!info->readyForDisplay)
      inspectorThread.reset();
  }
  middle::InspectorPane &pane = middleRegion->inspector();
  std::optional<ui::InspectorProjection> projection;
  switch (pane.tabs()->currentIndex()) {
  case 0:
    projection = ui::InspectorProjection::Plan;
    break;
  case 1:
    projection = ui::InspectorProjection::Agents;
    break;
  case 2:
    projection = ui::InspectorProjection::Changes;
    break;
  case 3:
    projection = ui::InspectorProjection::Requests;
    break;
  case 4:
    if (auto *infoStack =
            pane.findChild<QStackedWidget *>(QStringLiteral("infoStack"));
        infoStack && infoStack->currentIndex() != 0)
      projection = ui::InspectorProjection::State;
    break;
  default:
    break;
  }
  if (!projection)
    return true;
  if (auto snapshot = uiAdapter.inspector(inspectorThread, *projection)) {
    pane.refresh(*snapshot, *projection);
    return true;
  }
  return false;
}

void ShellWidget::Impl::schedulePaneCommit(bool immediate) {
  if (paneCommitScheduled)
    return;
  paneCommitScheduled = true;
  const auto token = alive;
  QTimer::singleShot(immediate ? 0 : 16, Qt::PreciseTimer, owner,
                     [this, token] {
                       if (!*token)
                         return;
                       paneCommitScheduled = false;
                       commitPendingPanes();
                     });
}

void ShellWidget::Impl::commitPendingPanes() {
  owner->setProperty("paneCommitInvocations",
                     owner->property("paneCommitInvocations").toULongLong() +
                         1);
  owner->setProperty("conversationPresentationRowsInLastPass", qulonglong{0});
  bool retry = false;
  if (!pendingThreadPane && !pendingThreadRows.empty()) {
    std::vector<nodegraph::NodeRef> rows = std::move(pendingThreadRows);
    pendingThreadRows.clear();
    bool structuralFallback = false;
    for (const nodegraph::NodeRef &thread : rows) {
      const auto row = uiAdapter.threadRow(thread);
      if (!row || !middleRegion->threads().applyRowPresentation(*row)) {
        structuralFallback = true;
        break;
      }
    }
    if (structuralFallback) {
      pendingThreadPane = true;
    } else {
      ++threadPaneRoutes;
      owner->setProperty("threadPaneRoutes",
                         static_cast<qulonglong>(threadPaneRoutes));
      owner->setProperty(
          "targetedThreadPaneRoutes",
          owner->property("targetedThreadPaneRoutes").toULongLong() + 1);
    }
  }
  if (pendingThreadPane) {
    if (auto threads = uiAdapter.threads(boundGraphThread)) {
      pendingThreadPane = false;
      pendingThreadRows.clear();
      ++threadPaneRoutes;
      owner->setProperty("threadPaneRoutes",
                         static_cast<qulonglong>(threadPaneRoutes));
      middleRegion->threads().refresh(*threads);
      if (newThreadDraft)
        scheduleDraftSelection();
    } else {
      retry = true;
    }
  }
  if (!pendingConversation && !pendingConversationItems.empty()) {
    std::size_t appliedRows = 0;
    bool requiresStructuralReconcile = false;
    while (appliedRows < ConversationPresentationRowsPerPass &&
           !pendingConversationItems.empty()) {
      nodegraph::NodeRef item = std::move(pendingConversationItems.front());
      pendingConversationItems.pop_front();
      auto card = uiAdapter.card(boundGraphThread, item);
      if (!card || !middleRegion->conversation().applyCardPresentation(
                       std::move(*card))) {
        pendingConversationItems.push_front(std::move(item));
        requiresStructuralReconcile = true;
        break;
      }
      ++appliedRows;
    }
    owner->setProperty("conversationPresentationRowsInLastPass",
                       static_cast<qulonglong>(appliedRows));
    owner->setProperty(
        "conversationPresentationRowsProcessed",
        owner->property("conversationPresentationRowsProcessed")
                .toULongLong() +
            static_cast<qulonglong>(appliedRows));
    owner->setProperty(
        "conversationPresentationMaxRowsPerPass",
        std::max(owner->property("conversationPresentationMaxRowsPerPass")
                     .toULongLong(),
                 static_cast<qulonglong>(appliedRows)));
    if (requiresStructuralReconcile) {
      pendingConversation = true;
    }
    if (appliedRows != 0) {
      ++conversationRoutes;
      owner->setProperty("conversationRoutes",
                         static_cast<qulonglong>(conversationRoutes));
      owner->setProperty(
          "targetedConversationRoutes",
          owner->property("targetedConversationRoutes").toULongLong() + 1);
    }
    if (!pendingConversation && !pendingConversationItems.empty())
      owner->setProperty(
          "conversationPresentationDeferredPasses",
          owner->property("conversationPresentationDeferredPasses")
                  .toULongLong() +
              1);
  }
  if (pendingConversation && !pendingConversationItems.empty() &&
      boundGraphThread &&
      !middleRegion->conversation().structuralStagingActive()) {
    std::optional<middle::PromptMaterialization> materialization;
    nodegraph::NodeRef authoritativeItem;
    bool ambiguous = false;
    for (const nodegraph::NodeRef &item : pendingConversationItems) {
      auto candidate = uiAdapter.promptMaterialization(boundGraphThread, item);
      if (!candidate)
        continue;
      if (materialization) {
        ambiguous = true;
        break;
      }
      materialization = std::move(*candidate);
      authoritativeItem = item;
    }
    const bool exactMaterialization =
        materialization && !ambiguous &&
        std::ranges::all_of(pendingConversationItems,
                            [&](const nodegraph::NodeRef &item) {
                              return item == authoritativeItem ||
                                     item == materialization->prompt;
                            });
    if (exactMaterialization &&
        middleRegion->conversation()
            .applyPromptMaterialization(std::move(*materialization))
            .has_value()) {
      pendingConversation = false;
      pendingConversationItems.clear();
      ++conversationRoutes;
      owner->setProperty("conversationRoutes",
                         static_cast<qulonglong>(conversationRoutes));
      owner->setProperty(
          "targetedConversationRoutes",
          owner->property("targetedConversationRoutes").toULongLong() + 1);
      owner->setProperty(
          "targetedConversationPromptMaterializations",
          owner->property("targetedConversationPromptMaterializations")
                  .toULongLong() +
              1);
    }
  }
  if (pendingConversation && pendingConversationItems.size() == 1 &&
      boundGraphThread &&
      !middleRegion->conversation().structuralStagingActive()) {
    auto tail = uiAdapter.tailCard(boundGraphThread,
                                   pendingConversationItems.front());
    if (tail) {
      if (middleRegion->conversation().appendTailCard(std::move(*tail))) {
        pendingConversation = false;
        pendingConversationItems.clear();
        ++conversationRoutes;
        owner->setProperty("conversationRoutes",
                           static_cast<qulonglong>(conversationRoutes));
        owner->setProperty(
            "targetedConversationRoutes",
            owner->property("targetedConversationRoutes").toULongLong() + 1);
        owner->setProperty(
            "targetedConversationStructuralAppends",
            owner->property("targetedConversationStructuralAppends")
                    .toULongLong() +
                1);
      }
    }
  }
  if (pendingConversation && !pendingConversationItems.empty() &&
      boundGraphThread &&
      !middleRegion->conversation().structuralStagingActive()) {
    bool exact = true;
    bool touched = false;
    std::vector<nodegraph::NodeRef> unresolved;
    unresolved.reserve(pendingConversationItems.size());
    std::vector<middle::ConversationRowChange> rowChanges;
    rowChanges.reserve(pendingConversationItems.size());

    // Live projections run first so prompt retirement can transfer the stable
    // row to its authoritative NodeRef before the removed prompt is examined.
    for (const nodegraph::NodeRef &item : pendingConversationItems) {
      auto change = uiAdapter.rowChange(boundGraphThread, item);
      if (!change) {
        unresolved.push_back(item);
        continue;
      }
      rowChanges.push_back(std::move(*change));
    }

    std::vector<nodegraph::NodeRef> postponedRemovals;
    postponedRemovals.reserve(unresolved.size());
    for (const nodegraph::NodeRef &item : unresolved) {
      const QModelIndex index = middleRegion->conversation()
                                    .conversationModel()
                                    ->indexForTarget(item);
      const middle::ConversationItemModel::Row *row =
          middleRegion->conversation().conversationModel()->row(index.row());
      if (!index.isValid() || !row)
        continue;
      const bool replacementPending =
          std::ranges::any_of(rowChanges, [&](const auto &change) {
            return middle::stableKey(change.placement.card.key) ==
                       row->stableKey &&
                   change.placement.card.target != item;
          });
      if (replacementPending) {
        postponedRemovals.push_back(item);
        continue;
      }
      if (!middleRegion->conversation().removeCardTarget(item)) {
        exact = false;
        break;
      }
      touched = true;
    }

    // replaceChildren can name every changed sibling in an implementation
    // order that differs from the final provider order. Apply the small delta
    // batch in the neighbor order supplied by NodeGraph so already-correct
    // rows do not oscillate through redundant Qt moves.
    std::unordered_map<std::string, std::size_t> changedRows;
    changedRows.reserve(rowChanges.size());
    for (std::size_t index = 0; index < rowChanges.size(); ++index)
      changedRows.emplace(
          middle::stableKey(rowChanges[index].placement.card.key), index);
    std::vector<std::vector<std::size_t>> following(rowChanges.size());
    std::vector<std::size_t> predecessors(rowChanges.size(), 0);
    const auto relateOrder = [&](std::size_t before, std::size_t after) {
      if (before == after ||
          std::ranges::find(following[before], after) !=
              following[before].end())
        return;
      following[before].push_back(after);
      ++predecessors[after];
    };
    for (std::size_t index = 0; index < rowChanges.size(); ++index) {
      if (rowChanges[index].previousCardKey) {
        const auto previous = changedRows.find(
            middle::stableKey(*rowChanges[index].previousCardKey));
        if (previous != changedRows.end())
          relateOrder(previous->second, index);
      }
      if (rowChanges[index].nextCardKey) {
        const auto next = changedRows.find(
            middle::stableKey(*rowChanges[index].nextCardKey));
        if (next != changedRows.end())
          relateOrder(index, next->second);
      }
    }
    std::vector<std::size_t> orderedRows;
    orderedRows.reserve(rowChanges.size());
    std::vector<bool> emitted(rowChanges.size(), false);
    while (exact && orderedRows.size() < rowChanges.size()) {
      std::optional<std::size_t> ready;
      for (std::size_t index = 0; index < rowChanges.size(); ++index) {
        if (!emitted[index] && predecessors[index] == 0) {
          ready = index;
          break;
        }
      }
      if (!ready) {
        exact = false;
        break;
      }
      const std::size_t index = *ready;
      emitted[index] = true;
      orderedRows.push_back(index);
      for (const std::size_t next : following[index])
        --predecessors[next];
    }

    for (const std::size_t rowIndex : orderedRows) {
      if (!exact)
        break;
      middle::ConversationRowChange &change = rowChanges[rowIndex];
      const std::string key = middle::stableKey(change.placement.card.key);
      const bool represented =
          middleRegion->conversation()
              .conversationModel()
              ->indexForTarget(change.placement.card.target)
              .isValid() ||
          middleRegion->conversation()
              .conversationModel()
              ->indexForStableKey(key)
              .isValid();
      const bool adjacent =
          (change.previousCardKey &&
           middleRegion->conversation()
               .conversationModel()
               ->indexForStableKey(middle::stableKey(*change.previousCardKey))
               .isValid()) ||
          (change.nextCardKey &&
           middleRegion->conversation()
               .conversationModel()
               ->indexForStableKey(middle::stableKey(*change.nextCardKey))
               .isValid()) ||
          middleRegion->conversation().conversationModel()->rowCount() == 0;
      if (!represented && !adjacent)
        continue;
      if (!middleRegion->conversation().applyRowChange(std::move(change))) {
        exact = false;
        break;
      }
      touched = true;
    }
    if (exact) {
      for (const nodegraph::NodeRef &item : postponedRemovals) {
        if (!middleRegion->conversation()
                 .conversationModel()
                 ->indexForTarget(item)
                 .isValid())
          continue;
        if (!middleRegion->conversation().removeCardTarget(item)) {
          exact = false;
          break;
        }
        touched = true;
      }
    }
    if (exact) {
      pendingConversation = false;
      pendingConversationItems.clear();
      ++conversationRoutes;
      owner->setProperty("conversationRoutes",
                         static_cast<qulonglong>(conversationRoutes));
      owner->setProperty(
          "targetedConversationRoutes",
          owner->property("targetedConversationRoutes").toULongLong() + 1);
      owner->setProperty(
          "targetedConversationStructuralDeltas",
          owner->property("targetedConversationStructuralDeltas")
                  .toULongLong() +
              1);
      if (!touched)
        owner->setProperty(
            "targetedConversationStructuralNoops",
            owner->property("targetedConversationStructuralNoops")
                    .toULongLong() +
                1);
    }
  }
  if (pendingConversation) {
    if (refreshConversation()) {
      pendingConversation = false;
      pendingConversationItems.clear();
      ++conversationRoutes;
      owner->setProperty("conversationRoutes",
                         static_cast<qulonglong>(conversationRoutes));
    } else {
      retry = true;
    }
  }
  if (pendingInspector) {
    if (refreshInspector()) {
      pendingInspector = false;
      ++inspectorRoutes;
      owner->setProperty("inspectorRoutes",
                         static_cast<qulonglong>(inspectorRoutes));
    } else {
      retry = true;
    }
  }
  if (pendingChrome) {
    pendingChrome = false;
    render();
  }
  if (retry || pendingThreadPane || !pendingThreadRows.empty() ||
      pendingConversation || !pendingConversationItems.empty() ||
      pendingInspector || pendingChrome)
    schedulePaneCommit();
}

void ShellWidget::Impl::handleGraphChanged(
    const nodegraph::GraphChanged &change) {
  const bool updateChrome =
      shellChromeAffected(change, session.nodeGraph(), boundGraphThread);
  const bool optimisticCreationWasActive = !optimisticCreationThreadId.empty();
  for (const nodegraph::NodeRef &removed : change.removed) {
    if (!removed)
      continue;
    if (removed->id().kind == nodegraph::NodeKind::Thread)
      middleRegion->conversation().forgetThreadPresentation(
          removed->id().canonical);
    if (removed->id().kind == nodegraph::NodeKind::Interaction)
      retainedInteractionResponses.erase(removed->id().canonical);
    retainedRenames.erase(removed.get());
  }
  const nodegraph::NodeRef removedBoundThread =
      boundGraphThread && std::ranges::find(change.removed, boundGraphThread) !=
                              change.removed.end()
          ? boundGraphThread
          : nodegraph::NodeRef{};
  const bool providerReset =
      removedBoundThread &&
      std::ranges::any_of(change.affected, [](const auto &node) {
        return node && node->id().kind == nodegraph::NodeKind::Connection;
      });
  const ConversationRoute conversation =
      conversationRoute(change, session.nodeGraph(), boundGraphThread);
  InspectorDependency inspectorDependency = InspectorDependency::None;
  middle::InspectorPane &inspectorPane = middleRegion->inspector();
  if (inspectorPane.isVisible()) {
    switch (inspectorPane.tabs()->currentIndex()) {
    case 0:
      inspectorDependency = InspectorDependency::Plan;
      break;
    case 1:
      inspectorDependency = InspectorDependency::Agents;
      break;
    case 2:
      inspectorDependency = InspectorDependency::Changes;
      break;
    case 3:
      inspectorDependency = InspectorDependency::Requests;
      break;
    case 4:
      if (auto *infoStack = inspectorPane.findChild<QStackedWidget *>(
              QStringLiteral("infoStack"))) {
        if (infoStack->currentIndex() == 1)
          inspectorDependency = InspectorDependency::State;
        else if (infoStack->currentIndex() == 2)
          inspectorDependency = InspectorDependency::Protocol;
      }
      break;
    default:
      break;
    }
  }
  const ThreadPaneRoute threads =
      threadPaneRoute(change, session.nodeGraph(),
                      middleRegion->threads().currentSortCriterion());
  if (threads.structural) {
    pendingThreadPane = true;
    pendingThreadRows.clear();
  } else if (threads.affected && !pendingThreadPane) {
    for (const nodegraph::NodeRef &thread : threads.rows)
      if (std::ranges::find(pendingThreadRows, thread) ==
          pendingThreadRows.end())
        pendingThreadRows.push_back(thread);
  }
  if (conversation.structural) {
    if (!pendingConversation) {
      pendingConversationItems.assign(conversation.items.begin(),
                                      conversation.items.end());
    } else if (!std::ranges::equal(pendingConversationItems,
                                   conversation.items)) {
      // More than one structural transaction was coalesced. The complete
      // projection is the only safe way to establish the combined order.
      pendingConversationItems.clear();
    }
    pendingConversation = true;
  } else if (conversation.affected && !pendingConversation) {
    for (const nodegraph::NodeRef &item : conversation.items)
      if (std::ranges::find(pendingConversationItems, item) ==
          pendingConversationItems.end())
        pendingConversationItems.push_back(item);
  }
  pendingInspector = pendingInspector ||
                     inspectorAffected(change, session.nodeGraph(),
                                       boundGraphThread, inspectorDependency);
  pendingChrome = pendingChrome || updateChrome;
  if (change.rescanRequired ||
      containsKind(change, {nodegraph::NodeKind::Thread}))
    reconcileOptimisticCreation(change);
  if (graphUiFallbackAffected(change, session.nodeGraph()))
    reconcileGraphUiFallback();

  // Promotion may already have rebound a removed optimistic thread. Otherwise
  // clear all pane-held references synchronously before FrontendSession can
  // acknowledge retirement. Preserve the canonical selection only across a
  // provider generation reset so recreation can trigger one fresh hydration.
  if (removedBoundThread && boundGraphThread == removedBoundThread) {
    if (!providerReset)
      selectedGraphThreadId.clear();
    bindGraphPanes({});
  }

  // Retirement must not outlive presentation references. Ordinary state
  // traffic is merged to one old-UI reconciliation per display interval.
  if (!change.removed.empty())
    commitPendingPanes();
  else if (pendingThreadPane || !pendingThreadRows.empty() ||
           pendingConversation || !pendingConversationItems.empty() ||
           pendingInspector || pendingChrome)
    schedulePaneCommit();

  const bool selectedChanged =
      !graphPanesBound || change.rescanRequired ||
      (!boundGraphThread && !selectedGraphThreadId.empty() &&
       std::ranges::any_of(
           change.affected,
           [this](const auto &node) {
             return node && node->id().kind == nodegraph::NodeKind::Thread &&
                    node->id().canonical == selectedGraphThreadId;
           })) ||
      std::ranges::any_of(change.removed, [this](const auto &node) {
        return node && node->id().kind == nodegraph::NodeKind::Thread &&
               node->id().canonical == selectedGraphThreadId;
      });
  if (selectedChanged)
    scheduleGraphBinding();
  if (removedBoundThread || optimisticCreationWasActive ||
      !optimisticCreationThreadId.empty())
    scheduleRender();
}

void ShellWidget::Impl::reconcileGraphUiFallback() {
  std::string notice;
  bool noticeError = true;
  std::uint64_t noticeSerial = lastNoticeSerial;
  nodegraph::NodeRef selection;
  std::uint64_t selectionSerial = lastSelectionSerial;
  if (auto read = session.nodeGraph().tryRead()) {
    const auto considerNotice = [&](std::string_view id) {
      if (const nodegraph::NodeRef node =
              read->find({nodegraph::NodeKind::Notice, std::string(id)})) {
        const auto state = read->state(node);
        const std::int64_t rawSerial =
            graphInteger(graphField(*state, "noticeSerial")).value_or(0);
        if (rawSerial > 0 &&
            static_cast<std::uint64_t>(rawSerial) > noticeSerial) {
          noticeSerial = static_cast<std::uint64_t>(rawSerial);
          notice = graphString(graphField(*state, "noticeText"));
          if (notice.empty())
            notice = graphString(graphField(*state, "message"));
          noticeError =
              graphString(graphField(*state, "severity")) != "warning";
        }
      }
    };
    considerNotice("local-worker-notice");
    considerNotice("provider-notice");
    if (const nodegraph::NodeRef runtime =
            read->find({nodegraph::NodeKind::Runtime, "runtime"})) {
      const auto state = read->state(runtime);
      const std::int64_t rawSerial =
          graphInteger(graphField(*state, "uiSelectionSerial")).value_or(0);
      if (rawSerial > 0 &&
          static_cast<std::uint64_t>(rawSerial) > lastSelectionSerial) {
        const std::vector<nodegraph::NodeRef> targets =
            read->related(runtime, nodegraph::RelationKind::UiSelectionTarget);
        if (!targets.empty() && !read->removed(targets.front())) {
          selectionSerial = static_cast<std::uint64_t>(rawSerial);
          selection = targets.front();
        }
      }
    }
  } else {
    scheduleRender();
    if (!graphFallbackScheduled) {
      graphFallbackScheduled = true;
      const auto token = alive;
      QTimer::singleShot(GraphRetryDelayMilliseconds, owner, [this, token] {
        if (!*token)
          return;
        graphFallbackScheduled = false;
        reconcileGraphUiFallback();
      });
    }
    return;
  }

  if (noticeSerial > lastNoticeSerial) {
    lastNoticeSerial = noticeSerial;
    if (!notice.empty())
      showNotice(text(notice), noticeError);
  }
  if (selection && selectionSerial > lastSelectionSerial) {
    lastSelectionSerial = selectionSerial;
    selectGraphThread(std::move(selection));
  }
}

void ShellWidget::Impl::selectGraphThread(nodegraph::NodeRef thread) {
  if (!thread || thread->id().kind != nodegraph::NodeKind::Thread)
    return;
  {
    auto read = session.nodeGraph().tryRead();
    if (!read) {
      QTimer::singleShot(GraphRetryDelayMilliseconds, owner,
                         [this, thread = std::move(thread)]() mutable {
                           selectGraphThread(std::move(thread));
                         });
      return;
    }
    if (read->find(thread->id()) != thread || read->removed(thread))
      return;
  }
  const std::string targetId = thread->id().canonical;
  if (!optimisticCreationThreadId.empty() &&
      middleRegion->threads().isOptimisticThread(optimisticCreationThreadId)) {
    bool creationTarget = optimisticCreationThreadId == targetId;
    if (!creationTarget) {
      auto read = session.nodeGraph().tryRead();
      if (!read) {
        QTimer::singleShot(GraphRetryDelayMilliseconds, owner,
                           [this, thread = std::move(thread)]() mutable {
                             selectGraphThread(std::move(thread));
                           });
        return;
      }
      if (read->find(thread->id()) != thread || read->removed(thread))
        return;
      for (const nodegraph::NodeRef &prompt :
           read->related(thread, nodegraph::RelationKind::PendingPrompt)) {
        if (prompt && !read->removed(prompt) &&
            graphString(
                graphField(*read->state(prompt), "creationCorrelation")) ==
                creationDraftCorrelation) {
          creationTarget = true;
          break;
        }
      }
    }
    if (!creationTarget)
      return;
    if (creationTarget) {
      const bool draftStillSelected =
          middleRegion->threads().visiblySelectedThreadId() ==
          optimisticCreationThreadId;
      if (optimisticCreationThreadId != targetId) {
        middleRegion->threads().promoteOptimisticThread(
            optimisticCreationThreadId, targetId);
        optimisticCreationThreadId = targetId;
      }
      newThreadDraft.reset();
      if (!draftStillSelected)
        return;
    }
  }
  newThreadDraft.reset();
  selectedGraphThreadId = targetId;
  bindGraphPanes(std::move(thread));
}

void ShellWidget::Impl::reconcileOptimisticCreation(
    const nodegraph::GraphChanged &change) {
  if (optimisticCreationThreadId.empty())
    return;

  nodegraph::NodeRef prompt;
  nodegraph::NodeRef thread;
  std::string dispatchState;
  std::uint64_t newestSubmission = 0;
  if (auto read = session.nodeGraph().tryRead()) {
    const auto consider = [&](const nodegraph::NodeRef &candidate) {
      if (!candidate || candidate->id().kind != nodegraph::NodeKind::Item ||
          read->find(candidate->id()) != candidate)
        return;
      const auto state = read->state(candidate);
      if (graphString(graphField(*state, "type")) != "localPrompt" ||
          graphString(graphField(*state, "creationCorrelation")) !=
              creationDraftCorrelation)
        return;
      const std::int64_t rawSubmission =
          graphInteger(graphField(*state, "submissionId")).value_or(0);
      const std::uint64_t submission =
          rawSubmission < 0 ? 0 : static_cast<std::uint64_t>(rawSubmission);
      nodegraph::NodeRef candidateTurn = read->parent(candidate);
      nodegraph::NodeRef candidateThread =
          candidateTurn ? read->parent(candidateTurn) : nodegraph::NodeRef{};
      if (!candidateThread ||
          candidateThread->id().kind != nodegraph::NodeKind::Thread ||
          (prompt && submission < newestSubmission))
        return;
      prompt = candidate;
      thread = std::move(candidateThread);
      newestSubmission = submission;
      dispatchState = graphString(graphField(*state, "dispatchState"));
    };
    if (change.rescanRequired) {
      const nodegraph::NodeRef runtime =
          read->find({nodegraph::NodeKind::Runtime, "runtime"});
      for (const nodegraph::NodeRef &candidate :
           read->related(runtime, nodegraph::RelationKind::PendingPrompt))
        consider(candidate);
    } else {
      for (const nodegraph::NodeRef &candidate : change.affected)
        consider(candidate);
    }
  } else {
    QTimer::singleShot(GraphRetryDelayMilliseconds, owner,
                       [this, change] { reconcileOptimisticCreation(change); });
    return;
  }
  if (!prompt || !thread)
    return;

  const std::string targetId = thread->id().canonical;
  const bool selected = middleRegion->threads().visiblySelectedThreadId() ==
                        optimisticCreationThreadId;
  if (optimisticCreationThreadId != targetId &&
      middleRegion->threads().isOptimisticThread(optimisticCreationThreadId)) {
    middleRegion->threads().promoteOptimisticThread(optimisticCreationThreadId,
                                                    targetId);
    optimisticCreationThreadId = targetId;
  }
  if (selected) {
    newThreadDraft.reset();
    selectedGraphThreadId = targetId;
    bindGraphPanes(thread);
  }

  if (dispatchState == "awaitingMaterialization") {
    middleRegion->threads().confirmOptimisticThread(targetId);
    optimisticCreationThreadId.clear();
    creationDraftCorrelation.clear();
    creationInFlight = false;
  } else if (dispatchState == "failed" || dispatchState == "uncertain") {
    middleRegion->threads().failOptimisticThread(targetId);
    optimisticCreationThreadId.clear();
    creationDraftCorrelation.clear();
    creationInFlight = false;
  }
}

void ShellWidget::Impl::handleUiEffect(const nodegraph::UiEffect &effect) {
  switch (effect.kind) {
  case nodegraph::UiEffectKind::ShowNotice: {
    const std::int64_t rawSerial =
        graphInteger(graphField(effect.details, "serial")).value_or(0);
    if (rawSerial > 0) {
      const auto serial = static_cast<std::uint64_t>(rawSerial);
      if (serial <= lastNoticeSerial)
        break;
      lastNoticeSerial = serial;
    }
    showNotice(text(effect.text),
               graphString(graphField(effect.details, "severity")) !=
                   "warning");
    break;
  }
  case nodegraph::UiEffectKind::SelectThread: {
    const std::int64_t rawSerial =
        graphInteger(graphField(effect.details, "serial")).value_or(0);
    if (rawSerial > 0) {
      const auto serial = static_cast<std::uint64_t>(rawSerial);
      if (serial <= lastSelectionSerial)
        break;
      lastSelectionSerial = serial;
    }
    if (effect.target &&
        (*effect.target)->id().kind == nodegraph::NodeKind::Thread)
      selectGraphThread(*effect.target);
    break;
  }
  case nodegraph::UiEffectKind::ProtocolDiagnostic:
    middleRegion->inspector().appendProtocolDiagnostic(effect);
    return;
  }
  scheduleRender();
}

void ShellWidget::Impl::scheduleDraftSelection(bool replenishRetry) {
  if (replenishRetry)
    draftSelectionRetriesRemaining = 1;
  if (draftSelectionScheduled || !graphPanesBound || !newThreadDraft)
    return;
  draftSelectionScheduled = true;
  const auto token = alive;
  const int delay = replenishRetry ? 0 : GraphRetryDelayMilliseconds;
  QTimer::singleShot(delay, owner, [this, token] {
    if (!*token)
      return;
    draftSelectionScheduled = false;
    if (!graphPanesBound || !newThreadDraft)
      return;

    auto *threadList = middleRegion->threads().findChild<QListWidget *>(
        QStringLiteral("threadList"));
    if (!threadList)
      return;
    for (int row = 0; row < threadList->count(); ++row) {
      QListWidgetItem *item = threadList->item(row);
      if (item->data(Qt::UserRole).toString() !=
          QString::fromUtf8(DraftThreadId))
        continue;
      if (threadList->currentItem() != item)
        threadList->setCurrentItem(item);
      draftSelectionRetriesRemaining = 0;
      return;
    }
    if (draftSelectionRetriesRemaining != 0) {
      --draftSelectionRetriesRemaining;
      scheduleDraftSelection(false);
    }
  });
}

bool ShellWidget::Impl::sendNodeAction(nodegraph::NodeAction action,
                                       QString rejection) {
  const nodegraph::ChannelSendStatus status = session.sendNodeAction(action);
  if (!nodegraph::deliveryGuaranteed(status)) {
    if (!rejection.isEmpty())
      showNotice(std::move(rejection));
    return false;
  }
  if (nodegraph::wakeFailed(status))
    showNotice(QStringLiteral(
        "The action was admitted after a worker wake-up failure; bounded "
        "fallback delivery is active."));
  return true;
}

bool ShellWidget::Impl::sendRuntimeAction(nodegraph::RuntimeAction action,
                                          QString rejection) {
  const nodegraph::ChannelSendStatus status = session.sendRuntimeAction(action);
  if (!nodegraph::deliveryGuaranteed(status)) {
    if (!rejection.isEmpty())
      showNotice(std::move(rejection));
    return false;
  }
  if (nodegraph::wakeFailed(status))
    showNotice(QStringLiteral(
        "The action was admitted after a worker wake-up failure; bounded "
        "fallback delivery is active."));
  return true;
}

void ShellWidget::Impl::hydrateSelectedThreadIfNeeded(
    nodegraph::NodeRef thread) {
  if (!thread || boundGraphThread != thread)
    return;
  auto read = session.nodeGraph().tryRead();
  if (!read) {
    QTimer::singleShot(GraphRetryDelayMilliseconds, owner,
                       [this, thread = std::move(thread)]() mutable {
                         hydrateSelectedThreadIfNeeded(std::move(thread));
                       });
    return;
  }
  if (read->find(thread->id()) != thread || read->removed(thread))
    return;
  const bool recoveryOnly =
      graphBool(graphField(*read->state(thread), "recoveryOnly"));
  read.reset();
  if (recoveryOnly)
    return;
  nodegraph::NodeAction action{std::move(thread),
                               nodegraph::NodeActionKind::Hydrate};
  static_cast<void>(sendNodeAction(
      std::move(action),
      QStringLiteral(
          "Thread loading was not admitted; select Reload to retry.")));
}

nodegraph::NodeRef ShellWidget::Impl::activeTurn() const {
  if (!boundGraphThread)
    return {};
  auto read = session.nodeGraph().tryRead();
  if (!read || read->removed(boundGraphThread))
    return {};
  nodegraph::NodeRef indexed =
      read->relatedAt(boundGraphThread, nodegraph::RelationKind::ActiveTurn, 0);
  if (indexed && indexed->id().kind == nodegraph::NodeKind::Turn &&
      activeStatus(*read->state(indexed)))
    return indexed;
  return {};
}

nodegraph::NodeRef ShellWidget::Impl::threadById(const std::string &id,
                                                 bool *busy) const {
  if (busy)
    *busy = false;
  if (id.empty() || id == DraftThreadId)
    return {};
  auto read = session.nodeGraph().tryRead();
  if (!read) {
    if (busy)
      *busy = true;
    return {};
  }
  const nodegraph::NodeRef thread =
      read->find({nodegraph::NodeKind::Thread, id});
  return thread && !read->removed(thread) ? thread : nodegraph::NodeRef{};
}

std::optional<PendingGraphRequest>
ShellWidget::Impl::pendingRequest(const std::string &requestKey, bool *busy) {
  if (busy)
    *busy = false;
  bool canControl = false;
  std::optional<PendingGraphRequestSnapshot> snapshot;
  {
    auto read = session.nodeGraph().tryRead();
    if (!read) {
      if (busy)
        *busy = true;
      scheduleRender();
      return std::nullopt;
    }

    if (const nodegraph::NodeRef connection =
            read->find({nodegraph::NodeKind::Connection, "connection"})) {
      const auto state = read->state(connection);
      canControl =
          state->status == nodegraph::NodeStatus::Connected &&
          graphString(graphField(*state, "providerState")) == "ready" &&
          graphString(graphField(*state, "role")) == "controller";
    }
    snapshot = readPendingRequest(*read, boundGraphThread, requestKey);
  }
  if (!snapshot)
    return std::nullopt;
  // A rejected bridge write remains Failed and actionable while the worker
  // still owns the exact server request. Nothing retries it automatically.
  return materializePendingRequest(std::move(*snapshot), canControl);
}

void ShellWidget::Impl::render() {
  ShellChromeValues values;
  values.title = "Select a thread";
  values.workspace = "No workspace";
  std::shared_ptr<const nodegraph::NodeState> selectedSettingsState;
  std::shared_ptr<const nodegraph::NodeState> modelCatalogState;
  std::shared_ptr<const nodegraph::NodeState> permissionProfileCatalogState;
  bool modelCatalogChanged = false;
  bool nextModelCatalogPresent = false;
  std::uint64_t nextModelCatalogRevision = 0;
  bool permissionProfileCatalogChanged = false;
  bool nextPermissionProfileCatalogPresent = false;
  std::uint64_t nextPermissionProfileCatalogRevision = 0;
  std::optional<PendingGraphRequestSnapshot> attentionSnapshot;

  {
    auto read = session.nodeGraph().tryRead();
    if (!read) {
      scheduleRender();
      return;
    }
    if (const nodegraph::NodeRef connection =
            read->find({nodegraph::NodeKind::Connection, "connection"})) {
      const auto state = read->state(connection);
      const std::string transport =
          graphString(graphField(*state, "transportState"));
      values.connected = state->status == nodegraph::NodeStatus::Connected ||
                         transport == "connected";
      values.retrying = transport == "retrying" || transport == "connecting";
      values.role = graphString(graphField(*state, "role"));
      values.providerState = graphString(graphField(*state, "providerState"));
      values.canControl = values.connected && values.providerState == "ready" &&
                          values.role == "controller";
      if (const nodegraph::Value *settings = graphField(*state, "settings")) {
        if (const auto *object = settings->asObject()) {
          const std::string selected =
              graphString(graphField(*object, "selected"));
          const nodegraph::Value *available = graphField(*object, "available");
          if (available && available->asArray()) {
            for (const nodegraph::Value &entry : *available->asArray()) {
              const auto *transportEntry = entry.asObject();
              if (transportEntry &&
                  graphString(graphField(*transportEntry, "key")) == selected) {
                values.selectedTransport =
                    graphString(graphField(*transportEntry, "label"));
                break;
              }
            }
          }
        }
      }
    }

    if (const nodegraph::NodeRef runtime =
            read->find({nodegraph::NodeKind::Runtime, "runtime"})) {
      values.totalPending = read->relatedCount(
          runtime, nodegraph::RelationKind::PendingInteraction);
    }

    nodegraph::NodeRef selected = boundGraphThread;
    if (selected && read->removed(selected))
      selected.reset();
    if (selected) {
      const auto state = read->state(selected);
      values.title = graphString(graphField(*state, "name"));
      if (values.title.empty())
        values.title = graphString(graphField(*state, "title"));
      if (values.title.empty())
        values.title = "Untitled thread";
      values.workspace = graphString(graphField(*state, "cwd"));
      if (values.workspace.empty())
        values.workspace = graphString(graphField(*state, "workspace"));
      if (values.workspace.empty())
        values.workspace = "No workspace";
      values.status = graphStatus(*state);
      values.recoveryOnly = graphBool(graphField(*state, "recoveryOnly"));
      const std::string hydrationState =
          graphString(graphField(*state, "hydrationState"));
      const bool local = graphBool(graphField(*state, "local"));
      values.conversationReadyForDisplay =
          hydrationState == "ready" || local || values.recoveryOnly;
      values.hydrationFailed = hydrationState == "failed";
      values.threadAdmissionReady =
          !values.recoveryOnly && hydrationState != "loading" &&
          hydrationState != "failed" &&
          (state->status != nodegraph::NodeStatus::NotLoaded ||
           hydrationState == "ready");
      values.lastActivityAt =
          graphInteger(graphField(*state, "lastActivityAt"));
      for (const std::string_view field :
           {std::string_view("recencyAt"), std::string_view("updatedAt"),
            std::string_view("localActivityAt"),
            std::string_view("localPromptActivityAt")}) {
        const std::optional<std::int64_t> timestamp =
            graphInteger(graphField(*state, field));
        if (timestamp &&
            (!values.lastActivityAt || *timestamp > *values.lastActivityAt))
          values.lastActivityAt = timestamp;
      }
      selectedSettingsState = state;
      const std::optional<std::int64_t> settingsRevision =
          graphInteger(graphField(*state, "settingsRevision"));
      if (settingsRevision && *settingsRevision >= 0) {
        values.settingsRevision = static_cast<std::uint64_t>(*settingsRevision);
      } else {
        for (const std::string_view field :
             {std::string_view("model"), std::string_view("effort"),
              std::string_view("reasoningEffort"),
              std::string_view("personality"), std::string_view("sandbox"),
              std::string_view("sandboxPolicy"),
              std::string_view("approvalPolicy"),
              std::string_view("approvalsReviewer"), std::string_view("cwd"),
              std::string_view("activePermissionProfile"),
              std::string_view("serviceTier"), std::string_view("summary"),
              std::string_view("collaborationMode")})
          values.settingsRevision =
              std::max(values.settingsRevision,
                       read->fieldChangedRevision(selected, field));
      }
      nodegraph::NodeRef turn =
          read->relatedAt(selected, nodegraph::RelationKind::ActiveTurn, 0);
      values.activeTurn = turn &&
                          turn->id().kind == nodegraph::NodeKind::Turn &&
                          activeStatus(*read->state(turn));
    } else if (newThreadDraft) {
      values.title = newThreadDraft->name.trimmed().isEmpty()
                         ? "New thread"
                         : utf8(newThreadDraft->name.trimmed());
      values.workspace = utf8(newThreadDraft->workspace);
      values.canonicalSettings = nlohmann::json{{"cwd", values.workspace}};
    }

    const nodegraph::NodeRef modelCatalog =
        read->find({nodegraph::NodeKind::Catalog, "model"});
    nextModelCatalogPresent = modelCatalog != nullptr;
    if (modelCatalog)
      nextModelCatalogRevision = read->changedRevision(modelCatalog);
    modelCatalogChanged =
        !modelCatalogInitialized ||
        modelCatalogPresent != nextModelCatalogPresent ||
        (modelCatalog && modelCatalogRevision != nextModelCatalogRevision);
    if (modelCatalogChanged && modelCatalog)
      modelCatalogState = read->state(modelCatalog);

    const nodegraph::NodeRef permissionProfileCatalog =
        read->find({nodegraph::NodeKind::Catalog, "permissionProfile"});
    nextPermissionProfileCatalogPresent = permissionProfileCatalog != nullptr;
    if (permissionProfileCatalog) {
      nextPermissionProfileCatalogRevision =
          read->changedRevision(permissionProfileCatalog);
    }
    permissionProfileCatalogChanged =
        !permissionProfileCatalogInitialized ||
        permissionProfileCatalogPresent !=
            nextPermissionProfileCatalogPresent ||
        (permissionProfileCatalog && permissionProfileCatalogRevision !=
                                         nextPermissionProfileCatalogRevision);
    if (permissionProfileCatalogChanged && permissionProfileCatalog)
      permissionProfileCatalogState = read->state(permissionProfileCatalog);

    attentionSnapshot =
        readPendingRequest(*read, boundGraphThread, std::string{});
  }

  if (selectedSettingsState) {
    values.canonicalSettings = widgetSettingsJson(*selectedSettingsState);
    if (const nodegraph::Value *update =
            graphField(*selectedSettingsState, "latestSettingsUpdate");
        update && update->asObject())
      values.settingsUpdate = widgetJson(*update);
  }

  if (attentionSnapshot)
    values.attention = materializePendingRequest(std::move(*attentionSnapshot),
                                                 values.canControl);
  const bool chromeChanged = !renderedChrome || *renderedChrome != values;

  TurnSettingsWidget *turnSettings = middleRegion->composer().turnSettings();
  if (modelCatalogChanged) {
    turnSettings->setModelCatalog(
        catalogArray(modelCatalogState, {"models", "data"}));
    modelCatalogInitialized = true;
    modelCatalogPresent = nextModelCatalogPresent;
    modelCatalogRevision = nextModelCatalogRevision;
  }
  if (permissionProfileCatalogChanged) {
    turnSettings->setPermissionProfileCatalog(catalogArray(
        permissionProfileCatalogState, {"permissionProfiles", "data"}));
    permissionProfileCatalogInitialized = true;
    permissionProfileCatalogPresent = nextPermissionProfileCatalogPresent;
    permissionProfileCatalogRevision = nextPermissionProfileCatalogRevision;
  }
  if (!chromeChanged)
    return;

  const std::string &presentedThreadId =
      middleRegion->conversation().presentedThreadId();
  const bool replacementHydrating =
      boundGraphThread && !presentedThreadId.empty() &&
      boundGraphThread->id().canonical != presentedThreadId &&
      !values.hydrationFailed;

  attentionInteraction =
      values.attention ? values.attention->node : nodegraph::NodeRef{};
  if (!replacementHydrating) {
    middleRegion->conversation().setEmptyMessage(
        values.recoveryOnly
            ? QStringLiteral(
                  "This thread preserves an unsent prompt. Restore it from "
                  "the failed prompt card before continuing.")
        : boundGraphThread && values.hydrationFailed
            ? QStringLiteral("Thread loading failed. Select Reload to retry.")
        : boundGraphThread && !values.conversationReadyForDisplay
            ? QStringLiteral("Loading conversation…")
        : boundGraphThread
            ? QStringLiteral("Conversation activity appears here.")
            : (newThreadDraft
                   ? QStringLiteral("Send a message to create this thread.")
                   : QStringLiteral("Conversation activity appears here.")));
    if (boundGraphThread) {
      const QString activity = values.lastActivityAt
                                   ? lastActivityText(*values.lastActivityAt)
                                   : QString{};
      const QString status = text(values.status);
      const QString tone = values.activeTurn ? QStringLiteral("active")
                                             : QStringLiteral("neutral");
      middleRegion->setThreadHeading(text(values.title), text(values.workspace),
                                     activity, status, tone);
    } else {
      middleRegion->setThreadHeading(text(values.title),
                                     text(values.workspace));
    }
    turnSettings->setCanonicalContext(
        boundGraphThread ? boundGraphThread->id().canonical
                         : std::string(DraftThreadId),
        values.canonicalSettings, values.settingsRevision,
        values.settingsUpdate);
  }
  renderStatus(values, !replacementHydrating);
  renderedChrome = values;
  ++shellRenderCommits;
  owner->setProperty("shellRenderCommits",
                     static_cast<qulonglong>(shellRenderCommits));
  if (newThreadDraft)
    scheduleDraftSelection();
}

void ShellWidget::Impl::renderStatus(const ShellChromeValues &status,
                                     bool updateWorkspace) {
  QString dotStyle;
  QString dotTip;
  if (status.connected) {
    dotStyle = QStringLiteral("background:%1;border-radius:5px;")
                   .arg(QString::fromLatin1(UiStyle::green));
    dotTip = QStringLiteral("Connected");
  } else if (status.retrying) {
    dotStyle = QStringLiteral("background:%1;border-radius:5px;")
                   .arg(QString::fromLatin1(UiStyle::orange));
    dotTip = QStringLiteral("Disconnected, retrying");
  } else {
    dotStyle = QStringLiteral("background:%1;border-radius:5px;")
                   .arg(QString::fromLatin1(UiStyle::red));
    dotTip = QStringLiteral("Disconnected");
  }
  if (connectionStatusDot->styleSheet() != dotStyle)
    connectionStatusDot->setStyleSheet(dotStyle);
  if (connectionStatusDot->toolTip() != dotTip)
    connectionStatusDot->setToolTip(dotTip);
  const QString connectionText = status.selectedTransport.empty()
                                     ? QStringLiteral("Connection")
                                     : text(status.selectedTransport);
  if (connectionButton->text() != connectionText)
    connectionButton->setText(connectionText);
  const QString connectionTip =
      status.connected ? QStringLiteral("Connected bridge transport")
                       : QStringLiteral("Disconnected bridge transport");
  if (connectionButton->toolTip() != connectionTip)
    connectionButton->setToolTip(connectionTip);
  connectAction->setEnabled(!status.connected);
  disconnectAction->setEnabled(status.connected);
  reconnectAction->setEnabled(status.connected);
  const QString controllerText = status.role == "controller"
                                     ? QStringLiteral("Release control")
                                     : QStringLiteral("Claim control");
  if (controllerButton->text() != controllerText)
    controllerButton->setText(controllerText);
  controllerButton->setEnabled(status.connected);

  const QString requestText =
      QStringLiteral("Requests (%1)")
          .arg(static_cast<qulonglong>(status.totalPending));
  if (requestButton->text() != requestText)
    requestButton->setText(requestText);
  requestButton->setVisible(status.totalPending != 0);
  if (status.attention) {
    const PendingGraphRequest &request = *status.attention;
    const nlohmann::json raw = widgetJson(nodegraph::Value(request.payload));
    middleRegion->composer().setAttentionRequest(
        text(PendingRequestPolicy::title(request.kind)),
        text(PendingRequestPolicy::detail(request.displayId, request.threadId,
                                          raw)),
        PendingRequestPolicy::supportsDirectAccept(request.kind),
        text(PendingRequestPolicy::directAcceptLabel(request.kind)));
  }
  middleRegion->composer().setAttentionVisible(status.attention.has_value());
  middleRegion->composer().setAttentionActionEnabled(
      status.attention && status.attention->actionable,
      status.attention && status.attention->recoverable);

  QString globalStatus = QStringLiteral("Ready");
  QString globalTone = QStringLiteral("success");
  if (status.retrying) {
    globalStatus = QStringLiteral("Reconnecting");
    globalTone = QStringLiteral("warning");
  } else if (!status.connected) {
    globalStatus = QStringLiteral("Offline");
    globalTone = QStringLiteral("danger");
  } else if (status.providerState != "ready") {
    globalStatus = status.providerState.empty()
                       ? QStringLiteral("Waiting for provider")
                       : QStringLiteral("Provider unavailable");
    globalTone = status.providerState.empty() ? QStringLiteral("warning")
                                              : QStringLiteral("danger");
  } else if (status.totalPending != 0) {
    globalStatus = QStringLiteral("Attention required");
    globalTone = QStringLiteral("warning");
  }
  setStatusTone(globalStatusDot, globalStatusLabel, globalTone);
  setStatusLabelText(globalStatusLabel, globalStatus);

  if (updateWorkspace) {
    const QString workspace = text(status.workspace);
    if (workspaceBreadcrumb->toolTip() != workspace)
      workspaceBreadcrumb->setToolTip(workspace);
    const QString displayedWorkspace =
        workspaceBreadcrumb->fontMetrics().elidedText(
            workspace, Qt::ElideMiddle, workspaceBreadcrumb->maximumWidth());
    if (workspaceBreadcrumb->text() != displayedWorkspace)
      workspaceBreadcrumb->setText(displayedWorkspace);
  }

  middleRegion->composer().setActiveTurn(status.activeTurn);
  middleRegion->composer().setCanSubmit(
      status.canControl && (boundGraphThread || newThreadDraft) &&
      (status.activeTurn || status.threadAdmissionReady));
  middleRegion->composer().setSettingsEnabled(
      status.canControl && status.threadAdmissionReady &&
      (boundGraphThread || newThreadDraft) && !status.activeTurn);
}

void ShellWidget::Impl::beginNewThreadDialog() {
  if (creationInFlight) {
    showNotice(QStringLiteral("A new thread is already being created. Wait for "
                              "it to finish before "
                              "starting another."),
               false);
    return;
  }
  const QString fallback = QDir::currentPath();
  const QString initial =
      text(middleRegion->composer().turnSettings()->workspace(utf8(fallback)));
  NewThreadDialog dialog(initial, owner);
  if (dialog.exec() != QDialog::Accepted)
    return;
  NewThreadDraft draft = dialog.draft();
  middleRegion->composer().turnSettings()->setWorkspace(draft.workspace);
  newThreadDraft = draft;
  creationDraftCorrelation =
      "qt-draft:" + std::to_string(nextCreationDraftSerial++);
  optimisticCreationThreadId = DraftThreadId;
  creationInFlight = false;
  selectedGraphThreadId.clear();
  bindGraphPanes({});
  middleRegion->threads().beginOptimisticThread(
      DraftThreadId,
      draft.name.trimmed().isEmpty() ? "New thread"
                                     : utf8(draft.name.trimmed()),
      utf8(draft.workspace));
  if (auto threads = uiAdapter.threads({}))
    middleRegion->threads().refresh(*threads);
  scheduleDraftSelection();
  middleRegion->composer().clearDraft();
  middleRegion->composer().promptEditor()->setFocus();
  render();
}

void ShellWidget::Impl::renameThreadDialog(const nodegraph::NodeRef &thread) {
  if (!thread || thread->id().kind != nodegraph::NodeKind::Thread)
    return;
  std::string currentName;
  if (auto read = session.nodeGraph().tryRead()) {
    if (read->removed(thread))
      return;
    const auto state = read->state(thread);
    currentName = graphString(graphField(*state, "name"));
    if (currentName.empty())
      currentName = graphString(graphField(*state, "title"));
  } else {
    showNotice(QStringLiteral("Thread state is busy; try again."));
    return;
  }
  const auto retained = retainedRenames.find(thread.get());
  const QString initialName = retained == retainedRenames.end()
                                  ? text(currentName)
                                  : retained->second.second;
  bool accepted = false;
  const QString name =
      QInputDialog::getText(owner, QStringLiteral("Rename thread"),
                            QStringLiteral("Name"), QLineEdit::Normal,
                            initialName, &accepted)
          .trimmed();
  if (!accepted || name.isEmpty())
    return;
  nodegraph::NodeAction action{thread, nodegraph::NodeActionKind::Rename};
  action.payload.emplace("name", utf8(name));
  const nodegraph::ChannelSendStatus status = session.sendNodeAction(action);
  if (!nodegraph::deliveryGuaranteed(status)) {
    retainedRenames.insert_or_assign(thread.get(), std::pair{thread, name});
    showNotice(QStringLiteral(
        "Rename request was not admitted. Reopen Rename to recover the "
        "name you entered."));
    return;
  }
  retainedRenames.erase(thread.get());
  if (nodegraph::wakeFailed(status))
    showNotice(QStringLiteral(
        "The rename was admitted after a worker wake-up failure; bounded "
        "fallback delivery is active."));
}

void ShellWidget::Impl::confirmDeleteThread(const nodegraph::NodeRef &thread) {
  if (!thread || thread->id().kind != nodegraph::NodeKind::Thread)
    return;
  if (QMessageBox::question(owner, QStringLiteral("Delete thread"),
                            QStringLiteral("Delete the selected thread?"),
                            QMessageBox::Yes | QMessageBox::Cancel,
                            QMessageBox::Cancel) == QMessageBox::Yes)
    static_cast<void>(sendNodeAction(
        {thread, nodegraph::NodeActionKind::Delete},
        QStringLiteral("Delete request was not admitted; try again.")));
}

bool ShellWidget::Impl::submitPrompt(QString prompt,
                                     std::vector<AttachmentDraft> attachments) {
  prompt = prompt.trimmed();
  if (prompt.isEmpty())
    return false;
  TurnSettingsWidget *settings = middleRegion->composer().turnSettings();
  std::vector<nodegraph::Attachment> ownedAttachments;
  ownedAttachments.reserve(attachments.size());
  for (AttachmentDraft &attachment : attachments) {
    ownedAttachments.push_back({std::move(attachment.path),
                                std::move(attachment.name),
                                std::move(attachment.mimeType), std::nullopt});
  }

  bool admitted = false;
  const std::string visibleThreadId =
      middleRegion->threads().visiblySelectedThreadId();
  if (nodegraph::NodeRef target = threadById(visibleThreadId)) {
    QString graphRejection;
    if (auto read = session.nodeGraph().tryRead()) {
      if (read->removed(target)) {
        graphRejection =
            QStringLiteral("The selected thread is no longer available. Your "
                           "message was not sent.");
      } else {
        const auto state = read->state(target);
        const std::string hydration =
            graphString(graphField(*state, "hydrationState"));
        const nodegraph::NodeRef turn =
            read->relatedAt(target, nodegraph::RelationKind::ActiveTurn, 0);
        const bool steeringKnownActiveTurn =
            turn && turn->id().kind == nodegraph::NodeKind::Turn &&
            activeStatus(*read->state(turn));
        if (!steeringKnownActiveTurn &&
            (hydration == "loading" || hydration == "failed" ||
             (state->status == nodegraph::NodeStatus::NotLoaded &&
              hydration != "ready"))) {
          const std::string detail =
              graphString(graphField(*state, "hydrationError"));
          graphRejection = text(
              detail.empty()
                  ? "This thread must finish loading before sending. Your "
                    "draft is still available."
                  : detail +
                        ". Reload the thread; your draft is still available.");
        }
      }
    } else {
      graphRejection = QStringLiteral(
          "Thread state is busy. Your draft is still available; try again.");
    }
    // The graph read guard above is gone before any QWidget is touched.
    if (!graphRejection.isEmpty()) {
      showNotice(std::move(graphRejection));
      return false;
    }
    nodegraph::NodeAction action{target,
                                 nodegraph::NodeActionKind::SubmitPrompt};
    action.promptText = utf8(prompt);
    action.attachments = std::move(ownedAttachments);
    action.payload = actionObject(settings->turnStartOptions());
    admitted = sendNodeAction(
        std::move(action),
        QStringLiteral("Your message was not sent; the worker queue is full."));
  } else if (visibleThreadId == DraftThreadId && newThreadDraft) {
    nodegraph::RuntimeAction action;
    action.kind = nodegraph::RuntimeActionKind::CreateThread;
    action.correlation = creationDraftCorrelation;
    action.promptText = utf8(prompt);
    action.attachments = std::move(ownedAttachments);
    nodegraph::Value::Object threadStart =
        actionObject(settings->threadStartOptions());
    threadStart.insert_or_assign(
        "cwd", settings->workspace(utf8(QDir::currentPath())));
    if (!newThreadDraft->name.trimmed().isEmpty())
      action.payload.insert_or_assign("requestedName",
                                      utf8(newThreadDraft->name));
    if (!newThreadDraft->baseInstructions.isEmpty())
      threadStart.insert_or_assign("baseInstructions",
                                   utf8(newThreadDraft->baseInstructions));
    if (!newThreadDraft->developerInstructions.isEmpty())
      threadStart.insert_or_assign("developerInstructions",
                                   utf8(newThreadDraft->developerInstructions));
    if (newThreadDraft->ephemeral)
      threadStart.insert_or_assign("ephemeral", true);
    action.payload.insert_or_assign("threadStart",
                                    nodegraph::Value(std::move(threadStart)));
    action.payload.insert_or_assign(
        "turnStart",
        nodegraph::Value(actionObject(settings->turnStartOptions())));
    admitted = sendRuntimeAction(
        std::move(action),
        QStringLiteral("Your message was not sent; the worker queue is full."));
    if (admitted)
      creationInFlight = true;
  } else {
    showNotice(QStringLiteral(
        "No destination thread is selected. Your message was not sent."));
  }
  if (admitted)
    middleRegion->conversation().prepareForLocalPromptAdmission();
  return admitted;
}

void ShellWidget::Impl::chooseAttachments() {
  const QString initial =
      text(middleRegion->composer().turnSettings()->workspace(
          utf8(QDir::currentPath())));
  FileSelectionDialog dialog(FileSelectionDialog::Mode::Attachments, initial,
                             middleRegion->composer().attachments(), owner);
  if (dialog.exec() == QDialog::Accepted)
    middleRegion->composer().setAttachments(dialog.selectedAttachments());
}

void ShellWidget::Impl::recoverPrompt(const nodegraph::NodeRef &prompt) {
  if (!prompt || prompt->id().kind != nodegraph::NodeKind::Item)
    return;

  std::shared_ptr<const nodegraph::NodeState> state;
  if (auto read = session.nodeGraph().tryRead()) {
    if (read->removed(prompt))
      return;
    state = read->state(prompt);
    if (!graphBool(graphField(*state, "requiresExplicitRecovery")))
      return;
  } else {
    showNotice(QStringLiteral(
        "Prompt state is busy; the preserved prompt was not changed."));
    return;
  }
  std::string authoredText = graphString(graphField(*state, "authoredText"));
  if (authoredText.empty())
    authoredText = graphString(graphField(*state, "text"));
  std::vector<AttachmentDraft> attachments = graphAttachmentDrafts(*state);

  if (!middleRegion->composer().promptEditor()->toPlainText().isEmpty() ||
      !middleRegion->composer().attachments().empty()) {
    showNotice(QStringLiteral(
        "Clear or send the current draft before restoring this prompt. The "
        "preserved prompt was not changed."));
    return;
  }

  NewThreadDraft draft;
  const nodegraph::Value::Object *threadStart = nullptr;
  if (const nodegraph::Value *value = graphField(*state, "threadStartOptions"))
    threadStart = value->asObject();
  const std::string recoveredWorkspace =
      threadStart ? graphString(graphField(*threadStart, "cwd"))
                  : std::string{};
  draft.workspace =
      recoveredWorkspace.empty()
          ? text(middleRegion->composer().turnSettings()->workspace(
                utf8(QDir::currentPath())))
          : text(recoveredWorkspace);
  const std::string requestedName =
      graphString(graphField(*state, "requestedName"));
  draft.name = requestedName.empty() ? QStringLiteral("Recovered prompt")
                                     : text(requestedName);
  if (threadStart) {
    draft.baseInstructions =
        text(graphString(graphField(*threadStart, "baseInstructions")));
    draft.developerInstructions =
        text(graphString(graphField(*threadStart, "developerInstructions")));
    draft.ephemeral = graphBool(graphField(*threadStart, "ephemeral"));
  }
  middleRegion->composer().turnSettings()->setWorkspace(draft.workspace);
  newThreadDraft = draft;
  creationDraftCorrelation =
      "qt-draft:" + std::to_string(nextCreationDraftSerial++);
  optimisticCreationThreadId = DraftThreadId;
  creationInFlight = false;
  selectedGraphThreadId.clear();
  bindGraphPanes({});
  middleRegion->threads().beginOptimisticThread(DraftThreadId, utf8(draft.name),
                                                utf8(draft.workspace));
  scheduleDraftSelection();
  middleRegion->composer().promptEditor()->setPlainText(text(authoredText));
  middleRegion->composer().setAttachments(std::move(attachments));
  middleRegion->composer().promptEditor()->setFocus();
  showNotice(
      QStringLiteral("The unsent prompt was restored to a new-thread draft."),
      false);
  render();
}

void ShellWidget::Impl::respondToFirstPending(bool approve) {
  bool busy = false;
  const auto request = pendingRequest({}, &busy);
  if (!request) {
    showNotice(
        busy ? QStringLiteral("Request state is busy; no response was sent.")
             : QStringLiteral("The pending request is no longer actionable."));
    return;
  }
  if (!request->actionable) {
    showNotice(QStringLiteral(
        "Controller access is unavailable; no response was sent."));
    return;
  }
  if (approve)
    acceptPending(request->id);
  else
    rejectPending(request->id);
}

void ShellWidget::Impl::reviewPending(const std::string &requestKey) {
  bool busy = false;
  const auto request = pendingRequest(requestKey, &busy);
  if (!request) {
    showNotice(
        busy ? QStringLiteral("Request state is busy; no response was sent.")
             : QStringLiteral("The pending request is no longer actionable."));
    return;
  }
  if (!request->actionable &&
      !(request->recoveryOnly && request->retainedResponsePayload)) {
    showNotice(QStringLiteral(
        "Controller access is unavailable; no response was sent."));
    return;
  }
  const PendingRequestDescriptor presented{
      request->displayId, request->kind, request->threadId, 0,
      widgetJson(nodegraph::Value(request->payload))};
  const auto retained = retainedInteractionResponses.find(request->id);
  std::optional<PendingRequestResponse> graphRetained;
  if (retained == retainedInteractionResponses.end() &&
      request->retainedResponsePayload)
    graphRetained =
        retainedResponse(request->kind, *request->retainedResponsePayload);
  const PendingRequestResponse *initial =
      retained != retainedInteractionResponses.end()
          ? &retained->second
          : (graphRetained ? &*graphRetained : nullptr);
  const auto response =
      PendingRequestDialog::present(presented, owner, initial);
  if (!response)
    return;
  if (request->recoveryOnly) {
    retainedInteractionResponses.insert_or_assign(request->id, *response);
    showNotice(QStringLiteral(
        "The original request ended when the provider changed. Your authored "
        "response remains preserved for review, but was not sent."));
    return;
  }
  nodegraph::NodeAction action{request->node,
                               nodegraph::NodeActionKind::ResolveInteraction};
  action.payload = authoredResponsePayload(request->kind, *response);
  const nodegraph::ChannelSendStatus status = session.sendNodeAction(action);
  if (!nodegraph::deliveryGuaranteed(status)) {
    retainedInteractionResponses.insert_or_assign(request->id, *response);
    showNotice(QStringLiteral(
        "Your response was not admitted. Reopen Review to recover the input "
        "you entered; the request remains pending."));
    return;
  }
  retainedInteractionResponses.erase(request->id);
  if (nodegraph::wakeFailed(status))
    showNotice(QStringLiteral(
        "The response was admitted after a worker wake-up failure; bounded "
        "fallback delivery is active."));
}

void ShellWidget::Impl::acceptPending(const std::string &requestKey) {
  bool busy = false;
  const auto request = pendingRequest(requestKey, &busy);
  if (!request) {
    showNotice(
        busy ? QStringLiteral("Request state is busy; no response was sent.")
             : QStringLiteral("The pending request is no longer actionable."));
    return;
  }
  if (!request->actionable) {
    showNotice(QStringLiteral(
        "Controller access is unavailable; no response was sent."));
    return;
  }
  if (!PendingRequestPolicy::supportsDirectAccept(request->kind)) {
    reviewPending(requestKey);
    return;
  }
  const nlohmann::json raw = widgetJson(nodegraph::Value(request->payload));
  const PendingRequestResponse response =
      PendingRequestPolicy::positiveResponse(request->kind, raw);
  nodegraph::NodeAction action{request->node,
                               nodegraph::NodeActionKind::ResolveInteraction};
  action.payload = authoredResponsePayload(request->kind, response);
  static_cast<void>(sendNodeAction(
      std::move(action),
      QStringLiteral(
          "Approval was not admitted; the request remains pending.")));
}

void ShellWidget::Impl::rejectPending(const std::string &requestKey) {
  bool busy = false;
  const auto request = pendingRequest(requestKey, &busy);
  if (!request) {
    showNotice(
        busy ? QStringLiteral("Request state is busy; no response was sent.")
             : QStringLiteral("The pending request is no longer actionable."));
    return;
  }
  if (!request->actionable) {
    showNotice(QStringLiteral(
        "Controller access is unavailable; no response was sent."));
    return;
  }
  const nlohmann::json raw = widgetJson(nodegraph::Value(request->payload));
  const PendingRequestResponse response =
      PendingRequestPolicy::negativeResponse(request->kind, raw);
  nodegraph::NodeAction action{request->node,
                               nodegraph::NodeActionKind::ResolveInteraction};
  action.payload = authoredResponsePayload(request->kind, response);
  static_cast<void>(sendNodeAction(
      std::move(action),
      QStringLiteral(
          "Rejection was not admitted; the request remains pending.")));
}

ShellWidget::ShellWidget(FrontendSession &session, QWidget *parent)
    : QWidget(parent), impl(nullptr) {
  // Impl installs this widget as the application event filter. Keep the
  // member in a defined null state while Impl builds child widgets: their
  // construction can synchronously pass events through that filter.
  impl = std::make_unique<Impl>(this, session);
}

ShellWidget::~ShellWidget() = default;

bool ShellWidget::eventFilter(QObject *watched, QEvent *event) {
  if (impl && impl->middleRegion->routeScrollEvent(watched, event))
    return true;
  return QWidget::eventFilter(watched, event);
}

} // namespace codexui::codex
