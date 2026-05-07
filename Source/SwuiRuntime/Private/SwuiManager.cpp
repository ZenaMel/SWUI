#include "SwuiManager.h"

SwuiManager::SwuiManager()
{
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
	CommandLine->AppendSwitchWithValue("off-screen-frame-rate", "60");
	CommandLine->AppendSwitch("enable-font-antialiasing");
	CommandLine->AppendSwitch("enable-media-stream");

	// Should we use the render settings that use less CPU?
	if (CPURenderSettings)
	{
		CommandLine->AppendSwitch("disable-gpu");
		CommandLine->AppendSwitch("disable-gpu-compositing");
		CommandLine->AppendSwitch("enable-begin-frame-scheduling");
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

CefSettings SwuiManager::Settings;
CefMainArgs SwuiManager::MainArgs;
bool SwuiManager::CPURenderSettings = false;
bool SwuiManager::AutoPlay = true;