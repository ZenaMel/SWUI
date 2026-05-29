# Typed navigation payloads — defining and consuming typed JS→UE commands

## What you get

When you assign a payload struct (or `SwuiEvent` metadata) to a navigation event, the generated `.navigation.generated.ts` gains:

1. A TS interface matching the payload schema
2. A `SwuiNavigationPayloads` type map from tag to payload type
3. Typed emit helpers (e.g. `roomsHost(payload: FOneVRoomsHostPayload): void`)
4. Typed listener helpers (e.g. `onRoomsHost(fn: (detail: FOneVRoomsHostPayload) => void): () => void`)
5. The `SwuiNavigationContract` extended with `payloadType` metadata

No manually maintained type definitions. No untyped `Record<string, any>` payloads.

## How to define a typed navigation event

### Path A: Manual struct + editor config

1. Define a `UScriptStruct` in C++ or Blueprint:

```cpp
USTRUCT(BlueprintType)
struct FOneVRoomsHostPayload
{
    GENERATED_BODY()

    UPROPERTY()
    FString RoomName;

    UPROPERTY()
    FString MapId;

    UPROPERTY()
    FString Mode;

    UPROPERTY()
    bool bIsPublic = false;
};
```

2. Open the `USwuiNavigation` component details panel
3. Add a `NavigationEvents` entry
4. Set **Event** tag: `onev.rooms.host`
5. Pick **Payload Struct**: `FOneVRoomsHostPayload`
6. Click **Refresh JS Bindings**

### Path B: Code-exposed UFUNCTION (no struct needed)

Add `SwuiEvent` metadata to any `UFUNCTION` on an actor component:

```cpp
UFUNCTION(BlueprintCallable,
    meta = (SwuiEvent = "onev.rooms.host"))
void HostRoom(const FString& RoomName, const FString& MapId, const FString& Mode, bool bIsPublic);
```

The function parameters become the payload schema. The function name doesn't matter — only the `SwuiEvent` tag value is used.

## Generated TS output

```
interface HostRoomPayload {
    RoomName: string;
    MapId: string;
    Mode: string;
    bIsPublic: boolean;
}

export type SwuiNavigationPayloads = {
    'onev.rooms.host': HostRoomPayload;
    'onev.rooms.refresh': Record<string, never>;
};

export const SwuiNavigationEvents = {
    roomsHost(payload: HostRoomPayload): void {
        Swui.emitNavigationEvent('onev.rooms.host', payload);
    },
    onRoomsHost(fn: (detail: HostRoomPayload) => void): () => void {
        return Swui.onEvent('onev.rooms.host', fn);
    },
};
```

## Consuming on the UE side

In Blueprint or AngelScript, handle the navigation event:

```cpp
// On USwuiNavigation component, bind to OnNavigationEventWithPayload
// The K2Node_SwuiNavigationEvent deserializes JSON into the struct
```

## Files created/modified

- `Content/UI/generated/<InterfaceName>.navigation.generated.ts` — the generated contract with typed helpers
- `SwuiTypes.h` — `FSwuiNavigationEvent::PayloadStruct` field

## When NOT to use a payload struct

For events that carry no data (e.g. `onev.rooms.refresh`, `swui.navigation.cancel`), leave the payload struct empty. The generator emits `Record<string, never>` for the payload type.
