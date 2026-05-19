#include "SwuiView.h"

#include "ISwuiRuntime.h"
#include "SwuiCVars.h"
#include "SwuiCVarHelpers.h"
#include "SwuiManager.h"
#include "SwuiNavigation.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "GameplayTagsManager.h"
#include "InputCoreTypes.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TextureResource.h"
#include "Widgets/SViewport.h"

#include "SwuiFullSurfaceCpuRenderer.h"

struct FSwuiViewCefData
{
	CefRefPtr<BrowserClient> Client;
	CefRefPtr<CefBrowser> Browser;
};

// ---------------------------------------------------------------------------
// CEF task: execute optional JS, optionally invalidate, optionally send an
// external begin frame.
// ---------------------------------------------------------------------------
class FSwuiFlushAndBeginFrameTask : public CefTask
{
public:
	FSwuiFlushAndBeginFrameTask(
		CefRefPtr<CefBrowser> InBrowser,
		std::string InScript,
		bool bInInvalidateView,
		bool bInSendBeginFrame)
		: Browser(InBrowser)
		, Script(MoveTemp(InScript))
		, bInvalidateView(bInInvalidateView)
		, bSendBeginFrame(bInSendBeginFrame)
	{
	}

	void Execute() override
	{
		if (!Browser)
		{
			return;
		}

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

USwuiView::USwuiView()
{
	Texture = nullptr;
	CefData = MakeShared<FSwuiViewCefData>();
}

// ---------------------------------------------------------------------------
// UI Resolution preset helpers
// ---------------------------------------------------------------------------
static FIntPoint ResolveSwuiUiResolution(const FSwuiInstanceSettings& Settings, const FIntPoint& ViewportSize)
{
	const int32 PresetOverride = CVarSwuiUiResolutionPreset.GetValueOnGameThread();
	const ESwuiUiResolutionPreset EffectivePreset = (PresetOverride >= 0 && PresetOverride <= 5)
		? static_cast<ESwuiUiResolutionPreset>(PresetOverride)
		: Settings.UiResolutionPreset;

	switch (EffectivePreset)
	{
	case ESwuiUiResolutionPreset::Performance720p:
		return FIntPoint(1280, 720);
	case ESwuiUiResolutionPreset::Balanced900p:
		return FIntPoint(1600, 900);
	case ESwuiUiResolutionPreset::Quality1080p:
		return FIntPoint(1920, 1080);
	case ESwuiUiResolutionPreset::High1440p:
		return FIntPoint(2560, 1440);
	case ESwuiUiResolutionPreset::NativeViewport:
		return (ViewportSize.X > 0 && ViewportSize.Y > 0)
			? ViewportSize
			: FIntPoint(1920, 1080);
	case ESwuiUiResolutionPreset::Custom:
	{
		const int32 W = SwuiCVarInt(CVarSwuiCustomUiWidth.GetValueOnGameThread(),  Settings.CustomUiWidth);
		const int32 H = SwuiCVarInt(CVarSwuiCustomUiHeight.GetValueOnGameThread(), Settings.CustomUiHeight);
		return FIntPoint(FMath::Max(1, W), FMath::Max(1, H));
	}
	default:
		return FIntPoint(1920, 1080);
	}
}

static const TCHAR* SwuiUiResolutionPresetName(ESwuiUiResolutionPreset Preset)
{
	switch (Preset)
	{
	case ESwuiUiResolutionPreset::Performance720p: return TEXT("Performance720p");
	case ESwuiUiResolutionPreset::Balanced900p:    return TEXT("Balanced900p");
	case ESwuiUiResolutionPreset::Quality1080p:    return TEXT("Quality1080p");
	case ESwuiUiResolutionPreset::High1440p:       return TEXT("High1440p");
	case ESwuiUiResolutionPreset::NativeViewport:  return TEXT("NativeViewport");
	case ESwuiUiResolutionPreset::Custom:          return TEXT("Custom");
	default:                                       return TEXT("Unknown");
	}
}

void USwuiView::Init(const FSwuiInstanceSettings& InInstanceSettings)
{
	InstanceSettings = InInstanceSettings;
	UpdateHudRoiSettings(InInstanceSettings.HudRoiSettings);

	// Resolve UI resolution preset: override Width/Height with the internal
	// render size chosen by the user, keeping the Subsystem-provided values
	// as the viewport reference for NativeViewport mode.
	const FIntPoint ResolvedRes = ResolveSwuiUiResolution(InstanceSettings, FIntPoint(Width, Height));
	Width  = ResolvedRes.X;
	Height = ResolvedRes.Y;

	if (Width <= 0 || Height <= 0)
	{
		UE_LOG(LogSwuiRuntime, Warning, TEXT("USwuiView: Width or Height <= 0"));
		return;
	}

	const USwuiSettings* Settings = GetDefault<USwuiSettings>();

	const bool bVerboseLog =
		CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0 ||
		InstanceSettings.bVerbosePaintLog ||
		(Settings && Settings->bVerbosePaintLog);

	UE_LOG(LogSwuiRuntime, Log,
		TEXT("[SWUI RESOLUTION] preset=%s internal=%dx%d"),
		SwuiUiResolutionPresetName(InstanceSettings.UiResolutionPreset),
		Width, Height);

	CefWindowInfo Info;
	Info.SetAsWindowless(0);

	const int32 HudLockstepOverride = CVarSwuiHudLockstep.GetValueOnGameThread();
	const int32 HudExternalBeginFrameOverride = CVarSwuiHudExternalBeginFrames.GetValueOnGameThread();
	const int32 HudMaxBrowserFpsOverride = CVarSwuiHudMaxBrowserFPS.GetValueOnGameThread();

	const bool bHudLockstepEnabled = SwuiCVarBool(
		HudLockstepOverride,
		InstanceSettings.bUseUEFrameLockedBrowser);

	const bool bExternalBeginFramesEnabled = SwuiCVarBool(
		HudExternalBeginFrameOverride,
		InstanceSettings.bUseExternalBeginFrames);

	const int32 InitHudMaxBrowserFps = SwuiCVarInt(
		HudMaxBrowserFpsOverride,
		InstanceSettings.MaxBrowserFramesPerSecond);

	const bool bWantsExternalBeginFrames =
		InstanceSettings.bIsHUD &&
		bHudLockstepEnabled &&
		bExternalBeginFramesEnabled;

	Info.external_begin_frame_enabled = bWantsExternalBeginFrames ? 1 : 0;

	// Clean renderer path: CPU-compatible full-surface upload only.
	ResolvedRenderingMode = ESwuiRenderingMode::CpuCompatible;

	if (bVerboseLog)
	{
		const ESwuiRenderingMode RequestedMode = InstanceSettings.RenderingMode;
		UE_LOG(LogSwuiRuntime, Log,
			TEXT("[SWUI RENDER] Resolved mode: CPU FullSurface (requested=%d)"),
			(int32)RequestedMode);
	}

	CefBrowserSettings BrowserSettings;
	BrowserSettings.webgl = STATE_ENABLED;

	ISwuiRenderTarget* CpuTarget = static_cast<ISwuiRenderTarget*>(this);

	// Keep constructor shape compatible with the current RenderHandler API.
	// GPU target is intentionally null in the cleaned full-surface CPU path.
	RenderHandler* Renderer = new RenderHandler(
		Width,
		Height,
		CpuTarget,
		nullptr,
		ResolvedRenderingMode);

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
		UE_LOG(LogSwuiRuntime, Error,
			TEXT("USwuiView::Init: CefBrowserHost::CreateBrowserSync returned null — CEF may not be initialized or the subprocess is missing."));
		return;
	}

	int32 TargetFPS = 300;
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

	const bool bWantVerbose = InstanceSettings.bVerbosePaintLog || (Settings && Settings->bVerbosePaintLog);
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
		UE_LOG(LogSwuiRuntime, Error,
			TEXT("USwuiView::Init: Browser created but BrowserHost is null."));
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
		if (bVerboseLog)
		{
			UE_LOG(LogSwuiRuntime, Log,
				TEXT("USwuiView: External begin frame requested=%s, osrWindowless=%s, active=%s"),
				bWantsExternalBeginFrames ? TEXT("true") : TEXT("false"),
				bHostIsWindowless ? TEXT("true") : TEXT("false"),
				bExternalBeginFrameActive ? TEXT("true") : TEXT("false"));
		}
	}
	else if (InstanceSettings.bIsHUD && bHudLockstepEnabled && bExternalBeginFramesEnabled)
	{
		UE_LOG(LogSwuiRuntime, Warning,
			TEXT("USwuiView: External begin frame mode unavailable; using capped windowless frame pacing fallback."));
	}

	CefData->Client = Client;
	CefData->Browser = Browser;

	if (bVerboseLog)
	{
		UE_LOG(LogSwuiRuntime, Log, TEXT("USwuiView Initialized"));
	}

	ResetTexture();

	if (!DefaultURL.IsEmpty())
	{
		LoadURL(DefaultURL);
	}
}

void USwuiView::LoadURL(const FString& URI)
{
	if (!CefData || !CefData->Browser)
	{
		return;
	}

	{
		FScopeLock Lock(&PaintMutex);
		bHasPendingFullSurfacePaint = false;
		PendingFreshPaintArrivalTime = 0.0;
		LastPaintArrivalTime = 0.0;
	}

	FullSurfaceRenderer.ClearPendingPaint();

	if (URI.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase) ||
		URI.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase) ||
		URI.StartsWith(TEXT("localhost"), ESearchCase::IgnoreCase) ||
		URI.StartsWith(TEXT("file:///"), ESearchCase::IgnoreCase))
	{
		CefData->Browser->GetMainFrame()->LoadURL(*URI);
		RequestBrowserVisualRefresh(true);
		return;
	}

	FString Relative = URI;
	if (Relative.StartsWith(TEXT("swui://"), ESearchCase::IgnoreCase))
	{
		Relative = Relative.RightChop(7);
	}

	if (FPaths::GetExtension(Relative).IsEmpty())
	{
		Relative += TEXT(".html");
	}

	const FString ContentDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
	const FString LocalFile = FString(TEXT("file:///")) + ContentDir + Relative;

	CefData->Browser->GetMainFrame()->LoadURL(*LocalFile);
	RequestBrowserVisualRefresh(true);
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

	UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI JS->UE NAV] tag=%s payload=%s"),
		*TagName,
		*PayloadJson);

	AActor* OwnerActor = ResolveOwningActor();
	if (!OwnerActor)
	{
		UE_LOG(LogSwuiRuntime, Warning,
			TEXT("[SWUI JS->UE NAV] Message received without an owning actor."));
		return false;
	}

	USwuiNavigation* Navigation = OwnerActor->FindComponentByClass<USwuiNavigation>();
	if (!Navigation)
	{
		UE_LOG(LogSwuiRuntime, Warning,
			TEXT("[SWUI JS->UE NAV] Actor '%s' has no USwuiNavigation component to handle '%s'."),
			*OwnerActor->GetName(),
			*TagName);
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

bool USwuiView::IsForceFullFrameMode() const
{
	return SwuiCVarBool(
		CVarSwuiDebugForceFullFrameUploadEveryFrame.GetValueOnAnyThread(),
		InstanceSettings.bDebugForceFullFrameUploadEveryFrame);
}

bool USwuiView::HasFreshOnPaintDataPending() const
{
	FScopeLock Lock(&PaintMutex);
	return bHasPendingFullSurfacePaint && FullSurfaceRenderer.HasFreshPaintPending();
}

void USwuiView::RequestBrowserVisualRefresh(bool bForceFrame)
{
	InvalidateBrowserView();

	if (bForceFrame)
	{
		PumpBrowserFrameIfDue(FPlatformTime::Seconds(), true);
	}
}

bool USwuiView::FlushHudStateAndRequestBrowserFrame(
	const FString& CombinedScript,
	float DeltaTime,
	bool bForceFrame)
{
	const int32 CurrentHudLockstepCVar = CVarSwuiHudLockstep.GetValueOnGameThread();
	const int32 CurrentHudExternalBeginFramesCVar = CVarSwuiHudExternalBeginFrames.GetValueOnGameThread();
	const int32 CurrentHudMaxBrowserFPSCVar = CVarSwuiHudMaxBrowserFPS.GetValueOnGameThread();

	if (CurrentHudLockstepCVar != LastObservedHudLockstepCVar)
	{
		LastObservedHudLockstepCVar = CurrentHudLockstepCVar;

		if (CurrentHudLockstepCVar != AppliedHudLockstepCVar)
		{
			UE_LOG(LogSwuiRuntime, Log,
				TEXT("USwuiView: swui.hud.Lockstep changed at runtime; browser creation behavior updates on next view recreate."));
		}
	}

	if (CurrentHudExternalBeginFramesCVar != LastObservedHudExternalBeginFramesCVar)
	{
		LastObservedHudExternalBeginFramesCVar = CurrentHudExternalBeginFramesCVar;

		if (CurrentHudExternalBeginFramesCVar != AppliedHudExternalBeginFramesCVar)
		{
			UE_LOG(LogSwuiRuntime, Log,
				TEXT("USwuiView: swui.hud.ExternalBeginFrames changed at runtime; browser creation behavior updates on next view recreate."));
		}
	}

	if (CurrentHudMaxBrowserFPSCVar != LastObservedHudMaxBrowserFPSCVar)
	{
		LastObservedHudMaxBrowserFPSCVar = CurrentHudMaxBrowserFPSCVar;

		if (CurrentHudMaxBrowserFPSCVar != AppliedHudMaxBrowserFPSCVar)
		{
			UE_LOG(LogSwuiRuntime, Log,
				TEXT("USwuiView: swui.hud.MaxBrowserFPS changed at runtime; windowless host cap updates on next view recreate."));
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

	// Coalescing: if a previous begin frame is still pending without paint
	// and hasn't timed out yet, skip this non-forced, scriptless begin frame.
	{
		FScopeLock Lock(&PaintMutex);
		if (!bForceFrame && !bHasScript &&
			PendingBeginFrameSentTime > 0.0 &&
			!bPaintArrivedAfterExternalBeginFrame)
		{
			const double Now = FPlatformTime::Seconds();
			if ((Now - PendingBeginFrameSentTime) < BrowserFrameTimeout)
			{
				return false;
			}
		}
	}

	const int32 BrowserFpsSetting = SwuiCVarInt(
		CVarSwuiHudMaxBrowserFPS.GetValueOnGameThread(),
		InstanceSettings.MaxBrowserFramesPerSecond);

	const int32 TargetHz = FMath::Clamp(
		BrowserFpsSetting > 0 ? BrowserFpsSetting : WindowlessFrameRate,
		1,
		300);

	const double MinInterval = 1.0 / static_cast<double>(TargetHz);

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
		ExternalBeginFrameAccumulatedTime = FMath::Max(
			0.0,
			ExternalBeginFrameAccumulatedTime - MinInterval);

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
		const std::string StdScript = bHasScript
			? std::string(TCHAR_TO_UTF8(*CombinedScript))
			: std::string();

		const bool bInvalidateViewForThisFrame =
			bWillSendBeginFrame && (bForceFrame || bHasScript);

		CefPostTask(
			TID_UI,
			new FSwuiFlushAndBeginFrameTask(
				CefData->Browser,
				StdScript,
				bInvalidateViewForThisFrame,
				bWillSendBeginFrame));
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

void USwuiView::OnPaint(
	const void* Buffer,
	FUpdateTextureRegion2D* Regions,
	int32 RegionCount,
	int32 InWidth,
	int32 InHeight)
{
	GetOrCreateTexture(InWidth, InHeight);

	if (!Texture || !Texture->GetResource())
	{
		FMemory::Free(Regions);
		return;
	}

	if (InstanceSettings.bSkipOnPaintProcessing)
	{
		FMemory::Free(Regions);
		return;
	}

	const double PaintNow = FPlatformTime::Seconds();
	const bool bCanCopy = !InstanceSettings.bSkipPaintMemcpy && !InstanceSettings.bFreezeTexture;

	// Track external begin frame timing before staging to the renderer.
	{
		FScopeLock Lock(&PaintMutex);

		LastPaintArrivalTime = PaintNow;
		PendingFreshPaintArrivalTime = PaintNow;

		if (PendingBeginFrameSentTime > 0.0)
		{
			const double PaintAfterBeginMs = (PaintNow - PendingBeginFrameSentTime) * 1000.0;

			Stat_PaintAfterBeginFrameMsSum += PaintAfterBeginMs;
			Stat_PaintAfterBeginFrameMsMax = FMath::Max(
				Stat_PaintAfterBeginFrameMsMax,
				PaintAfterBeginMs);
			++Stat_PaintAfterBeginFrameSamples;

			bPaintArrivedAfterExternalBeginFrame = true;

			if (bPendingInvalidateForPaint)
			{
				++Stat_PaintsAfterInvalidate;
				bPendingInvalidateForPaint = false;
			}

			PendingBeginFrameSentTime = -1.0;
		}

		bHasPendingFullSurfacePaint = true;
	}

	// Stage the frame: either ROI direct path or full-surface pool.
	if (bCanCopy)
	{
		const TArray<FIntRect> RoiRects = BuildActiveHudRoiRects();
		if (!RoiRects.IsEmpty())
		{
			// ROI mode: copy only ROI rects directly from CEF buffer.
			FullSurfaceRenderer.StageRoiPaint(Buffer, InWidth, InHeight, RoiRects, PaintNow);
		}
		else
		{
			// Full-surface mode: copy full CEF buffer into pool.
			FullSurfaceRenderer.StagePaint(Buffer, InWidth, InHeight, PaintNow);
		}
	}

	FMemory::Free(Regions);
}

void USwuiView::TickDeferredUpload()
{
	const double Now = FPlatformTime::Seconds();
	++Stat_ViewUploadTicks;

	const bool bDebugForceEveryTick = IsForceFullFrameMode();
	DriveContinuousBrowserFrame(Now, bDebugForceEveryTick);

	if (!Texture || !Texture->GetResource())
	{
		LogFullSurfaceStatsIfNeeded(Now);
		return;
	}

	const bool bSkipUpload =
		CVarSwuiNoTextureUpload.GetValueOnAnyThread() != 0 ||
		InstanceSettings.bSkipTextureUpload ||
		InstanceSettings.bNoTextureUpload;

	if (bSkipUpload)
	{
		LogFullSurfaceStatsIfNeeded(Now);
		return;
	}

	const TArray<FIntRect> ActiveRects = BuildActiveHudRoiRects();
	const bool bUseRoiMode = !ActiveRects.IsEmpty();

	if (bUseRoiMode)
	{
		if (FullSurfaceSafetyFrames > 0)
		{
			--FullSurfaceSafetyFrames;
		}

		FSwuiRoiPayload Payload;

		if (!FullSurfaceRenderer.ConsumeLatestRoiPayload(Payload) || Payload.Regions.IsEmpty() || Payload.Pixels.IsEmpty())
		{
			++Stat_RoiSkipsNoFreshFrame;
		}
		else
		{
			FTextureResource* TexRes = static_cast<FTextureResource*>(Texture->GetResource());

			const int32 RoiRectCount = Payload.Regions.Num();
			int64 RoiPx = 0;
			for (const FUpdateTextureRegion2D& Rgn : Payload.Regions)
			{
				RoiPx += int64(Rgn.Width) * Rgn.Height;
			}

			const double EnqueueStart = FPlatformTime::Seconds();

			ENQUEUE_RENDER_COMMAND(SwuiRoiUpload)(
				[TexRes, Payload = MoveTemp(Payload)](FRHICommandList& RHICmdList) mutable
				{
					FRHITexture* Tex = TexRes ? TexRes->TextureRHI.GetReference() : nullptr;
					if (!Tex)
					{
						return;
					}

					int32 DataOffset = 0;
					for (const FUpdateTextureRegion2D& Region : Payload.Regions)
					{
						const int32 RectPitch = Region.Width * 4;
						const int32 RectBytes = RectPitch * Region.Height;
						RHIUpdateTexture2D(Tex, 0, Region, RectPitch,
							Payload.Pixels.GetData() + DataOffset);
						DataOffset += RectBytes;
					}
				});

			const double EnqueueMs = (FPlatformTime::Seconds() - EnqueueStart) * 1000.0;

			++Stat_RoiUploads;
			Stat_RoiUploadRects += RoiRectCount;
			Stat_RoiUploadedPx  += RoiPx;
			Stat_RoiEnqueueSamples++;
			Stat_RoiEnqueueMsSum += EnqueueMs;
			if (EnqueueMs > Stat_RoiEnqueueMsMax)
				Stat_RoiEnqueueMsMax = EnqueueMs;
		}
	}
	else
	{
		if (FullSurfaceSafetyFrames > 0)
		{
			--FullSurfaceSafetyFrames;
		}

		FullSurfaceRenderer.TickUpload(
			static_cast<FTextureResource*>(Texture->GetResource()),
			Now,
			bDebugForceEveryTick);
	}

	LogFullSurfaceStatsIfNeeded(Now);
}

void USwuiView::DriveContinuousBrowserFrame(double Now, bool bDebugForceEveryTick)
{
	const double DeltaSeconds = LastBrowserFrameTime > 0.0
		? FMath::Max(0.0, Now - LastBrowserFrameTime)
		: 1.0 / 60.0;

	if (bDebugForceEveryTick)
	{
		FlushHudStateAndRequestBrowserFrame(FString(), static_cast<float>(DeltaSeconds), true);
		LastBrowserFrameTime = Now;
		TargetFpsForLog = WindowlessFrameRate;
		return;
	}

	const int32 BrowserFpsSetting = SwuiCVarInt(
		CVarSwuiHudMaxBrowserFPS.GetValueOnGameThread(),
		InstanceSettings.MaxBrowserFramesPerSecond);

	const int32 TargetHz = FMath::Clamp(
		BrowserFpsSetting > 0 ? BrowserFpsSetting : WindowlessFrameRate,
		1,
		300);

	TargetFpsForLog = TargetHz;

	const double MinInterval = 1.0 / static_cast<double>(TargetHz);

	if (LastBrowserFrameTime <= 0.0 || (Now - LastBrowserFrameTime) >= MinInterval)
	{
		FlushHudStateAndRequestBrowserFrame(FString(), static_cast<float>(DeltaSeconds), false);
		LastBrowserFrameTime = Now;
	}
}

void USwuiView::PumpBrowserFrameIfDue(double Now, bool bForceFrame)
{
	DriveContinuousBrowserFrame(Now, bForceFrame);
}

// ── HUD ROI ───────────────────────────────────────────────────────────────

void USwuiView::UpdateHudRoiSettings(const FSwuiHudRoiSettings& NewSettings)
{
	FSwuiHudRoiSettings Resolved = NewSettings;

	const int32 OverrideEnabled = CVarSwuiHudRoiEnabled.GetValueOnGameThread();
	if (OverrideEnabled >= 0)
	{
		Resolved.bEnabled = OverrideEnabled != 0;
	}

	const int32 OverrideCenter = CVarSwuiHudRoiCenterEnabled.GetValueOnGameThread();
	if (OverrideCenter >= 0)
	{
		Resolved.bCenterRoiEnabled = OverrideCenter != 0;
	}

	const int32 OverrideOverlay = CVarSwuiHudRoiOverlay.GetValueOnGameThread();
	if (OverrideOverlay >= 0)
	{
		Resolved.bShowOverlay = OverrideOverlay != 0;
	}

	const int32 OverrideShade = CVarSwuiHudRoiShadeInactive.GetValueOnGameThread();
	if (OverrideShade >= 0)
	{
		Resolved.bShadeInactiveArea = OverrideShade != 0;
	}

	HudRoiSettings = Resolved;
}

void USwuiView::SetMenuInputActive(bool bActive)
{
	bMenuInputActive = bActive;

	if (bActive)
	{
		// When opening a menu, ensure we use full-surface updates immediately.
		FullSurfaceSafetyFrames = 0;
		RequestBrowserVisualRefresh(true);
	}
	else
	{
		// When closing a menu, schedule safety frames to clear stale pixels
		// outside ROI before resuming ROI-only mode.
		FullSurfaceSafetyFrames = 2;
		RequestBrowserVisualRefresh(true);
	}
}

void USwuiView::BuildHudRoiRects(TArray<FIntRect>& OutOuter, TArray<FIntRect>& OutCenter) const
{
	OutOuter.Reset();
	OutCenter.Reset();

	const int32 FullW = Width;
	const int32 FullH = Height;

	float TopF = 0.0f, BottomF = 0.0f, LeftF = 0.0f, RightF = 0.0f;

	if (HudRoiSettings.Mode == ESwuiHudRoiMode::UniformEdges)
	{
		TopF = BottomF = LeftF = RightF = FMath::Clamp(HudRoiSettings.UniformEdgePercent / 100.0f, 0.0f, 1.0f);
	}
	else
	{
		TopF    = FMath::Clamp(HudRoiSettings.TopPercent    / 100.0f, 0.0f, 1.0f);
		BottomF = FMath::Clamp(HudRoiSettings.BottomPercent / 100.0f, 0.0f, 1.0f);
		LeftF   = FMath::Clamp(HudRoiSettings.LeftPercent   / 100.0f, 0.0f, 1.0f);
		RightF  = FMath::Clamp(HudRoiSettings.RightPercent  / 100.0f, 0.0f, 1.0f);
	}

	const int32 TopPx    = FMath::RoundToInt(FullH * TopF);
	const int32 BottomPx = FMath::RoundToInt(FullH * BottomF);
	const int32 LeftPx   = FMath::RoundToInt(FullW * LeftF);
	const int32 RightPx  = FMath::RoundToInt(FullW * RightF);

	const int32 InnerTop = FMath::Min(TopPx, FullH);
	const int32 InnerBot = FMath::Max(FullH - BottomPx, InnerTop);

	OutOuter.Add(FIntRect(0, 0, FullW, InnerTop));                                     // top
	OutOuter.Add(FIntRect(0, InnerBot, FullW, FullH));                                  // bottom
	OutOuter.Add(FIntRect(0, InnerTop, FMath::Min(LeftPx, FullW), InnerBot));           // left
	OutOuter.Add(FIntRect(FMath::Max(FullW - RightPx, LeftPx), InnerTop, FullW, InnerBot)); // right

	if (HudRoiSettings.bCenterRoiEnabled && HudRoiSettings.CenterRoiPercent > 0)
	{
		const float CenterCoverage = FMath::Clamp(HudRoiSettings.CenterRoiPercent / 100.0f, 0.0f, 1.0f);
		const int32 Side = FMath::RoundToInt(FMath::Sqrt(CenterCoverage) * FMath::Min(FullW, FullH));
		const int32 H = Side / 2;
		FIntRect Center(FullW / 2 - H, FullH / 2 - H, FullW / 2 + H, FullH / 2 + H);
		Center.Clip(FIntRect(0, 0, FullW, FullH));
		if (Center.Area() > 0)
		{
			OutCenter.Add(Center);
		}
	}
}

TArray<FIntRect> USwuiView::BuildActiveHudRoiRects() const
{
	TArray<FIntRect> Rects;

	if (!HudRoiSettings.bEnabled)
	{
		return Rects;
	}

	if (bMenuInputActive)
	{
		return Rects;
	}

	if (FullSurfaceSafetyFrames > 0)
	{
		return Rects;
	}

	if (IsForceFullFrameMode())
	{
		return Rects;
	}

	TArray<FIntRect> OuterRects, CenterRects;
	BuildHudRoiRects(OuterRects, CenterRects);

	Rects.Append(OuterRects);
	Rects.Append(CenterRects);

	if (Rects.IsEmpty())
	{
		UE_LOG(LogSwuiRuntime, Warning,
			TEXT("[SwuiFullSurface] ROI enabled but all rects empty or invalid. Falling back to full surface."));
	}

	return Rects;
}

FSwuiHudRoiOverlayState USwuiView::GetHudRoiOverlayState() const
{
	FSwuiHudRoiOverlayState State;

	State.bVisible = HudRoiSettings.bShowOverlay;

	if (!HudRoiSettings.bEnabled)
	{
		State.bHudRoiModeActive = false;
		State.ModeLabel = TEXT("ROI Disabled");
		return State;
	}

	if (bMenuInputActive)
	{
		State.ModeLabel = TEXT("Menu Input Active");
		return State;
	}

	if (FullSurfaceSafetyFrames > 0)
	{
		State.ModeLabel = TEXT("Safety Frames");
		return State;
	}

	if (IsForceFullFrameMode())
	{
		State.ModeLabel = TEXT("Debug Force");
		return State;
	}

	TArray<FIntRect> OuterRects, CenterRects;
	BuildHudRoiRects(OuterRects, CenterRects);

	State.OuterRoiRects   = OuterRects;
	State.CenterRoiRects  = CenterRects;
	State.ActiveRoiRects  = OuterRects;
	State.ActiveRoiRects.Append(CenterRects);

	State.bHudRoiModeActive = !State.ActiveRoiRects.IsEmpty();

	if (State.bHudRoiModeActive)
	{
		State.ModeLabel = TEXT("Active");
	}

	const int32 TexArea = FMath::Max(1, Width * Height);

	int64 TotalArea = 0;
	for (const FIntRect& Rect : State.ActiveRoiRects)
	{
		TotalArea += Rect.Area();
	}

	State.RoiAreaPercent = FMath::Clamp(
		(float(TotalArea) / float(TexArea)) * 100.0f,
		0.0f,
		100.0f);

	return State;
}

void USwuiView::InvalidateBrowserView()
{
	if (!CefData || !CefData->Browser)
	{
		return;
	}

	CefRefPtr<CefBrowserHost> Host = CefData->Browser->GetHost();
	if (!Host)
	{
		return;
	}

	Host->Invalidate(PET_VIEW);
}

void USwuiView::LogFullSurfaceStatsIfNeeded(double Now)
{
	const bool bStatsEnabled =
		IsForceFullFrameMode() ||
		SwuiCVarBool(CVarSwuiDebugLogPaintStats.GetValueOnGameThread(), InstanceSettings.bLogSwuiPaintStats) ||
		(CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0) ||
		InstanceSettings.bVerbosePaintLog;

	if (!bStatsEnabled)
	{
		return;
	}

	if ((Now - Stat_LastLogTime) < 1.0)
	{
		return;
	}

	// Refresh pool snapshot and read stats from the renderer.
	FullSurfaceRenderer.RefreshPoolSnapshot();
	const FSwuiFullSurfaceCpuRenderer::FStats& R = FullSurfaceRenderer.GetStats();

	const float PaintCopyAvgMs   = R.StatInterval_PaintCopySamples > 0
		? static_cast<float>(R.StatInterval_PaintCopyMsSum / R.StatInterval_PaintCopySamples)
		: 0.f;
	const float EnqueueAvgMs     = R.StatInterval_EnqueueSamples > 0
		? static_cast<float>(R.StatInterval_EnqueueMsSum / R.StatInterval_EnqueueSamples)
		: 0.f;
	const float PaintToUploadAvgMs = R.StatInterval_PaintToUploadSamples > 0
		? static_cast<float>(R.StatInterval_PaintToUploadMsSum / R.StatInterval_PaintToUploadSamples)
		: 0.f;
	const float PaintAfterBeginFrameAvgMs = Stat_PaintAfterBeginFrameSamples > 0
		? static_cast<float>(Stat_PaintAfterBeginFrameMsSum / Stat_PaintAfterBeginFrameSamples)
		: 0.f;

	const TCHAR* PresetName = SwuiUiResolutionPresetName(InstanceSettings.UiResolutionPreset);

	// Sanity checks.
	if (R.StatInterval_Uploads > Stat_ViewUploadTicks + 2)
	{
		UE_LOG(LogSwuiRuntime, Warning,
			TEXT("[SwuiFullSurface] Impossible stats: uploads (%d) exceed upload ticks (%d). Stats reset/wiring bug."),
			R.StatInterval_Uploads, Stat_ViewUploadTicks);
	}

	if (R.StatInterval_Allocations > 0 && Stat_ViewUploadTicks > 10)
	{
		UE_LOG(LogSwuiRuntime, Warning,
			TEXT("[SwuiFullSurface] Unexpected steady-state allocation detected: allocs=%d > 0 after view stabilised."),
			R.StatInterval_Allocations);
	}

	const TArray<FIntRect> CurrentRoiRects = BuildActiveHudRoiRects();
	const bool bRoiActive = !CurrentRoiRects.IsEmpty();
	const TCHAR* RoiModeText = bRoiActive ? TEXT("HudRoi") : TEXT("Full");
	const TCHAR* RoiReasonText = TEXT("N/A");

	if (!HudRoiSettings.bEnabled)
		RoiReasonText = TEXT("Disabled");
	else if (bMenuInputActive)
		RoiReasonText = TEXT("MenuInputActive");
	else if (FullSurfaceSafetyFrames > 0)
		RoiReasonText = TEXT("SafetyFrames");
	else if (IsForceFullFrameMode())
		RoiReasonText = TEXT("DebugForce");
	else if (!bRoiActive)
		RoiReasonText = TEXT("NoValidRoiRects");

	const int32 TexArea = FMath::Max(1, Width * Height);
	int64 RoiTotalPx = 0;
	for (const FIntRect& Rect : CurrentRoiRects)
	{
		RoiTotalPx += Rect.Area();
	}
	const float RoiAreaPct = FMath::Clamp(
		(float(RoiTotalPx) / float(TexArea)) * 100.0f, 0.0f, 100.0f);

	UE_LOG(LogSwuiRuntime, Log,
		TEXT("[SwuiFullSurface] preset=%s targetFps=%d tex=%dx%d")
		TEXT(" roiMode=%s roiReason=%s roiCurrentRects=%d roiAreaPct=%.1f roiOverlay=%d")
		TEXT(" cefPaints/s=%d skippedNoFreshPaint/s=%d droppedPaints/s=%d replacedReady/s=%d")
		TEXT(" pool{free=%d ready=%d inFlight=%d} allocs=%d")
		TEXT(" paintCopyAvgMs=%.3f paintCopyMaxMs=%.3f")
		TEXT(" roiEnqueueAvgMs=%.4f roiEnqueueMaxMs=%.4f roiUploads/s=%d roiUploadRects/s=%d roiUploadedPx/s=%lld roiSkipsNoFresh/s=%d fullFallbacks/s=%d")
		TEXT(" uploads/s=%d uploadedPx/s=%lld enqueueAvgMs=%.3f paintToUploadAvgMs=%.3f"),
		PresetName,
		TargetFpsForLog,
		Texture ? Texture->GetSizeX() : 0,
		Texture ? Texture->GetSizeY() : 0,
		RoiModeText,
		RoiReasonText,
		CurrentRoiRects.Num(),
		RoiAreaPct,
		SwuiCVarBool(CVarSwuiHudRoiOverlay.GetValueOnGameThread(), HudRoiSettings.bShowOverlay) ? 1 : 0,
		R.StatInterval_CefPaints,
		R.StatInterval_SkippedNoFreshPaint,
		R.StatInterval_DroppedPaints,
		R.StatInterval_ReplacedReadyFrames,
		R.PoolFree,
		R.PoolReady,
		R.PoolInFlight,
		R.StatInterval_Allocations,
		PaintCopyAvgMs,
		R.StatInterval_PaintCopyMsMax,
		Stat_RoiEnqueueSamples > 0 ? static_cast<float>(Stat_RoiEnqueueMsSum / Stat_RoiEnqueueSamples) : 0.f,
		Stat_RoiEnqueueMsMax,
		Stat_RoiUploads,
		Stat_RoiUploadRects,
		Stat_RoiUploadedPx,
		Stat_RoiSkipsNoFreshFrame,
		Stat_FullFallbacks,
		R.StatInterval_Uploads,
		R.StatInterval_UploadedPx,
		EnqueueAvgMs,
		PaintToUploadAvgMs);

	ResetFullSurfaceStats();
	Stat_LastLogTime = Now;
}

void USwuiView::ResetFullSurfaceStats()
{
	Stat_SubsystemTicks = 0;
	Stat_ViewUploadTicks = 0;
	Stat_HudStateFlushes = 0;

	Stat_ExternalBeginFrames = 0;
	Stat_ExternalBeginFrameSkipInactive = 0;
	Stat_ExternalBeginFrameSkipDisabled = 0;
	Stat_ExternalBeginFrameSkipNoBrowser = 0;
	Stat_ExternalBeginFrameSkipRateLimited = 0;

	Stat_InvalidateView = 0;
	Stat_BeginFramesWithoutPaint = 0;
	Stat_PaintsAfterInvalidate = 0;

	Stat_PaintAfterBeginFrameMsSum = 0.0;
	Stat_PaintAfterBeginFrameMsMax = 0.0;
	Stat_PaintAfterBeginFrameSamples = 0;

	// ROI stats.
	Stat_RoiUploadRects     = 0;
	Stat_RoiUploadedPx      = 0;
	Stat_RoiUploads         = 0;
	Stat_RoiEnqueueMsSum    = 0.0;
	Stat_RoiEnqueueMsMax    = 0.0;
	Stat_RoiEnqueueSamples  = 0;
	Stat_RoiSkipsNoFreshFrame = 0;
	Stat_FullFallbacks      = 0;

	// Reset renderer interval counters too.
	FullSurfaceRenderer.ResetIntervalStats();
}

UTexture2D* USwuiView::GetOrCreateTexture(int32 InWidth, int32 InHeight)
{
	if (!Texture || Texture->GetSizeX() != InWidth || Texture->GetSizeY() != InHeight)
	{
		DestroyTexture();

		Texture = UTexture2D::CreateTransient(InWidth, InHeight, PF_B8G8R8A8);
		Texture->AddToRoot();
		Texture->UpdateResource();

		{
			FScopeLock Lock(&PaintMutex);
			bHasPendingFullSurfacePaint = false;
			PendingFreshPaintArrivalTime = 0.0;
			LastPaintArrivalTime = 0.0;
		}

		FullSurfaceRenderer.HandleTextureSizeChanged(InWidth, InHeight);

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

	{
		FScopeLock Lock(&PaintMutex);
		bHasPendingFullSurfacePaint = false;
		PendingFreshPaintArrivalTime = 0.0;
		LastPaintArrivalTime = 0.0;
	}

	FullSurfaceRenderer.Reset();
	FullSurfaceRenderer.HandleTextureSizeChanged(Width, Height);

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
		MaterialInstance = UMaterialInstanceDynamic::Create(BaseMaterial, nullptr);
		if (!MaterialInstance)
		{
			return;
		}
	}

	UTexture* ExistingTexture = nullptr;
	if (!MaterialInstance->GetTextureParameterValue(TextureParameterName, ExistingTexture))
	{
		UE_LOG(LogSwuiRuntime, Warning,
			TEXT("USwuiView: Texture parameter '%s' not found in material"),
			*TextureParameterName.ToString());
		return;
	}

	MaterialInstance->SetTextureParameterValue(TextureParameterName, Texture);
}

// ---------------------------------------------------------------------------
// Pointer Input Forwarding
// ---------------------------------------------------------------------------

void USwuiView::SetPointerInputEnabled(bool bEnabled)
{
	bPointerInputEnabled = bEnabled;

	UE_LOG(LogSwuiRuntime, Log,
		TEXT("[SwuiPointer] SetPointerInputEnabled(%s)"),
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
	if (!bPointerInputEnabled)
	{
		return false;
	}

	if (!CefData || !CefData->Browser)
	{
		return false;
	}

	CefRefPtr<CefBrowserHost> Host = CefData->Browser->GetHost();
	if (!Host)
	{
		return false;
	}

	int32 BX = 0;
	int32 BY = 0;
	if (!ScreenToBrowserPixel(ScreenPosition, BX, BY))
	{
		return false;
	}

	CefMouseEvent Event;
	Event.x = BX;
	Event.y = BY;
	Event.modifiers = 0;

	Host->SendMouseMoveEvent(Event, false);

	if (CVarSwuiVerbosePaint.GetValueOnAnyThread() != 0)
	{
		UE_LOG(LogSwuiRuntime, Verbose,
			TEXT("[SwuiPointer] MouseMove: (%.0f, %.0f) -> browser (%d, %d)"),
			ScreenPosition.X,
			ScreenPosition.Y,
			BX,
			BY);
	}

	return true;
}

static cef_mouse_button_type_t SwuiMapKeyToCefButton(FKey Button)
{
	if (Button == EKeys::LeftMouseButton)
	{
		return MBT_LEFT;
	}

	if (Button == EKeys::RightMouseButton)
	{
		return MBT_RIGHT;
	}

	if (Button == EKeys::MiddleMouseButton)
	{
		return MBT_MIDDLE;
	}

	return MBT_LEFT;
}

bool USwuiView::ForwardMouseButtonToBrowser(
	const FVector2D& ScreenPosition,
	FKey Button,
	bool bMouseUp,
	int32 ClickCount)
{
	if (!bPointerInputEnabled)
	{
		return false;
	}

	if (!CefData || !CefData->Browser)
	{
		return false;
	}

	CefRefPtr<CefBrowserHost> Host = CefData->Browser->GetHost();
	if (!Host)
	{
		return false;
	}

	int32 BX = 0;
	int32 BY = 0;
	if (!ScreenToBrowserPixel(ScreenPosition, BX, BY))
	{
		return false;
	}

	const cef_mouse_button_type_t CefButton = SwuiMapKeyToCefButton(Button);

	CefMouseEvent Event;
	Event.x = BX;
	Event.y = BY;
	Event.modifiers = 0;

	Host->SendMouseClickEvent(Event, CefButton, bMouseUp, ClickCount);

	const TCHAR* Action = bMouseUp ? TEXT("Up") : TEXT("Down");

	UE_LOG(LogSwuiRuntime, Log,
		TEXT("[SwuiPointer] MouseButton %s %s  clickCount=%d  (%d, %d)"),
		*Button.ToString(),
		Action,
		ClickCount,
		BX,
		BY);

	return true;
}

bool USwuiView::ForwardMouseWheelToBrowser(
	const FVector2D& ScreenPosition,
	float DeltaX,
	float DeltaY)
{
	if (!bPointerInputEnabled)
	{
		return false;
	}

	if (!CefData || !CefData->Browser)
	{
		return false;
	}

	CefRefPtr<CefBrowserHost> Host = CefData->Browser->GetHost();
	if (!Host)
	{
		return false;
	}

	int32 BX = 0;
	int32 BY = 0;
	if (!ScreenToBrowserPixel(ScreenPosition, BX, BY))
	{
		return false;
	}

	const int32 CefDeltaX = FMath::RoundToInt(DeltaX * 120.0f);
	const int32 CefDeltaY = FMath::RoundToInt(DeltaY * 120.0f);

	CefMouseEvent Event;
	Event.x = BX;
	Event.y = BY;
	Event.modifiers = 0;

	Host->SendMouseWheelEvent(Event, CefDeltaX, CefDeltaY);

	UE_LOG(LogSwuiRuntime, Log,
		TEXT("[SwuiPointer] Wheel: delta=(%d, %d)  (%d, %d)"),
		CefDeltaX,
		CefDeltaY,
		BX,
		BY);

	return true;
}

void USwuiView::SetBrowserInputFocus(bool bFocused)
{
	if (!CefData || !CefData->Browser)
	{
		return;
	}

	CefRefPtr<CefBrowserHost> Host = CefData->Browser->GetHost();
	if (Host)
	{
		Host->SetFocus(bFocused);

		UE_LOG(LogSwuiRuntime, Log,
			TEXT("[SwuiPointer] BrowserHost->SetFocus(%s)"),
			bFocused ? TEXT("true") : TEXT("false"));
	}
}

void USwuiView::BeginDestroy()
{
	if (CefData && CefData->Browser)
	{
		CefData->Browser->GetHost()->CloseBrowser(true);
	}

	FullSurfaceRenderer.Reset();
	DestroyTexture();

	Super::BeginDestroy();
}