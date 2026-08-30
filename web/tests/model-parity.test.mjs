import assert from "node:assert/strict";
import test from "node:test";

import {
    PresentationModel,
    ProtocolNormalizer,
    event,
    result,
} from "../dist/index.js";

function pipeline() {
    const model = new PresentationModel();
    const normalizer = new ProtocolNormalizer(frame => {
        model.applyEvent(frame);
        return true;
    });
    return {model, normalizer};
}

test("C++ presentation pipeline state and replacement invariants", () => {
    const {model, normalizer} = pipeline();
    normalizer.transportEvent("connected");
    normalizer.bridgeEvent({kind: "bridge.connection", event: "connected", connectionId: "frontend-test", role: "observer"});
    normalizer.bridgeEvent({kind: "bridge.controller", controllerConnectionId: "frontend-test"});
    normalizer.connectionSettings({selected: "ipv6", available: [{key: "ipv6", label: "IPv6"}]});
    normalizer.operationResult("threads.list", "list-1", {}, {
        id: "list-1", result: {data: [{id: "thread-1", preview: "Architecture pipeline", cwd: "/workspace", status: {type: "idle"}}]},
    });
    normalizer.operationResult("thread.read", "read-1", {threadId: "thread-1"}, {
        id: "read-1", result: {thread: {
            id: "thread-1", preview: "Architecture pipeline", cwd: "/workspace", status: {type: "idle"},
            turns: [{id: "turn-1", status: "completed", items: [{id: "user-1", type: "userMessage"}]}],
        }},
    });
    normalizer.serverNotification("turn/started", {
        threadId: "thread-1", turn: {id: "turn-2", status: "inProgress", items: []},
    });
    normalizer.serverNotification("item/started", {
        threadId: "thread-1", turnId: "turn-2",
        item: {id: "command-1", type: "commandExecution", command: "printf PIPELINE_OK", cwd: "/workspace", status: "inProgress"},
    });
    normalizer.serverNotification("item/commandExecution/outputDelta", {
        threadId: "thread-1", turnId: "turn-2", itemId: "command-1", delta: "PIPELINE_OK\n",
    });
    normalizer.serverNotification("item/completed", {
        threadId: "thread-1", turnId: "turn-2",
        item: {id: "command-1", type: "commandExecution", command: "printf PIPELINE_OK", cwd: "/workspace", status: "completed", aggregatedOutput: "PIPELINE_OK\n", exitCode: 0},
    });
    normalizer.serverNotification("turn/diff/updated", {threadId: "thread-1", turnId: "turn-2", diff: "diff text"});
    normalizer.serverNotification("turn/plan/updated", {
        threadId: "thread-1", turnId: "turn-2", explanation: "Keep live inspector state",
        plan: [{step: "Retain the plan", status: "completed"}],
    });
    normalizer.serverNotification("turn/completed", {
        threadId: "thread-1", turn: {id: "turn-2", status: "completed", items: []},
    });
    normalizer.operationResult("thread.read", "read-2", {threadId: "thread-1"}, {
        id: "read-2", result: {thread: {id: "thread-1", status: {type: "idle"}, turns: []}},
    });
    normalizer.operationResult("thread.resume", "resume-1", {threadId: "thread-1"}, {
        id: "resume-1", result: {thread: {id: "thread-1"}, model: "gpt-current", reasoningEffort: "high"},
    });
    normalizer.serverNotification("thread/settings/updated", {
        threadId: "thread-1", threadSettings: {model: "gpt-current", personality: "friendly"},
    });
    normalizer.serverNotification("thread/settings/updated", {
        threadId: "thread-1", threadSettings: {personality: null},
    });

    const thread = model.thread("thread-1");
    const turn = thread.turns.get("turn-2");
    const item = turn.items.get("command-1");
    assert.deepEqual(model.connection(), {
        connected: true, retrying: false, generation: 1, connectionId: "frontend-test", role: "controller",
        controllerConnectionId: "frontend-test", detail: "", providerGeneration: 0, providerState: "",
        providerDetail: "", settings: {selected: "ipv6", available: [{key: "ipv6", label: "IPv6"}]},
    });
    assert.deepEqual(thread.turnOrder, ["turn-1", "turn-2"]);
    assert.equal(thread.raw.model, "gpt-current");
    assert.equal(thread.raw.reasoningEffort, "high");
    assert.deepEqual(thread.domains.get("thread.settings.changed"), {
        threadId: "thread-1",
        threadSettings: {model: "gpt-current", personality: null},
    });
    assert.equal(thread.settingsRevision, 2);
    assert.equal(turn.status, "completed");
    assert.deepEqual(turn.plan, {explanation: "Keep live inspector state", steps: [{step: "Retain the plan", status: "completed"}]});
    assert.equal(turn.domains.get("turn.diff.changed").diff, "diff text");
    assert.equal(item.raw.aggregatedOutput, "PIPELINE_OK\n");
    assert.equal(item.raw.exitCode, 0);
    assert.equal(model.activeTurnId("thread-1"), undefined);

    normalizer.operationResult("thread.read", "stale-active", {threadId: "thread-1"}, {
        id: "stale-active", result: {thread: {id: "thread-1", status: {type: "active"}, turns: [
            {id: "turn-1", status: "completed"}, {id: "turn-2", status: "inProgress"},
        ]}},
    });
    assert.equal(thread.turns.get("turn-2").status, "completed");
    assert.equal(thread.status, "idle");
    assert.equal(model.activeTurnId("thread-1"), undefined);
});

test("C++ ordering, domains, telemetry, generation, and repository hints", () => {
    const model = new PresentationModel();
    model.applyEvent(event(1, 1, "thread.upsert", {thread: {id: "retained-a"}}, "merge", {threadId: "retained-a"}));
    model.applyEvent(event(2, 1, "thread.upsert", {thread: {id: "retained-b"}}, "merge", {threadId: "retained-b"}));
    model.applyEvent(result(3, 1, "threads.list", "list", true, {
        threads: [{id: "provider-a"}, {id: "provider-b"}, {id: "provider-a"}],
    }, "merge"));
    assert.deepEqual(model.threadOrder(), ["provider-a", "provider-b", "retained-b", "retained-a"]);

    model.applyEvent(event(4, 1, "catalog.skills.invalidated", {revision: 1}, "none"));
    model.applyEvent(event(5, 1, "catalog.models.changed", {models: ["a"]}, "replace"));
    model.applyEvent(event(6, 1, "catalog.models.changed", {}, "remove"));
    assert.equal(model.telemetry().length, 1);
    assert.equal(model.globalDomains().has("catalog.models.changed"), false);

    model.applyEvent(event(7, 1, "notice.added", {message: "scoped telemetry"}, "none", {threadId: "phantom-none"}));
    model.applyEvent(event(8, 1, "thread.goal.changed", {}, "remove", {threadId: "phantom-remove"}));
    assert.equal(model.thread("phantom-none"), undefined);
    assert.equal(model.thread("phantom-remove"), undefined);
    assert.equal(model.telemetry().length, 2);
    model.applyEvent(event(9, 1, "turn.upsert", {turn: {id: "real-turn", status: "inProgress"}}, "merge", {
        threadId: "real-thread", turnId: "real-turn",
    }));
    assert.notEqual(model.thread("real-thread"), undefined);

    model.applyEvent(result(10, 1, "thread.read", "hints", true, {thread: {
        id: "repository-thread", cwd: "/workspace", turns: [{id: "repository-turn", items: [
            {id: "command", type: "commandExecution", cwd: "/workspace/project/src"},
            {id: "change", type: "fileChange", changes: [{path: "lib/example.cpp"}, {path: "removed.txt"}]},
        ]}],
    }}, "replace", {threadId: "repository-thread"}));
    assert.deepEqual(model.thread("repository-thread").commandCwds, ["/workspace/project/src"]);
    assert.deepEqual(model.thread("repository-thread").changedPaths, ["lib/example.cpp", "removed.txt"]);

    model.applyEvent(event(1, 2, "connection.lifecycle", {state: "connected"}, "replace"));
    model.applyEvent(event(11, 1, "thread.upsert", {thread: {id: "stale"}}, "merge", {threadId: "stale"}));
    assert.equal(model.thread("stale"), undefined);
    model.applyEvent(event(2, 2, "pending-request.upsert", {requestId: 42, category: "approval", request: {}}, "merge", {threadId: "provider-a"}));
    assert.equal(model.pendingRequestCount(), 1);
    model.applyEvent(event(3, 2, "connection.lifecycle", {state: "retrying", detail: "again"}, "replace"));
    assert.equal(model.pendingRequestCount(), 0);
    assert.equal(model.connection().retrying, true);
});

test("C++ child ownership, correlation, replacement, and removal invariants", () => {
    const model = new PresentationModel();
    model.applyEvent(result(1, 1, "threads.list", "roots", true, {
        threads: [{id: "parent"}, {id: "child-one"}, {id: "second-root"}],
    }, "merge"));
    const item = (sequence, threadId, turnId, id, child) => model.applyEvent(event(
        sequence, 1, "conversation.item.upsert",
        {item: {id, type: "subAgentActivity", status: "started", agentThreadId: child}},
        "merge", {threadId, turnId, itemId: id},
    ));
    item(2, "parent", "parent-turn", "spawn-one", "child-one");
    item(3, "parent", "parent-turn", "spawn-two", "child-two");
    item(4, "child-one", "child-turn", "spawn-grandchild", "grandchild");
    assert.deepEqual(model.thread("parent").childThreadOrder, ["child-one", "child-two"]);
    assert.deepEqual(model.thread("child-one").childThreadOrder, ["grandchild"]);
    assert.deepEqual(model.threadOrder(), ["parent", "second-root"]);
    assert.deepEqual(model.childOwnership("child-one"), {parentThreadId: "parent", agentId: "spawn-one"});

    model.applyEvent(event(5, 1, "agents.activity.upsert", {activity: {
        id: "peer", type: "subAgentActivity", kind: "interacted", agentPath: "/root/child-two", agentThreadId: "child-two",
    }}, "merge", {threadId: "child-one", turnId: "child-turn", itemId: "peer"}));
    assert.equal(model.thread("child-one").agents.has("peer"), false);
    assert.equal(model.childOwnership("child-two").parentThreadId, "parent");
    assert.equal(model.thread("parent").agents.get("spawn-two").raw.agentPath, "/root/child-two");

    model.applyEvent(event(6, 1, "turn.upsert", {turn: {id: "child-turn", status: "completed"}}, "merge", {threadId: "child-one", turnId: "child-turn"}));
    model.applyEvent(event(7, 1, "conversation.item.upsert", {item: {
        id: "child-answer", type: "agentMessage", text: "direct child result",
    }}, "merge", {threadId: "child-one", turnId: "child-turn", itemId: "child-answer"}));
    const parent = model.thread("parent");
    assert.equal(parent.agents.get("spawn-one").status, "completed");
    assert.equal(parent.agents.get("spawn-one").raw.resultText, "direct child result");
    assert.equal(parent.turns.get("parent-turn").items.get("spawn-one").raw.resultText, "direct child result");

    model.applyEvent(result(8, 1, "thread.read", "replace-child", true, {thread: {
        id: "child-one", status: {type: "idle"}, turns: [{id: "child-turn", status: "inProgress", items: [
            {id: "spawn-grandchild", type: "subAgentActivity", status: "started", agentThreadId: "grandchild"},
        ]}],
    }}, "replace", {threadId: "child-one"}));
    assert.equal(parent.agents.get("spawn-one").status, "idle");
    assert.equal(Object.hasOwn(parent.agents.get("spawn-one").raw, "resultText"), false);

    model.applyEvent(event(9, 1, "thread.removed", {}, "remove", {threadId: "child-one"}));
    assert.equal(model.thread("child-one"), undefined);
    assert.equal(model.childOwnership("grandchild"), undefined);
    assert.deepEqual(model.threadOrder(), ["parent", "second-root", "grandchild"]);
    model.applyEvent(event(10, 1, "thread.removed", {}, "remove", {threadId: "parent"}));
    assert.deepEqual(model.threadOrder(), ["child-two", "second-root", "grandchild"]);
});

test("C++ live agent rebind and cycle protection invariants", () => {
    const model = new PresentationModel();
    model.applyEvent(event(1, 1, "thread.upsert", {thread: {id: "rebind-parent"}}, "merge", {threadId: "rebind-parent"}));
    model.applyEvent(event(2, 1, "conversation.item.upsert", {item: {
        id: "stable-agent", type: "subAgentActivity", status: "completed", resultText: "old result", agentThreadId: "old-child",
    }}, "merge", {threadId: "rebind-parent", turnId: "rebind-turn", itemId: "stable-agent"}));
    model.applyEvent(event(3, 1, "agents.activity.upsert", {activity: {
        type: "subAgentActivity", status: "started", agentThreadId: "new-child",
    }}, "merge", {threadId: "rebind-parent", turnId: "rebind-turn", itemId: "stable-agent"}));
    const parent = model.thread("rebind-parent");
    const agent = parent.agents.get("stable-agent");
    assert.deepEqual(parent.childThreadOrder, ["new-child"]);
    assert.equal(model.childOwnership("old-child"), undefined);
    assert.deepEqual(model.threadOrder(), ["rebind-parent", "old-child"]);
    assert.equal(agent.childThreadId, "new-child");
    assert.equal(agent.status, "started");
    assert.equal(Object.hasOwn(agent.raw, "resultText"), false);

    model.applyEvent(event(4, 1, "turn.upsert", {turn: {id: "new-child-turn", status: "completed"}}, "merge", {threadId: "new-child", turnId: "new-child-turn"}));
    model.applyEvent(event(5, 1, "conversation.item.upsert", {item: {
        id: "answer", type: "agentMessage", text: "new child result",
    }}, "merge", {threadId: "new-child", turnId: "new-child-turn", itemId: "answer"}));
    model.applyEvent(result(6, 1, "thread.read", "stale", true, {thread: {
        id: "rebind-parent", turns: [{id: "rebind-turn", items: [{
            id: "stable-agent", type: "subAgentActivity", status: "started", agentThreadId: "old-child",
        }]}],
    }}, "merge", {threadId: "rebind-parent"}));
    assert.equal(agent.childThreadId, "new-child");
    assert.equal(agent.status, "completed");
    assert.equal(agent.raw.resultText, "new child result");

    model.applyEvent(event(7, 1, "agents.activity.upsert", {activity: {
        type: "subAgentActivity", status: "started", agentThreadId: "rebind-parent",
    }}, "merge", {threadId: "new-child", turnId: "cycle-turn", itemId: "cycle-agent"}));
    assert.equal(model.childOwnership("rebind-parent"), undefined);
    assert.deepEqual(model.thread("new-child").childThreadOrder, []);
});

test("provider generations are scoped to one frontend connection generation", () => {
    const model = new PresentationModel();
    model.applyEvent(event(1, 1, "connection.lifecycle", {state: "connected"}, "replace"));
    model.applyEvent(event(2, 1, "connection.provider", {generation: 10, state: "ready"}, "replace"));
    model.applyEvent(event(3, 1, "thread.upsert", {thread: {id: "old-provider"}}, "merge", {threadId: "old-provider"}));
    model.applyEvent(event(1, 2, "connection.lifecycle", {state: "connected"}, "replace"));
    model.applyEvent(event(2, 2, "connection.provider", {generation: 1, state: "ready"}, "replace"));
    assert.notEqual(model.thread("old-provider"), undefined);
    assert.equal(model.connection().providerGeneration, 1);
    assert.equal(model.connection().providerState, "ready");
});

test("stream text retains bounded tails and explicit discarded-byte metadata", () => {
    const model = new PresentationModel();
    const oversized = "A".repeat(300 * 1024);
    model.applyEvent(event(1, 1, "conversation.item.upsert", {item: {
        id: "bounded-command", type: "commandExecution", aggregatedOutput: oversized,
    }}, "merge", {threadId: "bounded-thread", turnId: "bounded-turn", itemId: "bounded-command"}));
    let item = model.thread("bounded-thread").turns.get("bounded-turn").items.get("bounded-command");
    assert.ok(new TextEncoder().encode(item.raw.aggregatedOutput).length <= 256 * 1024);
    assert.ok(item.textRetention.get("aggregatedOutput").discardedBytes > 0);

    model.applyEvent(event(2, 1, "conversation.item.append", {
        field: "aggregatedOutput", text: "B".repeat(300 * 1024),
    }, "merge", {threadId: "bounded-thread", turnId: "bounded-turn", itemId: "bounded-command"}));
    item = model.thread("bounded-thread").turns.get("bounded-turn").items.get("bounded-command");
    assert.ok(new TextEncoder().encode(item.raw.aggregatedOutput).length <= 256 * 1024);
    assert.equal(item.raw.aggregatedOutput.startsWith("B"), true);
    assert.ok(item.textRetention.get("aggregatedOutput").discardedBytes >= 300 * 1024);

    model.applyEvent(event(3, 1, "conversation.item.upsert", {item: {
        id: "bounded-reasoning", type: "reasoning", summary: ["C".repeat(160 * 1024), "D".repeat(160 * 1024)],
    }}, "merge", {threadId: "bounded-thread", turnId: "bounded-turn", itemId: "bounded-reasoning"}));
    const reasoning = model.thread("bounded-thread").turns.get("bounded-turn").items.get("bounded-reasoning");
    assert.ok(reasoning.textRetention.get("summary").retainedBytes <= 256 * 1024);
    assert.ok(reasoning.textRetention.get("summary").discardedBytes > 0);

    model.applyEvent(event(4, 1, "conversation.item.upsert", {item: {
        id: "large-prompt", type: "userMessage", text: oversized,
    }}, "merge", {threadId: "bounded-thread", turnId: "bounded-turn", itemId: "large-prompt"}));
    const prompt = model.thread("bounded-thread").turns.get("bounded-turn").items.get("large-prompt");
    assert.equal(prompt.raw.text, oversized);
    assert.equal(prompt.textRetention, undefined);
});
