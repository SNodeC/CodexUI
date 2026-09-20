import type {ItemPresentation, ThreadPresentation, TurnPresentation} from "../presentation/PresentationModel.js";
import {isObject, member, stringMember} from "../presentation/PresentationProtocol.js";
import type {JsonObject} from "../presentation/PresentationProtocol.js";
import {UnknownStatus, effectivePlanStepStatus, statusFromValue} from "../presentation/PresentationStatus.js";
import type {PresentationStatus} from "../presentation/PresentationStatus.js";
import {AuthoritativeHistoryPageSize, PendingAnimationDelayMilliseconds, terminalOutputHasVisibleText} from "./MiddleTypes.js";
import type {
    AgentActivityData, AgentMessageData, AuthoritativeItemKey, CardKey, CardKind, CardPayload,
    CommandExecutionData, ConversationSnapshot, FileChangeData, FileChangesData, GenericActivityData,
    ImageGenerationData, LocalPromptData, PlanData, ReasoningData, UserMessageData, VisibleCardData,
} from "./MiddleTypes.js";
import {
    authoritativePosition, indexAuthoritativeItems, localCardVisible,
} from "./PromptCoordinator.js";
import type {AuthoritativeItemIndex, PromptSubmission} from "./PromptCoordinator.js";

export const DefaultAuthoritativeItemLimit = AuthoritativeHistoryPageSize;

function integerValue(object: unknown, key: string): number | undefined {
    const value = isObject(object) ? object[key] : undefined;
    return typeof value === "number" && Number.isInteger(value) ? value : undefined;
}
function messageText(item: unknown): string {
    const type = stringMember(item, "type");
    if (type === "agentMessage" || type === "plan") return stringMember(item, "text");
    if (type !== "userMessage") return "";
    const parts: string[] = [];
    const content = member(item, "content", []);
    if (Array.isArray(content)) for (const entry of content) {
        const value = stringMember(entry, "text"); if (value !== "") parts.push(value);
    }
    if (parts.length === 0) { const fallback = stringMember(item, "text"); if (fallback !== "") parts.push(fallback); }
    return parts.join("\n");
}
function messageImagePaths(item: unknown): string[] {
    const content = member(item, "content", []);
    return Array.isArray(content) ? content.filter(entry => stringMember(entry, "type") === "localImage")
        .map(entry => stringMember(entry, "path")).filter(Boolean) : [];
}
function localImagePaths(submission: PromptSubmission): string[] {
    return submission.attachments.filter(value => value.mimeType.startsWith("image/")).map(value => value.path);
}
function stringList(value: unknown): string[] {
    return Array.isArray(value) ? value.filter((entry): entry is string => typeof entry === "string") : [];
}
function unifiedDiffCounts(diff: string): [number, number] {
    let additions = 0, deletions = 0;
    for (const line of diff.split("\n")) {
        if (line.startsWith("+++ ") || line.startsWith("--- ")) continue;
        if (line.startsWith("+")) ++additions; else if (line.startsWith("-")) ++deletions;
    }
    return [additions, deletions];
}
function omittedTextBytes(item: ItemPresentation, field: string): number {
    return item.textRetention?.get(field)?.discardedBytes ?? 0;
}
function withTruncationNotice(value: string, omitted: number, subject: string, markdown: boolean): string {
    if (omitted === 0) return value;
    const notice = `Earlier ${subject} was truncated (${omitted} bytes omitted).`;
    return markdown ? `> ${notice}\n\n${value}` : `[${notice}]\n${value}`;
}

function projectPlan(plan: JsonObject, turnStatus: PresentationStatus,
    threadStatus: PresentationStatus): PlanData {
    const steps = Array.isArray(plan.steps) ? plan.steps : [];
    return {explanation: stringMember(plan, "explanation"), steps: steps.flatMap(step => {
        const text = stringMember(step, "step") || stringMember(step, "text");
        return text === "" ? [] : [{text, status: effectivePlanStepStatus(
            statusFromValue(member(step, "status")), turnStatus, threadStatus)}];
    }), legacyText: ""};
}

export function projectTurnPlan(turn: TurnPresentation, threadStatus: PresentationStatus): PlanData | undefined {
    if (Object.keys(turn.plan).length > 0) return projectPlan(turn.plan, turn.status, threadStatus);
    for (let index = turn.itemOrder.length - 1; index >= 0; --index) {
        const item = turn.items.get(turn.itemOrder[index]!);
        if (!item || stringMember(item.raw, "type") !== "plan") continue;
        return {explanation: "", steps: [], legacyText: withTruncationNotice(
            messageText(item.raw), omittedTextBytes(item, "text"), "plan text", true)};
    }
    return undefined;
}

// Shared with graphDisplayDetail: readable, code-point-ordered fields, four
// nesting levels, and at most 4000 UTF-8 bytes before the truncation notice.
function genericActivityDetail(item: JsonObject): string {
    const bytes = new Uint8Array(4000);
    const encoder = new TextEncoder();
    let used = 0, truncated = false;
    const append = (text: string): void => {
        if (truncated) return;
        const {read, written} = encoder.encodeInto(text.slice(0, bytes.length - used + 1), bytes.subarray(used));
        used += written;
        truncated = read < text.length;
    };
    const keyOrder = (left: string, right: string): number => {
        for (let a = 0, b = 0; a < left.length && b < right.length;) {
            const x = left.codePointAt(a)!, y = right.codePointAt(b)!;
            if (x !== y) return x - y;
            a += x > 0xffff ? 2 : 1; b += y > 0xffff ? 2 : 1;
        }
        return left.length - right.length;
    };
    const visit = (value: unknown, depth: number): void => {
        if (truncated) return;
        if (value === null || typeof value !== "object") { append(value == null ? "none" : String(value)); return; }
        if (depth >= 4) { append("nested detail omitted"); return; }
        if (Array.isArray(value)) {
            if (value.length === 0) append("none");
            for (const entry of value) {
                append(`\n${"  ".repeat(depth + 1)}- `); visit(entry, depth + 1);
                if (truncated) return;
            }
            return;
        }
        const keys = Object.keys(value).sort(keyOrder);
        if (keys.length === 0) { append("none"); return; }
        for (const key of keys) {
            append(`\n${"  ".repeat(depth + 1)}`);
            append(`${key}: `);
            visit((value as Record<string, unknown>)[key], depth + 1);
            if (truncated) return;
        }
    };
    for (const key of Object.keys(item).sort(keyOrder)) {
        if (["protocolId", "protocolThreadId", "protocolTurnId", "textRetention"].includes(key)) continue;
        append(`${key}: `); visit(item[key], 0);
        if (truncated) break;
        append("\n");
    }
    return new TextDecoder().decode(bytes.subarray(0, used)) + (truncated ? "\n\n[Activity details truncated]" : "");
}

function authoritativeCard(identity: AuthoritativeItemKey, presentation: ItemPresentation,
    visualKey: CardKey, threadCwd: string): VisibleCardData {
    const item = presentation.raw;
    const type = stringMember(item, "type");
    let kind: CardKind = "genericActivity";
    let status = statusFromValue(member(item, "status"));
    let payload: CardPayload;
    if (type === "userMessage") {
        status = UnknownStatus;
        kind = "userMessage"; payload = {text: messageText(item), imagePaths: messageImagePaths(item)} satisfies UserMessageData;
    } else if (type === "agentMessage") {
        status = UnknownStatus;
        kind = "agentMessage";
        payload = {text: withTruncationNotice(messageText(item), omittedTextBytes(presentation, "text"), "Codex response", true),
            finalAnswer: stringMember(item, "phase") === "final_answer"} satisfies AgentMessageData;
    } else if (type === "commandExecution") {
        kind = "commandExecution";
        const outputField = stringMember(item, "aggregatedOutput") !== "" ? "aggregatedOutput" : "output";
        let output = withTruncationNotice(stringMember(item, outputField), omittedTextBytes(presentation, outputField), "command output", false);
        if (!terminalOutputHasVisibleText(output)) output = "";
        const exitCode = integerValue(item, "exitCode");
        const durationMilliseconds = integerValue(item, "durationMs") ?? integerValue(item, "duration_ms");
        payload = {
            command: stringMember(item, "command"), output, cwd: stringMember(item, "cwd"),
            ...(exitCode !== undefined ? {exitCode} : {}),
            ...(durationMilliseconds !== undefined ? {durationMilliseconds} : {}),
        } satisfies CommandExecutionData;
    } else if (type === "collabAgentToolCall" || type === "subAgentActivity") {
        kind = "agentActivity";
        payload = {
            tool: stringMember(item, "tool"), kind: stringMember(item, "kind"),
            prompt: stringMember(item, "prompt"), resultText: stringMember(item, "resultText"),
            receivers: stringList(member(item, "receiverThreadIds", [])).filter(Boolean), model: stringMember(item, "model"),
            reasoningEffort: stringMember(item, "reasoningEffort"), childThreadId: stringMember(item, "agentThreadId"),
            agentPath: stringMember(item, "agentPath"), senderThreadId: stringMember(item, "senderThreadId"),
        } satisfies AgentActivityData;
    } else if (type === "reasoning") {
        kind = "reasoning"; payload = {summary: withTruncationNotice(
            stringList(member(item, "summary", [])).join(", "), omittedTextBytes(presentation, "summary"), "reasoning", true)} satisfies ReasoningData;
    } else if (type === "fileChange") {
        kind = "fileChanges";
        const rawChanges = member(item, "changes", []);
        const changes: FileChangeData[] = Array.isArray(rawChanges) ? rawChanges.map(change => {
            const diff = stringMember(change, "diff");
            if (diff === "") return {path: stringMember(change, "path"), kind: stringMember(change, "kind")};
            const [additions, deletions] = unifiedDiffCounts(diff);
            return {path: stringMember(change, "path"), kind: stringMember(change, "kind"), additions, deletions};
        }) : [];
        payload = {changes, cwd: stringMember(item, "cwd") || threadCwd} satisfies FileChangesData;
    } else if (type === "imageGeneration" || type === "imageView") {
        kind = "imageGeneration";
        if (type === "imageView") status = statusFromValue("completed");
        payload = {
            path: stringMember(item, "path") || stringMember(item, "savedPath") || stringMember(item, "saved_path"),
            revisedPrompt: stringMember(item, "revisedPrompt") || stringMember(item, "revised_prompt"),
        } satisfies ImageGenerationData;
    } else if (type === "plan" && messageText(item) !== "") {
        kind = "plan"; payload = {explanation: "", steps: [], legacyText: withTruncationNotice(
            messageText(item), omittedTextBytes(presentation, "text"), "plan text", true)};
    } else payload = {type, displayDetail: genericActivityDetail(item)} satisfies GenericActivityData;
    return {key: visualKey, kind, threadId: identity.threadId, turnId: identity.turnId, itemId: identity.itemId,
        payload, status};
}
function sectionComponent(prefix: string, threadId: string, suffix: string): string {
    return `${prefix}${threadId.length}:${threadId}${suffix.length}:${suffix}`;
}
function admissionBoundaryPosition(anchor: AuthoritativeItemKey | undefined, atStart: boolean,
    index: AuthoritativeItemIndex): number | undefined {
    if (anchor) { const position = authoritativePosition(index, anchor); if (position !== undefined) return (position + 1) * 2; }
    return atStart ? 0 : undefined;
}
function submissionPosition(submission: PromptSubmission, index: AuthoritativeItemIndex, materialized?: number): number {
    const admitted = admissionBoundaryPosition(submission.admissionAnchor, submission.admissionAtStart, index);
    if (admitted !== undefined) return admitted;
    if (materialized !== undefined) return materialized * 2 + 1;
    return index.ordered.length * 2 + 2;
}
interface ProjectedNode {
    position: number; tieBreaker: number; sectionKey: string; turnId: string; turnRoot: boolean; card: VisibleCardData;
}

export function projectConversation(
    source: AuthoritativeItemIndex | ThreadPresentation,
    localSubmissions: readonly PromptSubmission[],
    authoritativeItemLimit: number,
    nowMilliseconds: number,
    thread?: ThreadPresentation,
): ConversationSnapshot {
    const authoritativeItems = "ordered" in source ? source : indexAuthoritativeItems(source.id, source);
    const authoritativeThread = "ordered" in source ? thread : source;
    const suffixStart = Math.max(0, authoritativeItems.ordered.length - authoritativeItemLimit);
    const pinnedRoots = new Set<number>();
    for (let index = suffixStart; index < authoritativeItems.ordered.length; ++index) {
        const root = authoritativeItems.turnRoots.get(authoritativeItems.ordered[index]!.key.turnId);
        if (root !== undefined && root < suffixStart) pinnedRoots.add(root);
    }
    const retainedPositions = [...pinnedRoots].sort((left, right) => left - right);
    for (let index = suffixStart; index < authoritativeItems.ordered.length; ++index) retainedPositions.push(index);
    const hidden = suffixStart - pinnedRoots.size;
    const result: ConversationSnapshot = {
        threadId: authoritativeItems.threadId, sections: [], hiddenAuthoritativeItemCount: hidden,
        hasMore: hidden > 0, activeTurnId: undefined,
    };
    const bindings = new Map<string, PromptSubmission>();
    for (const submission of localSubmissions) if (submission.materializedItem)
        bindings.set(`${submission.materializedItem.threadId}\0${submission.materializedItem.turnId}\0${submission.materializedItem.itemId}`, submission);
    const nodes: ProjectedNode[] = [];
    for (const index of retainedPositions) {
        const item = authoritativeItems.ordered[index]!;
        const identity = `${item.key.threadId}\0${item.key.turnId}\0${item.key.itemId}`;
        const binding = bindings.get(identity);
        if (binding && localCardVisible(binding)) continue;
        let visualKey: CardKey = item.promptAlias?.key ?? item.key;
        if (binding) visualKey = {kind: "prompt", submissionId: binding.id};
        let position = index * 2 + 1;
        let tieBreaker = 0;
        if (binding) { position = submissionPosition(binding, authoritativeItems, index); tieBreaker = binding.admissionOrdinal; }
        else if (item.promptAlias) {
            position = admissionBoundaryPosition(item.promptAlias.admissionAnchor,
                item.promptAlias.admissionAnchor === undefined, authoritativeItems) ?? position;
            tieBreaker = item.promptAlias.admissionOrdinal;
        }
        nodes.push({position, tieBreaker, sectionKey: sectionComponent("turn:", authoritativeItems.threadId, item.key.turnId),
            turnId: item.key.turnId, turnRoot: authoritativeItems.turnRoots.get(item.key.turnId) === index,
            card: authoritativeCard(item.key, item.presentation, visualKey, authoritativeThread?.cwd ?? "")});
    }
    for (const submission of localSubmissions) {
        if (!localCardVisible(submission)) continue;
        const materialized = submission.materializedItem
            ? authoritativePosition(authoritativeItems, submission.materializedItem) : undefined;
        const position = submissionPosition(submission, authoritativeItems, materialized);
        const knownTurn = authoritativeThread !== undefined && submission.expectedTurnId !== undefined
            && authoritativeThread.turns.has(submission.expectedTurnId);
        const turnId = submission.expectedTurnId ?? "";
        const sectionKey = knownTurn ? sectionComponent("turn:", authoritativeItems.threadId, turnId) : `pending:${submission.id}`;
        const payload: LocalPromptData = {
            submissionId: submission.id, prompt: submission.prompt,
            state: submission.state === "queued" ? "inFlight" : submission.state,
            showPendingAnimation: (submission.state === "queued" || submission.state === "inFlight")
                && nowMilliseconds - submission.admittedAtMilliseconds >= PendingAnimationDelayMilliseconds,
            error: submission.error, imagePaths: localImagePaths(submission),
            admittedAtMilliseconds: submission.admittedAtMilliseconds, requiresExplicitRecovery: false,
        };
        const turnRootPosition = authoritativeItems.turnRoots.get(turnId);
        const turnRoot = turnRootPosition !== undefined
            ? materialized === turnRootPosition : submission.startsTurn;
        nodes.push({position, tieBreaker: submission.admissionOrdinal, sectionKey, turnId, turnRoot, card: {
            key: {kind: "prompt", submissionId: submission.id}, kind: "localPrompt", threadId: authoritativeItems.threadId,
            turnId, itemId: "", payload, status: UnknownStatus,
        }});
    }
    nodes.sort((left, right) => left.position - right.position || left.tieBreaker - right.tieBreaker);
    const sectionIndexes = new Map<string, number>();
    for (const node of nodes) {
        let sectionIndex = sectionIndexes.get(node.sectionKey);
        if (sectionIndex === undefined) {
            sectionIndex = result.sections.length; sectionIndexes.set(node.sectionKey, sectionIndex);
            result.sections.push({key: node.sectionKey, turnId: node.turnId, cards: []});
        }
        const section = result.sections[sectionIndex]!;
        section.cards.push(node.card);
        if (node.turnRoot) section.rootCardKey = node.card.key;
    }
    return result;
}
