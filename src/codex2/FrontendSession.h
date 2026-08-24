// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#ifndef CODEXUI_CODEX2_FRONTENDSESSION_H
#define CODEXUI_CODEX2_FRONTENDSESSION_H

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace ai::openai::codex2::protocol {
class JsonLineFramer;
}

namespace codexui::codex2::ipc {
class QtSocketPairEndpoint;
}

namespace codexui::codex2 {

class Configuration;

class FrontendSession final {
public:
  using EventHandler = std::function<void(const nlohmann::json &)>;
  using ResponseHandler = std::function<void(const nlohmann::json &)>;
  using RuntimeStoppedHandler = std::function<void()>;

  explicit FrontendSession(Configuration &configuration);
  ~FrontendSession();

  FrontendSession(const FrontendSession &) = delete;
  FrontendSession &operator=(const FrontendSession &) = delete;

  void start(bool connectBridge = true);
  void wait();
  void shutdown();
  void setEventHandler(EventHandler handler);
  void setRuntimeStoppedHandler(RuntimeStoppedHandler handler);

  std::string request(std::string operation, nlohmann::json parameters,
                      ResponseHandler handler = {});
  std::string listThreads(nlohmann::json options = nlohmann::json::object(),
                          ResponseHandler handler = {});
  std::string readThread(std::string threadId, ResponseHandler handler = {});
  std::string createThread(nlohmann::json options,
                           ResponseHandler handler = {});
  std::string resumeThread(std::string threadId,
                           nlohmann::json options = nlohmann::json::object(),
                           ResponseHandler handler = {});
  std::string forkThread(std::string threadId,
                         nlohmann::json options = nlohmann::json::object(),
                         ResponseHandler handler = {});
  std::string renameThread(std::string threadId, std::string name,
                           ResponseHandler handler = {});
  std::string archiveThread(std::string threadId, ResponseHandler handler = {});
  std::string unarchiveThread(std::string threadId,
                              ResponseHandler handler = {});
  std::string deleteThread(std::string threadId, ResponseHandler handler = {});
  std::string listModels(nlohmann::json options = nlohmann::json::object(),
                         ResponseHandler handler = {});
  std::string readModelProviderCapabilities(
      nlohmann::json options = nlohmann::json::object(),
      ResponseHandler handler = {});
  std::string readAccount(nlohmann::json options = nlohmann::json::object(),
                          ResponseHandler handler = {});
  std::string readAccountRateLimits(ResponseHandler handler = {});
  std::string readAccountTokenUsage(ResponseHandler handler = {});
  std::string readConfig(nlohmann::json options = nlohmann::json::object(),
                         ResponseHandler handler = {});
  std::string
  listPermissionProfiles(nlohmann::json options = nlohmann::json::object(),
                         ResponseHandler handler = {});
  std::string
  listExperimentalFeatures(nlohmann::json options = nlohmann::json::object(),
                           ResponseHandler handler = {});
  std::string listSkills(nlohmann::json options = nlohmann::json::object(),
                         ResponseHandler handler = {});
  std::string listHooks(nlohmann::json options = nlohmann::json::object(),
                        ResponseHandler handler = {});
  std::string listPlugins(nlohmann::json options = nlohmann::json::object(),
                          ResponseHandler handler = {});
  std::string listApps(nlohmann::json options = nlohmann::json::object(),
                       ResponseHandler handler = {});
  std::string listMcpServers(nlohmann::json options = nlohmann::json::object(),
                             ResponseHandler handler = {});
  std::string startTurn(std::string threadId, nlohmann::json input,
                        nlohmann::json options = nlohmann::json::object(),
                        ResponseHandler handler = {});
  std::string steerTurn(std::string threadId, std::string expectedTurnId,
                        nlohmann::json input, ResponseHandler handler = {});
  std::string interruptTurn(std::string threadId, std::string turnId,
                            ResponseHandler handler = {});
  bool respondToServerRequest(nlohmann::json requestId, nlohmann::json result,
                              nlohmann::json error = nullptr);
  bool sendRaw(nlohmann::json appServerMessage);
  bool reconnect();
  bool claimController();
  bool releaseController();

private:
  bool sendMessage(const nlohmann::json &message);
  void receiveMessage(nlohmann::json message);
  void reportLocalError(std::string message);

  std::unique_ptr<ipc::QtSocketPairEndpoint> endpoint;
  std::unique_ptr<ai::openai::codex2::protocol::JsonLineFramer> framer;
  std::thread clientThread;
  int clientDescriptor = -1;
  std::uint64_t nextOperation = 1;
  std::unordered_map<std::string, ResponseHandler> pending;
  EventHandler eventHandler;
  RuntimeStoppedHandler runtimeStoppedHandler;
  bool started = false;
  bool stopping = false;
  Configuration &configuration;
};

} // namespace codexui::codex2

#endif // CODEXUI_CODEX2_FRONTENDSESSION_H
