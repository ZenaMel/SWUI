#include "SwuiFunctionLibrary.h"
#include "SwuiSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

USwuiSubsystem* USwuiFunctionLibrary::GetSwuiSubsystem(UObject* WorldContext)
{
	if (!WorldContext) return nullptr;
	UWorld* World = WorldContext->GetWorld();
	if (!World) return nullptr;
	UGameInstance* GI = World->GetGameInstance();
	if (!GI) return nullptr;
	return GI->GetSubsystem<USwuiSubsystem>();
}

void USwuiFunctionLibrary::SwuiObserve(UObject* Source, FName PropertyName, FString Namespace)
{
	if (USwuiSubsystem* Sub = GetSwuiSubsystem(Source))
		Sub->ObserveProperty(Source, Namespace, PropertyName);
}

void USwuiFunctionLibrary::SwuiObserveEvent(UObject* Source, FName DelegateName, FString Namespace)
{
	if (USwuiSubsystem* Sub = GetSwuiSubsystem(Source))
		Sub->ObserveDelegate(Source, Namespace, DelegateName);
}

void USwuiFunctionLibrary::SwuiUnobserve(UObject* Source)
{
	if (USwuiSubsystem* Sub = GetSwuiSubsystem(Source))
		Sub->Unobserve(Source);
}
