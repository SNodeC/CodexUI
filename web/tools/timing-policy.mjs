// CI accepts elapsed-time overruns, not correctness or bounded-work failures.
export function timingLimit(met, message) {
    if (met || process.env.CODEXUI_TIMING_POLICY !== "report") return met;
    console.warn(`${process.env.GITHUB_ACTIONS === "true" ? "::warning::" : "TIMING WARNING: "}${message}`);
    return true;
}
