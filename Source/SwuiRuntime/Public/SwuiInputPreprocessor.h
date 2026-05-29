#pragma once

#include "Framework/Application/IInputProcessor.h"
#include "UObject/WeakObjectPtr.h"

class USwuiSubsystem;

// Slate input preprocessor that intercepts mouse and keyboard events before they
// reach the normal game input pipeline and forwards them to the CEF browser.
//
// Mouse events go through SendMouseClickEvent / SendMouseMoveEvent etc. on the
// CefBrowserHost. Keyboard events go through SendKeyEvent and SendCharEvent.
// Without this preprocessor, the CEF browser receives no input at all — SWUI
// renders offscreen and has no native window to receive OS input messages.
//
// The forwarding gate is IsPointerInputEnabled() on the active USwuiView.
// When the SWUI menu is open (SetMenuInputActive(true)), both mouse and keyboard
// are forwarded. When closed, neither is.
class SWUIRUNTIME_API FSwuiInputPreprocessor : public IInputProcessor
{
public:
	explicit FSwuiInputPreprocessor(USwuiSubsystem* InSubsystem);

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

	virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent) override;
	virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;

	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
	virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;

	virtual const TCHAR* GetDebugName() const override { return TEXT("FSwuiInputPreprocessor"); }

private:
	bool ShouldForwardEvent(const FPointerEvent& MouseEvent) const;
	bool ShouldForwardKeyboard() const;
	void UpdateInteractionTime() const;

	TWeakObjectPtr<USwuiSubsystem> Subsystem;
};
