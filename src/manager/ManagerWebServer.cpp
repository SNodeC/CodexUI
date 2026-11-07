// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#include "manager/ManagerWebServer.h"

#include "WebColors.h"

#include <Log.h>
#include <core/SNodeC.h>
#include <core/eventreceiver/ReadEventReceiver.h>
#include <core/socket/stream/SocketConnection.h>
#include <express/legacy/in/Server.h>
#include <express/middleware/BasicAuthentication.h>
#include <express/middleware/JsonMiddleware.h>
#include <express/middleware/StaticMiddleware.h>
#include <express/tls/in/Server.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <utils/Config.h>
#include <vector>
#include <web/http/server/SocketContext.h>

namespace codexui::manager {
    namespace {
        std::string webPassword(const std::string& path) {
            std::string password;
            struct stat info{};
            if (::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid() && (info.st_mode & 0077) == 0) {
                std::ifstream file(path);
                std::getline(file, password);
            }
            return password;
        }
        class MailboxReceiver final : private core::eventreceiver::ReadEventReceiver {
        public:
            MailboxReceiver(int fd, std::function<void()> drain)
                : ReadEventReceiver("Manager mailbox", snode::log::Scope{.component = "codexmanager.web", .identity = {}}, TIMEOUT::DISABLE)
                , drain_(std::move(drain)) {
                if (!enable(fd)) {
                    throw std::runtime_error("Cannot observe the manager mailbox");
                }
            }

        private:
            void readEvent() override {
                drain_();
            }
            void destruct() override {
                disable();
            }
            void shutdownEvent(const core::ShutdownContext&) override {
                disable();
            }
            void unobservedEvent() override {
                delete this;
            }
            std::function<void()> drain_;
        };
    } // namespace
    ManagerOptions initializeManager(int argc, char* argv[], const std::string& profiles, const std::string& webRoot) {
        auto& root = utils::Config::configRoot;
        auto* path = root.addOption("--profiles", "Profile-store JSON file", "FILE", profiles, CLI::Validator{});
        auto* screenshot = root.addOption("--screenshot", "Save a Qt preview and exit", "FILE", std::string{}, CLI::Validator{});
        root.setConfigurable(screenshot, false);
        auto* assets = root.addOption("--manager-web-root", "Installed manager web assets", "DIRECTORY", webRoot, CLI::Validator{});
        auto* user = root.addOption("--manager-web-user", "Manager HTTP Basic username", "USER", std::string{"manager"}, CLI::Validator{});
        auto* password = root.addOption(
            "--manager-web-password-file", "Owner-only file containing the HTTP Basic password", "FILE", std::string{}, CLI::Validator{});
        core::SNodeC::init(argc, argv);
        root.getOption("--daemonize")->check(CLI::IsMember({"false"}));
        bool configurationOnly = false;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            configurationOnly = configurationOnly || argument == "-h" || argument.starts_with("--help") || argument == "-s" ||
                                argument == "--show-config" || argument == "-w" || argument == "--write-config" || argument == "-v" ||
                                argument == "--version" || argument == "-k" || argument == "--kill" ||
                                argument.starts_with("--command-line");
        }
        return {path->as<std::string>(),
                screenshot->as<std::string>(),
                assets->as<std::string>(),
                user->as<std::string>(),
                password->as<std::string>(),
                configurationOnly};
    }
    ManagerWebServer::ManagerWebServer(ManagerOptions options)
        : options_(std::move(options)) {
        if (!toQt_.valid() || !toWeb_.valid()) {
            throw std::runtime_error("Cannot create manager eventfds");
        }
    }
    ManagerWebServer::~ManagerWebServer() {
        stopping_.store(true);
        static_cast<void>(toWeb_.notify());
        static_cast<void>(wait());
    }
    int ManagerWebServer::wait() {
        if (thread_.joinable()) {
            thread_.join();
        }
        return result_.load();
    }
    void prepareManagerSignals() {
        // Let SNode.C receive its process signals in its own event-loop wait.
        // Qt must not consume SIGTERM while the network thread sleeps in epoll.
        sigset_t signals;
        ::sigemptyset(&signals);
        for (const int signal : {SIGINT, SIGTERM, SIGALRM, SIGHUP}) {
            ::sigaddset(&signals, signal);
        }
        if (::pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
            throw std::runtime_error("Cannot configure manager event-loop signal ownership");
        }
    }
    void ManagerWebServer::start() {
        thread_ = std::thread([this] {
            try {
                result_.store(run());
            } catch (const std::exception& error) {
                result_.store(1);
                snode::log::application().error() << "Manager web UI: " << error.what();
            }
            static_cast<void>(toQt_.notify());
        });
    }
    bool ManagerWebServer::stopped() const {
        return result_.load() != -1;
    }
    int ManagerWebServer::descriptor() const {
        return toQt_.descriptor();
    }
    void ManagerWebServer::drainWake() {
        static_cast<void>(toQt_.drain());
    }
    bool ManagerWebServer::take(Request& request) {
        return !replies_.full() && requests_.tryPop(request);
    }
    void ManagerWebServer::reply(Reply reply) {
        const bool queued = replies_.tryPush(std::move(reply));
        if (!queued) {
            throw std::runtime_error("Manager reply capacity invariant violated");
        }
        static_cast<void>(toWeb_.notify());
    }
    bool ManagerWebServer::configure(std::string request) {
        const bool queued = settingsRequests_.tryPush(std::move(request));
        static_cast<void>(toWeb_.notify());
        return queued;
    }
    bool ManagerWebServer::configuration(std::string& result) {
        const bool received = settingsResults_.tryPop(result);
        if (received) {
            static_cast<void>(toWeb_.notify());
        }
        return received;
    }
    void ManagerWebServer::changed() {
        changed_.store(true);
        static_cast<void>(toWeb_.notify());
    }
    int ManagerWebServer::run() {
        std::map<std::uint64_t, std::shared_ptr<express::Response>> pending, streams;
        std::uint64_t sequence = 0;
        const auto sendChange = [](const std::shared_ptr<express::Response>& res) {
            auto* socket = res->getSocketContext()->getSocketConnection();
            if (socket->getTotalQueued() - socket->getTotalSent() > 65536) {
                socket->close();
            } else {
                res->sendFragment("data: changed\n\n");
            }
        };
        const auto password = webPassword(options_.passwordFile);
        const express::Router router;
        router.use([valid = !password.empty()] MIDDLEWARE(req, res, next) {
            res->set({{"Cache-Control", "no-store"},
                      {"X-Content-Type-Options", "nosniff"},
                      {"Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'"}});
            if (valid) {
                next();
            } else {
                res->status(503).send("Configure an owner-only manager web password file.");
            }
        });
        router.use(express::middleware::BasicAuthentication(options_.user, password, "CodexManager"));
        router.use("/api", [] MIDDLEWARE(req, res, next) {
            const auto origin = req->get("Origin");
            if (req->method != "GET" &&
                (req->get("X-CodexManager-Request") != "1" ||
                 (!origin.empty() && origin != "http://" + req->get("Host") && origin != "https://" + req->get("Host")))) {
                res->status(403).send("Cross-origin manager mutation rejected.");
            } else {
                next();
            }
        });
        router.use("/api", express::middleware::JsonMiddleware());
        const auto dispatch = [&](std::string action, std::string body, const std::shared_ptr<express::Response>& res) {
            const auto id = ++sequence;
            Request request{id, std::move(action), std::move(body)};
            if (pending.size() >= requests_.capacity() || !requests_.tryPush(std::move(request))) {
                res->status(503).send("Manager request queue is full; no operation was accepted.");
            } else {
                pending.emplace(id, res);
                res->getSocketContext()->setOnDisconnected([&, id] {
                    pending.erase(id);
                });
                static_cast<void>(toQt_.notify());
            }
        };
        router.get("/api/state", [&] APPLICATION(req, res) {
            dispatch("state", nlohmann::json{{"id", req->query("profile")}}.dump(), res);
        });
        router.post("/api/:action", [&] APPLICATION(req, res) {
            req->getAttribute<nlohmann::json>(
                [&](const nlohmann::json& body) {
                    if (body.is_object()) {
                        dispatch(req->params.at("action"), body.dump(), res);
                    } else {
                        res->status(400).send("Expected a JSON object.");
                    }
                },
                [res](const std::string&) {
                    res->status(400).send("Expected JSON.");
                });
        });
        router.get("/api/events", [&] APPLICATION(req, res) {
            const auto id = ++sequence;
            streams.emplace(id, res);
            res->getSocketContext()->setOnDisconnected([&, id] {
                streams.erase(id);
            });
            res->set({{"Content-Type", "text/event-stream"}, {"Cache-Control", "no-cache"}}).sendHeader();
            sendChange(res);
        });
        router.use("/api", [] APPLICATION(req, res) {
            res->sendStatus(404);
        });
        router.use([] MIDDLEWARE(req, res, next) {
            if (req->path == "/" || req->path == "/index.html" || req->path == "/app.js" || req->path == "/style.css" ||
                req->path == "/colors.css") {
                next();
            } else {
                res->sendStatus(404);
            }
        });
        router.get("/colors.css", [] APPLICATION(req, res) {
            res->type("text/css").send(CODEXMANAGER_WEB_COLORS);
        });
        router.use(express::middleware::StaticMiddleware(options_.webRoot));
        nlohmann::json running = {{"manager-web", {{"status", "Disabled"}, {"url", ""}}},
                                  {"manager-web-tls", {{"status", "Disabled"}, {"url", ""}}}};
        const auto report = [&](const std::string& name, const auto& address, const core::socket::State& state) {
            const auto host = address.getHost() == "0.0.0.0" ? "127.0.0.1" : address.getHost();
            running[name] = {{"status", state.what()},
                             {"url",
                              state == core::socket::State::OK
                                  ? (name == "manager-web" ? "http://" : "https://") + host + ":" + std::to_string(address.getPort()) + "/"
                                  : ""}};
            if (state == core::socket::State::OK) {
                snode::log::application().info() << name << " listening on " << address.toString();
            } else if (state != core::socket::State::DISABLED) {
                snode::log::application().error() << name << ": " << state.what();
            }
        };
        auto http = express::legacy::in::Server("manager-web", router, report, [](auto* config) {
            config->setDisabled(true);
            config->setHost("127.0.0.1");
            config->setPort(4510);
        });
        auto https = express::tls::in::Server("manager-web-tls", router, report, [](auto* config) {
            config->setDisabled(true);
            config->setHost("127.0.0.1");
            config->setPort(4511);
        });
        struct Field {
            utils::SubCommand* owner;
            std::string name, label;
        };
        std::vector<Field> fields;
        const auto add = [&](utils::SubCommand* owner, const std::string& name, const std::string& label) {
            owner->option(name)->promoteDefault();
            fields.push_back({owner, name, label});
        };
        auto& root = utils::Config::configRoot;
        add(&root, "--manager-web-user", "Username");
        add(&root, "--manager-web-password-file", "Password file");
        const auto listener = [&](auto* config, const std::string& prefix) {
            add(static_cast<net::config::ConfigInstance*>(config), "--disabled", "Enable " + prefix);
            add(static_cast<typename std::remove_pointer_t<decltype(config)>::Local*>(config), "--host", prefix + " bind address");
            add(static_cast<typename std::remove_pointer_t<decltype(config)>::Local*>(config), "--port", prefix + " port");
        };
        listener(http.getConfig(), "HTTP");
        listener(https.getConfig(), "HTTPS");
        add(static_cast<decltype(https)::Config::Tls*>(https.getConfig()), "--cert", "HTTPS certificate");
        add(static_cast<decltype(https)::Config::Tls*>(https.getConfig()), "--cert-key", "HTTPS private key");
        const auto settings = [&](const std::string& text) {
            auto response = nlohmann::json::parse(text);
            const auto paths = root.getOption("--config-file")->as<std::vector<std::string>>();
            const auto path = paths.empty() ? root.getOption("--write-config")->as<std::string>() : paths.front();
            std::vector<std::pair<std::string, CLI::results_t>> previous;
            for (const auto& field : fields) {
                const auto* option = field.owner->getOption(field.name);
                previous.emplace_back(option->get_default_str(), option->results());
            }
            try {
                if (response.contains("values")) {
                    const auto values = response.at("values").get<std::vector<std::string>>();
                    if (values.size() != fields.size()) {
                        throw std::runtime_error("Incomplete settings form.");
                    }
                    for (std::size_t index = 0; index < fields.size(); ++index) {
                        const auto& field = fields[index];
                        if (!field.owner->setDefaultValue(field.owner->getOption(field.name), values[index])) {
                            throw std::runtime_error("Invalid " + field.label);
                        }
                        field.owner->option(field.name)->promoteDefault();
                    }
                    if ((!http.getConfig()->getDisabled() || !https.getConfig()->getDisabled()) &&
                        (root.getOption("--manager-web-user")->as<std::string>().empty() ||
                         webPassword(root.getOption("--manager-web-password-file")->as<std::string>()).empty())) {
                        throw std::runtime_error("Enabled web access requires a username and an owner-only password file.");
                    }
                    if (!https.getConfig()->getDisabled() &&
                        (https.getConfig()->getCert().empty() || https.getConfig()->getCertKey().empty())) {
                        throw std::runtime_error("HTTPS requires a certificate and private key.");
                    }
                    if ((std::filesystem::exists(path) && !std::filesystem::is_regular_file(path)) || !utils::Config::writeConfig(path)) {
                        throw std::runtime_error("Could not save configuration to " + path);
                    }
                    response["saved"] = true;
                }
            } catch (const std::exception& error) {
                response["error"] = error.what();
                for (std::size_t index = 0; index < fields.size(); ++index) {
                    auto* option = fields[index].owner->getOption(fields[index].name);
                    option->default_str(previous[index].first);
                    option->clear();
                    option->add_result(previous[index].second);
                    if (const auto callback = option->get_callback()) {
                        static_cast<void>(callback(previous[index].second));
                    }
                }
            }
            response.erase("values");
            response["path"] = path;
            response["running"] = running;
            response["fields"] = nlohmann::json::array();
            for (const auto& field : fields) {
                const auto* option = field.owner->getOption(field.name);
                response["fields"].push_back({{"label", field.label},
                                              {"option", field.name},
                                              {"value", option->results().empty() ? option->get_default_str() : option->results().back()}});
            }
            return response.dump();
        };
        new MailboxReceiver(toWeb_.descriptor(), [&] {
            static_cast<void>(toWeb_.drain());
            std::string request;
            while (!settingsResults_.full() && settingsRequests_.tryPop(request)) {
                static_cast<void>(settingsResults_.tryPush(settings(request)));
            }
            Reply reply;
            while (replies_.tryPop(reply)) {
                const auto it = pending.find(reply.id);
                if (it != pending.end()) {
                    const auto response = it->second;
                    pending.erase(it);
                    if (response->isConnected()) {
                        response->status(reply.status).type("application/json").send(reply.body);
                    }
                }
            }
            static_cast<void>(toQt_.notify());
            if (changed_.exchange(false)) {
                // Invalidation only; profile state remains owned by Qt. Reconnect fetches a fresh snapshot.
                const auto subscribers = streams;
                for (const auto& [id, response] : subscribers) {
                    static_cast<void>(id);
                    if (response->isConnected()) {
                        sendChange(response);
                    }
                }
            }
            if (stopping_.load()) {
                core::SNodeC::stop();
            }
        });
        const int result = core::SNodeC::start();
        return result < 0 ? 0 : result;
    }
} // namespace codexui::manager
