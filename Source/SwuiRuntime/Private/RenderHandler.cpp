#include "RenderHandler.h"
#include "ISwuiRuntime.h"
#include "SwuiView.h"
#include "Interfaces/IPluginManager.h"
#include "Async/Async.h"
#include "include/wrapper/cef_message_router.h"

namespace
{
	int32 SwuiHexValue(const TCHAR C)
	{
		if (C >= TEXT('0') && C <= TEXT('9')) return C - TEXT('0');
		if (C >= TEXT('a') && C <= TEXT('f')) return 10 + (C - TEXT('a'));
		if (C >= TEXT('A') && C <= TEXT('F')) return 10 + (C - TEXT('A'));
		return -1;
	}

	FString SwuiUrlDecodeUtf8(const FString& Encoded)
	{
		TArray<ANSICHAR> Bytes;
		Bytes.Reserve(Encoded.Len() + 1);

		for (int32 I = 0; I < Encoded.Len(); ++I)
		{
			const TCHAR C = Encoded[I];

			if (C == TEXT('%') && I + 2 < Encoded.Len())
			{
				const int32 Hi = SwuiHexValue(Encoded[I + 1]);
				const int32 Lo = SwuiHexValue(Encoded[I + 2]);

				if (Hi >= 0 && Lo >= 0)
				{
					Bytes.Add(static_cast<ANSICHAR>((Hi << 4) | Lo));
					I += 2;
					continue;
				}
			}

			if (C == TEXT('+'))
			{
				Bytes.Add(' ');
				continue;
			}

			Bytes.Add(static_cast<ANSICHAR>(C));
		}

		Bytes.Add('\0');
		return FString(UTF8_TO_TCHAR(Bytes.GetData()));
	}
}

class FSwuiBrowserQueryHandler final : public CefMessageRouterBrowserSide::Handler
{
public:
	explicit FSwuiBrowserQueryHandler(USwuiView* InOwningView)
		: OwningView(InOwningView)
	{
	}

	bool OnQuery(CefRefPtr<CefBrowser> Browser,
		CefRefPtr<CefFrame> Frame,
		int64 QueryId,
		const CefString& Request,
		bool Persistent,
		CefRefPtr<Callback> Callback) override
	{
		UNREFERENCED_PARAMETER(Browser);
		UNREFERENCED_PARAMETER(Frame);
		UNREFERENCED_PARAMETER(QueryId);
		UNREFERENCED_PARAMETER(Persistent);

		const FString RequestJson = FString(Request.ToWString().c_str());

		UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI JS BUS] cefQuery received raw=%s"), *RequestJson);
		Callback->Success("ok");

		TWeakObjectPtr<USwuiView> WeakView = OwningView;
		AsyncTask(ENamedThreads::GameThread, [WeakView, RequestJson]()
		{
			if (WeakView.IsValid())
			{
				WeakView->HandleIncomingMessage(RequestJson);
			}
		});
		return true;
	}

private:
	TWeakObjectPtr<USwuiView> OwningView;
};

BrowserClient::BrowserClient(RenderHandler* InRenderHandler, USwuiView* InOwningView)
	: RenderHandlerRef(InRenderHandler)
	, QueryHandler(MakeUnique<FSwuiBrowserQueryHandler>(InOwningView))
	, BrowserId(0)
	, bIsClosing(false)
	, OwningView(InOwningView)
{
	const CefMessageRouterConfig RouterConfig;
	MessageRouter = CefMessageRouterBrowserSide::Create(RouterConfig);
	if (MessageRouter && QueryHandler)
	{
		MessageRouter->AddHandler(QueryHandler.Get(), false);
	}
}

BrowserClient::~BrowserClient()
{
	ReleaseQueryHandler();
}

void BrowserClient::ReleaseQueryHandler()
{
	if (MessageRouter && QueryHandler)
	{
		MessageRouter->RemoveHandler(QueryHandler.Get());
	}

	QueryHandler.Reset();
	MessageRouter = nullptr;
}

RenderHandler::RenderHandler(int32 Width, int32 Height, ISwuiRenderTarget* InRenderTarget,
	ISwuiAcceleratedRenderTarget* InAcceleratedTarget,
	ESwuiRenderingMode InMode)
{
	this->Width = Width;
	this->Height = Height;
	this->RenderTarget = InRenderTarget;
	this->AcceleratedTarget = InAcceleratedTarget;
	this->ActiveMode = InMode;
}

void RenderHandler::GetViewRect(CefRefPtr<CefBrowser> Browser, CefRect &Rect)
{
	Rect = CefRect(0, 0, Width, Height);
}

void RenderHandler::OnPaint(CefRefPtr<CefBrowser> Browser, PaintElementType Type, const RectList &DirtyRects, const void *Buffer, int InWidth, int InHeight)
{
	if (!RenderTarget || !Buffer)
	{
		return;
	}

	FUpdateTextureRegion2D* UpdateRegions = static_cast<FUpdateTextureRegion2D*>(FMemory::Malloc(sizeof(FUpdateTextureRegion2D) * DirtyRects.size()));

	int RegionIndex = 0;
	for (auto DirtyRect : DirtyRects)
	{
		UpdateRegions[RegionIndex].DestX = UpdateRegions[RegionIndex].SrcX = DirtyRect.x;
		UpdateRegions[RegionIndex].DestY = UpdateRegions[RegionIndex].SrcY = DirtyRect.y;
		UpdateRegions[RegionIndex].Height = DirtyRect.height;
		UpdateRegions[RegionIndex].Width = DirtyRect.width;

		RegionIndex++;
	}

	RenderTarget->OnPaint(Buffer, UpdateRegions, DirtyRects.size(), InWidth, InHeight);
}

// ---------------------------------------------------------------------------
// GPU Accelerated path — CEF calls this instead of OnPaint when
// shared_texture_enabled is set on the CefWindowInfo.
//
// Thread: CEF renderer thread.
// The shared handle in Info is only valid for the duration of this call.
// We forward the raw HANDLE to USwuiView which opens/copies it on the
// render thread before returning.
// ---------------------------------------------------------------------------
void RenderHandler::OnAcceleratedPaint(CefRefPtr<CefBrowser> Browser, PaintElementType Type, const RectList &DirtyRects, const CefAcceleratedPaintInfo& Info)
{
	if (!AcceleratedTarget)
	{
		return;
	}

#if PLATFORM_WINDOWS
	void* SharedHandle = Info.shared_texture_handle;
	if (!SharedHandle)
	{
		UE_LOG(LogSwuiRuntime, Warning, TEXT("OnAcceleratedPaint: null shared texture handle"));
		return;
	}

	AcceleratedTarget->OnAcceleratedPaint(SharedHandle, Width, Height);
#endif
}

void BrowserClient::OnAfterCreated(CefRefPtr<CefBrowser> Browser)
{
	if (!BrowserRef.get())
	{
		BrowserRef = Browser;
		BrowserId = Browser->GetIdentifier();
	}
}

void BrowserClient::OnBeforeClose(CefRefPtr<CefBrowser> Browser)
{
	if (MessageRouter)
	{
		MessageRouter->OnBeforeClose(Browser);
	}
	ReleaseQueryHandler();

	if (BrowserId == Browser->GetIdentifier())
	{
		BrowserRef = nullptr;
	}
}

bool BrowserClient::OnBeforeBrowse(CefRefPtr<CefBrowser> Browser,
	CefRefPtr<CefFrame> Frame,
	CefRefPtr<CefRequest> Request,
	bool UserGesture,
	bool IsRedirect)
{
	UNREFERENCED_PARAMETER(UserGesture);
	UNREFERENCED_PARAMETER(IsRedirect);

	// ── SWUI URL Bridge (JS->UE fallback) ──────────────────────────────────
	// Intercept swui://bus?payload=... URL navigation used as a fallback
	// transport when window.cefQuery is unavailable.
	{
		const CefString CefUrl = Request->GetURL();
		const FString UrlString(CefUrl.ToWString().c_str());

		if (UrlString.StartsWith(TEXT("swui://bus")))
		{
			// Extract payload query parameter using FString.
			FString PayloadEncoded;

			const FString PayloadMarker = TEXT("payload=");
			const int32 PayloadStart = UrlString.Find(PayloadMarker);

			if (PayloadStart != INDEX_NONE)
			{
				const int32 ValueStart = PayloadStart + PayloadMarker.Len();

				FString QueryValue = UrlString.Mid(ValueStart);

				int32 AmpIndex = INDEX_NONE;
				if (QueryValue.FindChar(TEXT('&'), AmpIndex))
				{
					QueryValue = QueryValue.Left(AmpIndex);
				}

				PayloadEncoded = QueryValue;
			}

			// URL-decode the extracted payload.
			const FString RawJson = SwuiUrlDecodeUtf8(PayloadEncoded);

			UE_LOG(LogSwuiRuntime, Log, TEXT("[SWUI JS BUS] urlBridge received raw=%s"), *RawJson);

			if (OwningView)
			{
				TWeakObjectPtr<USwuiView> WeakView(OwningView);
				AsyncTask(ENamedThreads::GameThread, [WeakView, RawJson]()
				{
					if (WeakView.IsValid())
					{
						WeakView->HandleIncomingMessage(RawJson);
					}
				});
			}

			return true; // Cancel navigation — do not load the swui://bus URL.
		}
	}

	if (MessageRouter)
	{
		MessageRouter->OnBeforeBrowse(Browser, Frame);
	}

	return false;
}

void BrowserClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> Browser,
	TerminationStatus Status,
	int ErrorCode,
	const CefString& ErrorString)
{
	UNREFERENCED_PARAMETER(Status);
	UNREFERENCED_PARAMETER(ErrorCode);
	UNREFERENCED_PARAMETER(ErrorString);

	if (MessageRouter)
	{
		MessageRouter->OnRenderProcessTerminated(Browser);
	}
}

bool BrowserClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> Browser,
	CefRefPtr<CefFrame> Frame,
	CefProcessId SourceProcess,
	CefRefPtr<CefProcessMessage> Message)
{
	if (MessageRouter && MessageRouter->OnProcessMessageReceived(Browser, Frame, SourceProcess, Message))
	{
		return true;
	}

	return false;
}

bool BrowserClient::OnConsoleMessage(CefRefPtr<CefBrowser> Browser, cef_log_severity_t Level, const CefString& Message, const CefString& Source, int Line)
{
	UE_LOG(LogSwuiRuntime, Log, TEXT("CEF Console: %s"), *FString(Message.ToWString().c_str()));
	return true;
}

void BrowserClient::OnFullscreenModeChange(CefRefPtr<CefBrowser> Browser, bool Fullscreen)
{
	UE_LOG(LogSwuiRuntime, Log, TEXT("Changed to Fullscreen: %d"), Fullscreen);
}

void BrowserClient::OnTitleChange(CefRefPtr<CefBrowser> Browser, const CefString& Title)
{
	UE_LOG(LogSwuiRuntime, Log, TEXT("CEF Title: %s"), *FString(Title.ToWString().c_str()));
}

CefRefPtr<CefBrowser> BrowserClient::GetCEFBrowser()
{
	return BrowserRef;
}

void BrowserClient::OnUncaughtException(CefRefPtr<CefBrowser> Browser, CefRefPtr<CefFrame> Frame, CefRefPtr<CefV8Context> Context, CefRefPtr<CefV8Exception> Exception, CefRefPtr<CefV8StackTrace> StackTrace)
{
	FString ErrorMessage = FString(Exception->GetMessage().ToWString().c_str());
	UE_LOG(LogSwuiRuntime, Warning, TEXT("%s"), *ErrorMessage);
}

FString ReversePathSlashes(FString ForwardPath)
{
	return ForwardPath.Replace(TEXT("/"), TEXT("\\"));
}

FString UtilitySWUIDownloadsFolder()
{
	return ReversePathSlashes(FPaths::ConvertRelativePathToFull(IPluginManager::Get().FindPlugin("SimpleWebUI")->GetBaseDir() + "/Downloads/"));
}

bool BrowserClient::OnBeforeDownload(
	CefRefPtr<CefBrowser> Browser,
	CefRefPtr<CefDownloadItem> DownloadItem,
	const CefString & SuggestedName,
	CefRefPtr<CefBeforeDownloadCallback> Callback)
{
	UNREFERENCED_PARAMETER(Browser);
	UNREFERENCED_PARAMETER(DownloadItem);

	FString DownloadPath = UtilitySWUIDownloadsFolder() + FString(SuggestedName.ToWString().c_str());

	Callback->Continue(*DownloadPath, false);

	UE_LOG(LogSwuiRuntime, Log, TEXT("Downloading file for path %s"), *DownloadPath);

	return true;
}

void BrowserClient::OnDownloadUpdated(
	CefRefPtr<CefBrowser> ForBrowser,
	CefRefPtr<CefDownloadItem> DownloadItem,
	CefRefPtr<CefDownloadItemCallback> Callback)
{
	int Percentage = DownloadItem->GetPercentComplete();
	FString Url = FString(DownloadItem->GetFullPath().ToWString().c_str());

	UE_LOG(LogSwuiRuntime, Log, TEXT("Download %s Updated: %d"), *Url, Percentage);

	if (Percentage == 100 && DownloadItem->IsComplete()) {
		UE_LOG(LogSwuiRuntime, Log, TEXT("Download %s Complete"), *Url);
	}
}
