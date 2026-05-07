#pragma once

#include "UObject/Object.h"
#include "SwuiTypes.h"
#include "SwuiView.generated.h"

struct FSwuiViewCefData;

UCLASS(ClassGroup=Swui, Blueprintable)
class SWUIRUNTIME_API USwuiView : public UObject, public ISwuiRenderTarget
{
	GENERATED_BODY()

public:
	USwuiView();

	// Internal URL loaded by the backend adapter — not set directly by users.
	FString DefaultURL;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	int32 Width = 1280;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	int32 Height = 720;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	bool bIsTransparent = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	UMaterialInterface* BaseMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="SwuiRuntime")
	FName TextureParameterName = "SwuiTexture";

	void Init();

	// Internal — called by the backend adapter, not directly from Blueprint.
	void LoadURL(const FString& URI);

	// Internal — called by the backend adapter, not directly from Blueprint.
	void ExecuteJavaScript(const FString& Script);

	UFUNCTION(BlueprintCallable, Category="SwuiRuntime")
	UTexture2D* GetTexture() const;

	// ISwuiRenderTarget
	virtual void OnPaint(const void* Buffer, FUpdateTextureRegion2D* Regions, int32 RegionCount, int32 Width, int32 Height) override;
	virtual UTexture2D* GetOrCreateTexture(int32 InWidth, int32 InHeight) override;

	virtual void BeginDestroy() override;

private:
	UPROPERTY()
	UTexture2D* Texture;

	UMaterialInstanceDynamic* MaterialInstance;

	TSharedPtr<FSwuiViewCefData> CefData;

	void ResetTexture();
	void DestroyTexture();
	void ResetMatInstance();
};
