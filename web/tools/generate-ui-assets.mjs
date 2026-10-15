import {readFileSync, writeFileSync} from "node:fs";
import {fileURLToPath} from "node:url";
import {resolve} from "node:path";

// Browser semantic roles project native UiStyle; values are never maintained twice.
const roles = {
    "@overlay": "drawer-backdrop modal-backdrop",
    "@pendingPromptSweepCenter": "pending-prompt-sweep",
    "@pendingSteeringSweepCenter": "pending-steering-sweep",
    "Palette::blue": "reasoning-accent",
    "Palette::blueBorder": "activity-spinner-track presentation-selected-border",
    "Palette::green": "file-changes-accent",
    "Palette::orangeBorderStrong": "request-action-hover-border",
    "Palette::orangeSurfaceHover": "request-action-hover-surface",
    "Palette::orangeText": "attention-text notice-text request-action-text request-detail-label",
    "activeGroupBorder": "active-group-border",
    "activeGroupHeaderSurface": "active-group-header-surface",
    "activeGroupSurface": "active-group-surface",
    "activeTurnBorder": "active-turn-border",
    "appBackground": "app-background conversation-surface",
    "attentionBorder": "request-action-border request-border",
    "attentionSurface": "request-action-surface request-surface",
    "codeSurface": "code-surface",
    "codeText": "code-text",
    "copySuccessText": "copy-success-text",
    "destructiveAction": "destructive-action stop-action-surface",
    "destructiveActionHover": "destructive-action-hover",
    "diffAdditionText": "diff-addition-text",
    "diffDeletionText": "diff-deletion-text",
    "divider": "card-border composer-border divider drawer-close-border file-divider info-divider input-border overlay-border pane-toggle-border plan-divider protocol-divider quote-border section-divider thread-menu-border",
    "dividerStrong": "control-hover-border copy-feedback-border pane-toggle-hover-border tooltip-border",
    "draftThreadBorder": "draft-thread-border",
    "draftThreadSurface": "draft-thread-surface",
    "failedPromptBorder": "failed-thread-border",
    "failedPromptSurface": "failed-thread-surface",
    "finalAnswerBorder": "final-answer-border",
    "finalAnswerSurface": "final-answer-surface",
    "finalAnswerText": "final-answer-text",
    "focusBorder": "composer-focus-border focus-border input-focus-border selected-option-border",
    "hover": "control-hover-surface drawer-close-hover-surface menu-item-hover-surface pane-toggle-hover-surface thread-menu-hover-surface",
    "inspector": "inspector-surface",
    "linkText": "link-text",
    "onAccent": "on-accent",
    "panel": "panel",
    "pendingPromptBorderStrong": "pending-prompt-border",
    "pendingPromptSurface": "pending-prompt-surface",
    "pendingSteeringBorderStrong": "pending-steering-border",
    "placeholder": "active-work-border disabled-text status-idle",
    "presentationSelectedSurface": "presentation-selected-surface",
    "primary": "card-title-text composer-text copy-feedback-surface input-text text-primary text-strong tooltip-surface",
    "primaryAction": "activity-active-text activity-spinner-accent primary-action",
    "raised": "raised-surface subdued-surface",
    "secondary": "connection-input-text global-status-text text-secondary thread-disclosure-text thread-menu-text",
    "secondaryStrong": "text-detail text-emphasis",
    "statusActive": "status-active thread-status-active",
    "statusActiveText": "status-active-text",
    "statusDanger": "status-danger thread-status-danger",
    "statusDangerText": "copy-failure-border copy-failure-surface destructive-text status-danger-text status-failed-text validation-text",
    "statusSuccess": "status-success thread-status-success",
    "statusSuccessText": "final-answer-phase-text status-completed-text status-success-text",
    "statusWarning": "status-warning",
    "statusWarningText": "status-warning-text",
    "steerAction": "steer-action",
    "steerActionHover": "steer-action-hover",
    "steerActionPressed": "steer-action-pressed",
    "steeringMessageBorder": "steering-message-border",
    "steeringMessageSurface": "steering-message-surface",
    "steeringMessageText": "steering-message-text",
    "threadInactive": "thread-status-idle",
    "threadSelected": "thread-selected",
    "threadSelectedBorder": "thread-selected-border",
    "updateMessageBorder": "update-message-border",
    "updateMessageSurface": "update-message-surface",
    "updateMessageText": "update-message-phase-text",
    "userMessageBorder": "user-message-border",
    "userMessageSurface": "user-message-surface",
    "userMessageText": "user-message-text",
    "warningNoticeBorder": "notice-border request-detail-border",
    "warningNoticeSurface": "notice-surface request-detail-surface",
    "primaryActionHover": "primary-action-hover",
    "primaryActionPressed": "primary-action-pressed",
    "destructiveActionPressed": "destructive-action-pressed",
};
export function generatedUiAssets() {
    const source = readFileSync(new URL("../../src/codex/ui/UiStyle.h", import.meta.url), "utf8");
    const values = new Map(); let palette = false;
    for (const line of source.split("\n")) {
        if (line.includes("namespace Palette {")) palette = true;
        if (line.includes("} // namespace Palette")) palette = false;
        const match = line.match(/inline constexpr auto (\w+) = ([^;]+);/u);
        if (match) values.set(`${palette ? "Palette::" : ""}${match[1]}`, match[2].replaceAll('"', ""));
    }
    const value = role => {
        if (role === "@overlay") return `${value("primary")}66`;
        if (role.startsWith("@pending")) {
            const rgba = source.match(new RegExp(`${role.slice(1)} = qRgba\\(([^)]+)\\)`))[1].split(",").map(Number);
            return `#${rgba.map(channel => channel.toString(16).padStart(2,"0")).join("")}`;
        }
        if (role.startsWith("#")) return role;
        if (!values.has(role)) throw new Error(`Unknown native UI role ${role}`);
        return value(values.get(role));
    };
    const declarations = Object.entries(roles).flatMap(([role,names]) => names.split(" ").map(name => `--${name}: ${value(role)};`));
    const colors = `/* Generated from src/codex/ui/UiStyle.h by web/tools/generate-ui-assets.mjs. */\n:root {\n${Array.from({length: Math.ceil(declarations.length / 6)}, (_, i) => "    " + declarations.slice(i * 6, i * 6 + 6).join(" ")).join("\n")}\n}\n`;
    const brand = readFileSync(new URL("../../resources/icons/codex-ui.svg", import.meta.url), "utf8")
        .replace('fill="#2f6feb"', `fill="${value("brandPrimary")}"`).replaceAll('stroke="#ffffff"', `stroke="${value("onAccent")}"`)
        .replace('stroke="#63d5a5"', `stroke="${value("brandAccent")}"`);
    return {colors, brand};
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    const assets = generatedUiAssets();
    writeFileSync(new URL("../src/colors.css", import.meta.url), assets.colors);
    writeFileSync(new URL("../src/brand.svg", import.meta.url), assets.brand);
}
