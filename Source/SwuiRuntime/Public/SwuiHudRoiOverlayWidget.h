#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "SwuiTypes.h"
#include "SwuiHudRoiOverlayWidget.generated.h"

enum { ESwuiRoiOverlayMaxRects = 8 };

UCLASS(meta=(DisableNativeTick))
class SWUIRUNTIME_API USwuiHudRoiOverlayWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	USwuiHudRoiOverlayWidget(const FObjectInitializer& ObjectInitializer);

	void UpdateOverlay(const FSwuiHudRoiOverlayState& State, int32 TexW, int32 TexH, bool bShadeInactive);

protected:
	virtual void NativeOnInitialized() override;

private:
	void CreateBorderImage(const FString& Name, UCanvasPanel* Parent, UImage*& OutImage);
	void UpdateRectImage(UImage* Image, const FIntRect& ViewportRect, const FLinearColor& Color);
	void SyncBorderList(UImage** BorderArray, const TArray<FIntRect>& Rects, const FLinearColor& Color, float ScaleX, float ScaleY);

	UPROPERTY()
	UCanvasPanel* RootPanel = nullptr;

	// Pooled border images (green for outer, cyan for center).
	static constexpr int32 MaxBorders = ESwuiRoiOverlayMaxRects;
	UPROPERTY()
	UImage* OuterBorders[MaxBorders];

	UPROPERTY()
	UImage* CenterBorders[MaxBorders];

	// Shade block behind the dead zone (percentage mode) or outside outer rect (manual).
	UPROPERTY()
	UImage* DeadZoneShade = nullptr;

	UPROPERTY()
	UTextBlock* ModeText = nullptr;
};
