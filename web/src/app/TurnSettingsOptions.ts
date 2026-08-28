import {isObject, stringMember} from "../presentation/PresentationProtocol.js";

export const DefaultSetting = "default";
export type SettingField = "model" | "effort" | "personality" | "sandbox" | "network" | "approval"
    | "reviewer" | "cwd" | "permissionProfile" | "serviceTier" | "summary" | "collaboration";
export type SettingValues = Record<SettingField, string>;

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
