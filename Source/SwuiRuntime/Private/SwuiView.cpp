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

// -----------------------------------------------------------------------
// Tile diff helpers
// -----------------------------------------------------------------------
static constexpr int32 SwuiTileW             = 128;
static constexpr int32 SwuiTileH             = 64;
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

	if (!Texture || !Texture->GetResource()) { FMemory::Free(Regions); return; }

	// Stage gate 1: skip entire pipeline.
	if (InstanceSettings.bSkipOnPaintProcessing) { FMemory::Free(Regions); return; }

	const int32 FullPitch = InWidth * 4;
	const int32 BufBytes  = FullPitch * InHeight;

	FScopeLock Lock(&PaintMutex);

	// Resize backing buffer when texture dimensions change.
	if (BackingBuffer.Num() != BufBytes)
		BackingBuffer.SetNumUninitialized(BufBytes);

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

	// ---- Phase 1: take snapshot from CEF thread state (under lock) ---
	TArray<FIntRect>               LocalRects;
	TArray<FUpdateTextureRegion2D> LocalOverlayRects;
	TArray<uint8>                  FrameBuffer;
	int32 LocalCefPaints = 0, LocalInRects = 0, LocalLargestIn = 0;
	int64 LocalInPx = 0;

	if (bHasPendingUpload && Texture && Texture->GetResource())
	{
		FScopeLock Lock(&PaintMutex);
		if (bHasPendingUpload)
		{
			bHasPendingUpload      = false;
			LocalRects             = MoveTemp(PendingDirtyRects);
			LocalOverlayRects      = MoveTemp(PendingOverlayRects);
			LocalCefPaints         = PendingCefPaints;         PendingCefPaints        = 0;
			LocalInRects           = PendingIncomingRects;     PendingIncomingRects    = 0;
			LocalInPx              = PendingIncomingPx;        PendingIncomingPx       = 0;
			LocalLargestIn         = PendingLargestIncoming;   PendingLargestIncoming  = 0;
			FrameBuffer            = BackingBuffer;             // full copy, ~14 MB @ 2560×1440
		}
	}

	// Accumulate per-second stats (these are game-thread-only, no mutex needed).
	Stat_CefPaints     += LocalCefPaints;
	Stat_IncomingRects += LocalInRects;
	Stat_IncomingPx    += LocalInPx;
	if (LocalLargestIn > Stat_LargestIncoming) Stat_LargestIncoming = LocalLargestIn;

	// ---- Phase 2: optimization pipeline (game thread, no lock held) ----
	if (LocalRects.Num() > 0 && Texture && Texture->GetResource()
		&& !InstanceSettings.bSkipDirtyRectStrategy)
	{
		const int32 SnapW     = LastSnapW = Texture->GetSizeX();
		const int32 SnapH     = LastSnapH = Texture->GetSizeY();
		const int32 FullPitch = SnapW * 4;

		// ---- Tile grid: init or resize when dimensions change ----
		const int32 NTX = FMath::DivideAndRoundUp(SnapW, SwuiTileW);
		const int32 NTY = FMath::DivideAndRoundUp(SnapH, SwuiTileH);
		if (TilesX != NTX || TilesY != NTY)
		{
			TilesX = NTX; TilesY = NTY;
			LastTileHashes.Init(~0u, TilesX * TilesY); // ~0u → all tiles dirty on first pass
		}

		// ---- Build candidate rects: small → expand, large → tile diff ----
		TArray<FIntRect, TInlineAllocator<64>> Candidates;
		for (const FIntRect& Raw : LocalRects)
		{
			const FIntRect R = SwuiClampRect(Raw, SnapW, SnapH);
			if (R.Width() <= 0 || R.Height() <= 0) continue;

			const int64 Area   = (int64)R.Width() * R.Height();
			const bool bLarge  = Area >= SwuiLargeDirtyRectArea
				|| R.Width() >= SnapW || R.Height() >= SnapH;

			if (bLarge)
			{
				// Tile diff: only add tiles whose pixel content changed since last upload.
				const int32 MinTX = R.Min.X / SwuiTileW;
				const int32 MaxTX = (R.Max.X - 1) / SwuiTileW;
				const int32 MinTY = R.Min.Y / SwuiTileH;
				const int32 MaxTY = (R.Max.Y - 1) / SwuiTileH;

				for (int32 Ty = MinTY; Ty <= MaxTY; ++Ty)
				{
					for (int32 Tx = MinTX; Tx <= MaxTX; ++Tx)
					{
						// Tile rect clamped to (texture ∩ dirty rect).
						const FIntRect TileRect(
							FMath::Max(Tx * SwuiTileW,       R.Min.X),
							FMath::Max(Ty * SwuiTileH,       R.Min.Y),
							FMath::Min((Tx + 1) * SwuiTileW, FMath::Min(R.Max.X, SnapW)),
							FMath::Min((Ty + 1) * SwuiTileH, FMath::Min(R.Max.Y, SnapH)));
						if (TileRect.Width() <= 0 || TileRect.Height() <= 0) continue;

						const int32  TIdx    = Ty * TilesX + Tx;
						const uint32 NewHash = SwuiHashTile(FrameBuffer.GetData(), TileRect, FullPitch);
						if (NewHash == LastTileHashes[TIdx])
						{
							++Stat_SkippedTiles;
							continue;
						}
						LastTileHashes[TIdx] = NewHash;
						++Stat_ChangedTiles;
						Candidates.Add(TileRect);
					}
				}
			}
			else
			{
				Candidates.Add(SwuiExpandToMinSize(R, SnapW, SnapH));
			}
		}

		for (const FIntRect& C : Candidates)
		{
			++Stat_CandidateRects;
			Stat_CandidatePx += (int64)C.Width() * C.Height();
		}

		if (Candidates.Num() > 0)
		{
			// ---- Greedy merge (proximity, cheap union only) ----
			TArray<FIntRect, TInlineAllocator<32>> OptRects;
			for (const FIntRect& C : Candidates)
			{
				bool bMerged = false;
				for (FIntRect& E : OptRects)
				{
					if (SwuiShouldMerge(E, C))
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

			// ---- Gate 3: skip memcpy / upload ----
			if (!InstanceSettings.bSkipPaintMemcpy && !InstanceSettings.bFreezeTexture)
			{
				// ---- Tight-pack memcpy from FrameBuffer into upload payload ----
				int32 TotalBytes = 0;
				for (const FIntRect& R2 : OptRects)
					TotalBytes += R2.Width() * 4 * R2.Height();

				FSwuiPaintUploadData* UploadData = new FSwuiPaintUploadData;
				UploadData->Texture2DResource = (FTextureResource*)Texture->GetResource();
				UploadData->PackedPixels.SetNumUninitialized(TotalBytes);
				UploadData->Rects.Reserve(OptRects.Num());

				const double McpyT0    = FPlatformTime::Seconds();
				uint8*       WriteCursor = UploadData->PackedPixels.GetData();
				int64        UploadedPx  = 0;

				for (const FIntRect& R2 : OptRects)
				{
					const int32 RectW      = R2.Width();
					const int32 RectH      = R2.Height();
					const int32 TightPitch = RectW * 4;

					FSwuiPackedRectDesc& Desc  = UploadData->Rects.AddDefaulted_GetRef();
					Desc.Region.DestX          = (uint32)R2.Min.X;
					Desc.Region.DestY          = (uint32)R2.Min.Y;
					Desc.Region.SrcX           = 0;
					Desc.Region.SrcY           = 0;
					Desc.Region.Width          = (uint32)RectW;
					Desc.Region.Height         = (uint32)RectH;
					Desc.SrcPitch              = (uint32)TightPitch;
					Desc.SrcOffsetBytes        = (int32)(WriteCursor - UploadData->PackedPixels.GetData());

					const uint8* Src = FrameBuffer.GetData() + (int64)R2.Min.Y * FullPitch + (int64)R2.Min.X * 4;
					uint8*       Dst = WriteCursor;
					for (int32 Row = 0; Row < RectH; ++Row, Src += FullPitch, Dst += TightPitch)
						FPlatformMemory::Memcpy(Dst, Src, TightPitch);

					WriteCursor  += (int64)TightPitch * RectH;
					UploadedPx   += (int64)RectW * RectH;
					++Stat_UploadedRects;
					const int32 RectArea = RectW * RectH;
					if (RectArea > Stat_LargestUploaded) Stat_LargestUploaded = RectArea;
				}

				const int64 McpyUs = int64((FPlatformTime::Seconds() - McpyT0) * 1e6);
				Stat_MemcpyUs       += McpyUs;
				Stat_MemcpyMaxUs     = FMath::Max(Stat_MemcpyMaxUs, McpyUs);
				Stat_UploadedPixels += UploadedPx;
				++Stat_UeUploads;

				if (CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0)
				{
					UE_LOG(LogSwuiRuntime, Verbose,
						TEXT("[SwuiPaint][tick] cefPaints=%d inRects=%d inPx=%lld cand=%d opt=%d uploadPx=%lld skippedTiles=%d changedTiles=%d memcpy=%.3fms"),
						LocalCefPaints, LocalRects.Num(), LocalInPx,
						Candidates.Num(), OptRects.Num(), UploadedPx,
						Stat_SkippedTiles, Stat_ChangedTiles, McpyUs / 1000.f);
				}

				const bool bSkipUpload = CVarSwuiNoTextureUpload.GetValueOnAnyThread() != 0
					|| InstanceSettings.bSkipTextureUpload || InstanceSettings.bNoTextureUpload;
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
			}
		}
	}

	// -----------------------------------------------------------------------
	// One-second aggregate stats log (runs every frame, fires once per second)
	// -----------------------------------------------------------------------
	if (Now - Stat_LastLogTime >= 1.0)
	{
		const float McAvgMs = Stat_UeUploads > 0 ? float(Stat_MemcpyUs) / Stat_UeUploads / 1000.f : 0.f;
		const float McMaxMs = float(Stat_MemcpyMaxUs) / 1000.f;
		UE_LOG(LogSwuiRuntime, Log,
			TEXT("[SwuiPaint] cefPaints/s=%d  ueUploads/s=%d  inRects/s=%d  inPx/s=%lld  largestIncoming=%d")
			TEXT("  candRects/s=%d  uploadedRects/s=%d  uploadedPx/s=%lld  largestUploaded=%d")
			TEXT("  skippedTiles/s=%d  changedTiles/s=%d  memcpyAvgMs=%.3f  memcpyMaxMs=%.3f  tex=%dx%d"),
			Stat_CefPaints, Stat_UeUploads,
			Stat_IncomingRects, Stat_IncomingPx, Stat_LargestIncoming,
			Stat_CandidateRects, Stat_UploadedRects, Stat_UploadedPixels, Stat_LargestUploaded,
			Stat_SkippedTiles, Stat_ChangedTiles,
			McAvgMs, McMaxMs, LastSnapW, LastSnapH);

		Stat_CefPaints       = 0; Stat_UeUploads      = 0;
		Stat_IncomingRects   = 0; Stat_IncomingPx     = 0; Stat_LargestIncoming = 0;
		Stat_CandidateRects  = 0; Stat_CandidatePx    = 0;
		Stat_UploadedRects   = 0; Stat_UploadedPixels = 0; Stat_LargestUploaded = 0;
		Stat_SkippedTiles    = 0; Stat_ChangedTiles   = 0;
		Stat_MemcpyUs        = 0; Stat_MemcpyMaxUs    = 0;
		Stat_LastLogTime     = Now;
	}

	// -----------------------------------------------------------------------
	// Dirty-rect overlay push (~10 Hz, only when bShowDirtyRectOverlay is set)
	// NOTE: The overlay is rendered inside the same CEF surface so it generates
	// its own dirty rects. Disable overlay for final perf measurements.
	// -----------------------------------------------------------------------
	if (InstanceSettings.bShowDirtyRectOverlay && (Now - Stat_OverlayLastPushTime >= 0.1)
		&& LocalOverlayRects.Num() > 0)
	{
		Stat_OverlayLastPushTime = Now;

		const float Elapsed      = FMath::Clamp(float(Now - Stat_LastLogTime) + 1.f, 0.001f, 2.f);
		const int64 DirtyPxRate  = (int64)(Stat_IncomingPx    / Elapsed);
		const int64 UploadPxRate = (int64)(Stat_UploadedPixels / Elapsed);
		const float MemcpyAvgMs  = Stat_UeUploads > 0 ? float(Stat_MemcpyUs) / Stat_UeUploads / 1000.f : 0.f;
		const float MemcpyMaxMs  = float(Stat_MemcpyMaxUs) / 1000.f;
		const float BandRatio    = Stat_IncomingPx > 0 ? float(Stat_UploadedPixels) / float(Stat_IncomingPx) : 0.f;

		FString RectsJson;
		RectsJson.Reserve(LocalOverlayRects.Num() * 52 + 2);
		RectsJson.AppendChar('[');
		for (int32 i = 0; i < LocalOverlayRects.Num(); ++i)
		{
			const FUpdateTextureRegion2D& R = LocalOverlayRects[i];
			if (i > 0) RectsJson.AppendChar(',');
			RectsJson += FString::Printf(
				TEXT("{\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u,\"area\":%lld}"),
				R.DestX, R.DestY, R.Width, R.Height, (int64)R.Width * R.Height);
		}
		RectsJson.AppendChar(']');

		const FString Script = FString::Printf(
			TEXT("if(window.__SWUI_DEBUG_RECTS__)window.__SWUI_DEBUG_RECTS__(")
			TEXT("{\"texW\":%d,\"texH\":%d,\"rects\":%s,")
			TEXT("\"stats\":{\"largestRect\":%d,\"largestUploaded\":%d,")
			TEXT("\"dirtyPxS\":%lld,\"uploadedPxS\":%lld,")
			TEXT("\"skippedTiles\":%d,\"changedTiles\":%d,")
			TEXT("\"memcpyAvgMs\":%.3f,\"memcpyMaxMs\":%.3f,\"bandRatio\":%.2f}});"),
			LastSnapW, LastSnapH, *RectsJson,
			Stat_LargestIncoming, Stat_LargestUploaded,
			DirtyPxRate, UploadPxRate,
			Stat_SkippedTiles, Stat_ChangedTiles,
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
