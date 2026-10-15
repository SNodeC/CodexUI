import assert from "node:assert/strict";
import test from "node:test";
import {readFileSync} from "node:fs";
import {createElement} from "react";
import {renderToStaticMarkup} from "react-dom/server";
import {App, Card, GroupDialog} from "../dist/app/App.js";
import {BrowserFrontendSession, DraftThreadId} from "../dist/app/BrowserFrontendSession.js";
import {ConversationViewportState} from "../dist/app/ConversationViewportState.js";
import {PresentationModel, ProtocolNormalizer, event, result, statusFromValue} from "../dist/index.js";
import {protocolTimes, protocolFrameTimes, targetTimes, timingDetails, timingSummary, tokenUsage} from "../dist/presentation/PresentationTelemetry.js";
import {generatedTimingPaths} from "../tools/generate-timing-paths.mjs";
import {FakeSocket, appserver, requests, respond, reject, rejectWithCode, readyProvider, completeControllerHydration, waitForPublish} from "./browser-harness.mjs";

const contract = JSON.parse(readFileSync(new URL("../../tests/fixtures/frontend-presentation.json", import.meta.url)));
const flush = async () => { for (let index = 0; index < 8; index++) await Promise.resolve(); };
async function setup(t, role = "controller") {
    const socket = new FakeSocket(), session = new BrowserFrontendSession("ws://catchup.test/codex", () => socket);
    t.after(() => session.dispose()); session.connect(); socket.open(); await readyProvider(socket, "catchup", role);
    return {socket, session};
}
async function catalogs(socket, projects = [], sections = []) {
    respond(socket, requests(socket, "project/list").at(-1), {data: projects, nextCursor: null}); await flush();
    respond(socket, requests(socket, "threadSection/list").at(-1), {data: sections, nextCursor: null}); await flush();
}
function notification(socket, method, params) { socket.receive(appserver({jsonrpc: "2.0", method, params})); }

for (const entry of contract.timingCases) test(`shared timing: ${entry.id}`, () => {
    assert.deepEqual(protocolFrameTimes(entry.method, entry.body, entry.response), entry.expected);
});
for (const entry of contract.tokenUsageCases) test(`shared token usage: ${entry.id}`, () => {
    const model = new PresentationModel();
    model.applyEvent(result(1, 1, "threads.list", "list", true, {threads: entry.threads}));
    if (entry.account) model.applyEvent(result(2, 1, "account.usage.read", "account", true, entry.account));
    const usage = tokenUsage(model, entry.selected);
    assert.equal(usage.thread.summary, entry.threadSummary); assert.equal(usage.overall.summary, entry.overallSummary);
    for (const detail of entry.threadDetails) assert.ok(usage.thread.details.includes(detail));
    for (const detail of entry.overallDetails) assert.ok(usage.overall.details.includes(detail));
});
test("timing schema is generated from the native authority and extraction stays bounded", () => {
    assert.equal(readFileSync(new URL("../src/presentation/TimingPaths.ts", import.meta.url), "utf8"), generatedTimingPaths());
    assert.equal(protocolFrameTimes("thread/list", {data: Array.from({length: 10000}, () => ({createdAt: 1, updatedAt: 2}))}, true).length, 128);
    assert.deepEqual(protocolTimes({payload: {startedAt: 10}}, "ItemLifecycle"), []);
    assert.match(timingDetails(protocolTimes({createdAt: 0}, "Thread")), /1970-01-01T00:00:00.000Z/u);
    assert.match(timingDetails(protocolTimes({createdAtMs: 0}, "FsGetMetadataResponse")), /protocol sentinel/u);
    assert.match(timingDetails(protocolTimes({durationMs: -1}, "Turn")), /Invalid duration/u);
    assert.match(timingDetails(protocolTimes({startedAt: 999999999999999}, "Turn")), /outside supported/u);
    assert.equal(timingSummary(protocolTimes({durationMs: 1234}, "Turn")), "1.234 s");
});
test("live token notifications replace snapshots and provider replacement retires telemetry", () => {
    const model = new PresentationModel(), normalizer = new ProtocolNormalizer(frame => model.applyEvent(frame));
    normalizer.serverNotification("thread/tokenUsage/updated", {threadId: "t", tokenUsage: {total: {totalTokens: 100}}});
    normalizer.serverNotification("thread/tokenUsage/updated", {threadId: "t", tokenUsage: {total: {totalTokens: 200}}});
    assert.match(tokenUsage(model, "t").thread.summary, /^Tokens 200/u);
    normalizer.bridgeEvent({kind: "bridge.provider", state: "ready", providerGeneration: 1});
    normalizer.bridgeEvent({kind: "bridge.provider", state: "ready", providerGeneration: 2});
    assert.match(tokenUsage(model, "t").thread.summary, /^Tokens 0/u);
});
test("card timing preserves turn and item lifecycle fields independently of text updates", () => {
    const model = new PresentationModel(), normalizer = new ProtocolNormalizer(frame => model.applyEvent(frame));
    normalizer.serverNotification("turn/started", {threadId: "t", turn: {id: "turn", startedAt: 100}});
    normalizer.serverNotification("item/started", {threadId: "t", turnId: "turn", startedAtMs: 100001, item: {id: "item", type: "commandExecution", command: "true"}});
    normalizer.serverNotification("item/commandExecution/outputDelta", {threadId: "t", turnId: "turn", itemId: "item", delta: "ok"});
    normalizer.serverNotification("item/completed", {threadId: "t", turnId: "turn", completedAtMs: 100020, item: {id: "item", type: "commandExecution", durationMs: 19}});
    const times = targetTimes(model, {threadId: "t", turnId: "turn", itemId: "item"});
    assert.equal(times.find(time => time.field === "startedAtMs").value, 100001);
    assert.equal(times.find(time => time.field === "completedAtMs").value, 100020);
    assert.equal(times.find(time => time.field === "turn/startedAt").value, 100);
});
test("search invalidates old pages before they can update entities or visible results", async t => {
    const {socket, session} = await setup(t); const obsolete = requests(socket, "thread/list").at(-1);
    session.changeBrowser({search: "新しい"}); const current = requests(socket, "thread/list").at(-1);
    assert.equal(current.payload.params.searchTerm, "新しい");
    respond(socket, obsolete, {data: [{id: "obsolete", name: "Wrong"}], nextCursor: "wrong"});
    respond(socket, current, {data: [{id: "match", name: "新しい"}], nextCursor: "next"}); await flush();
    assert.equal(session.model.thread("obsolete"), undefined); assert.deepEqual(session.threadOrder(), ["match"]);
    await waitForPublish(); respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "match", name: "新しい"}], nextCursor: "next"}); await flush();
    session.loadMoreThreads(); assert.equal(requests(socket, "thread/list").at(-1).payload.params.cursor, "next");
});
test("All uses separate active/archived queries and section order keeps active first", async t => {
    const {socket, session} = await setup(t); respond(socket, requests(socket, "thread/list").at(-1), {data: []}); await flush();
    session.changeBrowser({archive: "all", sort: "section"});
    const active = requests(socket, "thread/list").findLast(request => request.payload.params.sortKey === "section_position" && !request.payload.params.archived);
    respond(socket, active, {data: [{id: "active"}], nextCursor: null}); await flush();
    const archived = requests(socket, "thread/list").findLast(request => request.payload.params.archived === true);
    assert.ok(archived); respond(socket, archived, {data: [{id: "archived"}], nextCursor: null}); await flush();
    assert.deepEqual(session.threadOrder("section"), ["active", "archived"]);
    assert.equal(session.model.thread("archived").archived, true);
});
test("unsupported catalogs fall back once per connection without disabling thread browsing", async t => {
    const {socket, session} = await setup(t); const project = requests(socket, "project/list").at(-1);
    rejectWithCode(socket, project, -32601, "unsupported"); await flush();
    rejectWithCode(socket, requests(socket, "threadSection/list").at(-1), -32601, "unsupported"); await flush();
    session.refreshGroups(); session.refreshBrowser(); await flush();
    assert.equal(requests(socket, "project/list").length, 1); assert.equal(session.groupingAvailable(), false);
});
test("group refresh removes externally deleted section presentation without deleting its threads", async t => {
    const {socket, session} = await setup(t); await catalogs(socket, [], [{id: "s", name: "Empty"}]);
    assert.deepEqual(session.groups("section").map(group => group.id), ["s"]);
    session.refreshGroups(); respond(socket, requests(socket, "project/list").at(-1), {data: []}); await flush();
    respond(socket, requests(socket, "threadSection/list").at(-1), {data: []}); await flush();
    assert.deepEqual(session.groups("section"), []);
});
test("project edits preserve freshly read foreign metadata and recheck controller ownership", async t => {
    const {socket, session} = await setup(t); await catalogs(socket, [{id: "p", name: "Project"}], []);
    const editing = session.manageGroup("project.update", {projectId: "p", name: "Renamed", roots: [{path: "/w"}], description: "Description"});
    respond(socket, requests(socket, "project/read").at(-1), {project: {id: "p", metadata: {foreign: {retained: true}, "codexui.description": "old"}}}); await flush();
    const update = requests(socket, "project/update").at(-1);
    assert.deepEqual(update.payload.params.metadata, {foreign: {retained: true}, "codexui.description": "Description"});
    assert.equal(Object.hasOwn(update.payload.params, "description"), false);
    respond(socket, update, {project: {id: "p", name: "Renamed"}}); assert.equal(await editing, true);
    const second = session.manageGroup("project.update", {projectId: "p", description: "lost control"});
    socket.receive({kind: "bridge.controller", event: "released", controllerConnectionId: null, role: "observer"});
    respond(socket, requests(socket, "project/read").at(-1), {project: {id: "p", metadata: {}}}); await flush();
    assert.equal(await second, false); assert.equal(requests(socket, "project/update").length, 1);
});
test("group mutations are single-flight and observers cannot send them", async t => {
    const {socket, session} = await setup(t, "observer");
    assert.equal(await session.manageGroup("project.create", {name: "No"}), false);
    assert.equal(requests(socket, "project/create").length, 0);
    socket.receive({kind: "bridge.controller", event: "claimed", controllerConnectionId: "catchup", role: "controller"});
    const first = session.manageGroup("section.create", {name: "Section"});
    assert.equal(await session.manageGroup("section.create", {name: "Duplicate"}), false);
    respond(socket, requests(socket, "threadSection/create").at(-1), {section: {id: "s", name: "Section"}});
    assert.equal(await first, true); assert.equal(requests(socket, "threadSection/create").length, 1);
});
test("newer project/archive notifications survive an older list snapshot", async t => {
    const {socket, session} = await setup(t); const list = requests(socket, "thread/list").at(-1);
    notification(socket, "thread/started", {thread: {id: "t", projectId: "first", section: {id: "s", name: "Section"}}});
    notification(socket, "thread/project/updated", {threadId: "t", projectId: "new"});
    notification(socket, "thread/archived", {threadId: "t"});
    respond(socket, list, {data: [{id: "t", projectId: "old", archived: false}]}); await flush();
    assert.equal(session.model.thread("t").projectId, "new"); assert.equal(session.model.thread("t").archived, true);
    assert.equal(session.model.thread("t").sectionId, "s");
});
test("same-membership children nest while cross-membership children remain discoverable", async t => {
    const {socket, session} = await setup(t);
    respond(socket, requests(socket, "thread/list").at(-1), {data: [{id: "parent", projectId: "p"}, {id: "same", parentThreadId: "parent", projectId: "p"}, {id: "other", parentThreadId: "parent", projectId: "q"}]}); await flush();
    assert.deepEqual(new Set(session.threadOrder()), new Set(["parent", "other"]));
    session.changeBrowser({grouping: "ungrouped"}); assert.deepEqual(session.threadOrder(), ["parent"]);
});
test("grouped creation sends project context and section failure retains the thread and prompt", async t => {
    const {socket, session} = await setup(t); await catalogs(socket);
    session.beginNewThread({workspace: "/w", name: "", baseInstructions: "", developerInstructions: "", ephemeral: false, projectId: "p", sectionId: "s"});
    const sending = session.submitPrompt("first"); const create = requests(socket, "thread/start").at(-1);
    assert.equal(create.payload.params.projectId, "p");
    respond(socket, create, {thread: {id: "created", projectId: "p"}}); await flush();
    reject(socket, requests(socket, "thread/section/move").at(-1), "assignment failed"); await sending; await flush();
    assert.equal(session.getSnapshot().selectedThreadId, "created"); assert.ok(session.model.thread("created"));
    assert.equal(requests(socket, "thread/start").length, 1); assert.equal(requests(socket, "turn/start").length, 1);
    assert.match(session.getSnapshot().notice, /assignment failed/u);
});
test("explicit prompt restoration retains group context without sending or overwriting authored text", async t => {
    const {socket, session} = await setup(t);
    session.beginNewThread({workspace: "/w", name: "Original", baseInstructions: "Base", developerInstructions: "", ephemeral: false, projectId: "p", sectionId: "s"});
    const sending = session.submitPrompt("  preserved\n\nexact  "); reject(socket, requests(socket, "thread/start").at(-1), "offline"); await sending;
    const key = session.threadVisualKey(DraftThreadId), submission = session.prompts.submissions(DraftThreadId)[0];
    assert.equal(session.restorePrompt(DraftThreadId, submission.id, key, "current draft"), undefined);
    const restored = session.restorePrompt(DraftThreadId, submission.id, key, "");
    assert.equal(restored, "  preserved\n\nexact  "); assert.equal(session.getSnapshot().newThreadDraft.projectId, "p");
    assert.equal(session.getSnapshot().newThreadDraft.sectionId, "s"); assert.equal(requests(socket, "thread/start").length, 1);
    assert.notEqual(session.threadVisualKey(DraftThreadId), key);
    assert.equal(session.restorePrompt(DraftThreadId, submission.id, key, ""), undefined);
});
test("code scroll state belongs to exact presentation identity and survives promotion", () => {
    const viewport = new ConversationViewportState(), owner = {};
    const key = viewport.bind("draft", owner), output = viewport.codeScroll("draft", key, "output", true);
    output.scrollTop = 50; output.following = false;
    assert.equal(viewport.codeScroll("draft", key, "command", false).following, false);
    viewport.promote("draft", "real", owner); assert.equal(viewport.codeScroll("real", key, "output", true), output);
    const fresh = viewport.bind("real", {}); assert.notEqual(fresh, key);
    assert.deepEqual(viewport.codeScroll("real", fresh, "output", true), {scrollTop: 0, following: true});
});
test("grouped UI contains empty groups, one representation per thread, and full controls", () => {
    const session = new BrowserFrontendSession();
    session.model.applyEvent(result(1, 1, "projects.list", "p", true, {data: [{id: "p", name: "Project"}]}));
    session.model.applyEvent(result(2, 1, "sections.list", "s", true, {data: [{id: "s", name: "Section"}, {id: "empty", name: "Empty section"}]}));
    session.model.applyEvent(result(3, 1, "threads.list", "t", true, {threads: [{id: "t", name: "Only once", projectId: "p", section: {id: "s"}}]}));
    const markup = renderToStaticMarkup(createElement(App, {session}));
    assert.equal((markup.match(/<span class="thread-title">Only once<\/span>/gu) ?? []).length, 1);
    for (const text of ["Empty section", "New project…", "New section…", "Thread archive filter", "Search thread titles", "Section order", "New file changes cards start collapsed", "Overall tokens", "Resize Threads", "Hide Inspector"]) assert.ok(markup.includes(text), text);
    session.dispose();
});

test("a deleted group cannot be resurrected by an older catalog response", async t => {
    const {socket, session} = await setup(t);
    const old = requests(socket, "project/list").at(-1);
    notification(socket, "project/changed", {projectId: "p", changeType: "deleted"});
    respond(socket, old, {data: [{id: "p", name: "Stale"}]}); await flush();
    assert.equal(session.model.projects.has("p"), false);
    const fresh = requests(socket, "project/list").at(-1);
    if (fresh !== old) { respond(socket, fresh, {data: []}); await flush(); }
    assert.equal(session.model.projects.has("p"), false);
});
test("removed and recreated groups reject commands from an old details dialog", async t => {
    const {socket, session} = await setup(t); await catalogs(socket, [{id: "p", name: "First"}], []);
    const original = session.model.projects.get("p");
    const deleting = session.manageGroup("project.delete", {projectId: "p"});
    respond(socket, requests(socket, "project/delete").at(-1), {}); assert.equal(await deleting, true);
    const creating = session.manageGroup("project.create", {name: "Replacement", roots: []});
    respond(socket, requests(socket, "project/create").at(-1), {project: {id: "p", name: "Replacement"}}); assert.equal(await creating, true);
    assert.notEqual(session.model.projects.get("p"), original);
    const reads = requests(socket, "project/read").length;
    assert.equal(await session.manageGroup("project.update", {projectId: "p", description: "stale"}, original), false);
    assert.equal(requests(socket, "project/read").length, reads);
});
test("account refresh failure identifies and preserves its last successful value", () => {
    const model = new PresentationModel();
    model.applyEvent(result(1, 1, "account.usage.read", "success", true, {summary: {lifetimeTokens: 42}}));
    model.applyEvent(result(2, 1, "account.usage.read", "failure", false, {message: "offline"}));
    const details = tokenUsage(model, "").overall.details;
    assert.match(details, /Account lifetime tokens: 42/u); assert.match(details, /last successful snapshot/u);
});
test("metadata assignment and section ordering use separate protocol operations", async t => {
    const {socket, session} = await setup(t); notification(socket, "thread/started", {thread: {id: "t"}});
    const assigning = session.assignThreadGroup("t", "project", "p");
    const project = requests(socket, "thread/metadata/update").at(-1);
    assert.deepEqual(project.payload.params, {threadId: "t", projectId: "p"});
    respond(socket, project, {thread: {id: "t", projectId: "p"}}); assert.equal(await assigning, true);
    const moving = session.assignThreadGroup("t", "section", "s", "before");
    const section = requests(socket, "thread/section/move").at(-1);
    assert.deepEqual(section.payload.params, {threadId: "t", sectionId: "s", beforeThreadId: "before"});
    respond(socket, section, {}); await flush();
    const metadata = requests(socket, "thread/read").at(-1); assert.equal(metadata.payload.params.includeTurns, false);
    respond(socket, metadata, {thread: {id: "t", projectId: "p", section: {id: "s"}}});
    assert.equal(await moving, true); assert.equal(session.model.thread("t").sectionId, "s");
});
test("paged catalogs retain earlier members and retry the same failed cursor", async t => {
    const {socket, session} = await setup(t);
    respond(socket, requests(socket, "project/list").at(-1), {data: [{id: "p1", name: "First"}], nextCursor: "next"}); await flush();
    const page = session.catalogPages().find(page => page.action === "projects.list"); session.loadPage(page);
    respond(socket, requests(socket, "threadSection/list").at(-1), {data: []}); await flush();
    const next = requests(socket, "project/list").at(-1); assert.equal(next.payload.params.cursor, "next");
    reject(socket, next, "temporary"); await flush(); session.loadPage(page);
    const retry = requests(socket, "project/list").at(-1); assert.equal(retry.payload.params.cursor, "next");
    respond(socket, retry, {data: [{id: "p2", name: "Second"}], nextCursor: null}); await flush();
    assert.deepEqual(new Set(session.groups("project").map(group => group.id)), new Set(["p1", "p2"]));
});
test("group creation forms disclose descriptions and section scope honestly", () => {
    const session = new BrowserFrontendSession();
    const project = renderToStaticMarkup(createElement(GroupDialog, {session, kind: "project", readOnly: false, onClose() {}}));
    const section = renderToStaticMarkup(createElement(GroupDialog, {session, kind: "section", readOnly: false, onClose() {}}));
    assert.match(project, /not model instructions/u); assert.match(project, /absolute app-server path/u);
    assert.match(section, /Sections are shared across projects/u); assert.match(section, /Colour/u);
    session.dispose();
});

test("changing filters retires the pending repair and reconciles the new query", async t => {
    const {socket, session} = await setup(t);
    respond(socket, requests(socket, "thread/list").at(-1), {data: []}); await flush();
    session.changeBrowser({search: "replacement"});
    const fast = requests(socket, "thread/list").at(-1); respond(socket, fast, {data: []}); await waitForPublish();
    const repair = requests(socket, "thread/list").at(-1);
    assert.equal(repair.payload.params.searchTerm, "replacement"); assert.equal(repair.payload.params.useStateDbOnly, false);
});
test("manual project sorting retires an old catalog read before admitting its replacement", async t => {
    const {socket, session} = await setup(t); const old = requests(socket, "project/list").at(-1);
    session.changeBrowser({manualProjects: true}); await flush();
    const current = requests(socket, "project/list").at(-1);
    assert.notEqual(current, old); assert.equal(current.payload.params.sortKey, "position");
    respond(socket, old, {data: [{id: "old", name: "Stale"}]});
    respond(socket, current, {data: [{id: "new", name: "Current", position: 1}]}); await flush();
    assert.equal(session.model.projects.has("old"), false); assert.equal(session.model.projects.has("new"), true);
});
