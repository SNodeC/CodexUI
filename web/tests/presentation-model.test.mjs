import assert from "node:assert/strict";
import test from "node:test";

import {
    changeSettingDraft,
    PresentationModel,
    ProtocolNormalizer,
    event,
    result,
    settingDraftFor,
    statusToken,
    turnSettingCatalog,
} from "../dist/index.js";

function pipeline() {
    const model = new PresentationModel();
    const normalizer = new ProtocolNormalizer(frame => {
        model.applyEvent(frame);
        return true;
    });
    return {model, normalizer};
}

test("presentation pipeline state and replacement invariants", () => {
    const {model, normalizer} = pipeline();
    normalizer.transportEvent("connected");
    normalizer.bridgeEvent({kind: "bridge.connection", event: "connected", connectionId: "frontend-test", role: "observer"});
    normalizer.bridgeEvent({kind: "bridge.controller", controllerConnectionId: "frontend-test"});
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
        threadId: "thread-1", threadSettings: {model: "gpt-current", personality: "friendly", cwd: "/updated"},
    });
    normalizer.serverNotification("thread/settings/updated", {
        threadId: "thread-1", threadSettings: {personality: null},
    });

    const thread = model.thread("thread-1");
    const turn = thread.turns.get("turn-2");
    const item = turn.items.get("command-1");
    assert.deepEqual(model.connection(), {
        connected: true, retrying: false, generation: 1,
        providerGeneration: 0, providerState: "",
    });
    assert.deepEqual(thread.turnOrder, ["turn-1", "turn-2"]);
    assert.equal(thread.raw.model, "gpt-current");
    assert.equal(thread.raw.reasoningEffort, "high");
    assert.equal(Object.hasOwn(thread.raw, "personality"), false);
    assert.equal(thread.raw.cwd, "/updated");
    assert.equal(thread.cwd, "/updated");
    const firstAcknowledgement = thread.settingStamps.get("model").acknowledgements;
    assert.ok(firstAcknowledgement > 0);
    assert.ok(thread.settingStamps.get("personality").acknowledgements > firstAcknowledgement);
    assert.equal(thread.settingStamps.get("cwd").acknowledgements, firstAcknowledgement);
    assert.equal(statusToken(turn.status), "completed");
    assert.deepEqual(turn.plan, {explanation: "Keep live inspector state", steps: [{step: "Retain the plan", status: "completed"}]});
    assert.equal(item.raw.aggregatedOutput, "PIPELINE_OK\n");
    assert.equal(item.raw.exitCode, 0);
    assert.equal(model.activeTurnId("thread-1"), undefined);

    normalizer.serverNotification("thread/settings/updated", {
        threadId: "thread-1", threadSettings: {cwd: null},
    });
    assert.equal(Object.hasOwn(thread.raw, "cwd"), false);
    assert.equal(thread.cwd, "");
    assert.ok(thread.settingStamps.get("cwd").acknowledgements > firstAcknowledgement);

    normalizer.operationResult("thread.read", "stale-active", {threadId: "thread-1"}, {
        id: "stale-active", result: {thread: {id: "thread-1", status: {type: "active"}, turns: [
            {id: "turn-1", status: "completed"}, {id: "turn-2", status: "inProgress"},
        ]}},
    });
    assert.equal(statusToken(thread.turns.get("turn-2").status), "completed");
    assert.equal(statusToken(thread.status), "completed");
    assert.equal(model.activeTurnId("thread-1"), undefined);
});

for (const authority of ["replace", "merge"]) test(`thread ${authority} owns metadata and history independently`, () => {
    const model = new PresentationModel();
    const raw = {id: "owned", model: "gpt-current", futureMetadata: {nested: ["metadata"]},
        turns: [{id: "turn", items: [{id: "item", type: "agentMessage", text: "original"}]}]};
    const original = structuredClone(raw);
    model.applyEvent(result(1, 1, "thread.read", "seed", true, {thread: {id: "owned"}}, "merge"));
    assert.equal(model.applyEvent(result(2, 1, "thread.read", "read", true, {thread: raw}, authority)), "accepted");
    assert.deepEqual(raw, original, "metadata extraction must not mutate the incoming frame");
    const thread = model.thread("owned");
    const item = thread.turns.get("turn").items.get("item");
    assert.equal(Object.hasOwn(thread.raw, "turns"), false);
    assert.deepEqual(thread.turnOrder, ["turn"]);
    assert.equal(thread.raw.model, "gpt-current");
    raw.futureMetadata.nested[0] = "changed externally";
    raw.turns[0].items[0].text = "changed externally";
    assert.equal(thread.raw.futureMetadata.nested[0], "metadata");
    assert.equal(item.raw.text, "original");
    model.applyEvent(event(3, 1, "conversation.item.append", {field: "text", text: " appended"}, "merge",
        {threadId: "owned", turnId: "turn", itemId: "item"}));
    assert.equal(item.raw.text, "original appended");
    assert.equal(raw.turns[0].items[0].text, "changed externally");
});

test("delayed thread reads preserve newer effective settings", () => {
    const {model, normalizer} = pipeline();
    normalizer.serverNotification("thread/started", {thread: {
        id: "settings-race", model: "gpt-a", effort: "low", sandbox: "workspace-write",
    }});
    const startedAtSequence = normalizer.sequence;
    normalizer.serverNotification("thread/settings/updated", {
        threadId: "settings-race",
        threadSettings: {reasoningEffort: "ultra", futureSetting: {mode: "retained"}},
    });
    normalizer.serverNotification("thread/settings/updated", {
        threadId: "settings-race", threadSettings: {sandboxPolicy: {type: "readOnly"}},
    });
    normalizer.operationResult("thread.read", "stale-settings", {threadId: "settings-race"}, {
        id: "stale-settings", result: {thread: {
            id: "settings-race", model: "gpt-b", effort: "medium", sandbox: "workspace-write", turns: [],
        }},
    }, startedAtSequence);
    const thread = model.thread("settings-race");
    assert.equal(thread.raw.reasoningEffort, "ultra");
    assert.deepEqual(thread.raw.sandboxPolicy, {type: "readOnly"});
    assert.equal(Object.hasOwn(thread.raw, "effort"), false);
    assert.equal(Object.hasOwn(thread.raw, "sandbox"), false);
    assert.deepEqual(thread.raw.futureSetting, {mode: "retained"});
    assert.equal(thread.raw.model, "gpt-b", "an unrelated stale-read field remains admissible");
    assert.ok(thread.settingStamps.get("effort").acknowledgements > 0);
    assert.ok(thread.settingStamps.get("sandbox").acknowledgements
        > thread.settingStamps.get("effort").acknowledgements);
    assert.equal(thread.settingStamps.has("futureSetting"), false, "unknown settings cannot grow causal state");

    const laterRead = normalizer.sequence;
    normalizer.serverNotification("thread/started", {thread: {id: "settings-race", model: "gpt-b"}});
    normalizer.operationResult("thread.read", "fresh-model", {threadId: "settings-race"}, {
        result: {thread: {id: "settings-race", model: "gpt-c", turns: []}},
    }, laterRead);
    assert.equal(thread.raw.model, "gpt-c",
        "an equal intervening upsert must not fabricate a newer field revision");
});

for (const action of ["threads.list", "thread.create", "thread.resume", "thread.fork"]) {
    test(`${action} preserves setting concepts changed after dispatch`, () => {
        const {model, normalizer} = pipeline();
        normalizer.transportEvent("connected");
        normalizer.serverNotification("thread/started", {thread: {
            id: "settings-result-race", model: "gpt-a", effort: "low",
        }});
        const startedAtSequence = normalizer.sequence;
        normalizer.serverNotification("thread/settings/updated", {
            threadId: "settings-result-race", threadSettings: {reasoningEffort: "ultra"},
        });
        const stale = {id: "settings-result-race", model: "gpt-b", effort: "medium"};
        const value = action === "threads.list" ? {data: [stale]} : {thread: stale};
        normalizer.operationResult(action, `stale-${action}`, {threadId: "settings-result-race"}, {
            result: value,
        }, startedAtSequence);
        const thread = model.thread("settings-result-race");
        assert.equal(thread.raw.reasoningEffort, "ultra");
        assert.equal(Object.hasOwn(thread.raw, "effort"), false);
        assert.equal(thread.raw.model, "gpt-b");
    });
}

test("setting causal stamps remain valid when event sequences restart", () => {
    const model = new PresentationModel();
    model.applyEvent(event(1, 1, "thread.upsert", {thread: {
        id: "settings-generation", effort: "low",
    }}, "merge", {threadId: "settings-generation"}));
    model.applyEvent(event(2, 1, "thread.settings.changed", {
        reasoningEffort: "ultra",
    }, "merge", {threadId: "settings-generation"}));
    model.applyEvent(event(1, 2, "connection.lifecycle", {state: "connected"}, "merge"));
    model.applyEvent(result(2, 2, "thread.read", "new-generation-read", true, {
        thread: {id: "settings-generation", effort: "medium"}, requestSequence: 1,
    }, "merge", {threadId: "settings-generation"}));
    const thread = model.thread("settings-generation");
    assert.equal(thread.raw.effort, "medium");
    assert.equal(Object.hasOwn(thread.raw, "reasoningEffort"), false);
});

test("lifecycle fallbacks preserve explicit status authority", () => {
    const model = new PresentationModel();
    model.applyEvent(event(1, 1, "thread.upsert", {
        thread: {id: "lifecycle-thread", status: "inProgress"},
    }, "merge", {threadId: "lifecycle-thread"}));
    model.applyEvent(event(2, 1, "thread.lifecycle", {state: "archived"}, "merge", {
        threadId: "lifecycle-thread",
    }));
    let thread = model.thread("lifecycle-thread");
    assert.equal(thread.archived, true);
    assert.equal(statusToken(thread.status), "running");
    assert.equal(thread.raw.status, "inProgress");
    assert.equal(thread.raw.lifecycle, "archived");
    model.applyEvent(event(3, 1, "thread.lifecycle", {state: "unarchived"}, "merge", {
        threadId: "lifecycle-thread",
    }));
    assert.equal(thread.archived, false);
    assert.equal(statusToken(thread.status), "running");
    assert.equal(thread.raw.status, "inProgress");
    assert.equal(thread.raw.lifecycle, "unarchived");

    model.applyEvent(event(4, 1, "turn.upsert", {
        lifecycle: "completed", turn: {id: "explicit-running", status: "inProgress"},
    }, "merge", {threadId: "lifecycle-thread", turnId: "explicit-running"}));
    model.applyEvent(event(5, 1, "turn.upsert", {
        lifecycle: "completed", turn: {id: "implicit-completed"},
    }, "merge", {threadId: "lifecycle-thread", turnId: "implicit-completed"}));
    model.applyEvent(event(6, 1, "turn.upsert", {
        lifecycle: "started", turn: {id: "retained-terminal", status: "completed"},
    }, "merge", {threadId: "lifecycle-thread", turnId: "retained-terminal"}));
    model.applyEvent(event(7, 1, "turn.upsert", {
        lifecycle: "started", turn: {id: "implicit-running"},
    }, "merge", {threadId: "lifecycle-thread", turnId: "implicit-running"}));
    assert.equal(statusToken(thread.turns.get("explicit-running").status), "running");
    assert.equal(statusToken(thread.turns.get("implicit-completed").status), "completed");
    assert.equal(statusToken(thread.turns.get("retained-terminal").status), "completed");
    assert.equal(statusToken(thread.turns.get("implicit-running").status), "running");

    const itemEvent = (sequence, lifecycle, item) => model.applyEvent(event(sequence, 1,
        "conversation.item.upsert", {lifecycle, item}, "merge", {
            threadId: "lifecycle-thread", turnId: "item-turn", itemId: item.id,
        }));
    itemEvent(8, "completed", {id: "explicit-running-item", type: "commandExecution", status: "inProgress"});
    itemEvent(9, "completed", {id: "implicit-completed-item", type: "commandExecution"});
    itemEvent(10, "started", {id: "retained-terminal-item", type: "commandExecution", status: "completed"});
    itemEvent(11, "started", {id: "implicit-running-item", type: "commandExecution"});
    const itemTurn = thread.turns.get("item-turn");
    assert.equal(itemTurn.items.get("explicit-running-item").raw.status, "inProgress");
    assert.equal(itemTurn.items.get("implicit-completed-item").raw.status, "completed");
    assert.equal(itemTurn.items.get("retained-terminal-item").raw.status, "completed");
    assert.equal(itemTurn.items.get("implicit-running-item").raw.status, "running");

    itemEvent(12, "started", {id: "typed-completed-agent", type: "subAgentActivity",
        status: "completed", kind: "started", agentThreadId: "completed-child"});
    itemEvent(13, "completed", {id: "typed-running-agent", type: "collabAgentToolCall",
        tool: "spawn_agent", status: "inProgress", kind: "completed", agentThreadId: "running-child"});
    assert.equal(statusToken(thread.agents.get("typed-completed-agent").status), "completed");
    assert.equal(statusToken(thread.agents.get("typed-running-agent").status), "running");

    model.applyEvent(event(14, 1, "thread.lifecycle", {state: "closed"}, "merge", {
        threadId: "lifecycle-thread",
    }));
    thread = model.thread("lifecycle-thread");
    assert.equal(statusToken(thread.status), "notLoaded");
    assert.equal(thread.raw.status, "notLoaded");
    assert.equal(thread.raw.lifecycle, "closed");
});

test("ordering, generation, and repository hints", () => {
    const model = new PresentationModel();
    model.applyEvent(event(1, 1, "thread.upsert", {thread: {id: "retained-a"}}, "merge", {threadId: "retained-a"}));
    model.applyEvent(event(2, 1, "thread.upsert", {thread: {id: "retained-b"}}, "merge", {threadId: "retained-b"}));
    model.applyEvent(result(3, 1, "threads.list", "list", true, {
        threads: [{id: "provider-a"}, {id: "provider-b"}, {id: "provider-a"}],
    }, "merge"));
    assert.deepEqual(model.threadOrder(), ["provider-a", "provider-b", "retained-b", "retained-a"]);

    const structural = new PresentationModel();
    structural.applyEvent(event(1, 1, "turn.upsert", {
        turn: {id: "placeholder-turn", status: "inProgress"},
    }, "merge", {threadId: "placeholder", turnId: "placeholder-turn"}));
    assert.notEqual(structural.thread("placeholder"), undefined);
    assert.deepEqual(structural.threadOrder(), []);
    structural.applyEvent(result(2, 1, "thread.resume", "resume", true, {
        thread: {id: "placeholder", parentThreadId: null},
    }, "merge"));
    assert.deepEqual(structural.threadOrder(), ["placeholder"]);
    structural.applyEvent(result(3, 1, "threads.list", "structural", true, {
        threads: [{id: "child", parentThreadId: "parent"}, {id: "parent", parentThreadId: null}],
    }, "merge"));
    assert.deepEqual(structural.childOwnership("child"), {parentThreadId: "parent", agentId: ""});
    assert.deepEqual(structural.threadOrder(), ["parent", "placeholder"]);
    structural.applyEvent(result(4, 1, "thread.read", "read-parent", true, {
        thread: {id: "parent", parentThreadId: null, turns: []},
    }, "replace", {threadId: "parent"}));
    assert.deepEqual(structural.childOwnership("child"), {parentThreadId: "parent", agentId: ""});
    assert.deepEqual(structural.threadOrder(), ["parent", "placeholder"]);

    model.applyEvent(event(7, 1, "notice.added", {message: "scoped telemetry"}, "none", {threadId: "phantom-none"}));
    model.applyEvent(event(8, 1, "thread.goal.changed", {}, "remove", {threadId: "phantom-remove"}));
    assert.equal(model.thread("phantom-none"), undefined);
    assert.equal(model.thread("phantom-remove"), undefined);
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
    assert.deepEqual(model.thread("repository-thread").turns.get("repository-turn").items.get("change").raw.changes,
        [{path: "lib/example.cpp"}, {path: "removed.txt"}]);

    model.applyEvent(event(1, 2, "connection.lifecycle", {state: "connected"}, "replace"));
    model.applyEvent(event(11, 1, "thread.upsert", {thread: {id: "stale"}}, "merge", {threadId: "stale"}));
    assert.equal(model.thread("stale"), undefined);
    model.applyEvent(event(2, 2, "pending-request.upsert", {requestId: 42, category: "approval", request: {}}, "merge", {threadId: "provider-a"}));
    assert.equal(model.pendingRequestPresentations().size, 1);
    model.applyEvent(event(3, 2, "connection.lifecycle", {state: "retrying", detail: "again"}, "replace"));
    assert.equal(model.pendingRequestPresentations().size, 0);
    assert.equal(model.connection().retrying, true);
});

test("child ownership, correlation, replacement, and removal invariants", () => {
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
    model.notePromptActivity("grandchild", 100);
    assert.equal(model.thread("grandchild").localPromptActivityAt, 100);
    assert.equal(model.thread("child-one").localPromptActivityAt, 100);
    assert.equal(model.thread("parent").localPromptActivityAt, 100);
    assert.equal(model.thread("grandchild").recencyAt, undefined,
        "local confirmation does not rewrite the provider's recency value");

    model.applyEvent(event(5, 1, "conversation.item.upsert", {item: {
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
    assert.equal(statusToken(parent.agents.get("spawn-one").status), "completed");
    assert.equal(parent.agents.get("spawn-one").raw.resultText, "direct child result");
    assert.equal(parent.turns.get("parent-turn").items.get("spawn-one").raw.resultText, "direct child result");

    model.applyEvent(result(8, 1, "thread.read", "replace-child", true, {thread: {
        id: "child-one", status: {type: "idle"}, turns: [{id: "child-turn", status: "inProgress", items: [
            {id: "spawn-grandchild", type: "subAgentActivity", status: "started", agentThreadId: "grandchild"},
        ]}],
    }}, "replace", {threadId: "child-one"}));
    assert.equal(statusToken(parent.agents.get("spawn-one").status), "completed");
    assert.equal(Object.hasOwn(parent.agents.get("spawn-one").raw, "resultText"), false);

    model.applyEvent(event(9, 1, "thread.removed", {}, "remove", {threadId: "child-one"}));
    assert.equal(model.thread("child-one"), undefined);
    assert.equal(model.childOwnership("grandchild"), undefined);
    assert.deepEqual(model.threadOrder(), ["parent", "second-root", "grandchild"]);
    model.applyEvent(event(10, 1, "thread.removed", {}, "remove", {threadId: "parent"}));
    assert.deepEqual(model.threadOrder(), ["child-two", "second-root", "grandchild"]);
});

test("live agent rebind and cycle protection invariants", () => {
    const model = new PresentationModel();
    model.applyEvent(event(1, 1, "thread.upsert", {thread: {id: "rebind-parent"}}, "merge", {threadId: "rebind-parent"}));
    model.applyEvent(event(2, 1, "conversation.item.upsert", {item: {
        id: "stable-agent", type: "subAgentActivity", status: "completed", resultText: "old result", agentThreadId: "old-child",
    }}, "merge", {threadId: "rebind-parent", turnId: "rebind-turn", itemId: "stable-agent"}));
    model.applyEvent(event(3, 1, "conversation.item.upsert", {item: {
        id: "stable-agent", type: "subAgentActivity", status: "started", agentThreadId: "new-child",
    }}, "merge", {threadId: "rebind-parent", turnId: "rebind-turn", itemId: "stable-agent"}));
    const parent = model.thread("rebind-parent");
    const agent = parent.agents.get("stable-agent");
    assert.deepEqual(parent.childThreadOrder, ["new-child"]);
    assert.equal(model.childOwnership("old-child"), undefined);
    assert.deepEqual(model.threadOrder(), ["rebind-parent", "old-child"]);
    assert.equal(agent.childThreadId, "new-child");
    assert.equal(statusToken(agent.status), "running");
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
    assert.equal(statusToken(agent.status), "completed");
    assert.equal(agent.raw.resultText, "new child result");

    model.applyEvent(event(7, 1, "conversation.item.upsert", {item: {
        id: "cycle-agent", type: "subAgentActivity", status: "started", agentThreadId: "rebind-parent",
    }}, "merge", {threadId: "new-child", turnId: "cycle-turn", itemId: "cycle-agent"}));
    assert.equal(model.childOwnership("rebind-parent"), undefined);
    assert.deepEqual(model.thread("new-child").childThreadOrder, []);
});

test("provider generations are scoped to one frontend connection generation", () => {
    const model = new PresentationModel();
    model.applyEvent(event(1, 1, "connection.lifecycle", {state: "connected"}, "replace"));
    model.applyEvent(event(2, 1, "connection.provider", {generation: 10, state: "ready"}, "replace"));
    model.applyEvent(event(3, 1, "thread.upsert", {thread: {id: "old-provider"}}, "merge", {threadId: "old-provider"}));
    const oldPresentation = model.thread("old-provider");
    assert.equal(model.applyEvent(event(1, 2, "connection.lifecycle", {state: "connected"}, "replace")),
        "provider-reset");
    assert.equal(model.thread("old-provider"), undefined,
        "a new connection generation rebuilds presentation state through discovery and hydration");
    model.applyEvent(event(2, 2, "connection.provider", {generation: 1, state: "ready"}, "replace"));
    model.applyEvent(event(3, 2, "thread.upsert", {thread: {id: "old-provider"}}, "merge", {threadId: "old-provider"}));
    assert.notEqual(model.thread("old-provider"), oldPresentation);
    assert.equal(model.connection().providerGeneration, 1);
    assert.equal(model.connection().providerState, "ready");
});

test("settings acknowledgements reconcile retained drafts within one thread incarnation", () => {
    const {model, normalizer} = pipeline();
    const catalog = turnSettingCatalog({models: [], permissionProfiles: []});
    const drafts = new Map();
    const identity = "stable-settings-identity";
    normalizer.transportEvent("connected");
    normalizer.bridgeEvent({kind: "bridge.provider", state: "ready", providerGeneration: 1});
    normalizer.serverNotification("thread/started", {
        thread: {id: "recreated-settings", approvalPolicy: "never"},
    });
    normalizer.serverNotification("thread/settings/updated", {
        threadId: "recreated-settings", threadSettings: {approvalPolicy: "never"},
    });
    let thread = model.thread("recreated-settings");
    let draft = settingDraftFor(drafts, identity, thread.raw, catalog, thread.settingStamps);
    draft = changeSettingDraft(drafts, identity, thread.raw, catalog, thread.settingStamps,
        "approval", "never");
    assert.equal(draft.touched.has("approval"), true);
    normalizer.serverNotification("thread/started", {
        thread: {id: "recreated-settings", approvalPolicy: "on-request"},
    });
    thread = model.thread("recreated-settings");
    draft = settingDraftFor(drafts, identity, thread.raw, catalog, thread.settingStamps);
    assert.equal(draft.values.approval, "never");
    assert.equal(draft.touched.has("approval"), true);

    normalizer.serverNotification("thread/settings/updated", {
        threadId: "recreated-settings", threadSettings: {approvalPolicy: "never"},
    });
    thread = model.thread("recreated-settings");
    draft = settingDraftFor(drafts, identity, thread.raw, catalog, thread.settingStamps);
    assert.equal(draft.values.approval, "never");
    assert.equal(draft.touched.has("approval"), false);
});

for (const [type, field, eventType, textKey] of [
    ["agentMessage", "text", "conversation.item.append", "text"],
    ["fileChange", "output", "conversation.file-change.output-appended", "delta"],
]) test(`${type} scalar deltas preserve identity and normalize input once`, () => {
    const model = new PresentationModel();
    const scope = Object.freeze({threadId: "thread", turnId: "turn", itemId: "item", delta: "not content"});
    model.applyEvent(event(1, 1, "conversation.item.upsert", {item: {
        id: "item", type, [field]: "before",
    }}, "merge", scope));
    const item = model.thread("thread").turns.get("turn").items.get("item");
    const raw = item.raw;
    let sequence = 2;
    for (const text of ["", null, 23, "🧭é"]) {
        const data = Object.freeze({field, [textKey]: text});
        assert.equal(model.applyEvent(event(sequence++, 1, eventType, data, "merge", scope)), "accepted");
        assert.equal(model.thread("thread").turns.get("turn").items.get("item"), item);
        assert.equal(item.raw, raw);
        assert.equal(raw[field], `before${typeof text === "string" ? text : ""}`);
    }
    assert.equal(item.textRetention.get(field).retainedBytes, new TextEncoder().encode("before🧭é").length);
    assert.equal(scope.delta, "not content");
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
