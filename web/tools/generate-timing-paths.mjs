import {readFileSync, writeFileSync} from "node:fs";
import {fileURLToPath} from "node:url";
import {resolve} from "node:path";

export function generatedTimingPaths() {
    const source = readFileSync(new URL("../../src/codex/ProtocolTimingPaths.inc", import.meta.url), "utf8");
    const types = Object.fromEntries([...source.split("TimingMethods")[0].matchAll(/\{"([^"\n]+)",\s*\{([^}]+)\}\}/gu)]
        .map(([, name, paths]) => [name, [...paths.matchAll(/"([^"]+)"/gu)].map(match => match[1])]));
    const methods = Object.fromEntries([...source.split("TimingMethods")[1].matchAll(/\{"([^"]+)",\s*"([^"]+)"\}/gu)].map(([, method, type]) => [method, type]));
    return `// Generated from src/codex/ProtocolTimingPaths.inc. Run node web/tools/generate-timing-paths.mjs.\nexport const TimingTypes: Readonly<Record<string, readonly string[]>> = ${JSON.stringify(types)};\nexport const TimingMethods: Readonly<Record<string, string>> = ${JSON.stringify(methods)};\n`;
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url))
    writeFileSync(new URL("../src/presentation/TimingPaths.ts", import.meta.url), generatedTimingPaths());
