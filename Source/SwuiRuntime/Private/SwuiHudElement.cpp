#include "SwuiHudElement.h"
#include "SwuiView.h"
#include "Blueprint/UserWidget.h"
#include "Components/Image.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Engine/Engine.h"
#include "UObject/UObjectGlobals.h"
#include "Components/Widget.h"

USwuiHudElement::USwuiHudElement()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void USwuiHudElement::BeginPlay()
{
	Super::BeginPlay();

	if (!DefaultURL.IsEmpty())
	{
		Init();
	}
}

void USwuiHudElement::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Widget && Widget->IsInViewport())
	{
		Widget->RemoveFromParent();
		Widget = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

void USwuiHudElement::Init()
{
	if (!GetWorld())
	{
		return;
	}

	View = NewObject<USwuiView>(this);
	View->DefaultURL = DefaultURL;
	View->Width = ViewWidth;
	View->Height = ViewHeight;
	View->bIsTransparent = true;
	View->BaseMaterial = BaseMaterial;
	View->TextureParameterName = TextureParameterName;
	View->Init();

	Widget = CreateWidget<UUserWidget>(GetWorld(), UUserWidget::StaticClass());
	if (!Widget)
	{
		return;
	}

	UCanvasPanel* RootPanel = NewObject<UCanvasPanel>(Widget);
	RootPanel->bIsVariable = false;

	UImage* Image = NewObject<UImage>(Widget);
	Image->SetBrushFromTexture(View->GetTexture());
	Image->Brush.ImageSize = FVector2D(ViewWidth, ViewHeight);
	Image->Brush.DrawAs = ESlateBrushDrawType::Image;
	Image->SynchronizeProperties();

	UPanelSlot* Slot = RootPanel->AddChild(Image);
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
	{
		CanvasSlot->SetAnchors(FAnchors(0.5f, 0.5f, 0.5f, 0.5f));
		CanvasSlot->SetPosition(FVector2D(0, 0));
		CanvasSlot->SetSize(FVector2D(ViewWidth, ViewHeight));
		CanvasSlot->SetAutoSize(false);
	}

	Widget->WidgetTree->RootWidget = RootPanel;
	Widget->bIsFocusable = false;

	Widget->AddToViewport(ZOrder);
}

void USwuiHudElement::LoadURL(const FString& URL)
{
	if (View)
	{
		View->LoadURL(URL);
	}
}

void USwuiHudElement::ExecuteJavaScript(const FString& Script)
{
	if (View)
	{
		View->ExecuteJavaScript(Script);
	}
}
