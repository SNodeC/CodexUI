export type StatusKind = "unknown" | "active" | "completed" | "failed" | "interrupted";

export interface PresentationStatus {
    readonly kind: StatusKind;
    readonly text: string;
    readonly tone: string;
}

export function classifyStatus(status: string): PresentationStatus {
    if (["active", "inProgress", "running", "started"].includes(status))
        return {kind: "active", text: "Running", tone: "active"};
    if (["completed", "idle"].includes(status))
        return {kind: "completed", text: "Completed", tone: "success"};
    if (["failed", "systemError"].includes(status))
        return {kind: "failed", text: "Failed", tone: "danger"};
    if (status === "interrupted")
        return {kind: "interrupted", text: "Interrupted", tone: "warning"};
    return {kind: "unknown", text: status === "" ? "Unknown" : status, tone: ""};
}

export function isActiveStatus(status: string): boolean {
    return classifyStatus(status).kind === "active";
}

export function isTerminalTurnStatus(status: string): boolean {
    return ["completed", "failed", "interrupted"].includes(classifyStatus(status).kind);
}
