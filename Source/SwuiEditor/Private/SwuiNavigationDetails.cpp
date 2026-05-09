#include "SwuiNavigationDetails.h"
#include "SwuiNavigation.h"
#include "SwuiTSGenerator.h"
#include "Swui.h"

#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "PropertyCustomizationHelpers.h"
#include "GameplayTagsManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SBoxPanel.h"

#define LOCTEXT_NAMESPACE "SwuiNavigationDetails"

TSharedRef<IDetailCustomization> FSwuiNavigationDetails::MakeInstance()
{
	return MakeShareable(new FSwuiNavigationDetails);
}

void FSwuiNavigationDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	CachedDetailBuilder = &DetailBuilder;

	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	if (Objects.Num() == 0) return;

	USwuiNavigation* Nav = Cast<USwuiNavigation>(Objects[0].Get());
	if (!Nav) return;
	NavPtr = Nav;

	// Hide the raw NavigationEvents array — we draw our own rows.
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

	// ---- Existing navigation events ----
	for (int32 i = 0; i < Nav->NavigationEvents.Num(); ++i)
	{
		const FSwuiNavigationEvent& Evt = Nav->NavigationEvents[i];
		const FString TagStr = Evt.Event.IsValid() ? Evt.Event.GetTagName().ToString() : TEXT("(none)");
		const FString JsName = Evt.GetEffectiveJsEventName();

		Cat.AddCustomRow(FText::FromString(TagStr))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(FText::FromString(TagStr))
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ToolTipText(FText::Format(
				LOCTEXT("EventRowTip", "JS event: {0}"), FText::FromString(JsName)))
		]
		.ValueContent()
		.MinDesiredWidth(100.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(JsName))
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				PropertyCustomizationHelpers::MakeDeleteButton(
					FSimpleDelegate::CreateLambda([this, i]()
					{
						if (NavPtr.IsValid() && NavPtr->NavigationEvents.IsValidIndex(i))
						{
							NavPtr->Modify();
							NavPtr->NavigationEvents.RemoveAt(i);
							if (CachedDetailBuilder)
								CachedDetailBuilder->ForceRefreshDetails();
						}
					}),
					LOCTEXT("RemoveEventTip", "Remove this navigation event"))
			]
		];
	}

	// ---- Add new tag row ----
	TSharedRef<SEditableTextBox> NewTagBox = SNew(SEditableTextBox)
		.Font(IDetailLayoutBuilder::GetDetailFont())
		.HintText(LOCTEXT("NewTagHint", "swui.custom.myEvent"));

	Cat.AddCustomRow(LOCTEXT("AddTagRow", "Add Tag"))
	.WholeRowContent()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 2.f)
		[
			NewTagBox
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 2.f, 0.f, 2.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("AddTagBtn", "Add"))
			.ToolTipText(LOCTEXT("AddTagBtnTip",
				"Create a Gameplay Tag and add a Navigation Event for it."))
			.OnClicked_Lambda([this, NewTagBox]()
			{
				if (!NavPtr.IsValid()) return FReply::Handled();

				const FString TagString = NewTagBox->GetText().ToString().TrimStartAndEnd();
				if (TagString.IsEmpty()) return FReply::Handled();

				// Request the tag (creates if not yet registered).
				FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(
					FName(*TagString), /*bErrorIfNotFound=*/false);

				if (!Tag.IsValid())
				{
					// Manually add via native registration then re-request.
					UGameplayTagsManager::Get().AddNativeGameplayTag(
						FName(*TagString), TEXT("Custom SWUI navigation event"));
					Tag = UGameplayTagsManager::Get().RequestGameplayTag(
						FName(*TagString), /*bErrorIfNotFound=*/false);
				}

				if (Tag.IsValid())
				{
					NavPtr->Modify();
					FSwuiNavigationEvent NewEvt;
					NewEvt.Event = Tag;
					NavPtr->NavigationEvents.Add(NewEvt);
				}

				if (CachedDetailBuilder) CachedDetailBuilder->ForceRefreshDetails();
				return FReply::Handled();
			})
		]
	];
}

#undef LOCTEXT_NAMESPACE
