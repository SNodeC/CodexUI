import type {PendingRequestPresentation} from "../presentation/PresentationModel.js";
import {isObject, member} from "../presentation/PresentationProtocol.js";

export interface PendingResponse {result?: unknown; error?: {code: number; message: string}}
export interface PendingDecisionOption {
    readonly value: string;
    readonly label: string;
    readonly tone: "approve" | "danger" | "neutral";
    readonly requiresInput?: boolean;
}
export interface PendingRequestDetail {readonly path: string; readonly value: string}
export interface PendingRequestDetails {readonly entries: readonly PendingRequestDetail[]; readonly truncated: boolean}

const error = (message: string): PendingResponse => ({error: {code: -32601, message}});
const option = (value: string, label: string, tone: PendingDecisionOption["tone"], requiresInput = false): PendingDecisionOption =>
    ({value, label, tone, ...(requiresInput ? {requiresInput: true} : {})});

const approvalOptions: Readonly<Record<string, PendingDecisionOption>> = {
    accept: option("accept", "Accept", "approve"),
    acceptForSession: option("acceptForSession", "Accept for session", "approve"),
    decline: option("decline", "Decline", "danger"),
    cancel: option("cancel", "Cancel", "neutral"),
};

export function pendingRequestDetails(request: PendingRequestPresentation): PendingRequestDetails {
    const entries: PendingRequestDetail[] = [];
    const excluded = new Set(["command", "questions"]);
    let truncated = false;
    const append = (value: unknown, path: string, depth: number): void => {
        if (entries.length >= 256 || depth > 16) { truncated = true; return; }
        if (Array.isArray(value)) {
            if (value.length === 0) entries.push({path, value: "None"});
            else value.forEach((entry, index) => append(entry, `${path} / ${index + 1}`, depth + 1));
            return;
        }
        if (isObject(value)) {
            const fields = Object.entries(value);
            if (fields.length === 0) entries.push({path, value: "None"});
            else for (const [key, entry] of fields) append(entry, path === "" ? key : `${path} / ${key}`, depth + 1);
            return;
        }
        entries.push({path: path || "value", value: value === null || value === undefined ? "None"
            : typeof value === "boolean" ? value ? "Yes" : "No" : String(value)});
    };
    if (isObject(request.raw)) for (const [key, value] of Object.entries(request.raw))
        if (!excluded.has(key)) append(value, key, 0);
    return {entries, truncated};
}

export function pendingDecisionOptions(request: PendingRequestPresentation): readonly PendingDecisionOption[] {
    if (request.kind === "command-approval") {
        const raw = isObject(request.raw) ? request.raw : {};
        const declared = Array.isArray(raw.availableDecisions) ? raw.availableDecisions.filter(value => typeof value === "string") : [];
        if (declared.length > 0) {
            const supported = declared.flatMap(value => approvalOptions[value] ? [approvalOptions[value]] : []);
            return supported.length > 0 ? supported : [option("unsupported", "Return unsupported", "neutral")];
        }
        return Object.values(approvalOptions);
    }
    if (request.kind === "file-change-approval") return Object.values(approvalOptions);
    if (request.kind === "user-input") return [
        option("decline", "Decline", "danger"), option("submit", "Submit", "approve", true),
    ];
    if (request.kind === "mcp-elicitation") return [
        option("decline", "Decline", "danger"), option("cancel", "Cancel", "neutral"),
        option("accept", "Accept", "approve", true),
    ];
    if (request.kind === "permissions-approval") return [
        option("decline", "Decline", "danger"), option("turn", "Allow this turn", "approve"),
        option("session", "Allow this session", "approve"),
    ];
    if (request.kind === "legacy-patch-approval" || request.kind === "legacy-command-approval") return [
        option("denied", "Deny", "danger"), option("abort", "Abort", "neutral"),
        option("approved", "Approve", "approve"), option("approved_for_session", "Approve for session", "approve"),
    ];
    if (request.kind === "dynamic-tool-call") return [option("unavailable", "Return unavailable", "neutral")];
    return [option("unsupported", "Return unsupported", "neutral")];
}

export function pendingResponse(request: PendingRequestPresentation, decision: string, input: unknown = {}): PendingResponse | undefined {
    if (decision === "unsupported") return error("CodexUI does not support this server request");
    if (request.kind === "command-approval" || request.kind === "file-change-approval") {
        if (!Object.hasOwn(approvalOptions, decision)) return undefined;
        return {result: {decision}};
    }
    if (request.kind === "user-input") {
        if (decision === "submit") return {result: {answers: input}};
        if (decision === "decline") return error("Request declined by user");
        return undefined;
    }
    if (request.kind === "mcp-elicitation") {
        if (decision === "accept") return {result: {action: "accept", content: isObject(input) ? input : null, _meta: null}};
        if (decision === "decline" || decision === "cancel") return {result: {action: decision, content: null, _meta: null}};
        return undefined;
    }
    if (request.kind === "permissions-approval") {
        if (decision === "turn" || decision === "session")
            return {result: {permissions: member(request.raw, "permissions", {}), scope: decision}};
        if (decision === "decline") return error("Request declined by user");
        return undefined;
    }
    if (request.kind === "legacy-patch-approval" || request.kind === "legacy-command-approval") {
        if (decision === "approved" || decision === "approved_for_session") return {result: {decision}};
        if (decision === "denied") return {result: {decision: {denied: {rejection: "Denied by user"}}}};
        if (decision === "abort") return {result: {decision: "abort"}};
        return undefined;
    }
    if (request.kind === "dynamic-tool-call" && decision === "unavailable") return {
        result: {contentItems: [{type: "inputText", text: "CodexUI does not provide this dynamic tool"}], success: false},
    };
    return undefined;
}

export function negativePendingResponse(request: PendingRequestPresentation): PendingResponse {
    const decision = request.kind === "legacy-patch-approval" || request.kind === "legacy-command-approval"
        ? "denied" : request.kind === "dynamic-tool-call" ? "unavailable" : "decline";
    return pendingResponse(request, decision) ?? error("Request declined by user");
}

export function positivePendingResponse(request: PendingRequestPresentation, input: unknown = {}): PendingResponse {
    const decision = request.kind === "user-input" ? "submit"
        : request.kind === "permissions-approval" ? "turn"
            : request.kind === "legacy-patch-approval" || request.kind === "legacy-command-approval" ? "approved"
                : request.kind === "dynamic-tool-call" ? "unavailable"
                    : request.kind === "attestation" || request.kind === "authentication-refresh" ? "unsupported" : "accept";
    return pendingResponse(request, decision, input) ?? error("CodexUI does not support this server request");
}
