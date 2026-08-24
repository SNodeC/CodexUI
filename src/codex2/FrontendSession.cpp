// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex2/FrontendSession.h"

#include "codex2/ClientRuntime.h"
#include "codex2/PresentationProtocol.h"
#include "codex2/ipc/QtSocketPairEndpoint.h"
#include "codex2/ipc/SocketPair.h"

#include <ai/openai/codex2/protocol/JsonLineFramer.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace codexui::codex2 {
namespace {

constexpr std::size_t MaximumFrameBytes = 64U * 1024U * 1024U;
constexpr std::size_t MaximumWriteQueueBytes = 128U * 1024U * 1024U;

} // namespace

FrontendSession::FrontendSession(Configuration &configuration)
    : framer(std::make_unique<ai::openai::codex2::protocol::JsonLineFramer>(
          MaximumFrameBytes)),
      configuration(configuration) {
  ipc::SocketPair pair;
  if (!pair.isValid())
    throw std::system_error(pair.error(), std::generic_category(),
                            "unable to create CodexUI socketpair");

  endpoint = std::make_unique<ipc::QtSocketPairEndpoint>(
      pair.releaseFirstEndpoint(), MaximumWriteQueueBytes);
  clientDescriptor = pair.releaseSecondEndpoint();
  endpoint->setOnData([this](const char *data, std::size_t size) {
    framer->consume(
        std::string_view(data, size),
        [this](nlohmann::json message) { receiveMessage(std::move(message)); },
        [this](std::string message) { reportLocalError(std::move(message)); });
  });
  endpoint->setOnError([this](int errorNumber) {
    reportLocalError(std::string("Qt socketpair failure: ") +
                     std::strerror(errorNumber));
  });
  endpoint->setOnClosed([this] {
    if (!stopping) {
      reportLocalError("SNode.C client thread disconnected");
      if (runtimeStoppedHandler)
        runtimeStoppedHandler();
    }
  });
}

FrontendSession::~FrontendSession() { shutdown(); }

void FrontendSession::start(bool connectBridge) {
  if (started)
    return;
  started = true;
  const int descriptor = std::exchange(clientDescriptor, -1);
  clientThread = std::thread([this, descriptor, connectBridge] {
    static_cast<void>(
        runClientRuntime(descriptor, configuration, connectBridge));
  });
}

void FrontendSession::wait() {
  if (clientThread.joinable())
    clientThread.join();
}

void FrontendSession::shutdown() {
  if (stopping)
    return;
  if (started)
    static_cast<void>(sendMessage(presentation::command("runtime.shutdown")));
  stopping = true;
  if (endpoint)
    endpoint->close();
  if (clientDescriptor >= 0) {
    ::close(clientDescriptor);
    clientDescriptor = -1;
  }
  wait();
  pending.clear();
}

void FrontendSession::setEventHandler(EventHandler handler) {
  eventHandler = std::move(handler);
}

void FrontendSession::setRuntimeStoppedHandler(RuntimeStoppedHandler handler) {
  runtimeStoppedHandler = std::move(handler);
}

std::string FrontendSession::request(std::string operation,
                                     nlohmann::json parameters,
                                     ResponseHandler handler) {
  const std::string requestId = "ui-request-" + std::to_string(nextOperation++);
  if (handler)
    pending.emplace(requestId, std::move(handler));
  if (!sendMessage(presentation::command(std::move(operation),
                                         std::move(parameters), requestId))) {
    const auto iterator = pending.find(requestId);
    if (iterator != pending.end()) {
      ResponseHandler failed = std::move(iterator->second);
      pending.erase(iterator);
      failed({{"protocol", presentation::ProtocolName},
              {"version", presentation::ProtocolVersion},
              {"kind", "result"},
              {"correlationId", requestId},
              {"ok", false},
              {"error",
               {{"code", -32020},
                {"message", "CodexUI IPC rejected operation"}}}});
    }
  }
  return requestId;
}

std::string FrontendSession::listThreads(nlohmann::json options,
                                         ResponseHandler handler) {
  return request("threads.list", std::move(options), std::move(handler));
}

std::string FrontendSession::readThread(std::string threadId,
                                        ResponseHandler handler) {
  return request("thread.read",
                 {{"threadId", std::move(threadId)}, {"includeTurns", true}},
                 std::move(handler));
}

std::string FrontendSession::createThread(nlohmann::json options,
                                          ResponseHandler handler) {
  return request("thread.create", std::move(options), std::move(handler));
}

std::string FrontendSession::resumeThread(std::string threadId,
                                          nlohmann::json options,
                                          ResponseHandler handler) {
  options["threadId"] = std::move(threadId);
  return request("thread.resume", std::move(options), std::move(handler));
}

std::string FrontendSession::forkThread(std::string threadId,
                                        nlohmann::json options,
                                        ResponseHandler handler) {
  options["threadId"] = std::move(threadId);
  return request("thread.fork", std::move(options), std::move(handler));
}

std::string FrontendSession::renameThread(std::string threadId,
                                          std::string name,
                                          ResponseHandler handler) {
  return request("thread.rename",
                 {{"threadId", std::move(threadId)}, {"name", std::move(name)}},
                 std::move(handler));
}

std::string FrontendSession::archiveThread(std::string threadId,
                                           ResponseHandler handler) {
  return request("thread.archive", {{"threadId", std::move(threadId)}},
                 std::move(handler));
}

std::string FrontendSession::unarchiveThread(std::string threadId,
                                             ResponseHandler handler) {
  return request("thread.unarchive", {{"threadId", std::move(threadId)}},
                 std::move(handler));
}

std::string FrontendSession::deleteThread(std::string threadId,
                                          ResponseHandler handler) {
  return request("thread.delete", {{"threadId", std::move(threadId)}},
                 std::move(handler));
}

std::string FrontendSession::listModels(nlohmann::json options,
                                        ResponseHandler handler) {
  return request("models.list", std::move(options), std::move(handler));
}

std::string
FrontendSession::readModelProviderCapabilities(nlohmann::json options,
                                               ResponseHandler handler) {
  return request("model-provider-capabilities.read", std::move(options),
                 std::move(handler));
}

std::string FrontendSession::readAccount(nlohmann::json options,
                                         ResponseHandler handler) {
  return request("account.read", std::move(options), std::move(handler));
}

std::string FrontendSession::readAccountRateLimits(ResponseHandler handler) {
  return request("account.rate-limits.read", nlohmann::json::object(),
                 std::move(handler));
}

std::string FrontendSession::readAccountTokenUsage(ResponseHandler handler) {
  return request("account.token-usage.read", nlohmann::json::object(),
                 std::move(handler));
}

std::string FrontendSession::readConfig(nlohmann::json options,
                                        ResponseHandler handler) {
  return request("config.read", std::move(options), std::move(handler));
}

std::string FrontendSession::listPermissionProfiles(nlohmann::json options,
                                                    ResponseHandler handler) {
  return request("permission-profiles.list", std::move(options),
                 std::move(handler));
}

std::string FrontendSession::listExperimentalFeatures(nlohmann::json options,
                                                      ResponseHandler handler) {
  return request("experimental-features.list", std::move(options),
                 std::move(handler));
}

std::string FrontendSession::listSkills(nlohmann::json options,
                                        ResponseHandler handler) {
  return request("skills.list", std::move(options), std::move(handler));
}

std::string FrontendSession::listHooks(nlohmann::json options,
                                       ResponseHandler handler) {
  return request("hooks.list", std::move(options), std::move(handler));
}

std::string FrontendSession::listPlugins(nlohmann::json options,
                                         ResponseHandler handler) {
  return request("plugins.list", std::move(options), std::move(handler));
}

std::string FrontendSession::listApps(nlohmann::json options,
                                      ResponseHandler handler) {
  return request("apps.list", std::move(options), std::move(handler));
}

std::string FrontendSession::listMcpServers(nlohmann::json options,
                                            ResponseHandler handler) {
  return request("mcp-servers.list", std::move(options), std::move(handler));
}

std::string FrontendSession::startTurn(std::string threadId,
                                       nlohmann::json input,
                                       nlohmann::json options,
                                       ResponseHandler handler) {
  options["threadId"] = std::move(threadId);
  options["input"] = std::move(input);
  return request("turn.start", std::move(options), std::move(handler));
}

std::string FrontendSession::steerTurn(std::string threadId,
                                       std::string expectedTurnId,
                                       nlohmann::json input,
                                       ResponseHandler handler) {
  return request("turn.steer",
                 {{"threadId", std::move(threadId)},
                  {"expectedTurnId", std::move(expectedTurnId)},
                  {"input", std::move(input)}},
                 std::move(handler));
}

std::string FrontendSession::interruptTurn(std::string threadId,
                                           std::string turnId,
                                           ResponseHandler handler) {
  return request(
      "turn.interrupt",
      {{"threadId", std::move(threadId)}, {"turnId", std::move(turnId)}},
      std::move(handler));
}

bool FrontendSession::respondToServerRequest(nlohmann::json requestId,
                                             nlohmann::json result,
                                             nlohmann::json error) {
  nlohmann::json data{{"requestId", std::move(requestId)}};
  if (!error.is_null())
    data["error"] = std::move(error);
  else
    data["result"] = std::move(result);
  return sendMessage(
      presentation::command("pending-request.resolve", std::move(data)));
}

bool FrontendSession::sendRaw(nlohmann::json appServerMessage) {
  return sendMessage(presentation::command(
      "diagnostic.raw.send", {{"message", std::move(appServerMessage)}}));
}

bool FrontendSession::reconnect() {
  return sendMessage(presentation::command("connection.reconnect"));
}

bool FrontendSession::claimController() {
  return sendMessage(presentation::command("controller.claim"));
}

bool FrontendSession::releaseController() {
  return sendMessage(presentation::command("controller.release"));
}

bool FrontendSession::sendMessage(const nlohmann::json &message) {
  if (!endpoint || stopping || !endpoint->isOpen())
    return false;
  try {
    return endpoint->send(ai::openai::codex2::protocol::JsonLineFramer::encode(
        message, MaximumFrameBytes));
  } catch (const std::exception &exception) {
    reportLocalError(exception.what());
    return false;
  }
}

void FrontendSession::receiveMessage(nlohmann::json message) {
  if (!presentation::isPresentationFrame(message)) {
    reportLocalError(
        "SNode.C client emitted an incompatible presentation frame");
    return;
  }
  if (presentation::stringMember(message, "kind") == "result") {
    const std::string requestId =
        presentation::stringMember(message, "correlationId");
    const auto iterator = pending.find(requestId);
    if (iterator != pending.end()) {
      ResponseHandler handler = std::move(iterator->second);
      pending.erase(iterator);
      if (handler)
        handler(message);
    }
  }
  if (eventHandler)
    eventHandler(message);
}

void FrontendSession::reportLocalError(std::string message) {
  if (eventHandler)
    eventHandler(presentation::event(0, 0, "system.local-diagnostic",
                                     {{"source", "qt"},
                                      {"code", "local-ipc-error"},
                                      {"message", std::move(message)}}));
}

} // namespace codexui::codex2
