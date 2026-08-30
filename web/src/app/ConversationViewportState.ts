import {AuthoritativeHistoryPageSize} from "../conversation/MiddleTypes.js";

interface ThreadViewport {
    requested: number;
    effective: number;
    lastAuthoritativeCount: number;
    scrollTop: number;
    following: boolean;
    anchor: ConversationViewportAnchor | undefined;
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

function initialViewport(): ThreadViewport {
    return {
        requested: AuthoritativeHistoryPageSize,
        effective: AuthoritativeHistoryPageSize,
        lastAuthoritativeCount: 0,
        scrollTop: 0,
        following: true,
        anchor: undefined,
    };
}

export class ConversationViewportState {
    private readonly byThread = new Map<string, ThreadViewport>();

    effectiveLimit(threadId: string, authoritativeCount: number): number {
        const state = this.state(threadId);
        if (!state.following && authoritativeCount > state.lastAuthoritativeCount)
            state.effective += authoritativeCount - state.lastAuthoritativeCount;
        else if (state.following) state.effective = state.requested;
        state.lastAuthoritativeCount = authoritativeCount;
        return state.effective;
    }

    loadMore(threadId: string): number {
        const state = this.state(threadId);
        state.requested += AuthoritativeHistoryPageSize;
        state.effective += AuthoritativeHistoryPageSize;
        return state.effective;
    }

    updateScroll(threadId: string, scrollTop: number, following: boolean, anchor?: ConversationViewportAnchor): void {
        const state = this.state(threadId);
        state.scrollTop = Math.max(0, scrollTop);
        state.following = following;
        state.anchor = anchor;
    }

    scroll(threadId: string): Readonly<{scrollTop: number; following: boolean; anchor?: ConversationViewportAnchor}> {
        const state = this.state(threadId);
        return {...(state.anchor ? {anchor: state.anchor} : {}), scrollTop: state.scrollTop, following: state.following};
    }

    clear(threadId: string): void { this.byThread.delete(threadId); }

    private state(threadId: string): ThreadViewport {
        let state = this.byThread.get(threadId);
        if (!state) { state = initialViewport(); this.byThread.set(threadId, state); }
        return state;
    }
}
