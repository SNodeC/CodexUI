// SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
const $ = (id) => document.getElementById(id);
let state = {profiles: [], detail: {}, revision: 0};
let selected = location.hash.slice(1), opening = '', editing, deletion;
let loading = false, dirty = false, connected = false;
let actionError = '';
const cards = new Map();
function report(error) {
    const dialogs = [...document.querySelectorAll('dialog[open]')];
    const target = dialogs.at(-1)?.querySelector('[role=alert]') || $('error');
    if (target === $('error')) { actionError = error.message; }
    target.textContent = error.message;
}
async function perform(task, userAction = true) {
    if (userAction) { actionError = ''; }
    try { await task(); } catch (error) { report(error); }
}
async function api(action, body = {}) {
    let response;
    try {
        response = await fetch(action === 'state' ? `api/state?profile=${encodeURIComponent(selected)}` : `api/${action}`, {
        method: action === 'state' ? 'GET' : 'POST',
        headers: {'Content-Type': 'application/json', 'X-CodexManager-Request': '1'},
        body: action === 'state' ? undefined : JSON.stringify(body),
        cache: 'no-store'
        });
    } catch {
        throw new Error(action === 'state' ? 'Manager connection lost.' : 'Connection lost. The operation may have been accepted; reconnect and check status before retrying.');
    }
    const text = await response.text();
    const result = response.headers.get('Content-Type')?.includes('application/json') ? JSON.parse(text) : {error: text};
    if (!response.ok) { throw new Error(result.error || text); }
    return result;
}
function button(parent, text, task, className = '') {
    const element = document.createElement('button');
    element.textContent = text;
    element.className = className;
    element.onclick = () => perform(task);
    parent.append(element);
    return element;
}
function current(id = selected) { return state.profiles.find((p) => p.id === id); }
async function command(action, extra = {}) {
    await api(action, {id: selected, revision: state.revision, ...extra});
    await reload();
}
async function reload() {
    dirty = true;
    if (!loading) {
        loading = true;
        try {
            while (dirty) {
                dirty = false;
                const id = selected;
                const next = await api('state');
                if (id === selected) {
                    state = next;
                    render();
                } else { dirty = true; }
            }
        } finally { loading = false; }
    }
}
async function join(id) {
    const profile = current(id);
    if (!profile.browserUrl) { throw new Error('Configure browser access for this profile first.'); }
    if (profile.ready) {
        location.assign(profile.browserUrl);
    } else {
        await api('start', {id});
        opening = id;
        await reload();
    }
}
async function showDetails(id) {
    selected = id;
    location.hash = id;
    if (!$('details').open) { $('details').showModal(); }
    await reload();
}
function renderLogs() {
    if (!$('pause').checked) {
        $('logs').textContent = (state.detail.logs || '').split('\n').filter((line) => line.toLowerCase().includes($('log-filter').value.toLowerCase())).join('\n');
    }
}
function render() {
    $('new').disabled = !connected;
    $('import').disabled = !connected;
    const query = $('search').value.toLowerCase();
    for (const [id, card] of cards) {
        if (!current(id)) { card.remove(); cards.delete(id); }
    }
    for (const p of state.profiles) {
        let card = cards.get(p.id);
        if (!card) {
            card = document.createElement('article');
            card.className = 'card';
            for (const tag of ['h2', 'p', 'p', 'p', 'p']) { card.append(document.createElement(tag)); }
            const footer = document.createElement('div');
            footer.className = 'footer';
            button(footer, 'Open CodexWUI', () => join(p.id), 'primary');
            button(footer, 'Details →', () => showDetails(p.id));
            card.append(footer);
            cards.set(p.id, card);
            $('profiles').append(card);
        }
        const values = [p.name, p.status, p.account, p.description || 'An independent Codex environment', p.location];
        values.forEach((text, index) => { card.children[index].textContent = text; });
        card.style.borderTopColor = p.accent;
        card.hidden = !`${p.name} ${p.description} ${p.location}`.toLowerCase().includes(query);
        const open = card.querySelector('button');
        open.disabled = !connected || !p.browserUrl || p.busy;
        open.textContent = p.busy ? 'Working…' : p.ready ? 'Open CodexWUI' : 'Start & open CodexWUI';
        open.title = p.browserUrl || 'Configure a browser WebSocket listener in this profile.';
    }
    $('summary').textContent = `${state.profiles.length} profiles · ${state.profiles.filter((p) => p.ready).length} ready`;
    $('error').textContent = actionError || state.error || (state.profiles.length ? '' : 'Create a profile to get started.');
    const profile = current();
    if (profile && state.detail.configuration) {
        $('detail-name').textContent = profile.name;
        $('detail-status').textContent = profile.status;
        $('account').textContent = profile.account;
        $('components').textContent = Object.entries(state.detail.components).map(([name, status]) => `${name}: ${status}`).join('\n');
        $('configuration').textContent = JSON.stringify(state.detail.configuration, null, 2);
        $('instructions').textContent = state.detail.instructions;
        $('auth-message').textContent = state.detail.authenticationMessage;
        $('login-code').textContent = state.detail.loginCode;
        const url = state.detail.loginUrl;
        $('login-url').hidden = !url.startsWith('https://');
        $('login-url').href = url.startsWith('https://') ? url : '#';
        $('signin').disabled = !connected || !state.detail.authenticationAvailable;
        $('signout').disabled = !connected || !state.detail.authenticationAvailable;
        $('cancel').disabled = !connected || !state.detail.authenticating;
        $('edit').disabled = !connected || !profile.editable;
        $('remove').disabled = !connected || !profile.editable;
        for (const action of ['start', 'stop', 'native', 'delete']) { $(action).disabled = !connected || profile.busy; }
        renderLogs();
    } else if ($('details').open) { $('details').close(); }
    const target = current(opening);
    if (opening && target?.ready) { opening = ''; location.assign(target.browserUrl); }
    if (deletion && !current(deletion.id)) { $('deletion').close(); deletion = undefined; }
}
function download(name, content) {
    const url = URL.createObjectURL(new Blob([content], {type: 'application/json'}));
    const link = document.createElement('a');
    link.href = url;
    link.download = name;
    link.click();
    URL.revokeObjectURL(url);
}
const labels = {
    id: 'Profile ID',
    name: 'Name',
    description: 'Description',
    accent: 'Accent',
    codexHome: 'CODEX_HOME',
    uiExecutable: 'Native CodexUI executable',
    bridgeExecutable: 'Bridge executable',
    appServerExecutable: 'App-server executable',
    probeExecutable: 'Readiness client executable',
    shareInstructions: 'Link global instructions for a new local home',
    browserAddress: 'Browser address (optional public HTTP(S) URL)',
    ownership: 'Process ownership',
    sshTarget: 'SSH host alias or user@host',
    identityFile: 'SSH identity-file path',
    workingDirectory: 'Working directory',
    environment: 'Environment (KEY=value, one per line; no secrets)',
    transport: 'Transport',
    host: 'Connect host / browser hostname',
    bindHost: 'Listener bind address',
    port: 'Port',
    socketPath: 'Unix socket path',
    webSocketPath: 'WebSocket path',
    certificate: 'TLS certificate path',
    privateKey: 'TLS private-key path',
    caCertificate: 'CA certificate path',
    serverName: 'TLS server name',
    channel: 'Bluetooth channel'
};
async function edit(configuration, revision) {
    editing = {configuration: structuredClone(configuration), revision};
    $('fields').replaceChildren();
    configuration = editing.configuration;
    const groups = {Identity: Object.fromEntries(Object.entries(configuration).filter(([, value]) => typeof value !== 'object')), Bridge: configuration.bridge, 'App-server': configuration.appServer, 'Native connection': configuration.frontend, Provider: configuration.provider, 'Additional browser listener': configuration.browser};
    for (const [group, fields] of Object.entries(groups)) {
        const section = document.createElement('fieldset');
        const legend = document.createElement('legend');
        legend.textContent = group;
        section.append(legend);
        for (const [key, value] of Object.entries(fields)) {
            const label = document.createElement('label');
            label.textContent = labels[key];
            let choices;
            if (key === 'ownership') { choices = [[0, 'Managed locally'], [1, 'Managed over SSH'], [2, 'Started externally']]; }
            if (key === 'accent') { choices = ['blue', 'green', 'purple', 'teal', 'amber', 'orange'].map((x) => [x, x]); }
            if (key === 'transport') {
                const transports = state.transports[group === 'Provider' ? 'provider' : group === 'Additional browser listener' ? 'browser' : 'frontend'];
                choices = transports.map((x) => [x, x]);
            }
            const input = document.createElement(choices ? 'select' : key === 'environment' ? 'textarea' : 'input');
            if (choices) {
                for (const [id, text] of choices) { input.add(new Option(text, id)); }
            } else if (key !== 'environment') { input.type = typeof value === 'boolean' ? 'checkbox' : typeof value === 'number' ? 'number' : 'text'; }
            input.name = `${group}.${key}`;
            input.value = key === 'environment' ? Object.entries(value).map(([k, v]) => `${k}=${v}`).join('\n') : value;
            input.checked = value === true;
            input.readOnly = key === 'id';
            label.append(input);
            section.append(label);
            input.onchange = () => perform(async () => {
                input.setCustomValidity('');
                try {
                    const target = group === 'Identity' ? editing.configuration : fields;
                    if (key === 'environment') {
                        target[key] = Object.fromEntries(input.value.split('\n').filter((line) => line.trim()).map((line) => {
                            const index = line.indexOf('=');
                            if (index < 1) { throw new Error('Environment entries must use KEY=value.'); }
                            return [line.slice(0, index), line.slice(index + 1)];
                        }));
                    } else { target[key] = typeof value === 'boolean' ? input.checked : typeof value === 'number' ? Number(input.value) : input.value; }
                    if (group !== 'Identity') {
                        const groupKey = {'Bridge': 'bridge', 'App-server': 'appServer', 'Native connection': 'frontend', 'Provider': 'provider', 'Additional browser listener': 'browser'}[group];
                        editing.configuration[groupKey] = fields;
                    }
                } catch (error) { input.setCustomValidity(error.message); throw error; }
            });
        }
        $('fields').append(section);
    }
    $('edit-error').textContent = '';
    $('editor').showModal();
}
$('profile-form').onsubmit = (event) => {
    event.preventDefault();
    perform(async () => {
        await api('save', editing);
        $('editor').close();
        await reload();
    });
};
$('new').onclick = () => perform(async () => { const draft = await api('new'); await edit(draft.configuration, draft.revision); });
$('import').onclick = () => $('import-file').click();
$('import-file').onchange = () => perform(async () => {
    const document = JSON.parse(await $('import-file').files[0].text());
    await api('import', {...document, revision: state.revision});
    $('import-file').value = '';
    await reload();
});
for (const [action, label] of [['start', 'Start / connect backend'], ['native', 'Open native app on manager desktop'], ['stop', 'Stop owned components…'], ['refresh', 'Refresh']]) {
    button($('actions'), label, async () => {
        if (action !== 'stop' || confirm('Stop only this profile’s owned components?')) { await command(action); }
    }).id = action;
}
button($('actions'), 'Configure…', () => edit(state.detail.configuration, state.revision)).id = 'edit';
button($('actions'), 'Export', async () => download('profile.json', JSON.stringify({version: 1, profiles: [state.detail.configuration]}, null, 2)));
button($('actions'), 'Remove registration…', async () => {
    if (confirm('Remove this registration? Profile data and credentials will remain.')) { await command('remove'); }
}).id = 'remove';
button($('actions'), 'Delete profile and data…', async () => {
    deletion = {id: selected, name: current().name, revision: state.revision};
    $('delete-preview').textContent = state.detail.deletionPreview;
    $('delete-instructions').textContent = state.detail.deletionInstructions;
    $('delete-caption').textContent = `Type the exact profile name: ${deletion.name}`;
    $('delete-form').reset();
    $('delete-error').textContent = '';
    $('deletion').showModal();
}, 'danger').id = 'delete';
$('delete-form').onsubmit = (event) => {
    event.preventDefault();
    perform(async () => {
        if ($('delete-name').value !== deletion.name) { throw new Error('The profile name does not match.'); }
        await api('delete', {...deletion, name: $('delete-name').value, exclusive: $('exclusive').checked});
        $('deletion').close();
        await reload();
    });
};
$('signin').onclick = () => perform(async () => {
    const key = $('key').value;
    $('key').value = '';
    await command('signin', {method: $('method').value, key});
});
$('signout').onclick = () => perform(async () => { if (confirm('Sign out on this profile’s app-server? Other clients sharing its home will also lose this account.')) { await command('signout'); } });
$('cancel').onclick = () => perform(() => command('cancel'));
$('method').onchange = () => { $('key-label').hidden = $('method').value !== 'apiKey'; };
$('export-logs').onclick = () => download('profile-logs.txt', $('logs').textContent);
$('log-filter').oninput = renderLogs;
$('pause').onchange = renderLogs;
$('search').oninput = render;
for (const close of document.querySelectorAll('[data-close]')) { close.onclick = () => $(close.dataset.close).close(); }
const events = new EventSource('api/events');
events.onmessage = () => perform(reload, false);
events.onopen = () => { connected = true; $('connection').textContent = 'Connected to manager'; perform(reload, false); };
events.onerror = () => { connected = false; $('connection').textContent = 'Connection lost · reconnecting…'; render(); };
perform(async () => { await reload(); if (selected && current()) { await showDetails(selected); } });
