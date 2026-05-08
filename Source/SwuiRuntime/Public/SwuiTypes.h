#pragma once

#include "CoreMinimal.h"

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
