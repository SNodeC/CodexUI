import assert from "node:assert/strict";
import test from "node:test";
import {readFileSync} from "node:fs";
import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {FakeSocket, appserver, requests, respond, reject, readyProvider, completeThreadDiscovery, completeControllerHydration} from "./browser-harness.mjs";

const fixture = JSON.parse(readFileSync(new URL("../../tests/fixtures/thread-subscriptions.json", import.meta.url)));
const settle = async () => { for (let pass = 0; pass < 12; ++pass) { await Promise.resolve(); } };
const closeThread = socket => socket.receive(appserver({method: "thread/closed", params: {threadId: "source"}}));

async function setup(materialized = true, holdMetadata = false) {
    const socket = new FakeSocket();
    const send = socket.send.bind(socket);
    socket.send = data => {
        send(data);
        const envelope = JSON.parse(data);
        if (!holdMetadata && envelope.kind === "appserver" && envelope.payload.method === "fs/getMetadata") {
            queueMicrotask(() => respond(socket, envelope, {isFile: materialized}));
        }
    };
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "subscription-test");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [
        {id: "source", cwd: "/tmp", path: "/saved/source", status: {type: "idle"}},
        {id: "other", cwd: "/tmp", path: "/saved/other", status: {type: "notLoaded"}},
    ]});
    await completeThreadDiscovery(socket);
    session.selectThread("source");
    await completeControllerHydration(socket, {id: "source", cwd: "/tmp", path: "/saved/source", status: {type: "idle"},
        turns: [{id: "retained", status: "completed", items: []}]});
    const items = requests(socket, "thread/items/list").at(-1);
    if (items) {
        respond(socket, items, {data: [{id: "message", type: "agentMessage", text: "Retained history"}], nextCursor: null});
    }
    await settle();
    return {session, socket, resumes: requests(socket, "thread/resume").length};
}

for (const scenario of fixture.cases) {
    test(`thread subscriptions: ${scenario.name}`, async () => {
        const {session, socket, resumes} = await setup(scenario.failure !== "persistence");
        const source = session.model.thread("source");
        const turn = source.turns.get("retained");
        try {
            session.resumeThread("source", scenario.options, undefined, true); await settle();
            const unsubscribe = requests(socket, "thread/unsubscribe").at(-1);
            assert.equal(!!unsubscribe, scenario.failure !== "persistence");
            assert.equal(requests(socket, "thread/resume").length, resumes);
            if (scenario.closeBeforeAck) {
                closeThread(socket);
                assert.equal(requests(socket, "thread/resume").length, resumes, "wait for unsubscribe acknowledgement too");
            }
            if (scenario.cancelBeforeAck) {
                session.unsubscribeThread("source");
            }
            if (scenario.failure === "persistence") {
                assert.equal(session.threadSubscriptionPhase("source"), "");
            } else if (scenario.failure === "unsubscribe") {
                reject(socket, unsubscribe, "unsubscribe rejected");
            } else {
                respond(socket, unsubscribe, {status: scenario.status});
            }
            await settle();
            if (scenario.cancelAfterAck) {
                session.unsubscribeThread("source");
            }
            if (!scenario.closeBeforeAck && scenario.status !== "notLoaded" && scenario.failure !== "unsubscribe" && scenario.failure !== "persistence") {
                assert.equal(requests(socket, "thread/resume").length, resumes);
                closeThread(socket);
                await settle();
            }
            const shouldResume = !scenario.cancelBeforeAck && !scenario.cancelAfterAck && scenario.failure !== "unsubscribe" && scenario.failure !== "persistence";
            assert.equal(requests(socket, "thread/resume").length, resumes + Number(shouldResume));
            if (shouldResume) {
                const resume = requests(socket, "thread/resume").at(-1);
                assert.deepEqual(resume.payload.params, {...scenario.options, threadId: "source", excludeTurns: true});
                if (scenario.failure === "resume") {
                    reject(socket, resume, "resume rejected");
                } else {
                    respond(socket, resume, {thread: {id: "source", status: {type: "idle"}}});
                }
                await settle();
                assert.equal(session.threadSubscriptionPhase("source"), scenario.failure ? "paused" : "");
            }
            assert.equal(session.model.thread("source"), source);
            assert.equal(source.turns.get("retained"), turn);
            assert.equal(session.getSnapshot().selectedThreadId, "source");
            assert.equal(requests(socket, "thread/unsubscribe").length, scenario.failure === "persistence" ? 0 : 1, "cancellation does not repeat the mutation");
            if (scenario.failure) {
                assert.match(session.getSnapshot().notice, /rejected|not materialized/u);
                assert.deepEqual(session.resumeOptions("source"), scenario.options, "failed replacement text remains available for retry");
            }
        } finally { session.dispose(); }
    });
}

for (const retirement of ["control", "deleted", "disconnect"]) {
    test(`persistence check cannot unsubscribe after ${retirement}`, async () => {
        const {session, socket} = await setup(true, true);
        try {
            session.resumeThread("source", {developerInstructions: "Retired replacement"}, undefined, true);
            const metadata = requests(socket, "fs/getMetadata").at(-1);
            assert.equal(metadata.payload.params.path, "/saved/source");
            if (retirement === "control") {
                socket.receive({kind: "bridge.controller", connectionId: "another-client", role: "observer"});
                socket.receive({kind: "bridge.controller", connectionId: "subscription-test", role: "controller"});
                session.resumeThread("source", {}, undefined, true);
                assert.equal(requests(socket, "fs/getMetadata").length, 1, "the existing operation owns its request until retirement");
            } else if (retirement === "deleted") {
                socket.receive(appserver({method: "thread/deleted", params: {threadId: "source"}}));
            } else {
                session.disconnect();
            }
            respond(socket, metadata, {isFile: true}); await settle();
            assert.equal(requests(socket, "thread/unsubscribe").length, 0);
        } finally { session.dispose(); }
    });
}

test("plain unsubscribe retains history and explicit resume restores subscription", async () => {
    const {session, socket, resumes} = await setup();
    try {
        const source = session.model.thread("source");
        session.unsubscribeThread("source"); await settle();
        respond(socket, requests(socket, "thread/unsubscribe").at(-1), {status: "unsubscribed"});
        await settle();
        assert.notEqual(source.status.semantic, "notLoaded", "unsubscribe acknowledgement does not invent unloaded status");
        session.selectThread("source"); await settle();
        assert.equal(requests(socket, "thread/resume").length, resumes, "selection does not undo unsubscribe");
        closeThread(socket); await settle();
        assert.equal(source.status.semantic, "notLoaded");
        assert.ok(source.turns.get("retained"));
        session.resumeThread("source");
        const resume = requests(socket, "thread/resume").at(-1);
        assert.deepEqual(resume.payload.params, {threadId: "source", excludeTurns: true});
        respond(socket, resume, {thread: {id: "source", status: {type: "idle"}}}); await settle();
        assert.equal(session.threadSubscriptionPhase("source"), "");
    } finally { session.dispose(); }
});

for (const retirement of ["control", "deleted", "disconnect"]) {
    test(`waiting resume is retired on ${retirement}`, async () => {
        const {session, socket, resumes} = await setup();
        try {
            session.resumeThread("source", {developerInstructions: "Never send after retirement"}, undefined, true); await settle();
            respond(socket, requests(socket, "thread/unsubscribe").at(-1), {status: "unsubscribed"}); await settle();
            session.resumeThread("other");
            const other = requests(socket, "thread/resume").at(-1);
            assert.equal(other.payload.params.threadId, "other", "independent threads remain usable while waiting");
            respond(socket, other, {thread: {id: "other", status: {type: "idle"}}}); await settle();
            if (retirement === "control") {
                socket.receive({kind: "bridge.controller", connectionId: "another-client", role: "observer"});
            } else if (retirement === "deleted") {
                socket.receive(appserver({method: "thread/deleted", params: {threadId: "source"}}));
            } else {
                session.disconnect();
            }
            closeThread(socket); await settle();
            assert.equal(requests(socket, "thread/resume").length, resumes + 1);
        } finally { session.dispose(); }
    });
}

test("resume options are rejected for a loaded, active, temporary, unpersisted, or stale target", async () => {
    const {session, socket, resumes} = await setup();
    try {
        const source = session.model.thread("source");
        session.resumeThread("source", {developerInstructions: "Loaded override"});
        assert.equal(requests(socket, "thread/resume").length, resumes);
        const saved = {...source.raw};
        for (const fields of [{ephemeral: true}, {path: null}, {status: {type: "active"}}]) {
            source.raw = {...saved, ...fields};
            if (fields.status) {
                socket.receive(appserver({method: "thread/status/changed", params: {threadId: "source", status: fields.status}}));
            }
            session.resumeThread("source", {developerInstructions: "Rejected"}, undefined, true);
            assert.equal(requests(socket, "thread/unsubscribe").length, 0);
        }
        source.raw = saved;
        session.resumeThread("source", {}, "obsolete-presentation", true);
        assert.equal(requests(socket, "thread/unsubscribe").length, 0);
    } finally { session.dispose(); }
});

for (const unloaded of [false, true]) {
    test(`sending explicitly resumes an unsubscribed thread, unloaded=${unloaded}`, async () => {
        const {session, socket, resumes} = await setup();
        try {
            session.unsubscribeThread("source"); await settle();
            respond(socket, requests(socket, "thread/unsubscribe").at(-1), {status: "unsubscribed"}); await settle();
            if (unloaded) { closeThread(socket); }
            await session.submitPrompt("Explicit new work"); await settle();
            const resume = requests(socket, "thread/resume").at(-1);
            assert.equal(requests(socket, "thread/resume").length, resumes + 1);
            assert.equal(requests(socket, "turn/start").length, 0, "sending first restores the event subscription");
            respond(socket, resume, {thread: {id: "source", status: {type: "idle"}}}); await settle();
            assert.equal(requests(socket, "turn/start").at(-1).payload.params.input[0].text, "Explicit new work");
            assert.equal(session.threadSubscriptionPhase("source"), "");
        } finally { session.dispose(); }
    });
}

test("an unloaded persisted thread can resume by ID without a rollout path", async () => {
    const {session, socket} = await setup();
    try {
        session.model.thread("other").raw.path = null;
        session.resumeThread("other");
        const request = requests(socket, "thread/resume").at(-1);
        assert.equal(request.payload.params.threadId, "other");
        assert.deepEqual(request.payload.params, {threadId: "other", excludeTurns: true});
    } finally { session.dispose(); }
});
