import assert from "node:assert/strict";
import test from "node:test";

import {BrowserFrontendSession, suggestForkName} from "../dist/app/BrowserFrontendSession.js";
import {cardKeys, result, stableKey} from "../dist/index.js";

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
async function readyProvider(socket, connectionId, role = "controller", providerGeneration = 1) {
    socket.receive({kind: "bridge.connection", event: "opened", connectionId, role});
    socket.receive({kind: "bridge.provider", state: "ready", providerGeneration});
    await Promise.resolve();
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
    const readA = requests(socket, "thread/read").at(-1);
    assert.equal(session.getSnapshot().selectedThreadLoading, true);
    session.selectThread("thread-b");
    const readB = requests(socket, "thread/read").at(-1);
    assert.equal(session.getSnapshot().selectedThreadLoading, true);

    respond(socket, readA, {thread: {id: "thread-a", turns: []}});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(session.getSnapshot().selectedThreadId, "thread-b");
    assert.equal(session.getSnapshot().selectedThreadLoading, true,
        "a superseded hydration cannot complete the visible loading state");

    respond(socket, readB, {thread: {id: "thread-b", turns: []}});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(session.getSnapshot().selectedThreadLoading, false);
    session.dispose();
});

test("user thread operations are single-flight and report failures", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "operation-feedback");

    session.renameThread("thread-1", "Renamed");
    session.renameThread("thread-1", "Duplicate");
    const rename = requests(socket, "thread/name/set");
    assert.equal(rename.length, 1);
    assert.equal(session.operationPending("thread.rename", "thread-1"), true);
    reject(socket, rename[0], "rename denied");
    await Promise.resolve(); await Promise.resolve();
    assert.equal(session.operationPending("thread.rename", "thread-1"), false);
    assert.match(session.getSnapshot().notice, /Rename thread failed: rename denied/u);

    respond(socket, requests(socket, "thread/list").at(-1), {data: [], nextCursor: null});
    await Promise.resolve(); await Promise.resolve();
    const listsBefore = requests(socket, "thread/list").length;
    session.requestThreads(); session.requestThreads();
    assert.equal(requests(socket, "thread/list").length, listsBefore + 1);
    assert.equal(session.operationPending("threads.refresh"), true);
    const refresh = requests(socket, "thread/list").at(-1);
    reject(socket, refresh, "refresh denied");
    await Promise.resolve(); await Promise.resolve();
    assert.equal(session.operationPending("threads.refresh"), false);
    assert.match(session.getSnapshot().notice, /Refresh threads failed: refresh denied/u);
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
    assert.deepEqual(fork.payload.params, {threadId: "fork-source"},
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

test("browser session uses the C++ action routing and preserves prompt-response order", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect();
    socket.open();
    await readyProvider(socket, "web-test");

    const threadList = requests(socket, "thread/list").at(-1);
    const modelList = requests(socket, "model/list").at(-1);
    const profiles = requests(socket, "permissionProfile/list").at(-1);
    assert.ok(threadList && modelList && profiles, "native catalog actions map to current protocol methods");
    respond(socket, threadList, {data: [{id: "thread-1", preview: "Browser parity", cwd: "/workspace", status: {type: "idle"}}]});
    respond(socket, modelList, {data: [{id: "gpt-current", displayName: "Current"}]});
    respond(socket, profiles, {data: []});
    session.selectThread("thread-1");
    const read = requests(socket, "thread/read").at(-1);
    assert.deepEqual(read.payload.params, {threadId: "thread-1", includeTurns: true});
    respond(socket, read, {thread: {id: "thread-1", preview: "Browser parity", cwd: "/workspace", status: {type: "idle"}, turns: []}});

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
    respond(socket, requests(socket, "thread/read").at(-1),
        {thread: {id: "handoff-thread", status: {type: "idle"}, turns: []}});
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
    respond(socket, requests(socket, "thread/read").at(-1), {thread: {
        id: "thread-1", turns: [{id: "turn-1", status: "inProgress", items: [{
            id: "command-1", type: "commandExecution", status: "inProgress", command: "printf output",
        }]}],
    }});
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

test("new threads retain one optimistic row through first-turn acknowledgment", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open();
    await readyProvider(socket, "new-thread");

    session.beginNewThread({
        workspace: "/workspace", name: "Named draft", baseInstructions: "Base context",
        developerInstructions: "Developer context", ephemeral: true,
    });
    const draft = session.getSnapshot().optimisticThreads[0];
    assert.equal(draft?.id, "__codexui_new_thread__");
    assert.equal(draft?.state, "awaiting");
    assert.equal(draft?.title, "Named draft");
    assert.equal(draft?.cwd, "/workspace");

    const submitted = session.submitPrompt("first prompt", [], {}, {model: "gpt-current"});
    const create = requests(socket, "thread/start").at(-1);
    assert.ok(create);
    assert.deepEqual(create.payload.params, {
        model: "gpt-current", cwd: "/workspace", baseInstructions: "Base context",
        developerInstructions: "Developer context", ephemeral: true,
    });
    assert.equal(await session.submitPrompt("queued during creation"), true);
    session.beginNewThread();
    assert.equal(session.getSnapshot().optimisticThreads[0]?.visualKey, draft?.visualKey);
    assert.equal(requests(socket, "thread/start").length, 1,
        "additional prompts and New Thread cannot duplicate an in-flight creation");
    respond(socket, create, {thread: {id: "created-thread", name: "Created thread", cwd: "/workspace", status: {type: "idle"}}});
    assert.equal(await submitted, true);
    await Promise.resolve();
    assert.equal(session.getSnapshot().optimisticThreads[0]?.title, "Named draft",
        "the canonical thread handoff never substitutes the provider name or UUID");
    assert.equal(session.model.thread("created-thread")?.title, "Named draft");
    const rename = requests(socket, "thread/name/set").at(-1);
    assert.equal(rename?.payload.params.name, "Named draft");
    respond(socket, rename, {});

    const promoted = session.getSnapshot().optimisticThreads[0];
    assert.equal(promoted?.id, "created-thread");
    assert.equal(promoted?.visualKey, draft?.visualKey);
    assert.equal(promoted?.state, "awaiting");
    assert.equal(session.threadVisualKey("created-thread"), draft?.visualKey);

    const start = requests(socket, "turn/start").at(-1);
    assert.ok(start);
    respond(socket, start, {turn: {id: "created-turn", status: "inProgress"}});
    await Promise.resolve(); await waitForPublish();
    assert.notEqual(session.getSnapshot().optimisticThreads[0]?.state, "awaiting");
    assert.equal(session.threadVisualKey("created-thread"), draft?.visualKey,
        "canonical styling retains the optimistic row's React identity");
    assert.equal(session.model.thread("created-thread")?.title, "Named draft",
        "turn acknowledgement leaves the chosen name intact");
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/name/updated", params: {
        threadId: "created-thread", threadName: "Named draft",
    }}));
    assert.equal(session.model.thread("created-thread")?.localNameOverlay, undefined);
    assert.equal(session.model.thread("created-thread")?.title, "Named draft",
        "matching name acknowledgement retires the overlay without changing the title");
    session.dispose();
});

test("new-thread completion preserves later navigation and explicit drafts start clean", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open();
    await readyProvider(socket, "new-thread-navigation");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "other-thread"}]});

    session.beginNewThread();
    const firstDraftRevision = session.getSnapshot().newThreadDraftRevision;
    const submitted = session.submitPrompt("background creation");
    const create = requests(socket, "thread/start").at(-1);
    session.selectThread("other-thread");
    respond(socket, create, {thread: {id: "background-thread", status: {type: "idle"}}});
    assert.equal(await submitted, true);
    await Promise.resolve();
    assert.equal(session.getSnapshot().selectedThreadId, "other-thread",
        "creation by a background draft cannot replace the user's later selection");
    assert.equal(requests(socket, "turn/start").at(-1).payload.params.threadId, "background-thread");

    session.beginNewThread();
    assert.equal(session.prompts.submissions("__codexui_new_thread__").length, 0);
    assert.equal(session.getSnapshot().newThreadDraftRevision, firstDraftRevision + 1,
        "explicit New thread establishes a clean shared composer generation");
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

test("Fork with options sends adjustable fork fields and names separately", async () => {
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
        threadId: "source", cwd: "/new", baseInstructions: "Base",
        developerInstructions: "Developer", ephemeral: true,
    });
    respond(socket, fork, {thread: {id: "advanced", name: "Provider name", cwd: "/new"}});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(session.model.thread("advanced")?.title, "Chosen fork");
    assert.deepEqual(requests(socket, "thread/name/set").at(-1).payload.params,
        {threadId: "advanced", name: "Chosen fork"});
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
        respond(socket, requests(socket, "thread/read").at(-1), {thread: {
            id: "older", recencyAt: 10, updatedAt: 12, status: {type: "idle"}, turns: [],
        }});
        await Promise.resolve(); await Promise.resolve();
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
        assert.equal(session.model.thread("older").updatedAt, 12,
            "turn ordering never rewrites last-changed data");

        session.selectThread("recent");
        respond(socket, requests(socket, "thread/read").at(-1), {thread: {
            id: "recent", recencyAt: 30, updatedAt: 32, status: {type: "idle"}, turns: [],
        }});
        await Promise.resolve(); await Promise.resolve();
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
        "selection-driven read does not replace authoritative activity");

    const read = requests(socket, "thread/read").at(-1);
    respond(socket, read, {thread: {id: "tracked", updatedAt: 20, recencyAt: 30, turns: []}});
    assert.equal(session.model.thread("tracked").lastActivityAt, 30,
        "authoritative read response remains the activity source during hydration");

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
    assert.equal(session.model.thread("tracked").updatedAt, 20);
    assert.equal(session.model.thread("tracked").recencyAt, 30,
        "non-prompt traffic does not reorder Recent or rewrite provider timestamps");

    session.model.thread("tracked").lastActivityAt = 1;
    const beforeInbound = Math.floor(Date.now() / 1000);
    socket.receive(appserver({jsonrpc: "2.0", method: "thread/status/changed", params: {
        threadId: "tracked", status: {type: "idle"},
    }}));
    assert.ok(session.model.thread("tracked").lastActivityAt >= beforeInbound,
        "thread-scoped app-server frames advance local activity");
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
    assert.equal(session.model.connection().role, "controller");
    session.dispose();
});

test("a pre-open WebSocket failure can retry after completed detach", async () => {
    const sockets = [];
    const session = new BrowserFrontendSession("ws://bridge.test/", () => {
        const socket = new DelayedCloseSocket(); sockets.push(socket); return socket;
    });
    session.connect();
    sockets[0].onerror?.();
    assert.equal(session.model.connection().detail, "bridge WebSocket transport failed");
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
    respond(socket, requests(socket, "thread/read").at(-1), {thread: {id: "sleeping", status: {type: "notLoaded"}, turns: []}});
    await session.submitPrompt("wake and work");
    await Promise.resolve();
    const resume = requests(socket, "thread/resume").at(-1);
    assert.ok(resume);
    assert.equal(requests(socket, "turn/start").length, 0);
    respond(socket, resume, {thread: {id: "sleeping", status: {type: "idle"}}});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(requests(socket, "turn/start").at(-1).payload.params.input[0].text, "wake and work");
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
    assert.equal(session.resolvePending(request, {result: {decision: "accept"}}), true);
    assert.equal(session.resolvePending(request, {result: {decision: "accept"}}), false);
    const responses = socket.sent.filter(message => message.kind === "appserver" && message.payload.id === 77
        && !Object.hasOwn(message.payload, "method"));
    assert.equal(responses.length, 1, "a repeated action cannot emit a duplicate JSON-RPC response");
    assert.equal(session.isPendingResolving(request), true);

    socket.receive(appserver({jsonrpc: "2.0", method: "serverRequest/resolved", params: {
        requestId: 77, threadId: "thread-request",
    }}));
    assert.equal(session.model.pendingRequestCount(), 0);
    assert.equal(session.isPendingResolving(request), false);

    socket.receive(appserver({jsonrpc: "2.0", id: 78, method: "item/commandExecution/requestApproval", params: {
        threadId: "thread-request", turnId: "turn-request", itemId: "item-request-2", command: "echo guarded",
    }}));
    const guarded = [...session.model.pendingRequestPresentations().values()][0];
    assert.ok(guarded);
    socket.receive({kind: "bridge.controller", controllerConnectionId: "another-client"});
    assert.equal(session.canResolvePending(guarded), false);
    assert.equal(session.resolvePending(guarded, {result: {decision: "accept"}}), false);
    socket.receive({kind: "bridge.controller", controllerConnectionId: "request-controller"});
    assert.equal(session.canResolvePending(guarded), true);
    session.dispose();
});

test("a prompt admitted during thread hydration waits for the authoritative read", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "hydrate-before-send");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "thread-h", status: {type: "idle"}}]});
    session.selectThread("thread-h");
    const read = requests(socket, "thread/read").at(-1);
    assert.ok(read);
    assert.equal(await session.submitPrompt("wait for read"), true);
    await Promise.resolve();
    assert.equal(requests(socket, "turn/start").length, 0);

    respond(socket, read, {thread: {id: "thread-h", status: {type: "idle"}, turns: []}});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(requests(socket, "turn/start").at(-1).payload.params.input[0].text, "wait for read");
    session.dispose();
});

test("a queued prompt waits through controller loss and resumes after control returns", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "role-transition");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "role-thread", status: {type: "idle"}}]});
    session.selectThread("role-thread");
    const read = requests(socket, "thread/read").at(-1);
    assert.equal(await session.submitPrompt("wait for control"), true);
    socket.receive({kind: "bridge.controller", controllerConnectionId: "other-client"});
    respond(socket, read, {thread: {id: "role-thread", status: {type: "idle"}, turns: []}});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(requests(socket, "turn/start").length, 0);

    socket.receive({kind: "bridge.controller", controllerConnectionId: "role-transition"});
    await Promise.resolve();
    assert.equal(requests(socket, "turn/start").at(-1).payload.params.input[0].text, "wait for control");
    session.dispose();
});

test("provider loss preserves a queued hydration-gated prompt for the next generation", async () => {
    const sockets = [];
    const session = new BrowserFrontendSession("ws://bridge.test/", () => {
        const socket = new FakeSocket(); sockets.push(socket); return socket;
    });
    session.connect(); sockets[0].open(); await readyProvider(sockets[0], "generation-a");
    respond(sockets[0], requests(sockets[0], "thread/list").at(-1), {data: [{id: "retained", status: {type: "idle"}}]});
    session.selectThread("retained");
    const staleRead = requests(sockets[0], "thread/read").at(-1);
    assert.equal(await session.submitPrompt("survive restart"), true);
    sockets[0].receive({kind: "bridge.provider", state: "disconnected", providerGeneration: 1, reason: "restart"});
    sockets[0].close(1000, "restart");
    respond(sockets[0], staleRead, {thread: {id: "retained", status: {type: "idle"}, turns: []}});

    session.reconnect(); await Promise.resolve();
    sockets[1].open(); await readyProvider(sockets[1], "generation-b", "controller", 2);
    respond(sockets[1], requests(sockets[1], "thread/list").at(-1), {data: [{id: "retained", status: {type: "idle"}}]});
    const currentRead = requests(sockets[1], "thread/read").at(-1);
    respond(sockets[1], currentRead, {thread: {id: "retained", status: {type: "idle"}, turns: []}});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(requests(sockets[0], "turn/start").length, 0);
    assert.equal(requests(sockets[1], "turn/start").at(-1).payload.params.input[0].text, "survive restart");
    session.dispose();
});

test("a provider-generation transition invalidates prior thread hydration", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "generation-transition");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "same-thread", status: {type: "idle"}}]});
    session.selectThread("same-thread");
    respond(socket, requests(socket, "thread/read").at(-1), {thread: {id: "same-thread", status: {type: "idle"}, turns: []}});
    await Promise.resolve();
    const readsBefore = requests(socket, "thread/read").length;

    socket.receive({kind: "bridge.provider", state: "ready", providerGeneration: 2});
    await Promise.resolve();
    assert.equal(requests(socket, "thread/list").length, 2);
    const currentRead = requests(socket, "thread/read").at(-1);
    assert.equal(requests(socket, "thread/read").length, readsBefore + 1);
    assert.equal(await session.submitPrompt("new generation"), true);
    await Promise.resolve();
    assert.equal(requests(socket, "turn/start").length, 0);
    respond(socket, currentRead, {thread: {id: "same-thread", status: {type: "idle"}, turns: []}});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(requests(socket, "turn/start").length, 1);
    session.dispose();
});

test("thread-not-found turn submission performs exactly one resume recovery", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "recover");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "recover-thread", status: {type: "idle"}}]});
    session.selectThread("recover-thread");
    respond(socket, requests(socket, "thread/read").at(-1), {thread: {id: "recover-thread", status: {type: "idle"}, turns: []}});
    await Promise.resolve();
    await session.submitPrompt("recover once"); await Promise.resolve();
    const first = requests(socket, "turn/start").at(-1);
    reject(socket, first, "thread not found");
    await Promise.resolve();
    const resume = requests(socket, "thread/resume").at(-1);
    assert.ok(resume);
    respond(socket, resume, {thread: {id: "recover-thread", status: {type: "idle"}}});
    await Promise.resolve(); await Promise.resolve();
    assert.equal(requests(socket, "turn/start").length, 2);
    const second = requests(socket, "turn/start").at(-1);
    reject(socket, second, "thread not found");
    await Promise.resolve();
    assert.equal(requests(socket, "thread/resume").length, 1);
    assert.equal(session.prompts.submissions("recover-thread").at(-1).state, "failed");
    session.dispose();
});
