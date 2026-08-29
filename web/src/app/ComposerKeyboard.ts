export interface ComposerKeyState {
    key: string;
    altKey: boolean;
    ctrlKey: boolean;
    metaKey: boolean;
    shiftKey: boolean;
    repeat: boolean;
    isComposing: boolean;
}

export function shouldSubmitPromptFromKey(state: ComposerKeyState): boolean {
    return state.key === "Enter" && !state.altKey && !state.shiftKey && !state.repeat && !state.isComposing;
}
