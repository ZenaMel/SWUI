#pragma once

#include "GameplayTagContainer.h"
#include "K2Node.h"
#include "K2Node_SwuiCommandHook.generated.h"

class FCompilerResultsLog;
class FKismetCompilerContext;
class FMulticastDelegateProperty;
class UBlueprint;
class UBlueprintActionDatabaseRegistrar;
class UClass;
class UEdGraph;
struct FSwuiFunctionCommand;

/**
 * Public UX node for function-backed SwuiCommand hooks.
 *
 * Shows typed output pins (one per UFUNCTION input param). Does NOT register
 * its own dynamic binding — during ExpandNode it spawns a hidden intermediate
 * UK2Node_Event that binds to OnSwuiCommandExecuted. The intermediate event
 * owns compiler-compatible delegate binding; this node owns UX and typed pins.
 */
UCLASS()
class SWUIUNCOOKEDONLY_API UK2Node_SwuiCommandHook : public UK2Node
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FName ComponentPropertyName;

	UPROPERTY(EditAnywhere, Category = "SWUI")
	FGameplayTag CommandTag;

	UPROPERTY()
	FString StoredClassPath;

	UPROPERTY()
	FName StoredFunctionName;

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
	virtual FText GetMenuCategory() const override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual UClass* GetDynamicBindingClass() const override { return nullptr; }
	virtual void RegisterDynamicBinding(class UDynamicBlueprintBinding* BindingObject) const override {}
	virtual void ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;

private:
	UEdGraphPin* GetOutputPinForField(FName FieldName) const;

	mutable FNodeTextCache CachedNodeTitle;
};
