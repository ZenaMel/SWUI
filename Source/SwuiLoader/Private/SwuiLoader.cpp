#include "ISwuiLoader.h"
#include "Interfaces/IPluginManager.h"
#include "CoreMinimal.h"
#include "Misc/Paths.h"
#include <string>

#if PLATFORM_WINDOWS
#include "Windows/WindowsPlatformProcess.h"
#endif

class FSwuiLoader : public ISwuiLoader
{

	/** IModuleInterface implementation */
	virtual void StartupModule() override
	{
		FString LibPath = FPaths::ConvertRelativePathToFull(IPluginManager::Get().FindPlugin("SimpleWebUI")->GetBaseDir() + "/ThirdParty/cef/");

		// If we're on Windows we need to load DLLs from our custom path
		#if PLATFORM_WINDOWS
			LibPath += "Win/shipping/";
			FPlatformProcess::PushDllDirectory(*LibPath);
            UE_LOG(LogSwuiLoader, Log, TEXT("patched dll directory paths"));
		#endif
        
        #if PLATFORM_MAC
            // We need to load OUR CEF3 framework bundle here. It uses this identifier: org.chromium.ContentShell.SWUI.framework
            LibPath += "Mac/lib/Chromium Embedded Framework.framework/Chromium Embedded Framework";
            void* framework_hdl = dlopen(TCHAR_TO_ANSI(*LibPath), RTLD_NOW);
            UE_LOG(LogSwuiLoader, Log, TEXT("dlopen has loaded CEF framework"));
        #endif

		UE_LOG(LogSwuiLoader, Log, TEXT("STATUS: SWUI Ready to Load!"));
	}

	virtual void ShutdownModule() override
	{
		UE_LOG(LogSwuiLoader, Log, TEXT("STATUS: SWUI Has Shutdown"));
	}

};

IMPLEMENT_MODULE( FSwuiLoader, SwuiLoader )
DEFINE_LOG_CATEGORY(LogSwuiLoader);