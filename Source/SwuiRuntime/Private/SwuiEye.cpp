
#include "SwuiEye.h"
#include "ISwuiRuntime.h"
#include "RenderHandler.h"

FTickEventLoopData USwuiEye::EventLoopData = FTickEventLoopData();

FSwuiEyeSettings::FSwuiEyeSettings()
{
	FrameRate = 60.f;

	ViewSize.X = 1280;
	ViewSize.Y = 720;

	bIsTransparent = false;
	bEnableWebGL = true;
	bAudioMuted = false;
	bAutoPlayEnabled = true;
	bDebugLogTick = false;
}

USwuiEye::USwuiEye(const class FObjectInitializer& PCIP)
	: Super(PCIP)
{
	Texture = nullptr;
	bValidTexture = false;
}

void USwuiEye::Init()
{

	/** 
	 * We don't want this running in editor unless it's PIE
	 * If we don't check this, CEF will spawn infinite processes with widget components
	 **/

	if (GEngine)
	{
		if (GEngine->IsEditor() && !GWorld->IsPlayInEditor())
		{
			UE_LOG(LogSwuiRuntime, Log, TEXT("Notice: not playing - Component Will Not Initialize"));
			return;
		}
	}
	
	if (Settings.ViewSize.X <= 0 || Settings.ViewSize.Y <= 0)
	{
		UE_LOG(LogSwuiRuntime, Log, TEXT("Can't initialize when Width or Height are <= 0"));
		return;
	}

	//These no longer exist
	//BrowserSettings.universal_access_from_file_urls = STATE_ENABLED;
	//BrowserSettings.file_access_from_file_urls = STATE_ENABLED;

	//BrowserSettings.web_security = STATE_DISABLED;
	//BrowserSettings.fullscreen_enabled = true;

	// Set transparant option
	Info.SetAsWindowless(0); //bIsTransparent

	// Figure out if we want to turn on WebGL support
	if (Settings.bEnableWebGL)
	{
		if (SwuiManager::CPURenderSettings)
		{
			UE_LOG(LogSwuiRuntime, Error, TEXT("You have enabled WebGL for this browser, but CPU Saver is enabled in SwuiManager.cpp - WebGL will not work!"));
		}
		BrowserSettings.webgl = STATE_ENABLED;
	}

	//NB: this setting will change it globally for all new instances
	SwuiManager::AutoPlay = Settings.bAutoPlayEnabled;

	Renderer = new RenderHandler(Settings.ViewSize.X, Settings.ViewSize.Y, this);
	ClientHandler = new BrowserClient(Renderer);

	// Setup JS event emitter
	ClientHandler->SetEventEmitter(&ScriptEventEmitter);
	ClientHandler->SetLogEmitter(&LogEventEmitter);

	Browser = CefBrowserHost::CreateBrowserSync(
		Info,
		ClientHandler.get(),
		"about:blank",
		BrowserSettings,
		nullptr,
		nullptr);


	Browser->GetHost()->SetWindowlessFrameRate(Settings.FrameRate);
	Browser->GetHost()->SetAudioMuted(Settings.bAudioMuted);

	UE_LOG(LogSwuiRuntime, Log, TEXT("Component Initialized"));
	UE_LOG(LogSwuiRuntime, Log, TEXT("Loading URL: %s"), *DefaultURL);

	// Load the default URL
	LoadURL(DefaultURL);
	ResetTexture();

	//Instead of manually ticking, we now tick whenever one Swui eye is created
	SpawnTickEventLoopIfNeeded();
}

void USwuiEye::ResetTexture()
{

	// Here we init the texture to its initial state
	DestroyTexture();

	bValidTexture = false;
	Texture = nullptr;
	

	// init the new Texture2D
	Texture = UTexture2D::CreateTransient(Settings.ViewSize.X, Settings.ViewSize.Y, PF_B8G8R8A8);
	Texture->AddToRoot();
	Texture->UpdateResource();

	//RenderParams.Texture2DResource = (FTexture2DResource*)Texture->GetResource();

	ResetMatInstance();

	bValidTexture = true;
}

void USwuiEye::DestroyTexture()
{
	// Here we destroy the texture and its resource
	if (Texture)
	{
		Texture->RemoveFromRoot();

		FTextureResource* Resource = Texture->GetResource();

		if (Resource)
		{
			BeginReleaseResource(Resource);
			Texture->UpdateResource();

			//NB: these lines are the problem for 5.4 causes deadlock on game thread on exit
			//StartBatchedRelease();
			//BeginReleaseResource(Texture->GetResource());	// (FRenderCommandPipe*) &UE::RenderCommandPipe::GetPipes()[0]
			//EndBatchedRelease();
			//FlushRenderingCommands();

			/* This is what that command does...
			ENQUEUE_RENDER_COMMAND(UpdateSWUICommand)(
			[Resource](FRHICommandList& CommandList)
			{
				Resource->ReleaseResource();
			});*/
		}
		Resource = nullptr;

		Texture->MarkAsGarbage();
		Texture = nullptr;
	}
	bValidTexture = false;
}

void USwuiEye::TextureUpdate(const void *Buffer, FUpdateTextureRegion2D *UpdateRegions, uint32  RegionCount)
{
	if (!Browser || !bEnabled)
	{
		UE_LOG(LogSwuiRuntime, Warning, TEXT("No Browser access or SwuiEye not Enabled"))
		return;
	}

	if (bValidTexture && Texture->IsValidLowLevelFast())
	{
		if (Buffer == nullptr)
		{
			UE_LOG(LogSwuiRuntime, Warning, TEXT("No Texture Data Buffer"))
			return;
		}
	 
		FUpdateTextureRegionsData* RegionData = new FUpdateTextureRegionsData;
		RegionData->Texture2DResource = (FTextureResource*)Texture->GetResource();
		RegionData->NumRegions = RegionCount;
		RegionData->SrcBpp = 4;
		RegionData->SrcPitch = int32(Settings.ViewSize.X) * 4;
		RegionData->Regions = UpdateRegions;

		//We need to copy this memory or it might get uninitialized
		RegionData->SrcData.SetNumUninitialized(RegionData->SrcPitch * int32(Settings.ViewSize.Y));
		FPlatformMemory::Memcpy(RegionData->SrcData.GetData(), Buffer, RegionData->SrcData.Num());

		ENQUEUE_RENDER_COMMAND(UpdateSWUICommand)(
			[RegionData](FRHICommandList& CommandList)
			{
				for (uint32 RegionIndex = 0; RegionIndex < RegionData->NumRegions; RegionIndex++)
				{
					RHIUpdateTexture2D(RegionData->Texture2DResource->TextureRHI->GetTexture2D(), 0, RegionData->Regions[RegionIndex], RegionData->SrcPitch, RegionData->SrcData.GetData());
				}

				FMemory::Free(RegionData->Regions);
				delete RegionData;
			});
	}
	else 
	{
		UE_LOG(LogSwuiRuntime, Warning, TEXT("No Texture or Texture->GetResource()"))
	}
}

void USwuiEye::ExecuteJS(const FString& Code)
{
	CefString CodeStr = *Code;
	Browser->GetMainFrame()->ExecuteJavaScript(CodeStr, "", 0);
}

void USwuiEye::ExecuteJSMethodWithParams(const FString& methodName, const TArray<FString> params)
{

	// Empty param string
	FString ParamString = "(";

	// Build the param string
	for (FString param : params)
	{
		ParamString += param;
		ParamString += ",";
	}
		
	// Remove the last , it's not needed
	ParamString.RemoveFromEnd(",");
	ParamString += ");";

	// time to call the function
	ExecuteJS(methodName + ParamString);
}

void USwuiEye::LoadURL(const FString& newURL)
{
	FString FinalUrl = newURL;

	//Detect chrome-devtools, and re-target them to regular devtools
	if (newURL.Contains(TEXT("chrome-devtools://devtools")))
	{
		//devtools://devtools/inspector.html?v8only=true&ws=localhost:9229
		//browser->GetHost()->ShowDevTools(info, g_handler, browserSettings, CefPoint());
		FinalUrl = FinalUrl.Replace(TEXT("chrome-devtools://devtools/bundled/inspector.html"), TEXT("devtools://devtools/inspector.html"));
	}

	// Check if we want to load a local file
	if (newURL.Contains(TEXT("swui://"), ESearchCase::IgnoreCase, ESearchDir::FromStart))
	{

		// Get the current working directory
		FString GameDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

		// We're loading a local file, so replace the proto with our game directory path
		FString LocalFile = newURL.Replace(TEXT("swui://"), *GameDir, ESearchCase::IgnoreCase);

		// Now we use the file proto
		LocalFile = FString(TEXT("file:///")) + LocalFile;

		UE_LOG(LogSwuiRuntime, Log, TEXT("Load Local File: %s"), *LocalFile)

		// Load it up 
		Browser->GetMainFrame()->LoadURL(*LocalFile);

		return;

	}

	// Load as usual
	Browser->GetMainFrame()->LoadURL(*FinalUrl);

}

FString USwuiEye::GetCurrentURL()
{
	return FString(Browser->GetMainFrame()->GetURL().ToWString().c_str());
}

void USwuiEye::SetZoom(const float Scale /*= 1*/)
{
	Browser->GetHost()->SetZoomLevel(Scale);
}

float USwuiEye::GetZoom()
{
	return Browser->GetHost()->GetZoomLevel();
}

void USwuiEye::DownloadFile(const FString& FileUrl)
{
	Browser->GetHost()->StartDownload(*FileUrl);
	//Todo: ensure downloading works in some way, shape or form?
}

bool USwuiEye::IsBrowserLoading()
{
	return Browser->IsLoading();
}

void USwuiEye::ReloadBrowser(bool IgnoreCache)
{

	if (IgnoreCache)
	{
		return Browser->ReloadIgnoreCache();
	}

	Browser->Reload();

}

void USwuiEye::NavBack()
{

	if (Browser->CanGoBack())
	{
		Browser->GoBack();
	}

}

void USwuiEye::NavForward()
{

	if (Browser->CanGoForward())
	{
		Browser->GoForward();
	}

}

UTexture2D* USwuiEye::ResizeBrowser(const int32 NewWidth, const int32 NewHeight)
{

	if (NewWidth <= 0 || NewHeight <= 0)
	{
		// We can't do this, just do nothing.
		UE_LOG(LogSwuiRuntime, Log, TEXT("Can't resize when one or both of the sizes are <= 0!"));
		return Texture;
	}

	// Disable the web view while we resize
	bEnabled = false;

	// Set our new Width and Height
	Settings.ViewSize.X = NewWidth;
	Settings.ViewSize.Y = NewHeight;
	
	// Update our render handler
	Renderer->Width = NewWidth;
	Renderer->Height = NewHeight;

	bValidTexture = false;

	Texture = UTexture2D::CreateTransient(Settings.ViewSize.X, Settings.ViewSize.Y, PF_B8G8R8A8);
	Texture->AddToRoot();
	Texture->UpdateResource();

	bValidTexture = true;

	// Let the browser's host know we resized it
	Browser->GetHost()->WasResized();

	// Now we can keep going
	bEnabled = true;

	UE_LOG(LogSwuiRuntime, Log, TEXT("SwuiEye was resized!"))

	return Texture;

}

UTexture2D* USwuiEye::CropWindow(const int32 Y, const int32 X, const int32 NewWidth, const int32 NewHeight)
{
	// Disable the web view while we resize
	bEnabled = false;


	// Set our new Width and Height
	Settings.ViewSize.X = NewWidth;
	Settings.ViewSize.Y = NewHeight;

	// Update our render handler
	Renderer->Width = NewWidth;
	Renderer->Height = NewHeight;

	bValidTexture = false;

	Texture = UTexture2D::CreateTransient(Settings.ViewSize.X, Settings.ViewSize.Y, PF_B8G8R8A8);
	Texture->AddToRoot();
	Texture->UpdateResource();

	bValidTexture = true;

	// Now we can keep going
	bEnabled = true;

	UE_LOG(LogSwuiRuntime, Log, TEXT("SwuiEye was cropped!"))

	return Texture;
}

USwuiEye* USwuiEye::SetProperties(const int32 SetWidth,
	const int32 SetHeight,
	const bool SetIsTransparent,
	const bool SetEnabled,
	const bool SetWebGL,
	const FString& SetDefaultURL,
	const FName& SetTextureParameterName,
	UMaterialInterface* SetBaseMaterial)
{
	Settings.ViewSize.X = SetWidth;
	Settings.ViewSize.Y = SetHeight;

	bEnabled = SetEnabled;

	Settings.bIsTransparent = SetIsTransparent;
	Settings.bEnableWebGL = SetWebGL;
	BaseMaterial = SetBaseMaterial;

	DefaultURL = SetDefaultURL;
	TextureParameterName = SetTextureParameterName;

	return this;
}

void USwuiEye::TriggerMouseMove(const FVector2D& Pos, const float Scale)
{

	MouseEvent.x = Pos.X / Scale;
	MouseEvent.y = Pos.Y / Scale;

	Browser->GetHost()->SetFocus(true);
	Browser->GetHost()->SendMouseMoveEvent(MouseEvent, false);

}

void USwuiEye::TriggerLeftClick(const FVector2D& Pos, const float Scale)
{
	TriggerLeftMouseDown(Pos, Scale);
	TriggerLeftMouseUp(Pos, Scale);
}

void USwuiEye::TriggerRightClick(const FVector2D& Pos, const float Scale)
{
	TriggerRightMouseDown(Pos, Scale);
	TriggerRightMouseUp(Pos, Scale);
}

void USwuiEye::TriggerLeftMouseDown(const FVector2D& Pos, const float Scale)
{
	MouseEvent.x = Pos.X / Scale;
	MouseEvent.y = Pos.Y / Scale;

	Browser->GetHost()->SendMouseClickEvent(MouseEvent, MBT_LEFT, false, 1);
}

void USwuiEye::TriggerRightMouseDown(const FVector2D& Pos, const float Scale)
{
	MouseEvent.x = Pos.X / Scale;
	MouseEvent.y = Pos.Y / Scale;

	Browser->GetHost()->SendMouseClickEvent(MouseEvent, MBT_RIGHT, false, 1);
}

void USwuiEye::TriggerLeftMouseUp(const FVector2D& Pos, const float Scale)
{
	MouseEvent.x = Pos.X / Scale;
	MouseEvent.y = Pos.Y / Scale;

	Browser->GetHost()->SendMouseClickEvent(MouseEvent, MBT_LEFT, true, 1);
}

void USwuiEye::TriggerRightMouseUp(const FVector2D& Pos, const float Scale)
{
	MouseEvent.x = Pos.X / Scale;
	MouseEvent.y = Pos.Y / Scale;

	Browser->GetHost()->SendMouseClickEvent(MouseEvent, MBT_RIGHT, true, 1);
}

void USwuiEye::TriggerMouseWheel(const float MouseWheelDelta, const FVector2D& Pos, const float Scale)
{
	MouseEvent.x = Pos.X / Scale;
	MouseEvent.y = Pos.Y / Scale;

	Browser->GetHost()->SendMouseWheelEvent(MouseEvent, MouseWheelDelta * 10, MouseWheelDelta * 10);
}

void USwuiEye::KeyDown(FKeyEvent InKey)
{

	ProcessKeyMods(InKey);
	ProcessKeyCode(InKey);

	KeyEvent.type = KEYEVENT_KEYDOWN;
	Browser->GetHost()->SendKeyEvent(KeyEvent);

}

void USwuiEye::KeyUp(FKeyEvent InKey)
{

	ProcessKeyMods(InKey);
	ProcessKeyCode(InKey);

	KeyEvent.type = KEYEVENT_KEYUP;
	Browser->GetHost()->SendKeyEvent(KeyEvent);

}

void USwuiEye::KeyPress(FKeyEvent InKey)
{

	// Simply trigger down, then up key events
	KeyDown(InKey);
	KeyUp(InKey);

}

void USwuiEye::ProcessKeyCode(FKeyEvent InKey)
{
	KeyEvent.native_key_code = InKey.GetKeyCode();
	KeyEvent.windows_key_code = InKey.GetKeyCode();
}

void USwuiEye::CharKeyInput(FCharacterEvent CharEvent)
{

	// Process keymods like usual
	ProcessKeyMods(CharEvent);

	// Below char input needs some special treatment, se we can't use the normal key down/up methods

#if PLATFORM_MAC
	KeyEvent.character = CharEvent.GetCharacter();
#else
    KeyEvent.windows_key_code = CharEvent.GetCharacter();
    KeyEvent.native_key_code = CharEvent.GetCharacter();
#endif
	KeyEvent.type = KEYEVENT_CHAR;
	Browser->GetHost()->SetFocus(true);
	Browser->GetHost()->SendKeyEvent(KeyEvent);
}

void USwuiEye::CharKeyDownUp(FCharacterEvent CharEvent)
{
	// Process keymods like usual
	ProcessKeyMods(CharEvent);

	// Below char input needs some special treatment, se we can't use the normal key down/up methods

#if PLATFORM_MAC
	KeyEvent.character = CharEvent.GetCharacter();
#else
	KeyEvent.windows_key_code = CharEvent.GetCharacter();
	KeyEvent.native_key_code = CharEvent.GetCharacter();
#endif
	KeyEvent.type = KEYEVENT_KEYDOWN;
	Browser->GetHost()->SendKeyEvent(KeyEvent);

	KeyEvent.type = KEYEVENT_KEYUP;
	Browser->GetHost()->SendKeyEvent(KeyEvent);
}

void USwuiEye::RawCharKeyPress(const FString CharToPress, bool bIsRepeat,
	bool LeftShiftDown,
	bool RightShiftDown,
	bool LeftControlDown,
	bool RightControlDown,
	bool LeftAltDown,
	bool RightAltDown,
	bool LeftCommandDown,
	bool RightCommandDown,
	bool CapsLocksOn)
{

	FModifierKeysState* KeyState = new FModifierKeysState(LeftShiftDown, RightShiftDown, LeftControlDown, 
		RightControlDown, LeftAltDown, RightAltDown, LeftCommandDown, RightCommandDown, CapsLocksOn);

	FCharacterEvent* CharEvent = new FCharacterEvent(CharToPress.GetCharArray()[0], *KeyState, 0, bIsRepeat);

	CharKeyInput(*CharEvent);

}

void USwuiEye::RawCharKeyDownUp(const FString CharToPress, bool bIsRepeat, bool LeftShiftDown, bool RightShiftDown, bool LeftControlDown, bool RightControlDown, bool LeftAltDown, bool RightAltDown, bool LeftCommandDown, bool RightCommandDown, bool CapsLocksOn)
{
	FModifierKeysState* KeyState = new FModifierKeysState(LeftShiftDown, RightShiftDown, LeftControlDown,
		RightControlDown, LeftAltDown, RightAltDown, LeftCommandDown, RightCommandDown, CapsLocksOn);

	FCharacterEvent* CharEvent = new FCharacterEvent(CharToPress.GetCharArray()[0], *KeyState, 0, bIsRepeat);

	CharKeyDownUp(*CharEvent);
}

void USwuiEye::SpecialKeyPress(ESwuiSpecialKeys Key, bool LeftShiftDown,
	bool RightShiftDown,
	bool LeftControlDown,
	bool RightControlDown,
	bool LeftAltDown,
	bool RightAltDown,
	bool LeftCommandDown,
	bool RightCommandDown,
	bool CapsLocksOn)
{

	int32 KeyValue = Key;

	KeyEvent.windows_key_code = KeyValue;
	KeyEvent.native_key_code = KeyValue;
	KeyEvent.type = KEYEVENT_KEYDOWN;
	Browser->GetHost()->SendKeyEvent(KeyEvent);

	KeyEvent.windows_key_code = KeyValue;
	KeyEvent.native_key_code = KeyValue;
	// bits 30 and 31 should be always 1 for WM_KEYUP
	KeyEvent.type = KEYEVENT_KEYUP;
	Browser->GetHost()->SendKeyEvent(KeyEvent);

}

void USwuiEye::ProcessKeyMods(FInputEvent InKey)
{

	int Mods = 0;

	// Test alt
	if (InKey.IsAltDown())
	{
		Mods |= cef_event_flags_t::EVENTFLAG_ALT_DOWN;
	}
	else
	// Test control
	if (InKey.IsControlDown())
	{
		Mods |= cef_event_flags_t::EVENTFLAG_CONTROL_DOWN;
	} 
	else
	// Test shift
	if (InKey.IsShiftDown())
	{
		Mods |= cef_event_flags_t::EVENTFLAG_SHIFT_DOWN;
	}

	KeyEvent.modifiers = Mods;

}

void USwuiEye::SpawnTickEventLoopIfNeeded()
{
	if (!EventLoopData.DelegateHandle.IsValid())
	{
		EventLoopData.DelegateHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([&](float DeltaTime)
		{
			if (EventLoopData.bShouldTickEventLoop)
			{
				if (Settings.bDebugLogTick)
				{
					UE_LOG(LogTemp, Log, TEXT("Delta: %1.2f"), DeltaTime);
				}

				//NB: this wrapper doesn't crash, but will fail to render
				//Async(EAsyncExecution::ThreadPool, [this] 
				//{
					SwuiManager::DoSwuiMessageLoop();
				//});
				
			}
			
			return true;
		}));
	}

	EventLoopData.EyeCount++;
}

UTexture2D* USwuiEye::GetTexture() const
{
	if (!Texture)
	{
		return UTexture2D::CreateTransient(Settings.ViewSize.X, Settings.ViewSize.Y, PF_B8G8R8A8);
	}

	return Texture;
}

void USwuiEye::ResetMatInstance()
{
	if (!Texture || !BaseMaterial || TextureParameterName.IsNone())
	{
		return;
	}

	// Create material instance
	if (!MaterialInstance)
	{
		MaterialInstance = UMaterialInstanceDynamic::Create(BaseMaterial, NULL);
		if (!MaterialInstance)
		{
			UE_LOG(LogSwuiRuntime, Warning, TEXT("UI Material instance can't be created"));
			return;
		}
	}

	// Check again, we must have material instance
	if (!MaterialInstance)
	{
		UE_LOG(LogSwuiRuntime, Error, TEXT("UI Material instance wasn't created"));
		return;
	}

	// Check we have desired parameter
	UTexture* Tex = nullptr;
	if (!MaterialInstance->GetTextureParameterValue(TextureParameterName, Tex))
	{
		UE_LOG(LogSwuiRuntime, Warning, TEXT("UI Material instance Texture parameter not found"));
		return;
	}

	MaterialInstance->SetTextureParameterValue(TextureParameterName, Texture);
}

void USwuiEye::CloseBrowser()
{
	BeginDestroy();

	/*if (Browser)
	{
		// Close up the browser
		Browser->GetHost()->SetAudioMuted(true);
		Browser->GetMainFrame()->LoadURL("about:blank");
		//browser->GetMainFrame()->Delete();
		Browser->GetHost()->CloseDevTools();
		Browser->GetHost()->CloseBrowser(true);
		Browser = nullptr;

		UE_LOG(LogSwuiRuntime, Warning, TEXT("Browser Closing"));
	}

	DestroyTexture();

	//Remove our auto-ticking setup
	EventLoopData.EyeCount--;
	if (EventLoopData.EyeCount <= 0)
	{
		FTSTicker::GetCoreTicker().RemoveTicker(EventLoopData.DelegateHandle);
		EventLoopData.DelegateHandle = FTSTicker::FDelegateHandle();
	}*/
}

void USwuiEye::BeginDestroy()
{
	if (Browser)
	{
		// Close up the browser
		Browser->GetHost()->SetAudioMuted(true);
		Browser->GetMainFrame()->LoadURL("about:blank");
		//Browser->GetMainFrame()->Delete();
		Browser->GetHost()->CloseDevTools();
		Browser->GetHost()->CloseBrowser(true);
		Browser = nullptr;

		UE_LOG(LogSwuiRuntime, Warning, TEXT("Browser Closing"));
	}

	DestroyTexture();
	SetFlags(RF_BeginDestroyed);

	//Remove our auto-ticking setup
	EventLoopData.EyeCount--;
	if (EventLoopData.EyeCount <= 0)
	{
		FTSTicker::GetCoreTicker().RemoveTicker(EventLoopData.DelegateHandle);
		EventLoopData.DelegateHandle = FTSTicker::FDelegateHandle();
	}
	Super::BeginDestroy();
}

void USwuiEye::SetShouldTickEventLoop(bool ShouldTick /*= true*/)
{
	EventLoopData.bShouldTickEventLoop = ShouldTick;
}
