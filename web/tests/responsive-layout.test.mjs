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
    session.model.applyEvent(result(1, 1, "threads.list", "list", true, {threads: [{
        id: "thread-1", preview: "Accessible thread", cwd: "/workspace", status: {type: "idle"}, updatedAt: 10,
    }]}, "replace"));
    session.selectThread("thread-1");
    const markup = renderToStaticMarkup(createElement(App, {session}));
    assert.match(markup, /class="thread-list" role="tree" aria-label="Threads"/u);
    assert.match(markup, /role="treeitem" aria-level="1" aria-selected="true"/u);
    assert.match(markup, /aria-current="true" aria-label="Open Accessible thread, \/workspace"/u);
    assert.match(markup, /class="conversation-lockup"[\s\S]*Last activity:/u);
    session.dispose();
});

test("responsive CSS keeps the desktop grid and removes the old document-width floor", async () => {
    const css = await readFile(new URL("../src/styles.css", import.meta.url), "utf8");
    assert.match(css, /grid-template-columns:\s*260px minmax\(420px, 1fr\) 300px/u);
    assert.match(css, /grid-template-areas:\s*"top" "notice" "workspace" "status"/u);
    assert.doesNotMatch(css, /\.app-shell\s*\{[^}]*min-width:\s*980px/u);
    assert.match(css, /@media \(max-width:\s*1160px\)[\s\S]*grid-template-columns:\s*220px minmax\(0, 1fr\)/u);
    assert.match(css, /@media \(max-width:\s*760px\)[\s\S]*grid-template-columns:\s*minmax\(0, 1fr\)/u);
    assert.match(css, /@media \(max-width:\s*760px\)[\s\S]*\.top-bar\s*\{[^}]*flex-wrap:\s*wrap/u);
    assert.match(css, /\.conversation-heading \.conversation-activity\s*\{[^}]*color:\s*#1d2633/u);
    assert.match(css, /\.responsive-drawer\s*\{[^}]*position:\s*fixed/u);
    assert.match(css, /\.drawer-backdrop\s*\{[^}]*position:\s*fixed/u);
    assert.match(css, /button:focus-visible[\s\S]*outline:\s*2px solid #6f98e8/u);
    assert.match(css, /@media \(pointer:\s*coarse\)[\s\S]*min-height:\s*44px/u);
    assert.match(css, /\.composer-actions span\s*\{[^}]*color:\s*#667085/u);
    assert.match(css, /\.conversation-lockup\s*\{[^}]*align-items:\s*baseline/u);
    assert.match(css, /\.conversation-heading \.conversation-activity\s*\{[^}]*margin-left:\s*auto[^}]*text-align:\s*right/u);
    assert.match(css, /@media \(max-width:\s*520px\)[\s\S]*\.conversation-lockup \.conversation-activity\s*\{[^}]*flex-basis:\s*100%/u);
    assert.match(css, /\.composer-dock\s*\{[^}]*bottom:\s*0[^}]*padding:\s*8px 0 16px[^}]*background:\s*#f2f5f9/u);
    assert.match(css, /\.composer-dock::before\s*\{[^}]*bottom:\s*100%[^}]*height:\s*8px[^}]*background:\s*#f2f5f9/u);
    assert.match(css, /\.composer-dock::after\s*\{[^}]*top:\s*-1px[^}]*height:\s*1px[^}]*background:\s*#d7dee8/u);
    assert.match(css, /\.composer textarea\s*\{[^}]*background:\s*#fff/u);
    assert.match(css, /@media \(max-width:\s*520px\)[\s\S]*\.composer-dock\s*\{[^}]*bottom:\s*0[^}]*padding-bottom:\s*8px/u);
});
