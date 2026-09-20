import type {PresentationStatus} from "../presentation/PresentationStatus.js";

export const PendingAnimationDelayMilliseconds = 1000;
export const AuthoritativeHistoryPageSize = 80;

export interface AuthoritativeItemKey {kind: "item"; threadId: string; turnId: string; itemId: string}
export interface LocalPromptKey {kind: "prompt"; submissionId: number}
export type CardKey = AuthoritativeItemKey | LocalPromptKey;
export type PromptState = "queued" | "inFlight" | "accepted" | "failed";
export type CardKind = "userMessage" | "agentMessage" | "commandExecution" | "agentActivity"
    | "reasoning" | "fileChanges" | "imageGeneration" | "plan" | "genericActivity" | "localPrompt";

export interface UserMessageData {text: string; imagePaths: string[]}
export interface AgentMessageData {text: string; finalAnswer: boolean}
export interface CommandExecutionData {
    command: string; output: string; cwd: string; exitCode?: number; durationMilliseconds?: number;
}
export interface AgentActivityData {
    tool: string; kind: string; prompt: string; resultText: string; receivers: string[];
    model: string; reasoningEffort: string; childThreadId: string; agentPath: string; senderThreadId: string;
}
export interface ReasoningData {summary: string}
export interface FileChangeData {path: string; kind: string; additions?: number; deletions?: number}
export interface FileChangesData {changes: FileChangeData[]; cwd: string}
export interface ImageGenerationData {path: string; revisedPrompt: string}
export interface PlanStepData {text: string; status: PresentationStatus}
export interface PlanData {explanation: string; steps: PlanStepData[]; legacyText: string}
export interface GenericActivityData {type: string; displayDetail: string}
export interface LocalPromptData {
    submissionId: number; prompt: string; state: PromptState; showPendingAnimation: boolean;
    error: string; imagePaths: string[]; admittedAtMilliseconds?: number; requiresExplicitRecovery: boolean;
}
export type CardPayload = UserMessageData | AgentMessageData | CommandExecutionData | AgentActivityData
    | ReasoningData | FileChangesData | ImageGenerationData | PlanData | GenericActivityData | LocalPromptData;
export interface VisibleCardData {
    key: CardKey; kind: CardKind; threadId: string; turnId: string; itemId: string; payload: CardPayload;
    status: PresentationStatus;
}
export interface TurnSection {key: string; turnId: string; cards: VisibleCardData[]; rootCardKey?: CardKey}
export interface ConversationSnapshot {
    threadId: string; sections: TurnSection[]; hiddenAuthoritativeItemCount: number; hasMore: boolean;
    activeTurnId: string | undefined;
}

function component(value: string): string { return `${value.length}:${value}`; }
export function stableKey(key: CardKey): string {
    if (key.kind === "item") return `item:${component(key.threadId)}${component(key.turnId)}${component(key.itemId)}`;
    return `prompt:${key.submissionId}`;
}

export function terminalOutputHasVisibleText(output: string): boolean {
    for (let index = 0; index < output.length; ++index) {
        const code = output.charCodeAt(index);
        if (code === 0x9b) {
            while (++index < output.length) { const c = output.charCodeAt(index); if (c >= 0x40 && c <= 0x7e) break; }
            continue;
        }
        if ([0x90, 0x98, 0x9d, 0x9e, 0x9f].includes(code)) {
            while (++index < output.length) {
                const c = output.charCodeAt(index);
                if (c === 0x07 || c === 0x9c) break;
                if (c === 0x1b && output[index + 1] === "\\") { ++index; break; }
            }
            continue;
        }
        if (code === 0x1b) {
            if (++index >= output.length) break;
            const introducer = output[index];
            if (introducer === "[") {
                while (++index < output.length) { const c = output.charCodeAt(index); if (c >= 0x40 && c <= 0x7e) break; }
                continue;
            }
            if ("]P^_X".includes(introducer ?? "")) {
                while (++index < output.length) {
                    const c = output.charCodeAt(index);
                    if (c === 0x07 || c === 0x9c) break;
                    if (c === 0x1b && output[index + 1] === "\\") { ++index; break; }
                }
                continue;
            }
            const introCode = introducer?.charCodeAt(0) ?? 0;
            if (introCode >= 0x20 && introCode <= 0x2f)
                while (++index < output.length) { const c = output.charCodeAt(index); if (c >= 0x30 && c <= 0x7e) break; }
            continue;
        }
        if (code > 0x1f && code !== 0x7f && !/\s/u.test(output[index] ?? "")) return true;
    }
    return false;
}

export function trimTrailingEmptyLines(text: string): string {
    let end = text.length;
    while (end > 0) {
        while (end > 0 && (text[end - 1] === "\n" || text[end - 1] === "\r")) --end;
        if (end === 0) break;
        let start = end;
        while (start > 0 && text[start - 1] !== "\n" && text[start - 1] !== "\r") --start;
        if (/[^\p{White_Space}\u001c-\u001f]/u.test(text.slice(start, end))) break;
        end = start;
    }
    return text.slice(0, end);
}

export function cardKeys(snapshot: ConversationSnapshot): CardKey[] {
    return snapshot.sections.flatMap(section => section.cards.map(card => card.key));
}

export function findCard(snapshot: ConversationSnapshot, key: CardKey): VisibleCardData | undefined {
    const identity = stableKey(key);
    return snapshot.sections.flatMap(section => section.cards).find(card => stableKey(card.key) === identity);
}
