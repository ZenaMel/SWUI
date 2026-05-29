# CEF input forwarding — making HTML form controls work inside SWUI

## What this solves

HTML `<input>`, `<textarea>`, `<select>`, and contenteditable elements receive no keyboard input inside the CEF offscreen browser because SWUI never called `CefBrowserHost::SendKeyEvent()`. Mouse clicks, pointer moves, and scroll wheel events were forwarded (via `SendMouseClickEvent`, `SendMouseMoveEvent`, `SendMouseWheelEvent`), but keyboard input was dispatched as JS `CustomEvent`s on the DOM — invisible to native HTML form controls.

## Mental model

```
UE Slate key event
 → FSwuiInputPreprocessor::HandleKeyDownEvent / HandleKeyUpEvent
 → USwuiView::ForwardKeyEventToBrowser
 → CefBrowserHost::SendKeyEvent(CefKeyEvent{KEYDOWN, windows_key_code, modifiers, ...})
 → For printable keys: ForwardCharToBrowser → CefBrowserHost::SendKeyEvent(KEYEVENT_CHAR)
 → CEF dispatches to focused DOM element
 → React onChange / normal HTML input.value update
```

The Slate preprocessor gates on the same condition as mouse forwarding (`IsPointerInputEnabled()`), so keyboard follows the same active/inactive boundary as the mouse cursor.

## Why SendKeyEvent was missing

The original `FSwuiInputPreprocessor` only overrode mouse-related `IInputProcessor` methods (`HandleMouseMoveEvent`, `HandleMouseButtonDownEvent`, etc.). The preprocessor interface also provides `HandleKeyDownEvent` and `HandleKeyUpEvent` from `IInputProcessor`, but these weren't implemented. CEF's offscreen browser host requires an explicit `SendKeyEvent` call for every keyboard action — the CEF process handles its own key mapping, IME, and text composition once it receives the raw events.

## Keyboard event mapping

| UE Slate event | CEF event type | CefKeyEvent fields |
|---|---|---|
| `HandleKeyDownEvent` | `KEYEVENT_KEYDOWN` | `windows_key_code` from `FKeyEvent::GetKeyCode()`, modifiers from `GetModifierKeys(): Shift / Ctrl / Alt / Cmd` |
| `HandleKeyUpEvent` | `KEYEVENT_KEYUP` | Same as KEYDOWN |
| Printable key down | `KEYEVENT_CHAR` (synthesized) | `windows_key_code` = `GetCharacter()`, `character` = `GetCharacter()`, `unmodified_character` = `GetCharacter()` |

The character event is synthesized from the key down because `IInputProcessor` doesn't expose `HandleKeyCharEvent` — only key down/up are available. This means the character event is generated alongside every printable key down, which is the standard CEF pattern (KEYDOWN + CHAR for each printable keystroke).

The `focus_on_editable_field` field is set from `USwuiView::bTextInputFocused` (populated by the DOM focus bridge — see text-input-focus.md). This tells CEF whether to handle IME, keyboard shortcuts, and Tab navigation normally.

## Focus state synchronization

The `focus_on_editable_field` flag on every `CefKeyEvent` is updated from the DOM focus bridge, which watches `focusin`/`focusout` events on editable elements via injected JS. This means:

- When an `<input>` is focused, `focus_on_editable_field = true` → CEF handles Ctrl+A/C/V, Tab, Enter normally
- When no editable element is focused, `focus_on_editable_field = false` → CEF treats keys as UI navigation

## Important files

- `SwuiInputPreprocessor.h/.cpp` — `HandleKeyDownEvent`, `HandleKeyUpEvent`, `ShouldForwardKeyboard`
- `SwuiView.h/.cpp` — `ForwardKeyEventToBrowser`, `ForwardCharToBrowser`

## How to use it

Once compiled, any HTML form element inside the SWUI CEF runtime receives keyboard input. React controlled components work normally:

```tsx
const [text, setText] = useState('');
return <input value={text} onChange={e => setText(e.target.value)} />;
```

No special SWUI API calls needed — the forwarding is transparent.

## Gotchas

- **`IInputProcessor` has no `HandleKeyCharEvent`**: The character / IME / composition event path is not available through the input processor. Characters are synthesized from `FKeyEvent::GetCharacter()` on key down. This works for standard Latin characters and shifted keys but may not handle complex IME composition correctly on all keyboard layouts.
- **Repeat keys**: `KeyEvent::IsRepeat()` is used to generate repeated KEYDOWN + CHAR events, which is consistent with CEF's expected input pattern.
- **Modifier-only keys**: Keys like Ctrl, Shift, Alt have `IsModifierKey() == true` and don't generate a CHAR event — only KEYDOWN/KEYUP.
- **Keyboard layout mapping**: `FKeyEvent::GetKeyCode()` returns the Windows virtual key code on Windows. On other platforms, key codes may differ and some keys may return 0.

## Debug checklist

- `ShouldForwardKeyboard()` returns true? (checks `IsPointerInputEnabled()`)
- `CefBrowserHost::SendKeyEvent` called for KEYDOWN? Set breakpoint in `ForwardKeyEventToBrowser`.
- CHAR events sent for printable keys? Check `ForwardCharToBrowser` is called when `GetCharacter() != 0`.
- `focus_on_editable_field` set correctly? Check `bTextInputFocused` from the DOM focus bridge.
- React controlled `<input>` receives typed characters and handles backspace/delete correctly?
