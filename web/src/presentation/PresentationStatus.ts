export type StatusKind = "unknown" | "pending" | "running" | "completed" | "failed" | "interrupted"
    | "notLoaded" | "connected" | "disconnected";

export interface PresentationStatus {
    readonly semantic: StatusKind;
    readonly unknownText: string;
}

export const UnknownStatus: PresentationStatus = {semantic: "unknown", unknownText: ""};

function protocolWords(value: string, format: "label" | "status"): string {
    if (format === "label") value = value.replace(/^\p{White_Space}+|\p{White_Space}+$/gu, "");
    if (format === "label" && value.toLowerCase() === "xhigh") return "Extra high";
    const result: string[] = [];
    let pendingSpace = false;
    const characters = [...value];
    const upper = format === "label" ? /\p{Lu}/u : /[A-Z]/u;
    const lower = format === "label" ? /\p{Ll}/u : /[a-z]/u;
    const digit = format === "label" ? /\p{Nd}/u : /[0-9]/u;
    for (let index = 0; index < characters.length; ++index) {
        let character = characters[index]!;
        const separator = format === "label" ? /\p{White_Space}|[-_./]/u.test(character)
            : /[ \t\n\r\f\v_.\/-]/u.test(character);
        if (separator) { pendingSpace = result.length > 0; continue; }
        const previous = characters[index - 1] ?? "";
        const next = characters[index + 1] ?? "";
        const boundary = upper.test(character) && (lower.test(previous) || digit.test(previous)
            || (upper.test(previous) && lower.test(next)));
        if ((pendingSpace || boundary) && result.at(-1) !== " ") result.push(" ");
        if (format === "status" ? /[A-Z]/u.test(character)
            : boundary || (pendingSpace && upper.test(character) && lower.test(next)))
            character = character.toLowerCase();
        result.push(character); pendingSpace = false;
    }
    if (result.length === 0) return format === "label" ? "" : "unknown";
    if (format === "label") result[0] = result[0]!.toUpperCase();
    return result.join("");
}

export function humanizeProtocolLabel(value: string): string {
    return protocolWords(value, "label");
}

export function statusFromValue(value: unknown): PresentationStatus {
    const raw = typeof value === "string" ? value
        : value !== null && typeof value === "object" && typeof (value as {type?: unknown}).type === "string"
            ? (value as {type: string}).type : "";
    if (["pending", "queued"].includes(raw)) return {semantic: "pending", unknownText: ""};
    if (["active", "inProgress", "running", "started"].includes(raw)) return {semantic: "running", unknownText: ""};
    if (["complete", "completed", "idle", "succeeded"].includes(raw)) return {semantic: "completed", unknownText: ""};
    if (["blocked", "error", "failed", "systemError"].includes(raw)) return {semantic: "failed", unknownText: ""};
    if (["canceled", "cancelled", "interrupted", "stopped"].includes(raw))
        return {semantic: "interrupted", unknownText: ""};
    if (raw === "notLoaded" || raw === "connected" || raw === "disconnected")
        return {semantic: raw, unknownText: ""};
    return {semantic: "unknown", unknownText: raw};
}

export function statusToken(status: PresentationStatus): string {
    return status.semantic === "unknown" ? status.unknownText : status.semantic;
}

export function displayStatus(status: PresentationStatus): string {
    if (status.semantic !== "unknown") return status.semantic === "notLoaded" ? "not loaded" : status.semantic;
    return protocolWords(status.unknownText, "status");
}

export function statusTone(status: PresentationStatus): string {
    if (status.semantic === "running") return "active";
    if (status.semantic === "completed" || status.semantic === "connected") return "success";
    if (status.semantic === "failed" || status.semantic === "disconnected") return "danger";
    return status.semantic === "interrupted" ? "warning" : "";
}

export function isEmptyStatus(status: PresentationStatus): boolean {
    return status.semantic === "unknown" && status.unknownText === "";
}

export function isActiveStatus(status: PresentationStatus): boolean {
    return status.semantic === "running";
}

export function isWorkingStatus(status: PresentationStatus): boolean {
    return status.semantic === "pending" || status.semantic === "running";
}

export function isTerminalTurnStatus(status: PresentationStatus): boolean {
    return status.semantic === "completed" || status.semantic === "failed" || status.semantic === "interrupted";
}

export function effectivePlanStepStatus(step: PresentationStatus, turn: PresentationStatus,
    thread: PresentationStatus): PresentationStatus {
    if (!isActiveStatus(step)) return step;
    const outcome = isTerminalTurnStatus(turn) ? turn : thread;
    return isTerminalTurnStatus(outcome) ? outcome : step;
}
