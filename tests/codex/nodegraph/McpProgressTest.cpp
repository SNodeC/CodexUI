// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

#include "codex/nodegraph/NodeGraph.h"
#include "codex/nodegraph/ProtocolUpdater.h"
#include "codex/nodegraph/Value.h"

#include <cstddef>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    using namespace codexui::nodegraph;

    void expect(bool condition, std::string_view scenario, std::string_view detail, int& failures) {
        if (!condition) {
            std::cerr << "FAILED: " << scenario << ": " << detail << '\n';
            ++failures;
        }
    }

    void check(const nlohmann::json& scenario, int& failures) {
        NodeGraph graph;
        ProtocolUpdater updater(graph);
        const auto send = [&updater](std::string method, Value::Object payload) {
            static_cast<void>(updater.apply({DecodedMessageKind::ServerNotification, std::move(method), {}, std::move(payload)}));
        };
        const auto start = [&send](std::string thread) {
            send("item/started",
                 {{"threadId", std::move(thread)},
                  {"turnId", "turn"},
                  {"item", Value::Object{{"id", "call"}, {"type", "mcpToolCall"}, {"status", "inProgress"}}}});
        };
        start("first-thread");
        start("other-thread");
        const std::string name = scenario.at("name");
        const auto messages = scenario.at("messages").get<std::vector<std::string>>();
        const auto count = scenario.at("count").get<std::size_t>();
        for (std::size_t i = 0; i < count; ++i) {
            send("item/mcpToolCall/progress",
                 {{"threadId", "first-thread"}, {"turnId", "turn"}, {"itemId", "call"}, {"message", messages[i % messages.size()]}});
        }
        send("item/mcpToolCall/progress",
             {{"threadId", "other-thread"}, {"turnId", "turn"}, {"itemId", "call"}, {"message", "independent"}});
        const auto inspect = [&graph, &scenario, &name, &messages, &failures] {
            auto read = graph.tryRead();
            const auto node = read->find(scopedItemNodeId(scopedTurnNodeId("first-thread", "turn"), "call"));
            const auto state = read->state(node);
            const Value* value = valueMember(state->fields, "progress");
            const auto* progress = value ? value->asArray() : nullptr;
            const std::size_t expected =
                scenario.contains("expected") ? scenario.at("expected").size() : scenario.at("expectedCount").get<std::size_t>();
            expect(progress && progress->size() == expected, name, "complete ordered progress history", failures);
            if (progress) {
                for (std::size_t i = 0; i < progress->size() && i < expected; ++i) {
                    const std::string expectedMessage =
                        scenario.contains("expected") ? scenario.at("expected").at(i).get<std::string>() : messages.front();
                    expect(exactStringFromValue(&(*progress)[i]) == expectedMessage, name, "message order", failures);
                }
            }
            const auto other = read->find(scopedItemNodeId(scopedTurnNodeId("other-thread", "turn"), "call"));
            const auto otherState = read->state(other);
            const Value* otherValue = valueMember(otherState->fields, "progress");
            const auto* otherProgress = otherValue ? otherValue->asArray() : nullptr;
            expect(otherProgress && otherProgress->size() == 1 && exactStringFromValue(&otherProgress->front()) == "independent",
                   name,
                   "equal item ids stay isolated between threads",
                   failures);
        };
        inspect();
        send("item/completed",
             {{"threadId", "first-thread"},
              {"turnId", "turn"},
              {"item",
               Value::Object{{"id", "call"},
                             {"type", "mcpToolCall"},
                             {"status", "completed"},
                             {"result", Value::Object{{"content", Value::Array{}}}}}}});
        inspect();
    }

} // namespace

int main() {
    int failures = 0;
    std::ifstream input(CODEXUI_MCP_PROGRESS_FIXTURE);
    if (input) {
        const auto corpus = nlohmann::json::parse(input);
        for (const auto& scenario : corpus.at("cases")) {
            check(scenario, failures);
        }
    } else {
        expect(false, "fixture", "cannot open shared progress cases", failures);
    }
    return failures == 0 ? 0 : 1;
}
