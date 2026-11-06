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
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utils/Config.h>
#include <web/http/server/SocketContext.h>

namespace codexui::manager {
    namespace {
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
        new MailboxReceiver(toWeb_.descriptor(), [&] {
            static_cast<void>(toWeb_.drain());
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
        std::string password;
        struct stat info{};
        if (::stat(options_.passwordFile.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid() &&
            (info.st_mode & 0077) == 0) {
            std::ifstream file(options_.passwordFile);
            std::getline(file, password);
        }
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
        const auto report = [](const std::string& name, const auto& address, const core::socket::State& state) {
            if (state == core::socket::State::OK) {
                snode::log::application().info() << name << " listening on " << address.toString();
            } else if (state != core::socket::State::DISABLED) {
                snode::log::application().error() << name << ": " << state.what();
            }
        };
        express::legacy::in::Server("manager-web", router, report, [](auto* config) {
            config->setDisabled(true);
            config->setHost("127.0.0.1");
            config->setPort(4510);
        });
        express::tls::in::Server("manager-web-tls", router, report, [](auto* config) {
            config->setDisabled(true);
            config->setHost("127.0.0.1");
            config->setPort(4511);
        });
        const int result = core::SNodeC::start();
        return result < 0 ? 0 : result;
    }
} // namespace codexui::manager
