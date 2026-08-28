import assert from "node:assert/strict";
import test from "node:test";

import {ConversationViewportState} from "../dist/index.js";

test("native paused-history and per-thread viewport rules", () => {
    const state = new ConversationViewportState();
    assert.equal(state.effectiveLimit("a", 80), 80);
    state.updateScroll("a", 320, false);
    assert.equal(state.effectiveLimit("a", 85), 85, "new tail items cannot evict a paused visual anchor");
    assert.deepEqual(state.scroll("a"), {scrollTop: 320, following: false});
    assert.equal(state.loadMore("a"), 165);
    assert.equal(state.effectiveLimit("a", 90), 170);

    assert.deepEqual(state.scroll("b"), {scrollTop: 0, following: true}, "threads own independent viewport state");
    state.updateScroll("a", 0, true);
    assert.equal(state.effectiveLimit("a", 90), 160, "following restores the explicitly requested window");
    state.clear("a");
    assert.deepEqual(state.scroll("a"), {scrollTop: 0, following: true});
});
