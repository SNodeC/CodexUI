import assert from "node:assert/strict";

export async function qualifyContextMenus(devTools) {
    const result = await devTools.evaluate(`(async () => {
        const frame = () => new Promise(done => requestAnimationFrame(() => requestAnimationFrame(done)));
        const waitFor = async (predicate, message) => {
            for (let pass = 0; pass < 200 && !predicate(); ++pass) { await new Promise(done => setTimeout(done, 5)); }
            if (!predicate()) { throw new Error(message); }
        };
        const socket = globalThis.codexuiApplicationProfile.socket;
        const original = socket.send.bind(socket);
        const turns = [1, 2, 3].map(index => ({id: "menu-turn-" + index, status: "completed", items: [
            {id: "menu-item-" + index, type: "userMessage", content: [{type: "text", text: "Menu prompt " + index}]}]}));
        const source = {id: "menu-source", name: "Menu source", cwd: "/source", historyMode: "paginated", ephemeral: false,
            projectId: "", section: null, status: {type: "idle"}, turns};
        const threads = new Map([[source.id, source]]);
        const groups = {project: [{id: "menu-project", name: "Existing project", roots: []}], section: [{id: "menu-section", name: "Existing section"}]};
        const held = [];
        const reply = (request, result) => socket.receive({kind: "appserver", payload: {jsonrpc: "2.0", id: request.id, result}});
        const error = (request, message) => socket.receive({kind: "appserver", payload: {jsonrpc: "2.0", id: request.id, error: {code: -32000, message}}});
        const requests = method => socket.sent.filter(message => message.kind === "appserver" && message.payload.method === method).map(message => message.payload);
        socket.send = data => {
            const envelope = JSON.parse(data), request = envelope.payload;
            if (envelope.kind === "appserver" && (threads.has(request?.params?.threadId)
                || ["project/create", "threadSection/create", "project/list", "threadSection/list", "thread/list"].includes(request?.method))) {
                socket.sent.push(envelope);
                if (["thread/fork", "project/create", "threadSection/create", "thread/metadata/update", "thread/section/move"].includes(request.method)) {
                    held.push(request);
                } else {
                    const thread = threads.get(request.params.threadId);
                    const result = request.method === "thread/resume" || request.method === "thread/read" ? {thread}
                        : request.method === "thread/turns/list" ? {data: thread.turns.toReversed(), nextCursor: null}
                        : request.method === "thread/items/list" ? {data: thread.turns.find(turn => turn.id === request.params.turnId).items.toReversed().map(item => ({turnId: request.params.turnId, item})), nextCursor: null}
                        : request.method === "project/list" ? {data: groups.project, nextCursor: null}
                        : request.method === "threadSection/list" ? {data: groups.section, nextCursor: null}
                        : request.method === "thread/list" ? {data: [...threads.values()], nextCursor: null} : {};
                    queueMicrotask(() => reply(request, result));
                }
            } else { original(data); }
        };
        const row = () => [...document.querySelectorAll(".thread-row")].find(element => element.querySelector(".thread-title")?.textContent === source.name);
        const openThreadMenu = async () => { row().closest(".thread-row-wrap").querySelector(".thread-menu-trigger").click(); await frame(); };
        const openTurnMenu = async () => {
            document.querySelectorAll('button[aria-label="Turn actions"]')[1].click(); await frame();
        };
        const turnButtons = () => [...document.querySelector('[aria-label="Turn actions"][role="menu"]').querySelectorAll("button")];
        const input = (element, value) => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value").set.call(element, value); element.dispatchEvent(new Event("input", {bubbles: true})); };
        socket.receive({kind: "appserver", payload: {method: "thread/started", params: {thread: source}}});
        await waitFor(row, "source row missing"); row().click();
        await waitFor(() => document.querySelectorAll('button[aria-label="Turn actions"]').length === 3, "source history missing");
        await frame(); await openTurnMenu();
        const menu = document.querySelector('[aria-label="Turn actions"][role="menu"]');
        const ordered = menu.children[0].textContent === "Quick fork from here" && menu.children[1].textContent === "Fork from here with options…"
            && menu.children[2].getAttribute("role") === "separator" && menu.children[3].textContent === "Remove this turn and all later turns…";
        menu.dispatchEvent(new KeyboardEvent("keydown", {key: "ArrowDown", bubbles: true, cancelable: true}));
        const keyboard = document.activeElement === turnButtons()[1];
        const divider = menu.querySelector('hr');
        const visibleDivider = divider.getBoundingClientRect().width > 100 && getComputedStyle(divider).borderTopWidth === "1px";
        turnButtons()[1].click(); await frame();
        let dialog = document.querySelector('[aria-labelledby="thread-options-title"]');
        const optionsTitle = dialog.querySelector("h2").textContent === "Fork with options";
        [...dialog.querySelectorAll("button")].find(button => button.textContent === "Cancel").click(); await frame();
        const cancelledFork = requests("thread/fork").length === 0;
        for (const advanced of [false, true]) {
            await openTurnMenu(); turnButtons()[advanced ? 1 : 0].click(); await frame();
            if (advanced) {
                dialog = document.querySelector('[aria-labelledby="thread-options-title"]');
                input(dialog.querySelector('label input'), "/chosen"); await frame();
                dialog.querySelector("form").requestSubmit(); await frame();
            }
            const fork = held.findLast(request => request.method === "thread/fork");
            if (fork.params.lastTurnId !== "menu-turn-2" || fork.params.threadId !== source.id || fork.params.cwd !== (advanced ? "/chosen" : undefined)) {
                throw new Error("wrong fork boundary or options");
            }
            const forked = {...source, id: advanced ? "options-fork" : "quick-fork", name: advanced ? "Options fork" : "Quick fork",
                forkedFromId: source.id, turns: turns.slice(0, 2)};
            threads.set(forked.id, forked); reply(fork, {thread: forked});
            await waitFor(() => document.querySelector(".thread-heading-context h1")?.textContent.includes(forked.name) || requests("thread/name/set").some(request => request.params.threadId === forked.id), "fork not selected");
            await frame();
            row().click(); await waitFor(() => document.querySelectorAll('button[aria-label="Turn actions"]').length === 3, "source not restored"); await frame();
        }
        let creationOrder = true, cancelledCreation = true, assignmentTarget = true, failureNotice = true, submittedCreation = true;
        for (const kind of ["project", "section"]) {
            const createMethod = kind === "project" ? "project/create" : "threadSection/create";
            for (const outcome of ["cancel", "success", "create-failure", "assign-failure"]) {
                await openThreadMenu();
                const select = document.querySelector('select[aria-label="Assign ' + kind + '"]');
                creationOrder &&= select.children[0].hidden && select.children[1].textContent === "New " + kind + "…" && select.children[2].tagName === "HR";
                const count = requests(createMethod).length;
                select.value = "__new__"; select.dispatchEvent(new Event("change", {bubbles: true})); await frame();
                dialog = document.querySelector(".group-dialog");
                if (outcome === "cancel") {
                    [...dialog.querySelectorAll("button")].find(button => button.textContent === "Cancel").click(); await frame();
                    cancelledCreation &&= requests(createMethod).length === count;
                } else {
                    input(dialog.querySelector("input"), "Created " + kind); await frame();
                    dialog.querySelector("form").requestSubmit(); await frame();
                    const create = held.findLast(request => request.method === createMethod);
                    const cancel = [...dialog.querySelectorAll("button")].find(button => button.textContent === "Cancel");
                    const cancellation = new Event("cancel", {cancelable: true}); dialog.dispatchEvent(cancellation);
                    submittedCreation &&= cancel.disabled && cancellation.defaultPrevented && dialog.open;
                    if (outcome === "create-failure") {
                        error(create, "creation denied"); await frame();
                        failureNotice &&= !!document.querySelector(".group-dialog [role=alert]");
                        [...dialog.querySelectorAll("button")].find(button => button.textContent === "Cancel").click(); await frame();
                    } else {
                        const group = {id: "created-" + kind + "-" + outcome, name: "Created " + kind, ...(kind === "project" ? {roots: []} : {})};
                        groups[kind].push(group); reply(create, {[kind]: group}); await frame();
                        const assignment = held.findLast(request => request.method === (kind === "project" ? "thread/metadata/update" : "thread/section/move"));
                        assignmentTarget &&= assignment.params.threadId === source.id && assignment.params[kind + "Id"] === group.id;
                        if (outcome === "assign-failure") { error(assignment, "assignment denied"); }
                        else {
                            if (kind === "project") { source.projectId = group.id; } else { source.section = group; }
                            reply(assignment, kind === "project" ? {thread: source} : {});
                        }
                        await frame();
                        await waitFor(() => !document.querySelector(".group-dialog"), "successful creation did not close");
                        if (outcome === "assign-failure") { failureNotice &&= document.querySelector('[role="alert"]')?.textContent.includes("assignment denied"); }
                    }
                }
            }
        }
        await openTurnMenu();
        socket.send = original;
        return {ordered, keyboard, visibleDivider, optionsTitle, cancelledFork, forks: requests("thread/fork").length === 2,
            sourcePreserved: source.turns.length === 3 && requests("thread/revert").length === 0,
            creationOrder, cancelledCreation, assignmentTarget, failureNotice, submittedCreation};
    })()`);
    for (const [name, value] of Object.entries(result)) {
        assert.equal(value, true, `Context menus: ${name}`);
    }
    return result;
}
