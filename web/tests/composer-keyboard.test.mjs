import assert from "node:assert/strict";
import test from "node:test";

import {shouldSubmitPromptFromKey} from "../dist/app/ComposerKeyboard.js";

const keyState = overrides => ({
    key: "Enter",
    altKey: false,
    ctrlKey: false,
    metaKey: false,
    shiftKey: false,
    repeat: false,
    isComposing: false,
    ...overrides,
});

test("composer keyboard policy submits Enter aliases only when safe", () => {
    assert.equal(shouldSubmitPromptFromKey(keyState({})), true, "Enter submits");
    assert.equal(shouldSubmitPromptFromKey(keyState({ctrlKey: true})), true, "Control+Enter remains an alias");
    assert.equal(shouldSubmitPromptFromKey(keyState({metaKey: true})), true, "Meta+Enter remains an alias");

    assert.equal(shouldSubmitPromptFromKey(keyState({shiftKey: true})), false, "Shift+Enter remains a newline");
    assert.equal(shouldSubmitPromptFromKey(keyState({ctrlKey: true, shiftKey: true})), false, "Shift wins over Control");
    assert.equal(shouldSubmitPromptFromKey(keyState({altKey: true})), false, "Alt+Enter is not captured");
    assert.equal(shouldSubmitPromptFromKey(keyState({repeat: true})), false, "auto-repeat does not submit");
    assert.equal(shouldSubmitPromptFromKey(keyState({isComposing: true})), false, "IME candidate confirmation does not submit");
    assert.equal(shouldSubmitPromptFromKey(keyState({key: "a"})), false, "other keys do not submit");
});
