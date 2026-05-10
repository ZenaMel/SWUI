#include "SwuiManager.h"
#include "SwuiSettings.h"
#include "include/wrapper/cef_message_router.h"

SwuiManager::SwuiManager()
{
	const CefMessageRouterConfig RouterConfig;
	MessageRouter = CefMessageRouterRendererSide::Create(RouterConfig);
}

void SwuiManager::OnBeforeCommandLineProcessing(const CefString& process_type,
	CefRefPtr< CefCommandLine > CommandLine)
{

	/////////////////
	/**
	* Used to pick command line switches
	* If set to "true": CEF will use less CPU, but rendering performance will be lower. CSS3 and WebGL are not be usable
	* If set to "false": CEF will use more CPU, but rendering will be better, CSS3 and WebGL will also be usable
	*/
	SwuiManager::CPURenderSettings = false;
	/////////////////

	CommandLine->AppendSwitch("off-screen-rendering-enabled");

	// Required for OSR / external begin-frame scheduling.
	// Keep this outside CPURenderSettings so HUD/external begin-frame mode can work
	// while still using GPU/WebGL-capable rendering.
	CommandLine->AppendSwitch("enable-begin-frame-scheduling");

	// Prevent Chromium from throttling the offscreen/windowless browser as if it
	// were a hidden/background page. This is important for responsive HUD usage.
	CommandLine->AppendSwitch("disable-background-timer-throttling");
	CommandLine->AppendSwitch("disable-renderer-backgrounding");
	CommandLine->AppendSwitch("disable-backgrounding-occluded-windows");
	CommandLine->AppendSwitchWithValue("disable-features", "CalculateNativeWinOcclusion");

	// Process-wide scheduler ceiling from Project Settings.
	// Per-browser SetWindowlessFrameRate() still takes precedence per view.
	const USwuiSettings* SwuiCfg = GetDefault<USwuiSettings>();
	const int32 OsrRate = (SwuiCfg && SwuiCfg->CefOffscreenFrameRate > 0) ? SwuiCfg->CefOffscreenFrameRate : 300;
	CommandLine->AppendSwitchWithValue("off-screen-frame-rate", TCHAR_TO_UTF8(*FString::FromInt(OsrRate)));
	CommandLine->AppendSwitch("enable-font-antialiasing");
	CommandLine->AppendSwitch("enable-media-stream");

	// Should we use the render settings that use less CPU?
	if (CPURenderSettings)
	{
		CommandLine->AppendSwitch("disable-gpu");
		CommandLine->AppendSwitch("disable-gpu-compositing");
	}
	else
	{
		// Enables things like CSS3 and WebGL
		CommandLine->AppendSwitch("enable-gpu-rasterization");
		CommandLine->AppendSwitch("enable-webgl");
		CommandLine->AppendSwitch("disable-web-security");
	}

	CommandLine->AppendSwitchWithValue("enable-blink-features", "HTMLImports");

	if (AutoPlay)
	{
		CommandLine->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
	}
	
	// Append more command line options here if you want
	// Visit Peter Beverloo's site: http://peter.sh/experiments/chromium-command-line-switches/ for more info on the switches

}

void SwuiManager::DoSwuiMessageLoop()
{
	CefDoMessageLoopWork();
}

CefRefPtr<CefRenderProcessHandler> SwuiManager::GetRenderProcessHandler()
{
	return this;
}

void SwuiManager::OnContextCreated(CefRefPtr<CefBrowser> Browser,
	CefRefPtr<CefFrame> Frame,
	CefRefPtr<CefV8Context> Context)
{
	if (MessageRouter)
	{
		MessageRouter->OnContextCreated(Browser, Frame, Context);
	}
}

void SwuiManager::OnContextReleased(CefRefPtr<CefBrowser> Browser,
	CefRefPtr<CefFrame> Frame,
	CefRefPtr<CefV8Context> Context)
{
	if (MessageRouter)
	{
		MessageRouter->OnContextReleased(Browser, Frame, Context);
	}
}

bool SwuiManager::OnProcessMessageReceived(CefRefPtr<CefBrowser> Browser,
	CefRefPtr<CefFrame> Frame,
	CefProcessId SourceProcess,
	CefRefPtr<CefProcessMessage> Message)
{
	if (MessageRouter)
	{
		return MessageRouter->OnProcessMessageReceived(Browser, Frame, SourceProcess, Message);
	}

	return false;
}

CefSettings SwuiManager::Settings;
CefMainArgs SwuiManager::MainArgs;
bool SwuiManager::CPURenderSettings = false;
bool SwuiManager::AutoPlay = true;