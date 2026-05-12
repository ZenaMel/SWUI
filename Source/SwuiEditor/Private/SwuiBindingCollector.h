#pragma once

#include "CoreMinimal.h"
#include "UObject/UnrealType.h"
#include "UObject/Field.h"
#include "Containers/Ticker.h"

class USwui;

// ── Binding origin classification ──────────────────────────────────────────

enum class ESwuiBindingOrigin : uint8
{
	Manual,
	CodeExposed,
	Both
};

// ── Effective binding data (one property or delegate in the effective set) ──

struct FSwuiEffectiveProperty
{
	FName PropName;
	ESwuiBindingOrigin Origin;
};

struct FSwuiEffectiveDelegate
{
	FName DelegateName;
	ESwuiBindingOrigin Origin;
};

struct FSwuiEffectiveSource
{
	UClass* SourceClass = nullptr;
	FString ObjectName;
	FString Namespace;
	TArray<FSwuiEffectiveProperty> Properties;
	TArray<FSwuiEffectiveDelegate> Delegates;
	bool bAutoDiscovered = false; // source found via SwuiExpose scanning, no manual BindingSource entry
};

// ── Complete effective binding set for one USwui asset ─────────────────────

struct FSwuiEffectiveBindings
{
	TArray<FSwuiEffectiveSource> Sources;
	TArray<FString> Warnings;
};

// ── Per-asset generation result ────────────────────────────────────────────

struct FSwuiGenerationResult
{
	FString AssetName;
	bool bSuccess = false;
	bool bHadWarnings = false;
	TArray<FString> Messages;
};

// ══════════════════════════════════════════════════════════════════════════
// Single source of truth — all binding logic lives here.
// ══════════════════════════════════════════════════════════════════════════

/**
 * Collect effective bindings for a single USwui asset.
 * Merges manual (saved checkboxes) + code-exposed (meta=SwuiExpose).
 * Used identically by the Details panel and the TS generator.
 */
FSwuiEffectiveBindings SwuiCollectEffectiveBindings(USwui* Bridge);

/**
 * Discover all live USwui instances across the project.
 * Covers: editor-world actor components, loaded Blueprint CDOs,
 * and any loaded USwui asset that is not a template/CDO.
 */
TArray<USwui*> SwuiFindAllSwuiAssets();

/**
 * Regenerate TS bindings for one USwui asset.
 * Returns the result including success/failure/warnings.
 */
FSwuiGenerationResult SwuiRegenerateAsset(USwui* Bridge);

/**
 * Regenerate TS bindings for ALL discovered USwui assets.
 * Logs results and returns aggregate success/failure.
 */
bool SwuiRegenerateAllBindings();

/**
 * Schedule a debounced auto-regeneration.
 * Call this from editor hooks (hot reload, module change, etc.).
 * Debounces at ~800ms to avoid bursty triggers.
 * Pass a human-readable reason for logging.
 *
 * If called during PIE, the regeneration is deferred until EndPIE.
 */
void SwuiScheduleAutoRegeneration(const FString& Reason);

// ── PIE deferral ────────────────────────────────────────────────────────────

/** True when regeneration was requested during PIE and is pending after PIE ends. */
extern bool GSwuiPendingRegenerationAfterPIE;

// ── AngelScript project detection ───────────────────────────────────────────

/**
 * Returns true if the project looks like an AngelScript project
 * (Script/ directory exists and contains *.as files).
 * Does not load any AS modules. Safe to call at any time.
 */
bool SwuiProjectLooksLikeAngelScriptProject();
