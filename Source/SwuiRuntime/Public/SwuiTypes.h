#pragma once

#include "CoreMinimal.h"

// Per-instance rendering overrides forwarded from USwui → InitRenderer → USwuiView::Init().
// A value of 0 / 0.f means "use the project-wide USwuiSettings default".
struct FSwuiInstanceSettings
{
	int32 OverrideFrameRate        = 0;    // 0 = use project setting / engine MaxFPS
	float OverrideBandOvercopyRatio = 0.f; // 0 = use project setting (1.25)
	int32 OverrideMaxPerRectUploads = 0;   // 0 = use project setting (32)
	bool  bVerbosePaintLog          = false;
	bool  bNoTextureUpload          = false;

	// Stage-level isolation flags (all false = normal operation)
	bool  bSkipOnPaintProcessing    = false; // return at top of OnPaint after counting
	bool  bSkipDirtyRectStrategy    = false; // skip rect validation + strategy, no upload
	bool  bSkipPaintMemcpy          = false; // skip pixel copy into upload buffers
	bool  bSkipTextureUpload        = false; // skip RHIUpdateTexture2D enqueue (alias for bNoTextureUpload)
	bool  bFreezeTexture            = false; // keep last texture, skip new uploads
	bool  bPauseBrowserUpdates      = false; // skip JS state push and runtime tick dispatch
	bool  bHideDrawComponent        = false; // hide the UE widget/material draw surface
};

// Holds a single region + its tightly-packed pixel data for one RHIUpdateTexture2D call.
// SrcX/SrcY in Region = destination in the texture. SrcPitch = Region.Width * 4 (tight).
struct FSwuiRectUpload
{
	FUpdateTextureRegion2D Region;
	uint32                 SrcPitch; // bytes per source row (tight = Width*4)
	TArray<uint8>          SrcData;  // row-major pixels, Region.Height rows × SrcPitch bytes
};

// Legacy band-path struct kept for the band upload strategy.
struct FUpdateTextureRegionsData
{
	FTextureResource*         Texture2DResource;
	uint32                    NumRegions;
	FUpdateTextureRegion2D*   Regions;
	uint32                    SrcPitch;
	uint32                    SrcBpp;
	TArray<uint8>             SrcData;
};

// Render-command payload for the per-rect upload path.
struct FSwuiPerRectUploadData
{
	FTextureResource*           Texture2DResource;
	TArray<FSwuiRectUpload>     Rects;
};

class ISwuiRenderTarget
{
public:
	virtual ~ISwuiRenderTarget() = default;
	virtual void OnPaint(const void* Buffer, FUpdateTextureRegion2D* Regions, int32 RegionCount, int32 Width, int32 Height) = 0;
	virtual UTexture2D* GetOrCreateTexture(int32 Width, int32 Height) = 0;
};
