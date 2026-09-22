import {
    ClientConnection,
    CodexBridgeClient,
    WebSocketTransport,
    serverNotificationOperations,
    serverRequestOperations,
} from "@snodec/codex-frontend";
import type {WebSocketFactory} from "@snodec/codex-frontend";
import type {JsonObject} from "../presentation/PresentationProtocol.js";
import {isObject, member, stringMember} from "../presentation/PresentationProtocol.js";
import {PresentationModel} from "../presentation/PresentationModel.js";
import type {PendingRequestPresentation} from "../presentation/PresentationModel.js";
import {ProtocolNormalizer} from "../presentation/ProtocolNormalizer.js";
import {PromptCoordinator, indexAuthoritativeItems, localCardVisible, promptWithFileLinks} from "../conversation/PromptCoordinator.js";
import type {AttachmentDraft, PromptDispatch} from "../conversation/PromptCoordinator.js";
import {DefaultAuthoritativeItemLimit, projectConversation} from "../conversation/ConversationProjection.js";
import {PendingAnimationDelayMilliseconds} from "../conversation/MiddleTypes.js";
import type {ConversationSnapshot} from "../conversation/MiddleTypes.js";
import {readBrowserStorage, writeBrowserStorage} from "./BrowserStorage.js";
import {ConversationViewportState} from "./ConversationViewportState.js";
import {pendingResponse} from "./PendingRequestResponses.js";
import type {PendingRequestSubmission} from "./PendingRequestResponses.js";
import type {SettingDraft} from "./TurnSettingsOptions.js";

export const DraftThreadId = "__codexui_new_thread__";
const MaximumProtocolFrames = 500;
const MaximumHistoricalHydrations = 8;
const MaximumItemHydrations = 8;

function isThreadHydrationAction(action: string): boolean {
    return action === "thread.read" || action === "thread.resume"
        || action === "thread.turns.list" || action === "thread.items.list";
}

function retainedProtocolFrame(frame: JsonObject): unknown {
    // The private normalizer emits fresh frames from independently parsed messages.
    // Only the model mutates protocol data, and it owns copies; history can retain the input.
    if (stringMember(frame, "type") !== "pending-request.upsert") return frame;
    const data = isObject(frame.data) ? frame.data : {};
    return {...frame, data: {
        requestId: member(data, "requestId"),
        category: stringMember(data, "category"),
        request: "[redacted; inspect the typed Requests view]",
    }};
}

const actionMethods = {
    "threads.list": "thread/list", "thread.read": "thread/read", "thread.create": "thread/start",
    "thread.resume": "thread/resume", "thread.fork": "thread/fork", "thread.rename": "thread/name/set",
    "thread.turns.list": "thread/turns/list", "thread.items.list": "thread/items/list",
    "thread.archive": "thread/archive", "thread.unarchive": "thread/unarchive", "thread.delete": "thread/delete",
    "models.list": "model/list", "permission-profiles.list": "permissionProfile/list",
    "turn.start": "turn/start", "turn.steer": "turn/steer", "turn.interrupt": "turn/interrupt",
} as const;
type Action = keyof typeof actionMethods;

export interface BrowserSessionSnapshot {
    readonly revision: number;
    readonly selectedThreadId: string;
    readonly selectedThreadLoading: boolean;
    readonly newThreadDraft?: NewThreadDraft;
    readonly protocolFrames: readonly unknown[];
    readonly notice: string;
    readonly bridgeUrl: string;
}
export interface NewThreadDraft {
    workspace: string;
    name: string;
    baseInstructions: string;
    developerInstructions: string;
    ephemeral: boolean;
}

export type ThreadSortCriterion = "alphanumeric" | "created" | "recent";

const threadTitleCollator = new Intl.Collator(undefined, {
    numeric: true, sensitivity: "base", ignorePunctuation: true,
});

function forkNameParts(title: string): {base: string; lineage: number[]} {
    const match = /^(.*) \(fork ([1-9]\d*(?:\.[1-9]\d*)*)\)$/u.exec(title);
    if (!match) return {base: title, lineage: []};
    const lineage = match[2]!.split(".").map(Number);
    if (lineage.some(component => !Number.isSafeInteger(component)))
        return {base: title, lineage: []};
    return {base: match[1]!, lineage};
}

export function suggestForkName(sourceTitle: string, existingThreadTitles: readonly string[]): string {
    const source = forkNameParts(sourceTitle);
    const base = source.base || "Thread";
    const directChildren = new Set<number>();
    for (const title of existingThreadTitles) {
        const candidate = forkNameParts(title);
        if (candidate.base !== base || candidate.lineage.length !== source.lineage.length + 1) continue;
        if (source.lineage.every((component, index) => candidate.lineage[index] === component))
            directChildren.add(candidate.lineage.at(-1)!);
    }
    let next = 1;
    while (directChildren.has(next)) ++next;
    return `${base} (fork ${[...source.lineage, next].join(".")})`;
}

type ThreadPhase = "cold" | "hydrating" | "ready" | "resuming" | "failed";
interface ThreadRuntimeState {
    phase: ThreadPhase;
    historyPageInFlight: boolean;
    readonly seenHistoryCursors: Set<string>;
}
interface ItemPage {threadId: string; turnId: string; cursor: string; runtime: ThreadRuntimeState}
interface OperationResponse {ok: boolean; data?: unknown; error?: unknown; stale?: boolean}

type RawRequest = (method: string, params: unknown, handler: (response: unknown) => void) => string;
type RegisterNotification = (method: string, handler: (notification: JsonObject) => void) => void;
type RegisterRequest = (method: string, handler: (request: JsonObject) => void) => void;

export class BrowserFrontendSession {
    readonly model = new PresentationModel();
    readonly prompts = new PromptCoordinator();
    readonly settingDrafts = new Map<string, SettingDraft>();
    readonly viewportState = new ConversationViewportState();
    private readonly sdk = new CodexBridgeClient();
    private readonly connection: ClientConnection;
    private readonly normalizer: ProtocolNormalizer;
    private readonly listeners = new Set<() => void>();
    private readonly protocolFrames: unknown[] = [];
    private readonly runtimeByThread = new Map<string, ThreadRuntimeState>();
    private readonly resolvingRequests = new Set<PendingRequestPresentation>();
    private readonly pendingUserOperations = new Map<string, object>();
    private readonly pendingAnimationTimers = new Map<number, ReturnType<typeof setTimeout>>();
    private readonly historicalHydrationQueue: string[] = [];
    private readonly historicalHydrations = new Set<string>();
    private readonly historicalHydrationVisited = new Set<string>();
    private readonly itemPageQueue: ItemPage[] = [];
    private pendingItemPages = 0;
    private transport: WebSocketTransport | undefined;
    private selectedThreadId = "";
    private newThreadDraft: NewThreadDraft | undefined;
    private composerGeneration = 0;
    private newThreadCreationInFlight = false;
    private notice = "";
    private revision = 0;
    private nextCorrelation = 1;
    private noticeTimer: ReturnType<typeof setTimeout> | undefined;
    private publishScheduled = false;
    private disposed = false;
    private transportFailed = false;
    private reconnectAfterDetach = false;
    private lifecycleEpoch = 0;
    private catalogHydrationKey = "";
    private threadListInFlightEpoch = -1;
    private threadListRepairPending = false;
    private threadListLoadMoreRequested = false;
    private threadListNextCursor = "";
    private readonly threadListSeenCursors = new Set<string>();
    private threadListCycle = 0;
    private threadListRepairTimer: ReturnType<typeof setTimeout> | undefined;
    private bridgeUrl: string;
    private readonly createWebSocket: WebSocketFactory | undefined;
    private snapshot: BrowserSessionSnapshot;

    constructor(bridgeUrl = BrowserFrontendSession.defaultBridgeUrl(), createWebSocket?: WebSocketFactory) {
        this.bridgeUrl = bridgeUrl;
        this.createWebSocket = createWebSocket;
        this.normalizer = new ProtocolNormalizer(frame => {
            const scope = isObject(frame.scope) ? frame.scope : {};
            const retiringRequest = stringMember(frame, "type") === "pending-request.removed"
                && Object.hasOwn(scope, "requestId")
                ? this.model.pendingRequestPresentations().get(JSON.stringify(scope.requestId)) : undefined;
            const application = this.model.applyEvent(frame);
            if (application === "rejected") return;
            const threadId = stringMember(scope, "threadId");
            const hydrationResult = stringMember(frame, "kind") === "result"
                && isThreadHydrationAction(stringMember(frame, "action"));
            const settingsNoOp = stringMember(frame, "type") === "thread.settings.changed"
                && application === "settings-unchanged";
            if (threadId !== "" && !hydrationResult && !settingsNoOp)
                this.model.noteThreadActivity(threadId, Math.floor(Date.now() / 1000));
            this.protocolFrames.push(retainedProtocolFrame(frame));
            if (this.protocolFrames.length > MaximumProtocolFrames) this.protocolFrames.shift();
            this.reconcilePromptsForFrame(frame);
            this.handlePresentationFrame(frame, application === "provider-reset",
                application === "requests-reset" || application === "provider-reset", retiringRequest);
            if (threadId !== "" && stringMember(frame, "type") === "conversation.item.upsert")
                this.queueActiveAgentChildren(threadId);
            this.schedulePublish();
        });
        this.connection = new ClientConnection(this.sdk, {
            onConnected: () => { this.transportFailed = false; this.normalizer.transportEvent("connected"); },
            onDetached: () => {
                const failed = this.transportFailed;
                this.transport = undefined;
                this.transportFailed = false;
                if (!failed) this.normalizer.transportEvent("disconnected");
                this.finishReconnectAfterDetach();
            },
            onFailure: reason => {
                this.transportFailed = true;
                this.normalizer.transportEvent("failure", reason);
            },
        });
        this.sdk.onRawJson((direction, message) => {
            if (direction === "from-app-server") this.normalizer.observeRawInbound(message);
        });
        this.sdk.onBridgeEvent(message => {
            this.normalizer.bridgeEvent(message);
        });
        const registerNotification = this.sdk.onServerNotification.bind(this.sdk) as RegisterNotification;
        for (const method of Object.keys(serverNotificationOperations)) registerNotification(method, notification => {
            const params = isObject(notification.params) ? notification.params : {};
            this.normalizer.serverNotification(method, params);
        });
        const registerRequest = this.sdk.onServerRequest.bind(this.sdk) as RegisterRequest;
        for (const method of Object.keys(serverRequestOperations)) registerRequest(method, request => {
            const params = isObject(request.params) ? request.params : {};
            this.normalizer.serverRequest(method, member(request, "id"), params);
        });
        this.snapshot = this.makeSnapshot();
    }

    static defaultBridgeUrl(): string {
        if (typeof window === "undefined") return "ws://127.0.0.1:8080/codex";
        const configured = readBrowserStorage("codexui.bridgeUrl");
        if (configured) return configured;
        const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
        return `${protocol}//${window.location.host || "127.0.0.1:8080"}/codex`;
    }

    subscribe = (listener: () => void): (() => void) => {
        this.listeners.add(listener);
        return () => this.listeners.delete(listener);
    };
    getSnapshot = (): BrowserSessionSnapshot => this.snapshot;

    connect(url = this.bridgeUrl): void {
        if (this.disposed) return;
        if (this.transport) {
            if (this.transportFailed) {
                this.reconnectAfterDetach = true;
                this.connection.disconnect("replace failed transport");
            }
            return;
        }
        this.bridgeUrl = url.trim();
        writeBrowserStorage("codexui.bridgeUrl", this.bridgeUrl);
        this.normalizer.transportEvent("connecting");
        try { this.transport = new WebSocketTransport(this.connection, this.bridgeUrl,
            this.createWebSocket ? {createWebSocket: this.createWebSocket} : {}); }
        catch (error) {
            this.transport = undefined;
            this.normalizer.transportEvent("failure", error instanceof Error ? error.message : "WebSocket setup failed");
        }
        this.schedulePublish();
    }
    disconnect(): void {
        this.reconnectAfterDetach = false;
        this.connection.disconnect("local-disconnect");
    }
    reconnect(): void {
        this.reconnectAfterDetach = true;
        this.connection.disconnect("local-reconnect");
        this.finishReconnectAfterDetach();
    }
    dispose(): void {
        this.disposed = true;
        this.connection.dispose(); this.transport = undefined;
        for (const timer of this.pendingAnimationTimers.values()) clearTimeout(timer);
        this.pendingAnimationTimers.clear();
        if (this.threadListRepairTimer) clearTimeout(this.threadListRepairTimer);
        this.threadListRepairTimer = undefined;
        if (this.noticeTimer) clearTimeout(this.noticeTimer);
        this.noticeTimer = undefined;
    }

    private finishReconnectAfterDetach(): void {
        if (!this.reconnectAfterDetach || this.connection.attached || this.disposed) return;
        this.reconnectAfterDetach = false;
        queueMicrotask(() => { if (!this.disposed) this.connect(); });
    }
    dismissNotice(): void {
        if (this.noticeTimer) clearTimeout(this.noticeTimer);
        this.noticeTimer = undefined; this.notice = ""; this.publish();
    }
    claimController(): boolean { return this.sdk.claimController(); }
    releaseController(): boolean { return this.sdk.releaseController(); }
    role(): string { return this.sdk.role ?? ""; }
    canSubmit(presentationKey?: string): boolean {
        const connection = this.model.connection();
        return connection.connected && connection.providerState === "ready" && this.sdk.role === "controller"
            && (presentationKey === undefined
                || this.matchesThreadPresentation(this.selectedThreadId, presentationKey));
    }

    selectThread(threadId: string, presentationKey?: string): void {
        if (!this.matchesThreadPresentation(threadId, presentationKey)) return;
        if (threadId === DraftThreadId && this.newThreadDraft) {
            this.selectedThreadId = DraftThreadId; this.publish(); return;
        }
        if (this.prompts.submissions(DraftThreadId).length === 0) {
            if (this.newThreadDraft) {
                const retiredIdentity = this.viewportState.retire(DraftThreadId);
                if (retiredIdentity) this.settingDrafts.delete(retiredIdentity);
            } else this.viewportState.retire(DraftThreadId);
            this.newThreadDraft = undefined;
        }
        this.selectedThreadId = threadId;
        if (threadId !== "") this.ensureThreadHydrated(threadId);
        this.publish();
    }
    beginNewThread(draft: NewThreadDraft = {
        workspace: "", name: "", baseInstructions: "", developerInstructions: "", ephemeral: false,
    }): void {
        if (this.newThreadCreationInFlight) { this.setNotice("The current new thread is still being created.", false); return; }
        this.prompts.clearThread(DraftThreadId);
        if (this.newThreadDraft) {
            const retiredIdentity = this.viewportState.retire(DraftThreadId);
            if (retiredIdentity) this.settingDrafts.delete(retiredIdentity);
        } else this.viewportState.retire(DraftThreadId);
        this.newThreadDraft = {
            workspace: draft.workspace.trim(), name: draft.ephemeral ? "" : draft.name.trim(),
            baseInstructions: draft.baseInstructions.trim(), developerInstructions: draft.developerInstructions.trim(),
            ephemeral: draft.ephemeral,
        };
        this.selectedThreadId = DraftThreadId;
        ++this.composerGeneration;
        this.viewportState.bind(DraftThreadId, this.newThreadDraft);
        this.publish();
    }
    composerVisualKey(): number { return this.composerGeneration; }
    threadVisualKey(threadId: string): string {
        if (threadId === DraftThreadId) {
            if (this.newThreadDraft) return this.viewportState.bind(threadId, this.newThreadDraft);
            return this.viewportState.presentationKey(threadId) ?? threadId;
        }
        const thread = this.model.thread(threadId);
        return thread ? this.viewportState.bind(threadId, thread) : "";
    }
    threadPromptAnimating(threadId: string): boolean {
        const now = Date.now();
        return this.prompts.submissions(threadId).some(submission =>
            localCardVisible(submission) && submission.state !== "failed"
            && now - submission.admittedAtMilliseconds >= PendingAnimationDelayMilliseconds);
    }
    threadRecentAt(threadId: string): number | undefined {
        let latest: number | undefined;
        const visited = new Set<string>();
        const visit = (id: string) => {
            if (visited.has(id)) return;
            visited.add(id);
            const thread = this.model.thread(id);
            for (const timestamp of [thread?.recencyAt, thread?.localPromptActivityAt])
                if (timestamp !== undefined && (latest === undefined || timestamp > latest)) latest = timestamp;
            for (const submission of this.prompts.submissions(id))
                if (submission.startsTurn && (submission.state === "queued" || submission.state === "inFlight")
                    && submission.sortActivityAt !== undefined
                    && (latest === undefined || submission.sortActivityAt > latest)) latest = submission.sortActivityAt;
            for (const childId of thread?.childThreadOrder ?? []) visit(childId);
        };
        visit(threadId);
        return latest;
    }
    private nextPromptActivityAt(nowMilliseconds: number): number {
        let activityAt = Math.floor(nowMilliseconds / 1000);
        for (const id of this.model.threadIds()) {
            const thread = this.model.thread(id);
            for (const timestamp of [thread?.recencyAt, thread?.localPromptActivityAt])
                if (timestamp !== undefined && timestamp >= activityAt) activityAt = timestamp + 1;
            for (const submission of this.prompts.submissions(id))
                if ((submission.state === "queued" || submission.state === "inFlight")
                    && submission.sortActivityAt !== undefined && submission.sortActivityAt >= activityAt)
                    activityAt = submission.sortActivityAt + 1;
        }
        return activityAt;
    }
    threadOrder(criterion: ThreadSortCriterion = "recent"): readonly string[] {
        const order = this.model.threadOrder().filter(id => this.model.childOwnership(id) === undefined);
        const timestamp = (threadId: string) => {
            const thread = this.model.thread(threadId);
            if (criterion === "created") return thread?.createdAt;
            return this.threadRecentAt(threadId);
        };
        order.sort((leftId, rightId) => {
            const left = this.model.thread(leftId); const right = this.model.thread(rightId);
            if (!left || !right) return leftId.localeCompare(rightId);
            if (criterion === "alphanumeric") {
                const leftTitle = left.title.trim(); const rightTitle = right.title.trim();
                const leftNumeric = /^\p{Nd}/u.test(leftTitle); const rightNumeric = /^\p{Nd}/u.test(rightTitle);
                if (leftNumeric !== rightNumeric) return leftNumeric ? -1 : 1;
                const comparison = threadTitleCollator.compare(leftTitle, rightTitle);
                if (comparison !== 0) return comparison;
            } else {
                const leftTimestamp = timestamp(leftId); const rightTimestamp = timestamp(rightId);
                if (leftTimestamp !== rightTimestamp) {
                    if (leftTimestamp === undefined) return 1;
                    if (rightTimestamp === undefined) return -1;
                    return rightTimestamp - leftTimestamp;
                }
            }
            return leftId.localeCompare(rightId);
        });
        return order;
    }
    conversation(limit = DefaultAuthoritativeItemLimit): ConversationSnapshot {
        const projectionId = this.selectedThreadId;
        const thread = this.model.thread(projectionId);
        const index = indexAuthoritativeItems(projectionId, thread);
        if (thread) this.prompts.decorate(this.selectedThreadId, index);
        const conversation = projectConversation(index, this.prompts.submissions(projectionId), limit, Date.now(), thread);
        conversation.hasMore ||= thread?.historyHasMore === true;
        conversation.activeTurnId = this.model.activeTurnId(this.selectedThreadId);
        return conversation;
    }

    async submitPrompt(prompt: string, attachments: AttachmentDraft[] = [], turnOptions: JsonObject = {},
        threadOptions: JsonObject = {}, presentationKey?: string): Promise<boolean> {
        if (prompt.trim() === "") return false;
        const canonicalPrompt = promptWithFileLinks(prompt, attachments);
        if (!this.canSubmit(presentationKey)) {
            if (!this.canSubmit())
                this.setNotice("Codex is not ready for a controlled turn. Your message was not sent.");
            return false;
        }
        let destination = this.selectedThreadId;
        let thread = this.model.thread(destination);
        if (destination === "") { this.setNotice("Select a thread or choose New thread before sending."); return false; }
        const admittedAt = Date.now();
        const activeTurnId = destination === DraftThreadId ? undefined : this.model.activeTurnId(destination);
        const submissionId = this.prompts.admit(destination, canonicalPrompt, attachments, turnOptions, thread,
            activeTurnId, admittedAt, activeTurnId === undefined ? this.nextPromptActivityAt(admittedAt) : undefined);
        this.schedulePendingAnimation(submissionId);
        if (destination !== DraftThreadId) {
            this.threadRuntime(destination);
        }
        this.publish();
        if (destination === DraftThreadId) {
            if (this.newThreadCreationInFlight) return true;
            this.newThreadCreationInFlight = true;
            const threadDraft = this.newThreadDraft;
            const createOptions: JsonObject = {...threadOptions};
            if (threadDraft?.baseInstructions) createOptions.baseInstructions = threadDraft.baseInstructions;
            if (threadDraft?.developerInstructions) createOptions.developerInstructions = threadDraft.developerInstructions;
            if (threadDraft?.ephemeral) createOptions.ephemeral = true;
            if (threadDraft?.workspace && !Object.hasOwn(createOptions, "cwd")) createOptions.cwd = threadDraft.workspace;
            const created = await this.requestPromise("thread.create", createOptions);
            this.newThreadCreationInFlight = false;
            if (created.stale) {
                this.prompts.failQueued(DraftThreadId,
                    "Provider changed before thread creation was acknowledged");
                this.publish();
                return false;
            }
            const createdThread = isObject(created.data) ? member(created.data, "thread", {}) : {};
            const id = stringMember(createdThread, "id");
            if (!created.ok || id === "" || !this.prompts.reassignThread(DraftThreadId, id)) {
                this.prompts.failQueued(DraftThreadId, this.errorMessage(created)); this.setNotice(this.errorMessage(created)); return false;
            }
            const threadPresentation = this.model.thread(id);
            if (threadPresentation) {
                const displacedIdentity = this.viewportState.presentationKey(id);
                const promotedIdentity = this.viewportState.promote(DraftThreadId, id, threadPresentation);
                if (promotedIdentity && displacedIdentity && displacedIdentity !== promotedIdentity)
                    this.settingDrafts.delete(displacedIdentity);
            }
            if (this.selectedThreadId === DraftThreadId) this.selectedThreadId = id;
            destination = id;
            const runtime = this.threadRuntime(id);
            runtime.phase = "ready";
            const requestedName = threadDraft?.ephemeral ? "" : (threadDraft?.name ?? "");
            this.newThreadDraft = undefined;
            if (requestedName !== "") {
                this.model.setThreadTitleLocally(id, requestedName);
                this.renameThread(id, requestedName);
            }
            this.publish();
        }
        queueMicrotask(() => this.dispatchNextPrompt(destination));
        return true;
    }

    interrupt(presentationKey?: string): void {
        const threadId = this.selectedThreadId;
        const targetKey = presentationKey ?? this.threadVisualKey(threadId);
        if (targetKey === "" || !this.matchesThreadPresentation(threadId, targetKey)) return;
        const turnId = this.model.activeTurnId(threadId);
        if (turnId) this.request("turn.interrupt", {threadId, turnId}, undefined,
            () => this.matchesThreadPresentation(threadId, targetKey));
    }
    operationPending(action: string, threadId = "", presentationKey?: string): boolean {
        return this.pendingUserOperations.has(`${action}:${presentationKey ?? this.threadVisualKey(threadId)}`);
    }
    renameThread(threadId: string, name: string, presentationKey?: string): void {
        void this.performUserOperation("thread.rename", "thread.rename", {threadId, name}, "Rename thread", presentationKey);
    }
    reloadThread(threadId: string, presentationKey?: string): void {
        if (this.matchesThreadPresentation(threadId, presentationKey)) void this.hydrateThread(threadId, true, true);
    }
    historyPagePending(threadId: string, presentationKey?: string): boolean {
        return this.matchesThreadPresentation(threadId, presentationKey)
            && this.runtimeByThread.get(threadId)?.historyPageInFlight === true;
    }
    forkDraft(threadId: string): NewThreadDraft {
        const source = this.model.thread(threadId);
        return {
            workspace: source?.cwd ?? "",
            name: suggestForkName(source?.title || threadId, this.model.threadTitles()),
            baseInstructions: stringMember(source?.raw, "baseInstructions"),
            developerInstructions: stringMember(source?.raw, "developerInstructions"),
            ephemeral: source?.raw.ephemeral === true,
        };
    }
    forkThread(threadId: string, draft?: NewThreadDraft, presentationKey?: string): void {
        if (!this.matchesThreadPresentation(threadId, presentationKey)) return;
        const suggested = this.forkDraft(threadId);
        const requestedName = draft?.ephemeral ? "" : (draft?.name.trim() || suggested.name);
        const parameters: JsonObject = {threadId, excludeTurns: true};
        if (draft) {
            if (draft.workspace.trim() !== "") parameters.cwd = draft.workspace.trim();
            if (draft.baseInstructions.trim() !== "") parameters.baseInstructions = draft.baseInstructions.trim();
            if (draft.developerInstructions.trim() !== "")
                parameters.developerInstructions = draft.developerInstructions.trim();
            parameters.ephemeral = draft.ephemeral;
        }
        this.performUserOperation("thread.fork", "thread.fork", parameters, "Fork thread", presentationKey)?.then(response => {
            const thread = isObject(response.data) ? member(response.data, "thread", {}) : {};
            const id = stringMember(thread, "id");
            if (response.ok && id !== "") {
                if (requestedName !== "") this.model.setThreadTitleLocally(id, requestedName);
                const runtime = this.threadRuntime(id);
                runtime.phase = "ready";
                if (requestedName !== "") this.renameThread(id, requestedName);
                this.selectThread(id);
                this.loadForkHistory(id, runtime);
            }
            else if (response.ok) this.setNotice("Fork thread failed: no thread was returned.");
        });
    }
    archiveThread(threadId: string, archived: boolean, presentationKey?: string): void {
        void this.performUserOperation("thread.archive", archived ? "thread.unarchive" : "thread.archive",
            {threadId}, archived ? "Unarchive thread" : "Archive thread", presentationKey);
    }
    deleteThread(threadId: string, presentationKey?: string): void {
        void this.performUserOperation("thread.delete", "thread.delete", {threadId}, "Delete thread", presentationKey);
    }
    isPendingResolving(request: PendingRequestPresentation): boolean {
        return this.resolvingRequests.has(request);
    }
    canResolvePending(request: PendingRequestPresentation): boolean {
        const connection = this.model.connection();
        return this.canSubmit() && request.generation === connection.generation
            && this.model.pendingRequestPresentations().get(request.id) === request
            && !this.resolvingRequests.has(request);
    }
    resolvePending(request: PendingRequestPresentation, submission: PendingRequestSubmission): boolean {
        if (!this.canResolvePending(request)) return false;
        const response = pendingResponse(request, submission);
        if (!response) { this.setNotice("The authored pending response is invalid."); return false; }
        let requestId: unknown;
        try { requestId = JSON.parse(request.id); }
        catch { this.setNotice("The pending request has an invalid identity."); return false; }
        this.resolvingRequests.add(request);
        const sent = this.sdk.sendRawJson({jsonrpc: "2.0", id: requestId,
            ...("error" in response ? {error: response.error} : {result: response.result})});
        if (!sent) {
            this.resolvingRequests.delete(request);
            this.setNotice("The pending response could not be sent.");
            return false;
        }
        if (request.threadId !== "")
            this.model.noteThreadActivity(request.threadId, Math.floor(Date.now() / 1000));
        this.publish();
        return true;
    }

    private providerReady(): boolean {
        const connection = this.model.connection();
        return connection.connected && connection.providerState === "ready";
    }
    private threadRuntime(threadId: string): ThreadRuntimeState {
        let runtime = this.runtimeByThread.get(threadId);
        if (!runtime) {
            runtime = {phase: "cold", historyPageInFlight: false, seenHistoryCursors: new Set()};
            this.runtimeByThread.set(threadId, runtime);
        }
        return runtime;
    }
    private invalidateProviderWork(): void {
        ++this.lifecycleEpoch;
        ++this.threadListCycle;
        if (this.threadListRepairTimer) clearTimeout(this.threadListRepairTimer);
        this.threadListRepairTimer = undefined;
        this.threadListInFlightEpoch = -1;
        this.threadListRepairPending = false;
        this.threadListLoadMoreRequested = false;
        this.threadListNextCursor = "";
        this.threadListSeenCursors.clear();
        this.historicalHydrationQueue.length = 0;
        this.historicalHydrations.clear();
        this.historicalHydrationVisited.clear();
        this.itemPageQueue.length = 0;
        this.catalogHydrationKey = "";
        this.pendingUserOperations.clear();
        const affectedThreads = new Set([...this.runtimeByThread.keys(), ...this.prompts.queuedThreadIds()]);
        for (const threadId of affectedThreads) {
            const runtime = this.runtimeByThread.get(threadId);
            if (runtime) runtime.phase = "cold";
            for (const submission of this.prompts.submissions(threadId)) {
                if (submission.state !== "queued" && submission.state !== "inFlight") continue;
                const uncertain = submission.state === "inFlight";
                this.prompts.fail(threadId, submission.id, uncertain
                    ? "Provider changed before the turn was acknowledged; its outcome is unknown"
                    : "Provider changed before this queued prompt was sent");
                this.cancelPendingAnimation(submission.id);
            }
        }
        this.runtimeByThread.clear();
    }
    private retireProviderPresentations(): void {
        const retainedDraftId = this.newThreadDraft ? DraftThreadId : "";
        for (const identity of this.viewportState.retireExcept(retainedDraftId))
            this.settingDrafts.delete(identity);
    }
    private ensureThreadHydrated(threadId: string): void {
        if (!this.providerReady() || threadId === "" || threadId === DraftThreadId) return;
        const runtime = this.threadRuntime(threadId);
        if (runtime.phase !== "cold") return;
        void this.hydrateThread(threadId, true);
    }
    private async hydrateThread(threadId: string, interactive: boolean, forced = false): Promise<void> {
        if (!this.providerReady() || threadId === "") return;
        const runtime = this.threadRuntime(threadId);
        if (runtime.phase === "hydrating" || runtime.phase === "resuming" || runtime.historyPageInFlight) return;
        if (!forced && runtime.phase !== "cold") return;
        runtime.phase = "hydrating";
        runtime.historyPageInFlight = true;
        runtime.seenHistoryCursors.clear();
        this.schedulePublish();
        const current = () => this.runtimeByThread.get(threadId) === runtime && runtime.phase === "hydrating";
        let resumeAttempted = false;
        if (interactive && this.sdk.role === "controller") {
            resumeAttempted = true;
            const response = await this.requestPromise("thread.resume", {threadId, excludeTurns: true}, current);
            if (response.stale || !current()) return;
            if (!response.ok) this.setNotice(`Thread settings refresh failed: ${this.errorMessage(response)}`);
        }
        const response = await this.requestTurnPage(threadId, "", runtime, current);
        if (response.stale || !current()) return;
        runtime.historyPageInFlight = false;
        if (!response.ok) {
            runtime.phase = "failed";
            const message = `Thread history requires paginated app-server support: ${this.errorMessage(response)}`;
            this.prompts.failQueued(threadId, message);
            this.setNotice(message);
            return;
        }
        runtime.phase = "ready";
        this.queueTurnItems(threadId, response.data, runtime);
        this.queueActiveAgentChildren(threadId);
        this.publish();
        queueMicrotask(() => this.dispatchNextPrompt(threadId, resumeAttempted));
    }
    private requestTurnPage(threadId: string, cursor: string, runtime: ThreadRuntimeState,
        acceptResult: () => boolean = () => this.runtimeByThread.get(threadId) === runtime
            && runtime.phase !== "failed"): Promise<OperationResponse> {
        return this.requestPromise("thread.turns.list", {
            threadId, limit: 80, sortDirection: "desc", itemsView: "summary",
            ...(cursor === "" ? {} : {cursor}),
        }, acceptResult);
    }
    private queueTurnItems(threadId: string, responseData: unknown, runtime: ThreadRuntimeState): void {
        const turns = isObject(responseData) && Array.isArray(responseData.data)
            ? responseData.data.filter(isObject) : [];
        for (const turn of turns) {
            const turnId = stringMember(turn, "id");
            if (turnId !== "") this.itemPageQueue.push({threadId, turnId, cursor: "", runtime});
        }
        this.pumpItemPages();
    }
    private pumpItemPages(): void {
        while (this.pendingItemPages < MaximumItemHydrations && this.itemPageQueue.length > 0) {
            const page = this.itemPageQueue.shift()!;
            if (this.runtimeByThread.get(page.threadId) !== page.runtime || page.runtime.phase === "failed") continue;
            ++this.pendingItemPages;
            void this.requestPromise("thread.items.list", {
                threadId: page.threadId, turnId: page.turnId, limit: 80, sortDirection: "desc",
                ...(page.cursor === "" ? {} : {cursor: page.cursor}),
            }, () => this.runtimeByThread.get(page.threadId) === page.runtime
                && page.runtime.phase !== "failed").then(response => {
                --this.pendingItemPages;
                if (!response.stale && response.ok) {
                    const cursor = isObject(response.data) ? stringMember(response.data, "nextCursor") : "";
                    if (cursor !== "") this.itemPageQueue.push({...page, cursor});
                    this.queueActiveAgentChildren(page.threadId);
                } else if (!response.stale) {
                    this.setNotice(`Thread item history could not be loaded: ${this.errorMessage(response)}`);
                }
                this.pumpItemPages();
            });
        }
    }
    private queueActiveAgentChildren(parentThreadId: string): void {
        for (const child of this.model.activeAgentChildIds(parentThreadId)) {
            const runtime = this.threadRuntime(child);
            if (runtime.phase !== "cold" || this.historicalHydrationVisited.has(child)) continue;
            this.historicalHydrationVisited.add(child);
            this.historicalHydrationQueue.push(child);
        }
        this.pumpHistoricalHydrations();
    }
    private pumpHistoricalHydrations(): void {
        while (this.historicalHydrations.size < MaximumHistoricalHydrations
            && this.historicalHydrationQueue.length > 0) {
            const threadId = this.historicalHydrationQueue.shift()!;
            const runtime = this.threadRuntime(threadId);
            if (runtime.phase !== "cold") continue;
            this.historicalHydrations.add(threadId);
            void this.hydrateThread(threadId, false).finally(() => {
                this.historicalHydrations.delete(threadId);
                this.pumpHistoricalHydrations();
            });
        }
    }
    private loadForkHistory(threadId: string, runtime: ThreadRuntimeState): void {
        if (runtime.historyPageInFlight) return;
        runtime.historyPageInFlight = true;
        void this.requestTurnPage(threadId, "", runtime).then(response => {
            if (this.runtimeByThread.get(threadId) !== runtime) return;
            runtime.historyPageInFlight = false;
            if (response.stale) return;
            if (!response.ok) {
                this.setNotice(`Thread history requires paginated app-server support: ${this.errorMessage(response)}`);
                return;
            }
            this.queueTurnItems(threadId, response.data, runtime);
            this.queueActiveAgentChildren(threadId);
            this.publish();
        });
    }
    loadMoreHistory(threadId: string, presentationKey?: string): void {
        if (!this.providerReady() || !this.matchesThreadPresentation(threadId, presentationKey)) return;
        const runtime = this.threadRuntime(threadId);
        const cursor = this.model.thread(threadId)?.historyNextCursor ?? "";
        if (runtime.phase !== "ready" || runtime.historyPageInFlight || cursor === ""
            || runtime.seenHistoryCursors.has(cursor)) return;
        runtime.historyPageInFlight = true;
        runtime.seenHistoryCursors.add(cursor);
        this.publish();
        void this.requestTurnPage(threadId, cursor, runtime).then(response => {
            if (this.runtimeByThread.get(threadId) !== runtime) return;
            runtime.historyPageInFlight = false;
            if (response.stale) return;
            if (!response.ok) {
                runtime.seenHistoryCursors.delete(cursor);
                this.setNotice(`Loading more thread history failed: ${this.errorMessage(response)}`);
                return;
            }
            this.queueTurnItems(threadId, response.data, runtime);
            this.queueActiveAgentChildren(threadId);
            this.publish();
        });
    }
    private dispatchQueuedPrompts(): void {
        if (!this.canSubmit()) return;
        for (const threadId of this.prompts.queuedThreadIds()) this.dispatchNextPrompt(threadId);
    }
    private resumePromptQueue(threadId: string): void {
        const runtime = this.threadRuntime(threadId);
        if (runtime.phase !== "ready" || this.model.thread(threadId)?.status.semantic !== "notLoaded"
            || !this.canSubmit()) return;
        runtime.phase = "resuming";
        this.requestPromise("thread.resume", {threadId, excludeTurns: true}, () =>
            this.runtimeByThread.get(threadId) === runtime && runtime.phase === "resuming").then(response => {
            if (response.stale) return;
            const current = this.runtimeByThread.get(threadId);
            if (current !== runtime || current.phase !== "resuming") return;
            if (response.ok && this.model.thread(threadId)?.status.semantic !== "notLoaded") {
                current.phase = "ready";
                queueMicrotask(() => this.dispatchNextPrompt(threadId));
                return;
            }
            current.phase = "failed";
            const message = response.ok ? "Thread resume returned without loading the thread"
                : this.errorMessage(response);
            this.prompts.failQueued(threadId, message); this.setNotice(message);
        });
    }
    private hydrateCatalogs(): void {
        if (!this.providerReady()) return;
        const connection = this.model.connection();
        const key = `${connection.generation}:${connection.providerGeneration}`;
        if (this.catalogHydrationKey !== key) {
            this.catalogHydrationKey = key;
            void this.requestInitialThreadPage();
            this.request("models.list", {});
            this.request("permission-profiles.list", {});
        }
        const queued = new Set(this.prompts.queuedThreadIds());
        if (this.selectedThreadId !== "" && this.selectedThreadId !== DraftThreadId)
            queued.add(this.selectedThreadId);
        for (const threadId of queued) this.ensureThreadHydrated(threadId);
        this.dispatchQueuedPrompts();
    }
    private request(action: Action, parameters: JsonObject, callback?: (frame: JsonObject, stale: boolean) => void,
        acceptResult: () => boolean = () => true): string {
        const method = actionMethods[action];
        const correlation = `web-request-${this.nextCorrelation++}`;
        const startedAtSequence = this.normalizer.sequence;
        const epoch = this.lifecycleEpoch;
        const request = this.sdk.request.bind(this.sdk) as unknown as RawRequest;
        const threadId = stringMember(parameters, "threadId");
        const recordsActivity = threadId !== "" && !isThreadHydrationAction(action);
        if (recordsActivity) {
            this.model.noteThreadActivity(threadId, Math.floor(Date.now() / 1000));
            this.schedulePublish();
        }
        request(method, parameters, response => {
            const envelope = isObject(response) ? response : {};
            const deliver = () => {
                if (this.disposed || epoch !== this.lifecycleEpoch || !acceptResult()) {
                    callback?.(envelope, true);
                    return;
                }
                if (recordsActivity) {
                    this.model.noteThreadActivity(threadId, Math.floor(Date.now() / 1000));
                    this.schedulePublish();
                }
                this.normalizer.operationResult(action, correlation, parameters, envelope, startedAtSequence);
                callback?.(envelope, false);
            };
            const error = isObject(envelope.error) ? envelope.error : {};
            const providerOrTransportFailure = error.code === -32002 || error.code === -32020;
            // The SDK can emit lifecycle-class failures before its provider/detach
            // callback. One microtask lets that callback retire the epoch without
            // reordering ordinary app-server results and notifications.
            if (providerOrTransportFailure) queueMicrotask(deliver);
            else deliver();
        });
        return correlation;
    }
    private requestPromise(action: Action, parameters: JsonObject,
        acceptResult: () => boolean = () => true): Promise<OperationResponse> {
        return new Promise(resolve => this.request(action, parameters, (response, stale) => resolve(stale
            ? {ok: false, stale: true, error: response.error}
            : Object.hasOwn(response, "result") ? {ok: true, data: response.result} : {ok: false, error: response.error}), acceptResult));
    }
    private threadListParameters(useStateDbOnly: boolean, cursor = ""): JsonObject {
        return {
            sortKey: "recency_at", sortDirection: "desc", limit: 100, useStateDbOnly,
            ...(cursor === "" ? {} : {cursor}),
        };
    }
    private async requestInitialThreadPage(): Promise<OperationResponse> {
        const epoch = this.lifecycleEpoch;
        if (this.threadListInFlightEpoch === epoch) return {ok: true};
        if (this.threadListRepairPending) return {ok: true};
        const cycle = ++this.threadListCycle;
        if (this.threadListRepairTimer) clearTimeout(this.threadListRepairTimer);
        this.threadListRepairTimer = undefined;
        this.threadListRepairPending = true;
        this.threadListLoadMoreRequested = false;
        this.threadListNextCursor = "";
        this.threadListSeenCursors.clear();
        this.threadListInFlightEpoch = epoch;
        let response: OperationResponse;
        try {
            response = await this.requestPromise("threads.list", this.threadListParameters(true),
                () => cycle === this.threadListCycle);
        } finally {
            if (this.threadListInFlightEpoch === epoch && cycle === this.threadListCycle) {
                this.threadListInFlightEpoch = -1;
            }
        }
        if (response.stale || epoch !== this.lifecycleEpoch || cycle !== this.threadListCycle)
            return response;
        if (response.ok) this.threadListNextCursor = stringMember(response.data, "nextCursor");
        this.threadListRepairTimer = setTimeout(() => {
            this.threadListRepairTimer = undefined;
            void this.repairThreadList(epoch, cycle);
        }, 16);
        return response;
    }
    private async repairThreadList(epoch: number, cycle: number): Promise<void> {
        if (this.disposed || epoch !== this.lifecycleEpoch || cycle !== this.threadListCycle)
            return;
        this.threadListInFlightEpoch = epoch;
        const response = await this.requestPromise("threads.list", this.threadListParameters(false),
            () => cycle === this.threadListCycle);
        if (epoch !== this.lifecycleEpoch || cycle !== this.threadListCycle) return;
        this.threadListInFlightEpoch = -1;
        this.threadListRepairPending = false;
        if (!response.ok || response.stale) {
            if (!response.stale) this.setNotice(`Thread reconciliation failed: ${this.errorMessage(response)}`);
            if (this.threadListLoadMoreRequested) {
                this.threadListLoadMoreRequested = false;
                this.loadMoreThreads();
            }
            return;
        }
        this.threadListNextCursor = stringMember(response.data, "nextCursor");
        this.publish();
        if (this.threadListLoadMoreRequested) {
            this.threadListLoadMoreRequested = false;
            this.loadMoreThreads();
        }
    }
    loadMoreThreads(): void {
        if (!this.providerReady()) return;
        if (this.threadListInFlightEpoch === this.lifecycleEpoch || this.threadListRepairPending) {
            this.threadListLoadMoreRequested = true;
            return;
        }
        const cursor = this.threadListNextCursor;
        if (cursor === "" || this.threadListSeenCursors.has(cursor)) {
            if (this.threadListSeenCursors.has(cursor)) this.threadListNextCursor = "";
            return;
        }
        this.threadListSeenCursors.add(cursor);
        const epoch = this.lifecycleEpoch;
        const cycle = this.threadListCycle;
        this.threadListInFlightEpoch = epoch;
        void this.requestPromise("threads.list", this.threadListParameters(true, cursor),
            () => cycle === this.threadListCycle).then(response => {
            if (epoch !== this.lifecycleEpoch || cycle !== this.threadListCycle) return;
            this.threadListInFlightEpoch = -1;
            if (!response.ok || response.stale) {
                this.threadListSeenCursors.delete(cursor);
                this.threadListLoadMoreRequested = false;
                if (!response.stale) this.setNotice(`Loading more threads failed: ${this.errorMessage(response)}`);
                return;
            }
            this.threadListNextCursor = stringMember(response.data, "nextCursor");
            this.publish();
            if (this.threadListLoadMoreRequested) {
                this.threadListLoadMoreRequested = false;
                this.loadMoreThreads();
            }
        });
    }
    private performUserOperation(keyAction: Action, action: Action, parameters: JsonObject,
        failureContext: string, presentationKey?: string): Promise<OperationResponse> | undefined {
        const threadId = stringMember(parameters, "threadId");
        const targetKey = presentationKey ?? this.threadVisualKey(threadId);
        if (targetKey === "" || !this.matchesThreadPresentation(threadId, targetKey)) return undefined;
        const key = `${keyAction}:${targetKey}`;
        if (this.pendingUserOperations.has(key)) return undefined;
        if (!this.canSubmit()) {
            this.setNotice(`${failureContext} is unavailable until Codex is ready and controlled.`);
            return undefined;
        }
        const operationIdentity = {};
        this.pendingUserOperations.set(key, operationIdentity);
        this.publish();
        const operation = this.requestPromise(action, parameters,
            () => this.matchesThreadPresentation(threadId, targetKey));
        void operation.then(response => {
            if (this.pendingUserOperations.get(key) === operationIdentity)
                this.pendingUserOperations.delete(key);
            if (!response.ok && !response.stale)
                this.setNotice(`${failureContext} failed: ${this.errorMessage(response)}`);
            else this.schedulePublish();
        });
        return operation;
    }
    private matchesThreadPresentation(threadId: string, presentationKey?: string): boolean {
        return presentationKey === undefined || this.threadVisualKey(threadId) === presentationKey;
    }
    private dispatchNextPrompt(threadId: string, metadataResumeAttempted = false): void {
        if (!this.canSubmit() || this.prompts.hasInFlight(threadId)) return;
        if (!this.prompts.submissions(threadId).some(submission => submission.state === "queued")) return;
        const runtime = this.threadRuntime(threadId);
        if (runtime.phase !== "ready") {
            this.ensureThreadHydrated(threadId);
            return;
        }
        const thread = this.model.thread(threadId);
        if (!thread) {
            runtime.phase = "cold";
            this.ensureThreadHydrated(threadId);
            return;
        }
        if (thread.status.semantic === "notLoaded" && !metadataResumeAttempted) {
            this.resumePromptQueue(threadId);
            return;
        }
        const dispatch = this.prompts.beginNext(threadId, this.model.activeTurnId(threadId));
        if (!dispatch) return;
        const submission = this.prompts.submission(threadId, dispatch.id);
        if (submission?.startsTurn && submission.sortActivityAt === undefined)
            submission.sortActivityAt = this.nextPromptActivityAt(Date.now());
        this.dispatchPrompt(dispatch);
    }
    private dispatchPrompt(dispatch: PromptDispatch): void {
        const input: JsonObject[] = [{type: "text", text: dispatch.prompt, text_elements: []}];
        for (const attachment of dispatch.attachments) {
            if (attachment.mimeType.startsWith("image/")) input.push({type: "localImage", path: attachment.path});
            else if (attachment.mimeType.startsWith("audio/")) input.push({type: "localAudio", path: attachment.path});
        }
        const action = dispatch.expectedTurnId ? "turn.steer" : "turn.start";
        const params: JsonObject = dispatch.expectedTurnId
            ? {threadId: dispatch.threadId, expectedTurnId: dispatch.expectedTurnId, clientUserMessageId: dispatch.clientUserMessageId, input}
            : {...dispatch.turnOptions, threadId: dispatch.threadId, clientUserMessageId: dispatch.clientUserMessageId, input};
        this.requestPromise(action, params, () =>
            this.prompts.submission(dispatch.threadId, dispatch.id)?.state === "inFlight").then(response => {
            if (response.stale) return;
            if (response.ok) {
                const turn = isObject(response.data) ? member(response.data, "turn", {}) : {};
                const turnId = stringMember(turn, "id") || undefined;
                const submission = this.prompts.submission(dispatch.threadId, dispatch.id);
                const startsTurn = submission?.startsTurn === true;
                if (startsTurn && submission?.sortActivityAt !== undefined)
                    this.model.notePromptActivity(dispatch.threadId, submission.sortActivityAt);
                this.prompts.acknowledge(dispatch.threadId, dispatch.id, turnId);
            } else {
                this.prompts.fail(dispatch.threadId, dispatch.id, this.errorMessage(response));
                this.cancelPendingAnimation(dispatch.id);
                this.setNotice(this.errorMessage(response));
            }
            this.publish();
            queueMicrotask(() => this.dispatchNextPrompt(dispatch.threadId));
        });
    }
    private reconcilePromptsForFrame(frame: JsonObject): void {
        const scope = isObject(frame.scope) ? frame.scope : {};
        const threadId = stringMember(scope, "threadId");
        if (threadId === "" || this.prompts.submissions(threadId).length === 0) return;
        const thread = this.model.thread(threadId);
        if (!thread) return;
        const historyResult = frame.kind === "result" && frame.ok === true
            && ["thread.read", "thread.turns.list", "thread.items.list"].includes(stringMember(frame, "action"));
        const type = stringMember(frame, "type");
        if (!historyResult && type !== "conversation.item.upsert" && type !== "conversation.item.append") return;
        if (!historyResult) {
            const turnId = stringMember(scope, "turnId");
            const itemId = stringMember(scope, "itemId");
            if (stringMember(thread.turns.get(turnId)?.items.get(itemId)?.raw, "type") !== "userMessage") return;
        }
        const submissions = this.prompts.submissions(threadId);
        this.prompts.reconcile(threadId, thread);
        for (const submission of submissions) if (!localCardVisible(submission)) this.cancelPendingAnimation(submission.id);
    }
    private errorMessage(response: {error?: unknown}): string {
        return stringMember(response.error, "message") || "Codex operation failed";
    }
    private schedulePendingAnimation(submissionId: number): void {
        this.cancelPendingAnimation(submissionId);
        this.pendingAnimationTimers.set(submissionId, setTimeout(() => {
            this.pendingAnimationTimers.delete(submissionId);
            this.publish();
        }, PendingAnimationDelayMilliseconds));
    }
    private cancelPendingAnimation(submissionId: number): void {
        const timer = this.pendingAnimationTimers.get(submissionId);
        if (timer) clearTimeout(timer);
        this.pendingAnimationTimers.delete(submissionId);
    }
    private handlePresentationFrame(frame: JsonObject, providerReset: boolean, requestsReset: boolean,
        retiringRequest?: PendingRequestPresentation): void {
        if (frame.kind !== "event") return;
        const type = stringMember(frame, "type");
        const data = isObject(frame.data) ? frame.data : {};
        if (providerReset) {
            this.invalidateProviderWork();
            this.retireProviderPresentations();
        }
        if (requestsReset) this.resolvingRequests.clear();
        if (type === "connection.lifecycle") {
            const state = stringMember(data, "state");
            if (["connecting", "retrying", "disconnected", "failure"].includes(state)) this.invalidateProviderWork();
        } else if (type === "connection.provider") {
            const state = stringMember(data, "state");
            if (state === "ready") this.hydrateCatalogs();
            else if (!providerReset) this.invalidateProviderWork();
        } else if (type === "connection.bridge" || type === "connection.controller") {
            if (this.providerReady()) queueMicrotask(() => {
                if (this.selectedThreadId !== "" && this.selectedThreadId !== DraftThreadId)
                    this.ensureThreadHydrated(this.selectedThreadId);
                this.dispatchQueuedPrompts();
            });
        } else if (type === "thread.removed") {
            const scope = isObject(frame.scope) ? frame.scope : {};
            const threadId = stringMember(scope, "threadId");
            this.prompts.clearThread(threadId);
            this.runtimeByThread.delete(threadId);
            const retiredIdentity = this.viewportState.retire(threadId);
            if (retiredIdentity) {
                this.settingDrafts.delete(retiredIdentity);
                for (const operation of this.pendingUserOperations.keys())
                    if (operation.endsWith(`:${retiredIdentity}`)) this.pendingUserOperations.delete(operation);
            }
            if (this.selectedThreadId === threadId) this.selectedThreadId = "";
        } else if (type === "pending-request.removed") {
            if (retiringRequest) this.resolvingRequests.delete(retiringRequest);
        } else if (type === "notice.added") {
            const notice = isObject(data.notice) ? data.notice : {};
            const message = stringMember(notice, "message") || stringMember(notice, "reason") || stringMember(notice, "detail");
            if (message !== "") this.setNotice(message, data.severity === "error");
        } else if (type === "system.diagnostic") {
            const message = stringMember(data, "message");
            if (message !== "") this.setNotice(`Protocol diagnostic: ${message}`, false);
        }
    }
    private setNotice(message: string, error = true): void {
        if (this.disposed) return;
        if (this.noticeTimer) clearTimeout(this.noticeTimer);
        this.notice = message;
        this.noticeTimer = setTimeout(() => { this.noticeTimer = undefined; this.notice = ""; this.publish(); }, error ? 10_000 : 6_000);
        this.publish();
    }
    private schedulePublish(): void {
        if (this.publishScheduled) return;
        this.publishScheduled = true;
        const schedule = typeof requestAnimationFrame === "function" ? requestAnimationFrame : (callback: FrameRequestCallback) => setTimeout(callback, 16);
        schedule(() => {
            this.publishScheduled = false;
            if (!this.disposed) this.publish();
        });
    }
    private publish(): void {
        ++this.revision; this.snapshot = this.makeSnapshot();
        for (const listener of this.listeners) listener();
    }
    private makeSnapshot(): BrowserSessionSnapshot {
        return {
            revision: this.revision, selectedThreadId: this.selectedThreadId,
            selectedThreadLoading: this.selectedThreadId !== "" && this.selectedThreadId !== DraftThreadId
                && this.runtimeByThread.get(this.selectedThreadId)?.phase === "hydrating",
            ...(this.newThreadDraft ? {newThreadDraft: this.newThreadDraft} : {}),
            protocolFrames: this.protocolFrames, notice: this.notice, bridgeUrl: this.bridgeUrl,
        };
    }
}
