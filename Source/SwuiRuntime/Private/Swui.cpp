#include "Swui.h"
#include "SwuiSubsystem.h"
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
	Sub->InitRenderer(DefaultURI, InterfaceName, bIsHUD,
		ViewWidth, ViewHeight, ZOrder, BaseMaterial, TextureParameterName);
	// SetBindingSources already scanned the world and observed all matching actors.
	// No manual ObserveSource calls needed for the common case.
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

