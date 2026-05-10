#include "K2Node_SwuiNavigationEvent.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintGameplayTagLibrary.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraphNodeUtils.h"
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

#include "SwuiNavigation.h"

#define LOCTEXT_NAMESPACE "K2Node_SwuiNavigationEvent"

namespace SwuiNavigationEventNode
{
	static const FName ExecPinName(UEdGraphSchema_K2::PN_Then);
	static const FName EventPinName(TEXT("Event"));
	static const FName PayloadPinName(TEXT("JsonPayload"));

	static FString HumanizeTagSegment(const FString& Segment)
	{
		FString Result;
		Result.Reserve(Segment.Len() * 2);

		for (int32 Index = 0; Index < Segment.Len(); ++Index)
		{
			const TCHAR Char = Segment[Index];
			const bool bIsSeparator = (Char == TEXT('_')) || (Char == TEXT('-'));
			const bool bInsertSpace = Index > 0
				&& !bIsSeparator
				&& FChar::IsUpper(Char)
				&& FChar::IsLower(Segment[Index - 1]);

			if (bIsSeparator)
			{
				if (!Result.IsEmpty() && Result[Result.Len() - 1] != TEXT(' '))
				{
					Result.AppendChar(TEXT(' '));
				}
				continue;
			}

			if (bInsertSpace && Result[Result.Len() - 1] != TEXT(' '))
			{
				Result.AppendChar(TEXT(' '));
			}

			Result.AppendChar(Index == 0 ? FChar::ToUpper(Char) : Char);
		}

		return Result.TrimStartAndEnd();
	}

	static FText MakeNodeTitle(const FGameplayTag& Tag)
	{
		if (!Tag.IsValid())
		{
			return LOCTEXT("UnsetNodeTitle", "On Navigation Event");
		}

		TArray<FString> Segments;
		Tag.GetTagName().ToString().ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() > 0 && Segments[0].Equals(TEXT("swui"), ESearchCase::IgnoreCase))
		{
			Segments.RemoveAt(0);
		}

		FString Title = TEXT("On");
		for (const FString& Segment : Segments)
		{
			const FString Humanized = HumanizeTagSegment(Segment);
			if (!Humanized.IsEmpty())
			{
				Title += TEXT(" ") + Humanized;
			}
		}

		return FText::FromString(Title);
	}
}

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
		{
			CustomFunctionName = BuildCustomFunctionName();
		}
	}
}

void UK2Node_SwuiNavigationEvent::AllocateDefaultPins()
{
	InitializeDelegateSignature();
	Super::AllocateDefaultPins();

	if (UEdGraphPin* EventPin = FindPin(SwuiNavigationEventNode::EventPinName))
	{
		EventPin->PinFriendlyName = LOCTEXT("EventPinLabel", "Event");
	}

	if (UEdGraphPin* PayloadPin = FindPin(SwuiNavigationEventNode::PayloadPinName))
	{
		PayloadPin->PinFriendlyName = LOCTEXT("PayloadPinLabel", "Payload");
	}
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
	{
		return LOCTEXT("MenuTitle", "SWUI Navigation Event");
	}

	return SwuiNavigationEventNode::MakeNodeTitle(NavigationEventTag);
}

FText UK2Node_SwuiNavigationEvent::GetTooltipText() const
{
	if (!NavigationEventTag.IsValid())
	{
		return LOCTEXT("TooltipUnset", "Filtered wrapper around OnNavigationEvent.");
	}

	return FText::Format(
		LOCTEXT("Tooltip", "Filtered wrapper around OnNavigationEvent for the exact tag '{0}'."),
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

void UK2Node_SwuiNavigationEvent::RegisterDynamicBinding(UDynamicBlueprintBinding* BindingObject) const
{
	UComponentDelegateBinding* ComponentBindingObject = Cast<UComponentDelegateBinding>(BindingObject);
	if (!ComponentBindingObject)
	{
		FMessageLog("Blueprint").Error(LOCTEXT("RegisterDynamicBindingFailed", "SWUI navigation event failed to register dynamic binding because the binding object was invalid."));
		return;
	}

	if (ComponentPropertyName.IsNone())
	{
		FMessageLog("Blueprint").Error(LOCTEXT("RegisterDynamicBindingMissingComponent", "SWUI navigation event failed to register dynamic binding because ComponentPropertyName is missing."));
		return;
	}

	if (!NavigationEventTag.IsValid())
	{
		FMessageLog("Blueprint").Error(LOCTEXT("RegisterDynamicBindingMissingTag", "SWUI navigation event failed to register dynamic binding because NavigationEventTag is invalid."));
		return;
	}

	if (!GetTargetDelegateProperty())
	{
		FMessageLog("Blueprint").Error(LOCTEXT("RegisterDynamicBindingMissingDelegate", "SWUI navigation event failed to register dynamic binding because OnNavigationEvent could not be resolved."));
		return;
	}

	FBlueprintComponentDelegateBinding Binding;
	Binding.ComponentPropertyName = ComponentPropertyName;
	Binding.DelegatePropertyName = GET_MEMBER_NAME_CHECKED(USwuiNavigation, OnNavigationEvent);
	Binding.FunctionNameToBind = CustomFunctionName;

	ComponentBindingObject->ComponentDelegateBindings.Add(Binding);
}

void UK2Node_SwuiNavigationEvent::HandleVariableRenamed(UBlueprint* InBlueprint, UClass* InVariableClass, UEdGraph* InGraph, const FName& InOldVarName, const FName& InNewVarName)
{
	if (InVariableClass && InBlueprint && InBlueprint->GeneratedClass && InVariableClass->IsChildOf(InBlueprint->GeneratedClass) && InOldVarName == ComponentPropertyName)
	{
		Modify();
		ComponentPropertyName = InNewVarName;
	}
}

bool UK2Node_SwuiNavigationEvent::HasValidBindingData(FText* OutError) const
{
	auto SetError = [OutError](const FText& Message)
	{
		if (OutError)
		{
			*OutError = Message;
		}
		return false;
	};

	if (ComponentPropertyName.IsNone())
	{
		return SetError(LOCTEXT("MissingComponentProperty", "Missing SWUI navigation component property"));
	}

	if (!NavigationEventTag.IsValid())
	{
		return SetError(LOCTEXT("MissingNavigationTag", "Missing valid NavigationEventTag"));
	}

	if (!GetTargetDelegateProperty())
	{
		return SetError(LOCTEXT("MissingNavigationDelegate", "Could not resolve USwuiNavigation::OnNavigationEvent"));
	}

	const UBlueprint* Blueprint = GetBlueprint();
	if (!Blueprint)
	{
		return SetError(LOCTEXT("MissingBlueprint", "Could not resolve owning Blueprint"));
	}

	const FObjectProperty* ComponentProperty = nullptr;
	if (Blueprint->SkeletonGeneratedClass)
	{
		ComponentProperty = FindFProperty<FObjectProperty>(Blueprint->SkeletonGeneratedClass, ComponentPropertyName);
	}
	if (!ComponentProperty && Blueprint->GeneratedClass)
	{
		ComponentProperty = FindFProperty<FObjectProperty>(Blueprint->GeneratedClass, ComponentPropertyName);
	}

	if (!ComponentProperty || !ComponentProperty->PropertyClass || !ComponentProperty->PropertyClass->IsChildOf(USwuiNavigation::StaticClass()))
	{
		return SetError(LOCTEXT("MissingBoundComponent", "Missing matching SWUI navigation component property"));
	}

	if (CustomFunctionName.IsNone())
	{
		return SetError(LOCTEXT("MissingBindingFunction", "Could not generate a function name for dynamic binding registration"));
	}

	return true;
}

void UK2Node_SwuiNavigationEvent::ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const
{
	FText ErrorText;
	if (!HasValidBindingData(&ErrorText))
	{
		MessageLog.Error(*FString::Printf(TEXT("%s for @@"), *ErrorText.ToString()), this);
	}

	Super::ValidateNodeDuringCompilation(MessageLog);
}

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

FMulticastDelegateProperty* UK2Node_SwuiNavigationEvent::GetTargetDelegateProperty() const
{
	return FindFProperty<FMulticastDelegateProperty>(USwuiNavigation::StaticClass(), GET_MEMBER_NAME_CHECKED(USwuiNavigation, OnNavigationEvent));
}

FName UK2Node_SwuiNavigationEvent::BuildCustomFunctionName() const
{
	const UBlueprint* Blueprint = GetBlueprint();
	const FString BlueprintName = Blueprint ? Blueprint->GetName() : TEXT("SwuiBlueprint");
	const FName DelegateName = GET_MEMBER_NAME_CHECKED(USwuiNavigation, OnNavigationEvent);
	return FName(*FString::Printf(TEXT("BndEvt__%s_%s_%s"), *BlueprintName, *GetName(), *DelegateName.ToString()));
}

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

	UEdGraphPin* ThenPin = FindPin(SwuiNavigationEventNode::ExecPinName);
	UEdGraphPin* EventPin = FindPin(SwuiNavigationEventNode::EventPinName);
	UEdGraphPin* PayloadPin = FindPin(SwuiNavigationEventNode::PayloadPinName);
	if (!ThenPin || !EventPin || !PayloadPin)
	{
		CompilerContext.MessageLog.Error(TEXT("@@ is missing expected OnNavigationEvent pins."), this);
		BreakAllNodeLinks();
		return;
	}

	UFunction* EqualFunction = UBlueprintGameplayTagLibrary::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UBlueprintGameplayTagLibrary, EqualEqual_GameplayTag));
	UFunction* LiteralFunction = UBlueprintGameplayTagLibrary::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UBlueprintGameplayTagLibrary, MakeLiteralGameplayTag));
	if (!EqualFunction || !LiteralFunction)
	{
		CompilerContext.MessageLog.Error(TEXT("@@ could not resolve GameplayTag Blueprint helper functions."), this);
		BreakAllNodeLinks();
		return;
	}

	const UEdGraphSchema_K2* Schema = CompilerContext.GetSchema();

	UK2Node_CallFunction* LiteralNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	LiteralNode->SetFromFunction(LiteralFunction);
	LiteralNode->AllocateDefaultPins();

	FString TagDefaultValue;
	FGameplayTag::StaticStruct()->ExportText(TagDefaultValue, &NavigationEventTag, nullptr, nullptr, PPF_None, nullptr);
	Schema->TrySetDefaultValue(*LiteralNode->FindPinChecked(TEXT("Value")), TagDefaultValue);

	UK2Node_CallFunction* EqualNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	EqualNode->SetFromFunction(EqualFunction);
	EqualNode->AllocateDefaultPins();

	UK2Node_IfThenElse* BranchNode = CompilerContext.SpawnIntermediateNode<UK2Node_IfThenElse>(this, SourceGraph);
	BranchNode->AllocateDefaultPins();

	CompilerContext.MovePinLinksToIntermediate(*ThenPin, *BranchNode->FindPinChecked(UEdGraphSchema_K2::PN_Then));
	Schema->TryCreateConnection(ThenPin, BranchNode->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
	Schema->TryCreateConnection(EventPin, EqualNode->FindPinChecked(TEXT("A")));
	Schema->TryCreateConnection(LiteralNode->GetReturnValuePin(), EqualNode->FindPinChecked(TEXT("B")));
	Schema->TryCreateConnection(EqualNode->GetReturnValuePin(), BranchNode->FindPinChecked(UEdGraphSchema_K2::PN_Condition));
	BranchNode->FindPinChecked(UEdGraphSchema_K2::PN_Else)->BreakAllPinLinks();
}

#undef LOCTEXT_NAMESPACE
