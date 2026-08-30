// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/FrontendSession.h"

#include "codex/ClientRuntime.h"
#include "codex/PresentationProtocol.h"
#include "codex/ipc/QtSocketPairEndpoint.h"
#include "codex/ipc/SocketPair.h"

#include <ai/openai/codex/protocol/JsonLineFramer.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace codexui::codex {
namespace {

constexpr std::size_t MaximumFrameBytes = 64U * 1024U * 1024U;
constexpr std::size_t MaximumWriteQueueBytes = 128U * 1024U * 1024U;
constexpr std::size_t MaximumOutstandingRequests = 4096;

} // namespace

FrontendSession::FrontendSession(Configuration &configuration)
    : framer(std::make_unique<ai::openai::codex::protocol::JsonLineFramer>(
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
    std::string framingError;
    try {
      const bool accepted = framer->consume(
          std::string_view(data, size),
          [this](nlohmann::json message) { receiveMessage(std::move(message)); },
          [&framingError](std::string message) {
            framingError = std::move(message);
          });
      if (!accepted)
        terminalFailure(framingError.empty() ? "CodexUI IPC framing failed"
                                             : std::move(framingError));
    } catch (const std::exception &exception) {
      terminalFailure(std::string("CodexUI IPC dispatch failed: ") +
                      exception.what());
    } catch (...) {
      terminalFailure("CodexUI IPC dispatch failed with an unknown exception");
    }
  });
  endpoint->setOnError([this](int errorNumber) {
    terminalFailure(std::string("Qt socketpair failure: ") +
                    std::strerror(errorNumber));
  });
  endpoint->setOnClosed([this] {
    failAllPending(-32020, stopping ? "CodexUI is shutting down"
                                    : "SNode.C client thread disconnected");
    if (!stopping) {
      if (!terminal)
        reportLocalError("SNode.C client thread disconnected");
      notifyRuntimeStopped();
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
  if (started && endpoint && endpoint->isOpen() &&
      QCoreApplication::instance() &&
      endpoint->thread() == QThread::currentThread()) {
    QEventLoop acknowledgementLoop;
    const std::string requestId =
        request("runtime.shutdown", nlohmann::json::object(),
                [&acknowledgementLoop](const nlohmann::json &) {
                  acknowledgementLoop.quit();
                });
    QTimer::singleShot(750, &acknowledgementLoop, &QEventLoop::quit);
    acknowledgementLoop.exec();
    // A timeout must not leave a callback capturing the completed nested loop.
    outstanding.erase(requestId);
  } else if (started) {
    static_cast<void>(sendMessage(presentation::command("runtime.shutdown")));
  }
  stopping = true;
  failAllPending(-32800, "CodexUI is shutting down");
  if (endpoint)
    endpoint->close();
  if (clientDescriptor >= 0) {
    ::close(clientDescriptor);
    clientDescriptor = -1;
  }
  wait();
}

void FrontendSession::setEventHandler(EventHandler handler) {
  eventHandler = std::move(handler);
}

void FrontendSession::setActivityHandler(ActivityHandler handler) {
  activityHandler = std::move(handler);
}

void FrontendSession::setRuntimeStoppedHandler(RuntimeStoppedHandler handler) {
  runtimeStoppedHandler = std::move(handler);
}

PresentationClient FrontendSession::presentationClient() {
  return PresentationClient{
      [this](std::string action, nlohmann::json data,
             PresentationClient::Completion completion) {
        return request(std::move(action), std::move(data),
                       std::move(completion));
      },
      [this](std::string action, nlohmann::json data) {
        return sendMessage(
            presentation::command(std::move(action), std::move(data)));
      },
      [this](nlohmann::json requestId, nlohmann::json result,
             nlohmann::json error) {
        return respondToServerRequest(std::move(requestId), std::move(result),
                                      std::move(error));
      }};
}

std::string FrontendSession::request(std::string operation,
                                     nlohmann::json parameters,
                                     ResponseHandler handler) {
  const std::string requestId = "ui-request-" + std::to_string(nextOperation++);
  const std::string threadId =
      presentation::stringMember(parameters, "threadId");
  const std::string action = operation;
  if (outstanding.size() >= MaximumOutstandingRequests) {
    if (handler) {
      try {
        handler(presentation::result(
            0, activeGeneration, action, requestId, false,
            {{"code", -32021},
             {"message", "CodexUI has too many outstanding operations"}}));
      } catch (...) {
      }
    }
    return requestId;
  }
  outstanding.emplace(
      requestId, OutstandingRequest{action, threadId, std::move(handler)});
  const bool sent = sendMessage(presentation::command(
      std::move(operation), std::move(parameters), requestId));
  if (sent && !threadId.empty() && activityHandler)
    activityHandler(threadId);
  if (!sent) {
    const auto iterator = outstanding.find(requestId);
    if (iterator != outstanding.end()) {
      ResponseHandler failed = std::move(iterator->second.completion);
      const std::string failedAction = std::move(iterator->second.action);
      outstanding.erase(iterator);
      if (!failed)
        return requestId;
      try {
        failed(presentation::result(
            0, activeGeneration, failedAction, requestId, false,
            {{"code", -32020},
             {"message", "CodexUI IPC rejected operation"}}));
      } catch (...) {
      }
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

bool FrontendSession::connectTransport() {
  return sendMessage(presentation::command("connection.connect"));
}

bool FrontendSession::disconnectTransport() {
  return sendMessage(presentation::command("connection.disconnect"));
}

std::string FrontendSession::configureConnection(nlohmann::json settings,
                                                 ResponseHandler handler) {
  return request("connection.configure", std::move(settings),
                 std::move(handler));
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
    return endpoint->send(ai::openai::codex::protocol::JsonLineFramer::encode(
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
  const auto generation = message.find("generation");
  if (generation != message.end()) {
    if (!generation->is_number_unsigned()) {
      terminalFailure("presentation frame has an invalid generation");
      return;
    }
    const std::uint64_t incoming = generation->get<std::uint64_t>();
    if (activeGeneration != 0 && incoming < activeGeneration)
      return;
    if (activeGeneration != 0 && incoming > activeGeneration) {
      failAllPending(-32020, "bridge connection generation changed", true);
      lastSequenceReceived = 0;
    }
    activeGeneration = incoming;
  }
  const auto sequence = message.find("sequence");
  if (sequence != message.end()) {
    if (!sequence->is_number_unsigned()) {
      terminalFailure("presentation frame has an invalid sequence");
      return;
    }
    const std::uint64_t incoming = sequence->get<std::uint64_t>();
    if (incoming != 0 && lastSequenceReceived != 0 &&
        incoming != lastSequenceReceived + 1) {
      terminalFailure("presentation frame sequence gap detected");
      return;
    }
    if (incoming != 0)
      lastSequenceReceived = incoming;
  }
  if (presentation::stringMember(message, "kind") == "result") {
    const std::string requestId =
        presentation::stringMember(message, "correlationId");
    const auto iterator = outstanding.find(requestId);
    if (iterator == outstanding.end())
      return;
    if (presentation::stringMember(message, "action") !=
        iterator->second.action) {
      terminalFailure("presentation result action does not match its request");
      return;
    }
    if (!iterator->second.threadId.empty() && activityHandler)
      activityHandler(iterator->second.threadId);
    ResponseHandler handler = std::move(iterator->second.completion);
    outstanding.erase(iterator);
    if (handler) {
      try {
        handler(message);
      } catch (...) {
      }
    }
  }
  if (presentation::stringMember(message, "kind") == "event") {
    const std::string type = presentation::stringMember(message, "type");
    const nlohmann::json data = presentation::member(
        message, "data", nlohmann::json::object());
    if (type == "connection.lifecycle") {
      const std::string state = presentation::stringMember(data, "state");
      if (state == "disconnected" || state == "failure")
        failAllPending(-32020, "bridge connection was lost", true);
    } else if (type == "connection.provider") {
      const auto provider = data.find("generation");
      if (provider == data.end() || !provider->is_number_unsigned()) {
        terminalFailure("provider lifecycle event has an invalid generation");
        return;
      }
      const std::uint64_t incoming = provider->get<std::uint64_t>();
      if (incoming < providerGeneration)
        return;
      if (providerGeneration != 0 && incoming > providerGeneration)
        failAllPending(-32002, "app-server provider generation changed", true);
      providerGeneration = incoming;
      if (presentation::stringMember(data, "state") == "disconnected")
        failAllPending(-32002, "app-server provider was restarted", true);
    }
  }
  if (eventHandler) {
    try {
      eventHandler(message);
    } catch (...) {
    }
  }
}

void FrontendSession::reportLocalError(std::string message) {
  if (eventHandler) {
    try {
      eventHandler(presentation::event(0, activeGeneration,
                                       "system.local-diagnostic",
                                       {{"source", "qt"},
                                        {"code", "local-ipc-error"},
                                        {"message", std::move(message)}}));
    } catch (...) {
    }
  }
}

void FrontendSession::terminalFailure(std::string message) {
  if (terminal || stopping)
    return;
  terminal = true;
  failAllPending(-32020, message);
  reportLocalError(std::move(message));
  if (endpoint && endpoint->isOpen())
    endpoint->close();
  notifyRuntimeStopped();
}

void FrontendSession::failAllPending(int code, std::string message,
                                     bool transient) noexcept {
  auto failed = std::move(outstanding);
  outstanding.clear();
  for (auto &[correlationId, request] : failed) {
    ResponseHandler &handler = request.completion;
    if (!handler)
      continue;
    try {
      nlohmann::json error{{"code", code}, {"message", message}};
      if (transient)
        error["transient"] = true;
      handler(presentation::result(0, activeGeneration, request.action,
                                   correlationId, false, std::move(error)));
    } catch (...) {
    }
  }
}

void FrontendSession::notifyRuntimeStopped() noexcept {
  if (runtimeStopReported)
    return;
  runtimeStopReported = true;
  if (runtimeStoppedHandler) {
    try {
      runtimeStoppedHandler();
    } catch (...) {
    }
  }
}

} // namespace codexui::codex
