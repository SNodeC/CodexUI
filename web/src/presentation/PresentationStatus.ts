export type StatusKind = "unknown" | "active" | "completed" | "failed" | "interrupted" | "pending" | "notLoaded";

export interface PresentationStatus {
    readonly kind: StatusKind;
    readonly text: string;
    readonly tone: string;
}

export function classifyStatus(status: string): PresentationStatus {
    if (["active", "inProgress", "running", "started"].includes(status))
        return {kind: "active", text: "running", tone: "active"};
    if (["completed", "idle"].includes(status))
        return {kind: "completed", text: "completed", tone: "success"};
    if (["failed", "systemError"].includes(status))
        return {kind: "failed", text: "failed", tone: "danger"};
    if (status === "interrupted")
        return {kind: "interrupted", text: "interrupted", tone: "warning"};
    if (status === "pending") return {kind: "pending", text: "pending", tone: ""};
    if (status === "notLoaded") return {kind: "notLoaded", text: "not loaded", tone: ""};
    return {kind: "unknown", text: status === "" ? "unknown" : status, tone: ""};
}

export function displayStatus(status: string): string {
    const classified = classifyStatus(status);
    if (classified.kind !== "unknown" || status === "") return classified.text;
    const words = [...status.trim()].reduce<string[]>((result, original, index, characters) => {
        if (/\s|[-_./]/u.test(original)) {
            if (result.length > 0 && result.at(-1) !== " ") result.push(" ");
            return result;
        }
        const previous = characters[index - 1] ?? "";
        const next = characters[index + 1] ?? "";
        const boundary = /[A-Z]/u.test(original) && (/[a-z\d]/u.test(previous)
            || (/[A-Z]/u.test(previous) && /[a-z]/u.test(next)));
        if (boundary && result.length > 0 && result.at(-1) !== " ") result.push(" ");
        result.push(original.toLocaleLowerCase());
        return result;
    }, []);
    return words.join("").trim() || "unknown";
}

export function isActiveStatus(status: string): boolean {
    return classifyStatus(status).kind === "active";
}

export function isTerminalTurnStatus(status: string): boolean {
    return ["completed", "failed", "interrupted"].includes(classifyStatus(status).kind);
}
