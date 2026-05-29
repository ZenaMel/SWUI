#pragma once

#include "GameplayTagContainer.h"
#include "K2Node_Event.h"
#include "K2Node_SwuiNavigationEvent.generated.h"

class FCompilerResultsLog;
class FKismetCompilerContext;
class FMulticastDelegateProperty;
class UBlueprint;
class UBlueprintActionDatabaseRegistrar;
class UClass;
class UDynamicBlueprintBinding;
class UEdGraph;

/**
 * Typed navigation event node for USwuiNavigation.
 *
 * Every navigation tag MUST have a PayloadStruct resolved from the owning
 * Blueprint component's NavigationEvents array. Events without a PayloadStruct
 * trigger a compile error — use FSwuiEmptyPayload for no-payload events.
 *
 * Node pins:
 *   Exec         — fires when the tag matches
 *   Payload      — struct-by-value output (the deserialized PayloadStruct)
 *
 * Internally binds to USwuiNavigation::OnNavigationEvent, filters by tag,
 * deserializes JsonPayload into the resolved PayloadStruct via
 * USwuiJsonBlueprintLibrary, and routes the typed struct to the Payload pin.
 */
UCLASS()
class SWUIUNCOOKEDONLY_API UK2Node_SwuiNavigationEvent : public UK2Node_Event
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FName ComponentPropertyName;

	/** The navigation tag this node listens for.
	 *  PayloadStruct is resolved from the component's NavigationEvents array. */
	UPROPERTY(EditAnywhere, Category="SWUI")
	FGameplayTag NavigationEventTag;

	virtual bool Modify(bool bAlwaysMarkDirty = true) override;
	virtual void AllocateDefaultPins() override;
	virtual void ReconstructNode() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FText GetMenuCategory() const override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual UClass* GetDynamicBindingClass() const override;
	virtual void RegisterDynamicBinding(UDynamicBlueprintBinding* BindingObject) const override;
	virtual void HandleVariableRenamed(UBlueprint* InBlueprint, UClass* InVariableClass, UEdGraph* InGraph, const FName& InOldVarName, const FName& InNewVarName) override;
	virtual void ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;

	bool HasValidBindingData(FText* OutError = nullptr) const;

private:
	void InitializeDelegateSignature();
	FMulticastDelegateProperty* GetTargetDelegateProperty() const;
	FName BuildCustomFunctionName() const;

	/** Resolve PayloadStruct from NavigationEvents array.
	 *  Falls back to FSwuiEmptyPayload::StaticStruct() if the tag has no
	 *  explicit struct — every event node must have a typed output. */
	const UScriptStruct* ResolvePayloadStruct() const;

	mutable FNodeTextCache CachedNodeTitle;
};
