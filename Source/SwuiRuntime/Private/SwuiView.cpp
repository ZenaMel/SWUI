#include "SwuiView.h"
#include "RenderHandler.h"
#include "ISwuiRuntime.h"
#include "SwuiManager.h"

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

// ---------------------------------------------------------------------------
// Rect optimization helpers — file-scope, called only from CEF renderer thread.
// ---------------------------------------------------------------------------
static constexpr int32  SwuiMinDirtyRectSize   = 32;   // expand tiny rects to at least this px size
static constexpr double SwuiMaxMergeWasteRatio = 1.15; // merge two rects only if union ≤ sum * this

static FIntRect SwuiClampRect(const FIntRect& R, int32 TexW, int32 TexH)
{
	return FIntRect(
		FMath::Clamp(R.Min.X, 0, TexW), FMath::Clamp(R.Min.Y, 0, TexH),
		FMath::Clamp(R.Max.X, 0, TexW), FMath::Clamp(R.Max.Y, 0, TexH));
}

static FIntRect SwuiExpandToMinSize(const FIntRect& R, int32 TexW, int32 TexH)
{
	const int32 ExtraW = FMath::Max(0, SwuiMinDirtyRectSize - R.Width());
	const int32 ExtraH = FMath::Max(0, SwuiMinDirtyRectSize - R.Height());
	const FIntRect Expanded(
		R.Min.X - ExtraW / 2,             R.Min.Y - ExtraH / 2,
		R.Max.X + (ExtraW - ExtraW / 2),  R.Max.Y + (ExtraH - ExtraH / 2));
	return SwuiClampRect(Expanded, TexW, TexH);
}

static bool SwuiShouldMerge(const FIntRect& A, const FIntRect& B)
{
	const int64 SumArea = (int64)A.Width() * A.Height() + (int64)B.Width() * B.Height();
	if (SumArea <= 0) return false;
	const FIntRect U(
		FIntPoint(FMath::Min(A.Min.X, B.Min.X), FMath::Min(A.Min.Y, B.Min.Y)),
		FIntPoint(FMath::Max(A.Max.X, B.Max.X), FMath::Max(A.Max.Y, B.Max.Y)));
	return (double)((int64)U.Width() * U.Height()) <= (double)SumArea * SwuiMaxMergeWasteRatio;
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
	if (InstanceSettings.OverrideFrameRate > 0)
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

UTexture2D* USwuiView::GetTexture() const
{
	return Texture;
}

void USwuiView::OnPaint(const void* Buffer, FUpdateTextureRegion2D* Regions, int32 RegionCount, int32 InWidth, int32 InHeight)
{
	GetOrCreateTexture(InWidth, InHeight);

	if (!Texture || !Texture->GetResource())
	{
		FMemory::Free(Regions);
		return;
	}

	// -----------------------------------------------------------------------
	// Diagnostic bookkeeping (CEF renderer thread only — no atomics needed)
	// -----------------------------------------------------------------------
	++Stat_Paints;
	Stat_DirtyRects += RegionCount;

	// Stage gate 1: skip everything downstream of the paint callback.
	if (InstanceSettings.bSkipOnPaintProcessing) { FMemory::Free(Regions); return; }

	int64 DirtyAreaThisPaint = 0;
	for (int32 i = 0; i < RegionCount; ++i)
	{
		// Clamp degenerate rects against texture bounds
		Regions[i].SrcX  = Regions[i].DestX = (uint32)FMath::Clamp((int32)Regions[i].SrcX,  0, InWidth);
		Regions[i].SrcY  = Regions[i].DestY = (uint32)FMath::Clamp((int32)Regions[i].SrcY,  0, InHeight);
		Regions[i].Width  = (uint32)FMath::Clamp((int32)Regions[i].Width,  0, InWidth  - (int32)Regions[i].SrcX);
		Regions[i].Height = (uint32)FMath::Clamp((int32)Regions[i].Height, 0, InHeight - (int32)Regions[i].SrcY);

		const int64 RectArea = (int64)Regions[i].Width * (int64)Regions[i].Height;
		DirtyAreaThisPaint += RectArea;
		if ((int32)RectArea > Stat_LargestDirtyRect) Stat_LargestDirtyRect = (int32)RectArea;
	}
	Stat_DirtyPixels += DirtyAreaThisPaint;

	// Snapshot valid dirty rects for the overlay push (captured before any FMemory::Free).
	// Zero-size rects are excluded. Only allocated when overlay is active.
	TArray<FUpdateTextureRegion2D> OverlayRects;
	if (InstanceSettings.bShowDirtyRectOverlay)
	{
		for (int32 i = 0; i < RegionCount; ++i)
			if (Regions[i].Width > 0 && Regions[i].Height > 0)
				OverlayRects.Add(Regions[i]);
	}

	// Stage gate 2: skip rect strategy, memcpy, and upload.
	if (InstanceSettings.bSkipDirtyRectStrategy) { FMemory::Free(Regions); return; }

	// -----------------------------------------------------------------------
	// Rect optimization: expand tiny rects to MinSize, greedy proximity merge.
	// Keeps spatially distant HUD regions separate; only merges when cheap.
	//   SwuiMinDirtyRectSize   = 32 px  (solves tiny-rect RHI overhead)
	//   SwuiMaxMergeWasteRatio = 1.15   (merge only if union ≤ sum * 1.15)
	// -----------------------------------------------------------------------
	const int32 FullPitch = InWidth * 4;
	const bool  bVerbose  = CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0;

	TArray<FIntRect, TInlineAllocator<32>> OptRects;
	for (int32 i = 0; i < RegionCount; ++i)
	{
		if (Regions[i].Width == 0 || Regions[i].Height == 0) continue;
		FIntRect R(
			(int32)Regions[i].SrcX,
			(int32)Regions[i].SrcY,
			(int32)Regions[i].SrcX + (int32)Regions[i].Width,
			(int32)Regions[i].SrcY + (int32)Regions[i].Height);
		R = SwuiExpandToMinSize(R, InWidth, InHeight);

		bool bMerged = false;
		for (FIntRect& E : OptRects)
		{
			if (SwuiShouldMerge(E, R))
			{
				E = FIntRect(
					FIntPoint(FMath::Min(E.Min.X, R.Min.X), FMath::Min(E.Min.Y, R.Min.Y)),
					FIntPoint(FMath::Max(E.Max.X, R.Max.X), FMath::Max(E.Max.Y, R.Max.Y)));
				bMerged = true;
				break;
			}
		}
		if (!bMerged) OptRects.Add(R);
	}

	FMemory::Free(Regions);

	if (OptRects.Num() == 0) return;

	// Stage gate 3: skip pixel copy into upload buffers.
	if (InstanceSettings.bSkipPaintMemcpy || InstanceSettings.bFreezeTexture) return;

	// ---- Single-allocation tight-pack for all optimized rects ----
	int32 TotalBytes = 0;
	for (const FIntRect& R : OptRects)
		TotalBytes += R.Width() * 4 * R.Height();

	FSwuiPaintUploadData* UploadData = new FSwuiPaintUploadData;
	UploadData->Texture2DResource = (FTextureResource*)Texture->GetResource();
	UploadData->PackedPixels.SetNumUninitialized(TotalBytes);
	UploadData->Rects.Reserve(OptRects.Num());

	const double McpyT0 = FPlatformTime::Seconds();
	uint8* WriteCursor = UploadData->PackedPixels.GetData();
	int64  UploadedAreaThisPaint = 0;

	for (const FIntRect& R : OptRects)
	{
		const int32 RectW      = R.Width();
		const int32 RectH      = R.Height();
		const int32 TightPitch = RectW * 4;

		FSwuiPackedRectDesc& Desc   = UploadData->Rects.AddDefaulted_GetRef();
		Desc.Region.DestX           = (uint32)R.Min.X;
		Desc.Region.DestY           = (uint32)R.Min.Y;
		Desc.Region.SrcX            = 0;
		Desc.Region.SrcY            = 0;
		Desc.Region.Width           = (uint32)RectW;
		Desc.Region.Height          = (uint32)RectH;
		Desc.SrcPitch               = (uint32)TightPitch;
		Desc.SrcOffsetBytes         = (int32)(WriteCursor - UploadData->PackedPixels.GetData());

		const uint8* Src = (const uint8*)Buffer + (int64)R.Min.Y * FullPitch + (int64)R.Min.X * 4;
		uint8*       Dst = WriteCursor;
		for (int32 Row = 0; Row < RectH; ++Row)
		{
			FPlatformMemory::Memcpy(Dst, Src, TightPitch);
			Src += FullPitch;
			Dst += TightPitch;
		}
		WriteCursor          += (int64)TightPitch * RectH;
		UploadedAreaThisPaint += (int64)RectW * RectH;
	}

	const int64 McpyUs = int64((FPlatformTime::Seconds() - McpyT0) * 1e6);
	Stat_MemcpyUs       += McpyUs;
	Stat_MemcpyMaxUs     = FMath::Max(Stat_MemcpyMaxUs, McpyUs);
	Stat_UploadedPixels += UploadedAreaThisPaint;

	if (bVerbose)
	{
		UE_LOG(LogSwuiRuntime, Verbose,
			TEXT("[SwuiPaint][frame] inRects=%d optRects=%d dirtyPx=%lld uploadPx=%lld ratio=%.2f memcpy=%.3fms"),
			RegionCount, OptRects.Num(), DirtyAreaThisPaint, UploadedAreaThisPaint,
			DirtyAreaThisPaint > 0 ? (float)UploadedAreaThisPaint / (float)DirtyAreaThisPaint : 0.f,
			McpyUs / 1000.f);
	}

	const bool bSkipUpload = CVarSwuiNoTextureUpload.GetValueOnAnyThread() != 0
		|| InstanceSettings.bSkipTextureUpload
		|| InstanceSettings.bNoTextureUpload;
	if (bSkipUpload) { delete UploadData; }
	else
	{
		ENQUEUE_RENDER_COMMAND(UpdateSwuiViewOptimized)(
			[UploadData](FRHICommandList& CommandList)
			{
				FRHITexture* Tex  = UploadData->Texture2DResource->TextureRHI.GetReference();
				const uint8* Base = UploadData->PackedPixels.GetData();
				for (const FSwuiPackedRectDesc& Rd : UploadData->Rects)
					RHIUpdateTexture2D(Tex, 0, Rd.Region, Rd.SrcPitch, Base + Rd.SrcOffsetBytes);
				delete UploadData;
			});
	}

	// -----------------------------------------------------------------------
	// One-second aggregate stats log
	// -----------------------------------------------------------------------
	const double Now = FPlatformTime::Seconds();
	if (Now - Stat_LastLogTime >= 1.0)
	{
		const float McAvgMs = Stat_Paints > 0 ? float(Stat_MemcpyUs) / Stat_Paints / 1000.f : 0.f;
		const float McMaxMs = Stat_MemcpyMaxUs / 1000.f;
		UE_LOG(LogSwuiRuntime, Log,
			TEXT("[SwuiPaint] paints/s=%d  dirtyRects/s=%d  dirtyPx/s=%lld  uploadedPx/s=%lld")
			TEXT("  memcpyAvgMs=%.3f  memcpyMaxMs=%.3f  bandRatio=%.2f  largestDirtyRect=%d  tex=%dx%d"),
			Stat_Paints, Stat_DirtyRects, Stat_DirtyPixels, Stat_UploadedPixels,
			McAvgMs, McMaxMs,
			Stat_DirtyPixels > 0 ? float(Stat_UploadedPixels) / float(Stat_DirtyPixels) : 0.f,
			Stat_LargestDirtyRect,
			InWidth, InHeight);

		Stat_Paints           = 0;
		Stat_DirtyRects       = 0;
		Stat_DirtyPixels      = 0;
		Stat_UploadedPixels   = 0;
		Stat_MemcpyUs         = 0;
		Stat_MemcpyMaxUs      = 0;
		Stat_LargestDirtyRect = 0;
		Stat_LastLogTime      = Now;
	}

	// -----------------------------------------------------------------------
	// Dirty-rect overlay push (~10 Hz, only when bShowDirtyRectOverlay is set)
	// NOTE: The overlay is rendered inside the same CEF surface, so it generates
	// its own dirty rects. Use logs with the overlay disabled for final measurements.
	// -----------------------------------------------------------------------
	if (InstanceSettings.bShowDirtyRectOverlay && (Now - Stat_OverlayLastPushTime >= 0.1))
	{
		Stat_OverlayLastPushTime = Now;

		// Rates based on the current accumulation window (capped to 1 s to avoid
		// huge numbers on the very first push before the log timer fires).
		const float Elapsed       = FMath::Clamp((float)(Now - (Stat_LastLogTime - 1.0)), 0.001f, 1.0f);
		const int64 DirtyPxRate   = Elapsed > 0.f ? (int64)(Stat_DirtyPixels    / Elapsed) : 0;
		const int64 UploadPxRate  = Elapsed > 0.f ? (int64)(Stat_UploadedPixels / Elapsed) : 0;
		const float MemcpyAvgMs   = Stat_Paints > 0 ? float(Stat_MemcpyUs) / Stat_Paints / 1000.f : 0.f;
		const float MemcpyMaxMs   = float(Stat_MemcpyMaxUs) / 1000.f;
		const float BandRatio     = Stat_DirtyPixels > 0 ? float(Stat_UploadedPixels) / float(Stat_DirtyPixels) : 0.f;

		// Build rects JSON array
		FString RectsJson;
		RectsJson.Reserve(OverlayRects.Num() * 52 + 2);
		RectsJson.AppendChar('[');
		for (int32 i = 0; i < OverlayRects.Num(); ++i)
		{
			const FUpdateTextureRegion2D& R = OverlayRects[i];
			if (i > 0) RectsJson.AppendChar(',');
			RectsJson += FString::Printf(
				TEXT("{\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u,\"area\":%lld}"),
				R.DestX, R.DestY, R.Width, R.Height, (int64)R.Width * R.Height);
		}
		RectsJson.AppendChar(']');

		const FString Script = FString::Printf(
			TEXT("if(window.__SWUI_DEBUG_RECTS__)window.__SWUI_DEBUG_RECTS__("
				 "{\"texW\":%d,\"texH\":%d,\"rects\":%s,"
				 "\"stats\":{\"largestRect\":%d,\"dirtyPxS\":%lld,\"uploadedPxS\":%lld,"
				 "\"memcpyAvgMs\":%.3f,\"memcpyMaxMs\":%.3f,\"bandRatio\":%.2f}});"),
			InWidth, InHeight, *RectsJson,
			Stat_LargestDirtyRect, DirtyPxRate, UploadPxRate,
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
