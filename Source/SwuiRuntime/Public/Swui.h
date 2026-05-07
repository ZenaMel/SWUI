#pragma once

#include "Components/ActorComponent.h"
#include "Blueprint/UserWidget.h"
#include "Swui.generated.h"

/**
 * Minimal concrete UUserWidget subclass used to host the CEF render surface.
 * UUserWidget::StaticClass() is abstract-flagged in some UE builds; this
 * concrete subclass avoids the CreateWidget assertion.
 */
UCLASS()
class SWUIRUNTIME_API USwuiWidget : public UUserWidget
{
	GENERATED_BODY()
};

/**
 * USwui — Add this to any Actor to configure and launch a SimpleWebUI surface.
 * Place on a PlayerController for a HUD, or any Actor for world-space surfaces.
 *
 * To sync game state into the web UI, call "SWUI Observe" Blueprint nodes on
 * any object in the game world — no component needed on those objects.
 */
UCLASS(ClassGroup=Swui, Blueprintable, meta=(BlueprintSpawnableComponent))
class SWUIRUNTIME_API USwui : public UActorComponent
{
	GENERATED_BODY()

public:
	USwui();

	// Used as the TypeScript interface name and generated file prefix.
	// e.g. "PlayerHUD" → Content/UI/generated/PlayerHUD.generated.ts
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI")
	FString InterfaceName = TEXT("MyHUD");

	// URI to load. Bare paths and swui:// resolve under Content/ (.html implicit).
	// http://, https://, and localhost URIs are passed through directly.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI")
	FString DefaultURI;

	// When true the surface automatically matches the game render resolution.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI")
	bool bIsHUD = true;

	// Manual resolution — ignored when bIsHUD is true.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI", meta=(EditCondition="!bIsHUD"))
	int32 ViewWidth = 1280;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI", meta=(EditCondition="!bIsHUD"))
	int32 ViewHeight = 720;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI")
	int32 ZOrder = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI")
	UMaterialInterface* BaseMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="SimpleWebUI")
	FName TextureParameterName = TEXT("SwuiTexture");

	// ---- Bindings ----------------------------------------------------------
	// The class you pass to "SWUI Observe" at runtime (e.g. your Character class).
	// Used ONLY for TypeScript codegen — has zero effect at runtime.
	// Tip: set this to whatever class owns the properties in the checklist below.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Bindings",
		meta=(DisplayName="Codegen Source Class",
		      ToolTip="The class whose properties you are observing via SWUI Observe nodes (e.g. your Character or PlayerState class). Only used to generate TypeScript types — ignored at runtime."))
	TSubclassOf<UObject> CodegenSourceClass;

	// Properties checked in the Details panel checklist.
	// Each entry is registered with the SWUI subsystem on BeginPlay.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SimpleWebUI|Bindings")
	TArray<FName> ExposedProperties;

	// Stop syncing all observed properties/events for a source object.
	// Call from PlayerController's OnUnPossess, passing the old pawn.
	UFUNCTION(BlueprintCallable, Category="SimpleWebUI", meta=(DefaultToSelf="Source"))
	void Unobserve(UObject* Source);

	// Show or hide the web surface. The CEF browser keeps running in the background.
	// Use to suppress the HUD during cutscenes, loading screens, etc.
	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void SetHUDVisible(bool bVisible);

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void ShowHUD() { SetHUDVisible(true); }

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void HideHUD() { SetHUDVisible(false); }

private:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
};
