# ADR 0002 — Navigation payloads use UScriptStructs

## Status

Accepted (2026-05-29)

## Context

Navigation events carry typed payload data from JS to UE. The payload schema needs to be known by:
1. The TypeScript generator (for typed emit/listen helpers)
2. The K2Node expander (for the Blueprint struct pin type)
3. The runtime JSON deserializer (for converting the transport payload back to a C++ struct)

## Decision

Use `UScriptStruct` references (`TSoftObjectPtr<UScriptStruct>`) assigned to each `FSwuiNavigationEvent` entry. The same `UScriptStruct` drives:
- TS interface generation (field names and types from `FProperty` iteration)
- K2Node Blueprint pin type resolution
- Runtime JSON deserialization via `FJsonObjectConverter::JsonObjectStringToUStruct`

## Consequences

**Positive:**
- Single struct definition drives TS types, BP pins, and runtime behavior
- The struct picker in the editor UI shows all available `UScriptStruct` types
- `SwuiEvent` UFUNCTION metadata provides a lightweight alternative (no struct needed — function params become the schema)

**Negative:**
- Struct fields must be explicitly defined — no dynamic payloads
- Struct must be loaded (or soft-loaded via `TSoftObjectPtr`) before generation

## Files

- `SwuiTypes.h` — `FSwuiNavigationEvent::PayloadStruct`
- `SwuiTSGenerator.cpp` — `SwuiBuildStructInterface`
- `SwuiNavigationDetails.cpp` — PayloadStruct picker UI
- `K2Node_SwuiNavigationEvent.cpp` — `ResolvePayloadStruct()`
