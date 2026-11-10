import {useEffect, useLayoutEffect, useRef, useState} from "react";
import type {ReactNode} from "react";

export interface TreeEntry {
    level: number;
    expanded: boolean | undefined;
    element: () => HTMLElement | undefined;
    focus: () => void;
    toggle: () => void;
}
export const useBrowserLayoutEffect = typeof window === "undefined" ? useEffect : useLayoutEffect;
export const windowTreeEntries = new WeakMap<HTMLElement, () => TreeEntry[]>();

// Only scalar geometry survives recycling; the caller owns data and interaction state.
export function ScrollWindow<T>({items, itemKey, renderItem, tree}: {
    items: readonly T[]; itemKey: (item: T) => string; renderItem: (item: T) => ReactNode;
    tree?: (item: T) => Pick<TreeEntry, "level" | "expanded" | "toggle">;
}) {
    const host = useRef<HTMLDivElement>(null), heights = useRef(new Map<string, number>());
    const pendingFocus = useRef<{key: string; last: boolean}>();
    const anchor = useRef<{key: string; offset: number}>();
    const [viewport, setViewport] = useState({top: 0, bottom: 1000, pinned: "", revision: 0});
    const keys = items.map(itemKey), offsets = [0];
    for (const key of keys) { offsets.push(offsets[offsets.length - 1]! + (heights.current.get(key) ?? 40)); }
    useBrowserLayoutEffect(() => {
        const element = host.current!;
        let scroller = element.parentElement!;
        while (scroller.parentElement && !/auto|scroll/.test(getComputedStyle(scroller).overflowY)) { scroller = scroller.parentElement; }
        if (anchor.current && keys.includes(anchor.current.key)) {
            scroller.scrollTop += offsets[keys.indexOf(anchor.current.key)]! - anchor.current.offset
                - (scroller.getBoundingClientRect().top + scroller.clientTop - element.getBoundingClientRect().top);
        }
        const retained = new Set(keys);
        for (const key of heights.current.keys()) { if (!retained.has(key)) { heights.current.delete(key); } }
        const row = (key: string) => [...element.children].find(child => (child as HTMLElement).dataset.windowKey === key) as HTMLElement | undefined;
        const button = (key: string) => row(key)?.querySelector<HTMLElement>(tree ? ".thread-row" : "button:not(:disabled)") ?? undefined;
        const update = () => {
            const top = scroller.getBoundingClientRect().top + scroller.clientTop - element.getBoundingClientRect().top;
            let changed = false;
            for (const child of element.children) {
                const key = (child as HTMLElement).dataset.windowKey;
                if (key !== undefined) {
                    const height = child.getBoundingClientRect().height, previous = heights.current.get(key) ?? 40;
                    if (Math.abs(height - previous) > 0.25) {
                        heights.current.set(key, height); changed = true;
                    }
                }
            }
            const index = offsets.findIndex(value => value > top) - 1;
            anchor.current = index >= 0 && keys[index] !== undefined ? {key: keys[index]!, offset: offsets[index]! - top} : undefined;
            const focused = document.activeElement?.closest<HTMLElement>("[data-window-key]");
            const selection = document.getSelection();
            const selected = selection && !selection.isCollapsed ? selection.anchorNode?.parentElement?.closest<HTMLElement>("[data-window-key]") : undefined;
            const selectionEnd = !selection?.isCollapsed ? selection?.focusNode?.parentElement?.closest<HTMLElement>("[data-window-key]") : undefined;
            const menu = element.querySelector<HTMLElement>('[aria-haspopup="menu"][aria-expanded="true"]')?.closest<HTMLElement>("[data-window-key]");
            const pinned = [focused, selected, selectionEnd, menu].filter(node => node && element.contains(node)).map(node => node!.dataset.windowKey!).join("\0");
            if (viewport.top !== top || viewport.bottom !== top + scroller.clientHeight || viewport.pinned !== pinned || changed) {
                setViewport(old => ({top, bottom: top + scroller.clientHeight, pinned, revision: old.revision + 1}));
            }
        };
        const focus = (key: string, last = false) => {
            pendingFocus.current = {key, last};
            scroller.scrollTop += offsets[keys.indexOf(key)]! + element.getBoundingClientRect().top - scroller.getBoundingClientRect().top;
            update();
        };
        if (tree) { windowTreeEntries.set(element, () => items.map((item, index) => ({...tree(item), element: () => button(keys[index]!), focus: () => focus(keys[index]!)}))); }
        // Pending-row animation follows the browser timeline across recycling.
        for (const animation of element.getAnimations({subtree: true})) {
            if (animation instanceof CSSAnimation && animation.animationName === "awaiting") { animation.startTime = 0; }
        }
        const observer = new ResizeObserver(update);
        observer.observe(scroller);
        for (const child of element.children) { if ((child as HTMLElement).dataset.windowKey !== undefined) { observer.observe(child); } }
        window.addEventListener("scroll", update, true);
        document.addEventListener("focusin", update);
        document.addEventListener("focusout", update);
        document.addEventListener("selectionchange", update);
        update();
        const pending = pendingFocus.current;
        if (pending && row(pending.key)) {
            const buttons = row(pending.key)!.querySelectorAll<HTMLElement>("button:not(:disabled)");
            (pending.last ? buttons[buttons.length - 1] : button(pending.key))?.focus(); pendingFocus.current = undefined;
        }
        const tab = (event: KeyboardEvent) => {
            const current = (event.target as HTMLElement).closest<HTMLElement>("[data-window-key]");
            const buttons = current?.querySelectorAll("button:not(:disabled)");
            const index = keys.indexOf(current?.dataset.windowKey ?? "") + (event.shiftKey ? -1 : 1);
            if (event.key === "Tab" && buttons && event.target === buttons[event.shiftKey ? 0 : buttons.length - 1] && keys[index] !== undefined) {
                event.preventDefault(); focus(keys[index]!, event.shiftKey);
            }
        };
        element.addEventListener("keydown", tab);
        return () => {
            observer.disconnect(); windowTreeEntries.delete(element); element.removeEventListener("keydown", tab);
            window.removeEventListener("scroll", update, true);
            document.removeEventListener("focusin", update); document.removeEventListener("focusout", update); document.removeEventListener("selectionchange", update);
        };
    });
    const children: ReactNode[] = [];
    const pinned = new Set(viewport.pinned.split("\0"));
    let end = 0;
    for (let index = 0; index < items.length; ++index) {
        if (offsets[index + 1]! >= viewport.top - 400 && offsets[index]! <= viewport.bottom + 400 || pinned.has(keys[index]!) || pendingFocus.current?.key === keys[index]) {
            if (offsets[index]! > end) { children.push(<div key={`gap:${keys[index]}`} aria-hidden="true" style={{height: offsets[index]! - end}} />); }
            children.push(<div key={keys[index]} data-window-key={keys[index]} role={tree ? undefined : "listitem"} aria-posinset={tree ? undefined : index + 1} aria-setsize={tree ? undefined : items.length} style={{display: "flow-root"}}>{renderItem(items[index]!)}</div>);
            end = offsets[index + 1]!;
        }
    }
    if (offsets[items.length]! > end) { children.push(<div key="tail" aria-hidden="true" style={{height: offsets[items.length]! - end}} />); }
    return <div ref={host} className="scroll-window" data-window-count={items.length} role={tree ? undefined : "list"} style={{overflowAnchor: "none"}}>{children}</div>;
}
