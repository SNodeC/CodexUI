// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
#pragma once
#include "codex/nodegraph/EventFd.h"
#include "codex/nodegraph/SpscQueue.h"

#include <atomic>
#include <string>
#include <thread>

namespace codexui::manager {
    struct ManagerOptions {
        std::string profiles, screenshot, webRoot, user, passwordFile;
        bool configurationOnly = false;
    };
    void prepareManagerSignals();
    ManagerOptions initializeManager(int argc, char* argv[], const std::string& profiles, const std::string& webRoot);
    class ManagerWebServer final {
    public:
        static constexpr std::size_t Capacity = 64;
        struct Request {
            std::uint64_t id = 0;
            std::string action, body;
        };
        struct Reply {
            std::uint64_t id = 0;
            int status = 200;
            std::string body;
        };
        explicit ManagerWebServer(ManagerOptions options);
        ~ManagerWebServer();
        void start();
        int wait();
        [[nodiscard]] int descriptor() const;
        void drainWake();
        bool take(Request& request);
        void reply(Reply reply);
        void changed();
        [[nodiscard]] bool stopped() const;

    private:
        int run();
        ManagerOptions options_;
        nodegraph::SpscQueue<Request, Capacity> requests_;
        nodegraph::SpscQueue<Reply, Capacity> replies_;
        nodegraph::EventFd toQt_, toWeb_;
        std::atomic_bool stopping_{false}, changed_{false};
        std::atomic_int result_{-1};
        std::thread thread_;
    };
} // namespace codexui::manager
