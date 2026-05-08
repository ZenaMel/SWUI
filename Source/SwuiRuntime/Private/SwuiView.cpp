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

	// Apply upload strategy thresholds: instance overrides → project settings → CVar defaults.
	if (InstanceSettings.OverrideBandOvercopyRatio > 0.f)
		CVarSwuiMaxMergeOvercopyRatio->Set(InstanceSettings.OverrideBandOvercopyRatio, ECVF_SetByCode);
	else if (Settings)
		CVarSwuiMaxMergeOvercopyRatio->Set(Settings->MaxBandOvercopyRatio, ECVF_SetByCode);

	if (InstanceSettings.OverrideMaxPerRectUploads > 0)
		CVarSwuiMaxPerRectUploads->Set(InstanceSettings.OverrideMaxPerRectUploads, ECVF_SetByCode);
	else if (Settings)
		CVarSwuiMaxPerRectUploads->Set(Settings->MaxPerRectUploads, ECVF_SetByCode);

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

	// Stage gate 2: skip rect strategy, memcpy, and upload.
	if (InstanceSettings.bSkipDirtyRectStrategy) { FMemory::Free(Regions); return; }

	// -----------------------------------------------------------------------
	// Band extents (used for both strategy decision and band path)
	// -----------------------------------------------------------------------
	int32 MinRow = InHeight, MaxRow = 0;
	int32 ValidRectCount = 0;
	for (int32 i = 0; i < RegionCount; ++i)
	{
		if (Regions[i].Width == 0 || Regions[i].Height == 0) continue;
		MinRow = FMath::Min(MinRow, (int32)Regions[i].SrcY);
		MaxRow = FMath::Max(MaxRow, (int32)Regions[i].SrcY + (int32)Regions[i].Height);
		++ValidRectCount;
	}

	if (ValidRectCount == 0) { FMemory::Free(Regions); return; }
	MinRow = FMath::Clamp(MinRow, 0, InHeight);
	MaxRow = FMath::Clamp(MaxRow, 0, InHeight);

	const int32 FullPitch = InWidth * 4;
	const int32 BandRows  = FMath::Max(1, MaxRow - MinRow);
	const int64 BandArea  = (int64)InWidth * BandRows;

	// Read thresholds from CVars (runtime-overridable); CVars are initialised from Settings in Init().
	const float MaxRatio    = CVarSwuiMaxMergeOvercopyRatio.GetValueOnAnyThread();
	const int32 MaxPerRects = CVarSwuiMaxPerRectUploads.GetValueOnAnyThread();
	const bool  bVerbose    = CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0;

	// Use per-rect tight-pack when band overcopy exceeds the ratio AND rect count is manageable.
	const bool bUsePerRect = (DirtyAreaThisPaint > 0)
		&& (BandArea > (int64)(DirtyAreaThisPaint * MaxRatio))
		&& (ValidRectCount <= MaxPerRects);

	int64        UploadedAreaThisPaint = 0;
	const double McpyT0 = FPlatformTime::Seconds();

	if (bUsePerRect)
	{
		// ---- Per-rect tight-pack path ----
		//
		// Each rect gets its own tightly-packed pixel buffer:
		//   SrcPitch = Rect.Width * 4  (no wasted columns)
		//   SrcX = 0, SrcY = 0        (data starts at byte 0 of the buffer)
		//   DestX, DestY              (where to place it in the texture)
		//
		// This is the safe pattern for RHIUpdateTexture2D — no virtual-row rebasing,
		// no dependency on full-width stride in the source buffer.
		// Lifetime: FSwuiPerRectUploadData is heap-allocated and deleted on RHI thread.

		// Stage gate 3 (per-rect path): skip pixel copy into upload buffers.
		if (InstanceSettings.bSkipPaintMemcpy || InstanceSettings.bFreezeTexture) { FMemory::Free(Regions); return; }

		// ---- Single-allocation packing ----
		// Pass 1: compute total bytes needed so we do ONE SetNumUninitialized.
		int32 TotalBytes = 0;
		for (int32 i = 0; i < RegionCount; ++i)
		{
			if (Regions[i].Width == 0 || Regions[i].Height == 0) continue;
			TotalBytes += (int32)Regions[i].Width * 4 * (int32)Regions[i].Height;
		}

		FSwuiPaintUploadData* UploadData = new FSwuiPaintUploadData;
		UploadData->Texture2DResource = (FTextureResource*)Texture->GetResource();
		UploadData->PackedPixels.SetNumUninitialized(TotalBytes);
		UploadData->Rects.Reserve(ValidRectCount);

		// Pass 2: fill descriptors and pack pixels into the shared buffer.
		uint8* WriteCursor = UploadData->PackedPixels.GetData();
		for (int32 i = 0; i < RegionCount; ++i)
		{
			if (Regions[i].Width == 0 || Regions[i].Height == 0) continue;

			const int32 RectW      = (int32)Regions[i].Width;
			const int32 RectH      = (int32)Regions[i].Height;
			const int32 TightPitch = RectW * 4;

			FSwuiPackedRectDesc& Desc = UploadData->Rects.AddDefaulted_GetRef();
			Desc.Region.DestX  = Regions[i].DestX;
			Desc.Region.DestY  = Regions[i].DestY;
			Desc.Region.SrcX   = 0;
			Desc.Region.SrcY   = 0;
			Desc.Region.Width  = (uint32)RectW;
			Desc.Region.Height = (uint32)RectH;
			Desc.SrcPitch      = (uint32)TightPitch;
			Desc.SrcOffsetBytes = (int32)(WriteCursor - UploadData->PackedPixels.GetData());

			// Row-by-row copy: source strides FullPitch, dest strides TightPitch.
			const uint8* Src = (const uint8*)Buffer + (int64)Regions[i].SrcY * FullPitch + (int64)Regions[i].SrcX * 4;
			uint8*       Dst = WriteCursor;
			for (int32 Row = 0; Row < RectH; ++Row)
			{
				FPlatformMemory::Memcpy(Dst, Src, TightPitch);
				Src += FullPitch;
				Dst += TightPitch;
			}
			WriteCursor += (int64)TightPitch * RectH;

			UploadedAreaThisPaint += (int64)RectW * RectH;
		}

		const int64 McpyUs = int64((FPlatformTime::Seconds() - McpyT0) * 1e6);
		Stat_MemcpyUs    += McpyUs;
		Stat_MemcpyMaxUs  = FMath::Max(Stat_MemcpyMaxUs, McpyUs);
		Stat_UploadedPixels += UploadedAreaThisPaint;

		if (bVerbose)
		{
			UE_LOG(LogSwuiRuntime, Verbose,
				TEXT("[SwuiPaint][frame] rects=%d valid=%d strategy=per-rect(packed) dirtyPx=%lld uploadPx=%lld ratio=%.2f memcpy=%.3fms"),
				RegionCount, ValidRectCount, DirtyAreaThisPaint, UploadedAreaThisPaint,
				DirtyAreaThisPaint > 0 ? (float)UploadedAreaThisPaint / (float)DirtyAreaThisPaint : 0.f,
				McpyUs / 1000.f);
		}

		FMemory::Free(Regions);

		const bool bSkipUpload = CVarSwuiNoTextureUpload.GetValueOnAnyThread() != 0
			|| InstanceSettings.bSkipTextureUpload
			|| InstanceSettings.bNoTextureUpload;
		if (bSkipUpload) { delete UploadData; }
		else
		{
			ENQUEUE_RENDER_COMMAND(UpdateSwuiViewPerRect)(
				[UploadData](FRHICommandList& CommandList)
				{
					FRHITexture* Tex = UploadData->Texture2DResource->TextureRHI.GetReference();
					const uint8* Base  = UploadData->PackedPixels.GetData();
					for (const FSwuiPackedRectDesc& R : UploadData->Rects)
					{
						RHIUpdateTexture2D(Tex, 0, R.Region, R.SrcPitch, Base + R.SrcOffsetBytes);
					}
					delete UploadData;
				});
		}
	}
	else
	{
		// ---- Band path ----
		// One full-width memcpy of [MinRow..MaxRow], then one RHIUpdateTexture2D per rect.
		// All rects share the same SrcData buffer; SrcY is rebased to MinRow.
		// SrcPitch stays FullPitch — each rect row is read from the correct column via SrcX.

		// Stage gate 3 (band path): skip pixel copy into upload buffers.
		if (InstanceSettings.bSkipPaintMemcpy || InstanceSettings.bFreezeTexture)
		{
			FMemory::Free(Regions);
			return;
		}

		FUpdateTextureRegionsData* RegionData = new FUpdateTextureRegionsData;
		RegionData->Texture2DResource = (FTextureResource*)Texture->GetResource();
		RegionData->NumRegions = (uint32)RegionCount;
		RegionData->SrcBpp     = 4;
		RegionData->SrcPitch   = (uint32)FullPitch;
		RegionData->Regions    = Regions;
		RegionData->SrcData.SetNumUninitialized((int64)FullPitch * BandRows);

		FPlatformMemory::Memcpy(RegionData->SrcData.GetData(),
			(const uint8*)Buffer + (int64)MinRow * FullPitch,
			(int64)FullPitch * BandRows);

		// Rebase SrcY to band-relative offset so the RHI reads from the right row.
		for (int32 i = 0; i < RegionCount; ++i)
			Regions[i].SrcY = (Regions[i].Height > 0) ? (uint32)FMath::Max(0, (int32)Regions[i].SrcY - MinRow) : 0u;

		UploadedAreaThisPaint = BandArea;

		const int64 McpyUs = int64((FPlatformTime::Seconds() - McpyT0) * 1e6);
		Stat_MemcpyUs    += McpyUs;
		Stat_MemcpyMaxUs  = FMath::Max(Stat_MemcpyMaxUs, McpyUs);
		Stat_UploadedPixels += UploadedAreaThisPaint;

		if (bVerbose)
		{
			UE_LOG(LogSwuiRuntime, Verbose,
				TEXT("[SwuiPaint][frame] rects=%d valid=%d strategy=band dirtyPx=%lld uploadPx=%lld ratio=%.2f memcpy=%.3fms"),
				RegionCount, ValidRectCount, DirtyAreaThisPaint, UploadedAreaThisPaint,
				DirtyAreaThisPaint > 0 ? (float)UploadedAreaThisPaint / (float)DirtyAreaThisPaint : 0.f,
				McpyUs / 1000.f);
		}

		const bool bSkipUpload = CVarSwuiNoTextureUpload.GetValueOnAnyThread() != 0
			|| InstanceSettings.bSkipTextureUpload
			|| InstanceSettings.bNoTextureUpload;
		if (bSkipUpload)
		{
			FMemory::Free(RegionData->Regions);
			delete RegionData;
		}
		else
		{
			ENQUEUE_RENDER_COMMAND(UpdateSwuiViewBand)(
				[RegionData](FRHICommandList& CommandList)
				{
					for (uint32 Idx = 0; Idx < RegionData->NumRegions; ++Idx)
					{
						if (RegionData->Regions[Idx].Width == 0 || RegionData->Regions[Idx].Height == 0) continue;
						RHIUpdateTexture2D(RegionData->Texture2DResource->TextureRHI.GetReference(), 0, RegionData->Regions[Idx], RegionData->SrcPitch, RegionData->SrcData.GetData());
					}
					FMemory::Free(RegionData->Regions);
					delete RegionData;
				});
		}
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
