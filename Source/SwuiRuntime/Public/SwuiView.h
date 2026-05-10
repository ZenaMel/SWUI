#pragma once

#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "UObject/Object.h"
#include "SwuiTypes.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Containers/BitArray.h"
#include "SwuiView.generated.h"

struct FSwuiViewCefData;
class AActor;
struct FKey;

UCLASS(ClassGroup=Swui, Blueprintable)
class SWUIRUNTIME_API USwuiView : public UObject, public ISwuiRenderTarget, public ISwuiAcceleratedRenderTarget
{
	GENERATED_BODY()

public:
	USwuiView();

	// Internal URL loaded by the backend adapter — not set directly by users.
	FString DefaultURL;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	int32 Width = 1280;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	int32 Height = 720;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	bool bIsTransparent = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	UMaterialInterface* BaseMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="SwuiRuntime")
	FName TextureParameterName = "SwuiTexture";

	void Init(const FSwuiInstanceSettings& InInstanceSettings = FSwuiInstanceSettings{});

	// Returns the CEF windowless frame rate this view was initialised with.
	int32 GetWindowlessFrameRate() const { return WindowlessFrameRate; }

	// Internal — called by the backend adapter, not directly from Blueprint.
	void LoadURL(const FString& URI);

	// Internal — called by the backend adapter, not directly from Blueprint.
	void ExecuteJavaScript(const FString& Script);
	void SetOwningActor(AActor* InOwningActor) { OwningActor = InOwningActor; }
	bool HandleIncomingMessage(const FString& MessageJson);

	UFUNCTION(BlueprintCallable, Category="SwuiRuntime")
	UTexture2D* GetTexture() const;

	// ISwuiRenderTarget (CPU Compatible path)
	virtual void OnPaint(const void* Buffer, FUpdateTextureRegion2D* Regions, int32 RegionCount, int32 Width, int32 Height) override;
	virtual UTexture2D* GetOrCreateTexture(int32 InWidth, int32 InHeight) override;

	// ISwuiAcceleratedRenderTarget (GPU Accelerated path)
	virtual void OnAcceleratedPaint(void* SharedHandle, int32 InWidth, int32 InHeight) override;

	// The rendering mode resolved at Init() time (never Auto — always concrete).
	ESwuiRenderingMode GetResolvedRenderingMode() const { return ResolvedRenderingMode; }

	virtual void BeginDestroy() override;

	// Called every UE frame by USwuiSubsystem::Tick — flushes pending CEF paints
	// as a single per-frame upload, with tile diff to skip unchanged tiles.
	void TickDeferredUpload();
	bool FlushHudStateAndRequestBrowserFrame(const FString& CombinedScript, float DeltaTime, bool bForceFrame);
	bool SendExternalBeginFrameIfDue(float DeltaTime);
	void NotifySubsystemTick();
	void NotifyHudStateFlushed();
	bool IsExternalBeginFrameActive() const { return bExternalBeginFrameActive; }
	bool HasPaintAfterExternalBeginFrame() const { return bPaintArrivedAfterExternalBeginFrame; }
	bool HasFreshOnPaintDataPending() const;
	void RequestFullTextureUploadNextFrame();

	// Per-instance settings forwarded from USwui component at Init() time.
	// Kept public so USwuiSubsystem can read isolation flags (e.g. bPauseBrowserUpdates).
	FSwuiInstanceSettings InstanceSettings;

	// ---- Pointer Input Forwarding ----

	/** Enable/disable automatic CEF pointer event forwarding. */
	void SetPointerInputEnabled(bool bEnabled);

	/** Whether pointer input forwarding is currently active. */
	bool IsPointerInputEnabled() const { return bPointerInputEnabled; }

	/** Returns whether the CEF browser host is available for forwarding. */
	bool HasBrowserHost() const;

	/** Convert a Slate screen-space position to CEF browser pixel coordinates.
	 *  Returns false when the position is outside the SWUI browser area. */
	bool ScreenToBrowserPixel(const FVector2D& ScreenPos, int32& OutX, int32& OutY) const;

	/** Forward a mouse-move event to the CEF browser.
	 *  Returns true if the event was forwarded. */
	bool ForwardMouseMoveToBrowser(const FVector2D& ScreenPosition);

	/** Forward a mouse-button (down/up) event to CEF.
	 *  @param Button      Unreal FKey (LeftMouseButton, RightMouseButton, MiddleMouseButton)
	 *  @param bMouseUp    true = mouse up, false = mouse down
	 *  @param ClickCount  1 = single, 2 = double
	 *  Returns true if the event was forwarded. */
	bool ForwardMouseButtonToBrowser(const FVector2D& ScreenPosition, FKey Button, bool bMouseUp, int32 ClickCount = 1);

	/** Forward a mouse-wheel event to CEF.
	 *  @param DeltaX, DeltaY  Unreal wheel delta values (e.g. from FPointerEvent::GetWheelDelta)
	 *  Returns true if the event was forwarded. */
	bool ForwardMouseWheelToBrowser(const FVector2D& ScreenPosition, float DeltaX, float DeltaY);

	/** Set/unset CEF browser keyboard/mouse focus. */
	void SetBrowserInputFocus(bool bFocused);

	// ---- Render Activity API ----

	enum class ESwuiRenderActivityMode
	{
		NormalHud,
		FullTransition,
		InteractiveUi
	};

	/** Begins a short FullTransition burst: forces full CEF surface copies into
	 *  the BackingBuffer, uploads the full surface for FreshPaintCount frames,
	 *  and suppresses center-critical/tile optimization paths. */
	void BeginFullTransitionRefresh(int32 FreshPaintCount = 3);

	/** Marks whether UI interaction (menu/inventory/etc.) is active.
	 *  When true, center-critical rect is disabled and upload budget is relaxed. */
	void SetUiInteractionActive(bool bActive);

	/** Returns the current render activity mode. */
	ESwuiRenderActivityMode GetRenderActivityMode() const { return RenderActivityMode; }

	/** Whether UI interaction is currently active. */
	bool IsUiInteractionActive() const { return bUiInteractionActive; }

private:

	/** Whether pointer events should be forwarded to CEF. */
	bool bPointerInputEnabled = false;

	/** Current render activity mode. */
	ESwuiRenderActivityMode RenderActivityMode = ESwuiRenderActivityMode::NormalHud;

	/** Whether UI interaction (menu/inventory/etc.) is active. */
	bool bUiInteractionActive = false;

	// Pending full CEF copies from OnPaint to BackingBuffer.
	int32 PendingFullCefPaintCopies = 0;

	// --- Browser frame pacer ---
	AActor* ResolveOwningActor() const;
	TWeakObjectPtr<AActor> OwningActor;
	double LastBrowserFrameTime = 0.0;
	double LastDirtyTime = 0.0;
	bool bBrowserAnimating = false;
	bool bBrowserDirty = false;
	double BrowserFrameTimeout = 0.100; // 100ms safety
	UPROPERTY()
	UTexture2D* Texture;

	UMaterialInstanceDynamic* MaterialInstance;

	TSharedPtr<FSwuiViewCefData> CefData;

	int32 WindowlessFrameRate = 300;
	bool bExternalBeginFrameActive = false;
	int32 AppliedHudLockstepCVar = -1;
	int32 AppliedHudExternalBeginFramesCVar = -1;
	int32 AppliedHudMaxBrowserFPSCVar = -1;
	int32 LastObservedHudLockstepCVar = MIN_int32;
	int32 LastObservedHudExternalBeginFramesCVar = MIN_int32;
	int32 LastObservedHudMaxBrowserFPSCVar = MIN_int32;
	double LastExternalBeginFrameSentTime = 0.0;
	double ExternalBeginFrameAccumulatedTime = 0.0;
	double PendingBeginFrameSentTime = -1.0;
	double LastPaintArrivalTime = 0.0;
	bool bPaintArrivedAfterExternalBeginFrame = false;
	bool bPendingInvalidateForPaint = false;

	// -----------------------------------------------------------------------
	// Coalescing: CEF OnPaint accumulates here; TickDeferredUpload drains.
	// ALL fields in this block are guarded by PaintMutex.
	// Written from CEF renderer thread, read/swapped from game thread.
	// -----------------------------------------------------------------------
	FCriticalSection               PaintMutex;
	TArray<uint8>                  BackingBuffer;          // full W*H*4 latest CEF pixels
	TArray<FIntRect>               PendingDirtyRects;      // accumulated since last Tick
	TArray<FUpdateTextureRegion2D> PendingOverlayRects;    // for debug overlay (optional)
	int32                          PendingCefPaints       = 0;
	int32                          PendingIncomingRects   = 0;
	int64                          PendingIncomingPx      = 0;
	int32                          PendingLargestIncoming = 0;
	bool                           bHasPendingUpload      = false;

	// Monotonic fresh-paint tracking.
	// Used by the subsystem to know whether CEF has produced new paint data
	// that has not yet been drained by TickDeferredUpload.
	uint64                         PendingFreshPaintGeneration = 0;
	uint64                         DrainedFreshPaintGeneration = 0;
	double                         PendingFreshPaintArrivalTime = 0.0;

	// Game-thread paint generation counter, incremented when TickDeferredUpload
	// drains a fresh batch of CEF paint. Used to gate forced full uploads.
	uint64                         PaintGeneration = 0;

	// -----------------------------------------------------------------------
	// Tile diff state — game thread only, no mutex needed.
	// -----------------------------------------------------------------------
	int32          TilesX         = 0;
	int32          TilesY         = 0;
	TBitArray<>    ActiveDirtyTileMask; // persistent pending large/fullscreen tile work
	TBitArray<>    UploadedTileMask;     // tiles already known to have valid texture content
	TArray<uint32> LastTileHashes; // TilesX*TilesY entries; ~0u = not yet hashed
	int32          DirtyTileScanCursor = 0;
	int32          LastSnapW      = 0;
	int32          LastSnapH      = 0;
	bool           bSeenFirstPaint = false;
	bool           bNeedsFullBaselineUpload = true;
	int32          SuppressCenterCriticalRectFrames = 0;
	uint64         ForcedUploadRequestedAtPaintGeneration = 0;
	int32          PendingFreshFullUploads = 0;
	bool           bAwaitingFreshPaintForForcedUpload = false;

	// -----------------------------------------------------------------------
	// Aggregate stats — game thread only, reset every second in TickDeferredUpload.
	// -----------------------------------------------------------------------
	int32  Stat_CefPaints       = 0;   // # CEF OnPaint calls
	int32  Stat_SubsystemTicks  = 0;   // # USwuiSubsystem::Tick calls
	int32  Stat_ViewUploadTicks = 0;   // # TickDeferredUpload calls
	int32  Stat_ExternalBeginFrames = 0;
	int32  Stat_ExternalBeginFrameSkipInactive = 0;
	int32  Stat_ExternalBeginFrameSkipDisabled = 0;
	int32  Stat_ExternalBeginFrameSkipNoBrowser = 0;
	int32  Stat_ExternalBeginFrameSkipRateLimited = 0;
	int32  Stat_InvalidateView = 0;
	int32  Stat_BeginFramesWithoutPaint = 0;
	int32  Stat_PaintsAfterInvalidate = 0;
	int32  Stat_HudStateFlushes = 0;
	int32  Stat_UeUploads       = 0;   // # TickDeferredUpload enqueue calls
	int32  Stat_UeUploadsFresh  = 0;
	int32  Stat_UeUploadsBacklog = 0;
	int32  Stat_IncomingRects   = 0;   // total dirty rects from CEF
	int64  Stat_IncomingPx      = 0;   // total dirty pixels from CEF
	int32  Stat_LargestIncoming = 0;   // largest single CEF dirty rect area (px)
	int32  Stat_CandidateRects  = 0;   // rects entering greedy merge
	int64  Stat_CandidatePx     = 0;
	int32  Stat_UploadedRects   = 0;
	int64  Stat_UploadedPixels  = 0;
	int32  Stat_LargestUploaded = 0;
	int32  Stat_SkippedTiles    = 0;
	int32  Stat_ChangedTiles    = 0;
	int32  Stat_DeferredTiles   = 0;
	double Stat_DeferredUploadMsSum = 0.0;
	double Stat_DeferredUploadMsMax = 0.0;
	int32  Stat_DeferredUploadSamples = 0;
	double Stat_HashMsSum           = 0.0;
	double Stat_HashMsMax           = 0.0;
	int32  Stat_HashSamples         = 0;
	double Stat_PackMemcpyMsSum     = 0.0;
	double Stat_PackMemcpyMsMax     = 0.0;
	int32  Stat_PackMemcpySamples   = 0;
	double Stat_PaintAfterBeginFrameMsSum = 0.0;
	double Stat_PaintAfterBeginFrameMsMax = 0.0;
	int32  Stat_PaintAfterBeginFrameSamples = 0;
	double Stat_UploadAfterPaintMsSum = 0.0;
	double Stat_UploadAfterPaintMsMax = 0.0;
	int32  Stat_UploadAfterPaintSamples = 0;
	double Stat_LockWaitMs       = 0.0;
	int64  Stat_MemcpyUs        = 0;
	int64  Stat_MemcpyMaxUs     = 0;
	double Stat_LastLogTime     = 0.0;
	double Stat_OverlayLastPushTime = 0.0;

	// -----------------------------------------------------------------------
	// GPU Accelerated backend state
	// -----------------------------------------------------------------------

	// Concrete mode resolved from Auto / explicit at Init() time.
	ESwuiRenderingMode ResolvedRenderingMode = ESwuiRenderingMode::CpuCompatible;

	// Reason string logged when Auto falls back to CPU Compatible.
	FString GpuFallbackReason;

	// --- Accelerated paint coalescing (CEF thread → game thread → render thread) ---
	// Guarded by AccelPaintMutex.
	FCriticalSection               AccelPaintMutex;
	void*                          PendingSharedHandle = nullptr;
	int32                          PendingAccelWidth   = 0;
	int32                          PendingAccelHeight  = 0;
	bool                           bHasPendingAccelPaint = false;
	uint64                         AccelPaintGeneration = 0;
	uint64                         AccelDrainedGeneration = 0;

	// --- GPU stats (game thread only) ---
	int32  Stat_AccelPaints         = 0;   // OnAcceleratedPaint calls/s
	int32  Stat_AccelCopies         = 0;   // successful GPU copies/s
	int32  Stat_AccelHandleFails    = 0;   // shared handle open failures/s
	int32  Stat_AccelTexRecreates   = 0;   // texture recreations
	int32  Stat_AccelResizes        = 0;   // resize events
	double Stat_AccelCopyMsSum      = 0.0;
	double Stat_AccelCopyMsMax      = 0.0;
	int32  Stat_AccelCopySamples    = 0;

	// Drains the latest accelerated paint from the CEF thread and issues
	// a render-thread GPU copy into the persistent UE texture.
	void TickAcceleratedUpload();

	// Checks if the current platform/RHI supports the GPU Accelerated path.
	static bool IsGpuAcceleratedSupported();

	void ResetTexture();
	void DestroyTexture();
	void ResetMatInstance();
};
