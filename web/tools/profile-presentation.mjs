import {performance} from "node:perf_hooks";
import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {event, result} from "../dist/index.js";

const turns = Array.from({length: 100}, (_, turn) => ({
    id: `turn-${turn}`, status: turn === 99 ? "inProgress" : "completed",
    items: Array.from({length: 100}, (_, item) => ({
        id: `item-${turn}-${item}`, type: item % 3 === 0 ? "agentMessage" : item % 3 === 1 ? "reasoning" : "commandExecution",
        text: item % 3 === 0 ? `Answer ${item}` : undefined, summary: item % 3 === 1 ? ["Thinking"] : undefined,
        command: item % 3 === 2 ? "true" : undefined, status: "completed",
    })),
}));
const session = new BrowserFrontendSession("ws://profile.invalid/", () => { throw new Error("profile does not connect"); });
const model = session.model;
let started = performance.now();
model.applyEvent(result(1, 1, "thread.read", "profile-read", true, {
    thread: {id: "profile", status: {type: "active"}, turns},
}, "replace", {threadId: "profile"}));
const hydrateMilliseconds = performance.now() - started;

const thread = model.thread("profile");
session.selectThread("profile");
started = performance.now();
const projection = session.conversation(10_000);
const projectMilliseconds = performance.now() - started;

started = performance.now();
for (let sequence = 2; sequence < 2_002; ++sequence) model.applyEvent(event(
    sequence, 1, "conversation.item.append", {field: "text", text: "x"}, "merge",
    {threadId: "profile", turnId: "turn-99", itemId: "item-99-99"},
));
const streamMilliseconds = performance.now() - started;
session.dispose();

process.stdout.write(`${JSON.stringify({
    authoritativeItems: 10_000,
    visibleCards: projection.sections.reduce((count, section) => count + section.cards.length, 0),
    streamedDeltas: 2_000,
    hydrateMilliseconds: Number(hydrateMilliseconds.toFixed(2)),
    projectMilliseconds: Number(projectMilliseconds.toFixed(2)),
    streamMilliseconds: Number(streamMilliseconds.toFixed(2)),
}, null, 2)}\n`);
