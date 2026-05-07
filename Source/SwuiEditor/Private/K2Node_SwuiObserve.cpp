#include "K2Node_SwuiObserve.h"
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

#define LOCTEXT_NAMESPACE "K2Node_SwuiObserve"

static const FName SwuiPin_Source      (TEXT("Source"));
static const FName SwuiPin_PropertyName(TEXT("PropertyName"));

// ----------------------------------------------------------------
// Slate widget — injects a combo dropdown into the graph node
// ----------------------------------------------------------------

class SGraphNode_SwuiObserve : public SGraphNode
{
public:
	SLATE_BEGIN_ARGS(SGraphNode_SwuiObserve) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&, UK2Node_SwuiObserve* InNode)
	{
		GraphNode = InNode;
		SetCursor(EMouseCursor::CardinalCross);
		UpdateGraphNode();
	}

	virtual void CreateBelowPinControls(TSharedPtr<SVerticalBox> MainBox) override
	{
		UK2Node_SwuiObserve* Node = CastChecked<UK2Node_SwuiObserve>(GraphNode);

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
				.Text(LOCTEXT("PropertyLabel", "Property"))
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
					TArray<FName> Props = Node->GetAvailableProperties();

					if (Props.Num() == 0)
					{
						MenuBuilder.AddMenuEntry(
							LOCTEXT("NoSource", "(connect Source pin first)"),
							FText::GetEmpty(), FSlateIcon(), FUIAction());
					}
					for (const FName& PropName : Props)
					{
						TWeakObjectPtr<UK2Node_SwuiObserve> WeakNode(Node);
						MenuBuilder.AddMenuEntry(
							FText::FromName(PropName),
							FText::GetEmpty(),
							FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([WeakNode, PropName]()
							{
								if (!WeakNode.IsValid()) return;
								FScopedTransaction Tx(LOCTEXT("SelectPropTx", "Select SWUI Property"));
								WeakNode->Modify();
								WeakNode->SelectedProperty = PropName;
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
						return Node->SelectedProperty.IsNone()
							? LOCTEXT("SelectPrompt", "— select property —")
							: FText::FromName(Node->SelectedProperty);
					})
					.Font(FAppStyle::GetFontStyle(TEXT("SmallFont")))
				]
			]
		];
	}
};

// ----------------------------------------------------------------
// UK2Node_SwuiObserve
// ----------------------------------------------------------------

void UK2Node_SwuiObserve::AllocateDefaultPins()
{
	CreatePin(EGPD_Input,  UEdGraphSchema_K2::PC_Exec,   UEdGraphSchema_K2::PN_Execute);
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec,   UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* SrcPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Object, UObject::StaticClass(), SwuiPin_Source);
	SrcPin->PinFriendlyName = LOCTEXT("SourcePinLabel", "Source");
	Super::AllocateDefaultPins();
}

UEdGraphPin* UK2Node_SwuiObserve::GetSourcePin() const
{
	return FindPinChecked(SwuiPin_Source, EGPD_Input);
}

UClass* UK2Node_SwuiObserve::GetSourceClass() const
{
	UEdGraphPin* SrcPin = GetSourcePin();
	if (!SrcPin || SrcPin->LinkedTo.Num() == 0) return nullptr;
	return Cast<UClass>(SrcPin->LinkedTo[0]->PinType.PinSubCategoryObject.Get());
}

TArray<FName> UK2Node_SwuiObserve::GetAvailableProperties() const
{
	TArray<FName> Result;
	UClass* Class = GetSourceClass();
	if (!Class) return Result;

	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_BlueprintVisible)) continue;
		if (It->IsA<FFloatProperty>()  || It->IsA<FDoubleProperty>() ||
			It->IsA<FIntProperty>()    || It->IsA<FInt64Property>()  ||
			It->IsA<FByteProperty>()   || It->IsA<FBoolProperty>()   ||
			It->IsA<FStrProperty>()    || It->IsA<FNameProperty>()   ||
			It->IsA<FTextProperty>())
		{
			Result.Add(It->GetFName());
		}
	}
	return Result;
}

FText UK2Node_SwuiObserve::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	if (TitleType == ENodeTitleType::MenuTitle)
		return LOCTEXT("MenuTitle", "SWUI Observe");
	return SelectedProperty.IsNone()
		? LOCTEXT("TitleEmpty", "SWUI Observe")
		: FText::Format(LOCTEXT("TitleFmt", "SWUI Observe\n{0}"), FText::FromName(SelectedProperty));
}

FText UK2Node_SwuiObserve::GetTooltipText() const
{
	return LOCTEXT("Tooltip",
		"Begin syncing a property to the SWUI web surface.\n"
		"Connect GetPawn() or Self to Source, then pick a property from the dropdown.\n"
		"Call from PlayerController's OnPossess. Auto-cleans up when Source is destroyed.");
}

FText UK2Node_SwuiObserve::GetMenuCategory() const
{
	return LOCTEXT("Category", "SimpleWebUI");
}

void UK2Node_SwuiObserve::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	UClass* Key = GetClass();
	if (ActionRegistrar.IsOpenForRegistration(Key))
	{
		UBlueprintNodeSpawner* Spawner = UBlueprintNodeSpawner::Create(Key);
		check(Spawner);
		ActionRegistrar.AddBlueprintAction(Key, Spawner);
	}
}

void UK2Node_SwuiObserve::PinConnectionListChanged(UEdGraphPin* Pin)
{
	Super::PinConnectionListChanged(Pin);
	if (Pin == GetSourcePin())
	{
		if (!GetAvailableProperties().Contains(SelectedProperty))
			SelectedProperty = NAME_None;
		ReconstructNode();
	}
}

void UK2Node_SwuiObserve::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	UEdGraphPin* ExecPin   = GetExecPin();
	UEdGraphPin* ThenPin   = FindPin(UEdGraphSchema_K2::PN_Then);
	UEdGraphPin* SourcePin = GetSourcePin();

	if (SelectedProperty.IsNone())
	{
		CompilerContext.MessageLog.Error(TEXT("@@ — No property selected. Open the node and pick a property from the dropdown."), this);
		BreakAllNodeLinks();
		return;
	}

	UFunction* Func = USwuiSubsystem::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(USwuiSubsystem, K2_Observe));
	check(Func);

	UK2Node_CallFunction* CallNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	CallNode->SetFromFunction(Func);
	CallNode->AllocateDefaultPins();

	CompilerContext.MovePinLinksToIntermediate(*ExecPin,   *CallNode->GetExecPin());
	CompilerContext.MovePinLinksToIntermediate(*ThenPin,   *CallNode->GetThenPin());
	CompilerContext.MovePinLinksToIntermediate(*SourcePin, *CallNode->FindPinChecked(SwuiPin_Source));
	CallNode->FindPinChecked(SwuiPin_PropertyName)->DefaultValue = SelectedProperty.ToString();

	BreakAllNodeLinks();
}

TSharedPtr<SGraphNode> UK2Node_SwuiObserve::CreateVisualWidget()
{
	return SNew(SGraphNode_SwuiObserve, this);
}

#undef LOCTEXT_NAMESPACE
