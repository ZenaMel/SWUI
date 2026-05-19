#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "SwuiTypes.generated.h"

// ---------------------------------------------------------------------------
// Rendering mode — selects the SWUI render backend per component instance.
//
//  Auto           – prefers GPU Accelerated on supported Windows/D3D setups,
//                   falls back to CPU Compatible with a log when unavailable.
//  GpuAccelerated – uses CEF OnAcceleratedPaint with shared D3D11 textures.
//                   Zero-copy GPU path for high-refresh HUDs and animated UI.
//  CpuCompatible  – uses the existing CEF OnPaint CPU BGRA bitmap path.
//                   Compatibility fallback; works on all platforms.
// ---------------------------------------------------------------------------
UENUM(BlueprintType)
enum class ESwuiRenderingMode : uint8
{
	Auto            UMETA(DisplayName = "Auto"),
	GpuAccelerated  UMETA(DisplayName = "GPU Accelerated"),
	CpuCompatible   UMETA(DisplayName = "CPU Compatible")
};

UENUM(BlueprintType)
enum class ESwuiLowLatencyFramePacingMode : uint8
{
	Disabled                  UMETA(DisplayName = "Disabled"),
	WhileInteractiveUiActive  UMETA(DisplayName = "While Interactive UI Active"),
	WhileAnySwuiViewActive    UMETA(DisplayName = "While Any SWUI View Active")
};

UENUM(BlueprintType)
enum class ESwuiUiResolutionPreset : uint8
{
	Performance720p UMETA(DisplayName = "Performance - 1280x720"),
	Balanced900p    UMETA(DisplayName = "Balanced - 1600x900"),
	Quality1080p    UMETA(DisplayName = "Quality - 1920x1080"),
	High1440p       UMETA(DisplayName = "High - 2560x1440"),
	NativeViewport  UMETA(DisplayName = "Native Viewport"),
	Custom          UMETA(DisplayName = "Custom")
};

// ---------------------------------------------------------------------------
// Navigation enums — used by USwuiNavigation for menu/input routing.
// ---------------------------------------------------------------------------

UENUM(BlueprintType)
enum class ESwuiNavDirection : uint8
{
	Up       UMETA(DisplayName = "Up"),
	Down     UMETA(DisplayName = "Down"),
	Left     UMETA(DisplayName = "Left"),
	Right    UMETA(DisplayName = "Right"),
	Next     UMETA(DisplayName = "Next"),
	Previous UMETA(DisplayName = "Previous")
};

UENUM(BlueprintType)
enum class ESwuiPointerButton : uint8
{
	Left   UMETA(DisplayName = "Left"),
	Right  UMETA(DisplayName = "Right"),
	Middle UMETA(DisplayName = "Middle")
};

UENUM(BlueprintType)
enum class ESwuiInputMode : uint8
{
	HudOnly   UMETA(DisplayName = "HUD Only"),
	UiOnly    UMETA(DisplayName = "UI Only"),
	GameAndUi UMETA(DisplayName = "Game and UI")
};

// ---------------------------------------------------------------------------
// Navigation event — Gameplay Tag based, configurable per-event routing.
// ---------------------------------------------------------------------------

USTRUCT(BlueprintType)
struct FSwuiNavigationEvent
{
	GENERATED_BODY()

	/** Navigation event tag used by Unreal, Blueprint, and JS. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation",
		meta=(ToolTip="Navigation event tag used by Unreal, Blueprint, and JS."))
	FGameplayTag Event;

	/** JS event name. Defaults to the navigation tag name, e.g. swui.menu.open. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation",
		meta=(DisplayName="JS Event Name", ToolTip="JS event name. Defaults to the navigation tag name."))
	FString JsEventName;

	/** Forward this event to the SWUI web view. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation",
		meta=(DisplayName="Forward to JS", ToolTip="Forward this navigation event to the SWUI web view."))
	bool bForwardToJS = true;

	/** Trigger Blueprint callbacks for this event. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation",
		meta=(ToolTip="Trigger Blueprint callbacks for this navigation event."))
	bool bBlueprintCallback = true;

	/** Returns the effective JS event name — the configured JsEventName, or the tag string. */
	FString GetEffectiveJsEventName() const
	{
		return JsEventName.IsEmpty()
			? (Event.IsValid() ? Event.ToString() : FString())
			: JsEventName;
	}
};

UENUM(BlueprintType)
enum class ESwuiHudRoiMode : uint8
{
	UniformEdges    UMETA(DisplayName = "Uniform Edges"),
	IndividualEdges UMETA(DisplayName = "Individual Edges")
};

USTRUCT(BlueprintType)
struct FSwuiHudRoiSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|HUD ROI")
	bool bEnabled = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|HUD ROI")
	ESwuiHudRoiMode Mode = ESwuiHudRoiMode::UniformEdges;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|HUD ROI",
		meta=(EditCondition="Mode==ESwuiHudRoiMode::UniformEdges", ClampMin="0", ClampMax="100"))
	int32 UniformEdgePercent = 25;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|HUD ROI",
		meta=(EditCondition="Mode==ESwuiHudRoiMode::IndividualEdges", ClampMin="0", ClampMax="100"))
	int32 TopPercent = 10;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|HUD ROI",
		meta=(EditCondition="Mode==ESwuiHudRoiMode::IndividualEdges", ClampMin="0", ClampMax="100"))
	int32 BottomPercent = 10;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|HUD ROI",
		meta=(EditCondition="Mode==ESwuiHudRoiMode::IndividualEdges", ClampMin="0", ClampMax="100"))
	int32 LeftPercent = 5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|HUD ROI",
		meta=(EditCondition="Mode==ESwuiHudRoiMode::IndividualEdges", ClampMin="0", ClampMax="100"))
	int32 RightPercent = 5;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|HUD ROI")
	bool bCenterRoiEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|HUD ROI", meta=(ClampMin="0", ClampMax="100"))
	int32 CenterRoiPercent = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|HUD ROI|Debug")
	bool bShowOverlay = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|HUD ROI|Debug")
	bool bShadeInactiveArea = true;
};

// Per-instance rendering overrides forwarded from USwui → InitRenderer → USwuiView::Init().
// A value of 0 / 0.f means "use the project-wide USwuiSettings default".
struct FSwuiInstanceSettings
{
	// Rendering backend selection (Auto / GPU Accelerated / CPU Compatible).
	ESwuiRenderingMode RenderingMode = ESwuiRenderingMode::Auto;

	bool  bIsHUD                          = false;
	bool  bUseUEFrameLockedBrowser        = true;
	bool  bUseExternalBeginFrames         = true;
	bool  bSendExternalBeginFrameFromTick = true;
	bool  bFlushHudStateBeforeBrowserFrame = true;
	int32 MaxBrowserFramesPerSecond       = 60;

	int32 OverrideFrameRate        = 0;    // 0 = use project setting / engine MaxFPS
	float OverrideBandOvercopyRatio = 0.f; // 0 = use project setting (1.25)
	int32 OverrideMaxPerRectUploads = 0;   // 0 = use project setting (32)
	bool  bVerbosePaintLog          = false;
	bool  bNoTextureUpload          = false;

	// Stage-level isolation flags (all false = normal operation)
	bool  bSkipOnPaintProcessing    = false; // return at top of OnPaint after counting
	bool  bSkipDirtyRectStrategy    = false; // skip rect validation + strategy, no upload
	bool  bSkipPaintMemcpy          = false; // skip pixel copy into upload buffers
	bool  bSkipTextureUpload        = false; // skip RHIUpdateTexture2D enqueue (alias for bNoTextureUpload)
	bool  bFreezeTexture            = false; // keep last texture, skip new uploads
	bool  bPauseBrowserUpdates      = false; // skip JS state push and runtime tick dispatch
	bool  bHideDrawComponent        = false; // hide the UE widget/material draw surface
	bool  bShowDirtyRectOverlay     = false; // push dirty rects + stats to __SWUI_DEBUG_RECTS__ at ~10 Hz
	bool  bDebugForceFullFrameUploadEveryFrame = false; // bypass all optimisations, upload full texture every frame

	// HUD ROI settings for partial-surface rendering.
	FSwuiHudRoiSettings HudRoiSettings;

	// UI resolution preset — internal render size before HUD scaling.
	ESwuiUiResolutionPreset UiResolutionPreset = ESwuiUiResolutionPreset::Quality1080p;
	int32 CustomUiWidth  = 1920;
	int32 CustomUiHeight = 1080;

	// Focused hybrid upload-path tuning
	bool  bEnableHybridDirtyUpload             = true;
	bool  bEnableTileDiffForLargeRects         = true;
	bool  bEnableUploadBudget                  = true;
	int32 TileWidth                            = 128;
	int32 TileHeight                           = 64;
	int32 MinDirtyRectWidth                    = 32;
	int32 MinDirtyRectHeight                   = 32;
	int32 CenterCriticalWidth                  = 64;
	int32 CenterCriticalHeight                 = 64;
	bool  bAlwaysProcessCenterCriticalRect     = true;
	int32 MaxNormalUploadBytesPerFrame         = 2 * 1024 * 1024;
	float MaxMergeWasteRatio                   = 1.15f;
	int32 MaxMergedRectWidth                   = 512;
	int32 MaxMergedRectHeight                  = 256;
	int32 MaxMergedRectArea                    = 256 * 1024;
	bool  bForceFullBaselineUploadOnFirstPaint = true;
	bool  bUseRotatingDeferredTileCursor       = true;
	bool  bLogSwuiPaintStats                   = true;
	bool  bShowSwuiDirtyRects                  = false;
};

// Descriptor for one dirty rect within a shared packed pixel buffer.
// SrcX/SrcY in Region are always 0 — data starts at SrcOffsetBytes in the shared buffer.
struct FSwuiPackedRectDesc
{
	FUpdateTextureRegion2D Region;    // DestX/DestY = texture destination; SrcX=SrcY=0
	uint32 SrcPitch       = 0;        // tight row stride = Width * 4
	int32  SrcOffsetBytes = 0;        // byte offset into FSwuiPaintUploadData::PackedPixels
};

// Single-allocation upload payload for the per-rect path.
// All rects are packed sequentially into one PackedPixels buffer — no per-rect heap alloc.
struct FSwuiPaintUploadData
{
	FTextureResource*           Texture2DResource = nullptr;
	TArray<uint8>               PackedPixels;  // all rect data end-to-end
	TArray<FSwuiPackedRectDesc> Rects;
};

// ---------------------------------------------------------------------------
// HUD ROI overlay state — consumed by the debug overlay widget each frame.
// Contains the same rects used by the renderer, plus display info.
// ---------------------------------------------------------------------------
USTRUCT(BlueprintType)
struct FSwuiHudRoiOverlayState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	bool bVisible = false;

	UPROPERTY(BlueprintReadOnly)
	bool bHudRoiModeActive = false;

	/** All outer ROI rects (1 for manual, 4 edge bands for percentage frame). */
	UPROPERTY(BlueprintReadOnly)
	TArray<FIntRect> OuterRoiRects;

	/** Center ROI rect (0 or 1). */
	UPROPERTY(BlueprintReadOnly)
	TArray<FIntRect> CenterRoiRects;

	/** Combined active rects — same list the renderer uploads. */
	UPROPERTY(BlueprintReadOnly)
	TArray<FIntRect> ActiveRoiRects;

	/** Union area of all active rects as percentage of total texture area. */
	UPROPERTY(BlueprintReadOnly)
	float RoiAreaPercent = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	FString ModeLabel;
};

class ISwuiRenderTarget
{
public:
	virtual ~ISwuiRenderTarget() = default;
	virtual void OnPaint(const void* Buffer, FUpdateTextureRegion2D* Regions, int32 RegionCount, int32 Width, int32 Height) = 0;
	virtual UTexture2D* GetOrCreateTexture(int32 Width, int32 Height) = 0;
};

// ---------------------------------------------------------------------------
// Accelerated paint target interface — implemented by USwuiView when the
// GPU Accelerated backend is active.
//
// Thread: OnAcceleratedPaint is called on the CEF renderer thread.
// The shared texture handle (a D3D11 HANDLE on Windows) is only valid for
// the duration of the call — the callee must open/copy the resource before
// returning.
// ---------------------------------------------------------------------------
class ISwuiAcceleratedRenderTarget
{
public:
	virtual ~ISwuiAcceleratedRenderTarget() = default;
	virtual void OnAcceleratedPaint(void* SharedHandle, int32 Width, int32 Height) = 0;
};
