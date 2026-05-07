#pragma once

#include "CEFInclude.h"

class SWUIRUNTIME_API SwuiManager : public CefApp
{
public:

	SwuiManager();

	static void DoSwuiMessageLoop();
	static CefSettings Settings;
	static CefMainArgs MainArgs;
	static bool CPURenderSettings;
	static bool AutoPlay;

	virtual void OnBeforeCommandLineProcessing(const CefString& ProcessType,
			CefRefPtr< CefCommandLine > CommandLine) override;

	IMPLEMENT_REFCOUNTING(SwuiManager);
};

