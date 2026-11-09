import assert from "node:assert/strict";
import test from "node:test";
import {readFileSync} from "node:fs";
import {createElement} from "react";
import {renderToStaticMarkup} from "react-dom/server";
import {asyncQuestionAnswer, asyncQuestionRequest, questionReplies, questionReplyText, questionReplyKey} from "../dist/app/PendingRequestResponses.js";
import {AsyncQuestionDialog, Card, cardCopyContent} from "../dist/app/App.js";
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
            assert.equal(asyncQuestionAnswer(request, {choice: "submit", input: entry.answer}, "question"), entry.wire);
            assert.equal(asyncQuestionAnswer(request, {choice: "decline", input: entry.answer}, "question"), undefined);
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
        assert.equal(asyncQuestionAnswer(request, {choice: "submit", input}, "question"), undefined);
    }
});

async function setup(entry = fixture.cases[0]) {
    const socket = new FakeSocket();
    const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
    session.connect(); socket.open(); await readyProvider(socket, "questions");
    respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "questions", status: {type: "idle"}}]});
    await completeThreadDiscovery(socket);
    session.selectThread("questions");
    await completeControllerHydration(socket, {id: "questions", status: {type: "idle"}});
    socket.receive(appserver({method: "turn/started", params: {threadId: "questions", turn: {id: "turn", status: "inProgress"}}}));
    const item = {type: "agentMessage", id: "question", phase: "final_answer", delivery: "async",
        text: entry.text, questions: entry.questions};
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
            assert.equal(request.payload.params.input[0].text, fixture.cases[0].wire);
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

for (const entry of [...fixture.cases.filter(entry => entry.wire).map(entry => ({...entry, source: entry.wire})), ...fixture.replyCases]) {
    test(`structured reply parsing and bold display: ${entry.id}`, () => {
        const replies = questionReplies(entry.source);
        assert.equal(replies.length, entry.replies?.length ?? entry.questions.length);
        if (replies.length) {
            assert.equal(questionReplyText(replies, true), entry.markdown);
            const card = {kind: "userMessage", payload: {text: entry.source, imagePaths: []}, status: {}, key: {kind: "item", threadId: "t", turnId: "u", itemId: "a"}};
            const markup = renderToStaticMarkup(createElement(Card, {card, collapsed: false, onToggle() {}}));
            assert.match(markup, /<strong>/u);
            assert.doesNotMatch(markup, /send_user_message_question_reply/u);
            assert.equal(cardCopyContent(card).text, entry.markdown, "Copy follows the readable reply presentation");
        }
    });
}

function historicalReply(entry, text = entry.wire, itemsFirst = false, threadId = "history") {
    const model = new PresentationModel();
    const normalizer = new ProtocolNormalizer(frame => { model.applyEvent(frame); return true; });
    const question = {id: "question", type: "agentMessage", phase: "commentary", delivery: "async", text: entry.text, questions: entry.questions};
    const answer = {id: "answer", type: "userMessage", content: [{type: "text", text}]};
    const items = itemsFirst ? [answer, question] : [question, answer];
    normalizer.operationResult("thread.read", threadId, {threadId}, {id: threadId, result: {thread: {
        id: threadId, turns: [{id: "turn", status: "completed", items}],
    }}});
    const snapshot = projectConversation(model.thread(threadId), [], 80, Date.now());
    return {model, normalizer, snapshot, question: snapshot.sections.flatMap(section => section.cards).find(card => card.itemId === "question")};
}

for (const entry of fixture.cases.filter(entry => entry.wire)) {
    for (const answerFirst of [false, true]) {
        test(`history restores explicit reply links: ${entry.id}, answer first ${answerFirst}`, () => {
            const {question} = historicalReply(entry, entry.wire, answerFirst);
            assert.equal(question.payload.questionStatus, "✓ Answer sent");
            assert.deepEqual(question.payload.answers, Object.values(entry.answer).map(value => value.answers[0]));
            assert.equal(question.payload.text, entry.markdown);
            const markup = renderToStaticMarkup(createElement(Card, {card: question, collapsed: false, onToggle() {}, onAnswer() {}}));
            assert.match(markup, /Question · ✓ Answer sent/u);
            assert.match(markup, /Answer again…/u);
        });
    }
    test(`unlinked legacy reply stays unknown: ${entry.id}`, () => {
        const {question} = historicalReply(entry, entry.text);
        assert.equal(question.payload.questionStatus, "");
        assert.ok(question.payload.answers.every(answer => answer === ""));
    });
}

for (const active of [true, false]) {
    test(`two-question ${active ? "steering" : "turn-start"} admission, failure and acknowledgement`, async () => {
        const entry = fixture.cases.find(entry => entry.id === "two-independent-choices");
        const {socket, session, card, key} = await setup(entry);
        const current = () => session.conversation().sections.flatMap(section => section.cards).find(card => card.itemId === "question");
        try {
            if (!active) socket.receive(appserver({method: "turn/completed", params: {threadId: "questions", turn: {id: "turn", status: "completed"}}}));
            for (const success of [false, true]) {
                assert.equal(await session.answerQuestion(card, key, {choice: "submit", input: entry.answer}), true);
                assert.equal(current().payload.questionStatus, "Sending…");
                assert.ok(session.conversation().sections.flatMap(section => section.cards).some(card => card.kind === "localPrompt"));
                await Promise.resolve(); await Promise.resolve();
                const request = requests(socket, active ? "turn/steer" : "turn/start").at(-1);
                assert.equal(request.payload.params.input[0].text, entry.wire);
                if (success) respond(socket, request, active ? {turnId: "turn"} : {turn: {id: "answer-turn", status: "inProgress"}});
                else socket.receive(appserver({id: request.payload.id, error: {code: -32603, message: "Rejected for test"}}));
                await Promise.resolve(); await Promise.resolve(); await Promise.resolve();
                assert.equal(current().payload.questionStatus, success ? "✓ Answer sent" : "");
            }
        } finally { session.dispose(); }
    });
}

test("maximum supported free-text replies round-trip without an invented wire-size restriction", () => {
    const questions = Array.from({length: 32}, () => ({title: "\u0001".repeat(4096)}));
    const input = Object.fromEntries(questions.map((_, index) => [String(index), {answers: ["\u0002".repeat(4096)]}]));
    const wire = asyncQuestionAnswer(asyncQuestionRequest(questions), {choice: "submit", input}, "question");
    assert.ok(wire.length > 1048576);
    const replies = questionReplies(wire);
    assert.equal(replies.length, 32);
    assert.ok(replies.every(reply => reply.answer === input[0].answers[0]));
});

for (const entry of fixture.replyCases.filter(entry => entry.replies.length === 0)) {
    test(`history never claims an answer from an invalid or unlinked reply: ${entry.id}`, () => {
        assert.equal(historicalReply(fixture.cases[0], entry.source).question.payload.questionStatus, "");
    });
}

test("a later reply from another client wins over an older acknowledged local submission", () => {
    const entry = fixture.cases.find(entry => entry.id === "two-independent-choices");
    const {model, normalizer} = historicalReply(entry);
    const corrected = questionReplies(entry.wire).map((reply, index) => ({...reply, answer: entry.questions[index].options[0]}));
    normalizer.serverNotification("item/completed", {threadId: "history", turnId: "turn", item: {
        id: "newer", type: "userMessage", content: [{type: "text", text: questionReplyText(corrected)}],
    }});
    const local = {id: 1, admissionOrdinal: 1, threadId: "history", clientUserMessageId: "old-local", prompt: entry.wire,
        attachments: [], turnOptions: {}, state: "accepted", admittedAtMilliseconds: 1, error: "", startsTurn: false,
        expectedTurnId: "turn", admissionAnchor: {kind: "item", threadId: "history", turnId: "turn", itemId: "question"}};
    const question = projectConversation(model.thread("history"), [local], 80, Date.now()).sections[0].cards.find(card => card.itemId === "question");
    assert.deepEqual(question.payload.answers, ["Approve", "Staging"]);
    assert.equal(question.payload.questionStatus, "✓ Answer sent");
});

test("authoritative echo confirms an async reply before a delayed RPC result", async () => {
    const {socket, session, card, key} = await setup();
    const current = () => session.conversation().sections.flatMap(section => section.cards).find(card => card.itemId === "question");
    try {
        assert.equal(await session.answerQuestion(card, key, {choice: "submit", input: fixture.cases[0].answer}), true);
        await Promise.resolve(); await Promise.resolve();
        const request = requests(socket, "turn/steer").at(-1);
        socket.receive(appserver({method: "item/completed", params: {threadId: "questions", turnId: "turn", item: {
            id: "reply", type: "userMessage", clientId: request.payload.params.clientUserMessageId,
            content: [{type: "text", text: request.payload.params.input[0].text}],
        }}}));
        assert.equal(current().payload.questionStatus, "✓ Answer sent");
        socket.receive(appserver({id: request.payload.id, error: {code: -32603, message: "Late response"}}));
        await Promise.resolve(); await Promise.resolve();
        assert.equal(current().payload.questionStatus, "✓ Answer sent");
    } finally { session.dispose(); }
});

test("a compatible client may answer one question at a time", () => {
    const entry = fixture.cases.find(entry => entry.id === "two-independent-choices");
    const {question} = historicalReply(entry, questionReplyText(questionReplies(entry.wire).slice(0, 1)));
    assert.deepEqual(question.payload.answers, ["Hold", ""]);
    assert.equal(question.payload.questionStatus, "1/2 answers sent");
});

test("changing a question title invalidates the previous answer for that question identity", () => {
    const entry = fixture.cases.find(entry => entry.id === "two-independent-choices");
    const replies = questionReplies(entry.wire).map(reply => ({...reply, question: "A different question"}));
    const {question} = historicalReply(entry, questionReplyText(replies));
    assert.equal(question.payload.questionStatus, "");
    assert.ok(question.payload.answers.every(answer => answer === ""));
});
