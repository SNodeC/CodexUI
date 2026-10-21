import assert from "node:assert/strict";
import test from "node:test";
import {createElement} from "react";
import {renderToStaticMarkup} from "react-dom/server";
import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {Card} from "../dist/app/App.js";
import {projectConversation} from "../dist/index.js";
import {FakeSocket, appserver, requests, respond, reject, readyProvider, completeControllerHydration} from "./browser-harness.mjs";

const settle = async () => { for (let pass = 0; pass < 12; ++pass) { await Promise.resolve(); } };
const turns = [1, 2, 3].map(index => ({id: `t${index}`, status: "completed", items: [
    {id: `i${index}`, type: index === 3 ? "agentMessage" : "userMessage", text: `message ${index}`, content: [{type: "text", text: `prompt ${index}`}]},
]}));
async function fixture(overrides = {}) {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "remove-turns");
    const thread = {id: "cut", historyMode: "paginated", ephemeral: false, status: {type: "idle"}, ...overrides};
    respond(socket, requests(socket, "thread/list").at(-1), {data: [thread]});
    session.selectThread(thread.id);
    await completeControllerHydration(socket, {...thread, turns: [...turns].reverse()});
    const answered = new Set();
    const drain = async () => {
        for (let pass = 0; pass < 4; ++pass) {
            for (const request of requests(socket, "thread/items/list")) {
                if (!answered.has(request)) {
                    answered.add(request);
                    respond(socket, request, {data: [], nextCursor: null});
                }
            }
            await settle();
        }
    };
    await drain();
    return {session, socket, thread, drain, key: session.threadVisualKey(thread.id)};
}
const reverted = socket => socket.receive(appserver({method: "thread/reverted", params: {threadId: "cut"}}));

for (const boundary of [1, 2, 3]) {
    for (const notificationFirst of [false, true]) {
        test(`remove t${boundary} inclusively; notification ${notificationFirst ? "before" : "after"} result`, async () => {
            const {session, socket, key, drain} = await fixture();
            try {
                const thread = session.model.thread("cut");
                const prefix = thread.turns.get("t1");
                const target = thread.turns.get(`t${boundary}`);
                assert.equal(session.revertUnavailable("cut", target.id, key, target), "");
                session.removeTurns("cut", target.id, key, target);
                session.removeTurns("cut", target.id, key, target);
                const mutation = requests(socket, "thread/revert");
                assert.equal(mutation.length, 1);
                assert.deepEqual(mutation[0].payload.params, {threadId: "cut", beforeTurnId: target.id});
                assert.equal(await session.submitPrompt("racing prompt", [], {}, {}, key), false);
                if (notificationFirst) { reverted(socket); }
                respond(socket, mutation[0], {thread: {id: "cut", turns: []}, turnsBackwardsCursor: boundary === 1 ? null : "anchor", itemsBackwardsCursor: "items-anchor"});
                await settle();
                const pages = requests(socket, "thread/turns/list");
                if (boundary > 1) {
                    assert.equal(pages.at(-1).payload.params.cursor, "anchor", "use the supplied inclusive anchor cursor");
                }
                if (!notificationFirst) { reverted(socket); }
                await settle();
                assert.equal(requests(socket, "thread/turns/list").length, pages.length, "one reload in either ordering");
                if (boundary > 1) {
                    respond(socket, pages.at(-1), {data: turns.slice(0, boundary - 1).reverse(), nextCursor: null});
                    await drain();
                }
                assert.deepEqual(session.model.thread("cut").turnOrder, turns.slice(0, boundary - 1).map(turn => turn.id));
                if (boundary > 1) { assert.equal(session.model.thread("cut").turns.get("t1"), prefix, "surviving identity is retained"); }
                assert.equal(session.operationPending("thread.revert", "cut", key), false);
                assert.notEqual(session.model.thread("cut").raw.historyStale, true);
            } finally { session.dispose(); }
        });
    }
}

test("unsupported, busy, stale and observer targets cannot dispatch", async () => {
    for (const overrides of [{ephemeral: true}, {historyMode: "legacy"}, {status: {type: "active", activeFlags: []}}]) {
        const {session, socket, key} = await fixture(overrides);
        const target = session.model.thread("cut").turns.get("t2");
        session.removeTurns("cut", "t2", key, target);
        assert.equal(requests(socket, "thread/revert").length, 0);
        session.dispose();
    }
    const {session, socket, key} = await fixture();
    try {
        session.removeTurns("cut", "t2", key, {});
        session.removeTurns("cut", "t2", "retired-presentation", session.model.thread("cut").turns.get("t2"));
        socket.receive({kind: "bridge.controller", controllerConnectionId: "other"});
        session.removeTurns("cut", "t2", key, session.model.thread("cut").turns.get("t2"));
        assert.equal(requests(socket, "thread/revert").length, 0);
    } finally { session.dispose(); }
});

test("rejected removal preserves history and permits a deliberate retry", async () => {
    const {session, socket, key} = await fixture();
    try {
        const thread = session.model.thread("cut"), target = thread.turns.get("t2");
        session.removeTurns("cut", "t2", key, target);
        reject(socket, requests(socket, "thread/revert").at(-1), "server rejected removal");
        await settle();
        assert.deepEqual(thread.turnOrder, ["t1", "t2", "t3"]);
        assert.equal(session.revertUnavailable("cut", "t2", key, target), "");
        assert.match(session.getSnapshot().notice, /server rejected removal/);
        session.removeTurns("cut", "t2", key, target);
        assert.equal(requests(socket, "thread/revert").length, 2);
    } finally { session.dispose(); }
});

test("external revert fences a late page and preserves an earlier reading target", async () => {
    const {session, socket, key, drain} = await fixture();
    try {
        const thread = session.model.thread("cut");
        const prefix = thread.turns.get("t1");
        thread.historyNextCursor = "old-page"; thread.historyHasMore = true;
        session.loadMoreHistory("cut", key);
        const stale = requests(socket, "thread/turns/list").at(-1);
        reverted(socket);
        const replacement = requests(socket, "thread/turns/list").at(-1);
        assert.notEqual(replacement, stale);
        respond(socket, stale, {data: turns.slice().reverse(), nextCursor: "obsolete"});
        respond(socket, replacement, {data: [turns[0]], nextCursor: null});
        await drain();
        assert.deepEqual(thread.turnOrder, ["t1"]);
        assert.equal(thread.turns.get("t1"), prefix);
        assert.notEqual(thread.historyNextCursor, "obsolete");
    } finally { session.dispose(); }
});

test("menu is projected once for a turn; rootless turns require complete item history", async () => {
    const {session} = await fixture();
    try {
        const thread = session.model.thread("cut");
        const first = thread.turns.get("t1");
        first.itemOrder.push("steer");
        first.items.set("steer", {id: "steer", raw: {type: "userMessage", content: [{type: "text", text: "steer"}]}});
        let cards = projectConversation(thread, [], 100, 0).sections.flatMap(section => section.cards);
        assert.equal(cards.filter(card => card.turnId === "t1" && card.turnOpening).length, 1);
        assert.equal(cards.find(card => card.itemId === "steer").turnOpening, undefined);
        assert.equal(cards.find(card => card.itemId === "i3").turnOpening, true);
        thread.turns.get("t3").itemsComplete = false;
        cards = projectConversation(thread, [], 100, 0).sections.flatMap(section => section.cards);
        assert.equal(cards.find(card => card.itemId === "i3").turnOpening, undefined);
        first.itemsComplete = false;
        cards = projectConversation(thread, [], 100, 0).sections.flatMap(section => section.cards);
        assert.equal(cards.filter(card => card.turnId === "t1" && card.turnOpening).length, 0);
        first.itemsComplete = true;
        cards = projectConversation(thread, [], 100, 0).sections.flatMap(section => section.cards);
        const card = cards.find(card => card.itemId === "i1");
        const html = renderToStaticMarkup(createElement(Card, {card, active: false, collapsed: false, onToggle() {}, onTurnMenu() {}}));
        assert.equal((html.match(/aria-label="Turn actions"/g) ?? []).length, 1);
        assert.ok(html.indexOf("Turn actions") > html.indexOf(">You<"));
    } finally { session.dispose(); }
});

test("shared native/browser removal contract preserves overlapping pages, complete items and opener identity", async () => {
    const {readFileSync} = await import("node:fs");
    const {PresentationModel, ProtocolNormalizer} = await import("../dist/index.js");
    const fixture = JSON.parse(readFileSync(new URL("../../tests/fixtures/turn-removal.json", import.meta.url), "utf8"));
    for (const entry of fixture.cases) {
        const model = new PresentationModel();
        const normalizer = new ProtocolNormalizer(frame => model.applyEvent(frame));
        const threadId = fixture.thread.id;
        normalizer.serverNotification("thread/started", {thread: fixture.thread});
        for (const turn of fixture.thread.turns) {
            normalizer.operationResult("thread.items.list", "complete", {threadId, turnId: turn.id, sortDirection: "desc"}, {result: {data: [], nextCursor: null}});
        }
        const before = model.thread(threadId).turns.get("t1")?.items.get("answer-1");
        normalizer.operationResult("thread.revert", "remove", {threadId, beforeTurnId: entry.beforeTurnId}, {result: {
            thread: {id: threadId, turns: []}, turnsBackwardsCursor: entry.pageTurnIds.length ? "anchor" : null,
        }});
        const page = entry.pageTurnIds.map(id => {
            const turn = fixture.thread.turns.find(turn => turn.id === id);
            return {...turn, items: turn.items.slice(0, 1)};
        });
        normalizer.operationResult("thread.turns.list", "retained", {threadId, cursor: "anchor", sortDirection: "desc", itemsView: "summary"}, {result: {data: page, nextCursor: null}});
        const thread = model.thread(threadId);
        assert.deepEqual(thread.turnOrder, entry.retainedTurnIds);
        const snapshot = projectConversation(thread, [], 80, 0);
        assert.deepEqual(snapshot.sections.map(section => section.turnId), entry.retainedTurnIds);
        for (const section of snapshot.sections) {
            assert.equal(section.cards.length, fixture.thread.turns.find(turn => turn.id === section.turnId).items.length);
            assert.equal(section.cards.filter(card => card.turnOpening).length, 1);
        }
        if (entry.retainedTurnIds.length) { assert.equal(thread.turns.get("t1").items.get("answer-1"), before); }
    }
});

test("shared external refresh locates the old prefix across new pages without losing intervening live turns", async () => {
    const {readFileSync} = await import("node:fs");
    const {PresentationModel, ProtocolNormalizer} = await import("../dist/index.js");
    const fixture = JSON.parse(readFileSync(new URL("../../tests/fixtures/turn-removal.json", import.meta.url), "utf8"));
    const model = new PresentationModel();
    const normalizer = new ProtocolNormalizer(frame => model.applyEvent(frame));
    const threadId = fixture.thread.id;
    normalizer.serverNotification("thread/started", {thread: fixture.thread});
    normalizer.serverNotification("thread/reverted", {threadId});
    normalizer.serverNotification("turn/started", {threadId, turn: fixture.external.liveTurn});
    normalizer.operationResult("thread.turns.list", "first", {threadId, sortDirection: "desc"}, {result: fixture.external.pages[0]});
    assert.equal(model.thread(threadId).raw.historyStale, true);
    normalizer.operationResult("thread.turns.list", "second", {threadId, cursor: "older", sortDirection: "desc"}, {result: fixture.external.pages[1]});
    assert.deepEqual(model.thread(threadId).turnOrder, fixture.external.retainedTurnIds);
    assert.equal(model.thread(threadId).raw.historyStale, undefined);
    assert.equal(model.thread(threadId).raw.historyReload, undefined);
});

test("external session refresh follows only the pages needed to identify the surviving cached prefix", async () => {
    const {session, socket, drain} = await fixture();
    try {
        const initialPages = requests(socket, "thread/turns/list").length;
        reverted(socket);
        const first = requests(socket, "thread/turns/list").at(-1);
        socket.receive(appserver({method: "turn/started", params: {threadId: "cut", turn: {id: "new-live", status: "inProgress", items: [{id: "live", type: "agentMessage", text: "Live"}]}}}));
        respond(socket, first, {data: [{id: "new-history", status: "completed", items: [{id: "new", type: "agentMessage", text: "History"}]}], nextCursor: "older"});
        await settle();
        const older = requests(socket, "thread/turns/list").at(-1);
        assert.equal(older.payload.params.cursor, "older");
        respond(socket, older, {data: [turns[0]], nextCursor: "older-than-cache"});
        await drain();
        assert.deepEqual(session.model.thread("cut").turnOrder, ["t1", "new-history", "new-live"]);
        assert.equal(requests(socket, "thread/turns/list").length, initialPages + 2);
        assert.equal(session.model.thread("cut").raw.historyReload, undefined);
    } finally { session.dispose(); }
});

test("uncertain transport failure preserves history and requires hydration without resending removal", async () => {
    const {rejectWithCode} = await import("./browser-harness.mjs");
    const {session, socket, key, drain} = await fixture();
    try {
        const target = session.model.thread("cut").turns.get("t2");
        session.removeTurns("cut", "t2", key, target);
        rejectWithCode(socket, requests(socket, "thread/revert").at(-1), -32020, "transport outcome unknown");
        await settle();
        assert.deepEqual(session.model.thread("cut").turnOrder, ["t1", "t2", "t3"]);
        assert.equal(session.model.thread("cut").raw.historyStale, true);
        assert.equal(session.canSubmit(key), false);
        session.reloadThread("cut", key);
        await completeControllerHydration(socket, {id: "cut", historyMode: "paginated", status: {type: "idle"}, turns: [turns[0]]});
        await drain();
        assert.deepEqual(session.model.thread("cut").turnOrder, ["t1"]);
        assert.equal(requests(socket, "thread/revert").length, 1);
    } finally { session.dispose(); }
});

test("unloaded removal resumes metadata and revalidates before one destructive request", async () => {
    const {session, socket, key} = await fixture({status: {type: "notLoaded"}});
    try {
        const target = session.model.thread("cut").turns.get("t2");
        session.removeTurns("cut", "t2", key, target);
        assert.equal(requests(socket, "thread/revert").length, 0);
        const resume = requests(socket, "thread/resume").at(-1);
        assert.deepEqual(resume.payload.params, {threadId: "cut", excludeTurns: true});
        respond(socket, resume, {thread: {id: "cut", historyMode: "paginated", status: {type: "idle"}, turns: []}});
        await settle();
        assert.equal(requests(socket, "thread/revert").length, 1);
        reject(socket, requests(socket, "thread/revert").at(-1), "keep history");
        await settle();
        assert.deepEqual(session.model.thread("cut").turnOrder, ["t1", "t2", "t3"]);
    } finally { session.dispose(); }
});

test("removal retires only discarded card interaction state", async () => {
    const {stableKey} = await import("../dist/index.js");
    const {session, socket, key} = await fixture();
    try {
        const retained = stableKey({kind: "item", threadId: "cut", turnId: "t1", itemId: "i1"});
        const removed = stableKey({kind: "item", threadId: "cut", turnId: "t3", itemId: "i3"});
        session.viewportState.setCardCollapsed("cut", key, retained, true);
        session.viewportState.setCardCollapsed("cut", key, removed, true);
        const codeState = session.viewportState.codeScroll("cut", key, `${removed}:output`, true);
        codeState.scrollTop = 42; codeState.following = false;
        session.removeTurns("cut", "t2", key, session.model.thread("cut").turns.get("t2"));
        respond(socket, requests(socket, "thread/revert").at(-1), {thread: {id: "cut", turns: []}, turnsBackwardsCursor: null});
        reverted(socket); await settle();
        assert.equal(session.viewportState.cardCollapsed("cut", key, retained, false), true);
        assert.equal(session.viewportState.cardCollapsed("cut", key, removed, false), false);
        assert.notEqual(session.viewportState.codeScroll("cut", key, `${removed}:output`, true), codeState);
    } finally { session.dispose(); }
});


test("external removal preserves queued input and fences late prompt acknowledgement", async () => {
    const {session, socket, key, drain} = await fixture();
    try {
        assert.equal(await session.submitPrompt("keep authored prompt", [], {}, {}, key), true);
        const prompt = requests(socket, "turn/start").at(-1);
        const target = session.model.thread("cut").turns.get("t2");
        session.removeTurns("cut", "t2", key, target);
        assert.equal(requests(socket, "thread/revert").length, 0);
        reverted(socket);
        respond(socket, prompt, {turn: {id: "discarded", status: "completed", items: []}});
        await settle();
        const page = requests(socket, "thread/turns/list").at(-1);
        respond(socket, page, {data: [turns[0]], nextCursor: null});
        await drain();
        assert.deepEqual(session.model.thread("cut").turnOrder, ["t1"]);
        const preserved = session.prompts.submissions("cut").find(prompt => prompt.prompt === "keep authored prompt");
        assert.equal(preserved?.state, "failed");
        assert.equal(requests(socket, "turn/start").length, 1);
    } finally { session.dispose(); }
});
