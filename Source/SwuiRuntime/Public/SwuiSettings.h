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

	// -----------------------------------------------------------------------
	// General
	// -----------------------------------------------------------------------

	/**
	 * Completely disables SimpleWebUI — no CEF initialisation, no subsystem, no rendering.
	 * Useful for diagnosing frame hitches caused by the plugin.
	 * Requires an editor restart to take effect.
	 */
	UPROPERTY(Config, EditAnywhere, Category="General")
	bool bDisablePlugin = false;

	// -----------------------------------------------------------------------
	// Rendering | Frame Rate
	// -----------------------------------------------------------------------

	/**
	 * CEF process-wide off-screen frame rate ceiling.
	 * Passed as the --off-screen-frame-rate command-line switch at startup,
	 * so it affects all browser instances. Must be >= DefaultViewFrameRate.
	 * Set to 300 for 144/165/240 Hz HUDs. Requires an editor restart.
	 */
	UPROPERTY(Config, EditAnywhere, Category="Rendering|Frame Rate",
		meta=(ClampMin="10", ClampMax="300", UIMin="30", UIMax="300"))
	int32 CefOffscreenFrameRate = 300;

	/**
	 * Default per-view windowless frame rate passed to SetWindowlessFrameRate()
	 * when each CEF browser is created. 0 = use the engine MaxFPS setting,
	 * falling back to 300. Set to your monitor refresh rate (144, 165, 240, …).
	 */
	UPROPERTY(Config, EditAnywhere, Category="Rendering|Frame Rate",
		meta=(ClampMin="0", ClampMax="300"))
	int32 DefaultViewFrameRate = 0;

	// -----------------------------------------------------------------------
	// Rendering | Upload Strategy
	// -----------------------------------------------------------------------

	/**
	 * Band-merge overcopy ratio guard.
	 * When (full-width row-band area / total dirty rect area) > this value,
	 * the upload path switches to per-rect tight-packed copies instead of the
	 * single full-width band copy. Lower = prefer per-rect more aggressively.
	 * 1.0 = always per-rect; 10.0 = always band. Default 1.25.
	 * Runtime override: swui.paint.MaxMergeOvercopyRatio
	 */
	UPROPERTY(Config, EditAnywhere, Category="Rendering|Upload Strategy",
		meta=(ClampMin="1.0", ClampMax="10.0"))
	float MaxBandOvercopyRatio = 1.25f;

	/**
	 * Maximum dirty rect count for per-rect uploads.
	 * If more rects arrive than this, falls back to band-merge to avoid
	 * flooding the RHI with too many small upload calls per paint.
	 * Runtime override: swui.paint.MaxPerRectUploads
	 */
	UPROPERTY(Config, EditAnywhere, Category="Rendering|Upload Strategy",
		meta=(ClampMin="1", ClampMax="256"))
	int32 MaxPerRectUploads = 32;

	// -----------------------------------------------------------------------
	// Debug | Profiling
	// -----------------------------------------------------------------------

	/**
	 * Log upload strategy and timing details for every paint call:
	 * dirty rects, strategy chosen, dirty/upload area, ratio, memcpy time.
	 * WARNING: extremely spammy at 144+ Hz. Short sessions only.
	 * Runtime override: swui.prof.VerbosePaint 1
	 */
	UPROPERTY(Config, EditAnywhere, Category="Debug|Profiling")
	bool bVerbosePaintLog = false;

	/**
	 * Skip all RHIUpdateTexture2D calls. Paint and memcpy still execute so
	 * you can measure CPU-side cost in isolation without GPU upload influence.
	 * Runtime override: swui.prof.NoTextureUpload 1
	 */
	UPROPERTY(Config, EditAnywhere, Category="Debug|Profiling")
	bool bNoTextureUpload = false;
};

