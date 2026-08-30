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
import {PromptCoordinator, indexAuthoritativeItems, promptWithFileLinks} from "../conversation/PromptCoordinator.js";
import type {AttachmentDraft, PromptDispatch} from "../conversation/PromptCoordinator.js";
import {DefaultAuthoritativeItemLimit, projectConversation} from "../conversation/ConversationProjection.js";
import type {ConversationSnapshot} from "../conversation/MiddleTypes.js";

const DraftThreadId = "__codexui_new_thread__";
const MaximumProtocolFrames = 500;

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
    readonly optimisticThreads: readonly OptimisticThreadSnapshot[];
    readonly conversation: ConversationSnapshot;
    readonly protocolFrames: readonly unknown[];
    readonly notice: string;
    readonly bridgeUrl: string;
}

export interface OptimisticThreadSnapshot {
    readonly id: string;
    readonly visualKey: string;
    readonly title: string;
    readonly cwd: string;
    readonly state: "awaiting" | "failed" | "confirmed";
}

interface PromptPromotion {
    readonly rootThreadId: string;
    readonly recencyAt: number | undefined;
}

type HydrationState = "notHydrated" | "inFlight" | "hydrated" | "failed";
interface ThreadRuntimeState {
    hydration: HydrationState;
    operationReady: boolean;
    resumeInFlight: boolean;
    readRevision: number;
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
    private readonly acceptedTransitionTimers = new Map<number, ReturnType<typeof setTimeout>>();
    private transport: WebSocketTransport | undefined;
    private selectedThreadId = "";
    private newThreadIntent = false;
    private optimisticThreads: OptimisticThreadSnapshot[] = [];
    private readonly threadVisualKeys = new Map<string, string>();
    private promptPromotion: PromptPromotion | undefined;
    private nextOptimisticThread = 1;
    private newThreadCreationInFlight = false;
    private notice = "";
    private revision = 0;
    private nextCorrelation = 1;
    private noticeTimer: ReturnType<typeof setTimeout> | undefined;
    private publishScheduled = false;
    private disposed = false;
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
            this.protocolFrames.push(structuredClone(frame));
            if (this.protocolFrames.length > MaximumProtocolFrames) this.protocolFrames.shift();
            const scope = isObject(frame.scope) ? frame.scope : {};
            const threadId = stringMember(scope, "threadId");
            const thread = this.model.thread(threadId);
            if (thread) this.prompts.reconcile(threadId, thread, Date.now());
            this.handlePresentationFrame(frame);
            this.schedulePublish();
            return true;
        });
        this.connection = new ClientConnection(this.sdk, {
            onConnected: () => this.normalizer.transportEvent("connected"),
            onDisconnected: () => { this.transport = undefined; this.normalizer.transportEvent("disconnected"); },
            onFailure: reason => this.normalizer.transportEvent("failure", reason),
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
        const configured = window.localStorage.getItem("codexui.bridgeUrl");
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
        if (this.transport) return;
        this.bridgeUrl = url.trim();
        if (typeof window !== "undefined") window.localStorage.setItem("codexui.bridgeUrl", this.bridgeUrl);
        this.normalizer.transportEvent("connecting");
        try { this.transport = new WebSocketTransport(this.connection, this.bridgeUrl,
            this.createWebSocket ? {createWebSocket: this.createWebSocket} : {}); }
        catch (error) {
            this.transport = undefined;
            this.normalizer.transportEvent("failure", error instanceof Error ? error.message : "WebSocket setup failed");
        }
        this.schedulePublish();
    }
    disconnect(): void { this.connection.disconnect("local-disconnect"); this.transport = undefined; }
    reconnect(): void { this.disconnect(); queueMicrotask(() => this.connect()); }
    dispose(): void {
        this.disposed = true;
        this.connection.dispose(); this.transport = undefined;
        for (const timer of this.acceptedTransitionTimers.values()) clearTimeout(timer);
        this.acceptedTransitionTimers.clear();
        if (this.noticeTimer) clearTimeout(this.noticeTimer);
        this.noticeTimer = undefined;
    }
    dismissNotice(): void {
        if (this.noticeTimer) clearTimeout(this.noticeTimer);
        this.noticeTimer = undefined; this.notice = ""; this.publish();
    }
    claimController(): boolean { return this.sdk.claimController(); }
    releaseController(): boolean { return this.sdk.releaseController(); }
    canSubmit(): boolean {
        const connection = this.model.connection();
        return connection.connected && connection.providerState === "ready" && connection.role === "controller";
    }

    selectThread(threadId: string): void {
        if (threadId === DraftThreadId && this.newThreadIntent) { this.publish(); return; }
        if (this.prompts.submissions(DraftThreadId).length === 0)
            this.optimisticThreads = this.optimisticThreads.filter(thread => thread.id !== DraftThreadId);
        this.selectedThreadId = threadId; this.newThreadIntent = false; this.publish();
        if (threadId !== "") this.ensureThreadHydrated(threadId);
    }
    beginNewThread(): void {
        if (this.newThreadCreationInFlight) { this.setNotice("The current new thread is still being created.", false); return; }
        this.selectedThreadId = ""; this.newThreadIntent = true;
        this.optimisticThreads = [{
            id: DraftThreadId, visualKey: `optimistic-thread-${this.nextOptimisticThread++}`,
            title: "New thread", cwd: "", state: "awaiting",
        }, ...this.optimisticThreads.filter(thread => thread.id !== DraftThreadId)];
        this.publish();
    }
    threadVisualKey(threadId: string): string { return this.threadVisualKeys.get(threadId) ?? threadId; }
    threadOrder(): readonly string[] {
        const order = [...this.model.threadOrder()];
        const promotion = this.promptPromotion;
        if (!promotion || this.model.thread(promotion.rootThreadId)?.recencyAt !== promotion.recencyAt)
            return order;
        const index = order.indexOf(promotion.rootThreadId);
        if (index > 0) order.unshift(...order.splice(index, 1));
        return order;
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
        this.prompts.admit(destination, canonicalPrompt, attachments, turnOptions, thread,
            destination === DraftThreadId ? undefined : this.model.activeTurnId(destination), Date.now());
        if (destination !== DraftThreadId) {
            this.threadRuntime(destination);
            this.promotePromptedThread(destination);
        }
        this.publish();
        if (destination === DraftThreadId) {
            if (this.newThreadCreationInFlight) return true;
            this.newThreadCreationInFlight = true;
            this.optimisticThreads = this.optimisticThreads.map(thread =>
                thread.id === DraftThreadId ? {...thread, state: "awaiting"} : thread);
            const created = await this.requestPromise("thread.create", threadOptions);
            this.newThreadCreationInFlight = false;
            const createdThread = isObject(created.data) ? member(created.data, "thread", {}) : {};
            const id = stringMember(createdThread, "id");
            if (!created.ok || id === "" || !this.prompts.reassignThread(DraftThreadId, id)) {
                this.optimisticThreads = this.optimisticThreads.map(thread =>
                    thread.id === DraftThreadId ? {...thread, state: "failed"} : thread);
                this.prompts.failQueued(DraftThreadId, this.errorMessage(created)); this.setNotice(this.errorMessage(created)); return false;
            }
            const draft = this.optimisticThreads.find(thread => thread.id === DraftThreadId);
            if (draft) this.threadVisualKeys.set(id, draft.visualKey);
            this.optimisticThreads = this.optimisticThreads.map(thread =>
                thread.id === DraftThreadId ? {
                    ...thread, id,
                    title: stringMember(createdThread, "name") || thread.title,
                    cwd: stringMember(createdThread, "cwd") || thread.cwd,
                } : thread);
            this.selectedThreadId = id; this.newThreadIntent = false; destination = id;
            const runtime = this.threadRuntime(id);
            runtime.hydration = "hydrated"; runtime.operationReady = true;
            this.publish();
        }
        queueMicrotask(() => this.dispatchNextPrompt(destination));
        return true;
    }

    interrupt(): void {
        const turnId = this.model.activeTurnId(this.selectedThreadId);
        if (turnId) this.request("turn.interrupt", {threadId: this.selectedThreadId, turnId});
    }
    requestThreads(): void { this.request("threads.list", {}); }
    renameThread(threadId: string, name: string): void { this.request("thread.rename", {threadId, name}); }
    reloadThread(threadId: string): void { this.readThread(threadId, true); }
    forkThread(threadId: string): void {
        this.requestPromise("thread.fork", {threadId}).then(response => {
            const thread = isObject(response.data) ? member(response.data, "thread", {}) : {};
            const id = stringMember(thread, "id");
            if (response.ok && id !== "") this.selectThread(id);
            else this.setNotice(this.errorMessage(response));
        });
    }
    archiveThread(threadId: string, archived: boolean): void {
        this.request(archived ? "thread.unarchive" : "thread.archive", {threadId});
    }
    deleteThread(threadId: string): void { this.request("thread.delete", {threadId}); }
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
            runtime = {hydration: "notHydrated", operationReady: false, resumeInFlight: false,
                readRevision: 0, recoveryAttemptedSubmissions: new Set()};
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
        request(method, parameters, response => {
            if (!acceptResult()) { callback?.({}, true); return; }
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
        const dispatch = this.prompts.beginNext(threadId, this.model.activeTurnId(threadId));
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
                this.prompts.acknowledge(dispatch.threadId, dispatch.id, stringMember(turn, "id") || undefined, Date.now());
                this.scheduleAcceptedTransition(dispatch.threadId, dispatch.id);
                this.optimisticThreads = this.optimisticThreads.map(thread =>
                    thread.id === dispatch.threadId ? {...thread, state: "confirmed"} : thread);
            } else {
                this.prompts.fail(dispatch.threadId, dispatch.id, this.errorMessage(response));
                this.setNotice(this.errorMessage(response));
                this.optimisticThreads = this.optimisticThreads.map(thread =>
                    thread.id === dispatch.threadId ? {...thread, state: "failed"} : thread);
            }
            this.publish();
            queueMicrotask(() => this.dispatchNextPrompt(dispatch.threadId));
        });
    }
    private promotePromptedThread(threadId: string): void {
        let rootThreadId = threadId;
        const visited = new Set<string>();
        while (!visited.has(rootThreadId)) {
            visited.add(rootThreadId);
            const ownership = this.model.childOwnership(rootThreadId);
            if (!ownership) break;
            rootThreadId = ownership.parentThreadId;
        }
        this.promptPromotion = {
            rootThreadId,
            recencyAt: this.model.thread(rootThreadId)?.recencyAt,
        };
    }
    private errorMessage(response: {error?: unknown}): string {
        return stringMember(response.error, "message") || "Codex operation failed";
    }
    private scheduleAcceptedTransition(threadId: string, submissionId: number): void {
        const previous = this.acceptedTransitionTimers.get(submissionId);
        if (previous) clearTimeout(previous);
        this.acceptedTransitionTimers.set(submissionId, setTimeout(() => {
            this.acceptedTransitionTimers.delete(submissionId);
            const thread = this.model.thread(threadId);
            if (thread) this.prompts.reconcile(threadId, thread, Date.now());
            this.publish();
        }, 500));
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
        const projectionId = this.selectedThreadId === "" && this.newThreadIntent ? DraftThreadId : this.selectedThreadId;
        const thread = this.model.thread(this.selectedThreadId);
        const index = indexAuthoritativeItems(projectionId, thread);
        if (thread) this.prompts.reconcile(this.selectedThreadId, index, Date.now());
        return {
            revision: this.revision, selectedThreadId: this.selectedThreadId, newThreadIntent: this.newThreadIntent,
            optimisticThreads: this.optimisticThreads,
            conversation: projectConversation(index, this.prompts.submissions(projectionId), DefaultAuthoritativeItemLimit, Date.now(), thread),
            protocolFrames: this.protocolFrames, notice: this.notice, bridgeUrl: this.bridgeUrl,
        };
    }
}
