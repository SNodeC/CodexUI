import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {FakeSocket, appserver, requests, respond, reject, readyProvider, completeThreadDiscovery} from "./browser-harness.mjs";

const fixtures = JSON.parse(readFileSync(new URL("../../tests/fixtures/context-menu-creation.json", import.meta.url), "utf8"));
const settle = async () => { for (let pass = 0; pass < 12; ++pass) { await Promise.resolve(); } };
for (const {kind, failure} of fixtures) {
    test(`create and assign ${kind}: ${failure || "success"}`, async () => {
        const socket = new FakeSocket();
        const session = new BrowserFrontendSession("ws://bridge.test/", () => socket);
        const source = {id: "source", name: "Original", status: {type: "idle"}, projectId: "old-project", section: {id: "old-section", name: "Old"}};
        try {
            session.connect(); socket.open(); await readyProvider(socket, `create-${kind}-${failure}`);
            respond(socket, requests(socket, "thread/list").at(-1), {data: [source], nextCursor: null});
            await completeThreadDiscovery(socket); await settle();
            const target = {threadId: "source", presentationKey: session.threadVisualKey("source")};
            const parameters = {name: "Chosen name", ...(kind === "project" ? {roots: [], idempotencyKey: "unique-create-identity"} : {})};
            const saving = session.manageGroup(`${kind}.create`, parameters, undefined, target);
            const method = kind === "project" ? "project/create" : "threadSection/create";
            const create = requests(socket, method).at(-1);
            assert.deepEqual(create.payload.params, parameters, "UI assignment context is not sent as a creation parameter");
            if (failure === "create") {
                reject(socket, create, "creation denied");
            } else {
                if (failure === "retired-target") {
                    socket.receive(appserver({method: "thread/deleted", params: {threadId: "source"}}));
                }
                respond(socket, create, {[kind]: {id: "returned-id", name: "Chosen name", ...(kind === "project" ? {roots: []} : {})}});
            }
            await settle();
            const assignmentMethod = kind === "project" ? "thread/metadata/update" : "thread/section/move";
            const assignments = requests(socket, assignmentMethod);
            assert.equal(assignments.length, failure === "create" || failure === "retired-target" ? 0 : 1);
            if (assignments.length) {
                assert.equal(await saving, true, "creation completes independently while assignment is pending");
                const assignment = assignments[0];
                assert.deepEqual(assignment.payload.params, {threadId: "source", [`${kind}Id`]: "returned-id"}, "assignment uses the returned ID and original thread");
                assert.equal(session.model.thread("source")[`${kind}Id`], `old-${kind}`, "pending assignment does not overwrite authoritative membership");
                if (failure === "assign") {
                    reject(socket, assignment, "assignment denied");
                } else {
                    respond(socket, assignment, kind === "project" ? {thread: {...source, projectId: "returned-id"}} : {});
                    await settle();
                    if (kind === "section") {
                        const read = requests(socket, "thread/read").at(-1);
                        respond(socket, read, {thread: {...source, section: {id: "returned-id", name: "Chosen name"}}});
                    }
                }
            }
            assert.equal(await saving, failure !== "create", "successful creation closes its flow even when assignment fails");
            await settle();
            if (failure === "create" || failure === "assign") {
                assert.match(session.getSnapshot().notice, new RegExp(`${failure === "create" ? "creation" : "assignment"} denied`, "u"));
                assert.equal(session.model.thread("source")[`${kind}Id`], `old-${kind}`);
            } else if (failure === "retired-target") {
                assert.match(session.getSnapshot().notice, /Group created, but assignment could not be sent/u);
            } else {
                assert.equal(session.model.thread("source")[`${kind}Id`], "returned-id");
            }
            assert.equal(requests(socket, method).length, 1, "no automatic duplicate creation");
        } finally { session.dispose(); }
    });
}
