/**
 * swui.ts — SimpleWebUI client runtime
 *
 * IIFE build  → window.Swui  (load via <script src="swui.js">)
 * ESM build   → import Swui from './swui.esm.js'
 *
 * State API:
 *   Swui.on(key, fn)   — subscribe; fires immediately if value already in snapshot.
 *                        Returns an unsubscribe function.
 *   Swui.get(key)      — one-shot snapshot read.
 *   Swui.getAll()      — full state snapshot.
 *
 * Navigation API:
 *   Swui.onNavigation(fn)          — generic navigation events
 *   Swui.onAction(name, fn)        — specific named action
 *   Swui.onNavigate(fn)            — directional navigation
 *   Swui.onConfirm(fn)             — confirm action
 *   Swui.onCancel(fn)              — cancel action
 *   Swui.onNextTab(fn)             — next tab
 *   Swui.onPreviousTab(fn)         — previous tab
 *   Swui.emitAction(name, payload?)— JS → Unreal action (if bridge exists)
 *   Swui.confirm()                 — emit confirm
 *   Swui.cancel()                  — emit cancel
 *   Swui.nextTab()                 — emit next tab
 *   Swui.previousTab()             — emit previous tab
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

// ── Navigation types ────────────────────────────────────────────────────────

export type SwuiNavDirection =
  | "Up"
  | "Down"
  | "Left"
  | "Right"
  | "Next"
  | "Previous";

export interface SwuiNavigationEvent {
  action: string;
  payload?: unknown;
  source?: string;
}

export interface SwuiNavigateEvent {
  direction: SwuiNavDirection;
  source?: string;
}

// ── Navigation helpers ──────────────────────────────────────────────────────

/** Listen for a CustomEvent and return an unsubscribe function. */
function _listenEvent<T>(eventName: string, fn: (detail: T) => void): Unsubscribe {
  const handler = (e: Event) => fn((e as CustomEvent<T>).detail);
  window.addEventListener(eventName, handler);
  return () => window.removeEventListener(eventName, handler);
}

/** Subscribe to all navigation events (generic `swui:navigation`). */
function onNavigation(fn: (event: SwuiNavigationEvent) => void): Unsubscribe {
  return _listenEvent<SwuiNavigationEvent>("swui:navigation", fn);
}

/** Subscribe to a specific named action from `swui:navigation`. */
function onAction(actionName: string, fn: (event: SwuiNavigationEvent) => void): Unsubscribe {
  return _listenEvent<SwuiNavigationEvent>("swui:navigation", (e) => {
    if (e.action === actionName) fn(e);
  });
}

/** Subscribe to directional navigation (`swui:navigate`). */
function onNavigate(fn: (event: SwuiNavigateEvent) => void): Unsubscribe {
  return _listenEvent<SwuiNavigateEvent>("swui:navigate", fn);
}

/** Subscribe to confirm action (`swui:confirm`). */
function onConfirm(fn: () => void): Unsubscribe {
  return _listenEvent("swui:confirm", fn);
}

/** Subscribe to cancel action (`swui:cancel`). */
function onCancel(fn: () => void): Unsubscribe {
  return _listenEvent("swui:cancel", fn);
}

/** Subscribe to next-tab action (`swui:next-tab`). */
function onNextTab(fn: () => void): Unsubscribe {
  return _listenEvent("swui:next-tab", fn);
}

/** Subscribe to previous-tab action (`swui:previous-tab`). */
function onPreviousTab(fn: () => void): Unsubscribe {
  return _listenEvent("swui:previous-tab", fn);
}

// ── Emit helpers (JS → Unreal, if a bridge is available) ────────────────────

/** Dispatch a CustomEvent that UE can observe (or other JS listeners). */
function _emitEvent(eventName: string, detail?: unknown): void {
  window.dispatchEvent(new CustomEvent(eventName, { detail }));
}

/** Emit a named action. */
function emitAction(actionName: string, payload?: unknown): void {
  _emitEvent("swui:navigation", { action: actionName, payload, source: "SwuiClientLib" });
}

/** Emit confirm. */
function confirm(): void { _emitEvent("swui:confirm", { source: "SwuiClientLib" }); }

/** Emit cancel. */
function cancel(): void { _emitEvent("swui:cancel", { source: "SwuiClientLib" }); }

/** Emit next-tab. */
function nextTab(): void { _emitEvent("swui:next-tab", { source: "SwuiClientLib" }); }

/** Emit previous-tab. */
function previousTab(): void { _emitEvent("swui:previous-tab", { source: "SwuiClientLib" }); }

// ── Export ──────────────────────────────────────────────────────────────────

const Swui = {
  // State
  on, get, getAll,
  // Navigation — subscribe
  onNavigation, onAction, onNavigate, onConfirm, onCancel, onNextTab, onPreviousTab,
  // Navigation — emit
  emitAction, confirm, cancel, nextTab, previousTab,
};
export default Swui;
