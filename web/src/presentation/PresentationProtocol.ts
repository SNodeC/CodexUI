export const PresentationProtocolName = "codexui.presentation";
export const PresentationProtocolVersion = 1;

export type Authority = "none" | "merge" | "replace" | "remove";
export type JsonObject = Record<string, unknown>;
export const ThreadSettingFields = ["activePermissionProfile", "approvalPolicy", "approvalsReviewer",
    "collaborationMode", "cwd", "effort", "instructionSources", "model", "modelProvider", "personality",
    "reasoningEffort", "sandbox", "sandboxPolicy", "serviceTier", "summary"] as const;
const ThreadSettingConceptAliases: Record<string, string> = {activePermissionProfile: "permissionProfile",
    approvalPolicy: "approval", approvalsReviewer: "reviewer", collaborationMode: "collaboration",
    permissions: "permissionProfile", reasoningEffort: "effort", sandboxPolicy: "sandbox"};

export interface ThreadSettingStamp {generation: number; sequence: number; acknowledgements: number}

export function threadSettingConcept(field: string): string {
    const alias = ThreadSettingConceptAliases[field];
    return alias ?? ((ThreadSettingFields as readonly string[]).includes(field) ? field : "");
}

export interface PresentationFrame extends JsonObject {
    protocol: typeof PresentationProtocolName;
    version: typeof PresentationProtocolVersion;
    kind: "command" | "event" | "result";
}

function baseFrame(kind: PresentationFrame["kind"]): PresentationFrame {
    return {
        protocol: PresentationProtocolName,
        version: PresentationProtocolVersion,
        kind,
    };
}

function addAuthorityAndScope(
    frame: PresentationFrame,
    authority: Authority,
    scope: JsonObject,
): void {
    frame.authority = authority;
    if (Object.keys(scope).length > 0) frame.scope = scope;
}

export function command(
    action: string,
    data: JsonObject = {},
    correlationId = "",
): PresentationFrame {
    const frame = baseFrame("command");
    frame.action = action;
    frame.data = data;
    if (correlationId !== "") frame.correlationId = correlationId;
    return frame;
}

export function result(
    sequence: number,
    generation: number,
    action: string,
    correlationId: string,
    ok: boolean,
    data: unknown,
    authority: Authority = "none",
    scope: JsonObject = {},
): PresentationFrame {
    const frame = baseFrame("result");
    frame.sequence = sequence;
    frame.generation = generation;
    frame.action = action;
    frame.correlationId = correlationId;
    frame.ok = ok;
    frame[ok ? "data" : "error"] = data;
    addAuthorityAndScope(frame, authority, scope);
    return frame;
}

export function event(
    sequence: number,
    generation: number,
    type: string,
    data: JsonObject = {},
    authority: Authority = "none",
    scope: JsonObject = {},
): PresentationFrame {
    const frame: PresentationFrame = {protocol: PresentationProtocolName, version: PresentationProtocolVersion,
        kind: "event", sequence, generation, type, data};
    addAuthorityAndScope(frame, authority, scope);
    return frame;
}

export function isObject(value: unknown): value is JsonObject {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}

export function jsonEqual(left: unknown, right: unknown): boolean {
    if (Object.is(left, right)) return true;
    if (Array.isArray(left) || Array.isArray(right)) return Array.isArray(left) && Array.isArray(right)
        && left.length === right.length && left.every((value, index) => jsonEqual(value, right[index]));
    if (!isObject(left) || !isObject(right)) return false;
    const keys = Object.keys(left);
    return keys.length === Object.keys(right).length
        && keys.every(key => Object.hasOwn(right, key) && jsonEqual(left[key], right[key]));
}

function isUnsigned(value: unknown): value is number {
    return typeof value === "number" && Number.isInteger(value) && value >= 0;
}

export function isPresentationFrame(value: unknown): value is PresentationFrame {
    if (!isObject(value)) return false;
    if (
        value.protocol !== PresentationProtocolName
        || value.version !== PresentationProtocolVersion
        || typeof value.kind !== "string"
    ) {
        return false;
    }
    if (value.kind === "command") {
        return stringMember(value, "action") !== "" && isObject(value.data);
    }
    if (value.kind !== "event" && value.kind !== "result") return false;
    if (
        !isUnsigned(value.sequence)
        || !isUnsigned(value.generation)
        || (value.authority !== "none"
            && value.authority !== "merge"
            && value.authority !== "replace"
            && value.authority !== "remove")
        || (Object.hasOwn(value, "scope") && !isObject(value.scope))
    ) {
        return false;
    }
    if (value.kind === "event") {
        return stringMember(value, "type") !== "" && isObject(value.data);
    }
    if (
        stringMember(value, "action") === ""
        || stringMember(value, "correlationId") === ""
        || typeof value.ok !== "boolean"
    ) {
        return false;
    }
    return value.ok
        ? Object.hasOwn(value, "data") && !Object.hasOwn(value, "error")
        : Object.hasOwn(value, "error") && !Object.hasOwn(value, "data");
}

export function stringMember(value: unknown, name: string): string {
    return isObject(value) && typeof value[name] === "string"
        ? value[name]
        : "";
}

export function member(
    value: unknown,
    name: string,
    fallback: unknown = null,
): unknown {
    return isObject(value) && Object.hasOwn(value, name) ? value[name] : fallback;
}
