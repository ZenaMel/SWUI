#include "SwuiNavigation.h"
#include "Swui.h"
#include "SwuiSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "GameplayTagsManager.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogSwuiNavigation, Log, All);

// ---------------------------------------------------------------------------
// Built-in Gameplay Tags singleton
// ---------------------------------------------------------------------------

FSwuiNavTags FSwuiNavTags::Instance;
bool FSwuiNavTags::bInitialized = false;
TSet<FName> FSwuiNavTags::BuiltInTagNames;

const FSwuiNavTags& FSwuiNavTags::Get()
{
	if (!bInitialized)
	{
		Instance.Initialize();
		bInitialized = true;
	}
	return Instance;
}

void FSwuiNavTags::Initialize()
{
	UGameplayTagsManager& Mgr = UGameplayTagsManager::Get();

	auto Add = [&](const FString& Name, const FString& Comment) -> FGameplayTag
	{
		return Mgr.AddNativeGameplayTag(FName(*Name), Comment);
	};

	Confirm        = Add(TEXT("swui.navigation.confirm"),       TEXT("Confirm / accept / select"));
	Cancel         = Add(TEXT("swui.navigation.cancel"),        TEXT("Cancel / back / escape"));
	Up             = Add(TEXT("swui.navigation.up"),            TEXT("Navigate up"));
	Down           = Add(TEXT("swui.navigation.down"),          TEXT("Navigate down"));
	Left           = Add(TEXT("swui.navigation.left"),          TEXT("Navigate left"));
	Right          = Add(TEXT("swui.navigation.right"),         TEXT("Navigate right"));
	NextTab        = Add(TEXT("swui.navigation.nextTab"),       TEXT("Next tab / bumper right"));
	PreviousTab    = Add(TEXT("swui.navigation.previousTab"),   TEXT("Previous tab / bumper left"));

	MenuOpen           = Add(TEXT("swui.menu.open"),            TEXT("Open menu"));
	MenuClose          = Add(TEXT("swui.menu.close"),           TEXT("Close menu"));
	MenuBack           = Add(TEXT("swui.menu.back"),            TEXT("Back / previous menu"));
	MenuContinue       = Add(TEXT("swui.menu.continue"),        TEXT("Continue / resume"));
	MenuSettings       = Add(TEXT("swui.menu.settings"),        TEXT("Open settings"));
	MenuQuit           = Add(TEXT("swui.menu.quit"),            TEXT("Quit / exit"));

	PointerHover       = Add(TEXT("swui.pointer.hover"),        TEXT("Pointer hovered or focused"));
	PointerLeftClick   = Add(TEXT("swui.pointer.leftClick"),    TEXT("Primary click / select"));
	PointerRightClick  = Add(TEXT("swui.pointer.rightClick"),   TEXT("Secondary click / context"));
	PointerMiddleClick = Add(TEXT("swui.pointer.middleClick"),  TEXT("Middle mouse click"));
	PointerScrollUp    = Add(TEXT("swui.pointer.scrollUp"),     TEXT("Scroll wheel up"));
	PointerScrollDown  = Add(TEXT("swui.pointer.scrollDown"),   TEXT("Scroll wheel down"));

	PointerMove        = Add(TEXT("swui.pointer.move"),         TEXT("Raw pointer movement"));
	PointerLeftDown    = Add(TEXT("swui.pointer.leftDown"),     TEXT("Left button pressed"));
	PointerLeftUp      = Add(TEXT("swui.pointer.leftUp"),       TEXT("Left button released"));
	PointerRightDown   = Add(TEXT("swui.pointer.rightDown"),    TEXT("Right button pressed"));
	PointerRightUp     = Add(TEXT("swui.pointer.rightUp"),      TEXT("Right button released"));
	PointerMiddleDown  = Add(TEXT("swui.pointer.middleDown"),   TEXT("Middle button pressed"));
	PointerMiddleUp    = Add(TEXT("swui.pointer.middleUp"),     TEXT("Middle button released"));
	PointerWheel       = Add(TEXT("swui.pointer.wheel"),        TEXT("Raw wheel delta"));

	// Collect all built-in tag names.
	BuiltInTagNames.Reset();
	const FGameplayTag* Begin = &Confirm;
	const FGameplayTag* End   = &PointerWheel + 1;
	for (const FGameplayTag* It = Begin; It != End; ++It)
	{
		if (It->IsValid()) BuiltInTagNames.Add(It->GetTagName());
	}
}

const TSet<FName>& FSwuiNavTags::GetAllBuiltInTagNames()
{
	Get(); // ensure initialized
	return BuiltInTagNames;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

USwuiNavigation::USwuiNavigation()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickGroup = TG_DuringPhysics;
	// Force tags to register early.
	FSwuiNavTags::Get();
}

void USwuiNavigation::BeginPlay()
{
	Super::BeginPlay();
	ApplyInputMode();
}

USwuiSubsystem* USwuiNavigation::GetSubsystem() const
{
	UWorld* World = GetWorld();
	if (!World) return nullptr;
	UGameInstance* GI = World->GetGameInstance();
	if (!GI) return nullptr;
	return GI->GetSubsystem<USwuiSubsystem>();
}

// ---------------------------------------------------------------------------
// Target Resolution
// ---------------------------------------------------------------------------

USwui* USwuiNavigation::GetTargetSwui() const
{
	if (AActor* Owner = GetOwner())
	{
		if (USwui* Swui = Owner->FindComponentByClass<USwui>())
		{
			return Swui;
		}
	}

	if (AActor* OuterActor = GetTypedOuter<AActor>())
	{
		if (USwui* Swui = OuterActor->FindComponentByClass<USwui>())
		{
			return Swui;
		}
	}

	if (UClass* OuterClass = GetTypedOuter<UClass>())
	{
		if (OuterClass->IsChildOf<AActor>())
		{
			if (AActor* ActorCDO = Cast<AActor>(OuterClass->GetDefaultObject()))
			{
				if (USwui* Swui = ActorCDO->FindComponentByClass<USwui>())
				{
					return Swui;
				}
			}
		}
	}

	return nullptr;
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
		PC->bShowMouseCursor = bShowCursorWhenFocused;
		{
			FInputModeUIOnly UIMode;
			PC->SetInputMode(UIMode);
		}
		break;

	case ESwuiInputMode::GameAndUi:
		PC->bShowMouseCursor = bShowCursorWhenFocused;
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

void USwuiNavigation::RestoreGameInput()
{
	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (PC)
	{
		PC->bShowMouseCursor = false;
		FInputModeGameOnly GameMode;
		PC->SetInputMode(GameMode);
	}
}

// ---------------------------------------------------------------------------
// Menu Input Mode — public Blueprint API
// ---------------------------------------------------------------------------

void USwuiNavigation::SetMenuInputActive(bool bActive)
{
	bMenuInputActive = bActive;

	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (!PC)
	{
		UE_LOG(LogSwuiNavigation, Warning,
			TEXT("[SwuiNav] SetMenuInputActive(%s) — owner is not a PlayerController"),
			bActive ? TEXT("true") : TEXT("false"));
		return;
	}

	USwuiSubsystem* SwuiSub = GetSubsystem();
	if (!SwuiSub)
	{
		UE_LOG(LogSwuiNavigation, Warning,
			TEXT("[SwuiNav] SetMenuInputActive(%s) — USwuiSubsystem not found"),
			bActive ? TEXT("true") : TEXT("false"));
		return;
	}

	const bool bDebugLog = bDebugMouseCapture || bLogNavigationEvents;
	SwuiSub->SetInputDebugLoggingEnabled(bDebugLog);

	if (bActive)
	{
		PC->bShowMouseCursor = true;
		PC->bEnableClickEvents = true;
		PC->bEnableMouseOverEvents = true;

		{
			FInputModeGameAndUI Mode;
			Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
			Mode.SetHideCursorDuringCapture(false);
			PC->SetInputMode(Mode);
		}

		SwuiSub->SetPointerInputEnabled(true);
		SwuiSub->SetBrowserInputFocus(true);

		SwuiSub->RequestHudVisualRefresh(0.50f, true);

		if (bDebugLog)
		{
			UE_LOG(LogSwuiNavigation, Log,
				TEXT("[SwuiNav] SetMenuInputActive(true) — cursor=show  input=GameAndUI  pointerForwarding=enabled  browserFocus=true  refresh=RequestHudVisualRefresh  debugLog=%d"),
				bDebugLog ? 1 : 0);
		}
	}
	else
	{
		SwuiSub->SetBrowserInputFocus(false);
		SwuiSub->SetPointerInputEnabled(false);

		PC->bShowMouseCursor = false;
		PC->bEnableClickEvents = false;
		PC->bEnableMouseOverEvents = false;
		{
			FInputModeGameOnly Mode;
			PC->SetInputMode(Mode);
		}

		if (bDebugLog)
		{
			UE_LOG(LogSwuiNavigation, Log,
				TEXT("[SwuiNav] SetMenuInputActive(false) — cursor=hidden  input=GameOnly  pointerForwarding=disabled  browserFocus=false  debugLog=%d"),
				bDebugLog ? 1 : 0);
		}
	}
}

// ---------------------------------------------------------------------------
// TickComponent — Slate input preprocessor handles pointer forwarding now.
// ---------------------------------------------------------------------------

void USwuiNavigation::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
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

const FSwuiNavigationEvent* USwuiNavigation::FindEventConfig(FGameplayTag Event) const
{
	for (const FSwuiNavigationEvent& NavEvent : NavigationEvents)
	{
		if (NavEvent.Event == Event) return &NavEvent;
	}
	return nullptr;
}

void USwuiNavigation::ForwardToJs(const FString& JsEventName, const FString& DetailJson)
{
	USwui* Target = GetTargetSwui();
	if (!Target) return;

	UWorld* World = GetWorld();
	if (!World) return;
	UGameInstance* GI = World->GetGameInstance();
	if (!GI) return;
	USwuiSubsystem* Sub = GI->GetSubsystem<USwuiSubsystem>();
	if (!Sub) return;

	// Send generic "swui.navigation" event with the tag name in the detail.
	const FString GenericDetail = FString::Printf(
		TEXT("{\"event\":\"%s\",\"detail\":%s}"),
		*EscapeJsonString(JsEventName),
		*DetailJson);
	const FString GenericScript = FString::Printf(
		TEXT("window.dispatchEvent(new CustomEvent(\"%s\",{detail:%s}));"),
		TEXT("swui.navigation"),
		*GenericDetail);
	Sub->ExecuteJavaScript(GenericScript);

	// Send tag-specific event.
	const FString SpecificScript = FString::Printf(
		TEXT("window.dispatchEvent(new CustomEvent(\"%s\",{detail:%s}));"),
		*EscapeJsonString(JsEventName),
		*DetailJson);
	Sub->ExecuteJavaScript(SpecificScript);
}

// ---------------------------------------------------------------------------
// Core dispatch: Blueprint first, then JS if unconsumed + enabled.
// ---------------------------------------------------------------------------

void USwuiNavigation::DispatchEvent(FGameplayTag Event, const FString& JsonPayload,
	TFunction<bool()> BlueprintHandler,
	bool bAllowJsForwarding)
{
	const FSwuiNavigationEvent* Config = FindEventConfig(Event);
	const FString TagName = Event.GetTagName().ToString();

	if (bLogNavigationEvents)
	{
		UE_LOG(LogSwuiNavigation, Log, TEXT("[SwuiNav] %s  payload=%s"), *TagName, *JsonPayload);
	}

	// Fire generic delegate.
	OnNavigationEvent.Broadcast(Event, FString());
	OnNavigationEventWithPayload.Broadcast(Event, JsonPayload);

	if (bAllowJsForwarding)
	{
		UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI UE->JS NAV] DispatchEvent tag=%s payload=%s forwardToJs=1"), *TagName, *JsonPayload);
	}
	else
	{
		UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI JS->UE NAV] DispatchEvent tag=%s payload=%s forwardToJs=0"), *TagName, *JsonPayload);
	}

	// Blueprint handler — returns true if consumed.
	bool bConsumed = false;
	if (!Config || Config->bBlueprintCallback)
	{
		bConsumed = BlueprintHandler();
	}

	// JS forwarding.
	if (bAllowJsForwarding && !bConsumed && (!Config || Config->bForwardToJS))
	{
		const FString JsName = Config ? Config->GetEffectiveJsEventName() : TagName;
		const FString Detail = JsonPayload.IsEmpty() ? TEXT("{}") : JsonPayload;
		ForwardToJs(JsName, Detail);
	}
}

// ---------------------------------------------------------------------------
// Core Navigation API
// ---------------------------------------------------------------------------

void USwuiNavigation::SendNavigationEvent(FGameplayTag Event)
{
	SendNavigationEventWithPayload(Event, TEXT("{}"));
}

void USwuiNavigation::SendNavigationEventWithPayload(FGameplayTag Event, const FString& JsonPayload)
{
	DispatchEvent(Event, JsonPayload, [this, Event, &JsonPayload]()
	{
		return HandleNavigationEvent(Event, JsonPayload);
	});
}

void USwuiNavigation::RefreshHudFrame(bool bForceFullFrameRefresh)
{
	if (!bForceFullFrameRefresh) return;
	UWorld* World = GetWorld();
	if (!World) return;
	UGameInstance* GI = World->GetGameInstance();
	if (!GI) return;
	USwuiSubsystem* Sub = GI->GetSubsystem<USwuiSubsystem>();
	if (Sub) Sub->RequestHudVisualRefresh(0.50f, true);
}

void USwuiNavigation::EmitEvent(FGameplayTag Event, bool bForceFullFrameRefresh)
{
	EmitEventWithPayload(Event, TEXT("{}"), bForceFullFrameRefresh);
}

void USwuiNavigation::EmitEventWithPayload(FGameplayTag Event, const FString& JsonPayload, bool bForceFullFrameRefresh)
{
	DispatchEvent(Event, JsonPayload, [this, Event, &JsonPayload]()
	{
		return HandleNavigationEvent(Event, JsonPayload);
	});
	RefreshHudFrame(bForceFullFrameRefresh);
}

void USwuiNavigation::ReceiveNavigationEventFromJs(FGameplayTag Event, const FString& JsonPayload)
{
	UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI JS->UE NAV] ReceiveNavigationEventFromJs tag=%s payload=%s"), *Event.GetTagName().ToString(), *JsonPayload);
	const FString Detail = JsonPayload.IsEmpty() ? TEXT("{}") : JsonPayload;
	const FSwuiNavTags& Tags = FSwuiNavTags::Get();

	if (Event == Tags.Up)
	{
		DispatchEvent(Event, Detail, [this]()
		{
			OnNavigate.Broadcast(ESwuiNavDirection::Up);
			return HandleNavigate(ESwuiNavDirection::Up);
		}, false);
		return;
	}

	if (Event == Tags.Down)
	{
		DispatchEvent(Event, Detail, [this]()
		{
			OnNavigate.Broadcast(ESwuiNavDirection::Down);
			return HandleNavigate(ESwuiNavDirection::Down);
		}, false);
		return;
	}

	if (Event == Tags.Left)
	{
		DispatchEvent(Event, Detail, [this]()
		{
			OnNavigate.Broadcast(ESwuiNavDirection::Left);
			return HandleNavigate(ESwuiNavDirection::Left);
		}, false);
		return;
	}

	if (Event == Tags.Right)
	{
		DispatchEvent(Event, Detail, [this]()
		{
			OnNavigate.Broadcast(ESwuiNavDirection::Right);
			return HandleNavigate(ESwuiNavDirection::Right);
		}, false);
		return;
	}

	if (Event == Tags.Confirm)
	{
		DispatchEvent(Event, Detail, [this]()
		{
			OnConfirm.Broadcast();
			return HandleConfirm();
		}, false);
		return;
	}

	if (Event == Tags.Cancel)
	{
		DispatchEvent(Event, Detail, [this]()
		{
			OnCancel.Broadcast();
			return HandleCancel();
		}, false);
		if (bCloseOnEscape)
		{
			RestoreGameInput();
		}
		return;
	}

	if (Event == Tags.NextTab)
	{
		DispatchEvent(Event, Detail, [this]()
		{
			OnNextTab.Broadcast();
			return HandleNextTab();
		}, false);
		return;
	}

	if (Event == Tags.PreviousTab)
	{
		DispatchEvent(Event, Detail, [this]()
		{
			OnPreviousTab.Broadcast();
			return HandlePreviousTab();
		}, false);
		return;
	}

	if (Event == Tags.MenuOpen)
	{
		DispatchEvent(Event, Detail, [this]()
		{
			OnMenuOpen.Broadcast();
			return HandleMenuOpen();
		}, false);
		return;
	}

	if (Event == Tags.MenuClose)
	{
		if (bLogNavigationEvents)
		{
			UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI PAINT] Explicit close refresh bypassed AutoFullTransition cooldown"));
		}
		DispatchEvent(Event, Detail, [this]()
		{
			OnMenuClose.Broadcast();
			return HandleMenuClose();
		}, false);
		RefreshHudFrame(true);
		// Schedule a delayed refresh to catch CEF paint after React commits close state.
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(CloseRefreshRetryHandle);
			World->GetTimerManager().SetTimer(CloseRefreshRetryHandle,
				FTimerDelegate::CreateWeakLambda(this, [this]()
				{
					if (bLogNavigationEvents)
					{
						UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI PAINT] Delayed close refresh retry"));
					}
					RefreshHudFrame(true);
				}),
				0.04f, false);
		}
		return;
	}

	if (Event == Tags.MenuBack)
	{
		DispatchEvent(Event, Detail, [this]()
		{
			OnMenuBack.Broadcast();
			return HandleMenuBack();
		}, false);
		RefreshHudFrame(true);
		return;
	}

	if (Event == Tags.MenuContinue)
	{
		DispatchEvent(Event, Detail, [this]()
		{
			OnMenuContinue.Broadcast();
			return HandleMenuContinue();
		}, false);
		RefreshHudFrame(true);
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(CloseRefreshRetryHandle);
			World->GetTimerManager().SetTimer(CloseRefreshRetryHandle,
				FTimerDelegate::CreateWeakLambda(this, [this]()
				{
					if (bLogNavigationEvents)
					{
						UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI PAINT] Delayed close refresh retry"));
					}
					RefreshHudFrame(true);
				}),
				0.04f, false);
		}
		return;
	}

	if (Event == Tags.MenuSettings)
	{
		DispatchEvent(Event, Detail, [this]()
		{
			OnMenuSettings.Broadcast();
			return HandleMenuSettings();
		}, false);
		RefreshHudFrame(true);
		return;
	}

	if (Event == Tags.MenuQuit)
	{
		DispatchEvent(Event, Detail, [this]()
		{
			OnMenuQuit.Broadcast();
			return HandleMenuQuit();
		}, false);
		return;
	}

	DispatchEvent(Event, Detail, [this, Event, &Detail]()
	{
		return HandleNavigationEvent(Event, Detail);
	}, false);
}

// ---------------------------------------------------------------------------
// Convenience Navigation Wrappers
// ---------------------------------------------------------------------------

void USwuiNavigation::Navigate(ESwuiNavDirection Direction, bool bForceFullFrameRefresh)
{
	const TCHAR* DirStr = DirectionToString(Direction);
	const FString Detail = FString::Printf(TEXT("{\"direction\":\"%s\"}"), DirStr);
	const auto& Tags = FSwuiNavTags::Get();

	FGameplayTag DirTag;
	switch (Direction)
	{
	case ESwuiNavDirection::Up:       DirTag = Tags.Up; break;
	case ESwuiNavDirection::Down:     DirTag = Tags.Down; break;
	case ESwuiNavDirection::Left:     DirTag = Tags.Left; break;
	case ESwuiNavDirection::Right:    DirTag = Tags.Right; break;
	case ESwuiNavDirection::Next:     DirTag = Tags.NextTab; break;
	case ESwuiNavDirection::Previous: DirTag = Tags.PreviousTab; break;
	}

	DispatchEvent(DirTag, Detail, [this, Direction]()
	{
		OnNavigate.Broadcast(Direction);
		return HandleNavigate(Direction);
	});
	RefreshHudFrame(bForceFullFrameRefresh);
}

void USwuiNavigation::Confirm(bool bForceFullFrameRefresh)
{
	DispatchEvent(FSwuiNavTags::Get().Confirm, TEXT("{}"), [this]()
	{
		OnConfirm.Broadcast();
		return HandleConfirm();
	});
	RefreshHudFrame(bForceFullFrameRefresh);
}

void USwuiNavigation::Cancel(bool bForceFullFrameRefresh)
{
	DispatchEvent(FSwuiNavTags::Get().Cancel, TEXT("{}"), [this]()
	{
		OnCancel.Broadcast();
		return HandleCancel();
	});

	if (bCloseOnEscape)
	{
		RestoreGameInput();
	}
	RefreshHudFrame(bForceFullFrameRefresh);
}

void USwuiNavigation::NextTab(bool bForceFullFrameRefresh)
{
	DispatchEvent(FSwuiNavTags::Get().NextTab, TEXT("{}"), [this]()
	{
		OnNextTab.Broadcast();
		return HandleNextTab();
	});
	RefreshHudFrame(bForceFullFrameRefresh);
}

void USwuiNavigation::PreviousTab(bool bForceFullFrameRefresh)
{
	DispatchEvent(FSwuiNavTags::Get().PreviousTab, TEXT("{}"), [this]()
	{
		OnPreviousTab.Broadcast();
		return HandlePreviousTab();
	});
	RefreshHudFrame(bForceFullFrameRefresh);
}

// ---------------------------------------------------------------------------
// Menu Convenience Wrappers
// ---------------------------------------------------------------------------

void USwuiNavigation::MenuOpen(bool bForceFullFrameRefresh)
{
	DispatchEvent(FSwuiNavTags::Get().MenuOpen, TEXT("{}"), [this]()
	{
		OnMenuOpen.Broadcast();
		return HandleMenuOpen();
	});
	RefreshHudFrame(bForceFullFrameRefresh);
}

void USwuiNavigation::MenuClose(bool bForceFullFrameRefresh)
{
	DispatchEvent(FSwuiNavTags::Get().MenuClose, TEXT("{}"), [this]()
	{
		OnMenuClose.Broadcast();
		return HandleMenuClose();
	});
	RefreshHudFrame(bForceFullFrameRefresh);
}

void USwuiNavigation::MenuBack(bool bForceFullFrameRefresh)
{
	DispatchEvent(FSwuiNavTags::Get().MenuBack, TEXT("{}"), [this]()
	{
		OnMenuBack.Broadcast();
		return HandleMenuBack();
	});
	RefreshHudFrame(bForceFullFrameRefresh);
}

void USwuiNavigation::MenuContinue(bool bForceFullFrameRefresh)
{
	DispatchEvent(FSwuiNavTags::Get().MenuContinue, TEXT("{}"), [this]()
	{
		OnMenuContinue.Broadcast();
		return HandleMenuContinue();
	});
	RefreshHudFrame(bForceFullFrameRefresh);
}

void USwuiNavigation::MenuSettings(bool bForceFullFrameRefresh)
{
	DispatchEvent(FSwuiNavTags::Get().MenuSettings, TEXT("{}"), [this]()
	{
		OnMenuSettings.Broadcast();
		return HandleMenuSettings();
	});
	RefreshHudFrame(bForceFullFrameRefresh);
}

void USwuiNavigation::MenuQuit(bool bForceFullFrameRefresh)
{
	DispatchEvent(FSwuiNavTags::Get().MenuQuit, TEXT("{}"), [this]()
	{
		OnMenuQuit.Broadcast();
		return HandleMenuQuit();
	});
	RefreshHudFrame(bForceFullFrameRefresh);
}

// ---------------------------------------------------------------------------
// High-Level Pointer Convenience Wrappers
// ---------------------------------------------------------------------------

void USwuiNavigation::Hover()
{
	SendNavigationEvent(FSwuiNavTags::Get().PointerHover);
}

void USwuiNavigation::LeftClick()
{
	SendNavigationEvent(FSwuiNavTags::Get().PointerLeftClick);
}

void USwuiNavigation::RightClick()
{
	SendNavigationEvent(FSwuiNavTags::Get().PointerRightClick);
}

void USwuiNavigation::MiddleClick()
{
	SendNavigationEvent(FSwuiNavTags::Get().PointerMiddleClick);
}

void USwuiNavigation::ScrollUp()
{
	SendNavigationEvent(FSwuiNavTags::Get().PointerScrollUp);
}

void USwuiNavigation::ScrollDown()
{
	SendNavigationEvent(FSwuiNavTags::Get().PointerScrollDown);
}

// ---------------------------------------------------------------------------
// Low-Level Pointer Input
// ---------------------------------------------------------------------------

void USwuiNavigation::PointerMove(FVector2D ScreenPosition)
{
	const FString Detail = FString::Printf(
		TEXT("{\"x\":%.1f,\"y\":%.1f}"), ScreenPosition.X, ScreenPosition.Y);

	DispatchEvent(FSwuiNavTags::Get().PointerMove, Detail, [this, ScreenPosition]()
	{
		OnPointerMove.Broadcast(ScreenPosition);
		return false;
	});
}

void USwuiNavigation::PointerPress(ESwuiPointerButton Button)
{
	const auto& Tags = FSwuiNavTags::Get();
	FGameplayTag BtnTag;
	switch (Button)
	{
	case ESwuiPointerButton::Left:   BtnTag = Tags.PointerLeftDown; break;
	case ESwuiPointerButton::Right:  BtnTag = Tags.PointerRightDown; break;
	case ESwuiPointerButton::Middle: BtnTag = Tags.PointerMiddleDown; break;
	}

	const FString Detail = FString::Printf(
		TEXT("{\"button\":\"%s\"}"), PointerButtonToString(Button));

	DispatchEvent(BtnTag, Detail, [this, Button]()
	{
		OnPointerPress.Broadcast(Button);
		return false;
	});
}

void USwuiNavigation::PointerRelease(ESwuiPointerButton Button)
{
	const auto& Tags = FSwuiNavTags::Get();
	FGameplayTag BtnTag;
	switch (Button)
	{
	case ESwuiPointerButton::Left:   BtnTag = Tags.PointerLeftUp; break;
	case ESwuiPointerButton::Right:  BtnTag = Tags.PointerRightUp; break;
	case ESwuiPointerButton::Middle: BtnTag = Tags.PointerMiddleUp; break;
	}

	const FString Detail = FString::Printf(
		TEXT("{\"button\":\"%s\"}"), PointerButtonToString(Button));

	DispatchEvent(BtnTag, Detail, [this, Button]()
	{
		OnPointerRelease.Broadcast(Button);
		return false;
	});
}

void USwuiNavigation::PointerWheel(float Delta)
{
	const FString Detail = FString::Printf(TEXT("{\"delta\":%.4f}"), Delta);

	DispatchEvent(FSwuiNavTags::Get().PointerWheel, Detail, [this, Delta]()
	{
		OnPointerWheel.Broadcast(Delta);
		return false;
	});

	// Also emit scroll-up/scroll-down for nonzero deltas.
	if (Delta > 0.f) ScrollUp();
	else if (Delta < 0.f) ScrollDown();
}

// ---------------------------------------------------------------------------
// Keyboard Input
// ---------------------------------------------------------------------------

void USwuiNavigation::KeyDown(FKey Key)
{
	OnKeyDown.Broadcast(Key);

	const FString Detail = FString::Printf(
		TEXT("{\"key\":\"%s\"}"), *EscapeJsonString(Key.ToString()));
	ForwardToJs(TEXT("swui.keyboard.keyDown"), Detail);
}

void USwuiNavigation::KeyUp(FKey Key)
{
	OnKeyUp.Broadcast(Key);

	const FString Detail = FString::Printf(
		TEXT("{\"key\":\"%s\"}"), *EscapeJsonString(Key.ToString()));
	ForwardToJs(TEXT("swui.keyboard.keyUp"), Detail);
}

void USwuiNavigation::TextInput(const FString& Text)
{
	OnTextInput.Broadcast(Text);

	const FString Detail = FString::Printf(
		TEXT("{\"text\":\"%s\"}"), *EscapeJsonString(Text));
	ForwardToJs(TEXT("swui.keyboard.textInput"), Detail);
}

// ---------------------------------------------------------------------------
// Editor Validation
// ---------------------------------------------------------------------------

#if WITH_EDITOR
EDataValidationResult USwuiNavigation::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	AActor* Owner = GetOwner();
	if (Owner && !Owner->FindComponentByClass<USwui>())
	{
		Context.AddError(FText::FromString(
			TEXT("USwuiNavigation requires a sibling USwui component on the same Actor.")));
		Result = EDataValidationResult::Invalid;
	}

	// Check for duplicate Navigation Event entries.
	TSet<FGameplayTag> Seen;
	for (int32 i = 0; i < NavigationEvents.Num(); ++i)
	{
		const FSwuiNavigationEvent& Evt = NavigationEvents[i];
		if (!Evt.Event.IsValid())
		{
			Context.AddError(FText::Format(
				NSLOCTEXT("SwuiNav", "InvalidTag", "Navigation Event [{0}] has an invalid/missing Gameplay Tag."),
				FText::AsNumber(i)));
			Result = EDataValidationResult::Invalid;
			continue;
		}
		if (Seen.Contains(Evt.Event))
		{
			Context.AddError(FText::Format(
				NSLOCTEXT("SwuiNav", "DuplicateTag", "Duplicate Navigation Event for tag '{0}' at index [{1}]."),
				FText::FromName(Evt.Event.GetTagName()), FText::AsNumber(i)));
			Result = EDataValidationResult::Invalid;
		}
		Seen.Add(Evt.Event);
	}

	return Result;
}
#endif

// ---------------------------------------------------------------------------
// BlueprintNativeEvent defaults — return false (unconsumed) by default.
// ---------------------------------------------------------------------------

bool USwuiNavigation::HandleNavigationEvent_Implementation(FGameplayTag Event, const FString& JsonPayload) { return false; }
bool USwuiNavigation::HandleNavigate_Implementation(ESwuiNavDirection Direction) { return false; }
bool USwuiNavigation::HandleConfirm_Implementation() { return false; }
bool USwuiNavigation::HandleCancel_Implementation() { return false; }
bool USwuiNavigation::HandleNextTab_Implementation() { return false; }
bool USwuiNavigation::HandlePreviousTab_Implementation() { return false; }
bool USwuiNavigation::HandleMenuOpen_Implementation() { return false; }
bool USwuiNavigation::HandleMenuClose_Implementation() { return false; }
bool USwuiNavigation::HandleMenuBack_Implementation() { return false; }
bool USwuiNavigation::HandleMenuContinue_Implementation() { return false; }
bool USwuiNavigation::HandleMenuSettings_Implementation() { return false; }
bool USwuiNavigation::HandleMenuQuit_Implementation() { return false; }
