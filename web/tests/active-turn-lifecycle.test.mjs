import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import {PresentationModel, ProtocolNormalizer, statusToken} from "../dist/index.js";
import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {FakeSocket, appserver, requests, respond, readyProvider, completeControllerHydration} from "./browser-harness.mjs";

const fixture = JSON.parse(readFileSync(new URL("../../tests/fixtures/active-turn-lifecycle.json", import.meta.url), "utf8"));
const actions = {"thread/resume": "thread.resume", "thread/read": "thread.read", "thread/turns/list": "thread.turns.list"};

for (const scenario of fixture.cases) test(`current-turn authority: ${scenario.id}`, () => {
    const model = new PresentationModel();
    const normalizer = new ProtocolNormalizer(frame => { model.applyEvent(frame); });
    const pending = new Map();
    for (const name of scenario.steps) {
        const step = fixture.actions[name];
        if (step.kind === "notification") {
            normalizer.serverNotification(step.method, step.payload);
        } else if (step.kind === "request") {
            pending.set(step.id, {context: step.payload, sequence: normalizer.sequence});
        } else {
            const request = pending.get(step.id);
            normalizer.operationResult(actions[step.method], step.id, request.context,
                {id: step.id, result: step.payload}, request.sequence);
        }
    }
    assert.equal(model.activeTurnId(fixture.threadId) ?? null, scenario.activeTurnId);
    if (scenario.status !== undefined) {
        assert.equal(statusToken(model.thread(fixture.threadId).status), scenario.status === "idle" ? "completed" : "running");
    }
});

test("reloading completed history admits the next prompt as a new turn", async () => {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "lifecycle");
    try {
        respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "lifecycle", status: {type: "idle"}}]});
        session.selectThread("lifecycle");
        await completeControllerHydration(socket, {...fixture.actions.seed.payload.thread, turns: [
            {id: "old", status: "inProgress"}, {id: "latest", status: "completed"},
        ]});
        socket.receive(appserver({method: "thread/status/changed", params: fixture.actions.idle.payload}));
        session.reloadThread("lifecycle");
        respond(socket, requests(socket, "thread/resume").at(-1), fixture.actions["resume-idle"].payload);
        for (let pass = 0; pass < 4; ++pass) { await Promise.resolve(); }
        const page = requests(socket, "thread/turns/list").at(-1);
        if (!socket.replied?.has(page.payload.id)) {
            respond(socket, page, fixture.actions["completed-page"].payload);
        }
        for (let pass = 0; pass < 4; ++pass) { await Promise.resolve(); }
        assert.equal(session.conversation().activeTurnId, undefined);
        assert.equal(await session.submitPrompt("Next task"), true);
        assert.equal(requests(socket, "turn/start").at(-1)?.payload.params.threadId, "lifecycle");
        assert.equal(requests(socket, "turn/steer").length, 0);
    } finally {
        session.dispose();
    }
});
