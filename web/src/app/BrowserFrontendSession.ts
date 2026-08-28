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
    readonly conversation: ConversationSnapshot;
    readonly protocolFrames: readonly unknown[];
    readonly notice: string;
    readonly bridgeUrl: string;
}

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
    private readonly resumeInFlight = new Set<string>();
    private readonly acceptedTransitionTimers = new Map<number, ReturnType<typeof setTimeout>>();
    private transport: WebSocketTransport | undefined;
    private selectedThreadId = "";
    private newThreadIntent = false;
    private notice = "";
    private revision = 0;
    private nextCorrelation = 1;
    private noticeTimer: ReturnType<typeof setTimeout> | undefined;
    private publishScheduled = false;
    private disposed = false;
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
            if (isObject(message) && ((message.kind === "bridge.connection" && message.event === "opened")
                || (message.kind === "bridge.provider" && message.state === "ready")))
                this.hydrateCatalogs();
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
        if (typeof window === "undefined") return "ws://127.0.0.1:8080/";
        const configured = window.localStorage.getItem("codexui.bridgeUrl");
        if (configured) return configured;
        const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
        return `${protocol}//${window.location.hostname || "127.0.0.1"}:8080/`;
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

    selectThread(threadId: string): void {
        this.selectedThreadId = threadId; this.newThreadIntent = false; this.publish();
        if (threadId !== "") this.request("thread.read", {threadId, includeTurns: true});
    }
    beginNewThread(): void { this.selectedThreadId = ""; this.newThreadIntent = true; this.publish(); }
    loadMore(): void { /* Default parity window is sufficient until viewport pausing is introduced. */ }

    async submitPrompt(prompt: string, attachments: AttachmentDraft[] = [], turnOptions: JsonObject = {}, threadOptions: JsonObject = {}): Promise<boolean> {
        const canonicalPrompt = promptWithFileLinks(prompt.trim(), attachments);
        if (canonicalPrompt === "") return false;
        let destination = this.selectedThreadId;
        let thread = this.model.thread(destination);
        if (destination === "") {
            if (!this.newThreadIntent) { this.setNotice("Select a thread or choose New thread before sending."); return false; }
            destination = DraftThreadId; thread = undefined;
        }
        this.prompts.admit(destination, canonicalPrompt, attachments, turnOptions, thread,
            destination === DraftThreadId ? undefined : this.model.activeTurnId(destination), Date.now());
        this.publish();
        if (destination === DraftThreadId) {
            const created = await this.requestPromise("thread.create", threadOptions);
            const createdThread = isObject(created.data) ? member(created.data, "thread", {}) : {};
            const id = stringMember(createdThread, "id");
            if (!created.ok || id === "" || !this.prompts.reassignThread(DraftThreadId, id)) {
                this.prompts.failQueued(DraftThreadId, this.errorMessage(created)); this.setNotice(this.errorMessage(created)); return false;
            }
            this.selectedThreadId = id; this.newThreadIntent = false; destination = id;
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
    reloadThread(threadId: string): void { this.request("thread.read", {threadId, includeTurns: true}); }
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
    resolvePending(requestId: unknown, response: {result?: unknown; error?: unknown}): boolean {
        return this.sdk.sendRawJson({jsonrpc: "2.0", id: requestId,
            ...(Object.hasOwn(response, "error") ? {error: response.error} : {result: response.result ?? {}})});
    }

    private hydrateCatalogs(): void {
        this.request("threads.list", {}); this.request("models.list", {}); this.request("permission-profiles.list", {});
        const queued = new Set(this.prompts.queuedThreadIds());
        if (this.selectedThreadId !== "") queued.add(this.selectedThreadId);
        for (const threadId of queued) this.request("thread.read", {threadId, includeTurns: true}, () => this.dispatchNextPrompt(threadId));
    }
    private request(action: string, parameters: JsonObject, callback?: (frame: JsonObject) => void): string {
        const method = actionMethods[action];
        const correlation = `web-request-${this.nextCorrelation++}`;
        if (!method) { this.normalizer.operationRejected(action, correlation, -32601, "unsupported CodexUI presentation action"); return correlation; }
        const startedAtSequence = this.normalizer.sequence;
        const request = this.sdk.request.bind(this.sdk) as unknown as RawRequest;
        request(method, parameters, response => {
            const envelope = isObject(response) ? response : {};
            this.normalizer.operationResult(action, correlation, parameters, envelope, startedAtSequence);
            callback?.(envelope);
        });
        return correlation;
    }
    private requestPromise(action: string, parameters: JsonObject): Promise<{ok: boolean; data?: unknown; error?: unknown}> {
        return new Promise(resolve => this.request(action, parameters, response => resolve(Object.hasOwn(response, "result")
            ? {ok: true, data: response.result} : {ok: false, error: response.error})));
    }
    private dispatchNextPrompt(threadId: string): void {
        if (!this.model.connection().connected || this.prompts.hasInFlight(threadId)) return;
        const thread = this.model.thread(threadId);
        if (thread?.status === "notLoaded") {
            if (this.resumeInFlight.has(threadId)) return;
            this.resumeInFlight.add(threadId);
            this.requestPromise("thread.resume", {threadId}).then(response => {
                this.resumeInFlight.delete(threadId);
                if (response.ok) this.dispatchNextPrompt(threadId);
                else { this.prompts.failQueued(threadId, this.errorMessage(response)); this.setNotice(this.errorMessage(response)); }
            });
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
        this.requestPromise(action, params).then(response => {
            if (response.ok) {
                const turn = isObject(response.data) ? member(response.data, "turn", {}) : {};
                this.prompts.acknowledge(dispatch.threadId, dispatch.id, stringMember(turn, "id") || undefined, Date.now());
                this.scheduleAcceptedTransition(dispatch.threadId, dispatch.id);
            } else {
                this.prompts.fail(dispatch.threadId, dispatch.id, this.errorMessage(response));
                this.setNotice(this.errorMessage(response));
            }
            this.publish();
            queueMicrotask(() => this.dispatchNextPrompt(dispatch.threadId));
        });
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
        if (type === "thread.removed") {
            const scope = isObject(frame.scope) ? frame.scope : {};
            const threadId = stringMember(scope, "threadId");
            this.prompts.clearThread(threadId);
            if (this.selectedThreadId === threadId) this.selectedThreadId = "";
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
        const projectionId = this.selectedThreadId === "" && this.newThreadIntent ? DraftThreadId : this.selectedThreadId;
        const thread = this.model.thread(this.selectedThreadId);
        const index = indexAuthoritativeItems(projectionId, thread);
        if (thread) this.prompts.reconcile(this.selectedThreadId, index, Date.now());
        return {
            revision: this.revision, selectedThreadId: this.selectedThreadId, newThreadIntent: this.newThreadIntent,
            conversation: projectConversation(index, this.prompts.submissions(projectionId), DefaultAuthoritativeItemLimit, Date.now(), thread),
            protocolFrames: this.protocolFrames, notice: this.notice, bridgeUrl: this.bridgeUrl,
        };
    }
}
