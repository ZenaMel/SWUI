#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "SwuiFunctionLibrary.generated.h"

class USwuiSubsystem;

UCLASS()
class SWUIRUNTIME_API USwuiFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	// Register a property on any UObject to be synced to the web UI each tick.
	// Namespace defaults to the source class name (e.g. "mycharacter") if left empty.
	// Example: SWUI Observe(Self, "player", "Health")
	UFUNCTION(BlueprintCallable, Category="SimpleWebUI",
		meta=(DefaultToSelf="Source", AdvancedDisplay="Namespace"))
	static void SwuiObserve(UObject* Source, FName PropertyName, FString Namespace = TEXT(""));

	// Bind to a BlueprintAssignable delegate on any UObject.
	// Fires a JS CustomEvent on the page when the delegate broadcasts.
	// Example: SWUI Observe Event(Self, "player", "OnTakeDamage")
	UFUNCTION(BlueprintCallable, Category="SimpleWebUI",
		meta=(DefaultToSelf="Source", AdvancedDisplay="Namespace"))
	static void SwuiObserveEvent(UObject* Source, FName DelegateName, FString Namespace = TEXT(""));

	// Remove all SWUI observations for a source object.
	// Called automatically if the source becomes invalid — only needed for early removal.
	UFUNCTION(BlueprintCallable, Category="SimpleWebUI",
		meta=(DefaultToSelf="Source"))
	static void SwuiUnobserve(UObject* Source);

	// Convenience — get the SWUI subsystem from any context object.
	UFUNCTION(BlueprintCallable, Category="SimpleWebUI",
		meta=(DefaultToSelf="WorldContext", HidePin="WorldContext"))
	static USwuiSubsystem* GetSwuiSubsystem(UObject* WorldContext);
};
