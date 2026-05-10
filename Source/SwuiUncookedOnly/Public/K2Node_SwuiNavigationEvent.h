#pragma once

#include "GameplayTagContainer.h"
#include "K2Node_Event.h"
#include "K2Node_SwuiNavigationEvent.generated.h"

class FCompilerResultsLog;
class FKismetCompilerContext;
class FMulticastDelegateProperty;
class FNodeHandlingFunctor;
class UBlueprint;
class UBlueprintActionDatabaseRegistrar;
class UClass;
class UDynamicBlueprintBinding;
class UEdGraph;

/**
 * Real component-bound event node for USwuiNavigation::OnNavigationEvent.
 * The visible exec output is gated by an exact GameplayTag check against NavigationEventTag.
 */
UCLASS()
class SWUIUNCOOKEDONLY_API UK2Node_SwuiNavigationEvent : public UK2Node_Event
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FName ComponentPropertyName;

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

	mutable FNodeTextCache CachedNodeTitle;
};
