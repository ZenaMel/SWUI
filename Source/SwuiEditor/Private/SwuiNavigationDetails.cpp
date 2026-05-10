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
#include "IDetailGroup.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/ComponentEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Misc/MessageDialog.h"
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

	if (AActor* Owner = Nav->GetOwner())
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(Owner->GetClass()->ClassGeneratedBy))
		{
			return Blueprint;
		}
	}

	return Nav->GetTypedOuter<UBlueprint>();
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
			LOCTEXT("OpenBlueprintGraphToAddEvent", "Open a Blueprint graph to add this navigation event."),
			false);
		return false;
	}

	OutBlueprintEditor = FKismetEditorUtilities::GetIBlueprintEditorForObject(OutBlueprint, false);
	if (!OutBlueprintEditor.IsValid())
	{
		ShowNavigationEventNotification(
			LOCTEXT("OpenBlueprintGraphToAddEvent", "Open a Blueprint graph to add this navigation event."),
			false);
		return false;
	}

	OutConcreteBlueprintEditor = StaticCastSharedPtr<FBlueprintEditor>(OutBlueprintEditor);

	UEdGraph* FocusedGraph = OutBlueprintEditor->GetFocusedGraph();
	if (FocusedGraph && FBlueprintEditorUtils::FindBlueprintForGraph(FocusedGraph) == OutBlueprint && OutBlueprint->UbergraphPages.Contains(FocusedGraph))
	{
		OutTargetGraph = FocusedGraph;
		return true;
	}

	OutTargetGraph = FBlueprintEditorUtils::FindEventGraph(OutBlueprint);
	if (!OutTargetGraph)
	{
		ShowNavigationEventNotification(
			LOCTEXT("NoActiveBlueprintGraph", "No active Blueprint graph found."),
			false);
		return false;
	}

	return true;
}

static FObjectProperty* FindNavigationComponentProperty(UBlueprint* Blueprint, USwuiNavigation* Nav)
{
	if (!Blueprint || !Nav)
	{
		return nullptr;
	}

	const FName VariableName = FComponentEditorUtils::FindVariableNameGivenComponentInstance(Nav);
	if (VariableName.IsNone())
	{
		return nullptr;
	}

	if (Blueprint->SkeletonGeneratedClass)
	{
		if (FObjectProperty* Property = FindFProperty<FObjectProperty>(Blueprint->SkeletonGeneratedClass, VariableName))
		{
			return Property;
		}
	}

	if (Blueprint->GeneratedClass)
	{
		if (FObjectProperty* Property = FindFProperty<FObjectProperty>(Blueprint->GeneratedClass, VariableName))
		{
			return Property;
		}
	}

	return nullptr;
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

	FObjectProperty* ComponentProperty = FindNavigationComponentProperty(Blueprint, Nav);
	if (!ComponentProperty)
	{
		ShowNavigationEventNotification(
			LOCTEXT("CreateSwuiNavigationNodeFailed", "Could not create SWUI navigation event node."),
			false);
		return false;
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
		[ComponentProperty, Tag](UK2Node_SwuiNavigationEvent* NewInstance)
		{
			NewInstance->ComponentPropertyName = ComponentProperty->GetFName();
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

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	ShowNavigationEventNotification(
		FText::Format(
			LOCTEXT("AddedBlueprintEventNode", "Added BP event for {0}"),
			FText::FromString(Tag.GetTagName().ToString())),
		true);
	return true;
}

/** Gather all registered Gameplay Tags that start with "swui." */
static void GatherSwuiTags(TArray<FGameplayTag>& OutDefault, TArray<FGameplayTag>& OutCustom)
{
	const TSet<FName>& BuiltIn = FSwuiNavTags::GetAllBuiltInTagNames();
	UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();

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
		else if (!IsExcludedCustomNamespaceTag(Tag))
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
				USwui* Swui = NavPtr->GetTargetSwui();
				if (Swui)
					bOK = FSwuiTSGenerator::Generate(Swui);
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
		LOCTEXT("DefaultEventsHeader", "Default Events"),
		/*bForAdvanced=*/false, /*bStartExpanded=*/true);
	AddTagRows(DefaultGroup, DefaultTags, Nav, CachedDetailBuilder, /*bAllowRemove=*/false);

	// ---- Custom Events group ----
	IDetailGroup& CustomGroup = Cat.AddGroup(
		TEXT("SwuiCustomEvents"),
		LOCTEXT("CustomEventsHeader", "Custom Events"),
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
}

#undef LOCTEXT_NAMESPACE
