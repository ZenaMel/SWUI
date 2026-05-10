#pragma once

#include "K2Node.h"
#include "K2Node_SwuiObserve.generated.h"

class FBlueprintActionDatabaseRegistrar;
class FKismetCompilerContext;
class SGraphNode;
class UEdGraph;
class UEdGraphPin;

UCLASS()
class SWUIUNCOOKEDONLY_API UK2Node_SwuiObserve : public UK2Node
{
	GENERATED_BODY()

public:
	/** Currently selected property name, saved with the node. */
	UPROPERTY()
	FName SelectedProperty;

	// UK2Node interface
	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FText GetMenuCategory() const override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual void PinConnectionListChanged(UEdGraphPin* Pin) override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	virtual TSharedPtr<SGraphNode> CreateVisualWidget() override;
	virtual bool IsNodePure() const override { return false; }
	// End UK2Node interface

	/** Returns the Source object input pin. */
	UEdGraphPin* GetSourcePin() const;
	/** Returns the class connected to the Source pin, or nullptr if unconnected. */
	UClass* GetSourceClass() const;
	/** Returns blueprint-visible scalar/string/bool properties of the connected class. */
	TArray<FName> GetAvailableProperties() const;
};
