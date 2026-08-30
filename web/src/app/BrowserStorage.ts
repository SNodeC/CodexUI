export function readBrowserStorage(key: string): string | undefined {
    if (typeof window === "undefined") return undefined;
    try { return window.localStorage.getItem(key) ?? undefined; }
    catch { return undefined; }
}

export function writeBrowserStorage(key: string, value: string): void {
    if (typeof window === "undefined") return;
    try { window.localStorage.setItem(key, value); }
    catch { /* Browser storage is an optional convenience. */ }
}
