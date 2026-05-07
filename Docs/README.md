# SimpleWebUI (SWUI) — User Guide

> CEF-powered HTML/CSS/JS UI inside Unreal Engine. No UMG. No Slate. Just a browser.

---

## Table of Contents

1. [How it works](#how-it-works)
2. [Setup](#setup)
3. [Web UI Bindings — the Details panel](#web-ui-bindings--the-details-panel)
4. [State sync — what gets pushed to JS automatically](#state-sync)
5. [Late-spawned actors (characters, weapons, etc.)](#late-spawned-actors)
6. [Writing your HTML / TypeScript](#writing-your-html--typescript)
7. [Showing and hiding the HUD](#showing-and-hiding-the-hud)
8. [Loading a new URL at runtime](#loading-a-new-url-at-runtime)
9. [Executing JavaScript from Blueprint / C++](#executing-javascript-from-blueprint--c)
10. [Supported property types](#supported-property-types)
11. [Namespacing](#namespacing)
12. [Blueprint nodes reference](#blueprint-nodes-reference)

---

## How it works

```
Unreal (C++ / Blueprint)
  │
  │  USwui component (on your PlayerController)
  │    └─ BeginPlay → USwuiSubsystem::SetBindingSources + InitRenderer
  │
  │  USwuiSubsystem (GameInstance subsystem, lives for the whole session)
  │    ├─ Spawns a CEF browser off-screen (USwuiView / CefBrowser)
  │    ├─ Renders into a UTexture2D
  │    ├─ Displays texture on a full-screen UMG Image widget
  │    └─ Ticks every 50 ms → reads registered UPROPERTY values
  │         → serializes changed ones to JSON-like JS snippets
  │         → injects into the browser via ExecuteJavaScript
  │              window.__SWUI__.state["namespace.PropName"] = value
  │              window.__SWUI__._notify("namespace.PropName", value)
  │
HTML page (Content/UI/*.html)
  └─ Import generated .ts, call swuiOnChange("char.Health", v => ...)
```

CEF runs in its own process. The render result is shared as a texture — no GPU readback, no copies. The Unreal side never blocks waiting for JS.

---

## Setup

### 1. Add the `USwui` component

Add a **Swui** actor component to your **PlayerController** Blueprint (or any actor that stays alive for the session — PlayerController is the right choice for a HUD).

### 2. Configure the component

| Property | Description |
|---|---|
| **Interface Name** | Used as the TS interface prefix and the generated file name. e.g. `PlayerHUD` → `Content/UI/generated/PlayerHUD.generated.ts` |
| **Default URI** | Path to your HTML file. Bare paths resolve under `Content/` with `.html` implicit. e.g. `UI/hud` → `Content/UI/hud.html`. Full `http://` URLs pass through as-is. |
| **Is HUD** | When true, the surface automatically matches the game viewport resolution. |
| **View Width / Height** | Manual resolution, only used when Is HUD is false. |
| **Z Order** | Widget layer depth. Higher = on top. |

### 3. Configure Web UI Bindings

See the next section.

### 4. Hit Play

The subsystem initialises the browser, loads your URI, and starts syncing state.

---

## Web UI Bindings — the Details panel

This is the main configuration panel. You'll find it on the Swui component under **Web UI Bindings**.

### Source groups

Each source group represents one C++ or Blueprint class whose properties you want to sync to the browser.

- **Owner Class (slot 0)** — automatically filled with the class of the actor the Swui component is attached to (your PlayerController). Read-only. Expand it to see and tick properties.
- **Source 2, 3, …** — add extra classes (Character, Weapon, etc.) via the **+** button at the bottom. Use the class picker to choose a class. Delete with the **×** button.

### State Properties

Expand **State Properties** inside any source group to see a checklist of every `BlueprintReadWrite` or `BlueprintReadOnly` property on that class with a supported type. Check the ones you want synced.

Only checked properties get synced to JavaScript and included in the generated TypeScript file.

### Refresh JS Bindings

Hit this button to regenerate `Content/UI/generated/<InterfaceName>.generated.ts`. A toast notification confirms success or failure. Regenerate any time you check/uncheck properties or rename classes.

---

## State sync

Once properties are checked and Play is pressed:

1. `USwui::BeginPlay` calls `USwuiSubsystem::SetBindingSources`, which scans **every actor currently in the world** and auto-observes all matching instances.
2. A 50 ms timer ticks — for each observed property it reads the current value, compares to the last sent value, and if changed, injects a JS snippet:
   ```js
   window.__SWUI__.state["character.Health"] = 75;
   window.__SWUI__._notify("character.Health", 75);
   ```
3. Your HTML page reacts via `swuiOnChange`.

**Nothing to wire in Blueprint for actors that exist at BeginPlay.** If your PlayerController, GameState, or GameMode has the properties — they're handled automatically.

---

## Late-spawned actors

For actors that don't exist yet at `BeginPlay` (a Character that gets possessed, a weapon that gets picked up):

```
After spawn / possess, call:
  USwuiSubsystem → Observe Source (Instance = YourCharacter)
```

`ObserveSource` looks up the cached binding config, finds the entry whose class matches, and registers all checked properties for that instance. You don't need to know or specify which individual properties — the Details panel selection drives it.

**Typical pattern for a possessed character:**

```
Event On Possess (NewPawn)
  └─ Get Game Instance → Get Subsystem (SwuiSubsystem) → Observe Source (NewPawn)

Event On UnPossess
  └─ Get Game Instance → Get Subsystem (SwuiSubsystem) → Unobserve (OldPawn)
```

---

## Writing your HTML / TypeScript

The generated file (`Content/UI/generated/PlayerHUD.generated.ts`) gives you:

```ts
import { swuiOnChange, swuiGetState } from "./generated/PlayerHUD.generated";

// React to a value changing:
swuiOnChange("character.Health", (value) => {
    document.getElementById("health").textContent = String(value);
});

// Read the current snapshot synchronously (safe after first push):
const state = swuiGetState();
console.log(state["character.Health"]);
```

### Namespace format

Keys are always `"namespace.PropertyName"`:
- Namespace = class name, `A`/`U` prefix stripped, lowercased.
- `ADACharacter` → `"dacharacter"`
- `ADAPlayerController` → `"daplayercontroller"`
- `AMyGun` → `"mygun"`

So `Health` on `ADACharacter` → key `"dacharacter.Health"`.

### Placing HTML files

Put your files under `Content/UI/`. Reference them in Default URI as `UI/hud` (no extension needed). Sub-folders work: `UI/screens/inventory`.

---

## Showing and hiding the HUD

```
USwuiSubsystem → Set Widget Visible (true/false)
USwuiSubsystem → Show HUD  (shorthand on USwui)
USwuiSubsystem → Hide HUD  (shorthand on USwui)
```

CEF keeps running in the background — the texture is just hidden. Resume is instant.

---

## Loading a new URL at runtime

```
USwuiSubsystem → Load URI ("UI/inventory")
```

---

## Executing JavaScript from Blueprint / C++

```
USwuiSubsystem → Execute Java Script ("document.body.style.opacity = '0.5'")
```

Runs in the context of the currently loaded page. Fire and forget — no return value.

---

## Supported property types

| UE type | TypeScript type |
|---|---|
| `float`, `double`, `int32`, `int64`, `uint8` | `number` |
| `bool` | `boolean` |
| `FString`, `FName`, `FText` | `string` |

Structs, arrays, objects, and enums are not supported for sync. If you need complex data, serialize it to a string in Blueprint and expose the string property.

---

## Namespacing

Namespaces are derived automatically from the class name — you never set them manually.

| Class | Namespace |
|---|---|
| `ADAPlayerController` | `daplayercontroller` |
| `ADACharacter` | `dacharacter` |
| `ABP_MyGun_C` | `bp_mygun_c` |

If you add multiple source classes with properties that share a name (e.g. both have `Health`), the namespace distinguishes them: `"dacharacter.Health"` vs `"daplayercontroller.Health"`.

---

## Blueprint nodes reference

| Node | Where to call | What it does |
|---|---|---|
| **Observe Source** | After spawning / possessing a late actor | Registers all checked properties for that instance |
| **Unobserve** | Before destroying / un-possessing | Stops tracking all properties from that instance |
| **Set Widget Visible** | Anywhere | Show / hide the browser surface |
| **Load URI** | Anywhere | Navigate the browser to a new page |
| **Execute Java Script** | Anywhere | Run arbitrary JS in the loaded page |

The older **SWUI Observe** and **SWUI Observe Event** per-property nodes still work if you need fine-grained control, but `ObserveSource` is the recommended path for the common case.
