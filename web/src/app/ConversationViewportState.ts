import {AuthoritativeHistoryPageSize} from "../conversation/MiddleTypes.js";

interface ThreadViewport {
    requested: number;
    effective: number;
    lastAuthoritativeCount: number;
    scrollTop: number;
    following: boolean;
}

function initialViewport(): ThreadViewport {
    return {
        requested: AuthoritativeHistoryPageSize,
        effective: AuthoritativeHistoryPageSize,
        lastAuthoritativeCount: 0,
        scrollTop: 0,
        following: true,
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

    updateScroll(threadId: string, scrollTop: number, following: boolean): void {
        const state = this.state(threadId);
        state.scrollTop = Math.max(0, scrollTop);
        state.following = following;
    }

    scroll(threadId: string): Readonly<{scrollTop: number; following: boolean}> {
        const state = this.state(threadId);
        return {scrollTop: state.scrollTop, following: state.following};
    }

    clear(threadId: string): void { this.byThread.delete(threadId); }

    private state(threadId: string): ThreadViewport {
        let state = this.byThread.get(threadId);
        if (!state) { state = initialViewport(); this.byThread.set(threadId, state); }
        return state;
    }
}
