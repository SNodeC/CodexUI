export function humanizeProtocolLabel(value: string): string {
    if (value === "contextCompaction") return "Context compaction";
    if (value.toLocaleLowerCase() === "xhigh") return "Extra high";
    const result: string[] = [];
    let pendingSpace = false;
    const characters = [...value.trim()];
    for (let index = 0; index < characters.length; ++index) {
        let character = characters[index]!;
        if (/\s|[-_./]/u.test(character)) { pendingSpace = result.length > 0; continue; }
        const previous = characters[index - 1] ?? "";
        const next = characters[index + 1] ?? "";
        const boundary = /[A-Z]/u.test(character) && (/[a-z\d]/u.test(previous)
            || (/[A-Z]/u.test(previous) && /[a-z]/u.test(next)));
        if ((pendingSpace || boundary) && result.at(-1) !== " ") result.push(" ");
        if (boundary || (pendingSpace && /[A-Z]/u.test(character) && /[a-z]/u.test(next))) character = character.toLowerCase();
        result.push(character); pendingSpace = false;
    }
    if (result.length === 0) return "Activity";
    result[0] = result[0]!.toUpperCase();
    return result.join("");
}
