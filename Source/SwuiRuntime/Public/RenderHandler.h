#pragma once

#include "CEFInclude.h"
#include "SwuiTypes.h"
#include "Templates\UniquePtr.h"

class CefMessageRouterBrowserSide;
class USwuiView;
class FSwuiBrowserQueryHandler;

// ---------------------------------------------------------------------------
// RenderHandler — CEF CefRenderHandler implementation.
//
// Thread ownership:
//   - All CEF paint callbacks (OnPaint, OnAcceleratedPaint) fire on the
//     CEF renderer thread.
//   - RenderTarget / AcceleratedTarget pointers are set once during Init()
//     on the game thread before the browser is created, then read-only on
//     the CEF thread.
// ---------------------------------------------------------------------------
class RenderHandler : public CefRenderHandler
{
	public:
		int32 Width;
		int32 Height;

		// CPU Compatible backend target (existing path).
		ISwuiRenderTarget* RenderTarget;

		// GPU Accelerated backend target (new path).
		ISwuiAcceleratedRenderTarget* AcceleratedTarget;

		// Which backend this handler is configured for.
		ESwuiRenderingMode ActiveMode;

		virtual void GetViewRect(CefRefPtr<CefBrowser> Browser, CefRect &Rect) override;

		// CPU Compatible path — called when shared_texture_enabled is false.
		void OnPaint(CefRefPtr<CefBrowser> Browser, PaintElementType Type, const RectList &DirtyRects, const void *Buffer, int Width, int Height) override;

		// GPU Accelerated path — called when shared_texture_enabled is true.
		void OnAcceleratedPaint(CefRefPtr<CefBrowser> Browser, PaintElementType Type, const RectList &DirtyRects, const CefAcceleratedPaintInfo& Info) override;

		RenderHandler(int32 Width, int32 Height, ISwuiRenderTarget* InRenderTarget,
			ISwuiAcceleratedRenderTarget* InAcceleratedTarget = nullptr,
			ESwuiRenderingMode InMode = ESwuiRenderingMode::CpuCompatible);

	public:
		IMPLEMENT_REFCOUNTING(RenderHandler);
};

class BrowserClient : public CefClient, public CefLifeSpanHandler, public CefDownloadHandler, public CefDisplayHandler, public CefRequestHandler
{
	private:
		CefRefPtr<RenderHandler> RenderHandlerRef;
		CefRefPtr<CefMessageRouterBrowserSide> MessageRouter;
		TUniquePtr<FSwuiBrowserQueryHandler> QueryHandler;

		CefRefPtr<CefBrowser> BrowserRef;
		int BrowserId;
		bool bIsClosing;
		USwuiView* OwningView;

	public:
		BrowserClient(RenderHandler* InRenderHandler, USwuiView* InOwningView);

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

		virtual CefRefPtr<CefRequestHandler> GetRequestHandler() override
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
			CefLifeSpanHandler::WindowOpenDisposition TargetDisposition,
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
		bool OnBeforeBrowse(CefRefPtr<CefBrowser> Browser,
			CefRefPtr<CefFrame> Frame,
			CefRefPtr<CefRequest> Request,
			bool UserGesture,
			bool IsRedirect) override;
		void OnRenderProcessTerminated(CefRefPtr<CefBrowser> Browser,
			TerminationStatus Status,
			int ErrorCode,
			const CefString& ErrorString) override;
		bool OnProcessMessageReceived(CefRefPtr<CefBrowser> Browser,
			CefRefPtr<CefFrame> Frame,
			CefProcessId SourceProcess,
			CefRefPtr<CefProcessMessage> Message) override;

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

	private:
		void ReleaseQueryHandler();
		~BrowserClient() override;
};
