import assert from "node:assert/strict";
import test from "node:test";
import {readFileSync} from "node:fs";
import {createElement} from "react";
import {renderToStaticMarkup} from "react-dom/server";
import {asyncQuestionAnswer, asyncQuestionRequest} from "../dist/app/PendingRequestResponses.js";
import {AsyncQuestionDialog, Card} from "../dist/app/App.js";
import {PresentationModel, ProtocolNormalizer, projectConversation} from "../dist/index.js";
import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {FakeSocket, appserver, readyProvider, requests, respond,
    completeControllerHydration, completeThreadDiscovery} from "./browser-harness.mjs";

const fixture = JSON.parse(readFileSync(new URL("../../tests/fixtures/async-questions.json", import.meta.url)));
for (const entry of fixture.cases) {
    test(`async question contract: ${entry.id}`, () => {
        const request = asyncQuestionRequest(entry.questions);
        assert.equal(request === null, entry.text === undefined);
        if (entry.text !== undefined) {
            assert.equal(asyncQuestionAnswer(request, {choice: "submit", input: entry.answer}), entry.text);
            assert.equal(asyncQuestionAnswer(request, {choice: "decline", input: entry.answer}), undefined);
        }
    });
}

test("async question requests and answers remain bounded and require exactly one explicit answer per question", () => {
    for (const questions of [Array.from({length: 65}, () => ({title: "Question"})),
        [{title: "x".repeat(4097)}], [{title: "Question", options: Array(65).fill("choice")}],
        [{title: "Question", options: ["ü".repeat(2049)]}]]) {
        assert.equal(asyncQuestionRequest(questions), null);
    }
    const request = asyncQuestionRequest(fixture.cases[0].questions);
    for (const input of [{}, {0: {answers: []}}, {0: {answers: ["Approve", "Hold"]}},
        {0: {answers: ["Hold"]}, 1: {answers: ["unexpected"]}}]) {
        assert.equal(asyncQuestionAnswer(request, {choice: "submit", input}), undefined);
    }
});

async function setup() {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "questions");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "questions", status: {type: "idle"}}]});
    await completeThreadDiscovery(socket);
    session.selectThread("questions");
    await completeControllerHydration(socket, {id: "questions", status: {type: "idle"}});
    socket.receive(appserver({method: "turn/started", params: {threadId: "questions", turn: {id: "turn", status: "inProgress"}}}));
    const item = {type: "agentMessage", id: "question", phase: "final_answer", delivery: "async",
        text: "Approve the follow-up PR?\n- Approve\n- Hold", questions: fixture.cases[0].questions};
    socket.receive(appserver({method: "item/completed", params: {threadId: "questions", turnId: "turn", item}}));
    const card = session.conversation().sections.flatMap(section => section.cards).find(card => card.itemId === "question");
    assert.ok(card?.payload.questionRequest);
    return {socket, session, card, item, key: session.threadVisualKey("questions")};
}

test("async questions render an Answer action and reuse the existing form with radio choices and a free-text field", async () => {
    const {session, card, key} = await setup();
    try {
        const markup = renderToStaticMarkup(createElement(Card, {card, active: true, collapsed: false, onToggle() {}, onAnswer() {}}));
        assert.match(markup, />Answer…<\/button>/u);
        const dialog = renderToStaticMarkup(createElement(AsyncQuestionDialog, {session, card, presentationKey: key, onClose() {}}));
        assert.match(dialog, /<dialog[^>]*aria-label="Codex needs input"/u);
        assert.match(dialog, /type="radio"[^>]*checked=""/u);
        assert.match(dialog, /Other answer/u);
        assert.doesNotMatch(dialog, />Decline<|Return unsupported/u);
    } finally { session.dispose(); }
});

for (const active of [true, false]) {
    test(`a chosen async answer uses ordinary ${active ? "steering" : "turn start"} without resolving a server request`, async () => {
        const {socket, session, card, key} = await setup();
        try {
            if (!active) socket.receive(appserver({method: "turn/completed", params: {threadId: "questions", turn: {id: "turn", status: "completed"}}}));
            assert.equal(await session.answerQuestion(card, key, {choice: "submit", input: {0: {answers: ["Hold"]}}}), true);
            await Promise.resolve(); await Promise.resolve();
            const request = requests(socket, active ? "turn/steer" : "turn/start").at(-1);
            assert.ok(request);
            assert.equal(request.payload.params.threadId, "questions");
            assert.equal(request.payload.params.input[0].text, fixture.cases[0].text);
            assert.equal(socket.sent.some(frame => frame.kind === "appserver" && frame.payload.result), false);
            if (active) assert.equal(request.payload.params.expectedTurnId, "turn");
        } finally { session.dispose(); }
    });
}

for (const scenario of ["changed", "removed", "thread-switch", "observer", "disconnect", "invalid", "decline"]) {
    test(`async answer refuses ${scenario} without sending or claiming approval`, async () => {
        const {socket, session, card, item, key} = await setup();
        try {
            if (scenario === "changed") socket.receive(appserver({method: "item/completed", params: {
                threadId: "questions", turnId: "turn", item: {...item, questions: [{title: "A different question"}]},
            }}));
            if (scenario === "removed") session.model.thread("questions").turns.get("turn").items.delete("question");
            if (scenario === "thread-switch") session.selectThread("");
            if (scenario === "observer") socket.receive({kind: "bridge.controller", controllerConnectionId: "other", role: "observer"});
            if (scenario === "disconnect") session.disconnect();
            const before = socket.sent.length;
            assert.equal(await session.answerQuestion(card, key, {
                choice: scenario === "decline" ? "decline" : "submit", input: scenario === "invalid" ? {} : {0: {answers: ["Hold"]}},
            }), false);
            assert.equal(socket.sent.length, before);
        } finally { session.dispose(); }
    });
}

for (const entry of fixture.cases) {
    test(`historical question projection: ${entry.id}`, () => {
        const model = new PresentationModel();
        const normalizer = new ProtocolNormalizer(frame => { model.applyEvent(frame); return true; });
        normalizer.operationResult("thread.read", "history", {threadId: "history"}, {id: "history", result: {thread: {
            id: "history", turns: [{id: "turn", status: "completed", items: [{id: "question", type: "agentMessage",
                phase: "final_answer", delivery: "async", text: "Retained question", questions: entry.questions}]}],
        }}});
        const card = projectConversation(model.thread("history"), [], 80, Date.now()).sections[0].cards[0];
        assert.equal(card.kind, "agentMessage");
        assert.equal(card.payload.questionRequest === null, entry.text === undefined);
    });
}
