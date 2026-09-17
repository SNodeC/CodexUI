// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/PendingRequestPolicy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <unordered_set>
#include <utility>

namespace codexui::codex {
namespace {

using Tone = PendingRequestActionTone;

struct RequestKindDefinition {
  PendingRequestKind kind;
  std::string_view method;
  std::string_view token;
  std::string_view title;
  std::string_view dialogTitle;
};

constexpr std::array RequestKindDefinitions{
    RequestKindDefinition{PendingRequestKind::CommandApproval,
                          "item/commandExecution/requestApproval",
                          "command-approval", "Command approval requested",
                          "Command approval"},
    RequestKindDefinition{
        PendingRequestKind::FileChangeApproval,
        "item/fileChange/requestApproval", "file-change-approval",
        "File-change approval requested", "File-change approval"},
    RequestKindDefinition{PendingRequestKind::UserInput,
                          "item/tool/requestUserInput", "user-input",
                          "Codex needs input", "Codex needs input"},
    RequestKindDefinition{PendingRequestKind::McpElicitation,
                          "mcpServer/elicitation/request", "mcp-elicitation",
                          "MCP server request", "MCP server request"},
    RequestKindDefinition{PendingRequestKind::PermissionsApproval,
                          "item/permissions/requestApproval",
                          "permissions-approval", "Permission request",
                          "Permission request"},
    RequestKindDefinition{PendingRequestKind::DynamicToolCall, "item/tool/call",
                          "dynamic-tool-call", "Codex request needs attention",
                          "Dynamic tool request"},
    RequestKindDefinition{
        PendingRequestKind::AuthenticationRefresh,
        "account/chatgptAuthTokens/refresh", "authentication-refresh",
        "Codex request needs attention", "Authentication refresh"},
    RequestKindDefinition{
        PendingRequestKind::Attestation, "attestation/generate", "attestation",
        "Codex request needs attention", "Attestation request"},
    RequestKindDefinition{PendingRequestKind::LegacyPatchApproval,
                          "applyPatchApproval", "legacy-patch-approval",
                          "Legacy patch approval", "Legacy patch approval"},
    RequestKindDefinition{PendingRequestKind::LegacyCommandApproval,
                          "execCommandApproval", "legacy-command-approval",
                          "Legacy command approval", "Legacy command approval"},
    RequestKindDefinition{PendingRequestKind::Unsupported,
                          {},
                          "unsupported",
                          "Codex request needs attention",
                          "Unsupported Codex request"}};

const RequestKindDefinition &definition(PendingRequestKind kind) noexcept {
  const auto found = std::ranges::find(RequestKindDefinitions, kind,
                                       &RequestKindDefinition::kind);
  return found == RequestKindDefinitions.end() ? RequestKindDefinitions.back()
                                               : *found;
}

PendingRequestAction action(std::string value, std::string label, Tone tone,
                            bool requiresInput = false) {
  return {std::move(value), std::move(label), tone, requiresInput};
}

const std::array<PendingRequestAction, 4> &approvalActions() {
  static const std::array<PendingRequestAction, 4> actions{
      action("accept", "Accept", Tone::Approve),
      action("acceptForSession", "Accept for session", Tone::Approve),
      action("decline", "Decline", Tone::Danger),
      action("cancel", "Cancel", Tone::Neutral)};
  return actions;
}

std::string stringValue(const nlohmann::json &object, const char *key) {
  if (!object.is_object())
    return {};
  const auto found = object.find(key);
  return found != object.end() && found->is_string() ? found->get<std::string>()
                                                     : std::string{};
}

PendingRequestResponse jsonRpcError(std::string message) {
  return PendingRequestResponse(std::in_place_type<std::string>,
                                std::move(message));
}

std::string truncateUtf8(std::string value, std::size_t maximumBytes) {
  if (value.size() <= maximumBytes)
    return value;
  std::size_t end = maximumBytes;
  while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U)
    --end;
  value.resize(end);
  return value + "...";
}

void appendDetail(std::vector<std::string> &parts, std::string label,
                  std::string value) {
  constexpr std::size_t MaximumValueCharacters = 512;
  if (!value.empty())
    parts.push_back(std::move(label) +
                    truncateUtf8(std::move(value), MaximumValueCharacters));
}

std::string joinDetails(const std::vector<std::string> &parts) {
  std::string result;
  for (const std::string &part : parts) {
    constexpr std::size_t MaximumDetailCharacters = 4096;
    if (result.size() + part.size() + 5 > MaximumDetailCharacters) {
      result += "  |  ...";
      break;
    }
    if (!result.empty())
      result += "  |  ";
    result += part;
  }
  return result;
}

bool decisionAllowed(const std::vector<PendingRequestAction> &actions,
                     std::string_view decision) {
  return std::ranges::any_of(actions, [decision](const auto &candidate) {
    return candidate.value == decision;
  });
}

bool knownKeys(const nlohmann::json &object,
               std::initializer_list<std::string_view> keys) {
  return object.is_object() &&
         std::ranges::all_of(object.items(), [&](const auto &entry) {
           return std::ranges::find(keys, std::string_view(entry.key())) !=
                  keys.end();
         });
}

bool nonEmptyString(const nlohmann::json &object, const char *key) {
  const auto found = object.is_object() ? object.find(key) : object.end();
  return found != object.end() && found->is_string() && !found->empty() &&
         found->get_ref<const std::string &>().size() <= 4096;
}

bool boundedStringArray(const nlohmann::json &value, bool allowEmpty = false) {
  return value.is_array() && value.size() <= 64 &&
         (allowEmpty || !value.empty()) &&
         std::ranges::all_of(value, [](const nlohmann::json &entry) {
           return entry.is_string() && !entry.empty() &&
                  entry.get_ref<const std::string &>().size() <= 4096;
         });
}

nlohmann::json boundedDisclosureValue(const nlohmann::json &value,
                                      std::size_t depth, std::size_t &remaining,
                                      bool &truncated);

bool validFileSystemPath(const nlohmann::json &value) {
  const std::string type = stringValue(value, "type");
  if (type == "path")
    return knownKeys(value, {"path", "type"}) && nonEmptyString(value, "path");
  if (type == "glob_pattern")
    return knownKeys(value, {"pattern", "type"}) &&
           nonEmptyString(value, "pattern");
  if (type != "special" || !knownKeys(value, {"type", "value"}) ||
      !value.contains("value") || !value.at("value").is_object())
    return false;
  const nlohmann::json &special = value.at("value");
  const std::string kind = stringValue(special, "kind");
  if (kind == "root" || kind == "minimal" || kind == "tmpdir" ||
      kind == "slash_tmp")
    return special.size() == 1;
  const auto optionalSubpath = [&special] {
    const auto found = special.find("subpath");
    return found == special.end() || found->is_null() ||
           (found->is_string() && !found->empty() &&
            found->get_ref<const std::string &>().size() <= 4096);
  };
  if (kind == "project_roots")
    return knownKeys(special, {"kind", "subpath"}) && optionalSubpath();
  return kind == "unknown" && knownKeys(special, {"kind", "path", "subpath"}) &&
         nonEmptyString(special, "path") && optionalSubpath();
}

bool validFileSystemPermissions(const nlohmann::json &permissions) {
  if (!knownKeys(permissions,
                 {"entries", "globScanMaxDepth", "read", "write"}) ||
      permissions.empty())
    return false;
  bool hasRequest = false;
  for (const char *field : {"read", "write"}) {
    const auto found = permissions.find(field);
    if (found == permissions.end() || found->is_null())
      continue;
    if (!boundedStringArray(*found, true))
      return false;
    hasRequest |= !found->empty();
  }
  const auto depth = permissions.find("globScanMaxDepth");
  if (depth != permissions.end() && !depth->is_null()) {
    if (!depth->is_number())
      return false;
    const double value = depth->get<double>();
    if (!std::isfinite(value) || std::trunc(value) != value || value < 1 ||
        value > 1'000'000)
      return false;
  }
  const auto entries = permissions.find("entries");
  if (entries != permissions.end() && !entries->is_null()) {
    if (!entries->is_array() || entries->size() > 64 ||
        !std::ranges::all_of(*entries, [](const nlohmann::json &entry) {
          return knownKeys(entry, {"access", "path"}) &&
                 entry.contains("access") && entry.at("access").is_string() &&
                 (entry.at("access") == "read" ||
                  entry.at("access") == "write" ||
                  entry.at("access") == "deny") &&
                 entry.contains("path") &&
                 validFileSystemPath(entry.at("path"));
        }))
      return false;
    hasRequest |= !entries->empty();
  }
  return hasRequest;
}

bool validPermissionProfile(const nlohmann::json &profile) {
  if (!knownKeys(profile, {"fileSystem", "network"}) || profile.empty())
    return false;
  bool hasRequest = false;
  const auto network = profile.find("network");
  if (network != profile.end() && !network->is_null()) {
    if (!knownKeys(*network, {"enabled"}) || network->size() != 1 ||
        !network->contains("enabled") || !network->at("enabled").is_boolean())
      return false;
    hasRequest = true;
  }
  const auto fileSystem = profile.find("fileSystem");
  if (fileSystem != profile.end() && !fileSystem->is_null()) {
    if (!validFileSystemPermissions(*fileSystem))
      return false;
    hasRequest = true;
  }
  return hasRequest;
}

bool validNetworkApprovalContext(const nlohmann::json &context) {
  if (!knownKeys(context, {"host", "protocol"}) ||
      !nonEmptyString(context, "host") || !nonEmptyString(context, "protocol"))
    return false;
  const std::string protocol = context.at("protocol").get<std::string>();
  return protocol == "http" || protocol == "https" || protocol == "socks5Tcp" ||
         protocol == "socks5Udp";
}

bool validExecPolicyAmendment(const nlohmann::json &amendment) {
  return boundedStringArray(amendment);
}

bool validNetworkAmendment(const nlohmann::json &amendment) {
  return knownKeys(amendment, {"action", "host"}) &&
         nonEmptyString(amendment, "host") &&
         nonEmptyString(amendment, "action") &&
         (amendment.at("action") == "allow" ||
          amendment.at("action") == "deny");
}

bool validCommandActions(const nlohmann::json &actions) {
  if (!actions.is_array() || actions.empty() || actions.size() > 64)
    return false;
  return std::ranges::all_of(actions, [](const nlohmann::json &entry) {
    const std::string type = stringValue(entry, "type");
    if (!nonEmptyString(entry, "command"))
      return false;
    if (type == "read")
      return knownKeys(entry, {"command", "name", "path", "type"}) &&
             nonEmptyString(entry, "name") && nonEmptyString(entry, "path");
    if (type == "listFiles")
      return knownKeys(entry, {"command", "path", "type"}) &&
             (!entry.contains("path") || entry.at("path").is_null() ||
              entry.at("path").is_string());
    if (type == "search")
      return knownKeys(entry, {"command", "path", "query", "type"}) &&
             std::ranges::all_of(
                 std::array{"path", "query"}, [&entry](const char *field) {
                   return !entry.contains(field) || entry.at(field).is_null() ||
                          entry.at(field).is_string();
                 });
    return type == "unknown" && knownKeys(entry, {"command", "type"});
  });
}

bool validNetworkAmendments(const nlohmann::json &amendments) {
  return amendments.is_array() && !amendments.empty() &&
         amendments.size() <= 64 &&
         std::ranges::all_of(amendments, validNetworkAmendment);
}

struct CommandDecisionOption {
  PendingRequestAction action;
  nlohmann::json wireDecision;
};

std::optional<CommandDecisionOption>
simpleCommandDecision(std::string_view value) {
  const auto found =
      std::ranges::find(approvalActions(), value, &PendingRequestAction::value);
  if (found == approvalActions().end())
    return std::nullopt;
  return CommandDecisionOption{*found, found->value};
}

std::string abbreviated(std::string value) {
  constexpr std::size_t MaximumCharacters = 96;
  return truncateUtf8(std::move(value), MaximumCharacters);
}

std::optional<CommandDecisionOption>
structuredCommandDecision(const nlohmann::json &entry,
                          const nlohmann::json &request, std::size_t index) {
  if (!entry.is_object())
    return std::nullopt;
  if (entry.contains("acceptWithExecpolicyAmendment")) {
    const auto &body = entry.at("acceptWithExecpolicyAmendment");
    if (entry.size() != 1 || !knownKeys(body, {"execpolicy_amendment"}) ||
        body.size() != 1 ||
        !validExecPolicyAmendment(body.at("execpolicy_amendment")))
      return std::nullopt;
    const auto proposal = request.is_object()
                              ? request.find("proposedExecpolicyAmendment")
                              : request.end();
    if (proposal == request.end() ||
        *proposal != body.at("execpolicy_amendment"))
      return std::nullopt;
    const std::string prefix =
        abbreviated(proposal->front().get<std::string>());
    return CommandDecisionOption{
        action("acceptWithExecpolicyAmendment:" + std::to_string(index),
               "Accept and allow matching commands: " + prefix, Tone::Approve),
        entry};
  }
  if (entry.contains("applyNetworkPolicyAmendment")) {
    const auto &body = entry.at("applyNetworkPolicyAmendment");
    if (entry.size() != 1 || !knownKeys(body, {"network_policy_amendment"}) ||
        body.size() != 1 ||
        !validNetworkAmendment(body.at("network_policy_amendment")))
      return std::nullopt;
    const auto &amendment = body.at("network_policy_amendment");
    const auto proposals = request.is_object()
                               ? request.find("proposedNetworkPolicyAmendments")
                               : request.end();
    if (proposals == request.end() || !validNetworkAmendments(*proposals) ||
        std::ranges::find(*proposals, amendment) == proposals->end())
      return std::nullopt;
    const bool allow = amendment.at("action") == "allow";
    return CommandDecisionOption{
        action("applyNetworkPolicyAmendment:" + std::to_string(index),
               std::string(allow ? "Allow " : "Block ") +
                   abbreviated(amendment.at("host").get<std::string>()) +
                   " in future",
               allow ? Tone::Approve : Tone::Danger),
        entry};
  }
  return std::nullopt;
}

std::optional<std::vector<CommandDecisionOption>>
commandDecisionOptions(const nlohmann::json &raw) {
  std::vector<CommandDecisionOption> result;
  const auto append = [&result](CommandDecisionOption candidate) {
    if (std::ranges::find(result, candidate.action.value,
                          [](const CommandDecisionOption &entry) {
                            return entry.action.value;
                          }) == result.end())
      result.push_back(std::move(candidate));
  };
  const auto declared =
      raw.is_object() ? raw.find("availableDecisions") : raw.end();
  if (declared != raw.end() && !declared->is_null()) {
    if (!declared->is_array() || declared->empty() || declared->size() > 64)
      return std::nullopt;
    for (std::size_t index = 0; index < declared->size(); ++index) {
      const nlohmann::json &entry = declared->at(index);
      if (entry.is_string()) {
        if (auto candidate =
                simpleCommandDecision(entry.get_ref<const std::string &>()))
          append(std::move(*candidate));
        continue;
      }
      if (!entry.is_object())
        return std::nullopt;
      const bool recognized = entry.contains("acceptWithExecpolicyAmendment") ||
                              entry.contains("applyNetworkPolicyAmendment");
      auto candidate = structuredCommandDecision(entry, raw, index);
      if (recognized && !candidate)
        return std::nullopt;
      if (candidate)
        append(std::move(*candidate));
    }
    if (result.empty())
      return std::nullopt;
    return result;
  }

  append(*simpleCommandDecision("accept"));
  if (raw.is_object()) {
    const auto networkContext = raw.find("networkApprovalContext");
    const auto network = raw.find("proposedNetworkPolicyAmendments");
    const auto permissions = raw.find("additionalPermissions");
    if (networkContext != raw.end() && !networkContext->is_null()) {
      append(*simpleCommandDecision("acceptForSession"));
      if (network != raw.end() && validNetworkAmendments(*network)) {
        const auto allowed =
            std::ranges::find(*network, "allow", [](const auto &entry) {
              return stringValue(entry, "action");
            });
        if (allowed != network->end()) {
          append(*structuredCommandDecision(
              {{"applyNetworkPolicyAmendment",
                {{"network_policy_amendment", *allowed}}}},
              raw, 0));
        }
      }
    } else if (permissions == raw.end() || permissions->is_null()) {
      const auto exec = raw.find("proposedExecpolicyAmendment");
      if (exec != raw.end() && validExecPolicyAmendment(*exec)) {
        append(*structuredCommandDecision({{"acceptWithExecpolicyAmendment",
                                            {{"execpolicy_amendment", *exec}}}},
                                          raw, 0));
      }
    }
  }
  append(*simpleCommandDecision("cancel"));
  return result;
}

bool validQuestionRequest(const nlohmann::json &request) {
  if (!request.is_object())
    return false;
  const auto requested = request.find("questions");
  if (requested == request.end() || !requested->is_array() ||
      requested->empty() || requested->size() > 64)
    return false;
  std::size_t controls = 0;
  std::unordered_set<std::string> ids;
  for (const nlohmann::json &question : *requested) {
    if (!knownKeys(question, {"header", "id", "isOther", "isSecret", "options",
                              "question"}) ||
        !nonEmptyString(question, "id") ||
        !nonEmptyString(question, "question"))
      return false;
    const std::string id = question.at("id").get<std::string>();
    if (!ids.insert(id).second)
      return false;
    for (const char *field : {"header"}) {
      const auto found = question.find(field);
      if (found != question.end() &&
          (!found->is_string() ||
           found->get_ref<const std::string &>().size() > 4096))
        return false;
    }
    for (const char *field : {"isOther", "isSecret"}) {
      const auto found = question.find(field);
      if (found != question.end() && !found->is_boolean())
        return false;
    }
    const auto options = question.find("options");
    if (options != question.end() && !options->is_null() &&
        !options->is_array())
      return false;
    const std::size_t optionCount =
        options == question.end() || options->is_null() ? 0 : options->size();
    if (optionCount > 64 || controls + 1 + optionCount > 64)
      return false;
    controls += 1 + optionCount;
    if (options != question.end() && options->is_array())
      for (const nlohmann::json &option : *options)
        if (!knownKeys(option, {"description", "label"}) ||
            !nonEmptyString(option, "label") ||
            (option.contains("description") &&
             (!option.at("description").is_string() ||
              option.at("description").get_ref<const std::string &>().size() >
                  4096)))
          return false;
    if (optionCount == 0 || question.value("isOther", false)) {
      if (++controls > 64)
        return false;
    }
  }
  return true;
}

bool validAnswers(const nlohmann::json &request,
                  const nlohmann::json &answers) {
  if (!request.is_object() || !request.contains("questions") ||
      !request.at("questions").is_array() || !answers.is_object())
    return false;
  const nlohmann::json &questions = request.at("questions");
  if (answers.size() != questions.size())
    return false;
  return std::ranges::all_of(questions, [&](const nlohmann::json &question) {
    const std::string id = question.at("id").get<std::string>();
    const auto answer = answers.find(id);
    if (answer == answers.end() || !knownKeys(*answer, {"answers"}))
      return false;
    const auto values = answer->find("answers");
    if (values == answer->end() || !values->is_array() || values->empty() ||
        values->size() > 64)
      return false;
    const nlohmann::json options =
        question.value("options", nlohmann::json::array());
    const bool allowsOther =
        options.empty() || question.value("isOther", false);
    return std::ranges::all_of(*values, [&](const nlohmann::json &value) {
      if (!value.is_string() || value.empty() ||
          value.get_ref<const std::string &>().size() > 4096)
        return false;
      if (allowsOther)
        return true;
      return std::ranges::any_of(options, [&](const nlohmann::json &option) {
        return option.at("label") == value;
      });
    });
  });
}

std::string mcpApprovalKind(const nlohmann::json &request) {
  if (!request.is_object())
    return {};
  const auto metadata = request.find("_meta");
  return metadata == request.end() || !metadata->is_object()
             ? std::string{}
             : stringValue(*metadata, "codex_approval_kind");
}

bool mcpMessageOnly(const nlohmann::json &request) {
  if (!request.is_object())
    return false;
  const std::string mode = stringValue(request, "mode");
  if (mode == "url")
    return true;
  if (mode == "openai/form")
    return false;
  const auto schema = request.find("requestedSchema");
  if (schema == request.end())
    return false;
  if (schema->is_null())
    return true;
  if (!schema->is_object() || stringValue(*schema, "type") != "object")
    return false;
  const auto properties = schema->find("properties");
  return properties != schema->end() && properties->is_object() &&
         properties->empty();
}

bool privilegedMcpApproval(const nlohmann::json &request) {
  return stringValue(request, "mode") == "form" && mcpMessageOnly(request);
}

bool mcpPersistence(const nlohmann::json &request, std::string_view mode) {
  if (!request.is_object())
    return false;
  const auto metadata = request.find("_meta");
  if (metadata == request.end() || !metadata->is_object())
    return false;
  const auto persist = metadata->find("persist");
  if (persist == metadata->end())
    return false;
  if (persist->is_string())
    return persist->get_ref<const std::string &>() == mode;
  return persist->is_array() && persist->size() <= 2 &&
         std::ranges::any_of(*persist, [mode](const nlohmann::json &entry) {
           return entry.is_string() &&
                  entry.get_ref<const std::string &>() == mode;
         });
}

bool validMcpApprovalMetadata(const nlohmann::json &request) {
  if (mcpApprovalKind(request) != "mcp_tool_call" ||
      !privilegedMcpApproval(request))
    return true;
  const nlohmann::json &metadata = request.at("_meta");
  if (!knownKeys(metadata,
                 {"approvals_reviewer", "codex_approval_kind",
                  "codex_request_type", "codex_strict_auto_review",
                  "connector_description", "connector_id", "connector_name",
                  "persist", "source", "tool_description", "tool_name",
                  "tool_params", "tool_params_display", "tool_title"}))
    return false;
  const auto persist = metadata.find("persist");
  if (persist != metadata.end() && !persist->is_null()) {
    const auto validMode = [](const nlohmann::json &mode) {
      return mode.is_string() && (mode == "session" || mode == "always");
    };
    if (!(validMode(*persist) ||
          (persist->is_array() && !persist->empty() && persist->size() <= 2 &&
           std::ranges::all_of(*persist, validMode))))
      return false;
  }
  const auto display = metadata.find("tool_params_display");
  const bool validDisplay =
      display != metadata.end() && display->is_array() && !display->empty() &&
      display->size() <= 64 &&
      std::ranges::all_of(*display, [](const nlohmann::json &entry) {
        return knownKeys(entry, {"display_name", "name", "value"}) &&
               nonEmptyString(entry, "name") &&
               nonEmptyString(entry, "display_name") && entry.contains("value");
      });
  if (display != metadata.end() && !display->is_null() && !validDisplay)
    return false;
  const auto parameters = metadata.find("tool_params");
  if (parameters == metadata.end() || parameters->is_null())
    return true;
  if (!parameters->is_object() || parameters->empty())
    return parameters->is_object();
  if (!validDisplay || display->size() != parameters->size())
    return false;
  std::unordered_set<std::string> displayedNames;
  return std::ranges::all_of(*display, [&](const nlohmann::json &entry) {
    const std::string name = entry.at("name").get<std::string>();
    const auto parameter = parameters->find(name);
    if (!displayedNames.insert(name).second || parameter == parameters->end())
      return false;
    bool truncated = false;
    std::size_t remaining = 64;
    const nlohmann::json displayed =
        boundedDisclosureValue(entry.at("value"), 0, remaining, truncated);
    remaining = 64;
    const nlohmann::json actual =
        boundedDisclosureValue(*parameter, 0, remaining, truncated);
    return !truncated && displayed == actual;
  });
}

nlohmann::json boundedDisclosureValue(const nlohmann::json &value,
                                      std::size_t depth, std::size_t &remaining,
                                      bool &truncated) {
  if (remaining == 0 || depth == 8) {
    truncated = true;
    return nullptr;
  }
  --remaining;
  if (value.is_object()) {
    nlohmann::json result = nlohmann::json::object();
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
      if (remaining == 0) {
        truncated = true;
        break;
      }
      constexpr std::size_t MaximumKeyCharacters = 128;
      if (iterator.key().size() > MaximumKeyCharacters) {
        truncated = true;
        break;
      }
      result[iterator.key()] = boundedDisclosureValue(
          iterator.value(), depth + 1, remaining, truncated);
    }
    return result;
  }
  if (value.is_array()) {
    nlohmann::json result = nlohmann::json::array();
    for (const nlohmann::json &entry : value) {
      if (remaining == 0) {
        truncated = true;
        break;
      }
      result.push_back(
          boundedDisclosureValue(entry, depth + 1, remaining, truncated));
    }
    return result;
  }
  if (value.is_string()) {
    constexpr std::size_t MaximumValueCharacters = 1024;
    std::string result = value.get<std::string>();
    if (result.size() > MaximumValueCharacters) {
      result = truncateUtf8(std::move(result), MaximumValueCharacters);
      truncated = true;
    }
    return result;
  }
  return value;
}

struct ReviewContext {
  nlohmann::json disclosure = nlohmann::json::object();
  std::size_t remaining = 64;
  bool complete = true;
};

nlohmann::json
disclosedMembers(const nlohmann::json &raw,
                 std::initializer_list<std::string_view> visibleFields,
                 std::initializer_list<std::string_view> hiddenFields,
                 ReviewContext &context) {
  nlohmann::json result = nlohmann::json::object();
  if (!raw.is_object()) {
    context.complete = false;
    return result;
  }
  bool truncated = false;
  for (const std::string_view field : visibleFields) {
    const auto found = raw.find(std::string(field));
    if (found == raw.end())
      continue;
    if (context.remaining == 0) {
      truncated = true;
      break;
    }
    result[std::string(field)] =
        boundedDisclosureValue(*found, 0, context.remaining, truncated);
  }
  for (auto iterator = raw.begin(); iterator != raw.end(); ++iterator) {
    const std::string_view key = iterator.key();
    if (std::ranges::find(visibleFields, key) == visibleFields.end() &&
        std::ranges::find(hiddenFields, key) == hiddenFields.end()) {
      truncated = true;
      break;
    }
  }
  context.complete &= !truncated;
  return result;
}

nlohmann::json mcpDisclosure(const nlohmann::json &raw,
                             ReviewContext &context) {
  nlohmann::json result =
      disclosedMembers(raw,
                       {"serverName", "threadId", "turnId", "message", "mode",
                        "requestedSchema", "elicitationId", "url"},
                       {"_meta", "meta"}, context);
  const std::string approvalKind = mcpApprovalKind(raw);
  if (!mcpMessageOnly(raw) ||
      (approvalKind.empty() && !mcpPersistence(raw, "session") &&
       !mcpPersistence(raw, "always")))
    return result;

  const auto metadata = raw.find("_meta");
  nlohmann::json approval = disclosedMembers(
      *metadata,
      {"connector_description", "connector_name", "install_url", "persist",
       "source", "suggest_reason", "suggest_type", "tool_description",
       "tool_name", "tool_params_display", "tool_title", "tool_type"},
      {"approvals_reviewer", "codex_approval_kind", "codex_request_type",
       "codex_strict_auto_review", "connector_id", "tool_id", "tool_params"},
      context);
  if (!approval.empty())
    result["approval"] = std::move(approval);
  return result;
}

ReviewContext reviewContext(PendingRequestKind kind,
                            const nlohmann::json &raw) {
  ReviewContext context;
  switch (kind) {
  case PendingRequestKind::CommandApproval:
    context.disclosure = disclosedMembers(
        raw,
        {"additionalPermissions", "approvalId", "command", "commandActions",
         "cwd", "environmentId", "itemId", "networkApprovalContext",
         "proposedExecpolicyAmendment", "proposedNetworkPolicyAmendments",
         "reason", "startedAtMs", "threadId", "turnId"},
        {"availableDecisions"}, context);
    break;
  case PendingRequestKind::FileChangeApproval:
    context.disclosure = disclosedMembers(
        raw,
        {"grantRoot", "itemId", "reason", "startedAtMs", "threadId", "turnId"},
        {}, context);
    break;
  case PendingRequestKind::UserInput:
    context.disclosure = disclosedMembers(
        raw, {"autoResolutionMs", "isBlocking", "itemId", "threadId", "turnId"},
        {"questions"}, context);
    context.complete &= validQuestionRequest(raw);
    break;
  case PendingRequestKind::McpElicitation:
    context.disclosure = mcpDisclosure(raw, context);
    break;
  case PendingRequestKind::PermissionsApproval:
    context.disclosure =
        disclosedMembers(raw,
                         {"cwd", "environmentId", "itemId", "permissions",
                          "reason", "startedAtMs", "threadId", "turnId"},
                         {}, context);
    break;
  case PendingRequestKind::DynamicToolCall:
    context.disclosure = disclosedMembers(
        raw, {"callId", "namespace", "threadId", "tool", "turnId"},
        {"arguments"}, context);
    break;
  case PendingRequestKind::AuthenticationRefresh:
    context.disclosure =
        disclosedMembers(raw, {"reason"}, {"previousAccountId"}, context);
    break;
  case PendingRequestKind::Attestation:
    context.disclosure = disclosedMembers(raw, {}, {"challenge"}, context);
    break;
  case PendingRequestKind::LegacyPatchApproval:
    context.disclosure = disclosedMembers(
        raw, {"callId", "conversationId", "fileChanges", "grantRoot", "reason"},
        {}, context);
    break;
  case PendingRequestKind::LegacyCommandApproval:
    context.disclosure =
        disclosedMembers(raw,
                         {"approvalId", "callId", "command", "conversationId",
                          "cwd", "parsedCmd", "reason"},
                         {}, context);
    break;
  case PendingRequestKind::Unsupported:
    context.disclosure = disclosedMembers(raw, {}, {}, context);
    break;
  }
  return context;
}

bool approvalContextComplete(PendingRequestKind kind,
                             const nlohmann::json &raw) {
  if (!reviewContext(kind, raw).complete)
    return false;
  switch (kind) {
  case PendingRequestKind::CommandApproval: {
    const auto commands = raw.find("commandActions");
    const auto network = raw.find("networkApprovalContext");
    const auto permissions = raw.find("additionalPermissions");
    const auto command = raw.find("command");
    const auto execAmendment = raw.find("proposedExecpolicyAmendment");
    const auto networkAmendments = raw.find("proposedNetworkPolicyAmendments");
    if ((command != raw.end() && !command->is_null() &&
         !nonEmptyString(raw, "command")) ||
        (commands != raw.end() && !commands->is_null() &&
         !validCommandActions(*commands)) ||
        (network != raw.end() && !network->is_null() &&
         !validNetworkApprovalContext(*network)) ||
        (permissions != raw.end() && !permissions->is_null() &&
         !validPermissionProfile(*permissions)) ||
        (execAmendment != raw.end() && !execAmendment->is_null() &&
         !validExecPolicyAmendment(*execAmendment)) ||
        (networkAmendments != raw.end() && !networkAmendments->is_null() &&
         !validNetworkAmendments(*networkAmendments)))
      return false;
    return (command != raw.end() && !command->is_null()) ||
           (commands != raw.end() && !commands->is_null()) ||
           (network != raw.end() && !network->is_null()) ||
           (permissions != raw.end() && !permissions->is_null());
  }
  case PendingRequestKind::FileChangeApproval:
    return nonEmptyString(raw, "itemId") || nonEmptyString(raw, "reason") ||
           nonEmptyString(raw, "grantRoot");
  case PendingRequestKind::UserInput:
    return true;
  case PendingRequestKind::McpElicitation: {
    if (!nonEmptyString(raw, "serverName") ||
        !nonEmptyString(raw, "threadId") || !nonEmptyString(raw, "message") ||
        !validMcpApprovalMetadata(raw))
      return false;
    const std::string mode = stringValue(raw, "mode");
    if (mode == "url")
      return nonEmptyString(raw, "elicitationId") && nonEmptyString(raw, "url");
    const auto schema = raw.find("requestedSchema");
    if (mode == "openai/form")
      return schema != raw.end();
    return mode == "form" && schema != raw.end() && schema->is_object();
  }
  case PendingRequestKind::PermissionsApproval: {
    const auto permissions = raw.find("permissions");
    return permissions != raw.end() && validPermissionProfile(*permissions);
  }
  case PendingRequestKind::LegacyPatchApproval: {
    const auto changes = raw.find("fileChanges");
    return nonEmptyString(raw, "conversationId") && changes != raw.end() &&
           changes->is_object() && !changes->empty();
  }
  case PendingRequestKind::LegacyCommandApproval: {
    const auto command = raw.find("command");
    return nonEmptyString(raw, "conversationId") && command != raw.end() &&
           command->is_array() && !command->empty() &&
           std::ranges::all_of(*command, [](const nlohmann::json &part) {
             return part.is_string() && !part.empty();
           });
  }
  default:
    return false;
  }
}

std::vector<PendingRequestAction>
safeActions(PendingRequestKind kind, const nlohmann::json &raw,
            std::vector<PendingRequestAction> candidates) {
  if (approvalContextComplete(kind, raw))
    return candidates;
  std::erase_if(candidates, [](const PendingRequestAction &candidate) {
    return candidate.value != "decline" && candidate.value != "cancel" &&
           candidate.value != "denied" && candidate.value != "abort";
  });
  if (candidates.empty())
    candidates.push_back(
        action("unsupported", "Return unsupported", Tone::Neutral));
  return candidates;
}

struct CommandEvaluation {
  std::optional<std::vector<CommandDecisionOption>> decisions;
  std::vector<PendingRequestAction> actions;
};

CommandEvaluation evaluateCommand(const nlohmann::json &raw) {
  CommandEvaluation result{commandDecisionOptions(raw), {}};
  if (!result.decisions) {
    result.actions.push_back(
        action("unsupported", "Return unsupported", Tone::Neutral));
    return result;
  }
  result.actions.reserve(result.decisions->size());
  for (const CommandDecisionOption &entry : *result.decisions)
    result.actions.push_back(entry.action);
  result.actions = safeActions(PendingRequestKind::CommandApproval, raw,
                               std::move(result.actions));
  return result;
}

std::optional<PendingRequestAction>
positiveAction(const PendingRequestDescriptor &request,
               const std::vector<PendingRequestAction> &available) {
  // A direct approval is safe only when the compact surface contains the
  // complete decision context. Patches, permission sets, legacy payloads and
  // command amendments require the dialog's full disclosure.
  if (request.kind != PendingRequestKind::CommandApproval ||
      !request.raw.is_object())
    return std::nullopt;
  for (const std::string_view field :
       {std::string_view("additionalPermissions"),
        std::string_view("commandActions"),
        std::string_view("networkApprovalContext"),
        std::string_view("proposedExecpolicyAmendment"),
        std::string_view("proposedNetworkPolicyAmendments")})
    if (request.raw.contains(std::string(field)))
      return std::nullopt;
  constexpr std::size_t MaximumDirectContextCharacters = 512;
  for (const std::string_view field :
       {std::string_view("command"), std::string_view("reason"),
        std::string_view("cwd")}) {
    const auto found = request.raw.find(std::string(field));
    if (found != request.raw.end() &&
        (!found->is_string() || found->get_ref<const std::string &>().size() >
                                    MaximumDirectContextCharacters))
      return std::nullopt;
  }
  if (stringValue(request.raw, "command").empty())
    return std::nullopt;
  const auto found = std::ranges::find_if(available, [](const auto &candidate) {
    return candidate.tone == Tone::Approve && !candidate.requiresInput;
  });
  return found == available.end() ? std::nullopt
                                  : std::optional<PendingRequestAction>(*found);
}

std::optional<PendingRequestAction>
negativeAction(const std::vector<PendingRequestAction> &available) {
  const auto found = std::ranges::find_if(available, [](const auto &candidate) {
    return (candidate.value == "decline" || candidate.value == "denied") &&
           !candidate.requiresInput;
  });
  return found == available.end() ? std::nullopt
                                  : std::optional<PendingRequestAction>(*found);
}

} // namespace

PendingRequestKind
PendingRequestPolicy::kindForMethod(std::string_view method) noexcept {
  const auto found = std::ranges::find(RequestKindDefinitions, method,
                                       &RequestKindDefinition::method);
  return found == RequestKindDefinitions.end() ? PendingRequestKind::Unsupported
                                               : found->kind;
}

std::string_view
PendingRequestPolicy::kindToken(PendingRequestKind kind) noexcept {
  return definition(kind).token;
}

std::string_view PendingRequestPolicy::title(PendingRequestKind kind) noexcept {
  return definition(kind).title;
}

std::string_view
PendingRequestPolicy::dialogTitle(PendingRequestKind kind) noexcept {
  return definition(kind).dialogTitle;
}

std::string
PendingRequestPolicy::detail(const PendingRequestDescriptor &request) {
  std::vector<std::string> parts;
  appendDetail(parts, "Thread: ", request.threadId);
  if (!request.threadTitle.empty() && request.threadTitle != request.threadId)
    appendDetail(parts, "Title: ", request.threadTitle);
  appendDetail(parts, "Command: ", stringValue(request.raw, "command"));
  appendDetail(parts, "Reason: ", stringValue(request.raw, "reason"));
  appendDetail(parts, {}, stringValue(request.raw, "message"));
  appendDetail(parts, "Directory: ", stringValue(request.raw, "cwd"));
  appendDetail(parts, "Grant root: ", stringValue(request.raw, "grantRoot"));
  const auto questions = request.raw.find("questions");
  if (request.kind == PendingRequestKind::UserInput &&
      questions != request.raw.end() && questions->is_array())
    parts.push_back(std::to_string(questions->size()) + " questions");

  return joinDetails(parts);
}

std::string
PendingRequestPolicy::status(const PendingRequestDescriptor &request) {
  if (request.availability == PendingRequestAvailability::Submitting)
    return "Submitting response...";
  std::vector<std::string> facts;
  if (!request.error.empty())
    facts.push_back(request.error);
  if (request.availability == PendingRequestAvailability::RecoveryOnly)
    facts.emplace_back(request.retainedSubmission
                           ? "The authored response is retained for review."
                           : "The request is no longer actionable.");
  else if (request.availability == PendingRequestAvailability::Unavailable)
    facts.emplace_back("Controller access is unavailable.");
  return joinDetails(facts);
}

nlohmann::json
PendingRequestPolicy::disclosure(const PendingRequestDescriptor &request) {
  ReviewContext context = reviewContext(request.kind, request.raw);
  if (!context.complete)
    context.disclosure["additionalDetails"] = "Omitted for display";
  return std::move(context.disclosure);
}

std::vector<PendingRequestAction>
PendingRequestPolicy::actions(const PendingRequestDescriptor &request) {
  if (request.kind == PendingRequestKind::CommandApproval)
    return evaluateCommand(request.raw).actions;
  if (request.kind == PendingRequestKind::FileChangeApproval)
    return safeActions(request.kind, request.raw,
                       std::vector<PendingRequestAction>(
                           approvalActions().begin(), approvalActions().end()));
  if (request.kind == PendingRequestKind::UserInput)
    return safeActions(request.kind, request.raw,
                       {action("submit", "Submit", Tone::Approve, true),
                        action("decline", "Decline", Tone::Danger)});
  if (request.kind == PendingRequestKind::McpElicitation) {
    const std::string approvalKind = mcpApprovalKind(request.raw);
    const bool messageOnly = mcpMessageOnly(request.raw);
    const bool privileged =
        stringValue(request.raw, "mode") == "form" && messageOnly;
    const bool session = messageOnly && mcpPersistence(request.raw, "session");
    const bool always = messageOnly && mcpPersistence(request.raw, "always");
    if (!approvalKind.empty() && approvalKind != "mcp_tool_call" &&
        approvalKind != "tool_suggestion")
      return {action("unsupported", "Return unsupported", Tone::Neutral)};
    if (approvalKind == "tool_suggestion" && privileged)
      return {action("unsupported", "Return unsupported", Tone::Neutral)};
    if (approvalKind == "mcp_tool_call" && privileged) {
      std::vector<PendingRequestAction> candidates{
          action("accept", "Accept", Tone::Approve)};
      if (session)
        candidates.push_back(
            action("acceptForSession", "Accept for session", Tone::Approve));
      if (always)
        candidates.push_back(
            action("acceptAlways", "Always allow", Tone::Approve));
      candidates.push_back(action("cancel", "Cancel", Tone::Neutral));
      return safeActions(request.kind, request.raw, std::move(candidates));
    }
    std::vector<PendingRequestAction> candidates{
        action("accept", "Accept", Tone::Approve, !messageOnly)};
    if (session)
      candidates.push_back(
          action("acceptForSession", "Accept for session", Tone::Approve));
    if (always)
      candidates.push_back(
          action("acceptAlways", "Always allow", Tone::Approve));
    candidates.push_back(action("decline", "Decline", Tone::Danger));
    candidates.push_back(action("cancel", "Cancel", Tone::Neutral));
    return safeActions(request.kind, request.raw, std::move(candidates));
  }
  if (request.kind == PendingRequestKind::PermissionsApproval)
    return safeActions(request.kind, request.raw,
                       {action("turn", "Allow this turn", Tone::Approve),
                        action("session", "Allow this session", Tone::Approve),
                        action("decline", "Decline", Tone::Danger)});
  if (request.kind == PendingRequestKind::LegacyPatchApproval ||
      request.kind == PendingRequestKind::LegacyCommandApproval)
    return safeActions(
        request.kind, request.raw,
        {action("approved", "Approve", Tone::Approve),
         action("approved_for_session", "Approve for session", Tone::Approve),
         action("denied", "Deny", Tone::Danger),
         action("abort", "Abort", Tone::Neutral)});
  if (request.kind == PendingRequestKind::DynamicToolCall)
    return {action("unavailable", "Return unavailable", Tone::Neutral)};
  return {action("unsupported", "Return unsupported", Tone::Neutral)};
}

PendingRequestControls
PendingRequestPolicy::controls(const PendingRequestDescriptor &request) {
  PendingRequestControls result;
  const auto available = actions(request);
  if (!request.retainedSubmission) {
    result.positive = codexui::codex::positiveAction(request, available);
    result.negative = codexui::codex::negativeAction(available);
  }
  result.directEnabled =
      request.availability == PendingRequestAvailability::Actionable;
  result.reviewEnabled =
      request.availability != PendingRequestAvailability::Submitting &&
      (result.directEnabled || request.retainedSubmission.has_value());
  return result;
}

std::optional<std::size_t> PendingRequestPolicy::attentionIndex(
    const std::vector<PendingRequestDescriptor> &requests,
    std::string_view selectedThreadId) {
  std::optional<std::size_t> result;
  int bestRank = std::numeric_limits<int>::max();
  for (std::size_t index = 0; index < requests.size(); ++index) {
    const PendingRequestDescriptor &request = requests[index];
    const int rank = attentionRank(request.availability,
                                   request.retainedSubmission.has_value(),
                                   request.threadId == selectedThreadId);
    if (rank < bestRank) {
      bestRank = rank;
      result = index;
    }
  }
  return result;
}

int PendingRequestPolicy::attentionRank(PendingRequestAvailability availability,
                                        bool retainedSubmission,
                                        bool selectedThread) noexcept {
  int rank = 10;
  switch (availability) {
  case PendingRequestAvailability::Actionable:
    rank = 0;
    break;
  case PendingRequestAvailability::Submitting:
    rank = 2;
    break;
  case PendingRequestAvailability::RecoveryOnly:
    rank = retainedSubmission ? 4 : 8;
    break;
  case PendingRequestAvailability::Unavailable:
    rank = retainedSubmission ? 6 : 10;
    break;
  }
  return rank + !selectedThread;
}

std::optional<PendingRequestResponse>
PendingRequestPolicy::responseForSubmission(
    PendingRequestKind kind, const nlohmann::json &request,
    PendingRequestSubmission submission) {
  std::optional<CommandEvaluation> command;
  if (kind == PendingRequestKind::CommandApproval) {
    command = evaluateCommand(request);
    if (!decisionAllowed(command->actions, submission.choice))
      return std::nullopt;
  } else {
    PendingRequestDescriptor descriptor;
    descriptor.kind = kind;
    descriptor.raw = request;
    if (!decisionAllowed(actions(descriptor), submission.choice))
      return std::nullopt;
  }
  if (kind == PendingRequestKind::UserInput && submission.choice == "submit" &&
      !validAnswers(request, submission.input))
    return std::nullopt;
  PendingRequestResponse response{nlohmann::json::object()};
  if (submission.choice == "unsupported") {
    response = jsonRpcError(
        kind == PendingRequestKind::AuthenticationRefresh
            ? "CodexUI does not support authentication token refresh"
        : kind == PendingRequestKind::Attestation
            ? "CodexUI does not support attestation generation"
            : "CodexUI does not support this server request");
  } else if (kind == PendingRequestKind::CommandApproval) {
    const auto found = std::ranges::find(
        *command->decisions, submission.choice,
        [](const CommandDecisionOption &entry) { return entry.action.value; });
    if (found == command->decisions->end())
      return std::nullopt;
    response = nlohmann::json{{"decision", found->wireDecision}};
  } else if (kind == PendingRequestKind::FileChangeApproval) {
    response = nlohmann::json{{"decision", std::move(submission.choice)}};
  } else if (kind == PendingRequestKind::UserInput) {
    if (submission.choice == "decline")
      response = jsonRpcError("Request declined by user");
    else
      response = nlohmann::json{{"answers", std::move(submission.input)}};
  } else if (kind == PendingRequestKind::McpElicitation) {
    const std::string approvalKind = mcpApprovalKind(request);
    if (submission.choice == "acceptForSession" ||
        submission.choice == "acceptAlways") {
      response = nlohmann::json{
          {"action", "accept"},
          {"content", nullptr},
          {"_meta",
           {{"persist",
             submission.choice == "acceptForSession" ? "session" : "always"}}}};
    } else if (submission.choice != "accept") {
      response = nlohmann::json{{"action", std::move(submission.choice)},
                                {"content", nullptr},
                                {"_meta", nullptr}};
    } else if (approvalKind == "mcp_tool_call" &&
               privilegedMcpApproval(request)) {
      response = nlohmann::json{
          {"action", "accept"}, {"content", nullptr}, {"_meta", nullptr}};
    } else if (!mcpMessageOnly(request) &&
               stringValue(request, "mode") != "openai/form" &&
               !submission.input.is_object()) {
      return std::nullopt;
    } else {
      response = nlohmann::json{{"action", "accept"},
                                {"content", mcpMessageOnly(request)
                                                ? nlohmann::json(nullptr)
                                                : std::move(submission.input)},
                                {"_meta", std::move(submission.metadata)}};
    }
  } else if (kind == PendingRequestKind::PermissionsApproval) {
    if (submission.choice == "decline") {
      response = jsonRpcError("Permission request declined by user");
    } else {
      response = nlohmann::json{{"permissions", request.at("permissions")},
                                {"scope", std::move(submission.choice)}};
    }
  } else if (kind == PendingRequestKind::LegacyPatchApproval ||
             kind == PendingRequestKind::LegacyCommandApproval) {
    if (submission.choice == "approved" ||
        submission.choice == "approved_for_session")
      response = nlohmann::json{{"decision", std::move(submission.choice)}};
    else if (submission.choice == "denied")
      response = nlohmann::json{
          {"decision", {{"denied", {{"rejection", "Denied by user"}}}}}};
    else
      response = nlohmann::json{{"decision", "abort"}};
  } else if (kind == PendingRequestKind::DynamicToolCall) {
    response = nlohmann::json{
        {"contentItems",
         nlohmann::json::array(
             {{{"type", "inputText"},
               {"text", "CodexUI does not provide this dynamic tool"}}})},
        {"success", false}};
  } else {
    response = jsonRpcError("CodexUI does not support this server request");
  }
  return response;
}

} // namespace codexui::codex
