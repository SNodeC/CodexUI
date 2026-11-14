import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import {createElement} from "react";
import {renderToStaticMarkup} from "react-dom/server";
import {App} from "../dist/app/App.js";
import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {ProtocolNormalizer} from "../dist/index.js";
import {FakeSocket, appserver, readyProvider, requests, respond, completeThreadDiscovery} from "./browser-harness.mjs";

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

const visibilityCases = JSON.parse(readFileSync(new URL("../../tests/fixtures/thread-inspection-visibility.json", import.meta.url), "utf8"));
for (const scenario of visibilityCases) {
    test(`inspection follows the presented hierarchy: ${scenario.label}`, async () => {
        const socket = new FakeSocket(), session = new BrowserFrontendSession(`ws://inspection-${scenario.label}.test`, () => socket);
        const threads = fixture.threads.map(thread => thread.id === "ready-child" ? {...thread, ...scenario.child} : thread);
        const replyToLists = async () => {
            for (const request of requests(socket, "thread/list")) {
                if (!socket.replied?.has(request.payload.id)) {
                    const params = request.payload.params;
                    const data = threads.filter(thread => Boolean(thread.archived) === Boolean(params.archived)
                        && (!params.searchTerm || thread.name.includes(params.searchTerm)));
                    respond(socket, request, {data, nextCursor: null});
                }
            }
            await completeThreadDiscovery(socket);
        };
        const threadButton = (markup, id) => {
            const start = markup.indexOf(`aria-label="Open ${id},`);
            const button = start < 0 ? "" : markup.slice(markup.lastIndexOf("<button", start), markup.indexOf("</button>", start));
            return button;
        };
        try {
            session.connect(); socket.open(); await readyProvider(socket, scenario.label);
            respond(socket, requests(socket, "project/list").at(-1), {data: [fixture.project, {id: "other-project", name: "Other project"}]});
            await Promise.resolve(); await Promise.resolve();
            respond(socket, requests(socket, "threadSection/list").at(-1), {data: [fixture.section, {id: "other-section", name: "Other section"}]});
            await Promise.resolve(); await Promise.resolve();
            for (const thread of threads) {
                socket.receive(appserver({jsonrpc: "2.0", method: "thread/started", params: {thread}}));
            }
            await replyToLists();
            for (const threadId of ["ready-parent", "ready-child"]) {
                socket.receive(appserver({jsonrpc: "2.0", method: "turn/completed",
                    params: {threadId, turn: {id: "finished", status: "completed"}}}));
            }
            session.selectThread("ready-parent");
            session.changeBrowser({grouping: scenario.grouping, archive: scenario.archive, search: scenario.title});
            await replyToLists();
            const markup = renderToStaticMarkup(createElement(App, {session}));
            const parent = threadButton(markup, "ready-parent");
            assert.ok(parent.includes('aria-current="true"'), "the inspected parent remains selected");
            assert.equal(parent.includes("status-dot uninspected"), scenario.expected.parent);
            assert.equal(Boolean(threadButton(markup, "ready-child")), scenario.expected.childVisible);
            const level = id => Number(markup.match(new RegExp(`<div role="treeitem" aria-level="(\\d+)"[^>]*aria-label="${id},`))?.[1]);
            assert.equal(scenario.expected.childVisible && level("ready-child") === level("ready-parent") + 1,
                scenario.expected.childNested, "attention follows the actual rendered ancestry");
            for (const [group, key] of [[fixture.project, "project"], [fixture.section, "section"]]) {
                const start = markup.indexOf(`aria-label="${group.name},`);
                if (start >= 0) {
                    const header = markup.slice(start, markup.indexOf("</header>", start));
                    assert.equal(header.includes("status-dot uninspected"), scenario.expected[key]);
                }
            }
            assert.equal(session.model.thread("ready-parent").uninspectedResult, false);
            assert.equal(session.model.thread("ready-child").uninspectedResult, true, "filtering does not acknowledge the child");
            session.changeBrowser({archive: "all", search: ""});
            await replyToLists();
            assert.ok(threadButton(renderToStaticMarkup(createElement(App, {session})), "ready-child"), "removing filters reveals the unread child");
            session.selectThread("ready-child");
            session.selectThread("ready-parent");
            assert.equal(threadButton(renderToStaticMarkup(createElement(App, {session})), "ready-parent").includes("status-dot uninspected"), false);
        } finally {
            session.dispose();
        }
    });
}
