#pragma once

#include "Components/ActorComponent.h"
#include "Blueprint/UserWidget.h"
#include "SwuiHudElement.generated.h"

class USwuiView;

UCLASS()
class SWUIRUNTIME_API USwuiWidget : public UUserWidget
{
	GENERATED_BODY()
};

UCLASS(ClassGroup=Swui, Blueprintable, meta=(BlueprintSpawnableComponent))
class SWUIRUNTIME_API USwuiHudElement : public UActorComponent
{
	GENERATED_BODY()

public:
	USwuiHudElement();

	// When enabled, ViewWidth and ViewHeight are ignored and the game viewport
	// dimensions are used automatically.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	bool bIsHUD = true;

	// Ignored when bIsHUD is true.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime", meta=(EditCondition="!bIsHUD"))
	int32 ViewWidth = 1280;

	// Ignored when bIsHUD is true.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime", meta=(EditCondition="!bIsHUD"))
	int32 ViewHeight = 720;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	UMaterialInterface* BaseMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="SwuiRuntime")
	FName TextureParameterName = "SwuiTexture";

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	int32 ZOrder = 0;

	// URI to load. Bare paths (e.g. "UI/hud.html") and swui:// URIs both resolve
	// relative to the project directory. http://, https://, and localhost URIs
	// are passed through directly.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	FString DefaultURI;

	UFUNCTION(BlueprintCallable, Category="SwuiRuntime")
	void Init(const FString& URI);

	UFUNCTION(BlueprintCallable, Category="SwuiRuntime")
	void LoadURI(const FString& URI);

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
