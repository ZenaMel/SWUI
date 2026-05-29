#include "SwuiInputPreprocessor.h"
#include "SwuiSubsystem.h"
#include "SwuiView.h"
#include "Input/Events.h"

DEFINE_LOG_CATEGORY_STATIC(LogSwuiInputPreprocessor, Log, All);

FSwuiInputPreprocessor::FSwuiInputPreprocessor(USwuiSubsystem* InSubsystem)
	: Subsystem(InSubsystem)
{
}

bool FSwuiInputPreprocessor::ShouldForwardEvent(const FPointerEvent& MouseEvent) const
{
	USwuiSubsystem* Sub = Subsystem.Get();
	if (!Sub) return false;

	USwuiView* View = Sub->GetActiveView();
	if (!View) return false;

	if (!View->IsPointerInputEnabled()) return false;

	if (!View->HasBrowserHost()) return false;

	int32 BrowserX = 0, BrowserY = 0;
	if (!View->ScreenToBrowserPixel(MouseEvent.GetScreenSpacePosition(), BrowserX, BrowserY)) return false;

	return true;
}

void FSwuiInputPreprocessor::UpdateInteractionTime() const
{
	if (USwuiSubsystem* Sub = Subsystem.Get())
	{
		Sub->UpdateUiInteractionTime();
	}
}

bool FSwuiInputPreprocessor::HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	if (!ShouldForwardEvent(MouseEvent)) return false;

	USwuiSubsystem* Sub = Subsystem.Get();
	USwuiView* View = Sub->GetActiveView();

	const bool bForwarded = View->ForwardMouseMoveToBrowser(MouseEvent.GetScreenSpacePosition());
	if (!bForwarded) return false;

	UpdateInteractionTime();

	if (Sub->IsInputDebugLoggingEnabled())
	{
		const FVector2D ScreenPos = MouseEvent.GetScreenSpacePosition();
		UE_LOG(LogSwuiInputPreprocessor, Log, TEXT("[SwuiPreprocessor] MouseMove forwarded: (%.0f, %.0f)"),
			ScreenPos.X, ScreenPos.Y);
	}

	return true;
}

bool FSwuiInputPreprocessor::HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	if (!ShouldForwardEvent(MouseEvent)) return false;

	USwuiSubsystem* Sub = Subsystem.Get();
	USwuiView* View = Sub->GetActiveView();

	const bool bForwarded = View->ForwardMouseButtonToBrowser(
		MouseEvent.GetScreenSpacePosition(),
		MouseEvent.GetEffectingButton(),
		false,
		1);
	if (!bForwarded) return false;

	UpdateInteractionTime();

	if (Sub->IsInputDebugLoggingEnabled())
	{
		const FVector2D ScreenPos = MouseEvent.GetScreenSpacePosition();
		UE_LOG(LogSwuiInputPreprocessor, Log, TEXT("[SwuiPreprocessor] MouseButton Down: key=%s  (%.0f, %.0f)"),
			*MouseEvent.GetEffectingButton().ToString(), ScreenPos.X, ScreenPos.Y);
	}

	return true;
}

bool FSwuiInputPreprocessor::HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	if (!ShouldForwardEvent(MouseEvent)) return false;

	USwuiSubsystem* Sub = Subsystem.Get();
	USwuiView* View = Sub->GetActiveView();

	const bool bForwarded = View->ForwardMouseButtonToBrowser(
		MouseEvent.GetScreenSpacePosition(),
		MouseEvent.GetEffectingButton(),
		true,
		1);
	if (!bForwarded) return false;

	UpdateInteractionTime();

	if (Sub->IsInputDebugLoggingEnabled())
	{
		const FVector2D ScreenPos = MouseEvent.GetScreenSpacePosition();
		UE_LOG(LogSwuiInputPreprocessor, Log, TEXT("[SwuiPreprocessor] MouseButton Up: key=%s  (%.0f, %.0f)"),
			*MouseEvent.GetEffectingButton().ToString(), ScreenPos.X, ScreenPos.Y);
	}

	return true;
}

bool FSwuiInputPreprocessor::HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& InWheelEvent, const FPointerEvent* InGestureEvent)
{
	if (!ShouldForwardEvent(InWheelEvent)) return false;

	USwuiSubsystem* Sub = Subsystem.Get();
	USwuiView* View = Sub->GetActiveView();

	const float WheelDelta = InWheelEvent.GetWheelDelta();
	if (FMath::IsNearlyZero(WheelDelta)) return false;

	const bool bForwarded = View->ForwardMouseWheelToBrowser(
		InWheelEvent.GetScreenSpacePosition(),
		0.0f,
		WheelDelta);
	if (!bForwarded) return false;

	UpdateInteractionTime();

	if (Sub->IsInputDebugLoggingEnabled())
	{
		const FVector2D ScreenPos = InWheelEvent.GetScreenSpacePosition();
		UE_LOG(LogSwuiInputPreprocessor, Log, TEXT("[SwuiPreprocessor] Wheel: delta=%.1f  (%.0f, %.0f)"),
			WheelDelta, ScreenPos.X, ScreenPos.Y);
	}

	return true;
}

// Gate: only forward keyboard when the view exists, pointer input is enabled
// (meaning the SWUI menu/screen is active), and the CEF browser has a host.
// Identical conditions to mouse forwarding — keyboard follows the same on/off.
bool FSwuiInputPreprocessor::ShouldForwardKeyboard() const
{
	USwuiSubsystem* Sub = Subsystem.Get();
	if (!Sub) return false;
	USwuiView* View = Sub->GetActiveView();
	if (!View) return false;
	if (!View->IsPointerInputEnabled()) return false;
	if (!View->HasBrowserHost()) return false;
	return true;
}

// IInputProcessor exposes HandleKeyDownEvent and HandleKeyUpEvent but NOT
// HandleKeyCharEvent (character/IME events). CEF needs both KEYDOWN and KEYEVENT_CHAR
// for printable keystrokes. We synthesize the CHAR from FKeyEvent::GetCharacter()
// on every regular key press. Modifier-only keys (Ctrl, Shift, Alt) skip the CHAR event.
//
// Without this, HTML <input>, <textarea>, and contenteditable elements never receive
// text — CEF has no other path to know what character was typed.
bool FSwuiInputPreprocessor::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
	if (!ShouldForwardKeyboard()) return false;

	USwuiSubsystem* Sub = Subsystem.Get();
	USwuiView* View = Sub->GetActiveView();

	// Send KEYEVENT_KEYDOWN to CEF.
	View->ForwardKeyEventToBrowser(InKeyEvent, false);

	// Synthesize KEYEVENT_CHAR from the physical key press using Win32 ToUnicode.
	// FKeyEvent::GetCharacter() returns 0 for KEYDOWN on Windows — the character
	// cannot be derived from the key event alone. ToUnicode maps the virtual key
	// code + current keyboard state to the actual Unicode character the user typed.
	const uint32 VK = InKeyEvent.GetKeyCode();
	if (VK != 0 && !InKeyEvent.GetKey().IsModifierKey())
	{
		BYTE KeyState[256] = {};
		::GetKeyboardState(KeyState);

		wchar_t Buf[8] = {};
		const int32 Ret = ::ToUnicode(
			VK,
			::MapVirtualKeyW(VK, MAPVK_VK_TO_VSC),
			KeyState,
			Buf,
			8,
			0);

		// Ret > 0  → produced character(s), forward each one
		// Ret == 0 → no character (function key, etc.)
		// Ret == -1 → dead key (e.g. ^, ~), wait for next keystroke
		if (Ret > 0)
		{
			for (int32 i = 0; i < Ret; ++i)
			{
				View->ForwardCharToBrowser(Buf[i], InKeyEvent.GetModifierKeys());
			}
		}
	}

	UpdateInteractionTime();
	return true;
}

bool FSwuiInputPreprocessor::HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
	if (!ShouldForwardKeyboard()) return false;

	USwuiSubsystem* Sub = Subsystem.Get();
	USwuiView* View = Sub->GetActiveView();

	View->ForwardKeyEventToBrowser(InKeyEvent, true);

	UpdateInteractionTime();
	return true;
}

bool FSwuiInputPreprocessor::HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	if (!ShouldForwardEvent(MouseEvent)) return false;

	USwuiSubsystem* Sub = Subsystem.Get();
	USwuiView* View = Sub->GetActiveView();

	const bool bForwarded = View->ForwardMouseButtonToBrowser(
		MouseEvent.GetScreenSpacePosition(),
		MouseEvent.GetEffectingButton(),
		false,
		2);
	if (!bForwarded) return false;

	UpdateInteractionTime();

	if (Sub->IsInputDebugLoggingEnabled())
	{
		const FVector2D ScreenPos = MouseEvent.GetScreenSpacePosition();
		UE_LOG(LogSwuiInputPreprocessor, Log, TEXT("[SwuiPreprocessor] DoubleClick: key=%s  (%.0f, %.0f)"),
			*MouseEvent.GetEffectingButton().ToString(), ScreenPos.X, ScreenPos.Y);
	}

	return true;
}
