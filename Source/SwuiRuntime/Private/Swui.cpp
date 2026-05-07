#include "Swui.h"
#include "SwuiSubsystem.h"
#include "Engine/GameInstance.h"

USwui::USwui()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void USwui::BeginPlay()
{
	Super::BeginPlay();
	UWorld* World = GetWorld();
	if (!World) return;
	UGameInstance* GI = World->GetGameInstance();
	if (!GI) return;
	USwuiSubsystem* Sub = GI->GetSubsystem<USwuiSubsystem>();
	if (!Sub) return;

	Sub->InitRenderer(DefaultURI, InterfaceName, bIsHUD,
		ViewWidth, ViewHeight, ZOrder, BaseMaterial, TextureParameterName);
}

void USwui::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UWorld* World = GetWorld();
	if (World)
	{
		if (UGameInstance* GI = World->GetGameInstance())
		{
			if (USwuiSubsystem* Sub = GI->GetSubsystem<USwuiSubsystem>())
			{
				if (AActor* Owner = GetOwner())
					Sub->Unobserve(Owner);
				Sub->ShutdownRenderer();
			}
		}
	}
	Super::EndPlay(EndPlayReason);
}

