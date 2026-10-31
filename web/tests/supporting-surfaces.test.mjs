import assert from "node:assert/strict";
import test from "node:test";
import {createElement} from "react";
import {renderToStaticMarkup} from "react-dom/server";
import {NewThreadDialog, lastActivityText} from "../dist/app/App.js";

import {
    fixedSettingChoices, pendingRequestDetails, settingDraftFor, settingPresentation, turnSettingCatalog,
} from "../dist/index.js";

test("activity formatting preserves local time, dates and invalid-date behavior", () => {
    const now = new Date(2026, 8, 22, 12, 0, 0);
    for (const date of [new Date(2026, 8, 22, 1, 2, 3), new Date(2026, 8, 21, 23, 59, 59)]) {
        const time = date.toLocaleTimeString([], {hour: "2-digit", minute: "2-digit", second: "2-digit"});
        assert.equal(lastActivityText(date.getTime() / 1000, now),
            `Last activity: ${date.getDate() === now.getDate() ? time : `${date.toLocaleDateString()} ${time}`}`);
    }
    const invalid = new Date(Number.NaN);
    assert.equal(lastActivityText(Number.NaN, now),
        `Last activity: ${invalid.toLocaleDateString()} ${invalid.toLocaleTimeString()}`);
});

test("built-in permission profiles have user-facing labels", () => {
    const catalog = turnSettingCatalog({models: [], permissionProfiles: {data: [
        {id: ":workspace"}, {id: ":read-only"}, {id: ":danger-full-access"}, {id: "team-managed"},
    ]}});
    const draft = settingDraftFor(new Map(), "thread", {}, catalog);
    assert.deepEqual(settingPresentation(draft, catalog).permissionProfiles.map(choice => choice.label),
        ["Default (not reported)", "Workspace", "Read only", "Full access", "team-managed"]);
});

test("fixed setting projections retain unknown current values", () => {
    for (const field of ["sandbox", "network", "approval", "personality", "reviewer", "summary", "collaboration"])
        assert.deepEqual(fixedSettingChoices(field, `future-${field}`).at(-1),
            {value: `future-${field}`, label: `Future ${field}`, description: ""});
});

test("pending-request disclosure is bounded and suppresses unknown fields", () => {
    const request = (kind, raw = {}) => ({id: "1", kind, threadId: "thread", generation: 1, raw});
    const disclosed = pendingRequestDetails(request("permissions-approval", {
        permissions: {fileSystem: {write: ["/tmp/<literal>"]}, network: {enabled: true}},
        futureCapability: {mode: "bounded"},
    }));
    assert.equal(disclosed.truncated, true);
    assert.deepEqual(disclosed.entries, [
        {path: "permissions / fileSystem / write / 1", value: "/tmp/<literal>"},
        {path: "permissions / network / enabled", value: "Yes"},
    ], "request disclosure retains reviewable fields without exposing unknown protocol content");
});


test("catalog defaults are not presented as effective thread settings", () => {
    const catalog = turnSettingCatalog({models: [{model: "gpt-default", isDefault: true,
        defaultReasoningEffort: "high", defaultServiceTier: "fast"}], permissionProfiles: []});
    const draft = settingDraftFor(new Map(), "thread", {}, catalog);
    const presentation = settingPresentation(draft, catalog);
    for (const choices of [presentation.efforts, presentation.serviceTiers]) {
        assert.equal(choices[0].label, "Default (not reported)");
        assert.match(choices[0].description, /effective value was not reported/);
    }
});


test("an unreported collaboration mode is an unavailable placeholder, distinct from Code", () => {
    const catalog = turnSettingCatalog({models: [], permissionProfiles: []});
    const draft = settingDraftFor(new Map(), "unknown", {}, catalog);
    assert.equal(draft.values.collaboration, "");
    assert.deepEqual(fixedSettingChoices("collaboration", "")[0], {
        value: "", label: "Not reported", description: "Not reported by app-server", disabled: true,
    });
    const reported = settingDraftFor(new Map(), "code", {collaborationMode: {mode: "default"}}, catalog);
    assert.equal(reported.values.collaboration, "default");
    assert.equal(fixedSettingChoices("collaboration", "default").find(choice => choice.value === "default").label, "Code");
});

test("workspace options form supports explicit root choices and one deliberate change form", () => {
    const callbacks = {onCancel() {}, onContinue() {}};
    const rootChoices = ["/project/a", "/project/b"];
    const creation = renderToStaticMarkup(createElement(NewThreadDialog, {
        initialWorkspace: "", workspaceChoices: rootChoices, ...callbacks,
    }));
    assert.match(creation, /value=""/u);
    for (const path of rootChoices) assert.ok(creation.includes(`value="${path}"`));
    const change = renderToStaticMarkup(createElement(NewThreadDialog, {
        purpose: "workspace", workspaceChoices: rootChoices, initialWorkspace: "/remote/old", initialDraft: {
            workspace: "/remote/old", name: "Target B", baseInstructions: "", developerInstructions: "", ephemeral: false,
        }, ...callbacks,
    }));
    for (const purpose of ["workspace", "fork"]) {
        const markup = renderToStaticMarkup(createElement(NewThreadDialog, {
            purpose, workspaceChoices: rootChoices, initialWorkspace: "/remote/manual", ...callbacks,
        }));
        for (const path of rootChoices) assert.ok(markup.includes(`value="${path}"`));
        assert.ok(markup.includes('value="/remote/manual"'));
    }
    assert.match(change, /Change workspace…/u);
    assert.match(change, /Thread: Target B/u);
    assert.doesNotMatch(change, /Current workspace|readonly/u);
    assert.match(change, /Proposed workspace/u);
    assert.doesNotMatch(change, /Temporary thread|Developer instructions|Base instructions/u);
    const unknown = renderToStaticMarkup(createElement(NewThreadDialog, {purpose: "workspace", initialWorkspace: "", ...callbacks}));
    assert.match(unknown, /Proposed workspace/u);
    assert.doesNotMatch(unknown, /Current workspace|Workspace not reported/u);
});
