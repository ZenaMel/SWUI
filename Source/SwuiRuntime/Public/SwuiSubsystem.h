#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "UObject/UnrealType.h"
#include "UObject/Field.h"
#include "SwuiBindingSource.h"
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
class SWUIRUNTIME_API USwuiSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	// UGameInstanceSubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	// FTickableGameObject — ticks every engine frame, no timer needed
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override { return !IsTemplate() && View != nullptr; }
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(USwuiSubsystem, STATGROUP_Tickables); }
	virtual void Deinitialize() override;

	// ---- Renderer ----
	// Called by USwui component on BeginPlay.
	void InitRenderer(const FString& URI, const FString& InterfaceName,
		bool bIsHUD, int32 Width, int32 Height, int32 ZOrder,
		UMaterialInterface* BaseMaterial, FName TextureParamName);

	void ShutdownRenderer();

	/** Immediately shuts down SWUI rendering. Safe to call at BeginPlay on any actor.
	 *  Prevents any further InitRenderer calls for the lifetime of this game instance. */
	UFUNCTION(BlueprintCallable, Category="SimpleWebUI|Debug")
	void DisablePlugin();

	UFUNCTION(BlueprintPure, Category="SimpleWebUI|Debug")
	bool IsPluginDisabled() const { return bDisabledAtRuntime; }

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void SetWidgetVisible(bool bVisible);

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void LoadURI(const FString& URI);

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void ExecuteJavaScript(const FString& Script);

	// ---- K2Node expansion targets — do not call directly from C++ or Blueprint ----
	// These are the functions the "SWUI Observe" and "SWUI Observe Event" graph nodes
	// expand into at Blueprint compile time. Namespace is always auto-derived.

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true"))
	static void K2_Observe(UObject* Source, FName PropertyName);

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true"))
	static void K2_ObserveEvent(UObject* Source, FName DelegateName);

	// Called by USwui on BeginPlay; stores binding config so ObserveSource can look it up.
	void SetBindingSources(const TArray<FSwuiBindingSource>& Sources);

	// Finds the BindingSource entry whose SourceClass matches Instance's class (or a parent),
	// then calls ObserveProperty for every checked property in that entry.
	// Call this once per instance you want to sync (e.g., in OnPossess for your Character).
	// Slot 0 (owner class) is called automatically by the Swui component — no manual call needed.
	UFUNCTION(BlueprintCallable, Category="SimpleWebUI", meta=(DefaultToSelf="Instance"))
	void ObserveSource(UObject* Instance, bool bWarnOnMiss = true);

	// ---- Public API ----

	void ObserveProperty(UObject* Source, const FString& Namespace, const FName& PropertyName);
	void ObserveDelegate(UObject* Source, const FString& Namespace, const FName& DelegateName);

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI", meta=(DefaultToSelf="Source"))
	void Unobserve(UObject* Source);

	// Access the cached delegate payload shapes (used by TS codegen).
	const TArray<FSwuiObservedDelegate>& GetObservedDelegates() const { return ObservedDelegates; }
	const TArray<FSwuiObservedProperty>& GetObservedProperties() const { return ObservedProperties; }

private:
	bool  bDisabledAtRuntime = false;
	float TickAccumulator    = 0.f; // throttles JS pushes to CEF frame rate

	UPROPERTY()
	USwuiView* View = nullptr;

	UPROPERTY()
	UUserWidget* Widget = nullptr;

	TArray<FSwuiObservedProperty> ObservedProperties;
	TArray<FSwuiObservedDelegate> ObservedDelegates;
	TArray<FSwuiBindingSource>    CachedBindingSources;

	FString ResolveNamespace(UObject* Source, const FString& Namespace) const;

	// Dynamic delegate sink — one per observed delegate binding
	UFUNCTION()
	void OnObservedDelegateFired();
};
