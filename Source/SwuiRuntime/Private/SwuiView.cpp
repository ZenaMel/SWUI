#include "SwuiView.h"
#include "RenderHandler.h"
#include "ISwuiRuntime.h"
#include "SwuiManager.h"

struct FSwuiViewCefData
{
	CefRefPtr<BrowserClient> Client;
	CefRefPtr<CefBrowser> Browser;
};

USwuiView::USwuiView()
{
	Texture = nullptr;
	CefData = MakeShared<FSwuiViewCefData>();
}

void USwuiView::Init()
{
	if (Width <= 0 || Height <= 0)
	{
		UE_LOG(LogSwuiRuntime, Warning, TEXT("USwuiView: Width or Height <= 0"));
		return;
	}

	CefWindowInfo Info;
	Info.SetAsWindowless(0);

	CefBrowserSettings BrowserSettings;
	BrowserSettings.webgl = STATE_ENABLED;

	RenderHandler* Renderer = new RenderHandler(Width, Height, this);
	CefRefPtr<BrowserClient> Client = new BrowserClient(Renderer);

	CefRefPtr<CefBrowser> Browser = CefBrowserHost::CreateBrowserSync(
		Info,
		Client.get(),
		"about:blank",
		BrowserSettings,
		nullptr,
		nullptr);

	// Use engine max FPS if set; otherwise pass a high ceiling (300) so CEF
	// self-limits to whatever the OS/driver actually supports rather than
	// being artificially capped by us.
	int32 TargetFPS = 300;
	if (GEngine && GEngine->GetMaxFPS() > 0)
		TargetFPS = FMath::RoundToInt(GEngine->GetMaxFPS());

	Browser->GetHost()->SetWindowlessFrameRate(TargetFPS);
	WindowlessFrameRate = TargetFPS;

	CefData->Client = Client;
	CefData->Browser = Browser;

	UE_LOG(LogSwuiRuntime, Log, TEXT("USwuiView Initialized"));

	if (!DefaultURL.IsEmpty())
	{
		LoadURL(DefaultURL);
	}

	ResetTexture();
}

void USwuiView::LoadURL(const FString& URI)
{
	if (!CefData || !CefData->Browser)
	{
		return;
	}

	// http/https/localhost/file → pass through directly
	if (URI.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase)
		|| URI.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase)
		|| URI.StartsWith(TEXT("localhost"), ESearchCase::IgnoreCase)
		|| URI.StartsWith(TEXT("file:///"), ESearchCase::IgnoreCase))
	{
		CefData->Browser->GetMainFrame()->LoadURL(*URI);
		return;
	}

	// swui:// or bare path → resolve under Content/
	FString Relative = URI;
	if (Relative.StartsWith(TEXT("swui://"), ESearchCase::IgnoreCase))
	{
		Relative = Relative.RightChop(7); // strip "swui://"
	}

	// Append .html if no extension provided
	if (FPaths::GetExtension(Relative).IsEmpty())
	{
		Relative += TEXT(".html");
	}

	FString ContentDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
	FString LocalFile = FString(TEXT("file:///")) + ContentDir + Relative;
	CefData->Browser->GetMainFrame()->LoadURL(*LocalFile);
}

void USwuiView::ExecuteJavaScript(const FString& Script)
{
	if (CefData && CefData->Browser)
	{
		CefString CodeStr = *Script;
		CefData->Browser->GetMainFrame()->ExecuteJavaScript(CodeStr, "", 0);
	}
}

UTexture2D* USwuiView::GetTexture() const
{
	return Texture;
}

void USwuiView::OnPaint(const void* Buffer, FUpdateTextureRegion2D* Regions, int32 RegionCount, int32 InWidth, int32 InHeight)
{
	GetOrCreateTexture(InWidth, InHeight);

	if (!Texture || !Texture->GetResource())
	{
		FMemory::Free(Regions);
		return;
	}

	// Compute the row-range union of all dirty rects so we only copy the rows
	// that actually changed. Avoids a full-frame Memcpy for small updates
	// (e.g. a spinning compass needle should copy ~200 rows, not 1440).
	int32 MinRow = InHeight, MaxRow = 0;
	for (int32 i = 0; i < RegionCount; ++i)
	{
		MinRow = FMath::Min(MinRow, (int32)Regions[i].SrcY);
		MaxRow = FMath::Max(MaxRow, (int32)Regions[i].SrcY + (int32)Regions[i].Height);
	}
	MinRow = FMath::Clamp(MinRow, 0, InHeight);
	MaxRow = FMath::Clamp(MaxRow, 0, InHeight);

	const int32 FullPitch  = InWidth * 4;
	const int32 RowsToCopy = FMath::Max(1, MaxRow - MinRow);

	// Rebase each region's SrcY to be relative to MinRow
	for (int32 i = 0; i < RegionCount; ++i)
		Regions[i].SrcY = FMath::Max(0, (int32)Regions[i].SrcY - MinRow);

	FUpdateTextureRegionsData* RegionData = new FUpdateTextureRegionsData;
	RegionData->Texture2DResource = (FTextureResource*)Texture->GetResource();
	RegionData->NumRegions = RegionCount;
	RegionData->SrcBpp = 4;
	RegionData->SrcPitch = FullPitch;
	RegionData->Regions = Regions;
	RegionData->SrcData.SetNumUninitialized(FullPitch * RowsToCopy);
	FPlatformMemory::Memcpy(RegionData->SrcData.GetData(),
		(const uint8*)Buffer + (int64)MinRow * FullPitch,
		FullPitch * RowsToCopy);

	ENQUEUE_RENDER_COMMAND(UpdateSwuiViewCommand)(
		[RegionData](FRHICommandList& CommandList)
		{
			for (uint32 RegionIndex = 0; RegionIndex < RegionData->NumRegions; RegionIndex++)
			{
				RHIUpdateTexture2D(
					RegionData->Texture2DResource->TextureRHI->GetTexture2D(),
					0,
					RegionData->Regions[RegionIndex],
					RegionData->SrcPitch,
					RegionData->SrcData.GetData());
			}

			FMemory::Free(RegionData->Regions);
			delete RegionData;
		});
}

UTexture2D* USwuiView::GetOrCreateTexture(int32 InWidth, int32 InHeight)
{
	if (!Texture || Texture->GetSizeX() != InWidth || Texture->GetSizeY() != InHeight)
	{
		DestroyTexture();

		Texture = UTexture2D::CreateTransient(InWidth, InHeight, PF_B8G8R8A8);
		Texture->AddToRoot();
		Texture->UpdateResource();

		ResetMatInstance();
	}

	return Texture;
}

void USwuiView::ResetTexture()
{
	DestroyTexture();

	Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
	Texture->AddToRoot();
	Texture->UpdateResource();

	ResetMatInstance();
}

void USwuiView::DestroyTexture()
{
	if (Texture)
	{
		Texture->RemoveFromRoot();
		Texture->MarkAsGarbage();
		Texture = nullptr;
	}
}

void USwuiView::ResetMatInstance()
{
	if (!Texture || !BaseMaterial || TextureParameterName.IsNone())
	{
		return;
	}

	if (!MaterialInstance)
	{
		MaterialInstance = UMaterialInstanceDynamic::Create(BaseMaterial, NULL);
		if (!MaterialInstance)
		{
			return;
		}
	}

	UTexture* Tex = nullptr;
	if (!MaterialInstance->GetTextureParameterValue(TextureParameterName, Tex))
	{
		UE_LOG(LogSwuiRuntime, Warning, TEXT("USwuiView: Texture parameter '%s' not found in material"), *TextureParameterName.ToString());
		return;
	}

	MaterialInstance->SetTextureParameterValue(TextureParameterName, Texture);
}

void USwuiView::BeginDestroy()
{
	if (CefData && CefData->Browser)
	{
		CefData->Browser->GetHost()->CloseBrowser(true);
	}

	DestroyTexture();

	Super::BeginDestroy();
}
