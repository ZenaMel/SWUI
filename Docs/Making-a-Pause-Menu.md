# Tutorial: Build a SWUI Pause Menu with a Settings Subpage

This tutorial shows how to build a simple event-driven pause menu using SWUI.

By the end, you will have:

- A pause menu opened from game input such as `ESC`, `Start`, or any custom input action.
- A `Continue` button at the top.
- A `Settings` button that switches to a Settings subpage inside the same modal.
- A `Quit Game` button at the bottom.
- An `X` button in the top-right that closes the pause menu.
- `ESC` / Cancel behavior:
  - On the main pause page: closes the menu.
  - On the Settings page: returns to the main pause page.
- UE-native cursor and input mode handling through `SwuiNavigation`.
- JavaScript-side menu behavior through `SwuiClientLib`.

---

# Main Concepts

SWUI separates the system into three parts:

## 1. `USwui`

`USwui` owns the browser/UI view.

It handles:

- rendering the web UI
- loading the HTML/JS/CSS app
- Unreal ↔ JavaScript interop
- exposed values, functions, and events

Think of it as:

```text
The web UI surface
````

---

## 2. `USwuiNavigation`

`USwuiNavigation` owns menu/navigation input routing.

It handles:

* focusing a SWUI view
* switching input mode
* showing/hiding the UE cursor
* forwarding navigation actions to JavaScript
* exposing Blueprint callbacks for side effects
* handling actions such as Confirm, Cancel, NextTab, PreviousTab, and custom actions

Think of it as:

```text
The bridge between game input and web menu behavior
```

---

## 3. `SwuiClientLib`

`SwuiClientLib` is the JavaScript-side helper.

It lets the web app subscribe to events from Unreal:

```ts
Swui.onAction("OpenPause", ...)
Swui.onCancel(...)
Swui.onNextTab(...)
Swui.onPreviousTab(...)
```

It can also emit actions back to Unreal:

```ts
Swui.emitAction("ClosePause")
Swui.emitAction("QuitGame")
```

Think of it as:

```text
The JS runtime API for SWUI menus
```

---

# How the Pieces Work Together

The pause menu flow looks like this:

```text
Player presses ESC
→ Project input calls SwuiNavigation.TriggerAction("OpenPause")
→ SwuiNavigation focuses the Swui view and applies UI input mode
→ SwuiNavigation forwards "OpenPause" to JavaScript
→ JS opens PauseMenuModal
```

Closing the menu looks like this:

```text
Player presses ESC again or clicks Continue/X
→ JS closes PauseMenuModal
→ JS emits "ClosePause"
→ Unreal restores gameplay input and unpauses
```

Settings navigation looks like this:

```text
Player clicks Settings
→ JS switches the modal page to "settings"

Player presses ESC inside Settings
→ SwuiNavigation.Cancel()
→ JS handles cancel
→ JS returns to main pause page
```

---

# Step 1: Add SWUI Components in the Editor

Open your PlayerController Blueprint.

Add these components:

```text
Swui
SwuiNavigation
```

The PlayerController is a good place because menus are usually player-owned and input-driven.

---

# Step 2: Configure the `Swui` Component

Select the `Swui` component.

Configure it to load your HUD/menu app.

Example:

```text
URL: /hud/index.html
Rendering Mode: Auto
```

Use the same SWUI app that contains your HUD and menu code.

---

# Step 3: Configure the `SwuiNavigation` Component

Select the `SwuiNavigation` component.

Set:

```text
Focused Swui: your Swui component
Dispatch Order: BlueprintFirst
Show Cursor When Focused: true
Close On Escape: true
```

For the input mode, the pause menu should use:

```text
UiOnly
```

when the menu is open.

Gameplay HUD mode can use:

```text
HudOnly
```

when the menu is closed.

---

# Step 4: Create the Basic Open/Close Flow in Blueprint

Create a Blueprint event/function:

```text
OpenPauseMenu
```

Inside it:

```text
Set Game Paused = true
SwuiNavigation.SetFocusedSwui(Swui)
SwuiNavigation.SetInputMode(UiOnly)
SwuiNavigation.ApplyInputMode()
SwuiNavigation.TriggerAction("OpenPause")
```

Create another Blueprint event/function:

```text
ClosePauseMenu
```

Inside it:

```text
Set Game Paused = false
SwuiNavigation.ClearFocusedSwui()
SwuiNavigation.SetInputMode(HudOnly)
SwuiNavigation.ApplyInputMode()
```

Expose `ClosePauseMenu` to JavaScript through your existing SWUI interop pattern, or handle it from a Blueprint callback when JS emits `ClosePause`.

---

# Step 5: Bind Game Input to Navigation

Your project owns the input system.

With Enhanced Input, legacy input, or Blueprint input, bind your actions into `SwuiNavigation`.

Example:

```text
ESC / Start
→ if pause menu is closed:
     OpenPauseMenu
  if pause menu is open:
     SwuiNavigation.Cancel()
```

Menu navigation:

```text
D-pad / left stick
→ SwuiNavigation.Navigate(...)

Enter / A / Cross
→ SwuiNavigation.Confirm()

B / Circle
→ SwuiNavigation.Cancel()

Mouse move
→ SwuiNavigation.PointerMove(...)

Mouse click
→ SwuiNavigation.PointerPress(...)
→ SwuiNavigation.PointerRelease(...)

Mouse wheel
→ SwuiNavigation.PointerWheel(...)
```

At this stage, the Unreal side can open the menu and send navigation events.

---

# Step 6: Add the Pause Menu Component in JavaScript

Create a React component:

```tsx
PauseMenuModal.tsx
```

Start with the rough state model:

```tsx
import { useEffect, useState } from "react";
import Swui from "./swui.esm";

type PausePage = "main" | "settings";

export function PauseMenuModal() {
  const [open, setOpen] = useState(false);
  const [page, setPage] = useState<PausePage>("main");

  function openPause() {
    setPage("main");
    setOpen(true);
  }

  function closePause() {
    setOpen(false);
    setPage("main");
    Swui.emitAction?.("ClosePause");
  }

  function openSettings() {
    setPage("settings");
  }

  function backToMain() {
    setPage("main");
  }

  function handleCancel() {
    if (!open) return;

    if (page === "settings") {
      backToMain();
      return;
    }

    closePause();
  }

  useEffect(() => {
    const offOpen = Swui.onAction("OpenPause", openPause);
    const offCancel = Swui.onCancel(handleCancel);

    return () => {
      offOpen();
      offCancel();
    };
  }, [open, page]);

  if (!open) return null;

  return (
    <div className="pause-shell">
      <div className="pause-modal">
        {page === "main" && (
          <>
            <button className="pause-close" onClick={closePause}>
              ×
            </button>

            <h1 className="pause-title">Paused</h1>

            <nav className="pause-menu">
              <button onClick={closePause}>Continue</button>
              <button onClick={openSettings}>Settings</button>
              <button onClick={() => Swui.emitAction?.("QuitGame")}>
                Quit Game
              </button>
            </nav>
          </>
        )}

        {page === "settings" && (
          <>
            <button className="pause-back" onClick={backToMain}>
              ←
            </button>

            <h1 className="pause-title">Settings</h1>

            <div className="pause-settings-empty">
              Settings page placeholder
            </div>
          </>
        )}
      </div>
    </div>
  );
}
```

This already gives you:

```text
OpenPause action
Cancel behavior
main page
settings page
continue button
quit button
close button
back button
```

---

# Step 7: Mount the Pause Menu in Your App

In your main HUD app component:

```tsx
import { PauseMenuModal } from "./PauseMenuModal";

export function HudApp() {
  return (
    <>
      {/* existing HUD */}
      <PauseMenuModal />
    </>
  );
}
```

The modal stays hidden until `OpenPause` arrives from Unreal.

---

# Step 8: Handle Cancel Behavior

Cancel behavior is page-aware:

```text
Cancel on main page
→ close pause menu

Cancel on settings page
→ return to main pause page
```

This is handled by:

```tsx
function handleCancel() {
  if (!open) return;

  if (page === "settings") {
    backToMain();
    return;
  }

  closePause();
}
```

So `ESC`, `B`, or `Circle` can all call the same `SwuiNavigation.Cancel()` API from Unreal.

---

# Step 9: Add Blueprint Side Effects

Use `SwuiNavigation` Blueprint callbacks for native effects.

Examples:

```text
OnConfirm
→ play click sound

OnCancel
→ play back sound

OnNextTab
→ play tab sound

OnNavigationAction("OpenPause")
→ play menu open sound

OnNavigationAction("ClosePause")
→ play menu close sound
```

If a Blueprint handler consumes an action, it can prevent JS forwarding depending on your dispatch setup.

Good use cases:

```text
play UI sounds
trigger controller rumble
pause/unpause game
block QuitGame during unsafe states
open native confirmation dialogs
```

---

# Step 10: Add Basic Styling

Start with a simple functional layout.

```css
.pause-shell {
  position: fixed;
  inset: 0;
  display: grid;
  place-items: center;
  pointer-events: auto;
}

.pause-modal {
  position: relative;
  width: 520px;
  min-height: 420px;
  padding: 32px;
  color: white;

  background:
    linear-gradient(135deg, rgba(255,255,255,0.14), rgba(255,255,255,0.04)),
    rgba(8, 16, 26, 0.78);

  border: 1px solid rgba(120, 220, 255, 0.28);
  box-shadow:
    inset 0 1px 0 rgba(255,255,255,0.16),
    0 18px 48px rgba(0,0,0,0.45);

  clip-path: polygon(
    18px 0,
    100% 0,
    100% calc(100% - 18px),
    calc(100% - 18px) 100%,
    0 100%,
    0 18px
  );
}

.pause-title {
  margin-bottom: 32px;
  font-size: 28px;
  letter-spacing: 0.12em;
  text-transform: uppercase;
}

.pause-menu {
  display: flex;
  flex-direction: column;
  gap: 14px;
}

.pause-menu button,
.pause-close,
.pause-back {
  cursor: pointer;
  color: white;
  background: rgba(80, 180, 255, 0.14);
  border: 1px solid rgba(120, 220, 255, 0.32);
  padding: 12px 18px;
  font: inherit;
  text-transform: uppercase;
  letter-spacing: 0.08em;
}

.pause-menu button:hover,
.pause-menu button:focus,
.pause-close:hover,
.pause-back:hover {
  background: rgba(120, 220, 255, 0.24);
}

.pause-close {
  position: absolute;
  top: 18px;
  right: 18px;
}

.pause-back {
  position: absolute;
  top: 18px;
  left: 18px;
}

.pause-settings-empty {
  opacity: 0.7;
}
```

---

# Step 11: Add Keyboard/Gamepad Focus Behavior

For keyboard/gamepad menu navigation, give your menu buttons predictable focus order.

The rough structure already supports this:

```tsx
<nav className="pause-menu">
  <button onClick={closePause}>Continue</button>
  <button onClick={openSettings}>Settings</button>
  <button onClick={() => Swui.emitAction?.("QuitGame")}>
    Quit Game
  </button>
</nav>
```

When the menu opens, focus the first button:

```tsx
import { useEffect, useRef, useState } from "react";

const continueRef = useRef<HTMLButtonElement>(null);

useEffect(() => {
  if (open && page === "main") {
    continueRef.current?.focus();
  }
}, [open, page]);
```

Then:

```tsx
<button ref={continueRef} onClick={closePause}>
  Continue
</button>
```

For the Settings page, focus the back button:

```tsx
const backRef = useRef<HTMLButtonElement>(null);

useEffect(() => {
  if (open && page === "settings") {
    backRef.current?.focus();
  }
}, [open, page]);
```

---

# Step 12: Wire Quit Game

On the JS side:

```tsx
<button onClick={() => Swui.emitAction?.("QuitGame")}>
  Quit Game
</button>
```

On the Unreal side, bind to the `QuitGame` action through `SwuiNavigation` / SWUI interop.

Blueprint example:

```text
OnNavigationAction("QuitGame")
→ Quit Game
```

Or route it through a confirmation prompt first.

---

# Step 13: Final Behavior Checklist

Main pause page:

```text
Continue button at top
Settings button in the menu
Quit Game button at bottom
X button in top-right
ESC closes menu
Cancel closes menu
```

Settings page:

```text
Same PauseMenuModal
No new browser/window
Back arrow in top-left
ESC returns to main pause page
Cancel returns to main pause page
```

Unreal side:

```text
OpenPauseMenu pauses game
OpenPauseMenu focuses SWUI
OpenPauseMenu shows cursor
ClosePauseMenu unpauses game
ClosePauseMenu restores gameplay input
```

JS side:

```text
OpenPause action opens modal
ClosePause action returns control to UE
Cancel is page-aware
Buttons emit actions where needed
```

---

# Result

You now have a complete SWUI pause menu flow:

```text
Game input
→ SwuiNavigation
→ Blueprint side effects
→ JavaScript menu events
→ PauseMenuModal UI
→ JS actions back to Unreal
```

The system stays input-framework agnostic, works with Blueprint or C++, and keeps the menu behavior centralized through `SwuiNavigation`.