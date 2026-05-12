#include "SwuiDetails.h"
#include "Swui.h"
#include "SwuiBindingSource.h"
#include "SwuiTSGenerator.h"

#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailGroup.h"
#include "PropertyCustomizationHelpers.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "UObject/UnrealType.h"
#include "UObject/Field.h"
#include "Containers/Ticker.h"
#include "Styling/AppStyle.h"

#include "Framework/Application/SlateApplication.h"

#define LOCTEXT_NAMESPACE "SwuiDetails"

TSharedRef<IDetailCustomization> FSwuiDetails::MakeInstance()
{
return MakeShareable(new FSwuiDetails);
}

FSwuiDetails::~FSwuiDetails()
{
	if (RefreshTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RefreshTickerHandle);
	}
}

void FSwuiDetails::ScheduleDebouncedRefresh()
{
	bRefreshScheduled = true;

	// Remove existing ticker if any
	if (RefreshTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RefreshTickerHandle);
	}

	constexpr float DebounceDelay = 0.4f;
	RefreshTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this](float) -> bool
		{
			if (bRefreshScheduled)
			{
				bRefreshScheduled = false;
				RefreshTickerHandle.Reset();
				DoRefresh();
			}
			return false; // one-shot
		}),
		DebounceDelay);
}

static void SwuiShowRefreshNotification(bool bOK)
{
	FNotificationInfo Info(bOK
		? LOCTEXT("GenOK", "JS Bindings generated successfully.")
		: LOCTEXT("GenFail", "JS Bindings generation failed — check the Output Log."));

	Info.bFireAndForget = true;
	Info.FadeInDuration = 0.2f;
	Info.FadeOutDuration = 0.5f;
	Info.ExpireDuration = 3.f;
	Info.Image = FAppStyle::GetBrush(
		bOK
			? TEXT("NotificationList.SuccessImage")
			: TEXT("NotificationList.FailImage")
	);

	TSharedPtr<SNotificationItem> Notification =
		FSlateNotificationManager::Get().AddNotification(Info);

	if (Notification.IsValid())
	{
		Notification->SetCompletionState(
			bOK
				? SNotificationItem::CS_Success
				: SNotificationItem::CS_Fail
		);
	}
}

void FSwuiDetails::DoRefresh()
{
	if (SwuiPtr.IsValid())
	{
		const bool bOK = FSwuiTSGenerator::Generate(SwuiPtr.Get());
		SwuiShowRefreshNotification(bOK);
	}
}

void FSwuiDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
CachedDetailBuilder = &DetailBuilder;

TArray<TWeakObjectPtr<UObject>> Objects;
DetailBuilder.GetObjectsBeingCustomized(Objects);
if (Objects.Num() == 0) return;

USwui* Swui = Cast<USwui>(Objects[0].Get());
if (!Swui) return;
SwuiPtr = Swui;

// Resolve the owner actor class. Works for:
//   - Level instances (editor + PIE): GetOwner() returns the actor.
//   - Blueprint archetypes: GetOwner() is null; the outer chain is
//     Component -> BlueprintGeneratedClass, which IS a child of AActor.
auto FindOwnerClass = [](USwui* Comp) -> UClass*
{
	if (AActor* A = Comp->GetOwner()) return A->GetClass();
	if (AActor* A = Comp->GetTypedOuter<AActor>()) return A->GetClass();
	if (UClass* C = Comp->GetTypedOuter<UClass>())
		if (C->IsChildOf<AActor>()) return C;
	return nullptr;
};

// Auto-fill slot 0 with the owner class when needed.
{
	UClass* OwnerClass = FindOwnerClass(Swui);
	if (OwnerClass)
	{
		const bool bSlot0Wrong = Swui->BindingSources.IsEmpty() ||
			Swui->BindingSources[0].SourceClass != OwnerClass;
		if (bSlot0Wrong)
		{
			Swui->Modify();
			Swui->BindingSources.IsEmpty()
				? [&]{ FSwuiBindingSource S; S.SourceClass = OwnerClass; Swui->BindingSources.Insert(S, 0); }()
				: [&]{ Swui->BindingSources[0].SourceClass = OwnerClass; }();
		}
	}
}

// Get the array handle before hiding it — child handles remain valid.
TSharedRef<IPropertyHandle> BindingSourcesHandle =
	DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(USwui, BindingSources));
DetailBuilder.HideProperty(BindingSourcesHandle);
TSharedPtr<IPropertyHandleArray> ArrayHandle = BindingSourcesHandle->AsArray();

IDetailCategoryBuilder& Cat = DetailBuilder.EditCategory(
"SimpleWebUI|Bindings",
LOCTEXT("BindingsCat", "Web UI Bindings"),
ECategoryPriority::Important);

// ---- Generate button (top of section) ----
Cat.AddCustomRow(LOCTEXT("GenerateRow", "Generate"))
.WholeRowContent()
.HAlign(HAlign_Right)
[
SNew(SButton)
.Text(LOCTEXT("RefreshBtn", "Refresh JS Bindings"))
.ToolTipText(LOCTEXT("RefreshBtnTip",
"Generates TypeScript bindings into Content/UI/generated/ from the checked properties."))
.OnClicked_Lambda([this]()
{
	bool bOK = false;
	if (SwuiPtr.IsValid())
	{
		bOK = FSwuiTSGenerator::Generate(SwuiPtr.Get());
	}
	SwuiShowRefreshNotification(bOK);
	return FReply::Handled();
})
];

// ---- One collapsible group per binding source ----
for (int32 i = 0; i < Swui->BindingSources.Num(); ++i)
{
UClass* SourceClass = Swui->BindingSources[i].SourceClass;

// Group label: show only the bound class/object name.
// Keep the internal group ID indexed for uniqueness.
const FText SourceLabel = SourceClass
	? FText::FromString(SourceClass->GetName())
	: (i == 0
		? LOCTEXT("OwnerGroupEmpty", "Owner Class")
		: LOCTEXT("SourceGroupEmpty", "Unassigned Source"));

const FName SourceGroupId = FName(*FString::Printf(TEXT("SwuiSource%d"), i));
IDetailGroup& SourceGroup = Cat.AddGroup(SourceGroupId, SourceLabel, false);
// Native property handle for the SourceClass field in this slot.
TSharedPtr<IPropertyHandle> ElemHandle  = BindingSourcesHandle->GetChildHandle((uint32)i);
TSharedPtr<IPropertyHandle> ClassHandle = ElemHandle.IsValid()
	? ElemHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSwuiBindingSource, SourceClass))
	: nullptr;

if (ClassHandle.IsValid())
{
	ClassHandle->SetOnPropertyValueChanged(FSimpleDelegate::CreateLambda([this]()
	{
		if (CachedDetailBuilder) CachedDetailBuilder->ForceRefreshDetails();
	}));

	if (i == 0)
	{
		// Slot 0: greyed standard class picker (auto-filled from owner).
		SourceGroup.AddPropertyRow(ClassHandle.ToSharedRef())
			.DisplayName(LOCTEXT("OwnerClassProp", "Class"))
			.IsEnabled(false);
	}
	else
	{
		// Other slots: editable picker + Remove button.
		TSharedPtr<SWidget> DefaultName, DefaultValue;
		IDetailPropertyRow& Row = SourceGroup.AddPropertyRow(ClassHandle.ToSharedRef())
			.DisplayName(LOCTEXT("SourceClassProp", "Class"));
		Row.GetDefaultWidgets(DefaultName, DefaultValue);
		Row.CustomWidget()
			.NameContent()
			[
				DefaultName.ToSharedRef()
			]
			.ValueContent()
			.MinDesiredWidth(250.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f)
				[
					DefaultValue.ToSharedRef()
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f, 0.f, 0.f).VAlign(VAlign_Center)
				[
					PropertyCustomizationHelpers::MakeDeleteButton(
						FSimpleDelegate::CreateLambda([ArrayHandle, i, this]()
						{
							if (ArrayHandle.IsValid()) ArrayHandle->DeleteItem(i);
							if (CachedDetailBuilder) CachedDetailBuilder->ForceRefreshDetails();
						}),
						LOCTEXT("RemoveTip", "Remove this source class"))
				]
			];
	}
}

if (!SourceClass) continue;

// Nested collapsible group for the property checkboxes.
const FName StateGroupId = FName(*FString::Printf(TEXT("SwuiState%d"), i));
IDetailGroup& StateGroup = SourceGroup.AddGroup(StateGroupId, LOCTEXT("StatePropsGroup", "State Properties"), false);

// Build a category tree from properties to group them by UPROPERTY(Category="...") metadata.
struct FCategoryNode
{
	FString Name;
	TArray<FProperty*> Props;
	TArray<TSharedPtr<FCategoryNode>> Children;
};

auto FindOrAddChild = [](FCategoryNode& Node, const FString& Name) -> FCategoryNode*
{
	for (const TSharedPtr<FCategoryNode>& Child : Node.Children)
	{
		if (Child.IsValid() && Child->Name == Name)
		{
			return Child.Get();
		}
	}

	TSharedPtr<FCategoryNode> NewChild = MakeShared<FCategoryNode>();
	NewChild->Name = Name;
	Node.Children.Add(NewChild);
	return NewChild.Get();
};

FCategoryNode Root;

for (TFieldIterator<FProperty> It(SourceClass); It; ++It)
{
	FProperty* Prop = *It;
	if (!Prop->HasAnyPropertyFlags(CPF_BlueprintVisible)) continue;
	if (SwuiGetTSType(Prop).IsEmpty()) continue;

	const FString Category = Prop->HasMetaData(TEXT("Category"))
		? Prop->GetMetaData(TEXT("Category"))
		: FString();

	TArray<FString> Parts;
	Category.ParseIntoArray(Parts, TEXT("|"), true);

	FCategoryNode* Node = &Root;
	for (const FString& Part : Parts)
	{
		const FString CleanPart = Part.TrimStartAndEnd();
		if (CleanPart.IsEmpty()) continue;
		Node = FindOrAddChild(*Node, CleanPart);
	}
	Node->Props.Add(Prop);
}

// Recursively create nested groups for category segments.
int32 CatCounter = 0;
TFunction<void(const FCategoryNode&, IDetailGroup&)> BuildCategoryGroups;
BuildCategoryGroups = [&](const FCategoryNode& Node, IDetailGroup& ParentGroup)
{
	for (FProperty* Prop : Node.Props)
	{
		const FName PropName = Prop->GetFName();
		const FString TSType = SwuiGetTSType(Prop);

		ParentGroup.AddWidgetRow()
		.NameContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromName(PropName))
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("(%s)"), *TSType)))
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		]
		.ValueContent()
		[
			SNew(SCheckBox)
			.IsChecked(TAttribute<ECheckBoxState>::CreateLambda([Swui, i, PropName]()
			{
				if (!Swui->BindingSources.IsValidIndex(i)) return ECheckBoxState::Unchecked;
				return Swui->BindingSources[i].Properties.Contains(PropName)
					? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			}))
			.OnCheckStateChanged_Lambda([Swui, i, PropName, this](ECheckBoxState NewState)
			{
				if (!Swui->BindingSources.IsValidIndex(i)) return;
				Swui->Modify();
				if (NewState == ECheckBoxState::Checked)
					Swui->BindingSources[i].Properties.AddUnique(PropName);
				else
					Swui->BindingSources[i].Properties.Remove(PropName);
				this->ScheduleDebouncedRefresh();
			})
		];
	}

	for (const TSharedPtr<FCategoryNode>& Child : Node.Children)
	{
		if (!Child.IsValid()) continue;
		const FName ChildGroupId = FName(*FString::Printf(TEXT("SwuiCat%d_%d"), i, CatCounter++));
		IDetailGroup& ChildGroup = ParentGroup.AddGroup(ChildGroupId, FText::FromString(Child->Name), false);
		BuildCategoryGroups(*Child, ChildGroup);
	}
};

BuildCategoryGroups(Root, StateGroup);

// Nested collapsible group for delegate/event checkboxes.
const FName EventGroupId = FName(*FString::Printf(TEXT("SwuiEvents%d"), i));
IDetailGroup& EventGroup = SourceGroup.AddGroup(EventGroupId, LOCTEXT("EventsGroup", "Events"), false);

for (TFieldIterator<FMulticastDelegateProperty> It(SourceClass); It; ++It)
{
	FMulticastDelegateProperty* Prop = *It;
	if (!Prop->HasAnyPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintAssignable)) continue;

	const FName DelegateName = Prop->GetFName();

	EventGroup.AddWidgetRow()
	.NameContent()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(FText::FromName(DelegateName))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("EventType", "(event)"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
	]
	.ValueContent()
	[
		SNew(SCheckBox)
		.IsChecked(TAttribute<ECheckBoxState>::CreateLambda([Swui, i, DelegateName]()
		{
			if (!Swui->BindingSources.IsValidIndex(i)) return ECheckBoxState::Unchecked;
			return Swui->BindingSources[i].Delegates.Contains(DelegateName)
				? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		}))
		.OnCheckStateChanged_Lambda([Swui, i, DelegateName, this](ECheckBoxState NewState)
		{
			if (!Swui->BindingSources.IsValidIndex(i)) return;
			Swui->Modify();
			if (NewState == ECheckBoxState::Checked)
				Swui->BindingSources[i].Delegates.AddUnique(DelegateName);
			else
				Swui->BindingSources[i].Delegates.Remove(DelegateName);
			this->ScheduleDebouncedRefresh();
		})
	];
}
}

// ---- Add Source button (native UE look) ----
Cat.AddCustomRow(LOCTEXT("AddSourceRow", "Add Source"))
.WholeRowContent()
.HAlign(HAlign_Left)
[
	PropertyCustomizationHelpers::MakeAddButton(
		FSimpleDelegate::CreateLambda([ArrayHandle, this]()
		{
			if (ArrayHandle.IsValid()) ArrayHandle->AddItem();
			if (CachedDetailBuilder) CachedDetailBuilder->ForceRefreshDetails();
		}),
		LOCTEXT("AddSourceTip", "Add a new source class"))
];
}

#undef LOCTEXT_NAMESPACE
