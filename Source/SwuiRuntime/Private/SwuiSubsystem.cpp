#include "SwuiSubsystem.h"
#include "SwuiManager.h"
#include "SwuiSettings.h"
#include "Swui.h"
#include "SwuiView.h"
#include "SwuiInputPreprocessor.h"
#include "SwuiHudRoiOverlayWidget.h"
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
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UnrealType.h"
#include "UObject/Field.h"
#include "UObject/PropertyIterator.h"
#include "UObject/ScriptInterface.h"
#include "GameplayTagContainer.h"

// ---- Helpers ----

static FString Swui_GetTSType(const FProperty* Prop)
{
	if (Prop->IsA<FFloatProperty>()  || Prop->IsA<FDoubleProperty>() ||
		Prop->IsA<FIntProperty>()    || Prop->IsA<FInt64Property>()  ||
		Prop->IsA<FEnumProperty>()   ||
		Prop->IsA<FByteProperty>())    return TEXT("number");
	if (Prop->IsA<FBoolProperty>())    return TEXT("boolean");
	if (Prop->IsA<FStrProperty>()   || Prop->IsA<FNameProperty>() ||
		Prop->IsA<FTextProperty>())    return TEXT("string");

	if (const FStructProperty* StructProp = CastField<const FStructProperty>(Prop))
	{
		const FString StructName = StructProp->Struct->GetName();
		if (StructName == TEXT("GameplayTag")) return TEXT("string");
		return TEXT("object");
	}

	if (Prop->IsA<FArrayProperty>()) return TEXT("array");
	if (Prop->IsA<FMapProperty>()) return TEXT("map");
	if (Prop->IsA<FObjectPropertyBase>()) return TEXT("object");

	return FString();
}

static FString Swui_QuoteJsonString(const FString& Raw)
{
	FString Escaped = Raw;
	Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Escaped.ReplaceInline(TEXT("\t"), TEXT("\\t"));
	return TEXT("\"") + Escaped + TEXT("\"");
}

// Forward declaration
static FString Swui_SerializePropertyValue(const FProperty* Prop, const void* ValuePtr);

static FString Swui_SerializeStructFields(const UScriptStruct* Struct, const void* StructPtr)
{
	TArray<FString> Fields;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		const void* FieldValuePtr = It->ContainerPtrToValuePtr<void>(StructPtr);
		FString FieldValue = Swui_SerializePropertyValue(*It, FieldValuePtr);
		if (FieldValue.IsEmpty()) continue;
		Fields.Add(Swui_QuoteJsonString(It->GetName()) + TEXT(":") + FieldValue);
	}
	return TEXT("{") + FString::Join(Fields, TEXT(",")) + TEXT("}");
}

static FString Swui_SerializePropertyValue(const FProperty* Prop, const void* ValuePtr)
{
	if (const FFloatProperty*  P = CastField<FFloatProperty>(Prop))  return FString::SanitizeFloat(P->GetPropertyValue(ValuePtr));
	if (const FDoubleProperty* P = CastField<FDoubleProperty>(Prop)) return FString::SanitizeFloat(P->GetPropertyValue(ValuePtr));
	if (const FIntProperty*    P = CastField<FIntProperty>(Prop))    return FString::FromInt(P->GetPropertyValue(ValuePtr));
	if (const FInt64Property*  P = CastField<FInt64Property>(Prop))  return FString::Printf(TEXT("%lld"), P->GetPropertyValue(ValuePtr));
	if (const FEnumProperty*   P = CastField<FEnumProperty>(Prop))
	{
		const int64 EnumValue = P->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
		return Swui_QuoteJsonString(P->GetEnum()->GetNameStringByValue(EnumValue));
	}
	if (const FByteProperty* P = CastField<FByteProperty>(Prop))
	{
		if (P->Enum) return Swui_QuoteJsonString(P->Enum->GetNameStringByValue(P->GetPropertyValue(ValuePtr)));
		return FString::FromInt(P->GetPropertyValue(ValuePtr));
	}
	if (const FBoolProperty* P = CastField<FBoolProperty>(Prop)) return P->GetPropertyValue(ValuePtr) ? TEXT("true") : TEXT("false");
	if (const FStrProperty*  P = CastField<FStrProperty>(Prop))  return Swui_QuoteJsonString(P->GetPropertyValue(ValuePtr));
	if (const FNameProperty* P = CastField<FNameProperty>(Prop)) return Swui_QuoteJsonString(P->GetPropertyValue(ValuePtr).ToString());
	if (const FTextProperty* P = CastField<FTextProperty>(Prop)) return Swui_QuoteJsonString(P->GetPropertyValue(ValuePtr).ToString());

	if (const FStructProperty* P = CastField<FStructProperty>(Prop))
	{
		const FString StructName = P->Struct->GetName();
		if (StructName == TEXT("GameplayTag"))
		{
			const FGameplayTag* Tag = static_cast<const FGameplayTag*>(ValuePtr);
			return Tag ? Swui_QuoteJsonString(Tag->ToString()) : TEXT("null");
		}
		if (StructName == TEXT("Vector2D"))
		{
			const FVector2D* V = static_cast<const FVector2D*>(ValuePtr);
			return V ? FString::Printf(TEXT("{\"x\":%s,\"y\":%s}"), *FString::SanitizeFloat(V->X), *FString::SanitizeFloat(V->Y)) : TEXT("null");
		}
		if (StructName == TEXT("Vector"))
		{
			const FVector* V = static_cast<const FVector*>(ValuePtr);
			return V ? FString::Printf(TEXT("{\"x\":%s,\"y\":%s,\"z\":%s}"), *FString::SanitizeFloat(V->X), *FString::SanitizeFloat(V->Y), *FString::SanitizeFloat(V->Z)) : TEXT("null");
		}
		if (StructName == TEXT("Rotator"))
		{
			const FRotator* R = static_cast<const FRotator*>(ValuePtr);
			return R ? FString::Printf(TEXT("{\"pitch\":%s,\"yaw\":%s,\"roll\":%s}"), *FString::SanitizeFloat(R->Pitch), *FString::SanitizeFloat(R->Yaw), *FString::SanitizeFloat(R->Roll)) : TEXT("null");
		}
		if (StructName == TEXT("LinearColor"))
		{
			const FLinearColor* C = static_cast<const FLinearColor*>(ValuePtr);
			return C ? FString::Printf(TEXT("{\"r\":%s,\"g\":%s,\"b\":%s,\"a\":%s}"), *FString::SanitizeFloat(C->R), *FString::SanitizeFloat(C->G), *FString::SanitizeFloat(C->B), *FString::SanitizeFloat(C->A)) : TEXT("null");
		}
		if (StructName == TEXT("Color"))
		{
			const FColor* C = static_cast<const FColor*>(ValuePtr);
			if (C) return FString::Printf(TEXT("{\"r\":%d,\"g\":%d,\"b\":%d,\"a\":%d}"), C->R, C->G, C->B, C->A);
			return TEXT("null");
		}
		return Swui_SerializeStructFields(P->Struct, ValuePtr);
	}

	if (const FArrayProperty* P = CastField<FArrayProperty>(Prop))
	{
		FScriptArrayHelper Helper(P, ValuePtr);
		TArray<FString> Elements;
		for (int32 i = 0; i < Helper.Num(); ++i)
		{
			FString ElemValue = Swui_SerializePropertyValue(P->Inner, Helper.GetRawPtr(i));
			if (!ElemValue.IsEmpty()) Elements.Add(ElemValue);
		}
		return TEXT("[") + FString::Join(Elements, TEXT(",")) + TEXT("]");
	}

	if (const FMapProperty* P = CastField<FMapProperty>(Prop))
	{
		FScriptMapHelper MapHelper(P, ValuePtr);
		TArray<FString> Entries;
		for (int32 Index = 0; Index < MapHelper.GetMaxIndex(); ++Index)
		{
			if (!MapHelper.IsValidIndex(Index)) continue;

			const void* KeyPtr = MapHelper.GetKeyPtr(Index);
			const void* ValPtr = MapHelper.GetValuePtr(Index);

			FString KeyString;
			if (const FStrProperty* StrKey = CastField<FStrProperty>(P->KeyProp))
			{
				KeyString = StrKey->GetPropertyValue(KeyPtr);
			}
			else if (const FNameProperty* NameKey = CastField<FNameProperty>(P->KeyProp))
			{
				KeyString = NameKey->GetPropertyValue(KeyPtr).ToString();
			}
			else
			{
				continue;
			}

			FString ValValue = Swui_SerializePropertyValue(P->ValueProp, ValPtr);
			if (!ValValue.IsEmpty())
			{
				Entries.Add(Swui_QuoteJsonString(KeyString) + TEXT(":") + ValValue);
			}
		}
		return TEXT("{") + FString::Join(Entries, TEXT(",")) + TEXT("}");
	}

	if (const FObjectPropertyBase* P = CastField<FObjectPropertyBase>(Prop))
	{
		UObject* Obj = P->GetObjectPropertyValue(ValuePtr);
		if (!Obj) return TEXT("null");
		return FString::Printf(TEXT("{\"name\":%s,\"className\":%s}"),
			*Swui_QuoteJsonString(Obj->GetName()), *Swui_QuoteJsonString(Obj->GetClass()->GetName()));
	}

	if (Prop->IsA<FSoftObjectProperty>() || Prop->IsA<FSoftClassProperty>())
	{
		return TEXT("null");
	}

	return FString();
}

static FString Swui_SerializeProperty(const FProperty* Prop, const void* Container)
{
	if (!Prop || !Container) return FString();
	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container);
	return Swui_SerializePropertyValue(Prop, ValuePtr);
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
	// Ensure the CVar is restored even if ShutdownRenderer missed it.
	if (SavedOneFrameThreadLag >= 0)
	{
		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.OneFrameThreadLag"));
		if (CVar)
		{
			CVar->Set(SavedOneFrameThreadLag, ECVF_SetByGameOverride);
			if (CVarSwuiVerbosePaint.GetValueOnGameThread() != 0)
			{
				UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI Runtime] Restored r.OneFrameThreadLag=%d (on Deinitialize)"), SavedOneFrameThreadLag);
			}
		}
		SavedOneFrameThreadLag = -1;
	}
	LastAppliedFramePacingMode = ESwuiLowLatencyFramePacingMode::Disabled;
	ObservedProperties.Empty();
	ObservedDelegates.Empty();
	DelegateBridges.Empty();
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

	if (View)
	{
		UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI InitRenderer] view already exists — skipping duplicate init (URI=%s)"), *URI);
		return;
	}

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

	// Inject focus-tracking JS so the runtime knows when an editable element has focus.
	View->ExecuteJavaScript(
		TEXT("(function(){var f=false;")
		TEXT("document.addEventListener('focusin',function(e){")
		TEXT("var el=e.target,ed=el&&(el.tagName==='INPUT'||el.tagName==='TEXTAREA'||el.tagName==='SELECT'||el.isContentEditable);")
		TEXT("if(ed!==f){f=ed;window.cefQuery({request:JSON.stringify({type:'swui:focusInput',focused:ed}),onSuccess:function(){}});}")
		TEXT("});")
		TEXT("document.addEventListener('focusout',function(){")
		TEXT("setTimeout(function(){")
		TEXT("var el=document.activeElement,ed=el&&(el.tagName==='INPUT'||el.tagName==='TEXTAREA'||el.tagName==='SELECT'||el.isContentEditable);")
		TEXT("if(ed!==f){f=!!ed;window.cefQuery({request:JSON.stringify({type:'swui:focusInput',focused:!!ed}),onSuccess:function(){}});}")
		TEXT("},0);")
		TEXT("});")
		TEXT("})();")
	);

	UpdateLowLatencyFramePacing();

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
	DestroyRoiOverlay();
	bFocusScriptInjected = false;

	if (Widget && Widget->IsInViewport())
	{
		Widget->RemoveFromParent();
	}
	Widget = nullptr;
	View   = nullptr;
	UpdateLowLatencyFramePacing();
}

// ── HUD ROI Overlay ─────────────────────────────────────────────────────

void USwuiSubsystem::UpdateRoiOverlay()
{
	if (!View)
	{
		DestroyRoiOverlay();
		return;
	}

	const FSwuiHudRoiOverlayState State = View->GetHudRoiOverlayState();

	if (!State.bVisible && !RoiOverlay)
	{
		return;
	}

	if (!State.bVisible && RoiOverlay)
	{
		DestroyRoiOverlay();
		return;
	}

	if (State.bVisible && !RoiOverlay)
	{
		CreateRoiOverlay();
	}

	if (RoiOverlay && View)
	{
		const bool bShade = SwuiCVarBool(
			CVarSwuiHudRoiShadeInactive.GetValueOnGameThread(),
			View->GetHudRoiSettings().bShadeInactiveArea);

		RoiOverlay->UpdateOverlay(State, View->Width, View->Height, bShade);
	}
}

void USwuiSubsystem::CreateRoiOverlay()
{
	if (RoiOverlay)
	{
		return;
	}

	UWorld* World = GetGameInstance()->GetWorld();
	if (!World) return;

	RoiOverlay = CreateWidget<USwuiHudRoiOverlayWidget>(World, USwuiHudRoiOverlayWidget::StaticClass());
	if (RoiOverlay)
	{
		RoiOverlay->AddToViewport(10000); // High Z-order to stay on top
		RoiOverlay->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
}

void USwuiSubsystem::DestroyRoiOverlay()
{
	if (RoiOverlay)
	{
		if (RoiOverlay->IsInViewport())
		{
			RoiOverlay->RemoveFromParent();
		}
		RoiOverlay = nullptr;
	}
}

// ── UpdateInstanceSettings ──────────────────────────────────────────────

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

	View->UpdateHudRoiSettings(NewSettings.HudRoiSettings);
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

	const FString NsKey = ResolveNamespace(Source, Namespace) + TEXT(".") + DelegateName.ToString();

	FSwuiObservedDelegate Entry;
	Entry.Source        = Source;
	Entry.DelegateName  = DelegateName;
	Entry.NamespacedKey = NsKey;
	Entry.PayloadFields = PayloadFields;

	ObservedDelegates.Add(Entry);

	// Create a generic bridge that intercepts ProcessEvent and serializes the
	// delegate's actual broadcast parameters using the SignatureFunction's
	// property names and layout. Works for any delegate signature.
	USwuiDelegateBridge* Bridge = NewObject<USwuiDelegateBridge>(this);
	Bridge->Init(NsKey, this, SignatureFunc);
	DelegateBridges.Add(Bridge);

	FScriptDelegate ScriptDelegate;
	ScriptDelegate.BindUFunction(Bridge, GET_FUNCTION_NAME_CHECKED(USwuiDelegateBridge, DelegateHook));
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

	RebuildCommandRuntime();
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

	// Retry focus-tracking script injection on first tick after browser is ready.
	if (!bFocusScriptInjected && View->HasBrowserHost())
	{
		bFocusScriptInjected = true;
		View->ExecuteJavaScript(
			TEXT("(function(){var f=false;")
			TEXT("document.addEventListener('focusin',function(e){")
			TEXT("var el=e.target,ed=el&&(el.tagName==='INPUT'||el.tagName==='TEXTAREA'||el.tagName==='SELECT'||el.isContentEditable);")
			TEXT("if(ed!==f){f=ed;window.cefQuery({request:JSON.stringify({type:'swui:focusInput',focused:ed}),onSuccess:function(){}});}")
			TEXT("});")
			TEXT("document.addEventListener('focusout',function(){")
			TEXT("setTimeout(function(){")
			TEXT("var el=document.activeElement,ed=el&&(el.tagName==='INPUT'||el.tagName==='TEXTAREA'||el.tagName==='SELECT'||el.isContentEditable);")
			TEXT("if(ed!==f){f=!!ed;window.cefQuery({request:JSON.stringify({type:'swui:focusInput',focused:!!ed}),onSuccess:function(){}});}")
			TEXT("},0);")
			TEXT("});")
			TEXT("})();")
		);
	}

	// Sync focus state from CEF browser to subsystem-level flag.
	bTextInputFocused = View->IsTextInputFocused();

	View->NotifySubsystemTick();

	// Drive continuous browser frame + upload latest full surface if fresh paint exists.
	View->TickDeferredUpload();

	const bool bCanFlushJs = !View->InstanceSettings.bPauseBrowserUpdates;
	if (bCanFlushJs && FlushHudStateToJs(DeltaTime))
	{
		View->NotifyHudStateFlushed();
	}

	// ── HUD ROI overlay ─────────────────────────────────────────────────
	UpdateRoiOverlay();

	// Pump again after JS flush. This gives CEF a chance to process any
	// ExecuteJavaScript posted tasks and produce OnPaint.
	SwuiManager::DoSwuiMessageLoop();

	// Final pump for lockstep mode to keep CEF responsive.
	SwuiManager::DoSwuiMessageLoop();
}


bool USwuiSubsystem::FlushHudStateToJs(float DeltaTime)
{
	if (!View) return false;

	const bool bFlushBeforeFrame =
		SwuiCVarBool(
			CVarSwuiHudFlushBeforeFrame.GetValueOnGameThread(),
			View->InstanceSettings.bFlushHudStateBeforeBrowserFrame);
	if (!bFlushBeforeFrame) return false;

	const int32 CefFPS = SwuiCVarInt(
		CVarSwuiHudMaxBrowserFPS.GetValueOnGameThread(),
		View->InstanceSettings.MaxBrowserFramesPerSecond);
	const int32 EffectiveFPS = CefFPS > 0 ? CefFPS : View->GetWindowlessFrameRate();

	// Exponential moving average FPS (alpha=0.1, smoothed over ~10 frames)
	if (DeltaTime > 0.f)
		AvgFPS = AvgFPS * 0.9f + (1.f / DeltaTime) * 0.1f;
	LastDeltaTime = DeltaTime;

	// Rate-limit JS state pushes to the CEF frame rate to avoid flooding.
	{
		const float MinInterval = EffectiveFPS > 0 ? 1.0f / static_cast<float>(EffectiveFPS) : 1.0f / 120.f;
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
				if (Entry.NamespacedKey.Contains(TEXT("ammo"), ESearchCase::IgnoreCase)
					|| Entry.NamespacedKey.Contains(TEXT("reload"), ESearchCase::IgnoreCase)
					|| Entry.NamespacedKey.Contains(TEXT("crosshair"), ESearchCase::IgnoreCase)
					|| Entry.NamespacedKey.Contains(TEXT("ability"), ESearchCase::IgnoreCase))
				{
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

	View->ExecuteJavaScript(BatchedScript);

	return bFlushed;
}

void USwuiSubsystem::RequestHudVisualRefresh(float DurationSeconds, bool bForceFullUpload)
{
	if (View)
	{
		View->RequestBrowserVisualRefresh(true);
	}
}

void USwuiSubsystem::UpdateUiInteractionTime()
{
	LastUiInteractionTime = FPlatformTime::Seconds();
}

void USwuiSubsystem::QueueHudEventScript(const FString& Script)
{
	if (Script.IsEmpty())
	{
		return;
	}
	QueuedHudEventScripts.Add(Script);
}



void USwuiSubsystem::UpdateLowLatencyFramePacing()
{
	const USwuiSettings* Settings = GetDefault<USwuiSettings>();
	const ESwuiLowLatencyFramePacingMode ConfiguredMode = Settings
		? Settings->LowLatencyFramePacingMode
		: ESwuiLowLatencyFramePacingMode::Disabled;

	// Determine whether the condition is currently active.
	bool bShouldApply = false;
	switch (ConfiguredMode)
	{
	case ESwuiLowLatencyFramePacingMode::WhileInteractiveUiActive:
		bShouldApply = (View != nullptr);
		break;
	case ESwuiLowLatencyFramePacingMode::WhileAnySwuiViewActive:
		bShouldApply = (View != nullptr);
		break;
	default:
		break;
	}

	// Avoid repeated CVar sets when mode hasn't changed.
	if (bShouldApply == (LastAppliedFramePacingMode != ESwuiLowLatencyFramePacingMode::Disabled))
	{
		return;
	}

	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.OneFrameThreadLag"));
	if (!CVar) return;

	if (bShouldApply)
	{
		// Save previous value on first activation.
		if (SavedOneFrameThreadLag < 0)
		{
			SavedOneFrameThreadLag = CVar->GetInt();
		}
		CVar->Set(0, ECVF_SetByGameOverride);
		LastAppliedFramePacingMode = ConfiguredMode;
		if (View && (CVarSwuiVerbosePaint.GetValueOnGameThread() != 0 || View->InstanceSettings.bVerbosePaintLog))
		{
			UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI Runtime] Applied low latency frame pacing: r.OneFrameThreadLag 0"));
		}
	}
	else
	{
		// Restore previous value.
		if (SavedOneFrameThreadLag >= 0)
		{
			CVar->Set(SavedOneFrameThreadLag, ECVF_SetByGameOverride);
			if (View && (CVarSwuiVerbosePaint.GetValueOnGameThread() != 0 || View->InstanceSettings.bVerbosePaintLog))
			{
				UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI Runtime] Restored r.OneFrameThreadLag=%d"), SavedOneFrameThreadLag);
			}
			SavedOneFrameThreadLag = -1;
		}
		LastAppliedFramePacingMode = ESwuiLowLatencyFramePacingMode::Disabled;
	}
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
// This is called whenever any observed delegate fires using the OLD shared path.
// Only used as a fallback; per-delegate bridges (USwuiDelegateBridge) are preferred.
void USwuiSubsystem::OnObservedDelegateFired()
{
	for (const FSwuiObservedDelegate& Entry : ObservedDelegates)
	{
		if (!Entry.Source.IsValid()) continue;

		FString Script = FString::Printf(
			TEXT("(function(){var s=(window.__SWUI__=window.__SWUI__||{state:{},events:{}});")
			TEXT("var ev=new CustomEvent('%s',{detail:{}});document.dispatchEvent(ev);})();"),
			*Entry.NamespacedKey);
		QueueHudEventScript(Script);
	}
}

// ---- USwuiDelegateBridge ----
// Generic bridge: ProcessEvent is overridden so that when the observed delegate
// broadcasts, we intercept the call, read ALL parameters from the delegate's
// Parms buffer using the stored SignatureFunction's property layout, serialize
// each param by its original name, and dispatch the SWUI CustomEvent.
// No per-type Fire* functions needed — this handles any delegate signature.

void USwuiDelegateBridge::Init(const FString& InNsKey, USwuiSubsystem* InOwner, UFunction* InDelegateSignature)
{
	NamespacedKey = InNsKey;
	Owner = InOwner;
	DelegateSignature = InDelegateSignature;
	HookFunction = FindFunctionChecked(TEXT("DelegateHook"));
}

void USwuiDelegateBridge::ProcessEvent(UFunction* Function, void* Parms)
{
	if (Function != HookFunction || !Owner || !DelegateSignature)
	{
		// Not our delegate hook — chain to base class so normal UObject
		// events (Serialize, FinishDestroy, etc.) still work.
		Super::ProcessEvent(Function, Parms);
		return;
	}

	// When the observed delegate broadcasts, ProcessDelegate calls ProcessEvent
	// on this bridge object with the real broadcast arguments in the Parms buffer.
	// This buffer is a struct matching the delegate's SignatureFunction property
	// layout — guaranteed by UHT generating both from the same declaration
	// (_Script_Parms struct in _DelegateWrapper). No ABI-guessing needed.
	FString Json = TEXT("{");
	bool bFirst = true;

	for (TFieldIterator<FProperty> It(DelegateSignature); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Parm) || It->HasAnyPropertyFlags(CPF_ReturnParm))
			continue;

		if (!bFirst) Json += TEXT(",");
		bFirst = false;

		const FString FieldName = It->GetName();
		Json += Swui_QuoteJsonString(FieldName) + TEXT(":");

		void* ValuePtr = It->ContainerPtrToValuePtr<void>(Parms);
		Json += Swui_SerializePropertyValue(*It, ValuePtr);
	}

	Json += TEXT("}");

	const FString EscapedName = Swui_QuoteJsonString(NamespacedKey);
	const FString Script = FString::Printf(
		TEXT("document.dispatchEvent(new CustomEvent(%s,{detail:%s}));"),
		*EscapedName, *Json);

	Owner->QueueHudEventScript(Script);
}

// ══════════════════════════════════════════════════════════════════════════════
// Command Runtime — function-backed navigation events
// ══════════════════════════════════════════════════════════════════════════════

void USwuiSubsystem::RebuildCommandRuntime()
{
	FunctionCommands.Reset();

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Cls = *It;
		if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;
		if (Cls->GetName().StartsWith(TEXT("SKEL_")) || Cls->GetName().StartsWith(TEXT("REINST_"))) continue;

		for (TFieldIterator<UFunction> FnIt(Cls, EFieldIteratorFlags::ExcludeSuper); FnIt; ++FnIt)
		{
			const FString TagStr = FnIt->GetMetaData(TEXT("SwuiEvent"));
			if (TagStr.IsEmpty()) continue;

			FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), /*bErrorIfNotFound=*/false);
			if (!Tag.IsValid()) continue;

			// Validate: no return value, no out params.
			if (FnIt->GetReturnProperty() != nullptr)
			{
				UE_LOG(LogTemp, Error, TEXT("SWUI: UFUNCTION %s::%s has SwuiEvent metadata but returns a value. "
					"Function-backed commands must be void/delegate-returning."),
					*Cls->GetName(), *FnIt->GetName());
				continue;
			}
			bool bHasOutParam = false;
			for (TFieldIterator<FProperty> ParamIt(*FnIt); ParamIt; ++ParamIt)
			{
				if (ParamIt->HasAnyPropertyFlags(CPF_OutParm) && !ParamIt->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					bHasOutParam = true;
					break;
				}
			}
			if (bHasOutParam)
			{
				UE_LOG(LogTemp, Error, TEXT("SWUI: UFUNCTION %s::%s has SwuiEvent but contains out params. "
					"Function-backed commands must be fire-and-forget."),
					*Cls->GetName(), *FnIt->GetName());
				continue;
			}

			// Duplicate tag check across UFUNCTIONs.
			if (FunctionCommands.Contains(Tag))
			{
				const FSwuiFunctionCommand& Existing = FunctionCommands[Tag];
				UE_LOG(LogTemp, Error, TEXT("SWUI: Duplicate SwuiEvent tag '%s' — "
					"already registered on %s::%s, now also found on %s::%s. "
					"Each SwuiEvent tag must be unique."),
					*TagStr,
					*Existing.OwnerClass->GetName(), *Existing.Function->GetName(),
					*Cls->GetName(), *FnIt->GetName());
				continue;
			}

			FSwuiFunctionCommand Cmd;
			Cmd.Tag = Tag;
			Cmd.OwnerClass = Cls;
			Cmd.Function = *FnIt;
			FunctionCommands.Add(Tag, Cmd);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("SWUI: Command runtime rebuilt — %d function-backed command(s) registered."),
		FunctionCommands.Num());
}

bool USwuiSubsystem::TryResolveFunctionCommand(FGameplayTag Tag, FSwuiFunctionCommand& OutCommand) const
{
	if (const FSwuiFunctionCommand* Found = FunctionCommands.Find(Tag))
	{
		OutCommand = *Found;
		return true;
	}
	return false;
}

bool USwuiSubsystem::TryResolveActiveBindingTarget(UClass* RequiredClass, UObject*& OutTarget, FString& OutError) const
{
	OutTarget = nullptr;
	OutError.Empty();

	if (!RequiredClass)
	{
		OutError = TEXT("RequiredClass is null.");
		return false;
	}

	// ── Subsystem resolution ────────────────────────────────────────────
	if (RequiredClass->IsChildOf<UGameInstanceSubsystem>())
	{
		UGameInstance* GI = GetGameInstance();
		if (!GI)
		{
			OutError = TEXT("No GameInstance available.");
			return false;
		}
		OutTarget = GI->GetSubsystemBase(RequiredClass);
		if (!OutTarget)
		{
			OutError = FString::Printf(TEXT("GameInstance subsystem %s not found."), *RequiredClass->GetName());
			return false;
		}
		return true;
	}

	if (RequiredClass->IsChildOf<UWorldSubsystem>())
	{
		UWorld* World = GetWorld();
		if (!World)
		{
			OutError = TEXT("No World available.");
			return false;
		}
		OutTarget = World->GetSubsystemBase(RequiredClass);
		if (!OutTarget)
		{
			OutError = FString::Printf(TEXT("World subsystem %s not found."), *RequiredClass->GetName());
			return false;
		}
		return true;
	}

	if (RequiredClass->IsChildOf<ULocalPlayerSubsystem>())
	{
		const UGameInstance* GI = GetGameInstance();
		const ULocalPlayer* LP = GI ? GI->GetFirstGamePlayer() : nullptr;
		if (!LP)
		{
			OutError = TEXT("No LocalPlayer available.");
			return false;
		}
		OutTarget = LP->GetSubsystemBase(RequiredClass);
		if (!OutTarget)
		{
			OutError = FString::Printf(TEXT("LocalPlayer subsystem %s not found."), *RequiredClass->GetName());
			return false;
		}
		return true;
	}

	// ── Active binding source inst-ances ────────────────────────────────
	int32 MatchCount = 0;
	for (const FSwuiObservedProperty& Obs : ObservedProperties)
	{
		UObject* Obj = Obs.Source.Get();
		if (!Obj) continue;
		if (!Obj->IsA(RequiredClass)) continue;

		if (OutTarget == nullptr)
			OutTarget = Obj;
		else if (OutTarget != Obj)
			++MatchCount;
	}

	if (MatchCount > 1)
	{
		OutError = FString::Printf(TEXT("Multiple active instances found for %s — command dispatch is ambiguous."),
			*RequiredClass->GetName());
		OutTarget = nullptr;
		return false;
	}

	if (!OutTarget)
	{
		// Also check ObservedDelegates for instances.
		for (const FSwuiObservedDelegate& Obs : ObservedDelegates)
		{
			UObject* Obj = Obs.Source.Get();
			if (!Obj) continue;
			if (!Obj->IsA(RequiredClass)) continue;

			if (OutTarget == nullptr)
				OutTarget = Obj;
			else if (OutTarget != Obj)
				++MatchCount;
		}

		if (MatchCount > 1)
		{
			OutError = FString::Printf(TEXT("Multiple active instances found for %s — command dispatch is ambiguous."),
				*RequiredClass->GetName());
			OutTarget = nullptr;
			return false;
		}
	}

	if (!OutTarget)
	{
		OutError = FString::Printf(TEXT("No active binding-source instance found for %s."), *RequiredClass->GetName());
		return false;
	}

	return true;
}
