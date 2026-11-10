import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";

import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {FakeSocket, appserver, requests, respond, reject, readyProvider, completeThreadDiscovery} from "./browser-harness.mjs";

const fixtures = JSON.parse(readFileSync(new URL("../../tests/fixtures/fork-membership.json", import.meta.url), "utf8"));

async function settle() {
    for (let pass = 0; pass < 8; ++pass) {
        await Promise.resolve();
    }
}

for (const fixture of fixtures) {
    test(`fork membership: ${fixture.name}`, async () => {
        const socket = new FakeSocket();
        const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
        const section = id => id ? {id, name: id} : null;
        const source = {
            id: "source", name: "Find Slides", cwd: "/source-workspace",
            projectId: fixture.projectId, section: section(fixture.sectionId),
            forkedFromId: null, parentThreadId: null, status: {type: "idle"},
            turns: ["first", "middle", "last"].map(id => ({id, status: "completed", items: []})),
        };
        const forked = {
            id: "forked", name: "Provider title", cwd: "/fork-workspace",
            projectId: fixture.forkProjectId, section: section(fixture.forkSectionId),
            forkedFromId: "source", parentThreadId: null, status: {type: "idle"}, turns: [],
        };
        try {
            session.connect();
            socket.open();
            await readyProvider(socket, `fork-membership-${fixture.name}`);
            respond(socket, requests(socket, "thread/list").at(-1), {data: [source], nextCursor: null});
            await completeThreadDiscovery(socket);
            await settle();

            session.forkThread("source", fixture.options ? {
                workspace: "/fork-workspace", name: "Chosen fork",
                baseInstructions: "Fork base", developerInstructions: "Fork developer", ephemeral: fixture.ephemeral ?? false,
            } : undefined, session.threadVisualKey("source"), fixture.lastTurnId, session.model.thread("source")?.turns.get(fixture.lastTurnId));
            const fork = requests(socket, "thread/fork").at(-1);
            assert.ok(fork, "the fork is submitted");
            assert.equal(fork.payload.params.threadId, "source");
            assert.equal(fork.payload.params.lastTurnId, fixture.lastTurnId);
            assert.deepEqual(session.model.thread("source").turnOrder, ["first", "middle", "last"], "forking preserves the source history");
            assert.equal(fork.payload.params.cwd, fixture.options ? "/fork-workspace" : undefined);
            if (fixture.moveSource) {
                socket.receive(appserver({jsonrpc: "2.0", method: "thread/started", params: {thread: {
                    ...source, projectId: "moved-project", section: section("moved-section"),
                }}}));
            }
            respond(socket, fork, {thread: forked});
            await settle();

            assert.equal(await session.submitPrompt("answer the fork while assignments are pending"), true);
            const prompt = requests(socket, "turn/start").at(-1);
            assert.equal(prompt?.payload.params.threadId, "forked");
            respond(socket, prompt, {turn: {id: "fork-turn", status: "completed", items: []}});
            const assignments = [
                ["project", "thread/metadata/update", fixture.projectId, fixture.forkProjectId],
                ["section", "thread/section/move", fixture.sectionId, fixture.forkSectionId],
            ];
            if (fixture.reverse) {
                assignments.reverse();
            }
            for (const [kind, method, wanted, original] of assignments) {
                const changes = requests(socket, method).filter(value => value.payload.params.threadId === "forked");
                assert.equal(changes.length, wanted === original ? 0 : 1, `${kind} assignment is sent exactly when required`);
                if (changes.length) {
                    const change = changes[0];
                    assert.deepEqual(change.payload.params, {threadId: "forked", [`${kind}Id`]: wanted || (kind === "project" ? "" : null)});
                    if (fixture.reject === kind) {
                        reject(socket, change, `fixture ${kind} assignment failed`);
                    } else {
                        if (kind === "project") {
                            forked.projectId = wanted;
                            respond(socket, change, {thread: {...forked, section: section(fixture.forkSectionId)}});
                        } else {
                            forked.section = section(wanted);
                            respond(socket, change, {});
                        }
                    }
                    await settle();
                    for (const read of requests(socket, "thread/read").filter(value =>
                        value.payload.params.threadId === "forked" && !socket.replied.has(value.payload.id))) {
                        respond(socket, read, {thread: forked});
                    }
                    await settle();
                }
            }
            const expectedProject = fixture.reject === "project" ? fixture.forkProjectId : fixture.projectId;
            const expectedSection = fixture.reject === "section" ? fixture.forkSectionId : fixture.sectionId;
            assert.equal(session.model.thread("forked").projectId, expectedProject);
            assert.equal(session.model.thread("forked").sectionId, expectedSection);
            assert.equal(session.model.childOwnership("forked")?.parentThreadId, source.id);
            assert.equal(session.getSnapshot().selectedThreadId, "forked");
            assert.equal(requests(socket, "thread/resume").length, 0, "assignments do not gate the first prompt or reload history");
            if (fixture.reject) {
                assert.match(session.getSnapshot().notice, new RegExp(`fixture ${fixture.reject} assignment failed`, "u"));
            }
            const count = socket.sent.length;
            await settle();
            assert.equal(socket.sent.length, count, "no failed mutation is automatically repeated");
        } finally {
            session.dispose();
        }
    });
}
