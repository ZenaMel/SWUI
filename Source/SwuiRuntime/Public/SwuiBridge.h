#pragma once

#include "Components/ActorComponent.h"
#include "SwuiBindingAsset.h"
#include "SwuiBridge.generated.h"

UCLASS(ClassGroup=Swui, Blueprintable, meta=(BlueprintSpawnableComponent))
class SWUIRUNTIME_API USwuiBridge : public UActorComponent
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category="SwuiRuntime")
	void LoadBindings(USwuiBindingAsset* BindingAsset);

	UFUNCTION(BlueprintCallable, Category="SwuiRuntime")
	void RegisterSource(FName SourceName, UObject* SourceObject);

	UFUNCTION(BlueprintCallable, Category="SwuiRuntime")
	void ForceSync();
};
