#include "ISwuiRuntime.h"
#include "Interfaces/IPluginManager.h"
#include "SwuiManager.h"

class FSwuiRuntime : public ISwuiRuntime
{

	/** IModuleInterface implementation */
	virtual void StartupModule() override
	{
		CefString GameDirCef = *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() + "SwuiCache");
		FString ExecutablePath = FPaths::ConvertRelativePathToFull(IPluginManager::Get().FindPlugin("SimpleWebUI")->GetBaseDir() + "/ThirdParty/cef/");

		// Setup the default settings for SwuiManager
		SwuiManager::Settings.windowless_rendering_enabled = true;
		SwuiManager::Settings.no_sandbox = true;
		SwuiManager::Settings.remote_debugging_port = 7777;
		SwuiManager::Settings.uncaught_exception_stack_size = 5;

	#if PLATFORM_LINUX
		ExecutablePath = "./swui_ue_process";
		if (!FPaths::FileExists(ExecutablePath))
		{
			ExecutablePath = "./blu_ue4_process";
		}
	#endif
	#if PLATFORM_MAC
		ExecutablePath += "Mac/shipping/swui_ue_process.app/Contents/MacOS/swui_ue_process";
		if (!FPaths::FileExists(ExecutablePath))
		{
			ExecutablePath = FPaths::ConvertRelativePathToFull(IPluginManager::Get().FindPlugin("SimpleWebUI")->GetBaseDir() + "/ThirdParty/cef/Mac/shipping/blu_ue4_process.app/Contents/MacOS/blu_ue4_process");
		}
	#endif
	#if PLATFORM_WINDOWS
		ExecutablePath += "Win/shipping/SwuiBrowserProcess.exe";
		if (!FPaths::FileExists(ExecutablePath))
		{
			ExecutablePath = FPaths::ConvertRelativePathToFull(IPluginManager::Get().FindPlugin("SimpleWebUI")->GetBaseDir() + "/ThirdParty/cef/Win/shipping/BluBrowserProcess.exe");
		}
	#endif

		CefString realExePath = *ExecutablePath;

		// Set the sub-process path
		CefString(&SwuiManager::Settings.browser_subprocess_path).FromString(realExePath);

		// Set the cache path
		CefString(&SwuiManager::Settings.cache_path).FromString(GameDirCef);

		// Make a new manager instance
		CefRefPtr<SwuiManager> SwuiApp = new SwuiManager();

		//CefExecuteProcess(SwuiManager::main_args, SwuiApp, NULL);
		CefInitialize(SwuiManager::MainArgs, SwuiManager::Settings, SwuiApp, NULL);

		UE_LOG(LogSwuiRuntime, Log, TEXT(" STATUS: Loaded"));
	}

	virtual void ShutdownModule() override
	{
		UE_LOG(LogSwuiRuntime, Log, TEXT(" STATUS: Shutdown"));
		//CefShutdown();
	}

};


IMPLEMENT_MODULE( FSwuiRuntime, SwuiRuntime )
DEFINE_LOG_CATEGORY(LogSwuiRuntime);