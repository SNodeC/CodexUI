import {
    isObject,
    isPresentationFrame,
    member,
    stringMember,
} from "./PresentationProtocol.js";
import type {JsonObject, PresentationFrame} from "./PresentationProtocol.js";
import {classifyStatus, isActiveStatus, isTerminalTurnStatus} from "./PresentationStatus.js";

const MaximumRetainedTelemetry = 256;
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
    domains: Map<string, unknown>;
    textRetention?: Map<string, {retainedBytes: number; discardedBytes: number}>;
}

export interface TurnPresentation {
    id: string;
    status: string;
    itemOrder: string[];
    items: Map<string, ItemPresentation>;
    plan: JsonObject;
    raw: JsonObject;
    domains: Map<string, unknown>;
}

export interface AgentPresentation {
    id: string;
    itemId: string;
    ownerTurnId: string;
    childThreadId: string;
    status: string;
    raw: JsonObject;
}

export interface ChildThreadOwnership {
    parentThreadId: string;
    agentId: string;
}

export interface ThreadPresentation {
    id: string;
    title: string;
    preview: string;
    cwd: string;
    status: string;
    createdAt?: number;
    updatedAt?: number;
    recencyAt?: number;
    commandCwds: string[];
    changedPaths: string[];
    turnOrder: string[];
    turns: Map<string, TurnPresentation>;
    raw: JsonObject;
    domains: Map<string, unknown>;
    latestSettingsUpdate: unknown;
    settingsRevision: number;
    agentOrder: string[];
    agents: Map<string, AgentPresentation>;
    childThreadOrder: string[];
    archived: boolean;
}

export interface PendingRequestPresentation {
    id: string;
    kind: string;
    threadId: string;
    generation: number;
    raw: unknown;
}

export interface ConnectionPresentation {
    connected: boolean;
    retrying: boolean;
    generation: number;
    connectionId: string;
    role: string;
    controllerConnectionId: string;
    detail: string;
    providerGeneration: number;
    providerState: string;
    providerDetail: string;
    settings: unknown;
}

export interface TelemetryPresentation {
    sequence: number;
    generation: number;
    type: string;
    data: unknown;
    scope: JsonObject;
}

function clone<T>(value: T): T {
    return structuredClone(value);
}

function objectMember(value: unknown, name: string): JsonObject {
    const result = member(value, name, {});
    return isObject(result) ? result : {};
}

function boolValue(value: unknown, name: string, fallback = false): boolean {
    return isObject(value) && typeof value[name] === "boolean" ? value[name] : fallback;
}

function unsignedValue(value: unknown): number {
    return typeof value === "number" && Number.isSafeInteger(value) && value >= 0 ? value : 0;
}

function statusValue(value: unknown): string {
    return typeof value === "string" ? value : stringMember(value, "type");
}

function requestKey(value: unknown): string {
    return value === null ? "" : JSON.stringify(value);
}

function appendUnique(values: string[], value: string, maximum: number): void {
    if (value === "" || values.includes(value)) return;
    if (values.length === maximum) values.shift();
    values.push(value);
}

function retainRepositoryHints(thread: ThreadPresentation, item: JsonObject): void {
    const type = stringMember(item, "type");
    if (type === "commandExecution") appendUnique(thread.commandCwds, stringMember(item, "cwd"), 64);
    if (type !== "fileChange") return;
    const changes = item.changes;
    if (!Array.isArray(changes)) return;
    for (const change of changes) appendUnique(thread.changedPaths, stringMember(change, "path"), 512);
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

function mergeExplicitMembers(target: unknown, update: unknown): unknown {
    if (!isObject(target) || !isObject(update)) return clone(update);
    for (const [key, value] of Object.entries(update)) {
        if (isObject(target[key]) && isObject(value)) mergeExplicitMembers(target[key], value);
        else target[key] = clone(value);
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

function appendText(item: ItemPresentation, field: string, params: unknown): void {
    const delta = stringMember(params, "delta");
    if (delta === "") return;
    const existing = typeof item.raw[field] === "string" ? item.raw[field] : "";
    const existingBytes = item.textRetention?.get(field)?.retainedBytes ?? textEncoder.encode(existing).length;
    const deltaBytes = utf8ByteLength(delta);
    if (deltaBytes > MaximumRetainedStreamBytes) {
        const tail = utf8Tail(delta, RetainedStreamTailBytes);
        item.raw[field] = tail.text;
        setRetainedTextBytes(item, field, tail.retained);
        recordDiscardedText(item, field, existingBytes + tail.discarded);
        return;
    }
    const combined = existing + delta;
    const combinedBytes = existingBytes + deltaBytes;
    if (combinedBytes > MaximumRetainedStreamBytes) item.raw[field] = boundScalarText(item, field, combined);
    else { item.raw[field] = combined; setRetainedTextBytes(item, field, combinedBytes); }
}

function appendIndexedText(item: ItemPresentation, field: string, params: unknown, indexField: string): void {
    const rawIndex = isObject(params) ? params[indexField] : undefined;
    const position = typeof rawIndex === "number" && Number.isInteger(rawIndex) && rawIndex >= 0 ? rawIndex : 0;
    if (position >= MaximumIndexedTextParts) return;
    const parts: unknown[] = Array.isArray(item.raw[field]) ? item.raw[field] : [];
    item.raw[field] = parts;
    while (parts.length <= position) parts.push("");
    const retainedBefore = item.textRetention?.get(field)?.retainedBytes
        ?? parts.reduce<number>((sum, part) => sum + (typeof part === "string" ? textEncoder.encode(part).length : 0), 0);
    const delta = stringMember(params, "delta") || stringMember(params, "text");
    parts[position] = (typeof parts[position] === "string" ? parts[position] : "") + delta;
    const retained = retainedBefore + utf8ByteLength(delta);
    setRetainedTextBytes(item, field, retained);
    if (retained > MaximumRetainedStreamBytes) boundIndexedText(item, field, parts);
}

function applyDomainAuthority(
    domains: Map<string, unknown>,
    type: string,
    data: unknown,
    authority: string,
): void {
    if (authority === "none") return;
    if (authority === "remove") {
        domains.delete(type);
        return;
    }
    if (authority === "replace" || !domains.has(type)) {
        domains.set(type, clone(data));
        return;
    }
    const current = domains.get(type);
    domains.set(type, type === "thread.settings.changed"
        ? mergeExplicitMembers(current, data)
        : mergePreservingCompleteness(current, data));
}

function newThread(id: string): ThreadPresentation {
    return {
        id, title: "", preview: "", cwd: "", status: "",
        commandCwds: [], changedPaths: [], turnOrder: [], turns: new Map(), raw: {}, domains: new Map(),
        latestSettingsUpdate: {}, settingsRevision: 0, agentOrder: [], agents: new Map(),
        childThreadOrder: [], archived: false,
    };
}

function newTurn(id: string): TurnPresentation {
    return {id, status: "", itemOrder: [], items: new Map(), plan: {}, raw: {}, domains: new Map()};
}

export class PresentationModel {
    private readonly threads = new Map<string, ThreadPresentation>();
    private readonly childOwnerships = new Map<string, ChildThreadOwnership>();
    private readonly pendingRequests = new Map<string, PendingRequestPresentation>();
    private readonly connectionState: ConnectionPresentation = {
        connected: false, retrying: false, generation: 0, connectionId: "", role: "",
        controllerConnectionId: "", detail: "", providerGeneration: 0,
        providerState: "", providerDetail: "", settings: {},
    };
    private orderedThreads: string[] = [];
    private models: unknown = [];
    private readonly retainedGlobalDomains = new Map<string, unknown>();
    private readonly retainedTelemetry: TelemetryPresentation[] = [];
    private lastSequence = 0;

    applyEvent(event: unknown): void {
        try {
            this.applyValidatedEvent(event);
        } catch {
            // Presentation mutation is an untrusted-data boundary.
        }
    }

    threadOrder(): readonly string[] { return this.orderedThreads; }
    thread(threadId: string): ThreadPresentation | undefined { return this.threads.get(threadId); }
    childOwnership(childThreadId: string): ChildThreadOwnership | undefined {
        return this.childOwnerships.get(childThreadId);
    }
    activeTurnId(threadId: string): string | undefined {
        const thread = this.thread(threadId);
        if (!thread || (thread.status !== "" && !isActiveStatus(thread.status))) return undefined;
        for (let index = thread.turnOrder.length - 1; index >= 0; --index) {
            const id = thread.turnOrder[index]!;
            if (isActiveStatus(thread.turns.get(id)?.status ?? "")) return id;
        }
        return undefined;
    }
    pendingRequestCount(): number { return this.pendingRequests.size; }
    connection(): Readonly<ConnectionPresentation> { return this.connectionState; }
    modelCatalog(): unknown { return this.models; }
    globalDomains(): ReadonlyMap<string, unknown> { return this.retainedGlobalDomains; }
    telemetry(): readonly TelemetryPresentation[] { return this.retainedTelemetry; }
    pendingRequestPresentations(): ReadonlyMap<string, PendingRequestPresentation> {
        return this.pendingRequests;
    }

    private applyValidatedEvent(candidate: unknown): void {
        if (!isPresentationFrame(candidate)) return;
        const event = candidate as PresentationFrame;
        const generation = unsignedValue(event.generation);
        if (this.connectionState.generation !== 0 && generation !== 0
            && generation < this.connectionState.generation) return;
        if (generation > this.connectionState.generation) {
            const replacesConnection = this.connectionState.generation !== 0;
            this.connectionState.generation = generation;
            this.lastSequence = 0;
            this.pendingRequests.clear();
            if (replacesConnection) {
                this.clearConnectionIdentity();
                this.connectionState.providerGeneration = 0;
                this.connectionState.providerState = "";
                this.connectionState.providerDetail = "";
            }
        }
        const sequence = unsignedValue(event.sequence);
        if (sequence !== 0) {
            if (sequence <= this.lastSequence) return;
            this.lastSequence = sequence;
        }
        const data = member(event, "data", {});
        const scope = objectMember(event, "scope");
        if (event.kind === "result") {
            if (event.ok !== true) return;
            const action = stringMember(event, "action");
            if (action === "threads.list") this.mergeThreadList(member(data, "threads", []));
            else if (action === "thread.read") {
                this.upsertThread(objectMember(data, "thread"), stringMember(event, "authority") === "replace");
            } else if (["thread.create", "thread.resume", "thread.fork"].includes(action)) {
                this.upsertThread(objectMember(data, "thread"), false);
            } else if (action === "turn.start") {
                const thread = this.threads.get(stringMember(scope, "threadId"));
                if (thread) {
                    const turn = this.upsertTurn(thread, objectMember(data, "turn"), false);
                    if (isActiveStatus(turn.status)) thread.status = "active";
                }
            } else if (action === "models.list") {
                const listed = member(data, "models", []);
                if (Array.isArray(listed)) this.models = clone(listed);
            } else this.retainDomainEvent(`operation.${action}`, data, scope, stringMember(event, "authority"));
            return;
        }
        if (event.kind !== "event") return;
        const type = stringMember(event, "type");
        const authority = stringMember(event, "authority");
        if (authority === "none") {
            if (this.retainedTelemetry.length === MaximumRetainedTelemetry) this.retainedTelemetry.shift();
            this.retainedTelemetry.push({sequence, generation, type, data: clone(data), scope: clone(scope)});
        }
        if (type === "connection.lifecycle") {
            this.connectionState.generation = generation;
            const lifecycle = stringMember(data, "state");
            if (lifecycle === "connected") {
                this.connectionState.connected = true;
                this.connectionState.retrying = false;
                this.connectionState.detail = "";
            } else if (lifecycle === "connecting" || lifecycle === "retrying") {
                this.connectionState.connected = false;
                this.connectionState.retrying = true;
                this.clearConnectionIdentity();
                this.connectionState.detail = stringMember(data, "detail");
                this.pendingRequests.clear();
            } else if (lifecycle === "disconnected" || lifecycle === "failure") {
                this.connectionState.connected = false;
                this.connectionState.retrying = false;
                this.clearConnectionIdentity();
                this.connectionState.detail = stringMember(data, "detail");
                this.pendingRequests.clear();
            }
            return;
        }
        if (type === "connection.bridge") {
            this.connectionState.connectionId = stringMember(data, "connectionId");
            this.connectionState.role = stringMember(data, "role");
            return;
        }
        if (type === "connection.controller") {
            this.connectionState.controllerConnectionId = stringMember(data, "controllerConnectionId");
            if (this.connectionState.connectionId !== "") {
                this.connectionState.role = this.connectionState.controllerConnectionId === this.connectionState.connectionId
                    ? "controller" : "observer";
            }
            return;
        }
        if (type === "connection.provider") {
            const incoming = isObject(data) ? data.generation : undefined;
            if (typeof incoming !== "number" || !Number.isSafeInteger(incoming) || incoming < 0
                || incoming < this.connectionState.providerGeneration) return;
            const state = stringMember(data, "state");
            if ((this.connectionState.providerGeneration !== 0 && incoming > this.connectionState.providerGeneration)
                || state === "disconnected") this.clearProviderState();
            this.connectionState.providerGeneration = incoming;
            this.connectionState.providerState = state;
            this.connectionState.providerDetail = stringMember(data, "reason");
            return;
        }
        if (type === "connection.settings.changed") {
            this.connectionState.settings = clone(data);
            return;
        }
        if (type === "thread.upsert") {
            this.upsertThread(objectMember(data, "thread"), false);
            return;
        }
        if (type === "thread.name.changed") {
            const thread = this.threads.get(stringMember(scope, "threadId"));
            if (thread && isObject(data) && typeof data.name === "string") {
                thread.title = data.name;
                thread.raw.name = data.name;
            }
            return;
        }
        if (type === "thread.status.changed") {
            const thread = this.threads.get(stringMember(scope, "threadId"));
            if (thread) {
                thread.status = statusValue(member(data, "status"));
                thread.raw.status = clone(member(data, "status"));
                this.updateOwningAgentStatus(thread.id, thread.status);
            }
            return;
        }
        if (type === "thread.lifecycle") {
            const thread = this.threads.get(stringMember(scope, "threadId"));
            if (thread) {
                const lifecycle = stringMember(data, "state");
                thread.status = lifecycle;
                if (lifecycle === "archived") thread.archived = true;
                else if (lifecycle === "unarchived") thread.archived = false;
                thread.raw.presentationLifecycle = lifecycle;
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
            this.pendingRequests.set(key, {
                id: key, kind: stringMember(data, "category"), threadId: stringMember(scope, "threadId"),
                generation, raw: clone(member(data, "request")),
            });
            return;
        }
        if (type === "pending-request.removed") {
            if (Object.hasOwn(scope, "requestId")) this.pendingRequests.delete(requestKey(scope.requestId));
            return;
        }

        const threadId = stringMember(scope, "threadId");
        if (threadId === "") {
            this.retainDomainEvent(type, data, scope, authority);
            return;
        }
        let thread = this.threads.get(threadId);
        if (!thread) {
            if (authority === "none" || authority === "remove") return;
            this.upsertThread({id: threadId}, false);
            thread = this.threads.get(threadId);
            if (!thread) return;
        }
        this.retainDomainEvent(type, data, scope, authority);
        if (authority === "none" || authority === "remove") return;
        if (type === "thread.settings.changed" && isObject(data)) {
            thread.latestSettingsUpdate = clone(Object.hasOwn(data, "threadSettings") ? data.threadSettings : data);
            ++thread.settingsRevision;
        }
        if (type === "turn.upsert") {
            const rawTurn = objectMember(data, "turn");
            const turn = clone(rawTurn);
            const lifecycle = stringMember(data, "lifecycle");
            const embeddedStatus = statusValue(member(turn, "status"));
            if (lifecycle === "completed" && !isTerminalTurnStatus(embeddedStatus)) turn.status = "completed";
            else if (lifecycle === "started" && embeddedStatus === "") turn.status = "inProgress";
            const updated = this.upsertTurn(thread, turn, false);
            if (lifecycle === "started" && isActiveStatus(updated.status)) thread.status = "active";
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
                this.upsertItem(thread, turn, data.item, true);
            }
            return;
        }
        if (type === "agents.activity.upsert") {
            this.upsertAgentActivity(thread, scope, objectMember(data, "activity"));
            return;
        }
        if (type === "conversation.reasoning.part-added") {
            const item = this.findItem(scope);
            const index = isObject(data) ? data.summaryIndex : undefined;
            if (item && typeof index === "number" && Number.isInteger(index) && index >= 0
                && index < MaximumIndexedTextParts) {
                const parts: unknown[] = Array.isArray(item.raw.summary) ? item.raw.summary : [];
                item.raw.summary = parts;
                while (parts.length <= index) parts.push("");
            }
            return;
        }
        if (type === "conversation.file-change.output-appended") {
            const item = this.findItem(scope);
            if (item) appendText(item, "output", {delta: stringMember(data, "delta")});
            return;
        }
        if (type === "conversation.file-change.patch-replaced") {
            const item = this.findItem(scope);
            if (item) {
                item.raw.changes = clone(member(data, "changes", []));
                retainRepositoryHints(thread, item.raw);
            }
            return;
        }
        if (type === "conversation.mcp.progress") {
            const item = this.findItem(scope);
            if (item) {
                const progress: unknown[] = Array.isArray(item.raw.progress) ? item.raw.progress : [];
                item.raw.progress = progress;
                if (progress.length < MaximumIndexedTextParts) progress.push(stringMember(data, "message"));
            }
            return;
        }
        if (type !== "conversation.item.append") return;
        const identity = {...scope, delta: stringMember(data, "text")};
        const item = this.findItem(identity);
        if (!item) return;
        const field = stringMember(data, "field");
        if (field === "summary") appendIndexedText(item, "summary", data, "summaryIndex");
        else if (field === "content") appendIndexedText(item, "content", data, "contentIndex");
        else if (field !== "") appendText(item, field, identity);
        if (stringMember(item.raw, "type") === "agentMessage")
            this.updateOwningAgentResult(threadId, stringMember(item.raw, "text"));
    }

    private clearConnectionIdentity(): void {
        this.connectionState.connectionId = "";
        this.connectionState.role = "";
        this.connectionState.controllerConnectionId = "";
    }

    private mergeThreadList(listedThreads: unknown): void {
        if (!Array.isArray(listedThreads)) return;
        const listedIds = new Set<string>();
        const nextOrder: string[] = [];
        for (const raw of listedThreads) {
            const id = stringMember(raw, "id");
            if (id === "") continue;
            this.upsertThread(isObject(raw) ? raw : {}, false, false);
            if (!this.childOwnerships.has(id) && !listedIds.has(id)) {
                listedIds.add(id);
                nextOrder.push(id);
            }
        }
        for (const id of this.orderedThreads) if (!listedIds.has(id)) nextOrder.push(id);
        this.orderedThreads = nextOrder;
    }

    private upsertThread(raw: JsonObject, replaceTurns: boolean, prependNewThread = true): ThreadPresentation {
        const id = stringMember(raw, "id");
        if (id === "") return newThread("");
        let result = this.threads.get(id);
        if (!result) {
            result = newThread(id);
            this.threads.set(id, result);
            if (prependNewThread) this.orderedThreads.unshift(id);
        }
        const previousThreadStatus = result.status;
        const terminalTurnStatuses = new Map<string, string>();
        if (replaceTurns) {
            for (const [turnId, turn] of result.turns)
                if (isTerminalTurnStatus(turn.status)) terminalTurnStatuses.set(turnId, turn.status);
        }
        const threadFields = clone(raw);
        delete threadFields.turns;
        result.raw = replaceTurns ? threadFields : mergePreservingCompleteness(result.raw, threadFields) as JsonObject;
        const name = stringMember(raw, "name");
        const preview = stringMember(raw, "preview");
        if (name !== "") result.title = name;
        else if (preview !== "") result.title = preview.slice(0, 80);
        else if (result.title === "") result.title = id.slice(0, 12);
        if (preview !== "") result.preview = preview;
        const cwd = stringMember(raw, "cwd");
        if (cwd !== "") result.cwd = cwd;
        if (Object.hasOwn(raw, "status")) result.status = statusValue(raw.status);
        for (const key of ["createdAt", "updatedAt", "recencyAt"] as const) {
            if (typeof raw[key] === "number" && Number.isInteger(raw[key])) result[key] = raw[key];
        }
        result.archived = boolValue(raw, "archived", result.archived);
        if (Array.isArray(raw.turns)) {
            let previouslyOwnedChildren: string[] = [];
            if (replaceTurns) {
                previouslyOwnedChildren = [...result.childThreadOrder];
                for (const child of previouslyOwnedChildren) this.releaseChildOwnership(child, false);
                result.turnOrder = [];
                result.turns.clear();
                result.agentOrder = [];
                result.agents.clear();
                result.commandCwds = [];
                result.changedPaths = [];
            }
            for (const turn of raw.turns) if (isObject(turn)) this.upsertTurn(result, turn, replaceTurns);
            if (replaceTurns) {
                for (const child of previouslyOwnedChildren) {
                    if (!this.childOwnerships.has(child) && this.threads.has(child) && !this.orderedThreads.includes(child))
                        this.orderedThreads.push(child);
                }
                for (const [turnId, terminalStatus] of terminalTurnStatuses) {
                    const turn = result.turns.get(turnId);
                    if (turn && isActiveStatus(turn.status)) {
                        turn.status = terminalStatus;
                        turn.raw.status = terminalStatus;
                    }
                }
            }
            const containsActiveTurn = [...result.turns.values()].some(turn => isActiveStatus(turn.status));
            if (!containsActiveTurn && isActiveStatus(result.status)
                && classifyStatus(previousThreadStatus).kind === "completed") {
                result.status = previousThreadStatus;
                result.raw.status = previousThreadStatus;
            }
            this.synchronizeOwningAgent(id, replaceTurns);
        } else if (Object.hasOwn(raw, "status") && result.status !== "notLoaded") {
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
        const turnFields = clone(raw);
        delete turnFields.items;
        result.raw = replaceItems ? turnFields : mergePreservingCompleteness(result.raw, turnFields) as JsonObject;
        const status = statusValue(member(raw, "status"));
        if (status !== "" && !(isTerminalTurnStatus(result.status) && isActiveStatus(status))) result.status = status;
        if (isTerminalTurnStatus(result.status) && isActiveStatus(status)) result.raw.status = result.status;
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

    private upsertItem(thread: ThreadPresentation, turn: TurnPresentation, rawValue: unknown, live = false): ItemPresentation {
        const raw = isObject(rawValue) ? rawValue : {};
        const id = stringMember(raw, "id");
        if (id === "") return {id: "", raw: {}, domains: new Map()};
        const scope = {threadId: thread.id, turnId: turn.id, itemId: id};
        const incomingType = stringMember(raw, "type");
        if (["subAgentActivity", "collabAgentToolCall"].includes(incomingType)
            && isStaleAgentReplay(thread, scope, raw, live)) {
            return turn.items.get(id) ?? {id: "", raw: {}, domains: new Map()};
        }
        let result = turn.items.get(id);
        if (!result) {
            result = {id, raw: clone(raw), domains: new Map()};
            turn.items.set(id, result);
            turn.itemOrder.push(id);
        } else {
            resetIncomingTextBounds(result, raw);
            mergePreservingCompleteness(result.raw, raw);
        }
        boundRetainedItemText(result);
        const type = stringMember(result.raw, "type");
        retainRepositoryHints(thread, result.raw);
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
            if (stringMember(activity, "kind") === "interrupted") this.updateOwningAgentStatus(child, "interrupted");
            return;
        }
        if (type === "collabAgentToolCall" && !isSpawnActivity(activity)) {
            const states = member(activity, "agentsStates", {});
            if (!isObject(states)) return;
            for (const [child, state] of Object.entries(states)) {
                const existing = this.owningAgent(child);
                if (!existing || !isObject(state)) continue;
                const status = stringMember(state, "status");
                const message = stringMember(state, "message");
                if (status !== "") this.updateOwningAgentStatus(child, status);
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
            agent = {id, itemId: "", ownerTurnId: "", childThreadId: "", status: "", raw: {}};
            owner.agents.set(id, agent);
            owner.agentOrder.push(id);
        }
        const changesChild = childThreadId !== "" && agent.childThreadId !== "" && agent.childThreadId !== childThreadId;
        agent.itemId = stringMember(scope, "itemId");
        agent.ownerTurnId = stringMember(scope, "turnId");
        mergePreservingCompleteness(agent.raw, activity);
        if (changesChild) {
            agent.status = "";
            delete agent.raw.status;
            const item = this.agentSourceItem(owner, agent);
            if (item) delete item.raw.status;
            this.clearAgentResult(owner, agent);
            delete agent.raw.agentState;
        }
        const activityStatus = stringMember(activity, "status");
        const activityKind = stringMember(activity, "kind");
        let candidate = activityStatus;
        if (candidate === "" && live && activityKind === "started") candidate = "inProgress";
        else if (candidate === "" && activityKind !== "") candidate = activityKind;
        if (candidate !== "") this.setAgentStatus(owner, agent,
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
        if (promoteToRoot && this.threads.has(child) && !this.orderedThreads.includes(child)) this.orderedThreads.push(child);
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

    private setAgentStatus(parent: ThreadPresentation, agent: AgentPresentation, status: string): void {
        agent.status = status;
        agent.raw.status = status;
        const item = this.agentSourceItem(parent, agent);
        if (item) item.raw.status = status;
    }

    private setAgentResult(parent: ThreadPresentation, agent: AgentPresentation, resultText: string): void {
        agent.raw.resultText = resultText;
        const item = this.agentSourceItem(parent, agent);
        if (item) item.raw.resultText = resultText;
    }

    private clearAgentResult(parent: ThreadPresentation, agent: AgentPresentation): void {
        delete agent.raw.resultText;
        const item = this.agentSourceItem(parent, agent);
        if (item) delete item.raw.resultText;
    }

    private updateOwningAgentStatus(child: string, status: string): void {
        if (status === "") return;
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
        let childStatus = child.status === "notLoaded" ? "" : child.status;
        let resultText = "";
        for (let turnIndex = child.turnOrder.length - 1; turnIndex >= 0; --turnIndex) {
            const turn = child.turns.get(child.turnOrder[turnIndex]!);
            if (!turn) continue;
            if (childStatus === "" && turn.status !== "") childStatus = turn.status;
            for (let itemIndex = turn.itemOrder.length - 1; itemIndex >= 0; --itemIndex) {
                const item = turn.items.get(turn.itemOrder[itemIndex]!);
                if (!item || stringMember(item.raw, "type") !== "agentMessage") continue;
                resultText = stringMember(item.raw, "text");
                if (resultText !== "") break;
            }
            if (resultText !== "" && childStatus !== "") break;
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
        this.retainedGlobalDomains.clear();
    }

    private retainDomainEvent(type: string, data: unknown, scope: JsonObject, authority: string): void {
        const threadId = stringMember(scope, "threadId");
        const turnId = stringMember(scope, "turnId");
        const itemId = stringMember(scope, "itemId");
        if (itemId !== "") {
            const item = this.findItem(scope);
            if (item) applyDomainAuthority(item.domains, type, data, authority);
        } else if (turnId !== "") {
            const turn = this.findTurn(threadId, turnId);
            if (turn) applyDomainAuthority(turn.domains, type, data, authority);
        } else if (threadId !== "") {
            const thread = this.threads.get(threadId);
            if (thread) applyDomainAuthority(thread.domains, type, data, authority);
        } else applyDomainAuthority(this.retainedGlobalDomains, type, data, authority);
    }

    private findTurn(threadId: string, turnId: string): TurnPresentation | undefined {
        return this.threads.get(threadId)?.turns.get(turnId);
    }

    private findItem(params: unknown): ItemPresentation | undefined {
        return this.findTurn(stringMember(params, "threadId"), stringMember(params, "turnId"))
            ?.items.get(stringMember(params, "itemId"));
    }
}
