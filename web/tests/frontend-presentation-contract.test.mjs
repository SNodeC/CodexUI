import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {performance} from "node:perf_hooks";
import test from "node:test";
import {createElement} from "react";
import {renderToStaticMarkup} from "react-dom/server";

import {
    PresentationModel, ProtocolNormalizer, changeSettingDraft, displayStatus, effectivePlanStepStatus, event,
    isActiveStatus, isTerminalTurnStatus, isWorkingStatus, projectTurnPlan,
    pendingDecisionOptions, pendingResponse, settingDraftFor, settingPresentation,
    settingPromptOptions, statusFromValue, statusToken, statusTone, turnSettingCatalog,
    humanizeProtocolLabel, projectConversation,
} from "../dist/index.js";
import {Card, cardCopyContent, userMessageMarkdownText} from "../dist/app/App.js";

const contract = JSON.parse(readFileSync(
    new URL("../../tests/fixtures/frontend-presentation.json", import.meta.url), "utf8",
));

assert.equal(contract.schemaVersion, 6);

for (const entry of contract.labelCases) test(`label contract: ${JSON.stringify(entry.input)}`, () => {
    assert.equal(humanizeProtocolLabel(entry.input), entry.expected);
});
for (const [index, entry] of contract.copyCases.entries()) test(`Copy contract: ${index}`, () => {
    assert.deepEqual(cardCopyContent({kind: "commandExecution", payload: entry}), {text: entry.expected, markdown: false});
});
for (const [index, entry] of contract.markdownCases.entries()) test(`Markdown contract: ${index}`, () => {
    assert.equal(userMessageMarkdownText(entry.source), entry.expected);
    if (entry.paragraphs) {
        const card = {key: {kind: "item", threadId: "t", turnId: "r", itemId: "i"},
            kind: "userMessage", threadId: "t", turnId: "r", itemId: "i",
            payload: {text: entry.source, imagePaths: []}, status: statusFromValue("")};
        const markup = renderToStaticMarkup(createElement(Card, {
            card, active: false, collapsed: false, onToggle() {},
        }));
        assert.deepEqual([...markup.matchAll(/<p>(.*?)<\/p>/gsu)]
            .map(match => match[1].split(/<br\/>\n?/u)), entry.paragraphs);
    }
});
for (const [index, entry] of contract.genericDetailCases.entries()) test(`generic detail contract: ${index}`, () => {
    const {model, normalizer} = pipeline();
    normalizer.serverNotification("thread/started", {thread: {id: "detail"}});
    normalizer.serverNotification("turn/started", {threadId: "detail", turn: {id: "turn"}});
    normalizer.serverNotification("item/completed", {threadId: "detail", turnId: "turn", item: fixtureRaw(entry)});
    const card = projectConversation(model.thread("detail"), [], 80, 0).sections[0].cards[0];
    const repeat = entry.expectedRepeat;
    const expected = repeat ? `a: ${repeat.text.repeat(repeat.count)}\n\n[Activity details truncated]` : entry.expected;
    assert.equal(card.payload.displayDetail, expected);
    assert.deepEqual(cardCopyContent(card), {text: expected, markdown: false});
});

test("detail projection skips known-card extras and stops traversing at its output bound", () => {
    const {model, normalizer} = pipeline();
    normalizer.serverNotification("thread/started", {thread: {id: "detail"}});
    normalizer.serverNotification("turn/started", {threadId: "detail", turn: {id: "turn"}});
    normalizer.serverNotification("item/completed", {threadId: "detail", turnId: "turn",
        item: {id: "item", type: "agentMessage", text: "answer"}});
    const thread = model.thread("detail"), raw = thread.turns.get("turn").items.get("item").raw;
    Object.defineProperty(raw, "extra", {enumerable: true, configurable: true,
        get() { throw new Error("known cards must not traverse unrelated detail"); }});
    assert.equal(projectConversation(thread, [], 80, 0).sections[0].cards[0].payload.text, "answer");
    delete raw.extra;
    raw.type = "custom";
    let visits = 0;
    raw.a = new Proxy(Array(100_000).fill("value"), {get(target, key, receiver) {
        if (typeof key === "string" && /^[0-9]+$/u.test(key)) ++visits;
        return Reflect.get(target, key, receiver);
    }});
    const detail = projectConversation(thread, [], 80, 0).sections[0].cards[0].payload.displayDetail;
    assert.ok(visits > 0 && visits < 1000, `bounded array traversal: ${visits}`);
    assert.ok(Buffer.byteLength(detail) <= 4030);
    assert.ok(detail.endsWith("[Activity details truncated]"));
});

const SettingFields = ["model", "effort", "personality", "sandbox", "network", "approval",
    "reviewer", "cwd", "permissionProfile", "serviceTier", "summary", "collaboration"];

function pipeline() {
    const model = new PresentationModel();
    const normalizer = new ProtocolNormalizer(frame => {
        model.applyEvent(frame);
        return true;
    });
    return {model, normalizer};
}

function fixtureRaw(entry) {
    const raw = structuredClone(entry.raw);
    for (const repeat of entry.repeats ?? []) {
        let target = raw;
        for (const key of repeat.path.slice(0, -1)) target = target[key];
        const key = repeat.path.at(-1);
        target[key] = Object.hasOwn(repeat, "text") ? repeat.text.repeat(repeat.count)
            : Array.from({length: repeat.count}, () => structuredClone(repeat.element));
    }
    return raw;
}

function pendingFixtureRequest(entry) {
    const {model, normalizer} = pipeline();
    normalizer.serverRequest(entry.method, entry.id, fixtureRaw(entry));
    const requests = [...model.pendingRequestPresentations().values()];
    assert.equal(requests.length, 1);
    return requests[0];
}

for (const group of contract.statusCases) for (const input of group.inputs) {
    test(`presentation status contract: ${group.id}: ${JSON.stringify(input)}`, () => {
        const status = statusFromValue(input);
        assert.equal(status.semantic, group.expected.semantic);
        assert.equal(statusToken(status), group.expected.token);
        assert.equal(displayStatus(status), group.expected.display);
        assert.equal(statusTone(status), group.expected.tone);
        assert.equal(isActiveStatus(status), group.expected.active);
        assert.equal(isWorkingStatus(status), group.expected.working);
        assert.equal(isTerminalTurnStatus(status), group.expected.terminal);
    });
}

for (const entry of contract.planCases) {
    test(`effective plan status contract: ${entry.id}`, () => {
        const status = effectivePlanStepStatus(
            statusFromValue(entry.step), statusFromValue(entry.turn), statusFromValue(entry.thread),
        );
        assert.equal(status.semantic, entry.expected.semantic);
        assert.equal(statusToken(status), entry.expected.token);
    });
}

for (const entry of contract.lifecycleCases) {
    test(`lifecycle status contract: ${entry.id}`, () => {
        const {model, normalizer} = pipeline();
        normalizer.serverNotification("thread/started", {thread: {
            id: entry.id, ...(entry.initialThreadStatus ? {status: entry.initialThreadStatus} : {}),
        }});
        if (entry.entity !== "turn") normalizer.serverNotification("turn/started", {
            threadId: entry.id, turn: {id: "turn"},
        });
        const value = {id: entry.entity === "item" ? "item" : "turn"};
        if (Object.hasOwn(entry, "status")) value.status = entry.status;
        if (entry.entity === "item") value.type = "agentMessage";
        if (entry.entity === "thread") normalizer.serverNotification("thread/status/changed", {
            threadId: entry.id, status: entry.status,
        });
        else normalizer.serverNotification(`${entry.entity}/${entry.lifecycle}`, entry.entity === "turn"
            ? {threadId: entry.id, turn: value}
            : {threadId: entry.id, turnId: "turn", item: value});
        const thread = model.thread(entry.id);
        const status = entry.entity === "thread" ? thread.status
            : entry.entity === "turn" ? thread.turns.get("turn").status
                : statusFromValue(thread.turns.get("turn").items.get("item").raw.status);
        assert.equal(status.semantic, entry.expected.semantic);
        assert.equal(statusToken(status), entry.expected.token);
        if (entry.expectedThreadStatus)
            assert.equal(statusToken(thread.status), entry.expectedThreadStatus);
        if (Object.hasOwn(entry, "expectedActiveTurnId"))
            assert.equal(model.activeTurnId(entry.id) ?? "", entry.expectedActiveTurnId);
    });
}

for (const entry of contract.planReplacementCases) {
    test(`plan replacement contract: ${entry.id}`, () => {
        const {model, normalizer} = pipeline();
        normalizer.serverNotification("thread/started", {thread: {id: entry.id}});
        normalizer.serverNotification("turn/started", {
            threadId: entry.id, turn: {id: "turn"},
        });
        normalizer.serverNotification("turn/plan/updated", {
            threadId: entry.id, turnId: "turn", explanation: entry.initialExplanation,
            plan: [{step: "obsolete step", status: "pending"}],
        });
        normalizer.serverNotification("turn/plan/updated", {
            threadId: entry.id, turnId: "turn", plan: entry.replacementPlan,
        });
        const thread = model.thread(entry.id);
        const plan = projectTurnPlan(thread.turns.get("turn"), thread.status);
        assert.equal(plan.explanation, entry.expectedExplanation);
        assert.equal(plan.steps.length, entry.expectedStepCount);
    });
}

for (const entry of contract.childLifecycleCases) {
    test(`child lifecycle contract: ${entry.id}`, () => {
        const {model, normalizer} = pipeline();
        normalizer.serverNotification("thread/started", {thread: {id: "parent"}});
        normalizer.serverNotification("turn/started", {
            threadId: "parent", turn: {id: "parent-turn"},
        });
        normalizer.serverNotification("item/started", {
            threadId: "parent", turnId: "parent-turn",
            item: {id: "spawn", type: "subAgentActivity", status: "running", agentThreadId: "child"},
        });
        normalizer.serverNotification("thread/started", {thread: {id: "child", status: "running"}});
        normalizer.serverNotification(`thread/${entry.lifecycle}`, {threadId: "child"});
        assert.equal(statusToken(model.thread("parent").agents.get("spawn").status), entry.expectedAgentStatus);
    });
}

for (const entry of contract.agentStatusCases) {
    test(`agent status contract: ${entry.id}`, () => {
        const {model, normalizer} = pipeline();
        normalizer.serverNotification("thread/started", {thread: {id: "parent"}});
        normalizer.serverNotification("turn/started", {
            threadId: "parent", turn: {id: "turn"},
        });
        model.applyEvent(event(3, 1, "conversation.item.upsert", {item: {
                id: entry.id, type: "collabAgentToolCall", tool: "spawn_agent",
                kind: entry.kind, receiverThreadIds: [`${entry.id}-child`],
                ...(entry.status ? {status: entry.status} : {}),
            }}, "merge", {threadId: "parent", turnId: "turn", itemId: entry.id}));
        assert.equal(statusToken(model.thread("parent").agents.get(entry.id).status),
            entry.expectedAgentStatus);
    });
}

for (const entry of contract.turnSettingsCases) {
    test(`turn settings contract: ${entry.id}`, async context => {
        const {model, normalizer} = pipeline();
        normalizer.transportEvent("connected");
        normalizer.operationResult("models.list", "settings-models", {}, {
            result: {data: entry.catalogs.models},
        });
        normalizer.operationResult("permission-profiles.list", "settings-profiles", {}, {
            result: {data: entry.catalogs.permissionProfiles},
        });
        for (const thread of entry.threads)
            normalizer.serverNotification("thread/started", {thread: thread.canonical});

        const catalog = turnSettingCatalog(model.turnSettingsCatalogs());
        const threads = new Map(entry.threads.map(thread => [thread.id, thread]));
        const drafts = new Map();
        const newThread = settingDraftFor(drafts, entry.newThread.key, {cwd: entry.newThread.cwd}, catalog);
        await context.test("new-thread projection", () => {
            assert.equal(newThread.values.cwd, entry.newThread.cwd);
            assert.deepEqual([...newThread.touched], []);
        });

        let selectedId = "";
        let selectedKey = "";
        let draft;
        const observedAcknowledgements = new Map();
        const select = id => {
            const fixture = threads.get(id);
            const thread = model.thread(id);
            selectedId = id;
            selectedKey = fixture.key;
            draft = settingDraftFor(drafts, selectedKey, thread.raw, catalog, thread.settingStamps);
        };
        const change = ([field, value]) => {
            const thread = model.thread(selectedId);
            draft = changeSettingDraft(drafts, selectedKey, thread.raw, catalog, thread.settingStamps, field, value);
        };
        const verify = expected => {
            if (!expected) return;
            if (expected.identity) assert.equal(selectedKey, expected.identity);
            for (const [field, value] of Object.entries(expected.values ?? {}))
                assert.equal(draft.values[field], value, `${entry.id}: ${field}`);
            if (expected.touched) assert.deepEqual(SettingFields.filter(field => draft.touched.has(field)), expected.touched);
            const thread = model.thread(selectedId);
            const observed = observedAcknowledgements.get(selectedKey) ?? new Map();
            const acknowledged = SettingFields.filter(field => {
                const stamp = thread.settingStamps.get(field === "network" ? "sandbox" : field);
                return (stamp?.acknowledgements ?? 0) > (observed.get(field) ?? 0);
            });
            for (const field of SettingFields) {
                const stamp = thread.settingStamps.get(field === "network" ? "sandbox" : field);
                observed.set(field, Math.max(observed.get(field) ?? 0, stamp?.acknowledgements ?? 0));
            }
            observedAcknowledgements.set(selectedKey, observed);
            const presentation = settingPresentation(draft, catalog);
            if (expected.catalog) {
                const selected = draft.values.model === "default"
                    ? catalog.models.find(candidate => candidate.isDefault) : catalog.models.find(candidate =>
                        candidate.choice.value === draft.values.model);
                assert.deepEqual({
                    models: catalog.models.map(candidate => candidate.choice.value),
                    modelLabels: catalog.models.map(candidate => candidate.choice.label),
                    modelDescriptions: catalog.models.map(candidate => candidate.choice.description),
                    modelDefaults: catalog.models.map(candidate => candidate.isDefault),
                    modelPersonalitySupport: catalog.models.map(candidate => candidate.supportsPersonality),
                    defaultEfforts: catalog.models.map(candidate => candidate.defaultReasoningEffort),
                    defaultServiceTiers: catalog.models.map(candidate => candidate.defaultServiceTier),
                    efforts: selected?.reasoningEfforts ?? [],
                    serviceTiers: selected?.serviceTiers.map(choice => choice.value) ?? [],
                    serviceTierLabels: selected?.serviceTiers.map(choice => choice.label) ?? [],
                    serviceTierDescriptions: selected?.serviceTiers.map(choice => choice.description) ?? [],
                    permissionProfiles: catalog.permissionProfiles.map(choice => choice.value),
                    permissionProfileDescriptions: catalog.permissionProfiles.map(choice => choice.description),
                    selectedModel: selected?.choice.value ?? "",
                    personalityEnabled: presentation.personalityDisabledReason === "",
                }, expected.catalog);
            }
            if (expected.acknowledged) assert.deepEqual(acknowledged, expected.acknowledged);
            const options = settingPromptOptions(draft, catalog);
            if (expected.threadOptions) assert.deepEqual(options.thread, expected.threadOptions);
            if (expected.turnOptions) assert.deepEqual(options.turn, expected.turnOptions);
        };

        for (const [index, step] of entry.steps.entries()) {
            if (step.select) select(step.select);
            if (step.change) change(step.change);
            for (const authored of step.changes ?? []) change(authored);
            const updates = step.updates ?? (step.update ? [step.update] : []);
            for (const update of updates) {
                normalizer.serverNotification("thread/settings/updated", {
                    threadId: selectedId, threadSettings: update,
                });
            }
            if (updates.length > 0) select(selectedId);
            await context.test(`step ${index + 1}`, () => verify(step.expect));
        }
    });
}

test("turn settings catalog projection scales linearly", () => {
    const source = count => ({
        models: Array.from({length: count}, (_, index) => ({
            model: `model-${index}`, displayName: `Model ${index}`,
            supportedReasoningEfforts: [{reasoningEffort: "low"}],
        })),
        permissionProfiles: Array.from({length: count}, (_, index) => ({
            id: `profile-${index}`, description: `Profile ${index}`,
        })),
    });
    const measure = raw => {
        const started = performance.now();
        const catalog = turnSettingCatalog(raw);
        const draft = settingDraftFor(new Map(), "catalog-scale", {}, catalog);
        const presentation = settingPresentation(draft, catalog);
        return {milliseconds: performance.now() - started, catalog, presentation};
    };
    const minimum = raw => {
        let result;
        for (let run = 0; run < 5; ++run) {
            const current = measure(raw);
            if (!result || current.milliseconds < result.milliseconds) result = current;
        }
        return result;
    };

    measure(source(64));
    const small = minimum(source(2_048));
    const large = minimum(source(8_192));
    assert.equal(small.presentation.models.length, 2_049);
    assert.equal(small.presentation.permissionProfiles.length, 2_049);
    assert.equal(large.presentation.models.length, 8_193);
    assert.equal(large.presentation.permissionProfiles.length, 8_193);
    assert(large.milliseconds <= small.milliseconds * 7 + 10,
        `settings catalog scaling exceeded its linear gate: ${small.milliseconds}/${large.milliseconds} ms`);
});

for (const entry of contract.pendingRequestCases) {
    test(`pending request kind contract: ${entry.id}`, () => {
        assert.equal(pendingFixtureRequest(entry).kind, entry.expectedKind);
    });
    test(`pending request actions contract: ${entry.id}`, () => {
        const actions = pendingDecisionOptions(pendingFixtureRequest(entry)).map(action => ({
            value: action.value,
            label: action.label,
            tone: action.tone,
            requiresInput: action.requiresInput ?? false,
        }));
        assert.deepEqual(actions, entry.expectedActions);
    });
    for (const attempt of entry.submissions) {
        test(`pending request response contract: ${entry.id}: ${attempt.id}`, () => {
            const authored = {
                choice: attempt.choice,
                ...(Object.hasOwn(attempt, "input") ? {input: attempt.input} : {}),
                ...(Object.hasOwn(attempt, "metadata") ? {metadata: attempt.metadata} : {}),
            };
            assert.deepEqual(pendingResponse(pendingFixtureRequest(entry), authored) ?? null,
                attempt.expected);
        });
    }
}

for (const entry of contract.pendingRequestBoundaryCases) {
    test(`pending request boundary contract: ${entry.id}`, () => {
        const request = pendingFixtureRequest(entry);
        assert.deepEqual(pendingDecisionOptions(request).map(action => action.value), entry.expectedActionValues);
        for (const choice of entry.rejectedChoices)
            assert.equal(pendingResponse(request, {choice}), undefined);
    });
}
