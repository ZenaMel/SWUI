#pragma once

#include "GameplayTagContainer.h"
#include "K2Node_Event.h"
#include "K2Node_SwuiCommandHook.generated.h"

/**
 * Component-bound hook node for SwuiCommand function-backed commands.
 *
 * IS a UK2Node_Event — the Blueprint compiler sees this node itself as the
 * OnSwuiCommandExecuted event entry point before expansion. No intermediate
 * event node is spawned.
 *
 * Real delegate pins (Command, JsonPayload) are kept internally but hidden.
 * Typed convenience output pins are added from the resolved SwuiCommand
 * UFUNCTION signature. ExpandNode wires hidden pins -> extractors -> typed pins.
 *
 * Stores a serialized snapshot of the command signature (class path, function
 * name, parameter hash) so pin layout is deterministic between compiles.
 */
UCLASS()
class SWUIUNCOOKEDONLY_API UK2Node_SwuiCommandHook : public UK2Node_Event
{
	GENERATED_BODY()

public:
	/** Component variable name this hook is bound to. */
	UPROPERTY()
	FName ComponentPropertyName;

	/** The SwuiCommand tag this node hooks into. */
	UPROPERTY(EditAnywhere, Category = "SWUI")
	FGameplayTag CommandTag;

	/** Stored class path of the UFUNCTION's owner. */
	UPROPERTY()
	FString StoredClassPath;

	/** Stored function name. */
	UPROPERTY()
	FName StoredFunctionName;

	/** Hash of the UFUNCTION's parameter signature. */
	UPROPERTY()
	int32 ParamSignatureHash = 0;

	UFunction* ResolveCommandFunction() const;
	static int32 ComputeFunctionParamHash(UFunction* Function);
	void ResolveAndStoreSignature();

	// UK2Node overrides
	virtual void AllocateDefaultPins() override;
	virtual void ReconstructNode() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual UClass* GetDynamicBindingClass() const override;
	virtual void RegisterDynamicBinding(UDynamicBlueprintBinding* BindingObject) const override;
	virtual void HandleVariableRenamed(UBlueprint* InBlueprint, UClass* InVariableClass, UEdGraph* InGraph, const FName& InOldVarName, const FName& InNewVarName) override;
	virtual bool NodeCausesStructuralBlueprintChange() const override { return true; }
	virtual void ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;

	FMulticastDelegateProperty* GetTargetDelegateProperty() const;

private:
	UEdGraphPin* GetOutputPinForField(FName FieldName) const;

	mutable FNodeTextCache CachedNodeTitle;
};
