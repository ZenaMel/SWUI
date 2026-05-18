#include "SwuiFullSurfaceCpuRenderer.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

FSwuiFullSurfaceCpuRenderer::FSwuiFullSurfaceCpuRenderer()
{
}

FSwuiFullSurfaceCpuRenderer::~FSwuiFullSurfaceCpuRenderer()
{
	Reset();
}

// ── Init / resize ──────────────────────────────────────────────────────────

void FSwuiFullSurfaceCpuRenderer::InitializePool(int32 Width, int32 Height)
{
	FScopeLock Lock(&PoolMutex);

	if (AllocatedWidth == Width && AllocatedHeight == Height)
	{
		return;
	}

	// Flush any stale return queue entries before re-initializing.
	FSwuiFullSurfaceFrame* Discard = nullptr;
	while (ReturnedFrames.Dequeue(Discard)) {}

	const int32 ByteCount = Width * Height * 4;

	for (int32 i = 0; i < PoolSize; ++i)
	{
		OwnedFrames[i].Allocate(Width, Height);
		OwnedFrames[i].Generation = 0;
		OwnedFrames[i].PaintTime  = 0.0;
		FreeFrames[i] = &OwnedFrames[i];
	}
	FreeCount = PoolSize;

	LatestReadyFrame = nullptr;
	InFlightCount    = 0;

	AllocatedWidth  = Width;
	AllocatedHeight = Height;
	PaintGeneration = 0;
	UploadedGeneration = 0;
	bHasEverHadFrame  = false;

	Stats.StatInterval_Allocations += PoolSize;
	Stats.StatTotal_Allocations   += PoolSize;

	UE_LOG(LogTemp, Log,
		TEXT("[SwuiFullSurfacePool] initialized buffers=%d stride=%d bytesPerFrame=%d size=%dx%d"),
		PoolSize, Width * 4, ByteCount, Width, Height);
}

void FSwuiFullSurfaceCpuRenderer::HandleTextureSizeChanged(int32 NewWidth, int32 NewHeight)
{
	InitializePool(NewWidth, NewHeight);
}

// ── _Locked helpers (caller owns PoolMutex) ────────────────────────────────

void FSwuiFullSurfaceCpuRenderer::DrainReturnedQueue_Locked()
{
	FSwuiFullSurfaceFrame* ReturnedFrame = nullptr;
	while (ReturnedFrames.Dequeue(ReturnedFrame))
	{
		if (!ReturnedFrame)
		{
			continue;
		}

		// Remove from InFlightFrames (it was placed there by ConsumeLatestFrame_Locked).
		for (int32 i = 0; i < InFlightCount; ++i)
		{
			if (InFlightFrames[i] == ReturnedFrame)
			{
				InFlightFrames[i] = InFlightFrames[--InFlightCount];
				break;
			}
		}

		FreeFrames[FreeCount++] = ReturnedFrame;
	}
}

FSwuiFullSurfaceFrame* FSwuiFullSurfaceCpuRenderer::AcquireFreeFrame_Locked()
{
	DrainReturnedQueue_Locked();

	if (FreeCount == 0)
	{
		// Steal the oldest in-flight frame.
		if (InFlightCount > 0)
		{
			int32 OldestIdx = 0;
			uint64 MinGen = InFlightFrames[0]->Generation;
			for (int32 i = 1; i < InFlightCount; ++i)
			{
				if (InFlightFrames[i]->Generation < MinGen)
				{
					MinGen = InFlightFrames[i]->Generation;
					OldestIdx = i;
				}
			}

			FSwuiFullSurfaceFrame* Frame = InFlightFrames[OldestIdx];
			InFlightFrames[OldestIdx] = InFlightFrames[--InFlightCount];
			Frame->Generation = 0;
			Frame->PaintTime  = 0.0;
			return Frame;
		}

		return nullptr;
	}

	FSwuiFullSurfaceFrame* Frame = FreeFrames[--FreeCount];
	Frame->Generation = 0;
	Frame->PaintTime  = 0.0;
	return Frame;
}

void FSwuiFullSurfaceCpuRenderer::PublishLatestFrame_Locked(FSwuiFullSurfaceFrame* Frame)
{
	if (LatestReadyFrame)
	{
		FreeFrames[FreeCount++] = LatestReadyFrame;
		Stats.StatInterval_ReplacedReadyFrames++;
	}

	LatestReadyFrame = Frame;
}

FSwuiFullSurfaceFrame* FSwuiFullSurfaceCpuRenderer::ConsumeLatestFrame_Locked()
{
	DrainReturnedQueue_Locked();

	if (!LatestReadyFrame || PaintGeneration <= UploadedGeneration)
	{
		return nullptr;
	}

	FSwuiFullSurfaceFrame* Frame = LatestReadyFrame;
	LatestReadyFrame = nullptr;

	InFlightFrames[InFlightCount++] = Frame;
	UploadedGeneration = Frame->Generation;

	return Frame;
}

// ── StagePaint (CEF renderer thread) ───────────────────────────────────────

void FSwuiFullSurfaceCpuRenderer::StagePaint(
	const void* Buffer,
	int32 InWidth,
	int32 InHeight,
	double PaintArrivalTime)
{
	if (AllocatedWidth == 0 || AllocatedHeight == 0)
	{
		InitializePool(InWidth, InHeight);
	}
	else if (AllocatedWidth != InWidth || AllocatedHeight != InHeight)
	{
		InitializePool(InWidth, InHeight);
	}

	const int32 ByteCount = InWidth * InHeight * 4;
	const double CopyStart = FPlatformTime::Seconds();

	FSwuiFullSurfaceFrame* Frame;
	{
		FScopeLock Lock(&PoolMutex);
		Frame = AcquireFreeFrame_Locked();
	}

	if (!Frame)
	{
		FScopeLock Lock(&PoolMutex);
		Stats.StatInterval_DroppedPaints++;
		Stats.StatTotal_DroppedPaints++;
		return;
	}

	FPlatformMemory::Memcpy(Frame->Pixels.GetData(), Buffer, ByteCount);

	const double CopyMs = (FPlatformTime::Seconds() - CopyStart) * 1000.0;

	Frame->Width      = InWidth;
	Frame->Height     = InHeight;
	Frame->PaintTime  = PaintArrivalTime;

	{
		FScopeLock Lock(&PoolMutex);
		Frame->Generation = ++PaintGeneration;
		PublishLatestFrame_Locked(Frame);

		Stats.StatInterval_CefPaints++;
		Stats.StatInterval_PaintCopySamples++;
		Stats.StatInterval_PaintCopyMsSum += CopyMs;
		if (CopyMs > Stats.StatInterval_PaintCopyMsMax)
		{
			Stats.StatInterval_PaintCopyMsMax = CopyMs;
		}
		bHasEverHadFrame = true;
	}
}

// ── TickUpload (game thread) ───────────────────────────────────────────────

void FSwuiFullSurfaceCpuRenderer::TickUpload(
	FTextureResource* InTextureResource,
	double Now,
	bool bForceEveryTick)
{
	if (!InTextureResource || !InTextureResource->TextureRHI)
	{
		return;
	}

	FRHITexture* TexRHI = InTextureResource->TextureRHI.GetReference();
	if (!TexRHI)
	{
		return;
	}

	// Force-every-tick: re-upload the oldest in-flight frame.
	if (bForceEveryTick)
	{
		FScopeLock Lock(&PoolMutex);
		if (!LatestReadyFrame && InFlightCount > 0)
		{
			int32 OldestIdx = 0;
			uint64 MinGen = InFlightFrames[0]->Generation;
			for (int32 i = 1; i < InFlightCount; ++i)
			{
				if (InFlightFrames[i]->Generation < MinGen)
				{
					MinGen = InFlightFrames[i]->Generation;
					OldestIdx = i;
				}
			}
			LatestReadyFrame = InFlightFrames[OldestIdx];
			InFlightFrames[OldestIdx] = InFlightFrames[--InFlightCount];
		}
	}

	FSwuiFullSurfaceFrame* Frame;
	{
		FScopeLock Lock(&PoolMutex);
		Frame = ConsumeLatestFrame_Locked();
	}

	if (!Frame)
	{
		FScopeLock Lock(&PoolMutex);
		Stats.StatInterval_SkippedNoFreshPaint++;
		return;
	}

	const double EnqueueStart = FPlatformTime::Seconds();
	const int32 FrameWidth  = Frame->Width;
	const int32 FrameHeight = Frame->Height;
	const int32 FramePitch  = FrameWidth * 4;
	const double PaintArrivalTime = Frame->PaintTime;

	ENQUEUE_RENDER_COMMAND(SwuiFullSurfaceUpload)(
		[this, Frame, TexRHI, FrameWidth, FrameHeight, FramePitch](FRHICommandList& RHICmdList)
		{
			if (TexRHI)
			{
				FUpdateTextureRegion2D Region(0, 0, 0, 0, FrameWidth, FrameHeight);
				RHICmdList.UpdateTexture2D(TexRHI, 0, Region, FramePitch, Frame->Pixels.GetData());
			}

			this->ReturnFrameToFree(Frame);
		});

	const double EnqueueMs = (FPlatformTime::Seconds() - EnqueueStart) * 1000.0;

	{
		FScopeLock Lock(&PoolMutex);
		Stats.StatInterval_Uploads++;
		Stats.StatInterval_UploadedPx += int64(FrameWidth) * FrameHeight;
		Stats.StatInterval_EnqueueSamples++;
		Stats.StatInterval_EnqueueMsSum += EnqueueMs;
		if (EnqueueMs > Stats.StatInterval_EnqueueMsMax)
		{
			Stats.StatInterval_EnqueueMsMax = EnqueueMs;
		}

		if (PaintArrivalTime > 0.0)
		{
			const double PaintToUploadMs = (FPlatformTime::Seconds() - PaintArrivalTime) * 1000.0;
			Stats.StatInterval_PaintToUploadSamples++;
			Stats.StatInterval_PaintToUploadMsSum += PaintToUploadMs;
			if (PaintToUploadMs > Stats.StatInterval_PaintToUploadMsMax)
			{
				Stats.StatInterval_PaintToUploadMsMax = PaintToUploadMs;
			}
		}
	}
}

// ── ReturnFrameToFree (RHI thread) ─────────────────────────────────────────
// Lock-free enqueue into MPSC queue. No PoolMutex needed — single producer
// (RHI thread), consumer drains under PoolMutex in _Locked helpers.

void FSwuiFullSurfaceCpuRenderer::ReturnFrameToFree(FSwuiFullSurfaceFrame* Frame)
{
	Frame->Generation = 0;
	Frame->PaintTime  = 0.0;

	ReturnedFrames.Enqueue(Frame);
}

// ── Reset ───────────────────────────────────────────────────────────────────

void FSwuiFullSurfaceCpuRenderer::Reset()
{
	FScopeLock Lock(&PoolMutex);

	FSwuiFullSurfaceFrame* Discard = nullptr;
	while (ReturnedFrames.Dequeue(Discard)) {}

	FreeCount = 0;
	for (int32 i = 0; i < PoolSize; ++i)
	{
		OwnedFrames[i].Pixels.Empty();
		OwnedFrames[i].Width      = 0;
		OwnedFrames[i].Height     = 0;
		OwnedFrames[i].Generation = 0;
		OwnedFrames[i].PaintTime  = 0.0;
	}
	LatestReadyFrame   = nullptr;
	InFlightCount      = 0;
	AllocatedWidth     = 0;
	AllocatedHeight    = 0;
	PaintGeneration    = 0;
	UploadedGeneration = 0;
	bHasEverHadFrame   = false;

	FStats Z = {};
	Stats = Z;
}

// ── Query ──────────────────────────────────────────────────────────────────

bool FSwuiFullSurfaceCpuRenderer::HasFreshPaintPending() const
{
	FScopeLock Lock(&PoolMutex);
	return LatestReadyFrame != nullptr && PaintGeneration > UploadedGeneration;
}

void FSwuiFullSurfaceCpuRenderer::ClearPendingPaint()
{
	FScopeLock Lock(&PoolMutex);
	UploadedGeneration = PaintGeneration;
}

// ── Stats ──────────────────────────────────────────────────────────────────

void FSwuiFullSurfaceCpuRenderer::ResetIntervalStats()
{
	FScopeLock Lock(&PoolMutex);

	Stats.StatInterval_CefPaints            = 0;
	Stats.StatInterval_Uploads              = 0;
	Stats.StatInterval_SkippedNoFreshPaint  = 0;
	Stats.StatInterval_DroppedPaints        = 0;
	Stats.StatInterval_ReplacedReadyFrames  = 0;
	Stats.StatInterval_Allocations          = 0;
	Stats.StatInterval_PaintCopySamples     = 0;
	Stats.StatInterval_EnqueueSamples       = 0;
	Stats.StatInterval_PaintToUploadSamples = 0;
	Stats.StatInterval_UploadedPx           = 0;
	Stats.StatInterval_PaintCopyMsSum       = 0.0;
	Stats.StatInterval_PaintCopyMsMax       = 0.0;
	Stats.StatInterval_EnqueueMsSum         = 0.0;
	Stats.StatInterval_EnqueueMsMax         = 0.0;
	Stats.StatInterval_PaintToUploadMsSum   = 0.0;
	Stats.StatInterval_PaintToUploadMsMax   = 0.0;
}

void FSwuiFullSurfaceCpuRenderer::RefreshPoolSnapshot() const
{
	FScopeLock Lock(&PoolMutex);

	const_cast<FStats&>(Stats).PoolFree     = FreeCount;
	const_cast<FStats&>(Stats).PoolReady    = LatestReadyFrame ? 1 : 0;
	const_cast<FStats&>(Stats).PoolInFlight = InFlightCount;
}
