#include "SwuiView.h"
#include "RenderHandler.h"
#include "ISwuiRuntime.h"
#include "SwuiNavigation.h"

// ---------------------------------------------------------------------------
// SWUI JS→UE Native Message Bus (Navigation Only)
//
// Receives JSON messages from JS/React HUD via CEF/cefQuery:
//   { "type": "navigation", "tag": "swui.menu.close", "payload": {}, "source": "js" }
// Only routes type="navigation" for now. Tag is mapped to FGameplayTag.
// Blueprint API and React HUD usage remain unchanged.
// ---------------------------------------------------------------------------
#include "SwuiManager.h"
#include "SwuiCVars.h"
#include "SwuiCVarHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SViewport.h"
#include "InputCoreTypes.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagsManager.h"
#include "Misc/Crc.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <d3d11.h>
#include <dxgi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

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
	const int32 HudLockstepOverride = CVarSwuiHudLockstep.GetValueOnGameThread();
	const int32 HudExternalBeginFrameOverride = CVarSwuiHudExternalBeginFrames.GetValueOnGameThread();
	const int32 HudMaxBrowserFpsOverride = CVarSwuiHudMaxBrowserFPS.GetValueOnGameThread();
	const bool bHudLockstepEnabled = SwuiCVarBool(HudLockstepOverride, InstanceSettings.bUseUEFrameLockedBrowser);
	const bool bExternalBeginFramesEnabled = SwuiCVarBool(HudExternalBeginFrameOverride, InstanceSettings.bUseExternalBeginFrames);
	const int32 InitHudMaxBrowserFps = SwuiCVarInt(HudMaxBrowserFpsOverride, InstanceSettings.MaxBrowserFramesPerSecond);
	const bool bWantsExternalBeginFrames =
		InstanceSettings.bIsHUD &&
		bHudLockstepEnabled &&
		bExternalBeginFramesEnabled;
	Info.external_begin_frame_enabled = bWantsExternalBeginFrames ? 1 : 0;

	// -----------------------------------------------------------------------
	// Resolve rendering mode (Auto → concrete backend).
	//
	// Thread: Game thread.  This runs once during Init() before the browser
	// is created, so no synchronisation is needed with CEF callbacks.
	// -----------------------------------------------------------------------
	{
		const ESwuiRenderingMode RequestedMode = InstanceSettings.RenderingMode;
		if (RequestedMode == ESwuiRenderingMode::GpuAccelerated)
		{
			if (CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0 || InstanceSettings.bVerbosePaintLog)
			{
				UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI RENDER] GPU Accelerated requested by setting"));
			}

			// GPU Accelerated mode requires D3D11 RHI. CEF's shared_texture_enabled
			// always produces D3D11-native shared handles, and OnAcceleratedPaint
			// opens them via ID3D11Device::OpenSharedResource.  Non-D3D11 RHIs
			// (e.g. D3D12, Vulkan) cannot open these handles and will crash.
			const bool bIsD3D11 = GDynamicRHI && FString(GDynamicRHI->GetName()).Contains(TEXT("D3D11"));
			if (bIsD3D11)
			{
				ResolvedRenderingMode = ESwuiRenderingMode::GpuAccelerated;
			}
			else
			{
				const FString RHIName = GDynamicRHI ? GDynamicRHI->GetName() : TEXT("null");
				UE_LOG(LogSwuiRuntime, Error,
					TEXT("[SWUI RENDER] GPU Accelerated requires D3D11 RHI, but active RHI is '%s'. ")
					TEXT("CEF shared texture handles are D3D11-only. Falling back to CPU Compatible."),
					*RHIName);
				ResolvedRenderingMode = ESwuiRenderingMode::CpuCompatible;
			}
		}
		else if (RequestedMode == ESwuiRenderingMode::Auto)
		{
			if (IsGpuAcceleratedSupported())
			{
				ResolvedRenderingMode = ESwuiRenderingMode::GpuAccelerated;
			}
			else
			{
				ResolvedRenderingMode = ESwuiRenderingMode::CpuCompatible;
				if (CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0 || InstanceSettings.bVerbosePaintLog)
				{
					UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI RENDER] Auto: GPU Accelerated unavailable — using CPU Compatible."));
				}
			}
		}
		else
		{
			ResolvedRenderingMode = ESwuiRenderingMode::CpuCompatible;
		}

		// Enable CEF shared texture support for the GPU Accelerated path.
		if (ResolvedRenderingMode == ESwuiRenderingMode::GpuAccelerated)
		{
			Info.shared_texture_enabled = 1;
		}

		if (CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0 || InstanceSettings.bVerbosePaintLog)
	{
		const TCHAR* ModeStr =
			ResolvedRenderingMode == ESwuiRenderingMode::GpuAccelerated ? TEXT("GPU Accelerated") : TEXT("CPU Compatible");
		UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI RENDER] Resolved mode: %s (requested=%d)"),
			ModeStr, (int32)RequestedMode);
	}
	}

	CefBrowserSettings BrowserSettings;
	BrowserSettings.webgl = STATE_ENABLED;

	// Create render handler with the appropriate backend target pointers.
	// CPU Compatible: RenderTarget = this (ISwuiRenderTarget), AcceleratedTarget = null.
	// GPU Accelerated: RenderTarget = null, AcceleratedTarget = this (ISwuiAcceleratedRenderTarget).
	// CEF calls OnPaint or OnAcceleratedPaint exclusively depending on shared_texture_enabled.
	ISwuiRenderTarget* CpuTarget = (ResolvedRenderingMode == ESwuiRenderingMode::CpuCompatible) ? static_cast<ISwuiRenderTarget*>(this) : nullptr;
	ISwuiAcceleratedRenderTarget* GpuTarget = (ResolvedRenderingMode == ESwuiRenderingMode::GpuAccelerated) ? static_cast<ISwuiAcceleratedRenderTarget*>(this) : nullptr;
	RenderHandler* Renderer = new RenderHandler(Width, Height, CpuTarget, GpuTarget, ResolvedRenderingMode);
	CefRefPtr<BrowserClient> Client = new BrowserClient(Renderer, this);

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
	if (InstanceSettings.bIsHUD && bHudLockstepEnabled)
	{
		TargetFPS = InitHudMaxBrowserFps > 0 ? InitHudMaxBrowserFps : 60;
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
	if (IConsoleVariable* VerbosePaintVar = CVarSwuiVerbosePaint.operator->())
	{
		VerbosePaintVar->Set(bWantVerbose ? 1 : 0, ECVF_SetByCode);
	}
	if (IConsoleVariable* NoTextureUploadVar = CVarSwuiNoTextureUpload.operator->())
	{
		NoTextureUploadVar->Set(bWantNoUpload ? 1 : 0, ECVF_SetByCode);
	}

	CefRefPtr<CefBrowserHost> Host = Browser->GetHost();
	if (!Host)
	{
		UE_LOG(LogSwuiRuntime, Error, TEXT("USwuiView::Init: Browser created but BrowserHost is null."));
		return;
	}

	Host->SetWindowlessFrameRate(TargetFPS);
	WindowlessFrameRate = TargetFPS;
	AppliedHudLockstepCVar = HudLockstepOverride;
	AppliedHudExternalBeginFramesCVar = HudExternalBeginFrameOverride;
	AppliedHudMaxBrowserFPSCVar = HudMaxBrowserFpsOverride;
	LastObservedHudLockstepCVar = HudLockstepOverride;
	LastObservedHudExternalBeginFramesCVar = HudExternalBeginFrameOverride;
	LastObservedHudMaxBrowserFPSCVar = HudMaxBrowserFpsOverride;
	const bool bHostIsWindowless = Host->IsWindowRenderingDisabled();
	bExternalBeginFrameActive = bWantsExternalBeginFrames && bHostIsWindowless;
	ExternalBeginFrameAccumulatedTime = 0.0;
	bPaintArrivedAfterExternalBeginFrame = false;
	bPendingInvalidateForPaint = false;
	if (bWantsExternalBeginFrames)
	{
		if (CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0 || InstanceSettings.bVerbosePaintLog)
		{
			UE_LOG(LogSwuiRuntime, Log, TEXT("USwuiView: External begin frame requested=%s, osrWindowless=%s, active=%s"),
				bWantsExternalBeginFrames ? TEXT("true") : TEXT("false"),
				bHostIsWindowless ? TEXT("true") : TEXT("false"),
				bExternalBeginFrameActive ? TEXT("true") : TEXT("false"));
		}
	}
	else if (InstanceSettings.bIsHUD && bHudLockstepEnabled && bExternalBeginFramesEnabled)
	{
		UE_LOG(LogSwuiRuntime, Warning, TEXT("USwuiView: External begin frame mode unavailable; using capped windowless frame pacing fallback."));
	}

	CefData->Client = Client;
	CefData->Browser = Browser;

	if (CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0 || InstanceSettings.bVerbosePaintLog)
	{
		UE_LOG(LogSwuiRuntime, Log, TEXT("USwuiView Initialized"));
	}

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

AActor* USwuiView::ResolveOwningActor() const
{
	if (OwningActor.IsValid())
	{
		return OwningActor.Get();
	}

	if (const UActorComponent* OwnerComponent = GetTypedOuter<UActorComponent>())
	{
		return OwnerComponent->GetOwner();
	}

	return GetTypedOuter<AActor>();
}

bool USwuiView::HandleIncomingMessage(const FString& MessageJson)
{
       UE_LOG(LogSwuiRuntime, Verbose, TEXT("[SWUI JS BUS] raw=%s"), *MessageJson);

       TSharedPtr<FJsonObject> MessageObject;
       const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MessageJson);

       if (!FJsonSerializer::Deserialize(Reader, MessageObject) || !MessageObject.IsValid())
       {
	       UE_LOG(LogSwuiRuntime, Warning, TEXT("[SWUI JS BUS] Failed to parse JSON: %s"), *MessageJson);
	       return false;
       }

       FString MessageType;
       if (!MessageObject->TryGetStringField(TEXT("type"), MessageType))
       {
	       UE_LOG(LogSwuiRuntime, Warning, TEXT("[SWUI JS BUS] Missing 'type' field."));
	       return false;
       }

       UE_LOG(LogSwuiRuntime, Verbose, TEXT("[SWUI JS BUS] type=%s"), *MessageType);

       if (MessageType != TEXT("navigation"))
       {
	       UE_LOG(LogSwuiRuntime, Warning, TEXT("[SWUI JS BUS] Unsupported message type '%s'."), *MessageType);
	       return false;
       }

       FString TagName;
       if (!MessageObject->TryGetStringField(TEXT("tag"), TagName) || TagName.IsEmpty())
       {
	       UE_LOG(LogSwuiRuntime, Warning, TEXT("[SWUI JS->UE NAV] Navigation message missing 'tag' field."));
	       return false;
       }

       const FGameplayTag EventTag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*TagName), false);
       if (!EventTag.IsValid())
       {
	       UE_LOG(LogSwuiRuntime, Warning, TEXT("[SWUI JS->UE NAV] Ignoring unknown navigation tag '%s'."), *TagName);
	       return false;
       }

       FString PayloadJson = TEXT("{}");
       if (const TSharedPtr<FJsonValue>* PayloadValue = MessageObject->Values.Find(TEXT("payload")))
       {
	       PayloadJson.Reset();
	       const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		       TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PayloadJson);
	       FJsonSerializer::Serialize((*PayloadValue).ToSharedRef(), FString(), Writer);
       }

       UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI JS->UE NAV] tag=%s payload=%s"), *TagName, *PayloadJson);

       AActor* OwnerActor = ResolveOwningActor();
       if (!OwnerActor)
       {
	       UE_LOG(LogSwuiRuntime, Warning, TEXT("[SWUI JS->UE NAV] Message received without an owning actor."));
	       return false;
       }

       USwuiNavigation* Navigation = OwnerActor->FindComponentByClass<USwuiNavigation>();
       if (!Navigation)
       {
	       UE_LOG(LogSwuiRuntime, Warning, TEXT("[SWUI JS->UE NAV] Actor '%s' has no USwuiNavigation component to handle '%s'."), *OwnerActor->GetName(), *TagName);
	       return false;
       }

       Navigation->ReceiveNavigationEventFromJs(EventTag, PayloadJson);
       return true;
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
	// GPU Accelerated path: check accelerated paint pending flag.
	if (ResolvedRenderingMode == ESwuiRenderingMode::GpuAccelerated)
	{
		FScopeLock Lock(const_cast<FCriticalSection*>(&AccelPaintMutex));
		return bHasPendingAccelPaint && AccelPaintGeneration > AccelDrainedGeneration;
	}

	// CPU Compatible path: check CPU paint pending flag.
	FScopeLock Lock(const_cast<FCriticalSection*>(&PaintMutex));
	return
		bHasPendingFullSurfacePaint ||
		(bHasPendingUpload &&
		PendingIncomingRects > 0 &&
		PendingFreshPaintGeneration > DrainedFreshPaintGeneration);
}

void USwuiView::BeginFullTransitionRefresh(int32 FreshPaintCount)
{
	if (IsForceFullFrameMode())
	{
		if (CefData && CefData->Browser)
		{
			if (CefRefPtr<CefBrowserHost> Host = CefData->Browser->GetHost())
			{
				Host->Invalidate(PET_VIEW);
			}
		}
		return;
	}

	if (CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0 || InstanceSettings.bVerbosePaintLog)
	{
		UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI TRANSITION] BeginFullTransitionRefresh  freshCount=%d  mode=FullTransition"), FreshPaintCount);
	}

	RenderActivityMode = ESwuiRenderActivityMode::FullTransition;
	PendingFullCefPaintCopies = FreshPaintCount;
	bNeedsFullBaselineUpload = true;
	bAwaitingFreshPaintForForcedUpload = true;
	ForcedUploadRequestedAtPaintGeneration = PaintGeneration;
	PendingFreshFullUploads = FreshPaintCount;
	SuppressCenterCriticalRectFrames = FreshPaintCount + 2;
	if (ActiveDirtyTileMask.Num() > 0)
	{
		ActiveDirtyTileMask.Init(false, ActiveDirtyTileMask.Num());
		DirtyTileScanCursor = 0;
	}

	if (CefData && CefData->Browser)
	{
		CefRefPtr<CefBrowserHost> Host = CefData->Browser->GetHost();
		if (Host)
		{
			Host->Invalidate(PET_VIEW);
		}
	}
}

void USwuiView::SetUiInteractionActive(bool bActive)
{
	bUiInteractionActive = bActive;
	if (!bActive && RenderActivityMode == ESwuiRenderActivityMode::InteractiveUi)
	{
		RenderActivityMode = ESwuiRenderActivityMode::NormalHud;
	}
	else if (bActive)
	{
		// If a FullTransition is in progress, let it finish before switching
		if (RenderActivityMode != ESwuiRenderActivityMode::FullTransition)
		{
			RenderActivityMode = ESwuiRenderActivityMode::InteractiveUi;
		}
	}
}

void USwuiView::RequestFullTextureUploadNextFrame()
{
	if (IsForceFullFrameMode())
	{
		if (CefData && CefData->Browser)
		{
			if (CefRefPtr<CefBrowserHost> Host = CefData->Browser->GetHost())
			{
				Host->Invalidate(PET_VIEW);
			}
		}
		return;
	}

	bNeedsFullBaselineUpload = true;
	bAwaitingFreshPaintForForcedUpload = true;
	ForcedUploadRequestedAtPaintGeneration = PaintGeneration;
	PendingFreshFullUploads = 3;
	SuppressCenterCriticalRectFrames = 4;
	if (ActiveDirtyTileMask.Num() > 0)
	{
		ActiveDirtyTileMask.Init(false, ActiveDirtyTileMask.Num());
		DirtyTileScanCursor = 0;
	}
}

bool USwuiView::FlushHudStateAndRequestBrowserFrame(const FString& CombinedScript, float DeltaTime, bool bForceFrame)
{
	const int32 CurrentHudLockstepCVar = CVarSwuiHudLockstep.GetValueOnGameThread();
	const int32 CurrentHudExternalBeginFramesCVar = CVarSwuiHudExternalBeginFrames.GetValueOnGameThread();
	const int32 CurrentHudMaxBrowserFPSCVar = CVarSwuiHudMaxBrowserFPS.GetValueOnGameThread();
	if (CurrentHudLockstepCVar != LastObservedHudLockstepCVar)
	{
		LastObservedHudLockstepCVar = CurrentHudLockstepCVar;
		if (CurrentHudLockstepCVar != AppliedHudLockstepCVar)
		{
			UE_LOG(LogSwuiRuntime, Log, TEXT("USwuiView: swui.hud.Lockstep changed at runtime; browser creation behavior updates on next view recreate."));
		}
	}
	if (CurrentHudExternalBeginFramesCVar != LastObservedHudExternalBeginFramesCVar)
	{
		LastObservedHudExternalBeginFramesCVar = CurrentHudExternalBeginFramesCVar;
		if (CurrentHudExternalBeginFramesCVar != AppliedHudExternalBeginFramesCVar)
		{
			UE_LOG(LogSwuiRuntime, Log, TEXT("USwuiView: swui.hud.ExternalBeginFrames changed at runtime; browser creation behavior updates on next view recreate."));
		}
	}
	if (CurrentHudMaxBrowserFPSCVar != LastObservedHudMaxBrowserFPSCVar)
	{
		LastObservedHudMaxBrowserFPSCVar = CurrentHudMaxBrowserFPSCVar;
		if (CurrentHudMaxBrowserFPSCVar != AppliedHudMaxBrowserFPSCVar)
		{
			UE_LOG(LogSwuiRuntime, Log, TEXT("USwuiView: swui.hud.MaxBrowserFPS changed at runtime; windowless host cap updates on next view recreate (live begin-frame pacing uses the new value immediately)."));
		}
	}

	const bool bSendExternalBeginFrameFromTick = SwuiCVarBool(
		CVarSwuiHudSendExternalBeginFrameFromTick.GetValueOnGameThread(),
		InstanceSettings.bSendExternalBeginFrameFromTick);
	const bool bHasScript = !CombinedScript.IsEmpty();
	if (!CefData || !CefData->Browser)
	{
		if (bExternalBeginFrameActive && bSendExternalBeginFrameFromTick)
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
	if (!bSendExternalBeginFrameFromTick)
	{
		++Stat_ExternalBeginFrameSkipDisabled;
		if (bHasScript)
		{
			ExecuteJavaScript(CombinedScript);
		}
		return false;
	}

	const int32 BrowserFpsSetting = SwuiCVarInt(
		CVarSwuiHudMaxBrowserFPS.GetValueOnGameThread(),
		InstanceSettings.MaxBrowserFramesPerSecond);
	const int32 TargetHz = FMath::Clamp(
		BrowserFpsSetting > 0 ? BrowserFpsSetting : WindowlessFrameRate,
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

	if (InstanceSettings.bSkipOnPaintProcessing) { FMemory::Free(Regions); return; }

	const double PaintNow = FPlatformTime::Seconds();

	// Force-full-frame mode: stage full surface and skip all dirty/transition logic.
	if (IsForceFullFrameMode())
	{
		StageFullSurfacePaint(Buffer, InWidth, InHeight, PaintNow);
		FMemory::Free(Regions);
		return;
	}

	const int32 FullPitch = InWidth * 4;
	const int32 BufBytes  = FullPitch * InHeight;
	const bool bForceFullBaselineUploadOnFirstPaint = SwuiCVarBool(
		CVarSwuiPaintFullBaseline.GetValueOnAnyThread(),
		InstanceSettings.bForceFullBaselineUploadOnFirstPaint);
	const bool bShowDirtyRects = SwuiCVarBool(
		CVarSwuiDebugShowDirtyRects.GetValueOnAnyThread(),
		InstanceSettings.bShowDirtyRectOverlay || InstanceSettings.bShowSwuiDirtyRects);
	const bool bVerboseLog =
		CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0 ||
		InstanceSettings.bVerbosePaintLog;

	FScopeLock Lock(&PaintMutex);
	LastPaintArrivalTime = PaintNow;

	++PendingFreshPaintGeneration;
	PendingFreshPaintArrivalTime = PaintNow;

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

	if (BackingBuffer.Num() != BufBytes)
	{
		BackingBuffer.SetNumUninitialized(BufBytes);
		bNeedsFullBaselineUpload = true;
	}

	if (!bSeenFirstPaint)
	{
		bSeenFirstPaint = true;
		if (bForceFullBaselineUploadOnFirstPaint)
			bNeedsFullBaselineUpload = true;
	}

	// Automatic large-paint detection: if CEF produces a large or high-rect
	// paint that looks like a fullscreen UI transition, enter FullTransition.
	// Suppressed entirely when force-full-frame is active (caught above).
	{
		int64 TotalDirtyArea = 0;
		int32 MaxRectArea = 0;
		for (int32 i = 0; i < RegionCount; ++i)
		{
			const FUpdateTextureRegion2D& Rg = Regions[i];
			const int64 Area = (int64)Rg.Width * Rg.Height;
			TotalDirtyArea += Area;
			if ((int32)Area > MaxRectArea) MaxRectArea = (int32)Area;
		}
		const int64 SurfaceArea = (int64)InWidth * InHeight;
		const float DirtyRatio = SurfaceArea > 0 ? (float)TotalDirtyArea / (float)SurfaceArea : 0.f;
		const bool bLargeTransition =
			DirtyRatio >= 0.35f ||
			RegionCount >= 32 ||
			(float)MaxRectArea >= (float)SurfaceArea * 0.50f;
		static constexpr double AutoFullTransitionCooldown = 0.5;
		bool bCooldownBlocked = false;
		if (bLargeTransition
			&& !bAutoTransitionEverFired
			&& RenderActivityMode != ESwuiRenderActivityMode::FullTransition
			&& PendingFullCefPaintCopies == 0
			&& !bAwaitingFreshPaintForForcedUpload
			&& !bUiInteractionActive)
		{
			if ((PaintNow - LastAutoFullTransitionTime) >= AutoFullTransitionCooldown)
			{
				LastAutoFullTransitionTime = PaintNow;
				if (bVerboseLog)
				{
					UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI PAINT] Auto FullTransition  dirtyRatio=%.3f  rects=%d  maxRectArea=%d"),
						DirtyRatio, RegionCount, MaxRectArea);
				}
				BeginFullTransitionRefresh(3);
				bAutoTransitionEverFired = true;
			}
			else
			{
				bCooldownBlocked = true;
			}
		}
		if (bCooldownBlocked)
		{
			// UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI PAINT] Auto FullTransition suppressed by cooldown"));
		}
	}

	// CEF paint: full buffer copy (FullTransition) or dirty rect copy.
	if (PendingFullCefPaintCopies > 0)
	{
		if (BackingBuffer.Num() == BufBytes)
		{
			FPlatformMemory::Memcpy(BackingBuffer.GetData(), Buffer, BufBytes);
		}
		--PendingFullCefPaintCopies;
		++PendingIncomingRects;
		PendingIncomingPx += BufBytes / 4;
		if (BufBytes / 4 > PendingLargestIncoming) PendingLargestIncoming = BufBytes / 4;
		PendingDirtyRects.Add(FIntRect(0, 0, InWidth, InHeight));
	}
	else
	{
		for (int32 i = 0; i < RegionCount; ++i)
		{
			FUpdateTextureRegion2D& Rg = Regions[i];

			Rg.SrcX  = Rg.DestX = (uint32)FMath::Clamp((int32)Rg.SrcX,  0, InWidth  - 1);
			Rg.SrcY  = Rg.DestY = (uint32)FMath::Clamp((int32)Rg.SrcY,  0, InHeight - 1);
			Rg.Width  = (uint32)FMath::Clamp((int32)Rg.Width,  0, InWidth  - (int32)Rg.SrcX);
			Rg.Height = (uint32)FMath::Clamp((int32)Rg.Height, 0, InHeight - (int32)Rg.SrcY);

			if (Rg.Width == 0 || Rg.Height == 0) continue;

			const int32  RowBytes = (int32)Rg.Width * 4;
			const uint8* Src = (const uint8*)Buffer    + (int64)Rg.SrcY * FullPitch + (int64)Rg.SrcX * 4;
			uint8*       Dst = BackingBuffer.GetData() + (int64)Rg.SrcY * FullPitch + (int64)Rg.SrcX * 4;

			if (Rg.SrcX == 0 && Rg.Width == (uint32)InWidth)
			{
				const int64 CopyBytes = (int64)RowBytes * Rg.Height;
				FPlatformMemory::Memcpy(Dst, Src, CopyBytes);
			}
			else
			{
				for (uint32 Row = 0; Row < Rg.Height; ++Row, Src += FullPitch, Dst += FullPitch)
					FPlatformMemory::Memcpy(Dst, Src, RowBytes);
			}

			const int64 Area = (int64)Rg.Width * Rg.Height;
			++PendingIncomingRects;
			PendingIncomingPx += Area;
			if ((int32)Area > PendingLargestIncoming) PendingLargestIncoming = (int32)Area;
			PendingDirtyRects.Add(FIntRect((int32)Rg.SrcX, (int32)Rg.SrcY,
				(int32)Rg.SrcX + (int32)Rg.Width, (int32)Rg.SrcY + (int32)Rg.Height));
			if (bShowDirtyRects)
				PendingOverlayRects.Add(Rg);
		}
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

	if (ResolvedRenderingMode == ESwuiRenderingMode::GpuAccelerated)
	{
		TickAcceleratedUpload();

		int32 TargetFPS = WindowlessFrameRate > 0 ? WindowlessFrameRate : 60;
		double MinFrameInterval = 1.0 / double(TargetFPS);
		bool bShouldSendFrame = false;
		if ((Now - LastBrowserFrameTime) >= MinFrameInterval)
		{
			bShouldSendFrame = true;
		}
		if ((Now - LastBrowserFrameTime) > BrowserFrameTimeout)
		{
			bShouldSendFrame = true;
		}
		if (bShouldSendFrame)
		{
			SendExternalBeginFrameIfDue(float(Now - LastBrowserFrameTime));
			LastBrowserFrameTime = Now;
		}

		if (SwuiCVarBool(CVarSwuiDebugLogPaintStats.GetValueOnGameThread(), InstanceSettings.bLogSwuiPaintStats) && (Now - Stat_LastLogTime >= 1.0))
		{
			const float AccelCopyAvgMs = Stat_AccelCopySamples > 0 ? float(Stat_AccelCopyMsSum / Stat_AccelCopySamples) : 0.f;
			const TCHAR* ModeStr = TEXT("GPU Accelerated");
			UE_LOG(LogSwuiRuntime, Log,
				TEXT("[SwuiPaint] mode=%s  subsystemTicks/s=%d  viewUploadTicks/s=%d")
				TEXT("  accelPaints/s=%d  accelCopies/s=%d  accelHandleFails/s=%d")
				TEXT("  accelCopyAvgMs=%.3f  accelCopyMaxMs=%.3f")
				TEXT("  texRecreates=%d  resizes=%d")
				TEXT("  externalBeginFrames/s=%d")
				TEXT("  browserFpsCap=%d  tex=%dx%d"),
				ModeStr, Stat_SubsystemTicks, Stat_ViewUploadTicks,
				Stat_AccelPaints, Stat_AccelCopies, Stat_AccelHandleFails,
				AccelCopyAvgMs, Stat_AccelCopyMsMax,
				Stat_AccelTexRecreates, Stat_AccelResizes,
				Stat_ExternalBeginFrames,
				WindowlessFrameRate, Width, Height);
			if (!GpuFallbackReason.IsEmpty())
			{
				UE_LOG(LogSwuiRuntime, Log, TEXT("[SwuiPaint] fallbackReason=%s"), *GpuFallbackReason);
			}
			Stat_SubsystemTicks = 0; Stat_ViewUploadTicks = 0;
			Stat_AccelPaints = 0; Stat_AccelCopies = 0; Stat_AccelHandleFails = 0;
			Stat_AccelCopyMsSum = 0.0; Stat_AccelCopyMsMax = 0.0; Stat_AccelCopySamples = 0;
			Stat_AccelTexRecreates = 0; Stat_AccelResizes = 0;
			Stat_ExternalBeginFrames = 0;
			Stat_ExternalBeginFrameSkipInactive = 0; Stat_ExternalBeginFrameSkipDisabled = 0;
			Stat_ExternalBeginFrameSkipNoBrowser = 0; Stat_ExternalBeginFrameSkipRateLimited = 0;
			Stat_LastLogTime = Now;
		}
		return;
	}

	if (IsForceFullFrameMode())
	{
		TickForceFullSurfaceUpload(Now);
		return;
	}

	TickDirtyOptimizedUpload(Now);
}

// ---------------------------------------------------------------------------
// TickDirtyOptimizedUpload — normal dirty-rect / tile-diff / center-critical
// optimized upload path. Only used when force-full-frame mode is OFF.
// ---------------------------------------------------------------------------
void USwuiView::TickDirtyOptimizedUpload(double Now)
{
	auto RecordTimingSampleMs = [](double SampleMs, double& SumMs, double& MaxMs, int32& SampleCount)
	{
		SumMs += SampleMs;
		if (SampleMs > MaxMs) MaxMs = SampleMs;
		++SampleCount;
	};

	TArray<FIntRect>               LocalRects;
	TArray<FUpdateTextureRegion2D> LocalOverlayRects;
	TArray<FUpdateTextureRegion2D> OptimizedOverlayRects;
	int32 LocalCefPaints = 0, LocalInRects = 0, LocalLargestIn = 0;
	int64 LocalInPx = 0;
	uint64 LocalFreshPaintGeneration = 0;
	double LocalFreshPaintArrivalTime = 0.0;
	bool bDrainedFreshPaintThisTick = false;

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

			LocalCefPaints         = PendingCefPaints;
			LocalInRects           = PendingIncomingRects;
			LocalInPx              = PendingIncomingPx;
			LocalLargestIn         = PendingLargestIncoming;

			LocalFreshPaintGeneration = PendingFreshPaintGeneration;
			LocalFreshPaintArrivalTime = PendingFreshPaintArrivalTime;

			bDrainedFreshPaintThisTick =
				LocalInRects > 0 &&
				LocalFreshPaintGeneration > DrainedFreshPaintGeneration;

			if (bDrainedFreshPaintThisTick)
			{
				DrainedFreshPaintGeneration = LocalFreshPaintGeneration;
				++PaintGeneration;
			}

			PendingCefPaints        = 0;
			PendingIncomingRects    = 0;
			PendingIncomingPx       = 0;
			PendingLargestIncoming  = 0;
		}
	}

	Stat_CefPaints     += LocalCefPaints;
	Stat_IncomingRects += LocalInRects;
	Stat_IncomingPx    += LocalInPx;
	const bool bHasFreshPaintThisTick = bDrainedFreshPaintThisTick;
	if (LocalLargestIn > Stat_LargestIncoming) Stat_LargestIncoming = LocalLargestIn;

	int32 TargetFPS = WindowlessFrameRate > 0 ? WindowlessFrameRate : 60;
	double MinFrameInterval = 1.0 / double(TargetFPS);

	bBrowserDirty = (LocalRects.Num() > 0 || HasActiveDirtyTiles());
	if (bBrowserDirty)
		LastDirtyTime = Now;

	bool bShouldSendFrame = false;
	if (bBrowserDirty || bBrowserAnimating)
	{
		if ((Now - LastBrowserFrameTime) >= MinFrameInterval || bBrowserDirty)
		{
			bShouldSendFrame = true;
		}
	}
	if ((Now - LastBrowserFrameTime) > BrowserFrameTimeout)
	{
		bShouldSendFrame = true;
	}
	if (bShouldSendFrame)
	{
		SendExternalBeginFrameIfDue(float(Now - LastBrowserFrameTime));
		LastBrowserFrameTime = Now;
		if (!bBrowserAnimating)
			bBrowserDirty = false;
	}

	if (Texture && Texture->GetResource() && (LocalRects.Num() > 0 || bNeedsFullBaselineUpload || HasActiveDirtyTiles()))
	{
		const double DeferredStart = FPlatformTime::Seconds();
		const int32 SnapW     = LastSnapW = Texture->GetSizeX();
		const int32 SnapH     = LastSnapH = Texture->GetSizeY();
		const int32 FullPitch = SnapW * 4;

		const bool  bHybridEnabled   = SwuiCVarBool(CVarSwuiPaintHybridDirtyUpload.GetValueOnGameThread(), InstanceSettings.bEnableHybridDirtyUpload);
		const bool  bTileDiffEnabled = SwuiCVarBool(CVarSwuiPaintTileDiffLargeRects.GetValueOnGameThread(), InstanceSettings.bEnableTileDiffForLargeRects);
		const bool  bBudgetEnabled   = SwuiCVarBool(CVarSwuiPaintUploadBudget.GetValueOnGameThread(), InstanceSettings.bEnableUploadBudget);
		const bool  bCenterEnabled   = SwuiCVarBool(CVarSwuiPaintCenterCritical.GetValueOnGameThread(), InstanceSettings.bAlwaysProcessCenterCriticalRect);
		const bool  bRotatingCursor  = SwuiCVarBool(CVarSwuiPaintRotatingCursor.GetValueOnGameThread(), InstanceSettings.bUseRotatingDeferredTileCursor);
		const int32 TileW            = FMath::Max(8, SwuiCVarInt(CVarSwuiPaintTileWidth.GetValueOnGameThread(), InstanceSettings.TileWidth));
		const int32 TileH            = FMath::Max(8, SwuiCVarInt(CVarSwuiPaintTileHeight.GetValueOnGameThread(), InstanceSettings.TileHeight));
		const int32 MinDirtyW        = FMath::Max(1, SwuiCVarInt(CVarSwuiPaintMinDirtyRectWidth.GetValueOnGameThread(), InstanceSettings.MinDirtyRectWidth));
		const int32 MinDirtyH        = FMath::Max(1, SwuiCVarInt(CVarSwuiPaintMinDirtyRectHeight.GetValueOnGameThread(), InstanceSettings.MinDirtyRectHeight));
		const int32 CenterW          = FMath::Max(1, SwuiCVarInt(CVarSwuiPaintCenterCriticalWidth.GetValueOnGameThread(), InstanceSettings.CenterCriticalWidth));
		const int32 CenterH          = FMath::Max(1, SwuiCVarInt(CVarSwuiPaintCenterCriticalHeight.GetValueOnGameThread(), InstanceSettings.CenterCriticalHeight));
		const int32 MaxNormalUploadBytes = FMath::Max(0, SwuiCVarInt(CVarSwuiPaintMaxNormalUploadBytes.GetValueOnGameThread(), InstanceSettings.MaxNormalUploadBytesPerFrame));
		const int32 NormalBudget     = bBudgetEnabled ? MaxNormalUploadBytes : INT_MAX;
		const float MergeRatioDefault = InstanceSettings.MaxMergeWasteRatio > 0.f ? InstanceSettings.MaxMergeWasteRatio : (float)SwuiDefaultMaxMergeWasteRatio;
		const float MergeRatio       = SwuiCVarFloat(-1.f, MergeRatioDefault);
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

		if (bAwaitingFreshPaintForForcedUpload)
		{
			const bool bFreshPaintArrived = PaintGeneration > ForcedUploadRequestedAtPaintGeneration;
			if (!bFreshPaintArrived)
			{
				return;
			}
			bAwaitingFreshPaintForForcedUpload = false;
		}

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
				ActiveDirtyTileMask.Init(false, ActiveDirtyTileMask.Num());
				DirtyTileScanCursor = 0;
				bDidBaselineThisTick = true;

				if (PendingFreshFullUploads > 0)
				{
					--PendingFreshFullUploads;
					bNeedsFullBaselineUpload = (PendingFreshFullUploads > 0);
				}
				else
				{
					bNeedsFullBaselineUpload = false;
				}

				const bool bSkipUpload = CVarSwuiNoTextureUpload.GetValueOnAnyThread() != 0
					|| InstanceSettings.bSkipTextureUpload || InstanceSettings.bNoTextureUpload;
				if (bSkipUpload) { delete UploadData; }
				else
				{
					if (bHasFreshPaintThisTick && LocalFreshPaintArrivalTime > 0.0)
					{
						RecordTimingSampleMs(
							(FPlatformTime::Seconds() - LocalFreshPaintArrivalTime) * 1000.0,
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
			const bool bForceFullUploadMode =
				bAwaitingFreshPaintForForcedUpload ||
				bNeedsFullBaselineUpload ||
				PendingFreshFullUploads > 0 ||
				PendingFullCefPaintCopies > 0 ||
				bUiInteractionActive ||
				SuppressCenterCriticalRectFrames > 0;

			if (bCenterEnabled && !bForceFullUploadMode)
			{
				CenterRect = SwuiClampRect(
					FIntRect(
						FIntPoint((SnapW - CenterW) / 2, (SnapH - CenterH) / 2),
						FIntPoint((SnapW + CenterW) / 2, (SnapH + CenterH) / 2)),
					SnapW,
					SnapH);

				bHasCenterRect = CenterRect.Width() > 0 && CenterRect.Height() > 0;
			}

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

			if (bHasCenterRect && !bForceFullUploadMode)
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
					if (bHasFreshPaintThisTick && LocalFreshPaintArrivalTime > 0.0)
					{
						RecordTimingSampleMs(
							(FPlatformTime::Seconds() - LocalFreshPaintArrivalTime) * 1000.0,
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

	if (SuppressCenterCriticalRectFrames > 0)
	{
		--SuppressCenterCriticalRectFrames;
	}

	if (RenderActivityMode == ESwuiRenderActivityMode::FullTransition)
	{
		const bool bTransitionDone =
			PendingFullCefPaintCopies == 0 &&
			PendingFreshFullUploads == 0 &&
			!bAwaitingFreshPaintForForcedUpload &&
			!bNeedsFullBaselineUpload;
		if (bTransitionDone)
		{
			RenderActivityMode = bUiInteractionActive
				? ESwuiRenderActivityMode::InteractiveUi
				: ESwuiRenderActivityMode::NormalHud;
			if (CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0 || InstanceSettings.bVerbosePaintLog)
			{
				UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI TRANSITION] FullTransition complete → %s"),
					RenderActivityMode == ESwuiRenderActivityMode::InteractiveUi ? TEXT("InteractiveUi") : TEXT("NormalHud"));
			}
		}
	}

	if (SwuiCVarBool(CVarSwuiDebugLogPaintStats.GetValueOnGameThread(), InstanceSettings.bLogSwuiPaintStats) && (Now - Stat_LastLogTime >= 1.0))
	{
		const float McAvgMs = Stat_UeUploads > 0 ? float(Stat_MemcpyUs) / Stat_UeUploads / 1000.f : 0.f;
		const float McMaxMs = float(Stat_MemcpyMaxUs) / 1000.f;
		const float DeferredUploadAvgMs = Stat_DeferredUploadSamples > 0 ? float(Stat_DeferredUploadMsSum / Stat_DeferredUploadSamples) : 0.f;
		const float HashAvgMs = Stat_HashSamples > 0 ? float(Stat_HashMsSum / Stat_HashSamples) : 0.f;
		const float PackMemcpyAvgMs = Stat_PackMemcpySamples > 0 ? float(Stat_PackMemcpyMsSum / Stat_PackMemcpySamples) : 0.f;
		const float PaintAfterBeginFrameAvgMs = Stat_PaintAfterBeginFrameSamples > 0 ? float(Stat_PaintAfterBeginFrameMsSum / Stat_PaintAfterBeginFrameSamples) : 0.f;
		const float UploadAfterPaintAvgMs = Stat_UploadAfterPaintSamples > 0 ? float(Stat_UploadAfterPaintMsSum / Stat_UploadAfterPaintSamples) : 0.f;
		UE_LOG(LogSwuiRuntime, Log,
			TEXT("[SwuiPaint] mode=CPU Compatible  subsystemTicks/s=%d  viewUploadTicks/s=%d  cefPaints/s=%d")
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

	if (SwuiCVarBool(CVarSwuiDebugShowDirtyRects.GetValueOnGameThread(), InstanceSettings.bShowDirtyRectOverlay || InstanceSettings.bShowSwuiDirtyRects)
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

// ---------------------------------------------------------------------------
// StageFullSurfacePaint — force-full-frame paint path.
// Copies the full CEF buffer into BackingBuffer under the mutex.
// Does NOT touch pending dirty rects, transition state, or tile state.
// ---------------------------------------------------------------------------
void USwuiView::StageFullSurfacePaint(const void* Buffer, int32 InWidth, int32 InHeight, double PaintNow)
{
	const int32 FullPitch = InWidth * 4;
	const int32 BufBytes  = FullPitch * InHeight;

	const double CopyStart = FPlatformTime::Seconds();

	FScopeLock Lock(&PaintMutex);

	LastPaintArrivalTime = PaintNow;
	PendingFreshPaintArrivalTime = PaintNow;
	++PendingFreshPaintGeneration;

	if (BackingBuffer.Num() != BufBytes)
	{
		BackingBuffer.SetNumUninitialized(BufBytes);
	}

	if (!InstanceSettings.bSkipPaintMemcpy && !InstanceSettings.bFreezeTexture)
	{
		FPlatformMemory::Memcpy(BackingBuffer.GetData(), Buffer, BufBytes);
	}

	bHasPendingFullSurfacePaint = true;
	PendingFullSurfaceWidth = InWidth;
	PendingFullSurfaceHeight = InHeight;

	++Stat_ForceFullFrameCefPaints;

	const double CopyMs = (FPlatformTime::Seconds() - CopyStart) * 1000.0;
	Stat_ForceFullFramePaintCopyMsSum += CopyMs;
	Stat_ForceFullFramePaintCopyMsMax = FMath::Max(Stat_ForceFullFramePaintCopyMsMax, CopyMs);
	++Stat_ForceFullFramePaintCopySamples;
}

// ---------------------------------------------------------------------------
// TickForceFullSurfaceUpload — dedicated full-surface CPU upload path for
// force-full-frame mode. Does NOT interact with dirty rects, tile diff,
// center-critical rects, transition state, or baseline upload flags.
// ---------------------------------------------------------------------------
void USwuiView::TickForceFullSurfaceUpload(double Now)
{
	if (!bForceFullFrameFirstEntryLogged)
	{
		bForceFullFrameFirstEntryLogged = true;
		UE_LOG(LogSwuiRuntime, Log,
			TEXT("[SwuiForceFullFrame] ACTIVE — full-surface upload path enabled; dirty/tile/transition pipeline bypassed."));
	}

	PumpBrowserFrameForced(Now);

	if (!Texture || !Texture->GetResource())
	{
		LogForceFullSurfaceStatsIfNeeded(Now);
		return;
	}

	if (InstanceSettings.bSkipTextureUpload || InstanceSettings.bNoTextureUpload || CVarSwuiNoTextureUpload.GetValueOnAnyThread() != 0)
	{
		LogForceFullSurfaceStatsIfNeeded(Now);
		return;
	}

	const int32 SnapW = Texture->GetSizeX();
	const int32 SnapH = Texture->GetSizeY();
	const int32 FullPitch = SnapW * 4;
	const int32 BufBytes = FullPitch * SnapH;

	FSwuiPaintUploadData* UploadData = new FSwuiPaintUploadData;
	UploadData->Texture2DResource = static_cast<FTextureResource*>(Texture->GetResource());
	UploadData->PackedPixels.SetNumUninitialized(BufBytes);
	UploadData->Rects.SetNum(1);

	double LocalPaintArrivalTime = 0.0;
	bool bCopied = false;

	const double LockWaitStart = FPlatformTime::Seconds();
	{
		FScopeLock Lock(&PaintMutex);

		Stat_ForceFullFrameLockWaitMs += (FPlatformTime::Seconds() - LockWaitStart) * 1000.0;

		if (BackingBuffer.Num() == BufBytes)
		{
			const double CopyStart = FPlatformTime::Seconds();

			FPlatformMemory::Memcpy(
				UploadData->PackedPixels.GetData(),
				BackingBuffer.GetData(),
				BufBytes);

			const double CopyMs = (FPlatformTime::Seconds() - CopyStart) * 1000.0;
			Stat_ForceFullFrameUploadCopyMsSum += CopyMs;
			Stat_ForceFullFrameUploadCopyMsMax = FMath::Max(Stat_ForceFullFrameUploadCopyMsMax, CopyMs);
			++Stat_ForceFullFrameUploadCopySamples;

			LocalPaintArrivalTime = PendingFreshPaintArrivalTime;
			bHasPendingFullSurfacePaint = false;
			bCopied = true;
		}
	}

	if (!bCopied)
	{
		delete UploadData;
		LogForceFullSurfaceStatsIfNeeded(Now);
		return;
	}

	FSwuiPackedRectDesc& D = UploadData->Rects[0];
	D.Region = FUpdateTextureRegion2D(0, 0, 0, 0, SnapW, SnapH);
	D.SrcPitch = static_cast<uint32>(FullPitch);
	D.SrcOffsetBytes = 0;

	const double EnqueueStart = FPlatformTime::Seconds();

	ENQUEUE_RENDER_COMMAND(UpdateSwuiViewForceFullSurface)(
		[UploadData](FRHICommandList& RHICmdList)
		{
			FRHITexture* Tex = UploadData->Texture2DResource->TextureRHI.GetReference();
			if (Tex)
			{
				const FSwuiPackedRectDesc& Rd = UploadData->Rects[0];
				RHIUpdateTexture2D(Tex, 0, Rd.Region, Rd.SrcPitch, UploadData->PackedPixels.GetData());
			}
			delete UploadData;
		});

	const double EnqueueMs = (FPlatformTime::Seconds() - EnqueueStart) * 1000.0;
	Stat_ForceFullFrameEnqueueMsSum += EnqueueMs;
	Stat_ForceFullFrameEnqueueMsMax = FMath::Max(Stat_ForceFullFrameEnqueueMsMax, EnqueueMs);
	++Stat_ForceFullFrameEnqueueSamples;

	++Stat_ForceFullFrameUploads;
	Stat_ForceFullFramePx += int64(SnapW) * SnapH;

	if (LocalPaintArrivalTime > 0.0)
	{
		const double PaintToUploadMs = (FPlatformTime::Seconds() - LocalPaintArrivalTime) * 1000.0;
		Stat_ForceFullFramePaintToUploadMsSum += PaintToUploadMs;
		Stat_ForceFullFramePaintToUploadMsMax = FMath::Max(Stat_ForceFullFramePaintToUploadMsMax, PaintToUploadMs);
		++Stat_ForceFullFramePaintToUploadSamples;
	}

	DrainDirtyPipelineStateForForceMode();
	LogForceFullSurfaceStatsIfNeeded(Now);
}

// ---------------------------------------------------------------------------
// PumpBrowserFrameForced — force a CEF begin frame immediately, bypassing
// rate limiting and the normal send-if-due logic.
// ---------------------------------------------------------------------------
void USwuiView::PumpBrowserFrameForced(double Now)
{
	const double DeltaTime = LastBrowserFrameTime > 0.0
		? FMath::Max(0.0, Now - LastBrowserFrameTime)
		: 1.0 / 60.0;

	FlushHudStateAndRequestBrowserFrame(FString(), static_cast<float>(DeltaTime), true);
	LastBrowserFrameTime = Now;
}

// ---------------------------------------------------------------------------
// DrainDirtyPipelineStateForForceMode — resets all dirty-rect / tile /
// transition state so it cannot leak back into the normal path when force
// mode is later disabled.
// ---------------------------------------------------------------------------
void USwuiView::DrainDirtyPipelineStateForForceMode()
{
	FScopeLock Lock(&PaintMutex);

	bHasPendingUpload = false;
	PendingDirtyRects.Empty();
	PendingOverlayRects.Empty();
	PendingCefPaints = 0;
	PendingIncomingRects = 0;
	PendingIncomingPx = 0;
	PendingLargestIncoming = 0;

	PendingFullCefPaintCopies = 0;
	PendingFreshFullUploads = 0;
	bAwaitingFreshPaintForForcedUpload = false;
	bNeedsFullBaselineUpload = false;
	SuppressCenterCriticalRectFrames = 0;

	if (RenderActivityMode == ESwuiRenderActivityMode::FullTransition)
	{
		RenderActivityMode = bUiInteractionActive
			? ESwuiRenderActivityMode::InteractiveUi
			: ESwuiRenderActivityMode::NormalHud;
	}

	if (ActiveDirtyTileMask.Num() > 0)
	{
		ActiveDirtyTileMask.Init(false, ActiveDirtyTileMask.Num());
	}

	if (UploadedTileMask.Num() > 0)
	{
		UploadedTileMask.Init(false, UploadedTileMask.Num());
	}

	DirtyTileScanCursor = 0;
	bSeenFirstPaint = false;
	bAutoTransitionEverFired = false;
}

// ---------------------------------------------------------------------------
// LogForceFullSurfaceStatsIfNeeded — once-per-second stats line for the
// force-full-frame upload path.
// ---------------------------------------------------------------------------
void USwuiView::LogForceFullSurfaceStatsIfNeeded(double Now)
{
	if ((Now - Stat_ForceFullFrameLastLogTime) < 1.0)
	{
		return;
	}

	const float PaintCopyAvgMs = Stat_ForceFullFramePaintCopySamples > 0
		? float(Stat_ForceFullFramePaintCopyMsSum / Stat_ForceFullFramePaintCopySamples)
		: 0.f;

	const float UploadCopyAvgMs = Stat_ForceFullFrameUploadCopySamples > 0
		? float(Stat_ForceFullFrameUploadCopyMsSum / Stat_ForceFullFrameUploadCopySamples)
		: 0.f;

	const float EnqueueAvgMs = Stat_ForceFullFrameEnqueueSamples > 0
		? float(Stat_ForceFullFrameEnqueueMsSum / Stat_ForceFullFrameEnqueueSamples)
		: 0.f;

	const float PaintToUploadAvgMs = Stat_ForceFullFramePaintToUploadSamples > 0
		? float(Stat_ForceFullFramePaintToUploadMsSum / Stat_ForceFullFramePaintToUploadSamples)
		: 0.f;

	UE_LOG(LogSwuiRuntime, Log,
		TEXT("[SwuiForceFullFrame] forcedUploads/s=%d cefPaints/s=%d uploadedPx/s=%lld")
		TEXT(" paintCopyAvgMs=%.3f paintCopyMaxMs=%.3f")
		TEXT(" uploadCopyAvgMs=%.3f uploadCopyMaxMs=%.3f")
		TEXT(" enqueueAvgMs=%.3f enqueueMaxMs=%.3f")
		TEXT(" lockWaitMs=%.3f")
		TEXT(" paintToUploadAvgMs=%.3f paintToUploadMaxMs=%.3f")
		TEXT(" tex=%dx%d"),
		Stat_ForceFullFrameUploads,
		Stat_ForceFullFrameCefPaints,
		Stat_ForceFullFramePx,
		PaintCopyAvgMs,
		Stat_ForceFullFramePaintCopyMsMax,
		UploadCopyAvgMs,
		Stat_ForceFullFrameUploadCopyMsMax,
		EnqueueAvgMs,
		Stat_ForceFullFrameEnqueueMsMax,
		Stat_ForceFullFrameLockWaitMs,
		PaintToUploadAvgMs,
		Stat_ForceFullFramePaintToUploadMsMax,
		Texture ? Texture->GetSizeX() : 0,
		Texture ? Texture->GetSizeY() : 0);

	Stat_ForceFullFrameUploads = 0;
	Stat_ForceFullFrameCefPaints = 0;
	Stat_ForceFullFramePx = 0;

	Stat_ForceFullFramePaintCopyMsSum = 0.0;
	Stat_ForceFullFramePaintCopyMsMax = 0.0;
	Stat_ForceFullFramePaintCopySamples = 0;

	Stat_ForceFullFrameUploadCopyMsSum = 0.0;
	Stat_ForceFullFrameUploadCopyMsMax = 0.0;
	Stat_ForceFullFrameUploadCopySamples = 0;

	Stat_ForceFullFrameEnqueueMsSum = 0.0;
	Stat_ForceFullFrameEnqueueMsMax = 0.0;
	Stat_ForceFullFrameEnqueueSamples = 0;

	Stat_ForceFullFrameLockWaitMs = 0.0;

	Stat_ForceFullFramePaintToUploadMsSum = 0.0;
	Stat_ForceFullFramePaintToUploadMsMax = 0.0;
	Stat_ForceFullFramePaintToUploadSamples = 0;

	Stat_ForceFullFrameLastLogTime = Now;
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

// ---------------------------------------------------------------------------
// GPU Accelerated backend support
// ---------------------------------------------------------------------------

// static
bool USwuiView::IsGpuAcceleratedSupported()
{
#if PLATFORM_WINDOWS
	// Require D3D11 RHI — shared texture handles are D3D11-only in CEF.
	const FString RHIName = GDynamicRHI ? GDynamicRHI->GetName() : TEXT("");
	if (RHIName.Contains(TEXT("D3D11")))
	{
		return true;
	}
	// D3D12 could theoretically work with D3D11on12 interop, but CEF's
	// shared textures are D3D11-native.  Conservatively reject for now.
	return false;
#else
	return false;
#endif
}

// ---------------------------------------------------------------------------
// OnAcceleratedPaint — called on the CEF renderer thread.
//
// The shared handle in |SharedHandle| is only valid for the duration of this
// call.  Per CEF docs: "The handle's resource cannot be cached and cannot be
// accessed outside of this callback."
//
// We enqueue a blocking render command that opens the shared handle on UE's
// D3D11 device, copies it to the persistent UE texture, and releases the
// resource — all before this callback returns.
//
// Thread safety:
//   - D3D11 Device::OpenSharedResource is thread-safe.
//   - We synchronise with the render thread via FlushRenderingCommands.
//   - The CEF thread blocks briefly during the GPU copy (~0.1 ms typical).
// ---------------------------------------------------------------------------
void USwuiView::OnAcceleratedPaint(void* SharedHandle, int32 InWidth, int32 InHeight)
{
	if (!SharedHandle) return;

#if PLATFORM_WINDOWS
	// Track generation for HasFreshOnPaintDataPending().
	{
		FScopeLock Lock(&AccelPaintMutex);
		++AccelPaintGeneration;
		PendingAccelWidth  = InWidth;
		PendingAccelHeight = InHeight;
	}

	// Ensure texture exists on the game thread (GetOrCreateTexture is game-thread only).
	// If the texture doesn't exist yet or size mismatches, we'll skip this frame.
	// The next game-thread tick will create the texture, and the following accelerated
	// paint will succeed.
	if (!Texture || !Texture->GetResource()) return;
	if (Texture->GetSizeX() != InWidth || Texture->GetSizeY() != InHeight)
	{
		// Signal that a resize is needed — game thread will handle it.
		FScopeLock Lock(&AccelPaintMutex);
		bHasPendingAccelPaint = true;
		PendingSharedHandle = nullptr; // handle expires after this callback
		return;
	}

	FTextureResource* TexResource = (FTextureResource*)Texture->GetResource();
	if (!TexResource) return;

	// Open the shared handle on UE's D3D11 device.  Device::OpenSharedResource
	// is thread-safe on D3D11 — it can be called from the CEF thread.
	ID3D11Device* Device = static_cast<ID3D11Device*>(GDynamicRHI->RHIGetNativeDevice());
	if (!Device)
	{
		++Stat_AccelHandleFails;
		return;
	}

	ID3D11Texture2D* SharedTex = nullptr;
	HRESULT Hr = Device->OpenSharedResource(SharedHandle, __uuidof(ID3D11Texture2D), (void**)&SharedTex);
	if (FAILED(Hr) || !SharedTex)
	{
		UE_LOG(LogSwuiRuntime, Warning, TEXT("OnAcceleratedPaint: OpenSharedResource failed (HRESULT=0x%08X)"), Hr);
		++Stat_AccelHandleFails;
		return;
	}

	// Enqueue a render command to copy the opened shared texture to UE's texture.
	// We own the SharedTex reference — the render command releases it.
	int32* StatCopies = &Stat_AccelCopies;
	int32* StatFails  = &Stat_AccelHandleFails;

	// Use a completion event so we can block the CEF thread until the copy
	// finishes.  This ensures the shared handle pool resource remains valid
	// during the GPU copy.
	FEvent* CompletionEvent = FPlatformProcess::GetSynchEventFromPool(true);

	ENQUEUE_RENDER_COMMAND(SwuiAcceleratedCopy)(
		[TexResource, SharedTex, StatCopies, StatFails, CompletionEvent](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* RHITex = TexResource->TextureRHI.GetReference();
			ID3D11Texture2D* DestTex = RHITex ? static_cast<ID3D11Texture2D*>(RHITex->GetNativeResource()) : nullptr;

			if (DestTex)
			{
				ID3D11Device* Dev = nullptr;
				DestTex->GetDevice(&Dev);
				if (Dev)
				{
					ID3D11DeviceContext* Ctx = nullptr;
					Dev->GetImmediateContext(&Ctx);
					if (Ctx)
					{
						// Full GPU-to-GPU copy — no CPU staging, no memcpy.
						Ctx->CopyResource(DestTex, SharedTex);
						++(*StatCopies);
						Ctx->Release();
					}
					else
					{
						++(*StatFails);
					}
					Dev->Release();
				}
				else
				{
					++(*StatFails);
				}
			}
			else
			{
				++(*StatFails);
			}

			SharedTex->Release();
			CompletionEvent->Trigger();
		});

	// Block the CEF thread until the render command completes the copy.
	// The GPU copy is fast (~0.05–0.2 ms) — well within CEF callback tolerance.
	const double CopyStart = FPlatformTime::Seconds();
	CompletionEvent->Wait();
	FPlatformProcess::ReturnSynchEventToPool(CompletionEvent);

	const double CopyMs = (FPlatformTime::Seconds() - CopyStart) * 1000.0;
	Stat_AccelCopyMsSum += CopyMs;
	if (CopyMs > Stat_AccelCopyMsMax) Stat_AccelCopyMsMax = CopyMs;
	++Stat_AccelCopySamples;
	++Stat_AccelPaints;

	// Mark as drained so HasFreshOnPaintDataPending knows.
	{
		FScopeLock Lock(&AccelPaintMutex);
		AccelDrainedGeneration = AccelPaintGeneration;
		bHasPendingAccelPaint = false;
	}
#endif // PLATFORM_WINDOWS
}

// ---------------------------------------------------------------------------
// TickAcceleratedUpload — called on the game thread from TickDeferredUpload.
//
// In GPU Accelerated mode, the actual shared-texture copy happens
// synchronously inside OnAcceleratedPaint on the CEF thread (via a blocking
// render command).  TickAcceleratedUpload handles deferred work that must
// run on the game thread:
//   - Texture recreation on resize.
//   - Stats tracking.
// ---------------------------------------------------------------------------
void USwuiView::TickAcceleratedUpload()
{
#if PLATFORM_WINDOWS
	int32 LocalWidth = 0;
	int32 LocalHeight = 0;
	bool bNeedsResize = false;

	{
		FScopeLock Lock(&AccelPaintMutex);
		LocalWidth  = PendingAccelWidth;
		LocalHeight = PendingAccelHeight;
		// Check if OnAcceleratedPaint signaled a size mismatch.
		bNeedsResize = bHasPendingAccelPaint && PendingSharedHandle == nullptr
			&& LocalWidth > 0 && LocalHeight > 0;
		if (bNeedsResize)
		{
			bHasPendingAccelPaint = false;
		}
	}

	// Handle deferred texture resize (game thread only).
	if (bNeedsResize)
	{
		if (!Texture || Texture->GetSizeX() != LocalWidth || Texture->GetSizeY() != LocalHeight)
		{
			GetOrCreateTexture(LocalWidth, LocalHeight);
			++Stat_AccelTexRecreates;
			++Stat_AccelResizes;
			UE_LOG(LogSwuiRuntime, Log, TEXT("USwuiView: GPU Accelerated texture resized to %dx%d"), LocalWidth, LocalHeight);
		}
	}

#endif // PLATFORM_WINDOWS
}

// ---------------------------------------------------------------------------
// Pointer Input Forwarding — called from game thread, forwards to CEF.
// ---------------------------------------------------------------------------

void USwuiView::SetPointerInputEnabled(bool bEnabled)
{
	bPointerInputEnabled = bEnabled;
	UE_LOG(LogSwuiRuntime, Log, TEXT("[SwuiPointer] SetPointerInputEnabled(%s)"),
		bEnabled ? TEXT("true") : TEXT("false"));
}

bool USwuiView::HasBrowserHost() const
{
	return CefData && CefData->Browser && CefData->Browser->GetHost() != nullptr;
}

bool USwuiView::ScreenToBrowserPixel(const FVector2D& ScreenPos, int32& OutX, int32& OutY) const
{
	if (Width <= 0 || Height <= 0)
	{
		return false;
	}

	// Primary path: Slate viewport geometry conversion.
	if (FSlateApplication::IsInitialized())
	{
		TSharedPtr<SViewport> ViewportWidget = FSlateApplication::Get().GetGameViewport();

		if (ViewportWidget.IsValid())
		{
			const FGeometry& Geometry = ViewportWidget->GetCachedGeometry();
			const FVector2D LocalPos = Geometry.AbsoluteToLocal(ScreenPos);
			const FVector2D LocalSize = Geometry.GetLocalSize();

			if (LocalSize.X > 0.0f && LocalSize.Y > 0.0f &&
				LocalPos.X >= 0.0f && LocalPos.Y >= 0.0f &&
				LocalPos.X <= LocalSize.X && LocalPos.Y <= LocalSize.Y)
			{
				const float ScaleX = static_cast<float>(Width) / LocalSize.X;
				const float ScaleY = static_cast<float>(Height) / LocalSize.Y;

				OutX = FMath::Clamp(FMath::RoundToInt(LocalPos.X * ScaleX), 0, Width - 1);
				OutY = FMath::Clamp(FMath::RoundToInt(LocalPos.Y * ScaleY), 0, Height - 1);
				return true;
			}

			return false;
		}
	}

	// Fallback: direct screen-space to pixel (valid when viewport fills the entire window).
	if (ScreenPos.X < 0.0f || ScreenPos.Y < 0.0f ||
		ScreenPos.X >= static_cast<float>(Width) ||
		ScreenPos.Y >= static_cast<float>(Height))
	{
		return false;
	}

	OutX = FMath::Clamp(FMath::RoundToInt(ScreenPos.X), 0, Width - 1);
	OutY = FMath::Clamp(FMath::RoundToInt(ScreenPos.Y), 0, Height - 1);
	return true;
}

bool USwuiView::ForwardMouseMoveToBrowser(const FVector2D& ScreenPosition)
{
	if (!bPointerInputEnabled) return false;
	if (!CefData || !CefData->Browser) return false;

	CefRefPtr<CefBrowserHost> Host = CefData->Browser->GetHost();
	if (!Host) return false;

	int32 BX = 0, BY = 0;
	if (!ScreenToBrowserPixel(ScreenPosition, BX, BY)) return false;

	CefMouseEvent Event;
	Event.x = BX;
	Event.y = BY;
	Event.modifiers = 0;

	Host->SendMouseMoveEvent(Event, false);

	if (CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0)
	{
		UE_LOG(LogSwuiRuntime, Verbose, TEXT("[SwuiPointer] MouseMove: (%.0f, %.0f) -> browser (%d, %d)"),
			ScreenPosition.X, ScreenPosition.Y, BX, BY);
	}

	return true;
}

static cef_mouse_button_type_t SwuiMapKeyToCefButton(FKey Button)
{
	if (Button == EKeys::LeftMouseButton)   return MBT_LEFT;
	if (Button == EKeys::RightMouseButton)  return MBT_RIGHT;
	if (Button == EKeys::MiddleMouseButton) return MBT_MIDDLE;
	return MBT_LEFT;
}

bool USwuiView::ForwardMouseButtonToBrowser(const FVector2D& ScreenPosition, FKey Button, bool bMouseUp, int32 ClickCount)
{
	if (!bPointerInputEnabled) return false;
	if (!CefData || !CefData->Browser) return false;

	CefRefPtr<CefBrowserHost> Host = CefData->Browser->GetHost();
	if (!Host) return false;

	int32 BX = 0, BY = 0;
	if (!ScreenToBrowserPixel(ScreenPosition, BX, BY)) return false;

	cef_mouse_button_type_t CefButton = SwuiMapKeyToCefButton(Button);

	CefMouseEvent Event;
	Event.x = BX;
	Event.y = BY;
	Event.modifiers = 0;

	Host->SendMouseClickEvent(Event, CefButton, bMouseUp, ClickCount);

	const TCHAR* Action = bMouseUp ? TEXT("Up") : TEXT("Down");
	UE_LOG(LogSwuiRuntime, Log, TEXT("[SwuiPointer] MouseButton %s %s  clickCount=%d  (%d, %d)"),
		*Button.ToString(), Action, ClickCount, BX, BY);

	return true;
}

bool USwuiView::ForwardMouseWheelToBrowser(const FVector2D& ScreenPosition, float DeltaX, float DeltaY)
{
	if (!bPointerInputEnabled) return false;
	if (!CefData || !CefData->Browser) return false;

	CefRefPtr<CefBrowserHost> Host = CefData->Browser->GetHost();
	if (!Host) return false;

	int32 BX = 0, BY = 0;
	if (!ScreenToBrowserPixel(ScreenPosition, BX, BY)) return false;

	// CEF expects wheel deltas in physical pixels (typically ~120 per notch on Windows).
	const int32 CefDeltaX = FMath::RoundToInt(DeltaX * 120.0f);
	const int32 CefDeltaY = FMath::RoundToInt(DeltaY * 120.0f);

	CefMouseEvent Event;
	Event.x = BX;
	Event.y = BY;
	Event.modifiers = 0;

	Host->SendMouseWheelEvent(Event, CefDeltaX, CefDeltaY);

	UE_LOG(LogSwuiRuntime, Log, TEXT("[SwuiPointer] Wheel: delta=(%d, %d)  (%d, %d)"),
		CefDeltaX, CefDeltaY, BX, BY);

	return true;
}

void USwuiView::SetBrowserInputFocus(bool bFocused)
{
	if (!CefData || !CefData->Browser) return;

	CefRefPtr<CefBrowserHost> Host = CefData->Browser->GetHost();
	if (Host)
	{
		Host->SetFocus(bFocused);
		UE_LOG(LogSwuiRuntime, Log, TEXT("[SwuiPointer] BrowserHost->SetFocus(%s)"),
			bFocused ? TEXT("true") : TEXT("false"));
	}
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
