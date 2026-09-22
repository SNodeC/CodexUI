import assert from "node:assert/strict";
import {performance} from "node:perf_hooks";
import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {ConversationViewportState, event, result} from "../dist/index.js";

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

const presentationState = new ConversationViewportState();
started = performance.now();
for (let index = 0; index < 10_000; ++index)
    presentationState.bind(`presentation-${index}`, {});
for (let index = 0; index < 10_000; ++index)
    presentationState.retire(`presentation-${index}`);
const presentationChurnMilliseconds = performance.now() - started;
session.dispose();

const streamedText = thread?.turns.get("turn-99")?.items.get("item-99-99")?.raw.text;
const measurements = {
    nodeVersion: process.version,
    authoritativeItems: [...(thread?.turns.values() ?? [])].reduce((count, turn) => count + turn.items.size, 0),
    visibleCards: projection.sections.reduce((count, section) => count + section.cards.length, 0),
    streamedDeltas: typeof streamedText === "string" ? streamedText.length - "Answer 99".length : 0,
    presentationLifetimes: 10_000,
    hydrateMilliseconds,
    projectMilliseconds,
    streamMilliseconds,
    presentationChurnMilliseconds,
};
const limits = {hydrateMilliseconds: 47, projectMilliseconds: 45, streamMilliseconds: 4,
    presentationChurnMilliseconds: 20};
process.stdout.write(`${JSON.stringify({...measurements, limits}, null, 2)}\n`);
const failures = Object.entries(limits)
    .filter(([name, limit]) => !(measurements[name] <= limit))
    .map(([name, limit]) => `${name} ${measurements[name]} ms exceeds ${limit} ms`);
if (measurements.authoritativeItems !== 10_000) failures.push("Hydration did not retain 10,000 items");
if (measurements.visibleCards !== 10_000) failures.push("Projection did not present 10,000 cards");
if (streamedText !== `Answer 99${"x".repeat(2_000)}`) failures.push("Streaming did not append all 2,000 deltas");
if (presentationState.retainedPresentationCount() !== 0) failures.push("Retired presentation state was retained");
assert.deepEqual(failures, [], "Presentation profile failed");
