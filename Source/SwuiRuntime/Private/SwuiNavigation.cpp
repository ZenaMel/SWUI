#include "SwuiNavigation.h"
#include "Swui.h"
#include "SwuiSubsystem.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"

DEFINE_LOG_CATEGORY_STATIC(LogSwuiNavigation, Log, All);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

USwuiNavigation::USwuiNavigation()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void USwuiNavigation::BeginPlay()
{
	Super::BeginPlay();

	if (bFocusOnBeginPlay && !FocusedSwui)
	{
		AActor* Owner = GetOwner();
		if (Owner)
		{
			FocusedSwui = Owner->FindComponentByClass<USwui>();
		}
	}

	if (FocusedSwui)
	{
		ApplyInputMode();
	}
}

// ---------------------------------------------------------------------------
// Focus
// ---------------------------------------------------------------------------

void USwuiNavigation::SetFocusedSwui(USwui* InSwui)
{
	FocusedSwui = InSwui;
	ApplyInputMode();
}

void USwuiNavigation::ClearFocusedSwui()
{
	FocusedSwui = nullptr;

	// Restore game-only input when clearing focus.
	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (PC)
	{
		PC->bShowMouseCursor = false;
		FInputModeGameOnly GameMode;
		PC->SetInputMode(GameMode);
	}
}

// ---------------------------------------------------------------------------
// Input Mode
// ---------------------------------------------------------------------------

void USwuiNavigation::SetInputMode(ESwuiInputMode Mode)
{
	InputMode = Mode;
	ApplyInputMode();
}

void USwuiNavigation::ApplyInputMode()
{
	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (!PC) return;

	switch (InputMode)
	{
	case ESwuiInputMode::HudOnly:
		PC->bShowMouseCursor = false;
		{
			FInputModeGameOnly GameMode;
			PC->SetInputMode(GameMode);
		}
		break;

	case ESwuiInputMode::UiOnly:
		PC->bShowMouseCursor = bShowCursorWhenFocused && FocusedSwui != nullptr;
		{
			FInputModeUIOnly UIMode;
			PC->SetInputMode(UIMode);
		}
		break;

	case ESwuiInputMode::GameAndUi:
		PC->bShowMouseCursor = bShowCursorWhenFocused && FocusedSwui != nullptr;
		{
			FInputModeGameAndUI GameUI;
			PC->SetInputMode(GameUI);
		}
		break;
	}
}

void USwuiNavigation::ShowCursor(bool bShow)
{
	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (PC)
	{
		PC->bShowMouseCursor = bShow;
	}
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

FString USwuiNavigation::EscapeJsonString(const FString& Raw)
{
	FString Out = Raw;
	Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Out.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Out.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	Out.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Out.ReplaceInline(TEXT("\t"), TEXT("\\t"));
	return Out;
}

const TCHAR* USwuiNavigation::DirectionToString(ESwuiNavDirection Direction)
{
	switch (Direction)
	{
	case ESwuiNavDirection::Up:       return TEXT("Up");
	case ESwuiNavDirection::Down:     return TEXT("Down");
	case ESwuiNavDirection::Left:     return TEXT("Left");
	case ESwuiNavDirection::Right:    return TEXT("Right");
	case ESwuiNavDirection::Next:     return TEXT("Next");
	case ESwuiNavDirection::Previous: return TEXT("Previous");
	}
	return TEXT("Unknown");
}

const TCHAR* USwuiNavigation::PointerButtonToString(ESwuiPointerButton Button)
{
	switch (Button)
	{
	case ESwuiPointerButton::Left:   return TEXT("Left");
	case ESwuiPointerButton::Right:  return TEXT("Right");
	case ESwuiPointerButton::Middle: return TEXT("Middle");
	}
	return TEXT("Unknown");
}

const FSwuiNavigationAction* USwuiNavigation::FindAction(FName ActionName) const
{
	for (const FSwuiNavigationAction& Action : NavigationActions)
	{
		if (Action.ActionName == ActionName) return &Action;
	}
	return nullptr;
}

void USwuiNavigation::ForwardToJs(const FString& JsEventName, const FString& DetailJson)
{
	if (!FocusedSwui) return;

	UWorld* World = GetWorld();
	if (!World) return;
	UGameInstance* GI = World->GetGameInstance();
	if (!GI) return;
	USwuiSubsystem* Sub = GI->GetSubsystem<USwuiSubsystem>();
	if (!Sub) return;

	// window.dispatchEvent(new CustomEvent("eventName", { detail: {...} }));
	const FString Script = FString::Printf(
		TEXT("window.dispatchEvent(new CustomEvent(\"%s\",{detail:%s}));"),
		*EscapeJsonString(JsEventName),
		*DetailJson
	);

	Sub->ExecuteJavaScript(Script);
}

void USwuiNavigation::DispatchBuiltIn(const FString& JsEventName, const FString& DetailJson,
	TFunction<bool()> BlueprintHandler, const FString* GenericDetailJson)
{
	if (bLogNavigationActions)
	{
		UE_LOG(LogSwuiNavigation, Log, TEXT("[SwuiNav] %s  detail=%s  dispatch=%s"),
			*JsEventName, *DetailJson,
			DispatchOrder == ESwuiNavigationDispatchOrder::BlueprintFirst ? TEXT("BP->JS") : TEXT("JS->BP"));
	}

	auto DoJS = [&]()
	{
		if (GenericDetailJson)
		{
			ForwardToJs(TEXT("swui:navigation"), *GenericDetailJson);
		}
		ForwardToJs(JsEventName, DetailJson);
	};

	if (DispatchOrder == ESwuiNavigationDispatchOrder::BlueprintFirst)
	{
		const bool bConsumed = BlueprintHandler();
		if (!bConsumed)
		{
			DoJS();
		}
	}
	else // JavaScriptFirst
	{
		DoJS();
		BlueprintHandler();
	}
}

// ---------------------------------------------------------------------------
// Built-in Navigation Actions
// ---------------------------------------------------------------------------

void USwuiNavigation::Navigate(ESwuiNavDirection Direction)
{
	const TCHAR* DirStr = DirectionToString(Direction);
	const FString SpecializedDetail = FString::Printf(TEXT("{\"direction\":\"%s\",\"source\":\"USwuiNavigation\"}"), DirStr);
	const FString GenericDetail = FString::Printf(
		TEXT("{\"action\":\"Navigate\",\"payload\":{\"direction\":\"%s\"},\"source\":\"USwuiNavigation\"}"), DirStr);

	DispatchBuiltIn(TEXT("swui:navigate"), SpecializedDetail, [this, Direction]()
	{
		OnNavigate.Broadcast(Direction);
		return HandleNavigate(Direction);
	}, &GenericDetail);
}

void USwuiNavigation::Confirm()
{
	const FString Detail = TEXT("{\"source\":\"USwuiNavigation\"}");
	const FString GenericDetail = TEXT("{\"action\":\"Confirm\",\"source\":\"USwuiNavigation\"}");

	DispatchBuiltIn(TEXT("swui:confirm"), Detail, [this]()
	{
		OnConfirm.Broadcast();
		return HandleConfirm();
	}, &GenericDetail);
}

void USwuiNavigation::Cancel()
{
	const FString Detail = TEXT("{\"source\":\"USwuiNavigation\"}");
	const FString GenericDetail = TEXT("{\"action\":\"Cancel\",\"source\":\"USwuiNavigation\"}");

	DispatchBuiltIn(TEXT("swui:cancel"), Detail, [this]()
	{
		OnCancel.Broadcast();
		return HandleCancel();
	}, &GenericDetail);

	// bCloseOnEscape: after dispatch, clear focus and restore input.
	if (bCloseOnEscape && FocusedSwui)
	{
		ClearFocusedSwui();
	}
}

void USwuiNavigation::NextTab()
{
	const FString Detail = TEXT("{\"source\":\"USwuiNavigation\"}");
	const FString GenericDetail = TEXT("{\"action\":\"NextTab\",\"source\":\"USwuiNavigation\"}");

	DispatchBuiltIn(TEXT("swui:next-tab"), Detail, [this]()
	{
		OnNextTab.Broadcast();
		return HandleNextTab();
	}, &GenericDetail);
}

void USwuiNavigation::PreviousTab()
{
	const FString Detail = TEXT("{\"source\":\"USwuiNavigation\"}");
	const FString GenericDetail = TEXT("{\"action\":\"PreviousTab\",\"source\":\"USwuiNavigation\"}");

	DispatchBuiltIn(TEXT("swui:previous-tab"), Detail, [this]()
	{
		OnPreviousTab.Broadcast();
		return HandlePreviousTab();
	}, &GenericDetail);
}

// ---------------------------------------------------------------------------
// Custom Actions
// ---------------------------------------------------------------------------

void USwuiNavigation::TriggerAction(FName ActionName)
{
	TriggerActionWithPayload(ActionName, TEXT("{}"));
}

void USwuiNavigation::TriggerActionWithPayload(FName ActionName, const FString& JsonPayload)
{
	const FSwuiNavigationAction* ActionDef = FindAction(ActionName);

	const bool bForwardJs = ActionDef ? ActionDef->bForwardToJavaScript : true;
	const bool bTriggerBP = ActionDef ? ActionDef->bTriggerBlueprintEvents : true;
	const FString JsEvent = ActionDef ? ActionDef->JsEventName : TEXT("swui:navigation");

	if (bLogNavigationActions)
	{
		UE_LOG(LogSwuiNavigation, Log, TEXT("[SwuiNav] TriggerAction  name=%s  payload=%s  jsEvent=%s  dispatch=%s"),
			*ActionName.ToString(), *JsonPayload, *JsEvent,
			DispatchOrder == ESwuiNavigationDispatchOrder::BlueprintFirst ? TEXT("BP->JS") : TEXT("JS->BP"));
	}

	// Generic navigation event always sent.
	const FString GenericDetail = FString::Printf(
		TEXT("{\"action\":\"%s\",\"payload\":%s,\"source\":\"USwuiNavigation\"}"),
		*EscapeJsonString(ActionName.ToString()), *JsonPayload);

	auto DoBP = [&]() -> bool
	{
		if (bTriggerBP)
		{
			OnNavigationAction.Broadcast(ActionName, FString());
			OnNavigationActionWithPayload.Broadcast(ActionName, JsonPayload);
			return HandleNavigationAction(ActionName, JsonPayload);
		}
		return false;
	};

	auto DoJS = [&]()
	{
		if (bForwardJs)
		{
			ForwardToJs(TEXT("swui:navigation"), GenericDetail);

			// Also send to the action-specific JS event if configured differently.
			if (JsEvent != TEXT("swui:navigation"))
			{
				ForwardToJs(JsEvent, GenericDetail);
			}
		}
	};

	if (DispatchOrder == ESwuiNavigationDispatchOrder::BlueprintFirst)
	{
		const bool bConsumed = DoBP();
		if (!bConsumed) DoJS();
	}
	else
	{
		DoJS();
		DoBP();
	}
}

// ---------------------------------------------------------------------------
// Pointer Input
// ---------------------------------------------------------------------------

void USwuiNavigation::PointerMove(FVector2D ScreenPosition)
{
	OnPointerMove.Broadcast(ScreenPosition);

	const FString Detail = FString::Printf(
		TEXT("{\"x\":%.1f,\"y\":%.1f,\"source\":\"USwuiNavigation\"}"),
		ScreenPosition.X, ScreenPosition.Y);
	ForwardToJs(TEXT("swui:pointer-move"), Detail);
}

void USwuiNavigation::PointerPress(ESwuiPointerButton Button)
{
	OnPointerPress.Broadcast(Button);

	const FString Detail = FString::Printf(
		TEXT("{\"button\":\"%s\",\"source\":\"USwuiNavigation\"}"),
		PointerButtonToString(Button));
	ForwardToJs(TEXT("swui:pointer-press"), Detail);
}

void USwuiNavigation::PointerRelease(ESwuiPointerButton Button)
{
	OnPointerRelease.Broadcast(Button);

	const FString Detail = FString::Printf(
		TEXT("{\"button\":\"%s\",\"source\":\"USwuiNavigation\"}"),
		PointerButtonToString(Button));
	ForwardToJs(TEXT("swui:pointer-release"), Detail);
}

void USwuiNavigation::PointerWheel(float Delta)
{
	OnPointerWheel.Broadcast(Delta);

	const FString Detail = FString::Printf(
		TEXT("{\"delta\":%.4f,\"source\":\"USwuiNavigation\"}"), Delta);
	ForwardToJs(TEXT("swui:pointer-wheel"), Detail);
}

// ---------------------------------------------------------------------------
// Keyboard Input
// ---------------------------------------------------------------------------

void USwuiNavigation::KeyDown(FKey Key)
{
	OnKeyDown.Broadcast(Key);

	const FString Detail = FString::Printf(
		TEXT("{\"key\":\"%s\",\"source\":\"USwuiNavigation\"}"),
		*EscapeJsonString(Key.ToString()));
	ForwardToJs(TEXT("swui:key-down"), Detail);
}

void USwuiNavigation::KeyUp(FKey Key)
{
	OnKeyUp.Broadcast(Key);

	const FString Detail = FString::Printf(
		TEXT("{\"key\":\"%s\",\"source\":\"USwuiNavigation\"}"),
		*EscapeJsonString(Key.ToString()));
	ForwardToJs(TEXT("swui:key-up"), Detail);
}

void USwuiNavigation::TextInput(const FString& Text)
{
	OnTextInput.Broadcast(Text);

	const FString Detail = FString::Printf(
		TEXT("{\"text\":\"%s\",\"source\":\"USwuiNavigation\"}"),
		*EscapeJsonString(Text));
	ForwardToJs(TEXT("swui:text-input"), Detail);
}

// ---------------------------------------------------------------------------
// BlueprintNativeEvent defaults — return false (unconsumed) by default.
// Blueprint subclasses can override to consume actions.
// ---------------------------------------------------------------------------

bool USwuiNavigation::HandleNavigationAction_Implementation(FName ActionName, const FString& JsonPayload) { return false; }
bool USwuiNavigation::HandleNavigate_Implementation(ESwuiNavDirection Direction) { return false; }
bool USwuiNavigation::HandleConfirm_Implementation() { return false; }
bool USwuiNavigation::HandleCancel_Implementation() { return false; }
bool USwuiNavigation::HandleNextTab_Implementation() { return false; }
bool USwuiNavigation::HandlePreviousTab_Implementation() { return false; }
