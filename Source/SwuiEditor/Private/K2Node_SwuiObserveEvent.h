#pragma once

#include "K2Node.h"
#include "K2Node_SwuiObserveEvent.generated.h"

/**
 * "SWUI Observe Event" Blueprint node.
 * Shows a delegate dropdown populated from whatever class is connected to the Source pin.
 * Expands at compile-time into USwuiSubsystem::K2_ObserveEvent.
 */
UCLASS()
class UK2Node_SwuiObserveEvent : public UK2Node
{
	GENERATED_BODY()

public:
	/** Delegate name chosen in the dropdown. Serialised on the node asset. */
	UPROPERTY()
	FName SelectedDelegate;

	//~ UK2Node interface
	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FText GetMenuCategory() const override;
	virtual void  GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual void  ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	virtual void  PinConnectionListChanged(UEdGraphPin* Pin) override;
	virtual bool  IsNodePure() const override { return false; }
	//~ End UK2Node interface

	UEdGraphPin*  GetSourcePin() const;
	UClass*       GetSourceClass() const;
	TArray<FName> GetAvailableEvents() const;

	/** Called by the node factory to build the Slate widget. */
	TSharedPtr<class SGraphNode> CreateVisualWidget();
};
