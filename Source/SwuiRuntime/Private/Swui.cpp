#include "Swui.h"
#include "SwuiSubsystem.h"
#include "SwuiTypes.h"
#include "Engine/GameInstance.h"

void USwui::EnsureOwnerBindingSource()
{
	AActor* OwnerActor = GetOwner();
	if (!OwnerActor)
	{
		OwnerActor = GetTypedOuter<AActor>();
	}

	if (!OwnerActor)
	{
		return;
	}

	UClass* OwnerClass = OwnerActor->GetClass();
	if (!OwnerClass)
	{
		return;
	}

	if (BindingSources.Num() > 0 && BindingSources[0].SourceClass == OwnerClass)
	{
		return;
	}

	const int32 ExistingOwnerIndex = BindingSources.IndexOfByPredicate([OwnerClass](const FSwuiBindingSource& Source)
	{
		return Source.SourceClass == OwnerClass;
	});

	if (ExistingOwnerIndex > 0)
	{
		const FSwuiBindingSource OwnerSource = BindingSources[ExistingOwnerIndex];
		BindingSources.RemoveAt(ExistingOwnerIndex);
		BindingSources.Insert(OwnerSource, 0);
		return;
	}

	if (BindingSources.IsEmpty())
	{
		FSwuiBindingSource OwnerSource;
		OwnerSource.SourceClass = OwnerClass;
		BindingSources.Add(OwnerSource);
		return;
	}

	if (BindingSources[0].SourceClass == nullptr && BindingSources[0].Properties.IsEmpty())
	{
		BindingSources[0].SourceClass = OwnerClass;
		return;
	}

	FSwuiBindingSource OwnerSource;
	OwnerSource.SourceClass = OwnerClass;
	BindingSources.Insert(OwnerSource, 0);
}

USwui::USwui()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void USwui::BeginPlay()
{
	Super::BeginPlay();
	EnsureOwnerBindingSource();
	UWorld* World = GetWorld();
	if (!World) return;
	UGameInstance* GI = World->GetGameInstance();
	if (!GI) return;
	USwuiSubsystem* Sub = GI->GetSubsystem<USwuiSubsystem>();
	if (!Sub) return;

	Sub->SetBindingSources(BindingSources);

	// Defer renderer init to next tick so the viewport is guaranteed to exist
	// in standalone / dedicated-window launches where BeginPlay fires before
	// the first render frame. InitializeSwuiView retries until ready.
	World->GetTimerManager().SetTimerForNextTick(
		FTimerDelegate::CreateUObject(this, &USwui::InitializeSwuiView));
}

void USwui::InitializeSwuiView()
{
	UWorld* World = GetWorld();
	if (!World) return;

	// Retry until the viewport exists (standalone can take several ticks).
	if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
	{
		World->GetTimerManager().SetTimerForNextTick(
			FTimerDelegate::CreateUObject(this, &USwui::InitializeSwuiView));
		return;
	}

	UGameInstance* GI = World->GetGameInstance();
	if (!GI) return;
	USwuiSubsystem* Sub = GI->GetSubsystem<USwuiSubsystem>();
	if (!Sub) return;

	FSwuiInstanceSettings InstSettings;
	InstSettings.RenderingMode                    = RenderingMode;
	InstSettings.bIsHUD                           = bIsHUD;
	InstSettings.bUseUEFrameLockedBrowser         = bUseUEFrameLockedBrowser;
	InstSettings.bUseExternalBeginFrames          = bUseExternalBeginFrames;
	InstSettings.bSendExternalBeginFrameFromTick  = bSendExternalBeginFrameFromTick;
	InstSettings.bFlushHudStateBeforeBrowserFrame = bFlushHudStateBeforeBrowserFrame;
	InstSettings.MaxBrowserFramesPerSecond        = (bIsHUD && MaxBrowserFramesPerSecond <= 0) ? 60 : MaxBrowserFramesPerSecond;
	InstSettings.OverrideFrameRate         = OverrideFrameRate;
	InstSettings.OverrideBandOvercopyRatio = OverrideBandOvercopyRatio;
	InstSettings.OverrideMaxPerRectUploads = OverrideMaxPerRectUploads;
	InstSettings.bVerbosePaintLog          = bVerbosePaintLog;
	InstSettings.bNoTextureUpload          = bNoTextureUpload;
	InstSettings.bSkipOnPaintProcessing    = bSkipOnPaintProcessing;
	InstSettings.bSkipDirtyRectStrategy    = bSkipDirtyRectStrategy;
	InstSettings.bSkipPaintMemcpy          = bSkipPaintMemcpy;
	InstSettings.bSkipTextureUpload        = bSkipTextureUpload;
	InstSettings.bFreezeTexture            = bFreezeTexture;
	InstSettings.bPauseBrowserUpdates      = bPauseBrowserUpdates;
	InstSettings.bHideDrawComponent        = bHideDrawComponent;
	InstSettings.bShowDirtyRectOverlay     = bShowDirtyRectOverlay;
	InstSettings.bDebugForceFullFrameUploadEveryFrame = bDebugForceFullFrameUploadEveryFrame;
	InstSettings.UiResolutionPreset = UiResolutionPreset;
	InstSettings.CustomUiWidth     = CustomUiWidth;
	InstSettings.CustomUiHeight    = CustomUiHeight;
	InstSettings.bEnableHybridDirtyUpload             = DirtyUploadSettings.bEnabled;
	InstSettings.bEnableTileDiffForLargeRects         = DirtyUploadSettings.bEnableTileDiffForLargeRects;
	InstSettings.bEnableUploadBudget                  = DirtyUploadSettings.bEnableUploadBudget;
	InstSettings.TileWidth                            = DirtyUploadSettings.TileWidth;
	InstSettings.TileHeight                           = DirtyUploadSettings.TileHeight;
	InstSettings.MinDirtyRectWidth                    = DirtyUploadSettings.MinDirtyRectWidth;
	InstSettings.MinDirtyRectHeight                   = DirtyUploadSettings.MinDirtyRectHeight;
	InstSettings.CenterCriticalWidth                  = DirtyUploadSettings.CenterCriticalWidth;
	InstSettings.CenterCriticalHeight                 = DirtyUploadSettings.CenterCriticalHeight;
	InstSettings.bAlwaysProcessCenterCriticalRect     = DirtyUploadSettings.bAlwaysProcessCenterCriticalRect;
	InstSettings.MaxNormalUploadBytesPerFrame         = DirtyUploadSettings.MaxNormalUploadBytesPerFrame;
	InstSettings.MaxMergeWasteRatio                   = DirtyUploadSettings.MaxMergeWasteRatio;
	InstSettings.MaxMergedRectWidth                   = DirtyUploadSettings.MaxMergedRectWidth;
	InstSettings.MaxMergedRectHeight                  = DirtyUploadSettings.MaxMergedRectHeight;
	InstSettings.MaxMergedRectArea                    = DirtyUploadSettings.MaxMergedRectArea;
	InstSettings.bForceFullBaselineUploadOnFirstPaint = DirtyUploadSettings.bForceFullBaselineUploadOnFirstPaint;
	InstSettings.bUseRotatingDeferredTileCursor       = DirtyUploadSettings.bUseRotatingDeferredTileCursor;
	InstSettings.bLogSwuiPaintStats                   = DirtyUploadSettings.bLogSwuiPaintStats;
	InstSettings.bShowSwuiDirtyRects                  = DirtyUploadSettings.bShowSwuiDirtyRects;

	Sub->InitRenderer(DefaultURI, InterfaceName, GetOwner(), bIsHUD,
		ViewWidth, ViewHeight, ZOrder, BaseMaterial, TextureParameterName, InstSettings);
}

void USwui::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UWorld* World = GetWorld();
	if (World)
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			if (USwuiSubsystem* Sub = GI->GetSubsystem<USwuiSubsystem>())
				Sub->ShutdownRenderer();
		}
	}
	Super::EndPlay(EndPlayReason);
}

void USwui::Unobserve(UObject* Source)
{
	UWorld* World = GetWorld();
	if (!World) return;
	if (UGameInstance* GI = World->GetGameInstance())
		if (USwuiSubsystem* Sub = GI->GetSubsystem<USwuiSubsystem>())
			Sub->Unobserve(Source);
}

void USwui::SetHUDVisible(bool bVisible)
{
	UWorld* World = GetWorld();
	if (!World) return;
	if (UGameInstance* GI = World->GetGameInstance())
		if (USwuiSubsystem* Sub = GI->GetSubsystem<USwuiSubsystem>())
			Sub->SetWidgetVisible(bVisible);
}

#if WITH_EDITOR
void USwui::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Propagate any property change to the live view during PIE without restarting.
	UWorld* World = GetWorld();
	if (!World || !World->IsPlayInEditor()) return;
	UGameInstance* GI = World->GetGameInstance();
	if (!GI) return;
	USwuiSubsystem* Sub = GI->GetSubsystem<USwuiSubsystem>();
	if (!Sub) return;

	FSwuiInstanceSettings Rebuilt;
	Rebuilt.RenderingMode                    = RenderingMode;
	Rebuilt.bIsHUD                           = bIsHUD;
	Rebuilt.bUseUEFrameLockedBrowser         = bUseUEFrameLockedBrowser;
	Rebuilt.bUseExternalBeginFrames          = bUseExternalBeginFrames;
	Rebuilt.bSendExternalBeginFrameFromTick  = bSendExternalBeginFrameFromTick;
	Rebuilt.bFlushHudStateBeforeBrowserFrame = bFlushHudStateBeforeBrowserFrame;
	Rebuilt.MaxBrowserFramesPerSecond        = (bIsHUD && MaxBrowserFramesPerSecond <= 0) ? 60 : MaxBrowserFramesPerSecond;
	Rebuilt.OverrideFrameRate         = OverrideFrameRate;
	Rebuilt.OverrideBandOvercopyRatio = OverrideBandOvercopyRatio;
	Rebuilt.OverrideMaxPerRectUploads = OverrideMaxPerRectUploads;
	Rebuilt.bVerbosePaintLog          = bVerbosePaintLog;
	Rebuilt.bNoTextureUpload          = bNoTextureUpload;
	Rebuilt.bSkipOnPaintProcessing    = bSkipOnPaintProcessing;
	Rebuilt.bSkipDirtyRectStrategy    = bSkipDirtyRectStrategy;
	Rebuilt.bSkipPaintMemcpy          = bSkipPaintMemcpy;
	Rebuilt.bSkipTextureUpload        = bSkipTextureUpload;
	Rebuilt.bFreezeTexture            = bFreezeTexture;
	Rebuilt.bPauseBrowserUpdates      = bPauseBrowserUpdates;
	Rebuilt.bHideDrawComponent        = bHideDrawComponent;
	Rebuilt.bShowDirtyRectOverlay     = bShowDirtyRectOverlay;
	Rebuilt.bDebugForceFullFrameUploadEveryFrame = bDebugForceFullFrameUploadEveryFrame;
	Rebuilt.UiResolutionPreset = UiResolutionPreset;
	Rebuilt.CustomUiWidth     = CustomUiWidth;
	Rebuilt.CustomUiHeight    = CustomUiHeight;
	Rebuilt.bEnableHybridDirtyUpload             = DirtyUploadSettings.bEnabled;
	Rebuilt.bEnableTileDiffForLargeRects         = DirtyUploadSettings.bEnableTileDiffForLargeRects;
	Rebuilt.bEnableUploadBudget                  = DirtyUploadSettings.bEnableUploadBudget;
	Rebuilt.TileWidth                            = DirtyUploadSettings.TileWidth;
	Rebuilt.TileHeight                           = DirtyUploadSettings.TileHeight;
	Rebuilt.MinDirtyRectWidth                    = DirtyUploadSettings.MinDirtyRectWidth;
	Rebuilt.MinDirtyRectHeight                   = DirtyUploadSettings.MinDirtyRectHeight;
	Rebuilt.CenterCriticalWidth                  = DirtyUploadSettings.CenterCriticalWidth;
	Rebuilt.CenterCriticalHeight                 = DirtyUploadSettings.CenterCriticalHeight;
	Rebuilt.bAlwaysProcessCenterCriticalRect     = DirtyUploadSettings.bAlwaysProcessCenterCriticalRect;
	Rebuilt.MaxNormalUploadBytesPerFrame         = DirtyUploadSettings.MaxNormalUploadBytesPerFrame;
	Rebuilt.MaxMergeWasteRatio                   = DirtyUploadSettings.MaxMergeWasteRatio;
	Rebuilt.MaxMergedRectWidth                   = DirtyUploadSettings.MaxMergedRectWidth;
	Rebuilt.MaxMergedRectHeight                  = DirtyUploadSettings.MaxMergedRectHeight;
	Rebuilt.MaxMergedRectArea                    = DirtyUploadSettings.MaxMergedRectArea;
	Rebuilt.bForceFullBaselineUploadOnFirstPaint = DirtyUploadSettings.bForceFullBaselineUploadOnFirstPaint;
	Rebuilt.bUseRotatingDeferredTileCursor       = DirtyUploadSettings.bUseRotatingDeferredTileCursor;
	Rebuilt.bLogSwuiPaintStats                   = DirtyUploadSettings.bLogSwuiPaintStats;
	Rebuilt.bShowSwuiDirtyRects                  = DirtyUploadSettings.bShowSwuiDirtyRects;
	Sub->UpdateInstanceSettings(Rebuilt);
}
#endif

