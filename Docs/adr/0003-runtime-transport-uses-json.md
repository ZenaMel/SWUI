# ADR 0003 — Runtime transport uses JSON

## Status

Accepted (2026-05-29)

## Context

Navigation events and delegate payloads both need a serialization format for passing data across the CEF process boundary. Options include:
1. **Raw UE struct binary**: Fast but not debuggable, platform-dependent, harder to construct from JS
2. **JSON**: Human-readable, well-supported in CEF/JS, debuggable in browser DevTools
3. **MsgPack/CBOR**: More compact but requires additional library

## Decision

Use JSON as the runtime transport format for all CEF↔UE communication. Specifically:
- JS emits via `Swui.emitNavigationEvent(tag, payload)` where `payload` is a JS object serialized to JSON via `JSON.stringify` inside the SWUI client
- UE deserializes via `FJsonObjectConverter::JsonObjectStringToUStruct` in the K2Node expander path
- Delegate payloads are serialized as JSON in the `ProcessEvent` override and dispatched as `document.dispatchEvent(new CustomEvent(name, {detail: json}))`

## Consequences

**Positive:**
- Debubugable in CEF DevTools (network tab, console)
- `JSON.stringify`/`JSON.parse` are native to JS — no extra library
- `FJsonObjectConverter` is part of UE Core — no extra dependency
- String payloads are trivially constructable from JS for testing

**Negative:**
- JSON is larger than binary formats
- No schema enforcement at runtime — a mismatched payload produces silent data loss (field missing from struct)
- Floats with high precision may lose accuracy during string conversion

## See also

- K2Node expander: `USwuiJsonBlueprintLibrary::JsonToStruct`
- Delegate serialization: `Swui_SerializePropertyValue` in `SwuiSubsystem.cpp`
