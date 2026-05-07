#pragma once

#include "Components/ActorComponent.h"
#include "Blueprint/UserWidget.h"
#include "Swui.generated.h"

class USwuiView;

UCLASS()
class SWUIRUNTIME_API USwuiWidget : public UUserWidget
{
	GENERATED_BODY()
};

/**
 * USwui — Add this to any Actor to render a web UI and sync
 * reflected game state into it. Configure which properties to expose
 * via the "Web UI Bindings" section in the Details panel.
 */
UCLASS(ClassGroup=Swui, Blueprintable, meta=(BlueprintSpawnableComponent))
class SWUIRUNTIME_API USwui : public UActorComponent
{
	GENERATED_BODY()

public:
	USwui();

	// ---- Display ----

	// Used as the TypeScript interface name and generated file name.
	// e.g. "MainHUD" → Content/UI/generated/MainHUD.generated.ts
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI")
	FString InterfaceName = TEXT("MyHUD");

	// URI to load. Bare paths (e.g. "UI/hud") resolve under Content/ with .html implicit.
	// http://, https://, and localhost URIs are passed through directly.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI")
	FString DefaultURI;

	// When enabled, the web surface automatically matches the game render resolution.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI")
	bool bIsHUD = true;

	// Ignored when bIsHUD is true.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI", meta=(EditCondition="!bIsHUD"))
	int32 ViewWidth = 1280;

	// Ignored when bIsHUD is true.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI", meta=(EditCondition="!bIsHUD"))
	int32 ViewHeight = 720;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI")
	int32 ZOrder = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI")
	UMaterialInterface* BaseMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="SimpleWebUI")
	FName TextureParameterName = TEXT("SwuiTexture");

	// ---- Bindings ----

	// The class whose properties appear in the Web UI Bindings checklist.
	// Set this to your Character, PlayerController, or other game class.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Bindings")
	TSubclassOf<AActor> BindingSourceClass;

	// Properties checked in the Details panel checklist — synced to the web UI at runtime.
	// Prefer using the checklist rather than editing this array directly.
	UPROPERTY(EditAnywhere, Category="SimpleWebUI|Bindings")
	TArray<FName> ExposedProperties;

	// How often (seconds) state values are pushed to the web UI. Default: 0.05s (20 Hz).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Bindings", meta=(ClampMin="0.016"))
	float StateSyncInterval = 0.05f;

	// ---- Blueprint API ----

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void LoadURI(const FString& URI);

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void ExecuteJavaScript(const FString& Script);

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	USwuiView* GetView() const { return View; }

private:
	UPROPERTY()
	USwuiView* View = nullptr;

	UPROPERTY()
	UUserWidget* Widget = nullptr;

	FTimerHandle StateTickHandle;

	void InitView();
	void PushStateToJS();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
};
