// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/ProtocolCatalog.h"

#include <array>

namespace codexui::nodegraph {
namespace {

#define CODEXUI_CLIENT_REQUEST(methodName)                                     \
  MethodDescriptor {                                                           \
    methodName, ProtocolDirection::ClientRequest,                              \
        MessageDisposition::WorkerOperationResult                              \
  }
#define CODEXUI_SERVER_REQUEST(methodName)                                     \
  MethodDescriptor {                                                           \
    methodName, ProtocolDirection::ServerRequest,                              \
        MessageDisposition::ReverseInteraction                                 \
  }
#define CODEXUI_GRAPH_NOTIFICATION(methodName)                                 \
  MethodDescriptor {                                                           \
    methodName, ProtocolDirection::ServerNotification,                         \
        MessageDisposition::GraphUpdate                                        \
  }
#define CODEXUI_NEUTRAL_NOTIFICATION(methodName)                               \
  MethodDescriptor {                                                           \
    methodName, ProtocolDirection::ServerNotification,                         \
        MessageDisposition::IntentionallyStateNeutral                          \
  }
#define CODEXUI_UI_NOTIFICATION(methodName)                                    \
  MethodDescriptor {                                                           \
    methodName, ProtocolDirection::ServerNotification,                         \
        MessageDisposition::TypedUiEffect                                      \
  }
#define CODEXUI_CLIENT_NOTIFICATION(methodName)                                \
  MethodDescriptor {                                                           \
    methodName, ProtocolDirection::ClientNotification,                         \
        MessageDisposition::WorkerOperationResult                              \
  }

constexpr std::array<MethodDescriptor, 252> Methods{{
    CODEXUI_CLIENT_REQUEST("initialize"),
    CODEXUI_CLIENT_REQUEST("server/diagnostics"),
    CODEXUI_CLIENT_REQUEST("thread/start"),
    CODEXUI_CLIENT_REQUEST("thread/resume"),
    CODEXUI_CLIENT_REQUEST("thread/fork"),
    CODEXUI_CLIENT_REQUEST("thread/archive"),
    CODEXUI_CLIENT_REQUEST("thread/delete"),
    CODEXUI_CLIENT_REQUEST("thread/unsubscribe"),
    CODEXUI_CLIENT_REQUEST("thread/increment_elicitation"),
    CODEXUI_CLIENT_REQUEST("thread/decrement_elicitation"),
    CODEXUI_CLIENT_REQUEST("thread/name/set"),
    CODEXUI_CLIENT_REQUEST("thread/goal/set"),
    CODEXUI_CLIENT_REQUEST("thread/goal/get"),
    CODEXUI_CLIENT_REQUEST("thread/goal/clear"),
    CODEXUI_CLIENT_REQUEST("thread/queue/add"),
    CODEXUI_CLIENT_REQUEST("thread/queue/list"),
    CODEXUI_CLIENT_REQUEST("thread/queue/update"),
    CODEXUI_CLIENT_REQUEST("thread/queue/delete"),
    CODEXUI_CLIENT_REQUEST("thread/queue/reorder"),
    CODEXUI_CLIENT_REQUEST("thread/queue/start"),
    CODEXUI_CLIENT_REQUEST("thread/metadata/update"),
    CODEXUI_CLIENT_REQUEST("thread/section/move"),
    CODEXUI_CLIENT_REQUEST("thread/settings/update"),
    CODEXUI_CLIENT_REQUEST("thread/memoryMode/set"),
    CODEXUI_CLIENT_REQUEST("memory/reset"),
    CODEXUI_CLIENT_REQUEST("thread/unarchive"),
    CODEXUI_CLIENT_REQUEST("thread/compact/start"),
    CODEXUI_CLIENT_REQUEST("thread/shellCommand"),
    CODEXUI_CLIENT_REQUEST("thread/approveGuardianDeniedAction"),
    CODEXUI_CLIENT_REQUEST("thread/backgroundTerminals/clean"),
    CODEXUI_CLIENT_REQUEST("thread/backgroundTerminals/list"),
    CODEXUI_CLIENT_REQUEST("thread/backgroundTerminals/terminate"),
    CODEXUI_CLIENT_REQUEST("thread/rollback"),
    CODEXUI_CLIENT_REQUEST("thread/revert"),
    CODEXUI_CLIENT_REQUEST("thread/list"),
    CODEXUI_CLIENT_REQUEST("project/list"),
    CODEXUI_CLIENT_REQUEST("project/read"),
    CODEXUI_CLIENT_REQUEST("project/create"),
    CODEXUI_CLIENT_REQUEST("project/import"),
    CODEXUI_CLIENT_REQUEST("project/update"),
    CODEXUI_CLIENT_REQUEST("project/move"),
    CODEXUI_CLIENT_REQUEST("project/delete"),
    CODEXUI_CLIENT_REQUEST("threadSection/list"),
    CODEXUI_CLIENT_REQUEST("threadSection/create"),
    CODEXUI_CLIENT_REQUEST("threadSection/update"),
    CODEXUI_CLIENT_REQUEST("threadSection/delete"),
    CODEXUI_CLIENT_REQUEST("thread/search"),
    CODEXUI_CLIENT_REQUEST("thread/searchOccurrences"),
    CODEXUI_CLIENT_REQUEST("thread/loaded/list"),
    CODEXUI_CLIENT_REQUEST("thread/read"),
    CODEXUI_CLIENT_REQUEST("thread/turns/list"),
    CODEXUI_CLIENT_REQUEST("thread/items/list"),
    CODEXUI_CLIENT_REQUEST("thread/inject_items"),
    CODEXUI_CLIENT_REQUEST("skills/list"),
    CODEXUI_CLIENT_REQUEST("skills/extraRoots/set"),
    CODEXUI_CLIENT_REQUEST("hooks/list"),
    CODEXUI_CLIENT_REQUEST("marketplace/add"),
    CODEXUI_CLIENT_REQUEST("marketplace/remove"),
    CODEXUI_CLIENT_REQUEST("marketplace/upgrade"),
    CODEXUI_CLIENT_REQUEST("plugin/list"),
    CODEXUI_CLIENT_REQUEST("plugin/search"),
    CODEXUI_CLIENT_REQUEST("plugin/installed"),
    CODEXUI_CLIENT_REQUEST("plugin/read"),
    CODEXUI_CLIENT_REQUEST("plugin/skill/read"),
    CODEXUI_CLIENT_REQUEST("plugin/share/save"),
    CODEXUI_CLIENT_REQUEST("plugin/share/updateTargets"),
    CODEXUI_CLIENT_REQUEST("plugin/share/list"),
    CODEXUI_CLIENT_REQUEST("plugin/share/checkout"),
    CODEXUI_CLIENT_REQUEST("plugin/share/delete"),
    CODEXUI_CLIENT_REQUEST("app/read"),
    CODEXUI_CLIENT_REQUEST("app/list"),
    CODEXUI_CLIENT_REQUEST("app/installed"),
    CODEXUI_CLIENT_REQUEST("fs/readFile"),
    CODEXUI_CLIENT_REQUEST("fs/writeFile"),
    CODEXUI_CLIENT_REQUEST("fs/createDirectory"),
    CODEXUI_CLIENT_REQUEST("fs/getMetadata"),
    CODEXUI_CLIENT_REQUEST("fs/readDirectory"),
    CODEXUI_CLIENT_REQUEST("fs/remove"),
    CODEXUI_CLIENT_REQUEST("fs/copy"),
    CODEXUI_CLIENT_REQUEST("fs/watch"),
    CODEXUI_CLIENT_REQUEST("fs/unwatch"),
    CODEXUI_CLIENT_REQUEST("skills/config/write"),
    CODEXUI_CLIENT_REQUEST("plugin/install"),
    CODEXUI_CLIENT_REQUEST("plugin/uninstall"),
    CODEXUI_CLIENT_REQUEST("turn/start"),
    CODEXUI_CLIENT_REQUEST("turn/settings/update"),
    CODEXUI_CLIENT_REQUEST("turn/steer"),
    CODEXUI_CLIENT_REQUEST("turn/interrupt"),
    CODEXUI_CLIENT_REQUEST("thread/realtime/start"),
    CODEXUI_CLIENT_REQUEST("thread/realtime/appendAudio"),
    CODEXUI_CLIENT_REQUEST("thread/realtime/appendText"),
    CODEXUI_CLIENT_REQUEST("thread/realtime/appendSpeech"),
    CODEXUI_CLIENT_REQUEST("thread/realtime/stop"),
    CODEXUI_CLIENT_REQUEST("thread/timeline/list"),
    CODEXUI_CLIENT_REQUEST("thread/realtime/listVoices"),
    CODEXUI_CLIENT_REQUEST("review/start"),
    CODEXUI_CLIENT_REQUEST("model/list"),
    CODEXUI_CLIENT_REQUEST("modelProvider/capabilities/read"),
    CODEXUI_CLIENT_REQUEST("experimentalFeature/list"),
    CODEXUI_CLIENT_REQUEST("permissionProfile/list"),
    CODEXUI_CLIENT_REQUEST("experimentalFeature/enablement/set"),
    CODEXUI_CLIENT_REQUEST("remoteControl/enable"),
    CODEXUI_CLIENT_REQUEST("remoteControl/disable"),
    CODEXUI_CLIENT_REQUEST("remoteControl/status/read"),
    CODEXUI_CLIENT_REQUEST("remoteControl/pairing/start"),
    CODEXUI_CLIENT_REQUEST("remoteControl/pairing/status"),
    CODEXUI_CLIENT_REQUEST("remoteControl/client/list"),
    CODEXUI_CLIENT_REQUEST("remoteControl/client/revoke"),
    CODEXUI_CLIENT_REQUEST("collaborationMode/list"),
    CODEXUI_CLIENT_REQUEST("mock/experimentalMethod"),
    CODEXUI_CLIENT_REQUEST("environment/add"),
    CODEXUI_CLIENT_REQUEST("environment/info"),
    CODEXUI_CLIENT_REQUEST("environment/status"),
    CODEXUI_CLIENT_REQUEST("mcpServer/oauth/login"),
    CODEXUI_CLIENT_REQUEST("config/mcpServer/reload"),
    CODEXUI_CLIENT_REQUEST("mcpServerStatus/list"),
    CODEXUI_CLIENT_REQUEST("mcpServer/resource/read"),
    CODEXUI_CLIENT_REQUEST("mcpServer/event/stream/start"),
    CODEXUI_CLIENT_REQUEST("mcpServer/event/stream/stop"),
    CODEXUI_CLIENT_REQUEST("mcpServer/tool/call"),
    CODEXUI_CLIENT_REQUEST("windowsSandbox/setupStart"),
    CODEXUI_CLIENT_REQUEST("windowsSandbox/readiness"),
    CODEXUI_CLIENT_REQUEST("account/login/start"),
    CODEXUI_CLIENT_REQUEST("account/bedrock/discover"),
    CODEXUI_CLIENT_REQUEST("account/bedrock/setup"),
    CODEXUI_CLIENT_REQUEST("account/login/cancel"),
    CODEXUI_CLIENT_REQUEST("account/logout"),
    CODEXUI_CLIENT_REQUEST("account/rateLimits/read"),
    CODEXUI_CLIENT_REQUEST("account/rateLimitResetCredit/consume"),
    CODEXUI_CLIENT_REQUEST("account/usage/read"),
    CODEXUI_CLIENT_REQUEST("account/workspaceMessages/read"),
    CODEXUI_CLIENT_REQUEST("account/sendAddCreditsNudgeEmail"),
    CODEXUI_CLIENT_REQUEST("feedback/upload"),
    CODEXUI_CLIENT_REQUEST("command/exec"),
    CODEXUI_CLIENT_REQUEST("command/exec/write"),
    CODEXUI_CLIENT_REQUEST("command/exec/terminate"),
    CODEXUI_CLIENT_REQUEST("command/exec/resize"),
    CODEXUI_CLIENT_REQUEST("process/spawn"),
    CODEXUI_CLIENT_REQUEST("process/writeStdin"),
    CODEXUI_CLIENT_REQUEST("process/kill"),
    CODEXUI_CLIENT_REQUEST("process/resizePty"),
    CODEXUI_CLIENT_REQUEST("config/read"),
    CODEXUI_CLIENT_REQUEST("externalAgentConfig/detect"),
    CODEXUI_CLIENT_REQUEST("externalAgentConfig/import"),
    CODEXUI_CLIENT_REQUEST("externalAgentConfig/import/recordHistory"),
    CODEXUI_CLIENT_REQUEST("externalAgentConfig/import/readHistories"),
    CODEXUI_CLIENT_REQUEST("config/value/write"),
    CODEXUI_CLIENT_REQUEST("config/batchWrite"),
    CODEXUI_CLIENT_REQUEST("configRequirements/read"),
    CODEXUI_CLIENT_REQUEST("account/read"),
    CODEXUI_CLIENT_REQUEST("getConversationSummary"),
    CODEXUI_CLIENT_REQUEST("gitDiffToRemote"),
    CODEXUI_CLIENT_REQUEST("getAuthStatus"),
    CODEXUI_CLIENT_REQUEST("fuzzyFileSearch"),
    CODEXUI_CLIENT_REQUEST("fuzzyFileSearch/sessionStart"),
    CODEXUI_CLIENT_REQUEST("fuzzyFileSearch/sessionUpdate"),
    CODEXUI_CLIENT_REQUEST("fuzzyFileSearch/sessionStop"),

    CODEXUI_SERVER_REQUEST("item/commandExecution/requestApproval"),
    CODEXUI_SERVER_REQUEST("item/fileChange/requestApproval"),
    CODEXUI_SERVER_REQUEST("item/tool/requestUserInput"),
    CODEXUI_SERVER_REQUEST("mcpServer/elicitation/request"),
    CODEXUI_SERVER_REQUEST("item/permissions/requestApproval"),
    CODEXUI_SERVER_REQUEST("item/tool/call"),
    CODEXUI_SERVER_REQUEST("account/chatgptAuthTokens/refresh"),
    CODEXUI_SERVER_REQUEST("attestation/generate"),
    CODEXUI_SERVER_REQUEST("currentTime/read"),
    CODEXUI_SERVER_REQUEST("applyPatchApproval"),
    CODEXUI_SERVER_REQUEST("execCommandApproval"),

    CODEXUI_UI_NOTIFICATION("error"),
    CODEXUI_GRAPH_NOTIFICATION("thread/started"),
    CODEXUI_GRAPH_NOTIFICATION("thread/status/changed"),
    CODEXUI_GRAPH_NOTIFICATION("thread/archived"),
    CODEXUI_GRAPH_NOTIFICATION("thread/deleted"),
    CODEXUI_GRAPH_NOTIFICATION("thread/unarchived"),
    CODEXUI_GRAPH_NOTIFICATION("thread/closed"),
    CODEXUI_GRAPH_NOTIFICATION("thread/reverted"),
    CODEXUI_GRAPH_NOTIFICATION("skills/changed"),
    CODEXUI_GRAPH_NOTIFICATION("thread/name/updated"),
    CODEXUI_GRAPH_NOTIFICATION("thread/goal/updated"),
    CODEXUI_GRAPH_NOTIFICATION("thread/goal/cleared"),
    CODEXUI_GRAPH_NOTIFICATION("thread/queue/changed"),
    CODEXUI_GRAPH_NOTIFICATION("project/changed"),
    CODEXUI_GRAPH_NOTIFICATION("thread/project/updated"),
    CODEXUI_GRAPH_NOTIFICATION("thread/environment/connected"),
    CODEXUI_GRAPH_NOTIFICATION("thread/environment/disconnected"),
    CODEXUI_GRAPH_NOTIFICATION("thread/settings/updated"),
    CODEXUI_GRAPH_NOTIFICATION("thread/tokenUsage/updated"),
    CODEXUI_GRAPH_NOTIFICATION("turn/started"),
    CODEXUI_GRAPH_NOTIFICATION("hook/started"),
    CODEXUI_GRAPH_NOTIFICATION("turn/completed"),
    CODEXUI_GRAPH_NOTIFICATION("hook/completed"),
    CODEXUI_GRAPH_NOTIFICATION("turn/diff/updated"),
    CODEXUI_GRAPH_NOTIFICATION("turn/plan/updated"),
    CODEXUI_GRAPH_NOTIFICATION("item/started"),
    CODEXUI_GRAPH_NOTIFICATION("item/autoApprovalReview/started"),
    CODEXUI_GRAPH_NOTIFICATION("item/autoApprovalReview/completed"),
    CODEXUI_GRAPH_NOTIFICATION("autoApprovalReview/strictReviewRequired"),
    CODEXUI_GRAPH_NOTIFICATION("item/completed"),
    CODEXUI_NEUTRAL_NOTIFICATION("rawResponseItem/completed"),
    CODEXUI_NEUTRAL_NOTIFICATION("rawResponse/completed"),
    CODEXUI_GRAPH_NOTIFICATION("item/agentMessage/delta"),
    CODEXUI_GRAPH_NOTIFICATION("item/plan/delta"),
    CODEXUI_GRAPH_NOTIFICATION("command/exec/outputDelta"),
    CODEXUI_GRAPH_NOTIFICATION("process/outputDelta"),
    CODEXUI_GRAPH_NOTIFICATION("process/exited"),
    CODEXUI_GRAPH_NOTIFICATION("item/commandExecution/outputDelta"),
    CODEXUI_GRAPH_NOTIFICATION("item/commandExecution/terminalInteraction"),
    CODEXUI_GRAPH_NOTIFICATION("item/fileChange/outputDelta"),
    CODEXUI_GRAPH_NOTIFICATION("item/fileChange/patchUpdated"),
    CODEXUI_GRAPH_NOTIFICATION("serverRequest/resolved"),
    CODEXUI_GRAPH_NOTIFICATION("item/mcpToolCall/progress"),
    CODEXUI_GRAPH_NOTIFICATION("mcpServer/oauthLogin/completed"),
    CODEXUI_GRAPH_NOTIFICATION("mcpServer/startupStatus/updated"),
    CODEXUI_GRAPH_NOTIFICATION("mcpServer/event/stream/notification"),
    CODEXUI_GRAPH_NOTIFICATION("account/updated"),
    CODEXUI_GRAPH_NOTIFICATION("account/rateLimits/updated"),
    CODEXUI_GRAPH_NOTIFICATION("app/list/updated"),
    CODEXUI_GRAPH_NOTIFICATION("remoteControl/status/changed"),
    CODEXUI_GRAPH_NOTIFICATION("externalAgentConfig/import/progress"),
    CODEXUI_GRAPH_NOTIFICATION("externalAgentConfig/import/completed"),
    CODEXUI_GRAPH_NOTIFICATION("fs/changed"),
    CODEXUI_GRAPH_NOTIFICATION("item/reasoning/summaryTextDelta"),
    CODEXUI_GRAPH_NOTIFICATION("item/reasoning/summaryPartAdded"),
    CODEXUI_GRAPH_NOTIFICATION("item/reasoning/textDelta"),
    CODEXUI_GRAPH_NOTIFICATION("thread/compacted"),
    CODEXUI_GRAPH_NOTIFICATION("model/rerouted"),
    CODEXUI_GRAPH_NOTIFICATION("model/verification"),
    CODEXUI_GRAPH_NOTIFICATION("modelProvider/authRecoveryStarted"),
    CODEXUI_GRAPH_NOTIFICATION("modelProvider/authRecoveryCompleted"),
    CODEXUI_GRAPH_NOTIFICATION("turn/moderationMetadata"),
    CODEXUI_GRAPH_NOTIFICATION("model/safetyBuffering/updated"),
    CODEXUI_UI_NOTIFICATION("warning"),
    CODEXUI_UI_NOTIFICATION("guardianWarning"),
    CODEXUI_UI_NOTIFICATION("deprecationNotice"),
    CODEXUI_UI_NOTIFICATION("configWarning"),
    CODEXUI_GRAPH_NOTIFICATION("fuzzyFileSearch/sessionUpdated"),
    CODEXUI_GRAPH_NOTIFICATION("fuzzyFileSearch/sessionCompleted"),
    CODEXUI_GRAPH_NOTIFICATION("thread/realtime/started"),
    CODEXUI_GRAPH_NOTIFICATION("thread/realtime/itemAdded"),
    CODEXUI_GRAPH_NOTIFICATION("thread/realtime/item/started"),
    CODEXUI_GRAPH_NOTIFICATION("thread/realtime/item/transcript/delta"),
    CODEXUI_GRAPH_NOTIFICATION("thread/realtime/item/completed"),
    CODEXUI_GRAPH_NOTIFICATION("thread/realtime/transcript/delta"),
    CODEXUI_GRAPH_NOTIFICATION("thread/realtime/transcript/done"),
    CODEXUI_GRAPH_NOTIFICATION("thread/realtime/outputAudio/delta"),
    CODEXUI_GRAPH_NOTIFICATION("thread/realtime/sdp"),
    CODEXUI_GRAPH_NOTIFICATION("thread/realtime/error"),
    CODEXUI_GRAPH_NOTIFICATION("thread/realtime/closed"),
    CODEXUI_UI_NOTIFICATION("windows/worldWritableWarning"),
    CODEXUI_GRAPH_NOTIFICATION("windowsSandbox/setupCompleted"),
    CODEXUI_GRAPH_NOTIFICATION("account/login/completed"),

    CODEXUI_CLIENT_NOTIFICATION("initialized"),
}};

#undef CODEXUI_CLIENT_REQUEST
#undef CODEXUI_SERVER_REQUEST
#undef CODEXUI_UI_NOTIFICATION
#undef CODEXUI_GRAPH_NOTIFICATION
#undef CODEXUI_NEUTRAL_NOTIFICATION
#undef CODEXUI_CLIENT_NOTIFICATION

constexpr std::size_t countDirection(ProtocolDirection direction) noexcept {
  std::size_t count = 0;
  for (const MethodDescriptor &descriptor : Methods) {
    if (descriptor.direction == direction)
      ++count;
  }
  return count;
}

constexpr std::size_t
countDisposition(MessageDisposition disposition) noexcept {
  std::size_t count = 0;
  for (const MethodDescriptor &descriptor : Methods) {
    if (descriptor.disposition == disposition)
      ++count;
  }
  return count;
}

constexpr bool hasUniqueKeys() noexcept {
  for (std::size_t left = 0; left < Methods.size(); ++left) {
    if (Methods[left].method.empty())
      return false;
    for (std::size_t right = left + 1; right < Methods.size(); ++right) {
      if (Methods[left].direction == Methods[right].direction &&
          Methods[left].method == Methods[right].method)
        return false;
    }
  }
  return true;
}

static_assert(Methods.size() == 252);
static_assert(countDirection(ProtocolDirection::ClientRequest) == 157);
static_assert(countDirection(ProtocolDirection::ServerRequest) == 11);
static_assert(countDirection(ProtocolDirection::ServerNotification) == 83);
static_assert(countDirection(ProtocolDirection::ClientNotification) == 1);
static_assert(countDisposition(MessageDisposition::WorkerOperationResult) ==
              158);
static_assert(countDisposition(MessageDisposition::ReverseInteraction) == 11);
static_assert(countDisposition(MessageDisposition::GraphUpdate) == 75);
static_assert(countDisposition(MessageDisposition::IntentionallyStateNeutral) ==
              2);
static_assert(countDisposition(MessageDisposition::TypedUiEffect) == 6);
static_assert(hasUniqueKeys());

} // namespace

std::span<const MethodDescriptor> protocolMethods() noexcept { return Methods; }

std::optional<std::reference_wrapper<const MethodDescriptor>>
findProtocolMethod(ProtocolDirection direction,
                   std::string_view method) noexcept {
  for (const MethodDescriptor &descriptor : Methods) {
    if (descriptor.direction == direction && descriptor.method == method)
      return std::cref(descriptor);
  }
  return std::nullopt;
}

std::size_t protocolMethodCount(ProtocolDirection direction) noexcept {
  return countDirection(direction);
}

} // namespace codexui::nodegraph
