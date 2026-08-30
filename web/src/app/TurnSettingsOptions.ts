import {isObject, stringMember} from "../presentation/PresentationProtocol.js";

export const DefaultSetting = "default";
export type SettingField = "model" | "effort" | "personality" | "sandbox" | "network" | "approval"
    | "reviewer" | "cwd" | "permissionProfile" | "serviceTier" | "summary" | "collaboration";
export type SettingValues = Record<SettingField, string>;
export interface SettingDraft {
    values: SettingValues;
    touched: Set<SettingField>;
    canonicalSignature: string;
    settingsRevision: number;
}
export interface SettingPromptOptions {turn: Record<string, unknown>; thread: Record<string, unknown>}

export function permissionProfileLabel(id: string): string {
    if (id === ":workspace") return "Workspace";
    if (id === ":read-only") return "Read only";
    if (id === ":danger-full-access" || id === ":full-access") return "Full access";
    return id;
}

export function applySettingChange(values: SettingValues, touched: ReadonlySet<SettingField>, field: SettingField, value: string): {values: SettingValues; touched: Set<SettingField>} {
    const nextValues = {...values, [field]: value};
    const nextTouched = new Set(touched); nextTouched.add(field);
    if (field === "sandbox" || field === "network") {
        nextValues.permissionProfile = DefaultSetting;
        nextTouched.delete("permissionProfile");
    } else if (field === "permissionProfile" && value !== DefaultSetting) {
        nextTouched.delete("sandbox"); nextTouched.delete("network");
    }
    if (field === "sandbox" && (value === DefaultSetting || value === "danger-full-access"))
        nextValues.network = value === "danger-full-access" ? "enabled" : DefaultSetting;
    return {values: nextValues, touched: nextTouched};
}

function mergePatch(target: Record<string, unknown>, patch: Record<string, unknown>): void {
    for (const [key, value] of Object.entries(patch)) {
        if (value === null) delete target[key];
        else if (isObject(value) && isObject(target[key])) mergePatch(target[key], value);
        else target[key] = structuredClone(value);
    }
}

export function canonicalThreadSettings(raw: unknown, settingsDomain: unknown): Record<string, unknown> {
    const canonical = isObject(raw) ? structuredClone(raw) : {};
    if (!isObject(settingsDomain)) return canonical;
    const update = isObject(settingsDomain.threadSettings) ? settingsDomain.threadSettings : settingsDomain;
    if (Object.hasOwn(update, "effort")) delete canonical.reasoningEffort;
    if (Object.hasOwn(update, "sandboxPolicy")) delete canonical.sandbox;
    mergePatch(canonical, update);
    return canonical;
}

export function canonicalSettingValues(canonical: unknown): SettingValues {
    const value = isObject(canonical) ? canonical : {};
    const sandboxValue = isObject(value.sandboxPolicy) ? value.sandboxPolicy : value.sandbox;
    const sandboxType = typeof sandboxValue === "string" ? sandboxValue : stringMember(sandboxValue, "type");
    const sandbox = ({readOnly: "read-only", workspaceWrite: "workspace-write", dangerFullAccess: "danger-full-access", externalSandbox: "external"} as Record<string, string>)[sandboxType] ?? sandboxType ?? DefaultSetting;
    let network = DefaultSetting;
    if (sandbox !== DefaultSetting && sandbox !== "") {
        if (sandbox === "danger-full-access") network = "enabled";
        else if (isObject(sandboxValue)) network = sandboxValue.networkAccess === true || sandboxValue.networkAccess === "enabled" ? "enabled" : "restricted";
        else network = "restricted";
    }
    const profile = isObject(value.activePermissionProfile) ? stringMember(value.activePermissionProfile, "id") : "";
    const collaboration = isObject(value.collaborationMode) ? stringMember(value.collaborationMode, "mode") : "";
    const optional = (name: string) => typeof value[name] === "string" && value[name] !== "" ? value[name] as string : DefaultSetting;
    return {
        model: optional("model"), effort: optional("reasoningEffort") === DefaultSetting ? optional("effort") : optional("reasoningEffort"),
        personality: optional("personality"), sandbox: sandbox || DefaultSetting, network,
        approval: optional("approvalPolicy"), reviewer: optional("approvalsReviewer"), cwd: stringMember(value, "cwd"),
        permissionProfile: profile || DefaultSetting, serviceTier: optional("serviceTier"), summary: optional("summary"),
        collaboration: collaboration || "default",
    };
}

export function settingDraftFor(drafts: Map<string, SettingDraft>, key: string, canonical: unknown,
    settingsRevision: number): SettingDraft {
    const canonicalSignature = JSON.stringify(canonical) ?? "";
    const current = drafts.get(key);
    if (!current) {
        const created = {values: canonicalSettingValues(canonical), touched: new Set<SettingField>(),
            canonicalSignature, settingsRevision};
        drafts.set(key, created);
        return created;
    }
    if (current.canonicalSignature === canonicalSignature && current.settingsRevision === settingsRevision) return current;
    const fresh = canonicalSettingValues(canonical);
    const values = {...current.values};
    for (const field of Object.keys(fresh) as SettingField[]) if (!current.touched.has(field)) values[field] = fresh[field];
    const reconciled = {...current, values, canonicalSignature, settingsRevision};
    drafts.set(key, reconciled);
    return reconciled;
}

export function changeSettingDraft(drafts: Map<string, SettingDraft>, key: string, canonical: unknown,
    settingsRevision: number, field: SettingField, value: string): SettingDraft {
    const current = settingDraftFor(drafts, key, canonical, settingsRevision);
    const changed = applySettingChange(current.values, current.touched, field, value);
    const next = {...current, ...changed};
    drafts.set(key, next);
    return next;
}

export function settingPromptOptions(draft: SettingDraft, models: unknown): SettingPromptOptions {
    return {turn: turnStartOptions(draft.values, draft.touched, models),
        thread: threadStartOptions(draft.values, draft.touched)};
}

function choice(values: SettingValues, touched: ReadonlySet<SettingField>, field: SettingField, name: string,
    result: Record<string, unknown>): void {
    if (touched.has(field)) result[name] = values[field] === DefaultSetting ? null : values[field];
}
export function sandboxPolicy(values: SettingValues): unknown {
    if (values.sandbox === DefaultSetting) return null;
    if (values.sandbox === "danger-full-access") return {type: "dangerFullAccess"};
    if (values.sandbox === "external") return {type: "externalSandbox", networkAccess: values.network === "enabled" ? "enabled" : "restricted"};
    if (values.sandbox === "read-only") return {type: "readOnly", networkAccess: values.network === "enabled"};
    return {type: "workspaceWrite", writableRoots: [], networkAccess: values.network === "enabled", excludeTmpdirEnvVar: false, excludeSlashTmp: false};
}
export function collaborationMode(values: SettingValues, models: unknown): unknown {
    let model = values.model;
    if (model === DefaultSetting && Array.isArray(models)) {
        const definition = models.find(entry => isObject(entry) && entry.isDefault === true);
        model = stringMember(definition, "model") || stringMember(definition, "id");
    }
    if (model === "" || model === DefaultSetting) return null;
    return {mode: values.collaboration, settings: {model, developer_instructions: null,
        reasoning_effort: values.effort === DefaultSetting ? null : values.effort}};
}
export function threadStartOptions(values: SettingValues, touched: ReadonlySet<SettingField>): Record<string, unknown> {
    const result: Record<string, unknown> = {};
    choice(values, touched, "model", "model", result); choice(values, touched, "approval", "approvalPolicy", result);
    choice(values, touched, "reviewer", "approvalsReviewer", result); choice(values, touched, "personality", "personality", result);
    choice(values, touched, "serviceTier", "serviceTier", result);
    if (touched.has("cwd")) result.cwd = values.cwd.trim() === "" ? null : values.cwd.trim();
    if (touched.has("permissionProfile")) result.permissions = values.permissionProfile === DefaultSetting ? null : values.permissionProfile;
    if (values.permissionProfile === DefaultSetting && touched.has("sandbox"))
        result.sandbox = values.sandbox === DefaultSetting || values.sandbox === "external" ? null : values.sandbox;
    return result;
}
export function turnStartOptions(values: SettingValues, touched: ReadonlySet<SettingField>, models: unknown): Record<string, unknown> {
    const result: Record<string, unknown> = {};
    choice(values, touched, "model", "model", result); choice(values, touched, "effort", "effort", result);
    choice(values, touched, "personality", "personality", result); choice(values, touched, "approval", "approvalPolicy", result);
    choice(values, touched, "reviewer", "approvalsReviewer", result); choice(values, touched, "serviceTier", "serviceTier", result);
    choice(values, touched, "summary", "summary", result);
    if (touched.has("cwd")) result.cwd = values.cwd.trim() === "" ? null : values.cwd.trim();
    if (touched.has("permissionProfile")) result.permissions = values.permissionProfile === DefaultSetting ? null : values.permissionProfile;
    if (values.permissionProfile === DefaultSetting && (touched.has("sandbox") || touched.has("network"))) result.sandboxPolicy = sandboxPolicy(values);
    if (touched.has("collaboration")) { const mode = collaborationMode(values, models); if (mode !== null) result.collaborationMode = mode; }
    return result;
}
