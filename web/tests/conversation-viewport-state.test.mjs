import assert from "node:assert/strict";
import test from "node:test";

import {ConversationViewportState, anchoredScrollTop, foldedCardScrollTop, nestedScrollConsumes} from "../dist/index.js";

test("paused-history and per-thread viewport rules", () => {
    const state = new ConversationViewportState();
    const a = state.bind("a", {});
    assert.equal(state.effectiveLimit("a", a, 80), 80);
    state.updateScroll("a", a, 320, false);
    assert.equal(state.effectiveLimit("a", a, 85), 85, "new tail items cannot evict a paused visual anchor");
    assert.deepEqual(state.scroll("a", a), {scrollTop: 320, following: false});
    state.updateScroll("a", a, 325, false, {cardKey: "item:anchor", pixelOffset: 14});
    assert.deepEqual(state.scroll("a", a), {
        anchor: {cardKey: "item:anchor", pixelOffset: 14}, scrollTop: 325, following: false,
    }, "paused threads retain a stable card/pixel anchor rather than only an absolute coordinate");
    assert.equal(state.loadMore("a", a), 165);
    assert.equal(state.effectiveLimit("a", a, 90), 170);

    const b = state.bind("b", {});
    assert.deepEqual(state.scroll("b", b), {scrollTop: 0, following: true}, "threads own independent viewport state");
    state.updateScroll("a", a, 0, true);
    assert.equal(state.effectiveLimit("a", a, 90), 160, "following restores the explicitly requested window");
    state.retire("a");
    assert.deepEqual(state.scroll("a", a), {scrollTop: 0, following: true});
});

test("exact presentation lifetime owns viewport, folds, promotion, and retirement", () => {
    const state = new ConversationViewportState();
    const firstOwner = {};
    const first = state.bind("same-id", firstOwner);
    state.updateScroll("same-id", first, 240, false, {cardKey: "card-a", pixelOffset: 9});
    assert.equal(state.cardCollapsed("same-id", first, "card-a", false), false);
    assert.equal(state.setCardCollapsed("same-id", first, "card-a", true), true);
    assert.equal(state.setThreadExpanded("same-id", first, true), true);
    assert.equal(state.bind("same-id", firstOwner), first, "ordinary mutation preserves identity");

    const replacement = state.bind("same-id", {});
    assert.notEqual(replacement, first, "same protocol ID cannot preserve a retired presentation lifetime");
    assert.deepEqual(state.scroll("same-id", replacement), {scrollTop: 0, following: true});
    assert.equal(state.cardCollapsed("same-id", replacement, "card-a", false), false);
    assert.equal(state.threadExpanded("same-id", replacement), false);
    state.updateScroll("same-id", first, 999, false);
    assert.deepEqual(state.scroll("same-id", replacement), {scrollTop: 0, following: true},
        "a delayed callback from the old lifetime is a semantic no-op");

    const draft = state.bind("draft", {});
    state.updateScroll("draft", draft, 80, false);
    state.setCardCollapsed("draft", draft, "prompt", true);
    assert.equal(state.promote("draft", "created", {}), draft);
    assert.deepEqual(state.scroll("created", draft), {scrollTop: 80, following: false});
    assert.equal(state.cardCollapsed("created", draft, "prompt", false), true);
    assert.equal(state.threadBindingCount(), 2);
    assert.equal(state.retainedPresentationCount(), 2, "draft promotion moves one state rather than cloning it");
    assert.equal(state.presentationKey("draft"), undefined);
    assert.deepEqual(state.scroll("created", draft), {scrollTop: 80, following: false},
        "moving the draft state cannot retire the canonical viewport");
    const validTarget = state.bind("valid-target", {});
    assert.equal(state.promote("missing-draft", "valid-target", {}), undefined);
    assert.equal(state.presentationKey("valid-target"), validTarget,
        "an absent draft promotion is a complete no-op");
    state.retire("created");
    assert.equal(state.presentationKey("created"), undefined);
});

test("presentation and tree-fold state remain bounded under retirement churn", () => {
    const state = new ConversationViewportState();
    for (let index = 0; index < 10_000; ++index) {
        const id = `retired-${index}`;
        const key = state.bind(id, {});
        state.setThreadExpanded(id, key, true);
        assert.equal(state.retire(id), key);
    }
    assert.equal(state.threadBindingCount(), 0);
    assert.equal(state.retainedPresentationCount(), 0);
});

test("anchor and folding geometry follows the title rules", () => {
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
