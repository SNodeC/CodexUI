import {useEffect, useLayoutEffect, useMemo, useRef, useState, useSyncExternalStore} from "react";
import type {FormEvent, ReactNode, RefObject} from "react";
import Markdown from "react-markdown";
import remarkGfm from "remark-gfm";
import {
    ConversationViewportState, DefaultSetting, anchoredScrollTop, changeSettingDraft, canonicalThreadSettings, classifyStatus,
    foldedCardScrollTop, nestedScrollConsumes,
    pendingDecisionOptions, pendingRequestDetails, pendingResponse, permissionProfileLabel, stableKey,
    settingDraftFor, settingPromptOptions, trimTrailingEmptyLines,
} from "../index.js";
import type {ConversationViewportAnchor, PendingRequestPresentation, SettingDraft, SettingField, SettingPromptOptions, ThreadPresentation} from "../index.js";
import type {
    AgentActivityData, CommandExecutionData, FileChangesData, LocalPromptData,
    ReasoningData, UserMessageData, AgentMessageData, GenericActivityData,
    ImageGenerationData, PlanData, VisibleCardData,
} from "../index.js";
import type {BrowserFrontendSession, NewThreadDraft, ThreadSortCriterion} from "./BrowserFrontendSession.js";
import {shouldSubmitPromptFromKey} from "./ComposerKeyboard.js";
import {humanizeProtocolLabel as humanize} from "./Humanize.js";
import {readBrowserStorage, writeBrowserStorage} from "./BrowserStorage.js";

const useBrowserLayoutEffect = typeof window === "undefined" ? useEffect : useLayoutEffect;

export type ResponsiveMode = "desktop" | "tablet" | "mobile";

export function responsiveModeForWidth(width: number): ResponsiveMode {
    if (width <= 760) return "mobile";
    if (width <= 1160) return "tablet";
    return "desktop";
}

function currentResponsiveMode(): ResponsiveMode {
    if (typeof window === "undefined") return "desktop";
    if (typeof window.matchMedia === "function") {
        if (window.matchMedia("(max-width: 760px)").matches) return "mobile";
        if (window.matchMedia("(max-width: 1160px)").matches) return "tablet";
        return "desktop";
    }
    return Number.isFinite(window.innerWidth) ? responsiveModeForWidth(window.innerWidth) : "desktop";
}

function useResponsiveMode(): ResponsiveMode {
    const [mode, setMode] = useState(currentResponsiveMode);
    useEffect(() => {
        const update = () => setMode(currentResponsiveMode());
        if (typeof window.matchMedia !== "function") {
            window.addEventListener("resize", update);
            return () => window.removeEventListener("resize", update);
        }
        const mobile = window.matchMedia("(max-width: 760px)");
        const tablet = window.matchMedia("(max-width: 1160px)");
        mobile.addEventListener("change", update);
        tablet.addEventListener("change", update);
        return () => {
            mobile.removeEventListener("change", update);
            tablet.removeEventListener("change", update);
        };
    }, []);
    return mode;
}

interface DrawerPaneProps {
    drawer?: boolean;
    paneRef?: RefObject<HTMLElement>;
    onClose?: () => void;
}

export function runThreadPaneNavigation(navigate: () => void, onClose?: () => void): void {
    navigate();
    onClose?.();
}

interface ConversationPresentationOptions {
    showReasoning: boolean;
    showCodexUpdates: boolean;
    commandsInitiallyExpanded: boolean;
    imagesInitiallyExpanded: boolean;
}

function PresentationIcon({kind}: {kind: "reasoning" | "updates" | "command" | "image"}) {
    if (kind === "reasoning") return <svg viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="6" r="4"/><path d="M5.5 9 6.5 11h3L10.5 9M7 13h2"/></svg>;
    if (kind === "updates") return <svg viewBox="0 0 16 16" aria-hidden="true"><path d="M4.5 11.5 4 14l4-2.5h3.5A2.5 2.5 0 0 0 14 9V5a2.5 2.5 0 0 0-2.5-2.5h-7A2.5 2.5 0 0 0 2 5v4a2.5 2.5 0 0 0 2.5 2.5Z"/><path d="M5.5 7h.01M8 7h.01M10.5 7h.01"/></svg>;
    if (kind === "command") return <svg viewBox="0 0 16 16" aria-hidden="true"><rect x="1.5" y="2.5" width="13" height="11" rx="2"/><path d="m4.5 6 2 2-2 2M8.5 10h3"/></svg>;
    return <svg viewBox="0 0 16 16" aria-hidden="true"><rect x="1.5" y="2.5" width="13" height="11" rx="2"/><circle cx="10.5" cy="5.5" r="1"/><path d="m3.5 11 3-3.5 2 2L10 8l2.5 3"/></svg>;
}

function CopyIcon() {
    return <svg viewBox="0 0 16 16" aria-hidden="true"><rect x="3" y="2.5" width="8" height="9" rx="1.5"/><rect x="6" y="5.5" width="8" height="9" rx="1.5"/></svg>;
}

function FoldIcon({collapsed}: {collapsed: boolean}) {
    return <svg viewBox="0 0 16 16" aria-hidden="true"><path d={collapsed ? "m10 3-5 5 5 5" : "m3 6 5 5 5-5"} /></svg>;
}

function ImageRibbon({paths}: {paths: string[]}) {
    return paths.length > 0 ? <div className="image-ribbon">{paths.map(path => <code key={path}>{path}</code>)}</div> : null;
}

const defaultConversationPresentation: ConversationPresentationOptions = {
    showReasoning: false,
    showCodexUpdates: true,
    commandsInitiallyExpanded: true,
    imagesInitiallyExpanded: true,
};

function storedConversationPresentation(): ConversationPresentationOptions {
    if (typeof window === "undefined") return defaultConversationPresentation;
    const boolean = (key: string, fallback: boolean) => {
        const stored = readBrowserStorage(key);
        return stored === undefined ? fallback : stored === "true";
    };
    return {
        showReasoning: boolean("codexui.conversation.showReasoning", false),
        showCodexUpdates: boolean("codexui.conversation.showCodexUpdates", true),
        commandsInitiallyExpanded: boolean("codexui.conversation.commandsInitiallyExpanded", true),
        imagesInitiallyExpanded: boolean("codexui.conversation.imagesInitiallyExpanded", true),
    };
}

function persistConversationPresentation(options: ConversationPresentationOptions): void {
    writeBrowserStorage("codexui.conversation.showReasoning", String(options.showReasoning));
    writeBrowserStorage("codexui.conversation.showCodexUpdates", String(options.showCodexUpdates));
    writeBrowserStorage("codexui.conversation.commandsInitiallyExpanded", String(options.commandsInitiallyExpanded));
    writeBrowserStorage("codexui.conversation.imagesInitiallyExpanded", String(options.imagesInitiallyExpanded));
}

function StatusDot({tone}: {tone: string}) { return <span className={`status-dot ${tone}`} aria-hidden="true" />; }

function effectivePlanStepStatus(stepStatus: string, turnStatus: string, threadStatus: string): string {
    if (classifyStatus(stepStatus).kind !== "active") return stepStatus;
    let outcome = classifyStatus(turnStatus).kind;
    if (!["completed", "failed", "interrupted"].includes(outcome)) outcome = classifyStatus(threadStatus).kind;
    return outcome === "completed" ? "completed" : outcome === "failed" ? "failed" : outcome === "interrupted" ? "interrupted" : stepStatus;
}

function displayStatus(status: string): string {
    const classified = classifyStatus(status);
    return classified.kind === "unknown" ? humanize(status) : classified.text;
}

function ThreadPane({session, revision, onRequestNewThread, drawer = false, paneRef, onClose}: {session: BrowserFrontendSession; revision: number; onRequestNewThread: () => void} & DrawerPaneProps) {
    void revision;
    const snapshot = session.getSnapshot();
    const selected = snapshot.selectedThreadId || (snapshot.newThreadIntent ? "__codexui_new_thread__" : "");
    const [expanded, setExpanded] = useState<Set<string>>(() => new Set());
    const [sortCriterion, setSortCriterion] = useState<ThreadSortCriterion>("recent");
    const [contextMenu, setContextMenu] = useState<{threadId: string; x: number; y: number; trigger: HTMLElement} | null>(null);
    const contextMenuRef = useRef<HTMLDivElement>(null);
    useEffect(() => {
        if (!contextMenu) return;
        const menu = contextMenuRef.current;
        if (!menu) return;
        const placeAndFocus = () => {
            const bounds = menu.getBoundingClientRect();
            menu.style.left = `${Math.max(8, Math.min(contextMenu.x, window.innerWidth - bounds.width - 8))}px`;
            menu.style.top = `${Math.max(8, Math.min(contextMenu.y, window.innerHeight - bounds.height - 8))}px`;
            menu.querySelector<HTMLElement>("button:not(:disabled)")?.focus();
        };
        const frame = requestAnimationFrame(placeAndFocus);
        const dismiss = (event: PointerEvent) => {
            if (!menu.contains(event.target as Node)) setContextMenu(null);
        };
        const keyDown = (event: KeyboardEvent) => {
            if (event.key !== "Escape") return;
            event.preventDefault(); setContextMenu(null); contextMenu.trigger.focus();
        };
        document.addEventListener("pointerdown", dismiss);
        document.addEventListener("keydown", keyDown);
        return () => { cancelAnimationFrame(frame); document.removeEventListener("pointerdown", dismiss); document.removeEventListener("keydown", keyDown); };
    }, [contextMenu]);
    const openContextMenu = (threadId: string, x: number, y: number, trigger: HTMLElement) =>
        setContextMenu({threadId, x, y, trigger});
    const contextThread = contextMenu ? session.model.thread(contextMenu.threadId) : undefined;
    const connection = session.model.connection();
    const providerReady = connection.connected && connection.providerState === "ready";
    const invokeContextAction = (action: () => void) => { setContextMenu(null); action(); };
    const navigateContextMenu = (event: React.KeyboardEvent<HTMLDivElement>) => {
        if (!["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) return;
        const buttons = [...event.currentTarget.querySelectorAll<HTMLButtonElement>("button:not(:disabled)")];
        if (buttons.length === 0) return;
        event.preventDefault();
        const current = buttons.indexOf(document.activeElement as HTMLButtonElement);
        const index = event.key === "Home" ? 0 : event.key === "End" ? buttons.length - 1
            : event.key === "ArrowDown" ? (current + 1) % buttons.length
                : (current <= 0 ? buttons.length : current) - 1;
        buttons[index]?.focus();
    };
    const toggle = (id: string) => setExpanded(current => {
        const next = new Set(current); if (next.has(id)) next.delete(id); else next.add(id); return next;
    });
    const renderThread = (id: string, depth: number): ReactNode => {
        const thread = session.model.thread(id);
        const optimistic = snapshot.optimisticThreads.find(candidate => candidate.id === id);
        if (!thread && !optimistic) return null;
        const status = classifyStatus(thread?.status ?? "");
        const hasChildren = (thread?.childThreadOrder.length ?? 0) > 0;
        const optimisticClass = optimistic ? ` optimistic-${optimistic.state}` : "";
        return <div key={session.threadVisualKey(id)}>
            <div className={`thread-row-wrap ${selected === id ? "selected" : ""}${contextMenu?.threadId === id ? " context-open" : ""}${optimisticClass}`} style={{paddingLeft: `${8 + depth * 14}px`}}
                onContextMenu={event => { if (!thread) return; event.preventDefault(); event.stopPropagation(); openContextMenu(id, event.clientX, event.clientY, event.currentTarget); }}>
                <button className="tree-toggle" disabled={!hasChildren} onClick={() => toggle(id)} aria-label={expanded.has(id) ? "Collapse child threads" : "Expand child threads"}>{hasChildren ? (expanded.has(id) ? "⌄" : "›") : ""}</button>
                <button className="thread-row" onClick={() => runThreadPaneNavigation(() => session.selectThread(id), onClose)}>
                    <StatusDot tone={optimistic?.state === "failed" ? "danger" : optimistic ? "warning" : status.tone || "muted"} /><span><strong>{thread?.title || optimistic?.title || id}</strong><small>{optimistic ? optimistic.state === "failed" ? "Not created" : optimistic.state === "confirmed" ? "Created" : "Creating" : thread?.cwd || thread?.preview || id}</small></span>
                </button>
                {thread && <button className="thread-menu-trigger" title="Thread actions" aria-label={`Actions for ${thread.title || id}`}
                    aria-haspopup="menu" aria-expanded={contextMenu?.threadId === id}
                    onClick={event => { event.stopPropagation(); const bounds = event.currentTarget.getBoundingClientRect(); openContextMenu(id, bounds.right, bounds.bottom, event.currentTarget); }}>•••</button>}
            </div>
            {thread && hasChildren && expanded.has(id) && thread.childThreadOrder.map(child => renderThread(child, depth + 1))}
        </div>;
    };
    return <aside ref={paneRef} className={`thread-pane${drawer ? " responsive-drawer drawer-left" : ""}`} id={drawer ? "thread-pane" : undefined}
        role={drawer ? "dialog" : undefined} aria-modal={drawer || undefined} aria-labelledby={drawer ? "thread-pane-title" : undefined}>
        <div className="pane-heading"><div><span className="eyebrow">Workspace</span><h2 id={drawer ? "thread-pane-title" : undefined}>Threads</h2></div>
            <div className="pane-heading-actions"><button className="icon-button" onClick={onRequestNewThread} title="New thread" aria-label="New thread">＋</button>
                {drawer && <button type="button" className="drawer-close" data-drawer-close onClick={onClose} aria-label="Close Threads drawer">×</button>}</div></div>
        <label className="thread-sort"><span>Sort</span><select aria-label="Thread sort order" value={sortCriterion}
            onChange={event => setSortCriterion(event.target.value as ThreadSortCriterion)}>
            <option value="recent">Recent</option><option value="created">Created</option>
            <option value="updated">Last changed</option><option value="alphanumeric">Alphanumeric</option>
        </select></label>
        <div className="thread-list">
            {snapshot.optimisticThreads.map(thread => renderThread(thread.id, 0))}
            {session.threadOrder(sortCriterion).filter(id => !snapshot.optimisticThreads.some(thread => thread.id === id)).map(id => renderThread(id, 0))}
        </div>
        <button className="refresh-button" disabled={session.operationPending("threads.refresh")} onClick={() => session.requestThreads()}>↻ Refresh threads</button>
        {contextThread && contextMenu && <div ref={contextMenuRef} className="thread-context-menu" role="menu" aria-label={`Actions for ${contextThread.title || contextThread.id}`} onKeyDown={navigateContextMenu}>
            <button role="menuitem" disabled={!providerReady} onClick={() => invokeContextAction(() => session.reloadThread(contextThread.id))}>Reload</button>
            <button role="menuitem" disabled={!session.canSubmit() || session.operationPending("thread.rename", contextThread.id)} onClick={() => invokeContextAction(() => { const name = window.prompt("Thread name", contextThread.title); if (name?.trim()) session.renameThread(contextThread.id, name.trim()); })}>Rename</button>
            <button role="menuitem" disabled={!session.canSubmit() || session.operationPending("thread.fork", contextThread.id)} onClick={() => invokeContextAction(() => session.forkThread(contextThread.id))}>Fork</button>
            <button role="menuitem" disabled={!session.canSubmit() || session.operationPending("thread.archive", contextThread.id)} onClick={() => invokeContextAction(() => session.archiveThread(contextThread.id, contextThread.archived))}>{contextThread.archived ? "Unarchive" : "Archive"}</button>
            <button role="menuitem" className="danger" disabled={!session.canSubmit() || session.operationPending("thread.delete", contextThread.id)} onClick={() => invokeContextAction(() => { if (window.confirm(`Delete “${contextThread.title || contextThread.id}”?`)) session.deleteThread(contextThread.id); })}>Delete</button>
        </div>}
    </aside>;
}

export function NewThreadDialog({initialWorkspace, onCancel, onContinue}: {initialWorkspace: string; onCancel: () => void; onContinue: (draft: NewThreadDraft) => void}) {
    const dialog = useRef<HTMLElement>(null);
    const workspaceInput = useRef<HTMLInputElement>(null);
    const [workspace, setWorkspace] = useState(initialWorkspace);
    const [name, setName] = useState("");
    const [baseInstructions, setBaseInstructions] = useState("");
    const [developerInstructions, setDeveloperInstructions] = useState("");
    const [ephemeral, setEphemeral] = useState(false);
    const [error, setError] = useState("");
    useEffect(() => {
        const previous = typeof document === "undefined" ? null : document.activeElement as HTMLElement | null;
        workspaceInput.current?.focus();
        return () => { if (previous?.isConnected) previous.focus(); };
    }, []);
    const submit = (event: FormEvent) => {
        event.preventDefault();
        if (workspace.trim() === "") { setError("Enter the app-server workspace path."); workspaceInput.current?.focus(); return; }
        onContinue({workspace: workspace.trim(), name: name.trim(), baseInstructions: baseInstructions.trim(),
            developerInstructions: developerInstructions.trim(), ephemeral});
    };
    const keyDown = (event: React.KeyboardEvent<HTMLElement>) => {
        if (event.key === "Escape") { event.preventDefault(); onCancel(); return; }
        if (event.key !== "Tab" || !dialog.current) return;
        const focusable = [...dialog.current.querySelectorAll<HTMLElement>("button, input, textarea")]
            .filter(element => !element.hasAttribute("disabled"));
        const first = focusable[0]; const last = focusable.at(-1);
        if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last?.focus(); }
        else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first?.focus(); }
    };
    return <div className="modal-backdrop"><section ref={dialog} className="new-thread-dialog" role="dialog" aria-modal="true" aria-labelledby="new-thread-title" onKeyDown={keyDown}>
        <header><h2 id="new-thread-title">New thread</h2><p>Set thread context. Upcoming-turn controls retain model, reasoning, access, and style.</p></header>
        <form onSubmit={submit}>
            <label><span>Workspace</span><input ref={workspaceInput} value={workspace} onChange={event => { setWorkspace(event.target.value); setError(""); }} placeholder="Absolute app-server workspace path" /></label>
            <label><span>Name</span><input value={name} onChange={event => setName(event.target.value)} placeholder="Optional thread name" /></label>
            <label><span>Base instructions</span><textarea value={baseInstructions} onChange={event => setBaseInstructions(event.target.value)} placeholder="Optional base instructions" /></label>
            <label><span>Developer instructions</span><textarea value={developerInstructions} onChange={event => setDeveloperInstructions(event.target.value)} placeholder="Optional developer instructions" /></label>
            <label className="ephemeral-choice"><input type="checkbox" checked={ephemeral} onChange={event => setEphemeral(event.target.checked)} /><span><strong>Temporary thread</strong><small>Temporary threads are not retained in normal Codex history.</small></span></label>
            {error && <p className="dialog-error" role="alert">{error}</p>}
            <footer><button type="button" onClick={onCancel}>Cancel</button><button type="submit" className="primary">Continue</button></footer>
        </form>
    </section></div>;
}

function safeHref(value: string): string | undefined {
    try { const url = new URL(value); return url.protocol === "https:" || url.protocol === "http:" ? url.href : undefined; }
    catch { return undefined; }
}
function SafeMarkdown({text}: {text: string}) {
    return <div className="safe-markdown"><Markdown remarkPlugins={[remarkGfm]} skipHtml components={{
        a({href, children}) {
            const safe = safeHref(href ?? "");
            return safe ? <a href={safe} target="_blank" rel="noreferrer">{children}</a>
                : <span>{children}{href ? ` (${href})` : ""}</span>;
        },
        img({src, alt}) { return <span className="markdown-image-reference">{alt || "Image"}{src ? ` (${src})` : ""}</span>; },
    }}>{text}</Markdown></div>;
}

export interface CardCopyContent {text: string; markdown: boolean}

function joinCopyText(parts: string[]): string {
    return parts.filter(Boolean).join("\n\n");
}

function planMarkdown(plan: PlanData): string {
    if (plan.legacyText) return plan.legacyText;
    const rows = plan.explanation ? [plan.explanation] : [];
    if (plan.steps.length > 0 && rows.length > 0) rows.push("");
    for (const step of plan.steps) {
        const marker = step.status === "completed" ? "✓" : step.status === "inProgress" ? "◉" : "○";
        rows.push(`${marker} ${step.text}  `);
    }
    return rows.join("\n");
}

const MaximumGenericActivityCharacters = 4096;

function boundedGenericActivity(raw: unknown): string {
    const rendered = JSON.stringify(raw, null, 2) ?? "";
    return rendered.length <= MaximumGenericActivityCharacters ? rendered
        : `${rendered.slice(0, MaximumGenericActivityCharacters)}\n\n[Activity details truncated]`;
}

function commandMetadata(command: CommandExecutionData): string {
    const values = [displayStatus(command.status)];
    if (command.exitCode !== undefined) values.push(`exit ${command.exitCode}`);
    if (command.cwd) values.push(command.cwd);
    if (command.durationMilliseconds !== undefined) {
        const seconds = command.durationMilliseconds / 1000;
        values.push(`${seconds.toFixed(seconds < 10 ? 1 : 0)} s`);
    }
    return values.filter(Boolean).join("  |  ");
}

function agentMetadata(activity: AgentActivityData): string {
    const status = activity.status || activity.kind;
    return [activity.tool, displayStatus(status), activity.receivers.join(", "), activity.model,
        activity.reasoningEffort, activity.childThreadId ? `thread ${activity.childThreadId}` : "",
        activity.agentPath, activity.senderThreadId ? `sender ${activity.senderThreadId}` : ""]
        .filter(Boolean).join("  |  ");
}

function fileChangeMetadata(changes: FileChangesData): string {
    let additions = 0; let deletions = 0; let countsAvailable = false;
    for (const change of changes.changes) if (change.additions !== undefined && change.deletions !== undefined) {
        additions += change.additions; deletions += change.deletions; countsAvailable = true;
    }
    return [displayStatus(changes.status), `${changes.changes.length} paths`, countsAvailable ? `+${additions} −${deletions}` : ""]
        .filter(Boolean).join("  |  ");
}

export function cardCopyContent(card: VisibleCardData): CardCopyContent {
    if (card.kind === "userMessage") {
        const data = card.payload as UserMessageData;
        return data.text ? {text: data.text, markdown: true} : {text: data.imagePaths.join("\n"), markdown: false};
    }
    if (card.kind === "localPrompt") {
        const data = card.payload as LocalPromptData;
        return data.prompt ? {text: data.prompt, markdown: true} : {text: data.imagePaths.join("\n"), markdown: false};
    }
    if (card.kind === "agentMessage") return {text: (card.payload as AgentMessageData).text, markdown: true};
    if (card.kind === "reasoning") return {text: (card.payload as ReasoningData).summary, markdown: true};
    if (card.kind === "commandExecution") {
        const data = card.payload as CommandExecutionData;
        return {text: joinCopyText([data.command.trimEnd(), data.output.trimEnd()]), markdown: false};
    }
    if (card.kind === "agentActivity") {
        const data = card.payload as AgentActivityData;
        return {text: joinCopyText([data.prompt, data.resultText]), markdown: true};
    }
    if (card.kind === "fileChanges") {
        const data = card.payload as FileChangesData;
        return {text: data.changes.filter(change => change.path).map(change => {
            const counts = change.additions !== undefined && change.deletions !== undefined
                ? `  +${change.additions} −${change.deletions}` : "";
            return `${change.path}  ·  ${humanize(change.kind || "changed")}${counts}`;
        }).join("\n"), markdown: false};
    }
    if (card.kind === "plan") return {text: planMarkdown(card.payload as PlanData), markdown: true};
    if (card.kind === "imageGeneration") {
        const data = card.payload as ImageGenerationData;
        return {text: joinCopyText([data.revisedPrompt, data.path]), markdown: false};
    }
    const data = card.payload as GenericActivityData;
    return {text: boundedGenericActivity(data.raw), markdown: false};
}

export async function writeCardClipboard(content: CardCopyContent): Promise<"copied" | "unsupported" | "failed"> {
    if (!content.text || typeof navigator === "undefined" || !navigator.clipboard) return "unsupported";
    if (content.markdown && typeof ClipboardItem !== "undefined" && navigator.clipboard.write) {
        try {
            await navigator.clipboard.write([new ClipboardItem({
                "text/plain": new Blob([content.text], {type: "text/plain"}),
                "text/markdown": new Blob([content.text], {type: "text/markdown"}),
            })]);
            return "copied";
        } catch { /* Fall back to portable plain-text clipboard transport. */ }
    }
    if (typeof navigator.clipboard.writeText !== "function") return "unsupported";
    try { await navigator.clipboard.writeText(content.text); return "copied"; }
    catch { return "failed"; }
}

function ScrollableCode({text, className, label}: {text: string; className: string; label: string}) {
    const surface = useRef<HTMLPreElement>(null);
    const following = useRef(true);
    useBrowserLayoutEffect(() => {
        if (following.current && surface.current) surface.current.scrollTop = surface.current.scrollHeight;
    }, [text]);
    return <pre ref={surface} className={className} tabIndex={0} aria-label={label}
        onScroll={event => { const element = event.currentTarget; following.current = element.scrollHeight - element.scrollTop - element.clientHeight <= 2; }}
        onWheel={event => {
            const element = event.currentTarget;
            if (nestedScrollConsumes(event.deltaY, element.scrollTop, element.clientHeight, element.scrollHeight)) event.stopPropagation();
        }}><code>{text}</code></pre>;
}

export function Card({card, active, collapsed, onToggle, onCopy, nested, turnContainer = false}: {card: VisibleCardData; active: boolean; collapsed: boolean; onToggle: () => void; onCopy: (content: CardCopyContent) => void; nested?: ReactNode; turnContainer?: boolean}) {
    let title = humanize(card.kind);
    let body: ReactNode;
    let phaseClass = "";
    if (card.kind === "userMessage") {
        const data = card.payload as UserMessageData; title = "You";
        body = <><SafeMarkdown text={data.text} /><ImageRibbon paths={data.imagePaths} /></>;
    } else if (card.kind === "localPrompt") {
        const data = card.payload as LocalPromptData; title = data.state === "failed" ? "Not sent" : "You";
        body = <><div className="card-text">{data.prompt}</div><ImageRibbon paths={data.imagePaths} />{data.error && <div className="error-text">{data.error}</div>}</>;
    } else if (card.kind === "agentMessage") {
        const data = card.payload as AgentMessageData; title = "Codex"; phaseClass = data.finalAnswer ? "final" : "update";
        body = <SafeMarkdown text={data.text} />;
    } else if (card.kind === "reasoning") {
        const data = card.payload as ReasoningData; title = "Reasoning";
        body = data.summary ? <SafeMarkdown text={data.summary} /> : active ? <div className="activity-line"><i />Working…</div> : null;
    } else if (card.kind === "commandExecution") {
        const data = card.payload as CommandExecutionData; title = "Command execution";
        body = <><ScrollableCode className="command-line" label="Command" text={trimTrailingEmptyLines(data.command)} />
            {data.output && <ScrollableCode className="command-output" label="Command output" text={trimTrailingEmptyLines(data.output)} />}
            <small className={`card-status ${classifyStatus(data.status).tone}`}>{commandMetadata(data)}</small></>;
    } else if (card.kind === "fileChanges") {
        const data = card.payload as FileChangesData; title = "File changes";
        body = <><div className="file-list">{data.changes.map(change => <div key={`${change.path}:${change.kind}`}>
            <span>{change.path}</span><small>{humanize(change.kind)} {change.additions !== undefined && <b className="plus">+{change.additions}</b>} {change.deletions !== undefined && <b className="minus">−{change.deletions}</b>}</small>
        </div>)}</div><small className={`card-status ${classifyStatus(data.status).tone}`}>{fileChangeMetadata(data)}</small></>;
    } else if (card.kind === "agentActivity") {
        const data = card.payload as AgentActivityData; title = "Agent activity";
        body = <><small className={`card-status ${classifyStatus(data.status || data.kind).tone}`}>{agentMetadata(data)}</small>
            {data.prompt && <div className="card-text">{data.prompt}</div>}{data.resultText && <SafeMarkdown text={data.resultText} />}</>;
    } else if (card.kind === "imageGeneration") {
        const data = card.payload as ImageGenerationData; title = data.status || data.revisedPrompt ? "Generated image" : "Image";
        body = <>{data.status && <small className={`card-status ${classifyStatus(data.status).tone}`}>{displayStatus(data.status)}</small>}
            {data.revisedPrompt && <div className="card-text">{data.revisedPrompt}</div>}<ImageRibbon paths={data.path ? [data.path] : []} /></>;
    } else if (card.kind === "plan") {
        const data = card.payload as PlanData; title = "Plan"; body = <SafeMarkdown text={planMarkdown(data)} />;
    } else {
        const data = card.payload as GenericActivityData; title = data.type ? humanize(data.type) : "Activity";
        body = <pre className="generic-activity-data">{boundedGenericActivity(data.raw)}</pre>;
    }
    const copyContent = cardCopyContent(card);
    const foldable = ["userMessage", "localPrompt", "agentMessage", "commandExecution", "agentActivity", "reasoning", "fileChanges", "imageGeneration", "plan", "genericActivity"].includes(card.kind)
        && !(card.kind === "reasoning" && !(card.payload as ReasoningData).summary);
    const activeTurn = active && turnContainer && card.kind === "userMessage";
    const activeWork = (card.kind === "commandExecution" || card.kind === "imageGeneration")
        && ["active", "inProgress", "running", "started"].includes((card.payload as CommandExecutionData | ImageGenerationData).status);
    return <article className={`conversation-card ${card.kind} ${phaseClass} ${collapsed ? "collapsed" : ""} ${turnContainer ? "turn-container" : ""} ${activeTurn ? "active-turn" : ""} ${activeWork ? "active-work" : ""}`} data-card-key={stableKey(card.key)}>
        <header><span>{title}</span><span className="card-meta"><small>{card.itemId}</small>{copyContent.text && <button className="card-copy-button" onClick={() => onCopy(copyContent)} aria-label="Copy card content"><CopyIcon /></button>}{foldable && <button className="card-fold-button" onClick={onToggle} aria-label={collapsed ? "Expand card" : "Collapse card"}><FoldIcon collapsed={collapsed} /></button>}</span></header>{!collapsed && <>{body}{nested && <div className="turn-nested">{nested}</div>}</>}
    </article>;
}

interface PendingConversationGeometry {
    threadId: string;
    following: boolean;
    anchor: ConversationViewportAnchor | undefined;
    fold?: {cardKey: string; collapsed: boolean; previousTitleTop: number};
}

function conversationAnchor(container: HTMLElement): ConversationViewportAnchor | undefined {
    const viewportTop = container.getBoundingClientRect().top;
    for (const card of container.querySelectorAll<HTMLElement>(".conversation-card[data-card-key]")) {
        const header = card.querySelector<HTMLElement>(":scope > header");
        if (!header || header.getBoundingClientRect().bottom < viewportTop) continue;
        return {cardKey: card.dataset.cardKey ?? "", pixelOffset: header.getBoundingClientRect().top - viewportTop};
    }
    return undefined;
}

function cardForKey(container: HTMLElement, key: string): HTMLElement | undefined {
    return [...container.querySelectorAll<HTMLElement>(".conversation-card[data-card-key]")]
        .find(card => card.dataset.cardKey === key);
}

function restoreConversationAnchor(container: HTMLElement, anchor: ConversationViewportAnchor | undefined,
    fallbackScrollTop: number): void {
    if (!anchor) { container.scrollTop = fallbackScrollTop; return; }
    const card = cardForKey(container, anchor.cardKey);
    const header = card?.querySelector<HTMLElement>(":scope > header");
    if (!header) { container.scrollTop = fallbackScrollTop; return; }
    const cardContentTop = header.getBoundingClientRect().top - container.getBoundingClientRect().top + container.scrollTop;
    container.scrollTop = anchoredScrollTop(cardContentTop, anchor.pixelOffset, container.scrollHeight - container.clientHeight);
}

function Conversation({session, revision, paneControls}: {session: BrowserFrontendSession; revision: number; paneControls?: ReactNode}) {
    const snapshot = session.getSnapshot();
    const thread = session.model.thread(snapshot.selectedThreadId);
    const projectionId = snapshot.selectedThreadId || (snapshot.newThreadIntent ? "__codexui_new_thread__" : "");
    const viewport = useRef(new ConversationViewportState()).current;
    const authoritativeCount = thread?.turnOrder.reduce((count, id) =>
        count + (thread.turns.get(id)?.itemOrder.length ?? 0), 0) ?? 0;
    const limit = viewport.effectiveLimit(projectionId, authoritativeCount);
    const conversation = session.conversation(limit);
    const pane = useRef<HTMLElement>(null);
    const scroll = useRef<HTMLDivElement>(null);
    const composerDock = useRef<HTMLDivElement>(null);
    const previousThread = useRef(projectionId);
    const renderedRevision = useRef(revision);
    const pendingGeometry = useRef<PendingConversationGeometry>();
    const folding = useRef(new Map<string, boolean>());
    const [cardStateRevision, forceCardState] = useState(0);
    const [presentation, setPresentation] = useState(storedConversationPresentation);
    const drafts = useRef(new Map<string, string>());
    const draftRevision = useRef(snapshot.newThreadDraftRevision);
    if (draftRevision.current !== snapshot.newThreadDraftRevision) {
        drafts.current.clear();
        draftRevision.current = snapshot.newThreadDraftRevision;
    }
    const settingsDrafts = useRef(new Map<string, SettingDraft>()).current;
    const [, forceSettingsState] = useState(0);
    const canonicalSettings = canonicalThreadSettings(thread?.raw ?? (snapshot.newThreadDraft?.workspace
        ? {cwd: snapshot.newThreadDraft.workspace} : {}), thread?.domains.get("thread.settings.changed"));
    const settingsRevision = thread?.settingsRevision ?? 0;
    const settingsDraft = settingDraftFor(settingsDrafts, projectionId, canonicalSettings, settingsRevision);
    const settingsOptions = settingPromptOptions(settingsDraft, session.model.modelCatalog());
    useBrowserLayoutEffect(() => {
        const element = scroll.current;
        if (!element) return;
        const saved = viewport.scroll(projectionId);
        const revisionChanged = renderedRevision.current !== revision;
        renderedRevision.current = revision;
        const switchedThread = previousThread.current !== projectionId;
        const transaction = pendingGeometry.current?.threadId === projectionId ? pendingGeometry.current : undefined;
        if (switchedThread) {
            previousThread.current = projectionId;
            pendingGeometry.current = undefined;
            if (saved.following) element.scrollTop = element.scrollHeight;
            else restoreConversationAnchor(element, saved.anchor, saved.scrollTop);
        } else if (transaction?.fold) {
            const card = cardForKey(element, transaction.fold.cardKey);
            const header = card?.querySelector<HTMLElement>(":scope > header");
            if (card && header) {
                const viewportTop = element.getBoundingClientRect().top;
                const visibleBottom = Math.min(element.getBoundingClientRect().bottom,
                    (composerDock.current?.getBoundingClientRect().top ?? element.getBoundingClientRect().bottom) - 8);
                const cardContentTop = header.getBoundingClientRect().top - viewportTop + element.scrollTop;
                element.scrollTop = foldedCardScrollTop(cardContentTop, transaction.fold.previousTitleTop,
                    card.getBoundingClientRect().height, Math.max(0, visibleBottom - viewportTop),
                    transaction.fold.collapsed, element.scrollHeight - element.clientHeight);
            }
        } else if (transaction?.following || (!transaction && saved.following)) {
            element.scrollTop = element.scrollHeight;
        } else if (transaction || revisionChanged) {
            restoreConversationAnchor(element, transaction?.anchor ?? saved.anchor, saved.scrollTop);
        }
        const following = transaction?.fold ? false : transaction?.following ?? saved.following;
        viewport.updateScroll(projectionId, element.scrollTop, following, conversationAnchor(element));
        if (transaction) pendingGeometry.current = undefined;
    }, [revision, projectionId, cardStateRevision, presentation, viewport]);
    useBrowserLayoutEffect(() => {
        const dock = composerDock.current; const owner = pane.current;
        if (!dock || !owner || typeof ResizeObserver === "undefined") return;
        let previousHeight = 0;
        const observer = new ResizeObserver(() => {
            const element = scroll.current; const height = Math.ceil(dock.getBoundingClientRect().height);
            if (height === previousHeight) return;
            const saved = viewport.scroll(projectionId);
            const anchor = element ? conversationAnchor(element) ?? saved.anchor : saved.anchor;
            owner.style.setProperty("--composer-overlay-height", `${height}px`);
            if (element && previousHeight !== 0) {
                if (height > previousHeight && saved.following) element.scrollTop = element.scrollHeight;
                else restoreConversationAnchor(element, anchor, saved.scrollTop);
                viewport.updateScroll(projectionId, element.scrollTop, saved.following, conversationAnchor(element));
            }
            previousHeight = height;
        });
        observer.observe(dock);
        return () => observer.disconnect();
    }, [projectionId, viewport]);
    const pauseForRelayout = () => {
        if (!scroll.current) return;
        const anchor = conversationAnchor(scroll.current);
        pendingGeometry.current = {threadId: projectionId, following: false, anchor};
        viewport.updateScroll(projectionId, scroll.current.scrollTop, false, anchor);
    };
    const updatePresentation = (change: Partial<ConversationPresentationOptions>) => setPresentation(current => {
        if (change.showReasoning !== undefined || change.showCodexUpdates !== undefined) pauseForRelayout();
        const next = {...current, ...change}; persistConversationPresentation(next); return next;
    });
    const cardVisible = (card: VisibleCardData) => {
        if (card.kind === "reasoning") return presentation.showReasoning;
        if (card.kind !== "agentMessage") return true;
        return (card.payload as AgentMessageData).finalAnswer || presentation.showCodexUpdates;
    };
    const cardCollapsed = (card: VisibleCardData, key: string) => {
        if (!folding.current.has(key))
            folding.current.set(key,
                (card.kind === "commandExecution" && !presentation.commandsInitiallyExpanded)
                || (card.kind === "imageGeneration" && !presentation.imagesInitiallyExpanded)
                || (card.kind === "reasoning" && Boolean((card.payload as ReasoningData).summary))
                || ["fileChanges", "agentActivity", "plan", "genericActivity"].includes(card.kind));
        return folding.current.get(key) ?? false;
    };
    const toggleCard = (key: string, collapsed: boolean) => {
        const element = scroll.current; const card = element ? cardForKey(element, key) : undefined;
        const header = card?.querySelector<HTMLElement>(":scope > header");
        if (element && header) {
            const previousTitleTop = header.getBoundingClientRect().top - element.getBoundingClientRect().top;
            const anchor = conversationAnchor(element);
            pendingGeometry.current = {threadId: projectionId, following: false, anchor,
                fold: {cardKey: key, collapsed: !collapsed, previousTitleTop}};
            viewport.updateScroll(projectionId, element.scrollTop, false, anchor);
        }
        folding.current.set(key, !collapsed); forceCardState(value => value + 1);
    };
    const copyCard = (content: CardCopyContent) => void writeCardClipboard(content).then(outcome => {
        if (outcome === "copied") session.notify("Card content copied.");
        else if (outcome === "unsupported") session.notify("Clipboard access is not available in this browser.", true);
        else session.notify("Card content could not be copied.", true);
    });
    const visibleSections = conversation.sections
        .map(section => ({...section, cards: section.cards.filter(cardVisible)}))
        .filter(section => section.cards.length > 0);
    const renderCard = (card: VisibleCardData, nested?: ReactNode, turnContainer = false) => {
        const key = stableKey(card.key); const collapsed = cardCollapsed(card, key);
        return <Card key={key} card={card} active={session.model.activeTurnId(projectionId) === card.turnId} collapsed={collapsed} onToggle={() => toggleCard(key, collapsed)} onCopy={copyCard} nested={nested} turnContainer={turnContainer} />;
    };
    return <main ref={pane} className="conversation-pane">
        <div className="conversation-heading"><div className="conversation-title"><span className="eyebrow">Conversation</span>
            <h1>{thread?.title ?? (snapshot.newThreadIntent ? snapshot.newThreadDraft?.name || "New thread" : "Select a thread")}</h1>
            <p>{thread ? `${thread.cwd} · ${classifyStatus(thread.status).text}` : snapshot.newThreadIntent ? `${snapshot.newThreadDraft?.workspace ?? ""} · Send a message to create this thread.` : "Choose a thread from the left."}</p></div>
            <div className={`conversation-heading-actions${paneControls ? " responsive" : ""}`}>
                {paneControls && <div className="responsive-pane-controls">{paneControls}</div>}
                <div className="conversation-view-controls" aria-label="Conversation presentation">
                    <button className={presentation.showReasoning ? "active" : ""} aria-label={presentation.showReasoning ? "Hide reasoning cards" : "Show reasoning cards"} data-tooltip={presentation.showReasoning ? "Hide reasoning cards" : "Show reasoning cards"} aria-pressed={presentation.showReasoning} onClick={() => updatePresentation({showReasoning: !presentation.showReasoning})}><PresentationIcon kind="reasoning" /></button>
                    <button className={presentation.showCodexUpdates ? "active" : ""} aria-label={presentation.showCodexUpdates ? "Hide Codex update cards" : "Show Codex update cards"} data-tooltip={presentation.showCodexUpdates ? "Hide Codex update cards" : "Show Codex update cards"} aria-pressed={presentation.showCodexUpdates} onClick={() => updatePresentation({showCodexUpdates: !presentation.showCodexUpdates})}><PresentationIcon kind="updates" /></button>
                    <button className={presentation.commandsInitiallyExpanded ? "active" : ""} aria-label={presentation.commandsInitiallyExpanded ? "New command cards start expanded" : "New command cards start collapsed"} data-tooltip={presentation.commandsInitiallyExpanded ? "New command cards start expanded" : "New command cards start collapsed"} aria-pressed={presentation.commandsInitiallyExpanded} onClick={() => updatePresentation({commandsInitiallyExpanded: !presentation.commandsInitiallyExpanded})}><PresentationIcon kind="command" /></button>
                    <button className={presentation.imagesInitiallyExpanded ? "active" : ""} aria-label={presentation.imagesInitiallyExpanded ? "New image cards start expanded" : "New image cards start collapsed"} data-tooltip={presentation.imagesInitiallyExpanded ? "New image cards start expanded" : "New image cards start collapsed"} aria-pressed={presentation.imagesInitiallyExpanded} onClick={() => updatePresentation({imagesInitiallyExpanded: !presentation.imagesInitiallyExpanded})}><PresentationIcon kind="image" /></button>
                </div>
            </div>
        </div>
        <div className="conversation-scroll" ref={scroll} onScroll={event => {
            const element = event.currentTarget; const following = element.scrollHeight - element.scrollTop - element.clientHeight < 24;
            viewport.updateScroll(projectionId, element.scrollTop, following, conversationAnchor(element));
        }}>
            {conversation.hasMore && <button className="load-more" onClick={() => { viewport.loadMore(projectionId); forceCardState(value => value + 1); }}>Load earlier activity</button>}
            {visibleSections.length === 0 && <div className="empty-state"><div className="brand-orb">C</div><h3>Conversation activity appears here</h3></div>}
            {visibleSections.map(section => {
                const rootKey = section.rootCardKey ? stableKey(section.rootCardKey) : "";
                const prompt = rootKey === "" ? undefined : section.cards.find(card => stableKey(card.key) === rootKey);
                const nestedCards = prompt ? section.cards.filter(card => card !== prompt) : [];
                const nested = nestedCards.length > 0 ? nestedCards.map(card => renderCard(card)) : undefined;
                return <section key={section.key} className="turn-section">
                    {prompt ? renderCard(prompt, nested, true) : section.cards.map(card => renderCard(card))}
                </section>;
            })}
        </div>
        <div ref={composerDock} className="composer-dock">
            <SettingsPanel key={`settings:${projectionId}`} session={session} draft={settingsDraft} onChange={(field, value) => {
                changeSettingDraft(settingsDrafts, projectionId, canonicalSettings, settingsRevision, field, value);
                forceSettingsState(revision => revision + 1);
            }} />
            <Composer key={`shared-composer:${snapshot.newThreadDraftRevision}`} session={session} active={Boolean(thread || snapshot.newThreadIntent) && session.canSubmit()} draftKey="shared" drafts={drafts.current} options={settingsOptions} />
        </div>
    </main>;
}

function SettingsPanel({session, draft, onChange}: {session: BrowserFrontendSession; draft: SettingDraft; onChange: (field: SettingField, value: string) => void}) {
    const [open, setOpen] = useState(false);
    const {values, touched} = draft;
    const models = session.model.modelCatalog();
    const modelDefinitions = Array.isArray(models) ? models : [];
    const profilesDomain = session.model.globalDomains().get("operation.permission-profiles.list");
    const profiles = Array.isArray(profilesDomain) ? profilesDomain : (profilesDomain && typeof profilesDomain === "object" && Array.isArray((profilesDomain as {data?: unknown}).data) ? (profilesDomain as {data: unknown[]}).data : []);
    const select = (label: string, field: SettingField, choices: readonly [string, string][]) => <label><span>{label}</span><select value={values[field]} onChange={event => onChange(field, event.target.value)}>{choices.map(([name, value]) => <option key={value} value={value}>{name}</option>)}</select></label>;
    const defaults: [string, string] = ["Thread default", DefaultSetting];
    return <div className={`settings-panel ${open ? "open" : ""}`}>
        <button className="settings-toggle" onClick={() => setOpen(value => !value)} aria-expanded={open}>Turn settings <span>{touched.size > 0 ? `${touched.size} changed` : "Thread defaults"} {open ? "⌃" : "⌄"}</span></button>
        {open && <div className="settings-grid">
            {select("Model", "model", [defaults, ...modelDefinitions.filter(value => typeof value === "object" && value !== null && !((value as {hidden?: boolean}).hidden)).map(value => [String((value as {displayName?: string}).displayName ?? (value as {model?: string; id?: string}).model ?? (value as {id?: string}).id), String((value as {model?: string; id?: string}).model ?? (value as {id?: string}).id)] as [string, string])])}
            {select("Reasoning", "effort", [defaults, ...["minimal", "low", "medium", "high", "xhigh", "ultra"].map(value => [humanize(value), value] as [string, string])])}
            {select("Access", "sandbox", [defaults, ["Workspace", "workspace-write"], ["Read only", "read-only"], ["Full access", "danger-full-access"], ["External", "external"]])}
            {select("Network", "network", [defaults, ["Restricted", "restricted"], ["Enabled", "enabled"]])}
            <label><span>Workspace</span><input value={values.cwd} placeholder="Provider workspace path" onChange={event => onChange("cwd", event.target.value)} /></label>
            {select("Approval", "approval", [defaults, ["On request", "on-request"], ["Untrusted", "untrusted"], ["Never", "never"]])}
            {select("Style", "personality", [defaults, ["None", "none"], ["Friendly", "friendly"], ["Pragmatic", "pragmatic"]])}
            {select("Approval reviewer", "reviewer", [defaults, ["User", "user"], ["Auto review", "auto_review"], ["Guardian", "guardian_subagent"]])}
            {select("Permission profile", "permissionProfile", [defaults, ...profiles.filter(value => typeof value === "object" && value !== null && (value as {allowed?: boolean}).allowed !== false).map(value => { const id = String((value as {id?: string}).id); return [permissionProfileLabel(id), id] as [string, string]; })])}
            <label><span>Service tier</span><input value={values.serviceTier === DefaultSetting ? "" : values.serviceTier} placeholder="Thread default" onChange={event => onChange("serviceTier", event.target.value || DefaultSetting)} /></label>
            {select("Reasoning summary", "summary", [defaults, ["Auto", "auto"], ["Concise", "concise"], ["Detailed", "detailed"], ["None", "none"]])}
            {select("Collaboration mode", "collaboration", [["Code", "default"], ["Plan", "plan"]])}
        </div>}
    </div>;
}

function Composer({session, active, draftKey, drafts, options}: {session: BrowserFrontendSession; active: boolean; draftKey: string; drafts: Map<string, string>; options: SettingPromptOptions}) {
    const [prompt, setPrompt] = useState(drafts.get(draftKey) ?? "");
    const editor = useRef<HTMLTextAreaElement>(null);
    const running = session.model.activeTurnId(session.getSnapshot().selectedThreadId) !== undefined;
    useBrowserLayoutEffect(() => {
        const element = editor.current;
        if (!element) return;
        element.style.height = "auto";
        const maximum = 180;
        element.style.height = `${Math.min(element.scrollHeight, maximum)}px`;
        element.style.overflowY = element.scrollHeight > maximum ? "auto" : "hidden";
    }, [prompt]);
    const submit = (event: FormEvent) => {
        event.preventDefault();
        if (!active || prompt.trim() === "") return;
        const value = prompt; setPrompt(""); drafts.set(draftKey, "");
        void session.submitPrompt(value, [], options.turn, options.thread);
    };
    return <form className="composer" onSubmit={submit}>
        <textarea ref={editor} value={prompt} disabled={!active} onChange={event => { setPrompt(event.target.value); drafts.set(draftKey, event.target.value); }}
            aria-label="Message Codex" aria-describedby="composer-keyboard-hint" aria-keyshortcuts="Enter Control+Enter Meta+Enter"
            onKeyDown={event => { if (shouldSubmitPromptFromKey({
                key: event.key, altKey: event.altKey, ctrlKey: event.ctrlKey, metaKey: event.metaKey,
                shiftKey: event.shiftKey, repeat: event.repeat,
                isComposing: event.nativeEvent.isComposing || event.nativeEvent.keyCode === 229,
            })) { event.preventDefault(); event.currentTarget.form?.requestSubmit(); } }}
            placeholder={active ? "Message Codex…" : "Select or create a thread"} rows={1} />
        <div className="composer-actions"><span id="composer-keyboard-hint">Enter to send · Shift+Enter for a new line</span>
            {running ? <button type="button" className="stop-button" onClick={() => session.interrupt()}>■ Stop</button>
                : <button type="submit" className="send-button" disabled={!active || prompt.trim() === ""}>Send ↑</button>}</div>
    </form>;
}

export function inspectorPlainState(selected: ThreadPresentation | undefined, enabled: boolean): unknown {
    if (!enabled || !selected) return null;
    return {
        id: selected.id, title: selected.title, cwd: selected.cwd, status: selected.status, archived: selected.archived,
        turns: selected.turnOrder.map(id => { const turn = selected.turns.get(id)!; return {id, status: turn.status, plan: turn.plan,
            items: turn.itemOrder.map(itemId => turn.items.get(itemId)?.raw)}; }),
        agents: selected.agentOrder.map(id => selected.agents.get(id)), domains: Object.fromEntries(selected.domains),
    };
}

function Inspector({session, revision, drawer = false, paneRef, onClose}: {session: BrowserFrontendSession; revision: number} & DrawerPaneProps) {
    void revision;
    const [tab, setTab] = useState<"plan" | "agents" | "requests" | "state" | "protocol">("plan");
    const selected = session.model.thread(session.getSnapshot().selectedThreadId);
    const requests = [...session.model.pendingRequestPresentations().values()];
    const latestPlan = tab === "plan" && selected
        ? [...selected.turnOrder].reverse().map(id => selected.turns.get(id)).find(turn => turn && (Object.keys(turn.plan).length > 0 || turn.itemOrder.some(itemId => turn.items.get(itemId)?.raw.type === "plan")))
        : undefined;
    const plainState = inspectorPlainState(selected, tab === "state");
    return <aside ref={paneRef} className={`inspector-pane${drawer ? " responsive-drawer drawer-right" : ""}`} id={drawer ? "inspector-pane" : undefined}
        role={drawer ? "dialog" : undefined} aria-modal={drawer || undefined} aria-labelledby={drawer ? "inspector-pane-title" : undefined}>
        <div className="pane-heading"><div><span className="eyebrow">Details</span><h2 id={drawer ? "inspector-pane-title" : undefined}>Inspector</h2></div>
            {drawer && <button type="button" className="drawer-close" data-drawer-close onClick={onClose} aria-label="Close Inspector drawer">×</button>}</div>
        <nav className="inspector-tabs">{(["plan", "agents", "requests", "state", "protocol"] as const).map(value =>
            <button key={value} className={tab === value ? "active" : ""} onClick={() => setTab(value)}>{humanize(value)}{value === "requests" && requests.length > 0 ? ` ${requests.length}` : ""}</button>)}</nav>
        <div className="inspector-content">
            {tab === "plan" && (!selected ? <p className="muted-copy">Select a thread to inspect its plan.</p> : latestPlan ? <div className="plan-view">
                {typeof latestPlan.plan.explanation === "string" && <p>{latestPlan.plan.explanation}</p>}
                {Array.isArray(latestPlan.plan.steps) && latestPlan.plan.steps.map((step, index) => {
                    const status = effectivePlanStepStatus(String((step as {status?: string}).status ?? ""), latestPlan.status, selected.status);
                    return <div key={index}><StatusDot tone={classifyStatus(status).tone} /><span>{String((step as {step?: string}).step ?? "")}</span><small>{displayStatus(status)}</small></div>;
                })}
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

function RequestCard({request, session}: {request: PendingRequestPresentation; session: BrowserFrontendSession}) {
    const raw = request.raw && typeof request.raw === "object" ? request.raw as Record<string, unknown> : {};
    const questions = Array.isArray(raw.questions) ? raw.questions as Record<string, unknown>[] : [];
    const [answers, setAnswers] = useState<Record<string, string[]>>({});
    const [otherAnswers, setOtherAnswers] = useState<Record<string, string>>({});
    const [structured, setStructured] = useState("{}");
    const details = pendingRequestDetails(request);
    const decisions = pendingDecisionOptions(request);
    const resolving = session.isPendingResolving(request);
    const actionable = session.canResolvePending(request);
    const userInputValid = questions.length > 0 && questions.every(question => {
        const id = typeof question.id === "string" ? question.id : "";
        return id !== "" && ((answers[id]?.length ?? 0) > 0 || (otherAnswers[id]?.trim() ?? "") !== "");
    });
    let structuredInput: unknown = raw.requestedSchema === undefined ? null : {};
    let structuredValid = true;
    if (request.kind === "mcp-elicitation" && raw.requestedSchema !== undefined) {
        try { structuredInput = JSON.parse(structured); structuredValid = structuredInput !== null && typeof structuredInput === "object" && !Array.isArray(structuredInput); }
        catch { structuredValid = false; }
    }
    const respond = (decision: string) => {
        let input: unknown = {};
        if (request.kind === "user-input") input = Object.fromEntries(questions.map(question => {
            const id = typeof question.id === "string" ? question.id : "";
            const values = [...(answers[id] ?? [])];
            const other = otherAnswers[id]?.trim() ?? "";
            if (other !== "") values.push(other);
            return [id, {answers: values}];
        }));
        else if (request.kind === "mcp-elicitation") input = structuredInput;
        const response = pendingResponse(request, decision, input);
        if (response) session.resolvePending(request, response);
    };
    const toggleAnswer = (id: string, value: string, checked: boolean) => setAnswers(current => ({...current,
        [id]: checked ? [...(current[id] ?? []), value] : (current[id] ?? []).filter(answer => answer !== value),
    }));
    return <div className="request-card" aria-busy={resolving || undefined}>
        <strong>{humanize(request.kind)}</strong>
        {typeof raw.command === "string" && raw.command.trim() !== "" && <code>{raw.command}</code>}
        {details.entries.length > 0 && <dl className="request-details">{details.entries.map((detail, index) => <div key={`${detail.path}-${index}`}>
            <dt>{detail.path.split(" / ").map(humanize).join(" / ")}</dt><dd>{detail.value}</dd></div>)}
            {details.truncated && <div><dt>Additional detail</dt><dd>Too large to display safely</dd></div>}</dl>}
        {request.kind === "user-input" && questions.map((question, questionIndex) => {
            const id = typeof question.id === "string" ? question.id : "";
            const options = Array.isArray(question.options) ? question.options as Record<string, unknown>[] : [];
            const showOther = options.length === 0 || question.isOther === true;
            return <fieldset className="request-question" key={id || questionIndex}><legend>{String((question.header ?? question.question ?? id) || `Question ${questionIndex + 1}`)}</legend>
                {question.header !== undefined && question.question !== undefined && <span>{String(question.question)}</span>}
                {options.map((choice, optionIndex) => { const label = String(choice.label ?? ""); return label === "" ? null : <label key={`${label}-${optionIndex}`}>
                    <input type="checkbox" checked={(answers[id] ?? []).includes(label)} onChange={event => toggleAnswer(id, label, event.target.checked)} />
                    <span>{label}{typeof choice.description === "string" && choice.description !== "" && <small>{choice.description}</small>}</span></label>; })}
                {showOther && <input type={question.isSecret ? "password" : "text"} value={otherAnswers[id] ?? ""}
                    placeholder={options.length > 0 ? "Other answer" : "Type your answer"}
                    onChange={event => setOtherAnswers(current => ({...current, [id]: event.target.value}))} />}
            </fieldset>;
        })}
        {request.kind === "mcp-elicitation" && raw.requestedSchema !== undefined && <textarea value={structured} onChange={event => setStructured(event.target.value)} rows={5} aria-label="Structured MCP response" />}
        {request.kind === "mcp-elicitation" && raw.requestedSchema !== undefined && !structuredValid && <p className="request-validation">Enter a valid JSON object.</p>}
        <div className="request-actions">{resolving ? <span>Resolving…</span> : decisions.map(decision => {
            const inputValid = request.kind === "user-input" ? userInputValid : request.kind === "mcp-elicitation" ? structuredValid : true;
            const safeWithoutFullDisclosure = ["decline", "cancel", "denied", "abort", "unavailable", "unsupported"].includes(decision.value);
            return <button type="button" key={decision.value} className={`request-button ${decision.tone === "neutral" ? "" : decision.tone}`}
                disabled={!actionable || (details.truncated && !safeWithoutFullDisclosure) || (decision.requiresInput === true && !inputValid)}
                onClick={() => respond(decision.value)}>{decision.label}</button>;
        })}</div>
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
    const responsiveMode = useResponsiveMode();
    const [drawer, setDrawer] = useState<"threads" | "inspector" | null>(null);
    const [newThreadDialog, setNewThreadDialog] = useState(false);
    const threadTrigger = useRef<HTMLButtonElement>(null);
    const inspectorTrigger = useRef<HTMLButtonElement>(null);
    const threadDrawer = useRef<HTMLElement>(null);
    const inspectorDrawer = useRef<HTMLElement>(null);
    const threadsOverlay = responsiveMode === "mobile";
    const inspectorOverlay = responsiveMode !== "desktop";
    const activeDrawer = drawer === "threads" && threadsOverlay || drawer === "inspector" && inspectorOverlay ? drawer : null;
    const closeDrawer = () => setDrawer(null);
    const requestNewThread = () => { closeDrawer(); setNewThreadDialog(true); };
    const createNewThreadDraft = (draft: NewThreadDraft) => {
        session.beginNewThread(draft); setNewThreadDialog(false); closeDrawer();
    };
    useEffect(() => setDrawer(null), [responsiveMode]);
    useEffect(() => {
        if (!activeDrawer || typeof document === "undefined") return;
        const pane = activeDrawer === "threads" ? threadDrawer.current : inspectorDrawer.current;
        const trigger = activeDrawer === "threads" ? threadTrigger.current : inspectorTrigger.current;
        if (!pane) return;
        const focusableSelector = "a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex=\"-1\"])";
        const focusables = () => [...pane.querySelectorAll<HTMLElement>(focusableSelector)].filter(element => element.offsetParent !== null);
        const frame = window.requestAnimationFrame(() => (pane.querySelector<HTMLElement>("[data-drawer-close]") ?? pane).focus());
        const onKeyDown = (event: KeyboardEvent) => {
            if (event.key === "Escape") { event.preventDefault(); closeDrawer(); return; }
            if (event.key !== "Tab") return;
            const available = focusables();
            if (available.length === 0) { event.preventDefault(); pane.focus(); return; }
            const first = available[0]!; const last = available[available.length - 1]!;
            if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
            else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
        };
        document.addEventListener("keydown", onKeyDown);
        return () => {
            window.cancelAnimationFrame(frame);
            document.removeEventListener("keydown", onKeyDown);
            if (trigger?.isConnected) trigger.focus();
        };
    }, [activeDrawer]);
    const paneControls = <>
        {threadsOverlay && <button ref={threadTrigger} type="button" className="responsive-pane-button" aria-haspopup="dialog" aria-controls="thread-pane" aria-expanded={activeDrawer === "threads"} onClick={() => setDrawer("threads")}><span aria-hidden="true">☰</span> Threads</button>}
        {inspectorOverlay && <button ref={inspectorTrigger} type="button" className="responsive-pane-button" aria-haspopup="dialog" aria-controls="inspector-pane" aria-expanded={activeDrawer === "inspector"} onClick={() => setDrawer("inspector")}><span aria-hidden="true">ⓘ</span> Inspector</button>}
    </>;
    return <div className={`app-shell${activeDrawer ? " drawer-visible" : ""}`}>
        <header className="top-bar" aria-hidden={activeDrawer || newThreadDialog ? true : undefined}><div className="brand"><span className="brand-mark">C</span><span><b>CodexUI</b><small>Codex, clearly.</small></span></div>
            <div className="workspace-breadcrumb">{session.model.thread(snapshot.selectedThreadId)?.cwd || snapshot.newThreadDraft?.workspace || "No workspace"}</div>
            <div className="top-actions">
                {connection.connected && <button className="subtle-button" onClick={() => canControl ? session.releaseController() : session.claimController()}>{canControl ? "Release control" : "Claim control"}</button>}
                <label className="connection-control"><input value={url} onChange={event => setUrl(event.target.value)} aria-label="Bridge WebSocket URL" />
                    <button onClick={() => connection.connected || connection.retrying ? session.disconnect() : session.connect(url)}>{connection.connected ? "Disconnect" : "Connect"}</button><StatusDot tone={connectionTone} /></label>
            </div>
        </header>
        {snapshot.notice && <div className="notice-banner" role="alert" aria-hidden={activeDrawer || newThreadDialog ? true : undefined}><span>{snapshot.notice}</span><button onClick={() => session.dismissNotice()} aria-label="Dismiss notice">×</button></div>}
        <div className="workspace-grid" aria-hidden={activeDrawer || newThreadDialog ? true : undefined}>
            {!threadsOverlay && <ThreadPane session={session} revision={snapshot.revision} onRequestNewThread={requestNewThread} />}
            <Conversation session={session} revision={snapshot.revision} paneControls={responsiveMode === "desktop" ? undefined : paneControls} />
            {!inspectorOverlay && <Inspector session={session} revision={snapshot.revision} />}
        </div>
        <footer className="status-bar" aria-hidden={activeDrawer || newThreadDialog ? true : undefined}><div><strong>© Volker Christian &amp; Codex</strong><span> | </span>
            <a href="https://github.com/SNodeC/CodexUI">CodexUI</a><span> • </span><a href="https://github.com/SNodeC/AISuite">AISuite</a><span> • </span>
            <small>Powered by</small> <a href="https://github.com/SNodeC/snode.c">SNode.C</a></div>
            <div className="global-status"><span>Status:</span><StatusDot tone={connectionTone} /><strong>{globalStatus}</strong></div></footer>
        {activeDrawer && <button type="button" className="drawer-backdrop" tabIndex={-1} onClick={closeDrawer} aria-label={`Close ${activeDrawer === "threads" ? "Threads" : "Inspector"} drawer`} />}
        {activeDrawer === "threads" && <ThreadPane session={session} revision={snapshot.revision} onRequestNewThread={requestNewThread} drawer paneRef={threadDrawer} onClose={closeDrawer} />}
        {activeDrawer === "inspector" && <Inspector session={session} revision={snapshot.revision} drawer paneRef={inspectorDrawer} onClose={closeDrawer} />}
        {newThreadDialog && <NewThreadDialog initialWorkspace={session.model.thread(snapshot.selectedThreadId)?.cwd ?? ""} onCancel={() => setNewThreadDialog(false)} onContinue={createNewThreadDraft} />}
    </div>;
}
