import assert from "node:assert/strict";
import test from "node:test";

import {
    PromptCoordinator, cardKeys, findCard, indexAuthoritativeItems, projectConversation,
    promptWithFileLinks, stableKey, terminalOutputHasVisibleText, trimTrailingEmptyLines,
} from "../dist/index.js";

function item(id, raw) { return {id, raw, domains: new Map()}; }
function baseThread(id) {
    const thread = {
        id, title: "", preview: "", cwd: "", status: "", commandCwds: [], changedPaths: [],
        turnOrder: ["turn-1"], turns: new Map(), raw: {}, domains: new Map(), latestSettingsUpdate: {},
        settingsRevision: 0, agentOrder: [], agents: new Map(), childThreadOrder: [], archived: false,
    };
    thread.turns.set("turn-1", {
        id: "turn-1", status: "completed", itemOrder: ["user-old", "answer-old"], items: new Map([
            ["user-old", item("user-old", {type: "userMessage", content: [{type: "text", text: "old prompt"}]})],
            ["answer-old", item("answer-old", {type: "agentMessage", phase: "final_answer", text: "old answer"})],
        ]), plan: {}, raw: {}, domains: new Map(),
    });
    return thread;
}
function addTurn(thread, id, status = "inProgress") {
    thread.turnOrder.push(id);
    thread.turns.set(id, {id, status, itemOrder: [], items: new Map(), plan: {}, raw: {}, domains: new Map()});
}
function append(thread, turnId, id, raw) {
    const turn = thread.turns.get(turnId); turn.itemOrder.push(id); turn.items.set(id, item(id, raw));
}
function keys(snapshot) { return cardKeys(snapshot).map(stableKey); }

test("C++ canonical grouping, payload projection, and history limit", () => {
    const thread = baseThread("thread-a");
    addTurn(thread, "turn-2");
    append(thread, "turn-2", "command", {type: "commandExecution", command: "true", status: "completed", aggregatedOutput: " \n\t\x1b[0m"});
    append(thread, "turn-2", "files", {type: "fileChange", status: "completed", changes: [
        {path: "src/card.cpp", kind: "update", diff: "--- a\n+++ b\n-old\n+new\n++++literal\n+extra\n"},
    ]});
    append(thread, "turn-2", "image", {type: "imageGeneration", status: "completed", savedPath: "/tmp/generated.png", revised_prompt: "proposal", result: "A".repeat(10000)});
    const snapshot = projectConversation(thread, [], 80, 10);
    assert.equal(snapshot.sections.length, 2);
    assert.equal(snapshot.sections[0].cards.length, 2);
    assert.equal(snapshot.sections[1].cards.length, 3);
    assert.equal(snapshot.sections[1].cards[0].payload.output, "");
    assert.deepEqual(snapshot.sections[1].cards[1].payload.changes[0], {path: "src/card.cpp", kind: "update", additions: 3, deletions: 1});
    assert.deepEqual(snapshot.sections[1].cards[2].payload, {path: "/tmp/generated.png", status: "completed", revisedPrompt: "proposal"});
    assert.notEqual(stableKey(snapshot.sections[0].cards[0].key), stableKey(snapshot.sections[0].cards[1].key));
    const limited = projectConversation(thread, [], 1, 10);
    assert.equal(limited.hasMore, true);
    assert.equal(limited.hiddenAuthoritativeItemCount, 4);
    assert.equal(keys(limited).length, 1);
});

test("C++ prompt queue isolation and callback-only acknowledgement", () => {
    const first = baseThread("thread-a");
    const second = baseThread("thread-b");
    const prompts = new PromptCoordinator();
    const firstId = prompts.admit(first.id, "same", [], {}, first, undefined, 100);
    const secondId = prompts.admit(second.id, "other", [], {}, second, undefined, 101);
    assert.equal(prompts.beginNext(first.id).id, firstId);
    assert.equal(prompts.beginNext(first.id), undefined);
    assert.equal(prompts.beginNext(second.id).id, secondId);
    addTurn(first, "turn-2");
    append(first, "turn-2", "user-new", {type: "userMessage", content: [{type: "text", text: "same"}]});
    prompts.reconcile(first.id, first, 199);
    assert.equal(prompts.submission(first.id, firstId).state, "inFlight");
    assert.equal(prompts.submission(first.id, firstId).materializedItem, undefined);
    assert.equal(prompts.acknowledge(first.id, firstId, "turn-2", 200), true);
    prompts.reconcile(first.id, first, 200);
    assert.equal(prompts.submission(first.id, firstId).materializedItem.itemId, "user-new");
    const transitioning = projectConversation(first, prompts.submissions(first.id), 80, 699);
    const localKey = {kind: "prompt", submissionId: firstId};
    assert.equal(findCard(transitioning, localKey).kind, "localPrompt");
    const materialized = projectConversation(first, prompts.submissions(first.id), 80, 700);
    assert.equal(findCard(materialized, localKey).kind, "userMessage");
    const index = indexAuthoritativeItems(first.id, first);
    prompts.reconcile(first.id, index, 700);
    const compacted = projectConversation(index, prompts.submissions(first.id), 80, 701, first);
    assert.equal(prompts.submission(first.id, firstId), undefined);
    assert.equal(findCard(compacted, localKey).kind, "userMessage");
});

test("C++ first response order remains at the local prompt admission boundary", () => {
    const thread = baseThread("thread-reasoning-first");
    thread.turnOrder = [];
    thread.turns.clear();
    const prompts = new PromptCoordinator();
    const promptId = prompts.admit(thread.id, "new prompt", [], {}, thread, undefined, 600);
    const dispatch = prompts.beginNext(thread.id);
    addTurn(thread, "turn-new");
    append(thread, "turn-new", "reasoning", {type: "reasoning", summary: []});
    prompts.reconcile(thread.id, thread, 601);
    const promptKey = stableKey({kind: "prompt", submissionId: promptId});
    const reasoningKey = stableKey({kind: "item", threadId: thread.id, turnId: "turn-new", itemId: "reasoning"});
    assert.deepEqual(keys(projectConversation(thread, prompts.submissions(thread.id), 80, 601)), [promptKey, reasoningKey]);

    append(thread, "turn-new", "user-new", {
        type: "userMessage", clientId: dispatch.clientUserMessageId, content: [{type: "text", text: "new prompt"}],
    });
    prompts.reconcile(thread.id, thread, 602);
    assert.deepEqual(keys(projectConversation(thread, prompts.submissions(thread.id), 80, 602)), [promptKey, reasoningKey]);
    assert.equal(prompts.acknowledge(thread.id, promptId, "turn-new", 700), true);
    prompts.reconcile(thread.id, thread, 700);
    assert.deepEqual(keys(projectConversation(thread, prompts.submissions(thread.id), 80, 700)), [promptKey, reasoningKey]);
    const index = indexAuthoritativeItems(thread.id, thread);
    prompts.reconcile(thread.id, index, 1200);
    const blue = projectConversation(index, prompts.submissions(thread.id), 80, 1200, thread);
    assert.deepEqual(keys(blue), [promptKey, reasoningKey]);
    assert.equal(findCard(blue, {kind: "prompt", submissionId: promptId}).kind, "userMessage");
});

test("C++ duplicate prompts bind in admission order and share one turn section", () => {
    const thread = baseThread("duplicates");
    const prompts = new PromptCoordinator();
    const first = prompts.admit(thread.id, "repeat", [], {}, thread, undefined, 1000);
    const second = prompts.admit(thread.id, "repeat", [], {}, thread, undefined, 1001);
    prompts.beginNext(thread.id); prompts.acknowledge(thread.id, first, "turn-2", 1010);
    prompts.beginNext(thread.id, "turn-2"); prompts.acknowledge(thread.id, second, "turn-2", 1020);
    addTurn(thread, "turn-2");
    append(thread, "turn-2", "repeat-1", {type: "userMessage", content: [{type: "text", text: "repeat"}]});
    append(thread, "turn-2", "repeat-2", {type: "userMessage", content: [{type: "text", text: "repeat"}]});
    prompts.reconcile(thread.id, thread, 1021);
    assert.equal(prompts.submission(thread.id, first).materializedItem.itemId, "repeat-1");
    assert.equal(prompts.submission(thread.id, second).materializedItem.itemId, "repeat-2");
    const snapshot = projectConversation(thread, prompts.submissions(thread.id), 80, 1021);
    const visible = keys(snapshot);
    assert.ok(visible.indexOf(`prompt:${first}`) < visible.indexOf(`prompt:${second}`));
    assert.equal(snapshot.sections[1].turnId, "turn-2");
    assert.equal(snapshot.sections[1].cards.length, 2);
});

test("C++ terminal text and canonical attachment links", () => {
    assert.equal(terminalOutputHasVisibleText(""), false);
    assert.equal(terminalOutputHasVisibleText(" \n\t"), false);
    assert.equal(terminalOutputHasVisibleText("\x1b[0m\x1b]0;title\x07"), false);
    assert.equal(terminalOutputHasVisibleText("done\n"), true);
    assert.equal(trimTrailingEmptyLines("first\nsecond\n\n \t\r\n"), "first\nsecond");
    assert.equal(trimTrailingEmptyLines("  meaningful spacing  "), "  meaningful spacing  ");
    assert.equal(promptWithFileLinks("Review this", [
        {path: "/tmp/review notes [final] (2).pdf", name: "review notes [final] (2).pdf", mimeType: "application/pdf", size: 10},
        {path: "/tmp/image.png", name: "image.png", mimeType: "image/png", size: 10},
    ]), "Review this\n\nAttached files:\n- [review notes \\[final\\] (2).pdf](file:///tmp/review%20notes%20%5Bfinal%5D%20%282%29.pdf)");
});
