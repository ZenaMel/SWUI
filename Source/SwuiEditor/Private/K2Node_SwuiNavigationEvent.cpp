#include "K2Node_SwuiNavigationEvent.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintGameplayTagLibrary.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "KismetCompiler.h"

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

void UK2Node_SwuiNavigationEvent::AllocateDefaultPins()
{
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, SwuiNavigationEventNode::ExecPinName);
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Struct, FGameplayTag::StaticStruct(), SwuiNavigationEventNode::EventPinName);
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_String, SwuiNavigationEventNode::PayloadPinName);

	if (UEdGraphPin* EventPin = FindPin(SwuiNavigationEventNode::EventPinName))
	{
		EventPin->PinFriendlyName = LOCTEXT("EventPinLabel", "Event");
	}

	if (UEdGraphPin* PayloadPin = FindPin(SwuiNavigationEventNode::PayloadPinName))
	{
		PayloadPin->PinFriendlyName = LOCTEXT("PayloadPinLabel", "Payload");
	}

	Super::AllocateDefaultPins();
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

void UK2Node_SwuiNavigationEvent::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);

	if (ComponentPropertyName.IsNone())
	{
		CompilerContext.MessageLog.Error(TEXT("@@ must reference a valid SWUI navigation component property."), this);
		BreakAllNodeLinks();
		return;
	}

	if (!NavigationEventTag.IsValid())
	{
		CompilerContext.MessageLog.Error(TEXT("@@ must have a valid NavigationEventTag."), this);
		BreakAllNodeLinks();
		return;
	}

	UBlueprint* Blueprint = GetBlueprint();
	if (!Blueprint)
	{
		CompilerContext.MessageLog.Error(TEXT("@@ could not resolve its owning Blueprint."), this);
		BreakAllNodeLinks();
		return;
	}

	FObjectProperty* ComponentProperty = nullptr;
	if (Blueprint->SkeletonGeneratedClass)
	{
		ComponentProperty = FindFProperty<FObjectProperty>(Blueprint->SkeletonGeneratedClass, ComponentPropertyName);
	}
	if (!ComponentProperty && Blueprint->GeneratedClass)
	{
		ComponentProperty = FindFProperty<FObjectProperty>(Blueprint->GeneratedClass, ComponentPropertyName);
	}

	FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(
		USwuiNavigation::StaticClass(),
		GET_MEMBER_NAME_CHECKED(USwuiNavigation, OnNavigationEvent));
	if (!ComponentProperty || !DelegateProperty)
	{
		CompilerContext.MessageLog.Error(TEXT("@@ could not resolve OnNavigationEvent binding metadata."), this);
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

	UK2Node_ComponentBoundEvent* BoundEventNode = CompilerContext.SpawnIntermediateNode<UK2Node_ComponentBoundEvent>(this, SourceGraph);
	BoundEventNode->InitializeComponentBoundEventParams(ComponentProperty, DelegateProperty);
	BoundEventNode->AllocateDefaultPins();

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

	UEdGraphPin* BoundThenPin = BoundEventNode->FindPinChecked(UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* BoundEventPin = BoundEventNode->FindPinChecked(SwuiNavigationEventNode::EventPinName);
	UEdGraphPin* BoundPayloadPin = BoundEventNode->FindPinChecked(SwuiNavigationEventNode::PayloadPinName);

	Schema->TryCreateConnection(BoundThenPin, BranchNode->FindPinChecked(UEdGraphSchema_K2::PN_Execute));
	Schema->TryCreateConnection(BoundEventPin, EqualNode->FindPinChecked(TEXT("A")));
	Schema->TryCreateConnection(LiteralNode->GetReturnValuePin(), EqualNode->FindPinChecked(TEXT("B")));
	Schema->TryCreateConnection(EqualNode->GetReturnValuePin(), BranchNode->FindPinChecked(UEdGraphSchema_K2::PN_Condition));

	CompilerContext.MovePinLinksToIntermediate(*ThenPin, *BranchNode->FindPinChecked(UEdGraphSchema_K2::PN_Then));
	CompilerContext.MovePinLinksToIntermediate(*EventPin, *BoundEventPin);
	CompilerContext.MovePinLinksToIntermediate(*PayloadPin, *BoundPayloadPin);
	BranchNode->FindPinChecked(UEdGraphSchema_K2::PN_Else)->BreakAllPinLinks();

	BreakAllNodeLinks();
}

#undef LOCTEXT_NAMESPACE