import assert from "node:assert/strict";
import test from "node:test";

import {
    canonicalSettingValues, canonicalThreadSettings, collaborationMode, negativePendingResponse, positivePendingResponse,
    sandboxPolicy, threadStartOptions, turnStartOptions,
} from "../dist/index.js";

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
});
