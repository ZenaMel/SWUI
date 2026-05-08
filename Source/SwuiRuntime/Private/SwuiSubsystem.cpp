#include "SwuiSubsystem.h"
#include "SwuiSettings.h"
#include "Swui.h"
#include "SwuiView.h"
#include "ISwuiRuntime.h"
#include "GameFramework/GameUserSettings.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Image.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Engine/Engine.h"
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

// ---- Lifecycle ----

bool USwuiSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const USwuiSettings* Settings = GetDefault<USwuiSettings>();
	return Settings && !Settings->bDisablePlugin;
}

void USwuiSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void USwuiSubsystem::Deinitialize()
{
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
	bool bIsHUD, int32 Width, int32 Height, int32 ZOrder,
	UMaterialInterface* BaseMaterial, FName TextureParamName)
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
	View->Init();

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
	if (!View || ObservedProperties.Num() == 0) return;

	// Exponential moving average FPS (alpha=0.1, smoothed over ~10 frames)
	if (DeltaTime > 0.f)
		AvgFPS = AvgFPS * 0.9f + (1.f / DeltaTime) * 0.1f;
	LastDeltaTime = DeltaTime;

	// Throttle JS pushes to the CEF windowless frame rate (avoid hammering the browser).
	const int32 CefFPS  = View->GetWindowlessFrameRate();
	const float MinInterval = CefFPS > 0 ? 1.0f / static_cast<float>(CefFPS) : 1.0f / 120.f;
	TickAccumulator += DeltaTime;
	if (TickAccumulator < MinInterval) return;
	TickAccumulator = 0.f;

	// ---- Runtime info push (fps, dt, dimensions) --------------------------------
	const FString RuntimeScript = FString::Printf(
		TEXT("(function(){var s=(window.__SWUI__=window.__SWUI__||{state:{},events:{}});") \
		TEXT("s._runtime={fps:%.1f,dt:%.4f,cefFps:%d,width:%d,height:%d};") \
		TEXT("document.dispatchEvent(new CustomEvent('swui:tick',{detail:s._runtime}));") \
		TEXT("})()"),
		AvgFPS, LastDeltaTime,
		View->GetWindowlessFrameRate(), View->Width, View->Height);
	View->ExecuteJavaScript(RuntimeScript);

	// ---- State properties push --------------------------------------------------
FString Script = TEXT("(function(){var s=(window.__SWUI__=window.__SWUI__||{state:{},events:{}});");
	bool bAny = false;

	for (int32 i = ObservedProperties.Num() - 1; i >= 0; --i)
	{
		FSwuiObservedProperty& Entry = ObservedProperties[i];

		// Auto-drop dead objects
		if (!Entry.Source.IsValid())
		{
			ObservedProperties.RemoveAtSwap(i);
			continue;
		}

		UObject* Obj = Entry.Source.Get();
		if (!Entry.CachedProp) continue;

		const FString JSValue = Swui_SerializeProperty(Entry.CachedProp, Obj);
		if (JSValue.IsEmpty()) continue;

		Script += FString::Printf(
			TEXT("s.state['%s']=%s;if(s._notify)s._notify('%s',%s);"),
			*Entry.NamespacedKey, *JSValue,
			*Entry.NamespacedKey, *JSValue);
		bAny = true;
	}

	Script += TEXT("})();");
	if (bAny) View->ExecuteJavaScript(Script);
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

		if (View) View->ExecuteJavaScript(Script);
	}
}
