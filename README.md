# SWUI

**SWUI** is a web UI layer for Unreal Engine.

Build HUDs, menus, overlays, and interactive game UI with browser technologies such as **HTML, CSS, JavaScript, TypeScript, React, Vue, Svelte, or your own frontend setup**.

SWUI keeps gameplay state, events, and input wiring inside Unreal while letting the visual UI layer live in a web runtime.

## Why SWUI?

Unreal UI work often needs a lot of glue:

- manually pushing values to JavaScript
- hand-writing JSON payloads
- matching raw string event names
- wiring Blueprint events to browser callbacks
- keeping Unreal state and frontend state in sync

SWUI reduces that plumbing by generating a bridge between Unreal and the web UI.

The goal is simple:

> Build UI like a web app. Wire it like an Unreal system.

## Core idea

Define the UI-facing state and events in Unreal.

SWUI exposes them to JavaScript and generates optional TypeScript-friendly bindings for projects that want stronger typing.

```text
Unreal properties, events, and GameplayTags
        ↓
SWUI bridge
        ↓
Browser UI: HTML / CSS / JS / any framework
        ↓
GameplayTag-based events back to Unreal
````

## What already works

SWUI has already proven the core Unreal ↔ JavaScript interop path:

* Unreal values can be exposed to the web UI
* JavaScript can react to Unreal-driven updates
* JavaScript can emit events back into Unreal
* GameplayTag-based event routing works
* Blueprint-side event dispatchers and emitters are usable
* HUD/menu interactions can be wired with little to no manual glue
* local web UI files can run through the CEF-backed runtime
* preview/control workflows are already part of the architecture

## What SWUI handles

* exposing selected Unreal values to JavaScript
* forwarding Unreal events to the web UI
* sending web UI events back to Unreal
* routing UI actions through GameplayTags
* generating TypeScript-friendly contracts where useful
* updating UI state only when values change
* previewing UI outside normal gameplay
* keeping the Unreal/web bridge structured instead of stringly-typed

## Example use cases

* HUDs
* pause menus
* interaction prompts
* objective trackers
* dialogue UI
* inventory screens
* debug overlays
* internal control panels
* custom editor/runtime tools

## Workflow

1. Expose Unreal properties, events, and GameplayTags.
2. Generate or use the SWUI bridge API.
3. Build the UI with your preferred web stack.
4. Preview and test the UI.
5. Run the same UI inside Unreal.

## Local UI files

Place built web content in:

```text
YourProject/Content/html/
```

Example:

```text
YourProject/Content/html/MainHUD/dist/index.html
```

Load it with:

```text
local://MainHUD/dist/index.html
```

## Platform Support

SWUI currently targets desktop Unreal Engine projects using a CEF/Chromium backend.

| Platform                   |          Status | Notes                            |
| -------------------------- | --------------: | -------------------------------- |
| Windows x64                |  🧪 Experimental | Primary target, needs validation |
| Windows ARM64              |  🧪 Experimental | Needs validation                 |
| macOS                      |     ✅ Supported | Desktop CEF backend available    |
| Linux                      |  🧪 Experimental | Needs packaging validation       |
| Android                    | ❌ Not supported | No CEF backend                   |
| iOS                        | ❌ Not supported | No CEF backend                   |
| PlayStation 5              | ❌ Not supported | No UE-bundled CEF support        |
| Xbox Series X/S            | ❌ Not supported | No UE-bundled CEF support        |
| Nintendo Switch / Switch 2 | ❌ Not supported | No UE-bundled CEF support        |

### Legend

| Icon | Meaning       |
| ---- | ------------- |
| ✅    | Supported     |
| 🧪    | Experimental  |
| ❌    | Not supported |

## Status

SWUI is an active early-stage Unreal Engine plugin derived from the BLUI / SimpleWebUI lineage.

The core Unreal ↔ JavaScript interop model is already working well. The current focus is improving the developer workflow, generated bindings, preview tooling, event ergonomics, packaging, and desktop runtime stability.

## Credits

SWUI is derived from the BLUI / SimpleWebUI lineage and keeps the CEF-backed Unreal browser integration model.

Original license notices and credits are preserved according to the upstream license.