#include "SwuiSubsystem.h"
#include "SwuiManager.h"
#include "SwuiSettings.h"
#include "Swui.h"
#include "SwuiView.h"
#include "SwuiInputPreprocessor.h"
#include "SwuiCVars.h"
#include "SwuiCVarHelpers.h"
#include "ISwuiRuntime.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/GameUserSettings.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Image.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UnrealType.h"
#include "UObject/Field.h"
#include "UObject/PropertyIterator.h"

// ---- Helpers ----

static FString Swui_GetTSType(const FProperty* Prop)
{
	if (Prop->IsA<FFloatProperty>()  || Prop->IsA<FDoubleProperty>() ||
		Prop->IsA<FIntProperty>()    || Prop->IsA<FInt64Property>()  ||
		Prop->IsA<FByteProperty>())    return TEXT("number");
	if (Prop->IsA<FBoolProperty>())    return TEXT("boolean");
	if (Prop->IsA<FStrProperty>()   || Prop->IsA<FNameProperty>() ||
		Prop->IsA<FTextProperty>())    return TEXT("string");
	return FString();
}

static FString Swui_SerializeProperty(FProperty* Prop, void* Container)
{
	void* Value = Prop->ContainerPtrToValuePtr<void>(Container);
	if (const FFloatProperty*  P = CastField<FFloatProperty>(Prop))  return FString::SanitizeFloat(P->GetPropertyValue(Value));
	if (const FDoubleProperty* P = CastField<FDoubleProperty>(Prop)) return FString::SanitizeFloat(P->GetPropertyValue(Value));
	if (const FIntProperty*    P = CastField<FIntProperty>(Prop))    return FString::FromInt(P->GetPropertyValue(Value));
	if (const FInt64Property*  P = CastField<FInt64Property>(Prop))  return FString::Printf(TEXT("%lld"), P->GetPropertyValue(Value));
	if (const FByteProperty*   P = CastField<FByteProperty>(Prop))   return FString::FromInt(P->GetPropertyValue(Value));
	if (const FBoolProperty*   P = CastField<FBoolProperty>(Prop))   return P->GetPropertyValue(Value) ? TEXT("true") : TEXT("false");
	if (const FStrProperty* P = CastField<FStrProperty>(Prop))
	{
		FString S = P->GetPropertyValue(Value);
		S.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		S.ReplaceInline(TEXT("'"), TEXT("\\'"));
		return FString::Printf(TEXT("'%s'"), *S);
	}
	if (const FNameProperty* P = CastField<FNameProperty>(Prop)) return FString::Printf(TEXT("'%s'"), *P->GetPropertyValue(Value).ToString());
	if (const FTextProperty* P = CastField<FTextProperty>(Prop)) return FString::Printf(TEXT("'%s'"), *P->GetPropertyValue(Value).ToString());
	return FString();
}

static int32 SwuiGetIntCVar(const TCHAR* Name, int32 DefaultValue = -1)
{
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
	{
		return CVar->GetInt();
	}
	return DefaultValue;
}

// ---- Lifecycle ----

bool USwuiSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const USwuiSettings* Settings = GetDefault<USwuiSettings>();
	return Settings && !Settings->bDisablePlugin;
}

void USwuiSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	InputPreprocessor = MakeShared<FSwuiInputPreprocessor>(this);
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().RegisterInputPreProcessor(InputPreprocessor);
		UE_LOG(LogSwuiRuntime, Log, TEXT("[SwuiPreprocessor] Registered Slate input preprocessor."));
	}
}

void USwuiSubsystem::Deinitialize()
{
	if (FSlateApplication::IsInitialized() && InputPreprocessor.IsValid())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputPreprocessor);
		UE_LOG(LogSwuiRuntime, Log, TEXT("[SwuiPreprocessor] Unregistered Slate input preprocessor."));
	}
	InputPreprocessor.Reset();

	ShutdownRenderer();
	ObservedProperties.Empty();
	ObservedDelegates.Empty();
	Super::Deinitialize();
}

// ---- Renderer ----

void USwuiSubsystem::DisablePlugin()
{
	bDisabledAtRuntime = true;
	ShutdownRenderer();
}

void USwuiSubsystem::InitRenderer(const FString& URI, const FString& InterfaceName,
	AActor* OwnerActor, bool bIsHUD, int32 Width, int32 Height, int32 ZOrder,
	UMaterialInterface* BaseMaterial, FName TextureParamName,
	const FSwuiInstanceSettings& InstanceSettings)
{
	if (bDisabledAtRuntime) return;

	UWorld* World = GetGameInstance()->GetWorld();
	if (!World) return;

	int32 FinalWidth = Width, FinalHeight = Height;
	if (bIsHUD)
	{
		// 1. Try the live viewport (valid once the first frame has rendered)
		if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
		{
			FIntPoint RenderSize = GEngine->GameViewport->Viewport->GetSizeXY();
			if (RenderSize.X > 0 && RenderSize.Y > 0)
			{
				FinalWidth  = RenderSize.X;
				FinalHeight = RenderSize.Y;
			}
		}
		// 2. Viewport not ready yet (BeginPlay before first render) — use the
		// player-configured game resolution, NOT the physical monitor resolution.
		// FDisplayMetrics returns the desktop native res which can be 4K+ even
		// when the game runs at a lower resolution — that causes massive OnPaint copies.
		if (FinalWidth <= 0 || FinalHeight <= 0 || (FinalWidth == 1280 && FinalHeight == 720))
		{
			if (UGameUserSettings* GUS = UGameUserSettings::GetGameUserSettings())
			{
				const FIntPoint GameRes = GUS->GetScreenResolution();
				if (GameRes.X > 0 && GameRes.Y > 0)
				{
					FinalWidth  = GameRes.X;
					FinalHeight = GameRes.Y;
				}
			}
		}
	}

	View = NewObject<USwuiView>(this);
	View->DefaultURL    = URI;
	View->Width         = FinalWidth;
	View->Height        = FinalHeight;
	View->bIsTransparent = true;
	View->BaseMaterial  = BaseMaterial;
	View->TextureParameterName = TextureParamName;
	View->SetOwningActor(OwnerActor);
	View->Init(InstanceSettings);

	Widget = CreateWidget<UUserWidget>(World, USwuiWidget::StaticClass());
	if (!Widget) return;

	UCanvasPanel* RootPanel = NewObject<UCanvasPanel>(Widget);
	RootPanel->bIsVariable = false;

	UImage* Image = NewObject<UImage>(Widget);
	Image->SetBrushFromTexture(View->GetTexture());
	FSlateBrush Brush = Image->GetBrush();
	Brush.ImageSize = FVector2D(FinalWidth, FinalHeight);
	Brush.DrawAs    = ESlateBrushDrawType::Image;
	Image->SetBrush(Brush);
	Image->SynchronizeProperties();

	UPanelSlot* Slot = RootPanel->AddChild(Image);
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
	{
		if (bIsHUD)
		{
			CanvasSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
			CanvasSlot->SetOffsets(FMargin(0.f));
		}
		else
		{
			CanvasSlot->SetAnchors(FAnchors(0.5f, 0.5f, 0.5f, 0.5f));
			CanvasSlot->SetPosition(FVector2D(0.f, 0.f));
			CanvasSlot->SetSize(FVector2D(FinalWidth, FinalHeight));
			CanvasSlot->SetAutoSize(false);
		}
	}

	Widget->WidgetTree->RootWidget = RootPanel;
	Widget->SetIsFocusable(false);
	Widget->AddToViewport(ZOrder);
	if (InstanceSettings.bHideDrawComponent)
		Widget->SetVisibility(ESlateVisibility::Collapsed);
	// State is now pushed every engine frame via FTickableGameObject::Tick
}

void USwuiSubsystem::ShutdownRenderer()
{
	if (Widget && Widget->IsInViewport())
	{
		Widget->RemoveFromParent();
	}
	Widget = nullptr;
	View   = nullptr;
}

void USwuiSubsystem::UpdateInstanceSettings(const FSwuiInstanceSettings& NewSettings)
{
	if (!View) return;
	View->InstanceSettings = NewSettings;

	// Re-apply CVars so flags backed by CVars take effect immediately.
	// CVars are centrally owned in SwuiCVars.cpp.
	const USwuiSettings* Settings = GetDefault<USwuiSettings>();
	const bool bWantVerbose  = NewSettings.bVerbosePaintLog || (Settings && Settings->bVerbosePaintLog);
	const bool bWantNoUpload = NewSettings.bNoTextureUpload || (Settings && Settings->bNoTextureUpload);
	if (IConsoleVariable* VerbosePaintVar = CVarSwuiVerbosePaint.operator->())
		VerbosePaintVar->Set(bWantVerbose ? 1 : 0, ECVF_SetByCode);
	if (IConsoleVariable* NoTextureUploadVar = CVarSwuiNoTextureUpload.operator->())
		NoTextureUploadVar->Set(bWantNoUpload ? 1 : 0, ECVF_SetByCode);

	if (NewSettings.bHideDrawComponent && Widget)
		Widget->SetVisibility(ESlateVisibility::Collapsed);
	else if (!NewSettings.bHideDrawComponent && Widget)
		Widget->SetVisibility(ESlateVisibility::HitTestInvisible);
}

void USwuiSubsystem::SetWidgetVisible(bool bVisible)
{
	if (Widget)
		Widget->SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}

void USwuiSubsystem::LoadURI(const FString& URI)
{
	if (View) View->LoadURL(URI);
}

void USwuiSubsystem::ExecuteJavaScript(const FString& Script)
{
	if (View) View->ExecuteJavaScript(Script);
}

// ---- Pointer Input Forwarding (bridge to View) ----

void USwuiSubsystem::SetPointerInputEnabled(bool bEnabled)
{
	if (View) View->SetPointerInputEnabled(bEnabled);
}

void USwuiSubsystem::SetBrowserInputFocus(bool bFocused)
{
	if (View) View->SetBrowserInputFocus(bFocused);
}

// ---- Observe API ----

FString USwuiSubsystem::ResolveNamespace(UObject* Source, const FString& Namespace) const
{
	if (!Namespace.IsEmpty()) return Namespace;
	// Default: class name stripped of prefix, lowercased
	// AMyCharacter → "mycharacter"
	FString ClassName = Source->GetClass()->GetName();
	if (ClassName.StartsWith(TEXT("A")) || ClassName.StartsWith(TEXT("U")))
		ClassName = ClassName.RightChop(1);
	return ClassName.ToLower();
}

void USwuiSubsystem::ObserveProperty(UObject* Source, const FString& Namespace, const FName& PropertyName)
{
	if (!Source) return;

	FProperty* Prop = Source->GetClass()->FindPropertyByName(PropertyName);
	if (!Prop)
	{
		UE_LOG(LogTemp, Warning, TEXT("SWUI ObserveProperty: '%s' not found on '%s'"),
			*PropertyName.ToString(), *Source->GetClass()->GetName());
		return;
	}

	if (Swui_GetTSType(Prop).IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("SWUI ObserveProperty: '%s' has an unsupported type for JS sync"),
			*PropertyName.ToString());
		return;
	}

	FSwuiObservedProperty Entry;
	Entry.Source        = Source;
	Entry.PropertyName  = PropertyName;
	Entry.CachedProp    = Prop;
	Entry.NamespacedKey = ResolveNamespace(Source, Namespace) + TEXT(".") + PropertyName.ToString();

	ObservedProperties.Add(Entry);
}

void USwuiSubsystem::ObserveDelegate(UObject* Source, const FString& Namespace, const FName& DelegateName)
{
	if (!Source) return;

	FObjectProperty* DelegateProp = nullptr;
	FMulticastDelegateProperty* MCProp = nullptr;

	for (TFieldIterator<FMulticastDelegateProperty> It(Source->GetClass()); It; ++It)
	{
		if (It->GetFName() == DelegateName)
		{
			MCProp = *It;
			break;
		}
	}

	if (!MCProp)
	{
		UE_LOG(LogTemp, Warning, TEXT("SWUI ObserveDelegate: delegate '%s' not found on '%s'"),
			*DelegateName.ToString(), *Source->GetClass()->GetName());
		return;
	}

	// Cache payload field names + TS types from the delegate signature
	TArray<TTuple<FName, FString>> PayloadFields;
	UFunction* SignatureFunc = MCProp->SignatureFunction;
	if (SignatureFunc)
	{
		for (TFieldIterator<FProperty> ParamIt(SignatureFunc); ParamIt; ++ParamIt)
		{
			if (ParamIt->HasAnyPropertyFlags(CPF_Parm) && !ParamIt->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				const FString TSType = Swui_GetTSType(*ParamIt);
				if (!TSType.IsEmpty())
					PayloadFields.Add(MakeTuple(ParamIt->GetFName(), TSType));
			}
		}
	}

	FSwuiObservedDelegate Entry;
	Entry.Source        = Source;
	Entry.DelegateName  = DelegateName;
	Entry.NamespacedKey = ResolveNamespace(Source, Namespace) + TEXT(".") + DelegateName.ToString();
	Entry.PayloadFields = PayloadFields;

	ObservedDelegates.Add(Entry);

	// Bind a dynamic handler via the base-class AddDelegate API.
	// FMulticastDelegateProperty::AddDelegate works for both inline and sparse delegates.
	FScriptDelegate ScriptDelegate;
	ScriptDelegate.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(USwuiSubsystem, OnObservedDelegateFired));
	MCProp->AddDelegate(ScriptDelegate, Source);
}

// ---- Binding source auto-observe ----

void USwuiSubsystem::SetBindingSources(const TArray<FSwuiBindingSource>& Sources)
{
	CachedBindingSources = Sources;

	// At BeginPlay time all actors are already initialized — scan the world once
	// and auto-observe every actor whose class matches a configured source entry.
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (!World) return;

	// Iterate all live UObjects matching each configured source class.
	// This handles Actors, ActorComponents, and any other UObject subclass uniformly.
	for (const FSwuiBindingSource& Src : Sources)
	{
		if (!Src.SourceClass) continue;
		if (Src.Properties.IsEmpty() && Src.Delegates.IsEmpty()) continue;
		for (TObjectIterator<UObject> It; It; ++It)
		{
			if (It->GetWorld() != World) continue;
			if (!It->IsA(Src.SourceClass)) continue;
			ObserveSource(*It, /*bWarnOnMiss=*/false);

			// Auto-bind any checked delegate events on this instance.
			for (const FName& DelegateName : Src.Delegates)
				ObserveDelegate(*It, TEXT(""), DelegateName);
		}
	}
}

void USwuiSubsystem::ObserveSource(UObject* Instance, bool bWarnOnMiss)
{
	if (!Instance) return;
	UClass* InstanceClass = Instance->GetClass();

	for (const FSwuiBindingSource& Src : CachedBindingSources)
	{
		if (!Src.SourceClass || Src.Properties.IsEmpty()) continue;
		if (!InstanceClass->IsChildOf(Src.SourceClass)) continue;

		// Don't double-register the same instance.
		const bool bAlreadyObserved = ObservedProperties.ContainsByPredicate(
			[Instance](const FSwuiObservedProperty& E){ return E.Source == Instance; });
		if (bAlreadyObserved) return;

		for (const FName& PropName : Src.Properties)
			ObserveProperty(Instance, TEXT(""), PropName);
		return;
	}

	if (bWarnOnMiss)
		UE_LOG(LogTemp, Warning, TEXT("SWUI ObserveSource: no BindingSource entry found for class '%s'"),
			*InstanceClass->GetName());
}

void USwuiSubsystem::K2_Observe(UObject* Source, FName PropertyName)
{
	if (!Source) return;
	UWorld* World = Source->GetWorld();
	if (!World) return;
	if (UGameInstance* GI = World->GetGameInstance())
		if (USwuiSubsystem* Sub = GI->GetSubsystem<USwuiSubsystem>())
			Sub->ObserveProperty(Source, TEXT(""), PropertyName);
}

void USwuiSubsystem::K2_ObserveEvent(UObject* Source, FName DelegateName)
{
	if (!Source) return;
	UWorld* World = Source->GetWorld();
	if (!World) return;
	if (UGameInstance* GI = World->GetGameInstance())
		if (USwuiSubsystem* Sub = GI->GetSubsystem<USwuiSubsystem>())
			Sub->ObserveDelegate(Source, TEXT(""), DelegateName);
}

void USwuiSubsystem::Unobserve(UObject* Source)
{
	ObservedProperties.RemoveAll([&](const FSwuiObservedProperty& E) { return E.Source == Source; });
	ObservedDelegates.RemoveAll([&](const FSwuiObservedDelegate& E)  { return E.Source == Source; });
}

// ---- FTickableGameObject::Tick — runs every engine frame ----

void USwuiSubsystem::Tick(float DeltaTime)
{
	// Pump CEF at the start of the SWUI tick so queued CEF UI tasks,
	// browser timers, rAF, JS work, and pending paint work can progress.
	SwuiManager::DoSwuiMessageLoop();

	if (!View) return;

	View->NotifySubsystemTick();

	const bool bUseHudLockstepMode =
		View->InstanceSettings.bIsHUD &&
		SwuiCVarBool(
			SwuiGetIntCVar(TEXT("swui.hud.Lockstep"), -1),
			View->InstanceSettings.bUseUEFrameLockedBrowser);

	// If CEF produced fresh paint since the previous UE frame, upload it
	// immediately before requesting another browser frame. This keeps the
	// newest available HUD pixels as close as possible to the current UE render.
	if (View->HasFreshOnPaintDataPending())
	{
		View->TickDeferredUpload();
	}

	bForceBrowserFrameThisTick = false;
	bLastFlushSentExternalBeginFrame = false;

	const bool bCanFlushJs = !View->InstanceSettings.bPauseBrowserUpdates;
	if (bCanFlushJs && FlushHudStateToJs(DeltaTime))
	{
		View->NotifyHudStateFlushed();
	}

	// FlushHudStateToJs may post the combined JS + Invalidate + BeginFrame task
	// to the CEF UI thread. Pump immediately so that task can execute during
	// this UE tick instead of waiting for a later/irregular CEF pump.
	SwuiManager::DoSwuiMessageLoop();

	const bool bUseCombinedHudFramePath =
		bUseHudLockstepMode &&
		View->IsExternalBeginFrameActive();

	bool bSentExternalBeginFrame = bLastFlushSentExternalBeginFrame;
	if (!bSentExternalBeginFrame && !bUseCombinedHudFramePath)
	{
		bSentExternalBeginFrame = SendExternalBeginFrameIfDue(DeltaTime);
	}

	// Pump again after an explicit begin-frame request. This gives CEF a chance
	// to process Invalidate/BeginFrame and produce OnPaint in this UE tick.
	SwuiManager::DoSwuiMessageLoop();

	// If the browser produced fresh paint from the frame request above, upload it.
	// If no fresh paint is ready yet, the next tick's early upload path will pick it up.
	if (View->HasFreshOnPaintDataPending())
	{
		View->TickDeferredUpload();
	}

	// Interactive mode: keep animation active while pointer activity is recent.
	if (View->IsUiInteractionActive())
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - LastUiInteractionTime < 0.5)
		{
			bForceBrowserFrameThisTick = true;
			MarkHudAnimationActive(0.50);
		}
	}

	if (bUseHudLockstepMode)
	{
		// Final HUD-only pump for responsiveness. This keeps browser/compositor
		// work moving aggressively for lower perceived HUD latency.
		SwuiManager::DoSwuiMessageLoop();

	}
}


bool USwuiSubsystem::FlushHudStateToJs(float DeltaTime)
{
	if (!View) return false;

	const bool bHudLockStep =
		View->InstanceSettings.bIsHUD &&
		SwuiCVarBool(
			SwuiGetIntCVar(TEXT("swui.hud.Lockstep"), -1),
			View->InstanceSettings.bUseUEFrameLockedBrowser);
	const bool bFlushBeforeFrame =
		!bHudLockStep ||
		SwuiCVarBool(
			SwuiGetIntCVar(TEXT("swui.hud.FlushBeforeFrame"), -1),
			View->InstanceSettings.bFlushHudStateBeforeBrowserFrame);
	if (!bFlushBeforeFrame) return false;
	const bool bUseCombinedHudFramePath = bHudLockStep && View->IsExternalBeginFrameActive();
	const int32 BrowserFpsSetting = SwuiCVarInt(
		SwuiGetIntCVar(TEXT("swui.hud.MaxBrowserFPS"), -1),
		View->InstanceSettings.MaxBrowserFramesPerSecond);
	const int32 CefFPS = BrowserFpsSetting > 0 ? BrowserFpsSetting : View->GetWindowlessFrameRate();

	// Exponential moving average FPS (alpha=0.1, smoothed over ~10 frames)
	if (DeltaTime > 0.f)
		AvgFPS = AvgFPS * 0.9f + (1.f / DeltaTime) * 0.1f;
	LastDeltaTime = DeltaTime;

	if (!bHudLockStep)
	{
		const float MinInterval = CefFPS > 0 ? 1.0f / static_cast<float>(CefFPS) : 1.0f / 120.f;
		TickAccumulator += DeltaTime;
		if (TickAccumulator < MinInterval) return false;
		TickAccumulator = 0.f;
	}

	bool bFlushed = false;
	FString BatchedScript;

	const FString RuntimeScript = FString::Printf(
		TEXT("(function(){var s=(window.__SWUI__=window.__SWUI__||{state:{},events:{}});")
		TEXT("s._runtime={fps:%.1f,dt:%.4f,cefFps:%d,width:%d,height:%d};")
		TEXT("document.dispatchEvent(new CustomEvent('swui:tick',{detail:s._runtime}));")
		TEXT("})()"),
		AvgFPS, LastDeltaTime,
		CefFPS, View->Width, View->Height);
	BatchedScript += RuntimeScript;
	bFlushed = true;

	if (ObservedProperties.Num() > 0)
	{
		FString Script = TEXT("(function(){var s=(window.__SWUI__=window.__SWUI__||{state:{},events:{}});");
		bool bAny = false;
		FString LatestCurrentAmmoJs;
		FString LatestReserveAmmoJs;
		bool bCurrentAmmoChanged = false;
		bool bReserveAmmoChanged = false;

		for (int32 i = ObservedProperties.Num() - 1; i >= 0; --i)
		{
			FSwuiObservedProperty& Entry = ObservedProperties[i];
			if (!Entry.Source.IsValid())
			{
				ObservedProperties.RemoveAtSwap(i);
				continue;
			}

			UObject* Obj = Entry.Source.Get();
			if (!Entry.CachedProp) continue;

			const FString JSValue = Swui_SerializeProperty(Entry.CachedProp, Obj);
			if (JSValue.IsEmpty()) continue;
			const FString* PrevValue = LastObservedValues.Find(Entry.NamespacedKey);
			const bool bChanged = !PrevValue || *PrevValue != JSValue;
			if (bChanged)
			{
				LastObservedValues.Add(Entry.NamespacedKey, JSValue);
				if (Entry.NamespacedKey.Contains(TEXT("compass"), ESearchCase::IgnoreCase))
				{
					bForceBrowserFrameThisTick = true;
				}
				if (Entry.NamespacedKey.Contains(TEXT("ammo"), ESearchCase::IgnoreCase)
					|| Entry.NamespacedKey.Contains(TEXT("reload"), ESearchCase::IgnoreCase)
					|| Entry.NamespacedKey.Contains(TEXT("crosshair"), ESearchCase::IgnoreCase)
					|| Entry.NamespacedKey.Contains(TEXT("ability"), ESearchCase::IgnoreCase))
				{
					bForceBrowserFrameThisTick = true;
					MarkHudAnimationActive(0.20);
				}
			}

			Script += FString::Printf(
				TEXT("s.state['%s']=%s;if(s._notify)s._notify('%s',%s);"),
				*Entry.NamespacedKey, *JSValue,
				*Entry.NamespacedKey, *JSValue);

			if (Entry.NamespacedKey.Contains(TEXT("compass"), ESearchCase::IgnoreCase)
				&& Entry.NamespacedKey.Contains(TEXT("angle"), ESearchCase::IgnoreCase)
				&& bChanged)
			{
				Script += FString::Printf(
					TEXT("if(window.__SWUI_HUD__&&window.__SWUI_HUD__.setCompass)window.__SWUI_HUD__.setCompass(%s);"),
					*JSValue);
			}

			if (Entry.NamespacedKey.Contains(TEXT("currentammo"), ESearchCase::IgnoreCase))
			{
				LatestCurrentAmmoJs = JSValue;
				if (bChanged) bCurrentAmmoChanged = true;
			}
			if (Entry.NamespacedKey.Contains(TEXT("reserveammo"), ESearchCase::IgnoreCase))
			{
				LatestReserveAmmoJs = JSValue;
				if (bChanged) bReserveAmmoChanged = true;
			}
			if (Entry.NamespacedKey.Contains(TEXT("reloading"), ESearchCase::IgnoreCase) && bChanged)
			{
				Script += FString::Printf(
					TEXT("if(window.__SWUI_HUD__&&window.__SWUI_HUD__.setReloading)window.__SWUI_HUD__.setReloading(%s);"),
					*JSValue);
			}
			bAny = true;
		}

		if ((bCurrentAmmoChanged || bReserveAmmoChanged) && !LatestCurrentAmmoJs.IsEmpty() && !LatestReserveAmmoJs.IsEmpty())
		{
			Script += FString::Printf(
				TEXT("if(window.__SWUI_HUD__&&window.__SWUI_HUD__.setAmmo)window.__SWUI_HUD__.setAmmo(%s,%s);"),
				*LatestCurrentAmmoJs,
				*LatestReserveAmmoJs);
		}

		Script += TEXT("})();");
		if (bAny)
		{
			if (!BatchedScript.IsEmpty()) BatchedScript += TEXT(";");
			BatchedScript += Script;
			bFlushed = true;
		}
	}

	if (QueuedHudEventScripts.Num() > 0)
	{
		FString BatchedEvents;
		for (const FString& EventScript : QueuedHudEventScripts)
		{
			if (!BatchedEvents.IsEmpty()) BatchedEvents += TEXT(";");
			BatchedEvents += EventScript;
		}
		QueuedHudEventScripts.Reset();
		if (!BatchedEvents.IsEmpty())
		{
			if (!BatchedScript.IsEmpty()) BatchedScript += TEXT(";");
			BatchedScript += BatchedEvents;
			bFlushed = true;
		}
	}

	if (!bFlushed)
	{
		return false;
	}

	if (bUseCombinedHudFramePath)
	{
		const bool bAnimationWindowActive = FPlatformTime::Seconds() < HudAnimationActiveUntil;
		const bool bForceFrame = bForceBrowserFrameThisTick || bAnimationWindowActive;
		bLastFlushSentExternalBeginFrame = View->FlushHudStateAndRequestBrowserFrame(BatchedScript, DeltaTime, bForceFrame);
	}
	else
	{
		View->ExecuteJavaScript(BatchedScript);
	}

	return bFlushed;
}

void USwuiSubsystem::RequestHudVisualRefresh(float DurationSeconds, bool bForceFullUpload)
{
	UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI VISUAL REFRESH] RequestHudVisualRefresh  duration=%.3f  forceFullUpload=%d"),
		DurationSeconds, bForceFullUpload ? 1 : 0);
	bForceBrowserFrameThisTick = true;
	MarkHudAnimationActive(DurationSeconds);
	if (bForceFullUpload && View)
	{
		View->RequestFullTextureUploadNextFrame();
	}
}

void USwuiSubsystem::BeginFullTransitionRefresh(int32 FreshPaintCount)
{
	if (View) View->BeginFullTransitionRefresh(FreshPaintCount);
	bForceBrowserFrameThisTick = true;
	MarkHudAnimationActive(0.50);
}

void USwuiSubsystem::UpdateUiInteractionTime()
{
	LastUiInteractionTime = FPlatformTime::Seconds();
}

void USwuiSubsystem::SetUiInteractionActive(bool bActive)
{
	if (View) View->SetUiInteractionActive(bActive);
	if (bActive)
	{
		LastUiInteractionTime = FPlatformTime::Seconds();
		MarkHudAnimationActive(0.50);
	}
}

void USwuiSubsystem::QueueHudEventScript(const FString& Script)
{
	if (!Script.IsEmpty())
	{
		QueuedHudEventScripts.Add(Script);
	}
}

void USwuiSubsystem::MarkHudAnimationActive(double DurationSeconds)
{
	const double Now = FPlatformTime::Seconds();
	HudAnimationActiveUntil = FMath::Max(HudAnimationActiveUntil, Now + DurationSeconds);
}

bool USwuiSubsystem::SendExternalBeginFrameIfDue(float DeltaTime)
{
	if (View)
	{
		return View->SendExternalBeginFrameIfDue(DeltaTime);
	}
	return false;
}

// ---- Delegate fire trampoline ----
// This is called whenever any observed delegate fires.
// We read all currently live observed delegates, serialize what we can from the
// delegate payload via the signature function's parameter layout.
void USwuiSubsystem::OnObservedDelegateFired()
{
	// Without per-binding context we dispatch a generic ping for all live delegates
	// whose source is still valid. For rich payload dispatch the caller uses
	// ObserveDelegate which caches payload fields — future work.
	for (const FSwuiObservedDelegate& Entry : ObservedDelegates)
	{
		if (!Entry.Source.IsValid()) continue;

		FString Script = FString::Printf(
			TEXT("(function(){var s=(window.__SWUI__=window.__SWUI__||{state:{},events:{}});")
			TEXT("var ev=new CustomEvent('%s',{detail:{}});document.dispatchEvent(ev);})();"),
			*Entry.NamespacedKey);
		QueueHudEventScript(Script);

		if (Entry.NamespacedKey.Contains(TEXT("fired"), ESearchCase::IgnoreCase)
			|| Entry.NamespacedKey.Contains(TEXT("hit"), ESearchCase::IgnoreCase)
			|| Entry.NamespacedKey.Contains(TEXT("reload"), ESearchCase::IgnoreCase)
			|| Entry.NamespacedKey.Contains(TEXT("ability"), ESearchCase::IgnoreCase))
		{
			QueueHudEventScript(TEXT("if(window.__SWUI_HUD__&&window.__SWUI_HUD__.weaponFired)window.__SWUI_HUD__.weaponFired({});"));
			bForceBrowserFrameThisTick = true;
			MarkHudAnimationActive(0.20);
		}
	}
}
