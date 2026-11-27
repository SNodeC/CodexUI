import assert from "node:assert/strict";

export async function qualifyMcpApprovals(devTools, fixture) {
    const results = await devTools.evaluate(`(async () => {
        const fixture = JSON.parse(${JSON.stringify(JSON.stringify(fixture))});
        const socket = globalThis.codexuiApplicationProfile.socket;
        const frame = () => new Promise(done => requestAnimationFrame(() => requestAnimationFrame(done)));
        const waitFor = async (predicate, description) => {
            for (let pass = 0; pass < 200 && !predicate(); ++pass) { await new Promise(done => setTimeout(done, 5)); }
            if (!predicate()) { throw new Error(description); }
        };
        const results = [];
        if (fixture.id === "mcp-required-review-label-live-envelope") {
            for (const id of ["parallel-label-a", "parallel-label-b"]) {
                socket.receive({kind: "appserver", payload: {jsonrpc: "2.0", id, method: fixture.method,
                    params: {...fixture.raw, threadId: "profile"}}});
            }
            await waitFor(() => document.querySelector(".requests-indicator"), "parallel form attention missing");
            document.querySelector(".requests-indicator").click(); await frame();
            await waitFor(() => document.querySelectorAll('.request-card input[name="label"]').length === 2, "parallel fields missing");
            const fields = [...document.querySelectorAll('.request-card input[name="label"]')];
            if (new Set(fields.map(field => field.id)).size !== 2 || fields.some(field =>
                !field.closest(".request-card").querySelector('label[for="' + field.id + '"]'))) {
                throw new Error("Concurrent MCP forms must keep independent control identities and labels");
            }
            for (const id of ["parallel-label-a", "parallel-label-b"]) {
                socket.receive({kind: "appserver", payload: {method: "serverRequest/resolved", params: {requestId: id, threadId: "profile"}}});
            }
            await waitFor(() => !document.querySelector(".request-card"), "parallel forms did not retire");
        }
        for (const attempt of fixture.uiSubmissions ?? fixture.submissions) {
            const id = "mcp-browser-approval-" + fixture.id + "-" + attempt.id;
            socket.receive({kind: "appserver", payload: {jsonrpc: "2.0", id, method: fixture.method,
                params: {...fixture.raw, threadId: "profile"}}});
            await waitFor(() => document.querySelector(".requests-indicator"), "approval attention indicator missing");
            document.querySelector(".requests-indicator").click(); await frame();
            await waitFor(() => document.querySelector(".request-card"), "MCP approval card missing");
            const card = document.querySelector(".request-card");
            const editor = card.querySelector('textarea[aria-label="Structured MCP response"]');
            if (editor && Object.hasOwn(attempt, "input")) {
                Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, "value").set.call(editor, JSON.stringify(attempt.input));
                editor.dispatchEvent(new Event("input", {bubbles:true}));
                await frame();
            }
            for (const [name, value] of Object.entries(attempt.input ?? {})) {
                const field = [...card.querySelectorAll("[name]")].find(field => field.name === name);
                if (field && card.contains(field)) {
                    if (field.disabled) {
                        field.closest(".request-question").querySelector('input[type="checkbox"]:not([name])')?.click();
                        await frame();
                    }
                    if (field instanceof HTMLSelectElement) {
                        if (field.multiple) {
                            for (const option of field.options) { option.selected = Array.isArray(value) && value.includes(option.value); }
                        } else { field.value = String(value); }
                        field.dispatchEvent(new Event("change", {bubbles: true}));
                    } else if (field.type === "checkbox") {
                        if (field.checked !== value) { field.click(); }
                    } else {
                        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value").set.call(field, String(value));
                        field.dispatchEvent(new Event("input", {bubbles: true}));
                    }
                    await frame();
                }
            }
            const buttons = [...card.querySelectorAll(".request-actions button")];
            const labels = buttons.map(button => button.textContent);
            const expectedLabels = fixture.expectedActions.map(action => action.label);
            const invalid = attempt.expected === null;
            const index = fixture.expectedActions.findIndex(action => action.value === attempt.choice);
            if (JSON.stringify(labels) !== JSON.stringify(expectedLabels)
                || buttons.some((button, position) => (!fixture.expectedActions[position].requiresInput || position === index)
                    && button.disabled !== (invalid && fixture.expectedActions[position].requiresInput))) {
                throw new Error("MCP approval choices unavailable: " + JSON.stringify({fixture:fixture.id, attempt:attempt.id,
                    labels, disabled:buttons.map(button => button.disabled), input:editor?.value, invalid}));
            }
            if (invalid && (!card.querySelector(".request-validation")?.textContent.includes("required")
                || (editor ? editor.value !== JSON.stringify(attempt.input) : Object.entries(attempt.input).some(([name, value]) => {
                    const field = [...card.querySelectorAll("[name]")].find(field => field.name === name);
                    return !field || (field.type === "checkbox" ? field.checked !== value : field.value !== String(value));
                })))) {
                throw new Error("Invalid MCP form must retain authored field values and explain schema validation");
            }
            if (card.textContent.includes("must-not-be-displayed") || card.textContent.includes("Too large to display safely")) {
                throw new Error("Private metadata affected approval presentation");
            }
            buttons[index].focus(); buttons[index].click(); await frame();
            const sent = socket.sent.filter(message => message.kind === "appserver" && message.payload.id === id
                && Object.hasOwn(message.payload, "result"));
            results.push({choice: attempt.choice, labels, sends: sent.length, result: sent[0]?.payload.result});
            socket.receive({kind: "appserver", payload: {method: "serverRequest/resolved", params: {requestId: id, threadId: "profile"}}});
            await waitFor(() => !document.querySelector(".request-card"), "resolved approval did not retire");
        }
        return results;
    })()`);
    const submissions = fixture.uiSubmissions ?? fixture.submissions;
    for (let index = 0; index < submissions.length; ++index) {
        const expected = submissions[index].expected;
        assert.equal(results[index].sends, expected === null ? 0 : 1, "Only valid MCP responses reach the wire, exactly once");
        assert.deepEqual(results[index].result, expected?.result, "MCP approval preserves authored scope and content");
    }
    return results;
}
