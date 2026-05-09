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
 * Navigation API (dot-separated event names match Gameplay Tags):
 *   Swui.onEvent(tagName, fn)      — any dot-named event (e.g. "swui.navigation.confirm")
 *   Swui.onNavigate(fn)            — directional navigation
 *   Swui.onConfirm(fn)             — confirm action
 *   Swui.onCancel(fn)              — cancel action
 *   Swui.onNextTab(fn)             — next tab
 *   Swui.onPreviousTab(fn)         — previous tab
 *   Swui.onPointerMove(fn)         — pointer movement
 *   Swui.onPointerPress(fn)        — pointer button pressed
 *   Swui.onPointerRelease(fn)      — pointer button released
 *   Swui.onPointerWheel(fn)        — pointer wheel delta
 *   Swui.onKeyDown(fn)             — keyboard key pressed
 *   Swui.onKeyUp(fn)               — keyboard key released
 *   Swui.onTextInput(fn)           — text character input
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

export interface SwuiNavigateEvent {
  direction: SwuiNavDirection;
}

export interface SwuiPointerMoveEvent {
  x: number;
  y: number;
}

export interface SwuiPointerButtonEvent {
  button: "Left" | "Right" | "Middle";
}

export interface SwuiWheelEvent {
  delta: number;
}

export interface SwuiKeyEvent {
  key: string;
}

export interface SwuiTextEvent {
  text: string;
}

// ── Navigation helpers (dot-separated event names) ──────────────────────────

/** Listen for a CustomEvent and return an unsubscribe function. */
function _listenEvent<T>(eventName: string, fn: (detail: T) => void): Unsubscribe {
  const handler = (e: Event) => fn((e as CustomEvent<T>).detail);
  window.addEventListener(eventName, handler);
  return () => window.removeEventListener(eventName, handler);
}

/** Subscribe to any dot-named navigation event by its full tag name. */
function onEvent<T = unknown>(tagName: string, fn: (detail: T) => void): Unsubscribe {
  return _listenEvent<T>(tagName, fn);
}

/** Subscribe to directional navigation. */
function onNavigate(fn: (event: SwuiNavigateEvent) => void): Unsubscribe {
  // Direction-specific tags all have {direction} in detail.
  // Listen to each direction tag individually.
  const unsubs = [
    _listenEvent<SwuiNavigateEvent>("swui.navigation.up", fn),
    _listenEvent<SwuiNavigateEvent>("swui.navigation.down", fn),
    _listenEvent<SwuiNavigateEvent>("swui.navigation.left", fn),
    _listenEvent<SwuiNavigateEvent>("swui.navigation.right", fn),
  ];
  return () => unsubs.forEach(u => u());
}

/** Subscribe to confirm action. */
function onConfirm(fn: (detail: unknown) => void): Unsubscribe {
  return _listenEvent("swui.navigation.confirm", fn);
}

/** Subscribe to cancel action. */
function onCancel(fn: (detail: unknown) => void): Unsubscribe {
  return _listenEvent("swui.navigation.cancel", fn);
}

/** Subscribe to next-tab action. */
function onNextTab(fn: (detail: unknown) => void): Unsubscribe {
  return _listenEvent("swui.navigation.nextTab", fn);
}

/** Subscribe to previous-tab action. */
function onPreviousTab(fn: (detail: unknown) => void): Unsubscribe {
  return _listenEvent("swui.navigation.previousTab", fn);
}

// ── Pointer helpers ─────────────────────────────────────────────────────────

function onPointerMove(fn: (event: SwuiPointerMoveEvent) => void): Unsubscribe {
  return _listenEvent<SwuiPointerMoveEvent>("swui.pointer.move", fn);
}

function onPointerPress(fn: (event: SwuiPointerButtonEvent) => void): Unsubscribe {
  const unsubs = [
    _listenEvent<SwuiPointerButtonEvent>("swui.pointer.leftDown", fn),
    _listenEvent<SwuiPointerButtonEvent>("swui.pointer.rightDown", fn),
    _listenEvent<SwuiPointerButtonEvent>("swui.pointer.middleDown", fn),
  ];
  return () => unsubs.forEach(u => u());
}

function onPointerRelease(fn: (event: SwuiPointerButtonEvent) => void): Unsubscribe {
  const unsubs = [
    _listenEvent<SwuiPointerButtonEvent>("swui.pointer.leftUp", fn),
    _listenEvent<SwuiPointerButtonEvent>("swui.pointer.rightUp", fn),
    _listenEvent<SwuiPointerButtonEvent>("swui.pointer.middleUp", fn),
  ];
  return () => unsubs.forEach(u => u());
}

function onPointerWheel(fn: (event: SwuiWheelEvent) => void): Unsubscribe {
  return _listenEvent<SwuiWheelEvent>("swui.pointer.wheel", fn);
}

// ── Keyboard helpers ────────────────────────────────────────────────────────

function onKeyDown(fn: (event: SwuiKeyEvent) => void): Unsubscribe {
  return _listenEvent<SwuiKeyEvent>("swui.keyboard.keyDown", fn);
}

function onKeyUp(fn: (event: SwuiKeyEvent) => void): Unsubscribe {
  return _listenEvent<SwuiKeyEvent>("swui.keyboard.keyUp", fn);
}

function onTextInput(fn: (event: SwuiTextEvent) => void): Unsubscribe {
  return _listenEvent<SwuiTextEvent>("swui.keyboard.textInput", fn);
}

// ── Export ──────────────────────────────────────────────────────────────────

const Swui = {
  // State
  on, get, getAll,
  // Navigation — subscribe
  onEvent, onNavigate, onConfirm, onCancel, onNextTab, onPreviousTab,
  // Pointer
  onPointerMove, onPointerPress, onPointerRelease, onPointerWheel,
  // Keyboard
  onKeyDown, onKeyUp, onTextInput,
};
export default Swui;
