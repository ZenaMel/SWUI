#pragma once

#include "UObject/Object.h"
#include "SwuiTypes.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "SwuiView.generated.h"

struct FSwuiViewCefData;

UCLASS(ClassGroup=Swui, Blueprintable)
class SWUIRUNTIME_API USwuiView : public UObject, public ISwuiRenderTarget
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

	UFUNCTION(BlueprintCallable, Category="SwuiRuntime")
	UTexture2D* GetTexture() const;

	// ISwuiRenderTarget
	virtual void OnPaint(const void* Buffer, FUpdateTextureRegion2D* Regions, int32 RegionCount, int32 Width, int32 Height) override;
	virtual UTexture2D* GetOrCreateTexture(int32 InWidth, int32 InHeight) override;

	virtual void BeginDestroy() override;

	// Called every UE frame by USwuiSubsystem::Tick — flushes pending CEF paints
	// as a single per-frame upload, with tile diff to skip unchanged tiles.
	void TickDeferredUpload();

	// Per-instance settings forwarded from USwui component at Init() time.
	// Kept public so USwuiSubsystem can read isolation flags (e.g. bPauseBrowserUpdates).
	FSwuiInstanceSettings InstanceSettings;

private:
	UPROPERTY()
	UTexture2D* Texture;

	UMaterialInstanceDynamic* MaterialInstance;

	TSharedPtr<FSwuiViewCefData> CefData;

	int32 WindowlessFrameRate = 300;

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

	// -----------------------------------------------------------------------
	// Tile diff state — game thread only, no mutex needed.
	// -----------------------------------------------------------------------
	int32          TilesX         = 0;
	int32          TilesY         = 0;
	TArray<uint32> LastTileHashes; // TilesX*TilesY entries; ~0u = not yet hashed
	int32          LastSnapW      = 0;
	int32          LastSnapH      = 0;

	// -----------------------------------------------------------------------
	// Aggregate stats — game thread only, reset every second in TickDeferredUpload.
	// -----------------------------------------------------------------------
	int32  Stat_CefPaints       = 0;   // # CEF OnPaint calls
	int32  Stat_UeUploads       = 0;   // # TickDeferredUpload enqueue calls
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
	int64  Stat_MemcpyUs        = 0;
	int64  Stat_MemcpyMaxUs     = 0;
	double Stat_LastLogTime     = 0.0;
	double Stat_OverlayLastPushTime = 0.0;

	void ResetTexture();
	void DestroyTexture();
	void ResetMatInstance();
};
