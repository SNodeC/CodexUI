// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
import assert from 'node:assert/strict';
import {spawn, execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {get as httpsGet} from 'node:https';
import {mkdir, mkdtemp, readFile, writeFile, rm} from 'node:fs/promises';
import {createServer} from 'node:net';
import {createServer as createHttpServer} from 'node:http';
import {join, resolve} from 'node:path';
import {once} from 'node:events';
import test from 'node:test';

const [manager, fixture, assets, build] = process.argv.slice(2);
const root = await mkdtemp(join(build, 'manager-web-test-'));
const pause = (milliseconds) => new Promise((done) => setTimeout(done, milliseconds));
async function availablePort() {
    const probe = createServer();
    probe.listen(0, '127.0.0.1');
    await once(probe, 'listening');
    const port = probe.address().port;
    await new Promise((done) => probe.close(done));
    return port;
}
async function until(check, description = 'Condition') {
    let result;
    const deadline = Date.now() + 20000;
    while (!result && Date.now() < deadline) {
        result = await check();
        if (!result) { await pause(50); }
    }
    assert.ok(result, description + ' did not become true');
    return result;
}
class DevTools {
    sequence = 0;
    pending = new Map();
    constructor(socket) {
        this.socket = socket;
        socket.onmessage = ({data}) => {
            const message = JSON.parse(data);
            const promise = this.pending.get(message.id);
            if (message.method === 'Page.javascriptDialogOpening') { this.send('Page.handleJavaScriptDialog', {accept: true}); }
            if (message.method === 'Fetch.authRequired') {
                this.send('Fetch.continueWithAuth', {requestId: message.params.requestId, authChallengeResponse: {response: 'ProvideCredentials', username: 'manager', password: 'fixture-password'}});
            }
            if (message.method === 'Fetch.requestPaused') { this.send('Fetch.continueRequest', {requestId: message.params.requestId}); }
            if (promise) {
                this.pending.delete(message.id);
                if (message.error) { promise.reject(new Error(message.error.message)); }
                else { promise.resolve(message.result); }
            }
        };
    }
    async send(method, params = {}) {
        const id = ++this.sequence;
        const promise = new Promise((resolve, reject) => this.pending.set(id, {resolve, reject}));
        this.socket.send(JSON.stringify({id, method, params}));
        return promise;
    }
    async evaluate(expression) {
        const result = await this.send('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true});
        assert.equal(result.exceptionDetails, undefined, JSON.stringify(result.exceptionDetails));
        return result.result.value;
    }
    async click(selector) {
        const rect = await this.evaluate(`(() => { const element = document.querySelector(${JSON.stringify(selector)}); element.scrollIntoView({block:'center'}); const r = element.getBoundingClientRect(); return {x:r.x+r.width/2,y:r.y+r.height/2}; })()`);
        await this.send('Input.dispatchMouseEvent', {type: 'mousePressed', button: 'left', clickCount: 1, ...rect});
        await this.send('Input.dispatchMouseEvent', {type: 'mouseReleased', button: 'left', clickCount: 1, ...rect});
    }
    async fill(selector, text) {
        await this.evaluate(`(() => { const input = document.querySelector(${JSON.stringify(selector)}); input.focus(); input.select(); })()`);
        await this.send('Input.insertText', {text});
        await this.send('Input.dispatchKeyEvent', {type: 'keyDown', key: 'Tab'});
        await this.send('Input.dispatchKeyEvent', {type: 'keyUp', key: 'Tab'});
    }
}
const port = await availablePort();
const origin = `http://127.0.0.1:${port}/`;
const authorization = 'Basic ' + Buffer.from('manager:fixture-password').toString('base64');
const quote = (text) => "'" + text.replaceAll("'", "'\\''") + "'";
for (const tool of ['systemd-run', 'systemctl', 'journalctl', 'probe', 'codex-bridge', 'codex-ui', 'codex']) {
    await writeFile(join(root, tool), `#!/bin/sh\nexec ${quote(fixture)} --fixture ${quote(tool)} "$@"\n`, {mode: 0o700});
}
await writeFile(join(root, 'password'), 'fixture-password\n', {mode: 0o600});
let log = '';
const app = spawn(manager, ['--config-file', '/dev/null', '--profiles', join(root, 'profiles.json'), '--manager-web-root', assets,
    '--manager-web-password-file', join(root, 'password'), 'manager-web', '--disabled=false', 'socket', '--backlog', '128', 'local', '--host', '127.0.0.1', '--port', String(port)],
    {env: {...process.env, QT_QPA_PLATFORM: 'offscreen', XDG_CONFIG_HOME: root, CODEXMANAGER_FIXTURES: root, CODEXMANAGER_AUTH_FIXTURE: 'waiting', PATH: `${root}:${process.env.PATH}`}});
app.stdout.on('data', (data) => { log += data; });
app.stderr.on('data', (data) => { log += data; });
async function http(path, action, body, headers = {}) {
    const response = await fetch(origin + path, {signal: AbortSignal.timeout(25000), method: action ? 'POST' : 'GET', headers: {
        Authorization: authorization, ...(action ? {'Content-Type': 'application/json', 'X-CodexManager-Request': '1'} : {}), ...headers},
        body: action ? JSON.stringify(body) : undefined});
    return response;
}
async function operation(action, body = {}) {
    const response = await http('api/' + action, action, body);
    assert.equal(response.status, 200, await response.clone().text());
    return response.json();
}
async function snapshot(id = '') {
    const response = await http('api/state?profile=' + id);
    return response.json();
}
let chrome, landing, upgradedPath = '';
const wuiAssets = process.env.CODEXMANAGER_WUI_ASSETS;
try {
    await until(async () => {
        let ready = false;
        try { ready = (await http('api/state')).ok; } catch { assert.equal(app.exitCode, null, log); }
        return ready;
    });
    await test('HTTP authentication, origin checks and asset containment', async () => {
        const anonymous = await fetch(origin + 'api/state');
        assert.equal(anonymous.status, 401);
        assert.equal((await http('api/new', true, {}, {Origin: 'https://untrusted.example'})).status, 403);
        assert.equal((await http('api/new', true, {}, {'X-CodexManager-Request': ''})).status, 403);
        assert.equal((await http('api/new', true, [])).status, 400);
        assert.equal((await http('api/unknown', true, {})).status, 409);
        assert.equal((await http('api/unknown')).status, 404);
        assert.equal((await http('%2e%2e%2fpassword')).status, 404);
        assert.equal((await http('colors.css')).status, 200);
        assert.equal((await http('app.js')).status, 200);
    });
    await test('real browser creation, live updates, profile opening and responsive UI', async () => {
        const chromeRoot = join(root, 'chrome');
        await mkdir(chromeRoot);
        chrome = spawn(process.env.CHROME_BIN || '/opt/google/chrome/chrome', ['--headless=new', '--no-sandbox', '--disable-gpu',
            '--remote-debugging-port=0', `--user-data-dir=${chromeRoot}`, 'about:blank'], {stdio: 'ignore'});
        const debugging = await until(async () => {
            let result;
            try { result = (await readFile(join(chromeRoot, 'DevToolsActivePort'), 'utf8')).split('\n')[0]; } catch { /* Chrome is starting. */ }
            return result;
        });
        const targets = await (await fetch(`http://127.0.0.1:${debugging}/json/list`)).json();
        const socket = new WebSocket(targets.find((t) => t.type === 'page').webSocketDebuggerUrl);
        await once(socket, 'open');
        const page = new DevTools(socket);
        await page.send('Page.enable');
        await page.send('Network.enable');
        await page.send('Fetch.enable', {handleAuthRequests: true});
        await page.send('Page.navigate', {url: origin});
        await until(() => page.evaluate(`document.querySelector('#connection')?.textContent === 'Connected to manager'`));
        await page.click('#new');
        await until(() => page.evaluate(`document.querySelector('#editor').open && document.querySelectorAll('#fields input').length > 10`));
        await page.fill('[name="Identity.name"]', 'Browser-created profile');
        await page.fill('[name="Identity.codexHome"]', join(root, 'home-one'));
        await page.fill('[name="Identity.probeExecutable"]', join(root, 'probe'));
        await page.click('#profile-form button[type=submit]');
        await until(() => page.evaluate(`!document.querySelector('#editor').open && document.querySelector('.card h2')?.textContent === 'Browser-created profile'`));
        let state = await snapshot();
        assert.equal(state.profiles.length, 1);
        const id = state.profiles[0].id;
        await page.click('.card .footer button:last-child');
        await until(() => page.evaluate(`document.querySelector('#details').open && document.querySelector('#detail-name').textContent === 'Browser-created profile'`));
        let config = (await snapshot(id)).detail.configuration;
        config.browser.transport = 'websocket-ipv4';
        config.browser.port = await availablePort();
        config.name = 'Updated from another client';
        landing = createHttpServer(async (req, res) => {
            if (wuiAssets) {
                try {
                    const path = new URL(req.url, origin).pathname;
                    res.setHeader('Content-Type', path.endsWith('.js') ? 'text/javascript' : path.endsWith('.css') ? 'text/css' : 'text/html');
                    res.end(await readFile(join(wuiAssets, path === '/' ? 'index.html' : path.slice(1))));
                } catch { res.writeHead(404).end(); }
            } else { res.end('Selected profile destination'); }
        });
        landing.on('upgrade', (request, socket) => {
            upgradedPath = request.url;
            socket.end('HTTP/1.1 503 Fixture endpoint\r\nConnection: close\r\n\r\n');
        });
        config.frontend.webSocketPath = config.browser.webSocketPath = '/selected-profile';
        landing.listen(config.browser.port, '127.0.0.1');
        await once(landing, 'listening');
        await until(async () => !(await snapshot()).profiles[0].busy);
        await operation('save', {configuration: config, revision: state.revision});
        await until(() => page.evaluate(`document.querySelector('#detail-name').textContent === 'Updated from another client'`));
        const conflict = await http('api/save', true, {configuration: config, revision: 0});
        assert.equal(conflict.status, 409);
        await page.click('[data-close="details"]');
        const screenshots = resolve(build, 'manager-web-evidence');
        await mkdir(screenshots, {recursive: true});
        for (const width of [390, 1280]) {
            await page.send('Emulation.setDeviceMetricsOverride', {width, height: 900, deviceScaleFactor: 1, mobile: false});
            assert.ok(await page.evaluate('document.documentElement.scrollWidth <= innerWidth'), `Horizontal overflow at ${width}`);
            const image = await page.send('Page.captureScreenshot');
            await writeFile(join(screenshots, `profiles-${width}.png`), Buffer.from(image.data, 'base64'));
        }
        await page.send('Emulation.setDeviceMetricsOverride', {width: 1280, height: 900, deviceScaleFactor: 1, mobile: false});
        if (wuiAssets) {
            await page.send('Page.addScriptToEvaluateOnNewDocument', {source: "localStorage.setItem('codexui.bridgeUrl', 'ws://stale.invalid/other')"});
        }
        // Start the existing fixture backend through the manager; no personal services are touched.
        await page.click('.card .footer button:first-child');
        await until(async () => (await snapshot()).profiles[0].ready);
        await until(() => page.evaluate(`location.port === '${config.browser.port}'`));
        if (wuiAssets) { await until(async () => upgradedPath === '/selected-profile', 'Explicit CodexWUI WebSocket selection'); }
        state = await snapshot();
        assert.equal(state.profiles[0].id, id);
        await page.send('Page.navigate', {url: origin});
        await until(() => page.evaluate(`document.querySelector('.card h2')?.textContent === 'Updated from another client'`));
        await page.click('.card .footer button:last-child');
        await until(() => page.evaluate(`document.querySelector('#details').open && !document.querySelector('#signin').disabled`));
        await page.click('#signin');
        await until(() => page.evaluate(`document.querySelector('#login-code').textContent === 'TEST-CODE'`));
        await page.click('#cancel');
        await until(() => page.evaluate(`document.querySelector('#cancel').disabled && !document.querySelector('#signin').disabled`));
        await page.evaluate(`document.querySelector('#method').value = 'apiKey'; document.querySelector('#method').dispatchEvent(new Event('change'))`);
        await page.fill('#key', 'sk-fixture-not-a-real-key');
        await page.click('#signin');
        await until(() => page.evaluate(`document.querySelector('#account').textContent.startsWith('Signed in') && document.querySelector('#auth-message').textContent.includes('API key stored')`));
        assert.ok(!(await readFile(join(root, 'profiles.json'), 'utf8')).includes('sk-fixture'));
        await page.click('#signout');
        await until(() => page.evaluate(`document.querySelector('#account').textContent.includes('Sign-in required')`));
        await page.click('#delete');
        await until(() => page.evaluate(`document.querySelector('#deletion').open`));
        await page.click('#exclusive');
        await page.fill('#delete-name', 'wrong name');
        await page.click('#delete-form button[type=submit]');
        await until(() => page.evaluate(`document.querySelector('#delete-error').textContent.includes('does not match')`));
        await page.fill('#delete-name', config.name);
        await page.click('#delete-form button[type=submit]');
        await until(async () => (await snapshot()).profiles.length === 0);
        await assert.rejects(readFile(join(root, 'home-one', '.codexmanager-owner')));
        socket.close();
    });
    await test('TLS listener uses the same authenticated manager API', async () => {
        const key = join(root, 'server.key'), cert = join(root, 'server.pem');
        await promisify(execFile)('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key,
            '-out', cert, '-days', '1', '-subj', '/CN=localhost']);
        const tlsPort = await availablePort();
        let tlsLog = '';
        const tls = spawn(manager, ['--config-file', '/dev/null', '--profiles', join(root, 'tls-profiles.json'),
            '--manager-web-password-file', join(root, 'password'), '--manager-web-root', assets,
            'manager-web-tls', '--disabled=false', 'tls', '--cert', cert, '--cert-key', key,
            'local', '--host', '127.0.0.1', '--port', String(tlsPort)],
            {env: {...process.env, QT_QPA_PLATFORM: 'offscreen', XDG_CONFIG_HOME: root}});
        tls.stderr.on('data', (data) => { tlsLog += data; });
        tls.stdout.on('data', (data) => { tlsLog += data; });
        try {
            await until(async () => {
                let ready = false;
                try {
                    const response = await new Promise((resolve, reject) => {
                        // Disposable self-signed certificate: exercise encryption without changing system trust.
                        const request = httpsGet(`https://127.0.0.1:${tlsPort}/api/state`,
                            {rejectUnauthorized: false, headers: {Authorization: authorization}}, resolve);
                        request.on('error', reject);
                    });
                    ready = response.statusCode === 200;
                    response.resume();
                } catch { assert.equal(tls.exitCode, null, tlsLog); }
                return ready;
            }, 'TLS API readiness');
        } finally {
            const exited = tls.exitCode === null ? once(tls, 'exit') : Promise.resolve([tls.exitCode]);
            tls.kill('SIGTERM');
            const timer = setTimeout(() => tls.kill('SIGKILL'), 10000);
            const [code] = await exited;
            clearTimeout(timer);
            assert.equal(code, 0, tlsLog);
        }
    });
    await test('bounded concurrent request delivery and disconnects', async () => {
        const replies = await Promise.all(Array.from({length: 96}, () => http('api/state')));
        assert.ok(replies.every((r) => [200, 503].includes(r.status)));
        assert.ok(replies.some((r) => r.status === 200));
        await Promise.all(replies.map((r) => r.text()));
        assert.equal((await http('api/state')).status, 200);
    });
} finally {
    if (landing) { await new Promise((done) => landing.close(done)); }
    if (chrome) { chrome.kill('SIGTERM'); await once(chrome, 'exit'); }
    const exit = app.exitCode === null && app.signalCode === null ? once(app, 'exit') : Promise.resolve([app.exitCode, app.signalCode]);
    app.kill('SIGTERM');
    const timer = setTimeout(() => app.kill('SIGKILL'), 10000);
    const [code, signal] = await exit;
    clearTimeout(timer);
    await writeFile(join(build, 'manager-web-last-run.log'), log);
    assert.notEqual(signal, 'SIGKILL', 'Manager failed to shut down its event loops');
    assert.equal(code, 0, log);
    await rm(root, {recursive: true, force: true});
}
