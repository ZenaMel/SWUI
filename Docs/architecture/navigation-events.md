# Navigation events — typed JS → UE command channel

## What this solves

The web UI needs to trigger game actions (host a room, join a game, open a menu) without hardcoding string event names or manually parsing JSON payloads. Navigation events give every command a **GameplayTag identity** and a **UScriptStruct payload schema**, with generated typed helpers on both the TypeScript and Blueprint sides.

## Mental model

```
React/TS typed emit (.roomsHost({RoomName, ...}))
 → Swui.emitNavigationEvent(tag, payload)  ← internal JSON transport
 → CEF → USwuiView::ReceiveNavigationEventFromJs
 → USwuiNavigation::DispatchEvent
 → K2Node_SwuiNavigationEvent deserializes JSON → typed struct pin
 → Blueprint/AngelScript receives typed payload
```

The GameplayTag is the **event identity**. The UScriptStruct is the **payload schema**. JSON is the **internal transport** — invisible to both the TS emit side and the BP/AS receive side.

## Source of truth

The payload shape is defined by a `UScriptStruct` assigned to each `FSwuiNavigationEvent` entry in the `USwuiNavigation` component's `NavigationEvents` array.

Alternatively, `UFUNCTION(meta = (SwuiEvent = "onev.rooms.host"))` auto-registers a navigation event where the function parameters become the payload schema — no manual struct definition needed.

## Two registration paths

| Path | Where | Payload source |
|---|---|---|
| Manual config | `USwuiNavigation` Details panel → `NavigationEvents` + `PayloadStruct` picker | `UScriptStruct` field |
| Code-exposed | `UFUNCTION(meta = (SwuiEvent = "onev.rooms.host"))` on any actor component | Function parameter `FProperty` list |

Code-exposed events are auto-discovered by `SwuiCollectFunctionExposedNavInfos()` during binding regeneration.

## Runtime flow

1. React calls the generated typed helper, e.g. `SwuiNavigationEvents.roomsHost({RoomName: "test", MapId: "whitebox"})`
2. Helper calls `Swui.emitNavigationEvent("onev.rooms.host", payload)` which serializes to JSON
3. CEF forwards the message to UE via `cefQuery`
4. `USwuiView::HandleIncomingMessage` routes to `USwuiNavigation::ReceiveNavigationEventFromJs`
5. Tag doesn't match built-in → `DispatchEvent` fires `OnNavigationEventWithPayload`
6. `K2Node_SwuiNavigationEvent` (expanded graph) deserializes JSON into struct using the stored `PayloadStruct`
7. Blueprint/AngelScript receives typed struct pin

## K2Node_SwuiNavigationEvent — typed Blueprint node

The Blueprint node expands at compile time (`ExpandNode`) into:

1. Tag comparison via `UBlueprintGameplayTagLibrary`
2. `USwuiJsonBlueprintLibrary::JsonToStruct(JsonPayload, StructPath)` → `FInstancedStruct`
3. `UBlueprintInstancedStructLibrary::GetInstancedStructValue` → typed struct

The node hides the `Event` and `JsonPayload` pins and shows only `Exec` + `Payload` (struct-by-value). The `PayloadStruct` is resolved from the owning Blueprint component's `NavigationEvents` array (tag match), falling back to `FSwuiEmptyPayload`.

`FSwuiEmptyPayload` ensures every node always has a typed output pin even for zero-data events. Defined in `SwuiTypes.h`:

```cpp
USTRUCT(BlueprintType)
struct FSwuiEmptyPayload { GENERATED_BODY() };
```

## Full file list

| File | Module | Purpose |
|------|--------|---------|
| `SwuiRuntime/Public/SwuiNavigation.h` | SwuiRuntime | Component, event config |
| `SwuiRuntime/Public/SwuiTypes.h` | SwuiRuntime | `FSwuiNavigationEvent`, `FSwuiEmptyPayload` |
| `SwuiRuntime/Public/SwuiJsonBlueprintLibrary.h` | SwuiRuntime | Runtime JSON→struct helper |
| `SwuiUncookedOnly/Public/K2Node_SwuiNavigationEvent.h` | SwuiUncookedOnly | Blueprint node |
| `SwuiEditor/Private/SwuiNavigationDetails.cpp` | SwuiEditor | Details panel for tag/struct config |
| `SwuiEditor/Private/SwuiTSGenerator.cpp` | SwuiEditor | TS file generation |
| `Content/UI/generated/MyHUD.navigation.generated.ts` | — | Generated TS output |

## How to use it

### Manual path

1. Add an entry to `USwuiNavigation` → `NavigationEvents`
2. Set the **Event** tag (e.g. `onev.rooms.host`)
3. Pick a **Payload Struct** (e.g. `FOneVRoomsHostPayload`)
4. Regenerate bindings
5. TS gets typed helper: `roomsHost(payload: FOneVRoomsHostPayload): void`
6. BP gets typed pin when handling the event

### Code-exposed path

```cpp
UFUNCTION(meta = (SwuiEvent = "onev.rooms.host"))
void HostRoom(const FString& RoomName, const FString& MapId, bool bIsPublic);
```

No manual config needed — the generator discovers the `SwuiEvent` metadata, derives tag `onev.rooms.host`, generates payload interface from function params, and creates typed emit/listen helpers.

## Gotchas

- The `SwuiEvent` metadata value must be a valid GameplayTag string (dot-separated). Invalid tags are skipped with a warning.
- If both a manual `FSwuiNavigationEvent` and a code-exposed event produce the same tag, the manual one wins (code-exposed is skipped).
- Code-exposed events are only discovered on `AActor` + `UActorComponent` subclasses (auto-discovery scope).
- `PayloadStruct` on manual entries must be loaded before generation runs. The AssetRegistry scan in `SwuiFindAllSwuiAssets` handles this for commandlet mode.

## Debug checklist

- Regenerated bindings? Run `SwuiRegenerateBindings` console command or click the menu button.
- Generated `.navigation.generated.ts` file has the typed helpers?
- GameplayTag exists in the tag manager? Code-exposed events auto-register via `AddNativeGameplayTag`.
- K2 node expanded and showing the right payload struct pin?
- JSON payload at runtime matches the struct schema?
