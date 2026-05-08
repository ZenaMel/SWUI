#pragma once

#include "Engine/DeveloperSettings.h"
#include "SwuiSettings.generated.h"

/** Project Settings > Plugins > SimpleWebUI */
UCLASS(Config=EditorPerProjectUserSettings, defaultconfig, meta=(DisplayName="SimpleWebUI"))
class SWUIRUNTIME_API USwuiSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/**
	 * Completely disables SimpleWebUI — no CEF initialisation, no subsystem, no rendering.
	 * Useful for diagnosing frame hitches caused by the plugin.
	 * Requires an editor restart to take effect.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Debug")
	bool bDisablePlugin = false;
};
