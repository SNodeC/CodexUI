import type {ItemPresentation, ThreadPresentation} from "../presentation/PresentationModel.js";
import {isObject, member, stringMember} from "../presentation/PresentationProtocol.js";
import {AuthoritativeHistoryPageSize, terminalOutputHasVisibleText} from "./MiddleTypes.js";
import type {
    AgentActivityData, AgentMessageData, AuthoritativeItemKey, CardKey, CardKind, CardPayload,
    CommandExecutionData, ConversationSnapshot, FileChangeData, FileChangesData, GenericActivityData,
    ImageGenerationData, LocalPromptData, ReasoningData, UserMessageData, VisibleCardData,
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
function authoritativeCard(identity: AuthoritativeItemKey, presentation: ItemPresentation, visualKey: CardKey): VisibleCardData {
    const item = presentation.raw;
    const type = stringMember(item, "type");
    let kind: CardKind = "genericActivity";
    let payload: CardPayload = {type, raw: structuredClone(item)} satisfies GenericActivityData;
    if (type === "userMessage") {
        kind = "userMessage"; payload = {text: messageText(item), imagePaths: messageImagePaths(item)} satisfies UserMessageData;
    } else if (type === "agentMessage") {
        kind = "agentMessage";
        payload = {text: messageText(item), finalAnswer: stringMember(item, "phase") === "final_answer"} satisfies AgentMessageData;
    } else if (type === "commandExecution") {
        kind = "commandExecution";
        let output = stringMember(item, "aggregatedOutput") || stringMember(item, "output");
        if (!terminalOutputHasVisibleText(output)) output = "";
        const exitCode = integerValue(item, "exitCode");
        const durationMilliseconds = integerValue(item, "durationMs") ?? integerValue(item, "duration_ms");
        payload = {
            command: stringMember(item, "command"), output, status: stringMember(item, "status"), cwd: stringMember(item, "cwd"),
            ...(exitCode !== undefined ? {exitCode} : {}),
            ...(durationMilliseconds !== undefined ? {durationMilliseconds} : {}),
        } satisfies CommandExecutionData;
    } else if (type === "collabAgentToolCall" || type === "subAgentActivity") {
        kind = "agentActivity";
        payload = {
            tool: stringMember(item, "tool"), status: stringMember(item, "status"), kind: stringMember(item, "kind"),
            prompt: stringMember(item, "prompt"), resultText: stringMember(item, "resultText"),
            receivers: stringList(member(item, "receiverThreadIds", [])), model: stringMember(item, "model"),
            reasoningEffort: stringMember(item, "reasoningEffort"), childThreadId: stringMember(item, "agentThreadId"),
            agentPath: stringMember(item, "agentPath"), senderThreadId: stringMember(item, "senderThreadId"),
        } satisfies AgentActivityData;
    } else if (type === "reasoning") {
        kind = "reasoning"; payload = {summary: stringList(member(item, "summary", [])).join(", ")} satisfies ReasoningData;
    } else if (type === "fileChange") {
        kind = "fileChanges";
        const rawChanges = member(item, "changes", []);
        const changes: FileChangeData[] = Array.isArray(rawChanges) ? rawChanges.map(change => {
            const diff = stringMember(change, "diff");
            if (diff === "") return {path: stringMember(change, "path"), kind: stringMember(change, "kind")};
            const [additions, deletions] = unifiedDiffCounts(diff);
            return {path: stringMember(change, "path"), kind: stringMember(change, "kind"), additions, deletions};
        }) : [];
        payload = {status: stringMember(item, "status"), changes} satisfies FileChangesData;
    } else if (type === "imageGeneration" || type === "imageView") {
        kind = "imageGeneration";
        payload = {
            path: stringMember(item, "path") || stringMember(item, "savedPath") || stringMember(item, "saved_path"),
            status: stringMember(item, "status"),
            revisedPrompt: stringMember(item, "revisedPrompt") || stringMember(item, "revised_prompt"),
        } satisfies ImageGenerationData;
    } else if (type === "plan" && messageText(item) !== "") {
        kind = "plan"; payload = {explanation: "", steps: [], legacyText: messageText(item)};
    }
    return {key: visualKey, kind, threadId: identity.threadId, turnId: identity.turnId, itemId: identity.itemId, payload};
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
    position: number; tieBreaker: number; sectionKey: string; turnId: string; card: VisibleCardData;
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
    const hidden = Math.max(0, authoritativeItems.ordered.length - authoritativeItemLimit);
    const result: ConversationSnapshot = {
        threadId: authoritativeItems.threadId, sections: [], hiddenAuthoritativeItemCount: hidden, hasMore: hidden > 0,
    };
    const bindings = new Map<string, PromptSubmission>();
    for (const submission of localSubmissions) if (submission.materializedItem)
        bindings.set(`${submission.materializedItem.threadId}\0${submission.materializedItem.turnId}\0${submission.materializedItem.itemId}`, submission);
    const nodes: ProjectedNode[] = [];
    for (let index = hidden; index < authoritativeItems.ordered.length; ++index) {
        const item = authoritativeItems.ordered[index]!;
        const identity = `${item.key.threadId}\0${item.key.turnId}\0${item.key.itemId}`;
        const binding = bindings.get(identity);
        if (binding && localCardVisible(binding, nowMilliseconds)) continue;
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
            turnId: item.key.turnId, card: authoritativeCard(item.key, item.presentation, visualKey)});
    }
    for (const submission of localSubmissions) {
        if (!localCardVisible(submission, nowMilliseconds)) continue;
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
            acceptedAtMilliseconds: submission.acceptedAtMilliseconds, error: submission.error,
            imagePaths: localImagePaths(submission),
        };
        nodes.push({position, tieBreaker: submission.admissionOrdinal, sectionKey, turnId, card: {
            key: {kind: "prompt", submissionId: submission.id}, kind: "localPrompt", threadId: authoritativeItems.threadId,
            turnId, itemId: "", payload,
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
        result.sections[sectionIndex]!.cards.push(node.card);
    }
    return result;
}
