export const PresentationProtocolName = "codexui.presentation";
export const PresentationProtocolVersion = 1;

export type Authority = "none" | "merge" | "replace" | "remove";
export type JsonObject = Record<string, unknown>;

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
    const frame = baseFrame("event");
    frame.sequence = sequence;
    frame.generation = generation;
    frame.type = type;
    frame.data = data;
    addAuthorityAndScope(frame, authority, scope);
    return frame;
}

export function isObject(value: unknown): value is JsonObject {
    return typeof value === "object" && value !== null && !Array.isArray(value);
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
    const stringField = (name: string): boolean =>
        typeof value[name] === "string" && value[name] !== "";
    if (value.kind === "command") {
        return stringField("action") && isObject(value.data);
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
        return stringField("type") && isObject(value.data);
    }
    if (
        !stringField("action")
        || !stringField("correlationId")
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
