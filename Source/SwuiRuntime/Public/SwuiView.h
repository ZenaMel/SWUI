#pragma once

#include "UObject/Object.h"
#include "SwuiTypes.h"
#include "SwuiView.generated.h"

struct FSwuiViewCefData;

UCLASS(ClassGroup=Swui, Blueprintable)
class SWUIRUNTIME_API USwuiView : public UObject, public ISwuiRenderTarget
{
	GENERATED_BODY()

public:
	USwuiView();

	// Internal URL loaded by the backend adapter — not set directly by users.
	FString DefaultURL;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	int32 Width = 1280;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	int32 Height = 720;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	bool bIsTransparent = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="SwuiRuntime")
	UMaterialInterface* BaseMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="SwuiRuntime")
	FName TextureParameterName = "SwuiTexture";

	void Init(const FSwuiInstanceSettings& InInstanceSettings = FSwuiInstanceSettings{});

	// Returns the CEF windowless frame rate this view was initialised with.
	int32 GetWindowlessFrameRate() const { return WindowlessFrameRate; }

	// Internal — called by the backend adapter, not directly from Blueprint.
	void LoadURL(const FString& URI);

	// Internal — called by the backend adapter, not directly from Blueprint.
	void ExecuteJavaScript(const FString& Script);

	UFUNCTION(BlueprintCallable, Category="SwuiRuntime")
	UTexture2D* GetTexture() const;

	// ISwuiRenderTarget
	virtual void OnPaint(const void* Buffer, FUpdateTextureRegion2D* Regions, int32 RegionCount, int32 Width, int32 Height) override;
	virtual UTexture2D* GetOrCreateTexture(int32 InWidth, int32 InHeight) override;

	virtual void BeginDestroy() override;

	// Per-instance settings forwarded from USwui component at Init() time.
	// Kept public so USwuiSubsystem can read isolation flags (e.g. bPauseBrowserUpdates).
	FSwuiInstanceSettings InstanceSettings;

private:
	UPROPERTY()
	UTexture2D* Texture;

	UMaterialInstanceDynamic* MaterialInstance;

	TSharedPtr<FSwuiViewCefData> CefData;

	int32 WindowlessFrameRate = 300;

	// Paint diagnostics — all written only from CEF's renderer thread, safe without atomics.
	int32  Stat_Paints          = 0;
	int32  Stat_DirtyRects      = 0;
	int64  Stat_DirtyPixels     = 0;   // sum of each dirty rect's w*h
	int64  Stat_UploadedPixels  = 0;   // actual bytes/4 copied (band or per-rect row slices)
	int64  Stat_MemcpyUs        = 0;   // accumulated Memcpy time in microseconds
	int64  Stat_MemcpyMaxUs     = 0;   // worst-case single Memcpy this second
	int32  Stat_LargestDirtyRect = 0;  // largest single dirty rect area (px) this second
	double Stat_LastLogTime     = 0.0;

	void ResetTexture();
	void DestroyTexture();
	void ResetMatInstance();
};
