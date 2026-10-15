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
    assert.match(markup, /class="conversation-state success">completed<\/span>/u);
    session.dispose();
});

test("responsive CSS keeps the desktop grid and removes the old document-width floor", async () => {
    const source = await readFile(new URL("../src/styles.css", import.meta.url), "utf8");
    const colors = await readFile(new URL("../src/colors.css", import.meta.url), "utf8");
    const tokens = new Map([...colors.matchAll(/--([\w-]+):\s*([^;]+);/gu)].map(match => [match[1], match[2]]));
    const css = source.replace(/var\(--([\w-]+)\)/gu, (reference, name) => tokens.get(name) ?? reference);
    assert.match(css, /grid-template-columns:\s*282px minmax\(420px, 1fr\) 404px/u);
    assert.match(css, /pre\.command-output\s*\{[^}]*max-height:\s*212px;[^}]*padding:\s*4px 7px;[^}]*line-height:\s*17px;/u,
        "command output uses complete text rows and symmetric vertical padding");
    assert.match(css, /grid-template-areas:\s*"top" "workspace" "status"/u);
    assert.match(css, /\.notice-banner\s*\{[^}]*grid-area:\s*workspace[^}]*align-self:\s*start[^}]*z-index:\s*8[^}]*border-radius:\s*8px/u);
    assert.doesNotMatch(css, /\.app-shell\s*\{[^}]*min-width:\s*980px/u);
    assert.match(css, /@media \(max-width:\s*1160px\)[\s\S]*grid-template-columns:\s*220px minmax\(0, 1fr\)/u);
    assert.match(css, /@media \(max-width:\s*760px\)[\s\S]*grid-template-columns:\s*minmax\(0, 1fr\)/u);
    assert.match(css, /@media \(max-width:\s*760px\)[\s\S]*\.top-bar\s*\{[^}]*flex-wrap:\s*wrap/u);
    assert.match(css, /\.conversation-heading \.conversation-activity\s*\{[^}]*color:\s*#1d2633/u);
    assert.match(css, /\.thread-row \.status-dot\s*\{[^}]*background:\s*#cacccf/u);
    assert.match(css, /\.thread-row \.status-dot\.success\s*\{[^}]*background:\s*#388262/u);
    assert.match(css, /\.thread-row \.status-dot\.active\s*\{[^}]*background:\s*#5471a6/u);
    assert.match(css, /\.thread-row \.status-dot\.danger\s*\{[^}]*background:\s*#9f5b5d/u);
    assert.match(css, /\.conversation-card\.userMessage\.steering\s*\{[^}]*background:\s*#eaf8f7;[^}]*border-color:\s*#9bdcdb/u);
    assert.match(css, /\.conversation-card\.localPrompt\.steering\s*\{[^}]*background:\s*#eaf8f7;[^}]*border-color:\s*#53c9c9/u);
    assert.match(css, /\.turn-nested\s*\{[^}]*gap:\s*8px;[^}]*margin-top:\s*8px/u);
    assert.doesNotMatch(css, /acknowledgment-fade/u);
    assert.match(css, /conversation-card\.turn-container\.active-turn[^}]*#6f98e8/u);
    assert.match(css, /\.send-button:where\(\.steer\)\s*\{[^}]*background:\s*#138282[^}]*color:\s*#fff/u);
    assert.match(css, /\.send-button:where\(\.steer\):hover\s*\{[^}]*background:\s*#0f6e6e/u);
    assert.match(css, /\.send-button:where\(\.steer\):active\s*\{[^}]*background:\s*#0b5c5c/u);
    assert.match(css, /\.responsive-drawer\s*\{[^}]*position:\s*fixed/u);
    assert.match(css, /\.drawer-backdrop\s*\{[^}]*position:\s*fixed/u);
    assert.match(css, /button:focus-visible[\s\S]*outline:\s*2px solid #5471a6/u);
    assert.match(css, /@media \(pointer:\s*coarse\)[\s\S]*min-height:\s*44px/u);
    assert.match(css, /\.composer-submit-actions\s*\{[^}]*display:\s*flex/u);
    assert.match(css, /\.conversation-lockup\s*\{[^}]*align-items:\s*baseline/u);
    assert.match(css, /\.conversation-facts\s*\{[^}]*justify-content:\s*flex-end/u);
    assert.doesNotMatch(css, /font-variant-caps/u);
    assert.match(css, /\.card-phase\.status\.active\s*\{[^}]*color:\s*#415882/u);
    assert.match(css, /\.card-phase\.status\.success\s*\{[^}]*color:\s*#2a654c/u);
    assert.match(css, /\.conversation-card > header \.card-phase\s*\{[^}]*margin-right:\s*0/u);
    assert.doesNotMatch(css, /\.conversation-card\.userMessage \.safe-markdown\s*\{[^}]*white-space:\s*pre-wrap/u);
    assert.match(css, /\.card-copy-button \.copy-glyph, \.card-copy-button \.check-glyph\s*\{[^}]*transition:\s*opacity 160ms ease, transform 160ms ease/u);
    assert.match(css, /\.card-copy-button\.copied \.copy-glyph\s*\{[^}]*opacity:\s*0[^}]*transform:\s*scale\(\.72\)/u);
    assert.match(css, /\.card-copy-button\.copied \.check-glyph\s*\{[^}]*opacity:\s*1[^}]*transform:\s*scale\(1\)/u);
    assert.doesNotMatch(css, /copy-breathe/u);
    assert.match(css, /\.card-copy-overlay\s*\{[^}]*border-radius:\s*6px[^}]*background:\s*#1d2633/u);
    assert.match(css, /\.image-ribbon\s*\{[^}]*align-items:\s*center[^}]*padding:\s*4px[^}]*background:\s*#111827/u);
    assert.match(css, /@media \(prefers-reduced-motion:\s*reduce\)[\s\S]*animation-duration:\s*\.001ms/u);
    assert.match(css, /@media \(prefers-reduced-motion:\s*reduce\)[\s\S]*transition-duration:\s*\.001ms/u);
    assert.match(css, /@media \(max-width:\s*520px\)[\s\S]*\.conversation-lockup\s*\{[^}]*grid-template-columns:\s*minmax\(0, 1fr\)/u);
    assert.match(css, /\.composer-dock\s*\{[^}]*bottom:\s*24px[^}]*padding:\s*8px 10px 0[^}]*background:\s*#f6f8fb/u);
    assert.doesNotMatch(css, /\.composer-dock::(?:before|after)/u, "composer has one divider owner");
    assert.match(css, /\.composer-dock\s*\{[^}]*border-top:\s*1px solid #d7dee8/u);
    assert.match(css, /\.conversation-scroll::-webkit-scrollbar-track\s*\{[^}]*margin-block-end:\s*calc\(var\(--composer-overlay-height\) \+ 8px\)/u);
    assert.match(css, /\.conversation-loading-surface\s*\{[^}]*inset:\s*0 0 var\(--composer-overlay-height\)[^}]*place-items:\s*center[^}]*background:\s*#f6f8fb/u);
    assert.match(css, /\.thread-loading-spinner\s*\{[^}]*width:\s*30px[^}]*height:\s*30px[^}]*border:\s*3px solid #d7dee8[^}]*border-top-color:\s*#667085/u);
    assert.match(css, /\.composer textarea\s*\{[^}]*overscroll-behavior:\s*contain[^}]*background:\s*transparent/u);
    assert.match(css, /@media \(max-width:\s*520px\)[\s\S]*\.composer-dock\s*\{[^}]*bottom:\s*0[^}]*padding-bottom:\s*8px/u);
});

test("composer wheel and hidden-scroll ownership stay local to WebUI chrome", async () => {
    const source = await readFile(new URL("../src/app/App.tsx", import.meta.url), "utf8");
    assert.match(source, /settings-panel[\s\S]*onWheel=\{event => event\.preventDefault\(\)\}/u);
    assert.match(source, /if \(element\.scrollHeight <= maximum\) element\.scrollTop = 0/u);
    assert.match(source, /onWheel=\{event => event\.stopPropagation\(\)\}/u);
    assert.match(source, /event\.key === "Enter" && event\.repeat && !event\.shiftKey && !composing\) event\.preventDefault\(\)/u);
});


test("browser colors are defined once as semantic roles and every role resolves", async () => {
    const source = await readFile(new URL("../src/styles.css", import.meta.url), "utf8");
    const colors = await readFile(new URL("../src/colors.css", import.meta.url), "utf8");
    assert.match(source, /^@import "\.\/colors\.css";/u);
    assert.doesNotMatch(source, /#[0-9a-f]{3,8}\b|(?:rgba?|hsla?)\(/iu);
    const definitions = [...colors.matchAll(/--([\w-]+):\s*(#[0-9a-f]{3,8});/gu)];
    const names = new Set(definitions.map(match => match[1]));
    assert.equal(names.size, definitions.length, "each color role has one definition");
    const local = new Set([...source.matchAll(/--([\w-]+):/gu)].map(match => match[1]));
    const used = new Set([...source.matchAll(/var\(--([\w-]+)\)/gu)].map(match => match[1]));
    for (const name of used) assert.ok(names.has(name) || local.has(name), `undefined token: ${name}`);
    for (const name of names) {
        assert.ok(used.has(name), `unused token: ${name}`);
        assert.doesNotMatch(name, /(?:^|-)(?:blue|green|amber|yellow|orange|red|purple|teal|lime)(?:-|$)/u);
    }
});

test("browser palette and brand are generated from the native presentation authority", async () => {
    const {generatedUiAssets} = await import("../tools/generate-ui-assets.mjs");
    const assets = generatedUiAssets();
    assert.equal(await readFile(new URL("../src/colors.css", import.meta.url), "utf8"), assets.colors);
    assert.equal(await readFile(new URL("../src/brand.svg", import.meta.url), "utf8"), assets.brand);
});
