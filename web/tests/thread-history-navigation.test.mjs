import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import {createElement} from "react";
import {renderToStaticMarkup} from "react-dom/server";
import {App} from "../dist/app/App.js";
import {BrowserFrontendSession, ConversationViewportState, PresentationModel, ProtocolNormalizer,
    projectConversation, stableKey, turnOpenings, checkpointDestinations, checkpointNavigationTarget} from "../dist/index.js";

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

function turnNavigationTarget(openings, turnId, atOpening, direction) {
    const sections = openings.map(card => ({turnId: card.turnId, cards: [card]}));
    const card = openings.find(card => card.turnId === turnId);
    return card ? checkpointNavigationTarget(sections, "turns", {cardKey: stableKey(card.key), pixelOffset: atOpening ? 0 : -20}, direction) : undefined;
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
    assert.match(markup, />All checkpoints<\/button>/u);
    assert.match(markup, /class="thread-heading-context"[\s\S]*class="turn-navigation"[\s\S]*class="thread-workspace-copy"[\s\S]*class="thread-workspace"/u);
    assert.match(markup, /aria-label="Previous checkpoint;[\s\S]*class="turn-outline[\s\S]*aria-label="Next checkpoint"[\s\S]*aria-label="First turn;[\s\S]*class="conversation-latest"/u);
    const presentation = markup.match(/<div class="conversation-view-controls"[^>]*>([\s\S]*?)<\/div>/u)?.[1];
    assert.ok(presentation);
    assert.doesNotMatch(presentation, /class="turn-navigation"/u);
    assert.match(markup, /aria-label="Previous checkpoint; current checkpoint opening when inside it"[^>]*disabled/u);
    assert.match(markup, /aria-label="First turn; load earlier history if needed"[^>]*disabled/u);
    assert.match(markup, /aria-label="Next checkpoint"[^>]*disabled/u);
    assert.match(markup, /class="conversation-latest"[^>]*disabled/u);
    assert.doesNotMatch(markup, /\d+ of \d+/u);
    session.dispose();
});

const checkpointContract = JSON.parse(readFileSync(new URL("../../tests/fixtures/checkpoint-navigation.json", import.meta.url), "utf8"));
export function checkpointSections() {
    return checkpointContract.turns.map(turn => {
        const cards = turn.cards.map(item => ({threadId: checkpointContract.threadId, turnId: turn.id, itemId: item.id,
            kind: item.kind, key: {kind: "item", threadId: checkpointContract.threadId, turnId: turn.id, itemId: item.id},
            payload: {text: item.text, phase: item.phase, questionRequest: item.questionRequest, answers: item.answers, questionStatus: item.questionStatus}}));
        return {key: turn.id, turnId: turn.id, rootCardKey: cards[0].kind === "userMessage" ? cards[0].key : undefined, cards};
    });
}
const checkpointId = card => card ? `${card.turnId}/${card.itemId}` : null;
for (const [scope, expected] of Object.entries(checkpointContract.expected)) {
    test(`checkpoint differential outline: ${scope}`, () => {
        const destinations = checkpointDestinations(checkpointSections(), scope);
        assert.deepEqual(destinations.map(({card}) => checkpointId(card)), expected);
        assert.equal(new Set(destinations.map(({card}) => stableKey(card.key))).size, destinations.length);
        assert(destinations.every(({card, action}) => action === (scope === "turns" ? "Opening" : checkpointContract.actions[checkpointId(card)])));
        assert(destinations.every(({card, action, preview, label}) => label === `${String(checkpointContract.turns.findIndex(turn => turn.id === card.turnId) + 1).padStart(2, "0")} | ${action} ${preview}`));
        assert(destinations.every(({card, label}) => !card.payload.questionRequest
            || label.includes(card.payload.questionRequest.questions[0].question)), "question text identifies pending and answered checkpoints");
    });
}
for (const query of checkpointContract.queries) {
    test(`checkpoint differential jump: ${query.scope} ${query.anchor} ${query.offset} ${query.direction}`, () => {
        const sections = checkpointSections(), card = sections.flatMap(section => section.cards).find(card => checkpointId(card) === query.anchor);
        assert.equal(checkpointId(checkpointNavigationTarget(sections, query.scope,
            {cardKey: stableKey(card.key), pixelOffset: query.offset}, query.direction)), query.target);
    });
}
test("checkpoint filters respect visible data and pending steering semantics", () => {
    const sections = checkpointSections();
    const steering = sections[0].cards.find(card => card.itemId === "steer");
    const queued = {...steering, kind: "localPrompt", itemId: "local", key: {...steering.key, itemId: "local"}, payload: {prompt: "Pending steering", state: "queued"}};
    sections[0].cards.push(queued);
    assert.equal(checkpointDestinations(sections, "steering").at(1).card, queued);
    queued.payload.state = "failed";
    assert.equal(checkpointDestinations(sections, "steering").length, 2);
    sections[0].cards[1].payload.questionRequest = {questions: [{question: "Extra question"}]};
    assert.equal(checkpointDestinations(sections, "questions").length, 3, "pending and answered questions remain checkpoints");
    assert.equal(checkpointDestinations(sections, "updates").length, 2, "async questions are not Codex updates");
    for (const section of sections) { section.cards = section.cards.filter(card => card.kind !== "agentMessage" || card.payload.questionRequest || ["final_answer", "partial_answer"].includes(card.payload.phase)); }
    assert.deepEqual(checkpointDestinations(sections, "updates"), []);
    assert.equal(checkpointNavigationTarget(sections, "updates", undefined, 1), undefined);
    assert.deepEqual(checkpointDestinations([]), []);
});

for (const [scope, expected] of Object.entries(checkpointContract.hiddenUpdates)) {
    test(`checkpoint differential hidden updates: ${scope}`, () => {
        const sections = checkpointSections().map(section => ({...section,
            cards: section.cards.filter(card => card.kind !== "agentMessage" || card.payload.questionRequest || ["final_answer", "partial_answer"].includes(card.payload.phase))}));
        assert.deepEqual(checkpointDestinations(sections, scope).map(({card}) => checkpointId(card)), expected);
    });
}

test("unassigned prompts do not shift checkpoint turn context", () => {
    const sections = checkpointSections();
    sections.unshift({key: "unassigned", turnId: "", cards: [{...sections[0].cards[0], turnId: ""}]});
    assert(checkpointDestinations(sections)[0].label.startsWith("01 | Opening "));
});


test("checkpoint numbering pads single digits and preserves larger turn numbers", () => {
    const opening = checkpointSections()[0].cards[0];
    const sections = Array.from({length: 100}, (_, index) => ({key: `turn-${index}`, turnId: `turn-${index}`,
        cards: [{...opening, turnId: `turn-${index}`, key: {...opening.key, turnId: `turn-${index}`}}]}));
    const destinations = checkpointDestinations(sections);
    for (const [position, prefix] of [[0, "01 | Opening "], [22, "23 | Opening "], [99, "100 | Opening "]]) {
        assert(destinations[position].label.startsWith(prefix));
    }
});
