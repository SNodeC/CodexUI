import assert from "node:assert/strict";

export async function qualifySubmitScroll(devTools) {
    const results = await devTools.evaluate(`(async () => {
        const {socket, resume} = globalThis.codexuiApplicationProfile;
        const frame = () => new Promise(done => requestAnimationFrame(() => requestAnimationFrame(done)));
        const waitFor = async (predicate, label) => {
            for (let pass = 0; pass < 200 && !predicate(); ++pass) { await new Promise(done => setTimeout(done, 5)); }
            if (!predicate()) { throw new Error(label); }
        };
        socket.turns = [{id: "history", status: "completed", items: [
            {id: "old", type: "agentMessage", phase: "final_answer", text: "Earlier history paragraph.\\n\\n".repeat(100)}
        ]}];
        const original = socket.send.bind(socket);
        socket.send = data => {
            const message = JSON.parse(data);
            if (["turn/start", "turn/steer"].includes(message.payload?.method)) { socket.sent.push(message); }
            else { original(data); }
        };
        socket.receive({kind: "appserver", payload: {id: resume.payload.id, result: {thread: {id: "profile", status: {type: "idle"}}}}});
        const scroller = () => document.querySelector(".conversation-scroll");
        const editor = () => document.querySelector(".composer textarea");
        const send = () => document.querySelector(".composer .send-button");
        const bottom = () => scroller().scrollHeight - scroller().scrollTop - scroller().clientHeight <= 1;
        const setText = async text => {
            Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, "value").set.call(editor(), text);
            editor().dispatchEvent(new Event("input", {bubbles: true}));
            editor().focus(); await frame();
        };
        const detach = async () => {
            scroller().scrollTop = 0; scroller().dispatchEvent(new Event("scroll")); await frame();
        };
        const requests = method => socket.sent.filter(message => message.payload?.method === method);
        await waitFor(() => !scroller().classList.contains("thread-loading") && scroller().scrollHeight > scroller().clientHeight + 100, "populated conversation");
        await detach(); await setText("New task");
        const results = {detachedBeforeSend: !bottom()};
        editor().dispatchEvent(new KeyboardEvent("keydown", {key: "Enter", bubbles: true, cancelable: true}));
        await waitFor(() => requests("turn/start").length === 1, "new-turn request"); await frame();
        results.sendScrollsBottom = bottom() && document.activeElement === editor() && editor().value === "";
        const start = requests("turn/start")[0].payload;
        socket.receive({kind: "appserver", payload: {id: start.id, result: {turn: {id: "active", status: "inProgress"}}}});
        socket.receive({kind: "appserver", payload: {method: "turn/started", params: {threadId: "profile", turn: {id: "active", status: "inProgress"}}}});
        await frame(); await detach(); await setText("Steer the work");
        results.detachedBeforeSteer = !bottom() && send().textContent === "Steer";
        send().click();
        await waitFor(() => requests("turn/steer").length === 1, "steering request"); await frame();
        results.steerScrollsBottom = bottom() && document.activeElement === editor() && editor().value === "";
        socket.receive({kind: "appserver", payload: {method: "item/started", params: {threadId: "profile", turnId: "active", item: {id: "stream", type: "agentMessage", text: "Streaming follows after sending.\\n\\n".repeat(100)}}}});
        await frame(); results.streamingFollows = bottom();
        await detach(); await setText("Retain observer draft");
        socket.receive({kind: "bridge.controller", controllerConnectionId: "other"}); await frame();
        editor().form.requestSubmit(); await frame();
        results.rejectedPreservesPosition = !bottom() && editor().value === "Retain observer draft" && requests("turn/steer").length === 1;
        return results;
    })()`);
    for (const [name, passed] of Object.entries(results)) { assert.equal(passed, true, name); }
    return results;
}
