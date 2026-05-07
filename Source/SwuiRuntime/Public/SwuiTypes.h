#pragma once

#include "CoreMinimal.h"

struct FUpdateTextureRegionsData
{
	FTextureResource* Texture2DResource;
	uint32 NumRegions;
	FUpdateTextureRegion2D* Regions;
	uint32 SrcPitch;
	uint32 SrcBpp;
	TArray<uint8> SrcData;
};

class ISwuiRenderTarget
{
public:
	virtual ~ISwuiRenderTarget() = default;
	virtual void OnPaint(const void* Buffer, FUpdateTextureRegion2D* Regions, int32 RegionCount, int32 Width, int32 Height) = 0;
	virtual UTexture2D* GetOrCreateTexture(int32 Width, int32 Height) = 0;
};
