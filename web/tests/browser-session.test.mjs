import assert from "node:assert/strict";
import test from "node:test";

import {BrowserFrontendSession, DraftThreadId, suggestForkName} from "../dist/app/BrowserFrontendSession.js";
import {cardKeys, changeSettingDraft, result, settingDraftFor, settingPromptOptions, stableKey,
    turnSettingCatalog} from "../dist/index.js";

class FakeSocket {
    protocol = "codex";
    readyState = 0;
    bufferedAmount = 0;
    binaryType = "arraybuffer";
    onopen = null;
    onmessage = null;
    onerror = null;
    onclose = null;
    sent = [];
    send(data) { this.sent.push(JSON.parse(data)); }
    close(_code, reason) { this.readyState = 3; this.onclose?.({reason}); }
    open() { this.readyState = 1; this.onopen?.(); }
    receive(message) { this.onmessage?.({data: JSON.stringify(message)}); }
    receiveText(data) { this.onmessage?.({data}); }
}

class DelayedCloseSocket extends FakeSocket {
    closeReason = "";
    close(_code, reason) { this.readyState = 2; this.closeReason = reason; }
    finishClose() { this.readyState = 3; this.onclose?.({reason: this.closeReason}); }
}

function appserver(payload) { return {kind: "appserver", payload}; }
function requests(socket, method) {
    return socket.sent.filter(message => message.kind === "appserver" && message.payload.method === method);
}
function respond(socket, request, result) {
    socket.receive(appserver({jsonrpc: "2.0", id: request.payload.id, result}));
}
function reject(socket, request, message) {
    socket.receive(appserver({jsonrpc: "2.0", id: request.payload.id, error: {code: -32000, message}}));
}
function rejectWithCode(socket, request, code, message) {
    socket.receive(appserver({jsonrpc: "2.0", id: request.payload.id, error: {code, message}}));
}
async function readyProvider(socket, connectionId, role = "controller", providerGeneration = 1) {
    socket.receive({kind: "bridge.connection", event: "opened", connectionId, role});
    socket.receive({kind: "bridge.provider", state: "ready", providerGeneration});
    await Promise.resolve();
}
async function completeControllerHydration(socket, thread, nextCursor = null) {
    const resume = requests(socket, "thread/resume").at(-1);
    assert.ok(resume, "controller hydration starts with metadata-only resume");
    assert.deepEqual(resume.payload.params, {threadId: thread.id, excludeTurns: true});
    const {turns = [], ...metadata} = thread;
    respond(socket, resume, {thread: metadata});
    await Promise.resolve(); await Promise.resolve();
    const page = requests(socket, "thread/turns/list").at(-1);
    assert.ok(page, "metadata resume is followed by one bounded turn page");
    assert.deepEqual(page.payload.params, {
        threadId: thread.id, limit: 80, sortDirection: "desc", itemsView: "summary",
    });
    respond(socket, page, {data: turns, nextCursor});
    await Promise.resolve(); await Promise.resolve();
    return {resume, page};
}
const waitForPublish = () => new Promise(resolve => setTimeout(resolve, 25));

test("browser session defaults to the bridge's canonical WebSocket endpoint", () => {
    assert.equal(BrowserFrontendSession.defaultBridgeUrl(), "ws://127.0.0.1:8080/codex");
    globalThis.window = {
        localStorage: {getItem: () => null},
        location: {protocol: "https:", host: "codex.example:8443"},
    };
    try {
        assert.equal(BrowserFrontendSession.defaultBridgeUrl(), "wss://codex.example:8443/codex");
        globalThis.window.localStorage.getItem = () => "wss://configured.example/bridge";
        assert.equal(BrowserFrontendSession.defaultBridgeUrl(), "wss://configured.example/bridge");
    } finally {
        delete globalThis.window;
    }
});

test("selected-thread loading follows the latest hydration identity", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "thread-loading");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [
        {id: "thread-a", status: {type: "notLoaded"}},
        {id: "thread-b", status: {type: "notLoaded"}},
    ]});

    session.selectThread("thread-a");
    const resumeA = requests(socket, "thread/resume").at(-1);
    assert.equal(session.getSnapshot().selectedThreadLoading, true);
    session.selectThread("thread-b");
    const resumeB = requests(socket, "thread/resume").at(-1);
    assert.equal(session.getSnapshot().selectedThreadLoading, true);

    respond(socket, resumeA, {thread: {id: "thread-a"}});
    await Promise.resolve(); await Promise.resolve();
    respond(socket, requests(socket, "thread/turns/list").at(-1), {data: [], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(session.getSnapshot().selectedThreadId, "thread-b");
    assert.equal(session.getSnapshot().selectedThreadLoading, true,
        "a superseded hydration cannot complete the visible loading state");

    respond(socket, resumeB, {thread: {id: "thread-b"}});
    await Promise.resolve(); await Promise.resolve();
    respond(socket, requests(socket, "thread/turns/list").at(-1), {data: [], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(session.getSnapshot().selectedThreadLoading, false);
    session.dispose();
});

test("repeated reloads share one metadata resume and turn page until hydration settles", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "reload-single-flight");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [
        {id: "reload-thread", status: {type: "idle"}},
    ]});
    session.selectThread("reload-thread");
    const initialResume = requests(socket, "thread/resume").at(-1);

    session.reloadThread("reload-thread"); session.reloadThread("reload-thread");
    assert.equal(requests(socket, "thread/resume").length, 1);
    respond(socket, initialResume, {thread: {id: "reload-thread", status: {type: "idle"}}});
    await Promise.resolve(); await Promise.resolve();
    respond(socket, requests(socket, "thread/turns/list").at(-1), {data: [], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();

    session.reloadThread("reload-thread"); session.reloadThread("reload-thread");
    assert.equal(requests(socket, "thread/resume").length, 2);
    respond(socket, requests(socket, "thread/resume").at(-1), {
        thread: {id: "reload-thread", status: {type: "idle"}},
    });
    await Promise.resolve(); await Promise.resolve();
    respond(socket, requests(socket, "thread/turns/list").at(-1), {data: [], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();
    session.dispose();
});

test("browser history hydration separates metadata, turn summaries, and bounded item pages", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "paged-history");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [
        {id: "paged", status: {type: "idle"}},
    ]});
    session.selectThread("paged");
    const metadata = requests(socket, "thread/resume").at(-1);
    assert.deepEqual(metadata.payload.params, {threadId: "paged", excludeTurns: true});
    respond(socket, metadata, {thread: {id: "paged", status: {type: "idle"}}});
    await Promise.resolve(); await Promise.resolve();

    const turns = requests(socket, "thread/turns/list").at(-1);
    assert.deepEqual(turns.payload.params, {
        threadId: "paged", limit: 80, sortDirection: "desc", itemsView: "summary",
    });
    respond(socket, turns, {data: [
        {id: "newer", items: [
            {id: "newer-user", type: "userMessage", content: [{type: "inputText", text: "prompt"}]},
            {id: "newer-agent", type: "agentMessage", text: "answer"},
        ]},
        {id: "older", items: [
            {id: "older-user", type: "userMessage", content: [{type: "inputText", text: "prompt"}]},
            {id: "older-agent", type: "agentMessage", text: "answer"},
        ]},
    ], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();

    const itemPages = requests(socket, "thread/items/list");
    assert.equal(itemPages.length, 2);
    for (const page of itemPages) {
        assert.equal(page.payload.params.limit, 80);
        assert.equal(page.payload.params.sortDirection, "desc");
        const turnId = page.payload.params.turnId;
        respond(socket, page, {data: [
            {turnId, item: {id: `${turnId}-agent`, type: "agentMessage", text: "answer"}},
            {turnId, item: {id: `${turnId}-command`, type: "commandExecution", command: "pwd"}},
        ], nextCursor: turnId === "older" ? "older-items" : null});
    }
    await Promise.resolve(); await Promise.resolve();

    const continuation = requests(socket, "thread/items/list").at(-1);
    assert.deepEqual(continuation.payload.params, {
        threadId: "paged", turnId: "older", limit: 80, sortDirection: "desc", cursor: "older-items",
    });
    respond(socket, continuation, {data: [
        {turnId: "older", item: {id: "older-earliest", type: "agentMessage", text: "earliest"}},
    ], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();

    const thread = session.model.thread("paged");
    assert.deepEqual(thread?.turnOrder, ["older", "newer"]);
    assert.deepEqual(thread?.turns.get("older")?.itemOrder,
        ["older-earliest", "older-user", "older-command", "older-agent"]);
    assert.deepEqual(thread?.turns.get("newer")?.itemOrder,
        ["newer-user", "newer-command", "newer-agent"]);
    session.dispose();
});

test("observer hydration reads directly and the item-page ceiling is global across threads", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "bounded-item-pages", "observer");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [
        {id: "bounded-a", status: {type: "idle"}},
        {id: "bounded-b", status: {type: "idle"}},
    ]});
    session.selectThread("bounded-a");
    assert.equal(session.getSnapshot().selectedThreadLoading, true);
    const pageA = requests(socket, "thread/turns/list").at(-1);
    assert.equal(pageA.payload.params.threadId, "bounded-a");
    assert.equal(requests(socket, "thread/resume").length, 0,
        "observer selection never attempts a controller-only resume");
    respond(socket, pageA, {
        data: Array.from({length: 6}, (_, index) => ({id: `a-turn-${index}`, items: []})),
        nextCursor: null,
    });
    await Promise.resolve(); await Promise.resolve();
    assert.equal(session.getSnapshot().selectedThreadLoading, false);

    session.selectThread("bounded-b");
    const pageB = requests(socket, "thread/turns/list").at(-1);
    assert.equal(pageB.payload.params.threadId, "bounded-b");
    respond(socket, pageB, {
        data: Array.from({length: 6}, (_, index) => ({id: `b-turn-${index}`, items: []})),
        nextCursor: null,
    });
    await Promise.resolve(); await Promise.resolve();
    const firstWave = requests(socket, "thread/items/list");
    assert.equal(firstWave.length, 8,
        "the item scheduler has an exact eight-request concurrency ceiling");

    respond(socket, firstWave[0], {data: [], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(requests(socket, "thread/items/list").length, 9,
        "one completion admits exactly one queued turn");
    session.dispose();
});

test("turn history advances one explicit page at a time and retries the same cursor after failure", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "turn-pagination");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [
        {id: "paged-turns", status: {type: "idle"}},
    ]});
    session.selectThread("paged-turns");
    await completeControllerHydration(socket, {
        id: "paged-turns", status: {type: "idle"}, turns: [{id: "newest", items: []}],
    }, "turn-cursor-1");
    assert.equal(requests(socket, "thread/turns/list").length, 1,
        "the first page does not recursively drain retained history");
    assert.equal(session.conversation().hasMore, true);

    session.loadMoreHistory("paged-turns");
    session.loadMoreHistory("paged-turns");
    const failedPage = requests(socket, "thread/turns/list").at(-1);
    assert.equal(session.historyPagePending("paged-turns"), true);
    assert.equal(requests(socket, "thread/turns/list").length, 2,
        "a continuation page is single-flight");
    assert.equal(failedPage.payload.params.cursor, "turn-cursor-1");
    reject(socket, failedPage, "temporary paging failure");
    await Promise.resolve(); await Promise.resolve();
    assert.equal(session.historyPagePending("paged-turns"), false);

    session.loadMoreHistory("paged-turns");
    const retry = requests(socket, "thread/turns/list").at(-1);
    assert.equal(requests(socket, "thread/turns/list").length, 3);
    assert.equal(retry.payload.params.cursor, "turn-cursor-1");
    respond(socket, retry, {data: [{id: "older", items: []}], nextCursor: "turn-cursor-2"});
    await Promise.resolve(); await Promise.resolve();
    assert.deepEqual(session.model.thread("paged-turns")?.turnOrder, ["older", "newest"]);
    assert.equal(session.model.thread("paged-turns")?.historyNextCursor, "turn-cursor-2");
    session.dispose();
});

test("active retained agent children hydrate read-only without another metadata resume", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "agent-history");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [
        {id: "parent", status: {type: "idle"}},
    ]});
    session.selectThread("parent");
    await completeControllerHydration(socket, {id: "parent", status: {type: "idle"}, turns: [{
        id: "parent-turn", items: [{
            id: "spawn", type: "subAgentActivity", status: "started", agentThreadId: "child",
        }],
    }]});
    const childPage = requests(socket, "thread/turns/list").find(
        request => request.payload.params.threadId === "child");
    assert.ok(childPage, "an active retained child is admitted to bounded historical hydration");
    assert.equal(requests(socket, "thread/resume").length, 1,
        "historical child hydration does not mutate provider subscription state");
    respond(socket, childPage, {data: [], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();
    session.dispose();
});

test("user thread operations are single-flight and report failures", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "operation-feedback");
    for (const id of ["thread-1", "provider-code", "transport-code"])
        socket.receive(appserver({jsonrpc: "2.0", method: "thread/started", params: {thread: {id}}}));

    session.renameThread("thread-1", "Renamed");
    session.renameThread("thread-1", "Duplicate");
    const rename = requests(socket, "thread/name/set");
    assert.equal(rename.length, 1);
    assert.equal(session.operationPending("thread.rename", "thread-1"), true);
    reject(socket, rename[0], "rename denied");
    await Promise.resolve(); await Promise.resolve();
    assert.equal(session.operationPending("thread.rename", "thread-1"), false);
    assert.match(session.getSnapshot().notice, /Rename thread failed: rename denied/u);

    for (const [threadId, code] of [["provider-code", -32002], ["transport-code", -32020]]) {
        session.dismissNotice();
        session.renameThread(threadId, "Lifecycle-class rejection");
        const lifecycleFailure = requests(socket, "thread/name/set").at(-1);
        rejectWithCode(socket, lifecycleFailure, code, "request rejected without a lifecycle transition");
        assert.equal(session.operationPending("thread.rename", threadId), true,
            "lifecycle-class response waits for the SDK lifecycle ordering boundary");
        await Promise.resolve(); await Promise.resolve();
        assert.equal(session.operationPending("thread.rename", threadId), false);
        assert.match(session.getSnapshot().notice, /request rejected without a lifecycle transition/u,
            "an unchanged epoch completes the deferred error normally");
    }

    session.dispose();
});

test("stale operation completion cannot clear a newer exact pending operation", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "operation-generation");
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/started", params: {
        thread: {id: "operation-thread"},
    }}));
    const presentationKey = session.threadVisualKey("operation-thread");
    session.renameThread("operation-thread", "First", presentationKey);
    const first = requests(socket, "thread/name/set").at(-1);
    socket.receive({kind: "bridge.provider", state: "starting", providerGeneration: 1});
    socket.receive({kind: "bridge.provider", state: "ready", providerGeneration: 1});
    assert.equal(session.threadVisualKey("operation-thread"), presentationKey,
        "work invalidation alone cannot replace presentation identity");
    session.renameThread("operation-thread", "Second", presentationKey);
    const second = requests(socket, "thread/name/set").at(-1);
    assert.notEqual(second, first);
    respond(socket, first, {});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(session.operationPending("thread.rename", "operation-thread", presentationKey), true,
        "the stale promise cannot delete the new operation token");
    respond(socket, second, {});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(session.operationPending("thread.rename", "operation-thread", presentationKey), false);
    session.dispose();
});

test("thread catalog loads fast, repairs in the background, and pages on demand", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "thread-pages");

    const first = requests(socket, "thread/list").at(-1);
    assert.deepEqual(first.payload.params,
        {sortKey: "recency_at", sortDirection: "desc", limit: 100, useStateDbOnly: true});
    respond(socket, first, {data: [{id: "newest", recencyAt: 30}], nextCursor: "discarded-fast-cursor"});
    await waitForPublish();
    const repair = requests(socket, "thread/list").at(-1);
    assert.notEqual(repair, first);
    assert.deepEqual(repair.payload.params,
        {sortKey: "recency_at", sortDirection: "desc", limit: 100, useStateDbOnly: false});
    respond(socket, repair, {data: [
        {id: "repaired", recencyAt: 20}, {id: "newest", recencyAt: 30},
    ], nextCursor: "older-page"});
    await Promise.resolve(); await Promise.resolve();
    session.loadMoreThreads();
    const older = requests(socket, "thread/list").at(-1);
    assert.notEqual(older, repair);
    assert.deepEqual(older.payload.params, {
        sortKey: "recency_at", sortDirection: "desc", limit: 100,
        useStateDbOnly: true, cursor: "older-page",
    });
    respond(socket, older, {data: [{id: "oldest", recencyAt: 10}], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();

    assert.deepEqual(session.threadOrder(), ["newest", "repaired", "oldest"]);
    session.dispose();
});

test("thread paging falls back after repair failure and retries transient page errors", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "thread-page-recovery");

    const first = requests(socket, "thread/list").at(-1);
    respond(socket, first, {data: [{id: "newest", recencyAt: 30}], nextCursor: "fast-page"});
    await waitForPublish();
    const repair = requests(socket, "thread/list").at(-1);
    reject(socket, repair, "repair unavailable");
    await Promise.resolve(); await Promise.resolve();

    session.loadMoreThreads();
    const page = requests(socket, "thread/list").at(-1);
    assert.equal(page.payload.params.cursor, "fast-page",
        "failed reconciliation retains the DB-only fast cursor");
    reject(socket, page, "temporary paging failure");
    await Promise.resolve(); await Promise.resolve();

    session.loadMoreThreads();
    const retry = requests(socket, "thread/list").at(-1);
    assert.notEqual(retry, page);
    assert.equal(retry.payload.params.cursor, "fast-page",
        "a failed page does not consume its retry cursor");
    respond(socket, retry, {data: [{id: "older", recencyAt: 20}], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();
    assert.deepEqual(session.threadOrder(), ["newest", "older"]);
    session.dispose();
});

test("the first prompt after a successful fork starts without redundant hydration", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "fork-first-prompt");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [
        {id: "fork-source", preview: "Fork source", status: {type: "idle"}},
    ], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();

    session.forkThread("fork-source");
    const fork = requests(socket, "thread/fork").at(-1);
    assert.ok(fork);
    assert.deepEqual(fork.payload.params, {threadId: "fork-source", excludeTurns: true},
        "Quick fork changes no copied thread options and never sends a name field");
    const observedForkTitles = [];
    const unsubscribe = session.subscribe(() => {
        const title = session.model.thread("fork-result")?.title;
        if (title !== undefined) observedForkTitles.push(title);
    });
    respond(socket, fork, {thread: {
        id: "fork-result", forkedFromId: "fork-source", preview: "Fork result",
        status: {type: "idle"}, turns: [],
    }});
    await Promise.resolve(); await Promise.resolve();

    assert.equal(session.getSnapshot().selectedThreadId, "fork-result");
    assert.equal(session.model.thread("fork-result")?.title, "Fork source (fork 1)",
        "the automatic chosen name replaces the provider title immediately");
    assert.deepEqual([...new Set(observedForkTitles)], ["Fork source (fork 1)"],
        "no published frame exposes the provider title or thread ID");
    unsubscribe();
    const rename = requests(socket, "thread/name/set").at(-1);
    assert.ok(rename);
    assert.deepEqual(rename.payload.params,
        {threadId: "fork-result", name: "Fork source (fork 1)"});
    respond(socket, rename, {});
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/name/updated", params: {
        threadId: "fork-result", threadName: "Fork source (fork 1)",
    }}));
    assert.equal(session.model.thread("fork-result")?.title, "Fork source (fork 1)",
        "the chosen name survives its app-server acknowledgement");
    assert.equal(requests(socket, "thread/read").length, 0,
        "thread/fork already returns the loaded and subscribed thread");
    assert.equal(await session.submitPrompt("answer this fork"), true);
    await Promise.resolve(); await Promise.resolve();
    const start = requests(socket, "turn/start").at(-1);
    assert.ok(start);
    assert.equal(start.payload.params.threadId, "fork-result");
    assert.equal(start.payload.params.input[0].text, "answer this fork");
    assert.equal(requests(socket, "thread/resume").length, 0,
        "the fork prompt is never gated behind a redundant resume");
    session.dispose();
});

test("browser session uses canonical action routing and preserves prompt-response order", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect();
    socket.open();
    await readyProvider(socket, "web-test");

    const threadList = requests(socket, "thread/list").at(-1);
    const modelList = requests(socket, "model/list").at(-1);
    const profiles = requests(socket, "permissionProfile/list").at(-1);
    assert.ok(threadList && modelList && profiles, "catalog actions map to current protocol methods");
    respond(socket, threadList, {data: [{id: "thread-1", preview: "Browser thread", cwd: "/workspace", status: {type: "idle"}}]});
    respond(socket, modelList, {data: [{id: "gpt-current", displayName: "Current"}]});
    respond(socket, profiles, {data: []});
    session.selectThread("thread-1");
    await completeControllerHydration(socket,
        {id: "thread-1", preview: "Browser thread", cwd: "/workspace", status: {type: "idle"}, turns: []});

    const authoredPrompt = "  first authored line\n\nthird authored line\n\n";
    assert.equal(await session.submitPrompt(authoredPrompt), true);
    await Promise.resolve();
    const start = requests(socket, "turn/start").at(-1);
    assert.ok(start);
    assert.equal(start.payload.params.threadId, "thread-1");
    assert.equal(start.payload.params.input[0].text, authoredPrompt);
    assert.match(start.payload.params.clientUserMessageId, /^codexui-/u);
    assert.equal(session.conversation().sections[0].cards[0].payload.showPendingAnimation, false,
        "newly admitted prompts begin without motion");
    assert.equal(session.conversation().sections[0].cards[0].payload.prompt, authoredPrompt,
        "the optimistic card retains all authored blank lines");
    await new Promise(resolve => setTimeout(resolve, 1050));
    assert.equal(session.conversation().sections[0].cards[0].payload.showPendingAnimation, true,
        "the session republishes delayed feedback after one second");

    socket.receive(appserver({jsonrpc: "2.0", method: "turn/started", params: {
        threadId: "thread-1", turn: {id: "turn-1", status: "inProgress", items: []},
    }}));
    socket.receive(appserver({jsonrpc: "2.0", method: "item/started", params: {
        threadId: "thread-1", turnId: "turn-1", item: {id: "reasoning-1", type: "reasoning", summary: []},
    }}));
    socket.receive(appserver({jsonrpc: "2.0", method: "item/started", params: {
        threadId: "thread-1", turnId: "turn-1", item: {
            id: "user-1", type: "userMessage", clientId: start.payload.params.clientUserMessageId,
            content: [{type: "text", text: authoredPrompt}],
        },
    }}));
    respond(socket, start, {turn: {id: "turn-1", status: "inProgress"}});
    await waitForPublish();

    const visible = cardKeys(session.conversation()).map(stableKey);
    assert.equal(visible.length, 2);
    assert.match(visible[0], /^prompt:/u);
    assert.equal(visible[1], stableKey({kind: "item", threadId: "thread-1", turnId: "turn-1", itemId: "reasoning-1"}));
    assert.equal(session.model.connection().connected, true);
    assert.equal(session.model.connection().providerState, "ready");
    assert.equal(session.conversation().sections[0].cards[0].kind, "userMessage",
        "correlated acknowledgement materializes without a post-ack timer");
    assert.equal(session.conversation().sections[0].cards[0].payload.text, authoredPrompt,
        "the acknowledged card retains all authored blank lines");
    session.dispose();
});

test("acknowledged turn roots stay active across a delayed lifecycle event", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "border-handoff");
    respond(socket, requests(socket, "thread/list").at(-1),
        {data: [{id: "handoff-thread", status: {type: "idle"}}]});
    session.selectThread("handoff-thread");
    await completeControllerHydration(socket,
        {id: "handoff-thread", status: {type: "idle"}, turns: []});
    await session.submitPrompt("handoff prompt"); await Promise.resolve();
    const start = requests(socket, "turn/start").at(-1);
    socket.receive(appserver({jsonrpc: "2.0", method: "item/started", params: {
        threadId: "handoff-thread", turnId: "handoff-turn", item: {
            id: "handoff-user", type: "userMessage", clientId: start.payload.params.clientUserMessageId,
            content: [{type: "text", text: "handoff prompt"}],
        },
    }}));
    respond(socket, start, {turn: {id: "handoff-turn"}});
    await waitForPublish();
    const promoted = session.conversation();
    assert.equal(promoted.activeTurnId, "handoff-turn");
    assert.equal(session.model.activeTurnId("handoff-thread"), "handoff-turn",
        "the presentation model owns successful turn-start identity");
    assert.equal(promoted.sections[0]?.cards[0]?.kind, "userMessage");

    socket.receive(appserver({jsonrpc: "2.0", method: "turn/started", params: {
        threadId: "handoff-thread", turn: {id: "handoff-turn", status: "inProgress", items: []},
    }}));
    await waitForPublish();
    assert.equal(session.conversation().activeTurnId, "handoff-turn");
    socket.receive(appserver({jsonrpc: "2.0", method: "turn/completed", params: {
        threadId: "handoff-thread", turn: {id: "handoff-turn", status: "completed", items: []},
    }}));
    await waitForPublish();
    assert.equal(session.conversation().activeTurnId, undefined);
    session.dispose();
});

test("stream deltas reconcile prompts only when a user message can materialize", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open();
    await readyProvider(socket, "reconcile-scope");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "thread-1"}]});
    session.selectThread("thread-1");
    await completeControllerHydration(socket, {
        id: "thread-1", turns: [{id: "turn-1", status: "inProgress", items: [{
            id: "command-1", type: "commandExecution", status: "inProgress", command: "printf output",
        }]}],
    });
    let reconciliations = 0;
    const reconcile = session.prompts.reconcile.bind(session.prompts);
    session.prompts.reconcile = (...arguments_) => { ++reconciliations; return reconcile(...arguments_); };
    session.prompts.admit("thread-1", "local prompt", [], {}, session.model.thread("thread-1"), "turn-1", Date.now());

    socket.receive(appserver({jsonrpc: "2.0", method: "item/commandExecution/outputDelta", params: {
        threadId: "thread-1", turnId: "turn-1", itemId: "command-1", delta: "output\n",
    }}));
    assert.equal(reconciliations, 0, "command streaming does not scan authoritative prompt history");
    socket.receive(appserver({jsonrpc: "2.0", method: "item/started", params: {
        threadId: "thread-1", turnId: "turn-1", item: {
            id: "user-1", type: "userMessage", content: [{type: "text", text: "local prompt"}],
        },
    }}));
    assert.equal(reconciliations, 1, "authoritative user materialization performs one reconciliation");
    session.dispose();
});

test("new threads promote one draft identity into the authoritative row", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open();
    await readyProvider(socket, "new-thread");

    session.beginNewThread({
        workspace: "/workspace", name: "Named draft", baseInstructions: "Base context",
        developerInstructions: "Developer context", ephemeral: true,
    });
    const draftKey = session.threadVisualKey(DraftThreadId);
    const composerKey = session.composerVisualKey();
    const settingCatalog = turnSettingCatalog({models: [], permissionProfiles: []});
    const initialSettings = settingDraftFor(session.settingDrafts, draftKey, {cwd: "/workspace"}, settingCatalog);
    let authoredSettings = changeSettingDraft(session.settingDrafts, draftKey, {cwd: "/workspace"},
        settingCatalog, initialSettings.settingStamps, "approval", "never");
    authoredSettings = changeSettingDraft(session.settingDrafts, draftKey, {cwd: "/workspace"},
        settingCatalog, authoredSettings.settingStamps, "cwd", "/authored/workspace");
    assert.deepEqual(session.getSnapshot().newThreadDraft, {
        workspace: "/workspace", name: "", baseInstructions: "Base context",
        developerInstructions: "Developer context", ephemeral: true,
    });

    const options = settingPromptOptions(authoredSettings, settingCatalog);
    const submitted = session.submitPrompt("first prompt", [], {effort: "high", summary: "concise"},
        {...options.thread, model: "gpt-current"});
    const create = requests(socket, "thread/start").at(-1);
    assert.ok(create);
    assert.deepEqual(create.payload.params, {
        model: "gpt-current", approvalPolicy: "never", cwd: "/authored/workspace", baseInstructions: "Base context",
        developerInstructions: "Developer context", ephemeral: true,
    });
    assert.equal(await session.submitPrompt("queued during creation", [], {effort: "must-not-steer"},
        {model: "must-not-create"}), true);
    session.beginNewThread();
    assert.equal(session.threadVisualKey(DraftThreadId), draftKey);
    assert.equal(requests(socket, "thread/start").length, 1,
        "additional prompts and New Thread cannot duplicate an in-flight creation");
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/started", params: {
        thread: {id: "created-thread", name: "Created thread", cwd: "/workspace", status: {type: "idle"}},
    }}));
    const displacedKey = session.threadVisualKey("created-thread");
    settingDraftFor(session.settingDrafts, displacedKey, session.model.thread("created-thread").raw,
        settingCatalog, session.model.thread("created-thread").settingStamps);
    respond(socket, create, {thread: {id: "created-thread", name: "Created thread", cwd: "/workspace", status: {type: "idle"}}});
    assert.equal(await submitted, true);
    await Promise.resolve();
    assert.equal(session.getSnapshot().newThreadDraft, undefined);
    assert.equal(session.model.thread("created-thread")?.title, "Created thread");
    assert.equal(requests(socket, "thread/name/set").length, 0,
        "an ephemeral thread never sends an unsupported metadata update");

    assert.equal(session.threadVisualKey("created-thread"), draftKey);
    assert.equal(session.composerVisualKey(), composerKey,
        "acknowledgement cannot replace the shared composer object");
    assert.equal(session.settingDrafts.get(draftKey), authoredSettings,
        "optimistic promotion retains the exact settings draft object");
    assert.equal(session.settingDrafts.has(displacedKey), false,
        "promotion retires a pre-published duplicate presentation bucket and its draft");

    const start = requests(socket, "turn/start").at(-1);
    assert.ok(start);
    assert.equal(start.payload.params.effort, "high");
    assert.equal(start.payload.params.summary, "concise");
    assert.equal(Object.hasOwn(start.payload.params, "model"), false,
        "thread/start options cannot leak into turn/start");
    respond(socket, start, {turn: {id: "created-turn", status: "inProgress"}});
    await Promise.resolve(); await waitForPublish();
    const steer = requests(socket, "turn/steer").at(-1);
    assert.ok(steer);
    assert.equal(steer.payload.params.expectedTurnId, "created-turn");
    assert.equal(steer.payload.params.input[0].text, "queued during creation");
    assert.equal(Object.hasOwn(steer.payload.params, "effort"), false);
    assert.equal(Object.hasOwn(steer.payload.params, "model"), false);
    assert.equal(session.threadVisualKey("created-thread"), draftKey,
        "canonical styling retains the optimistic row's React identity");
    assert.equal(session.model.thread("created-thread")?.title, "Created thread");
    assert.equal(session.model.thread("created-thread")?.localNameOverlay, undefined);
    session.dispose();
});

test("new-thread completion preserves later navigation and explicit drafts start clean", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open();
    await readyProvider(socket, "new-thread-navigation");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "other-thread"}]});

    session.beginNewThread();
    const firstDraftKey = session.threadVisualKey(DraftThreadId);
    const firstComposerKey = session.composerVisualKey();
    const submitted = session.submitPrompt("background creation");
    const create = requests(socket, "thread/start").at(-1);
    socket.receive({kind: "bridge.provider", state: "ready", providerGeneration: 1});
    await Promise.resolve(); await waitForPublish();
    assert.equal(requests(socket, "thread/read").some(request =>
        request.payload.params.threadId === DraftThreadId), false,
    "provider publication cannot hydrate the optimistic sentinel");
    session.selectThread("other-thread");
    respond(socket, create, {thread: {id: "background-thread", status: {type: "idle"}}});
    assert.equal(await submitted, true);
    await Promise.resolve();
    assert.equal(session.getSnapshot().selectedThreadId, "other-thread",
        "creation by a background draft cannot replace the user's later selection");
    assert.equal(requests(socket, "turn/start").at(-1).payload.params.threadId, "background-thread");

    session.beginNewThread();
    assert.equal(session.prompts.submissions("__codexui_new_thread__").length, 0);
    assert.notEqual(session.threadVisualKey(DraftThreadId), firstDraftKey,
        "explicit New thread establishes a fresh shared composer identity");
    assert.notEqual(session.composerVisualKey(), firstComposerKey,
        "only explicit New thread replaces the shared composer draft lifetime");
    session.dispose();
});

test("settings drafts follow thread incarnations and remain bounded across retirement", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "settings-lifetime");
    const catalog = turnSettingCatalog({models: [], permissionProfiles: []});

    socket.receive(appserver({jsonrpc: "2.0", method: "thread/started", params: {
        thread: {id: "settings-thread", approvalPolicy: "on-request"},
    }}));
    const thread = session.model.thread("settings-thread");
    const firstIdentity = session.threadVisualKey("settings-thread");
    const initial = settingDraftFor(session.settingDrafts, firstIdentity, thread.raw,
        catalog, thread.settingStamps);
    const authored = changeSettingDraft(session.settingDrafts, firstIdentity, thread.raw,
        catalog, initial.settingStamps, "approval", "never");
    session.selectThread("settings-thread");
    session.selectThread("another-thread");
    assert.equal(session.settingDrafts.get(firstIdentity), authored,
        "ordinary navigation preserves the semantic thread's exact draft");

    socket.receive({kind: "bridge.provider", state: "ready", providerGeneration: 2});
    await Promise.resolve();
    assert.equal(session.settingDrafts.size, 0, "provider replacement retires prior-authority drafts");
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/started", params: {
        thread: {id: "settings-thread", approvalPolicy: "on-request"},
    }}));
    const replacement = session.model.thread("settings-thread");
    const replacementIdentity = session.threadVisualKey("settings-thread");
    assert.notEqual(replacementIdentity, firstIdentity);
    const fresh = settingDraftFor(session.settingDrafts, replacementIdentity, replacement.raw,
        catalog, replacement.settingStamps);
    assert.notEqual(fresh, authored);
    assert.equal(fresh.touched.size, 0, "a reused provider ID starts a fresh draft incarnation");

    for (let index = 0; index < 128; ++index) {
        const id = `retired-settings-${index}`;
        socket.receive(appserver({jsonrpc: "2.0", method: "thread/started", params: {thread: {id}}}));
        const current = session.model.thread(id);
        settingDraftFor(session.settingDrafts, session.threadVisualKey(id), current.raw, catalog,
            current.settingStamps);
        socket.receive(appserver({jsonrpc: "2.0", method: "thread/deleted", params: {threadId: id}}));
    }
    assert.equal(session.settingDrafts.size, 1,
        "explicit retirement removes each draft in constant work without a render-time sweep");
    assert.equal(session.viewportState.retainedPresentationCount(), 1,
        "viewport, fold, and presentation identity retire at the same boundary");
    session.dispose();
});

test("thread ordering exposes Alphanumeric, Created, and Recent contracts", () => {
    const session = new BrowserFrontendSession("ws://bridge.test/", () => new FakeSocket());
    session.model.applyEvent(result(96, 1, "threads.list", "list", true, {threads: [
        {id: "missing", name: "2 tasks", createdAt: 100, updatedAt: 1000},
        {id: "old", name: "20 tasks", recencyAt: 10, createdAt: 200, updatedAt: 2000},
        {id: "new", name: "Alpha", recencyAt: 30, createdAt: 1, updatedAt: 2},
    ]}, "merge"));
    assert.deepEqual(session.threadOrder("recent"), ["new", "old", "missing"]);
    assert.deepEqual(session.threadOrder("created"), ["old", "missing", "new"]);
    assert.deepEqual(session.threadOrder("alphanumeric"), ["missing", "old", "new"]);
    session.dispose();
});

test("a new turn in a child thread promotes its root Recent group", () => {
    const session = new BrowserFrontendSession("ws://bridge.test/", () => new FakeSocket());
    session.model.applyEvent(result(97, 1, "threads.list", "list", true, {threads: [
        {id: "newer-root", name: "Newer", recencyAt: 30},
        {id: "older-root", name: "Older", recencyAt: 20},
        {id: "child", name: "Child", parentThreadId: "older-root", recencyAt: 10},
    ]}, "merge"));
    assert.deepEqual(session.threadOrder(), ["newer-root", "older-root"]);
    session.prompts.admit("child", "child turn", [], {}, session.model.thread("child"), undefined, 40_000, 31);
    assert.deepEqual(session.threadOrder(), ["older-root", "newer-root"]);
    session.dispose();
});

test("fork names preserve root and nested lineage without collisions", () => {
    const titles = ["Original", "Original (fork 1)", "Original (fork 2)",
        "Original (fork 1.1)", "Original (fork 1.3)", "Original (fork 1.1.1)"];
    assert.equal(suggestForkName("Original", titles), "Original (fork 3)");
    assert.equal(suggestForkName("Original (fork 1)", titles), "Original (fork 1.2)");
    assert.equal(suggestForkName("Original (fork 1.1)", titles), "Original (fork 1.1.2)");
    assert.equal(suggestForkName("Separate", titles), "Separate (fork 1)");
});

test("ephemeral Fork with options sends adjustable fields without a rename", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "fork-options");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [
        {id: "source", name: "Original (fork 1)", cwd: "/old"},
        {id: "child", name: "Original (fork 1.1)", cwd: "/old"},
    ], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(session.forkDraft("source").name, "Original (fork 1.2)");

    session.forkThread("source", {
        workspace: "/new", name: "Chosen fork", baseInstructions: "Base",
        developerInstructions: "Developer", ephemeral: true,
    });
    const fork = requests(socket, "thread/fork").at(-1);
    assert.deepEqual(fork.payload.params, {
        threadId: "source", excludeTurns: true, cwd: "/new", baseInstructions: "Base",
        developerInstructions: "Developer", ephemeral: true,
    });
    respond(socket, fork, {thread: {id: "advanced", name: "Provider name", cwd: "/new"}});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(session.model.thread("advanced")?.title, "Provider name");
    assert.equal(requests(socket, "thread/name/set").length, 0);
    session.dispose();
});

test("Recent promotes on turn admission, reverts rejection, confirms acknowledgement, and shares prompt animation", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open();
    await readyProvider(socket, "promotion");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [
        {id: "older", recencyAt: 10, updatedAt: 12, status: {type: "idle"}},
        {id: "recent", recencyAt: 30, updatedAt: 32, status: {type: "idle"}},
    ], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();
    assert.deepEqual(session.threadOrder(), ["recent", "older"],
        "Recent is explicit newest-first ordering");

    const realNow = Date.now;
    try {
        Date.now = () => 40_000;
        session.selectThread("older");
        await completeControllerHydration(socket, {
            id: "older", recencyAt: 10, updatedAt: 12, status: {type: "idle"}, turns: [],
        });
        await session.submitPrompt("use older thread");
        await Promise.resolve();
        const rejectedStart = requests(socket, "turn/start").at(-1);
        assert.deepEqual(session.threadOrder(), ["older", "recent"],
            "the admitted prompt updates its thread recency immediately");
        assert.equal(session.threadPromptAnimating("older"), false,
            "the thread card and Turn/You card share the calm delay");
        Date.now = () => 41_001;
        assert.equal(session.threadPromptAnimating("older"), true,
            "the thread card begins motion at the Turn/You animation deadline");
        reject(socket, rejectedStart, "prompt rejected");
        await Promise.resolve(); await Promise.resolve();
        assert.deepEqual(session.threadOrder(), ["recent", "older"],
            "a rejected admission removes its optimistic recency");
        assert.equal(session.threadPromptAnimating("older"), false,
            "the exact failed prompt stops both animations");

        Date.now = () => 40_000;
        await session.submitPrompt("confirm older thread");
        await Promise.resolve();
        const acceptedOlder = requests(socket, "turn/start").at(-1);
        respond(socket, acceptedOlder, {turn: {id: "older-turn", status: "inProgress"}});
        await Promise.resolve(); await Promise.resolve();
        assert.deepEqual(session.threadOrder(), ["older", "recent"],
            "acknowledgement confirms the admitted turn order");
        assert.equal(session.model.thread("older").recencyAt, 10,
            "client confirmation preserves app-server recency as provider data");
        assert.equal(session.model.thread("older").raw.updatedAt, 12,
            "turn ordering never rewrites last-changed data");

        session.selectThread("recent");
        await completeControllerHydration(socket, {
            id: "recent", recencyAt: 30, updatedAt: 32, status: {type: "idle"}, turns: [],
        });
        await session.submitPrompt("confirm recent thread");
        await Promise.resolve();
        assert.deepEqual(session.threadOrder(), ["recent", "older"],
            "same-clock admissions still follow their monotonic admission order");
        respond(socket, requests(socket, "turn/start").at(-1),
            {turn: {id: "recent-turn", status: "inProgress"}});
        await Promise.resolve(); await Promise.resolve();
        assert.deepEqual(session.threadOrder(), ["recent", "older"]);
    } finally { Date.now = realNow; }

    session.model.applyEvent(result(101, 1, "thread.read", "read", true,
        {thread: {id: "older", recencyAt: 11}}, "merge", {threadId: "older"}));
    assert.deepEqual(session.threadOrder(), ["recent", "older"],
        "stale authoritative recency cannot undo newer local activity");
    assert.equal(session.model.thread("older").recencyAt, 11);
    assert.ok(session.model.thread("older").localPromptActivityAt > 30);
    session.dispose();
});

test("thread activity preserves provider time during hydration and advances for meaningful traffic", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "activity");
    const listed = requests(socket, "thread/list").at(-1);
    respond(socket, listed, {data: [
        {id: "tracked", updatedAt: 20, recencyAt: 30},
        {id: "updated-only", updatedAt: 25},
    ]});
    assert.equal(session.model.thread("tracked").lastActivityAt, 30);
    assert.equal(session.model.thread("updated-only").lastActivityAt, 25);

    session.selectThread("tracked");
    assert.equal(session.model.thread("tracked").lastActivityAt, 30,
        "selection-driven hydration does not replace authoritative activity");

    await completeControllerHydration(socket, {id: "tracked", updatedAt: 20, recencyAt: 30, turns: []});
    assert.equal(session.model.thread("tracked").lastActivityAt, 30,
        "authoritative resume response remains the activity source during hydration");

    const beforeOutbound = Math.floor(Date.now() / 1000);
    session.renameThread("tracked", "Renamed tracked thread");
    assert.ok(session.model.thread("tracked").lastActivityAt >= beforeOutbound,
        "meaningful thread requests advance local activity immediately");
    const rename = requests(socket, "thread/name/set").at(-1);
    session.model.thread("tracked").lastActivityAt = 1;
    const beforeResponse = Math.floor(Date.now() / 1000);
    respond(socket, rename, {});
    assert.ok(session.model.thread("tracked").lastActivityAt >= beforeResponse,
        "meaningful thread responses advance local activity");
    assert.equal(session.model.thread("tracked").raw.updatedAt, 20);
    assert.equal(session.model.thread("tracked").recencyAt, 30,
        "non-prompt traffic does not reorder Recent or rewrite provider timestamps");

    session.model.thread("tracked").lastActivityAt = 1;
    const beforeInbound = Math.floor(Date.now() / 1000);
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/status/changed", params: {
        threadId: "tracked", status: {type: "idle"},
    }}));
    assert.ok(session.model.thread("tracked").lastActivityAt >= beforeInbound,
        "thread-scoped app-server frames advance local activity");
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/settings/updated", params: {
        threadId: "tracked", threadSettings: {model: "gpt-a"},
    }}));
    session.model.thread("tracked").lastActivityAt = 1;
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/settings/updated", params: {
        threadId: "tracked", threadSettings: {model: "gpt-a"},
    }}));
    assert.equal(session.model.thread("tracked").lastActivityAt, 1,
        "an effective settings no-op does not create conversation activity");
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/settings/updated", params: {
        threadId: "tracked", threadSettings: {reasoningEffort: "high"},
    }}));
    session.model.thread("tracked").lastActivityAt = 1;
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/settings/updated", params: {
        threadId: "tracked", threadSettings: {effort: "high"},
    }}));
    assert.equal(session.model.thread("tracked").lastActivityAt, 1,
        "an equivalent setting-alias migration does not create activity");
    assert.equal(session.model.thread("tracked").raw.effort, "high");
    assert.equal(Object.hasOwn(session.model.thread("tracked").raw, "reasoningEffort"), false);
    session.dispose();
});

test("browser transport reconnects cleanly across provider generations", async () => {
    const sockets = [];
    const session = new BrowserFrontendSession("ws://bridge.test/", () => {
        const socket = new FakeSocket(); sockets.push(socket); return socket;
    });
    session.connect(); sockets[0].open();
    await readyProvider(sockets[0], "first", "observer");
    respond(sockets[0], requests(sockets[0], "thread/list").at(-1), {data: [{id: "old-thread"}]});
    assert.ok(session.model.thread("old-thread"));
    sockets[0].receive({kind: "bridge.provider", state: "disconnected", providerGeneration: 1, reason: "restart"});
    assert.equal(session.model.thread("old-thread"), undefined);
    sockets[0].close(1000, "restart");
    session.reconnect(); await Promise.resolve();
    assert.equal(sockets.length, 2);
    sockets[1].open();
    await readyProvider(sockets[1], "second", "controller", 2);
    assert.ok(requests(sockets[1], "thread/list").length > 0);
    assert.equal(session.model.connection().providerGeneration, 2);
    assert.equal(session.role(), "controller");
    session.dispose();
});

test("a pre-open WebSocket failure can retry after completed detach", async () => {
    const sockets = [];
    const session = new BrowserFrontendSession("ws://bridge.test/", () => {
        const socket = new DelayedCloseSocket(); sockets.push(socket); return socket;
    });
    session.connect();
    sockets[0].onerror?.();
    session.connect();
    assert.equal(sockets.length, 1, "replacement waits until the failed endpoint releases the connection");
    sockets[0].finishClose();
    await Promise.resolve();
    assert.equal(sockets.length, 2, "the requested retry starts immediately after detach completion");
    sockets[1].open();
    assert.equal(session.model.connection().connected, true);
    session.dispose();
});

test("malformed WebSocket text is contained at the transport boundary", () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); socket.receiveText("{");
    assert.equal(socket.readyState, 3);
    assert.equal(session.model.connection().connected, false);
    session.dispose();
});

test("browser session resumes a not-loaded thread before starting its queued turn", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open();
    await readyProvider(socket, "resume-test");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "sleeping", status: {type: "notLoaded"}}]});
    session.selectThread("sleeping");
    await session.submitPrompt("wake and work");
    const resume = requests(socket, "thread/resume").at(-1);
    assert.ok(resume);
    assert.equal(resume.payload.params.excludeTurns, true,
        "resume activates live state without embedding thread history");
    assert.equal(requests(socket, "turn/start").length, 0);
    respond(socket, resume, {thread: {id: "sleeping", status: {type: "idle"}}});
    await Promise.resolve(); await Promise.resolve();
    respond(socket, requests(socket, "thread/turns/list").at(-1), {data: [], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(requests(socket, "turn/start").at(-1).payload.params.input[0].text, "wake and work");
    session.dispose();
});

test("a hydration resume is not repeated before dispatching its already-queued prompt", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "resume-without-progress");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [
        {id: "still-sleeping", status: {type: "notLoaded"}},
    ]});
    await Promise.resolve();
    session.selectThread("still-sleeping");
    const resume = requests(socket, "thread/resume").at(-1);
    await session.submitPrompt("do not spin");
    assert.ok(resume);
    respond(socket, resume, {thread: {id: "still-sleeping", status: {type: "notLoaded"}}});
    await Promise.resolve(); await Promise.resolve();
    respond(socket, requests(socket, "thread/turns/list").at(-1), {data: [], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(requests(socket, "thread/resume").length, 1);
    assert.equal(requests(socket, "turn/start").length, 1);
    session.dispose();
});

test("resume eligibility follows authoritative lifecycle state after hydration", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "lifecycle-resume");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [
        {id: "closed-after-read", status: {type: "idle"}},
        {id: "woken-after-read", status: {type: "notLoaded"}},
    ]});

    session.selectThread("closed-after-read");
    await completeControllerHydration(socket,
        {id: "closed-after-read", status: {type: "idle"}, turns: []});
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/closed", params: {
        threadId: "closed-after-read",
    }}));
    await session.submitPrompt("resume closed thread"); await Promise.resolve();
    assert.equal(requests(socket, "turn/start").length, 0);
    assert.equal(requests(socket, "thread/resume").length, 2,
        "a post-hydration close is derived from current model status");

    session.selectThread("woken-after-read");
    await completeControllerHydration(socket,
        {id: "woken-after-read", status: {type: "notLoaded"}, turns: []});
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/status/changed", params: {
        threadId: "woken-after-read", status: {type: "idle"},
    }}));
    await session.submitPrompt("already awake"); await Promise.resolve();
    assert.equal(requests(socket, "thread/resume").length, 3,
        "a post-hydration active status does not retain stale resume state");
    assert.equal(requests(socket, "turn/start").at(-1).payload.params.threadId, "woken-after-read");
    session.dispose();
});

test("prompt admission requires provider readiness and controller authority", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open();
    socket.receive({kind: "bridge.connection", event: "opened", connectionId: "admission", role: "controller"});
    assert.equal(session.canSubmit(), false);
    session.beginNewThread();
    assert.equal(await session.submitPrompt("too early"), false);
    assert.equal(requests(socket, "thread/start").length, 0);

    socket.receive({kind: "bridge.provider", state: "ready", providerGeneration: 1});
    await Promise.resolve();
    socket.receive({kind: "bridge.controller", controllerConnectionId: "another-client"});
    assert.equal(session.canSubmit(), false);
    assert.equal(await session.submitPrompt("observer attempt"), false);
    assert.equal(requests(socket, "thread/start").length, 0);

    socket.receive({kind: "bridge.controller", controllerConnectionId: "admission"});
    assert.equal(session.canSubmit(), true);
    session.dispose();
});

test("bounded protocol history stays independent of mutable presentation state", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "history-ownership");
    session.selectThread("history");
    await completeControllerHydration(socket, {id: "history", turns: [
        {id: "turn", items: [{id: "item", type: "agentMessage", text: "original"}]},
    ]});
    socket.receive(appserver({jsonrpc: "2.0", method: "item/started", params: {
        threadId: "history", turnId: "turn", item: {id: "item", type: "agentMessage", text: "original"},
    }}));
    const diagnosticFrames = [...session.getSnapshot().protocolFrames];
    const originalFrames = structuredClone(diagnosticFrames);
    for (let index = 0; index < 510; ++index) socket.receive(appserver({jsonrpc: "2.0",
        method: "item/agentMessage/delta", params: {
            threadId: "history", turnId: "turn", itemId: "item", delta: "x",
        }}));
    assert.equal(session.model.thread("history").turns.get("turn").items.get("item").raw.text,
        `original${"x".repeat(510)}`);
    assert.deepEqual(diagnosticFrames, originalFrames, "later streaming cannot rewrite result or notification history");
    const retained = session.getSnapshot().protocolFrames;
    assert.equal(retained.length, 500);
    assert.ok(retained.every(frame => frame.type === "conversation.item.append" && frame.data.text === "x"));
    assert.equal(retained.at(-1).sequence - retained[0].sequence, 499);
    session.dispose();
});

test("pending request responses require current controller authority and resolve once", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open();
    await readyProvider(socket, "request-controller");

    socket.receive(appserver({jsonrpc: "2.0", id: 77, method: "item/commandExecution/requestApproval", params: {
        threadId: "thread-request", turnId: "turn-request", itemId: "item-request", command: "echo safe",
        availableDecisions: ["accept", "decline"],
    }}));
    const request = [...session.model.pendingRequestPresentations().values()][0];
    assert.ok(request);
    assert.equal(request.raw.command, "echo safe", "the typed decision surface retains required content");
    const retainedDiagnostic = JSON.stringify(session.getSnapshot().protocolFrames.at(-1));
    assert.doesNotMatch(retainedDiagnostic, /echo safe/u);
    assert.match(retainedDiagnostic, /redacted; inspect the typed Requests view/u,
        "Protocol diagnostics do not retain raw server-request content");
    assert.equal(session.canResolvePending(request), true);
    assert.equal(session.resolvePending(request, {choice: "accept"}), true);
    assert.equal(session.resolvePending(request, {choice: "accept"}), false);
    const responses = socket.sent.filter(message => message.kind === "appserver" && message.payload.id === 77
        && !Object.hasOwn(message.payload, "method"));
    assert.equal(responses.length, 1, "a repeated action cannot emit a duplicate JSON-RPC response");
    assert.equal(session.isPendingResolving(request), true);

    socket.receive(appserver({jsonrpc: "2.0", method: "serverRequest/resolved", params: {
        requestId: 77, threadId: "thread-request",
    }}));
    assert.equal(session.model.pendingRequestPresentations().size, 0);
    assert.equal(session.isPendingResolving(request), false);

    socket.receive(appserver({jsonrpc: "2.0", id: 78, method: "item/commandExecution/requestApproval", params: {
        threadId: "thread-request", turnId: "turn-request", itemId: "item-request-2", command: "echo guarded",
    }}));
    const guarded = [...session.model.pendingRequestPresentations().values()][0];
    assert.ok(guarded);
    socket.receive({kind: "bridge.controller", controllerConnectionId: "another-client"});
    assert.equal(session.canResolvePending(guarded), false);
    assert.equal(session.resolvePending(guarded, {choice: "accept"}), false);
    socket.receive({kind: "bridge.controller", controllerConnectionId: "request-controller"});
    assert.equal(session.canResolvePending(guarded), true);
    session.dispose();
});

test("pending request identity is immutable until retirement and resets before same-ID reuse", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "request-lifetime");
    const request = command => socket.receive(appserver({jsonrpc: "2.0", id: 90,
        method: "item/commandExecution/requestApproval", params: {
            threadId: "request-thread", turnId: "request-turn", itemId: "request-item", command,
            availableDecisions: ["accept", "decline"],
        }}));
    request("echo first");
    const first = session.model.pendingRequestPresentations().get("90");
    assert.ok(first);
    const firstPresentation = first.presentationKey;
    const protocolCount = session.getSnapshot().protocolFrames.length;
    request("echo updated");
    const updated = session.model.pendingRequestPresentations().get("90");
    assert.equal(updated, first);
    assert.equal(updated.presentationKey, firstPresentation);
    assert.equal(updated.raw.command, "echo first",
        "a duplicate JSON-RPC slot cannot mutate the request behind rendered actions");
    assert.equal(session.getSnapshot().protocolFrames.length, protocolCount,
        "an ambiguous same-ID request is rejected before presentation side effects");
    assert.equal(session.resolvePending(updated, {choice: "accept"}), true);
    socket.receive({kind: "bridge.provider", state: "starting", providerGeneration: 1});
    socket.receive({kind: "bridge.provider", state: "ready", providerGeneration: 1});
    assert.equal(session.model.pendingRequestPresentations().get("90"), first);
    assert.equal(session.isPendingResolving(first), true);
    assert.equal(session.resolvePending(first, {choice: "accept"}), false,
        "a transient provider state cannot revive an accepted reverse-interaction token");
    socket.receive(appserver({jsonrpc: "2.0", method: "serverRequest/resolved", params: {
        requestId: 90, threadId: "request-thread",
    }}));
    assert.equal(session.isPendingResolving(first), false);

    request("echo replacement");
    const replacement = session.model.pendingRequestPresentations().get("90");
    assert.ok(replacement);
    assert.notEqual(replacement, first);
    assert.notEqual(replacement.presentationKey, firstPresentation,
        "React form state cannot cross an explicit request retirement boundary");
    assert.equal(session.isPendingResolving(replacement), false);
    assert.equal(session.canResolvePending(first), false);
    assert.equal(session.canResolvePending(replacement), true);
    session.dispose();
});

test("a prompt admitted during thread hydration waits for the first authoritative turn page", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "hydrate-before-send");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "thread-h", status: {type: "idle"}}]});
    session.selectThread("thread-h");
    const resume = requests(socket, "thread/resume").at(-1);
    assert.ok(resume);
    assert.equal(await session.submitPrompt("wait for page"), true);
    await Promise.resolve();
    assert.equal(requests(socket, "turn/start").length, 0);

    respond(socket, resume, {thread: {id: "thread-h", status: {type: "idle"}}});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(requests(socket, "turn/start").length, 0);
    respond(socket, requests(socket, "thread/turns/list").at(-1), {data: [], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(requests(socket, "turn/start").at(-1).payload.params.input[0].text, "wait for page");
    session.dispose();
});

test("a queued prompt waits through controller loss and resumes after control returns", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "role-transition");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "role-thread", status: {type: "idle"}}]});
    session.selectThread("role-thread");
    const resume = requests(socket, "thread/resume").at(-1);
    assert.equal(await session.submitPrompt("wait for control"), true);
    socket.receive({kind: "bridge.controller", controllerConnectionId: "other-client"});
    respond(socket, resume, {thread: {id: "role-thread", status: {type: "idle"}}});
    await Promise.resolve(); await Promise.resolve();
    respond(socket, requests(socket, "thread/turns/list").at(-1), {data: [], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(requests(socket, "turn/start").length, 0);

    socket.receive({kind: "bridge.controller", controllerConnectionId: "role-transition"});
    await Promise.resolve();
    assert.equal(requests(socket, "turn/start").at(-1).payload.params.input[0].text, "wait for control");
    session.dispose();
});

test("provider loss makes a queued hydration-gated prompt terminal instead of resending it", async () => {
    const sockets = [];
    const session = new BrowserFrontendSession("ws://bridge.test/", () => {
        const socket = new FakeSocket(); sockets.push(socket); return socket;
    });
    session.connect(); sockets[0].open(); await readyProvider(sockets[0], "generation-a");
    respond(sockets[0], requests(sockets[0], "thread/list").at(-1), {data: [{id: "retained", status: {type: "idle"}}]});
    session.selectThread("retained");
    const staleResume = requests(sockets[0], "thread/resume").at(-1);
    assert.equal(await session.submitPrompt("do not send after restart"), true);
    sockets[0].receive({kind: "bridge.provider", state: "disconnected", providerGeneration: 1, reason: "restart"});
    sockets[0].close(1000, "restart");
    respond(sockets[0], staleResume, {thread: {id: "retained", status: {type: "idle"}}});
    assert.equal(session.prompts.submissions("retained").at(-1).state, "failed");
    assert.match(session.prompts.submissions("retained").at(-1).error, /queued prompt was sent/u);

    session.reconnect(); await Promise.resolve();
    sockets[1].open(); await readyProvider(sockets[1], "generation-b", "controller", 2);
    respond(sockets[1], requests(sockets[1], "thread/list").at(-1), {data: [{id: "retained", status: {type: "idle"}}]});
    await completeControllerHydration(sockets[1], {id: "retained", status: {type: "idle"}, turns: []});
    assert.equal(requests(sockets[0], "turn/start").length, 0);
    assert.equal(requests(sockets[1], "turn/start").length, 0,
        "reconnection and hydration never submit an already-admitted prompt");
    session.dispose();
});

test("provider replacement retires an in-flight turn with an uncertain outcome and no resend", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "inflight-generation-a");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [
        {id: "inflight-thread", status: {type: "idle"}},
    ]});
    await Promise.resolve();
    session.selectThread("inflight-thread");
    await completeControllerHydration(socket,
        {id: "inflight-thread", status: {type: "idle"}, turns: []});
    assert.equal(await session.submitPrompt("send exactly once"), true);
    await Promise.resolve();
    const start = requests(socket, "turn/start").at(-1);
    assert.ok(start);

    socket.receive({kind: "bridge.provider", state: "ready", providerGeneration: 2});
    await Promise.resolve(); await Promise.resolve();
    const submission = session.prompts.submissions("inflight-thread").at(-1);
    assert.equal(submission.state, "failed");
    assert.match(submission.error, /outcome is unknown/u);
    assert.equal(session.getSnapshot().protocolFrames.some(frame =>
        frame?.kind === "result" && frame?.action === "turn.start"), false,
    "the synthetic stale SDK failure never enters presentation diagnostics");

    await completeControllerHydration(socket,
        {id: "inflight-thread", status: {type: "idle"}, turns: []});
    assert.equal(requests(socket, "turn/start").length, 1);
    session.dispose();
});

test("transport detach retires an in-flight turn before its synthetic failure callback", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "inflight-detach");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [
        {id: "detach-thread", status: {type: "idle"}},
    ]});
    await Promise.resolve();
    session.selectThread("detach-thread");
    await completeControllerHydration(socket,
        {id: "detach-thread", status: {type: "idle"}, turns: []});
    await session.submitPrompt("uncertain transport outcome"); await Promise.resolve();
    assert.equal(requests(socket, "turn/start").length, 1);

    socket.close(1006, "network lost");
    await Promise.resolve(); await Promise.resolve();
    assert.equal(session.prompts.submissions("detach-thread").at(-1).state, "failed");
    assert.equal(session.getSnapshot().protocolFrames.some(frame =>
        frame?.kind === "result" && frame?.action === "turn.start"), false);
    session.dispose();
});

test("a provider-generation jump cannot apply pending create, user-operation, or interrupt callbacks", async () => {
    const createSocket = new FakeSocket();
    const createSession = new BrowserFrontendSession("ws://bridge.test/", () => createSocket);
    createSession.connect(); createSocket.open(); await readyProvider(createSocket, "stale-create");
    createSession.beginNewThread({workspace: "/work", name: "Draft name",
        baseInstructions: "", developerInstructions: "", ephemeral: false});
    const draftPresentation = createSession.threadVisualKey(DraftThreadId);
    const draftComposer = createSession.composerVisualKey();
    createSession.viewportState.updateScroll(DraftThreadId, draftPresentation, 144, false,
        {cardKey: "local-prompt", pixelOffset: 12});
    createSession.viewportState.setCardCollapsed(DraftThreadId, draftPresentation, "local-prompt", true);
    const creating = createSession.submitPrompt("retained draft prompt");
    assert.equal(requests(createSocket, "thread/start").length, 1);
    createSocket.receive({kind: "bridge.provider", state: "ready", providerGeneration: 2});
    assert.equal(await creating, false);
    await Promise.resolve();
    assert.equal(createSession.getSnapshot().selectedThreadId, DraftThreadId);
    assert.equal(createSession.getSnapshot().newThreadDraft?.name, "Draft name");
    assert.equal(createSession.threadVisualKey(DraftThreadId), draftPresentation,
        "provider replacement cannot remount an authored draft");
    assert.equal(createSession.composerVisualKey(), draftComposer);
    assert.deepEqual(createSession.viewportState.scroll(DraftThreadId, draftPresentation), {
        anchor: {cardKey: "local-prompt", pixelOffset: 12}, scrollTop: 144, following: false,
    });
    assert.equal(createSession.viewportState.cardCollapsed(DraftThreadId, draftPresentation,
        "local-prompt", false), true);
    assert.equal(createSession.prompts.submissions(DraftThreadId).at(-1).state, "failed");
    assert.equal(requests(createSocket, "turn/start").length, 0);
    assert.equal(createSession.getSnapshot().notice, "");
    assert.equal(createSession.getSnapshot().protocolFrames.some(frame =>
        frame?.kind === "result" && frame?.action === "thread.create"), false);
    createSession.dispose();

    const operationSocket = new FakeSocket();
    const operationSession = new BrowserFrontendSession("ws://bridge.test/", () => operationSocket);
    operationSession.connect(); operationSocket.open(); await readyProvider(operationSocket, "stale-operation");
    respond(operationSocket, requests(operationSocket, "thread/list").at(-1), {data: [
        {id: "operation-thread", status: {type: "active"}},
    ]});
    await Promise.resolve();
    operationSession.selectThread("operation-thread");
    await completeControllerHydration(operationSocket, {
        id: "operation-thread", status: {type: "active"},
        turns: [{id: "operation-turn", status: "inProgress", items: []}],
    });
    operationSession.renameThread("operation-thread", "Old generation name");
    operationSession.interrupt();
    assert.equal(requests(operationSocket, "thread/name/set").length, 1);
    assert.equal(requests(operationSocket, "turn/interrupt").length, 1);

    operationSocket.receive({kind: "bridge.provider", state: "ready", providerGeneration: 2});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(operationSession.operationPending("thread.rename", "operation-thread"), false);
    assert.equal(operationSession.getSnapshot().notice, "");
    assert.equal(operationSession.getSnapshot().protocolFrames.some(frame => frame?.kind === "result"
        && ["thread.rename", "turn.interrupt"].includes(frame?.action)), false);
    operationSocket.receive(appserver({jsonrpc: "2.0", method: "thread/started", params: {
        thread: {id: "operation-thread", status: {type: "idle"}},
    }}));
    operationSession.renameThread("operation-thread", "Current generation name");
    const currentRename = requests(operationSocket, "thread/name/set").at(-1);
    assert.equal(requests(operationSocket, "thread/name/set").length, 2,
        "the exact operation key is reusable after its stale completion retires");
    respond(operationSocket, currentRename, {});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(operationSession.getSnapshot().protocolFrames.filter(frame =>
        frame?.kind === "result" && frame?.action === "thread.rename").length, 1);
    operationSession.dispose();
});

test("same-ID replacement rejects stale browser interactions and provider frames", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "exact-browser-lifetime");
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/started", params: {
        thread: {id: "holder", status: {type: "idle"}},
    }}));
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/started", params: {
        thread: {id: "reused", status: {type: "active"}, turns: [{
            id: "reused-turn", status: "inProgress", items: [],
        }]},
    }}));
    const firstPresentation = session.threadVisualKey("reused");
    session.selectThread("reused", firstPresentation);
    session.interrupt(firstPresentation);
    const staleInterrupt = requests(socket, "turn/interrupt").at(-1);
    assert.ok(staleInterrupt);
    session.selectThread("holder");

    socket.receive(appserver({jsonrpc: "2.0", method: "thread/deleted", params: {threadId: "reused"}}));
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/started", params: {
        thread: {id: "reused", status: {type: "idle"}},
    }}));
    const replacement = session.model.thread("reused");
    const replacementPresentation = session.threadVisualKey("reused");
    assert.notEqual(replacementPresentation, firstPresentation);
    replacement.lastActivityAt = 17;
    respond(socket, staleInterrupt, {});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(replacement.lastActivityAt, 17,
        "an old interrupt callback cannot record activity against a reused ID");
    assert.equal(session.getSnapshot().protocolFrames.some(frame => frame?.kind === "result"
        && frame?.action === "turn.interrupt"), false);
    session.selectThread("reused", firstPresentation);
    assert.equal(session.getSnapshot().selectedThreadId, "holder");
    session.renameThread("reused", "Stale name", firstPresentation);
    assert.equal(requests(socket, "thread/name/set").length, 0);

    session.selectThread("reused", replacementPresentation);
    assert.equal(session.canSubmit(firstPresentation), false,
        "a stale mounted Composer cannot admit or clear its authored text");
    assert.equal(session.canSubmit(replacementPresentation), true);
    assert.equal(await session.submitPrompt("stale composer", [], {}, {}, firstPresentation), false);
    assert.equal(session.prompts.submissions("reused").length, 0);
    session.viewportState.updateScroll("reused", replacementPresentation, 91, false);
    session.viewportState.setCardCollapsed("reused", replacementPresentation, "card", true);
    const protocolCount = session.getSnapshot().protocolFrames.length;
    socket.receive({kind: "bridge.provider", state: "disconnected", providerGeneration: 0});
    assert.equal(session.model.thread("reused"), replacement,
        "a lower provider generation cannot retire current presentation state");
    assert.equal(session.threadVisualKey("reused"), replacementPresentation);
    assert.deepEqual(session.viewportState.scroll("reused", replacementPresentation), {
        scrollTop: 91, following: false,
    });
    assert.equal(session.viewportState.cardCollapsed("reused", replacementPresentation, "card", false), true);
    assert.equal(session.getSnapshot().protocolFrames.length, protocolCount,
        "a rejected stale frame is a complete semantic no-op");
    session.dispose();
});

test("a provider-generation transition invalidates prior thread hydration", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "generation-transition");
    const staleModels = requests(socket, "model/list").at(-1);
    const staleProfiles = requests(socket, "permissionProfile/list").at(-1);
    respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "same-thread", status: {type: "idle"}}]});
    session.selectThread("same-thread");
    await completeControllerHydration(socket,
        {id: "same-thread", status: {type: "idle"}, turns: []});
    const resumesBefore = requests(socket, "thread/resume").length;

    socket.receive({kind: "bridge.provider", state: "ready", providerGeneration: 2});
    await Promise.resolve();
    const currentModels = requests(socket, "model/list").at(-1);
    const currentProfiles = requests(socket, "permissionProfile/list").at(-1);
    respond(socket, staleModels, {data: [{id: "stale-model"}]});
    respond(socket, staleProfiles, {data: [{id: "stale-profile"}]});
    respond(socket, currentModels, {data: [{id: "current-model"}]});
    respond(socket, currentProfiles, {data: [{id: "current-profile"}]});
    await Promise.resolve(); await Promise.resolve();
    assert.deepEqual(session.model.turnSettingsCatalogs(), {
        models: [{id: "current-model"}], permissionProfiles: {data: [{id: "current-profile"}]},
    });
    assert.doesNotMatch(JSON.stringify(session.getSnapshot().protocolFrames), /stale-(?:model|profile)/u,
        "stale catalog results never enter retained diagnostics");
    assert.equal(requests(socket, "thread/list").length, 2);
    const currentResume = requests(socket, "thread/resume").at(-1);
    assert.equal(requests(socket, "thread/resume").length, resumesBefore + 1);
    assert.equal(await session.submitPrompt("new generation"), true);
    await Promise.resolve();
    assert.equal(requests(socket, "turn/start").length, 0);
    respond(socket, currentResume, {thread: {id: "same-thread", status: {type: "idle"}}});
    await Promise.resolve(); await Promise.resolve();
    respond(socket, requests(socket, "thread/turns/list").at(-1), {data: [], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(requests(socket, "turn/start").length, 1);
    session.dispose();
});

test("a rejected turn is terminal and never resubmitted automatically", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "recover");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "recover-thread", status: {type: "idle"}}]});
    session.selectThread("recover-thread");
    await completeControllerHydration(socket,
        {id: "recover-thread", status: {type: "idle"}, turns: []});
    await session.submitPrompt("do not resend"); await Promise.resolve();
    const first = requests(socket, "turn/start").at(-1);
    reject(socket, first, "thread not found");
    await Promise.resolve(); await Promise.resolve();
    assert.equal(requests(socket, "turn/start").length, 1);
    assert.equal(requests(socket, "thread/resume").length, 1);
    assert.equal(session.prompts.submissions("recover-thread").at(-1).state, "failed");
    session.dispose();
});
