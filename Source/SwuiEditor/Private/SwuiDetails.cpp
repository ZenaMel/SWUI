#include "SwuiDetails.h"
#include "Swui.h"
#include "SwuiBindingSource.h"
#include "SwuiTSGenerator.h"

#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "PropertyCustomizationHelpers.h"
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

// Ensure slot 0 is the owner actor's class.
// Must call Modify() before any UPROPERTY mutation so the editor tracks the change.
{
	AActor* OwnerActor = Swui->GetOwner();
	if (!OwnerActor) OwnerActor = Swui->GetTypedOuter<AActor>();
	if (OwnerActor)
	{
		UClass* OwnerClass = OwnerActor->GetClass();
		const bool bSlot0Wrong = Swui->BindingSources.IsEmpty() ||
			Swui->BindingSources[0].SourceClass != OwnerClass;
		if (bSlot0Wrong)
		{
			Swui->Modify();
			Swui->EnsureOwnerBindingSource();
		}
	}
}

// Hide the raw array — replaced by our custom UI below
DetailBuilder.HideProperty(GET_MEMBER_NAME_CHECKED(USwui, BindingSources));

IDetailCategoryBuilder& Cat = DetailBuilder.EditCategory(
"SimpleWebUI|Bindings",
LOCTEXT("BindingsCat", "Web UI Bindings"),
ECategoryPriority::Important);

// ---- One section per binding source ----
for (int32 i = 0; i < Swui->BindingSources.Num(); ++i)
{
UClass* SourceClass = Swui->BindingSources[i].SourceClass;

// Section header: class picker + Remove button
Cat.AddCustomRow(FText::Format(LOCTEXT("SourceRow", "Source {0}"), { FText::AsNumber(i + 1) }))
.NameContent()
[
SNew(STextBlock)
.Text(FText::Format(LOCTEXT("SourceLabel", "Source {0}"), { FText::AsNumber(i + 1) }))
.Font(IDetailLayoutBuilder::GetDetailFontBold())
]
.ValueContent()
.MinDesiredWidth(250.f)
[
SNew(SHorizontalBox)
+ SHorizontalBox::Slot()
.FillWidth(1.f)
[
SNew(SClassPropertyEntryBox)
.MetaClass(UObject::StaticClass())
.AllowNone(true)
.AllowAbstract(true)
.ShowDisplayNames(true)
.IsEnabled(i != 0)
.SelectedClass_Lambda([Swui, i]() -> const UClass*
{
return Swui->BindingSources.IsValidIndex(i)
? (const UClass*)Swui->BindingSources[i].SourceClass
: nullptr;
})
.OnSetClass_Lambda([Swui, i, this](const UClass* NewClass)
{
if (!SwuiPtr.IsValid() || !Swui->BindingSources.IsValidIndex(i)) return;
Swui->Modify();
Swui->BindingSources[i].SourceClass = const_cast<UClass*>(NewClass);
Swui->BindingSources[i].Properties.Empty(); // stale selections cleared
if (CachedDetailBuilder) CachedDetailBuilder->ForceRefreshDetails();
})
]
+ SHorizontalBox::Slot()
.AutoWidth()
.Padding(4.f, 0.f, 0.f, 0.f)
[
SNew(SButton)
.Text(LOCTEXT("RemoveBtn", "Remove"))
.IsEnabled(i != 0)
.OnClicked_Lambda([Swui, i, this]()
{
if (!SwuiPtr.IsValid()) return FReply::Handled();
Swui->Modify();
Swui->BindingSources.RemoveAt(i);
if (CachedDetailBuilder) CachedDetailBuilder->ForceRefreshDetails();
return FReply::Handled();
})
]
];

if (!SourceClass) continue;

// "State Properties" sub-header
Cat.AddCustomRow(LOCTEXT("StatePropsHeader", "State Properties"))
.WholeRowContent()
	[
		SNew(SBox)
		.Padding(FMargin(20.f, 2.f, 0.f, 2.f))
		[
			SNew(STextBlock)
			.Text(LOCTEXT("StatePropertiesLabel", "State Properties"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
	];

for (TFieldIterator<FProperty> It(SourceClass); It; ++It)
{
FProperty* Prop = *It;
if (!Prop->HasAnyPropertyFlags(CPF_BlueprintVisible)) continue;
if (SwuiGetTSType(Prop).IsEmpty()) continue;

const FName PropName  = Prop->GetFName();
const FString TSType  = SwuiGetTSType(Prop);

Cat.AddCustomRow(FText::FromName(PropName))
.NameContent()
[
	SNew(SBox)
	.Padding(FMargin(28.f, 0.f, 0.f, 0.f))
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
]
.ValueContent()
[
	SNew(SCheckBox)
	.IsChecked(TAttribute<ECheckBoxState>::CreateLambda([Swui, i, PropName]()
	{
		if (!Swui->BindingSources.IsValidIndex(i)) return ECheckBoxState::Unchecked;
		return Swui->BindingSources[i].Properties.Contains(PropName)
			? ECheckBoxState::Checked
			: ECheckBoxState::Unchecked;
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
}

// ---- Add Source button ----
Cat.AddCustomRow(LOCTEXT("AddSourceRow", "Add Source"))
.WholeRowContent()
[
SNew(SButton)
.Text(LOCTEXT("AddSourceBtn", "+ Add Source Class"))
.OnClicked_Lambda([Swui, this]()
{
if (!SwuiPtr.IsValid()) return FReply::Handled();
Swui->Modify();
Swui->BindingSources.Add(FSwuiBindingSource());
if (CachedDetailBuilder) CachedDetailBuilder->ForceRefreshDetails();
return FReply::Handled();
})
];

// ---- Generate button ----
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
if (SwuiPtr.IsValid())
FSwuiTSGenerator::Generate(SwuiPtr.Get());
return FReply::Handled();
})
];
}

#undef LOCTEXT_NAMESPACE
