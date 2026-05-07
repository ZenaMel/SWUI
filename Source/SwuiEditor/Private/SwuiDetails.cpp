#include "SwuiDetails.h"
#include "Swui.h"
#include "SwuiTSGenerator.h"

#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
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
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	if (Objects.Num() == 0) return;

	USwui* Bridge = Cast<USwui>(Objects[0].Get());
	if (!Bridge) return;

	SwuiPtr = Bridge;

	// Hide the raw array — the checklist below replaces it
	DetailBuilder.HideProperty(GET_MEMBER_NAME_CHECKED(USwui, ExposedProperties));

	// Force panel rebuild when the source class changes
	TSharedRef<IPropertyHandle> SourceClassHandle =
		DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(USwui, CodegenSourceClass));

	SourceClassHandle->SetOnPropertyValueChanged(FSimpleDelegate::CreateLambda([&DetailBuilder]()
	{
		DetailBuilder.ForceRefreshDetails();
	}));

	IDetailCategoryBuilder& Cat = DetailBuilder.EditCategory(
		"SimpleWebUI|Bindings",
		LOCTEXT("BindingsCat", "Web UI Bindings"),
		ECategoryPriority::Important);

	// Show the source class picker in the category
	Cat.AddProperty(SourceClassHandle);

	UClass* SourceClass = Bridge->CodegenSourceClass;
	if (!SourceClass) return;

	// Section header
	Cat.AddCustomRow(LOCTEXT("StateHeader", "State Properties"))
	.WholeRowContent()
	[
		SNew(STextBlock)
		.Text(LOCTEXT("StatePropertiesLabel", "State Properties"))
		.Font(IDetailLayoutBuilder::GetDetailFontBold())
	];

	// One checkbox row per BlueprintVisible property on the source class
	for (TFieldIterator<FProperty> It(SourceClass); It; ++It)
	{
		FProperty* Prop = *It;

		// Only show blueprint-visible properties (the "game designer" space)
		if (!Prop->HasAnyPropertyFlags(CPF_BlueprintVisible)) continue;

		// Only show types we can actually sync to JS
		if (SwuiGetTSType(Prop).IsEmpty()) continue;

		const FName PropName  = Prop->GetFName();
		const FString TSType  = SwuiGetTSType(Prop);

		Cat.AddCustomRow(FText::FromName(PropName))
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
				.Text(FText::FromString(TSType))
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		]
		.ValueContent()
		[
			SNew(SCheckBox)
			.IsChecked(TAttribute<ECheckBoxState>::CreateLambda([this, PropName]()
			{
				return (SwuiPtr.IsValid() && SwuiPtr->ExposedProperties.Contains(PropName))
					? ECheckBoxState::Checked
					: ECheckBoxState::Unchecked;
			}))
			.OnCheckStateChanged_Lambda([this, PropName](ECheckBoxState NewState)
			{
				if (!SwuiPtr.IsValid()) return;
				SwuiPtr->Modify();
				if (NewState == ECheckBoxState::Checked)
					SwuiPtr->ExposedProperties.AddUnique(PropName);
				else
					SwuiPtr->ExposedProperties.Remove(PropName);
			})
		];
	}

	// Generate button at the bottom of the category
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
