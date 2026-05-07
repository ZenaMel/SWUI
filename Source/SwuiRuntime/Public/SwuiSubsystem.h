#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "UObject/UnrealType.h"
#include "UObject/Field.h"
#include "SwuiSubsystem.generated.h"

class USwuiView;
class UUserWidget;

// ---- Internal registry entries ----

struct FSwuiObservedProperty
{
	TWeakObjectPtr<UObject> Source;
	FString                 NamespacedKey; // "player.Health"
	FName                   PropertyName;  // "Health"
	FProperty*              CachedProp;    // resolved once at Observe time
};

struct FSwuiObservedDelegate
{
	TWeakObjectPtr<UObject> Source;
	FString                 NamespacedKey; // "player.OnTakeDamage"
	FName                   DelegateName;
	// Payload field names + types cached at bind time for fast dispatch + codegen
	TArray<TTuple<FName, FString>> PayloadFields; // (FieldName, TSType)
};

// ---- Subsystem ----

UCLASS()
class SWUIRUNTIME_API USwuiSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ---- Renderer ----
	// Called by USwui component on BeginPlay.
	void InitRenderer(const FString& URI, const FString& InterfaceName,
		bool bIsHUD, int32 Width, int32 Height, int32 ZOrder,
		UMaterialInterface* BaseMaterial, FName TextureParamName);

	void ShutdownRenderer();

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void LoadURI(const FString& URI);

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void ExecuteJavaScript(const FString& Script);

	// ---- Observe API (called from FunctionLibrary BP nodes) ----

	// Register a property to be synced to the web UI each tick.
	// Namespace defaults to the source class name (lowercased) if empty.
	void ObserveProperty(UObject* Source, const FString& Namespace, const FName& PropertyName);

	// Bind to a BlueprintAssignable delegate — fires a JS CustomEvent when it broadcasts.
	// Namespace defaults to the source class name (lowercased) if empty.
	void ObserveDelegate(UObject* Source, const FString& Namespace, const FName& DelegateName);

	// Remove all observations for a source object.
	void Unobserve(UObject* Source);

	// Access the cached delegate payload shapes (used by TS codegen).
	const TArray<FSwuiObservedDelegate>& GetObservedDelegates() const { return ObservedDelegates; }
	const TArray<FSwuiObservedProperty>& GetObservedProperties() const { return ObservedProperties; }

private:
	UPROPERTY()
	USwuiView* View = nullptr;

	UPROPERTY()
	UUserWidget* Widget = nullptr;

	TArray<FSwuiObservedProperty> ObservedProperties;
	TArray<FSwuiObservedDelegate> ObservedDelegates;

	FTimerHandle StateTickHandle;

	void TickState();
	FString ResolveNamespace(UObject* Source, const FString& Namespace) const;

	// Dynamic delegate sink — one per observed delegate binding
	UFUNCTION()
	void OnObservedDelegateFired();
};
