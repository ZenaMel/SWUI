# SimpleWebUI (SWUI) — Internal Docs

## Four major data paths

1. **UE observed state → React**: `UPROPERTY(BlueprintReadOnly)` values are synced to the browser via `window.__SWUI__.state["ns.PropName"] = value` every tick. See usage/observed-state.md.
2. **UE delegates → React events**: `UPROPERTY(BlueprintAssignable)` delegates fire JS `CustomEvent`s when broadcast. See architecture/delegate-payloads.md.
3. **React navigation events → UE**: GameplayTag + typed payload → JSON transport → UE receives via `USwuiNavigation`. See architecture/navigation-events.md and usage/typed-navigation-payloads.md.
4. **React/CEF input → UE**: Slate keyboard events forwarded to CEF via `CefBrowserHost::SendKeyEvent()`. See architecture/cef-input-forwarding.md and usage/react-input-fields.md.

## Core principle

**Unreal reflection is the source of truth.** Generated TypeScript mirrors Unreal state, delegates, and navigation payload schemas. JSON is internal transport only.

## Directory map

| Path | Purpose |
|---|---|
| `architecture/` | How SWUI internals work (data flow, design decisions, tricky parts) |
| `usage/` | How a game project uses each feature (game-facing examples) |
| `adr/` | Architecture Decision Records — why we chose certain approaches |

## Architecture docs

- [Navigation events](architecture/navigation-events.md) — typed JS→UE command channel, GameplayTag identity, PayloadStruct schema
- [Delegate payloads](architecture/delegate-payloads.md) — serializing delegate params through ProcessEvent and SignatureFunction
- [CEF input forwarding](architecture/cef-input-forwarding.md) — making HTML form controls work via SendKeyEvent
- [Text input focus](architecture/text-input-focus.md) — DOM focus bridge for bTextInputFocused
- [Binding system](architecture/binding-system.md) — how SWUI discovers observed sources
- [Map travel and rebinding](architecture/map-travel-and-rebinding.md) — what survives and what needs re-binding

## Usage docs

- [Observed state](usage/observed-state.md) — syncing UPROPERTY values to React
- [Observed delegates](usage/observed-delegates.md) — UE delegate → React CustomEvent
- [Typed navigation payloads](usage/typed-navigation-payloads.md) — defining typed JS→UE commands
- [React input fields](usage/react-input-fields.md) — HTML form controls inside SWUI
- [Binding sources](usage/binding-sources.md) — configuring what SWUI observes

## The standard template

When adding a new doc, use this structure:

```markdown
# Feature name

## What this solves

## Mental model

## Source of truth

## Runtime flow

## Important files

## How to use it

## Gotchas

## Debug checklist
```
