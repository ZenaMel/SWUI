/**
 * swui.ts — SimpleWebUI client runtime
 *
 * IIFE build  → window.Swui  (load via <script src="swui.js">)
 * ESM build   → import Swui from './swui.esm.js'
 *
 * API:
 *   Swui.on(key, fn)   — subscribe; fires immediately if value already in snapshot.
 *                        Returns an unsubscribe function.
 *   Swui.get(key)      — one-shot snapshot read.
 *   Swui.getAll()      — full state snapshot.
 */

// ── Internal types ──────────────────────────────────────────────────────────

export type Unsubscribe = () => void;

interface SwuiRuntime {
  state:    Record<string, unknown>;
  _notify?: (key: string, value: unknown) => void;
}

// ── Subscriber map ──────────────────────────────────────────────────────────

const _subs: Record<string, Array<(v: unknown) => void>> = {};
let _patched = false;

function _getRuntime(): SwuiRuntime | undefined {
  return (window as unknown as Record<string, unknown>).__SWUI__ as SwuiRuntime | undefined;
}

function _patch(): void {
  if (_patched) return;
  _patched = true;

  const rt  = (_getRuntime() as SwuiRuntime);
  const prev = rt._notify;

  rt._notify = (k: string, v: unknown) => {
    _subs[k]?.slice().forEach(fn => fn(v));
    prev?.(k, v);
  };
}

function _whenReady(cb: () => void): void {
  if (_getRuntime()) { _patch(); cb(); return; }
  const t = setInterval(() => {
    if (_getRuntime()) { clearInterval(t); _patch(); cb(); }
  }, 50);
}

// ── Public API ──────────────────────────────────────────────────────────────

/**
 * Subscribe to a state key. Fires immediately if the value is already in the
 * snapshot (i.e. Unreal ticked before the page loaded). Returns `unsubscribe`.
 */
function on<T = unknown>(key: string, fn: (value: T) => void): Unsubscribe {
  (_subs[key] ??= []).push(fn as (v: unknown) => void);

  _whenReady(() => {
    const snap = _getRuntime()?.state?.[key];
    if (snap !== undefined) fn(snap as T);
  });

  return () => {
    _subs[key] = (_subs[key] ?? []).filter(f => f !== fn);
  };
}

/** One-shot read of a key from the current snapshot. */
function get<T = unknown>(key: string): T | undefined {
  return _getRuntime()?.state?.[key] as T | undefined;
}

/** Full state snapshot. */
function getAll(): Record<string, unknown> {
  return _getRuntime()?.state ?? {};
}

// ── Export ──────────────────────────────────────────────────────────────────

const Swui = { on, get, getAll };
export default Swui;
