import {
    isObject,
    isPresentationFrame,
    jsonEqual,
    member,
    stringMember,
    threadSettingConcept,
    ThreadSettingFields,
} from "./PresentationProtocol.js";
import type {JsonObject, PresentationFrame, ThreadSettingStamp} from "./PresentationProtocol.js";
import {
    UnknownStatus, isActiveStatus, isEmptyStatus, isTerminalTurnStatus, statusFromValue, statusToken,
} from "./PresentationStatus.js";
import type {PresentationStatus} from "./PresentationStatus.js";

const MaximumIndexedTextParts = 4096;
const MaximumRetainedStreamBytes = 256 * 1024;
const RetainedStreamTailBytes = 192 * 1024;
const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

function utf8ByteLength(value: string): number {
    if (value.length > 64) return textEncoder.encode(value).length;
    let bytes = 0;
    for (let index = 0; index < value.length; ++index) {
        const code = value.charCodeAt(index);
        if (code <= 0x7f) ++bytes;
        else if (code <= 0x7ff) bytes += 2;
        else if (code >= 0xd800 && code <= 0xdbff && index + 1 < value.length
            && value.charCodeAt(index + 1) >= 0xdc00 && value.charCodeAt(index + 1) <= 0xdfff) {
            bytes += 4; ++index;
        } else bytes += 3;
    }
    return bytes;
}

export interface ItemPresentation {
    id: string;
    raw: JsonObject;
    textRetention?: Map<string, {retainedBytes: number; discardedBytes: number}>;
}

export interface TurnPresentation {
    id: string;
    status: PresentationStatus;
    itemOrder: string[];
    items: Map<string, ItemPresentation>;
    plan: JsonObject;
}

export interface AgentPresentation {
    id: string;
    itemId: string;
    ownerTurnId: string;
    childThreadId: string;
    status: PresentationStatus;
    raw: JsonObject;
}

export interface ChildThreadOwnership {
    parentThreadId: string;
    agentId: string;
}

export interface ThreadPresentation {
    id: string;
    activeTurnId?: string;
    title: string;
    preview: string;
    cwd: string;
    status: PresentationStatus;
    createdAt?: number;
    recencyAt?: number;
    localPromptActivityAt?: number;
    localNameOverlay?: string;
    lastActivityAt?: number;
    turnOrder: string[];
    turns: Map<string, TurnPresentation>;
    raw: JsonObject;
    settingStamps: ReadonlyMap<string, ThreadSettingStamp>;
    agentOrder: string[];
    agents: Map<string, AgentPresentation>;
    childThreadOrder: string[];
    archived: boolean;
    historyNextCursor: string;
    historyHasMore: boolean;
}

export interface PendingRequestPresentation {
    id: string;
    presentationKey: string;
    kind: string;
    threadId: string;
    generation: number;
    raw: unknown;
}

export interface ConnectionPresentation {
    connected: boolean;
    retrying: boolean;
    generation: number;
    providerGeneration: number;
    providerState: string;
}

export type PresentationApplyResult = "accepted" | "settings-unchanged" | "requests-reset"
    | "provider-reset" | "rejected";

function clone<T>(value: T): T {
    if (value === undefined || value === null || typeof value === "string"
        || typeof value === "number" || typeof value === "boolean") return value;
    return structuredClone(value);
}

function objectMember(value: unknown, name: string): JsonObject {
    const result = member(value, name);
    return isObject(result) ? result : {};
}

function unsignedValue(value: unknown): number {
    return typeof value === "number" && Number.isSafeInteger(value) && value >= 0 ? value : 0;
}

function requestKey(value: unknown): string {
    return value === null ? "" : JSON.stringify(value);
}

function isSpawnActivity(activity: unknown): boolean {
    const type = stringMember(activity, "type");
    if (type === "subAgentActivity") {
        const kind = stringMember(activity, "kind");
        return kind === "" || kind === "started";
    }
    if (type !== "collabAgentToolCall") return false;
    return ["spawn_agent", "spawnAgent", "spawn_agents_on_csv", "spawnAgentsOnCsv"]
        .includes(stringMember(activity, "tool"));
}

function childThreadIdentity(activity: unknown): string {
    const direct = stringMember(activity, "agentThreadId");
    if (direct !== "") return direct;
    const receivers = member(activity, "receiverThreadIds", []);
    return Array.isArray(receivers) && receivers.length === 1 && typeof receivers[0] === "string"
        ? receivers[0]
        : "";
}

function agentIdentity(activity: unknown, scope: unknown): string {
    return stringMember(scope, "itemId") || stringMember(activity, "id") || childThreadIdentity(activity);
}

function isStaleAgentReplay(
    owner: ThreadPresentation,
    scope: unknown,
    activity: unknown,
    live: boolean,
): boolean {
    if (live) return false;
    const childThreadId = childThreadIdentity(activity);
    const agent = owner.agents.get(agentIdentity(activity, scope));
    return childThreadId !== "" && agent !== undefined && agent.childThreadId !== ""
        && agent.childThreadId !== childThreadId;
}

function mergePreservingCompleteness(target: unknown, update: unknown): unknown {
    if (!isObject(target) || !isObject(update)) return update !== null || target === null ? clone(update) : target;
    for (const [key, value] of Object.entries(update)) {
        if (!Object.hasOwn(target, key)) target[key] = clone(value);
        else if (isObject(target[key]) && isObject(value)) mergePreservingCompleteness(target[key], value);
        else if (value !== null || target[key] === null) target[key] = clone(value);
    }
    return target;
}

function utf8Tail(value: string, retainedBytes: number): {text: string; retained: number; discarded: number} {
    const encoded = textEncoder.encode(value);
    if (encoded.length <= retainedBytes) return {text: value, retained: encoded.length, discarded: 0};
    let start = encoded.length - retainedBytes;
    while (start < encoded.length && (encoded[start]! & 0xc0) === 0x80) ++start;
    return {text: textDecoder.decode(encoded.subarray(start)), retained: encoded.length - start, discarded: start};
}

function retention(item: ItemPresentation, field: string): {retainedBytes: number; discardedBytes: number} {
    item.textRetention ??= new Map();
    let value = item.textRetention.get(field);
    if (!value) { value = {retainedBytes: 0, discardedBytes: 0}; item.textRetention.set(field, value); }
    return value;
}

function recordDiscardedText(item: ItemPresentation, field: string, bytes: number): void {
    if (bytes > 0) retention(item, field).discardedBytes += bytes;
}

function setRetainedTextBytes(item: ItemPresentation, field: string, bytes: number): void {
    retention(item, field).retainedBytes = bytes;
}

function boundScalarText(item: ItemPresentation, field: string, value: string): string {
    if (!item.textRetention?.has(field) && value.length <= Math.floor(MaximumRetainedStreamBytes / 3))
        return value;
    const encodedBytes = textEncoder.encode(value).length;
    if (encodedBytes <= MaximumRetainedStreamBytes) {
        if (item.textRetention?.has(field)) setRetainedTextBytes(item, field, encodedBytes);
        return value;
    }
    const tail = utf8Tail(value, RetainedStreamTailBytes);
    recordDiscardedText(item, field, tail.discarded);
    setRetainedTextBytes(item, field, tail.retained);
    return tail.text;
}

function boundIndexedText(item: ItemPresentation, field: string, parts: unknown[]): void {
    if (!item.textRetention?.has(field)) {
        const codeUnits = parts.reduce<number>((sum, part) => sum + (typeof part === "string" ? part.length : 0), 0);
        if (codeUnits <= Math.floor(MaximumRetainedStreamBytes / 3)) return;
    }
    const encoded = parts.map(part => typeof part === "string" ? textEncoder.encode(part).length : 0);
    let retained = encoded.reduce((sum, bytes) => sum + bytes, 0);
    if (retained > MaximumRetainedStreamBytes) {
        let toDiscard = retained - RetainedStreamTailBytes;
        for (let index = 0; index < parts.length && toDiscard > 0; ++index) {
            if (typeof parts[index] !== "string") continue;
            const value = parts[index] as string;
            const bytes = encoded[index]!;
            if (bytes <= toDiscard) {
                parts[index] = ""; toDiscard -= bytes; retained -= bytes; recordDiscardedText(item, field, bytes);
            } else {
                const tail = utf8Tail(value, bytes - toDiscard);
                parts[index] = tail.text; retained -= tail.discarded; toDiscard = 0;
                recordDiscardedText(item, field, tail.discarded);
            }
        }
    }
    if (retained > MaximumRetainedStreamBytes || item.textRetention?.has(field))
        setRetainedTextBytes(item, field, retained);
}

function resetIncomingTextBounds(item: ItemPresentation, incoming: JsonObject): void {
    for (const field of ["text", "output", "aggregatedOutput", "summary", "content"])
        if (Object.hasOwn(incoming, field)) {
            item.textRetention?.delete(field);
            if (item.textRetention?.size === 0) delete item.textRetention;
        }
}

function boundRetainedItemText(item: ItemPresentation): void {
    const boundScalar = (field: string): void => {
        if (typeof item.raw[field] === "string") item.raw[field] = boundScalarText(item, field, item.raw[field]);
    };
    const boundIndexed = (field: string): void => {
        if (Array.isArray(item.raw[field])) boundIndexedText(item, field, item.raw[field]);
    };
    const type = stringMember(item.raw, "type");
    if (type === "commandExecution") {
        boundScalar("aggregatedOutput"); boundScalar("output");
    } else if (type === "agentMessage" || type === "plan") {
        boundScalar("text");
    } else if (type === "reasoning") {
        boundIndexed("summary"); boundIndexed("content");
    } else if (type === "fileChange") {
        boundScalar("output");
    } else if (type === "userMessage") {
        return;
    } else {
        for (const field of ["text", "output", "aggregatedOutput"]) boundScalar(field);
        for (const field of ["summary", "content"]) boundIndexed(field);
    }
}

function appendText(item: ItemPresentation, field: string, delta: string): boolean {
    if (delta === "") return false;
    const existing = typeof item.raw[field] === "string" ? item.raw[field] : "";
    const retained = item.textRetention?.get(field);
    const existingBytes = retained?.retainedBytes ?? textEncoder.encode(existing).length;
    const deltaBytes = utf8ByteLength(delta);
    if (deltaBytes > MaximumRetainedStreamBytes) {
        const tail = utf8Tail(delta, RetainedStreamTailBytes);
        item.raw[field] = tail.text;
        setRetainedTextBytes(item, field, tail.retained);
        recordDiscardedText(item, field, existingBytes + tail.discarded);
        return true;
    }
    const combined = existing + delta;
    const combinedBytes = existingBytes + deltaBytes;
    if (combinedBytes > MaximumRetainedStreamBytes) item.raw[field] = boundScalarText(item, field, combined);
    else { item.raw[field] = combined; (retained ?? retention(item, field)).retainedBytes = combinedBytes; }
    return true;
}

function appendIndexedText(item: ItemPresentation, field: string, params: unknown, indexField: string): boolean {
    const rawIndex = isObject(params) ? params[indexField] : undefined;
    const position = typeof rawIndex === "number" && Number.isInteger(rawIndex) && rawIndex >= 0 ? rawIndex : 0;
    const delta = stringMember(params, "delta") || stringMember(params, "text");
    if (position >= MaximumIndexedTextParts || delta === "") return false;
    const parts: unknown[] = Array.isArray(item.raw[field]) ? item.raw[field] : [];
    item.raw[field] = parts;
    while (parts.length <= position) parts.push("");
    const retainedBefore = item.textRetention?.get(field)?.retainedBytes
        ?? parts.reduce<number>((sum, part) => sum + (typeof part === "string" ? textEncoder.encode(part).length : 0), 0);
    parts[position] = (typeof parts[position] === "string" ? parts[position] : "") + delta;
    const retained = retainedBefore + utf8ByteLength(delta);
    setRetainedTextBytes(item, field, retained);
    if (retained > MaximumRetainedStreamBytes) boundIndexedText(item, field, parts);
    return true;
}

function threadSettings(source: JsonObject, remove = false): JsonObject {
    const result: JsonObject = {};
    for (const key of ThreadSettingFields) if (Object.hasOwn(source, key)) {
        result[key] = source[key];
        if (remove) delete source[key];
    }
    return result;
}

function mergeEffectiveThreadSettings(thread: ThreadPresentation, incoming: JsonObject, generation: number, sequence: number,
    preserveChangesAfter?: number, acknowledgement = 0): boolean {
    let stamps: Map<string, ThreadSettingStamp> | undefined;
    let effectiveChange = false;
    for (const [key, value] of Object.entries(incoming)) {
        if ((key === "effort" && Object.hasOwn(incoming, "reasoningEffort"))
            || (key === "sandbox" && Object.hasOwn(incoming, "sandboxPolicy"))) continue;
        const retired = key === "effort" ? "reasoningEffort" : key === "reasoningEffort" ? "effort"
            : key === "sandbox" ? "sandboxPolicy" : key === "sandboxPolicy" ? "sandbox" : "";
        const concept = threadSettingConcept(key);
        const stamp = concept === "" ? undefined : thread.settingStamps.get(concept);
        if (preserveChangesAfter !== undefined && stamp?.generation === generation
            && stamp.sequence >= preserveChangesAfter) continue;
        const previousKey = Object.hasOwn(thread.raw, key) ? key
            : retired !== "" && Object.hasOwn(thread.raw, retired) ? retired : "";
        const changed = value === null ? previousKey !== ""
            : previousKey === "" || !jsonEqual(thread.raw[previousKey], value);
        effectiveChange ||= changed;
        if (retired !== "") delete thread.raw[retired];
        if (value === null) delete thread.raw[key];
        else thread.raw[key] = clone(value);
        if (concept !== "" && sequence !== 0 && (changed || acknowledgement !== 0)) {
            stamps ??= new Map(thread.settingStamps);
            stamps.set(concept, {generation, sequence,
                acknowledgements: acknowledgement || stamp?.acknowledgements || 0});
        }
    }
    if (stamps) thread.settingStamps = stamps;
    thread.cwd = stringMember(thread.raw, "cwd");
    return effectiveChange;
}

function newThread(id: string): ThreadPresentation {
    return {
        id, title: "", preview: "", cwd: "", status: UnknownStatus,
        turnOrder: [], turns: new Map(), raw: {},
        settingStamps: new Map(), agentOrder: [], agents: new Map(),
        childThreadOrder: [], archived: false, historyNextCursor: "", historyHasMore: false,
    };
}

function newTurn(id: string): TurnPresentation {
    return {id, status: UnknownStatus, itemOrder: [], items: new Map(), plan: {}};
}

export class PresentationModel {
    private readonly threads = new Map<string, ThreadPresentation>();
    private readonly childOwnerships = new Map<string, ChildThreadOwnership>();
    private readonly pendingRequests = new Map<string, PendingRequestPresentation>();
    private readonly connectionState: ConnectionPresentation = {
        connected: false, retrying: false, generation: 0,
        providerGeneration: 0, providerState: "",
    };
    private orderedThreads: string[] = [];
    private models: unknown = [];
    private permissionProfiles: unknown;
    private lastSequence = 0;
    private settingsAcknowledgement = 0;

    applyEvent(event: unknown): PresentationApplyResult {
        try {
            const previousGeneration = this.connectionState.generation;
            const result = this.applyValidatedEvent(event);
            if (previousGeneration !== 0 && this.connectionState.generation > previousGeneration)
                return "provider-reset";
            if (result === false) return "settings-unchanged";
            return result === true ? "accepted" : result ?? "accepted";
        } catch {
            // Presentation mutation is an untrusted-data boundary.
            return "rejected";
        }
    }

    threadOrder(): readonly string[] { return this.orderedThreads; }
    threadIds(): readonly string[] { return [...this.threads.keys()]; }
    threadTitles(): readonly string[] { return [...this.threads.values()].map(thread => thread.title); }
    thread(threadId: string): ThreadPresentation | undefined { return this.threads.get(threadId); }
    setThreadTitleLocally(threadId: string, title: string): void {
        const thread = this.threads.get(threadId);
        if (thread && title.trim() !== "") {
            thread.localNameOverlay = title;
            thread.title = title;
        }
    }
    noteThreadActivity(threadId: string, timestamp: number): void {
        this.noteActivity(threadId, timestamp, false);
    }
    notePromptActivity(threadId: string, timestamp: number): void {
        this.noteActivity(threadId, timestamp, true);
    }
    private noteActivity(threadId: string, timestamp: number, prompt: boolean): void {
        if (!Number.isSafeInteger(timestamp)) return;
        let current = threadId;
        const visited = new Set<string>();
        while (current !== "" && !visited.has(current)) {
            visited.add(current);
            const thread = this.threads.get(current);
            if (!thread) break;
            if (thread.lastActivityAt === undefined || timestamp > thread.lastActivityAt)
                thread.lastActivityAt = timestamp;
            if (prompt && (thread.localPromptActivityAt === undefined || timestamp > thread.localPromptActivityAt))
                thread.localPromptActivityAt = timestamp;
            const ownership = this.childOwnerships.get(current);
            if (!ownership) break;
            current = ownership.parentThreadId;
        }
    }
    childOwnership(childThreadId: string): ChildThreadOwnership | undefined {
        return this.childOwnerships.get(childThreadId);
    }
    activeTurnId(threadId: string): string | undefined {
        return this.thread(threadId)?.activeTurnId;
    }
    activeAgentChildIds(threadId: string): readonly string[] {
        const thread = this.thread(threadId);
        if (!thread) return [];
        const result = new Set<string>();
        for (const id of thread.agentOrder) {
            const agent = thread.agents.get(id);
            if (agent?.childThreadId && ["pending", "running"].includes(agent.status.semantic))
                result.add(agent.childThreadId);
        }
        return [...result];
    }
    connection(): Readonly<ConnectionPresentation> { return this.connectionState; }
    turnSettingsCatalogs(): {models: unknown; permissionProfiles: unknown} {
        return {models: this.models, permissionProfiles: this.permissionProfiles};
    }
    pendingRequestPresentations(): ReadonlyMap<string, PendingRequestPresentation> {
        return this.pendingRequests;
    }

    private applyValidatedEvent(candidate: unknown): boolean | "requests-reset" | "provider-reset" | "rejected" | undefined {
        if (!isPresentationFrame(candidate)) return "rejected";
        const event = candidate as PresentationFrame;
        const generation = unsignedValue(event.generation);
        if (this.connectionState.generation !== 0 && generation !== 0
            && generation < this.connectionState.generation) return "rejected";
        if (generation > this.connectionState.generation) {
            const replacesConnection = this.connectionState.generation !== 0;
            this.connectionState.generation = generation;
            this.lastSequence = 0;
            if (replacesConnection) {
                this.clearProviderState();
                this.connectionState.providerGeneration = 0;
                this.connectionState.providerState = "";
            } else this.pendingRequests.clear();
        }
        const sequence = unsignedValue(event.sequence);
        if (sequence !== 0) {
            if (sequence <= this.lastSequence) return "rejected";
            this.lastSequence = sequence;
        }
        const data = member(event, "data", {});
        const scope = objectMember(event, "scope");
        if (event.kind === "result") {
            if (event.ok !== true) return;
            const action = stringMember(event, "action");
            const requestSequence = unsignedValue(member(data, "requestSequence")) || undefined;
            if (action === "threads.list") this.mergeThreadList(member(data, "threads", []), requestSequence);
            else if (action === "thread.read") {
                this.upsertThread(objectMember(data, "thread"), stringMember(event, "authority") === "replace",
                    true, requestSequence);
            } else if (action === "thread.turns.list") {
                this.mergeTurnPage(stringMember(scope, "threadId"), member(data, "turns", []),
                    stringMember(data, "sortDirection"), stringMember(data, "nextCursor"));
            } else if (action === "thread.items.list") {
                this.mergeItemPage(stringMember(scope, "threadId"), member(data, "entries", []),
                    stringMember(data, "sortDirection"), stringMember(data, "cursor"));
            } else if (["thread.create", "thread.resume", "thread.fork"].includes(action)) {
                this.upsertThread(objectMember(data, "thread"), false, true, requestSequence);
            } else if (action === "turn.start") {
                const thread = this.threads.get(stringMember(scope, "threadId"));
                if (thread) {
                    const turn = this.upsertTurn(thread, objectMember(data, "turn"), false);
                    if (turn.id !== "" && !isTerminalTurnStatus(turn.status)) thread.activeTurnId = turn.id;
                }
            } else if (action === "models.list") {
                const listed = member(data, "models", []);
                if (Array.isArray(listed)) this.models = clone(listed);
            } else if (action === "permission-profiles.list") this.permissionProfiles = clone(data);
            return;
        }
        if (event.kind !== "event") return "rejected";
        const type = event.type;
        const authority = event.authority;
        if (type === "connection.lifecycle") {
            this.connectionState.generation = generation;
            const lifecycle = stringMember(data, "state");
            if (lifecycle === "connected") {
                this.connectionState.connected = true;
                this.connectionState.retrying = false;
            } else if (lifecycle === "connecting" || lifecycle === "retrying") {
                this.connectionState.connected = false;
                this.connectionState.retrying = true;
                this.pendingRequests.clear();
            } else if (lifecycle === "disconnected" || lifecycle === "failure") {
                this.connectionState.connected = false;
                this.connectionState.retrying = false;
                this.pendingRequests.clear();
            }
            return lifecycle === "connecting" || lifecycle === "retrying"
                || lifecycle === "disconnected" || lifecycle === "failure" ? "requests-reset" : undefined;
        }
        if (type === "connection.provider") {
            const incoming = isObject(data) ? data.generation : undefined;
            if (typeof incoming !== "number" || !Number.isSafeInteger(incoming) || incoming < 0
                || incoming < this.connectionState.providerGeneration) return "rejected";
            const state = stringMember(data, "state");
            const reset = (this.connectionState.providerGeneration !== 0
                && incoming > this.connectionState.providerGeneration) || state === "disconnected";
            if (reset) this.clearProviderState();
            this.connectionState.providerGeneration = incoming;
            this.connectionState.providerState = state;
            return reset ? "provider-reset" : undefined;
        }
        if (type === "thread.upsert") {
            this.upsertThread(objectMember(data, "thread"), false);
            return;
        }
        if (type === "thread.name.changed") {
            const thread = this.threads.get(stringMember(scope, "threadId"));
            if (thread && isObject(data) && typeof data.name === "string") {
                thread.raw.name = data.name;
                if (thread.localNameOverlay === data.name) delete thread.localNameOverlay;
                thread.title = thread.localNameOverlay ?? data.name;
            }
            return;
        }
        if (type === "thread.status.changed") {
            const thread = this.threads.get(stringMember(scope, "threadId"));
            if (thread) {
                thread.status = statusFromValue(member(data, "status"));
                thread.raw.status = clone(member(data, "status"));
                if (!isActiveStatus(thread.status)) delete thread.activeTurnId;
                this.updateOwningAgentStatus(thread.id, thread.status);
            }
            return;
        }
        if (type === "thread.lifecycle") {
            const thread = this.threads.get(stringMember(scope, "threadId"));
            if (thread) {
                const lifecycle = stringMember(data, "state");
                if (lifecycle === "archived") thread.archived = true;
                else if (lifecycle === "unarchived") thread.archived = false;
                else if (lifecycle === "closed") {
                    thread.status = statusFromValue("notLoaded");
                    thread.raw.status = "notLoaded";
                    delete thread.activeTurnId;
                    this.updateOwningAgentStatus(thread.id, thread.status);
                }
                thread.raw.lifecycle = lifecycle;
            }
            return;
        }
        if (type === "thread.removed") {
            this.removeThread(stringMember(scope, "threadId"));
            return;
        }
        if (type === "pending-request.upsert") {
            if (!isObject(data) || !Object.hasOwn(data, "requestId") || data.requestId === null) return;
            const key = requestKey(data.requestId);
            if (this.pendingRequests.has(key)) return "rejected";
            this.pendingRequests.set(key, {
                id: key, presentationKey: `request:${generation}:${sequence}:${key}`,
                kind: stringMember(data, "category"), threadId: stringMember(scope, "threadId"),
                generation, raw: clone(member(data, "request")),
            });
            return;
        }
        if (type === "pending-request.removed") {
            if (Object.hasOwn(scope, "requestId")) this.pendingRequests.delete(requestKey(scope.requestId));
            return;
        }

        const threadId = stringMember(scope, "threadId");
        if (threadId === "") return;
        let thread = this.threads.get(threadId);
        if (!thread) {
            if (authority === "none" || authority === "remove") return;
            this.upsertThread({id: threadId}, false, false);
            thread = this.threads.get(threadId);
            if (!thread) return;
        }
        if (type === "thread.settings.changed" && isObject(data)) {
            if (authority === "none" || authority === "remove") return;
            const update = Object.hasOwn(data, "threadSettings") ? data.threadSettings : data;
            if (!isObject(update)) return;
            return mergeEffectiveThreadSettings(thread, update, generation, sequence, undefined,
                ++this.settingsAcknowledgement);
        }
        if (authority === "none" || authority === "remove") return;
        if (type === "turn.upsert") {
            const rawTurn = objectMember(data, "turn");
            const turn = clone(rawTurn);
            const lifecycle = stringMember(data, "lifecycle");
            const incomingStatus = statusFromValue(member(turn, "status"));
            if (lifecycle === "completed" && isEmptyStatus(incomingStatus))
                turn.status = "completed";
            else if (lifecycle === "started" && isEmptyStatus(incomingStatus))
                turn.status = "running";
            const updated = this.upsertTurn(thread, turn, false);
            if (isActiveStatus(updated.status)) thread.activeTurnId = updated.id;
            else if (thread.activeTurnId === updated.id) delete thread.activeTurnId;
            return;
        }
        if (type === "plan.replaced") {
            const turn = this.upsertTurn(thread, {id: stringMember(scope, "turnId")}, false);
            turn.plan = {explanation: clone(member(data, "explanation")), steps: clone(member(data, "steps", []))};
            return;
        }
        if (type === "conversation.item.upsert") {
            if (isObject(data) && Object.hasOwn(data, "item")) {
                const turn = this.upsertTurn(thread, {id: stringMember(scope, "turnId")}, false);
                const item = this.upsertItem(thread, turn, objectMember(data, "item"), true,
                    stringMember(data, "lifecycle"));
            }
            return;
        }
        if (type === "conversation.reasoning.part-added") {
            const item = this.findItem(scope);
            const index = isObject(data) ? data.summaryIndex : undefined;
            if (item && typeof index === "number" && Number.isInteger(index) && index >= 0
                && index < MaximumIndexedTextParts) {
                const parts: unknown[] = Array.isArray(item.raw.summary) ? item.raw.summary : [];
                item.raw.summary = parts;
                if (parts.length <= index) {
                    while (parts.length <= index) parts.push("");
                }
            }
            return;
        }
        if (type === "conversation.file-change.output-appended") {
            const item = this.findItem(scope);
            if (item) appendText(item, "output", stringMember(data, "delta"));
            return;
        }
        if (type === "conversation.file-change.patch-replaced") {
            const item = this.findItem(scope);
            if (item) {
                const changes = clone(member(data, "changes", []));
                if (!jsonEqual(item.raw.changes, changes)) {
                    item.raw.changes = changes;
                }
            }
            return;
        }
        if (type === "conversation.mcp.progress") {
            const item = this.findItem(scope);
            if (item) {
                const progress: unknown[] = Array.isArray(item.raw.progress) ? item.raw.progress : [];
                item.raw.progress = progress;
                if (progress.length < MaximumIndexedTextParts) {
                    progress.push(stringMember(data, "message"));
                }
            }
            return;
        }
        if (type !== "conversation.item.append") return;
        const item = this.findItem(scope);
        if (!item) return;
        const field = stringMember(data, "field");
        const changed = field === "summary" ? appendIndexedText(item, "summary", data, "summaryIndex")
            : field === "content" ? appendIndexedText(item, "content", data, "contentIndex")
                : field !== "" && appendText(item, field, stringMember(data, "text"));
        if (!changed) return;
        if (stringMember(item.raw, "type") === "agentMessage")
            this.updateOwningAgentResult(threadId, stringMember(item.raw, "text"));
    }

    private mergeThreadList(listedThreads: unknown, preserveSettingsAfter?: number): void {
        if (!Array.isArray(listedThreads)) return;
        const listedIds = new Set<string>();
        const nextOrder: string[] = [];
        for (const raw of listedThreads) {
            const id = stringMember(raw, "id");
            if (id === "") continue;
            this.upsertThread(isObject(raw) ? raw : {}, false, false, preserveSettingsAfter);
            if (!this.childOwnerships.has(id) && !listedIds.has(id)) {
                listedIds.add(id);
                nextOrder.push(id);
            }
        }
        for (const id of this.orderedThreads) if (!listedIds.has(id)) nextOrder.push(id);
        this.orderedThreads = nextOrder;
    }

    private mergeTurnPage(threadId: string, listedTurns: unknown, direction: string, nextCursor: string): void {
        if (threadId === "" || !Array.isArray(listedTurns)) return;
        const thread = this.threads.get(threadId) ?? this.upsertThread({id: threadId}, false, false);
        const page: string[] = [];
        for (const raw of listedTurns) {
            if (!isObject(raw)) continue;
            const turn = this.upsertTurn(thread, raw, false);
            if (turn.id !== "" && !page.includes(turn.id)) page.push(turn.id);
        }
        if (direction === "desc") page.reverse();
        const retained = thread.turnOrder.filter(id => !page.includes(id));
        thread.turnOrder = direction === "desc" ? [...page, ...retained] : [...retained, ...page];
        this.refreshActiveTurn(thread);
        thread.historyNextCursor = nextCursor;
        thread.historyHasMore = nextCursor !== "";
    }

    private refreshActiveTurn(thread: ThreadPresentation): void {
        const active = [...thread.turnOrder].reverse().find(
            turnId => isActiveStatus(thread.turns.get(turnId)?.status ?? UnknownStatus));
        if (active) thread.activeTurnId = active;
        else delete thread.activeTurnId;
    }

    private mergeItemPage(threadId: string, listedEntries: unknown, direction: string, cursor: string): void {
        const thread = this.threads.get(threadId);
        if (!thread || !Array.isArray(listedEntries)) return;
        const pages = new Map<string, Set<string>>();
        for (const entry of listedEntries) {
            if (!isObject(entry)) continue;
            const turnId = stringMember(entry, "turnId");
            const item = member(entry, "item");
            if (turnId === "" || !isObject(item)) continue;
            const turn = thread.turns.get(turnId) ?? this.upsertTurn(thread, {id: turnId}, false);
            const merged = this.upsertItem(thread, turn, item);
            if (merged.id === "") continue;
            const page = pages.get(turnId) ?? new Set<string>();
            page.add(merged.id);
            pages.set(turnId, page);
        }
        for (const [turnId, items] of pages) {
            const turn = thread.turns.get(turnId);
            if (!turn) continue;
            const page = [...items];
            if (direction === "desc") page.reverse();
            const retained = turn.itemOrder.filter(id => !items.has(id));
            turn.itemOrder = direction === "desc" && cursor !== ""
                ? [...page, ...retained] : [...retained, ...page];
        }
    }

    private upsertThread(raw: JsonObject, replaceTurns: boolean, prependNewThread = true,
        preserveSettingsAfter?: number): ThreadPresentation {
        const id = stringMember(raw, "id");
        if (id === "") return newThread("");
        let result = this.threads.get(id);
        if (!result) {
            result = newThread(id);
            this.threads.set(id, result);
        }
        const previousThreadStatus = result.status;
        const terminalTurnStatuses = new Map<string, PresentationStatus>();
        if (replaceTurns) {
            for (const [turnId, turn] of result.turns)
                if (isTerminalTurnStatus(turn.status)) terminalTurnStatuses.set(turnId, turn.status);
        }
        const {turns: _turns, ...metadata} = raw;
        const threadFields = clone(metadata);
        const incomingSettings = threadSettings(threadFields, true);
        result.raw = replaceTurns ? {...threadSettings(result.raw), ...threadFields}
            : mergePreservingCompleteness(result.raw, threadFields) as JsonObject;
        mergeEffectiveThreadSettings(result, incomingSettings, this.connectionState.generation,
            this.lastSequence, preserveSettingsAfter);
        const name = stringMember(raw, "name");
        const preview = stringMember(raw, "preview");
        if (name !== "" && result.localNameOverlay === name) delete result.localNameOverlay;
        if (result.localNameOverlay !== undefined) result.title = result.localNameOverlay;
        else if (name !== "") result.title = name;
        else if (preview !== "") result.title = preview.slice(0, 80);
        else if (result.title === "") result.title = id.slice(0, 12);
        if (preview !== "") result.preview = preview;
        if (Object.hasOwn(raw, "status")) result.status = statusFromValue(raw.status);
        if (typeof raw.createdAt === "number" && Number.isInteger(raw.createdAt))
            result.createdAt = raw.createdAt;
        const updatedAt = raw.updatedAt;
        if (typeof updatedAt === "number" && Number.isInteger(updatedAt)
            && (result.lastActivityAt === undefined || updatedAt > result.lastActivityAt))
            result.lastActivityAt = updatedAt;
        const recencyAt = raw.recencyAt;
        if (typeof recencyAt === "number" && Number.isInteger(recencyAt)
            && (result.recencyAt === undefined || recencyAt > result.recencyAt)) result.recencyAt = recencyAt;
        if (result.recencyAt !== undefined
            && (result.lastActivityAt === undefined || result.recencyAt > result.lastActivityAt))
            result.lastActivityAt = result.recencyAt;
        if (typeof raw.archived === "boolean") result.archived = raw.archived;
        if (Object.hasOwn(raw, "parentThreadId")) {
            const parentThreadId = stringMember(raw, "parentThreadId");
            if (parentThreadId !== "") this.retainStructuralOwnership(id, parentThreadId);
            else if (this.childOwnerships.get(id)?.agentId === "")
                this.releaseChildOwnership(id, false);
        }
        if (prependNewThread && !this.childOwnerships.has(id) && !this.orderedThreads.includes(id))
            this.orderedThreads = [id, ...this.orderedThreads];
        if (Array.isArray(raw.turns)) {
            let previouslyOwnedChildren: string[] = [];
            if (replaceTurns) {
                previouslyOwnedChildren = result.childThreadOrder.filter(child =>
                    (this.childOwnerships.get(child)?.agentId ?? "") !== "");
                for (const child of previouslyOwnedChildren) this.releaseChildOwnership(child, false);
                result.turnOrder = [];
                result.turns.clear();
                result.agentOrder = [];
                result.agents.clear();
            }
            for (const turn of raw.turns) if (isObject(turn)) this.upsertTurn(result, turn, replaceTurns);
            if (replaceTurns) {
                for (const child of previouslyOwnedChildren) {
                    if (!this.childOwnerships.has(child) && this.threads.has(child) && !this.orderedThreads.includes(child))
                        this.orderedThreads = [...this.orderedThreads, child];
                }
                for (const [turnId, terminalStatus] of terminalTurnStatuses) {
                    const turn = result.turns.get(turnId);
                    if (turn && isActiveStatus(turn.status)) turn.status = terminalStatus;
                }
            }
            this.refreshActiveTurn(result);
            if (!result.activeTurnId && isActiveStatus(result.status)
                && previousThreadStatus.semantic === "completed") {
                result.status = previousThreadStatus;
                result.raw.status = statusToken(previousThreadStatus);
            }
            this.synchronizeOwningAgent(id, replaceTurns);
        } else if (Object.hasOwn(raw, "status") && result.status.semantic !== "notLoaded") {
            this.updateOwningAgentStatus(id, result.status);
        }
        return result;
    }

    private upsertTurn(thread: ThreadPresentation, raw: JsonObject, replaceItems: boolean): TurnPresentation {
        const id = stringMember(raw, "id");
        if (id === "") return newTurn("");
        let result = thread.turns.get(id);
        if (!result) {
            result = newTurn(id);
            thread.turns.set(id, result);
            thread.turnOrder.push(id);
        }
        const status = statusFromValue(member(raw, "status"));
        if (!isEmptyStatus(status) && !(isTerminalTurnStatus(result.status) && isActiveStatus(status))) result.status = status;
        if (Array.isArray(raw.items)) {
            if (replaceItems) {
                result.itemOrder = [];
                result.items.clear();
            }
            for (const item of raw.items) this.upsertItem(thread, result, item);
        }
        this.updateOwningAgentStatus(thread.id, result.status);
        return result;
    }

    private upsertItem(thread: ThreadPresentation, turn: TurnPresentation, rawValue: unknown,
        live = false, lifecycle = ""): ItemPresentation {
        const raw = isObject(rawValue) ? rawValue : {};
        const id = stringMember(raw, "id");
        if (id === "") return {id: "", raw: {}};
        const scope = {threadId: thread.id, turnId: turn.id, itemId: id};
        const incomingType = stringMember(raw, "type");
        if (["subAgentActivity", "collabAgentToolCall"].includes(incomingType)
            && isStaleAgentReplay(thread, scope, raw, live)) {
            return turn.items.get(id) ?? {id: "", raw: {}};
        }
        let result = turn.items.get(id);
        if (!result) {
            result = {id, raw: clone(raw)};
            turn.items.set(id, result);
            turn.itemOrder.push(id);
        } else {
            resetIncomingTextBounds(result, raw);
            mergePreservingCompleteness(result.raw, raw);
        }
        if ((lifecycle === "completed" || lifecycle === "started")
            && isEmptyStatus(statusFromValue(member(raw, "status"))))
            result.raw.status = lifecycle === "completed" ? "completed" : "running";
        boundRetainedItemText(result);
        const type = stringMember(result.raw, "type");
        if (["subAgentActivity", "collabAgentToolCall"].includes(type))
            this.upsertAgentActivity(thread, scope, result.raw, live);
        if (type === "agentMessage") this.updateOwningAgentResult(thread.id, stringMember(result.raw, "text"));
        return result;
    }

    private upsertAgentActivity(owner: ThreadPresentation, scope: JsonObject, activity: JsonObject, live = true): void {
        const type = stringMember(activity, "type");
        if (type === "subAgentActivity" && !isSpawnActivity(activity)) {
            const child = childThreadIdentity(activity);
            const existing = this.owningAgent(child);
            if (!existing) return;
            const agentPath = stringMember(activity, "agentPath");
            if (agentPath !== "") existing.agent.raw.agentPath = agentPath;
            if (stringMember(activity, "kind") === "interrupted")
                this.updateOwningAgentStatus(child, statusFromValue("interrupted"));
            return;
        }
        if (type === "collabAgentToolCall" && !isSpawnActivity(activity)) {
            const states = member(activity, "agentsStates", {});
            if (!isObject(states)) return;
            for (const [child, state] of Object.entries(states)) {
                const existing = this.owningAgent(child);
                if (!existing || !isObject(state)) continue;
                const status = statusFromValue(member(state, "status"));
                const message = stringMember(state, "message");
                if (!isEmptyStatus(status)) this.updateOwningAgentStatus(child, status);
                if (message !== "") this.updateOwningAgentResult(child, message);
                existing.agent.raw.agentState = clone(state);
            }
            return;
        }
        const childThreadId = childThreadIdentity(activity);
        if (type === "collabAgentToolCall" && childThreadId === "") return;
        const id = agentIdentity(activity, scope);
        if (id === "" || isStaleAgentReplay(owner, scope, activity, live)) return;
        let agent = owner.agents.get(id);
        if (!agent) {
            agent = {id, itemId: "", ownerTurnId: "", childThreadId: "", status: UnknownStatus, raw: {}};
            owner.agents.set(id, agent);
            owner.agentOrder.push(id);
        }
        const activityKind = stringMember(activity, "kind");
        let candidate = statusFromValue(member(activity, "status"));
        if (isEmptyStatus(candidate)) {
            if (["completed", "interrupted", "failed"].includes(activityKind)) candidate = statusFromValue(activityKind);
            else if (activityKind === "interacted") candidate = UnknownStatus;
            else if (["started", "progress"].includes(activityKind))
                candidate = statusFromValue("running");
        }
        const changesChild = childThreadId !== "" && agent.childThreadId !== "" && agent.childThreadId !== childThreadId;
        agent.itemId = stringMember(scope, "itemId");
        agent.ownerTurnId = stringMember(scope, "turnId");
        mergePreservingCompleteness(agent.raw, activity);
        if (changesChild) {
            agent.status = UnknownStatus;
            delete agent.raw.status;
            const item = this.agentSourceItem(owner, agent);
            if (item) delete item.raw.status;
            this.clearAgentResult(owner, agent);
            delete agent.raw.agentState;
        }
        if (!isEmptyStatus(candidate)) this.setAgentStatus(owner, agent,
            isTerminalTurnStatus(agent.status) && isActiveStatus(candidate) ? agent.status : candidate);
        if (childThreadId !== "") this.assignChildOwnership(owner, agent, childThreadId, live);
    }

    private assignChildOwnership(parent: ThreadPresentation, agent: AgentPresentation, child: string, live: boolean): void {
        if (child === parent.id) return;
        let ancestorId = parent.id;
        const visited = new Set<string>();
        while (!visited.has(ancestorId)) {
            visited.add(ancestorId);
            const ancestor = this.childOwnerships.get(ancestorId);
            if (!ancestor) break;
            ancestorId = ancestor.parentThreadId;
            if (ancestorId === child) return;
        }
        if (agent.childThreadId !== "" && agent.childThreadId !== child) {
            const previous = this.childOwnerships.get(agent.childThreadId);
            if (previous?.parentThreadId === parent.id && previous.agentId === agent.id)
                this.releaseChildOwnership(agent.childThreadId, true);
        }
        const previous = this.childOwnerships.get(child);
        if (previous && (previous.parentThreadId !== parent.id || previous.agentId !== agent.id)) {
            const previousAgent = this.threads.get(previous.parentThreadId)?.agents.get(previous.agentId);
            if (!live && previousAgent?.childThreadId === child) return;
            this.releaseChildOwnership(child, false);
        }
        agent.childThreadId = child;
        agent.raw.childThreadId = child;
        this.childOwnerships.set(child, {parentThreadId: parent.id, agentId: agent.id});
        if (!parent.childThreadOrder.includes(child)) parent.childThreadOrder.push(child);
        if (!this.threads.has(child)) this.threads.set(child, newThread(child));
        this.orderedThreads = this.orderedThreads.filter(id => id !== child);
        this.synchronizeOwningAgent(child);
    }

    private retainStructuralOwnership(child: string, parentId: string): void {
        if (child === "" || parentId === "" || child === parentId) return;
        const existing = this.childOwnerships.get(child);
        if (existing?.parentThreadId === parentId) return;
        if (existing) this.releaseChildOwnership(child, false);
        let parent = this.threads.get(parentId);
        if (!parent) {
            parent = newThread(parentId);
            this.threads.set(parentId, parent);
        }
        if (!this.threads.has(child)) this.threads.set(child, newThread(child));
        this.childOwnerships.set(child, {parentThreadId: parentId, agentId: ""});
        if (!parent.childThreadOrder.includes(child)) parent.childThreadOrder.push(child);
        this.orderedThreads = this.orderedThreads.filter(id => id !== child);
    }

    private releaseChildOwnership(child: string, promoteToRoot: boolean): void {
        const previous = this.childOwnerships.get(child);
        if (!previous) return;
        const parent = this.threads.get(previous.parentThreadId);
        if (parent) {
            parent.childThreadOrder = parent.childThreadOrder.filter(id => id !== child);
            const agent = parent.agents.get(previous.agentId);
            if (agent?.childThreadId === child) {
                agent.childThreadId = "";
                delete agent.raw.childThreadId;
            }
        }
        this.childOwnerships.delete(child);
        if (promoteToRoot && this.threads.has(child) && !this.orderedThreads.includes(child))
            this.orderedThreads = [...this.orderedThreads, child];
    }

    private owningAgent(child: string): {parent: ThreadPresentation; agent: AgentPresentation} | undefined {
        const ownership = this.childOwnerships.get(child);
        if (!ownership) return undefined;
        const parent = this.threads.get(ownership.parentThreadId);
        const agent = parent?.agents.get(ownership.agentId);
        return parent && agent ? {parent, agent} : undefined;
    }

    private agentSourceItem(parent: ThreadPresentation, agent: AgentPresentation): ItemPresentation | undefined {
        return parent.turns.get(agent.ownerTurnId)?.items.get(agent.itemId);
    }

    private setAgentStatus(parent: ThreadPresentation, agent: AgentPresentation, status: PresentationStatus): void {
        const token = statusToken(status);
        const item = this.agentSourceItem(parent, agent);
        if (statusToken(agent.status) === token && agent.raw.status === token
            && (!item || item.raw.status === token)) return;
        agent.status = status;
        agent.raw.status = token;
        if (item) item.raw.status = token;
    }

    private setAgentResult(parent: ThreadPresentation, agent: AgentPresentation, resultText: string): void {
        const item = this.agentSourceItem(parent, agent);
        if (agent.raw.resultText === resultText && (!item || item.raw.resultText === resultText)) return;
        agent.raw.resultText = resultText;
        if (item) item.raw.resultText = resultText;
    }

    private clearAgentResult(parent: ThreadPresentation, agent: AgentPresentation): void {
        const item = this.agentSourceItem(parent, agent);
        if (!Object.hasOwn(agent.raw, "resultText") && (!item || !Object.hasOwn(item.raw, "resultText"))) return;
        delete agent.raw.resultText;
        if (item) delete item.raw.resultText;
    }

    private updateOwningAgentStatus(child: string, status: PresentationStatus): void {
        if (isEmptyStatus(status)) return;
        const owner = this.owningAgent(child);
        if (!owner) return;
        this.setAgentStatus(owner.parent, owner.agent,
            isTerminalTurnStatus(owner.agent.status) && isActiveStatus(status) ? owner.agent.status : status);
    }

    private updateOwningAgentResult(child: string, resultText: string): void {
        if (resultText === "") return;
        const owner = this.owningAgent(child);
        if (owner) this.setAgentResult(owner.parent, owner.agent, resultText);
    }

    private synchronizeOwningAgent(childId: string, clearMissingResult = false): void {
        const owner = this.owningAgent(childId);
        const child = this.threads.get(childId);
        if (!owner || !child) return;
        let childStatus = child.status.semantic === "notLoaded" ? UnknownStatus : child.status;
        let resultText = "";
        for (let turnIndex = child.turnOrder.length - 1; turnIndex >= 0; --turnIndex) {
            const turn = child.turns.get(child.turnOrder[turnIndex]!);
            if (!turn) continue;
            if (isEmptyStatus(childStatus) && !isEmptyStatus(turn.status)) childStatus = turn.status;
            for (let itemIndex = turn.itemOrder.length - 1; itemIndex >= 0; --itemIndex) {
                const item = turn.items.get(turn.itemOrder[itemIndex]!);
                if (!item || stringMember(item.raw, "type") !== "agentMessage") continue;
                resultText = stringMember(item.raw, "text");
                if (resultText !== "") break;
            }
            if (resultText !== "" && !isEmptyStatus(childStatus)) break;
        }
        this.updateOwningAgentStatus(childId, childStatus);
        if (resultText === "" && clearMissingResult) this.clearAgentResult(owner.parent, owner.agent);
        else if (resultText !== "") this.setAgentResult(owner.parent, owner.agent, resultText);
    }

    private removeThread(threadId: string): void {
        const thread = this.threads.get(threadId);
        if (!thread) return;
        const children = [...thread.childThreadOrder];
        const foundIndex = this.orderedThreads.indexOf(threadId);
        const rootIndex = foundIndex < 0 ? this.orderedThreads.length : foundIndex;
        for (const child of children) this.releaseChildOwnership(child, false);
        this.releaseChildOwnership(threadId, false);
        this.threads.delete(threadId);
        this.orderedThreads = this.orderedThreads.filter(id => id !== threadId);
        let insertion = Math.min(rootIndex, this.orderedThreads.length);
        for (const child of children) {
            if (!this.threads.has(child) || this.childOwnerships.has(child)) continue;
            this.orderedThreads.splice(insertion++, 0, child);
        }
    }

    private clearProviderState(): void {
        this.orderedThreads = [];
        this.threads.clear();
        this.childOwnerships.clear();
        this.pendingRequests.clear();
        this.models = [];
        this.permissionProfiles = undefined;
    }

    private findTurn(threadId: string, turnId: string): TurnPresentation | undefined {
        return this.threads.get(threadId)?.turns.get(turnId);
    }

    private findItem(params: unknown): ItemPresentation | undefined {
        return this.findTurn(stringMember(params, "threadId"), stringMember(params, "turnId"))
            ?.items.get(stringMember(params, "itemId"));
    }
}
