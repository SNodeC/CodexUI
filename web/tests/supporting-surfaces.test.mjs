import assert from "node:assert/strict";
import test from "node:test";

import {
    fixedSettingChoices, pendingRequestDetails, settingDraftFor, settingPresentation, turnSettingCatalog,
} from "../dist/index.js";

test("built-in permission profiles have user-facing labels", () => {
    const catalog = turnSettingCatalog({models: [], permissionProfiles: {data: [
        {id: ":workspace"}, {id: ":read-only"}, {id: ":danger-full-access"}, {id: "team-managed"},
    ]}});
    const draft = settingDraftFor(new Map(), "thread", {}, catalog);
    assert.deepEqual(settingPresentation(draft, catalog).permissionProfiles.map(choice => choice.label),
        ["Thread default", "Workspace", "Read only", "Full access", "team-managed"]);
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
