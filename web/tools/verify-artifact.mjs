import assert from "node:assert/strict";
import {readdir, readFile, stat} from "node:fs/promises";
import {fileURLToPath} from "node:url";

const root = fileURLToPath(new URL("../app-dist/", import.meta.url));
const index = await readFile(new URL("../app-dist/index.html", import.meta.url), "utf8");
assert.match(index, /(?:src|href)="\.\/assets\//u, "the artifact must be relocatable below any static base path");
assert.doesNotMatch(index, /\/src\/main\.tsx/u, "the artifact must not reference development sources");

const assets = await readdir(new URL("../app-dist/assets/", import.meta.url));
assert.ok(assets.some(name => name.endsWith(".js")), "the artifact must contain a JavaScript bundle");
assert.ok(assets.some(name => name.endsWith(".css")), "the artifact must contain a stylesheet bundle");
for (const name of assets) assert.ok((await stat(new URL(`../app-dist/assets/${name}`, import.meta.url))).size > 0, `${name} must not be empty`);

console.log(`Verified relocatable CodexWebUI artifact at ${root} (${assets.length} assets)`);
