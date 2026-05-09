#pragma once

#include "Components/ActorComponent.h"
#include "InputCoreTypes.h"
#include "SwuiTypes.h"
#include "SwuiNavigation.generated.h"

class USwui;

// ---------------------------------------------------------------------------
// Delegates — Blueprint-assignable events for side effects.
// ---------------------------------------------------------------------------

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FSwuiOnNavigationAction, FName, ActionName, const FString&, JsonPayload);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSwuiOnNavigate, ESwuiNavDirection, Direction);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSwuiOnSimpleAction);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSwuiOnPointerMove, FVector2D, ScreenPosition);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSwuiOnPointerButton, ESwuiPointerButton, Button);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSwuiOnPointerWheel, float, Delta);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSwuiOnKeyAction, FKey, Key);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSwuiOnTextInput, const FString&, Text);

/**
 * USwuiNavigation — Central API for routing menu/navigation input into SWUI.
 *
 * Add to a PlayerController alongside USwui. Provides Blueprint-callable
 * navigation methods, Blueprint event callbacks for native side effects
 * (sounds, animations, state), and automatic JS event forwarding.
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

	// ---- Focus ----

	/** The USwui component receiving forwarded navigation input. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation")
	USwui* FocusedSwui = nullptr;

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation")
	void SetFocusedSwui(USwui* InSwui);

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation")
	void ClearFocusedSwui();

	UFUNCTION(BlueprintPure, Category="SWUI|Navigation")
	USwui* GetFocusedSwui() const { return FocusedSwui; }

	// ---- Input Mode ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation")
	ESwuiInputMode InputMode = ESwuiInputMode::HudOnly;

	/** Show the mouse cursor when a USwui is focused and InputMode != HudOnly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation")
	bool bShowCursorWhenFocused = true;

	/** Auto-focus the first USwui found on the owner at BeginPlay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation")
	bool bFocusOnBeginPlay = false;

	/** Cancel() also clears focus and restores input mode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation")
	bool bCloseOnEscape = false;

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation")
	void SetInputMode(ESwuiInputMode Mode);

	/** Apply the current InputMode to the owning PlayerController. */
	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation")
	void ApplyInputMode();

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation")
	void ShowCursor(bool bShow);

	// ---- Dispatch ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation")
	ESwuiNavigationDispatchOrder DispatchOrder = ESwuiNavigationDispatchOrder::BlueprintFirst;

	/** Log every routed action for debugging. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation|Debug")
	bool bLogNavigationActions = false;

	// ---- Action Map ----

	/** Custom named navigation actions. Entries define JS event routing per action. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SWUI|Navigation")
	TArray<FSwuiNavigationAction> NavigationActions;

	// ---- Navigation API ----

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation")
	void Navigate(ESwuiNavDirection Direction);

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation")
	void Confirm();

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation")
	void Cancel();

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation")
	void NextTab();

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation")
	void PreviousTab();

	// ---- Custom Actions ----

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation")
	void TriggerAction(FName ActionName);

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation")
	void TriggerActionWithPayload(FName ActionName, const FString& JsonPayload);

	// ---- Pointer Input ----

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Pointer")
	void PointerMove(FVector2D ScreenPosition);

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Pointer")
	void PointerPress(ESwuiPointerButton Button);

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Pointer")
	void PointerRelease(ESwuiPointerButton Button);

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Pointer")
	void PointerWheel(float Delta);

	// ---- Keyboard Input ----

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Keyboard")
	void KeyDown(FKey Key);

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Keyboard")
	void KeyUp(FKey Key);

	UFUNCTION(BlueprintCallable, Category="SWUI|Navigation|Keyboard")
	void TextInput(const FString& Text);

	// ---- Blueprint Events (side effects: sounds, animations, state) ----

	UPROPERTY(BlueprintAssignable, Category="SWUI|Navigation|Events")
	FSwuiOnNavigationAction OnNavigationAction;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Navigation|Events")
	FSwuiOnNavigationAction OnNavigationActionWithPayload;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Navigation|Events")
	FSwuiOnNavigate OnNavigate;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Navigation|Events")
	FSwuiOnSimpleAction OnConfirm;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Navigation|Events")
	FSwuiOnSimpleAction OnCancel;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Navigation|Events")
	FSwuiOnSimpleAction OnNextTab;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Navigation|Events")
	FSwuiOnSimpleAction OnPreviousTab;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Navigation|Events")
	FSwuiOnPointerMove OnPointerMove;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Navigation|Events")
	FSwuiOnPointerButton OnPointerPress;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Navigation|Events")
	FSwuiOnPointerButton OnPointerRelease;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Navigation|Events")
	FSwuiOnPointerWheel OnPointerWheel;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Navigation|Events")
	FSwuiOnKeyAction OnKeyDown;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Navigation|Events")
	FSwuiOnKeyAction OnKeyUp;

	UPROPERTY(BlueprintAssignable, Category="SWUI|Navigation|Events")
	FSwuiOnTextInput OnTextInput;

	// ---- Handled/Consumed Handlers ----
	// Return true to consume the action (skip JS forwarding in BlueprintFirst mode).

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandleNavigationAction(FName ActionName, const FString& JsonPayload);

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandleNavigate(ESwuiNavDirection Direction);

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandleConfirm();

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandleCancel();

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandleNextTab();

	UFUNCTION(BlueprintNativeEvent, Category="SWUI|Navigation")
	bool HandlePreviousTab();

protected:
	virtual void BeginPlay() override;

private:
	// JS forwarding helpers.
	void ForwardToJs(const FString& JsEventName, const FString& DetailJson);
	void DispatchBuiltIn(const FString& JsEventName, const FString& DetailJson,
		TFunction<bool()> BlueprintHandler, const FString* GenericDetailJson = nullptr);

	static FString EscapeJsonString(const FString& Raw);
	static const TCHAR* DirectionToString(ESwuiNavDirection Direction);
	static const TCHAR* PointerButtonToString(ESwuiPointerButton Button);

	const FSwuiNavigationAction* FindAction(FName ActionName) const;
};
