#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "HAL/CriticalSection.h"

#include "RenderHandler.h"
#include "SwuiTypes.h"

#include "SwuiView.generated.h"

struct FSwuiViewCefData;
struct FUpdateTextureRegion2D;
struct FKey;

class AActor;
class UTexture2D;
class UMaterialInterface;
class UMaterialInstanceDynamic;

UCLASS(ClassGroup=Swui, Blueprintable)
class SWUIRUNTIME_API USwuiView : public UObject, public ISwuiRenderTarget
{
	GENERATED_BODY()

public:
	USwuiView();

	// URL loaded by the CEF browser.
	FString DefaultURL;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	int32 Width = 1280;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	int32 Height = 720;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	bool bIsTransparent = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	UMaterialInterface* BaseMaterial = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="SwuiRuntime")
	FName TextureParameterName = TEXT("SwuiTexture");

	// Per-instance settings forwarded from USwui at Init() time.
	FSwuiInstanceSettings InstanceSettings;

	void Init(const FSwuiInstanceSettings& InInstanceSettings = FSwuiInstanceSettings{});
	virtual void BeginDestroy() override;

	int32 GetWindowlessFrameRate() const { return WindowlessFrameRate; }

	// Current cleaned renderer path is CPU/full-surface.
	ESwuiRenderingMode GetResolvedRenderingMode() const { return ResolvedRenderingMode; }

	void LoadURL(const FString& URI);
	void ExecuteJavaScript(const FString& Script);

	void SetOwningActor(AActor* InOwningActor) { OwningActor = InOwningActor; }
	bool HandleIncomingMessage(const FString& MessageJson);

	UFUNCTION(BlueprintCallable, Category="SwuiRuntime")
	UTexture2D* GetTexture() const;

	// ISwuiRenderTarget
	virtual void OnPaint(
		const void* Buffer,
		FUpdateTextureRegion2D* Regions,
		int32 RegionCount,
		int32 InWidth,
		int32 InHeight) override;

	virtual UTexture2D* GetOrCreateTexture(int32 InWidth, int32 InHeight) override;

	// Called every UE frame by USwuiSubsystem::Tick.
	// Pumps CEF when needed and uploads the latest full CEF surface.
	void TickDeferredUpload();

	// Requests CEF to produce a fresh visual frame.
	// The next CEF OnPaint is staged and uploaded through the full-surface path.
	void RequestBrowserVisualRefresh(bool bForceFrame = true);

	bool IsForceFullFrameMode() const;
	bool HasFreshOnPaintDataPending() const;

	void NotifySubsystemTick();
	void NotifyHudStateFlushed();

	bool IsExternalBeginFrameActive() const { return bExternalBeginFrameActive; }
	bool HasPaintAfterExternalBeginFrame() const { return bPaintArrivedAfterExternalBeginFrame; }

	bool FlushHudStateAndRequestBrowserFrame(
		const FString& CombinedScript,
		float DeltaTime,
		bool bForceFrame);

	bool SendExternalBeginFrameIfDue(float DeltaTime);

	// ---- Pointer Input Forwarding ----

	void SetPointerInputEnabled(bool bEnabled);
	bool IsPointerInputEnabled() const { return bPointerInputEnabled; }
	bool HasBrowserHost() const;

	bool ScreenToBrowserPixel(const FVector2D& ScreenPos, int32& OutX, int32& OutY) const;

	bool ForwardMouseMoveToBrowser(const FVector2D& ScreenPosition);

	bool ForwardMouseButtonToBrowser(
		const FVector2D& ScreenPosition,
		FKey Button,
		bool bMouseUp,
		int32 ClickCount = 1);

	bool ForwardMouseWheelToBrowser(
		const FVector2D& ScreenPosition,
		float DeltaX,
		float DeltaY);

	void SetBrowserInputFocus(bool bFocused);

private:
	AActor* ResolveOwningActor() const;

	// ---- Full-surface rendering path ----

	void StageFullSurfacePaint(
		const void* Buffer,
		int32 InWidth,
		int32 InHeight,
		double PaintNow);

	void TickFullSurfaceUpload(double Now);

	void DriveContinuousBrowserFrame(double Now, bool bDebugForceEveryTick);
	void UploadLatestFullSurface(bool bForceMemcpy);

	void PumpBrowserFrameIfDue(double Now, bool bForceFrame);
	void InvalidateBrowserView();

	void LogFullSurfaceStatsIfNeeded(double Now);
	void ResetFullSurfaceStats();

	// ---- Texture / material lifecycle ----

	void ResetTexture();
	void DestroyTexture();
	void ResetMatInstance();

	// ---- Ownership / browser ----

	TWeakObjectPtr<AActor> OwningActor;
	TSharedPtr<FSwuiViewCefData> CefData;

	UPROPERTY()
	UTexture2D* Texture = nullptr;

	UPROPERTY()
	UMaterialInstanceDynamic* MaterialInstance = nullptr;

	ESwuiRenderingMode ResolvedRenderingMode = ESwuiRenderingMode::CpuCompatible;

	// ---- Pointer input ----

	bool bPointerInputEnabled = false;

	// ---- Browser frame pacing ----

	int32 WindowlessFrameRate = 300;

	bool bExternalBeginFrameActive = false;

	int32 AppliedHudLockstepCVar = -1;
	int32 AppliedHudExternalBeginFramesCVar = -1;
	int32 AppliedHudMaxBrowserFPSCVar = -1;

	int32 LastObservedHudLockstepCVar = MIN_int32;
	int32 LastObservedHudExternalBeginFramesCVar = MIN_int32;
	int32 LastObservedHudMaxBrowserFPSCVar = MIN_int32;

	double LastBrowserFrameTime = 0.0;
	double LastExternalBeginFrameSentTime = 0.0;
	double ExternalBeginFrameAccumulatedTime = 0.0;
	double BrowserFrameTimeout = 0.100;

	double PendingBeginFrameSentTime = -1.0;
	bool bPaintArrivedAfterExternalBeginFrame = false;
	bool bPendingInvalidateForPaint = false;

	// ---- Full-surface paint state ----
	// Written from the CEF renderer thread, consumed from the game thread.
	// Guarded by PaintMutex.

	mutable FCriticalSection PaintMutex;

	TArray<uint8> BackingBuffer;

	bool bHasPendingFullSurfacePaint = false;
	int32 PendingFullSurfaceWidth = 0;
	int32 PendingFullSurfaceHeight = 0;

	uint64 PendingFreshPaintGeneration = 0;
	uint64 UploadedFreshPaintGeneration = 0;

	double LastPaintArrivalTime = 0.0;
	double PendingFreshPaintArrivalTime = 0.0;

	// ---- General stats ----

	int32 Stat_SubsystemTicks = 0;
	int32 Stat_ViewUploadTicks = 0;
	int32 Stat_HudStateFlushes = 0;

	int32 Stat_ExternalBeginFrames = 0;
	int32 Stat_ExternalBeginFrameSkipInactive = 0;
	int32 Stat_ExternalBeginFrameSkipDisabled = 0;
	int32 Stat_ExternalBeginFrameSkipNoBrowser = 0;
	int32 Stat_ExternalBeginFrameSkipRateLimited = 0;
	int32 Stat_ExternalBeginFrameForced = 0;
	int32 Stat_ExternalBeginFrameNonForced = 0;
	int32 Stat_ExternalBeginFrameCoalescedPending = 0;
	int32 Stat_ExternalBeginFrameCoalescedTimeout = 0;
	int32 Stat_InvalidateView = 0;
	int32 Stat_BeginFramesWithoutPaint = 0;
	int32 Stat_PaintsAfterInvalidate = 0;

	double Stat_PaintAfterBeginFrameMsSum = 0.0;
	double Stat_PaintAfterBeginFrameMsMax = 0.0;
	int32 Stat_PaintAfterBeginFrameSamples = 0;

	double Stat_LastLogTime = 0.0;
	int32 TargetFpsForLog = 0;

	// ---- Full-surface upload stats ----

	bool bFullSurfaceFirstEntryLogged = false;

	int32 Stat_FullSurfaceUploadTicks = 0;
	int32 Stat_FullSurfaceUploads = 0;
	int32 Stat_FullSurfaceSkippedNoFreshPaint = 0;
	int32 Stat_FullSurfaceCefPaints = 0;

	int64 Stat_FullSurfaceUploadedPx = 0;

	double Stat_FullSurfacePaintCopyMsSum = 0.0;
	double Stat_FullSurfacePaintCopyMsMax = 0.0;
	int32 Stat_FullSurfacePaintCopySamples = 0;

	double Stat_FullSurfaceUploadCopyMsSum = 0.0;
	double Stat_FullSurfaceUploadCopyMsMax = 0.0;
	int32 Stat_FullSurfaceUploadCopySamples = 0;

	double Stat_FullSurfaceEnqueueMsSum = 0.0;
	double Stat_FullSurfaceEnqueueMsMax = 0.0;
	int32 Stat_FullSurfaceEnqueueSamples = 0;

	double Stat_FullSurfaceLockWaitMs = 0.0;

	double Stat_FullSurfacePaintToUploadMsSum = 0.0;
	double Stat_FullSurfacePaintToUploadMsMax = 0.0;
	int32 Stat_FullSurfacePaintToUploadSamples = 0;
};