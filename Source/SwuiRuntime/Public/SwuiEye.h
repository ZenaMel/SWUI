#pragma once

#include "RenderHandler.h"
#include "SwuiTypes.h"
#include "SwuiManager.h"
#include "UObject/Object.h"
#include "SwuiEye.generated.h"

UCLASS(ClassGroup = Swui, Blueprintable)
class SWUIRUNTIME_API USwuiEye : public UObject
{
	GENERATED_BODY()

	USwuiEye(const class FObjectInitializer& PCIP);

public:

	//Event delegates
	UPROPERTY(BlueprintAssignable, Category = "Swui Browser Events")
	FDownloadCompleteSignature DownloadComplete;

	UPROPERTY(BlueprintAssignable, Category = "Swui Browser Events")
	FDownloadUpdatedSignature DownloadUpdated;

	//GENERATED_UCLASS_BODY()

	/** Initialize function, should be called after properties are set */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void Init();

	/** The default URL this UI component will load */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SwuiRuntime")
	FString DefaultURL;

	/** Is this UI component current active? */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SwuiRuntime")
	bool bEnabled;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SwuiRuntime")
	FSwuiEyeSettings Settings;

	/** Material that will be instanced to load UI texture into it */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SwuiRuntime")
	UMaterialInterface* BaseMaterial;

	/** Name of parameter to load UI texture into material */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SwuiRuntime")
	FName TextureParameterName = "SwuiTexture";

	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	USwuiEye* SetProperties(const int32 SetWidth,
							const int32 SetHeight,
							const bool SetIsTransparent,
							const bool SetEnabled,
							const bool SetWebGL,
							const FString& SetDefaultURL,
							const FName& SetTextureParameterName,
							UMaterialInterface* SetBaseMaterial);

	/** Get the texture data from our UI component */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	UTexture2D* GetTexture() const;

	/** Execute JS code inside the browser */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void ExecuteJS(const FString& code);

	/** 
	 * Execute a JS function/method by name with FString Array as params.
	 * Each element in the array will be passed into the function in order and separated by a ,
	 * If you want to pass a JSON string as an object, simply don't put quotes around the outside braces {"foo" : "bar"}
	 * If you want to pass a number, do similar: 10.5
	 * To pass as a string, place quotes around the param when adding to the array: "10.5" and "hello" are strings
	 */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime", meta = (DisplayName = "Execute Javascript With Params", Keywords = "js javascript parameters"))
	void ExecuteJSMethodWithParams(const FString& methodName, const TArray<FString> params);

	/** Load a new URL into the browser */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void LoadURL(const FString& newURL);

	/** Get the currently loaded URL */
	UFUNCTION(BlueprintPure, Category = "SwuiRuntime")
	FString GetCurrentURL();

	/** Trigger Zoom */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void SetZoom(const float scale = 1);

	/** Get our zoom level */
	UFUNCTION(BlueprintPure, Category = "SwuiRuntime")
	float GetZoom();

	/** Download a file */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void DownloadFile(const FString& fileUrl);

	/** Trigger a LEFT click in the browser via a Vector2D */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void TriggerLeftClick(const FVector2D& pos, const float scale = 1);

	/** Trigger a RIGHT click in the browser via a Vector2D */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void TriggerRightClick(const FVector2D& pos, const float scale = 1);

	/** Trigger a LEFT MOUSE DOWN in the browser via a Vector2D */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void TriggerLeftMouseDown(const FVector2D& pos, const float scale = 1);

	/** Trigger a RIGHT MOUSE DOWN in the browser via a Vector2D */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void TriggerRightMouseDown(const FVector2D& pos, const float scale = 1);

	/** Trigger a LEFT MOUSE UP in the browser via a Vector2D */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void TriggerLeftMouseUp(const FVector2D& pos, const float scale = 1);

	/* Trigger a RIGHT MOUSE UP in the browser via a Vector2D */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void TriggerRightMouseUp(const FVector2D& pos, const float scale = 1);

	/** Move the mouse in the browser */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void TriggerMouseMove(const FVector2D& pos, const float scale = 1);

	/** Move the mouse in the browser */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void TriggerMouseWheel(const float MouseWheelDelta, const FVector2D& pos, const float scale = 1);

	/** Javascript event emitter */
	UPROPERTY(BlueprintAssignable)
	FScriptEvent ScriptEventEmitter;

	UPROPERTY(BlueprintAssignable)
	FLogEvent LogEventEmitter;

	/** Trigger a key down event */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void KeyDown(FKeyEvent InKey);

	/** Trigger a key up event */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void KeyUp(FKeyEvent InKey);

	/** Trigger a key press event */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void KeyPress(FKeyEvent InKey);

	/** Trigger a character key event as if typing input */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void CharKeyInput(FCharacterEvent CharEvent);

	/** Trigger a character key event as if pressing like a keyboard shortcut */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void CharKeyDownUp(FCharacterEvent CharEvent);

	/** Trigger a raw keypress via a character */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime", meta = (AdvancedDisplay = "2"))
	void RawCharKeyPress(const FString CharToPress, bool bIsRepeat,
								bool LeftShiftDown,
								bool RightShiftDown,
								bool LeftControlDown,
								bool RightControlDown,
								bool LeftAltDown,
								bool RightAltDown,
								bool LeftCommandDown,
								bool RightCommandDown,
								bool CapsLocksOn);

	/** Trigger a raw keypress via a character */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime", meta = (AdvancedDisplay = "2"))
	void RawCharKeyDownUp(const FString CharToPress, bool bIsRepeat,
		bool LeftShiftDown,
		bool RightShiftDown,
		bool LeftControlDown,
		bool RightControlDown,
		bool LeftAltDown,
		bool RightAltDown,
		bool LeftCommandDown,
		bool RightCommandDown,
		bool CapsLocksOn);

	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime", meta = (AdvancedDisplay = "2"))
	void SpecialKeyPress(ESwuiSpecialKeys key,
								bool LeftShiftDown,
								bool RightShiftDown,
								bool LeftControlDown,
								bool RightControlDown,
								bool LeftAltDown,
								bool RightAltDown,
								bool LeftCommandDown,
								bool RightCommandDown,
								bool CapsLocksOn);

	/** Close the browser */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void CloseBrowser();

	/** Check if the browser is still loading */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	bool IsBrowserLoading();

	/** Reloads the browser's current page */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void ReloadBrowser(bool IgnoreCache);

	/** Navigate back in this web view's history */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void NavBack();

	/** Navigate forward in this web view's history */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void NavForward();

	/** Resize the browser's viewport */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	UTexture2D* ResizeBrowser(const int32 NewWidth, const int32 NewHeight);

	//This cropping function doesn't work atm
	//UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	UTexture2D* CropWindow(const int32 Y, const int32 X, const int32 NewWidth, const int32 NewHeight);

	void TextureUpdate(const void* Buffer, FUpdateTextureRegion2D * UpdateRegions, uint32  RegionCount);

	void BeginDestroy() override;

	/** Use this to pause the tick loop in the new system */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	static void SetShouldTickEventLoop(bool ShouldTick = true);

protected:

	CefWindowInfo Info;
	CefRefPtr<BrowserClient> ClientHandler;
	CefBrowserSettings BrowserSettings;
	RenderHandler* Renderer;
	CefRefPtr<CefBrowser> Browser;
	
	CefMouseEvent MouseEvent;
	CefKeyEvent KeyEvent;

	void ResetTexture();
	void DestroyTexture();
	void ResetMatInstance();
		
	// Parse UE4 key events, helper
	void ProcessKeyCode(FKeyEvent InKey);

	// Helper for processing key modifiers
	void ProcessKeyMods(FInputEvent InKey);

	void SpawnTickEventLoopIfNeeded();

	static FTickEventLoopData EventLoopData;

	// Store UI state in this UTexture2D
	UPROPERTY()
	UTexture2D* Texture;

	UMaterialInstanceDynamic* MaterialInstance;

private:

	FSwuiTextureParams RenderParams;
	FThreadSafeBool bValidTexture;
};
