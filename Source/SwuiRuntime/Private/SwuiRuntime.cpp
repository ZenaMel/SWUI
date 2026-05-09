#include "ISwuiRuntime.h"
#include "Interfaces/IPluginManager.h"
#include "SwuiManager.h"
#include "SwuiSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/PreWindowsApi.h"
#include <windows.h>
#include "Windows/PostWindowsApi.h"
#include "Windows/HideWindowsPlatformTypes.h"
#endif

static void SwuiSetCefString(cef_string_t& Target, const FString& Value)
{
	const FTCHARToUTF8 Utf8Value(*Value);
	CefString(&Target).FromString(Utf8Value.Get(), Utf8Value.Length());
}

class FSwuiRuntime : public ISwuiRuntime
{
	/** IModuleInterface implementation */
	virtual void StartupModule() override
	{
		if (GetDefault<USwuiSettings>()->bDisablePlugin)
		{
			UE_LOG(LogSwuiRuntime, Log, TEXT(" STATUS: Disabled via Project Settings > Plugins > SimpleWebUI > Debug"));
			return;
		}

		TSharedPtr<IPlugin> SimpleWebUIPlugin = IPluginManager::Get().FindPlugin(TEXT("SimpleWebUI"));
		if (!SimpleWebUIPlugin.IsValid())
		{
			UE_LOG(LogSwuiRuntime, Error, TEXT("SWUI CEF: SimpleWebUI plugin not found."));
			return;
		}

		const FString PluginBaseDir = SimpleWebUIPlugin->GetBaseDir();
		FString ExecutablePath;
		FString CefRoot;
		FString ResourcesPath;
		FString LocalesPath;

		SwuiManager::Settings.windowless_rendering_enabled = 1;
		SwuiManager::Settings.no_sandbox = 1;
		SwuiManager::Settings.remote_debugging_port = 7777;
		SwuiManager::Settings.uncaught_exception_stack_size = 5;
		SwuiManager::Settings.command_line_args_disabled = 0;

#if PLATFORM_LINUX
		CefRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(PluginBaseDir, TEXT("ThirdParty/cef/Linux/shipping")));
		ExecutablePath = FPaths::Combine(CefRoot, TEXT("swui_ue_process"));
		if (!FPaths::FileExists(ExecutablePath))
		{
			ExecutablePath = FPaths::Combine(CefRoot, TEXT("blu_ue4_process"));
		}
		ResourcesPath = CefRoot;
		LocalesPath = FPaths::Combine(CefRoot, TEXT("locales"));
#elif PLATFORM_MAC
		CefRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(PluginBaseDir, TEXT("ThirdParty/cef/Mac/shipping")));
		ExecutablePath = FPaths::Combine(CefRoot, TEXT("swui_ue_process.app/Contents/MacOS/swui_ue_process"));
		if (!FPaths::FileExists(ExecutablePath))
		{
			ExecutablePath = FPaths::Combine(CefRoot, TEXT("blu_ue4_process.app/Contents/MacOS/blu_ue4_process"));
		}
		ResourcesPath = CefRoot;
		LocalesPath = FPaths::Combine(CefRoot, TEXT("locales"));
#elif PLATFORM_WINDOWS
		CefRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(PluginBaseDir, TEXT("ThirdParty/cef/Win/shipping")));
		ExecutablePath = FPaths::Combine(CefRoot, TEXT("SwuiBrowserProcess.exe"));
		if (!FPaths::FileExists(ExecutablePath))
		{
			ExecutablePath = FPaths::Combine(CefRoot, TEXT("BluBrowserProcess.exe"));
		}
		ResourcesPath = CefRoot;
		LocalesPath = FPaths::Combine(CefRoot, TEXT("locales"));
#else
		UE_LOG(LogSwuiRuntime, Error, TEXT("SWUI CEF: Unsupported platform."));
		return;
#endif

		const FString RootCachePath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("SWUI_BundledCEF")));

		const FString CachePath = FPaths::Combine(RootCachePath, TEXT("Default"));

		const FString CefLogPath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs/SwuiCefDebug.log")));

		IFileManager::Get().MakeDirectory(*RootCachePath, true);
		IFileManager::Get().MakeDirectory(*CachePath, true);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(CefLogPath), true);

		SwuiSetCefString(SwuiManager::Settings.browser_subprocess_path, ExecutablePath);
		SwuiSetCefString(SwuiManager::Settings.resources_dir_path, ResourcesPath);
		SwuiSetCefString(SwuiManager::Settings.locales_dir_path, LocalesPath);
		SwuiSetCefString(SwuiManager::Settings.root_cache_path, RootCachePath);
		SwuiSetCefString(SwuiManager::Settings.cache_path, CachePath);
		SwuiSetCefString(SwuiManager::Settings.log_file, CefLogPath);
		SwuiManager::Settings.log_severity = LOGSEVERITY_VERBOSE;

		UE_LOG(LogSwuiRuntime, Log, TEXT("SWUI CEF root: %s exists=%s"), *CefRoot, FPaths::DirectoryExists(CefRoot) ? TEXT("true") : TEXT("false"));
		UE_LOG(LogSwuiRuntime, Log, TEXT("SWUI CEF browser_subprocess_path: %s exists=%s"), *ExecutablePath, FPaths::FileExists(ExecutablePath) ? TEXT("true") : TEXT("false"));
		UE_LOG(LogSwuiRuntime, Log, TEXT("SWUI CEF resources_dir_path: %s exists=%s"), *ResourcesPath, FPaths::DirectoryExists(ResourcesPath) ? TEXT("true") : TEXT("false"));
		UE_LOG(LogSwuiRuntime, Log, TEXT("SWUI CEF locales_dir_path: %s exists=%s"), *LocalesPath, FPaths::DirectoryExists(LocalesPath) ? TEXT("true") : TEXT("false"));
		UE_LOG(LogSwuiRuntime, Log, TEXT("SWUI CEF root_cache_path: %s exists=%s"), *RootCachePath, FPaths::DirectoryExists(RootCachePath) ? TEXT("true") : TEXT("false"));
		UE_LOG(LogSwuiRuntime, Log, TEXT("SWUI CEF cache_path: %s exists=%s"), *CachePath, FPaths::DirectoryExists(CachePath) ? TEXT("true") : TEXT("false"));
		UE_LOG(LogSwuiRuntime, Log, TEXT("SWUI CEF log_file: %s"), *CefLogPath);

		CefRefPtr<SwuiManager> SwuiApp = new SwuiManager();

#if PLATFORM_WINDOWS
		CefMainArgs MainArgs(GetModuleHandle(nullptr));
		const bool bCefInitialized = CefInitialize(MainArgs, SwuiManager::Settings, SwuiApp, nullptr);
#else
		const bool bCefInitialized = CefInitialize(SwuiManager::MainArgs, SwuiManager::Settings, SwuiApp, nullptr);
#endif

		UE_LOG(LogSwuiRuntime, Log, TEXT("SWUI bundled CefInitialize result: %s"), bCefInitialized ? TEXT("true") : TEXT("false"));
		UE_LOG(LogSwuiRuntime, Log, TEXT(" STATUS: Loaded"));
	}

	virtual void ShutdownModule() override
	{
		UE_LOG(LogSwuiRuntime, Log, TEXT(" STATUS: Shutdown"));
		// CefShutdown();
	}
};

IMPLEMENT_MODULE(FSwuiRuntime, SwuiRuntime)
DEFINE_LOG_CATEGORY(LogSwuiRuntime);