#include "SwuiDetails.h"
#include "Swui.h"
#include "SwuiBindingSource.h"
#include "SwuiTSGenerator.h"
#include "SwuiNavigation.h"
#include "GameFramework/Actor.h"

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

#define LOCTEXT_NAMESPACE "SwuiDetails"

TSharedRef<IDetailCustomization> FSwuiDetails::MakeInstance()
{
return MakeShareable(new FSwuiDetails);
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
		// Also regenerate preview schema with whatever nav events currently exist.
		// If a SwuiNavigation component is present on the same actor it supplies
		// its events; otherwise we pass an empty list so the state/events still emit.
		TArray<FSwuiNavigationEvent> NavEvents;
		if (AActor* Owner = Cast<AActor>(SwuiPtr->GetOwner()))
		{
			if (USwuiNavigation* NavComp = Owner->FindComponentByClass<USwuiNavigation>())
				NavEvents = NavComp->NavigationEvents;
		}
		FSwuiTSGenerator::GeneratePreview(SwuiPtr.Get(), NavEvents);
	}

	FNotificationInfo Info(bOK
		? LOCTEXT("GenOK",  "JS Bindings generated successfully.")
		: LOCTEXT("GenFail", "JS Bindings generation failed — check the Output Log."));
	Info.bFireAndForget = true;
	Info.FadeInDuration  = 0.2f;
	Info.FadeOutDuration = 0.5f;
	Info.ExpireDuration  = 3.f;
	Info.Image = FAppStyle::GetBrush(bOK ? TEXT("NotificationList.SuccessImage") : TEXT("NotificationList.FailImage"));
	FSlateNotificationManager::Get().AddNotification(Info);
	return FReply::Handled();
})
];

// ---- One collapsible group per binding source ----
for (int32 i = 0; i < Swui->BindingSources.Num(); ++i)
{
UClass* SourceClass = Swui->BindingSources[i].SourceClass;

// Group label: include the class name when set so collapsed state is informative.
const FText SourceLabel = (i == 0)
	? (SourceClass
		? FText::Format(LOCTEXT("OwnerGroupNamed", "Owner Class ({0})"), FText::FromString(SourceClass->GetName()))
		: LOCTEXT("OwnerGroupEmpty", "Owner Class"))
	: (SourceClass
		? FText::Format(LOCTEXT("SourceGroupNamed", "Source {0} ({1})"), FText::AsNumber(i + 1), FText::FromString(SourceClass->GetName()))
		: FText::Format(LOCTEXT("SourceGroupEmpty", "Source {0}"), FText::AsNumber(i + 1)));

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

for (TFieldIterator<FProperty> It(SourceClass); It; ++It)
{
	FProperty* Prop = *It;
	if (!Prop->HasAnyPropertyFlags(CPF_BlueprintVisible)) continue;
	if (SwuiGetTSType(Prop).IsEmpty()) continue;

	const FName PropName = Prop->GetFName();
	const FString TSType = SwuiGetTSType(Prop);

	StateGroup.AddWidgetRow()
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
		.OnCheckStateChanged_Lambda([Swui, i, PropName](ECheckBoxState NewState)
		{
			if (!Swui->BindingSources.IsValidIndex(i)) return;
			Swui->Modify();
			if (NewState == ECheckBoxState::Checked)
				Swui->BindingSources[i].Properties.AddUnique(PropName);
			else
				Swui->BindingSources[i].Properties.Remove(PropName);
		})
	];
}

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
		.OnCheckStateChanged_Lambda([Swui, i, DelegateName](ECheckBoxState NewState)
		{
			if (!Swui->BindingSources.IsValidIndex(i)) return;
			Swui->Modify();
			if (NewState == ECheckBoxState::Checked)
				Swui->BindingSources[i].Delegates.AddUnique(DelegateName);
			else
				Swui->BindingSources[i].Delegates.Remove(DelegateName);
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
