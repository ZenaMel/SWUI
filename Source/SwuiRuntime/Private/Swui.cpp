#include "Swui.h"
#include "SwuiView.h"
#include "ISwuiRuntime.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Image.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Engine/Engine.h"
#include "UObject/UnrealType.h"
#include "UObject/Field.h"

USwui::USwui()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void USwui::BeginPlay()
{
	Super::BeginPlay();
	InitView();

	if (ExposedProperties.Num() > 0)
	{
		GetWorld()->GetTimerManager().SetTimer(
			StateTickHandle, this, &USwui::PushStateToJS,
			StateSyncInterval, true);
	}
}

void USwui::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorld()->GetTimerManager().ClearTimer(StateTickHandle);

	if (Widget && Widget->IsInViewport())
	{
		Widget->RemoveFromParent();
		Widget = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

void USwui::InitView()
{
	if (!GetWorld()) return;

	if (bIsHUD && GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		FIntPoint RenderSize = GEngine->GameViewport->Viewport->GetSizeXY();
		if (RenderSize.X > 0 && RenderSize.Y > 0)
		{
			ViewWidth  = RenderSize.X;
			ViewHeight = RenderSize.Y;
		}
	}

	View = NewObject<USwuiView>(this);
	View->DefaultURL = DefaultURI;
	View->Width = ViewWidth;
	View->Height = ViewHeight;
	View->bIsTransparent = true;
	View->BaseMaterial = BaseMaterial;
	View->TextureParameterName = TextureParameterName;
	View->Init();

	Widget = CreateWidget<UUserWidget>(GetWorld(), USwuiWidget::StaticClass());
	if (!Widget) return;

	UCanvasPanel* RootPanel = NewObject<UCanvasPanel>(Widget);
	RootPanel->bIsVariable = false;

	UImage* Image = NewObject<UImage>(Widget);
	Image->SetBrushFromTexture(View->GetTexture());
	FSlateBrush Brush = Image->GetBrush();
	Brush.ImageSize = FVector2D(ViewWidth, ViewHeight);
	Brush.DrawAs = ESlateBrushDrawType::Image;
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
			CanvasSlot->SetSize(FVector2D(ViewWidth, ViewHeight));
			CanvasSlot->SetAutoSize(false);
		}
	}

	Widget->WidgetTree->RootWidget = RootPanel;
	Widget->SetIsFocusable(false);
	Widget->AddToViewport(ZOrder);
}

void USwui::LoadURI(const FString& URI)
{
	if (View) View->LoadURL(URI);
}

void USwui::ExecuteJavaScript(const FString& Script)
{
	if (View) View->ExecuteJavaScript(Script);
}

// ---------------------------------------------------------------------------
// State sync
// ---------------------------------------------------------------------------

static FString SwuiSerializeProperty(FProperty* Prop, void* Container)
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
	if (const FNameProperty* P = CastField<FNameProperty>(Prop))
		return FString::Printf(TEXT("'%s'"), *P->GetPropertyValue(Value).ToString());
	if (const FTextProperty* P = CastField<FTextProperty>(Prop))
		return FString::Printf(TEXT("'%s'"), *P->GetPropertyValue(Value).ToString());

	return FString(); // unsupported type
}

void USwui::PushStateToJS()
{
	if (!View || ExposedProperties.Num() == 0) return;

	// Source object: prefer BindingSourceClass-matching owner, fallback to raw owner
	AActor* Owner = GetOwner();
	if (!Owner) return;

	// Build a single batched JS call
	FString Script = TEXT("(function(){var s=(window.__SWUI__=window.__SWUI__||{state:{}});");
	bool bAny = false;

	for (const FName& PropName : ExposedProperties)
	{
		FProperty* Prop = Owner->GetClass()->FindPropertyByName(PropName);
		if (!Prop) continue;

		const FString JSValue = SwuiSerializeProperty(Prop, Owner);
		if (JSValue.IsEmpty()) continue;

		Script += FString::Printf(
			TEXT("s.state['%s']=%s;if(s._notify)s._notify('%s',%s);"),
			*PropName.ToString(), *JSValue,
			*PropName.ToString(), *JSValue);
		bAny = true;
	}

	Script += TEXT("})();");

	if (bAny)
	{
		View->ExecuteJavaScript(Script);
	}
}
