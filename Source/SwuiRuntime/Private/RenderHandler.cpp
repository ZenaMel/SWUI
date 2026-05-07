#include "RenderHandler.h"
#include "ISwuiRuntime.h"
#include "Interfaces/IPluginManager.h"

RenderHandler::RenderHandler(int32 Width, int32 Height, ISwuiRenderTarget* InRenderTarget)
{
	this->Width = Width;
	this->Height = Height;
	this->RenderTarget = InRenderTarget;
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
	if (BrowserId == Browser->GetIdentifier())
	{
		BrowserRef = nullptr;
	}
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
