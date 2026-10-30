import {protocolTimes, targetTimes, timingSummary, timingDetails, tokenUsage} from "../presentation/PresentationTelemetry.js";
import type {ProtocolTime, TimingTarget, TokenUsageText} from "../presentation/PresentationTelemetry.js";
import {memo, useId, useEffect, useLayoutEffect, useMemo, useRef, useState, useSyncExternalStore} from "react";
import type {FormEvent, ReactNode, RefObject} from "react";
import Markdown from "react-markdown";
import remarkGfm from "remark-gfm";
import {
    AuthoritativeHistoryPageSize, UnknownStatus, anchoredScrollTop, changeSettingDraft, displayStatus,
    fixedSettingChoices, humanizeProtocolLabel as humanize, isObject, member,
    foldedCardScrollTop, nestedScrollConsumes,
    pendingDecisionOptions, pendingRequestDetails, stableKey, stringMember,
    isEmptyStatus, isWorkingStatus, projectTurnPlan, settingDraftFor, settingPresentation, settingPromptOptions, statusTone,
    statusToken, trimTrailingEmptyLines, turnSettingCatalog, turnOpenings, turnNavigationTarget,
} from "../index.js";
import type {ConversationViewportAnchor, PendingRequestPresentation, SettingCatalog, SettingDraft, SettingField, SettingPresentation, ThreadPresentation} from "../index.js";
import type {
    AgentActivityData, CommandExecutionData, FileChangesData, LocalPromptData,
    ReasoningData, UserMessageData, AgentMessageData, GenericActivityData,
    ImageGenerationData, PlanData, VisibleCardData,
} from "../index.js";
import {DraftThreadId} from "./BrowserFrontendSession.js";
import type {BrowserFrontendSession, NewThreadDraft, ThreadSortCriterion} from "./BrowserFrontendSession.js";
import type {WorkspaceGroup, AgentPresentation} from "../presentation/PresentationModel.js";
import {shouldSubmitPromptFromKey} from "./ComposerKeyboard.js";
import {readBrowserStorage, writeBrowserStorage} from "./BrowserStorage.js";

const useBrowserLayoutEffect = typeof window === "undefined" ? useEffect : useLayoutEffect;
export const ThreadLoadingSpinnerDelayMilliseconds = 500;

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
    onHide?: () => void;
}

interface ThreadPresentationTarget {threadId: string; presentationKey: string}

export function runThreadPaneNavigation(navigate: () => void, onClose?: () => void): void {
    navigate();
    onClose?.();
}

interface ConversationPresentationOptions {
    showReasoning: boolean;
    showCodexUpdates: boolean;
    commandsInitiallyExpanded: boolean;
    imagesInitiallyExpanded: boolean;
    fileChangesInitiallyExpanded: boolean;
}

function PresentationIcon({kind}: {kind: "reasoning" | "updates" | "command" | "image" | "files"}) {
    if (kind === "reasoning") return <svg viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="6" r="4"/><path d="M5.5 9 6.5 11h3L10.5 9M7 13h2"/></svg>;
    if (kind === "updates") return <svg viewBox="0 0 16 16" aria-hidden="true"><path d="M4.5 11.5 4 14l4-2.5h3.5A2.5 2.5 0 0 0 14 9V5a2.5 2.5 0 0 0-2.5-2.5h-7A2.5 2.5 0 0 0 2 5v4a2.5 2.5 0 0 0 2.5 2.5Z"/><path d="M5.5 7h.01M8 7h.01M10.5 7h.01"/></svg>;
    if (kind === "files") return <svg viewBox="0 0 16 16" aria-hidden="true"><path d="M4 1.5h5l3 3v10H4zM9 1.5v3h3M6 8h4M6 11h4" /></svg>;
    if (kind === "command") return <svg viewBox="0 0 16 16" aria-hidden="true"><rect x="1.5" y="2.5" width="13" height="11" rx="2"/><path d="m4.5 6 2 2-2 2M8.5 10h3"/></svg>;
    return <svg viewBox="0 0 16 16" aria-hidden="true"><rect x="1.5" y="2.5" width="13" height="11" rx="2"/><circle cx="10.5" cy="5.5" r="1"/><path d="m3.5 11 3-3.5 2 2L10 8l2.5 3"/></svg>;
}

function CopyIcon({checked}: {checked: boolean}) {
    return <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.3" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"><g className="copy-glyph"><rect x="2.5" y="2" width="8" height="9" rx="1.5"/><rect x="5.5" y="5" width="8" height="9" rx="1.5"/></g><path className="check-glyph" d="m2.5 8 3.25 3.25L13.5 3.5" data-visible={checked ? "true" : "false"}/></svg>;
}

function FoldIcon({collapsed, leading = false}: {collapsed: boolean; leading?: boolean}) {
    return <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"><path d={collapsed ? leading ? "m6 3 5 5-5 5" : "m10 3-5 5 5 5" : "m3 6 5 5 5-5"} /></svg>;
}

function ImageRibbon({paths}: {paths: string[]}) {
    return paths.length > 0 ? <div className="image-ribbon">{paths.map(path => <code key={path}>{path}</code>)}</div> : null;
}

const defaultConversationPresentation: ConversationPresentationOptions = {
    showReasoning: false,
    showCodexUpdates: true,
    commandsInitiallyExpanded: true,
    imagesInitiallyExpanded: true,
    fileChangesInitiallyExpanded: false,
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
        fileChangesInitiallyExpanded: boolean("codexui.conversation.fileChangesInitiallyExpanded", false),
    };
}

const activityTimeFormat = new Intl.DateTimeFormat([], {hour: "2-digit", minute: "2-digit", second: "2-digit"});
const activityDateFormat = new Intl.DateTimeFormat();
function threadTimestampText(timestamp: number, now = new Date()): string {
    const activity = new Date(timestamp * 1000);
    if (Number.isNaN(activity.getTime())) return `${activity.toLocaleDateString()} ${activity.toLocaleTimeString()}`;
    const sameDate = activity.getFullYear() === now.getFullYear()
        && activity.getMonth() === now.getMonth() && activity.getDate() === now.getDate();
    const time = activityTimeFormat.format(activity);
    return sameDate ? time : `${activityDateFormat.format(activity)} ${time}`;
}

export function lastActivityText(timestamp: number, now = new Date()): string {
    return `Last activity: ${threadTimestampText(timestamp, now)}`;
}

function persistConversationPresentation(options: ConversationPresentationOptions): void {
    writeBrowserStorage("codexui.conversation.showReasoning", String(options.showReasoning));
    writeBrowserStorage("codexui.conversation.showCodexUpdates", String(options.showCodexUpdates));
    writeBrowserStorage("codexui.conversation.commandsInitiallyExpanded", String(options.commandsInitiallyExpanded));
    writeBrowserStorage("codexui.conversation.imagesInitiallyExpanded", String(options.imagesInitiallyExpanded));
    writeBrowserStorage("codexui.conversation.fileChangesInitiallyExpanded", String(options.fileChangesInitiallyExpanded));
}

function StatusDot({tone}: {tone: string}) { return <span className={`status-dot ${tone}`} aria-hidden="true" />; }

const ThreadSortControl = memo(function ThreadSortControl({value, onChange}: {value: ThreadSortCriterion;
    onChange: (value: ThreadSortCriterion) => void}) {
    return <label className="thread-sort"><span>Sort:</span><select aria-label="Thread sort order" value={value}
        onChange={event => onChange(event.target.value as ThreadSortCriterion)}>
        <option value="alphanumeric">Alphanumeric</option><option value="created">Created</option>
        <option value="recent">Recent</option><option value="section">Section order</option></select></label>;
});

function ThreadPane({session, onRequestNewThread, onRequestGroup, onRequestForkWithOptions, onRequestWorkspace, drawer = false, paneRef, onClose, onHide}: {session: BrowserFrontendSession; onRequestNewThread: (draft?: NewThreadDraft) => void; onRequestGroup: (kind: "project" | "section", group?: WorkspaceGroup, readOnly?: boolean) => void; onRequestForkWithOptions: (target: ThreadPresentationTarget) => void; onRequestWorkspace: (target: ThreadPresentationTarget) => void} & DrawerPaneProps) {
    const snapshot = session.getSnapshot();
    const selected = snapshot.selectedThreadId;
    useEffect(() => session.refreshGroups(), [session]);
    const [, forceDisclosureState] = useState(0);
    const sortCriterion = session.browser.sort;
    const [search, setSearch] = useState(session.browser.search);
    const groupSignature = [...session.groups("project").map(group => group.id), ...session.groups("section").map(group => group.id)].join("\0");
    useEffect(() => {
        if (!session.groupingAvailable() || session.browser.grouping === "ungrouped") return;
        for (const [kind, groups] of [["project", session.model.projects], ["section", session.model.sections]] as const)
            for (const id of groups.keys()) if (session.groupExpanded(`${kind}:${id}`)) session.browseGroup(kind, id);
    }, [session, groupSignature, session.browser.search, session.browser.archive, sortCriterion, session.browser.grouping]);
    const [contextMenu, setContextMenu] = useState<(ThreadPresentationTarget | {kind: "project" | "section"; group: WorkspaceGroup; projectId: string}) & {x: number; y: number; trigger: HTMLElement} | null>(null);
    const contextMenuRef = useRef<HTMLDivElement>(null);
    const requestMoreNearEnd = (list: HTMLDivElement) => {
        if (list.scrollHeight - list.scrollTop - list.clientHeight <= Math.max(48, list.clientHeight / 2))
            session.loadMoreThreads();
    };
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
    const openContextMenu = (threadId: string, presentationKey: string, x: number, y: number, trigger: HTMLElement) =>
        setContextMenu({threadId, presentationKey, x, y, trigger});
    const contextThread = contextMenu && "threadId" in contextMenu && session.threadVisualKey(contextMenu.threadId) === contextMenu.presentationKey
        ? session.model.thread(contextMenu.threadId) : undefined;
    const groupTarget = contextMenu && "group" in contextMenu ? contextMenu : undefined;
    const contextGroup = groupTarget && (groupTarget.kind === "project" ? session.model.projects : session.model.sections).get(groupTarget.group.id) === groupTarget.group ? groupTarget : undefined;
    useEffect(() => { if (contextMenu && !contextGroup && !contextThread) setContextMenu(null); }, [contextMenu, contextThread, contextGroup]);
    const connection = session.model.connection();
    const providerReady = connection.connected && connection.providerState === "ready";
    const invokeContextAction = (action: () => void) => { contextMenu?.trigger.focus(); setContextMenu(null); action(); };
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
    const toggle = (threadId: string, presentationKey: string, expanded: boolean) => {
        if (session.viewportState.setThreadExpanded(threadId, presentationKey, !expanded))
            forceDisclosureState(revision => revision + 1);
    };
    const visibleRows: {id: string; presentationKey: string; depth: number; position: number;
        setSize: number; expanded: boolean}[] = [];
    const eligibleThreads = new Set(session.browserThreadIds());
    const indicatorTones = session.model.threadIndicatorTones();
    const appendVisibleRows = (ids: readonly string[], depth: number) => {
        const present = ids.filter(id => eligibleThreads.has(id) || id === DraftThreadId && snapshot.newThreadDraft);
        for (let index = 0; index < present.length; ++index) {
            const id = present[index]!;
            const presentationKey = session.threadVisualKey(id);
            const thread = session.model.thread(id);
            const expanded = session.viewportState.threadExpanded(id, presentationKey);
            visibleRows.push({id, presentationKey, depth, position: index + 1, setSize: present.length, expanded});
            if (thread && expanded) appendVisibleRows(thread.childThreadOrder.filter(child => session.browser.grouping === "ungrouped"
                || ["projectId", "sectionId"].every(key => session.model.thread(child)?.[key as "projectId" | "sectionId"] === thread[key as "projectId" | "sectionId"])), depth + 1);
        }
    };
    const roots = [...(snapshot.newThreadDraft ? [DraftThreadId] : []), ...session.threadOrder(sortCriterion)];
    const membership = (id: string, key: "projectId" | "sectionId") => id === DraftThreadId ? snapshot.newThreadDraft?.[key] ?? "" : session.model.thread(id)?.[key] ?? "";
    const renderThread = ({id, presentationKey, depth, position, setSize, expanded}: (typeof visibleRows)[number]): ReactNode => {
        const thread = session.model.thread(id);
        const draft = id === DraftThreadId ? snapshot.newThreadDraft : undefined;
        if (!thread && !draft) return null;
        const draftSubmissions = draft ? session.prompts.submissions(id) : [];
        const draftActive = draftSubmissions.some(value => value.state === "queued" || value.state === "inFlight");
        const draftFailed = !draftActive && draftSubmissions.some(value => value.state === "failed");
        const status = thread?.status ?? UnknownStatus;
        const hasChildren = (thread?.childThreadOrder.length ?? 0) > 0;
        const promptAnimating = session.threadPromptAnimating(id);
        const optimisticClass = draftFailed ? " optimistic-failed"
            : promptAnimating ? " prompt-awaiting"
                : draft ? " optimistic-awaiting" : "";
        const title = draft?.name || thread?.title || (draft ? "New thread" : id);
        const detail = draft ? draftFailed ? "not created" : "creating" : thread?.cwd || thread?.preview || id;
        const statusText = draft ? detail : displayStatus(status);
        const parentId = session.model.childOwnership(id)?.parentThreadId;
        const recentAt = session.threadRecentAt(id);
        const hoverDetails = [title, `Workspace: ${thread?.cwd || draft?.workspace || "Unknown"}`,
            `Status: ${statusText}`,
            ...(indicatorTones.get(id) === "indirect" ? ["Indirect activity: running descendant"] : []),
            `Recent turn: ${recentAt === undefined ? "Unknown" : threadTimestampText(recentAt)}`,
            `Created: ${thread?.createdAt === undefined ? "Unknown" : threadTimestampText(thread.createdAt)}`,
            `Last activity: ${thread?.lastActivityAt === undefined ? "Unknown" : threadTimestampText(thread.lastActivityAt)}`,
            ...(parentId ? [`Parent: ${session.model.thread(parentId)?.title || parentId}`] : [])];
        const accessibleDetails = hoverDetails.join(", ");
        const contextOpen = Boolean(contextMenu && "threadId" in contextMenu && contextMenu.threadId === id && contextMenu.presentationKey === presentationKey);
        return <div key={presentationKey} role="treeitem" aria-level={depth + 1} aria-selected={selected === id}
            aria-posinset={position} aria-setsize={setSize} aria-expanded={hasChildren ? expanded : undefined}
            aria-label={accessibleDetails} title={hoverDetails.join("\n")}>
            <div className={`thread-row-wrap ${selected === id ? "selected" : ""}${contextOpen ? " context-open" : ""}${optimisticClass}`} style={{paddingLeft: `${8 + depth * 12}px`}}
                onContextMenu={event => { if (!thread) return; event.preventDefault(); event.stopPropagation(); openContextMenu(id, presentationKey, event.clientX, event.clientY, event.currentTarget.querySelector<HTMLElement>(".thread-row") ?? event.currentTarget); }}>
                <button className="tree-toggle" disabled={!hasChildren} onClick={() => toggle(id, presentationKey, expanded)} aria-label={expanded ? "Collapse child threads" : "Expand child threads"}>{hasChildren && <FoldIcon collapsed={!expanded} leading />}</button>
                <button className="thread-row" aria-current={selected === id ? "true" : undefined}
                    aria-label={`Open ${accessibleDetails}`} onClick={() => runThreadPaneNavigation(() => session.selectThread(id, presentationKey), onClose)}>
                    <StatusDot tone={draftFailed ? "danger" : draft ? "warning" : indicatorTones.get(id) || "muted"} /><span className="thread-title">{title}</span>
                </button>
                {thread && <button className="thread-menu-trigger" title="Thread actions" aria-label={`Actions for ${thread.title || id}`}
                    aria-haspopup="menu" aria-expanded={contextOpen}
                    onClick={event => { event.stopPropagation(); const bounds = event.currentTarget.getBoundingClientRect(); openContextMenu(id, presentationKey, bounds.right, bounds.bottom, event.currentTarget); }}>•••</button>}
            </div>
        </div>;
    };
    const renderRows = (ids: string[], depth: number) => {
        visibleRows.length = 0; appendVisibleRows(ids, depth); return visibleRows.map(renderThread);
    };
    const pageControls = (pages: ReturnType<BrowserFrontendSession["threadPages"]>) => pages.map((page, index) =>
        (page.pending || page.error || page.nextCursor) && <button key={index} className="load-more" disabled={page.pending}
            onClick={() => session.loadPage(page)}>{page.pending ? "Loading…" : page.error ? `Retry: ${page.error}` : "Load more"}</button>);
    const groupLabels = new Map<WorkspaceGroup, string>();
    for (const kind of ["project", "section"] as const) {
        const groups = session.groups(kind), names = new Map<string, number>(), qualifiers = new Map<string, number>();
        const root = (group: WorkspaceGroup) => { const roots = member(group.raw, "roots", []); return Array.isArray(roots) ? stringMember(roots[0], "path") : ""; };
        for (const group of groups) { names.set(group.name, (names.get(group.name) ?? 0) + 1); const key = `${group.name}\0${root(group)}`; qualifiers.set(key, (qualifiers.get(key) ?? 0) + 1); }
        for (const group of groups) groupLabels.set(group, (group.name || "Unnamed") + ((names.get(group.name) ?? 0) > 1
            ? ` · ${kind === "project" && root(group) ? root(group) + ((qualifiers.get(`${group.name}\0${root(group)}`) ?? 0) > 1 ? ` · ${group.id}` : "") : group.id}` : ""));
    }
    const groupLabel = (_kind: "project" | "section", group: WorkspaceGroup) => groupLabels.get(group) ?? group.name;
    const renderGroup = (kind: "project" | "section", group: WorkspaceGroup, ids: string[], nested = false, projectId = ""): ReactNode => {
        const expanded = session.groupExpanded(`${kind}:${group.id}`);
        const active = membership(selected, `${kind}Id`) === group.id;
        const members = session.model.threadIds().filter(id => membership(id, `${kind}Id`) === group.id
            && (!nested || membership(id, "projectId") === projectId));
        const tone = members.some(id => ["active", "indirect"].includes(indicatorTones.get(id) ?? "")) ? "indirect"
            : members.some(id => statusTone(session.model.thread(id)!.status) !== "") ? "success" : "muted";
        const statusDescription = tone === "indirect" ? "Indirect activity: running descendant" : tone === "success" ? "Threads: loaded" : "Threads: not loaded";
        return <section key={`${projectId}:${kind}:${group.id}`} className={`browser-group ${kind}-group ${nested ? "nested-section" : ""}${active ? " active-group" : ""}`} role="treeitem" aria-label={`${groupLabel(kind, group)}, ${statusDescription}`} aria-expanded={expanded}>
            <header onContextMenu={event => { event.preventDefault(); setContextMenu({kind, group, projectId, x: event.clientX, y: event.clientY, trigger: event.currentTarget.querySelector<HTMLElement>(".group-disclosure") ?? event.currentTarget}); }}><button className="group-disclosure" title={groupLabel(kind, group)} onClick={() => session.toggleGroup(kind, group.id)} aria-expanded={expanded}><span className="group-arrow"><FoldIcon collapsed={!expanded} leading /></span><StatusDot tone={tone} /><span>{kind === "section" && !nested ? "Section · " : ""}{groupLabel(kind, group)}</span></button>
                <button className="thread-menu-trigger group-menu-trigger" aria-label={`Actions for ${groupLabel(kind, group)}`} aria-haspopup="menu"
                    aria-expanded={contextGroup?.group === group && contextGroup.projectId === projectId}
                    onClick={event => { const bounds = event.currentTarget.getBoundingClientRect(); setContextMenu({kind, group, projectId, x: bounds.right, y: bounds.bottom, trigger: event.currentTarget}); }}>•••</button></header>
            {expanded && <div role="group" className={kind === "project" ? "project-thread-body" : undefined}>{kind === "project" ? renderProjectMembers(ids, group.id) : renderRows(ids, nested ? 2 : 1)}
                {pageControls(session.threadPages({[`${kind}Id`]: group.id}))}</div>}
        </section>;
    };
    const renderProjectMembers = (ids: string[], projectId: string): ReactNode => <>{session.groups("section").filter(section => ids.some(id => membership(id, "sectionId") === section.id))
        .map(section => renderGroup("section", section, ids.filter(id => membership(id, "sectionId") === section.id), true, projectId))}
        {renderRows(ids.filter(id => !session.model.sections.has(membership(id, "sectionId"))), 1)}</>;
    const projects = session.groups("project").sort((a, b) => Number(b.raw.recencyAt ?? 0) - Number(a.raw.recencyAt ?? 0));
    if (session.browser.manualProjects) projects.sort((a, b) => Number(a.raw.position ?? 0) - Number(b.raw.position ?? 0));
    const grouped = session.groupingAvailable() && session.browser.grouping !== "ungrouped";
    const projectRoots = grouped && session.browser.grouping === "projects" ? projects : [];
    const standalone = roots.filter(id => !projectRoots.some(group => membership(id, "projectId") === group.id));
    return <aside ref={paneRef} className={`thread-pane${drawer ? " responsive-drawer drawer-left" : ""}`} id={drawer ? "thread-pane" : undefined}
        role={drawer ? "dialog" : undefined} aria-modal={drawer || undefined} aria-labelledby={drawer ? "thread-pane-title" : undefined}>
        <div className="pane-heading"><div><h2 id={drawer ? "thread-pane-title" : undefined}>Threads</h2></div>
            <div className="pane-heading-actions">
                {onHide && <button onClick={onHide} aria-label="Hide Threads">Hide</button>}
                {drawer && <button type="button" className="drawer-close" data-drawer-close onClick={onClose} aria-label="Close Threads drawer">×</button>}</div></div>
        <button className="new-thread-button" onClick={() => onRequestNewThread()} aria-label="New thread">+ New thread</button>
        <div className="thread-toolbar"><Menu className="thread-browser-menu" label={`${humanize(session.browser.grouping)} ⌄`}><div className="thread-browser-controls">
            <label>Group<select aria-label="Thread grouping" value={session.browser.grouping} onChange={event => session.changeBrowser({grouping: event.target.value as typeof session.browser.grouping})}><option value="projects">Projects</option><option value="sections">Sections</option><option value="ungrouped">Ungrouped</option></select></label>
            <button onClick={() => session.refreshBrowser()} disabled={!providerReady}>Refresh</button>
            <button disabled={!session.canSubmit() || !session.groupingAvailable()} onClick={() => onRequestGroup("project")}>New project…</button><button disabled={!session.canSubmit() || !session.groupingAvailable()} onClick={() => onRequestGroup("section")}>New section…</button>
            <label><input type="checkbox" checked={session.browser.manualProjects} onChange={event => session.changeBrowser({manualProjects: event.target.checked})} />Manual project order</label>
        </div></Menu><ThreadSortControl value={sortCriterion} onChange={sort => session.changeBrowser({sort})} /></div>
        <div className="thread-filter"><input aria-label="Search thread titles" placeholder="Search titles · Enter" value={search} onChange={event => { setSearch(event.target.value); if (!event.target.value) session.changeBrowser({search: ""}); }} onKeyDown={event => { if (event.key === "Enter") session.changeBrowser({search: search.trim()}); }} /><label><span className="visually-hidden">Show</span><select aria-label="Thread archive filter" value={session.browser.archive} onChange={event => session.changeBrowser({archive: event.target.value as typeof session.browser.archive})}><option value="active">Active</option><option value="archived">Archived</option><option value="all">All</option></select></label></div>
        <div className="thread-list" role="tree" aria-label="Threads"
            onScroll={event => requestMoreNearEnd(event.currentTarget)}>
            {projectRoots.map(group => renderGroup("project", group, roots.filter(id => membership(id, "projectId") === group.id)))}
            {grouped && session.groups("section").filter(section => standalone.some(id => membership(id, "sectionId") === section.id)
                || !session.model.threadIds().some(id => membership(id, "sectionId") === section.id)).map(section => renderGroup("section", section, standalone.filter(id => membership(id, "sectionId") === section.id)))}
            {renderRows(standalone.filter(id => !grouped || !session.model.sections.has(membership(id, "sectionId"))), 0)}
            {pageControls(session.threadPages())}
            {session.catalogPages().map((page, index) => <div key={index}>{pageControls([page])}</div>)}
        </div>
        {contextMenu && (contextThread || contextGroup) && <div ref={contextMenuRef} className="thread-context-menu" role="menu" aria-label={contextGroup ? `Actions for ${groupLabel(contextGroup.kind, contextGroup.group)}` : `Actions for ${contextThread?.title}`} onKeyDown={navigateContextMenu}>
            {contextGroup ? <>
                <button role="menuitem" disabled={!session.canSubmit()} onClick={() => invokeContextAction(() => {
                    const projectId = contextGroup.kind === "project" ? contextGroup.group.id : contextGroup.projectId;
                    const roots = member(session.model.projects.get(projectId)?.raw, "roots", []);
                    onRequestNewThread({workspace: Array.isArray(roots) && roots.length === 1 ? stringMember(roots[0], "path") : "", name: "", baseInstructions: "", developerInstructions: "", ephemeral: false,
                        ...(projectId ? {projectId} : {}), ...(contextGroup.kind === "section" ? {sectionId: contextGroup.group.id} : {})});
                })}>New thread here…</button>
                <button role="menuitem" onClick={() => invokeContextAction(() => onRequestGroup(contextGroup.kind, contextGroup.group, true))}>Details…</button>
                <button role="menuitem" disabled={!session.canSubmit()} onClick={() => invokeContextAction(() => onRequestGroup(contextGroup.kind, contextGroup.group))}>Edit…</button>
                <button role="menuitem" disabled={!session.canSubmit()} onClick={() => invokeContextAction(() => { if (window.confirm("Delete this group? Its threads will be retained.")) void session.manageGroup(`${contextGroup.kind}.delete`, {[`${contextGroup.kind}Id`]: contextGroup.group.id}, contextGroup.group); })}>Delete group…</button>
                {contextGroup.kind === "project" && <label>Move project before<select aria-label="Move project before" value="" disabled={!session.canSubmit()} onChange={event => { const value = event.target.value; if (value) invokeContextAction(() => void session.manageGroup("project.move", {projectId: contextGroup.group.id, ...(value === "__end__" ? {} : {beforeProjectId: value})}, contextGroup.group)); }}>
                    <option value="">Choose destination</option>{session.groups("project").filter(other => other !== contextGroup.group).map(other => <option key={other.id} value={other.id}>{groupLabel("project", other)}</option>)}<option value="__end__">Move to end</option></select></label>}
            </> : contextThread && "threadId" in contextMenu && <>
            {session.model.childOwnership(contextThread.id)?.parentThreadId && <button role="menuitem" onClick={() => invokeContextAction(() => session.selectThread(session.model.childOwnership(contextThread.id)!.parentThreadId))}>Go to parent</button>}
            {(["project", "section"] as const).map(kind => <label key={kind}>Assign {kind}<select aria-label={`Assign ${kind}`} value="" disabled={!session.canSubmit() || !session.groupingAvailable()} onChange={event => { const value = event.target.value; if (value) invokeContextAction(() => void session.assignThreadGroup(contextThread.id, kind, value === "__none__" ? "" : value)); }}><option value="">Choose {kind}</option><option value="__none__">Remove from {kind}</option>{[...(kind === "project" ? session.model.projects : session.model.sections).values()].map(group => <option key={group.id} value={group.id}>{groupLabel(kind, group)}</option>)}</select></label>)}
            {contextThread.sectionId && <label>Move before in shared section<select aria-label="Move before in shared section" value="" disabled={!session.canSubmit()} onChange={event => { if (event.target.value) invokeContextAction(() => void session.assignThreadGroup(contextThread.id, "section", contextThread.sectionId, event.target.value === "__end__" ? undefined : event.target.value)); }}><option value="">Choose destination</option>{session.model.threadIds().filter(id => id !== contextThread.id && session.model.thread(id)?.sectionId === contextThread.sectionId).map(id => <option key={id} value={id}>{session.model.thread(id)!.title}</option>)}<option value="__end__">Move to end</option></select></label>}
            <button role="menuitem" disabled={!providerReady} onClick={() => invokeContextAction(() => session.reloadThread(contextThread.id, contextMenu.presentationKey))}>Reload</button>
            <button role="menuitem" disabled={session.workspaceChangeUnavailable(contextThread.id, contextMenu.presentationKey) !== ""} title={session.workspaceChangeUnavailable(contextThread.id, contextMenu.presentationKey)} onClick={() => invokeContextAction(() => onRequestWorkspace({threadId: contextThread.id, presentationKey: contextMenu.presentationKey}))}>Change workspace…</button>
            <button role="menuitem" disabled={!session.canSubmit() || session.operationPending("thread.rename", contextThread.id, contextMenu.presentationKey)} onClick={() => invokeContextAction(() => { const name = window.prompt("Thread name", contextThread.title); if (name?.trim()) session.renameThread(contextThread.id, name.trim(), contextMenu.presentationKey); })}>Rename</button>
            <button role="menuitem" disabled={!session.canSubmit() || session.operationPending("thread.fork", contextThread.id, contextMenu.presentationKey)} onClick={() => invokeContextAction(() => session.forkThread(contextThread.id, undefined, contextMenu.presentationKey))}>Quick fork</button>
            <button role="menuitem" disabled={!session.canSubmit() || session.operationPending("thread.fork", contextThread.id, contextMenu.presentationKey)} onClick={() => invokeContextAction(() => onRequestForkWithOptions({threadId: contextThread.id, presentationKey: contextMenu.presentationKey}))}>Fork with options…</button>
            <button role="menuitem" disabled={!session.canSubmit() || session.operationPending("thread.archive", contextThread.id, contextMenu.presentationKey)} onClick={() => invokeContextAction(() => session.archiveThread(contextThread.id, contextThread.archived, contextMenu.presentationKey))}>{contextThread.archived ? "Unarchive" : "Archive"}</button>
            <button role="menuitem" className="danger" disabled={!session.canSubmit() || session.operationPending("thread.delete", contextThread.id, contextMenu.presentationKey)} onClick={() => invokeContextAction(() => { if (window.confirm(`Delete “${contextThread.title || contextThread.id}”?`)) session.deleteThread(contextThread.id, contextMenu.presentationKey); })}>Delete</button>
            </>}
        </div>}
    </aside>;
}

export function GroupDialog({session, kind, group, readOnly, onClose}: {session: BrowserFrontendSession; kind: "project" | "section"; group?: WorkspaceGroup; readOnly: boolean; onClose: () => void}) {
    const dialog = useRef<HTMLDialogElement>(null);
    const [name, setName] = useState(group?.name ?? "");
    const [roots, setRoots] = useState(() => {
        const values = member(group?.raw, "roots", []); return Array.isArray(values) ? values.map(root => stringMember(root, "path")).join("\n") : "";
    });
    const [description, setDescription] = useState(stringMember(member(group?.raw, "metadata", {}), "codexui.description"));
    const [icon, setIcon] = useState(stringMember(member(group?.raw, "appearance", {}), "icon"));
    const [color, setColor] = useState(stringMember(member(group?.raw, "appearance", {}), "color"));
    const [error, setError] = useState("");
    const [saving, setSaving] = useState(false);
    const [idempotencyKey] = useState(() => Array.from(globalThis.crypto.getRandomValues(new Uint32Array(4)), value => value.toString(16).padStart(8, "0")).join(""));
    useEffect(() => {
        const previous = document.activeElement as HTMLElement | null;
        dialog.current?.showModal();
        return () => { if (previous?.isConnected) previous.focus(); };
    }, []);
    const save = async (event: FormEvent) => {
        event.preventDefault(); if (saving || readOnly || !session.canSubmit()) return;
        if (!name.trim()) { setError("Enter a name."); return; }
        const paths = roots.split("\n").map(path => path.trim()).filter(Boolean);
        if (kind === "project" && paths.some(path => !path.startsWith("/"))) { setError("Workspace roots must be absolute app-server paths."); return; }
        setSaving(true); setError("");
        const parameters = kind === "project" ? {name: name.trim(), roots: paths.map(path => ({path})),
            ...(group ? {projectId: group.id, description} : {idempotencyKey, metadata: {"codexui.description": description}})}
            : {name: name.trim(), ...(group ? {sectionId: group.id} : {}), appearance: {icon: icon || null, color: color || null}};
        const saved = await session.manageGroup(`${kind}.${group ? "update" : "create"}`, parameters, group);
        setSaving(false); if (saved) onClose(); else setError("The group was not saved. Review the notice and retry.");
    };
    return <dialog ref={dialog} className="new-thread-dialog group-dialog" aria-labelledby="group-dialog-title" onCancel={onClose}>
        <header><h2 id="group-dialog-title">{group ? readOnly ? "Details" : "Edit" : "New"} {kind}</h2></header>
        <form onSubmit={event => void save(event)}>
            <label>Name<input autoFocus value={name} readOnly={readOnly} onChange={event => setName(event.target.value)} /></label>
            {kind === "project" ? <><label>Roots<textarea value={roots} readOnly={readOnly} onChange={event => setRoots(event.target.value)} placeholder="One absolute app-server path per line" /></label>
                <label>Description<textarea value={description} readOnly={readOnly} onChange={event => setDescription(event.target.value)} /></label><p>Descriptions organise work; they are not model instructions.</p></>
                : <><label>Icon<input value={icon} readOnly={readOnly} onChange={event => setIcon(event.target.value)} /></label><label>Colour<input value={color} readOnly={readOnly} onChange={event => setColor(event.target.value)} /></label><p>Sections are shared across projects. Changes apply everywhere.</p></>}
            {readOnly && <pre className="state-json">{JSON.stringify(group?.raw, null, 2)}</pre>}
            {error && <p role="alert">{error}</p>}
            <footer><button type="button" onClick={onClose}>{readOnly ? "Close" : "Cancel"}</button>{!readOnly && <button type="submit" disabled={saving || !session.canSubmit()}>{saving ? "Saving…" : "Save"}</button>}</footer>
        </form>
    </dialog>;
}

export function NewThreadDialog({initialWorkspace, initialDraft, purpose = "create", workspaceChoices = [], onCancel, onContinue}: {initialWorkspace: string; initialDraft?: NewThreadDraft; purpose?: "create" | "fork" | "workspace"; workspaceChoices?: readonly string[]; onCancel: () => void; onContinue: (draft: NewThreadDraft) => void}) {
    const dialog = useRef<HTMLElement>(null);
    const workspaceInput = useRef<HTMLInputElement>(null);
    const [workspace, setWorkspace] = useState(initialDraft?.workspace ?? initialWorkspace);
    const [name, setName] = useState(initialDraft?.ephemeral ? "" : (initialDraft?.name ?? ""));
    const [baseInstructions, setBaseInstructions] = useState(initialDraft?.baseInstructions ?? "");
    const [developerInstructions, setDeveloperInstructions] = useState(initialDraft?.developerInstructions ?? "");
    const [ephemeral, setEphemeral] = useState(initialDraft?.ephemeral ?? false);
    const [error, setError] = useState("");
    useBrowserLayoutEffect(() => {
        const previous = typeof document === "undefined" ? null : document.activeElement as HTMLElement | null;
        workspaceInput.current?.focus();
        return () => { if (previous?.isConnected) previous.focus(); };
    }, []);
    const submit = (event: FormEvent) => {
        event.preventDefault();
        if (workspace.trim() === "") { setError("Enter the app-server workspace path."); workspaceInput.current?.focus(); return; }
        onContinue({workspace: workspace.trim(), name: ephemeral ? "" : name.trim(), baseInstructions: baseInstructions.trim(),
            developerInstructions: developerInstructions.trim(), ephemeral,
            ...(initialDraft?.projectId ? {projectId: initialDraft.projectId} : {}), ...(initialDraft?.sectionId ? {sectionId: initialDraft.sectionId} : {})});
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
    const title = purpose === "workspace" ? "Change workspace…" : purpose === "fork" ? "Fork with options" : "New thread";
    return <div className="modal-backdrop"><section ref={dialog} className="new-thread-dialog" role="dialog" aria-modal="true" aria-labelledby="thread-options-title" onKeyDown={keyDown}>
        <header><h2 id="thread-options-title">{title}</h2><p>{purpose === "workspace" ? `Thread: ${initialDraft?.name ?? ""}` : purpose === "fork" ? "Adjust the copied thread context." : "Set thread context."}</p><p>Workspace paths refer to the app-server filesystem.</p></header>
        <form onSubmit={submit}>
            {purpose === "workspace" && <label>Current workspace<input readOnly value={initialWorkspace || "Workspace not reported"} /></label>}
            <label><span>{purpose === "workspace" ? "Proposed workspace" : "Workspace"}</span><input list={workspaceChoices.length ? "workspace-roots" : undefined} ref={workspaceInput} value={workspace} onChange={event => { setWorkspace(event.target.value); setError(""); }} placeholder="App-server directory path" /></label>
            {workspaceChoices.length > 0 && <datalist id="workspace-roots">{workspaceChoices.map(path => <option key={path} value={path} />)}</datalist>}
            {purpose !== "workspace" && <><label><span>Name</span><input value={name} disabled={ephemeral} onChange={event => setName(event.target.value)} placeholder="Optional thread name" /></label>
            <label><span>Base instructions</span><textarea value={baseInstructions} onChange={event => setBaseInstructions(event.target.value)} placeholder="Optional base instructions" /></label>
            <label><span>Developer instructions</span><textarea value={developerInstructions} onChange={event => setDeveloperInstructions(event.target.value)} placeholder="Optional developer instructions" /></label>
            <label className="ephemeral-choice"><input type="checkbox" checked={ephemeral} onChange={event => { const checked = event.target.checked; setEphemeral(checked); if (checked) setName(""); }} /><span><strong>Temporary thread</strong><small>Temporary threads are not retained in normal Codex history.</small></span></label></>}
            {error && <p className="dialog-error" role="alert">{error}</p>}
            <footer><button type="button" onClick={onCancel}>Cancel</button><button type="submit" className="primary">{purpose === "workspace" ? "Change workspace" : "Continue"}</button></footer>
        </form>
    </section></div>;
}

function safeHref(value: string): string | undefined {
    try { const url = new URL(value); return url.protocol === "https:" || url.protocol === "http:" ? url.href : undefined; }
    catch { return undefined; }
}
const SafeMarkdown = memo(function SafeMarkdown({text}: {text: string}) {
    return <div className="safe-markdown"><Markdown remarkPlugins={[remarkGfm]} skipHtml components={{
        a({href, children}) {
            const safe = safeHref(href ?? "");
            return safe ? <a href={safe} target="_blank" rel="noreferrer">{children}</a>
                : <span>{children}{href ? ` (${href})` : ""}</span>;
        },
        img({src, alt}) { return <span className="markdown-image-reference">{alt || "Image"}{src ? ` (${src})` : ""}</span>; },
    }}>{text}</Markdown></div>;
});

export function userMessageMarkdownText(text: string): string {
    const lines = text.split("\n").map(line => line.endsWith("\r") ? line.slice(0, -1) : line);
    let fenceMarker = "", fenceLength = 0;
    return lines.map((line, index) => {
        const marker = /^ {0,3}(`{3,}|~{3,})/u.exec(line);
        const opens = fenceLength === 0 && marker !== null;
        const closes = fenceLength > 0 && marker?.[1]?.[0] === fenceMarker
            && marker[1].length >= fenceLength && /^\p{White_Space}*$/u.test(line.slice(marker[0].length));
        const inside = fenceLength > 0 || opens;
        const blank = /^\p{White_Space}*$/u.test(line);
        const hardBreak = index < lines.length - 1 && !line.endsWith("\\") && !line.endsWith("  ")
            && !inside && !closes && !/^( {4}|\t)/u.test(line)
            && !blank && !/^\p{White_Space}*$/u.test(lines[index + 1]!) ? "  " : "";
        if (opens) { fenceMarker = marker[1]![0]!; fenceLength = marker[1]!.length; }
        else if (closes) { fenceMarker = ""; fenceLength = 0; }
        return `${line}${hardBreak}`;
    }).join("\n");
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
        const marker = step.status.semantic === "completed" ? "✓" : step.status.semantic === "running" ? "◉" : "○";
        rows.push(`${marker} ${step.text}  `);
    }
    return rows.join("\n");
}

function commandMetadata(command: CommandExecutionData): string {
    const values: string[] = [];
    if (command.exitCode !== undefined) values.push(`exit ${command.exitCode}`);
    if (command.cwd) values.push(command.cwd);
    if (command.durationMilliseconds !== undefined) {
        const seconds = command.durationMilliseconds / 1000;
        values.push(`${seconds.toFixed(seconds < 10 ? 1 : 0)} s`);
    }
    return values.filter(Boolean).join("  |  ");
}

function agentMetadata(activity: AgentActivityData, status: VisibleCardData["status"]): string {
    return [activity.tool, isEmptyStatus(status) ? activity.kind : "", activity.receivers.join(", "), activity.model,
        activity.reasoningEffort, activity.childThreadId ? `thread ${activity.childThreadId}` : "",
        activity.agentPath, activity.senderThreadId ? `sender ${activity.senderThreadId}` : ""]
        .filter(Boolean).join("  |  ");
}

function fileChangeMetadata(changes: FileChangesData): string {
    let additions = 0; let deletions = 0; let countsAvailable = false;
    for (const change of changes.changes) if (change.additions !== undefined && change.deletions !== undefined) {
        additions += change.additions; deletions += change.deletions; countsAvailable = true;
    }
    return [`${changes.changes.length} paths`, countsAvailable ? `+${additions} −${deletions}` : ""]
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
        return {text: joinCopyText([trimTrailingEmptyLines(data.command), trimTrailingEmptyLines(data.output)]), markdown: false};
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
    return {text: data.displayDetail, markdown: false};
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

function ScrollableCode({text, className, label, state}: {text: string; className: string; label: string; state: {scrollTop: number; following: boolean}}) {
    const surface = useRef<HTMLPreElement>(null);
    const appliedScrollTop = useRef<number>();
    useBrowserLayoutEffect(() => {
        const element = surface.current; if (!element) return;
        const settle = () => { element.scrollTop = state.following ? element.scrollHeight : state.scrollTop; appliedScrollTop.current = element.scrollTop; };
        settle();
        const observer = new ResizeObserver(settle); observer.observe(element);
        return () => observer.disconnect();
    }, [text, state]);
    return <pre ref={surface} className={className} tabIndex={0} aria-label={label}
        onScroll={event => { const element = event.currentTarget; if (appliedScrollTop.current === element.scrollTop) { appliedScrollTop.current = undefined; return; }
            appliedScrollTop.current = undefined; state.scrollTop = element.scrollTop;
            state.following = className === "command-output" && element.scrollHeight - element.scrollTop - element.clientHeight <= 2; }}
        onWheel={event => {
            const element = event.currentTarget;
            if (nestedScrollConsumes(event.deltaY, element.scrollTop, element.clientHeight, element.scrollHeight)) event.stopPropagation();
        }}><code>{text}</code></pre>;
}

type ClipboardOutcome = "copied" | "unsupported" | "failed";

export function Card({card, active, collapsed, onToggle, onCopy, nested, turnContainer = false, nestedCard = false, times = [], onTiming, onRestore, commandScroll, outputScroll, onTurnMenu}: {onTurnMenu?: (position: {x: number; y: number}, opener: HTMLButtonElement) => void; times?: ProtocolTime[]; onTiming?: () => void; onRestore?: () => void; commandScroll?: {scrollTop: number; following: boolean}; outputScroll?: {scrollTop: number; following: boolean}; card: VisibleCardData; active: boolean; collapsed: boolean; onToggle: () => void; onCopy?: (content: CardCopyContent) => ClipboardOutcome | Promise<ClipboardOutcome> | void; nested?: ReactNode; turnContainer?: boolean; nestedCard?: boolean}) {
    const fallbackScroll = useRef({command: {scrollTop: 0, following: false}, output: {scrollTop: 0, following: true}});
    const [copyFeedback, setCopyFeedback] = useState<{text: string; failed: boolean}>();
    const copyFeedbackTimer = useRef<ReturnType<typeof setTimeout>>();
    useEffect(() => () => { if (copyFeedbackTimer.current) clearTimeout(copyFeedbackTimer.current); }, []);
    const copy = async (content: CardCopyContent) => {
        const outcome = await (onCopy ? onCopy(content) : writeCardClipboard(content));
        if (!outcome) return;
        if (copyFeedbackTimer.current) clearTimeout(copyFeedbackTimer.current);
        const failed = outcome !== "copied";
        setCopyFeedback({text: failed ? "Copy failed" : "Copied", failed});
        copyFeedbackTimer.current = setTimeout(() => setCopyFeedback(undefined), 500);
    };
    let title = humanize(card.kind);
    let body: ReactNode;
    let cardVariant = "";
    const presentsStatus = ["commandExecution", "agentActivity", "fileChanges", "imageGeneration", "genericActivity"]
        .includes(card.kind) && !isEmptyStatus(card.status);
    let phaseClass = presentsStatus ? `status ${statusTone(card.status)}` : "";
    let phaseLabel = presentsStatus ? displayStatus(card.status) : "";
    if (card.kind === "userMessage") {
        const data = card.payload as UserMessageData; title = "You"; phaseLabel = nestedCard ? "steering" : "";
        body = <><SafeMarkdown text={userMessageMarkdownText(data.text)} /><ImageRibbon paths={data.imagePaths} /></>;
    } else if (card.kind === "localPrompt") {
        const data = card.payload as LocalPromptData; title = data.state === "failed" ? "Not sent" : "You"; phaseLabel = nestedCard && data.state !== "failed" ? "steering" : "";
        body = <><SafeMarkdown text={userMessageMarkdownText(data.prompt)} /><ImageRibbon paths={data.imagePaths} />{data.error && <div className="error-text">{data.error}</div>}{data.requiresExplicitRecovery && onRestore && <button onClick={onRestore}>Restore to composer</button>}</>;
    } else if (card.kind === "agentMessage") {
        const data = card.payload as AgentMessageData; title = "Codex"; cardVariant = data.phase === "final_answer" ? "final" : "update"; phaseClass = cardVariant; phaseLabel = data.phase === "final_answer" ? "final answer" : data.phase === "partial_answer" ? "partial answer" : "update";
        body = <SafeMarkdown text={data.text} />;
    } else if (card.kind === "reasoning") {
        const data = card.payload as ReasoningData; title = "Reasoning";
        body = data.summary ? <SafeMarkdown text={data.summary} /> : active ? <div className="activity-line"><i />Working…</div> : null;
    } else if (card.kind === "commandExecution") {
        const data = card.payload as CommandExecutionData; title = "Command execution";
        const metadata = commandMetadata(data);
        body = <><ScrollableCode className="command-line" label="Command" state={commandScroll ?? fallbackScroll.current.command} text={trimTrailingEmptyLines(data.command)} />
            {data.output && <ScrollableCode className="command-output" label="Command output" state={outputScroll ?? fallbackScroll.current.output} text={trimTrailingEmptyLines(data.output)} />}
            {metadata && <small className="card-status">{metadata}</small>}</>;
    } else if (card.kind === "fileChanges") {
        const data = card.payload as FileChangesData; title = "File changes";
        body = <><div className="file-list">{data.changes.map(change => <div key={`${change.path}:${change.kind}`}>
            <span>{change.path}</span><small>{humanize(change.kind || "changed")} {change.additions !== undefined && <b className="plus">+{change.additions}</b>} {change.deletions !== undefined && <b className="minus">−{change.deletions}</b>}</small>
        </div>)}</div><small className="card-status">{fileChangeMetadata(data)}</small></>;
    } else if (card.kind === "agentActivity") {
        const data = card.payload as AgentActivityData; title = "Agent activity";
        const metadata = agentMetadata(data, card.status);
        body = <>{metadata && <small className="card-status">{metadata}</small>}
            {data.prompt && <div className="card-text">{data.prompt}</div>}{data.resultText && <SafeMarkdown text={data.resultText} />}</>;
    } else if (card.kind === "imageGeneration") {
        const data = card.payload as ImageGenerationData; title = !isEmptyStatus(card.status) || data.revisedPrompt ? "Generated image" : "Image";
        body = <>{data.revisedPrompt && <div className="card-text">{data.revisedPrompt}</div>}<ImageRibbon paths={data.path ? [data.path] : []} /></>;
    } else if (card.kind === "plan") {
        const data = card.payload as PlanData; title = "Plan"; body = <SafeMarkdown text={planMarkdown(data)} />;
    } else {
        const data = card.payload as GenericActivityData; title = humanize(data.type) || "Activity";
        body = <pre className="generic-activity-data">{data.displayDetail}</pre>;
    }
    const copyContent = cardCopyContent(card);
    const foldable = ["userMessage", "localPrompt", "agentMessage", "commandExecution", "agentActivity", "reasoning", "fileChanges", "imageGeneration", "plan", "genericActivity"].includes(card.kind)
        && !(card.kind === "reasoning" && !(card.payload as ReasoningData).summary);
    const activeTurn = active && turnContainer && (card.kind === "localPrompt" || card.kind === "userMessage");
    const activeWork = isWorkingStatus(card.status);
    const delayedPending = card.kind === "localPrompt" && (card.payload as LocalPromptData).showPendingAnimation;
    return <article tabIndex={-1} className={`conversation-card ${card.kind} ${cardVariant} ${collapsed ? "collapsed" : ""} ${turnContainer ? "turn-container" : ""} ${nestedCard ? "steering" : ""} ${activeTurn ? "active-turn" : ""} ${activeWork ? "active-work" : ""} ${delayedPending ? "delayed-pending" : ""}`} data-card-key={stableKey(card.key)}>
        <header><span className="card-heading"><span title={title}>{title}</span>{phaseLabel && <span className={`card-phase ${phaseClass || "steering"}`} title={phaseLabel}>{phaseLabel}</span>}</span>{onTiming && <button className="card-timing" title={timingDetails(times)} aria-label="Timing details" onClick={onTiming}>{timingSummary(times) || "—"}</button>}<span className="card-meta">{card.turnOpening && onTurnMenu && <button aria-label="Turn actions" aria-haspopup="menu" onClick={event => { const rect = event.currentTarget.getBoundingClientRect(); onTurnMenu({x: rect.right, y: rect.bottom}, event.currentTarget); }}>⋯</button>}{copyContent.text && <span className="card-copy-control"><button className={`card-copy-button${copyFeedback && !copyFeedback.failed ? " copied" : ""}`} onClick={() => void copy(copyContent)} aria-label="Copy card content"><CopyIcon checked={Boolean(copyFeedback && !copyFeedback.failed)} /></button>{copyFeedback && <span className={`card-copy-overlay${copyFeedback.failed ? " failed" : ""}`} role="status" aria-live="polite">{copyFeedback.text}</span>}</span>}{foldable && <button className="card-fold-button" onClick={onToggle} aria-label={collapsed ? "Expand card" : "Collapse card"}><FoldIcon collapsed={collapsed} /></button>}</span></header>{!collapsed && <>{body}{nested && <div className="turn-nested">{nested}</div>}</>}
    </article>;
}

interface PendingConversationGeometry {
    presentationKey: string;
    following: boolean;
    anchor: ConversationViewportAnchor | undefined;
    focus?: boolean;
    first?: boolean;
    fold?: {cardKey: string; collapsed: boolean; previousTitleTop: number};
}

function conversationAnchor(container: HTMLElement, preferred?: ConversationViewportAnchor): ConversationViewportAnchor | undefined {
    const viewportTop = container.getBoundingClientRect().top;
    let result: ConversationViewportAnchor | undefined;
    const retained = preferred ? cardForKey(container, preferred.cardKey)?.querySelector<HTMLElement>(":scope > header") : undefined;
    if (retained) {
        result = {cardKey: preferred!.cardKey, pixelOffset: retained.getBoundingClientRect().top - viewportTop};
    } else {
        const cards = container.querySelectorAll<HTMLElement>(".conversation-card[data-card-key]");
        for (let index = 0; !result && index < cards.length; ++index) {
            const card = cards[index]!;
            const header = card.querySelector<HTMLElement>(":scope > header");
            const nested = card.querySelector<HTMLElement>(":scope > .turn-nested");
            if (header && (nested?.getBoundingClientRect().top ?? card.getBoundingClientRect().bottom) >= viewportTop) {
                result = {cardKey: card.dataset.cardKey ?? "", pixelOffset: header.getBoundingClientRect().top - viewportTop};
            }
        }
    }
    return result;
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

export function ThreadLoadingSurface({spinning}: {spinning: boolean}) {
    return <div className="conversation-loading-surface" role="status" aria-live="polite">
        {spinning && <span className="thread-loading-spinner" aria-hidden="true" />}
        <span className="visually-hidden">Loading conversation</span>
    </div>;
}

interface TurnRemovalTarget {
    threadId: string; turnId: string; presentationKey: string; turn: object;
    position: {x: number; y: number}; stage: "menu" | "confirm"; cardKey: string; opener: HTMLButtonElement;
}

function TurnRemovalSurface({session, target, onClose, onConfirm}: {
    session: BrowserFrontendSession; target: TurnRemovalTarget; onClose: () => void; onConfirm: () => void;
}) {
    const menu = useRef<HTMLDivElement>(null);
    const dialog = useRef<HTMLDialogElement>(null);
    useBrowserLayoutEffect(() => () => {
        dialog.current?.close();
        const opener = target.opener;
        const sameCard = opener.isConnected && opener.closest<HTMLElement>("[data-card-key]")?.dataset.cardKey === target.cardKey;
        const focus = sameCard ? opener : document.querySelector<HTMLElement>(".conversation-pane");
        focus?.focus({preventScroll: true});
    }, [target.opener, target.cardKey]);
    const reason = session.revertUnavailable(target.threadId, target.turnId, target.presentationKey, target.turn);
    useBrowserLayoutEffect(() => {
        if (target.stage === "confirm") {
            dialog.current?.showModal();
            dialog.current?.querySelector<HTMLButtonElement>("button[data-cancel]")?.focus();
        } else {
            const element = menu.current;
            if (element) {
                element.style.left = `${Math.max(8, Math.min(target.position.x - element.offsetWidth, window.innerWidth - element.offsetWidth - 8))}px`;
                element.style.top = `${Math.max(8, Math.min(target.position.y, window.innerHeight - element.offsetHeight - 8))}px`;
                (element.querySelector<HTMLButtonElement>("button:not(:disabled)") ?? element).focus({preventScroll: true});
            }
        }
    }, [target.stage]);
    useEffect(() => {
        const dismiss = (event: PointerEvent) => {
            if (target.stage === "menu" && !menu.current?.contains(event.target as Node)) { onClose(); }
        };
        document.addEventListener("pointerdown", dismiss);
        return () => { document.removeEventListener("pointerdown", dismiss); };
    }, [target.stage, onClose]);
    return target.stage === "menu"
        ? <div ref={menu} className="thread-context-menu" role="menu" aria-label="Turn actions" tabIndex={-1}
            onKeyDown={event => { if (event.key === "Escape" || event.key === "Tab") { onClose(); } }}>
            <button role="menuitem" className="danger" disabled={reason !== ""} title={reason} onClick={onConfirm}>Remove this turn and all later turns…</button>
        </div>
        : <dialog ref={dialog} className="new-thread-dialog" aria-labelledby="turn-removal-title" onCancel={event => { event.preventDefault(); onClose(); }}>
            <form onSubmit={event => { event.preventDefault(); session.removeTurns(target.threadId, target.turnId, target.presentationKey, target.turn); onClose(); }}>
                <h2 id="turn-removal-title">Remove conversation history?</h2>
                <p>This will permanently remove the selected turn and every later turn from this conversation. Earlier turns will remain. File changes and commands already performed will not be undone.</p>
                {reason && <p className="dialog-error" role="status">{reason}</p>}
                <footer><button type="button" data-cancel onClick={onClose}>Cancel</button><button type="submit" className="danger" disabled={reason !== ""}>Remove turns</button></footer>
            </form>
        </dialog>;
}

function Conversation({session, paneControls, onInspect, usage, onRequestWorkspace}: {session: BrowserFrontendSession; onRequestWorkspace: (target: ThreadPresentationTarget) => void; usage: TokenUsageText; paneControls?: ReactNode; onInspect: (view: "requests" | "timing", target?: TimingTarget) => void}) {
    const snapshot = session.getSnapshot();
    const [turnRemoval, setTurnRemoval] = useState<TurnRemovalTarget>();
    const thread = session.model.thread(snapshot.selectedThreadId);
    const projectionId = snapshot.selectedThreadId;
    const presentationKey = session.threadVisualKey(projectionId);
    const viewport = session.viewportState;
    const authoritativeCount = thread?.turnOrder.reduce((count, id) =>
        count + (thread.turns.get(id)?.itemOrder.length ?? 0), 0) ?? 0;
    const limit = viewport.effectiveLimit(projectionId, presentationKey, authoritativeCount);
    const conversation = session.conversation(limit);
    const pane = useRef<HTMLElement>(null);
    const scroll = useRef<HTMLDivElement>(null);
    const composerDock = useRef<HTMLDivElement>(null);
    const previousPresentation = useRef(presentationKey);
    const pendingGeometry = useRef<PendingConversationGeometry>();
    const [cardStateRevision, forceCardState] = useState(0);
    const displayedPresentation = useRef(presentationKey);
    const transitionGeneration = useRef(0);
    const [spinnerProjection, setSpinnerProjection] = useState("");
    const threadTransitionActive = projectionId !== "" && projectionId !== DraftThreadId
        && (snapshot.selectedThreadLoading || displayedPresentation.current !== presentationKey);
    useEffect(() => {
        const generation = ++transitionGeneration.current;
        if (!threadTransitionActive) {
            displayedPresentation.current = presentationKey;
            setSpinnerProjection("");
            return;
        }
        let spinnerTimer: ReturnType<typeof setTimeout> | undefined;
        let revealFrame: number | undefined;
        if (snapshot.selectedThreadLoading) {
            setSpinnerProjection(current => current === presentationKey ? current : "");
            spinnerTimer = setTimeout(() => {
                if (transitionGeneration.current === generation) setSpinnerProjection(presentationKey);
            }, ThreadLoadingSpinnerDelayMilliseconds);
        } else {
            revealFrame = requestAnimationFrame(() => {
                if (transitionGeneration.current !== generation) return;
                displayedPresentation.current = presentationKey;
                setSpinnerProjection("");
                forceCardState(value => value + 1);
            });
        }
        return () => {
            if (spinnerTimer !== undefined) clearTimeout(spinnerTimer);
            if (revealFrame !== undefined) cancelAnimationFrame(revealFrame);
        };
    }, [presentationKey, snapshot.selectedThreadLoading, threadTransitionActive]);
    const [presentation, setPresentation] = useState(storedConversationPresentation);
    const draftGeneration = session.composerVisualKey();
    const [authored, setAuthored] = useState({generation: draftGeneration, text: ""});
    const prompt = authored.generation === draftGeneration ? authored.text : "";
    const setPrompt = (text: string) => setAuthored({generation: session.composerVisualKey(), text});
    const attention = [...session.model.pendingRequestPresentations().values()][0];
    const settingsDrafts = session.settingDrafts;
    const [settingsStateRevision, forceSettingsState] = useState(0);
    const settingsIdentity = presentationKey;
    const catalogSource = session.model.turnSettingsCatalogs();
    const settingsCatalog = useMemo(() => turnSettingCatalog(catalogSource),
        [catalogSource.models, catalogSource.permissionProfiles]);
    const canonicalSettings = useMemo(() => thread?.raw ?? {}, [thread?.raw]);
    const [settingsDraft, settingsProjection, settingsOptions] = useMemo(() => {
        const draft = settingDraftFor(settingsDrafts, settingsIdentity, canonicalSettings,
            settingsCatalog, thread?.settingStamps);
        return [draft, settingPresentation(draft, settingsCatalog), settingPromptOptions(draft, settingsCatalog)];
    }, [canonicalSettings, settingsCatalog, settingsDrafts, settingsIdentity, settingsStateRevision,
        thread?.settingStamps]);
    useBrowserLayoutEffect(() => {
        const element = scroll.current;
        if (!element) return;
        const saved = viewport.scroll(projectionId, presentationKey);
        const switchedPresentation = previousPresentation.current !== presentationKey;
        const transaction = pendingGeometry.current?.presentationKey === presentationKey ? pendingGeometry.current : undefined;
        if (transaction?.first) {
            const first = turnOpenings(conversation.sections)[0];
            if (first) { transaction.anchor = {cardKey: stableKey(first.key), pixelOffset: 0}; }
        }
        if (switchedPresentation) {
            previousPresentation.current = presentationKey;
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
        } else {
            restoreConversationAnchor(element, transaction?.anchor ?? saved.anchor, saved.scrollTop);
        }
        const following = transaction?.fold ? false : transaction?.following ?? saved.following;
        const anchor = conversationAnchor(element, following || transaction?.fold ? undefined : transaction?.anchor ?? saved.anchor);
        viewport.updateScroll(projectionId, presentationKey, element.scrollTop, following, anchor);
        const focusOwned = pane.current?.contains(document.activeElement) && !composerDock.current?.contains(document.activeElement);
        if (transaction?.focus && (!transaction.first || focusOwned)) {
            const target = anchor ? cardForKey(element, anchor.cardKey) : undefined;
            (following ? pane.current : target ?? pane.current)?.focus({preventScroll: true});
        }
        if (transaction) {
            pendingGeometry.current = transaction.first && (thread?.historyHasMore || session.historyLoading(projectionId, presentationKey))
                ? {...transaction, focus: Boolean(transaction.focus && focusOwned)} : undefined;
        }
    }, [snapshot.revision, projectionId, presentationKey, cardStateRevision, presentation, viewport]);
    useBrowserLayoutEffect(() => {
        const dock = composerDock.current; const owner = pane.current;
        if (!dock || !owner || typeof ResizeObserver === "undefined") return;
        let previousHeight = 0, previousWidth = dock.clientWidth;
        const observer = new ResizeObserver(() => {
            if (session.threadVisualKey(projectionId) !== presentationKey) return;
            const width = dock.clientWidth; if (width !== previousWidth) { const editor = dock.querySelector("textarea"); if (editor) resizePromptEditor(editor); previousWidth = width; }
            const element = scroll.current; const height = Math.ceil(owner.getBoundingClientRect().bottom - dock.getBoundingClientRect().top);
            if (height === previousHeight) return;
            const saved = viewport.scroll(projectionId, presentationKey);
            const anchor = element ? conversationAnchor(element, saved.anchor) ?? saved.anchor : saved.anchor;
            owner.style.setProperty("--composer-overlay-height", `${height}px`);
            if (element && previousHeight !== 0) {
                if (height > previousHeight && saved.following) element.scrollTop = element.scrollHeight;
                else restoreConversationAnchor(element, anchor, saved.scrollTop);
                viewport.updateScroll(projectionId, presentationKey, element.scrollTop, saved.following,
                    conversationAnchor(element, anchor));
            }
            previousHeight = height;
        });
        observer.observe(dock);
        return () => observer.disconnect();
    }, [projectionId, presentationKey, viewport]);
    const pauseForRelayout = () => {
        if (!scroll.current) return;
        const anchor = conversationAnchor(scroll.current);
        pendingGeometry.current = {presentationKey, following: false, anchor};
        viewport.updateScroll(projectionId, presentationKey, scroll.current.scrollTop, false, anchor);
    };
    const updatePresentation = (change: Partial<ConversationPresentationOptions>) => setPresentation(current => {
        if (change.showReasoning !== undefined || change.showCodexUpdates !== undefined) pauseForRelayout();
        const next = {...current, ...change}; persistConversationPresentation(next); return next;
    });
    const cardVisible = (card: VisibleCardData) => {
        if (card.kind === "reasoning") return presentation.showReasoning;
        if (card.kind !== "agentMessage") return true;
        return ["final_answer", "partial_answer"].includes((card.payload as AgentMessageData).phase) || presentation.showCodexUpdates;
    };
    const cardCollapsed = (card: VisibleCardData, key: string) => {
        const initiallyCollapsed = (card.kind === "commandExecution" && !presentation.commandsInitiallyExpanded)
            || (card.kind === "imageGeneration" && !presentation.imagesInitiallyExpanded)
            || (card.kind === "reasoning" && Boolean((card.payload as ReasoningData).summary))
            || (card.kind === "fileChanges" && !presentation.fileChangesInitiallyExpanded)
            || ["agentActivity", "plan", "genericActivity"].includes(card.kind);
        return viewport.cardCollapsed(projectionId, presentationKey, key, initiallyCollapsed);
    };
    const toggleCard = (key: string, collapsed: boolean) => {
        const element = scroll.current; const card = element ? cardForKey(element, key) : undefined;
        const header = card?.querySelector<HTMLElement>(":scope > header");
        if (element && header) {
            const previousTitleTop = header.getBoundingClientRect().top - element.getBoundingClientRect().top;
            const anchor = conversationAnchor(element);
            pendingGeometry.current = {presentationKey, following: false, anchor,
                fold: {cardKey: key, collapsed: !collapsed, previousTitleTop}};
            viewport.updateScroll(projectionId, presentationKey, element.scrollTop, false, anchor);
        }
        if (viewport.setCardCollapsed(projectionId, presentationKey, key, !collapsed))
            forceCardState(value => value + 1);
    };
    const copyCard = (content: CardCopyContent) => writeCardClipboard(content);
    const visibleSections = conversation.sections
        .map(section => ({...section, cards: section.cards.filter(cardVisible)}))
        .filter(section => section.cards.length > 0);
    const historyPagePending = session.historyPagePending(projectionId, presentationKey);
    const historyLoading = session.historyLoading(projectionId, presentationKey);
    const navigation = viewport.scroll(projectionId, presentationKey);
    const openingCards = turnOpenings(visibleSections);
    const anchoredCard = visibleSections.flatMap(section => section.cards)
        .find(card => stableKey(card.key) === navigation.anchor?.cardKey);
    const currentOpening = openingCards.find(card => card.turnId === anchoredCard?.turnId)
        ?? (anchoredCard?.turnId === "" ? openingCards.at(-1) : openingCards[0]);
    const atOpening = currentOpening !== undefined && navigation.anchor?.cardKey === stableKey(currentOpening.key)
        && (Math.abs(navigation.anchor.pixelOffset) <= 1 || (navigation.anchor.pixelOffset >= 0
            && Boolean(scroll.current && scroll.current.scrollHeight - scroll.current.scrollTop - scroll.current.clientHeight <= 1)));
    const navigationEnabled = !threadTransitionActive && currentOpening !== undefined;
    const atFirstOpening = currentOpening === openingCards[0] && anchoredCard === currentOpening && (navigation.anchor?.pixelOffset ?? -Infinity) >= -1;
    const loadEarlier = () => {
        if (!historyPagePending) {
            viewport.loadMore(projectionId, presentationKey);
            if (conversation.hiddenAuthoritativeItemCount <= AuthoritativeHistoryPageSize) {
                session.loadMoreHistory(projectionId, presentationKey);
            }
            forceCardState(value => value + 1);
        }
    };
    const loadedOpenings = () => turnOpenings(session.conversation(authoritativeCount).sections
        .map(section => ({...section, cards: section.cards.filter(cardVisible)})));
    const navigateTo = (card?: VisibleCardData, first = false) => {
        if (card && navigationEnabled && session.threadVisualKey(projectionId) === presentationKey) {
            const key = stableKey(card.key);
            const loaded = session.conversation(authoritativeCount);
            const target = turnOpenings(loaded.sections).find(opening => stableKey(opening.key) === key);
            if (target) {
                const position = loaded.sections.flatMap(section => section.cards).findIndex(item => stableKey(item.key) === key);
                let requested = limit;
                while (requested < authoritativeCount - position) { requested = viewport.loadMore(projectionId, presentationKey); }
                const anchor = {cardKey: key, pixelOffset: 0};
                pendingGeometry.current = {presentationKey, following: false, anchor, focus: true, first};
                viewport.updateScroll(projectionId, presentationKey, scroll.current?.scrollTop ?? 0, false, anchor);
                if (first) {
                    pane.current?.focus({preventScroll: true});
                    session.loadMoreHistory(projectionId, presentationKey, true);
                }
                forceCardState(value => value + 1);
            }
        }
    };
    const navigateAdjacent = (direction: number) => {
        navigateTo(turnNavigationTarget(loadedOpenings(), currentOpening?.turnId ?? "", atOpening, direction));
    };

    const renderCard = (card: VisibleCardData, nested?: ReactNode, turnContainer = false, nestedCard = false) => {
        const key = stableKey(card.key); const collapsed = cardCollapsed(card, key);
        return <Card key={`${presentationKey}:${key}`} card={card} active={conversation.activeTurnId === card.turnId} collapsed={collapsed} onToggle={() => toggleCard(key, collapsed)} onCopy={copyCard} times={card.key.kind === "item" ? targetTimes(session.model, {threadId: card.threadId, turnId: card.turnId, itemId: card.itemId}) : []}
            onTurnMenu={(position, opener) => {
                const turn = session.model.thread(card.threadId)?.turns.get(card.turnId);
                if (turn) { setTurnRemoval({threadId: card.threadId, turnId: card.turnId, presentationKey, turn, position, stage: "menu", cardKey: key, opener}); }
            }}
            onTiming={() => onInspect("timing", {threadId: card.threadId, turnId: card.turnId, ...(card.itemId ? {itemId: card.itemId} : {})})}
            onRestore={() => { if (card.kind === "localPrompt") { const text = session.restorePrompt(projectionId, (card.payload as LocalPromptData).submissionId, presentationKey, prompt); if (text !== undefined) { setPrompt(text); queueMicrotask(() => document.querySelector<HTMLTextAreaElement>("textarea[aria-label='Message Codex']")?.focus()); } } }}
            {...(card.kind === "commandExecution" ? {commandScroll: viewport.codeScroll(projectionId, presentationKey, `${key}:command`, false), outputScroll: viewport.codeScroll(projectionId, presentationKey, `${key}:output`, true)} : {})} nested={nested} turnContainer={turnContainer} nestedCard={nestedCard} />;
    };
    return <main ref={pane} className="conversation-pane" tabIndex={-1}>
        {turnRemoval && <TurnRemovalSurface session={session} target={turnRemoval} onClose={() => setTurnRemoval(undefined)} onConfirm={() => setTurnRemoval({...turnRemoval, stage: "confirm"})} />}
        <div className="conversation-heading"><div className="conversation-title"><span className="eyebrow">CONVERSATION</span>
            <div className="conversation-lockup"><div className="thread-heading-context"><h1>{thread?.title ?? (projectionId === DraftThreadId ? snapshot.newThreadDraft?.name || "New thread" : "Select a thread")}</h1>
                <button className="thread-workspace" disabled={!thread || session.workspaceChangeUnavailable(projectionId, presentationKey) !== ""} title={`${thread?.cwd || "Workspace not reported"}\n${session.workspaceChangeUnavailable(projectionId, presentationKey) || "Change workspace…"}`} aria-label={`Thread workspace: ${thread?.cwd || "Workspace not reported"}`} onClick={() => onRequestWorkspace({threadId: projectionId, presentationKey})}>{thread?.cwd || "Workspace not reported"}</button>
                <button className="thread-workspace-copy" aria-label="Copy thread workspace" disabled={!thread?.cwd} onClick={() => { if (thread?.cwd) { void writeCardClipboard({text: thread.cwd, markdown: false}); } }}><CopyIcon checked={false} /></button></div>
                {!thread && <p className="conversation-meta">{projectionId === DraftThreadId ? `${snapshot.newThreadDraft?.workspace ?? ""} | Send a message to create this thread.` : "Choose a thread from the left."}</p>}
                {thread && <span className={`conversation-state ${statusTone(thread.status)}`}>{displayStatus(thread.status)}</span>}
                <div className="conversation-facts"><TokenUsage content={usage} />
                {thread && <button className="thread-timing" onClick={() => onInspect("timing", {threadId: projectionId})} aria-label="Thread timing" title={timingDetails(protocolTimes(thread.raw, "Thread"))}>◷</button>}
                {thread?.lastActivityAt !== undefined && <p className="conversation-activity">{lastActivityText(thread.lastActivityAt)}</p>}</div></div></div>
            <div className={`conversation-heading-actions${paneControls ? " responsive" : ""}`}>
                {paneControls && <div className="responsive-pane-controls">{paneControls}</div>}
                <div className="conversation-view-controls" aria-label="Conversation presentation">
                    <div className="turn-navigation" aria-label="Turn navigation">
                        <button aria-label="First turn; load earlier history if needed" data-tooltip={historyPagePending ? "Loading earlier history…" : "First turn"} disabled={!navigationEnabled || historyLoading || (!conversation.hasMore && atFirstOpening)} onClick={() => navigateTo(loadedOpenings()[0], true)}>↥</button>
                        <button aria-label="Turn opening; previous turn when already there" data-tooltip="Turn opening / previous turn" disabled={!navigationEnabled || (!turnNavigationTarget(openingCards, currentOpening?.turnId ?? "", atOpening, -1) && conversation.hiddenAuthoritativeItemCount === 0)} onClick={() => navigateAdjacent(-1)}>↑</button>
                        <Menu key={`turns:${presentationKey}:${threadTransitionActive}`} className="turn-outline" label="Turns" disabled={!navigationEnabled}>{() => {
                            const openings = loadedOpenings();
                            return <><div className="turn-outline-count">{openings.length} loaded{thread?.historyHasMore ? " · earlier available" : ""}</div>
                                {conversation.hasMore && <button disabled={historyPagePending} onClick={loadEarlier}>{historyPagePending ? "Loading earlier turns…" : "Load earlier turns"}</button>}
                                {openings.map(card => <button key={stableKey(card.key)} aria-current={card.turnId === currentOpening?.turnId ? "true" : undefined}
                                    autoFocus={card.turnId === currentOpening?.turnId} onClick={() => navigateTo(card)} title={card.kind === "userMessage" ? (card.payload as UserMessageData).text : "Turn without an opening prompt"}>
                                    {card.kind === "userMessage" ? (card.payload as UserMessageData).text.trim() || "Attachment prompt" : "Turn without an opening prompt"}</button>)}</>;
                        }}</Menu>
                        <button aria-label="Next turn opening" data-tooltip="Next turn opening" disabled={!navigationEnabled || !turnNavigationTarget(openingCards, currentOpening?.turnId ?? "", atOpening, 1)} onClick={() => navigateAdjacent(1)}>↓</button>
                        <button className="conversation-latest" disabled={navigation.following || !navigationEnabled} aria-label="Latest output; resume following" data-tooltip="Latest output / resume following" onClick={() => {
                            pendingGeometry.current = {presentationKey, following: true, anchor: undefined, focus: true};
                            viewport.updateScroll(projectionId, presentationKey, scroll.current?.scrollTop ?? 0, true);
                            forceCardState(value => value + 1);
                        }}>↧</button>
                    </div>
                    <button className={presentation.showReasoning ? "active" : ""} aria-label={presentation.showReasoning ? "Hide reasoning cards" : "Show reasoning cards"} data-tooltip={presentation.showReasoning ? "Hide reasoning cards" : "Show reasoning cards"} aria-pressed={presentation.showReasoning} onClick={() => updatePresentation({showReasoning: !presentation.showReasoning})}><PresentationIcon kind="reasoning" /></button>
                    <button className={presentation.showCodexUpdates ? "active" : ""} aria-label={presentation.showCodexUpdates ? "Hide Codex update cards" : "Show Codex update cards"} data-tooltip={presentation.showCodexUpdates ? "Hide Codex update cards" : "Show Codex update cards"} aria-pressed={presentation.showCodexUpdates} onClick={() => updatePresentation({showCodexUpdates: !presentation.showCodexUpdates})}><PresentationIcon kind="updates" /></button>
                    <button className={presentation.commandsInitiallyExpanded ? "active" : ""} aria-label={presentation.commandsInitiallyExpanded ? "New command cards start expanded" : "New command cards start collapsed"} data-tooltip={presentation.commandsInitiallyExpanded ? "New command cards start expanded" : "New command cards start collapsed"} aria-pressed={presentation.commandsInitiallyExpanded} onClick={() => updatePresentation({commandsInitiallyExpanded: !presentation.commandsInitiallyExpanded})}><PresentationIcon kind="command" /></button>
                    <button className={presentation.fileChangesInitiallyExpanded ? "active" : ""} data-tooltip={presentation.fileChangesInitiallyExpanded ? "New file changes cards start expanded" : "New file changes cards start collapsed"} aria-label={presentation.fileChangesInitiallyExpanded ? "New file changes cards start expanded" : "New file changes cards start collapsed"} aria-pressed={presentation.fileChangesInitiallyExpanded} onClick={() => updatePresentation({fileChangesInitiallyExpanded: !presentation.fileChangesInitiallyExpanded})}><PresentationIcon kind="files" /></button>
                    <button className={presentation.imagesInitiallyExpanded ? "active" : ""} aria-label={presentation.imagesInitiallyExpanded ? "New image cards start expanded" : "New image cards start collapsed"} data-tooltip={presentation.imagesInitiallyExpanded ? "New image cards start expanded" : "New image cards start collapsed"} aria-pressed={presentation.imagesInitiallyExpanded} onClick={() => updatePresentation({imagesInitiallyExpanded: !presentation.imagesInitiallyExpanded})}><PresentationIcon kind="image" /></button>
                </div>
            </div>
        </div>
        <div className={`conversation-scroll${threadTransitionActive ? " thread-loading" : ""}`} ref={scroll}
            aria-busy={threadTransitionActive || undefined} onScroll={event => {
            const element = event.currentTarget;
            const previous = viewport.scroll(projectionId, presentationKey);
            // Restores and explicit jumps record their achieved position before
            // asynchronous scroll events arrive; those events do not own intent.
            if (Math.abs(previous.scrollTop - element.scrollTop) > 0.5) {
                pendingGeometry.current = undefined;
                const following = element.scrollHeight - element.scrollTop - element.clientHeight <= 1;
                const anchor = conversationAnchor(element);
                viewport.updateScroll(projectionId, presentationKey, element.scrollTop, following, anchor);
                if (previous.following !== following || previous.anchor?.cardKey !== anchor?.cardKey
                    || ((previous.anchor?.pixelOffset ?? -Infinity) >= -1) !== ((anchor?.pixelOffset ?? -Infinity) >= -1)
                    || (Math.abs(previous.anchor?.pixelOffset ?? Infinity) <= 1) !== (Math.abs(anchor?.pixelOffset ?? Infinity) <= 1)) {
                    forceCardState(value => value + 1);
                }
            }
        }}>
            {threadTransitionActive
                ? <ThreadLoadingSurface spinning={spinnerProjection === presentationKey} />
                : <>{conversation.hasMore && <button className="load-more" disabled={historyPagePending}
                    aria-busy={historyPagePending || undefined} onClick={loadEarlier}>{historyPagePending ? "Loading earlier activity…" : "Load earlier activity"}</button>}
                    {visibleSections.length === 0 && <div className="empty-state"><h3>Conversation activity appears here</h3></div>}
                    {visibleSections.map(section => {
                        const rootKey = section.rootCardKey ? stableKey(section.rootCardKey) : "";
                        const prompt = rootKey === "" ? undefined : section.cards.find(card => stableKey(card.key) === rootKey);
                        const nestedCards = prompt ? section.cards.filter(card => card !== prompt) : [];
                        const nested = nestedCards.length > 0 ? nestedCards.map(card => renderCard(card, undefined, false, true)) : undefined;
                        return <section key={`${presentationKey}:${rootKey || section.key}`} className="turn-section">
                            {prompt ? renderCard(prompt, nested, true) : section.cards.map(card => renderCard(card))}
                        </section>;
                    })}</>}
        </div>
        <div ref={composerDock} className="composer-dock">
            {attention && <div className="attention-strip" role="status"><span>A Codex request needs attention · {humanize(attention.kind)}</span>
                <button onClick={() => onInspect("requests")}>Review {session.model.pendingRequestPresentations().size} request(s)</button>
                {pendingDecisionOptions(attention).filter(decision => !decision.requiresInput && decision.value !== "submit").slice(0, 2).map(decision => <button key={decision.value} disabled={!session.canResolvePending(attention)} onClick={() => session.resolvePending(attention, {choice: decision.value})}>{decision.label}</button>)}
            </div>}
            <SettingsPanel key={`settings:${settingsIdentity}`} draft={settingsDraft} presentation={settingsProjection}
                onChange={(field, value) => {
                if (session.threadVisualKey(projectionId) !== settingsIdentity) return;
                changeSettingDraft(settingsDrafts, settingsIdentity, canonicalSettings, settingsCatalog,
                    settingsDraft.settingStamps, field, value);
                forceSettingsState(revision => revision + 1);
            }} />
            <Composer key={`shared-composer:${draftGeneration}`} session={session} presentationKey={presentationKey}
                active={Boolean(thread || projectionId === DraftThreadId) && session.canSubmit()} options={settingsOptions} prompt={prompt} onChange={setPrompt} />
        </div>
    </main>;
}

const SettingsPanel = memo(function SettingsPanel({draft, presentation, onChange}: {draft: SettingDraft;
    presentation: SettingPresentation; onChange: (field: SettingField, value: string) => void}) {
    const {values, touched} = draft;
    const select = (label: string, field: SettingField, choices: SettingPresentation["models"], enabled = true, tooltip = "") => <label title={tooltip || undefined}><span>{label}</span><select value={values[field]} disabled={!enabled} onChange={event => onChange(field, event.target.value)}>{choices.map(choice => <option key={choice.value} value={choice.value} disabled={choice.disabled} title={choice.description || undefined}>{choice.label}</option>)}</select></label>;
    return <div className="settings-panel" onWheel={event => event.preventDefault()}><div className="settings-grid">
            {select("Model", "model", presentation.models)}
            {select("Reasoning", "effort", presentation.efforts)}
            {select("Access", "sandbox", fixedSettingChoices("sandbox", values.sandbox))}
            {select("Network", "network", fixedSettingChoices("network", values.network), presentation.networkDisabledReason === "", presentation.networkDisabledReason)}
            {select("Collaboration mode", "collaboration", fixedSettingChoices("collaboration", values.collaboration))}
            {select("Approval", "approval", fixedSettingChoices("approval", values.approval))}
            {select("Style", "personality", fixedSettingChoices("personality", values.personality), presentation.personalityDisabledReason === "", presentation.personalityDisabledReason)}
            <div className="additional-setting"><span>Additional</span><Menu className="additional-settings" label={`More${touched.size > 0 ? ` · ${touched.size} changed` : ""}`}><div>
            {select("Approval reviewer", "reviewer", fixedSettingChoices("reviewer", values.reviewer))}
            {select("Permission profile", "permissionProfile", presentation.permissionProfiles)}
            {select("Service tier", "serviceTier", presentation.serviceTiers)}
            {select("Reasoning summary", "summary", fixedSettingChoices("summary", values.summary))}
            </div></Menu></div>
        </div>
    </div>;
}, (previous, current) => previous.draft === current.draft && previous.presentation === current.presentation);

function Menu({label, className, children, disabled = false}: {label: string; className: string; children: ReactNode | (() => ReactNode); disabled?: boolean}) {
    const id = useId();
    const popover = useRef<HTMLDivElement>(null);
    const [open, setOpen] = useState(false);
    useEffect(() => {
        const element = popover.current;
        const toggle = () => setOpen(element?.matches(":popover-open") ?? false);
        element?.addEventListener("toggle", toggle);
        return () => element?.removeEventListener("toggle", toggle);
    }, []);
    useEffect(() => { if (disabled) { popover.current?.hidePopover(); } }, [disabled]);
    return <div className={className}><button type="button" disabled={disabled} {...{popovertarget: id}}>{label}</button><div ref={popover} id={id} className="menu-popover" {...{popover: "auto"}} role="dialog" aria-label={label} onWheel={event => event.stopPropagation()} onClick={event => { if ((event.target as HTMLElement).closest("button")) { event.currentTarget.hidePopover(); } }}
        onKeyDown={event => {
            if (typeof children === "function" && ["ArrowUp", "ArrowDown", "Home", "End"].includes(event.key)) {
                const buttons = [...event.currentTarget.querySelectorAll<HTMLButtonElement>("button:not(:disabled)")];
                const current = buttons.indexOf(document.activeElement as HTMLButtonElement);
                const next = event.key === "Home" ? 0 : event.key === "End" ? buttons.length - 1 : current + (event.key === "ArrowUp" ? -1 : 1);
                buttons[Math.max(0, Math.min(buttons.length - 1, next))]?.focus();
                event.preventDefault();
            }
        }}>{typeof children === "function" ? open && children() : children}</div></div>;
}

function resizePromptEditor(element: HTMLTextAreaElement) {
    element.form?.classList.remove("expanded");
    element.style.height = "auto";
    element.form?.classList.toggle("expanded", element.scrollHeight > 32);
    const maximum = Math.ceil(parseFloat(getComputedStyle(element).lineHeight) * 20 + 10);
    element.style.maxHeight = `${maximum}px`;
    element.style.height = `${Math.min(element.scrollHeight, maximum)}px`;
    element.style.overflowY = element.scrollHeight > maximum ? "auto" : "hidden";
    if (element.scrollHeight <= maximum) element.scrollTop = 0;
}

function Composer({session, presentationKey, active, options, prompt, onChange}: {prompt: string; onChange: (text: string) => void; session: BrowserFrontendSession;
    presentationKey: string; active: boolean; options: ReturnType<typeof settingPromptOptions>}) {
    const setPrompt = onChange;
    const editor = useRef<HTMLTextAreaElement>(null);
    const running = session.model.activeTurnId(session.getSnapshot().selectedThreadId) !== undefined;
    useBrowserLayoutEffect(() => {
        if (editor.current) resizePromptEditor(editor.current);
    }, [prompt]);
    const submit = (event: FormEvent) => {
        event.preventDefault();
        if (!active || prompt.trim() === "" || !session.canSubmit(presentationKey)) return;
        const value = prompt;
        void session.submitPrompt(value, [], options.turn, options.thread, presentationKey);
        setPrompt("");
    };
    return <form className="composer" onSubmit={submit}>
        <textarea ref={editor} spellCheck={true} value={prompt} onChange={event => setPrompt(event.target.value)}
            aria-label="Message Codex" aria-describedby="composer-keyboard-hint" aria-keyshortcuts="Enter Control+Enter Meta+Enter"
            onKeyDown={event => { const composing = event.nativeEvent.isComposing || event.nativeEvent.keyCode === 229; if (shouldSubmitPromptFromKey({
                key: event.key, altKey: event.altKey, ctrlKey: event.ctrlKey, metaKey: event.metaKey,
                shiftKey: event.shiftKey, repeat: event.repeat,
                isComposing: composing,
            })) { event.preventDefault(); event.currentTarget.form?.requestSubmit(); }
            else if (event.key === "Enter" && event.repeat && !event.shiftKey && !composing) event.preventDefault(); }}
            onWheel={event => event.stopPropagation()}
            placeholder="Message Codex" rows={1} />
        <div className="composer-actions"><span id="composer-keyboard-hint" className="visually-hidden">Enter to send · Shift+Enter for a new line</span>
            <span className="composer-submit-actions"><button type="submit" className={`send-button${running ? " steer" : ""}`} disabled={!active || prompt.trim() === ""}>{running ? "Steer" : "Send"}</button>
                {running && <button type="button" className="stop-button" onClick={() => session.interrupt(presentationKey)}>Stop</button>}</span></div>
    </form>;
}

export function inspectorPlainState(selected: ThreadPresentation | undefined, enabled: boolean) {
    if (!enabled || !selected) return null;
    const changedPaths = new Set<string>();
    const turns = selected.turnOrder.map(id => {
        const turn = selected.turns.get(id)!;
        return {id, status: statusToken(turn.status), plan: turn.plan, items: turn.itemOrder.map(itemId => {
            const raw = turn.items.get(itemId)?.raw;
            if (stringMember(raw, "type") === "fileChange" && Array.isArray(raw?.changes))
                for (const change of raw.changes) { const path = stringMember(change, "path"); if (path) changedPaths.add(path); }
            return raw;
        })};
    });
    return {
        id: selected.id, title: selected.title, cwd: selected.cwd, status: statusToken(selected.status), archived: selected.archived,
        turns,
        agents: selected.agentOrder.map(id => { const agent = selected.agents.get(id)!; return {...agent, status: statusToken(agent.status)}; }),
        changedFileCount: changedPaths.size,
    };
}

function inspectorAgentCopyText(id: string, agent: AgentPresentation): string {
    const values = ["Agent"];
    const append = (label: string, value: unknown) => {
        if (typeof value === "string" && value !== "") values.push(`${label}: ${value}`);
    };
    append("Path", agent.raw.agentPath);
    append("Status", displayStatus(agent.status));
    append("Tool", agent.raw.tool);
    append("Model", agent.raw.model);
    append("Reasoning", agent.raw.reasoningEffort);
    append("Prompt", agent.raw.prompt);
    append("Result", agent.raw.resultText);
    append("Thread", agent.childThreadId);
    append("Sender", agent.raw.senderThreadId);
    const receivers = agent.raw.receiverThreadIds;
    if (Array.isArray(receivers) && receivers.length > 0)
        values.push(`Receivers: ${receivers.map(String).join(", ")}`);
    if (values.length === 1) values.push(`ID: ${id}`);
    return values.join("\n");
}

export function InspectorAgentCard({id, agent, expanded = false, onToggle}: {id: string; agent: AgentPresentation; expanded?: boolean; onToggle?: () => void}) {
    const [copied, setCopied] = useState(false);
    const feedbackTimer = useRef<ReturnType<typeof setTimeout>>();
    useEffect(() => () => { if (feedbackTimer.current) clearTimeout(feedbackTimer.current); }, []);
    const copy = async () => {
        if (await writeCardClipboard({text: inspectorAgentCopyText(id, agent), markdown: true}) !== "copied") return;
        if (feedbackTimer.current) clearTimeout(feedbackTimer.current);
        setCopied(true);
        feedbackTimer.current = setTimeout(() => setCopied(false), 500);
    };
    const title = agent.raw.agentPath ? String(agent.raw.agentPath) : id;
    return <article className={`agent-card${expanded ? " expanded" : " collapsed"}`}>
        <header><strong>{title}</strong><span className="agent-card-actions"><span className={`status ${statusTone(agent.status)}`}>{displayStatus(agent.status)}</span>
            <button className={`agent-copy-button${copied ? " copied" : ""}`} onClick={() => void copy()} aria-label="Copy agent content"><CopyIcon checked={copied} /></button>
            <button className="agent-fold-button" onClick={onToggle} aria-label={expanded ? "Collapse agent" : "Expand agent"}><FoldIcon collapsed={!expanded} /></button></span></header>
        {expanded && <div className="agent-card-content">{agent.childThreadId && <small>Thread {agent.childThreadId}</small>}
            <small>{agentMetadata({tool: stringMember(agent.raw, "tool"), kind: "", prompt: "", resultText: "", receivers: Array.isArray(agent.raw.receiverThreadIds) ? agent.raw.receiverThreadIds.map(String) : [], model: stringMember(agent.raw, "model"), reasoningEffort: stringMember(agent.raw, "reasoningEffort"), childThreadId: "", agentPath: stringMember(agent.raw, "agentPath"), senderThreadId: stringMember(agent.raw, "senderThreadId")}, agent.status)}</small>
            {stringMember(agent.raw, "prompt") && <div className="card-text">{stringMember(agent.raw, "prompt")}</div>}
            {typeof agent.raw.resultText === "string" && <SafeMarkdown text={agent.raw.resultText} />}</div>}
    </article>;
}

interface Inspection {tab: "plan" | "agents" | "requests" | "info"; detail: "choices" | "state" | "protocol" | "timing"; target?: TimingTarget; identity?: string}
function Inspector({session, inspection, onInspect, drawer = false, paneRef, onClose, onHide}: {session: BrowserFrontendSession; inspection: Inspection; onInspect: (inspection: Inspection) => void} & DrawerPaneProps) {
    const [, forceAgentFold] = useState(0);
    const tab = inspection.tab === "info" ? inspection.detail : inspection.tab;
    const selectedThreadId = session.getSnapshot().selectedThreadId;
    const selected = session.model.thread(selectedThreadId);
    const selectedPresentationKey = session.threadVisualKey(selectedThreadId);
    const pendingRequests = session.model.pendingRequestPresentations();
    const requests = tab === "requests" ? [...pendingRequests.values()] : [];
    let latestPlan: PlanData | undefined;
    if (tab === "plan" && selected) for (let index = selected.turnOrder.length - 1; index >= 0 && !latestPlan; --index) {
        const turn = selected.turns.get(selected.turnOrder[index]!);
        if (turn) latestPlan = projectTurnPlan(turn, selected.status);
    }
    const plainState = inspectorPlainState(selected, tab === "state");
    return <aside ref={paneRef} className={`inspector-pane${drawer ? " responsive-drawer drawer-right" : ""}`} id={drawer ? "inspector-pane" : undefined}
        role={drawer ? "dialog" : undefined} aria-modal={drawer || undefined} aria-labelledby={drawer ? "inspector-pane-title" : undefined}>
        <div className="pane-heading"><div><h2 id={drawer ? "inspector-pane-title" : undefined}>Inspector</h2></div>
            {onHide && <button onClick={onHide} aria-label="Hide Inspector">Hide</button>}
            {drawer && <button type="button" className="drawer-close" data-drawer-close onClick={onClose} aria-label="Close Inspector drawer">×</button>}</div>
        <nav className="inspector-tabs">{(["plan", "agents", "requests", "info"] as const).map(value =>
            <button key={value} className={inspection.tab === value ? "active" : ""} onClick={() => onInspect({...inspection, tab: value})}>{humanize(value)}{value === "requests" && pendingRequests.size > 0 ? ` ${pendingRequests.size}` : ""}</button>)}</nav>
        <div className="inspector-content">
            {inspection.tab === "info" && (tab === "choices" ? <div className="info-choices">{(["state", "protocol", "timing"] as const).map(detail => <button key={detail} onClick={() => onInspect({...inspection, detail})}>{humanize(detail)}</button>)}</div>
                : <button className="subtle-button" onClick={() => onInspect({...inspection, detail: "choices"})}>‹ Info</button>)}
            {tab === "timing" && <><h3>Timing</h3><pre className="state-json">{inspection.target && inspection.identity !== session.threadVisualKey(inspection.target.threadId)
                ? "Object is no longer available."
                : timingDetails(targetTimes(session.model, inspection.target ?? {threadId: selectedThreadId}, true))}</pre><p className="muted-copy">Event timestamps are supplied by app-server, not inferred from browser receipt time.</p></>}
            {tab === "plan" && (!selected ? <p className="muted-copy">Select a thread to inspect its plan.</p> : latestPlan ? <div className="plan-view">
                {latestPlan.explanation && <p>{latestPlan.explanation}</p>}
                {latestPlan.legacyText && <SafeMarkdown text={latestPlan.legacyText} />}
                {latestPlan.steps.map((step, index) => <div key={index}><StatusDot tone={statusTone(step.status)} /><span>{step.text}</span><small>{displayStatus(step.status)}</small></div>)}
            </div> : <p className="muted-copy">No structured plan is available for this thread.</p>)}
            {tab === "agents" && (!selected || selected.agentOrder.length === 0 ? <p className="muted-copy">No correlated agents are present.</p> : selected.agentOrder.map(id => <InspectorAgentCard key={`${selectedPresentationKey}:${id}`} id={id} agent={selected.agents.get(id)!} expanded={!session.viewportState.cardCollapsed(selectedThreadId, selectedPresentationKey, `inspector:${id}`, true)}
                onToggle={() => { const key = `inspector:${id}`; session.viewportState.setCardCollapsed(selectedThreadId, selectedPresentationKey, key, !session.viewportState.cardCollapsed(selectedThreadId, selectedPresentationKey, key, true)); forceAgentFold(value => value + 1); }} />))}
            {tab === "requests" && (requests.length === 0 ? <p className="muted-copy">No pending approval or input requests.</p> : requests.map(request => <RequestCard key={request.presentationKey} request={request} session={session} />))}
            {tab === "state" && <>{selected && <div className="state-summary"><Info label="Thread" value={selected.id} /><Info label="Status" value={displayStatus(selected.status)} /><Info label="Workspace" value={selected.cwd} /><Info label="Turns" value={String(selected.turnOrder.length)} /><Info label="Changed files" value={String(plainState?.changedFileCount ?? 0)} /></div>}<pre className="state-json">{JSON.stringify(plainState, null, 2)}</pre></>}
            {tab === "protocol" && <div className="protocol-list">{[...session.getSnapshot().protocolFrames].reverse().map((frame, index) => <details key={index}><summary>{humanize(String((frame as Record<string, unknown>).type ?? (frame as Record<string, unknown>).action ?? "Frame"))}</summary><pre>{JSON.stringify(frame, null, 2)}</pre></details>)}</div>}
        </div>
    </aside>;
}
function TokenUsage({content}: {content: TokenUsageText}) {
    return <Menu className="token-usage" label={content.summary}><pre>{content.details}</pre></Menu>;
}

function Info({label, value}: {label: string; value: string}) { return <div className="info-row"><span>{label}</span><strong>{value || "—"}</strong></div>; }

function RequestCard({request, session}: {request: PendingRequestPresentation; session: BrowserFrontendSession}) {
    const raw = request.raw && typeof request.raw === "object" ? request.raw as Record<string, unknown> : {};
    const decisions = pendingDecisionOptions(request);
    const questions = decisions.some(decision => decision.value === "submit") && Array.isArray(raw.questions)
        ? raw.questions as Record<string, unknown>[] : [];
    const [answers, setAnswers] = useState<Record<string, string[]>>({});
    const [otherAnswers, setOtherAnswers] = useState<Record<string, string>>({});
    const [structured, setStructured] = useState("{}");
    const details = pendingRequestDetails(request);
    const resolving = session.isPendingResolving(request);
    const actionable = session.canResolvePending(request);
    const userInputValid = questions.length > 0 && questions.every(question => {
        const id = typeof question.id === "string" ? question.id : "";
        return id !== "" && ((answers[id]?.length ?? 0) > 0 || (otherAnswers[id]?.trim() ?? "") !== "");
    });
    let structuredInput: unknown = raw.requestedSchema === undefined ? null : {};
    let structuredValid = true;
    if (request.kind === "mcp-elicitation" && raw.requestedSchema !== undefined) {
        try { structuredInput = JSON.parse(structured); }
        catch { structuredValid = false; }
    }
    const submission = (decision: string) => {
        let input: unknown;
        if (request.kind === "user-input") input = Object.fromEntries(questions.map(question => {
            const id = typeof question.id === "string" ? question.id : "";
            const values = [...(answers[id] ?? [])];
            const other = otherAnswers[id]?.trim() ?? "";
            if (other !== "") values.push(other);
            return [id, {answers: values}];
        }));
        else if (request.kind === "mcp-elicitation") input = structuredInput;
        return {choice: decision, input};
    };
    const toggleAnswer = (id: string, value: string, checked: boolean) => setAnswers(current => ({...current,
        [id]: checked ? [...(current[id] ?? []), value] : (current[id] ?? []).filter(answer => answer !== value),
    }));
    return <div className="request-card" aria-busy={resolving || undefined}>
        <strong>{humanize(request.kind)}</strong>
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
        {request.kind === "mcp-elicitation" && decisions.some(decision => decision.value === "accept" && decision.requiresInput) && raw.requestedSchema !== undefined && <textarea value={structured} onChange={event => setStructured(event.target.value)} rows={5} aria-label="Structured MCP response" />}
        {request.kind === "mcp-elicitation" && raw.requestedSchema !== undefined && !structuredValid && <p className="request-validation">Enter valid JSON.</p>}
        <div className="request-actions">{resolving ? <span>Resolving…</span> : decisions.map(decision => {
            const inputValid = !decision.requiresInput || ((request.kind !== "user-input" || userInputValid)
                && (request.kind !== "mcp-elicitation" || structuredValid));
            return <button type="button" key={decision.value} className={`request-button ${decision.tone === "neutral" ? "" : decision.tone}`}
                disabled={!actionable || !inputValid}
                onClick={() => session.resolvePending(request, submission(decision.value))}>{decision.label}</button>;
        })}</div>
    </div>;
}

export function App({session}: {session: BrowserFrontendSession}) {
    const snapshot = useSyncExternalStore(session.subscribe, session.getSnapshot, session.getSnapshot);
    const usage = tokenUsage(session.model, snapshot.selectedThreadId);
    const connection = session.model.connection();
    const role = session.role();
    const connectionTone = connection.connected ? "success" : connection.retrying ? "warning" : "muted";
    const globalStatus = connection.connected
        ? `${humanize(connection.providerState || "connected")} · ${humanize(role || "observer")}`
        : connection.retrying ? "Connecting" : "Offline";
    const [url, setUrl] = useState(snapshot.bridgeUrl);
    const canControl = role === "controller";
    const [inspection, setInspection] = useState<Inspection>({tab: "plan", detail: "choices"});
    const [panes, setPanes] = useState({threads: true, inspector: true, left: 282, right: 404});
    const responsiveMode = useResponsiveMode();
    const [drawer, setDrawer] = useState<"threads" | "inspector" | null>(null);
    const [newThreadDialog, setNewThreadDialog] = useState(false);
    const [groupDraft, setGroupDraft] = useState<NewThreadDraft>();
    const [groupDialog, setGroupDialog] = useState<{kind: "project" | "section"; group?: WorkspaceGroup; readOnly: boolean}>();
    const [optionsTarget, setOptionsTarget] = useState<(ThreadPresentationTarget & {purpose: "fork" | "workspace"}) | null>(null);
    const shell = useRef<HTMLDivElement>(null);
    const resizeStart = useRef<{x: number; width: number; side: "left" | "right"}>();
    const threadTrigger = useRef<HTMLButtonElement>(null);
    const inspectorTrigger = useRef<HTMLButtonElement>(null);
    const threadDrawer = useRef<HTMLElement>(null);
    const inspectorDrawer = useRef<HTMLElement>(null);
    const pendingFocusReturn = useRef<HTMLElement | null>(null);
    const threadsOverlay = responsiveMode === "mobile";
    const inspectorOverlay = responsiveMode !== "desktop";
    const activeDrawer = drawer === "threads" && threadsOverlay || drawer === "inspector" && inspectorOverlay ? drawer : null;
    const activeOptionsTarget = optionsTarget
        && session.threadVisualKey(optionsTarget.threadId) === optionsTarget.presentationKey
        ? optionsTarget : null;
    const modalOpen = Boolean(activeDrawer || newThreadDialog || activeOptionsTarget);
    const closeDrawer = () => setDrawer(null);
    const requestNewThread = (draft?: NewThreadDraft) => { setGroupDraft(draft); closeDrawer(); setNewThreadDialog(true); };
    const requestGroup = (kind: "project" | "section", group?: WorkspaceGroup, readOnly = false) => { closeDrawer(); setGroupDialog({kind, ...(group ? {group} : {}), readOnly}); };
    const requestForkWithOptions = (target: ThreadPresentationTarget) => {
        closeDrawer(); setOptionsTarget({...target, purpose: "fork"});
    };
    const requestWorkspace = (target: ThreadPresentationTarget) => {
        closeDrawer(); setOptionsTarget({...target, purpose: "workspace"});
    };
    const createNewThreadDraft = (draft: NewThreadDraft) => {
        session.beginNewThread(draft); setNewThreadDialog(false); closeDrawer();
    };
    const forkThreadWithOptions = (draft: NewThreadDraft) => {
        if (activeOptionsTarget?.purpose === "workspace") {
            session.changeWorkspace(activeOptionsTarget.threadId, draft.workspace, activeOptionsTarget.presentationKey);
        } else if (activeOptionsTarget) {
            session.forkThread(activeOptionsTarget.threadId, draft, activeOptionsTarget.presentationKey);
        }
        setOptionsTarget(null); closeDrawer();
    };
    useEffect(() => setDrawer(null), [responsiveMode]);
    useEffect(() => {
        if (optionsTarget && !activeOptionsTarget) setOptionsTarget(null);
    }, [optionsTarget, activeOptionsTarget]);
    useBrowserLayoutEffect(() => {
        if (modalOpen) for (const popover of shell.current?.querySelectorAll<HTMLElement>(":popover-open") ?? []) popover.hidePopover();
        for (const region of shell.current?.querySelectorAll<HTMLElement>("[data-modal-background]") ?? [])
            region.toggleAttribute("inert", modalOpen);
        if (!modalOpen && pendingFocusReturn.current) {
            const target = pendingFocusReturn.current;
            pendingFocusReturn.current = null;
            if (target.isConnected) target.focus();
            else document.querySelector<HTMLElement>(".conversation-pane")?.focus();
        }
    }, [modalOpen]);
    useBrowserLayoutEffect(() => {
        if (!activeDrawer || typeof document === "undefined") return;
        const pane = activeDrawer === "threads" ? threadDrawer.current : inspectorDrawer.current;
        const trigger = activeDrawer === "threads" ? threadTrigger.current : inspectorTrigger.current;
        if (!pane) return;
        const focusableSelector = "a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex=\"-1\"])";
        const focusables = () => [...pane.querySelectorAll<HTMLElement>(focusableSelector)].filter(element => element.getClientRects().length > 0);
        (pane.querySelector<HTMLElement>("[data-drawer-close]") ?? pane).focus();
        const onKeyDown = (event: KeyboardEvent) => {
            if (event.key === "Escape" && !document.querySelector(":popover-open")) { event.preventDefault(); closeDrawer(); return; }
            if (event.key !== "Tab") return;
            const available = focusables();
            if (available.length === 0) { event.preventDefault(); pane.focus(); return; }
            const first = available[0]!; const last = available[available.length - 1]!;
            if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
            else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
        };
        document.addEventListener("keydown", onKeyDown);
        return () => {
            document.removeEventListener("keydown", onKeyDown);
            pendingFocusReturn.current = trigger;
        };
    }, [activeDrawer]);
    const inspect = (view: "requests" | "timing", target?: TimingTarget) => {
        setInspection(view === "requests" ? {tab: "requests", detail: "choices"}
            : {tab: "info", detail: "timing", ...(target ? {target, identity: session.threadVisualKey(target.threadId)} : {})});
        setPanes(current => ({...current, inspector: true}));
        if (inspectorOverlay) setDrawer("inspector");
    };
    const resizePane = (side: "left" | "right", width: number) => {
        const other = side === "left" ? "right" : "left", visible = side === "left" ? panes.inspector : panes.threads;
        const budget = (shell.current?.clientWidth ?? window.innerWidth) - 420 - 8 * (Number(panes.threads) + Number(panes.inspector));
        const next = Math.max(180, Math.min(600, width, budget - (visible ? 180 : 0)));
        const opposite = shell.current?.querySelector<HTMLElement>(other === "left" ? ".thread-pane" : ".inspector-pane")?.getBoundingClientRect().width ?? panes[other];
        setPanes(current => ({...current, [side]: next, [other]: visible ? Math.min(opposite, budget - next) : current[other]}));
    };
    const separator = (side: "left" | "right") => <div className="pane-separator" role="separator" tabIndex={0} aria-orientation="vertical" aria-label={side === "left" ? "Resize Threads" : "Resize Inspector"}
        aria-valuemin={180} aria-valuemax={600} aria-valuenow={panes[side]}
        onKeyDown={event => { if (["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) {
            event.preventDefault(); const width = event.currentTarget.previousElementSibling?.getBoundingClientRect().width ?? panes[side];
            const actual = side === "left" ? width : shell.current?.querySelector(".inspector-pane")?.getBoundingClientRect().width ?? panes.right;
            resizePane(side, event.key === "Home" ? 180 : event.key === "End" ? 600 : actual + (event.key === "ArrowRight" ? 10 : -10) * (side === "left" ? 1 : -1));
        } }}
        onPointerDown={event => { event.currentTarget.setPointerCapture(event.pointerId); resizeStart.current = {x: event.clientX, width: shell.current?.querySelector(side === "left" ? ".thread-pane" : ".inspector-pane")?.getBoundingClientRect().width ?? panes[side], side}; }}
        onPointerMove={event => { const start = resizeStart.current; if (start?.side === side) resizePane(side, start.width + (event.clientX - start.x) * (side === "left" ? 1 : -1)); }}
        onPointerUp={() => { resizeStart.current = undefined; }} onLostPointerCapture={() => { resizeStart.current = undefined; }} />;
    const paneControls = <>
        {threadsOverlay && <button ref={threadTrigger} type="button" className="responsive-pane-button" aria-haspopup="dialog" aria-controls="thread-pane" aria-expanded={activeDrawer === "threads"} onClick={() => setDrawer("threads")}><span aria-hidden="true">☰</span> Threads</button>}
        {inspectorOverlay && <button ref={inspectorTrigger} type="button" className="responsive-pane-button" aria-haspopup="dialog" aria-controls="inspector-pane" aria-expanded={activeDrawer === "inspector"} onClick={() => setDrawer("inspector")}><span aria-hidden="true">ⓘ</span> Inspector</button>}
    </>;
    return <div ref={shell} className={`app-shell${activeDrawer ? " drawer-visible" : ""}`}>
        <header className="top-bar" data-modal-background aria-hidden={modalOpen || undefined}><div className="brand"><img className="brand-mark" src={new URL("../brand.svg", import.meta.url).href} alt="" /><b>CodexUI</b><small>Codex agent workspace</small></div>
            <div className="top-actions">
                {responsiveMode === "desktop" && !panes.threads && <button onClick={() => setPanes(current => ({...current, threads: true}))}>Show threads</button>}
                {responsiveMode === "desktop" && !panes.inspector && <button onClick={() => setPanes(current => ({...current, inspector: true}))}>Show inspector</button>}
                {session.model.pendingRequestPresentations().size > 0 && <button className="requests-indicator" onClick={() => inspect("requests")}>{session.model.pendingRequestPresentations().size} requests</button>}
                {connection.connected && <button className="subtle-button" onClick={() => canControl ? session.releaseController() : session.claimController()}>{canControl ? "Release control" : "Claim control"}</button>}
                <Menu className="connection-menu" label="Connection ⌄"><label className="connection-control"><input value={url} onChange={event => setUrl(event.target.value)} aria-label="Bridge WebSocket URL" />
                    <button onClick={() => connection.connected || connection.retrying ? session.disconnect() : session.connect(url)}>{connection.connected ? "Disconnect" : "Connect"}</button><StatusDot tone={connectionTone} /></label></Menu>
            </div>
        </header>
        {snapshot.notice && <div className="notice-banner" data-modal-background role="alert" aria-hidden={modalOpen || undefined}><span>{snapshot.notice}</span><button onClick={() => session.dismissNotice()} aria-label="Dismiss notice">×</button></div>}
        <div className="workspace-grid" style={responsiveMode === "desktop" ? {gridTemplateColumns: `${panes.threads ? `minmax(180px, ${panes.left}px) 8px ` : ""}minmax(420px, 1fr)${panes.inspector ? ` 8px minmax(180px, ${panes.right}px)` : ""}`} : undefined} data-modal-background aria-hidden={modalOpen || undefined}>
            {!threadsOverlay && (responsiveMode !== "desktop" || panes.threads) && <ThreadPane {...(responsiveMode === "desktop" ? {onHide: () => setPanes(current => ({...current, threads: false}))} : {})} session={session} onRequestNewThread={requestNewThread} onRequestGroup={requestGroup} onRequestForkWithOptions={requestForkWithOptions} onRequestWorkspace={requestWorkspace} />}
            {responsiveMode === "desktop" && panes.threads && separator("left")}
            <Conversation onRequestWorkspace={requestWorkspace} usage={usage.thread} onInspect={inspect} session={session} paneControls={responsiveMode === "desktop" ? undefined : paneControls} />
            {responsiveMode === "desktop" && panes.inspector && separator("right")}
            {!inspectorOverlay && panes.inspector && <Inspector onHide={() => setPanes(current => ({...current, inspector: false}))} session={session} inspection={inspection} onInspect={setInspection} />}
        </div>
        <footer className="status-bar" data-modal-background aria-hidden={modalOpen || undefined}><div><strong>© Volker Christian &amp; Codex</strong><span> | </span>
            <a href="https://github.com/SNodeC/CodexUI">CodexUI</a><span> • </span><a href="https://github.com/SNodeC/AISuite">AISuite</a><span> • </span>
            <small>Powered by</small> <a href="https://github.com/SNodeC/snode.c">SNode.C</a></div>
            <TokenUsage content={usage.overall} />
            <div className="global-status"><span>Status:</span><StatusDot tone={connectionTone} /><strong>{globalStatus}</strong></div></footer>
        {activeDrawer && <button type="button" className="drawer-backdrop" tabIndex={-1} onClick={closeDrawer} aria-label={`Close ${activeDrawer === "threads" ? "Threads" : "Inspector"} drawer`} />}
        {activeDrawer === "threads" && <ThreadPane session={session} onRequestNewThread={requestNewThread} onRequestGroup={requestGroup} onRequestForkWithOptions={requestForkWithOptions} onRequestWorkspace={requestWorkspace} drawer paneRef={threadDrawer} onClose={closeDrawer} />}
        {activeDrawer === "inspector" && <Inspector session={session} inspection={inspection} onInspect={setInspection} drawer paneRef={inspectorDrawer} onClose={closeDrawer} />}
        {groupDialog && <GroupDialog session={session} {...groupDialog} onClose={() => setGroupDialog(current => current === groupDialog ? undefined : current)} />}
        {newThreadDialog && <NewThreadDialog {...(groupDraft ? {initialDraft: groupDraft} : {})} initialWorkspace="" workspaceChoices={(() => { const roots = member(session.model.projects.get(groupDraft?.projectId ?? "")?.raw, "roots", []); return Array.isArray(roots) ? roots.map(root => stringMember(root, "path")) : []; })()} onCancel={() => setNewThreadDialog(false)} onContinue={createNewThreadDraft} />}
        {activeOptionsTarget && <NewThreadDialog purpose={activeOptionsTarget.purpose} initialWorkspace={session.model.thread(activeOptionsTarget.threadId)?.cwd ?? ""} initialDraft={activeOptionsTarget.purpose === "fork" ? session.forkDraft(activeOptionsTarget.threadId) : {workspace: session.model.thread(activeOptionsTarget.threadId)?.cwd ?? "", name: session.model.thread(activeOptionsTarget.threadId)?.title ?? "", baseInstructions: "", developerInstructions: "", ephemeral: false}} onCancel={() => setOptionsTarget(null)} onContinue={forkThreadWithOptions} />}
    </div>;
}
