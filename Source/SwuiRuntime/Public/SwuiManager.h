#pragma once

#include "CEFInclude.h"

#include "include/wrapper/cef_message_router.h"

class SWUIRUNTIME_API SwuiManager : public CefApp, public CefRenderProcessHandler
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
	virtual CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override;
	virtual void OnContextCreated(CefRefPtr<CefBrowser> Browser,
		CefRefPtr<CefFrame> Frame,
		CefRefPtr<CefV8Context> Context) override;
	virtual void OnContextReleased(CefRefPtr<CefBrowser> Browser,
		CefRefPtr<CefFrame> Frame,
		CefRefPtr<CefV8Context> Context) override;
	virtual bool OnProcessMessageReceived(CefRefPtr<CefBrowser> Browser,
		CefRefPtr<CefFrame> Frame,
		CefProcessId SourceProcess,
		CefRefPtr<CefProcessMessage> Message) override;

	IMPLEMENT_REFCOUNTING(SwuiManager);

private:
	CefRefPtr<CefMessageRouterRendererSide> MessageRouter;
};

