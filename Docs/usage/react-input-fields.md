# React input fields — using HTML form controls inside SWUI

## What you get

Standard HTML `<input>`, `<textarea>`, `<select>`, and contenteditable elements work inside the SWUI CEF runtime. React controlled components update on keystroke. No fake div inputs, no game-side text entry, no special SWUI API calls.

## How it works

| Layer | What happens |
|---|---|
| User types | Slate pump delivers `FKeyEvent` to `FSwuiInputPreprocessor::HandleKeyDownEvent` |
| Preprocessor | Calls `USwuiView::ForwardKeyEventToBrowser` |
| CEF bridge | `CefBrowserHost::SendKeyEvent(KEYDOWN)` + `SendKeyEvent(KEYEVENT_CHAR)` for printable keys |
| Browser | Dispatches to focused `<input>` element → React `onChange` fires |
| React state | `useState` updates, component re-renders with new value |

No SWUI-specific input handling required. The CEF browser handles keyboard layout mapping, IME, composition, and text insertion natively once it receives the raw events.

## Focus model

| Action | Behavior |
|---|---|
| Click into `<input>` | `SendMouseClickEvent` focuses the element → `focusin` fires → DOM focus bridge sets `bTextInputFocused = true` |
| Type while focused | CEF receives KEYDOWN + CHAR → React `onChange` fires → component re-renders |
| Click outside input | `focusout` fires → DOM focus bridge sets `bTextInputFocused = false` |
| Tab between fields | CEF handles Tab navigation when `focus_on_editable_field = true` |
| Escape in input | First Escape: React input blur handler fires. Second Escape: game handles navigation. |

## State ownership

**React owns temporary input state while the user is typing. Game/AngelScript owns committed game state.**

| Field | React state | Triggers on | Sent to UE as |
|---|---|---|---|
| Username | `useState('Player')` | Blur / Enter / Save button | `Swui.emitNavigationEvent("onev.online.setUsername", {username})` |
| Room name | `useState('')` | Host click | Part of `onev.rooms.host` payload |
| Map dropdown | `useState('whitebox')` | Host click | Part of `onev.rooms.host` payload |
| Public checkbox | `useState(true)` | Host click | Part of `onev.rooms.host` payload |
| Room search/filter | `useState('')` | Every keystroke | React-only, never sent to UE |

## Suppressing game input while typing

When an input field has DOM focus, `USwuiSubsystem::IsTextInputFocused()` returns `true`. The game checks this flag and suppresses movement/fire/look input while the user is typing:

```cpp
if (SwuiSubsystem->IsTextInputFocused())
{
    // Block WASD, spacebar, mouse look — don't route to player
    return;
}
```

The flag is updated every tick from `USwuiView::bTextInputFocused`, which is set by injected JS that watches `focusin`/`focusout` on editable elements.

## Example: React controlled input

```tsx
function UsernameInput() {
    const [username, setUsername] = useState('Player');

    const commit = () => {
        Swui.emitNavigationEvent('onev.online.setUsername', { username });
    };

    return (
        <div>
            <input
                value={username}
                onChange={e => setUsername(e.target.value)}
                onBlur={commit}
                onKeyDown={e => { if (e.key === 'Enter') commit(); }}
                placeholder="Username"
            />
        </div>
    );
}
```

## Important files

- `SwuiInputPreprocessor.cpp` — keyboard forwarding from Slate to CEF
- `SwuiView.cpp` — `ForwardKeyEventToBrowser`, `ForwardCharToBrowser`
- `SwuiSubsystem.cpp` — DOM focus bridge JS injection + `bTextInputFocused` sync
- `SwuiSubsystem.h` — `IsTextInputFocused()` UFUNCTION

## Debug checklist

- Text appears in the input as you type? If not, check `ForwardCharToBrowser` is called.
- Backspace deletes? CEF handles this via KEYDOWN with VK_BACK.
- React `onChange` fires? CEF sends input events to the focused element.
- Game input suppressed while typing? Check `IsTextInputFocused()`.
- Tab between fields works? Requires `focus_on_editable_field = true`.
- Paste works? Ctrl+V is CEF-level; verify `EVENTFLAG_CONTROL_DOWN` is set in modifiers.
