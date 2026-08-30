import assert from "node:assert/strict";
import test from "node:test";

import {ConversationViewportState, anchoredScrollTop, foldedCardScrollTop, nestedScrollConsumes} from "../dist/index.js";

test("native paused-history and per-thread viewport rules", () => {
    const state = new ConversationViewportState();
    assert.equal(state.effectiveLimit("a", 80), 80);
    state.updateScroll("a", 320, false);
    assert.equal(state.effectiveLimit("a", 85), 85, "new tail items cannot evict a paused visual anchor");
    assert.deepEqual(state.scroll("a"), {scrollTop: 320, following: false});
    state.updateScroll("a", 325, false, {cardKey: "item:anchor", pixelOffset: 14});
    assert.deepEqual(state.scroll("a"), {
        anchor: {cardKey: "item:anchor", pixelOffset: 14}, scrollTop: 325, following: false,
    }, "paused threads retain a stable card/pixel anchor rather than only an absolute coordinate");
    assert.equal(state.loadMore("a"), 165);
    assert.equal(state.effectiveLimit("a", 90), 170);

    assert.deepEqual(state.scroll("b"), {scrollTop: 0, following: true}, "threads own independent viewport state");
    state.updateScroll("a", 0, true);
    assert.equal(state.effectiveLimit("a", 90), 160, "following restores the explicitly requested window");
    state.clear("a");
    assert.deepEqual(state.scroll("a"), {scrollTop: 0, following: true});
});

test("anchor and folding geometry follows the native title rules", () => {
    assert.equal(anchoredScrollTop(640, 40, 1000), 600, "the same header pixel is restored");
    assert.equal(anchoredScrollTop(20, 40, 1000), 0, "natural range clamping wins at the top");
    assert.equal(anchoredScrollTop(1200, 40, 900), 900, "natural range clamping wins at the bottom");

    assert.equal(foldedCardScrollTop(700, 120, 40, 500, true, 1000), 580,
        "collapse leaves the title at its prior pixel");
    assert.equal(foldedCardScrollTop(700, 420, 260, 500, false, 1000), 460,
        "expansion moves only enough to reveal a card that fits");
    assert.equal(foldedCardScrollTop(700, 120, 700, 500, false, 1000), 700,
        "an oversized expansion aligns its title with the visible top");

    assert.equal(nestedScrollConsumes(30, 20, 100, 300), true, "an inner surface owns a gesture while it can move");
    assert.equal(nestedScrollConsumes(-30, 0, 100, 300), false, "upward motion transfers at the top boundary");
    assert.equal(nestedScrollConsumes(30, 200, 100, 300), false, "downward motion transfers at the bottom boundary");
});
