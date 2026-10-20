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
import type {PendingRequestPresentation, WorkspaceGroup} from "../presentation/PresentationModel.js";
import {ProtocolNormalizer} from "../presentation/ProtocolNormalizer.js";
import {PromptCoordinator, indexAuthoritativeItems, localCardVisible, promptWithFileLinks} from "../conversation/PromptCoordinator.js";
import type {AttachmentDraft, PromptDispatch, PromptRecoveryContext} from "../conversation/PromptCoordinator.js";
import {DefaultAuthoritativeItemLimit, projectConversation} from "../conversation/ConversationProjection.js";
import {PendingAnimationDelayMilliseconds, stableKey} from "../conversation/MiddleTypes.js";
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
    "thread.workspace": "thread/settings/update", "thread.resume": "thread/resume", "thread.fork": "thread/fork", "thread.rename": "thread/name/set",
    "thread.turns.list": "thread/turns/list", "thread.items.list": "thread/items/list",
    "thread.revert": "thread/revert", "thread.archive": "thread/archive", "thread.unarchive": "thread/unarchive", "thread.delete": "thread/delete",
    "projects.list": "project/list", "project.read": "project/read", "project.create": "project/create",
    "project.update": "project/update", "project.move": "project/move", "project.delete": "project/delete",
    "sections.list": "threadSection/list", "section.create": "threadSection/create",
    "section.update": "threadSection/update", "section.delete": "threadSection/delete",
    "thread.metadata": "thread/metadata/update", "thread.section": "thread/section/move",
    "account.usage.read": "account/usage/read",
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
export interface NewThreadDraft extends PromptRecoveryContext {}

export type ThreadSortCriterion = "alphanumeric" | "created" | "recent" | "section";
export interface ThreadBrowserOptions {
    grouping: "projects" | "sections" | "ungrouped"; search: string;
    archive: "active" | "archived" | "all"; sort: ThreadSortCriterion; manualProjects: boolean;
}
export interface BrowserPage {
    action: "threads.list" | "projects.list" | "sections.list"; parameters: JsonObject;
    ids: string[]; nextCursor: string; pending: boolean; error: string;
    seen: Set<string>; loadMore: boolean; repairPending: boolean; startedAtSequence: number;
}
function pageKey(action: string, parameters: JsonObject): string {
    return action + JSON.stringify(Object.entries(parameters).sort(([a], [b]) => a.localeCompare(b)));
}

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
    itemRequests: number;
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
    readonly browser: ThreadBrowserOptions = {grouping: "projects", search: "", archive: "active", sort: "recent", manualProjects: false};
    private readonly pages = new Map<string, BrowserPage>();
    private readonly catalogReads = new Set<BrowserPage>();
    private readonly catalogQueue: {page: BrowserPage; cursor: string; repair: boolean}[] = [];
    private readonly unsupportedCatalogs = new Set<string>();
    private readonly groupExpansion = new Map<string, boolean>();
    private threadListRepairTimer: ReturnType<typeof setTimeout> | undefined;
    private bridgeUrl: string;
    private readonly createWebSocket: WebSocketFactory | undefined;
    private snapshot: BrowserSessionSnapshot;

    constructor(bridgeUrl = BrowserFrontendSession.defaultBridgeUrl(), createWebSocket?: WebSocketFactory) {
        this.bridgeUrl = bridgeUrl;
        try {
            const stored = JSON.parse(readBrowserStorage(`codexui.browser:${bridgeUrl}`) ?? "{}");
            if (["projects", "sections", "ungrouped"].includes(stored.grouping)) this.browser.grouping = stored.grouping;
            if (["recent", "created", "alphanumeric", "section"].includes(stored.sort)) this.browser.sort = stored.sort;
            this.browser.manualProjects = stored.manualProjects === true;
            if (Array.isArray(stored.expansion)) for (const entry of stored.expansion)
                if (Array.isArray(entry) && typeof entry[0] === "string" && typeof entry[1] === "boolean") this.groupExpansion.set(entry[0], entry[1]);
        } catch { /* Invalid preferences use canonical defaults. */ }
        this.createWebSocket = createWebSocket;
        this.normalizer = new ProtocolNormalizer(frame => {
            const scope = isObject(frame.scope) ? frame.scope : {};
            const retiringRequest = stringMember(frame, "type") === "pending-request.removed"
                && Object.hasOwn(scope, "requestId")
                ? this.model.pendingRequestPresentations().get(JSON.stringify(scope.requestId)) : undefined;
            const historyReplaced = frame.kind === "result" && frame.ok === true
                && (frame.action === "thread.revert" || (frame.action === "thread.turns.list"
                    && this.model.thread(stringMember(scope, "threadId"))?.raw.historyStale === true));
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
            if (historyReplaced) {
                const index = indexAuthoritativeItems(threadId, this.model.thread(threadId));
                this.prompts.retainHistoryAliases(threadId, index);
                this.prompts.decorate(threadId, index);
                const retained = new Set(index.ordered.map(item => stableKey(item.promptAlias?.key ?? item.key)));
                for (const prompt of this.prompts.submissions(threadId)) {
                    if (localCardVisible(prompt)) { retained.add(stableKey({kind: "prompt", submissionId: prompt.id})); }
                }
                this.viewportState.retainCards(threadId, this.threadVisualKey(threadId), retained);
            }
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
        this.pages.clear(); this.catalogQueue.length = 0; this.catalogReads.clear();
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
                || (this.matchesThreadPresentation(this.selectedThreadId, presentationKey)
                    && !this.operationPending("thread.revert", this.selectedThreadId)
                    && this.model.thread(this.selectedThreadId)?.raw.historyStale !== true));
    }

    selectThread(threadId: string, presentationKey?: string, refreshSettings = true): void {
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
        if (threadId !== "") {
            this.ensureThreadHydrated(threadId, refreshSettings);
        }
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
            ...(draft.projectId ? {projectId: draft.projectId} : {}),
            ...(draft.sectionId ? {sectionId: draft.sectionId} : {}),
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
        const eligible = this.browserThreadIds();
        const present = new Set(eligible);
        const order = eligible.filter(id => {
            const parent = this.model.childOwnership(id)?.parentThreadId;
            if (!parent || !present.has(parent)) return true;
            return this.browser.grouping !== "ungrouped" && ["projectId", "sectionId"].some(key =>
                this.model.thread(id)?.[key as "projectId" | "sectionId"] !== this.model.thread(parent)?.[key as "projectId" | "sectionId"]);
        });
        if (criterion === "section") return order;
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
        if (this.operationPending("thread.revert", this.selectedThreadId)
            || this.model.thread(this.selectedThreadId)?.raw.historyStale === true) {
            this.setNotice("History removal is pending; wait before sending.");
            return false;
        }
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
            activeTurnId, admittedAt, activeTurnId === undefined ? this.nextPromptActivityAt(admittedAt) : undefined,
            (destination === DraftThreadId ? this.newThreadDraft : undefined) ?? {workspace: thread?.cwd || "", name: "Recovered prompt",
                baseInstructions: stringMember(threadOptions, "baseInstructions"), developerInstructions: stringMember(threadOptions, "developerInstructions"), ephemeral: threadOptions.ephemeral === true,
                ...(thread?.projectId ? {projectId: thread?.projectId} : {}),
                ...(thread?.sectionId ? {sectionId: thread?.sectionId} : {})});
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
            if (threadDraft?.projectId) createOptions.projectId = threadDraft.projectId;
            if (threadDraft) { createOptions.cwd = threadDraft.workspace; }
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
            if (threadDraft?.sectionId) await this.assignThreadGroup(id, "section", threadDraft.sectionId);
            this.publish();
        }
        queueMicrotask(() => this.dispatchNextPrompt(destination));
        return true;
    }

    restorePrompt(threadId: string, submissionId: number, presentationKey: string, currentDraft: string): string | undefined {
        const submission = this.prompts.submission(threadId, submissionId);
        if (!this.matchesThreadPresentation(threadId, presentationKey) || !submission || submission.state !== "failed") return;
        if (currentDraft !== "") { this.setNotice("Clear or send the current draft before restoring this prompt. The preserved prompt was not changed.", false); return; }
        const text = submission.prompt;
        if (this.newThreadCreationInFlight) { this.setNotice("The current new thread is still being created.", false); return; }
        this.beginNewThread(submission.recoveryContext);
        this.setNotice("The unsent prompt was restored to a new-thread draft.", false);
        return text;
    }
    interrupt(presentationKey?: string): void {
        const threadId = this.selectedThreadId;
        const targetKey = presentationKey ?? this.threadVisualKey(threadId);
        if (targetKey === "" || !this.matchesThreadPresentation(threadId, targetKey)) return;
        const turnId = this.model.activeTurnId(threadId);
        if (turnId) this.request("turn.interrupt", {threadId, turnId}, undefined,
            () => this.matchesThreadPresentation(threadId, targetKey));
    }
    operationPending(action: string, threadId = "", presentationKey?: string, currentOperation?: object): boolean {
        const pending = this.pendingUserOperations.get(`${action}:${presentationKey ?? this.threadVisualKey(threadId)}`);
        return pending !== undefined && pending !== currentOperation;
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
    workspaceChangeUnavailable(threadId: string, presentationKey?: string, currentOperation?: object): string {
        const thread = this.model.thread(threadId);
        let reason = "";
        if (!thread || !this.matchesThreadPresentation(threadId, presentationKey)) {
            reason = "An existing thread is required.";
        } else if (!this.canSubmit()) {
            reason = "Workspace changes require a ready provider and controller access.";
        } else if (thread.status.semantic === "running" || this.model.activeTurnId(threadId) || this.prompts.hasInFlight(threadId)) {
            reason = "Wait until the active turn completes before changing workspace.";
        } else if (["hydrating", "resuming"].includes(this.runtimeByThread.get(threadId)?.phase ?? "")) {
            reason = "Thread context is loading; wait for app-server confirmation.";
        } else if (this.operationPending("thread.workspace", threadId, presentationKey, currentOperation)) {
            reason = "Workspace change pending app-server confirmation.";
        }
        return reason;
    }
    changeWorkspace(threadId: string, workspace: string, presentationKey?: string): void {
        const reason = this.workspaceChangeUnavailable(threadId, presentationKey);
        if (reason !== "") {
            this.setNotice(reason);
        } else if (workspace.trim() === "") {
            this.setNotice("Enter the app-server workspace path.");
        } else if (workspace.trim() !== this.model.thread(threadId)?.cwd) {
            void this.performUserOperation("thread.workspace", "thread.workspace", {threadId, cwd: workspace.trim()},
                "Change workspace", presentationKey);
        }
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
                // The fork response already supplies current execution settings.
                this.selectThread(id, undefined, false);
                this.loadForkHistory(id, runtime);
            }
            else if (response.ok) this.setNotice("Fork thread failed: no thread was returned.");
        });
    }
    archiveThread(threadId: string, archived: boolean, presentationKey?: string): void {
        void this.performUserOperation("thread.archive", archived ? "thread.unarchive" : "thread.archive",
            {threadId}, archived ? "Unarchive thread" : "Archive thread", presentationKey);
    }
    revertUnavailable(threadId: string, turnId: string, presentationKey?: string,
        expectedTurn: object | undefined = this.model.thread(threadId)?.turns.get(turnId), currentOperation?: object): string {
        let reason = "";
        const thread = this.model.thread(threadId);
        const runtime = this.runtimeByThread.get(threadId);
        if (!thread || !expectedTurn || thread.turns.get(turnId) !== expectedTurn
            || !this.matchesThreadPresentation(threadId, presentationKey)) {
            reason = "The selected turn is no longer available.";
        } else if (!this.canSubmit()) {
            reason = "Removing turns requires a ready provider and controller access.";
        } else if (thread.raw.ephemeral === true || thread.raw.historyMode !== "paginated") {
            reason = "Only persistent threads with paginated history support removing turns.";
        } else if (thread.status.semantic === "running" || this.model.activeTurnId(threadId)) {
            reason = "Wait until the active turn completes before removing turns.";
        } else if (!runtime || runtime.phase !== "ready" || runtime.historyPageInFlight || runtime.itemRequests !== 0
            || this.itemPageQueue.some(page => page.threadId === threadId && page.runtime === runtime)
            || (thread.raw.historyStale === true && currentOperation === undefined)
            || [...this.pendingUserOperations].some(([key, operation]) => key.endsWith(`:${this.threadVisualKey(threadId)}`) && operation !== currentOperation)) {
            reason = "Wait for thread operations and history loading to finish.";
        } else if (this.prompts.submissions(threadId).some(prompt => localCardVisible(prompt) && prompt.state !== "failed")) {
            reason = "Wait for pending prompts to finish before removing turns.";
        }
        return reason;
    }
    removeTurns(threadId: string, turnId: string, presentationKey: string, expectedTurn: object): void {
        const reason = this.revertUnavailable(threadId, turnId, presentationKey, expectedTurn);
        if (reason !== "") {
            this.setNotice(reason);
        } else {
            this.performUserOperation("thread.revert", "thread.revert", {threadId, beforeTurnId: turnId},
                "History removal", presentationKey)?.then(response => {
                if (response.ok && !response.stale) {
                    this.runtimeByThread.delete(threadId);
                    const runtime = this.threadRuntime(threadId);
                    runtime.phase = "ready";
                    const cursor = this.model.thread(threadId)?.historyNextCursor ?? "";
                    if (cursor !== "") { this.loadMoreHistory(threadId, presentationKey); }
                    this.publish();
                }
            });
        }
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
            runtime = {phase: "cold", historyPageInFlight: false, itemRequests: 0, seenHistoryCursors: new Set()};
            this.runtimeByThread.set(threadId, runtime);
        }
        return runtime;
    }
    private invalidateProviderWork(): void {
        ++this.lifecycleEpoch;
        if (this.threadListRepairTimer) clearTimeout(this.threadListRepairTimer);
        this.threadListRepairTimer = undefined;
        this.pages.clear(); this.catalogQueue.length = 0; this.catalogReads.clear();
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
            this.failPendingPrompts(threadId, "Provider changed");
        }
        this.runtimeByThread.clear();
    }
    private failPendingPrompts(threadId: string, context: string): void {
        for (const submission of this.prompts.submissions(threadId)) {
            if (submission.state === "queued" || submission.state === "inFlight") {
                this.prompts.fail(threadId, submission.id, submission.state === "inFlight"
                    ? `${context} before the turn was acknowledged; its outcome is unknown`
                    : `${context} before this queued prompt was sent`);
                this.cancelPendingAnimation(submission.id);
            }
        }
    }
    private retireProviderPresentations(): void {
        const retainedDraftId = this.newThreadDraft ? DraftThreadId : "";
        for (const identity of this.viewportState.retireExcept(retainedDraftId))
            this.settingDrafts.delete(identity);
    }
    private ensureThreadHydrated(threadId: string, refreshSettings = false): void {
        if (!this.providerReady() || threadId === "" || threadId === DraftThreadId) return;
        const runtime = this.threadRuntime(threadId);
        if (runtime.phase === "cold" || (refreshSettings && runtime.phase === "ready")) {
            void this.hydrateThread(threadId, true);
        }
    }
    private async hydrateThread(threadId: string, interactive: boolean, forced = false): Promise<void> {
        if (!this.providerReady() || threadId === "") return;
        const runtime = this.threadRuntime(threadId);
        if (runtime.phase === "hydrating" || runtime.phase === "resuming" || runtime.historyPageInFlight) return;
        const historyReady = !forced && runtime.phase === "ready";
        if (!forced && runtime.phase !== "cold" && !historyReady) {
            return;
        }
        const phase = historyReady ? "resuming" : "hydrating";
        runtime.phase = phase;
        runtime.historyPageInFlight = !historyReady;
        if (!historyReady) {
            runtime.seenHistoryCursors.clear();
        }
        this.schedulePublish();
        const current = () => this.runtimeByThread.get(threadId) === runtime && runtime.phase === phase;
        let resumeSucceeded = false;
        const resumeAttempted = interactive && this.sdk.role === "controller";
        if (resumeAttempted) {
            const response = await this.requestPromise("thread.resume", {threadId, excludeTurns: true}, current);
            if (response.stale || !current()) return;
            resumeSucceeded = response.ok;
            if (!response.ok) this.setNotice(`Thread settings refresh failed: ${this.errorMessage(response)}`);
        }
        if (!historyReady) {
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
            this.queueTurnItems(threadId, response.data, runtime);
            this.queueActiveAgentChildren(threadId);
        }
        runtime.phase = "ready";
        if ((interactive || this.selectedThreadId === threadId) && this.sdk.role === "controller" && !resumeAttempted) {
            await this.hydrateThread(threadId, true);
        } else {
            this.publish();
            queueMicrotask(() => this.dispatchNextPrompt(threadId, resumeSucceeded));
        }
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
            ++page.runtime.itemRequests;
            void this.requestPromise("thread.items.list", {
                threadId: page.threadId, turnId: page.turnId, limit: 80, sortDirection: "desc",
                ...(page.cursor === "" ? {} : {cursor: page.cursor}),
            }, () => this.runtimeByThread.get(page.threadId) === page.runtime
                && page.runtime.phase !== "failed").then(response => {
                --this.pendingItemPages;
                --page.runtime.itemRequests;
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
            if (this.model.thread(threadId)?.raw.historyStale === true) { this.loadMoreHistory(threadId); }
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
            if (this.model.thread(threadId)?.raw.historyStale === true) { this.loadMoreHistory(threadId); }
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
            this.request("account.usage.read", {});
            this.refreshGroups();
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
    private persistBrowser(): void {
        writeBrowserStorage(`codexui.browser:${this.bridgeUrl}`, JSON.stringify({grouping: this.browser.grouping, sort: this.browser.sort, manualProjects: this.browser.manualProjects, expansion: [...this.groupExpansion]}));
    }
    changeBrowser(change: Partial<ThreadBrowserOptions>): void {
        const queryChanged = change.search !== undefined || change.archive !== undefined || change.sort !== undefined;
        const catalogChanged = change.manualProjects !== undefined;
        Object.assign(this.browser, change); this.persistBrowser();
        if (queryChanged) {
            if (this.threadListRepairTimer) clearTimeout(this.threadListRepairTimer);
            this.threadListRepairTimer = undefined;
            for (const [key, page] of this.pages) if (page.action === "threads.list") this.retirePage(key, page);
            this.requestInitialThreadPage();
        }
        if (catalogChanged) {
            for (const [key, page] of this.pages) if (page.action === "projects.list") this.retirePage(key, page);
            this.refreshGroups();
        }
        this.publish();
    }
    groupExpanded(key: string): boolean { return this.groupExpansion.get(key) ?? true; }
    toggleGroup(kind: "project" | "section", id: string): void {
        const key = `${kind}:${id}`; const expanded = !this.groupExpanded(key);
        this.groupExpansion.set(key, expanded); this.persistBrowser();
        if (expanded) this.browseGroup(kind, id);
        this.publish();
    }
    private threadParameters(archived: boolean, membership: JsonObject = {}): JsonObject {
        return {sortKey: this.browser.sort === "section" ? "section_position" : "recency_at",
            sortDirection: this.browser.sort === "section" ? "asc" : "desc", limit: 100,
            ...(archived ? {archived: true} : {}), ...(this.browser.search ? {searchTerm: this.browser.search} : {}), ...membership};
    }
    threadPages(membership: JsonObject = {}): BrowserPage[] {
        return (this.browser.archive === "all" ? [false, true] : [this.browser.archive === "archived"])
            .map(archived => this.page("threads.list", this.threadParameters(archived, membership)));
    }
    private page(action: BrowserPage["action"], parameters: JsonObject): BrowserPage {
        const key = pageKey(action, parameters);
        let page = this.pages.get(key);
        if (!page) {
            page = {action, parameters, ids: [], nextCursor: "", pending: false, error: "", seen: new Set(), loadMore: false, repairPending: false, startedAtSequence: 0};
            this.pages.set(key, page);
        }
        return page;
    }
    private retirePage(key: string, page: BrowserPage): void {
        this.pages.delete(key); this.catalogReads.delete(page);
        for (let index = this.catalogQueue.length - 1; index >= 0; --index)
            if (this.catalogQueue[index]!.page === page) this.catalogQueue.splice(index, 1);
    }
    private currentPage(page: BrowserPage): boolean { return this.pages.get(pageKey(page.action, page.parameters)) === page; }
    private requestInitialThreadPage(): void {
        if (this.providerReady()) for (const page of this.threadPages()) this.readPage(page);
    }
    browseGroup(kind: "project" | "section", id: string): void {
        if (this.providerReady()) for (const page of this.threadPages({[`${kind}Id`]: id}))
            if (!page.pending && !page.ids.length && !page.seen.size && !page.error) this.readPage(page);
    }
    refreshGroups(): void {
        if (!this.providerReady()) return;
        for (const action of ["projects.list", "sections.list"] as const) {
            if (this.unsupportedCatalogs.has(action)) continue;
            const parameters: JsonObject = action === "projects.list" ? {limit: 100, sortKey: this.browser.manualProjects ? "position" : "recencyAt", sortDirection: this.browser.manualProjects ? "asc" : "desc"} : {limit: 100};
            const old = this.pages.get(pageKey(action, parameters));
            if (old?.pending) this.retirePage(pageKey(action, parameters), old);
            const page = this.page(action, parameters); if (!page.pending) { page.seen.clear(); this.readPage(page); }
        }
    }
    groups(kind: "project" | "section") { return [...(kind === "project" ? this.model.projects : this.model.sections).values()]; }
    catalogPages(): readonly BrowserPage[] { return [...this.pages.values()].filter(page => page.action !== "threads.list"); }
    refreshBrowser(): void {
        if (!this.providerReady()) return;
        for (const [key, page] of this.pages) {
            if (page.parameters.projectId && !this.model.projects.has(String(page.parameters.projectId))
                || page.parameters.sectionId && !this.model.sections.has(String(page.parameters.sectionId))) this.retirePage(key, page);
            else { page.seen.clear(); this.readPage(page); }
        }
    }
    loadMoreThreads(): void { for (const page of this.threadPages()) this.loadPage(page); }
    loadPage(page: BrowserPage): void {
        if (!this.providerReady() || !this.currentPage(page)) return;
        if (page.pending || page.repairPending) { page.loadMore = true; return; }
        if (page.error) this.readPage(page, page.nextCursor);
        else if (page.nextCursor && !page.seen.has(page.nextCursor)) this.readPage(page, page.nextCursor);
    }
    private readPage(page: BrowserPage, cursor = "", repair = false): void {
        if (!this.providerReady() || !this.currentPage(page) || page.pending || this.unsupportedCatalogs.has(page.action)) return;
        page.pending = true; page.error = "";
        this.catalogQueue.push({page, cursor, repair}); this.drainCatalogReads(); this.schedulePublish();
    }
    private drainCatalogReads(): void {
        while (this.catalogReads.size < 2) {
            const next = this.catalogQueue.findIndex(({page}) => this.currentPage(page)
                && (page.action === "threads.list" || ![...this.catalogReads].some(read => read.action !== "threads.list")));
            if (next < 0) return;
            const {page, cursor, repair} = this.catalogQueue.splice(next, 1)[0]!;
            this.catalogReads.add(page);
            const parameters = {...page.parameters, ...(page.action === "threads.list" ? {useStateDbOnly: !repair} : {}), ...(cursor ? {cursor} : {})};
            const epoch = this.lifecycleEpoch;
            if (!cursor) page.startedAtSequence = this.normalizer.sequence;
            void this.requestPromise(page.action, parameters, () => this.currentPage(page)).then(response => {
                this.catalogReads.delete(page);
                if (epoch !== this.lifecycleEpoch || !this.currentPage(page)) { this.drainCatalogReads(); return; }
                page.pending = false;
                if (!response.ok) {
                    page.error = this.errorMessage(response);
                    if (isObject(response.error) && response.error.code === -32601 && page.action !== "threads.list") this.unsupportedCatalogs.add(page.action);
                } else {
                    const data = isObject(response.data) && Array.isArray(response.data.data) ? response.data.data : [];
                    const ids = data.map(raw => stringMember(raw, "id")).filter(Boolean);
                    page.ids = [...new Set(cursor || repair ? [...page.ids, ...ids] : ids)];
                    page.seen.add(cursor);
                    page.nextCursor = stringMember(response.data, "nextCursor");
                    if (page.nextCursor && page.seen.has(page.nextCursor)) page.nextCursor = "";
                    if (!page.nextCursor && page.action !== "threads.list") this.model.finishGroupCatalog(page.action === "projects.list" ? "project" : "section", page.ids, page.startedAtSequence);
                }
                const general = page.action === "threads.list" && !page.parameters.projectId && !page.parameters.sectionId && !page.parameters.archived;
                if (general && !cursor && !repair && !this.threadListRepairTimer) {
                    page.repairPending = true;
                    this.threadListRepairTimer = setTimeout(() => {
                        this.threadListRepairTimer = undefined;
                        if (this.currentPage(page)) { page.repairPending = false; this.readPage(page, "", true); }
                    }, 16);
                }
                if (page.loadMore && !page.repairPending) { page.loadMore = false; this.loadPage(page); }
                this.publish(); this.drainCatalogReads();
            });
        }
    }
    browserThreadIds(): string[] {
        const pages = this.threadPages();
        const ids = this.browser.search || this.browser.sort === "section" ? pages.flatMap(page => page.ids) : [...this.model.threadIds()];
        if (this.browser.sort === "section") for (const page of this.pages.values()) if (page.action === "threads.list" && page.parameters.sectionId) ids.push(...page.ids);
        return [...new Set(ids)].filter(id => {
            const thread = this.model.thread(id);
            return thread && (this.browser.archive === "all" || thread.archived === (this.browser.archive === "archived"));
        });
    }
    groupingAvailable(): boolean { return !this.unsupportedCatalogs.size; }
    async manageGroup(action: Extract<Action, `project.${string}` | `section.${string}`>, parameters: JsonObject, target?: WorkspaceGroup): Promise<boolean> {
        if (!this.canSubmit()) return false;
        const key = `${action}:${stringMember(parameters, "projectId") || stringMember(parameters, "sectionId")}`;
        if (this.pendingUserOperations.has(key)) return false;
        const groups = action.startsWith("project.") ? this.model.projects : this.model.sections;
        const id = stringMember(parameters, "projectId") || stringMember(parameters, "sectionId");
        const expected = target ?? groups.get(id);
        const current = () => !expected || groups.get(id) === expected;
        if (!current()) return false;
        const identity = {}; this.pendingUserOperations.set(key, identity); this.publish();
        const epoch = this.lifecycleEpoch;
        try {
            if (action === "project.update") {
                const fresh = await this.requestPromise("project.read", {projectId: parameters.projectId}, current);
                if (epoch !== this.lifecycleEpoch || !current() || !this.canSubmit() || !fresh.ok) {
                    if (!fresh.stale) this.setNotice(`Project update failed: ${this.errorMessage(fresh)}`); return false;
                }
                const metadata = member(member(fresh.data, "project", {}), "metadata", {});
                const {description, ...update} = parameters;
                parameters = {...update, metadata: {...(isObject(metadata) ? metadata : {}), "codexui.description": description}};
            }
            const response = await this.requestPromise(action, parameters, current);
            if (response.stale || epoch !== this.lifecycleEpoch) return false;
            if (!response.ok) { this.setNotice(this.errorMessage(response)); return false; }
            this.refreshBrowser(); return true;
        } finally {
            if (this.pendingUserOperations.get(key) === identity) this.pendingUserOperations.delete(key);
            this.schedulePublish();
        }
    }
    async assignThreadGroup(threadId: string, kind: "project" | "section", id: string, beforeThreadId?: string): Promise<boolean> {
        const action = kind === "project" ? "thread.metadata" : "thread.section";
        const response = await this.performUserOperation(action, action, {threadId, [`${kind}Id`]: id || (kind === "project" ? "" : null),
            ...(beforeThreadId ? {beforeThreadId} : {})}, `Assign ${kind}`);
        if (!response?.ok || response.stale) return false;
        if (kind === "section") {
            const refresh = await this.requestPromise("thread.read", {threadId, includeTurns: false});
            if (refresh.stale) return false;
            if (!refresh.ok) this.setNotice(`Section changed, but metadata refresh failed: ${this.errorMessage(refresh)}`);
        }
        this.refreshBrowser(); return true;
    }
    private performUserOperation(keyAction: Action, action: Action, parameters: JsonObject,
        failureContext: string, presentationKey?: string): Promise<OperationResponse> | undefined {
        const threadId = stringMember(parameters, "threadId");
        const targetKey = presentationKey ?? this.threadVisualKey(threadId);
        if (targetKey === "" || !this.matchesThreadPresentation(threadId, targetKey)) return undefined;
        const key = `${keyAction}:${targetKey}`;
        if (this.pendingUserOperations.has(key) || this.operationPending("thread.revert", threadId)) return undefined;
        if (!this.canSubmit()) {
            this.setNotice(`${failureContext} is unavailable until Codex is ready and controlled.`);
            return undefined;
        }
        const operationIdentity: {revertObserved?: "result" | "notification"} = {};
        this.pendingUserOperations.set(key, operationIdentity);
        if (keyAction === "thread.revert") {
            const thread = this.model.thread(threadId);
            if (thread) { thread.raw.historyStale = true; }
        }
        this.publish();
        const expectedTurn = this.model.thread(threadId)?.turns.get(stringMember(parameters, "beforeTurnId"));
        const current = () => this.matchesThreadPresentation(threadId, targetKey)
            && (keyAction !== "thread.revert" || this.model.thread(threadId)?.turns.get(stringMember(parameters, "beforeTurnId")) === expectedTurn);
        const operation = (keyAction === "thread.workspace" || keyAction === "thread.revert") && this.model.thread(threadId)?.status.semantic === "notLoaded"
            ? this.requestPromise("thread.resume", {threadId, excludeTurns: true}, current).then(response => {
                let result: OperationResponse | Promise<OperationResponse> = response;
                if (response.ok && !response.stale) {
                    const reason = keyAction === "thread.revert"
                        ? this.revertUnavailable(threadId, stringMember(parameters, "beforeTurnId"), targetKey, expectedTurn, operationIdentity)
                        : this.workspaceChangeUnavailable(threadId, targetKey, operationIdentity);
                    result = reason === "" ? this.requestPromise(action, parameters, current)
                        : {ok: false, error: {message: reason}};
                }
                return result;
            })
            : this.requestPromise(action, parameters, current);
        void operation.then(response => {
            const notificationReceived = operationIdentity.revertObserved === "notification";
            if (keyAction === "thread.revert") {
                if (response.ok && !notificationReceived) { operationIdentity.revertObserved = "result"; }
                else if (!response.stale && !notificationReceived
                    && member(response.error, "code") !== -32002 && member(response.error, "code") !== -32020) {
                    const thread = this.model.thread(threadId);
                    if (thread) { delete thread.raw.historyStale; }
                }
            }
            if (this.pendingUserOperations.get(key) === operationIdentity
                && (!response.ok || (keyAction !== "thread.workspace"
                    && (keyAction !== "thread.revert" || notificationReceived)))) {
                this.pendingUserOperations.delete(key);
            }
            if (!response.ok && !response.stale) {
                const recovery = keyAction === "thread.revert" ? " Reload this thread before retrying if the connection was lost." : "";
                this.setNotice(`${failureContext} failed: ${this.errorMessage(response)}${recovery}`);
                if (keyAction === "thread.revert" && notificationReceived && this.providerReady()) {
                    this.runtimeByThread.delete(threadId);
                    const runtime = this.threadRuntime(threadId);
                    runtime.phase = "ready";
                    this.loadForkHistory(threadId, runtime);
                }
            } else {
                this.schedulePublish();
            }
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
            this.unsupportedCatalogs.clear();
            this.invalidateProviderWork();
            this.retireProviderPresentations();
        }
        if (requestsReset) this.resolvingRequests.clear();
        if (type === "connection.lifecycle") {
            const state = stringMember(data, "state");
            if (state === "connecting") this.unsupportedCatalogs.clear();
            if (["connecting", "retrying", "disconnected", "failure"].includes(state)) this.invalidateProviderWork();
        } else if (type === "connection.provider") {
            const state = stringMember(data, "state");
            if (state === "ready") this.hydrateCatalogs();
            else if (!providerReset) this.invalidateProviderWork();
        } else if (type === "connection.bridge" || type === "connection.controller") {
            if (this.providerReady()) queueMicrotask(() => {
                if (this.selectedThreadId !== "" && this.selectedThreadId !== DraftThreadId)
                    this.ensureThreadHydrated(this.selectedThreadId, true);
                this.dispatchQueuedPrompts();
            });
        } else if (type === "workspace.project.changed" || type === "thread.project.changed") {
            this.refreshGroups();
            this.refreshBrowser();
        } else if (type === "thread.settings.changed") {
            const threadId = stringMember(frame.scope, "threadId");
            const settings = isObject(data.threadSettings) ? data.threadSettings : data;
            if (typeof settings.cwd === "string") {
                this.pendingUserOperations.delete(`thread.workspace:${this.threadVisualKey(threadId)}`);
            }
        } else if (type === "thread.reverted") {
            const threadId = stringMember(frame.scope, "threadId");
            this.failPendingPrompts(threadId, "Conversation history changed");
            const key = `thread.revert:${this.threadVisualKey(threadId)}`;
            const own = this.pendingUserOperations.get(key) as {revertObserved?: "result" | "notification"} | undefined;
            if (own) {
                if (own.revertObserved === "result") {
                    this.pendingUserOperations.delete(key);
                    const thread = this.model.thread(threadId);
                    if (thread) {
                        delete thread.raw.historyStale;
                        delete thread.raw.historyReload;
                    }
                } else {
                    own.revertObserved = "notification";
                    this.runtimeByThread.delete(threadId);
                }
            } else {
                this.runtimeByThread.delete(threadId);
                const runtime = this.threadRuntime(threadId);
                runtime.phase = "ready";
                this.loadForkHistory(threadId, runtime);
            }
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
