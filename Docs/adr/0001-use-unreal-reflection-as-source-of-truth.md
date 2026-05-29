# ADR 0001 — Use Unreal Reflection as Source of Truth

## Status

Accepted (2026-05-29)

## Context

SWUI needs to generate TypeScript interfaces from Unreal data types. There are two viable approaches:

1. **Separate schema files**: Define TS types in a `.ts` file that mirrors the C++ types, maintained manually or via a separate codegen tool.
2. **Unreal reflection**: Use UHT-generated `UProperty` metadata as the single source of truth for both the C++ runtime types and the generated TS types.

## Decision

Use Unreal reflection as the source of truth. All TS types are generated from `UClass`, `UFunction`, `UStruct`, and `FProperty` metadata at editor time.

## Consequences

**Positive:**
- C++ type definitions are the single definition — no sync issues between C++ and TS
- The generator runs in-editor or via commandlet (`-run=SwuiRegenerateBindings`)
- Adding a new UPROPERTY or UFUNCTION automatically appears in the generated TS
- `SwuiExpose` metadata provides opt-in control without separate config files
- Delegate payloads use the same `SignatureFunction` property iteration for both TS generation and runtime serialization

**Negative:**
- Requires the editor process to regenerate bindings (no standalone TS-only pipeline)
- UHT-generated types must be loaded before regeneration (handled by AssetRegistry scan)
- Some UE types have no clean TS equivalent (e.g., `TMap<FString, FString>` maps to `Record<string, string>`)

## Related ADRs

- ADR 0002 — Navigation payloads use UScriptStructs
- ADR 0003 — Runtime transport uses JSON
- ADR 0004 — K2 navigation events are typed only

## Files

- `SwuiTSGenerator.h` — `SwuiGetTSType()` maps UE `FProperty` types to TS types
- `SwuiBindingCollector.cpp` — discovers source classes
- `SwuiSubsystem.h/.cpp` — runtime property observation and delegate payload serialization
