import assert from "node:assert/strict";
import test from "node:test";
import {renderToStaticMarkup} from "react-dom/server";
import {createElement} from "react";

import {App} from "../dist/app/App.js";
import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {humanizeProtocolLabel} from "../dist/index.js";

test("server-rendered shell exposes keyboard and landmark semantics", () => {
    const session = new BrowserFrontendSession("ws://bridge.test/", () => { throw new Error("not connected"); });
    const markup = renderToStaticMarkup(createElement(App, {session}));
    assert.match(markup, /<header class="top-bar">/u);
    assert.match(markup, /<main class="conversation-pane">/u);
    assert.match(markup, /<aside class="thread-pane">/u);
    assert.match(markup, /<aside class="inspector-pane">/u);
    assert.match(markup, /<footer class="status-bar">/u);
    assert.match(markup, /aria-label="Bridge WebSocket URL"/u);
    assert.match(markup, /aria-expanded="false"/u);
    session.dispose();
});

test("protocol labels are humanized only at the render boundary", () => {
    assert.equal(humanizeProtocolLabel("contextCompaction"), "Context compaction");
    assert.equal(humanizeProtocolLabel("thread.settings.changed"), "Thread settings changed");
    assert.equal(humanizeProtocolLabel("commandExecution"), "Command execution");
    assert.equal("contextCompaction", "contextCompaction", "the protocol value remains unchanged");
});
