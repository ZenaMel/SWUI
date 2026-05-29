#pragma once

#include "GameplayTagContainer.h"
#include "K2Node_Event.h"
#include "K2Node_SwuiCommandHook.generated.h"

class FCompilerResultsLog;
class FKismetCompilerContext;
class FMulticastDelegateProperty;
class UBlueprint;
class UBlueprintActionDatabaseRegistrar;
class UClass;
class UDynamicBlueprintBinding;
class UEdGraph;

/**
 * Optionally BP hook node for function-backed SwuiCommand UFUNCTIONs.
 *
 * Observes USwuiNavigation::OnSwuiCommandExecuted, filters by the selected
 * command tag, extracts each SwuiCommand UFUNCTION input parameter from the
 * JSON payload, and exposes them as typed output pins before firing Exec.
 *
 * Pins: one Exec out + one typed output pin per supported UFUNCTION param.
 * No raw JsonPayload pin. No synthesized structs.
 */
UCLASS()
class SWUIUNCOOKEDONLY_API UK2Node_SwuiCommandHook : public UK2Node_Event
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FName ComponentPropertyName;

	/** The SwuiCommand tag this node hooks into. */
	UPROPERTY(EditAnywhere, Category="SWUI")
	FGameplayTag CommandTag;

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

	/** Find the UFUNCTION with meta=(SwuiCommand="CommandTag"). */
	UFunction* ResolveCommandFunction() const;

	/** Return the output pin for a given parameter name, or nullptr. */
	UEdGraphPin* GetOutputPinForField(FName FieldName) const;

	mutable FNodeTextCache CachedNodeTitle;
};
