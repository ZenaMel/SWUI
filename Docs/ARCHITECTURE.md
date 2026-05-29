# SWUI Navigation Events — Typed Payload Architecture

## Overview

Navigation events are the **JS → UE** command channel. When the web UI needs to trigger an action in C++/Blueprint (e.g. "start a match", "open inventory", "quit to menu"), it emits a GameplayTag-based navigation event that is routed to the USwuiNavigation component.

The entire system is **payload-typed**: every navigation event carries a UScriptStruct that defines its data shape. No untyped paths exist in production code.

## Data flow

```
React / TS UI
    │  Swui.emitNavigationEvent("swui.rooms.host", {RoomName, MapId, ...})
    ▼
CEF → USwuiView::ReceiveNavigationEventFromJs
    │  parses JSON, extracts tag + payload
    ▼
USwuiNavigation::DispatchEvent
    │  fires OnNavigationEvent(FGameplayTag Event, FString JsonPayload)
    ▼
K2Node_SwuiNavigationEvent (expanded graph)
    │  1. compare Event against configured NavigationEventTag
    │  2. JsonToStruct(JsonPayload, StructPath) → FInstancedStruct
    │  3. GetInstancedStructValue(InstancedStruct) → typed Payload
    ▼
Blueprint / AngelScript receives typed struct pin
```

## Shape ownership

| Direction | Shape owner | Mechanism |
|-----------|-------------|-----------|
| JS → UE navigation commands | `FSwuiNavigationEvent.PayloadStruct` → `UScriptStruct` | Set in the Details panel on USwuiNavigation component |
| UE → JS observed delegate payloads | Delegate `SignatureFunction` params | `UFUNCTION` reflection |
| UE → JS observed state | `UPROPERTY` reflection | UHT |

There is no single global payload owner. Each direction uses its own reflection mechanism.

## Architecture pieces

### 1. Event identity — `FGameplayTag`

Every navigation event is identified by a GameplayTag, e.g. `swui.rooms.host`, `swui.menu.quit`. Tags are configured in the `NavigationEvents` array on `USwuiNavigation`.

Tags starting with `swui.` are the built-in namespace. Custom events use sub-namespaces under `swui.*` (e.g. `swui.rooms.*`, `swui.online.*`).

### 2. Payload shape — `FSwuiNavigationEvent.PayloadStruct`

Each entry in the `NavigationEvents` array has an optional `PayloadStruct` field:

```cpp
UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="SWUI|Navigation",
    meta=(DisplayName="Payload Struct"))
TSoftObjectPtr<UScriptStruct> PayloadStruct;
```

When set, this UScriptStruct defines the JSON payload schema for the event. **Every event configured for Blueprint use must have a PayloadStruct.** For events that carry no data, use the built-in `FSwuiEmptyPayload` struct.

### 3. TS payload generation — from UScriptStruct

During TS binding regeneration (`SwuiRegenerateBindings`), the generator reads each `FSwuiNavigationEvent.PayloadStruct` and emits:

```typescript
// From FOneVRoomHostPayload { RoomName: string; MapId: string; Mode: string; bIsPublic: bool; }
export interface HostRoomPayload {
    RoomName: string;
    MapId: string;
    Mode: string;
    bIsPublic: boolean;
}

// On the SwuiNavigationEvents helper:
roomsHost(payload: HostRoomPayload): void {
    Swui.emitNavigationEvent(SwuiNavigationEvents.roomsHost, payload);
}
```

**Field names must match between C++ and TS.** The JSON deserializer (`FJsonObjectConverter::JsonObjectStringToUStruct`) maps JSON keys directly to C++ struct field names by reflection. If the C++ struct has `bIsPublic`, the TS must emit `bIsPublic`.

### 4. K2Node_SwuiNavigationEvent — typed BP node

The Blueprint node for navigation events is `K2Node_SwuiNavigationEvent` (in `SwuiUncookedOnly` module).

At **compile time** (`ExpandNode`):
1. Resolves `PayloadStruct` from the owning Blueprint component's `NavigationEvents` array (tag match)
2. Falls back to `FSwuiEmptyPayload::StaticStruct()` if no explicit struct is configured
3. Sets the `Payload` output pin type to `PC_Struct` with `PinSubCategoryObject = PayloadStruct`
4. Spawns an intermediate graph:
   - Tag comparison via `UBlueprintGameplayTagLibrary`
   - `USwuiJsonBlueprintLibrary::JsonToStruct(JsonPayload, StructPath)` → `FInstancedStruct`
   - `UBlueprintInstancedStructLibrary::GetInstancedStructValue` → typed struct
5. Hides the `Event` and `JsonPayload` pins from the delegate signature
6. Shows only `Exec` + `Payload` (struct-by-value)

At **runtime**:
1. `USwuiNavigation::OnNavigationEvent` fires with `(FGameplayTag, FString)`
2. Tag comparison branch: if match, exec continues to the deserialization chain
3. `JsonToStruct` allocates an `FInstancedStruct` of the target type and calls `FJsonObjectConverter::JsonObjectStringToUStruct`
4. `GetInstancedStructValue` extracts the typed struct
5. Downstream BP nodes receive the typed struct on the `Payload` pin

### 5. USwuiJsonBlueprintLibrary — runtime JSON deserializer

```cpp
UCLASS()
class USwuiJsonBlueprintLibrary : public UBlueprintFunctionLibrary
{
    UFUNCTION(BlueprintInternalUseOnly)
    static FInstancedStruct JsonToStruct(const FString& JsonPayload, const FString& StructPath);
};
```

Lives in `SwuiRuntime` module. Called only by intermediate nodes created at compile time — never used directly in Blueprint graphs.

### 6. FSwuiEmptyPayload — explicit no-data payload

```cpp
USTRUCT(BlueprintType)
struct FSwuiEmptyPayload
{
    GENERATED_BODY()
};
```

Used as the default `PayloadStruct` for navigation events that carry no data. Ensures every node always has a typed output pin. Defined in `SwuiTypes.h`.

## Pin visibility

| Context | Typed event (has PayloadStruct) | No-payload event |
|---------|-------------------------------|------------------|
| Node title | `On Rooms Host` | `On Rooms Refresh` |
| Exec pin | ✅ then | ✅ then |
| Payload pin | ✅ typed struct (e.g. `HostRoomPayload`) | ✅ `FSwuiEmptyPayload` |
| Event pin | ❌ hidden (internal) | ❌ hidden |
| JsonPayload pin | ❌ hidden (internal) | ❌ hidden |

## Validation

- `K2Node_SwuiNavigationEvent::ValidateNodeDuringCompilation` emits a hard compile error if `ResolvePayloadStruct()` returns null
- Every node must have a resolution path: either the component's `NavigationEvents` entry has `PayloadStruct` set, or the fallback to `FSwuiEmptyPayload` succeeds
- `USwuiJsonBlueprintLibrary::JsonToStruct` logs a warning at runtime if the struct path doesn't resolve or JSON deserialization fails

## File locations

| File | Module | Purpose |
|------|--------|---------|
| `SwuiRuntime/Public/SwuiNavigation.h` | SwuiRuntime | `USwuiNavigation` component, `FSwuiNavigationEvent` config |
| `SwuiRuntime/Public/SwuiTypes.h` | SwuiRuntime | `FSwuiEmptyPayload` |
| `SwuiRuntime/Public/SwuiJsonBlueprintLibrary.h` | SwuiRuntime | Runtime JSON→struct helper |
| `SwuiUncookedOnly/Public/K2Node_SwuiNavigationEvent.h` | SwuiUncookedOnly | Blueprint node |
| `SwuiEditor/Private/SwuiNavigationDetails.cpp` | SwuiEditor | Details panel for tag/struct config |
| `SwuiEditor/Private/SwuiTSGenerator.cpp` | SwuiEditor | TS file generation |
| `Content/UI/generated/MyHUD.navigation.generated.ts` | — | Generated TS output |
