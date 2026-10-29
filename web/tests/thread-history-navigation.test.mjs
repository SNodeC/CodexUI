import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import {createElement} from "react";
import {renderToStaticMarkup} from "react-dom/server";
import {App} from "../dist/app/App.js";
import {BrowserFrontendSession, ConversationViewportState, PresentationModel, ProtocolNormalizer,
    projectConversation, stableKey, turnOpenings, turnNavigationTarget} from "../dist/index.js";

const contract = JSON.parse(readFileSync(new URL("../../tests/fixtures/conversation-navigation.json", import.meta.url), "utf8"));

function fixture() {
    const model = new PresentationModel();
    const normalizer = new ProtocolNormalizer(frame => { model.applyEvent(frame); return true; });
    normalizer.serverNotification("thread/started", {thread: {id: contract.threadId}});
    for (const entry of contract.turns) {
        normalizer.serverNotification("turn/started", {threadId: contract.threadId, turn: {id: entry.id}});
        const items = entry.prompt === null ? [{id: "opening", type: "agentMessage", phase: "final_answer", text: "Automatic turn"}]
            : [{id: "opening", type: "userMessage", content: [{type: "text", text: entry.prompt}]},
                {id: "answer", type: "agentMessage", phase: "final_answer", text: "Long response\n\n".repeat(35)}];
        if (entry.steering) { items.push({id: "steering", type: "userMessage", content: [{type: "text", text: entry.steering}]}); }
        for (const item of items) {
            normalizer.serverNotification("item/completed", {threadId: contract.threadId, turnId: entry.id, item});
        }
    }
    return {model, normalizer, projection: () => projectConversation(model.thread(contract.threadId), [], 1000, 0)};
}

for (const transition of contract.transitions) {
    test(`browser differential navigation: ${transition.turn} ${transition.direction}`, () => {
        const openings = turnOpenings(fixture().projection().sections);
        assert.equal(turnNavigationTarget(openings, transition.turn, true, transition.direction)?.turnId ?? null, transition.target);
        assert.equal(turnNavigationTarget(openings, transition.turn, false, -1)?.turnId, transition.turn,
            "Up from inside a turn first reaches its own opening");
    });
}

test("outline keeps real turn boundaries, duplicate prompts, fallback turns and loaded windows", () => {
    const {projection} = fixture();
    const openings = turnOpenings(projection().sections);
    assert.equal(openings.length, contract.turns.length, "steering does not create a turn");
    assert.equal(openings[1].payload.text, openings[2].payload.text);
    assert.notEqual(stableKey(openings[1].key), stableKey(openings[2].key), "duplicate text cannot alias destinations");
    assert.equal(openings[3].kind, "agentMessage", "a turn without a prompt has an existing item destination");
    assert.equal(turnNavigationTarget(openings, "deleted", true, 1), undefined);
    assert.deepEqual(turnOpenings([]), []);
    assert.equal(turnNavigationTarget([], "", false, -1), undefined);
});

test("chosen anchor survives streaming and lifetime changes through the existing viewport owner", () => {
    const {model, normalizer, projection} = fixture();
    const viewport = new ConversationViewportState();
    const lifetime = viewport.bind(contract.threadId, model.thread(contract.threadId));
    const selected = turnOpenings(projection().sections).at(-1);
    const anchor = {cardKey: stableKey(selected.key), pixelOffset: 140};
    viewport.updateScroll(contract.threadId, lifetime, 600, false, anchor);
    normalizer.serverNotification("item/completed", {threadId: contract.threadId, turnId: selected.turnId,
        item: {id: "stream", type: "agentMessage", phase: "final_answer", text: "New output"}});
    assert.equal(viewport.scroll(contract.threadId, lifetime).following, false);
    assert.deepEqual(viewport.scroll(contract.threadId, lifetime).anchor, anchor);
    assert.equal(turnOpenings(projection().sections).at(-1).turnId, selected.turnId);
    const nextLifetime = viewport.bind(contract.threadId, {});
    viewport.updateScroll(contract.threadId, lifetime, 999, true);
    assert.deepEqual(viewport.scroll(contract.threadId, nextLifetime), {scrollTop: 0, following: true},
        "an outgoing lifetime cannot navigate the replacement thread");
});

test("empty browser header offers disabled navigation alongside existing presentation controls", () => {
    const session = new BrowserFrontendSession("ws://bridge.test/", () => { throw new Error("offline fixture"); });
    const markup = renderToStaticMarkup(createElement(App, {session}));
    assert.match(markup, /class="conversation-view-controls"[\s\S]*class="turn-navigation"/u);
    assert.match(markup, /aria-label="Turn opening; previous turn when already there"[^>]*disabled/u);
    assert.match(markup, /aria-label="First turn; load earlier history if needed"[^>]*disabled/u);
    assert.match(markup, /aria-label="Next turn opening"[^>]*disabled/u);
    assert.match(markup, /class="conversation-latest"[^>]*disabled/u);
    assert.doesNotMatch(markup, /\d+ of \d+/u);
    session.dispose();
});
