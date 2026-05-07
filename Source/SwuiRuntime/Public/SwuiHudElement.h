#pragma once

#include "Components/ActorComponent.h"
#include "SwuiHudElement.generated.h"

class USwuiView;
class UUserWidget;

UCLASS(ClassGroup=Swui, Blueprintable, meta=(BlueprintSpawnableComponent))
class SWUIRUNTIME_API USwuiHudElement : public UActorComponent
{
	GENERATED_BODY()

public:
	USwuiHudElement();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	FString DefaultURL;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	int32 ViewWidth = 1280;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	int32 ViewHeight = 720;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	UMaterialInterface* BaseMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="SwuiRuntime")
	FName TextureParameterName = "SwuiTexture";

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	int32 ZOrder = 0;

	UFUNCTION(BlueprintCallable, Category="SwuiRuntime")
	void Init();

	UFUNCTION(BlueprintCallable, Category="SwuiRuntime")
	void LoadURL(const FString& URL);

	UFUNCTION(BlueprintCallable, Category="SwuiRuntime")
	void ExecuteJavaScript(const FString& Script);

	UFUNCTION(BlueprintCallable, Category="SwuiRuntime")
	USwuiView* GetView() const { return View; }

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY()
	USwuiView* View;

	UPROPERTY()
	UUserWidget* Widget;
};
