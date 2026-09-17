import {humanizeProtocolLabel as humanize} from "../presentation/PresentationStatus.js";
import {isObject, stringMember} from "../presentation/PresentationProtocol.js";
import type {JsonObject, ThreadSettingStamp} from "../presentation/PresentationProtocol.js";

export const DefaultSetting = "default";
export const SettingFields = ["model", "effort", "personality", "sandbox", "network", "approval",
    "reviewer", "cwd", "permissionProfile", "serviceTier", "summary", "collaboration"] as const;
export type SettingField = typeof SettingFields[number];
export type SettingValues = Record<SettingField, string>;
export interface SettingChoice {value: string; label: string; description: string}
export interface SettingDraft {
    values: SettingValues;
    touched: Set<SettingField>;
    settingStamps: ReadonlyMap<string, ThreadSettingStamp>;
}
export interface SettingPresentation {
    models: readonly SettingChoice[];
    efforts: readonly SettingChoice[];
    permissionProfiles: readonly SettingChoice[];
    serviceTiers: readonly SettingChoice[];
    networkDisabledReason: string;
    personalityDisabledReason: string;
}

const DefaultChoice: SettingChoice = {value: DefaultSetting, label: "Thread default", description: ""};
const FixedChoices: Partial<Record<SettingField, readonly SettingChoice[]>> = {
    sandbox: [DefaultChoice, {label: "Workspace", value: "workspace-write", description: ""}, {label: "Read only", value: "read-only", description: ""}, {label: "Full access", value: "danger-full-access", description: ""}, {label: "External", value: "external", description: ""}],
    network: [DefaultChoice, {label: "Restricted", value: "restricted", description: ""}, {label: "Enabled", value: "enabled", description: ""}],
    approval: [DefaultChoice, {label: "On request", value: "on-request", description: ""}, {label: "Untrusted", value: "untrusted", description: ""}, {label: "Never", value: "never", description: ""}],
    personality: [DefaultChoice, {label: "None", value: "none", description: ""}, {label: "Friendly", value: "friendly", description: ""}, {label: "Pragmatic", value: "pragmatic", description: ""}],
    reviewer: [DefaultChoice, {label: "User", value: "user", description: ""}, {label: "Auto review", value: "auto_review", description: ""}, {label: "Guardian", value: "guardian_subagent", description: ""}],
    summary: [DefaultChoice, {label: "Auto", value: "auto", description: ""}, {label: "Concise", value: "concise", description: ""}, {label: "Detailed", value: "detailed", description: ""}, {label: "None", value: "none", description: ""}],
    collaboration: [{label: "Code", value: DefaultSetting, description: ""}, {label: "Plan", value: "plan", description: ""}],
};

export interface ModelDefinition {
    choice: SettingChoice;
    reasoningEfforts: string[];
    defaultReasoningEffort: string;
    serviceTiers: SettingChoice[];
    defaultServiceTier: string;
    isDefault: boolean;
    supportsPersonality: boolean;
}

export interface SettingCatalog {
    models: ModelDefinition[];
    permissionProfiles: SettingChoice[];
}

const EmptySettingStamps = new Map<string, ThreadSettingStamp>();

function optional(value: unknown, name: string): string {
    return isObject(value) && typeof value[name] === "string" && value[name] !== ""
        ? value[name] as string : DefaultSetting;
}

function canonicalSettingValues(canonical: unknown): SettingValues {
    const value = isObject(canonical) ? canonical : {};
    const sandboxValue = isObject(value.sandboxPolicy) ? value.sandboxPolicy : value.sandbox;
    const sandboxType = typeof sandboxValue === "string" ? sandboxValue : stringMember(sandboxValue, "type");
    const sandbox = ({readOnly: "read-only", workspaceWrite: "workspace-write", dangerFullAccess: "danger-full-access",
        externalSandbox: "external", "read-only": "read-only", "workspace-write": "workspace-write",
        "danger-full-access": "danger-full-access", external: "external"} as Record<string, string>)[sandboxType]
        ?? DefaultSetting;
    const network = sandbox === DefaultSetting ? DefaultSetting : sandbox === "danger-full-access" ? "enabled"
        : isObject(sandboxValue) && (sandboxValue.networkAccess === true || sandboxValue.networkAccess === "enabled")
            ? "enabled" : "restricted";
    const profileValue = value.activePermissionProfile;
    const profile = typeof profileValue === "string" ? profileValue : stringMember(profileValue, "id");
    return {
        model: optional(value, "model"), effort: optional(value, "reasoningEffort") === DefaultSetting
            ? optional(value, "effort") : optional(value, "reasoningEffort"),
        personality: optional(value, "personality"), sandbox, network,
        approval: optional(value, "approvalPolicy"), reviewer: optional(value, "approvalsReviewer"),
        cwd: stringMember(value, "cwd"), permissionProfile: profile || DefaultSetting,
        serviceTier: optional(value, "serviceTier"), summary: optional(value, "summary"),
        collaboration: stringMember(value.collaborationMode, "mode") || "default",
    };
}

function models(values: unknown): ModelDefinition[] {
    if (!Array.isArray(values)) return [];
    const rows = new Map<string, JsonObject>();
    for (const raw of values) if (isObject(raw)) {
        const id = stringMember(raw, "model") || stringMember(raw, "id");
        if (id !== "") rows.set(id, raw);
    }
    const result: ModelDefinition[] = [];
    for (const [id, raw] of rows) {
        if (raw.hidden === true) continue;
        const reasoningEfforts: string[] = [];
        const effortIds = new Set<string>();
        for (const effort of Array.isArray(raw.supportedReasoningEfforts) ? raw.supportedReasoningEfforts : []) {
            const value = stringMember(effort, "reasoningEffort");
            if (value !== "" && !effortIds.has(value)) { effortIds.add(value); reasoningEfforts.push(value); }
        }
        const serviceTiers: SettingChoice[] = [];
        const tierIds = new Set<string>();
        for (const tier of Array.isArray(raw.serviceTiers) ? raw.serviceTiers : []) {
            const value = stringMember(tier, "id");
            if (value === "" || tierIds.has(value)) continue;
            tierIds.add(value); serviceTiers.push({value, label: stringMember(tier, "name") || value,
                description: stringMember(tier, "description")});
        }
        for (const tier of Array.isArray(raw.additionalSpeedTiers) ? raw.additionalSpeedTiers : []) {
            if (typeof tier !== "string" || tier === "" || tierIds.has(tier)) continue;
            tierIds.add(tier); serviceTiers.push({value: tier, label: "", description: ""});
        }
        result.push({choice: {value: id, label: stringMember(raw, "displayName") || id,
            description: stringMember(raw, "description")}, reasoningEfforts,
        defaultReasoningEffort: stringMember(raw, "defaultReasoningEffort"), serviceTiers,
        defaultServiceTier: stringMember(raw, "defaultServiceTier"), isDefault: raw.isDefault === true,
        supportsPersonality: raw.supportsPersonality !== false});
    }
    return result;
}

function permissionProfileLabel(id: string): string {
    if (id === ":workspace") return "Workspace";
    if (id === ":read-only") return "Read only";
    if (id === ":danger-full-access" || id === ":full-access") return "Full access";
    return id;
}

function profiles(envelope: unknown): SettingChoice[] {
    const values = Array.isArray(envelope) ? envelope : isObject(envelope) && Array.isArray(envelope.data)
        ? envelope.data : [];
    const rows = new Map<string, JsonObject>();
    for (const raw of values) if (isObject(raw)) {
        const id = stringMember(raw, "id");
        if (id !== "") rows.set(id, raw);
    }
    const result: SettingChoice[] = [];
    for (const [id, raw] of rows) {
        if (raw.allowed === false) continue;
        result.push({value: id, label: permissionProfileLabel(id),
            description: stringMember(raw, "description")});
    }
    return result;
}

export function turnSettingCatalog(source: {models: unknown; permissionProfiles: unknown}): SettingCatalog {
    return {models: models(source.models), permissionProfiles: profiles(source.permissionProfiles)};
}

function selectedModel(values: SettingValues, definitions: readonly ModelDefinition[]): ModelDefinition | undefined {
    return definitions.find(model => values.model === DefaultSetting ? model.isDefault : model.choice.value === values.model);
}

function normalize(values: SettingValues, catalog: SettingCatalog): SettingValues {
    const next = {...values};
    if (![DefaultSetting, "workspace-write", "read-only", "danger-full-access", "external"].includes(next.sandbox))
        next.sandbox = DefaultSetting;
    if (next.sandbox === DefaultSetting) next.network = DefaultSetting;
    else if (next.sandbox === "danger-full-access") next.network = "enabled";
    const model = selectedModel(next, catalog.models);
    if (model?.reasoningEfforts.length && next.effort !== DefaultSetting
        && !model.reasoningEfforts.includes(next.effort)) next.effort = DefaultSetting;
    if (model?.supportsPersonality === false) next.personality = DefaultSetting;
    return next;
}

export function settingDraftFor(drafts: Map<string, SettingDraft>, key: string, canonical: unknown,
    catalog: SettingCatalog, settingStamps: ReadonlyMap<string, ThreadSettingStamp> = EmptySettingStamps): SettingDraft {
    const fresh = canonicalSettingValues(canonical);
    const current = drafts.get(key);
    if (!current) {
        const created = {values: normalize(fresh, catalog), touched: new Set<SettingField>(),
            settingStamps};
        if (key !== "") drafts.set(key, created);
        return created;
    }
    const touched = new Set(current.touched);
    for (const field of SettingFields) {
        const concept = field === "network" ? "sandbox" : field;
        if ((settingStamps.get(concept)?.acknowledgements ?? 0)
                > (current.settingStamps.get(concept)?.acknowledgements ?? 0)
            && current.values[field] === fresh[field]) touched.delete(field);
    }
    const values = {...current.values};
    for (const field of SettingFields) if (!touched.has(field)) values[field] = fresh[field];
    const normalized = normalize(values, catalog);
    if (touched.size === current.touched.size
        && SettingFields.every(field => normalized[field] === current.values[field])) {
        current.settingStamps = settingStamps;
        return current;
    }
    const reconciled = {values: normalized, touched, settingStamps};
    drafts.set(key, reconciled);
    return reconciled;
}

export function changeSettingDraft(drafts: Map<string, SettingDraft>, key: string, canonical: unknown,
    catalog: SettingCatalog, settingStamps: ReadonlyMap<string, ThreadSettingStamp>, field: SettingField,
    value: string): SettingDraft {
    const current = settingDraftFor(drafts, key, canonical, catalog, settingStamps);
    const values = {...current.values, [field]: value};
    const touched = new Set(current.touched); touched.add(field);
    if (field === "sandbox" || field === "network") {
        values.permissionProfile = DefaultSetting; touched.add("permissionProfile");
    } else if (field === "permissionProfile" && value !== DefaultSetting) {
        const fresh = canonicalSettingValues(canonical);
        values.sandbox = fresh.sandbox; values.network = fresh.network;
        touched.delete("sandbox"); touched.delete("network");
    }
    const next = {...current, values: normalize(values, catalog), touched};
    if (key !== "") drafts.set(key, next);
    return next;
}

function withCurrent(choices: SettingChoice[], value: string, label = value): SettingChoice[] {
    if (value !== "" && value !== DefaultSetting && !choices.some(choice => choice.value === value))
        choices.push({value, label, description: ""});
    return choices;
}

export function fixedSettingChoices(field: SettingField, current: string): readonly SettingChoice[] {
    const choices = FixedChoices[field] ?? [];
    return current !== "" && current !== DefaultSetting && !choices.some(choice => choice.value === current)
        ? [...choices, {value: current, label: humanize(current), description: ""}] : choices;
}

export function settingPresentation(draft: SettingDraft, catalog: SettingCatalog): SettingPresentation {
    const definition = selectedModel(draft.values, catalog.models);
    const efforts = definition?.reasoningEfforts ?? [];
    const defaultEffort = definition?.defaultReasoningEffort;
    const defaultTier = definition?.defaultServiceTier;
    return {
        models: withCurrent([DefaultChoice, ...catalog.models.map(model => model.choice)], draft.values.model),
        efforts: withCurrent([{...DefaultChoice, label: defaultEffort ? `${humanize(defaultEffort)} - default` : DefaultChoice.label},
            ...efforts.map(value => ({value, label: humanize(value), description: ""}))], draft.values.effort),
        permissionProfiles: withCurrent([DefaultChoice, ...catalog.permissionProfiles], draft.values.permissionProfile,
            permissionProfileLabel(draft.values.permissionProfile)),
        serviceTiers: withCurrent([{...DefaultChoice, label: defaultTier ? `Thread default (${defaultTier})` : DefaultChoice.label},
            ...(definition?.serviceTiers.map(choice => ({...choice,
                label: choice.label || humanize(choice.value)})) ?? [])], draft.values.serviceTier,
            humanize(draft.values.serviceTier)),
        networkDisabledReason: draft.values.sandbox === "danger-full-access" ? "Full access already includes network access"
            : draft.values.sandbox === DefaultSetting ? "Select an access mode before network access" : "",
        personalityDisabledReason: definition?.supportsPersonality === false
            ? "The selected model does not support style choices" : "",
    };
}

function copyChoice(result: Record<string, unknown>, draft: SettingDraft, field: SettingField, name: string): void {
    if (draft.touched.has(field)) result[name] = draft.values[field] === DefaultSetting ? null : draft.values[field];
}

function sandboxPolicy(values: SettingValues): unknown {
    if (values.sandbox === DefaultSetting) return null;
    if (values.sandbox === "danger-full-access") return {type: "dangerFullAccess"};
    if (values.sandbox === "external") return {type: "externalSandbox", networkAccess: values.network === "enabled" ? "enabled" : "restricted"};
    if (values.sandbox === "read-only") return {type: "readOnly", networkAccess: values.network === "enabled"};
    return {type: "workspaceWrite", writableRoots: [], networkAccess: values.network === "enabled",
        excludeTmpdirEnvVar: false, excludeSlashTmp: false};
}

function startOptions(draft: SettingDraft, turn: boolean, resolvedModel = ""): Record<string, unknown> {
    const result: Record<string, unknown> = {};
    copyChoice(result, draft, "model", "model");
    if (turn) copyChoice(result, draft, "effort", "effort");
    copyChoice(result, draft, "personality", "personality"); copyChoice(result, draft, "approval", "approvalPolicy");
    copyChoice(result, draft, "reviewer", "approvalsReviewer");
    copyChoice(result, draft, "serviceTier", "serviceTier");
    if (turn) copyChoice(result, draft, "summary", "summary");
    if (draft.touched.has("cwd")) result.cwd = draft.values.cwd.trim() || null;
    if (draft.touched.has("permissionProfile"))
        result.permissions = draft.values.permissionProfile === DefaultSetting ? null : draft.values.permissionProfile;
    if (draft.values.permissionProfile === DefaultSetting && turn
        && (draft.touched.has("sandbox") || draft.touched.has("network")))
        result.sandboxPolicy = sandboxPolicy(draft.values);
    else if (draft.values.permissionProfile === DefaultSetting && draft.touched.has("sandbox"))
        result.sandbox = [DefaultSetting, "external"].includes(draft.values.sandbox) ? null : draft.values.sandbox;
    if (turn && resolvedModel !== "" && resolvedModel !== DefaultSetting) result.collaborationMode = {mode: draft.values.collaboration,
        settings: {model: resolvedModel, developer_instructions: null,
            reasoning_effort: draft.values.effort === DefaultSetting ? null : draft.values.effort}};
    return result;
}

export function settingPromptOptions(draft: SettingDraft, catalog: SettingCatalog) {
    const definition = selectedModel(draft.values, catalog.models);
    const model = draft.values.model === DefaultSetting ? definition?.choice.value ?? "" : draft.values.model;
    return {turn: startOptions(draft, true, model), thread: startOptions(draft, false)};
}
