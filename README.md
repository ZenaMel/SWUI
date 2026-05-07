# SWUI

**SWUI** is a web UI layer for Unreal Engine.

It lets you build HUDs, menus, overlays, and interactive game UI with HTML, CSS, and TypeScript, while keeping gameplay state and event wiring inside Unreal.

SWUI uses a bundled CEF/Chromium runtime through its initial BLUI-derived backend.

## What SWUI does

SWUI connects reflected Unreal properties and events to a generated TypeScript API.

Instead of manually building JSON, calling JavaScript, or matching raw string event names, you define the UI contract from Unreal objects, properties, and delegates.

SWUI then handles:

- reading reflected Unreal values
- inferring value types and ranges
- forwarding gameplay events
- generating TypeScript bindings
- updating the web UI only when values change
- previewing the UI without launching Unreal Editor

## Workflow

1. Select Unreal properties and events for your UI.
2. SWUI generates a typed TypeScript facade.
3. Build the visual layer with HTML, CSS, and TypeScript.
4. Preview the UI in a dedicated CEF window.
5. Run the same UI in Unreal.

## Example use cases

- HUDs
- dynamic crosshairs
- health, ammo, and stamina displays
- interaction prompts
- objective trackers
- menus
- inventory screens
- dialogue UI
- debug overlays

## Preview

SWUI includes a standalone preview window.

The preview uses the same bundled CEF/Chromium runtime as the Unreal backend, so the UI is tested in the same browser environment it will use at runtime.

Generated preview controls allow values such as numbers, booleans, strings, enums, and events to be changed live.

## Local UI files

Place built web content in:

```text
YourProject/Content/html/

````

Example:

```
YourProject/Content/html/MainHUD/dist/index.html

```

Load it with:

```
local://MainHUD/dist/index.html

```

## C++ naming

SWUI uses readable Unreal-style class names such as:

```
USwuiBridge
USwuiView
USwuiBindingAsset
FSwuiPayload
ISwuiBackend

```

## Status

SWUI is an early-stage project derived from the BLUI/SimpleWebUI plugin lineage.

The initial focus is a reflection-driven HUD workflow, using a dynamic crosshair sample to dogfood the API and preview system.

## Credits

SWUI is derived from the BLUI/SimpleWebUI lineage and keeps the CEF-backed Unreal browser integration model.

Original license notices and credits are preserved according to the upstream license.