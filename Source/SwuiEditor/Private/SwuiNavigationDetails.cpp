#include "SwuiNavigationDetails.h"
#include "K2Node_SwuiNavigationEvent.h"
#include "SwuiNavigation.h"
#include "SwuiTSGenerator.h"
#include "Swui.h"

#include "BlueprintEditor.h"
#include "BlueprintEditorModule.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GameplayTagContainer.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "IDetailGroup.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/ComponentEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "PropertyCustomizationHelpers.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsEditorModule.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ToolMenus.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SBoxPanel.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraph/EdGraphPin.h"

#include "EdGraph/EdGraphNodeUtils.h"

#include "EdGraph/EdGraphNode.h"

#include "EdGraphSchema_K2_Actions.h"

#define LOCTEXT_NAMESPACE "SwuiNavigationDetails"

// ---------------------------------------------------------------------------

TSharedRef<IDetailCustomization> FSwuiNavigationDetails::MakeInstance()
{
	return MakeShareable(new FSwuiNavigationDetails);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void ShowNavigationEventNotification(const FText& Message, bool bSuccess)
{
	FNotificationInfo Info(Message);
	Info.bFireAndForget = true;
	Info.FadeInDuration = 0.2f;
	Info.FadeOutDuration = 0.5f;
	Info.ExpireDuration = 3.f;
	Info.Image = FAppStyle::GetBrush(bSuccess
		? TEXT("NotificationList.SuccessImage")
		: TEXT("NotificationList.FailImage"));
	FSlateNotificationManager::Get().AddNotification(Info);
}

static bool IsExcludedCustomNamespaceTag(const FGameplayTag& Tag)
{
	const FName TagName = Tag.GetTagName();
	return TagName == TEXT("swui.menu")
		|| TagName == TEXT("swui.navigation")
		|| TagName == TEXT("swui.pointer");
}

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

static FText MakeFriendlyNavigationEventLabel(const FGameplayTag& Tag)
{
	TArray<FString> Segments;
	Tag.GetTagName().ToString().ParseIntoArray(Segments, TEXT("."), true);

	if (Segments.Num() > 0 && Segments[0].Equals(TEXT("swui"), ESearchCase::IgnoreCase))
	{
		Segments.RemoveAt(0);
	}

	FString Label = TEXT("On");
	for (const FString& Segment : Segments)
	{
		const FString Humanized = HumanizeTagSegment(Segment);
		if (!Humanized.IsEmpty())
		{
			Label += TEXT(" ") + Humanized;
		}
	}

	return FText::FromString(Label);
}

static UBlueprint* FindOwningBlueprint(USwuiNavigation* Nav)
{
	if (!Nav)
	{
		return nullptr;
	}

	auto ResolveBlueprintFromObject = [](const UObject* Object) -> UBlueprint*
	{
		for (const UObject* Current = Object; Current; Current = Current->GetOuter())
		{
			if (UBlueprint* Blueprint = const_cast<UBlueprint*>(Cast<UBlueprint>(Current)))
			{
				return Blueprint;
			}

			if (const UBlueprintGeneratedClass* GeneratedClass = Cast<UBlueprintGeneratedClass>(Current))
			{
				if (UBlueprint* Blueprint = Cast<UBlueprint>(GeneratedClass->ClassGeneratedBy))
				{
					return Blueprint;
				}
			}

			if (const AActor* Actor = Cast<AActor>(Current))
			{
				if (UBlueprint* Blueprint = Cast<UBlueprint>(Actor->GetClass()->ClassGeneratedBy))
				{
					return Blueprint;
				}
			}
		}

		if (const AActor* Actor = Cast<AActor>(Object))
		{
			if (UBlueprint* Blueprint = Cast<UBlueprint>(Actor->GetClass()->ClassGeneratedBy))
			{
				return Blueprint;
			}
		}

		if (const UActorComponent* Component = Cast<UActorComponent>(Object))
		{
			if (const AActor* Owner = Component->GetOwner())
			{
				if (UBlueprint* Blueprint = Cast<UBlueprint>(Owner->GetClass()->ClassGeneratedBy))
				{
					return Blueprint;
				}
			}
		}

		return nullptr;
	};

	if (UBlueprint* Blueprint = ResolveBlueprintFromObject(Nav))
	{
		return Blueprint;
	}

	if (UObject* Archetype = Nav->GetArchetype())
	{
		if (UBlueprint* Blueprint = ResolveBlueprintFromObject(Archetype))
		{
			return Blueprint;
		}
	}

	if (AActor* Owner = Nav->GetOwner())
	{
		if (UBlueprint* Blueprint = ResolveBlueprintFromObject(Owner))
		{
			return Blueprint;
		}
	}

	return Nav->GetTypedOuter<UBlueprint>();
}

static USwui* ResolveTargetSwuiForEditor(USwuiNavigation* Nav)
{
	if (!Nav)
	{
		return nullptr;
	}

	if (USwui* RuntimeTarget = Nav->GetTargetSwui())
	{
		return RuntimeTarget;
	}

	UBlueprint* Blueprint = FindOwningBlueprint(Nav);
	if (!Blueprint)
	{
		return nullptr;
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript.Get();
	if (!SCS)
	{
		return nullptr;
	}

	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (!Node || !Node->ComponentTemplate)
		{
			continue;
		}

		if (USwui* Swui = Cast<USwui>(Node->ComponentTemplate))
		{
			return Swui;
		}
	}

	return nullptr;
}

static TSharedPtr<IBlueprintEditor> FindOwningBlueprintEditor(UBlueprint* Blueprint, USwuiNavigation* Nav)
{
	auto MatchesBlueprint = [Blueprint](const TSharedPtr<IBlueprintEditor>& Candidate) -> bool
	{
		TSharedPtr<FBlueprintEditor> ConcreteEditor = StaticCastSharedPtr<FBlueprintEditor>(Candidate);
		return ConcreteEditor.IsValid() && ConcreteEditor->GetBlueprintObj() == Blueprint;
	};

	if (TSharedPtr<IBlueprintEditor> EditorFromObject = FKismetEditorUtilities::GetIBlueprintEditorForObject(Nav, false))
	{
		if (MatchesBlueprint(EditorFromObject))
		{
			return EditorFromObject;
		}
	}

	if (TSharedPtr<IBlueprintEditor> EditorFromBlueprint = FKismetEditorUtilities::GetIBlueprintEditorForObject(Blueprint, false))
	{
		if (MatchesBlueprint(EditorFromBlueprint))
		{
			return EditorFromBlueprint;
		}
	}

	if (FModuleManager::Get().IsModuleLoaded(TEXT("Kismet")))
	{
		FBlueprintEditorModule& BlueprintEditorModule = FModuleManager::GetModuleChecked<FBlueprintEditorModule>(TEXT("Kismet"));
		for (const TSharedRef<IBlueprintEditor>& Candidate : BlueprintEditorModule.GetBlueprintEditors())
		{
			if (MatchesBlueprint(Candidate))
			{
				return Candidate;
			}
		}
	}

	return nullptr;
}

static UEdGraph* FindUsableBlueprintGraph(UBlueprint* Blueprint, const TSharedPtr<IBlueprintEditor>& BlueprintEditor)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	if (BlueprintEditor.IsValid())
	{
		if (UEdGraph* FocusedGraph = BlueprintEditor->GetFocusedGraph())
		{
			if (UEdGraph* TopLevelGraph = FBlueprintEditorUtils::GetTopLevelGraph(FocusedGraph))
			{
				if (FBlueprintEditorUtils::FindBlueprintForGraph(TopLevelGraph) == Blueprint && Blueprint->UbergraphPages.Contains(TopLevelGraph))
				{
					return TopLevelGraph;
				}
			}
		}
	}

	if (UEdGraph* LastEditedGraph = Blueprint->GetLastEditedUberGraph())
	{
		if (FBlueprintEditorUtils::FindBlueprintForGraph(LastEditedGraph) == Blueprint)
		{
			return LastEditedGraph;
		}
	}

	if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint))
	{
		return EventGraph;
	}

	if (Blueprint->UbergraphPages.Num() > 0)
	{
		return Blueprint->UbergraphPages[0];
	}

	return nullptr;
}

static bool ResolveBlueprintGraphContext(
	USwuiNavigation* Nav,
	UBlueprint*& OutBlueprint,
	TSharedPtr<IBlueprintEditor>& OutBlueprintEditor,
	TSharedPtr<FBlueprintEditor>& OutConcreteBlueprintEditor,
	UEdGraph*& OutTargetGraph)
{
	OutBlueprint = FindOwningBlueprint(Nav);
	if (!OutBlueprint)
	{
		ShowNavigationEventNotification(
			LOCTEXT("NoOwningBlueprintForSwuiNavigation", "Could not find the Blueprint that owns this SwuiNavigation component."),
			false);
		return false;
	}

	OutBlueprintEditor = FindOwningBlueprintEditor(OutBlueprint, Nav);
	OutConcreteBlueprintEditor = StaticCastSharedPtr<FBlueprintEditor>(OutBlueprintEditor);
	OutTargetGraph = FindUsableBlueprintGraph(OutBlueprint, OutBlueprintEditor);
	if (!OutTargetGraph)
	{
		ShowNavigationEventNotification(
			LOCTEXT("NoUsableBlueprintGraph", "No usable Blueprint graph found."),
			false);
		return false;
	}

	return true;
}


static bool ResolveNavigationComponentVariableName(UBlueprint* Blueprint, const USwuiNavigation* Nav, FName& OutVariableName)
{
	OutVariableName = NAME_None;
	if (!Blueprint || !Nav)
		return false;

	UE_LOG(LogTemp, Warning, TEXT("[SWUI] ResolveNavigationComponentVariableName: Blueprint=%s Nav=%s"),
		*Blueprint->GetName(), *Nav->GetName());

	// Print outer chain
	{
		FString OuterChain;
		const UObject* Obj = Nav;
		while (Obj)
		{
			OuterChain += Obj->GetName();
			Obj = Obj->GetOuter();
			if (Obj) OuterChain += TEXT(" <- ");
		}
		UE_LOG(LogTemp, Warning, TEXT("[SWUI] Nav outer chain: %s"), *OuterChain);
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript.Get();
	if (SCS)
	{
		const TArray<USCS_Node*>& Nodes = SCS->GetAllNodes();
		FString NodeNames;
		for (USCS_Node* Node : Nodes)
		{
			if (Node)
			{
				NodeNames += Node->GetVariableName().ToString() + TEXT(", ");
			}
		}
		UE_LOG(LogTemp, Warning, TEXT("[SWUI] SCS Node names: %s"), *NodeNames);

		// 1. Exact template pointer match
		for (USCS_Node* Node : Nodes)
		{
			if (!Node || !Node->ComponentTemplate)
				continue;
			if (Node->ComponentTemplate == Nav)
			{
				OutVariableName = Node->GetVariableName();
				UE_LOG(LogTemp, Warning, TEXT("[SWUI] SCS exact match: %s"), *OutVariableName.ToString());
				return !OutVariableName.IsNone();
			}
		}
		// 2. Fallback: class match
		for (USCS_Node* Node : Nodes)
		{
			if (!Node || !Node->ComponentTemplate)
				continue;
			if (Node->ComponentTemplate->GetClass() == USwuiNavigation::StaticClass() ||
				Node->ComponentTemplate->IsA(USwuiNavigation::StaticClass()))
			{
				OutVariableName = Node->GetVariableName();
				UE_LOG(LogTemp, Warning, TEXT("[SWUI] SCS class match: %s"), *OutVariableName.ToString());
				return !OutVariableName.IsNone();
			}
		}
	}

	// 3. Fallback: search Blueprint generated/skeleton class for FObjectProperty of USwuiNavigation
	auto FindNavProperty = [](UStruct* Struct) -> FName
	{
		if (!Struct) return NAME_None;
		for (TFieldIterator<FObjectProperty> It(Struct); It; ++It)
		{
			FObjectProperty* Prop = *It;
			if (Prop && Prop->PropertyClass && Prop->PropertyClass->IsChildOf(USwuiNavigation::StaticClass()))
			{
				return Prop->GetFName();
			}
		}
		return NAME_None;
	};
	if (Blueprint->SkeletonGeneratedClass)
	{
		FName FallbackName = FindNavProperty(Blueprint->SkeletonGeneratedClass);
		if (!FallbackName.IsNone())
		{
			OutVariableName = FallbackName;
			UE_LOG(LogTemp, Warning, TEXT("[SWUI] SkeletonGeneratedClass fallback: %s"), *OutVariableName.ToString());
			return true;
		}
	}
	if (Blueprint->GeneratedClass)
	{
		FName FallbackName = FindNavProperty(Blueprint->GeneratedClass);
		if (!FallbackName.IsNone())
		{
			OutVariableName = FallbackName;
			UE_LOG(LogTemp, Warning, TEXT("[SWUI] GeneratedClass fallback: %s"), *OutVariableName.ToString());
			return true;
		}
	}

	// 4. Last fallback: use Nav->GetFName() if it resolves in Blueprint/SCS
	FName NavName = Nav->GetFName();
	if (SCS)
	{
		const TArray<USCS_Node*>& Nodes = SCS->GetAllNodes();
		for (USCS_Node* Node : Nodes)
		{
			if (Node && Node->GetVariableName() == NavName)
			{
				OutVariableName = NavName;
				UE_LOG(LogTemp, Warning, TEXT("[SWUI] Fallback: Nav->GetFName() matches SCS node: %s"), *OutVariableName.ToString());
				return true;
			}
		}
	}

	UE_LOG(LogTemp, Error, TEXT("[SWUI] Could not resolve SwuiNavigation component variable name."));
	return false;
}

static bool AddBlueprintNavigationEventNode(USwuiNavigation* Nav, const FGameplayTag& Tag)
{
	UBlueprint* Blueprint = nullptr;
	TSharedPtr<IBlueprintEditor> BlueprintEditor;
	TSharedPtr<FBlueprintEditor> ConcreteBlueprintEditor;
	UEdGraph* TargetGraph = nullptr;

	if (!ResolveBlueprintGraphContext(Nav, Blueprint, BlueprintEditor, ConcreteBlueprintEditor, TargetGraph))
	{
		return false;
	}

	FName ComponentVariableName;
	if (!ResolveNavigationComponentVariableName(Blueprint, Nav, ComponentVariableName))
	{
		ShowNavigationEventNotification(
			LOCTEXT("ResolveSwuiNavigationComponentFailed", "Could not resolve SwuiNavigation component variable in Blueprint."),
			false);
		return false;
	}

	TArray<UK2Node_SwuiNavigationEvent*> ExistingNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, ExistingNodes);
	for (UK2Node_SwuiNavigationEvent* ExistingNode : ExistingNodes)
	{
		if (!ExistingNode)
		{
			continue;
		}

		if (ExistingNode->ComponentPropertyName == ComponentVariableName && ExistingNode->NavigationEventTag == Tag)
		{
			if (UEdGraph* ExistingGraph = ExistingNode->GetGraph())
			{
				BlueprintEditor->OpenGraphAndBringToFront(ExistingGraph, true);
			}

			BlueprintEditor->AddToSelection(ExistingNode);
			if (ConcreteBlueprintEditor.IsValid())
			{
				ConcreteBlueprintEditor->JumpToNode(ExistingNode, false);
			}
			else
			{
				FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(ExistingNode, false);
			}

			FNotificationInfo Info(LOCTEXT("DuplicateSwuiNavigationEventNodeTitle", "SWUI navigation event already exists"));
			Info.bFireAndForget = true;
			Info.FadeInDuration = 0.2f;
			Info.FadeOutDuration = 0.5f;
			Info.ExpireDuration = 3.f;
			Info.Image = FAppStyle::GetBrush(TEXT("Icons.WarningWithColor"));
			Info.SubText = FText::Format(
				LOCTEXT("DuplicateSwuiNavigationEventNodeMessage", "A BP event for {0} already exists on this SwuiNavigation component."),
				FText::FromString(Tag.GetTagName().ToString()));

			if (TSharedPtr<SNotificationItem> NotificationItem = FSlateNotificationManager::Get().AddNotification(Info))
			{
				NotificationItem->SetCompletionState(SNotificationItem::CS_None);
			}
			return false;
		}
	}

	BlueprintEditor->OpenGraphAndBringToFront(TargetGraph, true);

	FVector2D NodePosition = TargetGraph->GetGoodPlaceForNewNode();
	if (ConcreteBlueprintEditor.IsValid() && BlueprintEditor->GetFocusedGraph() == TargetGraph)
	{
		float ZoomAmount = 1.f;
		ConcreteBlueprintEditor->GetViewLocation(NodePosition, ZoomAmount);
		NodePosition += FVector2D(96.f, 96.f);
	}

	const FScopedTransaction Transaction(LOCTEXT("AddSwuiNavigationEventNodeTransaction", "Add SWUI Navigation Event"));
	Blueprint->Modify();
	TargetGraph->Modify();

	UK2Node_SwuiNavigationEvent* NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_SwuiNavigationEvent>(
		TargetGraph,
		NodePosition,
		EK2NewNodeFlags::SelectNewNode,
		[ComponentVariableName, Tag](UK2Node_SwuiNavigationEvent* NewInstance)
		{
			NewInstance->ComponentPropertyName = ComponentVariableName;
			NewInstance->NavigationEventTag = Tag;
		});

	if (!NewNode)
	{
		ShowNavigationEventNotification(
			LOCTEXT("CreateSwuiNavigationNodeFailed", "Could not create SWUI navigation event node."),
			false);
		return false;
	}

	BlueprintEditor->AddToSelection(NewNode);
	if (ConcreteBlueprintEditor.IsValid())
	{
		ConcreteBlueprintEditor->JumpToNode(NewNode, false);
	}
	else
	{
		FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(NewNode, false);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	ShowNavigationEventNotification(
		FText::Format(
			LOCTEXT("AddedBlueprintEventNode", "Added BP event for {0}"),
			FText::FromString(Tag.GetTagName().ToString())),
		true);
	return true;
}

// Forward declaration for GatherSwuiTags exclusion.
static TArray<FGameplayTag> GatherSwuiEventTags();

/** Gather all registered Gameplay Tags that start with "swui." */
static void GatherSwuiTags(TArray<FGameplayTag>& OutDefault, TArray<FGameplayTag>& OutCustom)
{
	const TSet<FName>& BuiltIn = FSwuiNavTags::GetAllBuiltInTagNames();
	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();

	// Tags discovered via SwuiEvent metadata — these appear in their own
	// "Events from C++ / AngelScript Metadata" section; exclude from Custom.
	TSet<FName> SwuiEventTagNames;
	for (const FGameplayTag& ET : GatherSwuiEventTags())
		SwuiEventTagNames.Add(ET.GetTagName());

	FGameplayTagContainer AllTags;
	TagManager.RequestAllGameplayTags(AllTags, /*bOnlyIncludeDictionaryTags=*/false);

	for (const FGameplayTag& Tag : AllTags)
	{
		const FString Name = Tag.GetTagName().ToString();
		if (!Name.StartsWith(TEXT("swui."))) continue;

		if (BuiltIn.Contains(Tag.GetTagName()))
		{
			OutDefault.Add(Tag);
		}
		else if (!IsExcludedCustomNamespaceTag(Tag) && !SwuiEventTagNames.Contains(Tag.GetTagName()))
		{
			OutCustom.Add(Tag);
		}
		else
		{
			continue;
		}
	}

	OutDefault.Sort([](const FGameplayTag& A, const FGameplayTag& B)
	{
		return A.GetTagName().LexicalLess(B.GetTagName());
	});
	OutCustom.Sort([](const FGameplayTag& A, const FGameplayTag& B)
	{
		return A.GetTagName().LexicalLess(B.GetTagName());
	});
}

/**
 * Collect all event tags declared via SwuiEvent metadata on UFUNCTIONs
 * across all loaded UClasses (C++ and AngelScript).
 * Registers any missing tags via the editor tag module (persists to INI).
 */
static TArray<FGameplayTag> GatherSwuiEventTags()
{
	TArray<FGameplayTag> Result;
	TSet<FName> Seen;

	UGameplayTagsManager& TagMgr = UGameplayTagsManager::Get();

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Cls = *It;
		if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;
		if (Cls->GetName().StartsWith(TEXT("SKEL_")) || Cls->GetName().StartsWith(TEXT("REINST_"))) continue;

		for (TFieldIterator<UFunction> FnIt(Cls, EFieldIteratorFlags::ExcludeSuper); FnIt; ++FnIt)
		{
			const FString EventTag = FnIt->GetMetaData(TEXT("SwuiEvent"));
			if (EventTag.IsEmpty()) continue;
			if (!FGameplayTag::IsValidGameplayTagString(EventTag)) continue;

			const FName TagFName(*EventTag);
			if (Seen.Contains(TagFName)) continue;
			Seen.Add(TagFName);

			FGameplayTag Tag = TagMgr.RequestGameplayTag(TagFName, /*bErrorIfNotFound=*/false);
			if (!Tag.IsValid())
			{
				// Tag doesn't exist yet — register it via the editor module.
				IGameplayTagsEditorModule& TagEditor = IGameplayTagsEditorModule::Get();
				if (TagEditor.AddNewGameplayTagToINI(EventTag, TEXT("SWUI navigation event from C++/AS metadata")))
				{
					TagMgr.EditorRefreshGameplayTagTree();
					Tag = TagMgr.RequestGameplayTag(TagFName, /*bErrorIfNotFound=*/false);
				}
			}

			if (Tag.IsValid())
			{
				Result.Add(Tag);
			}
		}
	}

	Result.Sort([](const FGameplayTag& A, const FGameplayTag& B)
	{
		return A.GetTagName().LexicalLess(B.GetTagName());
	});
	return Result;
}

/** Check if a Navigation Event entry exists for the given tag. */
static bool HasNavigationEvent(const USwuiNavigation* Nav, const FGameplayTag& Tag)
{
	for (const FSwuiNavigationEvent& Evt : Nav->NavigationEvents)
	{
		if (Evt.Event == Tag) return true;
	}
	return false;
}

/** Add a Navigation Event entry for the tag. */
static void AddNavigationEvent(USwuiNavigation* Nav, const FGameplayTag& Tag)
{
	if (HasNavigationEvent(Nav, Tag)) return;
	Nav->Modify();
	FSwuiNavigationEvent Evt;
	Evt.Event = Tag;
	Nav->NavigationEvents.Add(Evt);
}

/** Remove the Navigation Event entry for the tag. */
static void RemoveNavigationEvent(USwuiNavigation* Nav, const FGameplayTag& Tag)
{
	Nav->Modify();
	Nav->NavigationEvents.RemoveAll([&Tag](const FSwuiNavigationEvent& Evt)
	{
		return Evt.Event == Tag;
	});
}

/** Validate a user-entered tag suffix. Returns empty string on success, error message on failure. */
static FString ValidateTagSuffix(const FString& Suffix)
{
	if (Suffix.IsEmpty())
		return TEXT("Suffix cannot be empty.");

	if (Suffix.StartsWith(TEXT(".")))
		return TEXT("Suffix cannot start with a dot.");

	if (Suffix.EndsWith(TEXT(".")))
		return TEXT("Suffix cannot end with a dot.");

	if (Suffix.Contains(TEXT("..")))
		return TEXT("Suffix cannot contain consecutive dots.");

	// Check for invalid characters (only alphanumeric and dots allowed).
	for (TCHAR Ch : Suffix)
	{
		if (!FChar::IsAlnum(Ch) && Ch != TEXT('.'))
			return FString::Printf(TEXT("Invalid character '%c'. Use alphanumeric and dots only."), Ch);
	}

	return FString();
}

// ---------------------------------------------------------------------------
// UI builder — adds one row per tag with a checkbox
// ---------------------------------------------------------------------------

static void AddTagRows(
	IDetailGroup& Group,
	const TArray<FGameplayTag>& Tags,
	USwuiNavigation* Nav,
	IDetailLayoutBuilder* Builder,
	bool bAllowRemove)
{
	if (Tags.IsEmpty())
	{
		Group.AddWidgetRow()
			.WholeRowContent()
			[
				SNew(STextBlock)
				.Text(bAllowRemove
					? LOCTEXT("NoCustomTags", "No custom swui.* tags found.")
					: LOCTEXT("NoDefaultTags", "No built-in swui.* tags found."))
				.Font(IDetailLayoutBuilder::GetDetailFont())
			];
		return;
	}

	for (const FGameplayTag& Tag : Tags)
	{
		const FString TagStr = Tag.GetTagName().ToString();
		auto NameWidget = SNew(STextBlock)
			.Text(FText::FromString(TagStr))
			.Font(IDetailLayoutBuilder::GetDetailFont());

		if (!bAllowRemove)
		{
			// Default tags: just the name, no controls.
			Group.AddWidgetRow()
				.NameContent()[NameWidget]
				.ValueContent()[SNullWidget::NullWidget];
		}
		else
		{
			// Custom tags: checkbox + add BP event + delete.
			auto ValueBox = SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.IsChecked(TAttribute<ECheckBoxState>::CreateLambda([Nav, Tag]()
					{
						return HasNavigationEvent(Nav, Tag)
							? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					}))
					.OnCheckStateChanged_Lambda([Nav, Tag, Builder](ECheckBoxState NewState)
					{
						if (NewState == ECheckBoxState::Checked)
							AddNavigationEvent(Nav, Tag);
						else
							RemoveNavigationEvent(Nav, Tag);
						if (Builder) Builder->ForceRefreshDetails();
					})
				];

			// Compact add button.
			ValueBox->AddSlot()
				.AutoWidth()
				.Padding(8.f, 0.f, 0.f, 0.f)
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.Text(LOCTEXT("AddBPEventCompact", "+"))
					.ToolTipText(LOCTEXT("AddBPEventTip", "Add BP Event"))
					.ContentPadding(FMargin(8.f, 0.f))
					.OnClicked_Lambda([Nav, Tag]()
					{
						AddBlueprintNavigationEventNode(Nav, Tag);
						return FReply::Handled();
					})
				];

			// Delete button
			ValueBox->AddSlot()
				.AutoWidth()
				.Padding(4.f, 0.f, 0.f, 0.f)
				.VAlign(VAlign_Center)
				[
					PropertyCustomizationHelpers::MakeDeleteButton(
						FSimpleDelegate::CreateLambda([Nav, Tag, Builder]()
						{
							const FText Msg = FText::Format(
								LOCTEXT("RemoveTagConfirm", "Remove custom tag '{0}' from the project?\nThis also removes its Navigation Event entry."),
								FText::FromString(Tag.GetTagName().ToString()));

							if (FMessageDialog::Open(EAppMsgType::YesNo, Msg) != EAppReturnType::Yes)
								return;

							RemoveNavigationEvent(Nav, Tag);

							IGameplayTagsEditorModule& TagEditor =
								IGameplayTagsEditorModule::Get();
							TSharedPtr<FGameplayTagNode> TagNode = UGameplayTagsManager::Get().FindTagNode(Tag.GetTagName());
							if (TagNode.IsValid())
							{
								TagEditor.DeleteTagFromINI(TagNode);
							}
							UGameplayTagsManager::Get().EditorRefreshGameplayTagTree();

							if (Builder) Builder->ForceRefreshDetails();
						})
					)
				];

			Group.AddWidgetRow()
				.NameContent()[NameWidget]
				.ValueContent()[ValueBox];
		}
	}
}

// ---------------------------------------------------------------------------
// CustomizeDetails
// ---------------------------------------------------------------------------

void FSwuiNavigationDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	CachedDetailBuilder = &DetailBuilder;

	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	if (Objects.Num() == 0) return;

	USwuiNavigation* Nav = Cast<USwuiNavigation>(Objects[0].Get());
	if (!Nav) return;
	NavPtr = Nav;

	// Ensure built-in tags are registered.
	FSwuiNavTags::Get();
	UGameplayTagsManager::Get().EditorRefreshGameplayTagTree();

	// Hide the raw NavigationEvents array — we draw our own UI.
	TSharedRef<IPropertyHandle> EventsHandle =
		DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(USwuiNavigation, NavigationEvents));
	DetailBuilder.HideProperty(EventsHandle);

	IDetailCategoryBuilder& Cat = DetailBuilder.EditCategory(
		"SWUI|Navigation",
		LOCTEXT("NavCat", "Navigation Events"),
		ECategoryPriority::Important);

	Cat.AddCustomRow(LOCTEXT("DefaultEventsNoteRow", "Default Events Note"))
	.WholeRowContent()
	[
		SNew(STextBlock)
		.Text(LOCTEXT("DefaultEventsNote",
			"These are GameplayTag event channels that SWUI can emit, receive, forward to JS, and expose as Blueprint listeners."))
		.Font(IDetailLayoutBuilder::GetDetailFont())
		.AutoWrapText(true)
	];

	// ---- Refresh Navigation Events JS Bindings (top of section) ----
	Cat.AddCustomRow(LOCTEXT("RefreshNavRow", "Refresh"))
	.WholeRowContent()
	.HAlign(HAlign_Right)
	[
		SNew(SButton)
		.Text(LOCTEXT("RefreshNavBtn", "Refresh Navigation Events JS Bindings"))
		.ToolTipText(LOCTEXT("RefreshNavBtnTip",
			"Re-generates JS binding stubs for the configured navigation events."))
		.OnClicked_Lambda([this]()
		{
			bool bOK = false;
			if (NavPtr.IsValid())
			{
				USwui* Swui = ResolveTargetSwuiForEditor(NavPtr.Get());
				if (Swui)
				{
					bOK = FSwuiTSGenerator::GenerateNavigation(Swui, NavPtr->NavigationEvents);
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("SWUI: Navigation JS bindings generation failed because no sibling USwui component could be resolved for this SwuiNavigation."));
				}
			}

			FNotificationInfo Info(bOK
				? LOCTEXT("GenOK",  "Navigation JS Bindings generated.")
				: LOCTEXT("GenFail", "Navigation JS Bindings generation failed — check the Output Log."));
			Info.bFireAndForget = true;
			Info.FadeInDuration  = 0.2f;
			Info.FadeOutDuration = 0.5f;
			Info.ExpireDuration  = 3.f;
			Info.Image = FAppStyle::GetBrush(bOK
				? TEXT("NotificationList.SuccessImage") : TEXT("NotificationList.FailImage"));
			FSlateNotificationManager::Get().AddNotification(Info);
			return FReply::Handled();
		})
	];

	// ---- Gather tags ----
	TArray<FGameplayTag> DefaultTags, CustomTags;
	GatherSwuiTags(DefaultTags, CustomTags);

	// ---- Default Events group ----
	IDetailGroup& DefaultGroup = Cat.AddGroup(
		TEXT("SwuiDefaultEvents"),
		LOCTEXT("DefaultEventsHeader", "Default SWUI Events"),
		/*bForAdvanced=*/false, /*bStartExpanded=*/true);
	AddTagRows(DefaultGroup, DefaultTags, Nav, CachedDetailBuilder, /*bAllowRemove=*/false);

	// ---- Custom Events group ----
	IDetailGroup& CustomGroup = Cat.AddGroup(
		TEXT("SwuiCustomEvents"),
		LOCTEXT("CustomEventsHeader", "Custom SWUI Events"),
		/*bForAdvanced=*/false, /*bStartExpanded=*/true);

	// ---- Add New row (at top of Custom Events) ----
	TSharedRef<SEditableTextBox> SuffixBox = SNew(SEditableTextBox)
		.Font(IDetailLayoutBuilder::GetDetailFont())
		.HintText(LOCTEXT("SuffixHint", "pause.open"));

	CustomGroup.AddWidgetRow()
	.WholeRowContent()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 2.f, 0.f, 2.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("FixedPrefix", "swui."))
			.Font(IDetailLayoutBuilder::GetDetailFontBold())
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 2.f)
		[
			SuffixBox
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 2.f, 0.f, 2.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("AddBtn", "Add"))
			.ToolTipText(LOCTEXT("AddBtnTip",
				"Create a Gameplay Tag with the swui. prefix and enable it as a Navigation Event."))
			.OnClicked_Lambda([this, SuffixBox]()
			{
				if (!NavPtr.IsValid()) return FReply::Handled();

				const FString Suffix = SuffixBox->GetText().ToString().TrimStartAndEnd();

				// Validate suffix.
				const FString Error = ValidateTagSuffix(Suffix);
				if (!Error.IsEmpty())
				{
					FNotificationInfo Info(FText::FromString(Error));
					Info.bFireAndForget = true;
					Info.ExpireDuration = 3.f;
					Info.Image = FAppStyle::GetBrush(TEXT("NotificationList.FailImage"));
					FSlateNotificationManager::Get().AddNotification(Info);
					return FReply::Handled();
				}

				const FString FullTagName = TEXT("swui.") + Suffix;
				const FName TagFName(*FullTagName);

				// Check for duplicates across all known tags.
				FGameplayTag Existing = UGameplayTagsManager::Get().RequestGameplayTag(
					TagFName, /*bErrorIfNotFound=*/false);
				if (Existing.IsValid())
				{
					FNotificationInfo Info(FText::Format(
						LOCTEXT("DuplicateTag", "Tag '{0}' already exists."),
						FText::FromString(FullTagName)));
					Info.bFireAndForget = true;
					Info.ExpireDuration = 3.f;
					Info.Image = FAppStyle::GetBrush(TEXT("NotificationList.FailImage"));
					FSlateNotificationManager::Get().AddNotification(Info);

					// If it exists but isn't enabled, enable it now.
					if (!HasNavigationEvent(NavPtr.Get(), Existing))
					{
						AddNavigationEvent(NavPtr.Get(), Existing);
					}

					if (CachedDetailBuilder) CachedDetailBuilder->ForceRefreshDetails();
					return FReply::Handled();
				}

				// Create the tag via the editor module (persists to project INI).
				IGameplayTagsEditorModule& TagEditor = IGameplayTagsEditorModule::Get();
				const bool bAdded = TagEditor.AddNewGameplayTagToINI(
					FullTagName, TEXT("Custom SWUI navigation event"));

				if (bAdded)
				{
					UGameplayTagsManager::Get().EditorRefreshGameplayTagTree();
					FGameplayTag NewTag = UGameplayTagsManager::Get().RequestGameplayTag(
						TagFName, /*bErrorIfNotFound=*/false);
					if (NewTag.IsValid())
					{
						AddNavigationEvent(NavPtr.Get(), NewTag);
					}
				}
				else
				{
					FNotificationInfo Info(FText::Format(
						LOCTEXT("AddFail", "Failed to create tag '{0}'."),
						FText::FromString(FullTagName)));
					Info.bFireAndForget = true;
					Info.ExpireDuration = 3.f;
					Info.Image = FAppStyle::GetBrush(TEXT("NotificationList.FailImage"));
					FSlateNotificationManager::Get().AddNotification(Info);
				}

				if (CachedDetailBuilder) CachedDetailBuilder->ForceRefreshDetails();
				return FReply::Handled();
			})
		]
	];

	AddTagRows(CustomGroup, CustomTags, Nav, CachedDetailBuilder, /*bAllowRemove=*/true);

	// ---- SwuiEvent (C++/AS metadata) Events group ----
	// Events discovered from SwuiEvent metadata on UFUNCTIONs across all loaded
	// UClasses. Read-only except for the enable/disable checkbox.
	TArray<FGameplayTag> SwuiEventTags = GatherSwuiEventTags();
	if (!SwuiEventTags.IsEmpty())
	{
		IDetailGroup& MetadataGroup = Cat.AddGroup(
			TEXT("SwuiMetadataEvents"),
			LOCTEXT("SwuiMetadataEventsHeader", "Events from C++ / AngelScript Metadata"),
			/*bForAdvanced=*/false, /*bStartExpanded=*/true);

		// Explanation text
		MetadataGroup.AddWidgetRow()
		.WholeRowContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("MetadataEventsNote",
				"Navigation events declared via SwuiEvent metadata on UFUNCTIONs in C++ or AngelScript code. "
				"Check to enable as a runtime Navigation Event."))
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.AutoWrapText(true)
		];

		for (const FGameplayTag& Tag : SwuiEventTags)
		{
			const FString TagStr = Tag.GetTagName().ToString();

			auto NameWidget = SNew(STextBlock)
				.Text(FText::FromString(TagStr))
				.Font(IDetailLayoutBuilder::GetDetailFont());

			auto ValueBox = SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.IsChecked(TAttribute<ECheckBoxState>::CreateLambda([Nav, Tag]()
					{
						return HasNavigationEvent(Nav, Tag)
							? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					}))
					.OnCheckStateChanged_Lambda([Nav, Tag, Builder = CachedDetailBuilder](ECheckBoxState NewState)
					{
						if (NewState == ECheckBoxState::Checked)
							AddNavigationEvent(Nav, Tag);
						else
							RemoveNavigationEvent(Nav, Tag);
						if (Builder) Builder->ForceRefreshDetails();
					})
				];

			// Add BP Event button
			ValueBox->AddSlot()
				.AutoWidth()
				.Padding(8.f, 0.f, 0.f, 0.f)
				.VAlign(VAlign_Center)
				[
					SNew(SButton)
					.Text(LOCTEXT("AddBPEventCompact", "+"))
					.ToolTipText(LOCTEXT("AddBPEventTip", "Add BP Event"))
					.ContentPadding(FMargin(8.f, 0.f))
					.OnClicked_Lambda([Nav, Tag]()
					{
						AddBlueprintNavigationEventNode(Nav, Tag);
						return FReply::Handled();
					})
				];

			MetadataGroup.AddWidgetRow()
				.NameContent()[NameWidget]
				.ValueContent()[ValueBox];
		}
	}
}

#undef LOCTEXT_NAMESPACE
