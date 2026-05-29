#pragma once

#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "UObject/UnrealType.h"
#include "UObject/Field.h"
#include "SwuiBindingSource.h"
#include "SwuiTypes.h"
#include "SwuiSubsystem.generated.h"

class USwuiView;
class UUserWidget;
class FSwuiInputPreprocessor;

// Generic bridge object — binds to any observed delegate, intercepts ProcessEvent,
// and serializes the actual broadcast parameters into the JS CustomEvent detail
// using the delegate's own SignatureFunction property layout + param names.
// This avoids per-type Fire* functions by hooking into UObject::ProcessEvent at
// the base level. When the delegate broadcasts, ProcessDelegate calls ProcessEvent
// on this object with the real param data. We read it using the delegate's
// SignatureFunction property offsets — which are guaranteed to match because UHT
// generates both the _Script_Parms struct and the property metadata from the same
// source. See architecture/delegate-payloads.md.
UCLASS()
class USwuiDelegateBridge : public UObject
{
	GENERATED_BODY()
public:
	void Init(const FString& InNsKey, USwuiSubsystem* InOwner, UFunction* InDelegateSignature);

	// Single no-op UFUNCTION that gets bound to the observed delegate via AddDelegate.
	// When the delegate fires, ProcessEvent intercepts the call before it reaches
	// the function body, serializes Parms, and never calls the no-op body.
	UFUNCTION()
	void DelegateHook() {}

	virtual void ProcessEvent(UFunction* Function, void* Parms) override;

protected:
	FString NamespacedKey;
	UPROPERTY()
	USwuiSubsystem* Owner = nullptr;
	UPROPERTY()
	UFunction* HookFunction = nullptr;
	UPROPERTY()
	UFunction* DelegateSignature = nullptr;
};

// ---- Internal registry entries ----

struct FSwuiObservedProperty
{
	TWeakObjectPtr<UObject> Source;
	FString                 NamespacedKey; // "player.Health"
	FName                   PropertyName;  // "Health"
	FProperty*              CachedProp;    // resolved once at Observe time
};

struct FSwuiObservedDelegate
{
	TWeakObjectPtr<UObject> Source;
	FString                 NamespacedKey; // "player.OnTakeDamage"
	FName                   DelegateName;
	// Payload field names + types cached at bind time for fast dispatch + codegen
	TArray<TTuple<FName, FString>> PayloadFields; // (FieldName, TSType)
};

// ---- Subsystem ----

UCLASS()
class SWUIRUNTIME_API USwuiSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	// UGameInstanceSubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	// FTickableGameObject — ticks every engine frame, no timer needed
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override { return !IsTemplate() && View != nullptr; }
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(USwuiSubsystem, STATGROUP_Tickables); }
	virtual void Deinitialize() override;

	// ---- Renderer ----
	// Called by USwui component on BeginPlay.
	void InitRenderer(const FString& URI, const FString& InterfaceName,
		AActor* OwnerActor, bool bIsHUD, int32 Width, int32 Height, int32 ZOrder,
		UMaterialInterface* BaseMaterial, FName TextureParamName,
		const FSwuiInstanceSettings& InstanceSettings = FSwuiInstanceSettings{});

	void ShutdownRenderer();

	/** Push updated per-instance settings to the live view without restarting.
	 *  Call from USwui::PostEditChangeProperty to apply Details panel changes during PIE. */
	void UpdateInstanceSettings(const FSwuiInstanceSettings& NewSettings);

	/** Immediately shuts down SWUI rendering. Safe to call at BeginPlay on any actor.
	 *  Prevents any further InitRenderer calls for the lifetime of this game instance. */
	UFUNCTION(BlueprintCallable, Category="SimpleWebUI|Debug")
	void DisablePlugin();

	UFUNCTION(BlueprintPure, Category="SimpleWebUI|Debug")
	bool IsPluginDisabled() const { return bDisabledAtRuntime; }

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void SetWidgetVisible(bool bVisible);

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void LoadURI(const FString& URI);

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI")
	void ExecuteJavaScript(const FString& Script);

	// ---- K2Node expansion targets — do not call directly from C++ or Blueprint ----
	// These are the functions the "SWUI Observe" and "SWUI Observe Event" graph nodes
	// expand into at Blueprint compile time. Namespace is always auto-derived.

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true"))
	static void K2_Observe(UObject* Source, FName PropertyName);

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true"))
	static void K2_ObserveEvent(UObject* Source, FName DelegateName);

	// Called by USwui on BeginPlay; stores binding config so ObserveSource can look it up.
	void SetBindingSources(const TArray<FSwuiBindingSource>& Sources);

	// Finds the BindingSource entry whose SourceClass matches Instance's class (or a parent),
	// then calls ObserveProperty for every checked property in that entry.
	// Call this once per instance you want to sync (e.g., in OnPossess for your Character).
	// Slot 0 (owner class) is called automatically by the Swui component — no manual call needed.
	UFUNCTION(BlueprintCallable, Category="SimpleWebUI", meta=(DefaultToSelf="Instance"))
	void ObserveSource(UObject* Instance, bool bWarnOnMiss = true);

	// ---- Render Activity API ----

	/** Requests a short HUD visual refresh burst: forces a browser frame, marks
	 *  animation active for the given duration, and optionally requests a full
	 *  texture upload on the next frame. */
	void RequestHudVisualRefresh(float DurationSeconds = 0.30f, bool bForceFullUpload = true);

	/** Updates the interaction timer (called from TickComponent when pointer
	 *  activity is detected). Extends the interactive frame-forcing window. */
	void UpdateUiInteractionTime();

	// ---- Pointer Input Forwarding (bridge to View) ----

	void SetPointerInputEnabled(bool bEnabled);
	void SetBrowserInputFocus(bool bFocused);

	// ---- Slate Input Preprocessor Helpers ----

	USwuiView* GetActiveView() const { return View; }
	bool IsInputDebugLoggingEnabled() const { return bInputDebugLogging; }
	void SetInputDebugLoggingEnabled(bool bEnabled) { bInputDebugLogging = bEnabled; }

	// ---- Command Runtime (function-backed navigation events) ----

	/** Re-scan binding sources for UFUNCTION(meta=(SwuiEvent="...")) and
	 *  rebuild the function-command registry. Called at bindings refresh
	 *  time (SetBindingSources) and on subsystem init. */
	void RebuildCommandRuntime();

	/** Look up a function-backed command by tag.
	 *  @return true if a matching UFUNCTION was registered. */
	bool TryResolveFunctionCommand(FGameplayTag Tag, FSwuiFunctionCommand& OutCommand) const;

	/** Find exactly one active UObject instance for the given class from
	 *  active binding sources and subsystem containers.
	 *  @return true if exactly one instance was found.
	 *  @param OutError Filled with a human-readable reason on failure. */
	bool TryResolveActiveBindingTarget(UClass* RequiredClass, UObject*& OutTarget, FString& OutError) const;

	/** Returns the cached command registry (used by details panel and validation). */
	const TMap<FGameplayTag, FSwuiFunctionCommand>& GetCommandRegistry() const { return FunctionCommands; }

	// ---- Public API ----

	void ObserveProperty(UObject* Source, const FString& Namespace, const FName& PropertyName);
	void ObserveDelegate(UObject* Source, const FString& Namespace, const FName& DelegateName);

	UFUNCTION(BlueprintCallable, Category="SimpleWebUI", meta=(DefaultToSelf="Source"))
	void Unobserve(UObject* Source);

	// Access the cached delegate payload shapes (used by TS codegen).
	const TArray<FSwuiObservedDelegate>& GetObservedDelegates() const { return ObservedDelegates; }
	const TArray<FSwuiObservedProperty>& GetObservedProperties() const { return ObservedProperties; }

	// Internal — called by USwuiDelegateBridge::ProcessEvent to enqueue JS.
	void QueueHudEventScript(const FString& Script);

private:
	bool  bDisabledAtRuntime = false;
	float TickAccumulator    = 0.f; // throttles JS pushes to CEF frame rate
	float AvgFPS             = 60.f; // exponential moving average engine FPS
	float LastDeltaTime      = 0.f;  // last engine frame delta (seconds)

	UPROPERTY()
	USwuiView* View = nullptr;

	UPROPERTY()
	UUserWidget* Widget = nullptr;

	UPROPERTY()
	class USwuiHudRoiOverlayWidget* RoiOverlay = nullptr;

	TArray<FSwuiObservedProperty> ObservedProperties;
	TArray<FSwuiObservedDelegate> ObservedDelegates;
	TArray<FSwuiBindingSource>    CachedBindingSources;

	/** Function-backed command registry — built by RebuildCommandRuntime. */
	TMap<FGameplayTag, FSwuiFunctionCommand> FunctionCommands;

	FString ResolveNamespace(UObject* Source, const FString& Namespace) const;

	// Dynamic delegate sink — one per observed delegate binding
	UFUNCTION()
	void OnObservedDelegateFired();

	bool FlushHudStateToJs(float DeltaTime);
	bool SendExternalBeginFrameIfDue(float DeltaTime);

	TArray<FString> QueuedHudEventScripts;
	TMap<FString, FString> LastObservedValues;

	// Per-delegate bridge objects created by ObserveDelegate.
	UPROPERTY()
	TArray<USwuiDelegateBridge*> DelegateBridges;

	/** True when a DOM input/textarea/select/contenteditable element has focus inside the CEF browser. */
	bool bTextInputFocused = false;

public:
	/** Whether the CEF browser currently has focus on an editable text element. */
	UFUNCTION(BlueprintPure, Category="SimpleWebUI")
	bool IsTextInputFocused() const { return bTextInputFocused; }

	/** Resets the focus script injection flag (called when browser is destroyed). */
	void ResetFocusScriptInjection() { bFocusScriptInjected = false; }

	/** Last interaction time, used to keep the animation window active after pointer events. */
	double LastUiInteractionTime = 0.0;

	/** Slate input preprocessor for raw pointer forwarding to CEF. */
	TSharedPtr<FSwuiInputPreprocessor> InputPreprocessor;

	/** Cached input debug logging state from USwuiNavigation. */
	bool bInputDebugLogging = false;

	// ---- Low-Latency Frame Pacing ----

	/** Evaluates the LowLatencyFramePacingMode setting and applies/restores r.OneFrameThreadLag. */
	void UpdateLowLatencyFramePacing();

	// ---- HUD ROI Overlay ----

	void UpdateRoiOverlay();
	void CreateRoiOverlay();
	void DestroyRoiOverlay();

	/** Saved r.OneFrameThreadLag value; -1 = not yet saved. */
	int32 SavedOneFrameThreadLag = -1;

	/** Last mode that was applied, to avoid repeated CVar sets. */
	ESwuiLowLatencyFramePacingMode LastAppliedFramePacingMode = ESwuiLowLatencyFramePacingMode::Disabled;

	bool bFocusScriptInjected = false;
};
