import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {constants as fsConstants} from "node:fs";
import {access, mkdtemp, readFile, rm} from "node:fs/promises";
import {createServer} from "node:http";
import {tmpdir} from "node:os";
import {dirname, extname, join, resolve, sep} from "node:path";
import {fileURLToPath} from "node:url";

const toolDirectory = dirname(fileURLToPath(import.meta.url));
const artifactDirectory = resolve(toolDirectory, "../app-dist");
const wait = milliseconds => new Promise(complete => setTimeout(complete, milliseconds));

async function chromeExecutable() {
    const candidates = [process.env.CHROME_BIN, "/opt/google/chrome/chrome", "/usr/bin/google-chrome",
        "/usr/bin/google-chrome-stable", "/usr/bin/chromium", "/usr/bin/chromium-browser"].filter(Boolean);
    for (const candidate of candidates) {
        try { await access(candidate, fsConstants.X_OK); return candidate; }
        catch { /* Try the next canonical installation path. */ }
    }
    throw new Error("Real-browser qualification requires Chrome or Chromium; set CHROME_BIN to its executable");
}

function contentType(path) {
    return ({".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
        ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml"})[extname(path)] ?? "application/octet-stream";
}

async function staticServer() {
    const server = createServer(async (request, response) => {
        try {
            const pathname = decodeURIComponent(new URL(request.url ?? "/", "http://127.0.0.1").pathname);
            const path = resolve(artifactDirectory, pathname === "/" ? "index.html" : `.${pathname}`);
            if (path !== artifactDirectory && !path.startsWith(`${artifactDirectory}${sep}`)) throw new Error("invalid path");
            const body = await readFile(path);
            response.writeHead(200, {"content-type": contentType(path), "cache-control": "no-store"});
            response.end(body);
        } catch {
            response.writeHead(404, {"content-type": "text/plain; charset=utf-8"});
            response.end("Not found");
        }
    });
    server.on("upgrade", (_request, socket) => socket.destroy());
    await new Promise((complete, reject) => {
        server.once("error", reject);
        server.listen(0, "127.0.0.1", complete);
    });
    const address = server.address();
    assert(address && typeof address === "object");
    return {server, url: `http://127.0.0.1:${address.port}/`};
}

async function availableLoopbackPort() {
    const probe = createServer();
    await new Promise((complete, reject) => {
        probe.once("error", reject);
        probe.listen(0, "127.0.0.1", complete);
    });
    const address = probe.address();
    assert(address && typeof address === "object");
    await new Promise((complete, reject) => probe.close(error => error ? reject(error) : complete()));
    return address.port;
}

class DevTools {
    #id = 0;
    #pending = new Map();
    #socket;

    constructor(socket) {
        this.#socket = socket;
        socket.addEventListener("message", event => {
            const message = JSON.parse(String(event.data));
            const pending = this.#pending.get(message.id);
            if (!pending) return;
            this.#pending.delete(message.id);
            if (message.error) pending.reject(new Error(message.error.message));
            else pending.resolve(message.result);
        });
    }

    static async connect(url) {
        const socket = new WebSocket(url);
        await new Promise((complete, reject) => {
            socket.addEventListener("open", complete, {once: true});
            socket.addEventListener("error", reject, {once: true});
        });
        return new DevTools(socket);
    }

    call(method, params = {}) {
        const id = ++this.#id;
        return new Promise((resolveCall, reject) => {
            this.#pending.set(id, {resolve: resolveCall, reject});
            this.#socket.send(JSON.stringify({id, method, params}));
        });
    }

    async evaluate(expression) {
        const result = await this.call("Runtime.evaluate", {expression, awaitPromise: true, returnByValue: true});
        if (result.exceptionDetails) throw new Error(result.exceptionDetails.text ?? "Browser evaluation failed");
        return result.result.value;
    }

    close() { this.#socket.close(); }
}

async function pageTarget(port) {
    for (let attempt = 0; attempt < 200; ++attempt) {
        try {
            const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
            const page = targets.find(target => target.type === "page");
            if (page) return page.webSocketDebuggerUrl;
        } catch { /* Chrome may still be starting. */ }
        await wait(25);
    }
    throw new Error("Chrome did not expose the CodexUI page target");
}

async function setWidth(devTools, width) {
    await devTools.call("Emulation.setDeviceMetricsOverride", {width, height: 900, deviceScaleFactor: 1, mobile: false});
    await waitUntil(devTools, `innerWidth===${width}`, `${width}px viewport`);
}

async function waitUntil(devTools, expression, description) {
    for (let attempt = 0; attempt < 100; ++attempt) {
        if (await devTools.evaluate(`Boolean(${expression})`)) return;
        await wait(25);
    }
    throw new Error(`Timed out waiting for ${description}`);
}

const fullTopBarMeasurement = `(()=>{
    const actions=document.querySelector(".top-actions");
    if(!actions.querySelector(".qualification-control")){
        const button=document.createElement("button");
        button.className="subtle-button qualification-control";
        button.textContent="Release control";
        actions.prepend(button);
    }
    const top=document.querySelector(".top-bar").getBoundingClientRect();
    const actionBounds=actions.getBoundingClientRect();
    return {width:innerWidth,overflow:document.documentElement.scrollWidth-document.documentElement.clientWidth,
        top:{left:top.left,right:top.right,bottom:top.bottom,height:top.height},
        actions:{left:actionBounds.left,right:actionBounds.right,bottom:actionBounds.bottom},
        drawers:[...document.querySelectorAll(".responsive-pane-button")].map(button=>button.textContent.trim())};
})()`;

function assertTopBar(measurement, width) {
    assert.equal(measurement.width, width);
    assert.equal(measurement.overflow, 0, `${width}px layout must not overflow the document`);
    assert(measurement.top.left >= 0 && measurement.top.right <= width);
    assert(measurement.actions.left >= 0 && measurement.actions.right <= width);
    assert(measurement.actions.bottom <= measurement.top.bottom + 0.5);
}

await access(join(artifactDirectory, "index.html"), fsConstants.R_OK);
const executable = await chromeExecutable();
const {server, url} = await staticServer();
const profile = await mkdtemp(join(tmpdir(), "codexui-browser-qualification."));
let chrome;
let chromeErrors = "";
let devTools;

try {
    const port = await availableLoopbackPort();
    const constrainedRunnerArguments = process.env.CI
        ? ["--no-sandbox", "--disable-dev-shm-usage"]
        : [];
    chrome = spawn(executable, ["--headless=new", "--disable-gpu", "--no-first-run",
        ...constrainedRunnerArguments,
        "--no-default-browser-check", "--remote-debugging-address=127.0.0.1",
        `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
        "--window-size=760,900", "--noerrdialogs", url], {stdio: ["ignore", "ignore", "pipe"]});
    chrome.stderr.on("data", chunk => { chromeErrors = `${chromeErrors}${chunk}`.slice(-8000); });
    devTools = await DevTools.connect(await pageTarget(port));
    await devTools.call("Runtime.enable");
    await waitUntil(devTools, `document.querySelector(".top-actions")`, "CodexUI application shell");
    await setWidth(devTools, 760);
    await waitUntil(devTools, `document.querySelectorAll(".responsive-pane-button").length===2`, "760px drawer controls");
    assertTopBar(await devTools.evaluate(fullTopBarMeasurement), 760);
    const focus = await devTools.evaluate(`(()=>{const input=document.querySelector(".connection-control input");input.focus();
        const style=getComputedStyle(input);return [style.outlineStyle,style.outlineWidth,style.outlineColor]})()`);
    assert.deepEqual(focus, ["solid", "2px", "rgb(111, 152, 232)"]);

    const opened = await devTools.evaluate(`(async()=>{[...document.querySelectorAll(".responsive-pane-button")]
        .find(button=>button.textContent.includes("Inspector")).click();await new Promise(done=>setTimeout(done,30));
        const drawer=document.querySelector("#inspector-pane");return {focus:document.activeElement?.getAttribute("aria-label"),
        inert:[...document.querySelectorAll("[data-modal-background]")].every(element=>element.hasAttribute("inert")&&element.getAttribute("aria-hidden")==="true"),
        role:drawer?.getAttribute("role"),modal:drawer?.getAttribute("aria-modal")}})()`);
    assert.deepEqual(opened, {focus: "Close Inspector drawer", inert: true, role: "dialog", modal: "true"});
    const trapped = await devTools.evaluate(`(()=>{document.dispatchEvent(new KeyboardEvent("keydown",{key:"Tab",shiftKey:true,bubbles:true,cancelable:true}));
        const backward=document.activeElement?.textContent?.trim();document.dispatchEvent(new KeyboardEvent("keydown",{key:"Tab",bubbles:true,cancelable:true}));
        return {backward,forward:document.activeElement?.getAttribute("aria-label")}})()`);
    assert.deepEqual(trapped, {backward: "Protocol", forward: "Close Inspector drawer"});
    const closed = await devTools.evaluate(`(async()=>{document.dispatchEvent(new KeyboardEvent("keydown",{key:"Escape",bubbles:true,cancelable:true}));
        await new Promise(done=>setTimeout(done,30));return {drawer:Boolean(document.querySelector("#inspector-pane")),
        focus:document.activeElement?.getAttribute("aria-controls"),inert:[...document.querySelectorAll("[data-modal-background]")].some(element=>element.hasAttribute("inert"))}})()`);
    assert.deepEqual(closed, {drawer: false, focus: "inspector-pane", inert: false});

    await devTools.evaluate(`[...document.querySelectorAll(".responsive-pane-button")].find(button=>button.textContent.includes("Threads")).click()`);
    await wait(30);
    await setWidth(devTools, 900);
    await waitUntil(devTools, `!document.querySelector("#thread-pane")&&!([...document.querySelectorAll(".responsive-pane-button")]
        .some(button=>button.textContent.includes("Threads")))`, "tablet drawer fallback");
    const fallback = await devTools.evaluate(`({drawer:Boolean(document.querySelector("#thread-pane")),
        focusTag:document.activeElement?.tagName,focusClass:document.activeElement?.className,
        threadTrigger:[...document.querySelectorAll(".responsive-pane-button")].some(button=>button.textContent.includes("Threads")),
        overflow:document.documentElement.scrollWidth-document.documentElement.clientWidth,
        width:innerWidth,mobile:matchMedia("(max-width: 760px)").matches})`);
    assert.deepEqual(fallback, {drawer: false, focusTag: "MAIN", focusClass: "conversation-pane", threadTrigger: false,
        overflow: 0, width: 900, mobile: false});

    for (const width of [521, 360]) {
        await setWidth(devTools, width);
        await waitUntil(devTools, `document.querySelectorAll(".responsive-pane-button").length===2`, `${width}px drawer controls`);
        const measurement = await devTools.evaluate(fullTopBarMeasurement);
        assertTopBar(measurement, width);
        assert.deepEqual(measurement.drawers, ["☰ Threads", "ⓘ Inspector"]);
    }
    await devTools.call("Emulation.setTouchEmulationEnabled", {enabled: true, maxTouchPoints: 1});
    await waitUntil(devTools, `matchMedia("(pointer: coarse)").matches`, "coarse-pointer media query");
    const touch = await devTools.evaluate(`({coarse:matchMedia("(pointer: coarse)").matches,
        paneHeight:document.querySelector(".responsive-pane-button").getBoundingClientRect().height,
        viewHeight:document.querySelector(".conversation-view-controls button").getBoundingClientRect().height})`);
    assert.equal(touch.coarse, true);
    assert(touch.paneHeight >= 44 && touch.viewHeight >= 44);

    console.log(JSON.stringify({browser: "Chromium", widths: [760, 521, 360], overflow: 0,
        drawerFocus: "qualified", focusTrap: "qualified", breakpointFallback: "qualified", coarseTargets: "44px"}, null, 2));
} catch (error) {
    if (chromeErrors) process.stderr.write(chromeErrors);
    throw error;
} finally {
    devTools?.close();
    chrome?.kill("SIGTERM");
    if (chrome && chrome.exitCode === null)
        await Promise.race([new Promise(complete => chrome.once("exit", complete)), wait(1000)]);
    if (chrome?.exitCode === null) chrome.kill("SIGKILL");
    await new Promise(complete => server.close(complete));
    await rm(profile, {recursive: true, force: true});
}
