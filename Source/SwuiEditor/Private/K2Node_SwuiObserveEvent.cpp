#include "K2Node_SwuiObserveEvent.h"
#include "SwuiSubsystem.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "KismetCompiler.h"
#include "SGraphNode.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SComboButton.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "UObject/PropertyIterator.h"

#define LOCTEXT_NAMESPACE "K2Node_SwuiObserveEvent"

static const FName SwuiEventPin_Source      (TEXT("Source"));
static const FName SwuiEventPin_DelegateName(TEXT("DelegateName"));

// ----------------------------------------------------------------
// Slate widget
// ----------------------------------------------------------------

class SGraphNode_SwuiObserveEvent : public SGraphNode
{
public:
	SLATE_BEGIN_ARGS(SGraphNode_SwuiObserveEvent) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&, UK2Node_SwuiObserveEvent* InNode)
	{
		GraphNode = InNode;
		SetCursor(EMouseCursor::CardinalCross);
		UpdateGraphNode();
	}

	virtual void CreateBelowPinControls(TSharedPtr<SVerticalBox> MainBox) override
	{
		UK2Node_SwuiObserveEvent* Node = CastChecked<UK2Node_SwuiObserveEvent>(GraphNode);

		MainBox->AddSlot()
		.AutoHeight()
		.Padding(FMargin(8.f, 2.f, 8.f, 6.f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 0.f, 6.f, 0.f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("EventLabel", "Event"))
				.Font(FAppStyle::GetFontStyle(TEXT("SmallFont")))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			[
				SNew(SComboButton)
				.ContentPadding(FMargin(4.f, 2.f))
				.OnGetMenuContent_Lambda([Node]()
				{
					FMenuBuilder MenuBuilder(true, nullptr);
					TArray<FName> Events = Node->GetAvailableEvents();

					if (Events.Num() == 0)
					{
						MenuBuilder.AddMenuEntry(
							LOCTEXT("NoSource", "(connect Source pin first)"),
							FText::GetEmpty(), FSlateIcon(), FUIAction());
					}
					for (const FName& EventName : Events)
					{
						TWeakObjectPtr<UK2Node_SwuiObserveEvent> WeakNode(Node);
						MenuBuilder.AddMenuEntry(
							FText::FromName(EventName),
							FText::GetEmpty(),
							FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([WeakNode, EventName]()
							{
								if (!WeakNode.IsValid()) return;
								FScopedTransaction Tx(LOCTEXT("SelectEventTx", "Select SWUI Event"));
								WeakNode->Modify();
								WeakNode->SelectedDelegate = EventName;
								WeakNode->ReconstructNode();
							}))
						);
					}
					return MenuBuilder.MakeWidget();
				})
				.ButtonContent()
				[
					SNew(STextBlock)
					.Text_Lambda([Node]()
					{
						return Node->SelectedDelegate.IsNone()
							? LOCTEXT("SelectPrompt", "— select event —")
							: FText::FromName(Node->SelectedDelegate);
					})
					.Font(FAppStyle::GetFontStyle(TEXT("SmallFont")))
				]
			]
		];
	}
};

// ----------------------------------------------------------------
// UK2Node_SwuiObserveEvent
// ----------------------------------------------------------------

void UK2Node_SwuiObserveEvent::AllocateDefaultPins()
{
	CreatePin(EGPD_Input,  UEdGraphSchema_K2::PC_Exec,   UEdGraphSchema_K2::PN_Execute);
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec,   UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* SrcPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Object, UObject::StaticClass(), SwuiEventPin_Source);
	SrcPin->PinFriendlyName = LOCTEXT("SourcePinLabel", "Source");
	Super::AllocateDefaultPins();
}

UEdGraphPin* UK2Node_SwuiObserveEvent::GetSourcePin() const
{
	return FindPinChecked(SwuiEventPin_Source, EGPD_Input);
}

UClass* UK2Node_SwuiObserveEvent::GetSourceClass() const
{
	UEdGraphPin* SrcPin = GetSourcePin();
	if (!SrcPin || SrcPin->LinkedTo.Num() == 0) return nullptr;
	return Cast<UClass>(SrcPin->LinkedTo[0]->PinType.PinSubCategoryObject.Get());
}

TArray<FName> UK2Node_SwuiObserveEvent::GetAvailableEvents() const
{
	TArray<FName> Result;
	UClass* Class = GetSourceClass();
	if (!Class) return Result;

	for (TFieldIterator<FMulticastDelegateProperty> It(Class); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_BlueprintAssignable))
			Result.Add(It->GetFName());
	}
	return Result;
}

FText UK2Node_SwuiObserveEvent::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	if (TitleType == ENodeTitleType::MenuTitle)
		return LOCTEXT("MenuTitle", "SWUI Observe Event");
	return SelectedDelegate.IsNone()
		? LOCTEXT("TitleEmpty", "SWUI Observe Event")
		: FText::Format(LOCTEXT("TitleFmt", "SWUI Observe Event\n{0}"), FText::FromName(SelectedDelegate));
}

FText UK2Node_SwuiObserveEvent::GetTooltipText() const
{
	return LOCTEXT("Tooltip",
		"Bind to a BlueprintAssignable delegate on any UObject.\n"
		"When the delegate broadcasts, a JS CustomEvent fires on the web page.\n"
		"Call from PlayerController's OnPossess. Auto-cleans up when Source is destroyed.");
}

FText UK2Node_SwuiObserveEvent::GetMenuCategory() const
{
	return LOCTEXT("Category", "SimpleWebUI");
}

void UK2Node_SwuiObserveEvent::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	UClass* Key = GetClass();
	if (ActionRegistrar.IsOpenForRegistration(Key))
	{
		UBlueprintNodeSpawner* Spawner = UBlueprintNodeSpawner::Create(Key);
		check(Spawner);
		ActionRegistrar.AddBlueprintAction(Key, Spawner);
	}
}

void UK2Node_SwuiObserveEvent::PinConnectionListChanged(UEdGraphPin* Pin)
{
	Super::PinConnectionListChanged(Pin);
	if (Pin == GetSourcePin())
	{
		if (!GetAvailableEvents().Contains(SelectedDelegate))
			SelectedDelegate = NAME_None;
		ReconstructNode();
	}
}

void UK2Node_SwuiObserveEvent::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	UEdGraphPin* ExecPin   = GetExecPin();
	UEdGraphPin* ThenPin   = FindPin(UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* SourcePin = GetSourcePin();

	if (SelectedDelegate.IsNone())
	{
		CompilerContext.MessageLog.Error(TEXT("@@ — No event selected. Open the node and pick a delegate from the dropdown."), this);
		BreakAllNodeLinks();
		return;
	}

	UFunction* Func = USwuiSubsystem::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(USwuiSubsystem, K2_ObserveEvent));
	check(Func);

	UK2Node_CallFunction* CallNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	CallNode->SetFromFunction(Func);
	CallNode->AllocateDefaultPins();

	CompilerContext.MovePinLinksToIntermediate(*ExecPin,   *CallNode->GetExecPin());
	CompilerContext.MovePinLinksToIntermediate(*ThenPin,   *CallNode->GetThenPin());
	CompilerContext.MovePinLinksToIntermediate(*SourcePin, *CallNode->FindPinChecked(SwuiEventPin_Source));
	CallNode->FindPinChecked(SwuiEventPin_DelegateName)->DefaultValue = SelectedDelegate.ToString();

	BreakAllNodeLinks();
}

TSharedPtr<SGraphNode> UK2Node_SwuiObserveEvent::CreateVisualWidget()
{
	return SNew(SGraphNode_SwuiObserveEvent, this);
}

#undef LOCTEXT_NAMESPACE
