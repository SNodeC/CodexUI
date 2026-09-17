import {AuthoritativeHistoryPageSize} from "../conversation/MiddleTypes.js";

interface ThreadViewport {
    identity: string;
    owner: object;
    requested: number;
    effective: number;
    lastAuthoritativeCount: number;
    scrollTop: number;
    following: boolean;
    anchor: ConversationViewportAnchor | undefined;
    foldedCards: Map<string, boolean>;
    expanded: boolean;
}

export interface ConversationViewportAnchor {cardKey: string; pixelOffset: number}

export function anchoredScrollTop(cardContentTop: number, pixelOffset: number, maximum: number): number {
    return Math.max(0, Math.min(maximum, cardContentTop - pixelOffset));
}

export function foldedCardScrollTop(cardContentTop: number, previousTitleTop: number, cardHeight: number,
    visibleHeight: number, collapsed: boolean, maximum: number): number {
    const visibleTop = collapsed ? previousTitleTop
        : Math.max(0, Math.min(previousTitleTop, Math.max(0, visibleHeight - cardHeight)));
    return Math.max(0, Math.min(maximum, cardContentTop - visibleTop));
}

export function nestedScrollConsumes(deltaY: number, scrollTop: number, clientHeight: number, scrollHeight: number): boolean {
    if (deltaY < 0) return scrollTop > 0;
    if (deltaY > 0) return scrollTop + clientHeight < scrollHeight - 1;
    return false;
}

function initialViewport(identity: string, owner: object): ThreadViewport {
    return {
        identity,
        owner,
        requested: AuthoritativeHistoryPageSize,
        effective: AuthoritativeHistoryPageSize,
        lastAuthoritativeCount: 0,
        scrollTop: 0,
        following: true,
        anchor: undefined,
        foldedCards: new Map(),
        expanded: false,
    };
}

export class ConversationViewportState {
    private readonly byThread = new Map<string, ThreadViewport>();
    private nextIdentity = 1;

    bind(threadId: string, owner: object): string {
        const current = this.byThread.get(threadId);
        if (current?.owner === owner) return current.identity;
        if (current) this.retire(threadId);
        const state = initialViewport(`thread-presentation-${this.nextIdentity++}`, owner);
        this.byThread.set(threadId, state);
        return state.identity;
    }

    presentationKey(threadId: string): string | undefined {
        return this.byThread.get(threadId)?.identity;
    }

    promote(fromThreadId: string, toThreadId: string, owner: object): string | undefined {
        const state = this.byThread.get(fromThreadId);
        if (!state) return;
        const replaced = this.byThread.get(toThreadId);
        if (replaced && replaced !== state) this.retire(toThreadId);
        state.owner = owner;
        if (fromThreadId !== toThreadId) this.byThread.delete(fromThreadId);
        this.byThread.set(toThreadId, state);
        return state.identity;
    }

    retire(threadId: string): string | undefined {
        const state = this.byThread.get(threadId);
        if (!state) return;
        this.byThread.delete(threadId);
        return state.identity;
    }

    retireExcept(threadId: string): readonly string[] {
        const retained = this.byThread.get(threadId);
        const retired = new Set<string>();
        for (const state of this.byThread.values())
            if (state !== retained) retired.add(state.identity);
        this.byThread.clear();
        if (retained) this.byThread.set(threadId, retained);
        return [...retired];
    }

    clear(): void { this.byThread.clear(); }

    retainedPresentationCount(): number { return new Set(this.byThread.values()).size; }
    threadBindingCount(): number { return this.byThread.size; }

    effectiveLimit(threadId: string, presentationKey: string, authoritativeCount: number): number {
        const state = this.state(threadId, presentationKey);
        if (!state) return AuthoritativeHistoryPageSize;
        if (!state.following && authoritativeCount > state.lastAuthoritativeCount)
            state.effective += authoritativeCount - state.lastAuthoritativeCount;
        else if (state.following) state.effective = state.requested;
        state.lastAuthoritativeCount = authoritativeCount;
        return state.effective;
    }

    loadMore(threadId: string, presentationKey: string): number {
        const state = this.state(threadId, presentationKey);
        if (!state) return AuthoritativeHistoryPageSize;
        state.requested += AuthoritativeHistoryPageSize;
        state.effective += AuthoritativeHistoryPageSize;
        return state.effective;
    }

    updateScroll(threadId: string, presentationKey: string, scrollTop: number, following: boolean,
        anchor?: ConversationViewportAnchor): void {
        const state = this.state(threadId, presentationKey);
        if (!state) return;
        state.scrollTop = Math.max(0, scrollTop);
        state.following = following;
        state.anchor = anchor;
    }

    scroll(threadId: string, presentationKey: string): Readonly<{
        scrollTop: number; following: boolean; anchor?: ConversationViewportAnchor;
    }> {
        const state = this.state(threadId, presentationKey);
        if (!state) return {scrollTop: 0, following: true};
        return {...(state.anchor ? {anchor: state.anchor} : {}), scrollTop: state.scrollTop, following: state.following};
    }

    cardCollapsed(threadId: string, presentationKey: string, cardKey: string, initiallyCollapsed: boolean): boolean {
        const state = this.state(threadId, presentationKey);
        if (!state) return initiallyCollapsed;
        if (!state.foldedCards.has(cardKey)) state.foldedCards.set(cardKey, initiallyCollapsed);
        return state.foldedCards.get(cardKey) ?? initiallyCollapsed;
    }

    setCardCollapsed(threadId: string, presentationKey: string, cardKey: string, collapsed: boolean): boolean {
        const state = this.state(threadId, presentationKey);
        if (!state) return false;
        state.foldedCards.set(cardKey, collapsed);
        return true;
    }

    threadExpanded(threadId: string, presentationKey: string): boolean {
        return this.state(threadId, presentationKey)?.expanded ?? false;
    }

    setThreadExpanded(threadId: string, presentationKey: string, expanded: boolean): boolean {
        const state = this.state(threadId, presentationKey);
        if (!state) return false;
        state.expanded = expanded;
        return true;
    }

    private state(threadId: string, presentationKey: string): ThreadViewport | undefined {
        const state = this.byThread.get(threadId);
        return state?.identity === presentationKey ? state : undefined;
    }
}
