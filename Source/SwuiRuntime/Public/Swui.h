#pragma once

#include "Components/ActorComponent.h"
#include "Blueprint/UserWidget.h"
#include "SwuiBindingSource.h"
#include "Swui.generated.h"

/**
 * Minimal concrete UUserWidget subclass used to host the CEF render surface.
 * UUserWidget::StaticClass() is abstract-flagged in some UE builds; this
 * concrete subclass avoids the CreateWidget assertion.
 */
UCLASS()
class SWUIRUNTIME_API USwuiWidget : public UUserWidget
{
	GENERATED_BODY()
};

/**
 * USwui — Add this to any Actor to configure and launch a SimpleWebUI surface.
 * Place on a PlayerController for a HUD, or any Actor for world-space surfaces.
 *
 * To sync game state into the web UI, call "SWUI Observe" Blueprint nodes on
 * any object in the game world — no component needed on those objects.
 */
UCLASS(ClassGroup=Swui, Blueprintable, meta=(BlueprintSpawnableComponent))
class SWUIRUNTIME_API USwui : public UActorComponent
{
	GENERATED_BODY()

public:
	USwui();
	void EnsureOwnerBindingSource();

	// Used as the TypeScript interface name and generated file prefix.
	// e.g. "PlayerHUD" → Content/UI/generated/PlayerHUD.generated.ts
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI")
	FString InterfaceName = TEXT("MyHUD");

	// URI to load. Bare paths and swui:// resolve under Content/ (.html implicit).
	// http://, https://, and localhost URIs are passed through directly.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI")
	FString DefaultURI;

	// When true the surface automatically matches the game render resolution.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI")
	bool bIsHUD = true;

	// Manual resolution — ignored when bIsHUD is true.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI", meta=(EditCondition="!bIsHUD"))
	int32 ViewWidth = 1280;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI", meta=(EditCondition="!bIsHUD"))
	int32 ViewHeight = 720;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI")
	int32 ZOrder = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI")
	UMaterialInterface* BaseMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="SimpleWebUI")
	FName TextureParameterName = TEXT("SwuiTexture");

	// ---- Bindings ----------------------------------------------------------
	// One entry per class you are observing (Character, Weapon, PlayerController, etc.).
	// First entry is auto-populated with the owner actor's class.
	// Each entry has its own property checklist. Used for TypeScript codegen only.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Bindings")
	TArray<FSwuiBindingSource> BindingSources;

	// ---- Performance | Frame Rate ------------------------------------------

	/** Override the per-browser windowless frame rate for this component.
	 *  0 = use the project-wide DefaultViewFrameRate setting (or engine MaxFPS / 300).
	 *  Set to your monitor refresh rate: 144, 165, 240, etc. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Performance",
		meta=(ClampMin="0", ClampMax="300"))
	int32 OverrideFrameRate = 0;

	// ---- Performance | Upload Strategy ------------------------------------

	/** Override the band-merge overcopy ratio for this component.
	 *  0 = use the project-wide MaxBandOvercopyRatio setting (default 1.25).
	 *  When (bandArea / dirtyArea) > this value, per-rect tight uploads are used instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Performance",
		meta=(ClampMin="0.0", ClampMax="10.0"))
	float OverrideBandOvercopyRatio = 0.f;

	/** Override the max per-rect upload count for this component.
	 *  0 = use the project-wide MaxPerRectUploads setting (default 32). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Performance",
		meta=(ClampMin="0", ClampMax="256"))
	int32 OverrideMaxPerRectUploads = 0;

	// ---- Debug -------------------------------------------------------------

	/** Log upload strategy and timing details for every paint call on this component.
	 *  WARNING: spammy at 144+ Hz. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Debug")
	bool bVerbosePaintLog = false;

	/** Skip all RHIUpdateTexture2D calls on this component to isolate CPU memcpy cost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Debug")
	bool bNoTextureUpload = false;

	// ---- Debug | Stage Isolation (enable one at a time to bisect stutter) ---

	/** Return at the very top of OnPaint after incrementing the paint counter.
	 *  Isolates: everything downstream of CEF's paint callback. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Debug")
	bool bSkipOnPaintProcessing = false;

	/** Skip dirty-rect validation, strategy selection, memcpy, and upload.
	 *  Isolates: rect processing overhead (strategy logic itself). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Debug")
	bool bSkipDirtyRectStrategy = false;

	/** Skip copying pixels into the upload buffer (memcpy stage).
	 *  Isolates: memory bandwidth cost of packing dirty rect data. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Debug")
	bool bSkipPaintMemcpy = false;

	/** Skip the RHIUpdateTexture2D enqueue (clears bNoTextureUpload confusion).
	 *  Isolates: GPU upload / render-thread texture update cost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Debug")
	bool bSkipTextureUpload = false;

	/** Keep the last uploaded texture; skip all new texture writes.
	 *  Isolates: whether stutter is caused by texture updates vs. other work. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Debug")
	bool bFreezeTexture = false;

	/** Skip JS state-push and runtime tick dispatch for this component.
	 *  Isolates: JavaScript execution overhead from Unreal→Web data sync. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Debug")
	bool bPauseBrowserUpdates = false;

	/** Hide the UE-side widget/material draw surface for this component.
	 *  Isolates: Slate/UMG render cost of compositing the web texture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Debug")
	bool bHideDrawComponent = false;

	/** Push dirty-rect and paint-stats data to window.__SWUI_DEBUG_RECTS__ in the browser at ~10 Hz.
	 *  Enables the in-browser colored overlay showing which areas CEF dirtied each frame.
	 *  NOTE: the overlay itself adds its own paint cost. Use logs for final measurements. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Debug")
	bool bShowDirtyRectOverlay = false;

	// Stop syncing all observed properties/events for a source object.
	// Call from PlayerController's OnUnPossess, passing the old pawn.
	UFUNCTION(BlueprintCallable, Category="SimpleWebUI", meta=(DefaultToSelf="Source"))
	void Unobserve(UObject* Source);

	// Show or hide the web surface. The CEF browser keeps running in the background.
	// Use to suppress the HUD during cutscenes, loading screens, etc.
	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void SetHUDVisible(bool bVisible);

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void ShowHUD() { SetHUDVisible(true); }

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void HideHUD() { SetHUDVisible(false); }

private:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// Deferred init: called on next tick (and retried) until viewport is ready.
	void InitializeSwuiView();

#if WITH_EDITOR
	/** Push updated settings to the running view when a property is changed
	 *  in the Details panel during PIE — no restart required. */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
