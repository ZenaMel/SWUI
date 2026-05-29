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
#include "K2Node_ComponentBoundEvent.h"
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

namespace SwuiCmdHookPins
{
	static const FName Exec(UEdGraphSchema_K2::PN_Then);
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
// Deterministic UFUNCTION resolution from stored class path + name
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
			if (FGameplayTag::RequestGameplayTag(FName(*Cmd), false) != CommandTag) continue;
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
	// Create the then-exec output pin.
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, SwuiCmdHookPins::Exec);

	// Create typed output pins from the stored command signature.
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UFunction* Fn = ResolveCommandFunction();
	if (Fn)
	{
		for (TFieldIterator<FProperty> It(Fn); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm)) continue;
			const FName PinName = It->GetFName();
			FEdGraphPinType PinType;
			if (!Schema->ConvertPropertyToPinType(*It, PinType))
			{
				UE_LOG(LogTemp, Warning, TEXT("SWUI: Command hook '%s' unsupported param type '%s'."),
					*CommandTag.ToString(), *It->GetCPPType());
				continue;
			}
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
	if (!CommandTag.IsValid()) return LOCTEXT("TooltipUnset", "Observes a function-backed SWUI command and provides its typed params.");
	return FText::Format(LOCTEXT("Tooltip", "Hooks into the function-backed command '{0}' after it is executed."),
		FText::FromString(CommandTag.GetTagName().ToString()));
}

FText UK2Node_SwuiCommandHook::GetMenuCategory() const
{
	return LOCTEXT("Category", "SimpleWebUI|Commands");
}

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
// Validation
// ══════════════════════════════════════════════════════════════════════════════

void UK2Node_SwuiCommandHook::ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const
{
	if (ComponentPropertyName.IsNone())
		MessageLog.Error(*LOCTEXT("MissingComponent", "@@ has no component property.").ToString(), this);
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
		CompilerContext.MessageLog.Error(*FString::Printf(
			TEXT("@@ : SWUI command signature changed. Refresh this node.")), this);
		BreakAllNodeLinks();
		return;
	}

	const UEdGraphSchema_K2* Schema = CompilerContext.GetSchema();

	// ── 1. Spawn intermediate UK2Node_ComponentBoundEvent ───────────────
	FMulticastDelegateProperty* DelegateProp = FindFProperty<FMulticastDelegateProperty>(
		USwuiNavigation::StaticClass(), GET_MEMBER_NAME_CHECKED(USwuiNavigation, OnSwuiCommandExecuted));
	if (!DelegateProp)
	{
		CompilerContext.MessageLog.Error(TEXT("@@ : Could not find OnSwuiCommandExecuted delegate."), this);
		BreakAllNodeLinks();
		return;
	}

	UBlueprint* BP = GetBlueprint();
	FObjectProperty* ComponentProp = nullptr;
	if (BP && BP->SkeletonGeneratedClass)
		ComponentProp = FindFProperty<FObjectProperty>(BP->SkeletonGeneratedClass, ComponentPropertyName);
	if (!ComponentProp && BP && BP->GeneratedClass)
		ComponentProp = FindFProperty<FObjectProperty>(BP->GeneratedClass, ComponentPropertyName);
	if (!ComponentProp)
	{
		CompilerContext.MessageLog.Error(TEXT("@@ : Could not find component property."), this);
		BreakAllNodeLinks();
		return;
	}

	UK2Node_ComponentBoundEvent* BoundEvent = CompilerContext.SpawnIntermediateNode<UK2Node_ComponentBoundEvent>(this, SourceGraph);
	BoundEvent->InitializeComponentBoundEventParams(ComponentProp, DelegateProp);
	BoundEvent->bInternalEvent = true;
	BoundEvent->bOverrideFunction = false;
	BoundEvent->CustomFunctionName = FName(*FString::Printf(
		TEXT("BndEvt__%s_SwuiCommandHook_%s"), *BP->GetName(), *CommandTag.GetTagName().ToString()));
	BoundEvent->ComponentPropertyName = ComponentPropertyName;
	BoundEvent->AllocateDefaultPins();

	// ── 2. Find the event node's output pins by iterating its properties ──
	// The delegate signature has (FGameplayTag, FString). Find pins by type/name.
	UEdGraphPin* EvtCmdTag = nullptr;
	UEdGraphPin* EvtJsonPayload = nullptr;
	for (UEdGraphPin* Pin : BoundEvent->Pins)
	{
		if (Pin->Direction != EGPD_Output || Pin->PinName == SwuiCmdHookPins::Exec) continue;
		if (Pin->PinType.PinCategory == Schema->PC_Struct && Pin->PinType.PinSubCategoryObject == FGameplayTag::StaticStruct())
			EvtCmdTag = Pin;
		else if (Pin->PinType.PinCategory == Schema->PC_String)
			EvtJsonPayload = Pin;
	}
	if (!EvtCmdTag || !EvtJsonPayload)
	{
		CompilerContext.MessageLog.Error(TEXT("@@ : Could not find (FGameplayTag, FString) output pins on intermediate event node."), this);
		BreakAllNodeLinks();
		return;
	}

	// ── 3. Tag compare + branch ────────────────────────────────────────
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

	// ── 4. Extract each parameter field from JSON ──────────────────────
	UEdGraphPin* LastExecPin = Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then);

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

	// Route the public node's exec through the final extractor's then-exec.
	CompilerContext.MovePinLinksToIntermediate(*FindPinChecked(SwuiCmdHookPins::Exec), *LastExecPin);
}

#undef LOCTEXT_NAMESPACE
