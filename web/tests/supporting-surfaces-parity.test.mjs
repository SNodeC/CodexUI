import assert from "node:assert/strict";
import test from "node:test";

import {
    applySettingChange, canonicalSettingValues, canonicalThreadSettings, changeSettingDraft, collaborationMode, negativePendingResponse,
    displayStatus, pendingDecisionOptions, pendingRequestDetails, pendingResponse, permissionProfileLabel, positivePendingResponse,
    sandboxPolicy, threadStartOptions, turnStartOptions,
    settingDraftFor, settingPromptOptions,
} from "../dist/index.js";

test("status presentation is lowercase and human-readable", () => {
    assert.equal(displayStatus("inProgress"), "running");
    assert.equal(displayStatus("notLoaded"), "not loaded");
    assert.equal(displayStatus("futureProviderState"), "future provider state");
});

test("native turn-setting option shaping", () => {
    assert.deepEqual(canonicalThreadSettings(
        {model: "old", reasoningEffort: "medium", sandbox: "readOnly", personality: "friendly"},
        {threadSettings: {model: "new", effort: "high", sandboxPolicy: {type: "workspaceWrite"}, personality: null}},
    ), {model: "new", effort: "high", sandboxPolicy: {type: "workspaceWrite"}});
    const values = canonicalSettingValues({
        model: "gpt-current", reasoningEffort: "high", personality: "friendly", approvalPolicy: "never",
        approvalsReviewer: "auto_review", cwd: "/workspace", serviceTier: "fast", summary: "concise",
        sandboxPolicy: {type: "workspaceWrite", networkAccess: true}, collaborationMode: {mode: "plan"},
        activePermissionProfile: {id: "workspace"},
    });
    assert.deepEqual(values, {
        model: "gpt-current", effort: "high", personality: "friendly", sandbox: "workspace-write", network: "enabled",
        approval: "never", reviewer: "auto_review", cwd: "/workspace", permissionProfile: "workspace",
        serviceTier: "fast", summary: "concise", collaboration: "plan",
    });
    values.permissionProfile = "default";
    const touched = new Set(["model", "effort", "personality", "sandbox", "network", "approval", "reviewer", "cwd",
        "permissionProfile", "serviceTier", "summary", "collaboration"]);
    assert.deepEqual(threadStartOptions(values, touched), {
        model: "gpt-current", approvalPolicy: "never", approvalsReviewer: "auto_review", personality: "friendly",
        serviceTier: "fast", cwd: "/workspace", permissions: null, sandbox: "workspace-write",
    });
    assert.deepEqual(turnStartOptions(values, touched, []), {
        model: "gpt-current", effort: "high", personality: "friendly", approvalPolicy: "never",
        approvalsReviewer: "auto_review", serviceTier: "fast", summary: "concise", cwd: "/workspace",
        permissions: null,
        sandboxPolicy: {type: "workspaceWrite", writableRoots: [], networkAccess: true, excludeTmpdirEnvVar: false, excludeSlashTmp: false},
        collaborationMode: {mode: "plan", settings: {model: "gpt-current", developer_instructions: null, reasoning_effort: "high"}},
    });
    values.sandbox = "external"; values.network = "restricted";
    assert.deepEqual(sandboxPolicy(values), {type: "externalSandbox", networkAccess: "restricted"});
    values.model = "default";
    assert.deepEqual(collaborationMode(values, [{id: "gpt-default", isDefault: true}]), {
        mode: "plan", settings: {model: "gpt-default", developer_instructions: null, reasoning_effort: "high"},
    });
});

test("explicit access replaces a named permission profile", () => {
    const initial = canonicalSettingValues({
        sandboxPolicy: {type: "workspaceWrite", networkAccess: false},
        activePermissionProfile: {id: ":workspace"},
    });
    const changed = applySettingChange(initial, new Set(), "sandbox", "danger-full-access");
    assert.equal(changed.values.permissionProfile, "default");
    assert.deepEqual([...changed.touched], ["sandbox"]);
    assert.deepEqual(turnStartOptions(changed.values, changed.touched, []), {
        sandboxPolicy: {type: "dangerFullAccess"},
    });
    assert.deepEqual(threadStartOptions(changed.values, changed.touched), {
        sandbox: "danger-full-access",
    });
});

test("turn-setting drafts and submitted options remain isolated by thread", () => {
    const drafts = new Map();
    const canonicalA = {model: "gpt-a", approvalPolicy: "on-request", sandboxPolicy: {type: "workspaceWrite"}};
    const canonicalB = {model: "gpt-b", approvalPolicy: "never", sandboxPolicy: {type: "readOnly"}};
    changeSettingDraft(drafts, "thread-a", canonicalA, 1, "sandbox", "danger-full-access");
    changeSettingDraft(drafts, "thread-a", canonicalA, 1, "approval", "untrusted");

    const threadB = settingDraftFor(drafts, "thread-b", canonicalB, 1);
    assert.equal(threadB.values.model, "gpt-b");
    assert.equal(threadB.values.sandbox, "read-only");
    assert.equal(threadB.touched.size, 0);
    assert.deepEqual(settingPromptOptions(threadB, []), {turn: {}, thread: {}});

    const threadA = settingDraftFor(drafts, "thread-a", canonicalA, 1);
    assert.equal(threadA.values.sandbox, "danger-full-access");
    assert.equal(threadA.values.approval, "untrusted");
    assert.deepEqual(settingPromptOptions(threadA, []), {
        turn: {approvalPolicy: "untrusted", sandboxPolicy: {type: "dangerFullAccess"}},
        thread: {approvalPolicy: "untrusted", sandbox: "danger-full-access"},
    });

    const refreshedA = settingDraftFor(drafts, "thread-a", {...canonicalA, model: "gpt-a-new"}, 2);
    assert.equal(refreshedA.values.model, "gpt-a-new");
    assert.equal(refreshedA.values.sandbox, "danger-full-access");
});

test("supported individual settings override retained values without discarding permissions", () => {
    let change = {
        values: canonicalSettingValues({
            model: "gpt-a", approvalPolicy: "never", personality: "friendly",
            sandboxPolicy: {type: "workspaceWrite", networkAccess: false},
            activePermissionProfile: {id: ":workspace"},
        }),
        touched: new Set(),
    };
    for (const [field, value] of [["model", "gpt-b"], ["approval", "on-request"], ["personality", "pragmatic"]])
        change = applySettingChange(change.values, change.touched, field, value);
    assert.equal(change.values.permissionProfile, ":workspace");
    assert.deepEqual(turnStartOptions(change.values, change.touched, []), {
        model: "gpt-b", personality: "pragmatic", approvalPolicy: "on-request",
    });
});

test("built-in permission profiles have user-facing labels", () => {
    assert.equal(permissionProfileLabel(":workspace"), "Workspace");
    assert.equal(permissionProfileLabel(":read-only"), "Read only");
    assert.equal(permissionProfileLabel(":danger-full-access"), "Full access");
    assert.equal(permissionProfileLabel("team-managed"), "team-managed");
});

test("native pending-request positive and negative response shapes", () => {
    const request = (kind, raw = {}) => ({id: "1", kind, threadId: "thread", generation: 1, raw});
    assert.deepEqual(positivePendingResponse(request("command-approval")), {result: {decision: "accept"}});
    assert.deepEqual(negativePendingResponse(request("file-change-approval")), {result: {decision: "decline"}});
    assert.deepEqual(positivePendingResponse(request("user-input"), {question: {answers: ["yes"]}}), {
        result: {answers: {question: {answers: ["yes"]}}},
    });
    assert.deepEqual(negativePendingResponse(request("mcp-elicitation")), {result: {action: "decline", content: null, _meta: null}});
    assert.deepEqual(positivePendingResponse(request("permissions-approval", {permissions: {network: true}})), {
        result: {permissions: {network: true}, scope: "turn"},
    });
    assert.deepEqual(negativePendingResponse(request("legacy-command-approval")), {
        result: {decision: {denied: {rejection: "Denied by user"}}},
    });
    assert.deepEqual(positivePendingResponse(request("dynamic-tool-call")), {
        result: {contentItems: [{type: "inputText", text: "CodexUI does not provide this dynamic tool"}], success: false},
    });
    assert.deepEqual(positivePendingResponse(request("attestation")), {
        error: {code: -32601, message: "CodexUI does not support this server request"},
    });

    const declared = request("command-approval", {availableDecisions: ["decline", "cancel", "future"]});
    assert.deepEqual(pendingDecisionOptions(declared).map(choice => choice.value), ["decline", "cancel"],
        "the web UI offers only declared, schema-supported command decisions");
    assert.deepEqual(pendingResponse(request("permissions-approval", {permissions: {network: {enabled: true}}}), "session"), {
        result: {permissions: {network: {enabled: true}}, scope: "session"},
    });
    assert.deepEqual(pendingResponse(request("mcp-elicitation"), "cancel"), {
        result: {action: "cancel", content: null, _meta: null},
    });

    const disclosed = pendingRequestDetails(request("permissions-approval", {
        permissions: {fileSystem: {write: ["/tmp/<literal>"]}, network: {enabled: true}},
        futureCapability: {mode: "bounded"},
    }));
    assert.equal(disclosed.truncated, false);
    assert.deepEqual(disclosed.entries, [
        {path: "permissions / fileSystem / write / 1", value: "/tmp/<literal>"},
        {path: "permissions / network / enabled", value: "Yes"},
        {path: "futureCapability / mode", value: "bounded"},
    ], "request disclosure preserves known and future fields without a raw JSON projection");
});
