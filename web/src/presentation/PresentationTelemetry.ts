import {isObject, member, stringMember} from "./PresentationProtocol.js";
import type {PresentationModel} from "./PresentationModel.js";
import {TimingMethods, TimingTypes} from "./TimingPaths.js";

export interface ProtocolTime {field: string; value?: number; unit: "seconds" | "milliseconds" | "duration"; unavailableZero: boolean}
export interface TimingTarget {threadId: string; turnId?: string; itemId?: string}
export function protocolTimes(value: unknown, type: string, prefix = "", own = true): ProtocolTime[] {
    const result: ProtocolTime[] = []; let visits = 0;
    const collect = (value: unknown, path: string, field: string) => {
        if (++visits > 2048 || result.length >= 128) return;
        if (path.startsWith("@")) { for (const child of TimingTypes[path.slice(1)] ?? []) collect(value, child, field); return; }
        if (!path) {
            const name = field.split("/").at(-1) ?? "";
            result.push({field, ...(typeof value === "number" && Number.isSafeInteger(value) ? {value} : {}),
                unit: name === "durationMs" || name === "duration_ms" ? "duration" : name.endsWith("Ms") ? "milliseconds" : "seconds", unavailableZero: type === "FsGetMetadataResponse"});
            return;
        }
        const [key, ...rest] = path.split("/");
        const descend = (child: unknown, name: string) => collect(child, rest.join("/"), `${field ? `${field}/` : ""}${name.slice(0, 192)}`);
        if (key === "*") {
            if (Array.isArray(value) || isObject(value)) for (const [name, child] of Object.entries(value)) {
                descend(child, name); if (visits >= 2048 || result.length >= 128) break;
            }
        } else if (isObject(value) && key && Object.hasOwn(value, key)) descend(value[key], key);
    };
    for (const path of TimingTypes[type] ?? []) {
        if (own && (path.startsWith("turns/") || path.startsWith("items/"))) continue;
        collect(value, path, prefix);
    }
    return result;
}
export function protocolFrameTimes(method: string, value: unknown, response: boolean): ProtocolTime[] {
    return protocolTimes(value, TimingMethods[`${method}:${response ? "result" : "params"}`] ?? "", "", false);
}
export function targetTimes(model: PresentationModel, target: TimingTarget, includeOwners = false): ProtocolTime[] {
    const thread = model.thread(target.threadId);
    const turn = target.turnId ? thread?.turns.get(target.turnId) : undefined;
    const item = target.itemId ? turn?.items.get(target.itemId) : undefined;
    if (target.itemId && !item || target.turnId && !turn) return [];
    const times = item ? [...protocolTimes(item.raw, "ItemLifecycle"), ...(turn ? turn.timing.map(time => ({...time, field: `turn/${time.field}`})) : [])].slice(0, 128)
        : turn ? turn.timing : protocolTimes(thread?.raw, "Thread");
    if (!includeOwners || !thread) return times;
    return [...times, ...(item || turn ? protocolTimes(thread.raw, "Thread", "thread") : []),
        ...protocolTimes(model.projects.get(thread.projectId)?.raw, "Project", "project")].slice(0, 128);
}
function date(time: ProtocolTime): Date | undefined {
    if (time.value === undefined || time.unit === "duration" || time.unavailableZero && time.value === 0) return;
    const milliseconds = time.unit === "seconds" ? time.value * 1000 : time.value;
    if (milliseconds < -62135596800000 || milliseconds > 253402300799999) return;
    return new Date(milliseconds);
}
export function timingSummary(times: readonly ProtocolTime[]): string {
    const turn = times.some(time => time.field.startsWith("turn/"));
    let start = "", end = "", duration = "";
    for (const time of times) {
        if (time.field.startsWith("turn/") !== turn) continue;
        if (time.unit === "duration" && time.value !== undefined && time.value >= 0) duration = `${(time.value / 1000).toFixed(3)} s`;
        const local = date(time); if (!local) continue;
        if (time.field.includes("startedAt")) start = local.toLocaleTimeString([], {hour12: false});
        if (time.field.includes("completedAt")) end = local.toLocaleTimeString([], {hour12: false});
    }
    return [start + (end ? `${start ? "–" : ""}${end}` : ""), duration].filter(Boolean).join(" · ");
}
export function timingDetails(times: readonly ProtocolTime[]): string {
    return times.map(time => {
        const local = date(time);
        const value = time.value === undefined ? "Not supplied" : time.unit === "duration"
            ? time.value < 0 ? "Invalid duration" : `${(time.value / 1000).toFixed(3)} s`
            : local ? `${local.toString()}\nUTC: ${local.toISOString()}` : time.unavailableZero && time.value === 0 ? "Unavailable (protocol sentinel)" : "Invalid / outside supported date range";
        return `${time.field}: ${value}${time.value === undefined ? "" : `\nRaw: ${time.value} ${time.unit === "seconds" ? "Unix seconds" : time.unit === "milliseconds" ? "Unix milliseconds" : "milliseconds (duration)"}`}`;
    }).join("\n\n") || "No timestamps supplied for this object.";
}

const TokenFields = ["totalTokens", "inputTokens", "outputTokens", "cachedInputTokens", "cacheWriteInputTokens", "reasoningOutputTokens"];
const TokenLabels = ["Total", "Input", "Output", "Cached input", "Cache-write input", "Reasoning output"];
function count(value: unknown): number | undefined { return typeof value === "number" && Number.isSafeInteger(value) && value >= 0 ? value : undefined; }
function counts(value: unknown): (number | undefined)[] { return TokenFields.map(field => count(member(value, field))); }
function number(value: number | undefined, compact = false): string {
    return value === undefined ? "—" : compact && value >= 1000 ? `${(value / (value >= 1e6 ? 1e6 : 1000)).toFixed(1)}${value >= 1e6 ? "M" : "k"}` : value.toLocaleString("en-US");
}
export interface TokenUsageText {summary: string; details: string}
function tokenText(values: (number | undefined)[], prefix: string): TokenUsageText {
    return {summary: `${prefix} ${number(values[0], true)} | In ${number(values[1], true)} | Out ${number(values[2], true)}`,
        details: values.map((value, index) => `${TokenLabels[index]}: ${number(value)}`).join("\n") + "\nCached input and reasoning are breakdowns, not additional totals.\n"};
}
export function tokenUsage(model: PresentationModel, threadId: string): {thread: TokenUsageText; overall: TokenUsageText} {
    let thread = tokenText(TokenFields.map(() => 0), "Tokens");
    thread.details += "No retained usage reported; counters initialized to zero.";
    const total = TokenFields.map(() => 0 as number | undefined), reported = TokenFields.map(() => 0);
    const threads = model.threadIds();
    for (const id of threads) {
        const usage = model.thread(id)?.raw.tokenUsage;
        const values = counts(member(usage, "total"));
        values.forEach((value, index) => {
            if (value === undefined) return;
            reported[index]!++; const sum = (total[index] ?? Number.NaN) + value;
            total[index] = Number.isSafeInteger(sum) ? sum : undefined;
        });
        if (id !== threadId || !isObject(usage)) continue;
        thread = tokenText(values, "Tokens");
        const last = counts(usage.last), capacity = count(usage.modelContextWindow);
        thread.details += `\nLatest reported usage\n${tokenText(last, "").details}\nContext capacity: ${number(capacity && capacity > 0 ? capacity : undefined)}`;
        if (capacity && last[0] !== undefined) {
            const remaining = capacity - Math.min(last[0], capacity);
            thread.summary += ` | Left ≈${number(remaining, true)}`;
            thread.details += `\nEstimated remaining: ${number(remaining)}\nBased on the latest report, not cumulative thread consumption.`;
        }
    }
    const overall = tokenText(total.map((value, index) => reported[index] ? value : undefined), "Overall tokens");
    overall.details += "\nScope: available thread snapshots, including archived threads.\n" + TokenFields.map((field, index) => `${field} coverage: ${reported[index]}/${threads.length} threads`).join("\n")
        + `\n\nAccount lifetime tokens: ${number(count(member(model.accountUsage.summary, "lifetimeTokens")))}\nSeparate account scope; input/output breakdowns are not provided.`;
    if (model.accountUsageFailed) overall.details += "\nAccount usage refresh unavailable; any account value is the last successful snapshot.";
    return {thread, overall};
}
