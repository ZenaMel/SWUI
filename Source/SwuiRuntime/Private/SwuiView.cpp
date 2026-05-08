#include "SwuiView.h"
#include "RenderHandler.h"
#include "ISwuiRuntime.h"
#include "SwuiManager.h"

// ---------------------------------------------------------------------------
// Console variables — paint tuning & debug isolation
// ---------------------------------------------------------------------------

// If >0 accepted as the multiplier at which we abandon band-merge and use
// per-rect uploads instead (e.g. 1.25 = switch when band is 25% bigger).
static TAutoConsoleVariable<float> CVarSwuiMaxMergeOvercopyRatio(
	TEXT("swui.paint.MaxMergeOvercopyRatio"),
	1.25f,
	TEXT("Band-merge overcopy guard: if (bandArea / dirtyArea) > this value, use per-rect uploads instead."),
	ECVF_Default);

// Maximum number of individual RHIUpdateTexture2D calls per paint before we
// fall back to band-merge (avoids CPU overhead from many small RHI calls).
static TAutoConsoleVariable<int32> CVarSwuiMaxPerRectUploads(
	TEXT("swui.paint.MaxPerRectUploads"),
	32,
	TEXT("Max per-rect RHI updates per paint before falling back to band-merge."),
	ECVF_Default);

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

USwuiView::USwuiView()
{
	Texture = nullptr;
	CefData = MakeShared<FSwuiViewCefData>();
}

void USwuiView::Init()
{
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

	// Use engine max FPS if set; otherwise pass a high ceiling (300) so CEF
	// self-limits to whatever the OS/driver actually supports rather than
	// being artificially capped by us.
	int32 TargetFPS = 300;
	if (GEngine && GEngine->GetMaxFPS() > 0)
		TargetFPS = FMath::RoundToInt(GEngine->GetMaxFPS());

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

	int64 DirtyAreaThisPaint = 0;
	for (int32 i = 0; i < RegionCount; ++i)
	{
		// Clamp and skip degenerate rects from CEF browser bugs
		Regions[i].SrcX  = Regions[i].DestX = FMath::Clamp((int32)Regions[i].SrcX,  0, InWidth);
		Regions[i].SrcY  = Regions[i].DestY = FMath::Clamp((int32)Regions[i].SrcY,  0, InHeight);
		Regions[i].Width  = (uint32)FMath::Clamp((int32)Regions[i].Width,  0, InWidth  - (int32)Regions[i].SrcX);
		Regions[i].Height = (uint32)FMath::Clamp((int32)Regions[i].Height, 0, InHeight - (int32)Regions[i].SrcY);

		const int64 RectArea = (int64)Regions[i].Width * (int64)Regions[i].Height;
		DirtyAreaThisPaint += RectArea;
		if ((int32)RectArea > Stat_LargestDirtyRect) Stat_LargestDirtyRect = (int32)RectArea;
	}
	Stat_DirtyPixels += DirtyAreaThisPaint;

	// -----------------------------------------------------------------------
	// Upload strategy selection
	// -----------------------------------------------------------------------

	// Compute band (MinRow..MaxRow) as if we did the simple merge
	int32 MinRow = InHeight, MaxRow = 0;
	for (int32 i = 0; i < RegionCount; ++i)
	{
		if (Regions[i].Width == 0 || Regions[i].Height == 0) continue;
		MinRow = FMath::Min(MinRow, (int32)Regions[i].SrcY);
		MaxRow = FMath::Max(MaxRow, (int32)Regions[i].SrcY + (int32)Regions[i].Height);
	}
	if (MinRow > MaxRow) { FMemory::Free(Regions); return; } // all rects were zero-size
	MinRow = FMath::Clamp(MinRow, 0, InHeight);
	MaxRow = FMath::Clamp(MaxRow, 0, InHeight);

	const int32  FullPitch  = InWidth * 4;
	const int32  BandRows   = FMath::Max(1, MaxRow - MinRow);
	const int64  BandArea   = (int64)InWidth * BandRows;

	const float  MaxRatio    = CVarSwuiMaxMergeOvercopyRatio.GetValueOnAnyThread();
	const int32  MaxPerRects = CVarSwuiMaxPerRectUploads.GetValueOnAnyThread();
	const bool   bVerbose    = CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0;

	// Count valid (non-zero) rects
	int32 ValidRectCount = 0;
	for (int32 i = 0; i < RegionCount; ++i)
		if (Regions[i].Width > 0 && Regions[i].Height > 0) ++ValidRectCount;

	// Per-rect uploads: each rect gets its own sub-buffer copy keyed to its row range.
	// Band upload: one full-width strip covering [MinRow..MaxRow].
	// Decision: use per-rect if band wastes >MaxRatio× the dirty area AND we have few enough rects.
	const bool bUsePerRect = (DirtyAreaThisPaint > 0)
		&& (BandArea > (int64)(DirtyAreaThisPaint * MaxRatio))
		&& (ValidRectCount <= MaxPerRects);

	FUpdateTextureRegionsData* RegionData = new FUpdateTextureRegionsData;
	RegionData->Texture2DResource = (FTextureResource*)Texture->GetResource();
	RegionData->SrcBpp   = 4;
	RegionData->SrcPitch = FullPitch; // stride is always full-width (CEF buffer layout)

	int64 UploadedAreaThisPaint = 0;
	const double McpyT0 = FPlatformTime::Seconds();

	if (bUsePerRect)
	{
		// ---- Per-rect path ----
		// For each valid rect, copy only its row-slice from the CEF buffer.
		// We pack them sequentially in SrcData and adjust SrcY to be the byte offset.
		// NOTE: SrcPitch stays = FullPitch because CEF's buffer is full-width.
		// We just copy fewer rows and re-point SrcY.

		// Pre-compute total bytes needed (sum of each rect's row range)
		int64 TotalBytes = 0;
		for (int32 i = 0; i < RegionCount; ++i)
		{
			if (Regions[i].Width == 0 || Regions[i].Height == 0) { Regions[i].SrcX = 0; Regions[i].SrcY = 0; continue; }
			TotalBytes += (int64)Regions[i].Height * FullPitch;
			UploadedAreaThisPaint += (int64)Regions[i].Width * (int64)Regions[i].Height;
		}

		RegionData->SrcData.SetNumUninitialized(TotalBytes);
		uint8* Dst = RegionData->SrcData.GetData();

		for (int32 i = 0; i < RegionCount; ++i)
		{
			if (Regions[i].Width == 0 || Regions[i].Height == 0) continue;

			const int32 RectMinRow = (int32)Regions[i].SrcY;
			const int32 RowCount   = (int32)Regions[i].Height;
			const int64 SrcOffset  = (int64)RectMinRow * FullPitch;

			FPlatformMemory::Memcpy(Dst, (const uint8*)Buffer + SrcOffset, (int64)RowCount * FullPitch);

			// Point this region's SrcY to its position within our packed SrcData.
			// The byte offset of Dst from SrcData.GetData() divided by SrcPitch = virtual row.
			const int32 VirtualRow = (int32)((Dst - RegionData->SrcData.GetData()) / FullPitch);
			Regions[i].SrcY = VirtualRow;
			// SrcX stays as-is; RHIUpdateTexture2D will stride by SrcPitch.

			Dst += (int64)RowCount * FullPitch;
		}
	}
	else
	{
		// ---- Band path ----
		// One full-width copy of [MinRow..MaxRow], rebase all SrcY relative to it.
		RegionData->SrcData.SetNumUninitialized((int64)FullPitch * BandRows);
		FPlatformMemory::Memcpy(RegionData->SrcData.GetData(),
			(const uint8*)Buffer + (int64)MinRow * FullPitch,
			(int64)FullPitch * BandRows);

		for (int32 i = 0; i < RegionCount; ++i)
			Regions[i].SrcY = (Regions[i].Height > 0) ? FMath::Max(0, (int32)Regions[i].SrcY - MinRow) : 0;

		UploadedAreaThisPaint = BandArea;
	}

	const int64 McpyUs = int64((FPlatformTime::Seconds() - McpyT0) * 1e6);
	Stat_MemcpyUs    += McpyUs;
	Stat_MemcpyMaxUs  = FMath::Max(Stat_MemcpyMaxUs, McpyUs);
	Stat_UploadedPixels += UploadedAreaThisPaint;

	// Verbose per-paint log
	if (bVerbose)
	{
		UE_LOG(LogSwuiRuntime, Verbose,
			TEXT("[SwuiPaint][frame] rects=%d valid=%d strategy=%s dirtyPx=%lld uploadPx=%lld ratio=%.2f memcpy=%.3fms"),
			RegionCount, ValidRectCount,
			bUsePerRect ? TEXT("per-rect") : TEXT("band"),
			DirtyAreaThisPaint, UploadedAreaThisPaint,
			DirtyAreaThisPaint > 0 ? (float)UploadedAreaThisPaint / (float)DirtyAreaThisPaint : 0.f,
			McpyUs / 1000.f);
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

		Stat_Paints          = 0;
		Stat_DirtyRects      = 0;
		Stat_DirtyPixels     = 0;
		Stat_UploadedPixels  = 0;
		Stat_MemcpyUs        = 0;
		Stat_MemcpyMaxUs     = 0;
		Stat_LargestDirtyRect = 0;
		Stat_LastLogTime     = Now;
	}

	// -----------------------------------------------------------------------
	// Enqueue RHI upload
	// -----------------------------------------------------------------------
	RegionData->NumRegions = RegionCount;
	RegionData->Regions    = Regions;

	const bool bSkipUpload = CVarSwuiNoTextureUpload.GetValueOnAnyThread() != 0;
	if (bSkipUpload)
	{
		FMemory::Free(RegionData->Regions);
		delete RegionData;
		return;
	}

	ENQUEUE_RENDER_COMMAND(UpdateSwuiViewCommand)(
		[RegionData](FRHICommandList& CommandList)
		{
			for (uint32 RegionIndex = 0; RegionIndex < RegionData->NumRegions; RegionIndex++)
			{
				if (RegionData->Regions[RegionIndex].Width == 0 || RegionData->Regions[RegionIndex].Height == 0)
					continue;

				RHIUpdateTexture2D(
					RegionData->Texture2DResource->TextureRHI->GetTexture2D(),
					0,
					RegionData->Regions[RegionIndex],
					RegionData->SrcPitch,
					RegionData->SrcData.GetData());
			}

			FMemory::Free(RegionData->Regions);
			delete RegionData;
		});
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
