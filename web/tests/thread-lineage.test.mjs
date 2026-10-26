import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {FakeSocket, appserver, requests, respond, reject, readyProvider, waitForPublish} from "./browser-harness.mjs";

const fixture = JSON.parse(readFileSync(new URL("../../tests/fixtures/thread-lineage.json", import.meta.url), "utf8"));
const settle = async () => { for (let pass = 0; pass < 4; ++pass) { await Promise.resolve(); } };
const pending = (socket, method) => requests(socket, method).filter(request => !socket.replied?.has(request.payload.id));
async function connect() {
    const socket = new FakeSocket(), session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "lineage");
    for (let pass = 0; pass < 4; ++pass) {
        for (const method of ["project/list", "threadSection/list"]) {
            for (const request of pending(socket, method)) { respond(socket, request, {data: []}); }
        }
        await settle();
    }
    return {socket, session};
}

test("initial pages resolve the shared real-backend lineage fixture before publication", async () => {
    const {socket, session} = await connect();
    const published = []; const unsubscribe = session.subscribe(() => published.push([...session.model.threadOrder()]));
    try {
        respond(socket, pending(socket, "thread/list")[0], {data: fixture.listed, nextCursor: null});
        await settle();
        for (const metadata of fixture.metadata) {
            assert.deepEqual(session.model.threadOrder(), [], "metadata-only requests cannot expose a temporary flat family");
            const reads = pending(socket, "thread/read");
            assert.equal(reads.length, 1);
            assert.deepEqual(reads[0].payload.params, {threadId: metadata.id, includeTurns: false});
            respond(socket, reads[0], {thread: metadata});
            await settle();
        }
        assert.deepEqual(session.model.threadOrder(), fixture.roots);
        for (let index = 1; index < fixture.metadata.length; ++index) {
            assert.equal(session.model.childOwnership(fixture.metadata[index].id)?.parentThreadId, fixture.metadata[index - 1].id);
        }
        assert.equal(requests(socket, "thread/resume").length + requests(socket, "thread/turns/list").length, 0);
        assert.ok(published.every(roots => roots.length === 0 || roots.length === 1 && roots[0] === fixture.roots[0]));
        const count = requests(socket, "thread/read").length;
        session.refreshBrowser(); await settle();
        respond(socket, pending(socket, "thread/list")[0], {data: fixture.listed, nextCursor: null});
        await settle();
        assert.equal(requests(socket, "thread/read").length, count, "refresh reuses known lineage");
        assert.deepEqual(session.model.threadOrder(), fixture.roots);
        assert.ok(fixture.listed.every(thread => session.model.thread(thread.id).status.semantic === "notLoaded"));
    } finally { unsubscribe(); session.dispose(); }
});

test("overlapping list queries share discovery and keep the same two-request ceiling", async () => {
    const {socket, session} = await connect();
    try {
        respond(socket, pending(socket, "thread/list")[0], {data: fixture.listed, nextCursor: null}); await settle();
        session.browseGroup("project", "overlap"); await settle();
        const overlap = pending(socket, "thread/list")[0];
        respond(socket, overlap, {data: fixture.listed, nextCursor: null}); await settle();
        assert.equal(pending(socket, "thread/read").length, 1, "the same identity is not read concurrently");
        for (const metadata of fixture.metadata) {
            const read = pending(socket, "thread/read")[0];
            assert.equal(read.payload.params.threadId, metadata.id);
            respond(socket, read, {thread: metadata}); await settle();
            assert.ok(pending(socket, "thread/read").length <= 2);
        }
        assert.equal(requests(socket, "thread/read").length, fixture.metadata.length);
        assert.deepEqual(session.model.threadOrder(), fixture.roots);
        assert.ok(session.threadPages({projectId: "overlap"})[0].ids.length === 3);
    } finally { session.dispose(); }
});

test("metadata failures keep discovery retryable and superseded replies cannot publish stale families", async () => {
    const {socket, session} = await connect();
    try {
        respond(socket, pending(socket, "thread/list")[0], {data: [fixture.listed[0]], nextCursor: null}); await settle();
        const failed = pending(socket, "thread/read")[0];
        socket.replied.add(failed.payload.id); reject(socket, failed, "lineage unavailable"); await settle();
        assert.deepEqual(session.model.threadOrder(), []);
        assert.match(session.threadPages()[0].error, /lineage unavailable/u);
        session.loadPage(session.threadPages()[0]); await waitForPublish();
        respond(socket, pending(socket, "thread/list")[0], {data: [fixture.listed[0]], nextCursor: null}); await settle();
        const old = pending(socket, "thread/read")[0];
        session.changeBrowser({search: "replacement"}); await settle();
        respond(socket, old, {thread: fixture.metadata[0]}); await settle();
        assert.deepEqual(session.model.threadOrder(), [], "a retired page's metadata cannot admit its descriptors");
        respond(socket, pending(socket, "thread/list")[0], {data: [fixture.listed[0]], nextCursor: null}); await settle();
        respond(socket, pending(socket, "thread/read")[0], {thread: fixture.metadata[0]}); await settle();
        assert.deepEqual(session.model.threadOrder(), fixture.roots);
    } finally { session.dispose(); }
});


test("shared discovery retains the list revision boundary across newer lineage notifications", async () => {
    const {socket, session} = await connect();
    try {
        respond(socket, pending(socket, "thread/list")[0], {data: fixture.listed}); await settle();
        session.browseGroup("project", "overlap"); await settle();
        respond(socket, pending(socket, "thread/list")[0], {data: fixture.listed}); await settle();
        socket.receive(appserver({jsonrpc: "2.0", method: "thread/started", params: {thread: {
            id: "startup-fork", forkedFromId: "newer-origin",
        }}}));
        for (const metadata of fixture.metadata) {
            respond(socket, pending(socket, "thread/read")[0], {thread: metadata}); await settle();
        }
        assert.equal(session.model.thread("startup-fork").raw.forkedFromId, "newer-origin");
        assert.equal(session.model.childOwnership("startup-fork")?.parentThreadId, "newer-origin");
    } finally { session.dispose(); }
});
