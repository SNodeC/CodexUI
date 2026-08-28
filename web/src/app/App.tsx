import {useMemo, useState, useSyncExternalStore} from "react";
import type {FormEvent, ReactNode} from "react";
import {classifyStatus, stableKey} from "../index.js";
import type {
    AgentActivityData, CommandExecutionData, FileChangesData, LocalPromptData,
    ReasoningData, UserMessageData, AgentMessageData, VisibleCardData,
} from "../index.js";
import type {BrowserFrontendSession} from "./BrowserFrontendSession.js";

function humanize(value: string): string {
    if (value === "contextCompaction") return "Context compaction";
    const spaced = value.replaceAll(/[._/-]+/gu, " ").replace(/([a-z\d])([A-Z])/gu, "$1 $2").trim();
    return spaced === "" ? "Activity" : spaced[0]!.toUpperCase() + spaced.slice(1);
}

function StatusDot({tone}: {tone: string}) { return <span className={`status-dot ${tone}`} aria-hidden="true" />; }

function ThreadPane({session, revision}: {session: BrowserFrontendSession; revision: number}) {
    void revision;
    const selected = session.getSnapshot().selectedThreadId;
    return <aside className="thread-pane">
        <div className="pane-heading"><div><span className="eyebrow">Workspace</span><h2>Threads</h2></div>
            <button className="icon-button" onClick={() => session.beginNewThread()} title="New thread">＋</button></div>
        <div className="thread-list">
            {session.model.threadOrder().map(id => {
                const thread = session.model.thread(id)!;
                const status = classifyStatus(thread.status);
                return <button key={id} className={`thread-row ${selected === id ? "selected" : ""}`}
                    onClick={() => session.selectThread(id)}>
                    <StatusDot tone={status.tone || "muted"} />
                    <span><strong>{thread.title}</strong><small>{thread.cwd || thread.preview || id}</small></span>
                </button>;
            })}
        </div>
        <button className="refresh-button" onClick={() => session.requestThreads()}>↻ Refresh threads</button>
    </aside>;
}

function Card({card}: {card: VisibleCardData}) {
    let title = humanize(card.kind);
    let body: ReactNode;
    if (card.kind === "userMessage") {
        const data = card.payload as UserMessageData; title = "You";
        body = <><div className="card-text">{data.text}</div>{data.imagePaths.map(path => <code key={path}>{path}</code>)}</>;
    } else if (card.kind === "localPrompt") {
        const data = card.payload as LocalPromptData; title = data.state === "failed" ? "Not sent" : "You";
        body = <><div className="card-text">{data.prompt}</div>{data.error && <div className="error-text">{data.error}</div>}</>;
    } else if (card.kind === "agentMessage") {
        const data = card.payload as AgentMessageData; title = data.finalAnswer ? "Codex" : "Agent message";
        body = <div className="card-text markdown-text">{data.text}</div>;
    } else if (card.kind === "reasoning") {
        const data = card.payload as ReasoningData; title = "Reasoning";
        body = data.summary ? <div className="card-text">{data.summary}</div> : <div className="activity-line"><i />Working…</div>;
    } else if (card.kind === "commandExecution") {
        const data = card.payload as CommandExecutionData; title = "Command";
        body = <><code className="command-line">{data.command}</code>{data.output && <pre>{data.output}</pre>}
            <small>{humanize(data.status)}{data.exitCode !== undefined ? ` · exit ${data.exitCode}` : ""}</small></>;
    } else if (card.kind === "fileChanges") {
        const data = card.payload as FileChangesData; title = "File changes";
        body = <div className="file-list">{data.changes.map(change => <div key={`${change.path}:${change.kind}`}>
            <span>{change.path}</span><small>{humanize(change.kind)} {change.additions !== undefined && <b className="plus">+{change.additions}</b>} {change.deletions !== undefined && <b className="minus">−{change.deletions}</b>}</small>
        </div>)}</div>;
    } else if (card.kind === "agentActivity") {
        const data = card.payload as AgentActivityData; title = humanize(data.tool || data.kind || "Agent activity");
        body = <><div className="card-text">{data.prompt || data.resultText || data.childThreadId}</div><small>{humanize(data.status)}</small></>;
    } else if (card.kind === "imageGeneration") {
        const data = card.payload as {path: string; status: string; revisedPrompt: string}; title = "Generated image";
        body = <><div className="card-text">{data.revisedPrompt}</div><code>{data.path}</code></>;
    } else if (card.kind === "plan") {
        const data = card.payload as {legacyText: string}; title = "Plan"; body = <div className="card-text">{data.legacyText}</div>;
    } else {
        const data = card.payload as {type: string; raw: unknown}; title = humanize(data.type);
        body = <details><summary>Protocol data</summary><pre>{JSON.stringify(data.raw, null, 2)}</pre></details>;
    }
    return <article className={`conversation-card ${card.kind}`} data-card-key={stableKey(card.key)}>
        <header><span>{title}</span><small>{card.itemId}</small></header>{body}
    </article>;
}

function Conversation({session, revision}: {session: BrowserFrontendSession; revision: number}) {
    void revision;
    const snapshot = session.getSnapshot();
    const thread = session.model.thread(snapshot.selectedThreadId);
    return <main className="conversation-pane">
        <div className="conversation-heading"><div><span className="eyebrow">Conversation</span>
            <h1>{thread?.title ?? (snapshot.newThreadIntent ? "New thread" : "Select a thread")}</h1>
            <p>{thread ? `${thread.cwd} · ${classifyStatus(thread.status).text}` : snapshot.newThreadIntent ? "Send a message to create this thread." : "Choose a thread from the left."}</p></div></div>
        <div className="conversation-scroll">
            {snapshot.conversation.hasMore && <button className="load-more" onClick={() => session.loadMore()}>Load earlier activity</button>}
            {snapshot.conversation.sections.length === 0 && <div className="empty-state"><div className="brand-orb">C</div><h3>Conversation activity appears here</h3></div>}
            {snapshot.conversation.sections.map(section => <section key={section.key} className="turn-section">
                {section.cards.map(card => <Card key={stableKey(card.key)} card={card} />)}
            </section>)}
        </div>
        <Composer session={session} active={Boolean(thread || snapshot.newThreadIntent)} />
    </main>;
}

function Composer({session, active}: {session: BrowserFrontendSession; active: boolean}) {
    const [prompt, setPrompt] = useState("");
    const running = session.model.activeTurnId(session.getSnapshot().selectedThreadId) !== undefined;
    const submit = (event: FormEvent) => {
        event.preventDefault();
        if (!active || prompt.trim() === "") return;
        const value = prompt; setPrompt(""); void session.submitPrompt(value);
    };
    return <form className="composer" onSubmit={submit}>
        <textarea value={prompt} disabled={!active} onChange={event => setPrompt(event.target.value)}
            onKeyDown={event => { if (event.key === "Enter" && !event.shiftKey) { event.preventDefault(); event.currentTarget.form?.requestSubmit(); } }}
            placeholder={active ? "Message Codex…" : "Select or create a thread"} rows={3} />
        <div className="composer-actions"><span>Enter to send · Shift+Enter for a new line</span>
            {running ? <button type="button" className="stop-button" onClick={() => session.interrupt()}>■ Stop</button>
                : <button type="submit" className="send-button" disabled={!active || prompt.trim() === ""}>Send ↑</button>}</div>
    </form>;
}

function Inspector({session, revision}: {session: BrowserFrontendSession; revision: number}) {
    void revision;
    const [tab, setTab] = useState<"activity" | "requests" | "protocol">("activity");
    const selected = session.model.thread(session.getSnapshot().selectedThreadId);
    const requests = [...session.model.pendingRequestPresentations().values()];
    return <aside className="inspector-pane">
        <div className="pane-heading"><div><span className="eyebrow">Details</span><h2>Inspector</h2></div></div>
        <nav className="inspector-tabs">{(["activity", "requests", "protocol"] as const).map(value =>
            <button key={value} className={tab === value ? "active" : ""} onClick={() => setTab(value)}>{humanize(value)}{value === "requests" && requests.length > 0 ? ` ${requests.length}` : ""}</button>)}</nav>
        <div className="inspector-content">
            {tab === "activity" && (selected ? <>
                <Info label="Thread" value={selected.id} /><Info label="Status" value={humanize(selected.status)} />
                <Info label="Workspace" value={selected.cwd} /><Info label="Turns" value={String(selected.turnOrder.length)} />
                <Info label="Changed files" value={String(selected.changedPaths.length)} />
                {[...selected.turnOrder].reverse().map(id => { const turn = selected.turns.get(id)!; return <div className="turn-summary" key={id}><strong>{id}</strong><span>{humanize(turn.status)} · {turn.itemOrder.length} items</span></div>; })}
            </> : <p className="muted-copy">Select a thread to inspect its authoritative presentation state.</p>)}
            {tab === "requests" && (requests.length === 0 ? <p className="muted-copy">No pending approval or input requests.</p> : requests.map(request => <div className="request-card" key={request.id}>
                <strong>{humanize(request.kind)}</strong><pre>{JSON.stringify(request.raw, null, 2)}</pre>
                <div><button onClick={() => session.resolvePending(JSON.parse(request.id), false)}>Deny</button><button className="approve" onClick={() => session.resolvePending(JSON.parse(request.id), true)}>Approve</button></div>
            </div>))}
            {tab === "protocol" && <div className="protocol-list">{[...session.getSnapshot().protocolFrames].reverse().map((frame, index) => <details key={index}><summary>{humanize(String((frame as Record<string, unknown>).type ?? (frame as Record<string, unknown>).action ?? "Frame"))}</summary><pre>{JSON.stringify(frame, null, 2)}</pre></details>)}</div>}
        </div>
    </aside>;
}
function Info({label, value}: {label: string; value: string}) { return <div className="info-row"><span>{label}</span><strong>{value || "—"}</strong></div>; }

export function App({session}: {session: BrowserFrontendSession}) {
    const snapshot = useSyncExternalStore(session.subscribe, session.getSnapshot, session.getSnapshot);
    const connection = session.model.connection();
    const connectionTone = connection.connected ? "success" : connection.retrying ? "warning" : "muted";
    const globalStatus = connection.connected
        ? `${humanize(connection.providerState || "connected")} · ${humanize(connection.role || "observer")}`
        : connection.retrying ? "Connecting" : "Offline";
    const [url, setUrl] = useState(snapshot.bridgeUrl);
    const canControl = connection.role === "controller";
    return <div className="app-shell">
        <header className="top-bar"><div className="brand"><span className="brand-mark">C</span><span><b>CodexUI</b><small>Codex, clearly.</small></span></div>
            <div className="workspace-breadcrumb">{session.model.thread(snapshot.selectedThreadId)?.cwd || "No workspace"}</div>
            <div className="top-actions">
                {connection.connected && <button className="subtle-button" onClick={() => canControl ? session.releaseController() : session.claimController()}>{canControl ? "Release control" : "Claim control"}</button>}
                <label className="connection-control"><input value={url} onChange={event => setUrl(event.target.value)} aria-label="Bridge WebSocket URL" />
                    <button onClick={() => connection.connected || connection.retrying ? session.disconnect() : session.connect(url)}>{connection.connected ? "Disconnect" : "Connect"}</button><StatusDot tone={connectionTone} /></label>
            </div>
        </header>
        {snapshot.notice && <div className="notice-banner">{snapshot.notice}</div>}
        <div className="workspace-grid"><ThreadPane session={session} revision={snapshot.revision} /><Conversation session={session} revision={snapshot.revision} /><Inspector session={session} revision={snapshot.revision} /></div>
        <footer className="status-bar"><div><strong>© Volker Christian @ Codex</strong><span> | </span>
            <a href="https://github.com/SNodeC/CodexUI">CodexUI</a><span> • </span><a href="https://github.com/SNodeC/AISuite">AISuite</a><span> • </span>
            <small>Powered by</small> <a href="https://github.com/SNodeC/snode.c">SNode.C</a></div>
            <div className="global-status"><span>Status:</span><StatusDot tone={connectionTone} /><strong>{globalStatus}</strong></div></footer>
    </div>;
}
