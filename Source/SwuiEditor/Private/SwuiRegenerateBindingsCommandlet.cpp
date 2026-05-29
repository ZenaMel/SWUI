#include "SwuiRegenerateBindingsCommandlet.h"
#include "SwuiBindingCollector.h"

int32 USwuiRegenerateBindingsCommandlet::Main(const FString& Params)
{
	UE_LOG(LogTemp, Log, TEXT("SWUI: Regenerating all TS bindings via commandlet..."));

	const bool bOK = SwuiRegenerateAllBindings();
	if (bOK)
	{
		UE_LOG(LogTemp, Log, TEXT("SWUI: Bindings regenerated successfully."));
		return 0;
	}

	UE_LOG(LogTemp, Error, TEXT("SWUI: Bindings regeneration FAILED."));
	return 1;
}
