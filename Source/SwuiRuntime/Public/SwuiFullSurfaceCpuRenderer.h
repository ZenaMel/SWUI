#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Containers/Queue.h"
#include "RHI.h"
#include "TextureResource.h"

struct FUpdateTextureRegion2D;

enum { ESwuiFullSurfacePoolSize = 4 };

struct FSwuiFullSurfaceFrame
{
	TArray<uint8> Pixels;
	int32 Width  = 0;
	int32 Height = 0;
	uint64 Generation = 0;
	double PaintTime  = 0.0;

	void Allocate(int32 W, int32 H)
	{
		Width  = W;
		Height = H;
		Pixels.SetNumUninitialized(W * H * 4);
	}
};

// ---------------------------------------------------------------------------
// Dedicated full-surface CPU renderer.
//
// Thread model:
//   InitializePool / HandleSizeChanged — game thread
//   StagePaint   — CEF renderer thread
//   TickUpload   — game thread
//   ReturnFrameToFree — RHI thread
//
// Conventions:
//   Top-level public methods lock PoolMutex if they touch shared state.
//   _Locked helpers assume caller already holds PoolMutex.
//   ReturnedFrames is MPSC TQueue — lock-free for producer (RHI thread),
//     drained under PoolMutex by consumer (game/CEF threads).
// ---------------------------------------------------------------------------
class FSwuiFullSurfaceCpuRenderer
{
public:
	FSwuiFullSurfaceCpuRenderer();
	~FSwuiFullSurfaceCpuRenderer();

	void InitializePool(int32 Width, int32 Height);
	void HandleTextureSizeChanged(int32 NewWidth, int32 NewHeight);

	void StagePaint(const void* Buffer, int32 InWidth, int32 InHeight, double PaintArrivalTime);

	void TickUpload(FTextureResource* InTextureResource, double Now, bool bForceEveryTick);

	void Reset();

	bool HasFreshPaintPending() const;
	void ClearPendingPaint();

	struct FStats
	{
		int32 PoolFree     = 0;
		int32 PoolReady    = 0;
		int32 PoolInFlight = 0;

		int32  StatInterval_CefPaints            = 0;
		int32  StatInterval_Uploads              = 0;
		int32  StatInterval_SkippedNoFreshPaint  = 0;
		int32  StatInterval_DroppedPaints        = 0;
		int32  StatInterval_ReplacedReadyFrames  = 0;
		int32  StatInterval_Allocations          = 0;
		int32  StatInterval_PaintCopySamples     = 0;
		int32  StatInterval_EnqueueSamples       = 0;
		int32  StatInterval_PaintToUploadSamples = 0;
		int64  StatInterval_UploadedPx           = 0;
		double StatInterval_PaintCopyMsSum       = 0.0;
		double StatInterval_PaintCopyMsMax       = 0.0;
		double StatInterval_EnqueueMsSum         = 0.0;
		double StatInterval_EnqueueMsMax         = 0.0;
		double StatInterval_PaintToUploadMsSum   = 0.0;
		double StatInterval_PaintToUploadMsMax   = 0.0;

		int32  StatTotal_Allocations   = 0;
		int32  StatTotal_DroppedPaints = 0;
	};

	const FStats& GetStats() const { return Stats; }
	void ResetIntervalStats();
	void RefreshPoolSnapshot() const;

	// RHI-thread safe. Called from render command after RHIUpdateTexture2D.
	void ReturnFrameToFree(FSwuiFullSurfaceFrame* Frame);

private:
	static constexpr int32 PoolSize = ESwuiFullSurfacePoolSize;

	FSwuiFullSurfaceFrame OwnedFrames[PoolSize];

	// All queues/sets are guarded by PoolMutex except ReturnedFrames.
	FSwuiFullSurfaceFrame* FreeFrames[PoolSize];
	int32 FreeCount = 0;

	FSwuiFullSurfaceFrame* LatestReadyFrame = nullptr;

	FSwuiFullSurfaceFrame* InFlightFrames[PoolSize];
	int32 InFlightCount = 0;

	// MPSC queue: producer = RHI thread (ReturnFrameToFree), consumer =
	// game/CEF threads (drained under PoolMutex in _Locked helpers).
	TQueue<FSwuiFullSurfaceFrame*, EQueueMode::Mpsc> ReturnedFrames;

	mutable FCriticalSection PoolMutex;

	int32 AllocatedWidth  = 0;
	int32 AllocatedHeight = 0;
	uint64 PaintGeneration = 0;
	uint64 UploadedGeneration = 0;
	bool bHasEverHadFrame = false;

	mutable FStats Stats;

	// _Locked helpers — caller must already hold PoolMutex.
	void DrainReturnedQueue_Locked();
	FSwuiFullSurfaceFrame* AcquireFreeFrame_Locked();
	void PublishLatestFrame_Locked(FSwuiFullSurfaceFrame* Frame);
	FSwuiFullSurfaceFrame* ConsumeLatestFrame_Locked();
};
