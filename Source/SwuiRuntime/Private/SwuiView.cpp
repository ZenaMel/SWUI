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
		PendingFreshPaintGeneration = 0;
		UploadedFreshPaintGeneration = 0;
		PendingFreshPaintArrivalTime = 0.0;
		LastPaintArrivalTime = 0.0;
	}

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

	return bHasPendingFullSurfacePaint &&
		PendingFreshPaintGeneration > UploadedFreshPaintGeneration;
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
	// When bHasScript is true we never coalesce — the script is piggybacked
	// on the begin frame to keep JS state updates responsive.
	{
		FScopeLock Lock(&PaintMutex);
		if (!bForceFrame && !bHasScript &&
			PendingBeginFrameSentTime > 0.0 &&
			!bPaintArrivedAfterExternalBeginFrame)
		{
			const double Now = FPlatformTime::Seconds();
			if ((Now - PendingBeginFrameSentTime) < BrowserFrameTimeout)
			{
				++Stat_ExternalBeginFrameCoalescedPending;
				// No script to execute (bHasScript was false), nothing to post.
				return false;
			}
			else
			{
				// Pending begin frame timed out — let this one through.
				++Stat_ExternalBeginFrameCoalescedTimeout;
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
		if (bForceFrame) ++Stat_ExternalBeginFrameForced;
		else ++Stat_ExternalBeginFrameNonForced;
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

	StageFullSurfacePaint(Buffer, InWidth, InHeight, FPlatformTime::Seconds());

	FMemory::Free(Regions);
}

void USwuiView::StageFullSurfacePaint(
	const void* Buffer,
	int32 InWidth,
	int32 InHeight,
	double PaintNow)
{
	const int32 FullPitch = InWidth * 4;
	const int32 BufBytes = FullPitch * InHeight;

	const double CopyStart = FPlatformTime::Seconds();

	FScopeLock Lock(&PaintMutex);

	LastPaintArrivalTime = PaintNow;
	PendingFreshPaintArrivalTime = PaintNow;
	++PendingFreshPaintGeneration;

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

	++Stat_FullSurfaceCefPaints;

	const double CopyMs = (FPlatformTime::Seconds() - CopyStart) * 1000.0;

	Stat_FullSurfacePaintCopyMsSum += CopyMs;
	Stat_FullSurfacePaintCopyMsMax = FMath::Max(Stat_FullSurfacePaintCopyMsMax, CopyMs);
	++Stat_FullSurfacePaintCopySamples;
}

void USwuiView::TickDeferredUpload()
{
	const double Now = FPlatformTime::Seconds();
	++Stat_ViewUploadTicks;

	TickFullSurfaceUpload(Now);
}

void USwuiView::TickFullSurfaceUpload(double Now)
{
	++Stat_FullSurfaceUploadTicks;

	const bool bDebugForceEveryTick = IsForceFullFrameMode();

	if (bDebugForceEveryTick)
	{
		if (!bFullSurfaceFirstEntryLogged)
		{
			bFullSurfaceFirstEntryLogged = true;

			UE_LOG(LogSwuiRuntime, Log,
				TEXT("[SwuiFullSurface] Force-every-tick mode ACTIVE — uploading full texture every tick, even without fresh CEF paint."));
		}
	}
	else
	{
		bFullSurfaceFirstEntryLogged = false;
	}

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

	if (!bDebugForceEveryTick && !HasFreshOnPaintDataPending())
	{
		++Stat_FullSurfaceSkippedNoFreshPaint;
		LogFullSurfaceStatsIfNeeded(Now);
		return;
	}

	UploadLatestFullSurface(bDebugForceEveryTick);
	LogFullSurfaceStatsIfNeeded(Now);
}

void USwuiView::UploadLatestFullSurface(bool bForceMemcpy)
{
	const int32 SnapW = Texture->GetSizeX();
	const int32 SnapH = Texture->GetSizeY();
	const int32 FullPitch = SnapW * 4;
	const int32 BufBytes = FullPitch * SnapH;

	bool bShouldUpload = false;
	uint64 LocalPaintGeneration = 0;
	double LocalPaintArrivalTime = 0.0;

	const double LockWaitStart = FPlatformTime::Seconds();

	FSwuiPaintUploadData* UploadData = nullptr;

	{
		FScopeLock Lock(&PaintMutex);

		Stat_FullSurfaceLockWaitMs += (FPlatformTime::Seconds() - LockWaitStart) * 1000.0;

		const bool bHasFreshPaint = HasFreshOnPaintDataPending();

		bShouldUpload = bForceMemcpy || bHasFreshPaint;

		if (!bShouldUpload)
		{
			++Stat_FullSurfaceSkippedNoFreshPaint;
		}
		else if (BackingBuffer.Num() == BufBytes)
		{
			UploadData = new FSwuiPaintUploadData;
			UploadData->Texture2DResource = static_cast<FTextureResource*>(Texture->GetResource());
			UploadData->PackedPixels.SetNumUninitialized(BufBytes);
			UploadData->Rects.SetNum(1);

			const double CopyStart = FPlatformTime::Seconds();

			if (bHasFreshPaint && !bForceMemcpy)
			{
				::Swap(UploadData->PackedPixels, BackingBuffer);
			}
			else
			{
				FPlatformMemory::Memcpy(
					UploadData->PackedPixels.GetData(),
					BackingBuffer.GetData(),
					BufBytes);
			}

			const double CopyMs = (FPlatformTime::Seconds() - CopyStart) * 1000.0;

			Stat_FullSurfaceUploadCopyMsSum += CopyMs;
			Stat_FullSurfaceUploadCopyMsMax = FMath::Max(Stat_FullSurfaceUploadCopyMsMax, CopyMs);
			++Stat_FullSurfaceUploadCopySamples;

			LocalPaintGeneration = PendingFreshPaintGeneration;
			LocalPaintArrivalTime = PendingFreshPaintArrivalTime;

			if (bHasFreshPaint)
			{
				bHasPendingFullSurfacePaint = false;
				UploadedFreshPaintGeneration = PendingFreshPaintGeneration;
			}
		}
	}

	if (!UploadData)
	{
		return;
	}

	FSwuiPackedRectDesc& Desc = UploadData->Rects[0];
	Desc.Region = FUpdateTextureRegion2D(0, 0, 0, 0, SnapW, SnapH);
	Desc.SrcPitch = static_cast<uint32>(FullPitch);
	Desc.SrcOffsetBytes = 0;

	const double EnqueueStart = FPlatformTime::Seconds();

	ENQUEUE_RENDER_COMMAND(UpdateSwuiViewFullSurface)(
		[UploadData](FRHICommandList& RHICmdList)
		{
			FRHITexture* Tex = UploadData->Texture2DResource
				? UploadData->Texture2DResource->TextureRHI.GetReference()
				: nullptr;

			if (Tex)
			{
				const FSwuiPackedRectDesc& Rd = UploadData->Rects[0];

				RHIUpdateTexture2D(
					Tex,
					0,
					Rd.Region,
					Rd.SrcPitch,
					UploadData->PackedPixels.GetData());
			}

			delete UploadData;
		});

	const double EnqueueMs = (FPlatformTime::Seconds() - EnqueueStart) * 1000.0;

	Stat_FullSurfaceEnqueueMsSum += EnqueueMs;
	Stat_FullSurfaceEnqueueMsMax = FMath::Max(Stat_FullSurfaceEnqueueMsMax, EnqueueMs);
	++Stat_FullSurfaceEnqueueSamples;

	++Stat_FullSurfaceUploads;
	Stat_FullSurfaceUploadedPx += int64(SnapW) * SnapH;

	if (LocalPaintGeneration > 0 && LocalPaintArrivalTime > 0.0)
	{
		const double PaintToUploadMs = (FPlatformTime::Seconds() - LocalPaintArrivalTime) * 1000.0;

		Stat_FullSurfacePaintToUploadMsSum += PaintToUploadMs;
		Stat_FullSurfacePaintToUploadMsMax = FMath::Max(
			Stat_FullSurfacePaintToUploadMsMax,
			PaintToUploadMs);
		++Stat_FullSurfacePaintToUploadSamples;
	}
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

	const float PaintCopyAvgMs = Stat_FullSurfacePaintCopySamples > 0
		? static_cast<float>(Stat_FullSurfacePaintCopyMsSum / Stat_FullSurfacePaintCopySamples)
		: 0.f;

	const float UploadCopyAvgMs = Stat_FullSurfaceUploadCopySamples > 0
		? static_cast<float>(Stat_FullSurfaceUploadCopyMsSum / Stat_FullSurfaceUploadCopySamples)
		: 0.f;

	const float EnqueueAvgMs = Stat_FullSurfaceEnqueueSamples > 0
		? static_cast<float>(Stat_FullSurfaceEnqueueMsSum / Stat_FullSurfaceEnqueueSamples)
		: 0.f;

	const float PaintToUploadAvgMs = Stat_FullSurfacePaintToUploadSamples > 0
		? static_cast<float>(Stat_FullSurfacePaintToUploadMsSum / Stat_FullSurfacePaintToUploadSamples)
		: 0.f;

	const float PaintAfterBeginFrameAvgMs = Stat_PaintAfterBeginFrameSamples > 0
		? static_cast<float>(Stat_PaintAfterBeginFrameMsSum / Stat_PaintAfterBeginFrameSamples)
		: 0.f;

	const TCHAR* PresetName = SwuiUiResolutionPresetName(InstanceSettings.UiResolutionPreset);

	UE_LOG(LogSwuiRuntime, Log,
		TEXT("[SwuiFullSurface] preset=%s subsystemTicks/s=%d viewUploadTicks/s=%d uploadTicks/s=%d")
		TEXT(" cefPaints/s=%d uploads/s=%d skippedNoFreshPaint/s=%d uploadedPx/s=%lld")
		TEXT(" externalBeginFrames/s=%d[forced=%d nonForced=%d] invalidateView/s=%d beginFramesWithoutPaint/s=%d paintsAfterInvalidate/s=%d")
		TEXT(" extBeginSkip[inactive=%d disabled=%d noBrowser=%d rateLimited=%d]")
		TEXT(" extBeginCoalesce[pending=%d timeout=%d]")
		TEXT(" paintCopyAvgMs=%.3f paintCopyMaxMs=%.3f")
		TEXT(" uploadCopyAvgMs=%.3f uploadCopyMaxMs=%.3f")
		TEXT(" enqueueAvgMs=%.3f enqueueMaxMs=%.3f")
		TEXT(" lockWaitMs=%.3f")
		TEXT(" paintAfterBeginFrameAvgMs=%.3f paintAfterBeginFrameMaxMs=%.3f")
		TEXT(" paintToUploadAvgMs=%.3f paintToUploadMaxMs=%.3f")
		TEXT(" targetFps=%d browserFpsCap=%d tex=%dx%d forceEveryTick=%d"),
		PresetName,
		Stat_SubsystemTicks,
		Stat_ViewUploadTicks,
		Stat_FullSurfaceUploadTicks,
		Stat_FullSurfaceCefPaints,
		Stat_FullSurfaceUploads,
		Stat_FullSurfaceSkippedNoFreshPaint,
		Stat_FullSurfaceUploadedPx,
		Stat_ExternalBeginFrames,
		Stat_ExternalBeginFrameForced,
		Stat_ExternalBeginFrameNonForced,
		Stat_InvalidateView,
		Stat_BeginFramesWithoutPaint,
		Stat_PaintsAfterInvalidate,
		Stat_ExternalBeginFrameSkipInactive,
		Stat_ExternalBeginFrameSkipDisabled,
		Stat_ExternalBeginFrameSkipNoBrowser,
		Stat_ExternalBeginFrameSkipRateLimited,
		Stat_ExternalBeginFrameCoalescedPending,
		Stat_ExternalBeginFrameCoalescedTimeout,
		PaintCopyAvgMs,
		Stat_FullSurfacePaintCopyMsMax,
		UploadCopyAvgMs,
		Stat_FullSurfaceUploadCopyMsMax,
		EnqueueAvgMs,
		Stat_FullSurfaceEnqueueMsMax,
		Stat_FullSurfaceLockWaitMs,
		PaintAfterBeginFrameAvgMs,
		Stat_PaintAfterBeginFrameMsMax,
		PaintToUploadAvgMs,
		Stat_FullSurfacePaintToUploadMsMax,
		TargetFpsForLog,
		WindowlessFrameRate,
		Texture ? Texture->GetSizeX() : 0,
		Texture ? Texture->GetSizeY() : 0,
		IsForceFullFrameMode() ? 1 : 0);

	ResetFullSurfaceStats();
	Stat_LastLogTime = Now;
}

void USwuiView::ResetFullSurfaceStats()
{
	Stat_SubsystemTicks = 0;
	Stat_ViewUploadTicks = 0;
	Stat_HudStateFlushes = 0;

	Stat_ExternalBeginFrames = 0;
	Stat_ExternalBeginFrameForced = 0;
	Stat_ExternalBeginFrameNonForced = 0;
	Stat_ExternalBeginFrameCoalescedPending = 0;
	Stat_ExternalBeginFrameCoalescedTimeout = 0;
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

	Stat_FullSurfaceUploadTicks = 0;
	Stat_FullSurfaceUploads = 0;
	Stat_FullSurfaceSkippedNoFreshPaint = 0;
	Stat_FullSurfaceCefPaints = 0;
	Stat_FullSurfaceUploadedPx = 0;

	Stat_FullSurfacePaintCopyMsSum = 0.0;
	Stat_FullSurfacePaintCopyMsMax = 0.0;
	Stat_FullSurfacePaintCopySamples = 0;

	Stat_FullSurfaceUploadCopyMsSum = 0.0;
	Stat_FullSurfaceUploadCopyMsMax = 0.0;
	Stat_FullSurfaceUploadCopySamples = 0;

	Stat_FullSurfaceEnqueueMsSum = 0.0;
	Stat_FullSurfaceEnqueueMsMax = 0.0;
	Stat_FullSurfaceEnqueueSamples = 0;

	Stat_FullSurfaceLockWaitMs = 0.0;

	Stat_FullSurfacePaintToUploadMsSum = 0.0;
	Stat_FullSurfacePaintToUploadMsMax = 0.0;
	Stat_FullSurfacePaintToUploadSamples = 0;
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
			PendingFullSurfaceWidth = InWidth;
			PendingFullSurfaceHeight = InHeight;
			PendingFreshPaintGeneration = 0;
			UploadedFreshPaintGeneration = 0;
			PendingFreshPaintArrivalTime = 0.0;
			LastPaintArrivalTime = 0.0;
			BackingBuffer.Reset();
		}

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
		PendingFullSurfaceWidth = Width;
		PendingFullSurfaceHeight = Height;
		PendingFreshPaintGeneration = 0;
		UploadedFreshPaintGeneration = 0;
		PendingFreshPaintArrivalTime = 0.0;
		LastPaintArrivalTime = 0.0;
		BackingBuffer.Reset();
	}

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

	DestroyTexture();

	Super::BeginDestroy();
}