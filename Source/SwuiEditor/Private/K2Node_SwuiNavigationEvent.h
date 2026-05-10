#pragma once

#include "GameplayTagContainer.h"
#include "K2Node.h"
#include "K2Node_SwuiNavigationEvent.generated.h"

/**
 * Filtered wrapper over USwuiNavigation::OnNavigationEvent.
 * Compiles down to the real component-bound delegate event plus an exact tag check.
 */
UCLASS()
class UK2Node_SwuiNavigationEvent : public UK2Node
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FName ComponentPropertyName;

	UPROPERTY(EditAnywhere, Category="SWUI")
	FGameplayTag NavigationEventTag;

	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FText GetMenuCategory() const override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	virtual bool IsNodePure() const override { return false; }
	virtual bool NodeCausesStructuralBlueprintChange() const override { return true; }
};