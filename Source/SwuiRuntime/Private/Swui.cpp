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

	Sub->InitRenderer(DefaultURI, InterfaceName, bIsHUD,
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
	Sub->UpdateInstanceSettings(Rebuilt);
}
#endif

