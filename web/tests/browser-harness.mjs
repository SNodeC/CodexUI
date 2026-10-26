import assert from "node:assert/strict";

export class FakeSocket {
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

export class DelayedCloseSocket extends FakeSocket {
    closeReason = "";
    close(_code, reason) { this.readyState = 2; this.closeReason = reason; }
    finishClose() { this.readyState = 3; this.onclose?.({reason: this.closeReason}); }
}

export function appserver(payload) { return {kind: "appserver", payload}; }
export function requests(socket, method) {
    return socket.sent.filter(message => message.kind === "appserver" && message.payload.method === method);
}
export function respond(socket, request, result) {
    socket.replied ??= new Set(); socket.replied.add(request.payload.id);
    if (request.payload.method === "thread/list") {
        socket.lineageFixtures ??= new Map();
        for (const thread of result.data ?? []) socket.lineageFixtures.set(thread.id, thread);
    }
    socket.receive(appserver({jsonrpc: "2.0", id: request.payload.id, result}));
}
export function reject(socket, request, message) {
    socket.receive(appserver({jsonrpc: "2.0", id: request.payload.id, error: {code: -32000, message}}));
}
export function rejectWithCode(socket, request, code, message) {
    socket.receive(appserver({jsonrpc: "2.0", id: request.payload.id, error: {code, message}}));
}
export async function readyProvider(socket, connectionId, role = "controller", providerGeneration = 1) {
    socket.receive({kind: "bridge.connection", event: "opened", connectionId, role});
    socket.receive({kind: "bridge.provider", state: "ready", providerGeneration});
    await Promise.resolve();
}
export async function completeControllerHydration(socket, thread, nextCursor = null) {
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
export const waitForPublish = () => new Promise(resolve => setTimeout(resolve, 25));

export async function completeThreadDiscovery(socket) {
    let idle = 0;
    for (let pass = 0; pass < 256 && idle < 4; ++pass) {
        await Promise.resolve();
        const request = requests(socket, "thread/read").find(value => value.payload.params.includeTurns === false
            && !socket.replied?.has(value.payload.id));
        if (request) {
            const thread = socket.lineageFixtures?.get(request.payload.params.threadId);
            assert.ok(thread, "lineage discovery reads an already-listed thread");
            respond(socket, request, {thread: {...thread, forkedFromId: thread.forkedFromId ?? null,
                parentThreadId: thread.parentThreadId ?? null}});
            idle = 0;
        } else { ++idle; }
    }
}
