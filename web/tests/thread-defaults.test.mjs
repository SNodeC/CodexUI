import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import test from "node:test";
import {configuredDefaults, inheritedSettingChoices, settingDraftFor, changeSettingDraft,
    settingPromptOptions, turnSettingCatalog} from "../dist/app/TurnSettingsOptions.js";

const fixture = JSON.parse(readFileSync(new URL("../../tests/fixtures/thread-defaults.json", import.meta.url), "utf8"));
for (const entry of fixture.cases) {
    test(`workspace defaults: ${entry.id}`, () => {
        assert.deepEqual(configuredDefaults(entry.config), entry.expected);
        const drafts = new Map();
        const catalog = {...turnSettingCatalog({models: [], permissionProfiles: []}), defaults: configuredDefaults(entry.config)};
        const draft = settingDraftFor(drafts, "draft", {}, catalog);
        assert.deepEqual(settingPromptOptions(draft, catalog), {thread: {}, turn: {}});
        const edited = changeSettingDraft(drafts, "draft", {}, catalog, new Map(), "model", "chosen");
        const updated = settingDraftFor(drafts, "draft", {}, {...catalog, defaults: {model: "another"}});
        assert.equal(updated.values.model, edited.values.model);
        assert.deepEqual(settingPromptOptions(updated, catalog).thread, {model: "chosen"});
    });
}
test("inherited labels retain the inheritance option value", () => {
    const catalog = {...turnSettingCatalog({models: [], permissionProfiles: []}), defaults: {model: "configured"}};
    const draft = settingDraftFor(new Map(), "draft", {}, catalog);
    const choices = [{value: "default", label: "Default (not reported)", description: ""}];
    assert.deepEqual(inheritedSettingChoices("model", choices, undefined, draft.values), choices);
    assert.equal(inheritedSettingChoices("model", choices, catalog.defaults, draft.values)[0].label, "Default — Configured");
    assert.equal(inheritedSettingChoices("model", choices, catalog.defaults, draft.values)[0].value, "default");
    assert.equal(inheritedSettingChoices("effort", choices, {}, draft.values)[0].label, "Server default");
    const changed = {...draft.values, model: "chosen"};
    assert.equal(inheritedSettingChoices("effort", choices, {model: "configured", effort: "high"}, changed)[0].label, "Server default");
});
