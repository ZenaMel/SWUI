#include "K2Node_SwuiCommandHook.h"

#include "SwuiJsonBlueprintLibrary.h"
#include "SwuiNavigation.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintGameplayTagLibrary.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/ComponentDelegateBinding.h"
#include "Engine/DynamicBlueprintBinding.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "KismetCompiler.h"
#include "Logging/MessageLog.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/Field.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "K2Node_SwuiCommandHook"

// ── Pin names ───────────────────────────────────────────────────────────────
namespace SwuiCmdHookPins
{
	static const FName Exec(UEdGraphSchema_K2::PN_Then);
	static const FName CmdTag(TEXT("CommandTag"));
	static const FName JsonPayload(TEXT("JsonPayload"));
}

// ── Title helpers ───────────────────────────────────────────────────────────
static FString HumanizeTagSegment(const FString& Segment)
{
	FString Result;
	Result.Reserve(Segment.Len() * 2);
	for (int32 Index = 0; Index < Segment.Len(); ++Index)
	{
		const TCHAR Char = Segment[Index];
		const bool bIsSeparator = (Char == TEXT('_')) || (Char == TEXT('-'));
		const bool bInsertSpace = Index > 0 && !bIsSeparator
			&& FChar::IsUpper(Char) && FChar::IsLower(Segment[Index - 1]);
		if (bIsSeparator)
		{
			if (!Result.IsEmpty() && Result[Result.Len() - 1] != TEXT(' '))
				Result.AppendChar(TEXT(' '));
			continue;
		}
		if (bInsertSpace && Result[Result.Len() - 1] != TEXT(' '))
			Result.AppendChar(TEXT(' '));
		Result.AppendChar(Index == 0 ? FChar::ToUpper(Char) : Char);
	}
	return Result.TrimStartAndEnd();
}

static FText MakeNodeTitle(const FGameplayTag& Tag)
{
	if (!Tag.IsValid())
		return LOCTEXT("UnsetNodeTitle", "On Command Hook");
	TArray<FString> Segments;
	Tag.GetTagName().ToString().ParseIntoArray(Segments, TEXT("."), true);
	if (Segments.Num() > 0 && Segments[0].Equals(TEXT("swui"), ESearchCase::IgnoreCase))
		Segments.RemoveAt(0);
	FString Title = TEXT("On");
	for (const FString& Segment : Segments)
		Title += TEXT(" ") + HumanizeTagSegment(Segment);
	Title += TEXT(" Hook");
	return FText::FromString(Title);
}

// ══════════════════════════════════════════════════════════════════════════════
// Lifecycle
// ══════════════════════════════════════════════════════════════════════════════

bool UK2Node_SwuiCommandHook::Modify(bool bAlwaysMarkDirty)
{
	CachedNodeTitle.MarkDirty();
	return Super::Modify(bAlwaysMarkDirty);
}

void UK2Node_SwuiCommandHook::InitializeDelegateSignature()
{
	if (FMulticastDelegateProperty* DelegateProperty = GetTargetDelegateProperty())
	{
		EventReference.SetFromField<UFunction>(DelegateProperty->SignatureFunction, false);
		bOverrideFunction = false;
		bInternalEvent = true;
		if (CustomFunctionName.IsNone())
			CustomFunctionName = BuildCustomFunctionName();
	}
}

void UK2Node_SwuiCommandHook::AllocateDefaultPins()
{
	InitializeDelegateSignature();
	Super::AllocateDefaultPins();

	// Hide the delegate-signature pins — they're internal to ExpandNode.
	if (UEdGraphPin* CmdPin = FindPin(SwuiCmdHookPins::CmdTag))
		CmdPin->bHidden = true;
	if (UEdGraphPin* JsonPin = FindPin(SwuiCmdHookPins::JsonPayload))
		JsonPin->bHidden = true;

	// Create typed output pins from the resolved UFUNCTION params.
	UFunction* Fn = ResolveCommandFunction();
	if (!Fn) return;

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm)) continue;
		const FName PinName = It->GetFName();
		FEdGraphPinType PinType;
		bool bSupported = Schema->ConvertPropertyToPinType(*It, PinType);
		if (!bSupported)
		{
			UE_LOG(LogTemp, Warning, TEXT("SWUI: Command hook '%s' has unsupported param type '%s' for '%s'."),
				*CommandTag.ToString(), *It->GetCPPType(), *PinName.ToString());
			continue;
		}
		UEdGraphPin* OutPin = CreatePin(EGPD_Output, PinType, PinName);
		OutPin->PinFriendlyName = FText::FromName(PinName);
	}
}

void UK2Node_SwuiCommandHook::ReconstructNode()
{
	InitializeDelegateSignature();
	CachedNodeTitle.MarkDirty();
	Super::ReconstructNode();
}

FText UK2Node_SwuiCommandHook::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	if (TitleType == ENodeTitleType::MenuTitle)
		return LOCTEXT("MenuTitle", "SWUI Command Hook");
	return MakeNodeTitle(CommandTag);
}

FText UK2Node_SwuiCommandHook::GetTooltipText() const
{
	if (!CommandTag.IsValid())
		return LOCTEXT("TooltipUnset", "Observes a function-backed SWUI command and provides its typed params.");
	return FText::Format(LOCTEXT("Tooltip", "Hooks into the function-backed command '{0}' after it is executed."),
		FText::FromString(CommandTag.GetTagName().ToString()));
}

FText UK2Node_SwuiCommandHook::GetMenuCategory() const
{
	return LOCTEXT("Category", "SimpleWebUI|Commands");
}

UClass* UK2Node_SwuiCommandHook::GetDynamicBindingClass() const
{
	return UComponentDelegateBinding::StaticClass();
}

// ══════════════════════════════════════════════════════════════════════════════
// Binding
// ══════════════════════════════════════════════════════════════════════════════

void UK2Node_SwuiCommandHook::RegisterDynamicBinding(UDynamicBlueprintBinding* BindingObject) const
{
	UComponentDelegateBinding* ComponentBinding = Cast<UComponentDelegateBinding>(BindingObject);
	if (!ComponentBinding)
	{
		FMessageLog("Blueprint").Error(LOCTEXT("RegBindFailed",
			"SWUI command hook failed to register dynamic binding."));
		return;
	}
	if (ComponentPropertyName.IsNone())
	{
		FMessageLog("Blueprint").Error(LOCTEXT("RegBindMissingComp",
			"SWUI command hook: ComponentPropertyName is missing."));
		return;
	}
	if (!CommandTag.IsValid())
	{
		FMessageLog("Blueprint").Error(LOCTEXT("RegBindMissingTag",
			"SWUI command hook: CommandTag is invalid."));
		return;
	}
	if (!GetTargetDelegateProperty())
	{
		FMessageLog("Blueprint").Error(LOCTEXT("RegBindMissingDel",
			"SWUI command hook: OnSwuiCommandExecuted could not be resolved."));
		return;
	}

	FBlueprintComponentDelegateBinding Binding;
	Binding.ComponentPropertyName = ComponentPropertyName;
	Binding.DelegatePropertyName = GET_MEMBER_NAME_CHECKED(USwuiNavigation, OnSwuiCommandExecuted);
	Binding.FunctionNameToBind = CustomFunctionName;
	ComponentBinding->ComponentDelegateBindings.Add(Binding);
}

void UK2Node_SwuiCommandHook::HandleVariableRenamed(
	UBlueprint* InBlueprint, UClass* InVariableClass, UEdGraph* InGraph,
	const FName& InOldVarName, const FName& InNewVarName)
{
	if (InVariableClass && InBlueprint && InBlueprint->GeneratedClass &&
		InVariableClass->IsChildOf(InBlueprint->GeneratedClass) &&
		InOldVarName == ComponentPropertyName)
	{
		Modify();
		ComponentPropertyName = InNewVarName;
	}
}

// ══════════════════════════════════════════════════════════════════════════════
// Validation
// ══════════════════════════════════════════════════════════════════════════════

bool UK2Node_SwuiCommandHook::HasValidBindingData(FText* OutError) const
{
	auto E = [OutError](const FText& M) { if (OutError) *OutError = M; return false; };
	if (ComponentPropertyName.IsNone())
		return E(LOCTEXT("MissingComponentProperty", "Missing SWUI navigation component property"));
	if (!CommandTag.IsValid())
		return E(LOCTEXT("MissingCommandTag", "Missing valid CommandTag"));
	if (!GetTargetDelegateProperty())
		return E(LOCTEXT("MissingDelegate", "Could not resolve OnSwuiCommandExecuted"));
	if (!ResolveCommandFunction())
		return E(LOCTEXT("MissingFunction", "Could not resolve the SwuiCommand UFUNCTION for this tag"));

	const UBlueprint* BP = GetBlueprint();
	if (!BP) return E(LOCTEXT("MissingBlueprint", "Could not resolve owning Blueprint"));

	const FObjectProperty* Prop = nullptr;
	if (BP->SkeletonGeneratedClass)
		Prop = FindFProperty<FObjectProperty>(BP->SkeletonGeneratedClass, ComponentPropertyName);
	if (!Prop && BP->GeneratedClass)
		Prop = FindFProperty<FObjectProperty>(BP->GeneratedClass, ComponentPropertyName);
	if (!Prop || !Prop->PropertyClass || !Prop->PropertyClass->IsChildOf(USwuiNavigation::StaticClass()))
		return E(LOCTEXT("MissingBoundComponent", "Missing matching USwuiNavigation component property"));
	if (CustomFunctionName.IsNone())
		return E(LOCTEXT("MissingBindingFunction", "Could not generate binding function name"));
	return true;
}

void UK2Node_SwuiCommandHook::ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const
{
	FText ErrorText;
	if (!HasValidBindingData(&ErrorText))
		MessageLog.Error(*FString::Printf(TEXT("%s for @@"), *ErrorText.ToString()), this);

	UFunction* Fn = ResolveCommandFunction();
	if (!Fn)
	{
		MessageLog.Error(*LOCTEXT("NoFunction",
			"@@ could not resolve the SwuiCommand UFUNCTION for this tag.").ToString(), this);
	}
	else
	{
		for (TFieldIterator<FProperty> It(Fn); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm)) continue;
			FEdGraphPinType Tmp;
			if (!GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(*It, Tmp))
			{
				MessageLog.Error(*FString::Printf(
					TEXT("@@ : UFUNCTION param '%s' (type '%s') is not supported as a command-hook output pin."),
					*It->GetName(), *It->GetCPPType()), this);
			}
		}
	}

	Super::ValidateNodeDuringCompilation(MessageLog);
}

// ══════════════════════════════════════════════════════════════════════════════
// Menu actions
// ══════════════════════════════════════════════════════════════════════════════

void UK2Node_SwuiCommandHook::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	UClass* Key = GetClass();
	if (ActionRegistrar.IsOpenForRegistration(Key))
	{
		UBlueprintNodeSpawner* Spawner = UBlueprintNodeSpawner::Create(Key);
		check(Spawner);
		ActionRegistrar.AddBlueprintAction(Key, Spawner);
	}
}

// ══════════════════════════════════════════════════════════════════════════════
// Helpers
// ══════════════════════════════════════════════════════════════════════════════

FMulticastDelegateProperty* UK2Node_SwuiCommandHook::GetTargetDelegateProperty() const
{
	return FindFProperty<FMulticastDelegateProperty>(USwuiNavigation::StaticClass(),
		GET_MEMBER_NAME_CHECKED(USwuiNavigation, OnSwuiCommandExecuted));
}

FName UK2Node_SwuiCommandHook::BuildCustomFunctionName() const
{
	const UBlueprint* BP = GetBlueprint();
	const FString BPName = BP ? BP->GetName() : TEXT("SwuiBlueprint");
	const FName DelName = GET_MEMBER_NAME_CHECKED(USwuiNavigation, OnSwuiCommandExecuted);
	return FName(*FString::Printf(TEXT("BndEvt__%s_%s_%s"), *BPName, *GetName(), *DelName.ToString()));
}

UFunction* UK2Node_SwuiCommandHook::ResolveCommandFunction() const
{
	if (!CommandTag.IsValid()) return nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Cls = *It;
		if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;
		if (Cls->GetName().StartsWith(TEXT("SKEL_")) || Cls->GetName().StartsWith(TEXT("REINST_"))) continue;
		for (TFieldIterator<UFunction> FnIt(Cls, EFieldIteratorFlags::ExcludeSuper); FnIt; ++FnIt)
		{
			const FString CmdStr = FnIt->GetMetaData(TEXT("SwuiCommand"));
			if (CmdStr.IsEmpty()) continue;
			FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*CmdStr), false);
			if (Tag == CommandTag) return *FnIt;
		}
	}
	return nullptr;
}

UEdGraphPin* UK2Node_SwuiCommandHook::GetOutputPinForField(FName FieldName) const
{
	return FindPin(FieldName, EGPD_Output);
}

// ══════════════════════════════════════════════════════════════════════════════
// Expansion
// ══════════════════════════════════════════════════════════════════════════════

void UK2Node_SwuiCommandHook::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);

	FText ErrorText;
	if (!HasValidBindingData(&ErrorText))
	{
		CompilerContext.MessageLog.Error(*FString::Printf(TEXT("%s for @@"), *ErrorText.ToString()), this);
		BreakAllNodeLinks();
		return;
	}

	UFunction* CmdFn = ResolveCommandFunction();
	if (!CmdFn)
	{
		CompilerContext.MessageLog.Error(TEXT("@@ could not resolve SwuiCommand UFUNCTION."), this);
		BreakAllNodeLinks();
		return;
	}

	const UEdGraphSchema_K2* Schema = CompilerContext.GetSchema();

	// ── Tag literal + equality + branch ──────────────────────────────────
	UFunction* LiteralFn = UBlueprintGameplayTagLibrary::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UBlueprintGameplayTagLibrary, MakeLiteralGameplayTag));
	UFunction* EqualFn = UBlueprintGameplayTagLibrary::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UBlueprintGameplayTagLibrary, EqualEqual_GameplayTag));
	if (!LiteralFn || !EqualFn)
	{
		CompilerContext.MessageLog.Error(TEXT("@@ could not resolve GameplayTag BP helpers."), this);
		BreakAllNodeLinks();
		return;
	}

	UK2Node_CallFunction* LitNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	LitNode->SetFromFunction(LiteralFn);
	LitNode->AllocateDefaultPins();
	{
		FString DefVal;
		FGameplayTag::StaticStruct()->ExportText(DefVal, &CommandTag, nullptr, nullptr, PPF_None, nullptr);
		Schema->TrySetDefaultValue(*LitNode->FindPinChecked(TEXT("Value")), DefVal);
	}

	UK2Node_CallFunction* EqNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	EqNode->SetFromFunction(EqualFn);
	EqNode->AllocateDefaultPins();

	UK2Node_IfThenElse* Branch = CompilerContext.SpawnIntermediateNode<UK2Node_IfThenElse>(this, SourceGraph);
	Branch->AllocateDefaultPins();

	// Wire: CommandTag → A, Literal → B, Equal → Condition
	Schema->TryCreateConnection(FindPinChecked(SwuiCmdHookPins::CmdTag),
		EqNode->FindPinChecked(TEXT("A")));
	Schema->TryCreateConnection(LitNode->GetReturnValuePin(),
		EqNode->FindPinChecked(TEXT("B")));
	Schema->TryCreateConnection(EqNode->GetReturnValuePin(),
		Branch->FindPinChecked(UEdGraphSchema_K2::PN_Condition));

	// ── Extract each parameter field from JSON ─────────────────────────
	UEdGraphPin* JsonPayloadPin = FindPinChecked(SwuiCmdHookPins::JsonPayload);
	UEdGraphPin* ThenPin = Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* LastExecPin = ThenPin;

	for (TFieldIterator<FProperty> It(CmdFn); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm)) continue;

		const FName FieldName = It->GetFName();
		UEdGraphPin* OutPin = GetOutputPinForField(FieldName);
		if (!OutPin) continue;

		// Pick the extractor function based on type
		FName ExtractorFnName;
		if (It->IsA<FStrProperty>() || It->IsA<FNameProperty>() || It->IsA<FTextProperty>())
			ExtractorFnName = GET_FUNCTION_NAME_CHECKED(USwuiJsonBlueprintLibrary, ExtractStringField);
		else if (It->IsA<FIntProperty>() || It->IsA<FInt64Property>() || It->IsA<FByteProperty>())
			ExtractorFnName = GET_FUNCTION_NAME_CHECKED(USwuiJsonBlueprintLibrary, ExtractIntField);
		else if (It->IsA<FFloatProperty>() || It->IsA<FDoubleProperty>())
			ExtractorFnName = GET_FUNCTION_NAME_CHECKED(USwuiJsonBlueprintLibrary, ExtractFloatField);
		else if (It->IsA<FBoolProperty>())
			ExtractorFnName = GET_FUNCTION_NAME_CHECKED(USwuiJsonBlueprintLibrary, ExtractBoolField);
		else
		{
			CompilerContext.MessageLog.Error(*FString::Printf(
				TEXT("@@ : UFUNCTION param '%s' (type '%s') has no JSON extractor."),
				*FieldName.ToString(), *It->GetCPPType()), this);
			continue;
		}

		UFunction* ExtractorFn = USwuiJsonBlueprintLibrary::StaticClass()->FindFunctionByName(ExtractorFnName);
		if (!ExtractorFn) continue;

		UK2Node_CallFunction* ExtractNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
		ExtractNode->SetFromFunction(ExtractorFn);
		ExtractNode->AllocateDefaultPins();

		// Wire: JsonPayload → extractor.JsonPayload, field name literal → FieldName
		Schema->TryCreateConnection(JsonPayloadPin, ExtractNode->FindPinChecked(TEXT("JsonPayload")));
		Schema->TrySetDefaultValue(*ExtractNode->FindPinChecked(TEXT("FieldName")), FieldName.ToString());

		// Wire exec: last → extractor → extractor.Then
		Schema->TryCreateConnection(LastExecPin, ExtractNode->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
		LastExecPin = ExtractNode->FindPinChecked(UEdGraphSchema_K2::PN_Then);

		// Route output pin through extractor's return value
		CompilerContext.MovePinLinksToIntermediate(*OutPin, *ExtractNode->GetReturnValuePin());
	}

	// Route node's exec through the last extractor's then-exec
	CompilerContext.MovePinLinksToIntermediate(
		*FindPinChecked(SwuiCmdHookPins::Exec), *LastExecPin);
}

#undef LOCTEXT_NAMESPACE
