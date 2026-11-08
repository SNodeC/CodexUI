import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import {createElement} from "react";
import {renderToStaticMarkup} from "react-dom/server";
import {App} from "../dist/app/App.js";
import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {ProtocolNormalizer} from "../dist/index.js";
import {FakeSocket, appserver, readyProvider} from "./browser-harness.mjs";

const fixture = JSON.parse(readFileSync(new URL("../../tests/fixtures/thread-inspection.json", import.meta.url), "utf8"));

test("shared completion/inspection contract reaches thread, section and project presentation", () => {
    const session = new BrowserFrontendSession();
    const normalizer = new ProtocolNormalizer(frame => session.model.applyEvent(frame, session.getSnapshot().selectedThreadId));
    try {
        normalizer.transportEvent("connected");
        normalizer.operationResult("projects.list", "projects", {}, {result: {data: [fixture.project]}});
        normalizer.operationResult("sections.list", "sections", {}, {result: {data: [fixture.section]}});
        for (const thread of fixture.threads) { normalizer.serverNotification("thread/started", {thread}); }
        for (const step of fixture.steps) {
            if (step.select) {
                session.selectThread(step.select);
            } else if (step.method) {
                normalizer.serverNotification(step.method, step.params);
            } else if (step.history) {
                normalizer.operationResult("thread.read", "history", {threadId: step.history.id, includeTurns: true},
                    {result: {thread: step.history}}, normalizer.sequence);
            } else {
                const tones = session.model.threadIndicatorTones();
                const markup = renderToStaticMarkup(createElement(App, {session}));
                for (const thread of fixture.threads) {
                    assert.equal(tones.get(thread.id), step.expected[thread.id], `${step.label}: ${thread.id}`);
                }
                for (const group of [fixture.project, fixture.section]) {
                    const start = markup.indexOf(`aria-label="${group.name},`);
                    assert.ok(start >= 0, `${step.label}: ${group.name} is visible`);
                    const header = markup.slice(start, markup.indexOf("</header>", start));
                    assert.ok(header.includes(`status-dot ${step.expected[group.id]}`), `${step.label}: ${group.name} indicator`);
                    if (step.expected[group.id] === "uninspected") { assert.match(header, /not yet inspected/u); }
                }
                session.toggleGroup("project", fixture.project.id);
                const collapsed = renderToStaticMarkup(createElement(App, {session}));
                assert.match(collapsed, new RegExp(`Ready project,[^"]*" aria-expanded="false"`));
                assert.ok(collapsed.includes(`status-dot ${step.expected[fixture.project.id]}`));
                session.toggleGroup("project", fixture.project.id);
            }
        }
    } finally { session.dispose(); }
});

test("the live browser session passes current selection to completion handling", async () => {
    const socket = new FakeSocket(), session = new BrowserFrontendSession("ws://inspection.test", () => socket);
    try {
        session.connect(); socket.open(); await readyProvider(socket, "inspection");
        for (const thread of fixture.threads) {
            socket.receive(appserver({jsonrpc: "2.0", method: "thread/started", params: {thread}}));
        }
        session.selectThread("ready-other");
        const finish = (id, turn) => socket.receive(appserver({jsonrpc: "2.0", method: "turn/completed",
            params: {threadId: id, turn: {id: turn, status: "completed"}}}));
        finish("ready-child", "background");
        assert.equal(session.model.threadIndicatorTones().get("ready-child"), "uninspected");
        session.selectThread("ready-child");
        finish("ready-child", "foreground");
        assert.equal(session.model.threadIndicatorTones().get("ready-child"), "success");
        session.selectThread("ready-other");
        finish("ready-child", "foreground");
        assert.equal(session.model.threadIndicatorTones().get("ready-child"), "success");
    } finally { session.dispose(); }
});
