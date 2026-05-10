#pragma once

#include "CoreMinimal.h"
#include "SwuiBindingSource.generated.h"

/**
 * One source class entry in the USwui bindings list.
 * Each entry covers one class (Character, Weapon, PlayerController, etc.).
 * The namespace is auto-derived from the class name at codegen + runtime time
 * e.g. ADACharacter -> "dacharacter", ADAWeapon -> "adaweapon".
 */
USTRUCT(BlueprintType)
struct SWUIRUNTIME_API FSwuiBindingSource
{
	GENERATED_BODY()

	// The class to show a property checklist for.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Binding")
	TSubclassOf<UObject> SourceClass;

	// Properties checked in the Details panel for this class.
	// Emitted into the generated TypeScript state interface as namespaced keys.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Binding")
	TArray<FName> Properties;

	// Multicast delegates checked in the Details panel for this class.
	// Emitted as document.addEventListener helpers in the generated TypeScript.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Binding")
	TArray<FName> Delegates;
};
