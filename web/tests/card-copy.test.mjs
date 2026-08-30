import assert from "node:assert/strict";
import test from "node:test";

import {createElement} from "react";
import {renderToStaticMarkup} from "react-dom/server";

import {Card, cardCopyContent} from "../dist/app/App.js";

function itemCard(kind, itemId, payload) {
    return {
        key: {kind: "item", threadId: "thread", turnId: "turn", itemId},
        kind, threadId: "thread", turnId: "turn", itemId, payload,
    };
}

test("card copy payloads preserve Markdown and structured source", () => {
    const prompt = itemCard("userMessage", "prompt", {
        text: "# Prompt", imagePaths: ["/tmp/image.png"],
    });
    assert.deepEqual(cardCopyContent(prompt), {text: "# Prompt", markdown: true});

    const markdown = itemCard("agentMessage", "answer", {
        text: "## Answer\n\n- **exact**", finalAnswer: true,
    });
    assert.deepEqual(cardCopyContent(markdown), {
        text: "## Answer\n\n- **exact**", markdown: true,
    });

    const command = itemCard("commandExecution", "command", {
        command: "printf test\n\n", output: "line\n\n", status: "completed", cwd: "", exitCode: 0,
    });
    assert.deepEqual(cardCopyContent(command), {
        text: "printf test\n\nline", markdown: false,
    });

    const files = itemCard("fileChanges", "files", {
        status: "completed", changes: [{path: "src/card.cpp", kind: "update", additions: 2, deletions: 1}],
    });
    assert.deepEqual(cardCopyContent(files), {
        text: "src/card.cpp  ·  Update  +2 −1", markdown: false,
    });
});

test("Copy precedes folding and remains available on collapsed cards", () => {
    const message = itemCard("agentMessage", "answer", {
        text: "Source **Markdown**", finalAnswer: true,
    });
    const markup = renderToStaticMarkup(createElement(Card, {
        card: message, active: false, collapsed: true, onToggle() {},
    }));
    assert.ok(markup.includes("Copy card content"));
    assert.ok(markup.indexOf("Copy card content") < markup.indexOf("Expand card"));
    assert.match(markup, /card-copy-button[^>]*><svg/u);
    assert.ok(!markup.includes(">Copy</button>"));
    assert.ok(!markup.includes("Source **Markdown**"));

    const emptyReasoning = itemCard("reasoning", "reasoning", {summary: ""});
    const emptyMarkup = renderToStaticMarkup(createElement(Card, {
        card: emptyReasoning, active: false, collapsed: true, onToggle() {},
    }));
    assert.ok(!emptyMarkup.includes("Copy card content"));
});

test("only an authoritative running-turn You container is emphasized", () => {
    const user = itemCard("userMessage", "prompt", {
        text: "Prompt", imagePaths: [],
    });
    const running = renderToStaticMarkup(createElement(Card, {
        card: user, active: true, collapsed: false, turnContainer: true,
        onToggle() {},
    }));
    assert.match(running, /userMessage.*turn-container.*active-turn/u);
    const finished = renderToStaticMarkup(createElement(Card, {
        card: user, active: false, collapsed: false, turnContainer: true,
        onToggle() {},
    }));
    assert.doesNotMatch(finished, /active-turn/u);
});

test("message attachments retain one horizontal ribbon in source order", () => {
    const user = itemCard("userMessage", "prompt", {
        text: "Prompt", imagePaths: ["/tmp/one.png", "/tmp/two.png", "/tmp/three.png"],
    });
    const markup = renderToStaticMarkup(createElement(Card, {
        card: user, active: false, collapsed: false, onToggle() {},
    }));
    assert.match(markup, /class="image-ribbon"/u);
    assert.ok(markup.indexOf("one.png") < markup.indexOf("two.png"));
    assert.ok(markup.indexOf("two.png") < markup.indexOf("three.png"));
});

test("authoritative messages render safe GitHub Markdown without fetching embedded images", () => {
    const user = itemCard("userMessage", "markdown", {
        text: "# Heading\n\n> quoted\n\n| A | B |\n| - | - |\n| 1 | 2 |\n\n- [x] done\n\n[local](file:///tmp/file)\n\n![remote](https://example.test/image.png)\n\n<script>unsafe()</script>",
        imagePaths: [],
    });
    const markup = renderToStaticMarkup(createElement(Card, {
        card: user, active: false, collapsed: false, onToggle() {}, onCopy() {},
    }));
    assert.match(markup, /<h1>Heading<\/h1>/u);
    assert.match(markup, /<blockquote>/u);
    assert.match(markup, /<table>/u);
    assert.match(markup, /type="checkbox"/u);
    assert.match(markup, /checked=""/u);
    assert.match(markup, /disabled=""/u);
    assert.doesNotMatch(markup, /<script|<img/u);
    assert.doesNotMatch(markup, /href="file:/u);
    assert.match(markup, /markdown-image-reference/u);
});

test("typed activity cards retain complete metadata and bounded diagnostics", () => {
    const command = itemCard("commandExecution", "command", {
        command: "printf test\n\n", output: "done\n\n", status: "completed", cwd: "/workspace", exitCode: 0,
        durationMilliseconds: 1500,
    });
    const commandMarkup = renderToStaticMarkup(createElement(Card, {
        card: command, active: false, collapsed: false, onToggle() {}, onCopy() {},
    }));
    assert.match(commandMarkup, /Command execution/u);
    assert.match(commandMarkup, /Completed  \|  exit 0  \|  \/workspace  \|  1\.5 s/u);
    assert.doesNotMatch(commandMarkup, /done\n\n/u);

    const agent = itemCard("agentActivity", "agent", {
        tool: "spawn_agent", status: "completed", kind: "", prompt: "Inspect this", resultText: "**Done**",
        receivers: ["worker"], model: "gpt-test", reasoningEffort: "medium", childThreadId: "child",
        agentPath: "root/worker", senderThreadId: "root",
    });
    const agentMarkup = renderToStaticMarkup(createElement(Card, {
        card: agent, active: false, collapsed: false, onToggle() {}, onCopy() {},
    }));
    for (const value of ["spawn_agent", "worker", "gpt-test", "medium", "thread child", "root/worker", "sender root", "Inspect this"])
        assert.match(agentMarkup, new RegExp(value, "u"));
    assert.match(agentMarkup, /<strong>Done<\/strong>/u);

    const files = itemCard("fileChanges", "files", {status: "completed", changes: [
        {path: "one.cpp", kind: "update", additions: 2, deletions: 1},
        {path: "two.cpp", kind: "create", additions: 3, deletions: 0},
    ]});
    const filesMarkup = renderToStaticMarkup(createElement(Card, {
        card: files, active: false, collapsed: false, onToggle() {}, onCopy() {},
    }));
    assert.match(filesMarkup, /Completed  \|  2 paths  \|  \+5 −1/u);

    const generic = itemCard("genericActivity", "generic", {type: "futureThing", raw: {text: "x".repeat(5000)}});
    assert.match(cardCopyContent(generic).text, /\[Activity details truncated\]$/u);
});
