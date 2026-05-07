#include "SwuiHudElement.h"
#include "SwuiView.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
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
	// Init is driven by USwuiBridge after binding asset resolution.
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

void USwuiHudElement::Init(const FString& URI)
{
	if (!GetWorld())
	{
		return;
	}

	if (bIsHUD && GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		// Use the actual render resolution, not the DPI-scaled window size
		FIntPoint RenderSize = GEngine->GameViewport->Viewport->GetSizeXY();
		if (RenderSize.X > 0 && RenderSize.Y > 0)
		{
			ViewWidth  = RenderSize.X;
			ViewHeight = RenderSize.Y;
		}
	}

	FString ResolvedURI = URI.IsEmpty() ? DefaultURI : URI;

	View = NewObject<USwuiView>(this);
	View->DefaultURL = ResolvedURI;
	View->Width = ViewWidth;
	View->Height = ViewHeight;
	View->bIsTransparent = true;
	View->BaseMaterial = BaseMaterial;
	View->TextureParameterName = TextureParameterName;
	View->Init();

	Widget = CreateWidget<UUserWidget>(GetWorld(), USwuiWidget::StaticClass());
	if (!Widget)
	{
		return;
	}

	UCanvasPanel* RootPanel = NewObject<UCanvasPanel>(Widget);
	RootPanel->bIsVariable = false;

	UImage* Image = NewObject<UImage>(Widget);
	Image->SetBrushFromTexture(View->GetTexture());
	FSlateBrush NewBrush = Image->GetBrush();
	NewBrush.ImageSize = FVector2D(ViewWidth, ViewHeight);
	NewBrush.DrawAs = ESlateBrushDrawType::Image;
	Image->SetBrush(NewBrush);
	Image->SynchronizeProperties();

	UPanelSlot* Slot = RootPanel->AddChild(Image);
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
	{
		if (bIsHUD)
		{
			// Stretch to fill the entire viewport
			CanvasSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
			CanvasSlot->SetOffsets(FMargin(0.f));
		}
		else
		{
			CanvasSlot->SetAnchors(FAnchors(0.5f, 0.5f, 0.5f, 0.5f));
			CanvasSlot->SetPosition(FVector2D(0, 0));
			CanvasSlot->SetSize(FVector2D(ViewWidth, ViewHeight));
			CanvasSlot->SetAutoSize(false);
		}
	}

	Widget->WidgetTree->RootWidget = RootPanel;
	Widget->SetIsFocusable(false);

	Widget->AddToViewport(ZOrder);
}

void USwuiHudElement::LoadURI(const FString& URI)
{
	if (View)
	{
		View->LoadURL(URI);
	}
}

void USwuiHudElement::ExecuteJavaScript(const FString& Script)
{
	if (View)
	{
		View->ExecuteJavaScript(Script);
	}
}
