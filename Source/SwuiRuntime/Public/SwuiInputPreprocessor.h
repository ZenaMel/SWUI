#pragma once

#include "Framework/Application/IInputProcessor.h"
#include "UObject/WeakObjectPtr.h"

class USwuiSubsystem;

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

	virtual const TCHAR* GetDebugName() const override { return TEXT("FSwuiInputPreprocessor"); }

private:
	bool ShouldForwardEvent(const FPointerEvent& MouseEvent) const;
	void UpdateInteractionTime() const;

	TWeakObjectPtr<USwuiSubsystem> Subsystem;
};
