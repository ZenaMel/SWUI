#include "K2Node_SwuiNavigationEvent.h"

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

#include "SwuiJsonBlueprintLibrary.h"
#include "SwuiNavigation.h"
#include "SwuiTypes.h"

#define LOCTEXT_NAMESPACE "K2Node_SwuiNavigationEvent"

// ── Pin names ───────────────────────────────────────────────────────────────
namespace SwuiNavEventPins
{
	static const FName Exec    (UEdGraphSchema_K2::PN_Then);
	static const FName Event   (TEXT("Event"));
	static const FName Payload (TEXT("Payload"));

	static FText ExecLabel()    { return LOCTEXT("Then", "then"); }
	static FText PayloadLabel() { return LOCTEXT("Payload", "Payload"); }
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
		return LOCTEXT("UnsetNodeTitle", "On Navigation Event");
	TArray<FString> Segments;
	Tag.GetTagName().ToString().ParseIntoArray(Segments, TEXT("."), true);
	if (Segments.Num() > 0 && Segments[0].Equals(TEXT("swui"), ESearchCase::IgnoreCase))
		Segments.RemoveAt(0);
	FString Title = TEXT("On");
	for (const FString& Segment : Segments)
		Title += TEXT(" ") + HumanizeTagSegment(Segment);
	return FText::FromString(Title);
}

// ══════════════════════════════════════════════════════════════════════════════
// Lifecycle
// ══════════════════════════════════════════════════════════════════════════════

bool UK2Node_SwuiNavigationEvent::Modify(bool bAlwaysMarkDirty)
{
	CachedNodeTitle.MarkDirty();
	return Super::Modify(bAlwaysMarkDirty);
}

void UK2Node_SwuiNavigationEvent::InitializeDelegateSignature()
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

void UK2Node_SwuiNavigationEvent::AllocateDefaultPins()
{
	InitializeDelegateSignature();
	Super::AllocateDefaultPins();

	// Event pin: keep hidden — used internally for tag comparison.
	if (UEdGraphPin* EventPin = FindPin(SwuiNavEventPins::Event))
	{
		EventPin->bHidden = true;
	}

	// Hide the JsonPayload pin inherited from the delegate signature.
	// It's used internally in ExpandNode but never shown to the user.
	if (UEdGraphPin* JsonPin = FindPin(TEXT("JsonPayload")))
	{
		JsonPin->bHidden = true;
	}

	// Create the typed Payload struct pin. Default to FSwuiEmptyPayload so the
	// pin is always a concrete struct type (breakable, destructureable).
	// ExpandNode may change the type if a different PayloadStruct is resolved.
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UEdGraphPin* PayloadPin = CreatePin(EGPD_Output, Schema->PC_Struct, SwuiNavEventPins::Payload);
	PayloadPin->PinType.PinSubCategoryObject = const_cast<UScriptStruct*>(FSwuiEmptyPayload::StaticStruct());
	PayloadPin->PinFriendlyName = SwuiNavEventPins::PayloadLabel();
}

void UK2Node_SwuiNavigationEvent::ReconstructNode()
{
	InitializeDelegateSignature();
	CachedNodeTitle.MarkDirty();
	Super::ReconstructNode();
}

FText UK2Node_SwuiNavigationEvent::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	if (TitleType == ENodeTitleType::MenuTitle)
		return LOCTEXT("MenuTitle", "SWUI Navigation Event");
	return MakeNodeTitle(NavigationEventTag);
}

FText UK2Node_SwuiNavigationEvent::GetTooltipText() const
{
	if (!NavigationEventTag.IsValid())
		return LOCTEXT("TooltipUnset", "Listens for a typed SWUI navigation event.");
	return FText::Format(LOCTEXT("Tooltip", "Listens for the navigation event '{0}' and deserializes its payload."),
		FText::FromString(NavigationEventTag.GetTagName().ToString()));
}

FText UK2Node_SwuiNavigationEvent::GetMenuCategory() const
{
	return LOCTEXT("Category", "SimpleWebUI|Navigation");
}

UClass* UK2Node_SwuiNavigationEvent::GetDynamicBindingClass() const
{
	return UComponentDelegateBinding::StaticClass();
}

// ══════════════════════════════════════════════════════════════════════════════
// Binding
// ══════════════════════════════════════════════════════════════════════════════

void UK2Node_SwuiNavigationEvent::RegisterDynamicBinding(UDynamicBlueprintBinding* BindingObject) const
{
	UComponentDelegateBinding* ComponentBinding = Cast<UComponentDelegateBinding>(BindingObject);
	if (!ComponentBinding)
	{
		FMessageLog("Blueprint").Error(LOCTEXT("RegBindFailed",
			"SWUI navigation event failed to register dynamic binding."));
		return;
	}
	if (ComponentPropertyName.IsNone())
	{
		FMessageLog("Blueprint").Error(LOCTEXT("RegBindMissingComp",
			"SWUI navigation event: ComponentPropertyName is missing."));
		return;
	}
	if (!NavigationEventTag.IsValid())
	{
		FMessageLog("Blueprint").Error(LOCTEXT("RegBindMissingTag",
			"SWUI navigation event: NavigationEventTag is invalid."));
		return;
	}
	if (!GetTargetDelegateProperty())
	{
		FMessageLog("Blueprint").Error(LOCTEXT("RegBindMissingDel",
			"SWUI navigation event: OnNavigationEvent could not be resolved."));
		return;
	}

	FBlueprintComponentDelegateBinding Binding;
	Binding.ComponentPropertyName = ComponentPropertyName;
	Binding.DelegatePropertyName = GET_MEMBER_NAME_CHECKED(USwuiNavigation, OnNavigationEvent);
	Binding.FunctionNameToBind = CustomFunctionName;
	ComponentBinding->ComponentDelegateBindings.Add(Binding);
}

void UK2Node_SwuiNavigationEvent::HandleVariableRenamed(
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

bool UK2Node_SwuiNavigationEvent::HasValidBindingData(FText* OutError) const
{
	auto E = [OutError](const FText& M) { if (OutError) *OutError = M; return false; };
	if (ComponentPropertyName.IsNone())
		return E(LOCTEXT("MissingComponentProperty", "Missing SWUI navigation component property"));
	if (!NavigationEventTag.IsValid())
		return E(LOCTEXT("MissingNavigationTag", "Missing valid NavigationEventTag"));
	if (!GetTargetDelegateProperty())
		return E(LOCTEXT("MissingNavigationDelegate", "Could not resolve OnNavigationEvent"));

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

void UK2Node_SwuiNavigationEvent::ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const
{
	FText ErrorText;
	if (!HasValidBindingData(&ErrorText))
	{
		MessageLog.Error(*FString::Printf(TEXT("%s for @@"), *ErrorText.ToString()), this);
	}

	// Reject function-backed commands — these are dispatched directly
	// by USwuiNavigation::ReceiveNavigationEventFromJs via ProcessEvent.
	if (NavigationEventTag.IsValid())
	{
		bool bIsFunctionBacked = false;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Cls = *It;
			if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;
			if (Cls->GetName().StartsWith(TEXT("SKEL_")) || Cls->GetName().StartsWith(TEXT("REINST_"))) continue;
			for (TFieldIterator<UFunction> FnIt(Cls, EFieldIteratorFlags::ExcludeSuper); FnIt; ++FnIt)
			{
				const FString Event = FnIt->GetMetaData(TEXT("SwuiCommand"));
				if (!Event.IsEmpty())
				{
					FGameplayTag CmdTag = FGameplayTag::RequestGameplayTag(FName(*Event), false);
					if (CmdTag == NavigationEventTag)
					{
						bIsFunctionBacked = true;
						break;
					}
				}
			}
			if (bIsFunctionBacked) break;
		}
		if (bIsFunctionBacked)
		{
			MessageLog.Error(*FString::Printf(
				TEXT("@@ : Tag '%s' is a function-backed command (UFUNCTION with SwuiCommand metadata). "
					"Function-backed commands are dispatched directly at runtime via ProcessEvent — "
					"a K2Node_SwuiNavigationEvent wrapper is not needed. "
					"Remove this node and use the generated SwuiCommands helper from JS instead."),
				*NavigationEventTag.GetTagName().ToString()), this);
		}
	}

	// Every navigation event node MUST resolve a PayloadStruct.
	// If none is configured, the tag lookup falls back to FSwuiEmptyPayload,
	// but if even that fails, it's a hard error.
	const UScriptStruct* Resolved = ResolvePayloadStruct();
	if (!Resolved)
	{
		MessageLog.Error(*LOCTEXT("NoPayloadStruct",
			"@@ could not resolve a PayloadStruct for tag '{0}'. "
			"Ensure the navigation event has a PayloadStruct configured, "
			"or remove the node.").ToString(), this);
	}

	Super::ValidateNodeDuringCompilation(MessageLog);
}

// ══════════════════════════════════════════════════════════════════════════════
// Menu actions
// ══════════════════════════════════════════════════════════════════════════════

void UK2Node_SwuiNavigationEvent::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
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

FMulticastDelegateProperty* UK2Node_SwuiNavigationEvent::GetTargetDelegateProperty() const
{
	return FindFProperty<FMulticastDelegateProperty>(USwuiNavigation::StaticClass(),
		GET_MEMBER_NAME_CHECKED(USwuiNavigation, OnNavigationEvent));
}

FName UK2Node_SwuiNavigationEvent::BuildCustomFunctionName() const
{
	const UBlueprint* BP = GetBlueprint();
	const FString BPName = BP ? BP->GetName() : TEXT("SwuiBlueprint");
	const FName DelName = GET_MEMBER_NAME_CHECKED(USwuiNavigation, OnNavigationEvent);
	return FName(*FString::Printf(TEXT("BndEvt__%s_%s_%s"), *BPName, *GetName(), *DelName.ToString()));
}

const UScriptStruct* UK2Node_SwuiNavigationEvent::ResolvePayloadStruct() const
{
	// First, check the component's NavigationEvents array.
	if (NavigationEventTag.IsValid() && !ComponentPropertyName.IsNone())
	{
		const UBlueprint* BP = GetBlueprint();
		// Can't read CDO during compile — the class is mid-construction.
		// Only resolve from CDO when the Blueprint is fully loaded and not compiling.
		if (BP && BP->GeneratedClass && !BP->bIsRegeneratingOnLoad
			&& !BP->GeneratedClass->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad))
		{
			if (const FObjectProperty* Prop = FindFProperty<FObjectProperty>(BP->GeneratedClass, ComponentPropertyName))
			{
				const UObject* CDO = BP->GeneratedClass->GetDefaultObject();
				if (CDO)
				{
					if (const USwuiNavigation* Nav = Cast<USwuiNavigation>(
							Prop->GetObjectPropertyValue(Prop->ContainerPtrToValuePtr<void>(CDO))))
					{
						for (const FSwuiNavigationEvent& E : Nav->NavigationEvents)
						{
							if (E.Event == NavigationEventTag && E.PayloadStruct.IsValid())
							{
								return E.PayloadStruct.LoadSynchronous();
							}
						}
					}
				}
			}
		}
	}

	// Fallback: every node MUST have a typed output. Use FSwuiEmptyPayload
	// when no explicit struct is configured so the node is always consistent.
	return FSwuiEmptyPayload::StaticStruct();
}

// ══════════════════════════════════════════════════════════════════════════════
// Expansion (compile-time graph generation)
// ══════════════════════════════════════════════════════════════════════════════

void UK2Node_SwuiNavigationEvent::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);

	FText ErrorText;
	if (!HasValidBindingData(&ErrorText))
	{
		CompilerContext.MessageLog.Error(*FString::Printf(TEXT("%s for @@"), *ErrorText.ToString()), this);
		BreakAllNodeLinks();
		return;
	}

	const UScriptStruct* PayloadStructDef = ResolvePayloadStruct();
	if (!PayloadStructDef)
	{
		CompilerContext.MessageLog.Error(TEXT("@@ could not resolve a PayloadStruct. "
			"Ensure the navigation event has a PayloadStruct configured."), this);
		BreakAllNodeLinks();
		return;
	}

	const UEdGraphSchema_K2* Schema = CompilerContext.GetSchema();

	// ── Configure the Payload pin type ───────────────────────────────────
	// Default is FSwuiEmptyPayload (set in AllocateDefaultPins).
	// If resolved to a different struct, update the pin now.
	UEdGraphPin* PayloadPin = FindPinChecked(SwuiNavEventPins::Payload);
	if (PayloadPin->PinType.PinSubCategoryObject != PayloadStructDef)
	{
		PayloadPin->PinType.PinCategory = Schema->PC_Struct;
		PayloadPin->PinType.PinSubCategoryObject = const_cast<UScriptStruct*>(PayloadStructDef);
	}

	// ── Spawn tag literal ───────────────────────────────────────────────
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
		FGameplayTag::StaticStruct()->ExportText(DefVal, &NavigationEventTag, nullptr, nullptr, PPF_None, nullptr);
		Schema->TrySetDefaultValue(*LitNode->FindPinChecked(TEXT("Value")), DefVal);
	}

	UK2Node_CallFunction* EqNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	EqNode->SetFromFunction(EqualFn);
	EqNode->AllocateDefaultPins();

	// ── Branch ──────────────────────────────────────────────────────────
	UK2Node_IfThenElse* Branch = CompilerContext.SpawnIntermediateNode<UK2Node_IfThenElse>(this, SourceGraph);
	Branch->AllocateDefaultPins();

	// Wire: Event → A, Literal → B, Equal → Condition
	Schema->TryCreateConnection(FindPinChecked(SwuiNavEventPins::Event),
		EqNode->FindPinChecked(TEXT("A")));
	Schema->TryCreateConnection(LitNode->GetReturnValuePin(),
		EqNode->FindPinChecked(TEXT("B")));
	Schema->TryCreateConnection(EqNode->GetReturnValuePin(),
		Branch->FindPinChecked(UEdGraphSchema_K2::PN_Condition));

	// ── Step 1: Deserialize JsonPayload into FSwuiInstancedStruct ────────
	UFunction* J2SFn = USwuiJsonBlueprintLibrary::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(USwuiJsonBlueprintLibrary, JsonToStruct));
	if (!J2SFn)
	{
		CompilerContext.MessageLog.Error(TEXT("@@ : USwuiJsonBlueprintLibrary::JsonToStruct not found."), this);
		BreakAllNodeLinks();
		return;
	}

	UK2Node_CallFunction* J2S = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	J2S->SetFromFunction(J2SFn);
	J2S->AllocateDefaultPins();

	// Wire hidden JsonPayload → J2S input
	if (UEdGraphPin* JsonPayloadPin = FindPin(TEXT("JsonPayload")))
	{
		Schema->TryCreateConnection(JsonPayloadPin, J2S->FindPinChecked(TEXT("JsonPayload")));
	}

	// Set StructPath as literal
	Schema->TrySetDefaultValue(*J2S->FindPinChecked(TEXT("StructPath")),
		PayloadStructDef->GetPathName());

	// ── Step 2: Extract typed struct from FSwuiInstancedStruct ──────────
	UFunction* GetValFn = USwuiJsonBlueprintLibrary::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(USwuiJsonBlueprintLibrary, GetSwuiInstancedStructValue));
	if (!GetValFn)
	{
		CompilerContext.MessageLog.Error(TEXT("@@ : GetSwuiInstancedStructValue not found."), this);
		BreakAllNodeLinks();
		return;
	}

	UK2Node_CallFunction* GetVal = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	GetVal->SetFromFunction(GetValFn);
	GetVal->AllocateDefaultPins();

	// Wire FSwuiInstancedStruct → GetVal.InstancedStruct
	Schema->TryCreateConnection(J2S->GetReturnValuePin(),
		GetVal->FindPinChecked(TEXT("InstancedStruct")));

	// Type the GetVal Value pin to our payload struct
	UEdGraphPin* TypedOut = GetVal->FindPinChecked(TEXT("Value"));
	TypedOut->PinType.PinCategory = Schema->PC_Struct;
	TypedOut->PinType.PinSubCategoryObject = const_cast<UScriptStruct*>(PayloadStructDef);

	// ── Wire exec chain: Then → J2S → GetVal → downstream ─────────────
	UEdGraphPin* ThenPin = Branch->FindPinChecked(UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* J2SThen = J2S->FindPinChecked(UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* GetValThen = GetVal->FindPinChecked(UEdGraphSchema_K2::PN_Then);

	Schema->TryCreateConnection(ThenPin, J2S->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
	Schema->TryCreateConnection(J2SThen, GetVal->FindPinChecked(UEdGraphSchema_K2::PN_Execute));

	// Route node's Payload pin through GetVal's typed output
	CompilerContext.MovePinLinksToIntermediate(*PayloadPin, *TypedOut);

	// Route node's exec through GetVal's then-exec
	CompilerContext.MovePinLinksToIntermediate(*FindPinChecked(SwuiNavEventPins::Exec), *GetValThen);

	// Disconnect Else branch (typed events always match or don't fire)
	Branch->FindPinChecked(UEdGraphSchema_K2::PN_Else)->BreakAllPinLinks();
}

#undef LOCTEXT_NAMESPACE
