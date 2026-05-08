#include "SwuiView.h"
#include "RenderHandler.h"
#include "ISwuiRuntime.h"
#include "SwuiManager.h"
#include "Misc/Crc.h"

// ---------------------------------------------------------------------------
// Console variables — paint tuning & debug isolation
// ---------------------------------------------------------------------------

// 1 = skip the actual texture upload (measure memcpy isolation).
static TAutoConsoleVariable<int32> CVarSwuiNoTextureUpload(
	TEXT("swui.prof.NoTextureUpload"),
	0,
	TEXT("Debug: skip RHIUpdateTexture2D (isolate GPU upload cost). 0=normal, 1=skip upload."),
	ECVF_Default);

// 1 = verbose per-paint log (upload strategy, areas, ratio).
static TAutoConsoleVariable<int32> CVarSwuiVerbosePaint(
	TEXT("swui.prof.VerbosePaint"),
	0,
	TEXT("Debug: log per-paint upload strategy details. 0=off, 1=on."),
	ECVF_Default);

struct FSwuiViewCefData
{
	CefRefPtr<BrowserClient> Client;
	CefRefPtr<CefBrowser> Browser;
};

// ---------------------------------------------------------------------------
// CefTask subclass for the dirty-rect overlay JS push.
// Avoids base::BindOnce lambda → no C4191 reinterpret_cast warning.
// ---------------------------------------------------------------------------
class FSwuiOverlayPushTask : public CefTask
{
public:
	FSwuiOverlayPushTask(CefRefPtr<CefBrowser> InBrowser, std::string InScript)
		: Browser(InBrowser), Script(MoveTemp(InScript)) {}

	void Execute() override
	{
		if (!Browser) return;
		CefRefPtr<CefFrame> Frame = Browser->GetMainFrame();
		if (Frame) Frame->ExecuteJavaScript(CefString(Script), Frame->GetURL(), 0);
	}

private:
	CefRefPtr<CefBrowser> Browser;
	std::string Script;
	IMPLEMENT_REFCOUNTING(FSwuiOverlayPushTask);
};

class FSwuiFlushAndBeginFrameTask : public CefTask
{
public:
	FSwuiFlushAndBeginFrameTask(CefRefPtr<CefBrowser> InBrowser, std::string InScript, bool bInInvalidateView, bool bInSendBeginFrame)
		: Browser(InBrowser), Script(MoveTemp(InScript)), bInvalidateView(bInInvalidateView), bSendBeginFrame(bInSendBeginFrame) {}

	void Execute() override
	{
		if (!Browser) return;

		if (!Script.empty())
		{
			CefRefPtr<CefFrame> Frame = Browser->GetMainFrame();
			if (Frame)
			{
				Frame->ExecuteJavaScript(CefString(Script), Frame->GetURL(), 0);
			}
		}

		if (bSendBeginFrame)
		{
			CefRefPtr<CefBrowserHost> Host = Browser->GetHost();
			if (Host)
			{
				if (bInvalidateView)
				{
					Host->Invalidate(PET_VIEW);
				}
				Host->SendExternalBeginFrame();
			}
		}
	}

private:
	CefRefPtr<CefBrowser> Browser;
	std::string Script;
	bool bInvalidateView = false;
	bool bSendBeginFrame = false;
	IMPLEMENT_REFCOUNTING(FSwuiFlushAndBeginFrameTask);
};

// ---------------------------------------------------------------------------
// Rect optimization helpers — file-scope, called only from CEF renderer thread.
// ---------------------------------------------------------------------------
static constexpr double SwuiDefaultMaxMergeWasteRatio = 1.15;
static constexpr int32  SwuiDefaultMaxMergedRectWidth  = 512;
static constexpr int32  SwuiDefaultMaxMergedRectHeight = 256;
static constexpr int32  SwuiDefaultMaxMergedRectArea   = 256 * 1024;

static FIntRect SwuiClampRect(const FIntRect& R, int32 TexW, int32 TexH)
{
	return FIntRect(
		FMath::Clamp(R.Min.X, 0, TexW), FMath::Clamp(R.Min.Y, 0, TexH),
		FMath::Clamp(R.Max.X, 0, TexW), FMath::Clamp(R.Max.Y, 0, TexH));
}

static FIntRect SwuiExpandToMinSizeCustom(const FIntRect& R, int32 TexW, int32 TexH, int32 MinW, int32 MinH)
{
	const int32 ExtraW = FMath::Max(0, MinW - R.Width());
	const int32 ExtraH = FMath::Max(0, MinH - R.Height());
	const FIntRect Expanded(
		R.Min.X - ExtraW / 2,             R.Min.Y - ExtraH / 2,
		R.Max.X + (ExtraW - ExtraW / 2),  R.Max.Y + (ExtraH - ExtraH / 2));
	return SwuiClampRect(Expanded, TexW, TexH);
}

static bool SwuiShouldMergeCapped(
	const FIntRect& A,
	const FIntRect& B,
	float MaxWasteRatio,
	int32 MaxW,
	int32 MaxH,
	int32 MaxArea)
{
	const FIntRect U(
		FIntPoint(FMath::Min(A.Min.X, B.Min.X), FMath::Min(A.Min.Y, B.Min.Y)),
		FIntPoint(FMath::Max(A.Max.X, B.Max.X), FMath::Max(A.Max.Y, B.Max.Y)));

	if (U.Width() > MaxW || U.Height() > MaxH) return false;
	const int64 UnionArea = (int64)U.Width() * U.Height();
	if (UnionArea > MaxArea) return false;

	const int64 SumArea = (int64)A.Width() * A.Height() + (int64)B.Width() * B.Height();
	if (SumArea <= 0) return false;
	return (double)UnionArea <= (double)SumArea * (double)MaxWasteRatio;
}

// -----------------------------------------------------------------------
// Tile diff helpers
// -----------------------------------------------------------------------
static constexpr int64 SwuiLargeDirtyRectArea = int64(512) * 512; // 262 144 px

// CRC32 over one tile — row-stride is FullPitch (full texture width * 4).
// ~0u initial value ensures uninitialized hash slots are never falsely matched.
static uint32 SwuiHashTile(const uint8* Buf, const FIntRect& R, int32 FullPitch)
{
	uint32 Hash = ~0u;
	const uint8* Row = Buf + (int64)R.Min.Y * FullPitch + (int64)R.Min.X * 4;
	const int32  RowBytes = R.Width() * 4;
	for (int32 y = 0; y < R.Height(); ++y, Row += FullPitch)
		Hash = FCrc::MemCrc32(Row, RowBytes, Hash);
	return Hash;
}

static void SwuiMarkTilesForRectCustom(
	const FIntRect& Rect,
	int32 TileW,
	int32 TileH,
	int32 TilesX,
	int32 TilesY,
	TBitArray<>& Mask)
{
	const int32 MinTX = FMath::Clamp(Rect.Min.X / TileW, 0, FMath::Max(TilesX - 1, 0));
	const int32 MaxTX = FMath::Clamp((Rect.Max.X - 1) / TileW, 0, FMath::Max(TilesX - 1, 0));
	const int32 MinTY = FMath::Clamp(Rect.Min.Y / TileH, 0, FMath::Max(TilesY - 1, 0));
	const int32 MaxTY = FMath::Clamp((Rect.Max.Y - 1) / TileH, 0, FMath::Max(TilesY - 1, 0));
	for (int32 Ty = MinTY; Ty <= MaxTY; ++Ty)
		for (int32 Tx = MinTX; Tx <= MaxTX; ++Tx)
			Mask[Ty * TilesX + Tx] = true;
}

static FIntRect SwuiTileRectFromIndexCustom(int32 TileIndex, int32 TilesX, int32 TileW, int32 TileH, int32 TexW, int32 TexH)
{
	const int32 Tx = TileIndex % TilesX;
	const int32 Ty = TileIndex / TilesX;
	return FIntRect(
		FIntPoint(Tx * TileW, Ty * TileH),
		FIntPoint(FMath::Min((Tx + 1) * TileW, TexW), FMath::Min((Ty + 1) * TileH, TexH)));
}

USwuiView::USwuiView()
{
	Texture = nullptr;
	CefData = MakeShared<FSwuiViewCefData>();
}

void USwuiView::Init(const FSwuiInstanceSettings& InInstanceSettings)
{
	InstanceSettings = InInstanceSettings;
	if (Width <= 0 || Height <= 0)
	{
		UE_LOG(LogSwuiRuntime, Warning, TEXT("USwuiView: Width or Height <= 0"));
		return;
	}

	CefWindowInfo Info;
	Info.SetAsWindowless(0);
	const bool bWantsExternalBeginFrames =
		InstanceSettings.bIsHUD &&
		InstanceSettings.bUseUEFrameLockedBrowser &&
		InstanceSettings.bUseExternalBeginFrames;
	Info.external_begin_frame_enabled = bWantsExternalBeginFrames ? 1 : 0;

	CefBrowserSettings BrowserSettings;
	BrowserSettings.webgl = STATE_ENABLED;

	RenderHandler* Renderer = new RenderHandler(Width, Height, this);
	CefRefPtr<BrowserClient> Client = new BrowserClient(Renderer);

	CefRefPtr<CefBrowser> Browser = CefBrowserHost::CreateBrowserSync(
		Info,
		Client.get(),
		"about:blank",
		BrowserSettings,
		nullptr,
		nullptr);

	if (!Browser)
	{
		UE_LOG(LogSwuiRuntime, Error, TEXT("USwuiView::Init: CefBrowserHost::CreateBrowserSync returned null — CEF may not be initialized or the subprocess is missing."));
		return;
	}

	// Determine per-browser frame rate:
	//  Priority: InstanceSettings.OverrideFrameRate > 0  → use it
	//         else Settings.DefaultViewFrameRate > 0      → use project setting
	//         else GEngine->GetMaxFPS() > 0               → follow engine cap
	//         else                                        → 300
	int32 TargetFPS = 300;
	const USwuiSettings* Settings = GetDefault<USwuiSettings>();
	if (InstanceSettings.bIsHUD && InstanceSettings.bUseUEFrameLockedBrowser)
	{
		TargetFPS = InstanceSettings.MaxBrowserFramesPerSecond > 0 ? InstanceSettings.MaxBrowserFramesPerSecond : 60;
	}
	else if (InstanceSettings.OverrideFrameRate > 0)
	{
		TargetFPS = InstanceSettings.OverrideFrameRate;
	}
	else if (Settings && Settings->DefaultViewFrameRate > 0)
	{
		TargetFPS = Settings->DefaultViewFrameRate;
	}
	else if (GEngine && GEngine->GetMaxFPS() > 0)
	{
		TargetFPS = FMath::RoundToInt(GEngine->GetMaxFPS());
	}

	// Debug flags: instance wins over project setting. Always write the CVar so
	// toggling a flag off at runtime actually takes effect (CVars are sticky otherwise).
	const bool bWantVerbose  = InstanceSettings.bVerbosePaintLog || (Settings && Settings->bVerbosePaintLog);
	const bool bWantNoUpload = InstanceSettings.bNoTextureUpload || (Settings && Settings->bNoTextureUpload);
	CVarSwuiVerbosePaint->Set(bWantVerbose  ? 1 : 0, ECVF_SetByCode);
	CVarSwuiNoTextureUpload->Set(bWantNoUpload ? 1 : 0, ECVF_SetByCode);

	Browser->GetHost()->SetWindowlessFrameRate(TargetFPS);
	WindowlessFrameRate = TargetFPS;
	const bool bHostIsWindowless = Browser->GetHost()->IsWindowRenderingDisabled();
	bExternalBeginFrameActive = bWantsExternalBeginFrames && bHostIsWindowless;
	ExternalBeginFrameAccumulatedTime = 0.0;
	bPaintArrivedAfterExternalBeginFrame = false;
	bPendingInvalidateForPaint = false;
	if (bWantsExternalBeginFrames)
	{
		UE_LOG(LogSwuiRuntime, Log, TEXT("USwuiView: External begin frame requested=%s, osrWindowless=%s, active=%s"),
			bWantsExternalBeginFrames ? TEXT("true") : TEXT("false"),
			bHostIsWindowless ? TEXT("true") : TEXT("false"),
			bExternalBeginFrameActive ? TEXT("true") : TEXT("false"));
	}
	else if (InstanceSettings.bIsHUD && InstanceSettings.bUseUEFrameLockedBrowser && InstanceSettings.bUseExternalBeginFrames)
	{
		UE_LOG(LogSwuiRuntime, Warning, TEXT("USwuiView: External begin frame mode unavailable; using capped windowless frame pacing fallback."));
	}

	CefData->Client = Client;
	CefData->Browser = Browser;

	UE_LOG(LogSwuiRuntime, Log, TEXT("USwuiView Initialized"));

	if (!DefaultURL.IsEmpty())
	{
		LoadURL(DefaultURL);
	}

	ResetTexture();
}

void USwuiView::LoadURL(const FString& URI)
{
	if (!CefData || !CefData->Browser)
	{
		return;
	}

	{
		FScopeLock Lock(&PaintMutex);
		bNeedsFullBaselineUpload = true;
	}

	// http/https/localhost/file → pass through directly
	if (URI.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase)
		|| URI.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase)
		|| URI.StartsWith(TEXT("localhost"), ESearchCase::IgnoreCase)
		|| URI.StartsWith(TEXT("file:///"), ESearchCase::IgnoreCase))
	{
		CefData->Browser->GetMainFrame()->LoadURL(*URI);
		return;
	}

	// swui:// or bare path → resolve under Content/
	FString Relative = URI;
	if (Relative.StartsWith(TEXT("swui://"), ESearchCase::IgnoreCase))
	{
		Relative = Relative.RightChop(7); // strip "swui://"
	}

	// Append .html if no extension provided
	if (FPaths::GetExtension(Relative).IsEmpty())
	{
		Relative += TEXT(".html");
	}

	FString ContentDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
	FString LocalFile = FString(TEXT("file:///")) + ContentDir + Relative;
	CefData->Browser->GetMainFrame()->LoadURL(*LocalFile);
}

void USwuiView::ExecuteJavaScript(const FString& Script)
{
	if (CefData && CefData->Browser)
	{
		CefString CodeStr = *Script;
		CefData->Browser->GetMainFrame()->ExecuteJavaScript(CodeStr, "", 0);
	}
}

void USwuiView::NotifyHudStateFlushed()
{
	++Stat_HudStateFlushes;
}

void USwuiView::NotifySubsystemTick()
{
	++Stat_SubsystemTicks;
}

bool USwuiView::HasFreshOnPaintDataPending() const
{
	FScopeLock Lock(const_cast<FCriticalSection*>(&PaintMutex));
	return bHasPendingUpload && PendingIncomingRects > 0;
}

bool USwuiView::FlushHudStateAndRequestBrowserFrame(const FString& CombinedScript, float DeltaTime, bool bForceFrame)
{
	const bool bHasScript = !CombinedScript.IsEmpty();
	if (!CefData || !CefData->Browser)
	{
		if (bExternalBeginFrameActive && InstanceSettings.bSendExternalBeginFrameFromTick)
		{
			++Stat_ExternalBeginFrameSkipNoBrowser;
		}
		return false;
	}

	if (!bExternalBeginFrameActive)
	{
		if (bHasScript)
		{
			ExecuteJavaScript(CombinedScript);
		}
		return false;
	}
	if (!InstanceSettings.bSendExternalBeginFrameFromTick)
	{
		++Stat_ExternalBeginFrameSkipDisabled;
		if (bHasScript)
		{
			ExecuteJavaScript(CombinedScript);
		}
		return false;
	}

	const int32 TargetHz = FMath::Clamp(
		InstanceSettings.MaxBrowserFramesPerSecond > 0 ? InstanceSettings.MaxBrowserFramesPerSecond : WindowlessFrameRate,
		1,
		300);
	const double MinInterval = 1.0 / (double)TargetHz;
	ExternalBeginFrameAccumulatedTime += FMath::Max(0.0f, DeltaTime);
	if (LastExternalBeginFrameSentTime <= 0.0)
	{
		ExternalBeginFrameAccumulatedTime = MinInterval;
	}

	bool bWillSendBeginFrame = bForceFrame;
	if (!bWillSendBeginFrame)
	{
		if (ExternalBeginFrameAccumulatedTime < MinInterval)
		{
			++Stat_ExternalBeginFrameSkipRateLimited;
		}
		else
		{
			bWillSendBeginFrame = true;
		}
	}

	if (bWillSendBeginFrame)
	{
		ExternalBeginFrameAccumulatedTime = FMath::Max(0.0, ExternalBeginFrameAccumulatedTime - MinInterval);
		const double Now = FPlatformTime::Seconds();
		if (PendingBeginFrameSentTime > 0.0)
		{
			++Stat_BeginFramesWithoutPaint;
		}
		LastExternalBeginFrameSentTime = Now;
		const bool bInvalidateViewForThisFrame = bForceFrame || bHasScript;
		if (bInvalidateViewForThisFrame)
		{
			++Stat_InvalidateView;
		}
		{
			FScopeLock Lock(&PaintMutex);
			PendingBeginFrameSentTime = Now;
			bPaintArrivedAfterExternalBeginFrame = false;
			bPendingInvalidateForPaint = bInvalidateViewForThisFrame;
		}
		++Stat_ExternalBeginFrames;
	}

	if (bHasScript || bWillSendBeginFrame)
	{
		const std::string StdScript = bHasScript ? std::string(TCHAR_TO_UTF8(*CombinedScript)) : std::string();
		const bool bInvalidateViewForThisFrame = bWillSendBeginFrame && (bForceFrame || bHasScript);
		CefPostTask(TID_UI, new FSwuiFlushAndBeginFrameTask(CefData->Browser, StdScript, bInvalidateViewForThisFrame, bWillSendBeginFrame));
	}

	return bWillSendBeginFrame;
}

bool USwuiView::SendExternalBeginFrameIfDue(float DeltaTime)
{
	if (!bExternalBeginFrameActive)
	{
		++Stat_ExternalBeginFrameSkipInactive;
		return false;
	}
	return FlushHudStateAndRequestBrowserFrame(FString(), DeltaTime, false);
}

UTexture2D* USwuiView::GetTexture() const
{
	return Texture;
}

void USwuiView::OnPaint(const void* Buffer, FUpdateTextureRegion2D* Regions, int32 RegionCount, int32 InWidth, int32 InHeight)
{
	GetOrCreateTexture(InWidth, InHeight);

	if (!Texture || !Texture->GetResource()) { FMemory::Free(Regions); return; }

	// Stage gate 1: skip entire pipeline.
	if (InstanceSettings.bSkipOnPaintProcessing) { FMemory::Free(Regions); return; }

	const int32 FullPitch = InWidth * 4;
	const int32 BufBytes  = FullPitch * InHeight;
	const double PaintNow = FPlatformTime::Seconds();

	FScopeLock Lock(&PaintMutex);
	LastPaintArrivalTime = PaintNow;
	if (PendingBeginFrameSentTime > 0.0)
	{
		const double PaintAfterBeginMs = (PaintNow - PendingBeginFrameSentTime) * 1000.0;
		Stat_PaintAfterBeginFrameMsSum += PaintAfterBeginMs;
		if (PaintAfterBeginMs > Stat_PaintAfterBeginFrameMsMax) Stat_PaintAfterBeginFrameMsMax = PaintAfterBeginMs;
		++Stat_PaintAfterBeginFrameSamples;
		bPaintArrivedAfterExternalBeginFrame = true;
		if (bPendingInvalidateForPaint)
		{
			++Stat_PaintsAfterInvalidate;
			bPendingInvalidateForPaint = false;
		}
		PendingBeginFrameSentTime = -1.0;
	}

	// Resize backing buffer when texture dimensions change.
	if (BackingBuffer.Num() != BufBytes)
	{
		BackingBuffer.SetNumUninitialized(BufBytes);
		bNeedsFullBaselineUpload = true;
	}

	if (!bSeenFirstPaint)
	{
		bSeenFirstPaint = true;
		if (InstanceSettings.bForceFullBaselineUploadOnFirstPaint)
			bNeedsFullBaselineUpload = true;
	}

	for (int32 i = 0; i < RegionCount; ++i)
	{
		FUpdateTextureRegion2D& Rg = Regions[i];

		// Clamp degenerate rects against texture bounds.
		Rg.SrcX  = Rg.DestX = (uint32)FMath::Clamp((int32)Rg.SrcX,  0, InWidth  - 1);
		Rg.SrcY  = Rg.DestY = (uint32)FMath::Clamp((int32)Rg.SrcY,  0, InHeight - 1);
		Rg.Width  = (uint32)FMath::Clamp((int32)Rg.Width,  0, InWidth  - (int32)Rg.SrcX);
		Rg.Height = (uint32)FMath::Clamp((int32)Rg.Height, 0, InHeight - (int32)Rg.SrcY);

		if (Rg.Width == 0 || Rg.Height == 0) continue;

		// Blit this dirty rect into the backing buffer (partial in-place update).
		const int32  RowBytes = (int32)Rg.Width * 4;
		const uint8* Src = (const uint8*)Buffer    + (int64)Rg.SrcY * FullPitch + (int64)Rg.SrcX * 4;
		uint8*       Dst = BackingBuffer.GetData() + (int64)Rg.SrcY * FullPitch + (int64)Rg.SrcX * 4;
		for (uint32 Row = 0; Row < Rg.Height; ++Row, Src += FullPitch, Dst += FullPitch)
			FPlatformMemory::Memcpy(Dst, Src, RowBytes);

		// Accumulate for TickDeferredUpload.
		const int64 Area = (int64)Rg.Width * Rg.Height;
		++PendingIncomingRects;
		PendingIncomingPx += Area;
		if ((int32)Area > PendingLargestIncoming) PendingLargestIncoming = (int32)Area;
		PendingDirtyRects.Add(FIntRect((int32)Rg.SrcX, (int32)Rg.SrcY,
			(int32)Rg.SrcX + (int32)Rg.Width, (int32)Rg.SrcY + (int32)Rg.Height));
		if (InstanceSettings.bShowDirtyRectOverlay)
			PendingOverlayRects.Add(Rg);
	}

	++PendingCefPaints;
	bHasPendingUpload = true;
	FMemory::Free(Regions);
}

// ---------------------------------------------------------------------------
// TickDeferredUpload — called every UE frame from USwuiSubsystem::Tick.
// Drains all CEF OnPaint calls accumulated since the previous frame,
// runs tile diff on large rects, greedy-merges survivors, and issues one
// RHI upload command for the final optimised set.
// ---------------------------------------------------------------------------
void USwuiView::TickDeferredUpload()
{
	const double Now = FPlatformTime::Seconds();
	++Stat_ViewUploadTicks;

	auto RecordTimingSampleMs = [](double SampleMs, double& SumMs, double& MaxMs, int32& SampleCount)
	{
		SumMs += SampleMs;
		if (SampleMs > MaxMs) MaxMs = SampleMs;
		++SampleCount;
	};

	// ---- Phase 1: move pending metadata out under lock ----
	TArray<FIntRect>               LocalRects;
	TArray<FUpdateTextureRegion2D> LocalOverlayRects;
	TArray<FUpdateTextureRegion2D> OptimizedOverlayRects;
	int32 LocalCefPaints = 0, LocalInRects = 0, LocalLargestIn = 0;
	int64 LocalInPx = 0;

	auto HasActiveDirtyTiles = [this]() -> bool
	{
		for (int32 i = 0; i < ActiveDirtyTileMask.Num(); ++i)
		{
			if (ActiveDirtyTileMask[i])
			{
				return true;
			}
		}
		return false;
	};

	if (bHasPendingUpload && Texture && Texture->GetResource())
	{
		const double LockWaitStart = FPlatformTime::Seconds();
		FScopeLock Lock(&PaintMutex);
		Stat_LockWaitMs += (FPlatformTime::Seconds() - LockWaitStart) * 1000.0;
		if (bHasPendingUpload)
		{
			bHasPendingUpload      = false;
			LocalRects             = MoveTemp(PendingDirtyRects);
			LocalOverlayRects      = MoveTemp(PendingOverlayRects);
			LocalCefPaints         = PendingCefPaints;         PendingCefPaints        = 0;
			LocalInRects           = PendingIncomingRects;     PendingIncomingRects    = 0;
			LocalInPx              = PendingIncomingPx;        PendingIncomingPx       = 0;
			LocalLargestIn         = PendingLargestIncoming;   PendingLargestIncoming  = 0;
		}
	}

	// Stats accumulation (game thread only)
	Stat_CefPaints     += LocalCefPaints;
	Stat_IncomingRects += LocalInRects;
	Stat_IncomingPx    += LocalInPx;
	const bool bHasFreshPaintThisTick = LocalInRects > 0;
	if (LocalLargestIn > Stat_LargestIncoming) Stat_LargestIncoming = LocalLargestIn;

	if (Texture && Texture->GetResource() && (LocalRects.Num() > 0 || bNeedsFullBaselineUpload || HasActiveDirtyTiles()))
	{
		const double DeferredStart = FPlatformTime::Seconds();
		const int32 SnapW     = LastSnapW = Texture->GetSizeX();
		const int32 SnapH     = LastSnapH = Texture->GetSizeY();
		const int32 FullPitch = SnapW * 4;

		const bool  bHybridEnabled   = InstanceSettings.bEnableHybridDirtyUpload;
		const bool  bTileDiffEnabled = InstanceSettings.bEnableTileDiffForLargeRects;
		const bool  bBudgetEnabled   = InstanceSettings.bEnableUploadBudget;
		const bool  bCenterEnabled   = InstanceSettings.bAlwaysProcessCenterCriticalRect;
		const bool  bRotatingCursor  = InstanceSettings.bUseRotatingDeferredTileCursor;
		const int32 TileW            = FMath::Max(8, InstanceSettings.TileWidth);
		const int32 TileH            = FMath::Max(8, InstanceSettings.TileHeight);
		const int32 MinDirtyW        = FMath::Max(1, InstanceSettings.MinDirtyRectWidth);
		const int32 MinDirtyH        = FMath::Max(1, InstanceSettings.MinDirtyRectHeight);
		const int32 CenterW          = FMath::Max(1, InstanceSettings.CenterCriticalWidth);
		const int32 CenterH          = FMath::Max(1, InstanceSettings.CenterCriticalHeight);
		const int32 NormalBudget     = bBudgetEnabled ? FMath::Max(0, InstanceSettings.MaxNormalUploadBytesPerFrame) : INT_MAX;
		const float MergeRatio       = InstanceSettings.MaxMergeWasteRatio > 0.f ? InstanceSettings.MaxMergeWasteRatio : (float)SwuiDefaultMaxMergeWasteRatio;
		const int32 MaxMergeW        = InstanceSettings.MaxMergedRectWidth  > 0 ? InstanceSettings.MaxMergedRectWidth  : SwuiDefaultMaxMergedRectWidth;
		const int32 MaxMergeH        = InstanceSettings.MaxMergedRectHeight > 0 ? InstanceSettings.MaxMergedRectHeight : SwuiDefaultMaxMergedRectHeight;
		const int32 MaxMergeArea     = InstanceSettings.MaxMergedRectArea   > 0 ? InstanceSettings.MaxMergedRectArea   : SwuiDefaultMaxMergedRectArea;

		const int32 NTX = FMath::DivideAndRoundUp(SnapW, TileW);
		const int32 NTY = FMath::DivideAndRoundUp(SnapH, TileH);
		if (TilesX != NTX || TilesY != NTY)
		{
			TilesX = NTX; TilesY = NTY;
			ActiveDirtyTileMask.Init(false, TilesX * TilesY);
			UploadedTileMask.Init(false, TilesX * TilesY);
			LastTileHashes.Init(~0u, TilesX * TilesY);
			DirtyTileScanCursor = 0;
			bNeedsFullBaselineUpload = true;
		}

		// Baseline upload: ensure whole texture is valid once before incremental mode.
		if (bNeedsFullBaselineUpload && !InstanceSettings.bSkipPaintMemcpy && !InstanceSettings.bFreezeTexture)
		{
			bool bDidBaselineThisTick = false;
			const double LockWaitStart = FPlatformTime::Seconds();
			FScopeLock Lock(&PaintMutex);
			Stat_LockWaitMs += (FPlatformTime::Seconds() - LockWaitStart) * 1000.0;
			if (BackingBuffer.Num() == FullPitch * SnapH)
			{
				FSwuiPaintUploadData* UploadData = new FSwuiPaintUploadData;
				UploadData->Texture2DResource = (FTextureResource*)Texture->GetResource();
				UploadData->PackedPixels = BackingBuffer;
				UploadData->Rects.SetNum(1);
				FSwuiPackedRectDesc& D = UploadData->Rects[0];
				D.Region = FUpdateTextureRegion2D(0, 0, 0, 0, SnapW, SnapH);
				D.SrcPitch = (uint32)FullPitch;
				D.SrcOffsetBytes = 0;

				const double HashStart = FPlatformTime::Seconds();
				for (int32 i = 0; i < LastTileHashes.Num(); ++i)
				{
					const FIntRect TileRect = SwuiTileRectFromIndexCustom(i, TilesX, TileW, TileH, SnapW, SnapH);
					LastTileHashes[i] = SwuiHashTile(BackingBuffer.GetData(), TileRect, FullPitch);
					UploadedTileMask[i] = true;
					ActiveDirtyTileMask[i] = false;
				}
				RecordTimingSampleMs(
					(FPlatformTime::Seconds() - HashStart) * 1000.0,
					Stat_HashMsSum,
					Stat_HashMsMax,
					Stat_HashSamples);
				bNeedsFullBaselineUpload = false;
				ActiveDirtyTileMask.Init(false, ActiveDirtyTileMask.Num());
				DirtyTileScanCursor = 0;
				bDidBaselineThisTick = true;

				const bool bSkipUpload = CVarSwuiNoTextureUpload.GetValueOnAnyThread() != 0
					|| InstanceSettings.bSkipTextureUpload || InstanceSettings.bNoTextureUpload;
				if (bSkipUpload) { delete UploadData; }
				else
				{
					if (bHasFreshPaintThisTick && LastPaintArrivalTime > 0.0)
					{
						RecordTimingSampleMs(
							(FPlatformTime::Seconds() - LastPaintArrivalTime) * 1000.0,
							Stat_UploadAfterPaintMsSum,
							Stat_UploadAfterPaintMsMax,
							Stat_UploadAfterPaintSamples);
					}
					ENQUEUE_RENDER_COMMAND(UpdateSwuiViewBaseline)(
						[UploadData](FRHICommandList& CommandList)
						{
							FRHITexture* Tex = UploadData->Texture2DResource->TextureRHI.GetReference();
							const FSwuiPackedRectDesc& Rd = UploadData->Rects[0];
							RHIUpdateTexture2D(Tex, 0, Rd.Region, Rd.SrcPitch, UploadData->PackedPixels.GetData());
							delete UploadData;
						});
					++Stat_UeUploads;
					if (bHasFreshPaintThisTick) ++Stat_UeUploadsFresh; else ++Stat_UeUploadsBacklog;
					++Stat_UploadedRects;
					Stat_UploadedPixels += (int64)SnapW * SnapH;
					Stat_LargestUploaded = FMath::Max(Stat_LargestUploaded, SnapW * SnapH);
				}
			}

			if (bDidBaselineThisTick)
			{
				// Baseline is complete and authoritative. Enter optimised mode next tick.
				RecordTimingSampleMs(
					(FPlatformTime::Seconds() - DeferredStart) * 1000.0,
					Stat_DeferredUploadMsSum,
					Stat_DeferredUploadMsMax,
					Stat_DeferredUploadSamples);
				return;
			}
		}

		if ((LocalRects.Num() > 0 || HasActiveDirtyTiles()) && !InstanceSettings.bSkipDirtyRectStrategy && !InstanceSettings.bSkipPaintMemcpy && !InstanceSettings.bFreezeTexture)
		{
			TArray<FIntRect, TInlineAllocator<128>> SmallRects;
			TBitArray<> FreshLargeTileMask(false, ActiveDirtyTileMask.Num());

			for (const FIntRect& Raw : LocalRects)
			{
				const FIntRect R = SwuiClampRect(Raw, SnapW, SnapH);
				if (R.Width() <= 0 || R.Height() <= 0) continue;

				const int64 RectArea = (int64)R.Width() * R.Height();
				const int64 ScreenArea = (int64)SnapW * SnapH;
				const bool bFullscreenLike =
					R.Width() >= SnapW ||
					R.Height() >= SnapH ||
					RectArea >= ScreenArea / 4;
				const bool bLarge = RectArea >= SwuiLargeDirtyRectArea || R.Width() >= SnapW || R.Height() >= SnapH;
				if (bHybridEnabled && bTileDiffEnabled && bLarge)
				{
					SwuiMarkTilesForRectCustom(R, TileW, TileH, TilesX, TilesY, ActiveDirtyTileMask);
					if (!bFullscreenLike)
					{
						SwuiMarkTilesForRectCustom(R, TileW, TileH, TilesX, TilesY, FreshLargeTileMask);
					}
				}
				else
				{
					SmallRects.Add(SwuiExpandToMinSizeCustom(R, SnapW, SnapH, MinDirtyW, MinDirtyH));
				}
			}

			TArray<int32, TInlineAllocator<256>> SelectedTileIndices;
			TBitArray<> SelectedMask(false, ActiveDirtyTileMask.Num());
			int32 UsedNormalBytes = 0;
			FIntRect CenterRect;
			bool bHasCenterRect = false;

			if (bCenterEnabled)
			{
				CenterRect = SwuiClampRect(
					FIntRect(
						FIntPoint((SnapW - CenterW) / 2, (SnapH - CenterH) / 2),
						FIntPoint((SnapW + CenterW) / 2, (SnapH + CenterH) / 2)),
					SnapW,
					SnapH);

				bHasCenterRect = CenterRect.Width() > 0 && CenterRect.Height() > 0;
			}

			// Fresh local large tiles from the latest batch are next priority.
			for (int32 i = 0; i < ActiveDirtyTileMask.Num(); ++i)
			{
				if (!ActiveDirtyTileMask[i] || !FreshLargeTileMask[i] || SelectedMask[i]) continue;
				const FIntRect TileRect = SwuiTileRectFromIndexCustom(i, TilesX, TileW, TileH, SnapW, SnapH);
				if (bHasCenterRect && TileRect.Intersect(CenterRect))
				{
					continue;
				}
				const int32 TileBytes = TileRect.Width() * TileRect.Height() * 4;
				if (UsedNormalBytes + TileBytes > NormalBudget) continue;
				UsedNormalBytes += TileBytes;
				SelectedMask[i] = true;
				SelectedTileIndices.Add(i);
			}

			// Old backlog tiles are fallback recovery, scanned last with rotating cursor.
			if (ActiveDirtyTileMask.Num() > 0)
			{
				const int32 Start = bRotatingCursor ? (DirtyTileScanCursor % ActiveDirtyTileMask.Num()) : 0;
				int32 NextCursor = Start;
				for (int32 Step = 0; Step < ActiveDirtyTileMask.Num(); ++Step)
				{
					const int32 Idx = (Start + Step) % ActiveDirtyTileMask.Num();
					if (!ActiveDirtyTileMask[Idx] || SelectedMask[Idx] || FreshLargeTileMask[Idx]) continue;
					const FIntRect TileRect = SwuiTileRectFromIndexCustom(Idx, TilesX, TileW, TileH, SnapW, SnapH);
					if (bHasCenterRect && TileRect.Intersect(CenterRect))
					{
						continue;
					}
					const int32 TileBytes = TileRect.Width() * TileRect.Height() * 4;
					if (UsedNormalBytes + TileBytes > NormalBudget)
					{
						break;
					}
					UsedNormalBytes += TileBytes;
					SelectedMask[Idx] = true;
					SelectedTileIndices.Add(Idx);
					NextCursor = (Idx + 1) % ActiveDirtyTileMask.Num();
				}
				if (bRotatingCursor)
				{
					DirtyTileScanCursor = NextCursor;
				}
			}

			TArray<FIntRect, TInlineAllocator<256>> Candidates;

			if (bHasCenterRect)
			{
				Candidates.Add(CenterRect);
			}

			Candidates.Append(SmallRects);

			const double LockWaitStart2 = FPlatformTime::Seconds();
			FScopeLock Lock(&PaintMutex);
			Stat_LockWaitMs += (FPlatformTime::Seconds() - LockWaitStart2) * 1000.0;

			const double HashStart = FPlatformTime::Seconds();
			TArray<int32, TInlineAllocator<256>> UploadedTilesThisFrame;
			for (int32 TileIdx : SelectedTileIndices)
			{
				const FIntRect TileRect = SwuiTileRectFromIndexCustom(TileIdx, TilesX, TileW, TileH, SnapW, SnapH);
				const uint32 NewHash = SwuiHashTile(BackingBuffer.GetData(), TileRect, FullPitch);
				if (NewHash == LastTileHashes[TileIdx] && UploadedTileMask[TileIdx])
				{
					++Stat_SkippedTiles;
					ActiveDirtyTileMask[TileIdx] = false;
					continue;
				}
				LastTileHashes[TileIdx] = NewHash;
				++Stat_ChangedTiles;
				ActiveDirtyTileMask[TileIdx] = false;
				UploadedTilesThisFrame.Add(TileIdx);
				Candidates.Add(TileRect);
			}
			RecordTimingSampleMs(
				(FPlatformTime::Seconds() - HashStart) * 1000.0,
				Stat_HashMsSum,
				Stat_HashMsMax,
				Stat_HashSamples);

			for (const FIntRect& C : Candidates)
			{
				++Stat_CandidateRects;
				Stat_CandidatePx += (int64)C.Width() * C.Height();
			}

			if (Candidates.Num() > 0)
			{
				TArray<FIntRect, TInlineAllocator<64>> OptRects;
				for (const FIntRect& C : Candidates)
				{
					bool bMerged = false;
					for (FIntRect& E : OptRects)
					{
						if (SwuiShouldMergeCapped(E, C, MergeRatio, MaxMergeW, MaxMergeH, MaxMergeArea))
						{
							E = FIntRect(
								FIntPoint(FMath::Min(E.Min.X, C.Min.X), FMath::Min(E.Min.Y, C.Min.Y)),
								FIntPoint(FMath::Max(E.Max.X, C.Max.X), FMath::Max(E.Max.Y, C.Max.Y)));
							bMerged = true;
							break;
						}
					}
					if (!bMerged) OptRects.Add(C);
				}

				int32 TotalBytes = 0;
				for (const FIntRect& R2 : OptRects)
				{
					TotalBytes += R2.Width() * 4 * R2.Height();
					OptimizedOverlayRects.Add(FUpdateTextureRegion2D(
						(uint32)R2.Min.X,
						(uint32)R2.Min.Y,
						0,
						0,
						(uint32)R2.Width(),
						(uint32)R2.Height()));
				}

				FSwuiPaintUploadData* UploadData = new FSwuiPaintUploadData;
				UploadData->Texture2DResource = (FTextureResource*)Texture->GetResource();
				UploadData->PackedPixels.SetNumUninitialized(TotalBytes);
				UploadData->Rects.Reserve(OptRects.Num());

				const double PackStart = FPlatformTime::Seconds();
				const double McpyT0 = PackStart;
				uint8* WriteCursor = UploadData->PackedPixels.GetData();
				int64 UploadedPx = 0;

				for (const FIntRect& R2 : OptRects)
				{
					const int32 RectW = R2.Width();
					const int32 RectH = R2.Height();
					const int32 TightPitch = RectW * 4;

					FSwuiPackedRectDesc& Desc = UploadData->Rects.AddDefaulted_GetRef();
					Desc.Region.DestX = (uint32)R2.Min.X;
					Desc.Region.DestY = (uint32)R2.Min.Y;
					Desc.Region.SrcX = 0;
					Desc.Region.SrcY = 0;
					Desc.Region.Width = (uint32)RectW;
					Desc.Region.Height = (uint32)RectH;
					Desc.SrcPitch = (uint32)TightPitch;
					Desc.SrcOffsetBytes = (int32)(WriteCursor - UploadData->PackedPixels.GetData());

					const uint8* Src = BackingBuffer.GetData() + (int64)R2.Min.Y * FullPitch + (int64)R2.Min.X * 4;
					uint8* Dst = WriteCursor;
					for (int32 Row = 0; Row < RectH; ++Row, Src += FullPitch, Dst += TightPitch)
						FPlatformMemory::Memcpy(Dst, Src, TightPitch);

					WriteCursor += (int64)TightPitch * RectH;
					UploadedPx += (int64)RectW * RectH;
					++Stat_UploadedRects;
					const int32 RectArea = RectW * RectH;
					if (RectArea > Stat_LargestUploaded) Stat_LargestUploaded = RectArea;
				}

				for (int32 TileIdx : UploadedTilesThisFrame) UploadedTileMask[TileIdx] = true;
				for (const FIntRect& R2 : OptRects)
					SwuiMarkTilesForRectCustom(R2, TileW, TileH, TilesX, TilesY, UploadedTileMask);

				RecordTimingSampleMs(
					(FPlatformTime::Seconds() - PackStart) * 1000.0,
					Stat_PackMemcpyMsSum,
					Stat_PackMemcpyMsMax,
					Stat_PackMemcpySamples);
				const int64 McpyUs = int64((FPlatformTime::Seconds() - McpyT0) * 1e6);
				Stat_MemcpyUs += McpyUs;
				Stat_MemcpyMaxUs = FMath::Max(Stat_MemcpyMaxUs, McpyUs);
				Stat_UploadedPixels += UploadedPx;

				const bool bSkipUpload = CVarSwuiNoTextureUpload.GetValueOnAnyThread() != 0
					|| InstanceSettings.bSkipTextureUpload || InstanceSettings.bNoTextureUpload;
				if (bSkipUpload) { delete UploadData; }
				else
				{
					if (bHasFreshPaintThisTick && LastPaintArrivalTime > 0.0)
					{
						RecordTimingSampleMs(
							(FPlatformTime::Seconds() - LastPaintArrivalTime) * 1000.0,
							Stat_UploadAfterPaintMsSum,
							Stat_UploadAfterPaintMsMax,
							Stat_UploadAfterPaintSamples);
					}
					ENQUEUE_RENDER_COMMAND(UpdateSwuiViewOptimized)(
						[UploadData](FRHICommandList& CommandList)
						{
							FRHITexture* Tex = UploadData->Texture2DResource->TextureRHI.GetReference();
							const uint8* Base = UploadData->PackedPixels.GetData();
							for (const FSwuiPackedRectDesc& Rd : UploadData->Rects)
								RHIUpdateTexture2D(Tex, 0, Rd.Region, Rd.SrcPitch, Base + Rd.SrcOffsetBytes);
							delete UploadData;
						});
					++Stat_UeUploads;
					if (bHasFreshPaintThisTick) ++Stat_UeUploadsFresh; else ++Stat_UeUploadsBacklog;
				}
			}
		}

		int32 DeferredTiles = 0;
		for (int32 i = 0; i < ActiveDirtyTileMask.Num(); ++i)
			if (ActiveDirtyTileMask[i]) ++DeferredTiles;
		Stat_DeferredTiles += DeferredTiles;
		RecordTimingSampleMs(
			(FPlatformTime::Seconds() - DeferredStart) * 1000.0,
			Stat_DeferredUploadMsSum,
			Stat_DeferredUploadMsMax,
			Stat_DeferredUploadSamples);
	}

	// -----------------------------------------------------------------------
	// One-second aggregate stats log (runs every frame, fires once per second)
	// -----------------------------------------------------------------------
	if (InstanceSettings.bLogSwuiPaintStats && (Now - Stat_LastLogTime >= 1.0))
	{
		const float McAvgMs = Stat_UeUploads > 0 ? float(Stat_MemcpyUs) / Stat_UeUploads / 1000.f : 0.f;
		const float McMaxMs = float(Stat_MemcpyMaxUs) / 1000.f;
		const float DeferredUploadAvgMs = Stat_DeferredUploadSamples > 0 ? float(Stat_DeferredUploadMsSum / Stat_DeferredUploadSamples) : 0.f;
		const float HashAvgMs = Stat_HashSamples > 0 ? float(Stat_HashMsSum / Stat_HashSamples) : 0.f;
		const float PackMemcpyAvgMs = Stat_PackMemcpySamples > 0 ? float(Stat_PackMemcpyMsSum / Stat_PackMemcpySamples) : 0.f;
		const float PaintAfterBeginFrameAvgMs = Stat_PaintAfterBeginFrameSamples > 0 ? float(Stat_PaintAfterBeginFrameMsSum / Stat_PaintAfterBeginFrameSamples) : 0.f;
		const float UploadAfterPaintAvgMs = Stat_UploadAfterPaintSamples > 0 ? float(Stat_UploadAfterPaintMsSum / Stat_UploadAfterPaintSamples) : 0.f;
		UE_LOG(LogSwuiRuntime, Log,
			TEXT("[SwuiPaint] subsystemTicks/s=%d  viewUploadTicks/s=%d  cefPaints/s=%d")
			TEXT("  externalBeginFrames/s=%d  invalidateView/s=%d  beginFramesWithoutPaint/s=%d  paintsAfterInvalidate/s=%d")
			TEXT("  ueUploads/s=%d (fresh=%d backlog=%d)  hudStateFlushes/s=%d")
			TEXT("  extBeginSkip[inactive=%d disabled=%d noBrowser=%d rateLimited=%d]")
			TEXT("  inRects/s=%d  inPx/s=%lld  largestIncoming=%d")
			TEXT("  candRects/s=%d  uploadedRects/s=%d  uploadedPx/s=%lld  largestUploaded=%d")
			TEXT("  skippedTiles/s=%d  changedTiles/s=%d  deferredTiles/s=%d")
			TEXT("  deferredUploadAvgMs=%.3f  deferredUploadMaxMs=%.3f")
			TEXT("  hashAvgMs=%.3f  hashMaxMs=%.3f")
			TEXT("  packMemcpyAvgMs=%.3f  packMemcpyMaxMs=%.3f  lockWaitMs=%.3f")
			TEXT("  paintAfterBeginFrameAvgMs=%.3f  paintAfterBeginFrameMaxMs=%.3f")
			TEXT("  uploadAfterPaintAvgMs=%.3f  uploadAfterPaintMaxMs=%.3f")
			TEXT("  memcpyAvgMs=%.3f  memcpyMaxMs=%.3f  browserFpsCap=%d  tex=%dx%d"),
			Stat_SubsystemTicks, Stat_ViewUploadTicks, Stat_CefPaints,
			Stat_ExternalBeginFrames, Stat_InvalidateView, Stat_BeginFramesWithoutPaint, Stat_PaintsAfterInvalidate,
			Stat_UeUploads, Stat_UeUploadsFresh, Stat_UeUploadsBacklog, Stat_HudStateFlushes,
			Stat_ExternalBeginFrameSkipInactive, Stat_ExternalBeginFrameSkipDisabled, Stat_ExternalBeginFrameSkipNoBrowser, Stat_ExternalBeginFrameSkipRateLimited,
			Stat_IncomingRects, Stat_IncomingPx, Stat_LargestIncoming,
			Stat_CandidateRects, Stat_UploadedRects, Stat_UploadedPixels, Stat_LargestUploaded,
			Stat_SkippedTiles, Stat_ChangedTiles, Stat_DeferredTiles,
			DeferredUploadAvgMs, Stat_DeferredUploadMsMax,
			HashAvgMs, Stat_HashMsMax,
			PackMemcpyAvgMs, Stat_PackMemcpyMsMax, Stat_LockWaitMs,
			PaintAfterBeginFrameAvgMs, Stat_PaintAfterBeginFrameMsMax,
			UploadAfterPaintAvgMs, Stat_UploadAfterPaintMsMax,
			McAvgMs, McMaxMs, WindowlessFrameRate, LastSnapW, LastSnapH);

		Stat_SubsystemTicks = 0; Stat_ViewUploadTicks = 0; Stat_CefPaints = 0; Stat_ExternalBeginFrames = 0;
		Stat_InvalidateView = 0; Stat_BeginFramesWithoutPaint = 0; Stat_PaintsAfterInvalidate = 0;
		Stat_ExternalBeginFrameSkipInactive = 0; Stat_ExternalBeginFrameSkipDisabled = 0; Stat_ExternalBeginFrameSkipNoBrowser = 0; Stat_ExternalBeginFrameSkipRateLimited = 0;
		Stat_UeUploads = 0; Stat_UeUploadsFresh = 0; Stat_UeUploadsBacklog = 0; Stat_HudStateFlushes = 0;
		Stat_IncomingRects   = 0; Stat_IncomingPx     = 0; Stat_LargestIncoming = 0;
		Stat_CandidateRects  = 0; Stat_CandidatePx    = 0;
		Stat_UploadedRects   = 0; Stat_UploadedPixels = 0; Stat_LargestUploaded = 0;
		Stat_SkippedTiles    = 0; Stat_ChangedTiles   = 0; Stat_DeferredTiles = 0;
		Stat_DeferredUploadMsSum = 0.0; Stat_DeferredUploadMsMax = 0.0; Stat_DeferredUploadSamples = 0;
		Stat_HashMsSum = 0.0; Stat_HashMsMax = 0.0; Stat_HashSamples = 0;
		Stat_PackMemcpyMsSum = 0.0; Stat_PackMemcpyMsMax = 0.0; Stat_PackMemcpySamples = 0;
		Stat_PaintAfterBeginFrameMsSum = 0.0; Stat_PaintAfterBeginFrameMsMax = 0.0; Stat_PaintAfterBeginFrameSamples = 0;
		Stat_UploadAfterPaintMsSum = 0.0; Stat_UploadAfterPaintMsMax = 0.0; Stat_UploadAfterPaintSamples = 0;
		Stat_LockWaitMs = 0.0;
		Stat_MemcpyUs        = 0; Stat_MemcpyMaxUs    = 0;
		Stat_LastLogTime     = Now;
	}

	// -----------------------------------------------------------------------
	// Dirty-rect overlay push (~10 Hz, only when bShowDirtyRectOverlay is set)
	// NOTE: The overlay is rendered inside the same CEF surface so it generates
	// its own dirty rects. Disable overlay for final perf measurements.
	// -----------------------------------------------------------------------
	if ((InstanceSettings.bShowDirtyRectOverlay || InstanceSettings.bShowSwuiDirtyRects)
		&& (Now - Stat_OverlayLastPushTime >= 0.1)
		&& (LocalOverlayRects.Num() > 0 || OptimizedOverlayRects.Num() > 0))
	{
		Stat_OverlayLastPushTime = Now;
		const bool bShowOptimizedRects = true;

		const float Elapsed      = FMath::Clamp(float(Now - Stat_LastLogTime) + 1.f, 0.001f, 2.f);
		const int64 DirtyPxRate  = (int64)(Stat_IncomingPx    / Elapsed);
		const int64 UploadPxRate = (int64)(Stat_UploadedPixels / Elapsed);
		const float MemcpyAvgMs  = Stat_UeUploads > 0 ? float(Stat_MemcpyUs) / Stat_UeUploads / 1000.f : 0.f;
		const float MemcpyMaxMs  = float(Stat_MemcpyMaxUs) / 1000.f;
		const float BandRatio    = Stat_IncomingPx > 0 ? float(Stat_UploadedPixels) / float(Stat_IncomingPx) : 0.f;
		const int32 CefPaintsRate = FMath::RoundToInt(float(Stat_CefPaints) / Elapsed);
		const int32 ExternalBeginFramesRate = FMath::RoundToInt(float(Stat_ExternalBeginFrames) / Elapsed);
		const int32 ExternalBeginSkipInactiveRate = FMath::RoundToInt(float(Stat_ExternalBeginFrameSkipInactive) / Elapsed);
		const int32 ExternalBeginSkipDisabledRate = FMath::RoundToInt(float(Stat_ExternalBeginFrameSkipDisabled) / Elapsed);
		const int32 ExternalBeginSkipNoBrowserRate = FMath::RoundToInt(float(Stat_ExternalBeginFrameSkipNoBrowser) / Elapsed);
		const int32 ExternalBeginSkipRateLimitedRate = FMath::RoundToInt(float(Stat_ExternalBeginFrameSkipRateLimited) / Elapsed);
		const int32 InvalidateViewRate = FMath::RoundToInt(float(Stat_InvalidateView) / Elapsed);
		const int32 BeginFramesWithoutPaintRate = FMath::RoundToInt(float(Stat_BeginFramesWithoutPaint) / Elapsed);
		const int32 PaintsAfterInvalidateRate = FMath::RoundToInt(float(Stat_PaintsAfterInvalidate) / Elapsed);
		const int32 UeUploadsRate = FMath::RoundToInt(float(Stat_UeUploads) / Elapsed);
		const int32 UeUploadsFreshRate = FMath::RoundToInt(float(Stat_UeUploadsFresh) / Elapsed);
		const int32 UeUploadsBacklogRate = FMath::RoundToInt(float(Stat_UeUploadsBacklog) / Elapsed);
		const int32 SubsystemTicksRate = FMath::RoundToInt(float(Stat_SubsystemTicks) / Elapsed);
		const int32 ViewUploadTicksRate = FMath::RoundToInt(float(Stat_ViewUploadTicks) / Elapsed);
		const int32 HudStateFlushesRate = FMath::RoundToInt(float(Stat_HudStateFlushes) / Elapsed);
		const float PaintAfterBeginFrameAvgMs = Stat_PaintAfterBeginFrameSamples > 0 ? float(Stat_PaintAfterBeginFrameMsSum / Stat_PaintAfterBeginFrameSamples) : 0.f;
		const float UploadAfterPaintAvgMs = Stat_UploadAfterPaintSamples > 0 ? float(Stat_UploadAfterPaintMsSum / Stat_UploadAfterPaintSamples) : 0.f;

		FString IncomingRectsJson;
		IncomingRectsJson.Reserve(LocalOverlayRects.Num() * 52 + 2);
		IncomingRectsJson.AppendChar('[');
		for (int32 i = 0; i < LocalOverlayRects.Num(); ++i)
		{
			const FUpdateTextureRegion2D& R = LocalOverlayRects[i];
			if (i > 0) IncomingRectsJson.AppendChar(',');
			IncomingRectsJson += FString::Printf(
				TEXT("{\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u,\"area\":%lld}"),
				R.DestX, R.DestY, R.Width, R.Height, (int64)R.Width * R.Height);
		}
		IncomingRectsJson.AppendChar(']');

		FString OptimizedRectsJson;
		OptimizedRectsJson.Reserve(OptimizedOverlayRects.Num() * 52 + 2);
		OptimizedRectsJson.AppendChar('[');
		for (int32 i = 0; i < OptimizedOverlayRects.Num(); ++i)
		{
			const FUpdateTextureRegion2D& R = OptimizedOverlayRects[i];
			if (i > 0) OptimizedRectsJson.AppendChar(',');
			OptimizedRectsJson += FString::Printf(
				TEXT("{\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u,\"area\":%lld}"),
				R.DestX, R.DestY, R.Width, R.Height, (int64)R.Width * R.Height);
		}
		OptimizedRectsJson.AppendChar(']');

		const FString& ActiveRectsJson = (bShowOptimizedRects && OptimizedOverlayRects.Num() > 0)
			? OptimizedRectsJson
			: IncomingRectsJson;

		const FString Script = FString::Printf(
			TEXT("if(!window.__SWUI_RAF_MONITOR__){")
			TEXT("window.__SWUI_JS_RAF_S=0;")
			TEXT("window.__SWUI_RAF_MONITOR__={count:0,last:performance.now(),hz:0};")
			TEXT("window.__SWUI_RAF_MONITOR_TICK__=function(t){")
			TEXT("var s=window.__SWUI_RAF_MONITOR__;s.count++;")
			TEXT("if(t-s.last>=1000){s.hz=Math.round((s.count*1000)/(t-s.last));s.count=0;s.last=t;window.__SWUI_JS_RAF_S=s.hz;}")
			TEXT("requestAnimationFrame(window.__SWUI_RAF_MONITOR_TICK__);};")
			TEXT("requestAnimationFrame(window.__SWUI_RAF_MONITOR_TICK__);}")
			TEXT("if(window.__SWUI_DEBUG_RECTS__)window.__SWUI_DEBUG_RECTS__(")
			TEXT("{\"texW\":%d,\"texH\":%d,\"rects\":%s,")
			TEXT("\"incomingRects\":%s,\"optimizedRects\":%s,")
			TEXT("\"stats\":{\"largestRect\":%d,\"largestUploaded\":%d,")
			TEXT("\"jsRafS\":(window.__SWUI_JS_RAF_S||0),")
			TEXT("\"browserFpsCap\":%d,")
			TEXT("\"subsystemTicksS\":%d,\"viewUploadTicksS\":%d,")
			TEXT("\"cefPaintsS\":%d,\"externalBeginFramesS\":%d,\"invalidateViewS\":%d,\"beginFramesWithoutPaintS\":%d,\"paintsAfterInvalidateS\":%d,")
			TEXT("\"externalBeginSkipInactiveS\":%d,\"externalBeginSkipDisabledS\":%d,\"externalBeginSkipNoBrowserS\":%d,\"externalBeginSkipRateLimitedS\":%d,")
			TEXT("\"ueUploadsS\":%d,\"freshUploadsS\":%d,\"ueUploadsFreshS\":%d,\"ueUploadsBacklogS\":%d,\"hudStateFlushesS\":%d,")
			TEXT("\"dirtyPxS\":%lld,\"uploadedPxS\":%lld,")
			TEXT("\"skippedTiles\":%d,\"changedTiles\":%d,")
			TEXT("\"paintAfterBeginFrameAvgMs\":%.3f,\"paintAfterBeginFrameMaxMs\":%.3f,")
			TEXT("\"uploadAfterPaintAvgMs\":%.3f,\"uploadAfterPaintMaxMs\":%.3f,")
			TEXT("\"memcpyAvgMs\":%.3f,\"memcpyMaxMs\":%.3f,\"bandRatio\":%.2f}});"),
			LastSnapW, LastSnapH, *ActiveRectsJson, *IncomingRectsJson, *OptimizedRectsJson,
			Stat_LargestIncoming, Stat_LargestUploaded,
			WindowlessFrameRate,
			SubsystemTicksRate, ViewUploadTicksRate,
			CefPaintsRate, ExternalBeginFramesRate, InvalidateViewRate, BeginFramesWithoutPaintRate, PaintsAfterInvalidateRate,
			ExternalBeginSkipInactiveRate, ExternalBeginSkipDisabledRate, ExternalBeginSkipNoBrowserRate, ExternalBeginSkipRateLimitedRate,
			UeUploadsRate, UeUploadsFreshRate, UeUploadsFreshRate, UeUploadsBacklogRate, HudStateFlushesRate,
			DirtyPxRate, UploadPxRate,
			Stat_SkippedTiles, Stat_ChangedTiles,
			PaintAfterBeginFrameAvgMs, (float)Stat_PaintAfterBeginFrameMsMax,
			UploadAfterPaintAvgMs, (float)Stat_UploadAfterPaintMsMax,
			MemcpyAvgMs, MemcpyMaxMs, BandRatio);

		if (CefData && CefData->Browser)
		{
			const std::string StdScript = TCHAR_TO_UTF8(*Script);
			CefPostTask(TID_UI, new FSwuiOverlayPushTask(CefData->Browser, StdScript));
		}
	}
}

UTexture2D* USwuiView::GetOrCreateTexture(int32 InWidth, int32 InHeight)
{
	if (!Texture || Texture->GetSizeX() != InWidth || Texture->GetSizeY() != InHeight)
	{
		DestroyTexture();

		Texture = UTexture2D::CreateTransient(InWidth, InHeight, PF_B8G8R8A8);
		Texture->AddToRoot();
		Texture->UpdateResource();
		bNeedsFullBaselineUpload = true;
		DirtyTileScanCursor = 0;
		LastTileHashes.Reset();
		UploadedTileMask.Reset();
		ActiveDirtyTileMask.Reset();

		ResetMatInstance();
	}

	return Texture;
}

void USwuiView::ResetTexture()
{
	DestroyTexture();

	Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
	Texture->AddToRoot();
	Texture->UpdateResource();
	bNeedsFullBaselineUpload = true;
	DirtyTileScanCursor = 0;
	LastTileHashes.Reset();
	UploadedTileMask.Reset();
	ActiveDirtyTileMask.Reset();

	ResetMatInstance();
}

void USwuiView::DestroyTexture()
{
	if (Texture)
	{
		Texture->RemoveFromRoot();
		Texture->MarkAsGarbage();
		Texture = nullptr;
	}
}

void USwuiView::ResetMatInstance()
{
	if (!Texture || !BaseMaterial || TextureParameterName.IsNone())
	{
		return;
	}

	if (!MaterialInstance)
	{
		MaterialInstance = UMaterialInstanceDynamic::Create(BaseMaterial, NULL);
		if (!MaterialInstance)
		{
			return;
		}
	}

	UTexture* Tex = nullptr;
	if (!MaterialInstance->GetTextureParameterValue(TextureParameterName, Tex))
	{
		UE_LOG(LogSwuiRuntime, Warning, TEXT("USwuiView: Texture parameter '%s' not found in material"), *TextureParameterName.ToString());
		return;
	}

	MaterialInstance->SetTextureParameterValue(TextureParameterName, Texture);
}

void USwuiView::BeginDestroy()
{
	if (CefData && CefData->Browser)
	{
		CefData->Browser->GetHost()->CloseBrowser(true);
	}

	DestroyTexture();

	Super::BeginDestroy();
}
