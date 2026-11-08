// Crash breadcrumbs + JS error capture, so a crash on a phone we can't attach a debugger
// to still leaves evidence: a rolling log of boot/network steps saved to disk (survives a
// NATIVE crash too - whatever was written last is the last step reached), and fatal JS
// errors are shown on screen (with "Copy details") instead of killing the app.
import { fileStore } from "./store";

const t0 = Date.now();
const MAX = 120;
let crumbs: string[] = [];
let previous: { crumbs: string[]; start: number; last: number } | null = null;
let fatal: string | null = null;
const listeners = new Set<() => void>();

export function crumb(step: string) {
  const line = `+${Date.now() - t0}ms ${step}`;
  crumbs.push(line);
  if (crumbs.length > MAX) crumbs = crumbs.slice(-MAX);
  fileStore.set("wb-crumbs", JSON.stringify({ start: t0, last: Date.now(), crumbs })).catch(() => {});
}

/** Call first thing: loads the previous session's log, then starts this one. */
export async function initCrashLog() {
  try {
    const raw = await fileStore.get("wb-crumbs");
    if (raw) previous = JSON.parse(raw);
  } catch { previous = null; }
  crumb("launch");
}

/** The previous session looks like a crash when it ended soon after starting. */
export function previousCrash(): { log: string; secs: number } | null {
  if (!previous || !previous.crumbs?.length) return null;
  const secs = Math.round((previous.last - previous.start) / 1000);
  if (secs > 45) return null;
  return { log: previous.crumbs.join("\n"), secs };
}
export const previousLog = () => (previous?.crumbs || []).join("\n");
export const currentLog = () => crumbs.join("\n");
export const fatalError = () => fatal;
export function onFatal(fn: () => void) { listeners.add(fn); return () => { listeners.delete(fn); }; }
export function reportFatal(e: any, where: string) {
  const msg = `${where}: ${e?.message || e}\n${String(e?.stack || "").split("\n").slice(0, 8).join("\n")}`;
  fatal = msg;
  crumb("FATAL " + msg.split("\n")[0]);
  for (const fn of listeners) { try { fn(); } catch { /* */ } }
}
export function clearFatal() { fatal = null; for (const fn of listeners) { try { fn(); } catch { /* */ } } }

/** Route uncaught JS errors here instead of letting the release build crash. */
export function installGlobalHandler() {
  const EU = (globalThis as any).ErrorUtils;
  if (!EU?.setGlobalHandler) return;
  const prev = EU.getGlobalHandler?.();
  EU.setGlobalHandler((e: any, isFatal?: boolean) => {
    try { reportFatal(e, isFatal ? "fatal" : "error"); } catch { /* */ }
    if (!isFatal && prev) { try { prev(e, isFatal); } catch { /* */ } }
  });
}
