#include "SwuiHudRoiOverlayWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/Engine.h"
#include "Styling/CoreStyle.h"

USwuiHudRoiOverlayWidget::USwuiHudRoiOverlayWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	for (int32 i = 0; i < MaxBorders; ++i)
	{
		OuterBorders[i] = nullptr;
		CenterBorders[i] = nullptr;
	}
}

void USwuiHudRoiOverlayWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	RootPanel = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootPanel"));
	WidgetTree->RootWidget = RootPanel;

	// Root fills the entire viewport.
	if (UCanvasPanelSlot* RootSlot = Cast<UCanvasPanelSlot>(RootPanel->Slot))
	{
		RootSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		RootSlot->SetOffsets(FMargin(0.0f));
	}

	for (int32 i = 0; i < MaxBorders; ++i)
	{
		CreateBorderImage(FString::Printf(TEXT("OuterBorder%d"), i), RootPanel, OuterBorders[i]);
		OuterBorders[i]->SetColorAndOpacity(FLinearColor(0.0f, 0.8f, 0.2f, 0.15f));
	}

	for (int32 i = 0; i < MaxBorders; ++i)
	{
		CreateBorderImage(FString::Printf(TEXT("CenterBorder%d"), i), RootPanel, CenterBorders[i]);
		CenterBorders[i]->SetColorAndOpacity(FLinearColor(0.0f, 0.9f, 1.0f, 0.15f));
	}

	DeadZoneShade = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("DeadZoneShade"));
	DeadZoneShade->SetColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.15f));
	DeadZoneShade->SetVisibility(ESlateVisibility::Collapsed);
	RootPanel->AddChild(DeadZoneShade);

	if (UCanvasPanelSlot* ShadeSlot = Cast<UCanvasPanelSlot>(DeadZoneShade->Slot))
	{
		ShadeSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		ShadeSlot->SetOffsets(FMargin(0.0f));
	}

	ModeText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ModeText"));
	ModeText->SetVisibility(ESlateVisibility::HitTestInvisible);
	ModeText->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 18));
	ModeText->SetColorAndOpacity(FLinearColor(1.0f, 1.0f, 0.0f, 1.0f));
	ModeText->SetShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.8f));
	ModeText->SetShadowOffset(FVector2D(1, 1));
	RootPanel->AddChild(ModeText);

	if (UCanvasPanelSlot* TextSlot = Cast<UCanvasPanelSlot>(ModeText->Slot))
	{
		TextSlot->SetAnchors(FAnchors(0.0f, 0.0f, 0.0f, 0.0f));
		TextSlot->SetPosition(FVector2D(8.0f, 8.0f));
		TextSlot->SetSize(FVector2D(600.0f, 200.0f));
		TextSlot->SetAutoSize(true);
	}
}

void USwuiHudRoiOverlayWidget::CreateBorderImage(const FString& Name, UCanvasPanel* Parent, UImage*& OutImage)
{
	OutImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), FName(*Name));
	OutImage->SetVisibility(ESlateVisibility::Collapsed);
	Parent->AddChild(OutImage);
}

void USwuiHudRoiOverlayWidget::UpdateRectImage(UImage* Image, const FIntRect& LocalRect, const FLinearColor& Color)
{
	if (!Image || !Image->Slot)
	{
		return;
	}

	if (LocalRect.Area() <= 0)
	{
		Image->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Image->Slot);
	if (!CanvasSlot)
	{
		return;
	}

	Image->SetColorAndOpacity(Color);
	Image->SetVisibility(ESlateVisibility::HitTestInvisible);

	CanvasSlot->SetAnchors(FAnchors(0.0f, 0.0f, 0.0f, 0.0f));
	CanvasSlot->SetPosition(FVector2D(LocalRect.Min.X, LocalRect.Min.Y));
	CanvasSlot->SetSize(FVector2D(LocalRect.Width(), LocalRect.Height()));
}

void USwuiHudRoiOverlayWidget::SyncBorderList(
	UImage** BorderArray,
	const TArray<FIntRect>& Rects,
	const FLinearColor& Color,
	float ScaleX,
	float ScaleY)
{
	const int32 Count = FMath::Min(Rects.Num(), MaxBorders);

	for (int32 i = 0; i < Count; ++i)
	{
		FIntRect Local;
		Local.Min.X = FMath::RoundToInt(Rects[i].Min.X * ScaleX);
		Local.Min.Y = FMath::RoundToInt(Rects[i].Min.Y * ScaleY);
		Local.Max.X = FMath::RoundToInt(Rects[i].Max.X * ScaleX);
		Local.Max.Y = FMath::RoundToInt(Rects[i].Max.Y * ScaleY);

		UpdateRectImage(BorderArray[i], Local, Color);
	}

	for (int32 i = Count; i < MaxBorders; ++i)
	{
		BorderArray[i]->SetVisibility(ESlateVisibility::Collapsed);
	}
}

void USwuiHudRoiOverlayWidget::UpdateOverlay(
	const FSwuiHudRoiOverlayState& State,
	int32 TexW,
	int32 TexH,
	bool bShadeInactive)
{
	if (!RootPanel)
	{
		return;
	}

	if (!State.bVisible || !State.bHudRoiModeActive)
	{
		SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	// Use the widget's own local geometry (DPI-corrected Slate space).
	const FVector2D OverlaySize = GetCachedGeometry().GetLocalSize();

	const float ScaleX = (TexW > 0) ? OverlaySize.X / float(TexW) : 1.0f;
	const float ScaleY = (TexH > 0) ? OverlaySize.Y / float(TexH) : 1.0f;

	const int32 OuterCount  = FMath::Min(State.OuterRoiRects.Num(), MaxBorders);
	const int32 CenterCount = FMath::Min(State.CenterRoiRects.Num(), MaxBorders);

	SyncBorderList(OuterBorders, State.OuterRoiRects,  FLinearColor(0.0f, 0.8f, 0.2f, 0.85f), ScaleX, ScaleY);
	SyncBorderList(CenterBorders, State.CenterRoiRects, FLinearColor(0.0f, 0.9f, 1.0f, 0.85f), ScaleX, ScaleY);

	if (bShadeInactive)
	{
		DeadZoneShade->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
	else
	{
		DeadZoneShade->SetVisibility(ESlateVisibility::Collapsed);
	}

	if (ModeText)
	{
		FString Text = FString::Printf(TEXT("SWUI ROI: %s\n%s"), *State.ModeLabel,
			State.bHudRoiModeActive ? TEXT("HUD ROI") : TEXT("Full Surface"));

		if (State.bHudRoiModeActive)
		{
			Text += FString::Printf(TEXT("\nOuter: %d rect(s)"), OuterCount);
			Text += FString::Printf(TEXT("\nCenter: %d rect(s)"), CenterCount);
			Text += FString::Printf(TEXT("\nROI Area: %.1f%%"), State.RoiAreaPercent);
		}

		ModeText->SetText(FText::FromString(Text));
	}

	SetVisibility(ESlateVisibility::HitTestInvisible);
}
