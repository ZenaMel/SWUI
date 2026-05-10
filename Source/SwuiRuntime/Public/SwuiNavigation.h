#pragma once

#include "Components/ActorComponent.h"
#include "InputCoreTypes.h"
#include "GameplayTagContainer.h"
#include "SwuiTypes.h"
#include "SwuiNavigation.generated.h"

class USwui;

// ---------------------------------------------------------------------------
// Delegates — Blueprint-assignable events for side effects.
// ---------------------------------------------------------------------------

/** Fired for every navigation event (generic). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSwuiNavigationEventDelegate, FGameplayTag, Event, const FString&, JsonPayload);
/** Fired for directional navigation. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSwuiOnNavigate, ESwuiNavDirection, Direction);
/** Fired for simple actions (confirm/cancel/next tab/previous tab). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSwuiOnSimpleAction);
/** Fired for pointer movement. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSwuiOnPointerMove, FVector2D, ScreenPosition);
/** Fired for pointer button transitions. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSwuiOnPointerButton, ESwuiPointerButton, Button);
/** Fired for pointer wheel input. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSwuiOnPointerWheel, float, Delta);
/** Fired for keyboard key input. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSwuiOnKeyAction, FKey, Key);
/** Fired for text input. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSwuiOnTextInput, const FString&, Text);

// ---------------------------------------------------------------------------
// Built-in SWUI Gameplay Tags — resolved once at startup.
// ---------------------------------------------------------------------------
struct SWUIRUNTIME_API FSwuiNavTags
{
	// Navigation
	FGameplayTag Confirm;
	FGameplayTag Cancel;
	FGameplayTag Up;
	FGameplayTag Down;
	FGameplayTag Left;
	FGameplayTag Right;
	FGameplayTag NextTab;
	FGameplayTag PreviousTab;

	// Menu
	FGameplayTag MenuOpen;
	FGameplayTag MenuClose;
	FGameplayTag MenuBack;
	FGameplayTag MenuContinue;
	FGameplayTag MenuSettings;
	FGameplayTag MenuQuit;

	// High-level pointer
	FGameplayTag PointerHover;
	FGameplayTag PointerLeftClick;
	FGameplayTag PointerRightClick;
	FGameplayTag PointerMiddleClick;
	FGameplayTag PointerScrollUp;
	FGameplayTag PointerScrollDown;

	// Low-level pointer
	FGameplayTag PointerMove;
	FGameplayTag PointerLeftDown;
	FGameplayTag PointerLeftUp;
	FGameplayTag PointerRightDown;
	FGameplayTag PointerRightUp;
	FGameplayTag PointerMiddleDown;
	FGameplayTag PointerMiddleUp;
	FGameplayTag PointerWheel;

	static const FSwuiNavTags& Get();

	/** Returns the set of all built-in (Default) swui.* tag names. */
	static const TSet<FName>& GetAllBuiltInTagNames();

private:
	void Initialize();
	static FSwuiNavTags Instance;
	static bool bInitialized;
	static TSet<FName> BuiltInTagNames;
};

/**
 * USwuiNavigation — Central API for routing menu/navigation input into SWUI.
 *
 * Add to a PlayerController alongside USwui. Automatically targets the
 * sibling USwui component on the same owner. Navigation events use Gameplay
 * Tags that map directly to JS event names (dot-separated).
 *
 * Event flow: Blueprint callback runs first and can consume the event.
 * Unconsumed events forward to JS when Forward to JS is enabled.
 *
 * Input-system agnostic: call methods from Enhanced Input, legacy input,
 * custom C++, or Blueprint — whatever the game project uses.
 */
UCLASS(ClassGroup=Swui, Blueprintable, meta=(BlueprintSpawnableComponent))
class SWUIRUNTIME_API USwuiNavigation : public UActorComponent
{
	GENERATED_BODY()

public:
	USwuiNavigation();

	// ---- Target Resolution ----

	/** Returns the sibling USwui component on the same owner Actor. */
	UFUNCTION(BlueprintPure, Category="SWUI|Navigation",
		meta=(ToolTip="Returns the sibling USwui component on the same owner."))
	USwui* GetTargetSwui() const;

	// ---- Input Mode ----

	/** Controls how player input is routed while this navigation component is active. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation",
		meta=(ToolTip="Controls how player input is routed while this navigation component is active."))
	ESwuiInputMode InputMode = ESwuiInputMode::HudOnly;

	/** Show the mouse cursor when InputMode is not HUD Only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation",
		meta=(ToolTip="Show the mouse cursor when InputMode is not HUD Only."))
	bool bShowCursorWhenFocused = true;

	/** Cancel also clears focus and restores game-only input mode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation",
		meta=(ToolTip="Cancel also clears focus and restores game-only input mode."))
	bool bCloseOnEscape = false;

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation",
		meta=(ToolTip="Set and apply the input mode on the owning PlayerController."))
	void SetInputMode(ESwuiInputMode Mode);

	/** Apply the current InputMode to the owning PlayerController. */
	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation",
		meta=(ToolTip="Apply the current InputMode to the owning PlayerController."))
	void ApplyInputMode();

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation",
		meta=(ToolTip="Show or hide the mouse cursor."))
	void ShowCursor(bool bShow);

	/** Restore game-only input mode and hide cursor. */
	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation",
		meta=(ToolTip="Restore game-only input mode and hide cursor."))
	void RestoreGameInput();

	// ---- Debug ----

	/** Log every routed navigation event for debugging. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation|Debug",
		meta=(ToolTip="Log every routed navigation event for debugging."))
	bool bLogNavigationEvents = false;

	// ---- Navigation Events ----

	/** Navigation events exposed by this component. Event names are Gameplay Tags
	 *  and map directly to JS event names by default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation",
		meta=(DisplayName="Navigation Events",
			ToolTip="Navigation events exposed by this component. Event names are Gameplay Tags and map directly to JS event names by default."))
	TArray<FSwuiNavigationEvent> NavigationEvents;

	// ---- Core Navigation API ----

	/** Routes a named SWUI navigation event through Blueprint callbacks and then to JS when forwarding is enabled. */
	UFUNCTION(BlueprintCallable, Category="SWUI|Event Emitters",
		meta=(ToolTip="Routes a named SWUI navigation event through Blueprint callbacks and then to JS when forwarding is enabled."))
	void SendNavigationEvent(UPARAM(meta=(Categories="swui")) FGameplayTag Event);

	/** Routes a named SWUI navigation event with a JSON payload. */
	UFUNCTION(BlueprintCallable, Category="SWUI|Event Emitters",
		meta=(ToolTip="Routes a named SWUI navigation event with a JSON payload."))
	void SendNavigationEventWithPayload(UPARAM(meta=(Categories="swui")) FGameplayTag Event, const FString& JsonPayload);

	/** Emits a SWUI event via GameplayTag through the unified event pipeline. */
	UFUNCTION(BlueprintCallable, Category="SWUI|Event Emitters",
		meta=(ToolTip="Emits a SWUI event via GameplayTag through the unified event pipeline."))
	void EmitEvent(UPARAM(meta=(Categories="swui")) FGameplayTag Event);

	/** Emits a SWUI event with a JSON-formatted string payload through the unified event pipeline. */
	UFUNCTION(BlueprintCallable, Category="SWUI|Event Emitters",
		meta=(ToolTip="Emits a SWUI event with a JSON-formatted string payload through the unified event pipeline."))
	void EmitEventWithPayload(UPARAM(meta=(Categories="swui")) FGameplayTag Event, const FString& JsonPayload);

	// Called by the browser bridge for JS-originated tag events.
	// Uses native routing but does not forward the event back to JS.
	void ReceiveNavigationEventFromJs(FGameplayTag Event, const FString& JsonPayload);

	// ---- Convenience Navigation Wrappers ----

	UFUNCTION(BlueprintCallable, Category="SWUI|Event Emitters",
		meta=(ToolTip="Send directional navigation event."))
	void Navigate(ESwuiNavDirection Direction);

	UFUNCTION(BlueprintCallable, Category="SWUI|Event Emitters",
		meta=(ToolTip="Send confirm navigation event."))
	void Confirm();

	UFUNCTION(BlueprintCallable, Category="SWUI|Event Emitters",
		meta=(ToolTip="Send cancel navigation event."))
	void Cancel();

	UFUNCTION(BlueprintCallable, Category="SWUI|Event Emitters",
		meta=(ToolTip="Send next-tab navigation event."))
	void NextTab();

	UFUNCTION(BlueprintCallable, Category="SWUI|Event Emitters",
		meta=(ToolTip="Send previous-tab navigation event."))
	void PreviousTab();

	// ---- Menu Convenience Wrappers ----

	UFUNCTION(BlueprintCallable, Category="SWUI|Event Emitters",
		meta=(ToolTip="Send menu-open navigation event."))
	void MenuOpen();

	UFUNCTION(BlueprintCallable, Category="SWUI|Event Emitters",
		meta=(ToolTip="Send menu-close navigation event."))
	void MenuClose();

	UFUNCTION(BlueprintCallable, Category="SWUI|Event Emitters",
		meta=(ToolTip="Send menu-back navigation event."))
	void MenuBack();

	UFUNCTION(BlueprintCallable, Category="SWUI|Event Emitters",
		meta=(ToolTip="Send menu-continue navigation event."))
	void MenuContinue();

	UFUNCTION(BlueprintCallable, Category="SWUI|Event Emitters",
		meta=(ToolTip="Send menu-settings navigation event."))
	void MenuSettings();

	UFUNCTION(BlueprintCallable, Category="SWUI|Event Emitters",
		meta=(ToolTip="Send menu-quit navigation event."))
	void MenuQuit();

	// ---- High-Level Pointer Convenience Wrappers ----

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Pointer",
		meta=(ToolTip="Pointer hovered or focused a UI element."))
	void Hover();

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Pointer",
		meta=(ToolTip="Primary click / select."))
	void LeftClick();

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Pointer",
		meta=(ToolTip="Secondary click / context / back."))
	void RightClick();

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Pointer",
		meta=(ToolTip="Middle mouse button click."))
	void MiddleClick();

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Pointer",
		meta=(ToolTip="Scroll wheel up / list up."))
	void ScrollUp();

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Pointer",
		meta=(ToolTip="Scroll wheel down / list down."))
	void ScrollDown();

	// ---- Low-Level Pointer Input ----

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Pointer",
		meta=(ToolTip="Raw pointer movement with screen position payload."))
	void PointerMove(FVector2D ScreenPosition);

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Pointer",
		meta=(ToolTip="Pointer button pressed."))
	void PointerPress(ESwuiPointerButton Button);

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Pointer",
		meta=(ToolTip="Pointer button released."))
	void PointerRelease(ESwuiPointerButton Button);

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Pointer",
		meta=(ToolTip="Raw wheel delta event. Also emits scroll-up/scroll-down for nonzero deltas."))
	void PointerWheel(float Delta);

	// ---- Keyboard Input ----

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Keyboard",
		meta=(ToolTip="Keyboard key pressed."))
	void KeyDown(FKey Key);

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Keyboard",
		meta=(ToolTip="Keyboard key released."))
	void KeyUp(FKey Key);

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Keyboard",
		meta=(ToolTip="Text character input."))
	void TextInput(const FString& Text);

	// ---- Blueprint Events (side effects: sounds, animations, state) ----

	/** Fired for every navigation event. */
	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiNavigationEventDelegate OnNavigationEvent;

	/** Fired for every navigation event with payload. */
	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiNavigationEventDelegate OnNavigationEventWithPayload;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnNavigate OnNavigate;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnSimpleAction OnConfirm;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnSimpleAction OnCancel;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnSimpleAction OnNextTab;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnSimpleAction OnPreviousTab;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnSimpleAction OnMenuOpen;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnSimpleAction OnMenuClose;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnSimpleAction OnMenuBack;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnSimpleAction OnMenuContinue;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnSimpleAction OnMenuSettings;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnSimpleAction OnMenuQuit;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnPointerMove OnPointerMove;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnPointerButton OnPointerPress;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnPointerButton OnPointerRelease;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnPointerWheel OnPointerWheel;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnKeyAction OnKeyDown;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnKeyAction OnKeyUp;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Event Dispatchers")
	FSwuiOnTextInput OnTextInput;

	// ---- Handled/Consumed Handlers ----
	// Return true to consume the event (skip JS forwarding).

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation",
		meta=(ToolTip="Override to consume a navigation event. Return true to skip JS forwarding."))
	bool HandleNavigationEvent(FGameplayTag Event, const FString& JsonPayload);

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandleConfirm();

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandleCancel();

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandleNavigate(ESwuiNavDirection Direction);

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandleNextTab();

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandlePreviousTab();

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandleMenuOpen();

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandleMenuClose();

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandleMenuBack();

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandleMenuContinue();

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandleMenuSettings();

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandleMenuQuit();

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif

protected:
	virtual void BeginPlay() override;

private:
	// JS forwarding — sends CustomEvent to the browser via the subsystem.
	void ForwardToJs(const FString& JsEventName, const FString& DetailJson);

	// Core dispatch: Blueprint first, then JS if unconsumed + enabled.
	void DispatchEvent(FGameplayTag Event, const FString& JsonPayload,
		TFunction<bool()> BlueprintHandler,
		bool bAllowJsForwarding = true);

	// Finds the FSwuiNavigationEvent config for a tag, or nullptr.
	const FSwuiNavigationEvent* FindEventConfig(FGameplayTag Event) const;

	static FString EscapeJsonString(const FString& Raw);
	static const TCHAR* DirectionToString(ESwuiNavDirection Direction);
	static const TCHAR* PointerButtonToString(ESwuiPointerButton Button);
};
