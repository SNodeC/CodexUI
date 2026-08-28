import {
    classifyJsonRpc,
    jsonRpcMethod,
    serverNotificationOperations,
    serverRequestOperations,
} from "@snodec/codex-frontend";

import {
    event,
    isObject,
    member,
    result,
    stringMember,
} from "./PresentationProtocol.js";
import type {
    Authority,
    JsonObject,
    PresentationFrame,
} from "./PresentationProtocol.js";

export type PresentationSink = (frame: PresentationFrame) => boolean;

const stableScopeKeys = [
    "threadId",
    "turnId",
    "itemId",
    "processId",
    "requestId",
] as const;

function stableScope(value: unknown): JsonObject {
    const scope: JsonObject = {};
    if (!isObject(value)) return scope;
    for (const key of stableScopeKeys) {
        if (Object.hasOwn(value, key) && value[key] !== null) scope[key] = value[key];
    }
    return scope;
}

function errorValue(response: JsonObject): unknown {
    return Object.hasOwn(response, "error")
        ? response.error
        : {code: -32000, message: "operation failed"};
}

function requestKind(method: string): string {
    const categories: Readonly<Record<string, string>> = {
        "item/commandExecution/requestApproval": "command-approval",
        "item/fileChange/requestApproval": "file-change-approval",
        "item/tool/requestUserInput": "user-input",
        "mcpServer/elicitation/request": "mcp-elicitation",
        "item/permissions/requestApproval": "permissions-approval",
        "item/tool/call": "dynamic-tool-call",
        "account/chatgptAuthTokens/refresh": "authentication-refresh",
        "attestation/generate": "attestation",
        applyPatchApproval: "legacy-patch-approval",
        execCommandApproval: "legacy-command-approval",
    };
    return categories[method] ?? "unsupported";
}

interface EventDescriptor {
    readonly type: string;
    readonly authority: Authority;
}

const remainingNotifications: Readonly<Record<string, EventDescriptor>> = {
    "thread/reverted": {type: "thread.reverted", authority: "merge"},
    "skills/changed": {type: "catalog.skills.invalidated", authority: "none"},
    "thread/goal/updated": {type: "thread.goal.changed", authority: "replace"},
    "thread/goal/cleared": {type: "thread.goal.removed", authority: "remove"},
    "thread/queue/changed": {type: "thread.queue.changed", authority: "replace"},
    "project/changed": {type: "workspace.project.changed", authority: "merge"},
    "thread/project/updated": {type: "thread.project.changed", authority: "replace"},
    "thread/environment/connected": {type: "thread.environment.connected", authority: "merge"},
    "thread/environment/disconnected": {type: "thread.environment.disconnected", authority: "merge"},
    "thread/settings/updated": {type: "thread.settings.changed", authority: "merge"},
    "hook/started": {type: "activity.hook.started", authority: "merge"},
    "hook/completed": {type: "activity.hook.completed", authority: "merge"},
    "turn/diff/updated": {type: "turn.diff.changed", authority: "replace"},
    "item/autoApprovalReview/started": {type: "approval.review.started", authority: "merge"},
    "item/autoApprovalReview/completed": {type: "approval.review.completed", authority: "merge"},
    "autoApprovalReview/strictReviewRequired": {type: "approval.strict-review.required", authority: "merge"},
    "command/exec/outputDelta": {type: "terminal.command.output-appended", authority: "merge"},
    "process/outputDelta": {type: "terminal.process.output-appended", authority: "merge"},
    "process/exited": {type: "terminal.process.completed", authority: "merge"},
    "item/commandExecution/terminalInteraction": {type: "conversation.command.interaction", authority: "merge"},
    "item/fileChange/outputDelta": {type: "conversation.file-change.output-appended", authority: "merge"},
    "item/fileChange/patchUpdated": {type: "conversation.file-change.patch-replaced", authority: "replace"},
    "item/mcpToolCall/progress": {type: "conversation.mcp.progress", authority: "merge"},
    "mcpServer/oauthLogin/completed": {type: "integration.mcp.login-completed", authority: "merge"},
    "mcpServer/startupStatus/updated": {type: "integration.mcp.status-changed", authority: "merge"},
    "mcpServer/event/stream/notification": {type: "integration.mcp.event", authority: "none"},
    "app/list/updated": {type: "catalog.apps.changed", authority: "replace"},
    "remoteControl/status/changed": {type: "connection.remote-control.changed", authority: "replace"},
    "externalAgentConfig/import/progress": {type: "settings.external-agent-import.progress", authority: "merge"},
    "externalAgentConfig/import/completed": {type: "settings.external-agent-import.completed", authority: "merge"},
    "fs/changed": {type: "workspace.files.changed", authority: "merge"},
    "item/reasoning/summaryPartAdded": {type: "conversation.reasoning.part-added", authority: "merge"},
    "thread/compacted": {type: "thread.compacted", authority: "merge"},
    "model/rerouted": {type: "model.rerouted", authority: "merge"},
    "model/verification": {type: "model.verification.changed", authority: "merge"},
    "turn/moderationMetadata": {type: "turn.moderation.changed", authority: "replace"},
    "model/safetyBuffering/updated": {type: "model.safety-buffering.changed", authority: "replace"},
    "fuzzyFileSearch/sessionUpdated": {type: "workspace.search.changed", authority: "merge"},
    "fuzzyFileSearch/sessionCompleted": {type: "workspace.search.completed", authority: "merge"},
    "thread/realtime/started": {type: "realtime.session.started", authority: "merge"},
    "thread/realtime/itemAdded": {type: "realtime.item.added", authority: "merge"},
    "thread/realtime/transcript/delta": {type: "realtime.transcript.appended", authority: "merge"},
    "thread/realtime/transcript/done": {type: "realtime.transcript.completed", authority: "merge"},
    "thread/realtime/outputAudio/delta": {type: "realtime.audio.appended", authority: "merge"},
    "thread/realtime/sdp": {type: "realtime.session-description.changed", authority: "replace"},
    "thread/realtime/error": {type: "realtime.session.failed", authority: "merge"},
    "thread/realtime/closed": {type: "realtime.session.closed", authority: "merge"},
    "windows/worldWritableWarning": {type: "system.windows-permission.warning", authority: "none"},
    "windowsSandbox/setupCompleted": {type: "system.windows-sandbox.completed", authority: "merge"},
    "account/login/completed": {type: "account.login.completed", authority: "merge"},
};

export class ProtocolNormalizer {
    private deliveryFailureHandler: (() => void) | undefined;
    private deliveryFailed = false;
    private connectionGeneration = 0;
    private nextSequence = 1;

    public constructor(private readonly sink: PresentationSink) {}

    public setDeliveryFailureHandler(handler?: () => void): void {
        this.deliveryFailureHandler = handler;
    }

    public transportEvent(eventName: string, detail = ""): void {
        if (eventName === "connected") ++this.connectionGeneration;
        const data: JsonObject = {state: eventName};
        if (detail !== "") data.detail = detail;
        this.emitEvent("connection.lifecycle", data);
    }

    public connectionSettings(settings: JsonObject): void {
        this.emitEvent("connection.settings.changed", settings, "replace");
    }

    public localOperationResult(
        action: string,
        correlationId: string,
        ok: boolean,
        data: unknown,
    ): void {
        this.emit(result(
            this.nextSequence++,
            this.connectionGeneration,
            action,
            correlationId,
            ok,
            data,
        ));
    }

    public bridgeEvent(value: unknown): void {
        const kind = stringMember(value, "kind");
        if (kind === "bridge.connection") {
            this.emitEvent("connection.bridge", {
                state: stringMember(value, "event"),
                connectionId: stringMember(value, "connectionId"),
                role: stringMember(value, "role"),
            });
            return;
        }
        if (kind === "bridge.controller") {
            this.emitEvent(
                "connection.controller",
                {controllerConnectionId: member(value, "controllerConnectionId")},
                "replace",
            );
            return;
        }
        if (kind === "bridge.provider") {
            const generation = member(value, "providerGeneration");
            if (
                typeof generation !== "number"
                || !Number.isInteger(generation)
                || generation < 0
            ) {
                this.diagnostic(
                    "bridge",
                    "invalid-provider-event",
                    "provider event has no unsigned generation",
                    isObject(value) ? value : {},
                );
                return;
            }
            const data: JsonObject = {
                state: stringMember(value, "state"),
                generation,
            };
            const reason = stringMember(value, "reason");
            if (reason !== "") data.reason = reason;
            this.emitEvent("connection.provider", data, "replace");
            return;
        }
        if (kind === "bridge.diagnostic") {
            const details = member(value, "details", {});
            this.diagnostic(
                "bridge",
                stringMember(value, "code"),
                stringMember(value, "message"),
                isObject(details) ? details : {},
            );
            return;
        }
        this.diagnostic("bridge", "unknown-event", kind, isObject(value) ? value : {});
    }

    public serverNotification(method: string, params: JsonObject): void {
        const scope = stableScope(params);
        if (method === "thread/started") {
            const thread = member(params, "thread", {});
            this.emitEvent("thread.upsert", {thread: isObject(thread) ? thread : {}}, "merge");
        } else if (method === "thread/status/changed") {
            this.emitEvent("thread.status.changed", {status: member(params, "status")}, "merge", scope);
        } else if (method === "thread/name/updated") {
            this.emitEvent("thread.name.changed", {name: member(params, "threadName")}, "replace", scope);
        } else if (method === "thread/deleted") {
            this.emitEvent("thread.removed", {}, "remove", scope);
        } else if (
            method === "thread/archived"
            || method === "thread/unarchived"
            || method === "thread/closed"
        ) {
            const state = method === "thread/archived"
                ? "archived"
                : method === "thread/unarchived" ? "unarchived" : "closed";
            this.emitEvent("thread.lifecycle", {state}, "merge", scope);
        } else if (method === "turn/started" || method === "turn/completed") {
            const turn = member(params, "turn", {});
            this.emitEvent("turn.upsert", {
                lifecycle: method === "turn/started" ? "started" : "completed",
                turn: isObject(turn) ? turn : {},
            }, "merge", scope);
        } else if (method === "turn/plan/updated") {
            const steps = member(params, "plan", []);
            this.emitEvent("plan.replaced", {
                explanation: member(params, "explanation"),
                steps: Array.isArray(steps) ? steps : [],
            }, "replace", scope);
        } else if (method === "item/started" || method === "item/completed") {
            const candidate = member(params, "item", {});
            const item = isObject(candidate) ? candidate : {};
            const itemScope = {...scope};
            if (!Object.hasOwn(itemScope, "itemId") && Object.hasOwn(item, "id") && item.id !== null) {
                itemScope.itemId = item.id;
            }
            const lifecycle = method === "item/started" ? "started" : "completed";
            this.emitEvent("conversation.item.upsert", {lifecycle, item}, "merge", itemScope);
            const itemType = stringMember(item, "type");
            if (itemType === "collabAgentToolCall" || itemType === "subAgentActivity") {
                this.emitEvent("agents.activity.upsert", {lifecycle, activity: item}, "merge", itemScope);
            }
        } else if (
            method === "item/agentMessage/delta"
            || method === "item/plan/delta"
            || method === "item/reasoning/summaryTextDelta"
            || method === "item/reasoning/textDelta"
            || method === "item/commandExecution/outputDelta"
        ) {
            let field = "text";
            if (method === "item/commandExecution/outputDelta") field = "aggregatedOutput";
            else if (method === "item/reasoning/summaryTextDelta") field = "summary";
            else if (method === "item/reasoning/textDelta") field = "content";
            const data: JsonObject = {field, text: stringMember(params, "delta")};
            if (Object.hasOwn(params, "summaryIndex")) data.summaryIndex = params.summaryIndex;
            if (Object.hasOwn(params, "contentIndex")) data.contentIndex = params.contentIndex;
            this.emitEvent("conversation.item.append", data, "merge", scope);
        } else if (method === "serverRequest/resolved") {
            this.emitEvent("pending-request.removed", {}, "remove", scope);
        } else if (
            method === "error"
            || method === "warning"
            || method === "guardianWarning"
            || method === "configWarning"
            || method === "deprecationNotice"
        ) {
            this.emitEvent("notice.added", {
                severity: method === "error" ? "error" : "warning",
                notice: params,
            }, "none", scope);
        } else if (method === "thread/tokenUsage/updated") {
            const tokenUsage = member(params, "tokenUsage", {});
            this.emitEvent(
                "thread.token-usage.changed",
                {tokenUsage: isObject(tokenUsage) ? tokenUsage : {}},
                "replace",
                scope,
            );
        } else if (method === "account/updated") {
            this.emitEvent("account.changed", {account: params}, "replace");
        } else if (method === "account/rateLimits/updated") {
            this.emitEvent("account.rate-limits.changed", {rateLimits: params}, "replace");
        } else {
            const descriptor = remainingNotifications[method];
            if (descriptor !== undefined) {
                this.emitEvent(descriptor.type, params, descriptor.authority, scope);
            } else {
                this.diagnostic("appserver", "unmapped-notification", method);
            }
        }
    }

    public serverRequest(method: string, requestId: unknown, params: JsonObject): void {
        const scope = stableScope(params);
        scope.requestId = requestId;
        this.emitEvent("pending-request.upsert", {
            requestId,
            category: requestKind(method),
            request: params,
        }, "merge", scope);
    }

    public observeRawInbound(message: unknown): void {
        const method = jsonRpcMethod(message);
        const kind = classifyJsonRpc(message);
        if (
            (kind === "request" || kind === "notification")
            && method !== undefined
            && !this.knownServerMethod(method)
        ) {
            this.diagnostic("appserver", "unknown-method", method);
        }
    }

    public operationResult(
        action: string,
        correlationId: string,
        context: JsonObject,
        response: JsonObject,
        startedAtSequence?: number,
    ): void {
        const ok = Object.hasOwn(response, "result");
        let data: unknown;
        let authority: Authority = "none";
        const scope = stableScope(context);
        if (ok) {
            const rawValue = response.result;
            const value = isObject(rawValue) ? rawValue : {};
            if (action === "threads.list") {
                const threads = member(value, "data", []);
                data = {
                    threads: Array.isArray(threads) ? threads : [],
                    nextCursor: member(value, "nextCursor"),
                    backwardsCursor: member(value, "backwardsCursor"),
                };
                authority = "merge";
            } else if (action === "thread.read") {
                const candidate = member(value, "thread", {});
                const thread = isObject(candidate) ? candidate : {};
                data = {thread};
                authority = startedAtSequence !== undefined
                    && startedAtSequence === this.nextSequence
                    ? "replace"
                    : "merge";
                const threadId = stringMember(thread, "id");
                if (threadId !== "") scope.threadId = threadId;
            } else if (
                action === "thread.create"
                || action === "thread.resume"
                || action === "thread.fork"
            ) {
                const candidate = member(value, "thread", {});
                const thread: JsonObject = isObject(candidate) ? {...candidate} : {};
                for (const field of [
                    "activePermissionProfile",
                    "approvalPolicy",
                    "approvalsReviewer",
                    "cwd",
                    "model",
                    "modelProvider",
                    "reasoningEffort",
                    "sandbox",
                    "serviceTier",
                ]) {
                    if (Object.hasOwn(value, field)) thread[field] = value[field];
                }
                data = {thread};
                authority = "merge";
            } else if (action === "models.list") {
                const models = member(value, "data", []);
                data = {
                    models: Array.isArray(models) ? models : [],
                    nextCursor: member(value, "nextCursor"),
                };
                authority = "replace";
            } else if (
                action === "model-provider-capabilities.read"
                || action === "account.read"
                || action === "account.rate-limits.read"
                || action === "account.token-usage.read"
                || action === "config.read"
                || action === "permission-profiles.list"
                || action === "experimental-features.list"
                || action === "skills.list"
                || action === "hooks.list"
                || action === "plugins.list"
                || action === "apps.list"
                || action === "mcp-servers.list"
                || action.endsWith(".list")
                || action.endsWith(".read")
                || action.endsWith(".get")
                || action === "plugins.installed"
                || action === "apps.installed"
                || action === "windows-sandbox.readiness"
            ) {
                data = rawValue;
                authority = "replace";
            } else if (action === "turn.start") {
                const turn = member(value, "turn", {});
                data = {turn: isObject(turn) ? turn : {}};
                authority = "merge";
            } else {
                data = rawValue;
            }
        } else {
            data = errorValue(response);
        }
        this.emit(result(
            this.nextSequence++,
            this.connectionGeneration,
            action,
            correlationId,
            ok,
            data,
            authority,
            scope,
        ));
    }

    public operationRejected(
        action: string,
        correlationId: string,
        code: number,
        message: string,
    ): void {
        this.emit(result(
            this.nextSequence++,
            this.connectionGeneration,
            action,
            correlationId,
            false,
            {code, message},
        ));
    }

    public get sequence(): number {
        return this.nextSequence;
    }

    private emit(frame: PresentationFrame): boolean {
        if (this.deliveryFailed) return false;
        if (this.sink(frame)) return true;
        this.deliveryFailed = true;
        this.deliveryFailureHandler?.();
        return false;
    }

    private emitEvent(
        type: string,
        data: JsonObject = {},
        authority: Authority = "none",
        scope: JsonObject = {},
    ): boolean {
        return this.emit(event(
            this.nextSequence++,
            this.connectionGeneration,
            type,
            data,
            authority,
            scope,
        ));
    }

    private diagnostic(
        source: string,
        code: string,
        message: string,
        details: JsonObject = {},
    ): void {
        this.emitEvent("system.diagnostic", {source, code, message, details});
    }

    private knownServerMethod(method: string): boolean {
        return Object.hasOwn(serverRequestOperations, method)
            || Object.hasOwn(serverNotificationOperations, method);
    }
}
