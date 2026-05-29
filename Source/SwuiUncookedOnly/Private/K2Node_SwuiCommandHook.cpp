#include "K2Node_SwuiCommandHook.h"

#include "SwuiJsonBlueprintLibrary.h"
#include "SwuiNavigation.h"

#include "BlueprintGameplayTagLibrary.h"
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

namespace SwuiCmdPins
{
	static const FName Exec(UEdGraphSchema_K2::PN_Then);
	static const FName CmdTag(TEXT("CommandTag"));
	static const FName JsonPayload(TEXT("JsonPayload"));
}

static FString HumanizeTagSegment(const FString& Segment)
{
	FString Result;
	Result.Reserve(Segment.Len() * 2);
	for (int32 i = 0; i < Segment.Len(); ++i)
	{
		const TCHAR Ch = Segment[i];
		const bool bSep = (Ch == TEXT('_')) || (Ch == TEXT('-'));
		const bool bIns = i > 0 && !bSep && FChar::IsUpper(Ch) && FChar::IsLower(Segment[i - 1]);
		if (bSep) { if (!Result.IsEmpty() && Result[Result.Len()-1] != ' ') Result.AppendChar(' '); continue; }
		if (bIns && Result[Result.Len()-1] != ' ') Result.AppendChar(' ');
		Result.AppendChar(i == 0 ? FChar::ToUpper(Ch) : Ch);
	}
	return Result.TrimStartAndEnd();
}

static FText MakeNodeTitle(const FGameplayTag& Tag)
{
	if (!Tag.IsValid()) return LOCTEXT("UnsetTitle", "On Command Hook");
	TArray<FString> Segs;
	Tag.GetTagName().ToString().ParseIntoArray(Segs, TEXT("."), true);
	if (Segs.Num() > 0 && Segs[0].Equals(TEXT("swui"), ESearchCase::IgnoreCase)) Segs.RemoveAt(0);
	FString Title = TEXT("On");
	for (const FString& S : Segs) Title += TEXT(" ") + HumanizeTagSegment(S);
	Title += TEXT(" Hook");
	return FText::FromString(Title);
}

// ══════════════════════════════════════════════════════════════════════════════
// Deterministic UFUNCTION resolution
// ══════════════════════════════════════════════════════════════════════════════

UFunction* UK2Node_SwuiCommandHook::ResolveCommandFunction() const
{
	if (StoredClassPath.IsEmpty() || StoredFunctionName.IsNone()) return nullptr;
	UClass* Cls = FindObject<UClass>(nullptr, *StoredClassPath);
	if (!Cls) return nullptr;
	return Cls->FindFunctionByName(StoredFunctionName, EIncludeSuperFlag::ExcludeSuper);
}

int32 UK2Node_SwuiCommandHook::ComputeFunctionParamHash(UFunction* Fn)
{
	if (!Fn) return 0;
	int32 Hash = 0;
	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm)) continue;
		Hash = HashCombine(Hash, GetTypeHash(It->GetFName()));
		Hash = HashCombine(Hash, GetTypeHash(It->GetCPPType()));
	}
	return Hash;
}

void UK2Node_SwuiCommandHook::ResolveAndStoreSignature()
{
	if (!CommandTag.IsValid()) { ParamSignatureHash = 0; ReconstructNode(); return; }

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Cls = *It;
		if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;
		if (Cls->GetName().StartsWith(TEXT("SKEL_")) || Cls->GetName().StartsWith(TEXT("REINST_"))) continue;
		for (TFieldIterator<UFunction> Fi(Cls, EFieldIteratorFlags::ExcludeSuper); Fi; ++Fi)
		{
			const FString Cmd = Fi->GetMetaData(TEXT("SwuiCommand"));
			if (Cmd.IsEmpty()) continue;
			if (Cmd != CommandTag.GetTagName().ToString()) continue;
			StoredClassPath = Cls->GetPathName();
			StoredFunctionName = Fi->GetFName();
			ParamSignatureHash = ComputeFunctionParamHash(*Fi);
			ReconstructNode();
			return;
		}
	}
	ParamSignatureHash = 0;
	ReconstructNode();
}

// ══════════════════════════════════════════════════════════════════════════════
// Lifecycle
// ══════════════════════════════════════════════════════════════════════════════

void UK2Node_SwuiCommandHook::AllocateDefaultPins()
{
	// Point EventReference to OnSwuiCommandExecuted so the parent can
	// create the correct delegate signature pins.
	FMulticastDelegateProperty* DelegateProp = GetTargetDelegateProperty();
	if (DelegateProp)
	{
		EventReference.SetFromField<UFunction>(DelegateProp->SignatureFunction, false);
	}

	bInternalEvent = true;
	bOverrideFunction = false;

	// Set a deterministic function name for the stub.
	const UBlueprint* BP = GetBlueprint();
	const FString BPName = BP ? BP->GetName() : TEXT("SwuiBlueprint");
	FString SafeTagStr = CommandTag.GetTagName().ToString();
	SafeTagStr.ReplaceCharInline(TEXT('.'), TEXT('_'));
	CustomFunctionName = FName(*FString::Printf(TEXT("BndEvt__%s_SwuiCmdHook_%s"), *BPName, *SafeTagStr));

	Super::AllocateDefaultPins();

	// Hide the raw delegate pins — they're wired internally in ExpandNode.
	if (UEdGraphPin* Pin = FindPin(SwuiCmdPins::CmdTag))
		Pin->bHidden = true;
	if (UEdGraphPin* Pin = FindPin(SwuiCmdPins::JsonPayload))
		Pin->bHidden = true;

	// Add typed output pins from the stored command signature.
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UFunction* Fn = ResolveCommandFunction();
	if (Fn)
	{
		for (TFieldIterator<FProperty> It(Fn); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm)) continue;
			const FName PinName = It->GetFName();
			FEdGraphPinType PinType;
			if (!Schema->ConvertPropertyToPinType(*It, PinType)) continue;
			UEdGraphPin* OutPin = CreatePin(EGPD_Output, PinType, PinName);
			OutPin->PinFriendlyName = FText::FromName(PinName);
		}
	}
}

void UK2Node_SwuiCommandHook::ReconstructNode()
{
	CachedNodeTitle.MarkDirty();
	Super::ReconstructNode();
}

FText UK2Node_SwuiCommandHook::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	if (TitleType == ENodeTitleType::MenuTitle) return LOCTEXT("MenuTitle", "SWUI Command Hook");
	return MakeNodeTitle(CommandTag);
}

FText UK2Node_SwuiCommandHook::GetTooltipText() const
{
	if (!CommandTag.IsValid()) return LOCTEXT("TooltipUnset", "Observes a function-backed SWUI command.");
	return FText::Format(LOCTEXT("Tooltip", "Hooks into the function-backed command '{0}' after it is executed."),
		FText::FromString(CommandTag.GetTagName().ToString()));
}

FMulticastDelegateProperty* UK2Node_SwuiCommandHook::GetTargetDelegateProperty() const
{
	return FindFProperty<FMulticastDelegateProperty>(
		USwuiNavigation::StaticClass(), GET_MEMBER_NAME_CHECKED(USwuiNavigation, OnSwuiCommandExecuted));
}

UClass* UK2Node_SwuiCommandHook::GetDynamicBindingClass() const
{
	return UComponentDelegateBinding::StaticClass();
}

void UK2Node_SwuiCommandHook::RegisterDynamicBinding(UDynamicBlueprintBinding* BindingObject) const
{
	UComponentDelegateBinding* ComponentBinding = Cast<UComponentDelegateBinding>(BindingObject);
	if (!ComponentBinding) return;
	if (ComponentPropertyName.IsNone()) return;

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

void UK2Node_SwuiCommandHook::ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const
{
	if (ComponentPropertyName.IsNone())
		MessageLog.Error(*LOCTEXT("MissingComponent", "@@ has no ComponentPropertyName.").ToString(), this);
	if (!CommandTag.IsValid())
		MessageLog.Error(*LOCTEXT("MissingTag", "@@ has no valid CommandTag.").ToString(), this);

	UFunction* Fn = ResolveCommandFunction();
	if (!Fn)
	{
		MessageLog.Error(*LOCTEXT("NoFunction",
			"@@ could not resolve the SwuiCommand UFUNCTION for the stored signature.").ToString(), this);
		return;
	}

	const int32 CurHash = ComputeFunctionParamHash(Fn);
	if (CurHash != ParamSignatureHash)
	{
		MessageLog.Error(*FString::Printf(
			TEXT("@@ : SWUI command signature changed (stored %d, current %d). Refresh this node."),
			ParamSignatureHash, CurHash), this);
		return;
	}

	for (TFieldIterator<FProperty> It(Fn); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm)) continue;
		FEdGraphPinType Tmp;
		if (!GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(*It, Tmp))
		{
			MessageLog.Error(*FString::Printf(
				TEXT("@@ : param '%s' (type '%s') not supported as output pin."),
				*It->GetName(), *It->GetCPPType()), this);
		}
	}

	Super::ValidateNodeDuringCompilation(MessageLog);
}

// ══════════════════════════════════════════════════════════════════════════════
// Helpers
// ══════════════════════════════════════════════════════════════════════════════

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

	const UEdGraphSchema_K2* Schema = CompilerContext.GetSchema();

	UFunction* CmdFn = ResolveCommandFunction();
	if (!CmdFn)
	{
		CompilerContext.MessageLog.Error(TEXT("@@ could not resolve SwuiCommand UFUNCTION."), this);
		BreakAllNodeLinks();
		return;
	}

	const int32 CurHash = ComputeFunctionParamHash(CmdFn);
	if (CurHash != ParamSignatureHash)
	{
		CompilerContext.MessageLog.Error(*FString::Printf(TEXT("@@ : SWUI command signature changed. Refresh this node.")), this);
		BreakAllNodeLinks();
		return;
	}

	// ── Real delegate pins (hidden on the node but still present) ──────
	UEdGraphPin* EvtCmdTag    = FindPin(SwuiCmdPins::CmdTag);
	UEdGraphPin* EvtJsonPayload = FindPin(SwuiCmdPins::JsonPayload);
	if (!EvtCmdTag || !EvtJsonPayload)
	{
		CompilerContext.MessageLog.Error(TEXT("@@ : Missing OnSwuiCommandExecuted delegate pins."), this);
		BreakAllNodeLinks();
		return;
	}

	// ── Tag compare + branch ───────────────────────────────────────────
	UFunction* LitFn = UBlueprintGameplayTagLibrary::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UBlueprintGameplayTagLibrary, MakeLiteralGameplayTag));
	UFunction* EqFn = UBlueprintGameplayTagLibrary::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UBlueprintGameplayTagLibrary, EqualEqual_GameplayTag));
	if (!LitFn || !EqFn)
	{
		CompilerContext.MessageLog.Error(TEXT("@@ could not resolve GameplayTag BP helpers."), this);
		BreakAllNodeLinks();
		return;
	}

	UK2Node_CallFunction* LitNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	LitNode->SetFromFunction(LitFn);
	LitNode->AllocateDefaultPins();
	FString DefVal;
	FGameplayTag::StaticStruct()->ExportText(DefVal, &CommandTag, nullptr, nullptr, PPF_None, nullptr);
	Schema->TrySetDefaultValue(*LitNode->FindPinChecked(TEXT("Value")), DefVal);

	UK2Node_CallFunction* EqNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	EqNode->SetFromFunction(EqFn);
	EqNode->AllocateDefaultPins();

	UK2Node_IfThenElse* Branch = CompilerContext.SpawnIntermediateNode<UK2Node_IfThenElse>(this, SourceGraph);
	Branch->AllocateDefaultPins();

	Schema->TryCreateConnection(EvtCmdTag, EqNode->FindPinChecked(TEXT("A")));
	Schema->TryCreateConnection(LitNode->GetReturnValuePin(), EqNode->FindPinChecked(TEXT("B")));
	Schema->TryCreateConnection(EqNode->GetReturnValuePin(), Branch->FindPinChecked(UEdGraphSchema_K2::PN_Condition));

	// ── Extraction chain ─────────────────────────────────────────────
	UEdGraphPin* BranchThen = Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* LastExecPin = BranchThen;

	for (TFieldIterator<FProperty> It(CmdFn); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm)) continue;

		const FName FieldName = It->GetFName();
		UEdGraphPin* OutPin = GetOutputPinForField(FieldName);
		if (!OutPin) continue;

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
				TEXT("@@ : param '%s' (type '%s') has no JSON extractor."), *FieldName.ToString(), *It->GetCPPType()), this);
			continue;
		}

		UFunction* ExtractorFn = USwuiJsonBlueprintLibrary::StaticClass()->FindFunctionByName(ExtractorFnName);
		if (!ExtractorFn) continue;

		UK2Node_CallFunction* ExNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
		ExNode->SetFromFunction(ExtractorFn);
		ExNode->AllocateDefaultPins();

		Schema->TryCreateConnection(EvtJsonPayload, ExNode->FindPinChecked(TEXT("JsonPayload")));
		Schema->TrySetDefaultValue(*ExNode->FindPinChecked(TEXT("FieldName")), FieldName.ToString());

		Schema->TryCreateConnection(LastExecPin, ExNode->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
		LastExecPin = ExNode->FindPinChecked(UEdGraphSchema_K2::PN_Then);

		CompilerContext.MovePinLinksToIntermediate(*OutPin, *ExNode->GetReturnValuePin());
	}

	// ── Route exec: event then -> Branch exec, user links -> last extractor then ──
	UEdGraphPin* ThenPin = FindPin(SwuiCmdPins::Exec);
	if (ThenPin)
	{
		// First move the user's original output exec links to the end of the chain.
		CompilerContext.MovePinLinksToIntermediate(*ThenPin, *LastExecPin);
		// Then connect the event entry exec into the generated branch.
		Schema->TryCreateConnection(ThenPin, Branch->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
	}
}

#undef LOCTEXT_NAMESPACE
