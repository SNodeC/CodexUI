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
import {isTerminalTurnStatus} from "../presentation/PresentationStatus.js";
import {ProtocolNormalizer} from "../presentation/ProtocolNormalizer.js";
import {PromptCoordinator, indexAuthoritativeItems, promptWithFileLinks} from "../conversation/PromptCoordinator.js";
import type {AttachmentDraft, PromptDispatch} from "../conversation/PromptCoordinator.js";
import {DefaultAuthoritativeItemLimit, projectConversation} from "../conversation/ConversationProjection.js";
import {PendingAnimationDelayMilliseconds} from "../conversation/MiddleTypes.js";
import type {ConversationSnapshot} from "../conversation/MiddleTypes.js";
import {readBrowserStorage, writeBrowserStorage} from "./BrowserStorage.js";

const DraftThreadId = "__codexui_new_thread__";
const MaximumProtocolFrames = 500;

function isThreadHydrationAction(action: string): boolean {
    return action === "thread.read" || action === "thread.resume";
}

function retainedProtocolFrame(frame: JsonObject): unknown {
    if (stringMember(frame, "type") !== "pending-request.upsert") return structuredClone(frame);
    const data = isObject(frame.data) ? frame.data : {};
    return structuredClone({...frame, data: {
        requestId: member(data, "requestId"),
        category: stringMember(data, "category"),
        request: "[redacted; inspect the typed Requests view]",
    }});
}

const actionMethods: Readonly<Record<string, string>> = {
    "threads.list": "thread/list", "thread.read": "thread/read", "thread.create": "thread/start",
    "thread.resume": "thread/resume", "thread.fork": "thread/fork", "thread.rename": "thread/name/set",
    "thread.archive": "thread/archive", "thread.unarchive": "thread/unarchive", "thread.delete": "thread/delete",
    "models.list": "model/list", "model-provider-capabilities.read": "model/provider/capabilities/read",
    "account.read": "account/read", "account.rate-limits.read": "account/rateLimits/read",
    "account.token-usage.read": "account/tokenUsage/read", "config.read": "config/read",
    "permission-profiles.list": "permissionProfile/list", "experimental-features.list": "experimentalFeature/list",
    "skills.list": "skills/list", "hooks.list": "hooks/list", "plugins.list": "plugin/list",
    "apps.list": "app/list", "mcp-servers.list": "mcpServer/status/list",
    "turn.start": "turn/start", "turn.steer": "turn/steer", "turn.interrupt": "turn/interrupt",
};

export interface BrowserSessionSnapshot {
    readonly revision: number;
    readonly selectedThreadId: string;
    readonly newThreadIntent: boolean;
    readonly newThreadDraft?: NewThreadDraft;
    readonly newThreadDraftRevision: number;
    readonly optimisticThreads: readonly OptimisticThreadSnapshot[];
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

export interface OptimisticThreadSnapshot {
    readonly id: string;
    readonly visualKey: string;
    readonly title: string;
    readonly cwd: string;
    readonly state: "awaiting" | "failed" | "confirmed";
}

export type ThreadSortCriterion = "alphanumeric" | "created" | "updated" | "recent";

const threadTitleCollator = new Intl.Collator(undefined, {
    numeric: true, sensitivity: "base", ignorePunctuation: true,
});

type HydrationState = "notHydrated" | "inFlight" | "hydrated" | "failed";
interface ThreadRuntimeState {
    hydration: HydrationState;
    operationReady: boolean;
    resumeInFlight: boolean;
    readRevision: number;
    provisionalActiveTurnId: string | undefined;
    readonly recoveryAttemptedSubmissions: Set<number>;
}
interface OperationResponse {ok: boolean; data?: unknown; error?: unknown; stale?: boolean}

type RawRequest = (method: string, params: unknown, handler: (response: unknown) => void) => string;
type RegisterNotification = (method: string, handler: (notification: JsonObject) => void) => void;
type RegisterRequest = (method: string, handler: (request: JsonObject) => void) => void;

export class BrowserFrontendSession {
    readonly model = new PresentationModel();
    readonly prompts = new PromptCoordinator();
    private readonly sdk = new CodexBridgeClient();
    private readonly connection: ClientConnection;
    private readonly normalizer: ProtocolNormalizer;
    private readonly listeners = new Set<() => void>();
    private readonly protocolFrames: unknown[] = [];
    private readonly runtimeByThread = new Map<string, ThreadRuntimeState>();
    private readonly resolvingRequests = new Set<string>();
    private readonly pendingUserOperations = new Set<string>();
    private readonly pendingAnimationTimers = new Map<number, ReturnType<typeof setTimeout>>();
    private transport: WebSocketTransport | undefined;
    private selectedThreadId = "";
    private newThreadIntent = false;
    private newThreadDraft: NewThreadDraft | undefined;
    private newThreadDraftRevision = 0;
    private optimisticThreads: OptimisticThreadSnapshot[] = [];
    private readonly threadVisualKeys = new Map<string, string>();
    private nextOptimisticThread = 1;
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
    private bridgeUrl: string;
    private readonly createWebSocket: WebSocketFactory | undefined;
    private snapshot: BrowserSessionSnapshot;

    constructor(bridgeUrl = BrowserFrontendSession.defaultBridgeUrl(), createWebSocket?: WebSocketFactory) {
        this.bridgeUrl = bridgeUrl;
        this.createWebSocket = createWebSocket;
        this.normalizer = new ProtocolNormalizer(frame => {
            this.model.applyEvent(frame);
            const scope = isObject(frame.scope) ? frame.scope : {};
            const threadId = stringMember(scope, "threadId");
            const hydrationResult = stringMember(frame, "kind") === "result"
                && isThreadHydrationAction(stringMember(frame, "action"));
            if (threadId !== "" && !hydrationResult)
                this.model.noteThreadActivity(threadId, Math.floor(Date.now() / 1000));
            this.protocolFrames.push(retainedProtocolFrame(frame));
            if (this.protocolFrames.length > MaximumProtocolFrames) this.protocolFrames.shift();
            this.reconcilePromptsForFrame(frame);
            this.handlePresentationFrame(frame);
            this.schedulePublish();
            return true;
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
    notify(message: string, error = false): void { this.setNotice(message, error); }
    claimController(): boolean { return this.sdk.claimController(); }
    releaseController(): boolean { return this.sdk.releaseController(); }
    canSubmit(): boolean {
        const connection = this.model.connection();
        return connection.connected && connection.providerState === "ready" && connection.role === "controller";
    }

    selectThread(threadId: string): void {
        if (threadId === DraftThreadId && this.newThreadIntent) { this.publish(); return; }
        if (this.prompts.submissions(DraftThreadId).length === 0) {
            this.optimisticThreads = this.optimisticThreads.filter(thread => thread.id !== DraftThreadId);
            this.newThreadDraft = undefined;
        }
        this.selectedThreadId = threadId; this.newThreadIntent = false; this.publish();
        if (threadId !== "") this.ensureThreadHydrated(threadId);
    }
    beginNewThread(draft: NewThreadDraft = {
        workspace: "", name: "", baseInstructions: "", developerInstructions: "", ephemeral: false,
    }): void {
        if (this.newThreadCreationInFlight) { this.setNotice("The current new thread is still being created.", false); return; }
        this.prompts.clearThread(DraftThreadId);
        this.newThreadDraft = {
            workspace: draft.workspace.trim(), name: draft.name.trim(),
            baseInstructions: draft.baseInstructions.trim(), developerInstructions: draft.developerInstructions.trim(),
            ephemeral: draft.ephemeral,
        };
        this.selectedThreadId = ""; this.newThreadIntent = true;
        ++this.newThreadDraftRevision;
        this.optimisticThreads = [{
            id: DraftThreadId, visualKey: `optimistic-thread-${this.nextOptimisticThread++}`,
            title: this.newThreadDraft.name || "New thread", cwd: this.newThreadDraft.workspace, state: "awaiting",
        }, ...this.optimisticThreads.filter(thread => thread.id !== DraftThreadId)];
        this.publish();
    }
    threadVisualKey(threadId: string): string { return this.threadVisualKeys.get(threadId) ?? threadId; }
    threadOrder(criterion: ThreadSortCriterion = "recent"): readonly string[] {
        const order = this.model.threadOrder().filter(id => this.model.childOwnership(id) === undefined);
        const timestamp = (threadId: string) => {
            const thread = this.model.thread(threadId);
            return criterion === "created" ? thread?.createdAt
                : criterion === "updated" ? thread?.updatedAt : thread?.recencyAt;
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
        const projectionId = this.selectedThreadId === "" && this.newThreadIntent ? DraftThreadId : this.selectedThreadId;
        const thread = this.model.thread(this.selectedThreadId);
        const index = indexAuthoritativeItems(projectionId, thread);
        if (thread) this.prompts.decorate(this.selectedThreadId, index);
        const conversation = projectConversation(index, this.prompts.submissions(projectionId), limit, Date.now(), thread);
        conversation.activeTurnId = this.activeTurnId(this.selectedThreadId);
        return conversation;
    }
    loadMore(): void { /* Default parity window is sufficient until viewport pausing is introduced. */ }

    async submitPrompt(prompt: string, attachments: AttachmentDraft[] = [], turnOptions: JsonObject = {}, threadOptions: JsonObject = {}): Promise<boolean> {
        const canonicalPrompt = promptWithFileLinks(prompt.trim(), attachments);
        if (canonicalPrompt === "") return false;
        if (!this.canSubmit()) {
            this.setNotice("Codex is not ready for a controlled turn. Your message was not sent.");
            return false;
        }
        let destination = this.selectedThreadId;
        let thread = this.model.thread(destination);
        if (destination === "") {
            if (!this.newThreadIntent) { this.setNotice("Select a thread or choose New thread before sending."); return false; }
            destination = DraftThreadId; thread = undefined;
        }
        const submissionId = this.prompts.admit(destination, canonicalPrompt, attachments, turnOptions, thread,
            destination === DraftThreadId ? undefined : this.activeTurnId(destination), Date.now());
        this.schedulePendingAnimation(submissionId);
        if (destination !== DraftThreadId) {
            this.threadRuntime(destination);
            this.model.notePromptActivity(destination, Math.floor(Date.now() / 1000));
        }
        this.publish();
        if (destination === DraftThreadId) {
            if (this.newThreadCreationInFlight) return true;
            this.newThreadCreationInFlight = true;
            this.optimisticThreads = this.optimisticThreads.map(thread =>
                thread.id === DraftThreadId ? {...thread, state: "awaiting"} : thread);
            const threadDraft = this.newThreadDraft;
            const createOptions: JsonObject = {...threadOptions};
            if (threadDraft?.baseInstructions) createOptions.baseInstructions = threadDraft.baseInstructions;
            if (threadDraft?.developerInstructions) createOptions.developerInstructions = threadDraft.developerInstructions;
            if (threadDraft?.ephemeral) createOptions.ephemeral = true;
            if (threadDraft?.workspace) createOptions.cwd = threadDraft.workspace;
            const created = await this.requestPromise("thread.create", createOptions);
            this.newThreadCreationInFlight = false;
            const createdThread = isObject(created.data) ? member(created.data, "thread", {}) : {};
            const id = stringMember(createdThread, "id");
            if (!created.ok || id === "" || !this.prompts.reassignThread(DraftThreadId, id)) {
                this.optimisticThreads = this.optimisticThreads.map(thread =>
                    thread.id === DraftThreadId ? {...thread, state: "failed"} : thread);
                this.prompts.failQueued(DraftThreadId, this.errorMessage(created)); this.setNotice(this.errorMessage(created)); return false;
            }
            const optimisticDraft = this.optimisticThreads.find(thread => thread.id === DraftThreadId);
            if (optimisticDraft) this.threadVisualKeys.set(id, optimisticDraft.visualKey);
            this.optimisticThreads = this.optimisticThreads.map(thread =>
                thread.id === DraftThreadId ? {
                    ...thread, id,
                    title: stringMember(createdThread, "name") || thread.title,
                    cwd: stringMember(createdThread, "cwd") || thread.cwd,
                } : thread);
            const draftStillSelected = this.selectedThreadId === "" && this.newThreadIntent;
            if (draftStillSelected) { this.selectedThreadId = id; this.newThreadIntent = false; }
            destination = id;
            const runtime = this.threadRuntime(id);
            runtime.hydration = "hydrated"; runtime.operationReady = true;
            const requestedName = threadDraft?.name ?? "";
            this.newThreadDraft = undefined;
            if (requestedName !== "") this.renameThread(id, requestedName);
            this.publish();
        }
        queueMicrotask(() => this.dispatchNextPrompt(destination));
        return true;
    }

    interrupt(): void {
        const turnId = this.activeTurnId(this.selectedThreadId);
        if (turnId) this.request("turn.interrupt", {threadId: this.selectedThreadId, turnId});
    }
    operationPending(action: string, threadId = ""): boolean {
        return this.pendingUserOperations.has(`${action}:${threadId}`);
    }
    requestThreads(): void {
        void this.performUserOperation("threads.refresh", "threads.list", {}, "Refresh threads", false);
    }
    renameThread(threadId: string, name: string): void {
        void this.performUserOperation("thread.rename", "thread.rename", {threadId, name}, "Rename thread");
    }
    reloadThread(threadId: string): void { this.readThread(threadId, true); }
    forkThread(threadId: string): void {
        this.performUserOperation("thread.fork", "thread.fork", {threadId}, "Fork thread")?.then(response => {
            const thread = isObject(response.data) ? member(response.data, "thread", {}) : {};
            const id = stringMember(thread, "id");
            if (response.ok && id !== "") this.selectThread(id);
            else if (response.ok) this.setNotice("Fork thread failed: no thread was returned.");
        });
    }
    archiveThread(threadId: string, archived: boolean): void {
        void this.performUserOperation("thread.archive", archived ? "thread.unarchive" : "thread.archive",
            {threadId}, archived ? "Unarchive thread" : "Archive thread");
    }
    deleteThread(threadId: string): void {
        void this.performUserOperation("thread.delete", "thread.delete", {threadId}, "Delete thread");
    }
    isPendingResolving(request: PendingRequestPresentation): boolean {
        return this.resolvingRequests.has(request.id);
    }
    canResolvePending(request: PendingRequestPresentation): boolean {
        const connection = this.model.connection();
        return this.canSubmit() && request.generation === connection.generation
            && this.model.pendingRequestPresentations().get(request.id) === request
            && !this.resolvingRequests.has(request.id);
    }
    resolvePending(request: PendingRequestPresentation, response: {result?: unknown; error?: unknown}): boolean {
        if (!this.canResolvePending(request)) return false;
        let requestId: unknown;
        try { requestId = JSON.parse(request.id); }
        catch { this.setNotice("The pending request has an invalid identity."); return false; }
        this.resolvingRequests.add(request.id);
        const sent = this.sdk.sendRawJson({jsonrpc: "2.0", id: requestId,
            ...(Object.hasOwn(response, "error") ? {error: response.error} : {result: response.result ?? {}})});
        if (!sent) {
            this.resolvingRequests.delete(request.id);
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
    activeTurnId(threadId: string): string | undefined {
        const authoritative = this.model.activeTurnId(threadId);
        if (authoritative !== undefined) return authoritative;
        const provisional = this.runtimeByThread.get(threadId)?.provisionalActiveTurnId;
        const turn = provisional === undefined ? undefined : this.model.thread(threadId)?.turns.get(provisional);
        return turn && isTerminalTurnStatus(turn.status) ? undefined : provisional;
    }
    private threadRuntime(threadId: string): ThreadRuntimeState {
        let runtime = this.runtimeByThread.get(threadId);
        if (!runtime) {
            runtime = {hydration: "notHydrated", operationReady: false, resumeInFlight: false,
                readRevision: 0, provisionalActiveTurnId: undefined,
                recoveryAttemptedSubmissions: new Set()};
            this.runtimeByThread.set(threadId, runtime);
        }
        return runtime;
    }
    private invalidateProviderWork(): void {
        ++this.lifecycleEpoch;
        this.catalogHydrationKey = "";
        this.resolvingRequests.clear();
        for (const [threadId, runtime] of this.runtimeByThread) {
            ++runtime.readRevision;
            runtime.hydration = "notHydrated";
            runtime.operationReady = false;
            runtime.resumeInFlight = false;
            runtime.provisionalActiveTurnId = undefined;
            runtime.recoveryAttemptedSubmissions.clear();
            for (const submission of this.prompts.submissions(threadId))
                if (submission.state === "inFlight") this.prompts.requeue(threadId, submission.id);
        }
    }
    private ensureThreadHydrated(threadId: string): void {
        if (!this.providerReady() || threadId === "") return;
        const runtime = this.threadRuntime(threadId);
        if (["inFlight", "hydrated", "failed"].includes(runtime.hydration)) return;
        this.readThread(threadId);
    }
    private readThread(threadId: string, forced = false): void {
        if (!this.providerReady() || threadId === "") return;
        const runtime = this.threadRuntime(threadId);
        if (runtime.resumeInFlight) return;
        if (!forced && runtime.hydration !== "notHydrated") return;
        runtime.hydration = "inFlight";
        runtime.operationReady = false;
        const revision = ++runtime.readRevision;
        const epoch = this.lifecycleEpoch;
        this.requestPromise("thread.read", {threadId, includeTurns: true}, () => epoch === this.lifecycleEpoch
            && this.runtimeByThread.get(threadId)?.readRevision === revision).then(response => {
            if (response.stale) return;
            const current = this.runtimeByThread.get(threadId);
            if (!current || current.readRevision !== revision) return;
            if (response.ok && this.model.thread(threadId)) {
                current.hydration = "hydrated";
                current.operationReady = this.model.thread(threadId)?.status !== "notLoaded";
                queueMicrotask(() => this.dispatchNextPrompt(threadId));
                return;
            }
            current.hydration = "failed";
            const message = response.ok ? "Thread loading returned no thread" : this.errorMessage(response);
            this.prompts.failQueued(threadId, message);
            this.setNotice(message);
        });
    }
    private dispatchQueuedPrompts(): void {
        if (!this.canSubmit()) return;
        for (const threadId of this.prompts.queuedThreadIds()) this.dispatchNextPrompt(threadId);
    }
    private resumePromptQueue(threadId: string): void {
        const runtime = this.threadRuntime(threadId);
        if (runtime.resumeInFlight || !this.canSubmit()) return;
        runtime.resumeInFlight = true;
        const epoch = this.lifecycleEpoch;
        this.requestPromise("thread.resume", {threadId}, () => epoch === this.lifecycleEpoch
            && this.runtimeByThread.get(threadId)?.resumeInFlight === true).then(response => {
            if (response.stale) return;
            const current = this.runtimeByThread.get(threadId);
            if (!current) return;
            current.resumeInFlight = false;
            if (response.ok) {
                current.hydration = "hydrated"; current.operationReady = true;
                queueMicrotask(() => this.dispatchNextPrompt(threadId));
                return;
            }
            current.hydration = "failed";
            const message = this.errorMessage(response);
            this.prompts.failQueued(threadId, message); this.setNotice(message);
        });
    }
    private attemptThreadRecovery(dispatch: PromptDispatch, response: OperationResponse): boolean {
        const message = this.errorMessage(response).toLowerCase();
        if (response.ok || !message.includes("thread") || !message.includes("not found")) return false;
        const runtime = this.threadRuntime(dispatch.threadId);
        if (runtime.recoveryAttemptedSubmissions.has(dispatch.id)) return false;
        runtime.recoveryAttemptedSubmissions.add(dispatch.id);
        if (!this.prompts.requeue(dispatch.threadId, dispatch.id)) return false;
        runtime.hydration = "hydrated";
        runtime.operationReady = false;
        this.resumePromptQueue(dispatch.threadId);
        return true;
    }
    private hydrateCatalogs(): void {
        if (!this.providerReady()) return;
        const connection = this.model.connection();
        const key = `${connection.generation}:${connection.providerGeneration}`;
        if (this.catalogHydrationKey !== key) {
            this.catalogHydrationKey = key;
            this.request("threads.list", {}); this.request("models.list", {}); this.request("permission-profiles.list", {});
        }
        const queued = new Set(this.prompts.queuedThreadIds());
        if (this.selectedThreadId !== "") queued.add(this.selectedThreadId);
        for (const threadId of queued) this.ensureThreadHydrated(threadId);
        this.dispatchQueuedPrompts();
    }
    private request(action: string, parameters: JsonObject, callback?: (frame: JsonObject, stale: boolean) => void,
        acceptResult: () => boolean = () => true): string {
        const method = actionMethods[action];
        const correlation = `web-request-${this.nextCorrelation++}`;
        if (!method) { this.normalizer.operationRejected(action, correlation, -32601, "unsupported CodexUI presentation action"); return correlation; }
        const startedAtSequence = this.normalizer.sequence;
        const request = this.sdk.request.bind(this.sdk) as unknown as RawRequest;
        const threadId = stringMember(parameters, "threadId");
        const recordsActivity = threadId !== "" && !isThreadHydrationAction(action);
        if (recordsActivity) {
            this.model.noteThreadActivity(threadId, Math.floor(Date.now() / 1000));
            this.schedulePublish();
        }
        request(method, parameters, response => {
            if (!acceptResult()) { callback?.({}, true); return; }
            if (recordsActivity) {
                this.model.noteThreadActivity(threadId, Math.floor(Date.now() / 1000));
                this.schedulePublish();
            }
            const envelope = isObject(response) ? response : {};
            this.normalizer.operationResult(action, correlation, parameters, envelope, startedAtSequence);
            callback?.(envelope, false);
        });
        return correlation;
    }
    private requestPromise(action: string, parameters: JsonObject,
        acceptResult: () => boolean = () => true): Promise<OperationResponse> {
        return new Promise(resolve => this.request(action, parameters, (response, stale) => resolve(stale
            ? {ok: false, stale: true}
            : Object.hasOwn(response, "result") ? {ok: true, data: response.result} : {ok: false, error: response.error}), acceptResult));
    }
    private performUserOperation(keyAction: string, action: string, parameters: JsonObject,
        failureContext: string, requiresControl = true): Promise<OperationResponse> | undefined {
        const threadId = stringMember(parameters, "threadId");
        const key = `${keyAction}:${threadId}`;
        if (this.pendingUserOperations.has(key)) return undefined;
        if (requiresControl ? !this.canSubmit() : !this.providerReady()) {
            this.setNotice(`${failureContext} is unavailable until Codex is ready${requiresControl ? " and controlled" : ""}.`);
            return undefined;
        }
        this.pendingUserOperations.add(key);
        this.publish();
        const operation = this.requestPromise(action, parameters);
        void operation.then(response => {
            this.pendingUserOperations.delete(key);
            if (!response.ok && !response.stale)
                this.setNotice(`${failureContext} failed: ${this.errorMessage(response)}`);
            else this.publish();
        });
        return operation;
    }
    private dispatchNextPrompt(threadId: string): void {
        if (!this.canSubmit() || this.prompts.hasInFlight(threadId)) return;
        if (!this.prompts.submissions(threadId).some(submission => submission.state === "queued")) return;
        const runtime = this.threadRuntime(threadId);
        if (runtime.hydration !== "hydrated") {
            this.ensureThreadHydrated(threadId);
            return;
        }
        const thread = this.model.thread(threadId);
        if (!runtime.operationReady && thread?.status === "notLoaded") {
            this.resumePromptQueue(threadId);
            return;
        }
        if (!thread) {
            runtime.hydration = "notHydrated"; runtime.operationReady = false;
            this.ensureThreadHydrated(threadId);
            return;
        }
        const dispatch = this.prompts.beginNext(threadId, this.activeTurnId(threadId));
        if (!dispatch) return;
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
        const epoch = this.lifecycleEpoch;
        this.requestPromise(action, params, () => epoch === this.lifecycleEpoch
            && this.prompts.submission(dispatch.threadId, dispatch.id)?.state === "inFlight").then(response => {
            if (response.stale) return;
            if (this.attemptThreadRecovery(dispatch, response)) return;
            const runtime = this.runtimeByThread.get(dispatch.threadId);
            runtime?.recoveryAttemptedSubmissions.delete(dispatch.id);
            if (response.ok) {
                if (runtime) runtime.operationReady = true;
                const turn = isObject(response.data) ? member(response.data, "turn", {}) : {};
                const turnId = stringMember(turn, "id") || undefined;
                const startsTurn = this.prompts.submission(dispatch.threadId, dispatch.id)?.startsTurn === true;
                this.prompts.acknowledge(dispatch.threadId, dispatch.id, turnId);
                if (startsTurn && turnId !== undefined) {
                    if (runtime) runtime.provisionalActiveTurnId = turnId;
                }
                this.optimisticThreads = this.optimisticThreads.map(thread =>
                    thread.id === dispatch.threadId ? {...thread, state: "confirmed"} : thread);
            } else {
                this.prompts.fail(dispatch.threadId, dispatch.id, this.errorMessage(response));
                this.setNotice(this.errorMessage(response));
                this.optimisticThreads = this.optimisticThreads.map(thread =>
                    thread.id === dispatch.threadId ? {...thread, state: "failed"} : thread);
            }
            this.cancelPendingAnimation(dispatch.id);
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
        const resultRead = frame.kind === "result" && frame.ok === true && frame.action === "thread.read";
        const type = stringMember(frame, "type");
        if (!resultRead && type !== "conversation.item.upsert" && type !== "conversation.item.append") return;
        if (!resultRead) {
            const turnId = stringMember(scope, "turnId");
            const itemId = stringMember(scope, "itemId");
            if (stringMember(thread.turns.get(turnId)?.items.get(itemId)?.raw, "type") !== "userMessage") return;
        }
        this.prompts.reconcile(threadId, thread);
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
    private handlePresentationFrame(frame: JsonObject): void {
        if (frame.kind !== "event") return;
        const type = stringMember(frame, "type");
        const data = isObject(frame.data) ? frame.data : {};
        if (type === "connection.lifecycle") {
            const state = stringMember(data, "state");
            if (["connecting", "retrying", "disconnected", "failure"].includes(state)) this.invalidateProviderWork();
        } else if (type === "connection.provider") {
            const state = stringMember(data, "state");
            if (state === "ready") {
                const connection = this.model.connection();
                const providerKey = `${connection.generation}:${connection.providerGeneration}`;
                if (this.catalogHydrationKey !== "" && this.catalogHydrationKey !== providerKey) this.invalidateProviderWork();
                queueMicrotask(() => this.hydrateCatalogs());
            }
            else this.invalidateProviderWork();
        } else if (type === "connection.bridge" || type === "connection.controller") {
            if (this.providerReady()) queueMicrotask(() => {
                if (this.selectedThreadId !== "") this.ensureThreadHydrated(this.selectedThreadId);
                this.dispatchQueuedPrompts();
            });
        } else if (type === "thread.removed") {
            const scope = isObject(frame.scope) ? frame.scope : {};
            const threadId = stringMember(scope, "threadId");
            this.prompts.clearThread(threadId);
            this.runtimeByThread.delete(threadId);
            if (this.selectedThreadId === threadId) this.selectedThreadId = "";
        } else if (type === "pending-request.removed") {
            const scope = isObject(frame.scope) ? frame.scope : {};
            if (Object.hasOwn(scope, "requestId")) this.resolvingRequests.delete(JSON.stringify(scope.requestId));
        } else if (type === "notice.added") {
            const notice = isObject(data.notice) ? data.notice : {};
            const message = stringMember(notice, "message") || stringMember(notice, "reason") || stringMember(notice, "detail");
            if (message !== "") this.setNotice(message, data.severity === "error");
        } else if (type === "system.diagnostic") {
            const message = stringMember(data, "message");
            if (message !== "") this.setNotice(`Protocol diagnostic: ${message}`, false);
        } else if (type === "connection.lifecycle" && (data.state === "failure" || data.state === "disconnected")) {
            const detail = stringMember(data, "detail");
            if (detail !== "" && !detail.startsWith("local-")) this.setNotice(detail);
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
        schedule(() => { this.publishScheduled = false; this.publish(); });
    }
    private publish(): void {
        ++this.revision; this.snapshot = this.makeSnapshot();
        for (const listener of this.listeners) listener();
    }
    private makeSnapshot(): BrowserSessionSnapshot {
        this.optimisticThreads = this.optimisticThreads.filter(thread =>
            thread.state !== "confirmed" || !this.model.thread(thread.id));
        return {
            revision: this.revision, selectedThreadId: this.selectedThreadId, newThreadIntent: this.newThreadIntent,
            ...(this.newThreadDraft ? {newThreadDraft: this.newThreadDraft} : {}),
            newThreadDraftRevision: this.newThreadDraftRevision,
            optimisticThreads: this.optimisticThreads,
            protocolFrames: this.protocolFrames, notice: this.notice, bridgeUrl: this.bridgeUrl,
        };
    }
}
