import assert from "node:assert/strict";
import test from "node:test";
import {renderToStaticMarkup} from "react-dom/server";
import {createElement} from "react";

import {App, NewThreadDialog, inspectorPlainState, writeCardClipboard} from "../dist/app/App.js";
import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {readBrowserStorage, writeBrowserStorage} from "../dist/app/BrowserStorage.js";
import {event, humanizeProtocolLabel, result} from "../dist/index.js";

test("browser storage denial falls back without breaking startup or persistence", () => {
    globalThis.window = {
        location: {protocol: "https:", host: "codex.example"},
        get localStorage() { throw new Error("storage denied"); },
    };
    try {
        assert.equal(readBrowserStorage("missing"), undefined);
        assert.doesNotThrow(() => writeBrowserStorage("preference", "true"));
        assert.equal(BrowserFrontendSession.defaultBridgeUrl(), "wss://codex.example/codex");
    } finally { delete globalThis.window; }
});

test("clipboard writes report success, unsupported access, and failure honestly", async () => {
    const retainedNavigator = Object.getOwnPropertyDescriptor(globalThis, "navigator");
    let copied = "";
    try {
        Object.defineProperty(globalThis, "navigator", {configurable: true, value: {
            clipboard: {writeText: async value => { copied = value; }},
        }});
        assert.equal(await writeCardClipboard({text: "copy me", markdown: false}), "copied");
        assert.equal(copied, "copy me");
        Object.defineProperty(globalThis, "navigator", {configurable: true, value: {}});
        assert.equal(await writeCardClipboard({text: "copy me", markdown: false}), "unsupported");
        Object.defineProperty(globalThis, "navigator", {configurable: true, value: {
            clipboard: {writeText: async () => { throw new Error("denied"); }},
        }});
        assert.equal(await writeCardClipboard({text: "copy me", markdown: false}), "failed");
    } finally {
        if (retainedNavigator) Object.defineProperty(globalThis, "navigator", retainedNavigator);
        else delete globalThis.navigator;
    }
});

test("server-rendered shell exposes keyboard and landmark semantics", () => {
    const session = new BrowserFrontendSession("ws://bridge.test/", () => { throw new Error("not connected"); });
    const markup = renderToStaticMarkup(createElement(App, {session}));
    assert.match(markup, /<header class="top-bar">/u);
    assert.match(markup, /<main class="conversation-pane">/u);
    assert.match(markup, /<aside class="thread-pane">/u);
    assert.match(markup, /<aside class="inspector-pane">/u);
    assert.match(markup, /<footer class="status-bar">/u);
    assert.match(markup, /aria-label="Bridge WebSocket URL"/u);
    assert.match(markup, /aria-label="Conversation presentation"/u);
    assert.match(markup, /aria-label="Show reasoning cards"/u);
    assert.match(markup, /aria-label="Hide Codex update cards"/u);
    assert.match(markup, /aria-label="New command cards start expanded"/u);
    assert.match(markup, /aria-label="New image cards start expanded"/u);
    assert.match(markup, /aria-label="Message Codex"/u);
    assert.match(markup, /aria-describedby="composer-keyboard-hint"/u);
    assert.match(markup, /aria-keyshortcuts="Enter Control\+Enter Meta\+Enter"/u);
    assert.match(markup, /id="composer-keyboard-hint">Enter to send · Shift\+Enter for a new line/u);
    assert.match(markup, /aria-expanded="false"/u);
    session.dispose();
});

test("new-thread dialog exposes the complete native creation draft", () => {
    const markup = renderToStaticMarkup(createElement(NewThreadDialog, {
        initialWorkspace: "/workspace", onCancel: () => {}, onContinue: () => {},
    }));
    assert.match(markup, /role="dialog"/u);
    assert.match(markup, />Workspace</u);
    assert.match(markup, />Name</u);
    assert.match(markup, />Base instructions</u);
    assert.match(markup, />Developer instructions</u);
    assert.match(markup, />Temporary thread</u);
    assert.match(markup, /value="\/workspace"/u);
});

test("protocol labels are humanized only at the render boundary", () => {
    assert.equal(humanizeProtocolLabel("contextCompaction"), "Context compaction");
    assert.equal(humanizeProtocolLabel("thread.settings.changed"), "Thread settings changed");
    assert.equal(humanizeProtocolLabel("commandExecution"), "Command execution");
    assert.equal("contextCompaction", "contextCompaction", "the protocol value remains unchanged");
});

test("Inspector state diagnostics read items only while State is selected", () => {
    let itemReads = 0;
    const item = {};
    Object.defineProperty(item, "raw", {get: () => { ++itemReads; return {type: "agentMessage"}; }});
    const thread = {
        id: "thread", title: "", cwd: "", status: "", archived: false,
        turnOrder: ["turn"], turns: new Map([["turn", {id: "turn", status: "", plan: {}, itemOrder: ["item"], items: new Map([["item", item]])}]]),
        agentOrder: [], agents: new Map(), domains: new Map(),
    };
    assert.equal(inspectorPlainState(thread, false), null);
    assert.equal(itemReads, 0);
    assert.notEqual(inspectorPlainState(thread, true), null);
    assert.equal(itemReads, 1);
});

test("empty reasoning spins only while its authoritative turn is active", () => {
    globalThis.window = {localStorage: {
        getItem: key => key === "codexui.conversation.showReasoning" ? "true" : null,
        setItem: () => {},
    }};
    const session = new BrowserFrontendSession("ws://bridge.test/codex", () => { throw new Error("not connected"); });
    session.model.applyEvent(result(1, 1, "threads.list", "list", true, {threads: [{id: "thread-1"}]}, "replace"));
    session.selectThread("thread-1");
    const thread = status => ({id: "thread-1", status: {type: status === "inProgress" ? "active" : "idle"}, turns: [{
        id: "turn-1", status, items: [{id: "reasoning-1", type: "reasoning", summary: []}],
    }]});
    session.model.applyEvent(result(2, 1, "thread.read", "active", true, {thread: thread("inProgress")}, "replace", {threadId: "thread-1"}));
    assert.match(renderToStaticMarkup(createElement(App, {session})), /Working…/u);

    session.model.applyEvent(result(3, 1, "thread.read", "completed", true, {thread: thread("completed")}, "replace", {threadId: "thread-1"}));
    const completed = renderToStaticMarkup(createElement(App, {session}));
    assert.doesNotMatch(completed, /Working…|activity-line/u);
    delete globalThis.window;
    session.dispose();
});

test("conversation presentation preferences retain filtered cards and initialize commands", () => {
    const session = new BrowserFrontendSession("ws://bridge.test/codex", () => { throw new Error("not connected"); });
    session.model.applyEvent(result(1, 1, "threads.list", "list", true, {threads: [{id: "thread-1"}]}, "replace"));
    session.selectThread("thread-1");
    const thread = {id: "thread-1", status: {type: "idle"}, turns: [{
        id: "turn-1", status: "completed", items: [
            {id: "reasoning-1", type: "reasoning", summary: ["Retained reasoning detail"]},
            {id: "update-1", type: "agentMessage", phase: "commentary", text: "Retained Codex update"},
            {id: "final-1", type: "agentMessage", phase: "final_answer", text: "Final answer always visible"},
            {id: "command-1", type: "commandExecution", command: "printf retained-command", status: "completed"},
            {id: "command-running", type: "commandExecution", command: "printf running-command", status: "inProgress"},
            {id: "image-1", type: "imageGeneration", path: "/missing/image.png", status: "completed", revisedPrompt: "Retained generated image"},
            {id: "image-running", type: "imageGeneration", path: "", status: "inProgress", revisedPrompt: "Loading generated image"},
        ],
    }]};
    session.model.applyEvent(result(2, 1, "thread.read", "read", true, {thread}, "replace", {threadId: "thread-1"}));

    const defaults = renderToStaticMarkup(createElement(App, {session}));
    assert.doesNotMatch(defaults, /Retained reasoning detail/u);
    assert.match(defaults, /Retained Codex update/u);
    assert.match(defaults, /Final answer always visible/u);
    assert.doesNotMatch(defaults, /conversation-card commandExecution\s+collapsed/u);
    assert.match(defaults, /printf retained-command/u);
    assert.match(defaults, /Retained generated image/u);
    assert.doesNotMatch(defaults, /conversation-card imageGeneration\s+collapsed/u);
    assert.equal((defaults.match(/active-work/gu) ?? []).length, 2);
    assert.match(defaults, /conversation-card commandExecution[^"]*active-work[^>]*>[\s\S]*?running-command/u);
    assert.match(defaults, /conversation-card imageGeneration[^"]*active-work[^>]*>[\s\S]*?Loading generated image/u);

    const values = new Map([
        ["codexui.conversation.showReasoning", "true"],
        ["codexui.conversation.showCodexUpdates", "false"],
        ["codexui.conversation.commandsInitiallyExpanded", "false"],
        ["codexui.conversation.imagesInitiallyExpanded", "false"],
    ]);
    globalThis.window = {localStorage: {
        getItem: key => values.get(key) ?? null,
        setItem: (key, value) => values.set(key, value),
    }};
    const filtered = renderToStaticMarkup(createElement(App, {session}));
    delete globalThis.window;
    assert.match(filtered, /conversation-card reasoning\s+collapsed/u);
    assert.doesNotMatch(filtered, /Retained reasoning detail/u);
    assert.match(filtered, /aria-label="Expand card"/u);
    assert.doesNotMatch(filtered, /Retained Codex update/u);
    assert.match(filtered, /Final answer always visible/u);
    assert.match(filtered, /conversation-card commandExecution\s+collapsed/u);
    assert.doesNotMatch(filtered, /printf retained-command/u);
    assert.doesNotMatch(filtered, /Retained generated image/u);
    assert.match(filtered, /conversation-card imageGeneration\s+collapsed/u);
    assert.match(filtered, /aria-label="Hide reasoning cards"/u);
    assert.match(filtered, /aria-label="Show Codex update cards"/u);
    assert.match(filtered, /aria-label="New command cards start collapsed"/u);
    assert.match(filtered, /aria-label="New image cards start collapsed"/u);
    session.dispose();
});

test("Plan reconciles stale Running against terminal lifecycle without changing Pending", () => {
    const session = new BrowserFrontendSession("ws://bridge.test/codex", () => { throw new Error("not connected"); });
    session.model.applyEvent(event(1, 1, "thread.upsert", {thread: {id: "plan-thread", status: "active"}}, "merge", {threadId: "plan-thread"}));
    session.model.applyEvent(event(2, 1, "turn.upsert", {turn: {id: "plan-turn", status: "inProgress"}}, "merge", {threadId: "plan-thread", turnId: "plan-turn"}));
    session.model.applyEvent(event(3, 1, "plan.replaced", {
        explanation: "Lifecycle plan",
        steps: [{step: "Active step", status: "inProgress"}, {step: "Pending step", status: "pending"}],
    }, "replace", {threadId: "plan-thread", turnId: "plan-turn"}));
    session.selectThread("plan-thread");
    const render = () => renderToStaticMarkup(createElement(App, {session}));
    assert.match(render(), /<small>Running<\/small>[\s\S]*<small>Pending<\/small>/u);

    for (const [sequence, source, display] of [[4, "completed", "Completed"], [5, "failed", "Failed"], [6, "interrupted", "Interrupted"]]) {
        session.model.applyEvent(event(sequence, 1, "thread.upsert", {thread: {id: "plan-thread", status: source}}, "merge", {threadId: "plan-thread"}));
        const markup = render();
        assert.doesNotMatch(markup, /<small>Running<\/small>/u);
        assert.match(markup, new RegExp(`<small>${display}</small>[\\s\\S]*<small>Pending</small>`, "u"));
    }
    session.dispose();
});
