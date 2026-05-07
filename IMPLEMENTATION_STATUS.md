Got it. This version keeps it **user-story only** and leaves engineering implementation choices out.

````
# SWUI — User Story Tracker

Last updated: 2026-05-07

Project/plugin name: **SWUI**

SWUI lets UE developers expose reflected game state and gameplay events to web-based HUDs/menus, while web/UI developers receive generated TypeScript stubs, typed bindings, and a dedicated bundled-CEF preview window.

---

# Status Legend

| Marker | Status |
|---|---|
| `[ ]` | Open |
| `[/]` | In progress |
| `[x]` | Complete |
| `[D]` | Dogfood validation required |

---

# Story Overview

## UE Developer Stories

| ID | Story | Progress |
|---|---|---|
| UE-1 | Create a SWUI interface from an HTML entry file | `[ ]` |
| UE-2 | Show a SWUI interface as a HUD/menu in Unreal | `[ ]` |
| UE-3 | Bind reflected Unreal properties to the interface | `[ ]` |
| UE-4 | Bind reflected Unreal events/delegates to the interface | `[ ]` |
| UE-5 | Generate frontend TypeScript bindings and stubs from Unreal bindings | `[ ]` |
| UE-6 | Preview the bound UI contract from Unreal metadata | `[ ]` |
| UE-7 | Validate the UE workflow with the dynamic crosshair sample | `[D]` |

## Web/UI Developer Stories

| ID | Story | Progress |
|---|---|---|
| WEB-1 | Receive generated TypeScript facade and stubs | `[ ]` |
| WEB-2 | Use autocomplete for state fields and events | `[ ]` |
| WEB-3 | Bind generated state fields to HTML/CSS behavior | `[ ]` |
| WEB-4 | Handle generated gameplay events in TypeScript | `[ ]` |
| WEB-5 | Design and iterate in the dedicated SWUI preview window | `[ ]` |
| WEB-6 | Manipulate preview values through generated controls | `[ ]` |
| WEB-7 | Trigger preview events through generated controls | `[ ]` |
| WEB-8 | Use the same UI code in preview and Unreal runtime | `[ ]` |
| WEB-9 | Validate the web workflow with the dynamic crosshair sample | `[D]` |

---

# UE Developer Stories

## UE-1 — Create a SWUI interface from an HTML entry file

As a UE developer, I want to create a SWUI interface by selecting an HTML entry file so I can define which web UI Unreal should display.

Expected workflow:

```text
Create SWUI Interface
  Name: MainHUD
  Entry: hud

````

The `.html` extension is implicit, so `hud` resolves to `hud.html`.

Expected behavior:

* I can create an interface such as `MainHUD`.
* I can choose an entry file such as `hud`.
* SWUI knows which web UI file belongs to the interface.
* The interface becomes available for HUD/menu usage.

Progress: `[ ]`

***

## UE-2 — Show a SWUI interface as a HUD/menu in Unreal

As a UE developer, I want to show a SWUI interface in-game so I can use it as a HUD, menu, overlay, or interactive screen.

Expected workflow:

```
Show SWUI Interface
  Interface: MainHUD
  Layer: HUD
  Z Order: 0

```

Expected behavior:

* I can show the selected interface in-game.
* I can choose whether it behaves like a HUD, menu, or overlay.
* I can configure basic display settings such as resolution, layer, and Z order.
* The selected web UI renders through the bundled CEF backend.

Progress: `[ ]`

***

## UE-3 — Bind reflected Unreal properties to the interface

As a UE developer, I want to bind Unreal properties to a SWUI interface so the web UI receives live state values automatically.

Example bindings:

```
Weapon.CurrentSpread
Weapon.CurrentAmmo
HUDState.CrosshairMode
Interaction.bCanInteract
Interaction.Prompt

```

Expected behavior:

* I can select reflected objects/classes from Unreal.
* I can pick exposed properties from dropdowns.
* SWUI infers the value type.
* SWUI infers useful preview metadata such as ranges and enum values.
* The selected state fields become part of the generated frontend API.

Progress: `[ ]`

***

## UE-4 — Bind reflected Unreal events/delegates to the interface

As a UE developer, I want to bind gameplay events/delegates to a SWUI interface so the web UI can react to gameplay moments.

Example events:

```
Weapon.OnPlayerFiredShot(GunType, AmmoRemaining, Spread)
Combat.OnHitConfirmed()
Player.OnTookDamage(Amount, Direction)

```

Expected behavior:

* I can select reflected events/delegates from dropdowns.
* SWUI infers event names.
* SWUI infers event payload fields.
* The selected events become part of the generated frontend API.

Progress: `[ ]`

***

## UE-5 — Generate frontend TypeScript bindings and stubs from Unreal bindings

As a UE developer, I want SWUI to generate the web-side TypeScript contract and starter files from the selected Unreal bindings.

Expected behavior:

* State bindings generate typed frontend state fields.
* Event bindings generate typed frontend event handlers.
* Starter stubs are created for the web/UI developer.
* The generated files reflect the current Unreal binding setup.
* The web side gets autocomplete for state and events.

Progress: `[ ]`

***

## UE-6 — Preview the bound UI contract from Unreal metadata

As a UE developer, I want SWUI to generate preview controls from the same Unreal bindings so the interface can be tested without manually writing mock data.

Expected behavior:

* Numeric properties become sliders or number inputs.
* Booleans become checkboxes.
* Enums become dropdowns.
* Strings/text become text inputs.
* Events become trigger buttons.
* Event payloads become generated input fields.

Progress: `[ ]`

***

## UE-7 — Validate the UE workflow with the dynamic crosshair sample

As a UE developer, I want the dynamic crosshair sample to prove that SWUI works for a real HUD scenario.

Expected sample bindings:

```
State:
  Weapon.CurrentSpread
  Weapon.CurrentAmmo
  HUDState.CrosshairMode
  Interaction.bCanInteract
  Interaction.Prompt

Events:
  Weapon.OnPlayerFiredShot(GunType, AmmoRemaining, Spread)
  Combat.OnHitConfirmed()

```

Expected behavior:

* The sample can be set up through the normal SWUI workflow.
* The sample generates frontend state/event stubs.
* The sample runs in Unreal.
* The sample runs in the SWUI preview window.

Progress: `[D]`

***

# Web/UI Developer Stories

## WEB-1 — Receive generated TypeScript facade and stubs

As a web/UI developer, I want SWUI to generate TypeScript files for the selected interface so I can start building the visual layer immediately.

Expected generated files:

```
generated/MainHUD.generated.ts
MainHUD.bindings.ts
MainHUD.events.ts

```

Expected behavior:

* The generated facade exposes state fields.
* The generated facade exposes event handlers.
* User-owned stubs give me a starting point.
* Generated files match the Unreal-side interface contract.

Progress: `[ ]`

***

## WEB-2 — Use autocomplete for state fields and events

As a web/UI developer, I want autocomplete for all state fields and events so I can work without guessing names or payload shapes.

Expected usage:

```
const hud = createMainHUD();

hud.state.currentSpread
hud.state.currentAmmo
hud.state.crosshairMode
hud.events.PlayerFiredShot
hud.events.HitConfirmed

```

Expected behavior:

* State fields are discoverable in the editor.
* Event names are discoverable in the editor.
* Event payloads are typed.
* Renamed Unreal bindings are reflected in generated TypeScript.

Progress: `[ ]`

***

## WEB-3 — Bind generated state fields to HTML/CSS behavior

As a web/UI developer, I want to bind generated state fields to DOM text, CSS variables, classes, visibility, and custom UI behavior.

Expected usage:

```
hud.state.currentSpread.bindStyle("#crosshair", "--spread", value => `${value}px`);

hud.state.crosshairMode.bindClass("#crosshair", mode => `crosshair ${mode}`);

hud.state.currentAmmo.bindText("#ammo", ammo => `${ammo}`);

hud.state.canInteract.bindVisible("#interaction-prompt");

hud.state.prompt.bindText("#interaction-prompt");

```

Expected behavior:

* State changes update the UI.
* DOM/CSS helpers are simple and generic.
* Custom watchers are available for custom animation logic.

Progress: `[ ]`

***

## WEB-4 — Handle generated gameplay events in TypeScript

As a web/UI developer, I want to handle generated gameplay events with typed payloads so I can trigger animations and visual reactions.

Expected usage:

```
hud.events.PlayerFiredShot.on(({ gunType, ammoRemaining, spread }) => {
  playCrosshairKick(gunType, spread);
});

hud.events.HitConfirmed.on(() => {
  playHitmarker();
});

```

Expected behavior:

* Event handlers receive typed payloads.
* Events can trigger animations, flashes, sound indicators, hitmarkers, or menu reactions.
* Event code works in both preview and Unreal runtime.

Progress: `[ ]`

***

## WEB-5 — Design and iterate in the dedicated SWUI preview window

As a web/UI developer, I want to open a dedicated preview window so I can design the HUD/menu without launching Unreal Editor.

Expected workflow:

```
Open SWUI Preview
  Interface: MainHUD

```

Expected behavior:

* The preview opens in its own window.
* The preview uses the bundled backend CEF runtime.
* The preview loads the actual web UI.
* The preview loads the generated interface contract.
* I can iterate on visual design quickly.

Progress: `[ ]`

***

## WEB-6 — Manipulate preview values through generated controls

As a web/UI developer, I want preview controls generated from Unreal metadata so I can test UI states interactively.

Expected controls:

```
currentSpread      slider
currentAmmo        number input
crosshairMode      dropdown
canInteract        checkbox
prompt             text input

```

Expected behavior:

* Changing a preview control updates the UI live.
* Preview controls match the inferred Unreal value types.
* Numeric ranges follow Unreal metadata where available.

Progress: `[ ]`

***

## WEB-7 — Trigger preview events through generated controls

As a web/UI developer, I want reflected events to appear as buttons in the preview window so I can test animations and event-driven UI behavior.

Expected controls:

```
PlayerFiredShot    trigger button + payload inputs
HitConfirmed       trigger button
TookDamage         trigger button + payload inputs

```

Expected behavior:

* Triggering an event calls the generated frontend event handler.
* Events with payloads provide editable payload fields.
* Preview event behavior matches runtime event behavior.

Progress: `[ ]`

***

## WEB-8 — Use the same frontend code in preview and runtime

As a web/UI developer, I want the same TypeScript/HTML/CSS code to run in both the preview window and Unreal runtime.

Expected behavior:

* Preview uses the generated facade.
* Runtime uses the generated facade.
* State bindings behave the same in both contexts.
* Event handlers behave the same in both contexts.

Progress: `[ ]`

***

## WEB-9 — Validate the web workflow with the dynamic crosshair sample

As a web/UI developer, I want the dynamic crosshair sample to prove that SWUI is pleasant for real HUD work.

Expected behavior:

* Crosshair spread is driven by generated state.
* Crosshair mode is driven by generated enum state.
* Ammo display is driven by generated state.
* Interaction prompt is driven by generated state.
* Firing animation is driven by generated event.
* Hitmarker animation is driven by generated event.
* Preview controls drive the same code as runtime.

Progress: `[D]`

***

# Dogfood Target: Dynamic Crosshair HUD

The dynamic crosshair sample is the shared validation target for both roles.

It should prove the full flow:

```
UE developer:
  selects interface
  binds reflected state
  binds reflected events
  generates frontend contract
  runs the HUD in Unreal

Web/UI developer:
  imports generated facade
  binds state to HTML/CSS
  handles generated events
  previews in bundled CEF window
  ships same code to runtime

```

Progress: `[D]`

```
```