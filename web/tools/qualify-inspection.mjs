import assert from "node:assert/strict";
import {mkdir, writeFile} from "node:fs/promises";
import {join} from "node:path";

export async function qualifyInspection(devTools) {
    const results = await devTools.evaluate(`(async () => {
        const {socket} = globalThis.codexuiApplicationProfile;
        const frame = () => new Promise(done => requestAnimationFrame(() => requestAnimationFrame(done)));
        const waitFor = async (predicate, label) => {
            for (let pass = 0; pass < 200 && !predicate(); ++pass) { await new Promise(done => setTimeout(done, 5)); }
            if (!predicate()) { throw new Error(label); }
        };
        const metadata = {id: "inspection", name: "Inspection result", status: {type: "idle"}, historyMode: "paginated", ephemeral: false};
        const turns = [];
        const original = socket.send.bind(socket);
        socket.send = data => {
            const envelope = JSON.parse(data), request = envelope.payload;
            if (envelope.kind === "appserver" && request?.params?.threadId === metadata.id) {
                socket.sent.push(envelope);
                const result = request.method === "thread/resume" ? {thread: metadata}
                    : request.method === "thread/turns/list" ? {data: turns.toReversed(), nextCursor: null}
                        : {data: (turns.find(turn => turn.id === request.params.turnId)?.items ?? []).toReversed()
                            .map(item => ({turnId: request.params.turnId, item})), nextCursor: null};
                queueMicrotask(() => socket.receive({kind: "appserver", payload: {jsonrpc: "2.0", id: request.id, result}}));
            } else { original(data); }
        };
        const notify = (method, params) => socket.receive({kind: "appserver", payload: {method, params}});
        notify("thread/started", {thread: metadata});
        const row = () => [...document.querySelectorAll(".thread-row")].find(element => element.textContent.includes(metadata.name));
        const pink = () => !!row()?.querySelector(".status-dot.uninspected");
        const addAnswer = (id, failed = false, command = false) => {
            const turn = {id, status: failed ? "failed" : "completed", items: [
                {id: "prompt", type: "userMessage", content: [{type: "text", text: "Earlier conversation paragraph.\\n\\n".repeat(100)}]},
                command ? {id: "answer", type: "commandExecution", command: "printf failed", aggregatedOutput: "Failed command result " + id, status: "failed"}
                    : {id: "answer", type: "agentMessage", phase: failed ? "commentary" : "final_answer", text: "Final result " + id},
            ]};
            turns.push(turn);
            notify("turn/started", {threadId: metadata.id, turn: {id, status: "inProgress"}});
            for (const item of turn.items) { notify("item/completed", {threadId: metadata.id, turnId: id, item}); }
        };
        const complete = (id, status = "completed") => {
            notify("turn/completed", {threadId: metadata.id, turn: {id, status}});
            notify("thread/status/changed", {threadId: metadata.id, status: {type: "idle"}});
        };
        const answer = id => [...document.querySelectorAll(".conversation-card.final")].find(element => element.textContent.includes("Final result " + id));
        const scroll = () => document.querySelector(".conversation-scroll");
        const editor = () => document.querySelector(".composer-dock textarea");
        const visible = id => {
            const body = answer(id)?.querySelector(":scope > .safe-markdown"), container = scroll();
            const bounds = body?.getBoundingClientRect(), viewport = container?.getBoundingClientRect();
            const bottom = Math.min(viewport?.bottom ?? 0, document.querySelector(".composer-dock").getBoundingClientRect().top);
            return !!bounds && bounds.height > 0 && bounds.bottom > viewport.top && bounds.top < bottom;
        };
        const key = () => editor().dispatchEvent(new KeyboardEvent("keydown", {key: "x", bubbles: true}));
        const results = {};
        await waitFor(row, "inspection thread missing");
        addAnswer("background"); complete("background"); await frame();
        results.backgroundPink = pink();
        row().click();
        await waitFor(() => answer("background") && !scroll().classList.contains("thread-loading"), "result did not hydrate");
        scroll().scrollTop = scroll().scrollHeight; await frame();
        results.selectionPreservesPink = pink() && visible("background");
        editor().focus();
        window.dispatchEvent(new Event("focus"));
        scroll().dispatchEvent(new MouseEvent("mousemove", {bubbles: true}));
        scroll().dispatchEvent(new Event("scroll", {bubbles: true}));
        row().click(); await frame();
        results.passiveEventsPreservePink = pink();
        answer("background").querySelector(":scope > .safe-markdown").dispatchEvent(new PointerEvent("pointerdown", {bubbles: true}));
        await frame(); results.visibleClickClearsPink = !pink();
        addAnswer("foreground"); complete("foreground"); await frame();
        scroll().scrollTop = scroll().scrollHeight; await frame();
        results.foregroundPink = pink() && visible("foreground");
        scroll().scrollTop = 0; await frame(); key(); await frame();
        results.offscreenInputPreservesPink = pink() && !visible("foreground");
        scroll().scrollTop = scroll().scrollHeight; await frame();
        results.programmaticScrollPreservesPink = pink();
        scroll().dispatchEvent(new WheelEvent("wheel", {deltaY: 100, bubbles: true}));
        await frame(); results.visibleWheelClearsPink = !pink();
        addAnswer("collapsed"); await frame();
        scroll().scrollTop = scroll().scrollHeight; await frame();
        const collapsedAnswer = answer("collapsed");
        collapsedAnswer.querySelector(".card-fold-button").click(); await frame();
        complete("collapsed"); await frame(); key(); await frame();
        results.collapsedInputPreservesPink = pink() && !visible("collapsed");
        collapsedAnswer.querySelector(".card-fold-button").click(); await frame();
        scroll().scrollTop = scroll().scrollHeight; await frame();
        results.expansionPreservesPink = pink() && visible("collapsed");
        globalThis.codexuiInspectionResults = {results, pink, addAnswer, complete, row, frame, waitFor, scroll, editor, key};
        editor().focus();
        return results;
    })()`);
    for (const [name, passed] of Object.entries(results)) { assert.equal(passed, true, name); }
    const capture = async name => {
        if (process.env.CODEXUI_TEST_EVIDENCE_DIR) {
            await mkdir(process.env.CODEXUI_TEST_EVIDENCE_DIR, {recursive: true});
            const screenshot = await devTools.call("Page.captureScreenshot", {format: "png"});
            await writeFile(join(process.env.CODEXUI_TEST_EVIDENCE_DIR, `browser-inspection-${name}.png`), Buffer.from(screenshot.data, "base64"));
        }
    };
    await capture("pink");
    await devTools.call("Input.dispatchKeyEvent", {type: "keyDown", key: "x", code: "KeyX", text: "x"});
    await devTools.call("Input.dispatchKeyEvent", {type: "keyUp", key: "x", code: "KeyX"});
    const typingClearsPink = await devTools.evaluate(`new Promise(done => requestAnimationFrame(() => requestAnimationFrame(() => done(!globalThis.codexuiInspectionResults.pink()))))`);
    assert.equal(typingClearsPink, true, "real keyboard input with visible final content clears pink");
    await capture("green");
    const failures = await devTools.evaluate(`(async () => {
        const {addAnswer, complete, row, frame, waitFor, scroll, editor, key} = globalThis.codexuiInspectionResults;
        const red = () => !!row()?.querySelector(".status-dot.failed");
        const card = id => [...document.querySelectorAll(".conversation-card")].find(element =>
            element.querySelector(":scope > .safe-markdown, :scope > .command-output")?.textContent.includes(id));
        const visible = id => {
            const bounds = card(id)?.querySelector(":scope > .safe-markdown, :scope > .command-output")?.getBoundingClientRect();
            const viewport = scroll().getBoundingClientRect();
            const bottom = Math.min(viewport.bottom, document.querySelector(".composer-dock").getBoundingClientRect().top);
            return !!bounds && bounds.height > 0 && bounds.bottom > viewport.top && bounds.top < bottom;
        };
        const results = {};
        for (const command of [false, true]) {
            const id = command ? "failed-command" : "failed-without-final";
            addAnswer(id, true, command); complete(id, "failed"); await frame();
            scroll().scrollTop = scroll().scrollHeight; await frame();
            await waitFor(() => red() && visible(id), "failed result was not presented");
            results[id + "RedWithoutFinal"] = red() && !card(id).classList.contains("final");
            editor().focus(); window.dispatchEvent(new Event("focus"));
            scroll().dispatchEvent(new MouseEvent("mousemove", {bubbles: true}));
            scroll().dispatchEvent(new Event("scroll", {bubbles: true}));
            row().click(); await frame();
            results[id + "PassiveEventsPreserveRed"] = red();
            scroll().scrollTop = 0; await frame(); key(); await frame();
            results[id + "OffscreenPreservesRed"] = red() && !visible(id);
            scroll().scrollTop = scroll().scrollHeight; await frame();
            results[id + "ProgrammaticScrollPreservesRed"] = red();
            const failedCard = card(id);
            failedCard.querySelector(".card-fold-button").click(); await frame(); key(); await frame();
            results[id + "CollapsedPreservesRed"] = red() && !visible(id);
            failedCard.querySelector(".card-fold-button").click(); await frame();
            scroll().scrollTop = scroll().scrollHeight; await frame();
            results[id + "ExpansionPreservesRed"] = red() && visible(id);
            scroll().dispatchEvent(command ? new WheelEvent("wheel", {deltaY: 100, bubbles: true})
                : new PointerEvent("pointerdown", {bubbles: true}));
            await frame(); results[id + "DeliberateInputClearsRed"] = !red();
            complete(id, "failed"); await frame();
            results[id + "DuplicateDoesNotRearm"] = !red();
        }
        addAnswer("failed-typing", true); complete("failed-typing", "failed"); await frame();
        scroll().scrollTop = scroll().scrollHeight; await frame();
        results.failedTypingStartsRed = red() && visible("failed-typing");
        globalThis.codexuiInspectionResults.red = red;
        editor().focus();
        return results;
    })()`);
    for (const [name, passed] of Object.entries(failures)) { assert.equal(passed, true, name); }
    await capture("red");
    await devTools.call("Input.dispatchKeyEvent", {type: "keyDown", key: "x", code: "KeyX", text: "x"});
    await devTools.call("Input.dispatchKeyEvent", {type: "keyUp", key: "x", code: "KeyX"});
    const typingClearsRed = await devTools.evaluate(`new Promise(done => requestAnimationFrame(() => requestAnimationFrame(() => done(!globalThis.codexuiInspectionResults.red()))))`);
    assert.equal(typingClearsRed, true, "real keyboard input with visible failed content clears red");
    await capture("red-cleared");
    return {...results, typingClearsPink, ...failures, typingClearsRed};
}
