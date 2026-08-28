import {useEffect, useLayoutEffect, useMemo, useRef, useState, useSyncExternalStore} from "react";
import type {FormEvent, ReactNode} from "react";
import {
    ConversationViewportState, DefaultSetting, applySettingChange, canonicalSettingValues, canonicalThreadSettings, classifyStatus,
    indexAuthoritativeItems, negativePendingResponse, permissionProfileLabel, positivePendingResponse, projectConversation, stableKey,
    threadStartOptions, turnStartOptions,
} from "../index.js";
import type {PendingRequestPresentation, SettingField, SettingValues} from "../index.js";
import type {
    AgentActivityData, CommandExecutionData, FileChangesData, LocalPromptData,
    ReasoningData, UserMessageData, AgentMessageData, VisibleCardData,
} from "../index.js";
import type {BrowserFrontendSession} from "./BrowserFrontendSession.js";
import {humanizeProtocolLabel as humanize} from "./Humanize.js";

const useBrowserLayoutEffect = typeof window === "undefined" ? useEffect : useLayoutEffect;

function StatusDot({tone}: {tone: string}) { return <span className={`status-dot ${tone}`} aria-hidden="true" />; }

function ThreadPane({session, revision}: {session: BrowserFrontendSession; revision: number}) {
    void revision;
    const selected = session.getSnapshot().selectedThreadId;
    const [expanded, setExpanded] = useState<Set<string>>(() => new Set());
    const [actionOpen, setActionOpen] = useState("");
    const toggle = (id: string) => setExpanded(current => {
        const next = new Set(current); if (next.has(id)) next.delete(id); else next.add(id); return next;
    });
    const renderThread = (id: string, depth: number): ReactNode => {
        const thread = session.model.thread(id);
        if (!thread) return null;
        const status = classifyStatus(thread.status);
        const hasChildren = thread.childThreadOrder.length > 0;
        return <div key={id}>
            <div className={`thread-row-wrap ${selected === id ? "selected" : ""}`} style={{paddingLeft: `${8 + depth * 14}px`}}>
                <button className="tree-toggle" disabled={!hasChildren} onClick={() => toggle(id)} aria-label={expanded.has(id) ? "Collapse child threads" : "Expand child threads"}>{hasChildren ? (expanded.has(id) ? "⌄" : "›") : ""}</button>
                <button className="thread-row" onClick={() => session.selectThread(id)}>
                    <StatusDot tone={status.tone || "muted"} /><span><strong>{thread.title}</strong><small>{thread.cwd || thread.preview || id}</small></span>
                </button>
                {selected === id && <div className="thread-actions">
                    <button title="Reload" onClick={() => session.reloadThread(id)}>↻</button>
                    <button title="Fork" onClick={() => session.forkThread(id)}>⑂</button>
                    <button title={thread.archived ? "Unarchive" : "Archive"} onClick={() => session.archiveThread(id, thread.archived)}>□</button>
                    <button title="More actions" onClick={() => setActionOpen(current => current === id ? "" : id)}>•••</button>
                    {actionOpen === id && <div className="action-popover">
                        <button onClick={() => { const name = window.prompt("Thread name", thread.title); if (name?.trim()) session.renameThread(id, name.trim()); }}>Rename</button>
                        <button className="danger" onClick={() => { if (window.confirm("Delete the selected thread?")) session.deleteThread(id); }}>Delete</button>
                    </div>}
                </div>}
            </div>
            {hasChildren && expanded.has(id) && thread.childThreadOrder.map(child => renderThread(child, depth + 1))}
        </div>;
    };
    return <aside className="thread-pane">
        <div className="pane-heading"><div><span className="eyebrow">Workspace</span><h2>Threads</h2></div>
            <button className="icon-button" onClick={() => session.beginNewThread()} title="New thread">＋</button></div>
        <div className="thread-list">
            {session.model.threadOrder().map(id => renderThread(id, 0))}
        </div>
        <button className="refresh-button" onClick={() => session.requestThreads()}>↻ Refresh threads</button>
    </aside>;
}

function safeHref(value: string): string | undefined {
    try { const url = new URL(value); return url.protocol === "https:" || url.protocol === "http:" ? url.href : undefined; }
    catch { return undefined; }
}
function InlineMarkdown({text}: {text: string}) {
    const expression = /(`[^`]+`|\[[^\]]+\]\([^)]+\)|\*\*[^*]+\*\*)/gu;
    return <>{text.split(expression).filter(Boolean).map((part, index) => {
        if (part.startsWith("`") && part.endsWith("`")) return <code key={index} className="inline-code">{part.slice(1, -1)}</code>;
        if (part.startsWith("**") && part.endsWith("**")) return <strong key={index}>{part.slice(2, -2)}</strong>;
        const link = /^\[([^\]]+)\]\(([^)]+)\)$/u.exec(part);
        if (link) { const href = safeHref(link[2]!); return href ? <a key={index} href={href} target="_blank" rel="noreferrer">{link[1]}</a> : <span key={index}>{link[1]} ({link[2]})</span>; }
        return part;
    })}</>;
}
function SafeMarkdown({text}: {text: string}) {
    const blocks: ReactNode[] = [];
    let code: string[] | undefined;
    let paragraph: string[] = [];
    const flush = () => { if (paragraph.length) { blocks.push(<p key={`p-${blocks.length}`}><InlineMarkdown text={paragraph.join("\n")} /></p>); paragraph = []; } };
    for (const line of text.split("\n")) {
        if (line.startsWith("```")) { if (code) { blocks.push(<pre key={`c-${blocks.length}`}>{code.join("\n")}</pre>); code = undefined; } else { flush(); code = []; } continue; }
        if (code) { code.push(line); continue; }
        if (/^#{1,4} /u.test(line)) { flush(); blocks.push(<h3 key={`h-${blocks.length}`}><InlineMarkdown text={line.replace(/^#{1,4} /u, "")} /></h3>); }
        else if (/^[-*] /u.test(line)) { flush(); blocks.push(<div className="markdown-list" key={`l-${blocks.length}`}>• <InlineMarkdown text={line.slice(2)} /></div>); }
        else if (line.trim() === "") flush(); else paragraph.push(line);
    }
    flush(); if (code) blocks.push(<pre key={`c-${blocks.length}`}>{code.join("\n")}</pre>);
    return <div className="safe-markdown">{blocks}</div>;
}

function Card({card, active, collapsed, onToggle}: {card: VisibleCardData; active: boolean; collapsed: boolean; onToggle: () => void}) {
    let title = humanize(card.kind);
    let body: ReactNode;
    let phaseClass = "";
    if (card.kind === "userMessage") {
        const data = card.payload as UserMessageData; title = "You";
        body = <><div className="card-text">{data.text}</div>{data.imagePaths.map(path => <code key={path}>{path}</code>)}</>;
    } else if (card.kind === "localPrompt") {
        const data = card.payload as LocalPromptData; title = data.state === "failed" ? "Not sent" : "You";
        body = <><div className="card-text">{data.prompt}</div>{data.error && <div className="error-text">{data.error}</div>}</>;
    } else if (card.kind === "agentMessage") {
        const data = card.payload as AgentMessageData; title = "Codex"; phaseClass = data.finalAnswer ? "final" : "update";
        body = <SafeMarkdown text={data.text} />;
    } else if (card.kind === "reasoning") {
        const data = card.payload as ReasoningData; title = "Reasoning";
        body = data.summary ? <div className="card-text">{data.summary}</div> : active ? <div className="activity-line"><i />Working…</div> : null;
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
    const foldable = ["agentMessage", "commandExecution", "agentActivity", "reasoning", "fileChanges", "genericActivity"].includes(card.kind)
        && !(card.kind === "reasoning" && !(card.payload as ReasoningData).summary);
    return <article className={`conversation-card ${card.kind} ${phaseClass} ${collapsed ? "collapsed" : ""}`} data-card-key={stableKey(card.key)}>
        <header><span>{title}</span><span className="card-meta"><small>{card.itemId}</small>{foldable && <button onClick={onToggle} aria-label={collapsed ? "Expand card" : "Collapse card"}>{collapsed ? "＋" : "−"}</button>}</span></header>{!collapsed && body}
    </article>;
}

function Conversation({session, revision}: {session: BrowserFrontendSession; revision: number}) {
    void revision;
    const snapshot = session.getSnapshot();
    const thread = session.model.thread(snapshot.selectedThreadId);
    const projectionId = snapshot.selectedThreadId || (snapshot.newThreadIntent ? "__codexui_new_thread__" : "");
    const index = indexAuthoritativeItems(projectionId, thread);
    const viewport = useRef(new ConversationViewportState()).current;
    const limit = viewport.effectiveLimit(projectionId, index.ordered.length);
    const conversation = projectConversation(index, session.prompts.submissions(projectionId), limit, Date.now(), thread);
    const scroll = useRef<HTMLDivElement>(null);
    const previousThread = useRef(projectionId);
    const collapsed = useRef(new Set<string>());
    const [, forceCardState] = useState(0);
    const drafts = useRef(new Map<string, string>());
    const settingsOptions = useRef<PromptOptions>({turn: {}, thread: {}});
    useEffect(() => {
        if (previousThread.current !== projectionId) {
            previousThread.current = projectionId;
            const saved = viewport.scroll(projectionId);
            requestAnimationFrame(() => { if (scroll.current) scroll.current.scrollTop = saved.following ? scroll.current.scrollHeight : saved.scrollTop; });
        }
    }, [projectionId, viewport]);
    useBrowserLayoutEffect(() => {
        const saved = viewport.scroll(projectionId);
        if (saved.following && scroll.current) scroll.current.scrollTop = scroll.current.scrollHeight;
    }, [revision, projectionId, viewport]);
    const toggleCard = (key: string) => { if (collapsed.current.has(key)) collapsed.current.delete(key); else collapsed.current.add(key); forceCardState(value => value + 1); };
    return <main className="conversation-pane">
        <div className="conversation-heading"><div><span className="eyebrow">Conversation</span>
            <h1>{thread?.title ?? (snapshot.newThreadIntent ? "New thread" : "Select a thread")}</h1>
            <p>{thread ? `${thread.cwd} · ${classifyStatus(thread.status).text}` : snapshot.newThreadIntent ? "Send a message to create this thread." : "Choose a thread from the left."}</p></div></div>
        <div className="conversation-scroll" ref={scroll} onScroll={event => {
            const element = event.currentTarget; const following = element.scrollHeight - element.scrollTop - element.clientHeight < 24;
            viewport.updateScroll(projectionId, element.scrollTop, following);
        }}>
            {conversation.hasMore && <button className="load-more" onClick={() => { viewport.loadMore(projectionId); forceCardState(value => value + 1); }}>Load earlier activity</button>}
            {conversation.sections.length === 0 && <div className="empty-state"><div className="brand-orb">C</div><h3>Conversation activity appears here</h3></div>}
            {conversation.sections.map(section => <section key={section.key} className="turn-section">
                {section.cards.map(card => { const key = stableKey(card.key); return <Card key={key} card={card} active={session.model.activeTurnId(projectionId) === section.turnId} collapsed={collapsed.current.has(key)} onToggle={() => toggleCard(key)} />; })}
            </section>)}
        </div>
        <div className="composer-dock">
            <SettingsPanel key={`settings:${projectionId}`} session={session} canonical={canonicalThreadSettings(thread?.raw ?? {}, thread?.domains.get("thread.settings.changed"))} settingsRevision={thread?.settingsRevision ?? 0} optionsRef={settingsOptions} />
            <Composer key={projectionId} session={session} active={Boolean(thread || snapshot.newThreadIntent)} draftKey={projectionId} drafts={drafts.current} optionsRef={settingsOptions} />
        </div>
    </main>;
}

interface PromptOptions {turn: Record<string, unknown>; thread: Record<string, unknown>}

function SettingsPanel({session, canonical, settingsRevision, optionsRef}: {session: BrowserFrontendSession; canonical: unknown; settingsRevision: number; optionsRef: {current: PromptOptions}}) {
    const [open, setOpen] = useState(false);
    const [values, setValues] = useState<SettingValues>(() => canonicalSettingValues(canonical));
    const [touched, setTouched] = useState<Set<SettingField>>(() => new Set());
    const canonicalSignature = JSON.stringify(canonical);
    const models = session.model.modelCatalog();
    const updateOptions = (nextValues: SettingValues, nextTouched: Set<SettingField>) => {
        optionsRef.current = {turn: turnStartOptions(nextValues, nextTouched, models), thread: threadStartOptions(nextValues, nextTouched)};
    };
    const change = (field: SettingField, value: string) => {
        const {values: nextValues, touched: nextTouched} = applySettingChange(values, touched, field, value);
        setValues(nextValues); setTouched(nextTouched); updateOptions(nextValues, nextTouched);
    };
    const modelDefinitions = Array.isArray(models) ? models : [];
    const profilesDomain = session.model.globalDomains().get("operation.permission-profiles.list");
    const profiles = Array.isArray(profilesDomain) ? profilesDomain : (profilesDomain && typeof profilesDomain === "object" && Array.isArray((profilesDomain as {data?: unknown}).data) ? (profilesDomain as {data: unknown[]}).data : []);
    const select = (label: string, field: SettingField, choices: readonly [string, string][]) => <label><span>{label}</span><select value={values[field]} onChange={event => change(field, event.target.value)}>{choices.map(([name, value]) => <option key={value} value={value}>{name}</option>)}</select></label>;
    const defaults: [string, string] = ["Thread default", DefaultSetting];
    useEffect(() => {
        const fresh = canonicalSettingValues(canonical);
        setValues(current => {
            const next = {...current};
            for (const field of Object.keys(fresh) as SettingField[]) if (!touched.has(field)) next[field] = fresh[field];
            return next;
        });
    }, [settingsRevision, canonicalSignature]);
    return <div className={`settings-panel ${open ? "open" : ""}`}>
        <button className="settings-toggle" onClick={() => setOpen(value => !value)} aria-expanded={open}>Turn settings <span>{touched.size > 0 ? `${touched.size} changed` : "Thread defaults"} {open ? "⌃" : "⌄"}</span></button>
        {open && <div className="settings-grid">
            {select("Model", "model", [defaults, ...modelDefinitions.filter(value => typeof value === "object" && value !== null && !((value as {hidden?: boolean}).hidden)).map(value => [String((value as {displayName?: string}).displayName ?? (value as {model?: string; id?: string}).model ?? (value as {id?: string}).id), String((value as {model?: string; id?: string}).model ?? (value as {id?: string}).id)] as [string, string])])}
            {select("Reasoning", "effort", [defaults, ...["minimal", "low", "medium", "high", "xhigh", "ultra"].map(value => [humanize(value), value] as [string, string])])}
            {select("Access", "sandbox", [defaults, ["Workspace", "workspace-write"], ["Read only", "read-only"], ["Full access", "danger-full-access"], ["External", "external"]])}
            {select("Network", "network", [defaults, ["Restricted", "restricted"], ["Enabled", "enabled"]])}
            <label><span>Workspace</span><input value={values.cwd} placeholder="Provider workspace path" onChange={event => change("cwd", event.target.value)} /></label>
            {select("Approval", "approval", [defaults, ["On request", "on-request"], ["Untrusted", "untrusted"], ["Never", "never"]])}
            {select("Style", "personality", [defaults, ["None", "none"], ["Friendly", "friendly"], ["Pragmatic", "pragmatic"]])}
            {select("Approval reviewer", "reviewer", [defaults, ["User", "user"], ["Auto review", "auto_review"], ["Guardian", "guardian_subagent"]])}
            {select("Permission profile", "permissionProfile", [defaults, ...profiles.filter(value => typeof value === "object" && value !== null && (value as {allowed?: boolean}).allowed !== false).map(value => { const id = String((value as {id?: string}).id); return [permissionProfileLabel(id), id] as [string, string]; })])}
            <label><span>Service tier</span><input value={values.serviceTier === DefaultSetting ? "" : values.serviceTier} placeholder="Thread default" onChange={event => change("serviceTier", event.target.value || DefaultSetting)} /></label>
            {select("Reasoning summary", "summary", [defaults, ["Auto", "auto"], ["Concise", "concise"], ["Detailed", "detailed"], ["None", "none"]])}
            {select("Collaboration mode", "collaboration", [["Code", "default"], ["Plan", "plan"]])}
        </div>}
    </div>;
}

function Composer({session, active, draftKey, drafts, optionsRef}: {session: BrowserFrontendSession; active: boolean; draftKey: string; drafts: Map<string, string>; optionsRef: {current: PromptOptions}}) {
    const [prompt, setPrompt] = useState(drafts.get(draftKey) ?? "");
    const running = session.model.activeTurnId(session.getSnapshot().selectedThreadId) !== undefined;
    const submit = (event: FormEvent) => {
        event.preventDefault();
        if (!active || prompt.trim() === "") return;
        const value = prompt; setPrompt(""); drafts.set(draftKey, "");
        void session.submitPrompt(value, [], optionsRef.current.turn, optionsRef.current.thread);
    };
    return <form className="composer" onSubmit={submit}>
        <textarea value={prompt} disabled={!active} onChange={event => { setPrompt(event.target.value); drafts.set(draftKey, event.target.value); }}
            onKeyDown={event => { if (event.key === "Enter" && !event.shiftKey) { event.preventDefault(); event.currentTarget.form?.requestSubmit(); } }}
            placeholder={active ? "Message Codex…" : "Select or create a thread"} rows={3} />
        <div className="composer-actions"><span>Enter to send · Shift+Enter for a new line</span>
            {running ? <button type="button" className="stop-button" onClick={() => session.interrupt()}>■ Stop</button>
                : <button type="submit" className="send-button" disabled={!active || prompt.trim() === ""}>Send ↑</button>}</div>
    </form>;
}

function Inspector({session, revision}: {session: BrowserFrontendSession; revision: number}) {
    void revision;
    const [tab, setTab] = useState<"plan" | "agents" | "requests" | "state" | "protocol">("plan");
    const selected = session.model.thread(session.getSnapshot().selectedThreadId);
    const requests = [...session.model.pendingRequestPresentations().values()];
    const latestPlan = selected && [...selected.turnOrder].reverse().map(id => selected.turns.get(id)).find(turn => turn && (Object.keys(turn.plan).length > 0 || turn.itemOrder.some(itemId => turn.items.get(itemId)?.raw.type === "plan")));
    const plainState = selected ? {
        id: selected.id, title: selected.title, cwd: selected.cwd, status: selected.status, archived: selected.archived,
        turns: selected.turnOrder.map(id => { const turn = selected.turns.get(id)!; return {id, status: turn.status, plan: turn.plan,
            items: turn.itemOrder.map(itemId => turn.items.get(itemId)?.raw)}; }),
        agents: selected.agentOrder.map(id => selected.agents.get(id)), domains: Object.fromEntries(selected.domains),
    } : null;
    return <aside className="inspector-pane">
        <div className="pane-heading"><div><span className="eyebrow">Details</span><h2>Inspector</h2></div></div>
        <nav className="inspector-tabs">{(["plan", "agents", "requests", "state", "protocol"] as const).map(value =>
            <button key={value} className={tab === value ? "active" : ""} onClick={() => setTab(value)}>{humanize(value)}{value === "requests" && requests.length > 0 ? ` ${requests.length}` : ""}</button>)}</nav>
        <div className="inspector-content">
            {tab === "plan" && (!selected ? <p className="muted-copy">Select a thread to inspect its plan.</p> : latestPlan ? <div className="plan-view">
                {typeof latestPlan.plan.explanation === "string" && <p>{latestPlan.plan.explanation}</p>}
                {Array.isArray(latestPlan.plan.steps) && latestPlan.plan.steps.map((step, index) => <div key={index}><StatusDot tone={String((step as {status?: string}).status) === "completed" ? "success" : "active"} /><span>{String((step as {step?: string}).step ?? "")}</span><small>{humanize(String((step as {status?: string}).status ?? ""))}</small></div>)}
            </div> : <p className="muted-copy">No structured plan is available for this thread.</p>)}
            {tab === "agents" && (!selected || selected.agentOrder.length === 0 ? <p className="muted-copy">No correlated agents are present.</p> : selected.agentOrder.map(id => { const agent = selected.agents.get(id)!; return <div className="agent-card" key={id}>
                <strong>{agent.raw.agentPath ? String(agent.raw.agentPath) : id}</strong><span>{humanize(agent.status)}</span>
                {agent.childThreadId && <small>Thread {agent.childThreadId}</small>}{typeof agent.raw.resultText === "string" && <p>{agent.raw.resultText}</p>}
            </div>; }))}
            {tab === "requests" && (requests.length === 0 ? <p className="muted-copy">No pending approval or input requests.</p> : requests.map(request => <RequestCard key={request.id} request={request} session={session} />))}
            {tab === "state" && <>{selected && <div className="state-summary"><Info label="Thread" value={selected.id} /><Info label="Status" value={humanize(selected.status)} /><Info label="Workspace" value={selected.cwd} /><Info label="Turns" value={String(selected.turnOrder.length)} /><Info label="Changed files" value={String(selected.changedPaths.length)} /></div>}<pre className="state-json">{JSON.stringify(plainState, null, 2)}</pre></>}
            {tab === "protocol" && <div className="protocol-list">{[...session.getSnapshot().protocolFrames].reverse().map((frame, index) => <details key={index}><summary>{humanize(String((frame as Record<string, unknown>).type ?? (frame as Record<string, unknown>).action ?? "Frame"))}</summary><pre>{JSON.stringify(frame, null, 2)}</pre></details>)}</div>}
        </div>
    </aside>;
}
function Info({label, value}: {label: string; value: string}) { return <div className="info-row"><span>{label}</span><strong>{value || "—"}</strong></div>; }

function requestSupportsDirectAccept(request: PendingRequestPresentation): boolean {
    return ["command-approval", "file-change-approval", "permissions-approval", "legacy-patch-approval", "legacy-command-approval"].includes(request.kind);
}

function requestAcceptLabel(request: PendingRequestPresentation): string {
    return request.kind === "permissions-approval" ? "Allow this turn" : "Accept";
}

function requestSummary(request: PendingRequestPresentation, raw: Record<string, unknown>, questions: readonly unknown[]): string {
    const parts: string[] = [];
    for (const [label, value] of [["Command", raw.command], ["Reason", raw.reason], ["Message", raw.message], ["Directory", raw.cwd], ["Grant root", raw.grantRoot]] as const)
        if (typeof value === "string" && value.trim() !== "") parts.push(label === "Message" ? value : `${label}: ${value}`);
    if (raw.permissions !== undefined) parts.push(`Permissions: ${JSON.stringify(raw.permissions)}`);
    if (questions.length > 0) parts.push(`${questions.length} questions`);
    return parts.length > 0 ? parts.join("  |  ") : `Request ${request.id} needs a decision.`;
}

function RequestCard({request, session}: {request: PendingRequestPresentation; session: BrowserFrontendSession}) {
    const raw = request.raw && typeof request.raw === "object" ? request.raw as Record<string, unknown> : {};
    const questions = Array.isArray(raw.questions) ? raw.questions as Record<string, unknown>[] : [];
    const [answers, setAnswers] = useState<Record<string, string>>({});
    const [structured, setStructured] = useState("{}");
    const approve = () => {
        let input: unknown = {};
        if (request.kind === "user-input") input = Object.fromEntries(questions.map(question => {
            const id = String(question.id ?? ""); return [id, {answers: [answers[id] ?? ""]}];
        }));
        else if (request.kind === "mcp-elicitation") { try { input = JSON.parse(structured); } catch { return; } }
        session.resolvePending(JSON.parse(request.id), positivePendingResponse(request, input));
    };
    const reject = () => session.resolvePending(JSON.parse(request.id), negativePendingResponse(request));
    const directAccept = requestSupportsDirectAccept(request);
    return <div className="request-card">
        <strong>{humanize(request.kind)}</strong>
        <p>{requestSummary(request, raw, questions)}</p>
        {typeof raw.command === "string" && raw.command.trim() !== "" && <code>{raw.command}</code>}
        {request.kind === "user-input" && questions.map(question => { const id = String(question.id ?? ""); const options = Array.isArray(question.options) ? question.options as Record<string, unknown>[] : []; return <label className="request-question" key={id}><span>{String(question.question ?? question.header ?? id)}</span>{options.length > 0
            ? <select value={answers[id] ?? ""} onChange={event => setAnswers(current => ({...current, [id]: event.target.value}))}><option value="">Choose…</option>{options.map(option => <option key={String(option.label)}>{String(option.label)}</option>)}</select>
            : <input type={question.isSecret ? "password" : "text"} value={answers[id] ?? ""} onChange={event => setAnswers(current => ({...current, [id]: event.target.value}))} />}</label>; })}
        {request.kind === "mcp-elicitation" && raw.requestedSchema !== undefined && <textarea value={structured} onChange={event => setStructured(event.target.value)} rows={5} aria-label="Structured MCP response" />}
        <details><summary>Request data</summary><pre>{JSON.stringify(request.raw, null, 2)}</pre></details>
        <div className="request-actions"><button className="request-button danger" onClick={reject}>Reject</button><button className="request-button approve" onClick={approve}>{directAccept ? requestAcceptLabel(request) : "Submit"}</button></div>
    </div>;
}

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
        {snapshot.notice && <div className="notice-banner" role="alert"><span>{snapshot.notice}</span><button onClick={() => session.dismissNotice()} aria-label="Dismiss notice">×</button></div>}
        <div className="workspace-grid"><ThreadPane session={session} revision={snapshot.revision} /><Conversation session={session} revision={snapshot.revision} /><Inspector session={session} revision={snapshot.revision} /></div>
        <footer className="status-bar"><div><strong>© Volker Christian @ Codex</strong><span> | </span>
            <a href="https://github.com/SNodeC/CodexUI">CodexUI</a><span> • </span><a href="https://github.com/SNodeC/AISuite">AISuite</a><span> • </span>
            <small>Powered by</small> <a href="https://github.com/SNodeC/snode.c">SNode.C</a></div>
            <div className="global-status"><span>Status:</span><StatusDot tone={connectionTone} /><strong>{globalStatus}</strong></div></footer>
    </div>;
}
