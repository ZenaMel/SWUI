# SWUI concepts — the two generated files and what they mean

## Core mental model

SWUI generates two TypeScript files. Each represents one direction of communication:

| Generated file | Direction | Meaning |
|---|---|---|
| `<Interface>.generated.ts` | **Unreal → React** | "Game tells UI what happened / what state currently is." |
| `<Interface>.navigation.generated.ts` | **React → Unreal** | "UI tells game what the player clicked / requested." |

---

## `<Interface>.generated.ts` — observing Unreal from React

This file contains typed wrappers for game state and events exposed to the web UI. It is generated from `UPROPERTY` and `UFunction` members tagged with `meta=(SwuiExpose)`:

- State fields (`UPROPERTY`): synced to React every tick as observable values
- Event delegates (`FMulticastDelegateProperty`): fire CustomEvents in JS when the game broadcasts

### What goes in it

| Game concept | Unreal declaration |
|---|---|
| Status text | `UPROPERTY(meta=(SwuiExpose)) FString StatusMessage;` |
| Room list | `UPROPERTY(meta=(SwuiExpose)) FString RoomsJson;` |
| Boolean flags | `UPROPERTY(meta=(SwuiExpose)) bool bIsHosting;` |
| Result events | `UPROPERTY(BlueprintAssignable, meta=(SwuiExpose)) FOnRoomHostResult OnRoomHostResult;` |

### React usage

```ts
// Listen for a delegate event:
ComponentName.OnRoomHostResult(({ bSuccess }) => {
  // game told UI the host result
});

// Read state:
const status = Swui.get("componentname.StatusMessage");
```

### Mental model

```
UE state / delegate fires
  → serialized by SWUI bridge
  → pushed to window.__SWUI__.state
  → React reads via Swui.on() / generated typed helpers
```

Use this for: status display, list rendering, result feedback, flag-driven UI visibility.

---

## `<Interface>.navigation.generated.ts` — commands and events from React to Unreal

This file contains two subgroups:

| Section | Purpose |
|---|---|
| `SwuiCommands` | Direct UFUNCTION calls from React |
| `SwuiStandaloneEvents` | Generic typed events handled by K2/BP nodes |

---

### `SwuiCommands` — React calls an Unreal function directly

Generated from `UFUNCTION(meta=(SwuiCommand="tag.string"))` declarations. The function parameters become the typed payload. The tag string becomes the command identity.

#### Unreal declaration

```cpp
UFUNCTION(BlueprintCallable,
    meta = (SwuiCommand = "rooms.host"))
void HostRoom(const FString& RoomName, const FString& MapId, const FString& Mode, bool bIsPublic);
```

#### Generated TS

```ts
SwuiCommands.roomsHost({
  RoomName: string;
  MapId: string;
  Mode: string;
  bIsPublic: boolean;
}): void;
```

#### React usage

```ts
SwuiCommands.roomsHost({
  roomName: "test",
  mapId: "whitebox",
  mode: "duel",
  bIsPublic: true,
});
```

#### Mental model

```
React click/form
  → typed TS command
  → Swui.emitNavigationEvent(tag, payload)
  → CEF → USwuiView → USwuiNavigation::DispatchEvent
  → your UFUNCTION is called with the deserialized parameters
```

Use this for: actual game actions — host room, join room, set username, submit form, etc.

---

### `SwuiStandaloneEvents` — React fires a typed K2/BP event

Configured through the `USwuiNavigation` component's `NavigationEvents` array (or via `meta=(SwuiEvent="tag.string")`). These are NOT direct function calls — they fire a typed Blueprint event.

#### Examples

| Event tag | Meaning |
|---|---|
| `swui.menu.back` | Navigate back to previous screen |
| `swui.navigation.confirm` | Confirm current selection |
| `swui.menu.open` | Open main menu |
| `swui.menu.close` | Close main menu |

#### React usage

```ts
SwuiStandaloneEvents.menuBack();
SwuiStandaloneEvents.menuOpen();

// With a typed payload:
SwuiStandaloneEvents.roomsHost({
  roomName: "test",
  mapId: "whitebox",
});
```

#### Blueprint handling

In Blueprint, add a `Swui Navigation Event` node. It expands at compile time into:
1. Tag comparison
2. JSON deserialization into the correct struct
3. A typed payload output pin

#### Mental model

```
React emits standalone event
  → SWUI JSON transport
  → typed K2/BP event node
  → your Blueprint graph handles the typed struct
```

Use these for: generic UI navigation, actions that are handled in Blueprint rather than C++.

---

## Summary

```
<Interface>.generated.ts          = game → UI
<Interface>.navigation.generated.ts = UI → game

  ┌─ SwuiCommands          = UI calls a C++ function directly
  └─ SwuiStandaloneEvents  = UI fires a typed K2/BP event
```

## Related

- [architecture/navigation-events.md](../architecture/navigation-events.md) — how the transport works under the hood
- [architecture/delegate-payloads.md](../architecture/delegate-payloads.md) — how delegate params become typed CustomEvents
- [usage/typed-navigation-payloads.md](../usage/typed-navigation-payloads.md) — how to define commands and events
