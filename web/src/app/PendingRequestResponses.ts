import type {PendingRequestPresentation} from "../presentation/PresentationModel.js";
import {isObject, jsonEqual} from "../presentation/PresentationProtocol.js";

export interface PendingRequestSubmission {
    readonly choice: string;
    readonly input?: unknown;
    readonly metadata?: unknown;
}
export type PendingResponse =
    | {result: unknown; error?: never}
    | {result?: never; error: {code: number; message: string}};
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

function boundedKeys(value: Record<string, unknown>, limit: number): string[] | undefined {
    const result: string[] = [];
    for (const key in value) if (Object.hasOwn(value, key)) {
        if (result.length === limit) return undefined;
        result.push(key);
    }
    return result;
}

function utf8Prefix(value: string, maximumBytes: number): string | undefined {
    let bytes = 0;
    let end = 0;
    for (const character of value) {
        const point = character.codePointAt(0)!;
        const width = point <= 0x7f ? 1 : point <= 0x7ff ? 2 : point <= 0xffff ? 3 : 4;
        if (bytes + width > maximumBytes) return value.slice(0, end);
        bytes += width;
        end += character.length;
    }
    return undefined;
}

function knownObject(value: unknown, keys: readonly string[]): value is Record<string, unknown> {
    if (!isObject(value)) return false;
    const actual = boundedKeys(value, keys.length);
    return actual !== undefined && actual.every(key => keys.includes(key));
}

function boundedJsonEqual(left: unknown, right: unknown): boolean {
    const leftState = {remaining: 64, truncated: false};
    const rightState = {remaining: 64, truncated: false};
    const boundedLeft = boundedDisclosureValue(left, 0, leftState);
    const boundedRight = boundedDisclosureValue(right, 0, rightState);
    return !leftState.truncated && !rightState.truncated && jsonEqual(boundedLeft, boundedRight);
}

function nonEmptyString(value: unknown, key: string): boolean {
    const candidate = isObject(value) ? value[key] : undefined;
    return typeof candidate === "string" && candidate.length > 0 && utf8Prefix(candidate, 4096) === undefined;
}

function boundedStringArray(value: unknown, allowEmpty = false): value is string[] {
    return Array.isArray(value) && value.length <= 64 && (allowEmpty || value.length > 0)
        && value.every(entry => typeof entry === "string" && entry.length > 0
            && utf8Prefix(entry, 4096) === undefined);
}

function validFileSystemPath(value: unknown): boolean {
    if (!isObject(value)) return false;
    if (value.type === "path") return knownObject(value, ["path", "type"]) && nonEmptyString(value, "path");
    if (value.type === "glob_pattern")
        return knownObject(value, ["pattern", "type"]) && nonEmptyString(value, "pattern");
    if (value.type !== "special" || !knownObject(value, ["type", "value"]) || !isObject(value.value)) return false;
    const special = value.value;
    if (["root", "minimal", "tmpdir", "slash_tmp"].includes(String(special.kind)))
        return knownObject(special, ["kind"]);
    const optionalSubpath = special.subpath === undefined || special.subpath === null
        || typeof special.subpath === "string" && special.subpath.length > 0
            && utf8Prefix(special.subpath, 4096) === undefined;
    if (special.kind === "project_roots") return knownObject(special, ["kind", "subpath"]) && optionalSubpath;
    return special.kind === "unknown" && knownObject(special, ["kind", "path", "subpath"])
        && nonEmptyString(special, "path") && optionalSubpath;
}

function validFileSystemPermissions(value: unknown): boolean {
    if (!knownObject(value, ["entries", "globScanMaxDepth", "read", "write"])
        || Object.keys(value).length === 0) return false;
    let hasRequest = false;
    for (const key of ["read", "write"] as const) {
        if (value[key] === undefined || value[key] === null) continue;
        if (!boundedStringArray(value[key], true)) return false;
        hasRequest ||= value[key].length > 0;
    }
    if (value.globScanMaxDepth !== undefined && value.globScanMaxDepth !== null
        && (!Number.isSafeInteger(value.globScanMaxDepth) || Number(value.globScanMaxDepth) <= 0
            || Number(value.globScanMaxDepth) > 1_000_000)) return false;
    if (value.entries !== undefined && value.entries !== null) {
        if (!Array.isArray(value.entries) || value.entries.length > 64 || !value.entries.every(entry =>
            knownObject(entry, ["access", "path"]) && ["read", "write", "deny"].includes(String(entry.access))
            && validFileSystemPath(entry.path))) return false;
        hasRequest ||= value.entries.length > 0;
    }
    return hasRequest;
}

function validPermissionProfile(value: unknown): boolean {
    if (!knownObject(value, ["fileSystem", "network"]) || Object.keys(value).length === 0) return false;
    let hasRequest = false;
    if (value.network !== undefined && value.network !== null) {
        if (!knownObject(value.network, ["enabled"]) || Object.keys(value.network).length !== 1
            || typeof value.network.enabled !== "boolean") return false;
        hasRequest = true;
    }
    if (value.fileSystem !== undefined && value.fileSystem !== null) {
        if (!validFileSystemPermissions(value.fileSystem)) return false;
        hasRequest = true;
    }
    return hasRequest;
}

function validNetworkApprovalContext(value: unknown): boolean {
    if (!knownObject(value, ["host", "protocol"]) || !nonEmptyString(value, "host")
        || !nonEmptyString(value, "protocol")) return false;
    return ["http", "https", "socks5Tcp", "socks5Udp"].includes(String(value.protocol));
}

function validExecPolicyAmendment(value: unknown): value is string[] {
    return boundedStringArray(value);
}

function validNetworkAmendment(value: unknown): value is {host: string; action: "allow" | "deny"} {
    return knownObject(value, ["action", "host"]) && nonEmptyString(value, "host")
        && nonEmptyString(value, "action") && (value.action === "allow" || value.action === "deny");
}

function validCommandActions(value: unknown): boolean {
    if (!Array.isArray(value) || value.length === 0 || value.length > 64) return false;
    return value.every(entry => {
        if (!isObject(entry) || !nonEmptyString(entry, "command")) return false;
        if (entry.type === "read") return knownObject(entry, ["command", "name", "path", "type"])
            && nonEmptyString(entry, "name") && nonEmptyString(entry, "path");
        if (entry.type === "listFiles") return knownObject(entry, ["command", "path", "type"])
            && (entry.path === undefined || entry.path === null || typeof entry.path === "string");
        if (entry.type === "search") return knownObject(entry, ["command", "path", "query", "type"])
            && [entry.path, entry.query].every(field => field === undefined || field === null || typeof field === "string");
        return entry.type === "unknown" && knownObject(entry, ["command", "type"]);
    });
}

function validNetworkAmendments(value: unknown): value is {host: string; action: "allow" | "deny"}[] {
    return Array.isArray(value) && value.length > 0 && value.length <= 64 && value.every(validNetworkAmendment);
}

interface CommandDecision {
    readonly option: PendingDecisionOption;
    readonly wire: unknown;
}

function simpleCommandDecision(value: string): CommandDecision | undefined {
    return approvalOptions[value] ? {option: approvalOptions[value], wire: value} : undefined;
}

function abbreviated(value: string): string {
    const prefix = utf8Prefix(value, 96);
    return prefix === undefined ? value : `${prefix}...`;
}

function structuredCommandDecision(entry: Record<string, unknown>, request: unknown, index: number):
        CommandDecision | undefined {
    if (Object.hasOwn(entry, "acceptWithExecpolicyAmendment")) {
        const body = entry.acceptWithExecpolicyAmendment;
        if (!knownObject(entry, ["acceptWithExecpolicyAmendment"])
            || !knownObject(body, ["execpolicy_amendment"])
            || !validExecPolicyAmendment(body.execpolicy_amendment))
            return undefined;
        const amendment = body.execpolicy_amendment;
        if (!isObject(request) || !validExecPolicyAmendment(request.proposedExecpolicyAmendment)
            || request.proposedExecpolicyAmendment.length !== amendment.length
            || request.proposedExecpolicyAmendment.some((part, partIndex) =>
                part !== amendment[partIndex]))
            return undefined;
        return {
            option: option(`acceptWithExecpolicyAmendment:${index}`,
                `Accept and allow matching commands: ${abbreviated(amendment[0]!)}`, "approve"),
            wire: entry,
        };
    }
    if (Object.hasOwn(entry, "applyNetworkPolicyAmendment")) {
        const body = entry.applyNetworkPolicyAmendment;
        if (!knownObject(entry, ["applyNetworkPolicyAmendment"])
            || !knownObject(body, ["network_policy_amendment"])
            || !validNetworkAmendment(body.network_policy_amendment))
            return undefined;
        const amendment = body.network_policy_amendment;
        if (!isObject(request) || !validNetworkAmendments(request.proposedNetworkPolicyAmendments)
            || !request.proposedNetworkPolicyAmendments.some(candidate => validNetworkAmendment(candidate)
                && candidate.host === amendment.host && candidate.action === amendment.action)) return undefined;
        return {
            option: option(`applyNetworkPolicyAmendment:${index}`,
                `${amendment.action === "allow" ? "Allow" : "Block"} ${abbreviated(amendment.host)} in future`,
                amendment.action === "allow" ? "approve" : "danger"),
            wire: entry,
        };
    }
    return undefined;
}

function commandDecisions(raw: unknown): CommandDecision[] | undefined {
    const result: CommandDecision[] = [];
    const append = (candidate: CommandDecision | undefined): void => {
        if (candidate && !result.some(entry => entry.option.value === candidate.option.value)) result.push(candidate);
    };
    if (isObject(raw) && Object.hasOwn(raw, "availableDecisions") && raw.availableDecisions !== null) {
        if (!Array.isArray(raw.availableDecisions) || raw.availableDecisions.length === 0
            || raw.availableDecisions.length > 64) return undefined;
        for (const [index, entry] of raw.availableDecisions.entries()) {
            if (typeof entry === "string") { append(simpleCommandDecision(entry)); continue; }
            if (!isObject(entry)) return undefined;
            const recognized = Object.hasOwn(entry, "acceptWithExecpolicyAmendment")
                || Object.hasOwn(entry, "applyNetworkPolicyAmendment");
            const candidate = structuredCommandDecision(entry, raw, index);
            if (recognized && !candidate) return undefined;
            append(candidate);
        }
        return result.length > 0 ? result : undefined;
    }
    append(simpleCommandDecision("accept"));
    if (isObject(raw)) {
        if (raw.networkApprovalContext !== undefined && raw.networkApprovalContext !== null) {
            append(simpleCommandDecision("acceptForSession"));
            const amendment = validNetworkAmendments(raw.proposedNetworkPolicyAmendments)
                ? raw.proposedNetworkPolicyAmendments.find(candidate => candidate.action === "allow") : undefined;
            if (amendment) append(structuredCommandDecision({
                applyNetworkPolicyAmendment: {network_policy_amendment: amendment},
            }, raw, 0));
        } else if (raw.additionalPermissions === undefined || raw.additionalPermissions === null) {
            if (validExecPolicyAmendment(raw.proposedExecpolicyAmendment)) append(structuredCommandDecision({
                acceptWithExecpolicyAmendment: {execpolicy_amendment: raw.proposedExecpolicyAmendment},
            }, raw, 0));
        }
    }
    append(simpleCommandDecision("cancel"));
    return result;
}

function validQuestionRequest(raw: unknown): raw is Record<string, unknown> & {questions: Record<string, unknown>[]} {
    if (!isObject(raw) || !Array.isArray(raw.questions) || raw.questions.length === 0 || raw.questions.length > 64)
        return false;
    const ids = new Set<string>();
    let controls = 0;
    for (const candidate of raw.questions) {
        if (!knownObject(candidate, ["header", "id", "isOther", "isSecret", "options", "question"])
            || !nonEmptyString(candidate, "id") || !nonEmptyString(candidate, "question")) return false;
        const id = String(candidate.id);
        if (ids.has(id)) return false;
        ids.add(id);
        if (candidate.header !== undefined && (typeof candidate.header !== "string"
            || utf8Prefix(candidate.header, 4096) !== undefined))
            return false;
        if (candidate.isOther !== undefined && typeof candidate.isOther !== "boolean") return false;
        if (candidate.isSecret !== undefined && typeof candidate.isSecret !== "boolean") return false;
        if (candidate.options !== undefined && candidate.options !== null && !Array.isArray(candidate.options)) return false;
        const choices = Array.isArray(candidate.options) ? candidate.options : [];
        if (choices.length > 64 || controls + 1 + choices.length > 64) return false;
        controls += 1 + choices.length;
        for (const choice of choices) {
            if (!knownObject(choice, ["description", "label"]) || !nonEmptyString(choice, "label")
                || (choice.description !== undefined
                    && (typeof choice.description !== "string"
                        || utf8Prefix(choice.description, 4096) !== undefined))) return false;
        }
        if (choices.length === 0 || candidate.isOther === true) {
            ++controls;
            if (controls > 64) return false;
        }
    }
    return true;
}

function validAnswers(raw: unknown, answers: unknown): boolean {
    if (!isObject(raw) || !Array.isArray(raw.questions) || !isObject(answers)) return false;
    const questions = raw.questions as Record<string, unknown>[];
    const answerKeys = boundedKeys(answers, questions.length);
    if (answerKeys === undefined || answerKeys.length !== questions.length) return false;
    return questions.every(question => {
        const answer = answers[String(question.id)];
        if (!knownObject(answer, ["answers"]) || !Array.isArray(answer.answers)
            || answer.answers.length === 0 || answer.answers.length > 64)
            return false;
        const choices = Array.isArray(question.options) ? question.options : [];
        const allowsOther = choices.length === 0 || question.isOther === true;
        return answer.answers.every(value => typeof value === "string" && value.length > 0
            && utf8Prefix(value, 4096) === undefined
            && (allowsOther || choices.some(choice => choice.label === value)));
    });
}

function mcpApprovalKind(raw: unknown): string {
    const metadata = isObject(raw) && isObject(raw._meta) ? raw._meta : undefined;
    return typeof metadata?.codex_approval_kind === "string" ? metadata.codex_approval_kind : "";
}

function mcpMessageOnly(raw: unknown): boolean {
    if (!isObject(raw)) return false;
    if (raw.mode === "url") return true;
    if (raw.mode === "openai/form") return false;
    const schema = raw.requestedSchema;
    return schema === null || (isObject(schema) && schema.type === "object"
        && isObject(schema.properties) && boundedKeys(schema.properties, 0) !== undefined);
}

function privilegedMcpApproval(raw: unknown): boolean {
    return isObject(raw) && raw.mode === "form" && mcpMessageOnly(raw);
}

function mcpPersistence(raw: unknown, mode: "session" | "always"): boolean {
    const metadata = isObject(raw) && isObject(raw._meta) ? raw._meta : undefined;
    const persist = metadata?.persist;
    return persist === mode || (Array.isArray(persist) && persist.length <= 2
        && persist.some(value => value === mode));
}

function validMcpApprovalMetadata(raw: unknown): boolean {
    if (mcpApprovalKind(raw) !== "mcp_tool_call" || !privilegedMcpApproval(raw)) return true;
    const metadata = isObject(raw) && isObject(raw._meta) ? raw._meta : undefined;
    if (!knownObject(metadata, ["approvals_reviewer", "codex_approval_kind", "codex_request_type",
        "codex_strict_auto_review", "connector_description", "connector_id", "connector_name", "persist",
        "source", "tool_description", "tool_name", "tool_params", "tool_params_display", "tool_title"])) return false;
    const validMode = (value: unknown) => value === "session" || value === "always";
    if (metadata.persist !== undefined && metadata.persist !== null
        && !validMode(metadata.persist) && !(Array.isArray(metadata.persist) && metadata.persist.length > 0
            && metadata.persist.length <= 2 && metadata.persist.every(validMode))) return false;
    const display = metadata.tool_params_display;
    const validDisplay = Array.isArray(display) && display.length > 0 && display.length <= 64
        && display.every(entry => knownObject(entry, ["display_name", "name", "value"])
            && nonEmptyString(entry, "name") && nonEmptyString(entry, "display_name") && Object.hasOwn(entry, "value"));
    if (display !== undefined && display !== null && !validDisplay) return false;
    const parameters = metadata.tool_params;
    if (parameters === undefined || parameters === null) return true;
    if (!isObject(parameters)) return false;
    const parameterKeys = boundedKeys(parameters, 64);
    if (parameterKeys === undefined) return false;
    if (parameterKeys.length === 0) return true;
    if (!validDisplay || display.length !== parameterKeys.length) return false;
    const names = new Set<string>();
    return display.every(entry => {
        const name = String(entry.name);
        if (names.has(name) || !Object.hasOwn(parameters, name)) return false;
        names.add(name);
        return boundedJsonEqual(parameters[name], entry.value);
    });
}

const disclosureFields: Readonly<Record<string, readonly [readonly string[], readonly string[]]>> = {
    "command-approval": [["additionalPermissions", "approvalId", "command", "commandActions", "cwd", "environmentId",
        "itemId", "networkApprovalContext", "proposedExecpolicyAmendment", "proposedNetworkPolicyAmendments", "reason",
        "startedAtMs", "threadId", "turnId"], ["availableDecisions"]],
    "file-change-approval": [["grantRoot", "itemId", "reason", "startedAtMs", "threadId", "turnId"], []],
    "user-input": [["autoResolutionMs", "isBlocking", "itemId", "threadId", "turnId"], ["questions"]],
    "mcp-elicitation": [["serverName", "threadId", "turnId", "message", "mode", "requestedSchema", "elicitationId", "url"],
        ["_meta", "meta"]],
    "permissions-approval": [["cwd", "environmentId", "itemId", "permissions", "reason", "startedAtMs", "threadId", "turnId"], []],
    "dynamic-tool-call": [["callId", "namespace", "threadId", "tool", "turnId"], ["arguments"]],
    "authentication-refresh": [["reason"], ["previousAccountId"]],
    attestation: [[], ["challenge"]],
    "legacy-patch-approval": [["callId", "conversationId", "fileChanges", "grantRoot", "reason"], []],
    "legacy-command-approval": [["approvalId", "callId", "command", "conversationId", "cwd", "parsedCmd", "reason"], []],
    unsupported: [[], []],
};

interface DisclosureState {remaining: number; truncated: boolean}

function boundedDisclosureValue(value: unknown, depth: number, state: DisclosureState): unknown {
    if (state.remaining === 0 || depth === 8) { state.truncated = true; return null; }
    --state.remaining;
    if (Array.isArray(value)) {
        const entries: unknown[] = [];
        for (const entry of value) {
            if (state.remaining === 0) { state.truncated = true; break; }
            entries.push(boundedDisclosureValue(entry, depth + 1, state));
        }
        return entries;
    }
    if (isObject(value)) {
        const entries: Record<string, unknown> = {};
        for (const key in value) if (Object.hasOwn(value, key)) {
            if (state.remaining === 0 || utf8Prefix(key, 128) !== undefined) {
                state.truncated = true;
                break;
            }
            entries[key] = boundedDisclosureValue(value[key], depth + 1, state);
        }
        return entries;
    }
    if (typeof value === "string") {
        const prefix = utf8Prefix(value, 1024);
        if (prefix !== undefined) {
            state.truncated = true;
            return `${prefix}...`;
        }
    }
    return value;
}

function requestContext(request: PendingRequestPresentation): {value: Record<string, unknown>; complete: boolean} {
    const result: Record<string, unknown> = {};
    if (!isObject(request.raw)) return {value: result, complete: false};
    const [visible, hidden] = disclosureFields[request.kind] ?? disclosureFields.unsupported!;
    const state: DisclosureState = {remaining: 64, truncated: false};
    for (const field of visible) if (Object.hasOwn(request.raw, field))
        result[field] = boundedDisclosureValue(request.raw[field], 0, state);
    const rawKeys = boundedKeys(request.raw, visible.length + hidden.length);
    if (rawKeys === undefined || rawKeys.some(key => !visible.includes(key) && !hidden.includes(key)))
        state.truncated = true;
    if (request.kind === "user-input" && !validQuestionRequest(request.raw)) state.truncated = true;
    if (request.kind === "mcp-elicitation" && mcpMessageOnly(request.raw) && isObject(request.raw._meta)
        && (mcpApprovalKind(request.raw) !== "" || mcpPersistence(request.raw, "session")
            || mcpPersistence(request.raw, "always"))) {
        const metadata = request.raw._meta;
        const approval: Record<string, unknown> = {};
        const shown = ["connector_description", "connector_name", "install_url", "persist", "source", "suggest_reason",
            "suggest_type", "tool_description", "tool_name", "tool_params_display", "tool_title", "tool_type"];
        const suppressed = ["approvals_reviewer", "codex_approval_kind", "codex_request_type", "codex_strict_auto_review",
            "connector_id", "tool_id", "tool_params"];
        for (const field of shown) if (Object.hasOwn(metadata, field))
            approval[field] = boundedDisclosureValue(metadata[field], 0, state);
        const metadataKeys = boundedKeys(metadata, shown.length + suppressed.length);
        if (metadataKeys === undefined
            || metadataKeys.some(key => !shown.includes(key) && !suppressed.includes(key))) state.truncated = true;
        if (Object.keys(approval).length > 0) result.approval = approval;
    }
    return {value: result, complete: !state.truncated};
}

export function pendingRequestDetails(request: PendingRequestPresentation): PendingRequestDetails {
    const context = requestContext(request);
    const entries: PendingRequestDetail[] = [];
    const append = (value: unknown, path: string): void => {
        if (Array.isArray(value)) {
            if (value.length === 0) entries.push({path, value: "None"});
            else value.forEach((entry, index) => append(entry, `${path} / ${index + 1}`));
        } else if (isObject(value)) {
            const fields = Object.entries(value);
            if (fields.length === 0) entries.push({path, value: "None"});
            else for (const [key, entry] of fields) append(entry, path === "" ? key : `${path} / ${key}`);
        } else entries.push({path: path || "value", value: value === null || value === undefined ? "None"
            : typeof value === "boolean" ? value ? "Yes" : "No" : String(value)});
    };
    append(context.value, "");
    return {entries, truncated: !context.complete};
}

function approvalContextComplete(request: PendingRequestPresentation): boolean {
    if (!requestContext(request).complete || !isObject(request.raw)) return false;
    const raw = request.raw;
    if (request.kind === "command-approval") {
        if ((raw.command !== undefined && raw.command !== null && !nonEmptyString(raw, "command"))
            || (raw.commandActions !== undefined && raw.commandActions !== null && !validCommandActions(raw.commandActions))
            || (raw.networkApprovalContext !== undefined && raw.networkApprovalContext !== null
                && !validNetworkApprovalContext(raw.networkApprovalContext))
            || (raw.additionalPermissions !== undefined && raw.additionalPermissions !== null
                && !validPermissionProfile(raw.additionalPermissions))
            || (raw.proposedExecpolicyAmendment !== undefined && raw.proposedExecpolicyAmendment !== null
                && !validExecPolicyAmendment(raw.proposedExecpolicyAmendment))
            || (raw.proposedNetworkPolicyAmendments !== undefined && raw.proposedNetworkPolicyAmendments !== null
                && !validNetworkAmendments(raw.proposedNetworkPolicyAmendments))) return false;
        return [raw.command, raw.commandActions, raw.networkApprovalContext, raw.additionalPermissions]
            .some(value => value !== undefined && value !== null);
    }
    if (request.kind === "file-change-approval")
        return nonEmptyString(raw, "itemId") || nonEmptyString(raw, "reason") || nonEmptyString(raw, "grantRoot");
    if (request.kind === "user-input") return true;
    if (request.kind === "mcp-elicitation") {
        if (!nonEmptyString(raw, "serverName") || !nonEmptyString(raw, "threadId") || !nonEmptyString(raw, "message")
            || !validMcpApprovalMetadata(raw))
            return false;
        if (raw.mode === "url") return nonEmptyString(raw, "elicitationId") && nonEmptyString(raw, "url");
        if (raw.mode === "openai/form") return Object.hasOwn(raw, "requestedSchema");
        return raw.mode === "form" && isObject(raw.requestedSchema);
    }
    if (request.kind === "permissions-approval") return validPermissionProfile(raw.permissions);
    if (request.kind === "legacy-patch-approval")
        return nonEmptyString(raw, "conversationId") && isObject(raw.fileChanges)
            && boundedKeys(raw.fileChanges, 0) === undefined;
    if (request.kind === "legacy-command-approval") return nonEmptyString(raw, "conversationId")
        && Array.isArray(raw.command) && raw.command.length > 0
        && raw.command.every(part => typeof part === "string" && part.length > 0);
    return false;
}

function safeOptions(request: PendingRequestPresentation, candidates: PendingDecisionOption[]): PendingDecisionOption[] {
    const safe = approvalContextComplete(request) ? candidates : candidates.filter(candidate =>
        ["decline", "cancel", "denied", "abort"].includes(candidate.value));
    return safe.length > 0 ? safe : [option("unsupported", "Return unsupported", "neutral")];
}

function evaluateCommand(request: PendingRequestPresentation):
        {decisions: CommandDecision[] | undefined; options: readonly PendingDecisionOption[]} {
    const decisions = commandDecisions(request.raw);
    return {decisions, options: decisions ? safeOptions(request, decisions.map(value => value.option))
        : [option("unsupported", "Return unsupported", "neutral")]};
}

export function pendingDecisionOptions(request: PendingRequestPresentation): readonly PendingDecisionOption[] {
    if (request.kind === "command-approval") return evaluateCommand(request).options;
    if (request.kind === "file-change-approval") return safeOptions(request, Object.values(approvalOptions));
    if (request.kind === "user-input") return safeOptions(request, [
        option("submit", "Submit", "approve", true), option("decline", "Decline", "danger"),
    ]);
    if (request.kind === "mcp-elicitation") {
        const approvalKind = mcpApprovalKind(request.raw);
        const messageOnly = mcpMessageOnly(request.raw);
        const privileged = isObject(request.raw) && request.raw.mode === "form" && messageOnly;
        const session = messageOnly && mcpPersistence(request.raw, "session");
        const always = messageOnly && mcpPersistence(request.raw, "always");
        if (approvalKind !== "" && !["mcp_tool_call", "tool_suggestion"].includes(approvalKind))
            return [option("unsupported", "Return unsupported", "neutral")];
        if (approvalKind === "tool_suggestion" && privileged)
            return [option("unsupported", "Return unsupported", "neutral")];
        if (approvalKind === "mcp_tool_call" && privileged) {
            const candidates = [option("accept", "Accept", "approve")];
            if (session)
                candidates.push(option("acceptForSession", "Accept for session", "approve"));
            if (always)
                candidates.push(option("acceptAlways", "Always allow", "approve"));
            candidates.push(option("cancel", "Cancel", "neutral"));
            return safeOptions(request, candidates);
        }
        const candidates = [option("accept", "Accept", "approve", !messageOnly)];
        if (session)
            candidates.push(option("acceptForSession", "Accept for session", "approve"));
        if (always)
            candidates.push(option("acceptAlways", "Always allow", "approve"));
        candidates.push(option("decline", "Decline", "danger"), option("cancel", "Cancel", "neutral"));
        return safeOptions(request, candidates);
    }
    if (request.kind === "permissions-approval") return safeOptions(request, [
        option("turn", "Allow this turn", "approve"), option("session", "Allow this session", "approve"),
        option("decline", "Decline", "danger"),
    ]);
    if (request.kind === "legacy-patch-approval" || request.kind === "legacy-command-approval") return safeOptions(request, [
        option("approved", "Approve", "approve"), option("approved_for_session", "Approve for session", "approve"),
        option("denied", "Deny", "danger"), option("abort", "Abort", "neutral"),
    ]);
    if (request.kind === "dynamic-tool-call") return [option("unavailable", "Return unavailable", "neutral")];
    return [option("unsupported", "Return unsupported", "neutral")];
}

export function pendingResponse(request: PendingRequestPresentation,
                                submission: PendingRequestSubmission): PendingResponse | undefined {
    const command = request.kind === "command-approval" ? evaluateCommand(request) : undefined;
    if (!(command?.options ?? pendingDecisionOptions(request)).some(option => option.value === submission.choice))
        return undefined;
    const decision = submission.choice;
    if (decision === "unsupported") return error(request.kind === "authentication-refresh"
        ? "CodexUI does not support authentication token refresh"
        : request.kind === "attestation" ? "CodexUI does not support attestation generation"
            : "CodexUI does not support this server request");
    if (request.kind === "command-approval") {
        const selected = command!.decisions?.find(candidate => candidate.option.value === decision);
        return selected ? {result: {decision: selected.wire}} : undefined;
    }
    if (request.kind === "file-change-approval") return {result: {decision}};
    if (request.kind === "user-input") {
        if (decision === "decline") return error("Request declined by user");
        return validAnswers(request.raw, submission.input) ? {result: {answers: submission.input}} : undefined;
    }
    if (request.kind === "mcp-elicitation") {
        const approvalKind = mcpApprovalKind(request.raw);
        if (decision === "acceptForSession" || decision === "acceptAlways") return {result: {
            action: "accept", content: null,
            _meta: {persist: decision === "acceptForSession" ? "session" : "always"},
        }};
        if (decision !== "accept") return {result: {action: decision, content: null, _meta: null}};
        if (approvalKind === "mcp_tool_call" && privilegedMcpApproval(request.raw))
            return {result: {action: "accept", content: null, _meta: null}};
        if (!mcpMessageOnly(request.raw) && request.raw && isObject(request.raw)
            && request.raw.mode !== "openai/form" && !isObject(submission.input)) return undefined;
        return {result: {action: "accept", content: mcpMessageOnly(request.raw) ? null : submission.input ?? null,
            _meta: submission.metadata ?? null}};
    }
    if (request.kind === "permissions-approval") {
        if (decision === "decline") return error("Permission request declined by user");
        return {result: {permissions: (request.raw as Record<string, unknown>).permissions, scope: decision}};
    }
    if (request.kind === "legacy-patch-approval" || request.kind === "legacy-command-approval") {
        if (decision === "approved" || decision === "approved_for_session") return {result: {decision}};
        if (decision === "denied") return {result: {decision: {denied: {rejection: "Denied by user"}}}};
        return {result: {decision: "abort"}};
    }
    if (request.kind === "dynamic-tool-call") return {result: {
        contentItems: [{type: "inputText", text: "CodexUI does not provide this dynamic tool"}], success: false,
    }};
    return undefined;
}
