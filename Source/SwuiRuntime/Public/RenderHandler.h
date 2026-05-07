#pragma once

#include "CEFInclude.h"
#include "SwuiTypes.h"

class RenderHandler : public CefRenderHandler
{
	public:
		int32 Width;
		int32 Height;
		ISwuiRenderTarget* RenderTarget;

		virtual void GetViewRect(CefRefPtr<CefBrowser> Browser, CefRect &Rect) override;

		void OnPaint(CefRefPtr<CefBrowser> Browser, PaintElementType Type, const RectList &DirtyRects, const void *Buffer, int Width, int Height) override;

		RenderHandler(int32 Width, int32 Height, ISwuiRenderTarget* InRenderTarget);

	public:
		IMPLEMENT_REFCOUNTING(RenderHandler);
};

class BrowserClient : public CefClient, public CefLifeSpanHandler, public CefDownloadHandler, public CefDisplayHandler
{
	private:
		CefRefPtr<RenderHandler> RenderHandlerRef;

		CefRefPtr<CefBrowser> BrowserRef;
		int BrowserId;
		bool bIsClosing;

	public:
		BrowserClient(RenderHandler* InRenderHandler) : RenderHandlerRef(InRenderHandler) { };

		virtual CefRefPtr<CefRenderHandler> GetRenderHandler()
		{
			return RenderHandlerRef;
		};

		virtual CefRefPtr<RenderHandler> GetRenderHandlerCustom()
		{
			return RenderHandlerRef;
		};

		virtual CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override
		{
			return this;
		}

		virtual CefRefPtr<CefDownloadHandler> GetDownloadHandler() override
		{
			return this;
		}

		virtual CefRefPtr<CefDisplayHandler> GetDisplayHandler() override
		{
			return this;
		}

		virtual void OnUncaughtException(CefRefPtr<CefBrowser> Browser,
			CefRefPtr<CefFrame> Frame,
			CefRefPtr<CefV8Context> Context,
			CefRefPtr<CefV8Exception> Exception,
			CefRefPtr<CefV8StackTrace> StackTrace);

		virtual bool OnBeforeDownload(
			CefRefPtr<CefBrowser> Browser,
			CefRefPtr<CefDownloadItem> DownloadItem,
			const CefString& SuggestedName,
			CefRefPtr<CefBeforeDownloadCallback> Callback) override;

		virtual void OnDownloadUpdated(
			CefRefPtr<CefBrowser> Browser,
			CefRefPtr<CefDownloadItem> DownloadItem,
			CefRefPtr<CefDownloadItemCallback> Callback) override;

		virtual bool OnBeforePopup(CefRefPtr<CefBrowser> Browser,
			CefRefPtr<CefFrame> Frame,
			const CefString& TargetUrl,
			const CefString& TargetFrameName,
			WindowOpenDisposition TargetDisposition,
			bool UserGesture,
			const CefPopupFeatures& PopupFeatures,
			CefWindowInfo& WindowInfo,
			CefRefPtr<CefClient>& Client,
			CefBrowserSettings& Settings,
			CefRefPtr<CefDictionaryValue>& ExtraInfo,
			bool* NoJavascriptAccess) {
			return false;
		}

		void OnAfterCreated(CefRefPtr<CefBrowser> Browser) override;
		void OnBeforeClose(CefRefPtr<CefBrowser> Browser) override;

		virtual bool OnConsoleMessage(CefRefPtr<CefBrowser> Browser,
				cef_log_severity_t Level,
				const CefString& Message,
				const CefString& Source,
				int Line) override;

		virtual void OnFullscreenModeChange(CefRefPtr<CefBrowser> Browser, bool Fullscreen) override;

		virtual void OnTitleChange(CefRefPtr<CefBrowser> Browser, const CefString& Title);

		CefRefPtr<CefBrowser> GetCEFBrowser();

	public:
		IMPLEMENT_REFCOUNTING(BrowserClient);
};
