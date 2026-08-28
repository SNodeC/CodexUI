import assert from "node:assert/strict";
import test from "node:test";

import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {cardKeys, stableKey} from "../dist/index.js";

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
}

function appserver(payload) { return {kind: "appserver", payload}; }
function requests(socket, method) {
    return socket.sent.filter(message => message.kind === "appserver" && message.payload.method === method);
}
function respond(socket, request, result) {
    socket.receive(appserver({jsonrpc: "2.0", id: request.payload.id, result}));
}
const waitForPublish = () => new Promise(resolve => setTimeout(resolve, 25));

test("browser session uses the C++ action routing and preserves prompt-response order", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect();
    socket.open();
    socket.receive({kind: "bridge.connection", event: "opened", connectionId: "web-test", role: "observer"});
    socket.receive({kind: "bridge.provider", state: "ready", providerGeneration: 1});

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

    assert.equal(await session.submitPrompt("new prompt"), true);
    await Promise.resolve();
    const start = requests(socket, "turn/start").at(-1);
    assert.ok(start);
    assert.equal(start.payload.params.threadId, "thread-1");
    assert.equal(start.payload.params.input[0].text, "new prompt");
    assert.match(start.payload.params.clientUserMessageId, /^codexui-/u);

    socket.receive(appserver({jsonrpc: "2.0", method: "turn/started", params: {
        threadId: "thread-1", turn: {id: "turn-1", status: "inProgress", items: []},
    }}));
    socket.receive(appserver({jsonrpc: "2.0", method: "item/started", params: {
        threadId: "thread-1", turnId: "turn-1", item: {id: "reasoning-1", type: "reasoning", summary: []},
    }}));
    socket.receive(appserver({jsonrpc: "2.0", method: "item/started", params: {
        threadId: "thread-1", turnId: "turn-1", item: {
            id: "user-1", type: "userMessage", clientId: start.payload.params.clientUserMessageId,
            content: [{type: "text", text: "new prompt"}],
        },
    }}));
    respond(socket, start, {turn: {id: "turn-1", status: "inProgress"}});
    await waitForPublish();

    const visible = cardKeys(session.getSnapshot().conversation).map(stableKey);
    assert.equal(visible.length, 2);
    assert.match(visible[0], /^prompt:/u);
    assert.equal(visible[1], stableKey({kind: "item", threadId: "thread-1", turnId: "turn-1", itemId: "reasoning-1"}));
    assert.equal(session.model.connection().connected, true);
    assert.equal(session.model.connection().providerState, "ready");
    session.dispose();
});
