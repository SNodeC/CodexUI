#include "codex/NodeGraphJson.h"
#include "codex/middle/ConversationCards.h"
#include "codex/nodegraph/NodeGraph.h"
#include "codex/ui/NodeGraphUiAdapter.h"
#include "codex/ui/UiStyle.h"

#include <QApplication>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
using namespace codexui::codex;
using namespace codexui::nodegraph;
using Clock = std::chrono::steady_clock;
double micros(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}
double percentile(std::vector<double> values, double fraction) {
    std::ranges::sort(values);
    return values[static_cast<std::size_t>((values.size() - 1) * fraction)];
}
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setStyleSheet(codexui::UiStyle::applicationStyleSheet());
    if (argc != 4) {
        throw std::invalid_argument("Usage: benchmark <item-count> <baseline|linked> <fixture.json>");
    }
    const int count = std::stoi(argv[1]);
    const bool linked = std::string_view(argv[2]) == "linked";
    std::ifstream file(argv[3]);
    nlohmann::json fixture;
    file >> fixture;
    nlohmann::json entry;
    for (const auto& candidate : fixture["cases"]) {
        if (candidate["id"] == "two-independent-choices") {
            entry = candidate;
        }
    }
    const auto source = entry[linked ? "wire" : "text"].get<std::string>();
    NodeGraph graph;
    NodeRef thread, turn, question, reply, streaming;
    const auto initial = Clock::now();
    {
        auto write = graph.write();
        thread = write.upsert({NodeKind::Thread, "t"}, {NodeStatus::Running, {{"type", "thread"}, {"hydrationState", "ready"}}});
        turn = write.upsert({NodeKind::Turn, "turn"}, {NodeStatus::Running, {{"type", "turn"}}});
        write.setParent(thread, turn);
        for (int i = 0; i < count; ++i) {
            streaming = write.upsert({NodeKind::Item, "item-" + std::to_string(i)},
                                     {NodeStatus::Completed, {{"type", "agentMessage"}, {"text", "Prior ordinary message"}}});
            write.setParent(turn, streaming);
        }
        question = write.upsert({NodeKind::Item, "question"},
                                {NodeStatus::Completed,
                                 objectFromJson({{"type", "agentMessage"},
                                                 {"protocolId", "question"},
                                                 {"delivery", "async"},
                                                 {"questions", entry["questions"]},
                                                 {"text", entry["text"]}})});
        write.setParent(turn, question);
        static_cast<void>(write.finish());
    }
    ui::NodeGraphUiAdapter adapter(graph);
    const auto snapshot = adapter.conversation(thread);
    const double history = micros(initial);
    const auto projected = adapter.conversationDelta(thread, std::array{question}, false);
    middle::ConversationCard card(projected->presentations[0], false, nullptr, 900);
    card.resize(900, card.settleHeightForWidth(900));
    card.show();
    QApplication::processEvents();
    std::unique_ptr<middle::ConversationCard> localCard;
    std::vector<double> pending, accepted, unrelated;
    for (int cycle = 0; cycle < 160; ++cycle) {
        for (const auto& state : {std::string("inFlight"), std::string("awaitingMaterialization")}) {
            const auto start = Clock::now();
            {
                auto write = graph.write();
                reply = write.upsert(
                    {NodeKind::Item, "local-reply"},
                    {NodeStatus::Running, {{"type", "localPrompt"}, {"text", source}, {"submissionId", 1}, {"dispatchState", state}}});
                write.setParent(turn, reply);
                static_cast<void>(write.finish());
            }
            const auto delta = adapter.conversationDelta(thread, std::array{question, reply}, false);
            for (const auto& data : delta->presentations) {
                if (data.target == question) {
                    static_cast<void>(card.applyPresentation(data));
                } else if (!localCard) {
                    localCard = std::make_unique<middle::ConversationCard>(data, false, nullptr, 900);
                    localCard->show();
                } else {
                    static_cast<void>(localCard->applyPresentation(data));
                }
            }
            card.resize(900, card.settleHeightForWidth(900));
            localCard->resize(900, localCard->settleHeightForWidth(900));
            QApplication::processEvents();
            static_cast<void>(card.grab());
            static_cast<void>(localCard->grab());
            if (cycle >= 20) {
                (state == "inFlight" ? pending : accepted).push_back(micros(start));
            }
        }
        const auto start = Clock::now();
        {
            auto write = graph.write();
            write.setField(streaming, "text", "Stream " + std::to_string(cycle));
            static_cast<void>(write.finish());
        }
        static_cast<void>(adapter.conversationDelta(thread, std::array{streaming}, false));
        if (cycle >= 20) {
            unrelated.push_back(micros(start));
        }
    }
    std::cout << "{\"items\":" << count << ",\"linked\":" << linked << ",\"initial_history_ms\":" << history / 1000
              << ",\"pending_p50_us\":" << percentile(pending, .5) << ",\"pending_p95_us\":" << percentile(pending, .95)
              << ",\"accepted_p50_us\":" << percentile(accepted, .5) << ",\"accepted_p95_us\":" << percentile(accepted, .95)
              << ",\"unrelated_p50_us\":" << percentile(unrelated, .5) << ",\"unrelated_p95_us\":" << percentile(unrelated, .95) << "}\n";
    const bool withinBudget = percentile(pending, .95) < 5000 && percentile(accepted, .95) < 5000 && percentile(unrelated, .95) < 250;
    return withinBudget ? 0 : 1;
}
