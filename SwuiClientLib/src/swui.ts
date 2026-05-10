// ── Debug Overlay ─────────────────────────────────────────────────────────

let _swuiDebugOverlay: HTMLDivElement | null = null;
let _swuiDebugOverlayTimeout: number | undefined;

function showSwuiDebugOverlay(tag: string, transport: string) {
  if (!_swuiDebugOverlay) {
    _swuiDebugOverlay = document.createElement('div');
    _swuiDebugOverlay.id = 'swui-debug-overlay';
    Object.assign(_swuiDebugOverlay.style, {
      position: 'fixed',
      top: '12px',
      right: '12px',
      zIndex: 99999,
      background: 'rgba(0,0,0,0.85)',
      color: '#fff',
      fontFamily: 'monospace',
      fontSize: '14px',
      padding: '8px 16px',
      borderRadius: '8px',
      boxShadow: '0 2px 8px rgba(0,0,0,0.25)',
      pointerEvents: 'none',
      transition: 'opacity 0.3s',
      opacity: '1',
      maxWidth: '90vw',
    });
    document.body.appendChild(_swuiDebugOverlay);
  }
  _swuiDebugOverlay.textContent = `[SWUI NAV] tag: ${tag}  transport: ${transport}`;
  _swuiDebugOverlay.style.opacity = '1';
  if (_swuiDebugOverlayTimeout) window.clearTimeout(_swuiDebugOverlayTimeout);
  _swuiDebugOverlayTimeout = window.setTimeout(() => {
    if (_swuiDebugOverlay) _swuiDebugOverlay.style.opacity = '0.25';
  }, 2000);
}
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
 *   Inbound:
 *     Swui.onEvent("swui.menu.open", fn)
 *     SwuiNavigationEvents.onMenuOpen(fn)
 *     Swui.onNavigate(fn)
 *     Swui.onConfirm(fn)
 *     Swui.onCancel(fn)
 *     Swui.onNextTab(fn)
 *     Swui.onPreviousTab(fn)
 *   Outbound:
 *     Swui.emitNavigationEvent("swui.menu.close")
 *     Swui.emitNavigationEvent("swui.menu.quit")
 *   Swui.onPointerMove(fn)         — pointer movement
 *   Swui.onPointerPress(fn)        — pointer button pressed
 *   Swui.onPointerRelease(fn)      — pointer button released
 *   Swui.onPointerWheel(fn)        — pointer wheel delta
 *   Swui.onKeyDown(fn)             — keyboard key pressed
 *   Swui.onKeyUp(fn)               — keyboard key released
 *   Swui.onTextInput(fn)           — text character input
 */

// ---------------------------------------------------------------------------
// SWUI JS→UE Native Message Bus (Navigation Only)
//
// Outbound messages from JS/React HUD to Unreal Engine use:
//   Swui.emitNavigationEvent(tag, payload?)
// which wraps:
//   Swui.postMessage({ type: "navigation", tag, payload: payload ?? {}, source: "js" })
//
// Transport:
//   - Uses window.cefQuery if available (preferred for CEF/Unreal)
//   - Falls back to window.__SWUI__?.postMessage if available
//   - Otherwise, safely no-ops (never throws)
//
// Only type="navigation" is routed to UE for now. Tag is mapped to FGameplayTag.
// Blueprint API and React HUD usage remain unchanged.
// ---------------------------------------------------------------------------

// ── Internal types ──────────────────────────────────────────────────────────

export type Unsubscribe = () => void;

type SwuiOutgoingMessage = {
  type: "navigation";
  tag: string;
  payload?: unknown;
};

type SwuiNativeMessage = {
  type: string;
  payload?: unknown;
  tag?: string;
  event?: string;
  id?: string;
  source?: 'js';
};
interface SwuiChromeWebview {
  postMessage: (message: unknown) => void;
}

interface SwuiCefQueryRequest {
  request: string;
  onSuccess?: (response: string) => void;
  onFailure?: (errorCode: number, errorMessage: string) => void;
}

interface SwuiRuntime {
  state:    Record<string, unknown>;
  _notify?: (key: string, value: unknown) => void;
  emitToUnreal?: (message: SwuiOutgoingMessage) => void;
  postMessage?: (message: SwuiOutgoingMessage) => void;
}

interface SwuiWindowExtensions {
  __SWUI__?: SwuiRuntime;
  cefQuery?: (request: SwuiCefQueryRequest) => void;
  chrome?: {
    webview?: SwuiChromeWebview;
  };
}

declare global {
  interface Window {
    cefQuery?: (args: SwuiCefQueryRequest) => void;
    __SWUI__?: SwuiRuntime & { postMessage?: (message: string) => void };
    [key: string]: unknown;
  }
}

function shouldLogSwuiClientDebug(): boolean {
  try {
    const dev = (typeof (globalThis as any).process !== 'undefined' && (globalThis as any).process?.env?.NODE_ENV === 'development');
    const metaDev = (typeof (globalThis as any).importMeta !== 'undefined') ? (globalThis as any).importMeta?.env?.DEV : false;
    const winFlag = typeof window !== 'undefined' && (window as any).__SWUI_DEBUG_NAV__;
    return Boolean(dev || metaDev || winFlag);
  } catch {
    return Boolean(typeof window !== 'undefined' && (window as any).__SWUI_DEBUG_NAV__);
  }
}

function safeStringify(value: unknown): string {
  try {
    return JSON.stringify(value);
  } catch {
    return JSON.stringify({
      type: 'error',
      payload: { message: 'Failed to serialize SWUI native message' },
      source: 'js',
    });
  }
}

function sendSwuiUrlBridge(raw: string, tag: string, debug: boolean): void {
  const encoded = encodeURIComponent(raw);
  const url = `swui://bus?payload=${encoded}&t=${Date.now()}`;

  let iframe = document.getElementById('__swui_native_bridge_iframe') as HTMLIFrameElement | null;

  if (!iframe) {
    iframe = document.createElement('iframe');
    iframe.id = '__swui_native_bridge_iframe';
    iframe.style.display = 'none';
    iframe.setAttribute('aria-hidden', 'true');
    document.documentElement.appendChild(iframe);
  }

  iframe.src = url;

  if ((window as any).__SWUI_DEBUG_NAV__) {
    showSwuiDebugOverlay(tag, 'urlBridge');
  }
}

function postMessage(message: SwuiNativeMessage): void {
  if (typeof window === 'undefined') return;

  const raw = safeStringify(message);
  const debug = shouldLogSwuiClientDebug();

  try {
    let transport = 'none';
    const msgTag = (message as any).tag || '';
    if (typeof window.cefQuery === 'function') {
      transport = 'cefQuery';
      if (debug) console.log('[SWUI CLIENT] postMessage transport=cefQuery', message);
      window.cefQuery({
        request: raw,
        onSuccess: () => { },
        onFailure: (code: number, err: string) => {
          if (debug) console.warn('[SWUI CLIENT] cefQuery failed', code, err, message);
        },
      });
      if ((window as any).__SWUI_DEBUG_NAV__ && message.type === 'navigation') {
        showSwuiDebugOverlay(msgTag, transport);
      }
      return;
    }

    if (typeof window.__SWUI__?.postMessage === 'function') {
      transport = '__SWUI__.postMessage';
      if (debug) console.log('[SWUI CLIENT] postMessage transport=__SWUI__.postMessage', message);
      try { (window.__SWUI__ as any).postMessage(raw); } catch (e) { if (debug) console.warn('[SWUI CLIENT] __SWUI__.postMessage failed', e); }
      if ((window as any).__SWUI_DEBUG_NAV__ && message.type === 'navigation') {
        showSwuiDebugOverlay(msgTag, transport);
      }
      return;
    }

    const webview = (window as any).chrome?.webview;
    if (webview && typeof webview.postMessage === 'function') {
      transport = 'chrome.webview.postMessage';
      if (debug) console.log('[SWUI CLIENT] postMessage transport=chrome.webview.postMessage', message);
      try { webview.postMessage(raw); } catch (e) { if (debug) console.warn('[SWUI CLIENT] chrome.webview.postMessage failed', e); }
      if ((window as any).__SWUI_DEBUG_NAV__ && message.type === 'navigation') {
        showSwuiDebugOverlay(msgTag, transport);
      }
      return;
    }

    if (debug) console.log('[SWUI CLIENT] postMessage transport=urlBridge', message);
    sendSwuiUrlBridge(raw, msgTag, debug);
  } catch (error) {
    console.error('[SWUI CLIENT] postMessage failed', error, message);
  }
}

// ── Subscriber map ──────────────────────────────────────────────────────────

const _subs: Record<string, Array<(v: unknown) => void>> = {};
let _patched = false;
let _warnedMissingOutboundBridge = false;
let _warnedOutboundSendFailure = false;

function _getWindow(): SwuiWindowExtensions {
  return window as unknown as SwuiWindowExtensions;
}

function _getRuntime(): SwuiRuntime | undefined {
  return _getWindow().__SWUI__;
}

function _warnMissingOutboundBridge(message: SwuiOutgoingMessage): void {
  if (_warnedMissingOutboundBridge) return;
  _warnedMissingOutboundBridge = true;
  console.warn("[SWUI] No outbound native bridge is available; dropped message.", message);
}

function _warnOutboundSendFailure(message: SwuiOutgoingMessage, error: unknown): void {
  if (_warnedOutboundSendFailure) return;
  _warnedOutboundSendFailure = true;
  console.warn("[SWUI] Failed to send outbound message to Unreal.", message, error);
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

function emitNavigationEvent(tagName: string, payload: unknown = {}): void {
  if (shouldLogSwuiClientDebug()) console.log('[SWUI CLIENT] emitNavigationEvent tag=', tagName, 'payload=', payload);
  postMessage({ type: 'navigation', tag: tagName, payload, source: 'js' });
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
  postMessage,
  emitNavigationEvent,
  // Pointer
  onPointerMove, onPointerPress, onPointerRelease, onPointerWheel,
  // Keyboard
  onKeyDown, onKeyUp, onTextInput,
};
export default Swui;
export { postMessage, emitNavigationEvent };
