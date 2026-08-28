import type {ThreadPresentation, ItemPresentation} from "../presentation/PresentationModel.js";
import {isObject, stringMember} from "../presentation/PresentationProtocol.js";
import {AcknowledgementTransitionMilliseconds} from "./MiddleTypes.js";
import type {AuthoritativeItemKey, LocalPromptKey, PromptState} from "./MiddleTypes.js";

export interface AttachmentDraft {path: string; name: string; mimeType: string; size: number}
export interface PromptSubmission {
    id: number; admissionOrdinal: number; threadId: string; clientUserMessageId: string; prompt: string;
    attachments: AttachmentDraft[]; turnOptions: Record<string, unknown>; state: PromptState;
    acceptedAtMilliseconds: number; error: string; admissionAnchor?: AuthoritativeItemKey;
    admissionAtStart: boolean; expectedTurnId?: string; materializedItem?: AuthoritativeItemKey;
}
export interface PromptDispatch {
    id: number; threadId: string; clientUserMessageId: string; prompt: string; attachments: AttachmentDraft[];
    turnOptions: Record<string, unknown>; expectedTurnId?: string;
}
export interface PromptVisualAlias {key: LocalPromptKey; admissionAnchor?: AuthoritativeItemKey; admissionOrdinal: number}
interface RetainedPromptVisualAlias extends PromptVisualAlias {materializedItem: AuthoritativeItemKey}
export interface AuthoritativeItem {key: AuthoritativeItemKey; presentation: ItemPresentation; promptAlias?: PromptVisualAlias}
export interface UserMessageByText {turnId: string; text: string; position: number}
export interface AuthoritativeItemIndex {
    threadId: string; ordered: AuthoritativeItem[]; positions: Map<string, number>;
    userMessagesByClientId: Map<string, number>; userMessagesByText: UserMessageByText[];
}

export function authoritativeKey(key: AuthoritativeItemKey): string {
    return `${key.threadId.length}:${key.threadId}${key.turnId.length}:${key.turnId}${key.itemId.length}:${key.itemId}`;
}
export function authoritativePosition(index: AuthoritativeItemIndex, key: AuthoritativeItemKey): number | undefined {
    return index.positions.get(authoritativeKey(key));
}
function userMessageText(item: unknown): string {
    const parts: string[] = [];
    const content = isObject(item) ? item.content : undefined;
    if (Array.isArray(content)) for (const entry of content) {
        const text = stringMember(entry, "text");
        if (text !== "") parts.push(text);
    }
    if (parts.length === 0) { const fallback = stringMember(item, "text"); if (fallback !== "") parts.push(fallback); }
    return parts.join("\n");
}
export function indexAuthoritativeItems(threadId: string, thread?: ThreadPresentation): AuthoritativeItemIndex {
    const result: AuthoritativeItemIndex = {
        threadId, ordered: [], positions: new Map(), userMessagesByClientId: new Map(), userMessagesByText: [],
    };
    if (!thread) return result;
    for (const turnId of thread.turnOrder) {
        const turn = thread.turns.get(turnId);
        if (!turn) continue;
        for (const itemId of turn.itemOrder) {
            const presentation = turn.items.get(itemId);
            if (!presentation) continue;
            const position = result.ordered.length;
            const key: AuthoritativeItemKey = {kind: "item", threadId, turnId, itemId};
            result.ordered.push({key, presentation});
            result.positions.set(authoritativeKey(key), position);
            if (stringMember(presentation.raw, "type") === "userMessage") {
                const clientId = stringMember(presentation.raw, "clientId");
                if (clientId !== "" && !result.userMessagesByClientId.has(clientId))
                    result.userMessagesByClientId.set(clientId, position);
                const text = userMessageText(presentation.raw).trim();
                result.userMessagesByText.push({turnId: "", text, position}, {turnId, text, position});
            }
        }
    }
    result.userMessagesByText.sort((a, b) => a.turnId.localeCompare(b.turnId) || a.text.localeCompare(b.text) || a.position - b.position);
    return result;
}

export function promptWithFileLinks(prompt: string, attachments: readonly AttachmentDraft[]): string {
    const links = attachments.filter(file => !file.mimeType.startsWith("image/") && !file.mimeType.startsWith("audio/"))
        .map(file => {
            const label = file.name.replaceAll("\\", "\\\\").replaceAll("[", "\\[").replaceAll("]", "\\]")
                .replace(/[\r\n]/gu, " ");
            const target = new URL(`file://${file.path.startsWith("/") ? "" : "/"}${file.path}`).href
                .replaceAll("[", "%5B").replaceAll("]", "%5D").replaceAll("(", "%28").replaceAll(")", "%29");
            return `- [${label}](${target})`;
        });
    return links.length === 0 ? prompt : `${prompt}\n\nAttached files:\n${links.join("\n")}`;
}

export function acceptedTransitionActive(submission: PromptSubmission, now: number): boolean {
    return submission.state === "accepted" && submission.acceptedAtMilliseconds > 0
        && now >= submission.acceptedAtMilliseconds
        && now - submission.acceptedAtMilliseconds < AcknowledgementTransitionMilliseconds;
}
export function localCardVisible(submission: PromptSubmission, now: number): boolean {
    return submission.state === "queued" || submission.state === "inFlight" || submission.state === "failed"
        || submission.materializedItem === undefined || acceptedTransitionActive(submission, now);
}
function cloneKey(key: AuthoritativeItemKey): AuthoritativeItemKey { return {...key}; }

export class PromptCoordinator {
    private readonly byThread = new Map<string, PromptSubmission[]>();
    private readonly visualAliasesByThread = new Map<string, Map<string, RetainedPromptVisualAlias>>();
    private nextSubmissionId = 1;
    private nextAdmissionOrdinal = 1;

    admit(threadId: string, prompt: string, attachments: AttachmentDraft[], turnOptions: Record<string, unknown>,
        authoritativeThread: ThreadPresentation | undefined, activeTurnId: string | undefined, now: number): number {
        const submission: PromptSubmission = {
            id: this.nextSubmissionId++, admissionOrdinal: this.nextAdmissionOrdinal++, threadId,
            clientUserMessageId: `codexui-${now}-${this.nextSubmissionId - 1}`, prompt, attachments: structuredClone(attachments),
            turnOptions: structuredClone(turnOptions), state: "queued", acceptedAtMilliseconds: 0, error: "",
            admissionAtStart: false,
        };
        if (activeTurnId !== undefined) submission.expectedTurnId = activeTurnId;
        if (authoritativeThread) {
            const items = indexAuthoritativeItems(threadId, authoritativeThread);
            const last = items.ordered.at(-1);
            if (last) submission.admissionAnchor = cloneKey(last.key);
        }
        const list = this.byThread.get(threadId) ?? [];
        if (!this.byThread.has(threadId)) this.byThread.set(threadId, list);
        list.push(submission);
        return submission.id;
    }

    beginNext(threadId: string, activeTurnId?: string): PromptDispatch | undefined {
        const list = this.byThread.get(threadId);
        if (!list || list.some(value => value.state === "inFlight")) return undefined;
        const next = list.find(value => value.state === "queued");
        if (!next) return undefined;
        next.admissionAtStart = next.admissionAnchor === undefined;
        next.state = "inFlight";
        if (activeTurnId === undefined) delete next.expectedTurnId; else next.expectedTurnId = activeTurnId;
        const dispatch: PromptDispatch = {
            id: next.id, threadId: next.threadId, clientUserMessageId: next.clientUserMessageId,
            prompt: next.prompt, attachments: structuredClone(next.attachments), turnOptions: structuredClone(next.turnOptions),
        };
        if (next.expectedTurnId !== undefined) dispatch.expectedTurnId = next.expectedTurnId;
        return dispatch;
    }

    acknowledge(threadId: string, id: number, turnId: string | undefined, now: number): boolean {
        const pending = this.find(threadId, id);
        if (!pending || pending.state !== "inFlight") return false;
        pending.state = "accepted"; pending.acceptedAtMilliseconds = now; pending.error = "";
        if (turnId !== undefined) pending.expectedTurnId = turnId;
        return true;
    }
    fail(threadId: string, id: number, error: string): boolean {
        const pending = this.find(threadId, id);
        if (!pending || (pending.state !== "inFlight" && pending.state !== "queued")) return false;
        pending.state = "failed"; pending.error = error; return true;
    }
    requeue(threadId: string, id: number): boolean {
        const pending = this.find(threadId, id);
        if (!pending || pending.state !== "inFlight") return false;
        pending.state = "queued"; pending.admissionAtStart = false; return true;
    }
    failQueued(threadId: string, error: string): number {
        let count = 0;
        for (const pending of this.byThread.get(threadId) ?? []) if (pending.state === "queued") {
            pending.state = "failed"; pending.error = error; ++count;
        }
        return count;
    }
    reassignThread(from: string, to: string): boolean {
        if (from === to) return true;
        const source = this.byThread.get(from);
        const target = this.byThread.get(to) ?? [];
        if (source?.some(value => value.state === "inFlight") && target.some(value => value.state === "inFlight")) return false;
        if (source) {
            this.byThread.delete(from);
            for (const submission of source) {
                submission.threadId = to;
                if (submission.admissionAnchor) submission.admissionAnchor.threadId = to;
                if (submission.materializedItem) submission.materializedItem.threadId = to;
            }
            target.push(...source); target.sort((a, b) => a.admissionOrdinal - b.admissionOrdinal); this.byThread.set(to, target);
        }
        const aliases = this.visualAliasesByThread.get(from);
        if (aliases) {
            this.visualAliasesByThread.delete(from);
            const destination = this.visualAliasesByThread.get(to) ?? new Map();
            for (const alias of aliases.values()) {
                alias.materializedItem.threadId = to;
                if (alias.admissionAnchor) alias.admissionAnchor.threadId = to;
                destination.set(authoritativeKey(alias.materializedItem), alias);
            }
            this.visualAliasesByThread.set(to, destination);
        }
        return true;
    }

    reconcile(threadId: string, threadOrIndex: ThreadPresentation | AuthoritativeItemIndex, now: number): AuthoritativeItemIndex {
        const index = "ordered" in threadOrIndex ? threadOrIndex : indexAuthoritativeItems(threadId, threadOrIndex);
        this.applyVisualAliases(threadId, index);
        const submissions = this.byThread.get(threadId);
        if (!submissions) return index;
        const claimed = index.ordered.map(item => item.promptAlias !== undefined);
        for (const submission of submissions) if (submission.materializedItem) {
            const position = authoritativePosition(index, submission.materializedItem); if (position !== undefined) claimed[position] = true;
        }
        for (const submission of submissions) {
            if (submission.materializedItem) continue;
            if (!submission.admissionAnchor && submission.state === "queued" && index.ordered.length > 0) {
                submission.admissionAnchor = cloneKey(index.ordered.at(-1)!.key); submission.admissionAtStart = false;
            }
            const exact = index.userMessagesByClientId.get(submission.clientUserMessageId);
            if (exact === undefined || claimed[exact]) continue;
            submission.materializedItem = cloneKey(index.ordered[exact]!.key);
            submission.expectedTurnId = submission.materializedItem.turnId;
            claimed[exact] = true;
        }
        for (const submission of submissions) {
            if (submission.materializedItem || submission.state !== "accepted") continue;
            let first = 0;
            if (submission.admissionAnchor) first = (authoritativePosition(index, submission.admissionAnchor) ?? -1) + 1;
            const turnId = submission.expectedTurnId ?? "";
            const candidate = index.userMessagesByText.find(value => value.turnId === turnId
                && value.text === submission.prompt.trim() && value.position >= first && !claimed[value.position]);
            if (!candidate) continue;
            submission.materializedItem = cloneKey(index.ordered[candidate.position]!.key);
            submission.expectedTurnId ??= submission.materializedItem.turnId;
            claimed[candidate.position] = true;
        }
        const aliases = this.visualAliasesByThread.get(threadId) ?? new Map<string, RetainedPromptVisualAlias>();
        for (const submission of submissions) {
            if (submission.state !== "accepted" || !submission.materializedItem || acceptedTransitionActive(submission, now)) continue;
            aliases.set(authoritativeKey(submission.materializedItem), {
                key: {kind: "prompt", submissionId: submission.id}, admissionOrdinal: submission.admissionOrdinal,
                materializedItem: cloneKey(submission.materializedItem),
                ...(submission.admissionAnchor ? {admissionAnchor: cloneKey(submission.admissionAnchor)} : {}),
            });
        }
        this.visualAliasesByThread.set(threadId, aliases);
        this.byThread.set(threadId, submissions.filter(submission => submission.state !== "accepted"
            || !submission.materializedItem || acceptedTransitionActive(submission, now)));
        this.applyVisualAliases(threadId, index);
        return index;
    }

    submissions(threadId: string): readonly PromptSubmission[] { return this.byThread.get(threadId) ?? []; }
    submission(threadId: string, id: number): PromptSubmission | undefined { return this.find(threadId, id); }
    hasInFlight(threadId: string): boolean { return this.submissions(threadId).some(value => value.state === "inFlight"); }
    queuedThreadIds(): string[] {
        return [...this.byThread].filter(([, values]) => values.some(value => value.state === "queued")).map(([id]) => id).sort();
    }
    clearThread(threadId: string): void { this.byThread.delete(threadId); this.visualAliasesByThread.delete(threadId); }
    private find(threadId: string, id: number): PromptSubmission | undefined {
        return this.byThread.get(threadId)?.find(value => value.id === id);
    }
    private applyVisualAliases(threadId: string, index: AuthoritativeItemIndex): void {
        const aliases = this.visualAliasesByThread.get(threadId);
        if (!aliases) return;
        for (const [key, retained] of aliases) {
            const position = index.positions.get(key);
            if (position !== undefined) index.ordered[position]!.promptAlias = retained;
        }
    }
}
