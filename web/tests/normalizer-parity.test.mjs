import assert from "node:assert/strict";
import {test} from "node:test";

import {
    command,
    event,
    isPresentationFrame,
    ProtocolNormalizer,
    result,
} from "../dist/index.js";

test("presentation frame grammar equals the C++ protocol boundary", () => {
    const values = [
        command("threads.list", {}, "request"),
        event(1, 2, "thread.upsert", {thread: {id: "thread"}}, "merge", {threadId: "thread"}),
        result(2, 2, "threads.list", "request", true, {threads: []}, "merge"),
        result(3, 2, "threads.list", "request", false, {code: -1}),
    ];
    assert.equal(values.every(isPresentationFrame), true);
    for (const invalid of [
        null,
        {},
        {...values[0], data: []},
        {...values[1], sequence: -1},
        {...values[1], authority: "invalid"},
        {...values[1], data: []},
        {...values[2], error: {}},
        {...values[3], data: {}},
    ]) {
        assert.equal(isPresentationFrame(invalid), false);
    }
});

test("normalizer emits the exact ordered connection frames used by C++", () => {
    const frames = [];
    const normalizer = new ProtocolNormalizer((frame) => {
        frames.push(frame);
        return true;
    });

    normalizer.transportEvent("connected");
    normalizer.bridgeEvent({
        kind: "bridge.connection",
        event: "connected",
        connectionId: "frontend-test",
        role: "observer",
    });
    normalizer.bridgeEvent({
        kind: "bridge.controller",
        controllerConnectionId: "frontend-test",
    });
    normalizer.connectionSettings({selected: "ipv6"});

    assert.deepEqual(frames, [
        event(1, 1, "connection.lifecycle", {state: "connected"}),
        event(2, 1, "connection.bridge", {
            state: "connected",
            connectionId: "frontend-test",
            role: "observer",
        }),
        event(3, 1, "connection.controller", {
            controllerConnectionId: "frontend-test",
        }, "replace"),
        event(4, 1, "connection.settings.changed", {selected: "ipv6"}, "replace"),
    ]);
    assert.equal(normalizer.sequence, 5);
});

test("operation result authority and payload shaping equal C++", () => {
    const frames = [];
    const normalizer = new ProtocolNormalizer((frame) => {
        frames.push(frame);
        return true;
    });
    normalizer.transportEvent("connected");

    normalizer.operationResult("threads.list", "list", {}, {
        id: "list",
        result: {data: [{id: "thread"}], nextCursor: null},
    });
    const startedAtSequence = normalizer.sequence;
    normalizer.operationResult("thread.read", "read", {threadId: "requested"}, {
        id: "read",
        result: {thread: {id: "returned", turns: []}},
    }, startedAtSequence);
    normalizer.operationResult("thread.resume", "resume", {threadId: "returned"}, {
        id: "resume",
        result: {
            thread: {id: "returned"},
            model: "gpt-current",
            reasoningEffort: "high",
            ignored: "not copied",
        },
    });
    normalizer.operationResult("models.list", "models", {}, {
        result: {data: [{id: "gpt"}], nextCursor: "next"},
    });
    normalizer.operationResult("account.read", "account", {}, {
        result: {account: {type: "chatgpt"}},
    });
    normalizer.operationResult("turn.start", "turn", {threadId: "returned"}, {
        result: {turn: {id: "turn"}},
    });
    normalizer.operationResult("turn.interrupt", "interrupt", {}, {result: null});
    normalizer.operationResult("thread.read", "failed", {}, {
        error: {code: -1, message: "failed"},
    });

    assert.deepEqual(frames.slice(1), [
        result(2, 1, "threads.list", "list", true, {
            threads: [{id: "thread"}],
            nextCursor: null,
            backwardsCursor: null,
        }, "merge"),
        result(3, 1, "thread.read", "read", true, {
            thread: {id: "returned", turns: []},
        }, "replace", {threadId: "returned"}),
        result(4, 1, "thread.resume", "resume", true, {
            thread: {
                id: "returned",
                model: "gpt-current",
                reasoningEffort: "high",
            },
        }, "merge", {threadId: "returned"}),
        result(5, 1, "models.list", "models", true, {
            models: [{id: "gpt"}],
            nextCursor: "next",
        }, "replace"),
        result(6, 1, "account.read", "account", true, {
            account: {type: "chatgpt"},
        }, "replace"),
        result(7, 1, "turn.start", "turn", true, {
            turn: {id: "turn"},
        }, "merge", {threadId: "returned"}),
        result(8, 1, "turn.interrupt", "interrupt", true, null),
        result(9, 1, "thread.read", "failed", false, {
            code: -1,
            message: "failed",
        }),
    ]);
});

test("core notification normalization preserves C++ scopes and authority", () => {
    const frames = [];
    const normalizer = new ProtocolNormalizer((frame) => {
        frames.push(frame);
        return true;
    });
    normalizer.transportEvent("connected");
    normalizer.serverNotification("thread/started", {thread: {id: "thread"}});
    normalizer.serverNotification("turn/started", {
        threadId: "thread",
        turn: {id: "turn"},
    });
    normalizer.serverNotification("item/started", {
        threadId: "thread",
        turnId: "turn",
        item: {
            id: "agent",
            type: "subAgentActivity",
            agentThreadId: "child",
        },
    });
    normalizer.serverNotification("item/reasoning/summaryTextDelta", {
        threadId: "thread",
        turnId: "turn",
        itemId: "reasoning",
        summaryIndex: 2,
        delta: "summary",
    });
    normalizer.serverNotification("turn/plan/updated", {
        threadId: "thread",
        turnId: "turn",
        explanation: "Plan",
        plan: [{step: "Work", status: "inProgress"}],
    });
    normalizer.serverNotification("serverRequest/resolved", {
        threadId: "thread",
        requestId: 42,
    });

    assert.deepEqual(frames.slice(1), [
        event(2, 1, "thread.upsert", {thread: {id: "thread"}}, "merge"),
        event(3, 1, "turn.upsert", {
            lifecycle: "started",
            turn: {id: "turn"},
        }, "merge", {threadId: "thread"}),
        event(4, 1, "conversation.item.upsert", {
            lifecycle: "started",
            item: {
                id: "agent",
                type: "subAgentActivity",
                agentThreadId: "child",
            },
        }, "merge", {threadId: "thread", turnId: "turn", itemId: "agent"}),
        event(5, 1, "agents.activity.upsert", {
            lifecycle: "started",
            activity: {
                id: "agent",
                type: "subAgentActivity",
                agentThreadId: "child",
            },
        }, "merge", {threadId: "thread", turnId: "turn", itemId: "agent"}),
        event(6, 1, "conversation.item.append", {
            field: "summary",
            text: "summary",
            summaryIndex: 2,
        }, "merge", {
            threadId: "thread",
            turnId: "turn",
            itemId: "reasoning",
        }),
        event(7, 1, "plan.replaced", {
            explanation: "Plan",
            steps: [{step: "Work", status: "inProgress"}],
        }, "replace", {threadId: "thread", turnId: "turn"}),
        event(8, 1, "pending-request.removed", {}, "remove", {
            threadId: "thread",
            requestId: 42,
        }),
    ]);
});

test("all remaining generated notification mappings equal the C++ vocabulary", () => {
    const cases = {
        "thread/reverted": ["thread.reverted", "merge"],
        "skills/changed": ["catalog.skills.invalidated", "none"],
        "thread/settings/updated": ["thread.settings.changed", "merge"],
        "turn/diff/updated": ["turn.diff.changed", "replace"],
        "item/fileChange/patchUpdated": ["conversation.file-change.patch-replaced", "replace"],
        "item/mcpToolCall/progress": ["conversation.mcp.progress", "merge"],
        "thread/compacted": ["thread.compacted", "merge"],
        "model/safetyBuffering/updated": ["model.safety-buffering.changed", "replace"],
        "thread/realtime/transcript/delta": ["realtime.transcript.appended", "merge"],
        "windows/worldWritableWarning": ["system.windows-permission.warning", "none"],
        "account/login/completed": ["account.login.completed", "merge"],
    };
    const frames = [];
    const normalizer = new ProtocolNormalizer((frame) => {
        frames.push(frame);
        return true;
    });
    for (const method of Object.keys(cases)) {
        normalizer.serverNotification(method, {threadId: "thread", marker: method});
    }
    for (const [index, [method, [type, authority]]] of Object.entries(cases).entries()) {
        assert.deepEqual(frames[index], event(
            index + 1,
            0,
            type,
            {threadId: "thread", marker: method},
            authority,
            {threadId: "thread"},
        ));
    }
});

test("server request categories and unknown-method diagnostics equal C++", () => {
    const frames = [];
    const normalizer = new ProtocolNormalizer((frame) => {
        frames.push(frame);
        return true;
    });
    const categories = {
        "item/commandExecution/requestApproval": "command-approval",
        "item/fileChange/requestApproval": "file-change-approval",
        "item/tool/requestUserInput": "user-input",
        "mcpServer/elicitation/request": "mcp-elicitation",
        "item/permissions/requestApproval": "permissions-approval",
        "item/tool/call": "dynamic-tool-call",
        "account/chatgptAuthTokens/refresh": "authentication-refresh",
        "attestation/generate": "attestation",
        applyPatchApproval: "legacy-patch-approval",
        execCommandApproval: "legacy-command-approval",
        unsupported: "unsupported",
    };
    let requestId = 1;
    for (const [method, category] of Object.entries(categories)) {
        normalizer.serverRequest(method, requestId, {threadId: "thread"});
        assert.deepEqual(frames.at(-1), event(
            requestId,
            0,
            "pending-request.upsert",
            {requestId, category, request: {threadId: "thread"}},
            "merge",
            {threadId: "thread", requestId},
        ));
        ++requestId;
    }

    const before = frames.length;
    normalizer.observeRawInbound({method: "thread/started", params: {}});
    assert.equal(frames.length, before);
    normalizer.observeRawInbound({method: "future/method", params: {}});
    assert.deepEqual(frames.at(-1), event(
        requestId,
        0,
        "system.diagnostic",
        {
            source: "appserver",
            code: "unknown-method",
            message: "future/method",
            details: {},
        },
    ));
});

test("delivery failure is sticky and reported once like C++", () => {
    let deliveries = 0;
    let failures = 0;
    const normalizer = new ProtocolNormalizer(() => {
        ++deliveries;
        return false;
    });
    normalizer.setDeliveryFailureHandler(() => ++failures);
    normalizer.transportEvent("connected");
    normalizer.transportEvent("disconnected");
    assert.equal(deliveries, 1);
    assert.equal(failures, 1);
    assert.equal(normalizer.sequence, 3, "sequence allocation continues before sticky rejection");
});
