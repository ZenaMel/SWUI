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
	bool  bShowDirtyRectOverlay     = false; // push dirty rects + stats to __SWUI_DEBUG_RECTS__ at ~10 Hz
};

// Descriptor for one dirty rect within a shared packed pixel buffer.
// SrcX/SrcY in Region are always 0 — data starts at SrcOffsetBytes in the shared buffer.
struct FSwuiPackedRectDesc
{
	FUpdateTextureRegion2D Region;    // DestX/DestY = texture destination; SrcX=SrcY=0
	uint32 SrcPitch       = 0;        // tight row stride = Width * 4
	int32  SrcOffsetBytes = 0;        // byte offset into FSwuiPaintUploadData::PackedPixels
};

// Single-allocation upload payload for the per-rect path.
// All rects are packed sequentially into one PackedPixels buffer — no per-rect heap alloc.
struct FSwuiPaintUploadData
{
	FTextureResource*           Texture2DResource = nullptr;
	TArray<uint8>               PackedPixels;  // all rect data end-to-end
	TArray<FSwuiPackedRectDesc> Rects;
};

class ISwuiRenderTarget
{
public:
	virtual ~ISwuiRenderTarget() = default;
	virtual void OnPaint(const void* Buffer, FUpdateTextureRegion2D* Regions, int32 RegionCount, int32 Width, int32 Height) = 0;
	virtual UTexture2D* GetOrCreateTexture(int32 Width, int32 Height) = 0;
};
