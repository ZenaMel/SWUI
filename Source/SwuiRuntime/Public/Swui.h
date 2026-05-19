#pragma once

#include "Components/ActorComponent.h"
#include "Blueprint/UserWidget.h"
#include "SwuiBindingSource.h"
#include "SwuiTypes.h"
#include "Swui.generated.h"

USTRUCT(BlueprintType)
struct FSwuiDirtyUploadSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload")
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload")
	bool bEnableTileDiffForLargeRects = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload")
	bool bEnableUploadBudget = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload", meta=(ClampMin="8", ClampMax="1024"))
	int32 TileWidth = 128;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload", meta=(ClampMin="8", ClampMax="1024"))
	int32 TileHeight = 64;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload", meta=(ClampMin="1", ClampMax="1024"))
	int32 MinDirtyRectWidth = 32;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload", meta=(ClampMin="1", ClampMax="1024"))
	int32 MinDirtyRectHeight = 32;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload", meta=(ClampMin="1", ClampMax="1024"))
	int32 CenterCriticalWidth = 64;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload", meta=(ClampMin="1", ClampMax="1024"))
	int32 CenterCriticalHeight = 64;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload")
	bool bAlwaysProcessCenterCriticalRect = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload", meta=(ClampMin="0", ClampMax="16777216"))
	int32 MaxNormalUploadBytesPerFrame = 2 * 1024 * 1024;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload", meta=(ClampMin="1.0", ClampMax="10.0"))
	float MaxMergeWasteRatio = 1.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload", meta=(ClampMin="1", ClampMax="4096"))
	int32 MaxMergedRectWidth = 512;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload", meta=(ClampMin="1", ClampMax="4096"))
	int32 MaxMergedRectHeight = 256;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload", meta=(ClampMin="1", ClampMax="8388608"))
	int32 MaxMergedRectArea = 256 * 1024;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload")
	bool bForceFullBaselineUploadOnFirstPaint = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload")
	bool bUseRotatingDeferredTileCursor = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload")
	bool bLogSwuiPaintStats = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Dirty Upload")
	bool bShowSwuiDirtyRects = false;
};

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

	// ---------------------------------------------------------------------------
	// Rendering Mode — selects the SWUI render backend for this component.
	//
	//  Auto           – Uses the best supported rendering path for the current
	//                   platform. Prefers GPU Accelerated on Windows/D3D.
	//  GPU Accelerated – Uses GPU-backed rendering (CEF OnAcceleratedPaint with
	//                   shared D3D11 textures) for high-refresh HUDs and
	//                   animated UI. Zero CPU memcpy on the paint path.
	//  CPU Compatible – Uses the CPU bitmap rendering path (CEF OnPaint) for
	//                   compatibility and fallback behavior. Existing optimized
	//                   dirty-rect upload pipeline.
	// ---------------------------------------------------------------------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Performance|Rendering",
		meta=(DisplayName="Rendering Mode"))
	ESwuiRenderingMode RenderingMode = ESwuiRenderingMode::Auto;

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

	/** For HUD views, lock browser updates to UE-driven scheduling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Performance|HUD Frame Lock", meta=(EditCondition="bIsHUD"))
	bool bUseUEFrameLockedBrowser = true;

	/** Enable CEF external begin frames for HUD lock-step where available. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Performance|HUD Frame Lock", meta=(EditCondition="bIsHUD && bUseUEFrameLockedBrowser"))
	bool bUseExternalBeginFrames = true;

	/** Send external begin frames from subsystem Tick when external mode is active. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Performance|HUD Frame Lock", meta=(EditCondition="bIsHUD && bUseUEFrameLockedBrowser && bUseExternalBeginFrames"))
	bool bSendExternalBeginFrameFromTick = true;

	/** Flush queued HUD state/events before browser frame requests in lock-step mode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Performance|HUD Frame Lock", meta=(EditCondition="bIsHUD && bUseUEFrameLockedBrowser"))
	bool bFlushHudStateBeforeBrowserFrame = true;

	/** Browser frame cap for HUD mode. If UE runs above this, frames are paced to this cap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Performance", meta=(EditCondition="bIsHUD", ClampMin="1", ClampMax="300"))
	int32 MaxBrowserFramesPerSecond = 60;

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

	/** Force full-frame upload every tick, bypassing dirty rects, tile-priority uploads,
	 *  center-critical rect processing, transition heuristics, and upload cooldowns.
	 *  Also forces browser frame pumping so every frame produces a full CEF paint.
	 *  Uses the existing persistent texture path (no reallocation).
	 *  CVar: swui.DebugForceFullFrameUploadEveryFrame (0=off, 1=on) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Debug")
	bool bDebugForceFullFrameUploadEveryFrame = false;

	// ---- UI Resolution -----------------------------------------------------

	/** Internal UI render resolution preset. Controls the CEF render size and
	 *  SWUI texture size before HUD scaling. Lower resolutions = faster paint
	 *  copies at the cost of sharpness when scaled to the game viewport. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|UI Resolution")
	ESwuiUiResolutionPreset UiResolutionPreset = ESwuiUiResolutionPreset::Quality1080p;

	/** Custom UI width (only used when UiResolutionPreset is Custom). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|UI Resolution",
		meta=(EditCondition="UiResolutionPreset == ESwuiUiResolutionPreset::Custom", ClampMin="1"))
	int32 CustomUiWidth = 1920;

	/** Custom UI height (only used when UiResolutionPreset is Custom). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|UI Resolution",
		meta=(EditCondition="UiResolutionPreset == ESwuiUiResolutionPreset::Custom", ClampMin="1"))
	int32 CustomUiHeight = 1080;

	/** Focused SWUI dirty upload tuning (hybrid rect+tile path with center-critical lane). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Performance")
	FSwuiDirtyUploadSettings DirtyUploadSettings;

	/** HUD ROI — partial-surface rendering for HUD mode.
	 *  When enabled and menu input is inactive, only configured ROI rects are
	 *  copied and uploaded instead of the full CEF paint buffer.
	 *  Use the debug overlay (bShowOverlay) to tune during PIE. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Performance|HUD ROI",
		meta=(EditCondition="bIsHUD"))
	FSwuiHudRoiSettings HudRoiSettings;

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
