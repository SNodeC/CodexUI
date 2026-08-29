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
