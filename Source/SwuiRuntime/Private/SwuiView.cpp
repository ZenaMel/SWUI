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

	Browser->GetHost()->SetWindowlessFrameRate(60);

	CefData->Client = Client;
	CefData->Browser = Browser;

	UE_LOG(LogSwuiRuntime, Log, TEXT("USwuiView Initialized"));

	if (!DefaultURL.IsEmpty())
	{
		LoadURL(DefaultURL);
	}

	ResetTexture();
}

void USwuiView::LoadURL(const FString& URL)
{
	if (!CefData || !CefData->Browser)
	{
		return;
	}

	FString FinalUrl = URL;

	if (URL.Contains(TEXT("swui://"), ESearchCase::IgnoreCase, ESearchDir::FromStart))
	{
		FString GameDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FString LocalFile = URL.Replace(TEXT("swui://"), *GameDir, ESearchCase::IgnoreCase);
		LocalFile = FString(TEXT("file:///")) + LocalFile;
		CefData->Browser->GetMainFrame()->LoadURL(*LocalFile);
		return;
	}

	CefData->Browser->GetMainFrame()->LoadURL(*FinalUrl);
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

	FUpdateTextureRegionsData* RegionData = new FUpdateTextureRegionsData;
	RegionData->Texture2DResource = (FTextureResource*)Texture->GetResource();
	RegionData->NumRegions = RegionCount;
	RegionData->SrcBpp = 4;
	RegionData->SrcPitch = InWidth * 4;
	RegionData->Regions = Regions;
	RegionData->SrcData.SetNumUninitialized(RegionData->SrcPitch * InHeight);
	FPlatformMemory::Memcpy(RegionData->SrcData.GetData(), Buffer, RegionData->SrcData.Num());

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
