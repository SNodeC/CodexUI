import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import test from "node:test";
import {createElement} from "react";
import {renderToStaticMarkup} from "react-dom/server";

import {App, responsiveModeForWidth, runThreadPaneNavigation} from "../dist/app/App.js";
import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {result} from "../dist/index.js";

function renderShellAt(width) {
    const previousWindow = globalThis.window;
    globalThis.window = {
        innerWidth: width,
        localStorage: {getItem: () => null, setItem: () => {}},
        matchMedia: query => {
            const maximum = Number(/max-width:\s*(\d+)px/u.exec(query)?.[1] ?? Number.POSITIVE_INFINITY);
            return {matches: width <= maximum, addEventListener: () => {}, removeEventListener: () => {}};
        },
    };
    const session = new BrowserFrontendSession("ws://bridge.test/", () => { throw new Error("not connected"); });
    try { return renderToStaticMarkup(createElement(App, {session})); }
    finally {
        session.dispose();
        if (previousWindow === undefined) delete globalThis.window;
        else globalThis.window = previousWindow;
    }
}

test("responsive modes retain the desktop boundary and switch at tablet and mobile widths", () => {
    assert.equal(responsiveModeForWidth(1600), "desktop");
    assert.equal(responsiveModeForWidth(1161), "desktop");
    assert.equal(responsiveModeForWidth(1160), "tablet");
    assert.equal(responsiveModeForWidth(761), "tablet");
    assert.equal(responsiveModeForWidth(760), "mobile");
    assert.equal(responsiveModeForWidth(480), "mobile");
});

test("thread drawer navigation completes the navigation before closing the overlay", () => {
    const events = [];
    runThreadPaneNavigation(() => events.push("navigate"), () => events.push("close"));
    assert.deepEqual(events, ["navigate", "close"]);
});

test("responsive shell exposes only the panes that fit and accessible drawer triggers for the rest", () => {
    const desktop = renderShellAt(1600);
    assert.match(desktop, /<aside class="thread-pane">/u);
    assert.match(desktop, /<aside class="inspector-pane">/u);
    assert.doesNotMatch(desktop, /class="responsive-pane-button"/u);

    const tablet = renderShellAt(1024);
    assert.match(tablet, /<aside class="thread-pane">/u);
    assert.doesNotMatch(tablet, /<aside class="inspector-pane/u);
    assert.doesNotMatch(tablet, /aria-controls="thread-pane"/u);
    assert.match(tablet, /aria-haspopup="dialog" aria-controls="inspector-pane" aria-expanded="false"/u);

    const mobile = renderShellAt(480);
    assert.doesNotMatch(mobile, /<aside class="thread-pane/u);
    assert.doesNotMatch(mobile, /<aside class="inspector-pane/u);
    assert.match(mobile, /aria-haspopup="dialog" aria-controls="thread-pane" aria-expanded="false"/u);
    assert.match(mobile, /aria-haspopup="dialog" aria-controls="inspector-pane" aria-expanded="false"/u);
});

test("thread hierarchy exposes selected tree-item semantics", () => {
    const session = new BrowserFrontendSession("ws://bridge.test/", () => { throw new Error("not connected"); });
    session.model.applyEvent(result(1, 1, "threads.list", "list", true, {threads: [
        {id: "thread-1", preview: "Accessible thread", cwd: "/workspace", status: {type: "idle"},
            createdAt: 5, updatedAt: 10, recencyAt: 9},
        {id: "thread-2", parentThreadId: "thread-1", preview: "Accessible child", status: {type: "idle"}},
    ]}, "replace"));
    const parentPresentation = session.threadVisualKey("thread-1");
    session.viewportState.setThreadExpanded("thread-1", parentPresentation, true);
    session.selectThread("thread-1", parentPresentation);
    const markup = renderToStaticMarkup(createElement(App, {session}));
    assert.match(markup, /class="thread-list" role="tree" aria-label="Threads"/u);
    assert.match(markup, /role="treeitem" aria-level="1" aria-selected="true" aria-posinset="1" aria-setsize="1"/u);
    assert.match(markup, /role="treeitem" aria-level="2" aria-selected="false" aria-posinset="1" aria-setsize="1"/u);
    assert.doesNotMatch(markup, /role="group"/u,
        "all exact thread rows share one keyed React parent across reparenting");
    assert.match(markup, /aria-current="true" aria-label="Open Accessible thread, Workspace: \/workspace, Status: completed, Recent turn: [^,]+, Created: [^,]+, Last activity: /u);
    assert.match(markup, /<span class="thread-title">Accessible thread<\/span><\/button>/u);
    assert.match(markup, /class="conversation-lockup"[\s\S]*Last activity:/u);
    assert.match(markup, /<h1>Accessible thread<\/h1>\s*<span class="conversation-state success">completed<\/span>\s*<div class="turn-navigation"/u);
    session.dispose();
});

test("project headings show authored names without a synthetic Project prefix", () => {
    const session = new BrowserFrontendSession("ws://bridge.test/", () => { throw new Error("not connected"); });
    session.model.applyEvent(result(1, 1, "projects.list", "projects", true, {data: [
        {id: "plain", name: "SNode.C"}, {id: "authored", name: "Project · Authored name"},
    ]}));
    session.model.applyEvent(result(2, 1, "sections.list", "sections", true, {data: [{id: "section", name: "Pinned"}]}));
    const markup = renderToStaticMarkup(createElement(App, {session}));
    assert.match(markup, /<span>SNode\.C<\/span><\/button>/u);
    assert.match(markup, /<span>Project · Authored name<\/span><\/button>/u);
    assert.doesNotMatch(markup, /Project · SNode\.C/u);
    assert.match(markup, /Section · Pinned/u);
    session.dispose();
});





test("browser palette and brand are generated from the native presentation authority", async () => {
    const {generatedUiAssets} = await import("../tools/generate-ui-assets.mjs");
    const assets = generatedUiAssets();
    assert.equal(await readFile(new URL("../src/colors.css", import.meta.url), "utf8"), assets.colors);
    assert.equal(await readFile(new URL("../src/brand.svg", import.meta.url), "utf8"), assets.brand);
});


test("project bodies retain all rows in their rendered group", async () => {
    const session = new BrowserFrontendSession("ws://bridge.test/", () => { throw new Error("not connected"); });
    try {
        session.model.applyEvent(result(1, 1, "projects.list", "projects", true, {data: [{id: "cap-project", name: "Large project"}]}));
        session.model.applyEvent(result(2, 1, "sections.list", "sections", true, {data: [{id: "cap-section", name: "Section"}]}));
        session.model.applyEvent(result(3, 1, "threads.list", "list", true, {threads: Array.from({length: 20}, (_, index) => ({id: `cap-${index}`, preview: `Thread ${index}`, projectId: "cap-project", section: {id: "cap-section"}, status: {type: "idle"}}))}, "replace"));
        const markup = renderToStaticMarkup(createElement(App, {session}));
        assert.match(markup, /<header[\s\S]*?<\/header><div role="group" class="project-thread-body">/u);
        assert.equal((markup.match(/class="thread-title"/gu) ?? []).length, 20);
        assert.match(markup, /nested-section/u);
    } finally { session.dispose(); }
});

test("initial hierarchy exposes direct and indirect activity in thread and group markup", () => {
    const session = new BrowserFrontendSession("ws://bridge.test/", () => { throw new Error("not connected"); });
    try {
        session.model.applyEvent(result(1, 1, "projects.list", "projects", true, {data: [{id: "activity-project", name: "Activity project"}]}));
        session.model.applyEvent(result(2, 1, "sections.list", "sections", true, {data: [{id: "activity-section", name: "Activity section"}]}));
        session.model.applyEvent(result(3, 1, "threads.list", "list", true, {threads: [
            {id: "activity-origin", name: "Origin", projectId: "activity-project", section: {id: "activity-section"}, status: {type: "idle"}},
            {id: "activity-child", name: "Child", parentThreadId: "activity-origin", projectId: "activity-project", section: {id: "activity-section"}, status: {type: "active"}},
        ]}, "replace"));
        const markup = renderToStaticMarkup(createElement(App, {session}));
        assert.equal((markup.match(/class="status-dot indirect"/gu) ?? []).length, 3, "project, section and idle parent show indirect activity");
        assert.equal((markup.match(/class="status-dot active"/gu) ?? []).length, 1, "only the running thread is blue");
        assert.match(markup, /Expand|Collapse/u);
        assert.match(markup, /Indirect activity: running descendant/u);
        assert.equal((markup.match(/class="thread-title"/gu) ?? []).length, 2, "new thread hierarchies start unfolded");
    } finally { session.dispose(); }
});

test("the controlled prompt explicitly enables browser spell checking", () => {
    const markup = renderShellAt(1600);
    assert.match(markup, /<textarea[^>]*spellcheck="true"[^>]*aria-label="Message Codex"/u);
});
