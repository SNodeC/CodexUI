import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import {createElement} from "react";
import {renderToStaticMarkup} from "react-dom/server";
import {App} from "../dist/app/App.js";
import {BrowserFrontendSession} from "../dist/app/BrowserFrontendSession.js";
import {FakeSocket, readyProvider, requests, respond} from "./browser-harness.mjs";

const fixture = JSON.parse(readFileSync(new URL("../../tests/fixtures/project-order.json", import.meta.url), "utf8"));
const flush = async () => { for (let index = 0; index < 8; ++index) { await Promise.resolve(); } };
test("manual project order uses current server pages, including moves of already known projects", async () => {
    const socket = new FakeSocket(), session = new BrowserFrontendSession("ws://project-order.test", () => socket);
    try {
        session.browser.manualProjects = true;
        session.connect(); socket.open(); await readyProvider(socket, "project-order");
        let ids = [];
        for (const step of fixture.steps) {
            if (step.append) {
                session.loadPage(session.catalogPages().find(page => page.action === "projects.list"));
            } else if (ids.length) {
                session.refreshGroups();
            }
            await flush();
            const request = requests(socket, "project/list").at(-1);
            assert.equal(request.payload.params.sortKey, "position");
            if (!step.append) { ids = []; }
            ids.push(...step.ids);
            const data = step.ids.map(id => fixture.projects.find(project => project.id === id));
            respond(socket, request, {data, nextCursor: step.cursor ?? null}); await flush();
            for (const section of requests(socket, "threadSection/list")) {
                if (!socket.replied?.has(section.payload.id)) { respond(socket, section, {data: [], nextCursor: null}); }
            }
            await flush();
            assert.deepEqual(session.groups("project").map(project => project.id), ids, step.label);
            const markup = renderToStaticMarkup(createElement(App, {session}));
            const names = ids.map(id => fixture.projects.find(project => project.id === id).name);
            const positions = names.map(name => markup.indexOf(`aria-label="${name},`));
            assert.ok(positions.every(position => position >= 0), "every group is rendered");
            assert.deepEqual(positions, [...positions].sort((left, right) => left - right), step.label + ": visible order");
        }
    } finally { session.dispose(); }
});
