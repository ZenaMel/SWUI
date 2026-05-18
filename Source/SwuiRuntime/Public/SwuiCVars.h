#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

// HUD / frame driving
extern TAutoConsoleVariable<int32> CVarSwuiHudLockstep;
extern TAutoConsoleVariable<int32> CVarSwuiHudExternalBeginFrames;
extern TAutoConsoleVariable<int32> CVarSwuiHudSendExternalBeginFrameFromTick;
extern TAutoConsoleVariable<int32> CVarSwuiHudFlushBeforeFrame;
extern TAutoConsoleVariable<int32> CVarSwuiHudMaxBrowserFPS;

// Paint / upload
extern TAutoConsoleVariable<int32> CVarSwuiPaintHybridDirtyUpload;
extern TAutoConsoleVariable<int32> CVarSwuiPaintTileDiffLargeRects;
extern TAutoConsoleVariable<int32> CVarSwuiPaintUploadBudget;
extern TAutoConsoleVariable<int32> CVarSwuiPaintMaxNormalUploadBytes;
extern TAutoConsoleVariable<int32> CVarSwuiPaintTileWidth;
extern TAutoConsoleVariable<int32> CVarSwuiPaintTileHeight;
extern TAutoConsoleVariable<int32> CVarSwuiPaintMinDirtyRectWidth;
extern TAutoConsoleVariable<int32> CVarSwuiPaintMinDirtyRectHeight;
extern TAutoConsoleVariable<int32> CVarSwuiPaintCenterCritical;
extern TAutoConsoleVariable<int32> CVarSwuiPaintCenterCriticalWidth;
extern TAutoConsoleVariable<int32> CVarSwuiPaintCenterCriticalHeight;
extern TAutoConsoleVariable<int32> CVarSwuiPaintRotatingCursor;
extern TAutoConsoleVariable<int32> CVarSwuiPaintFullBaseline;

// Debug
extern TAutoConsoleVariable<int32> CVarSwuiDebugLogPaintStats;
extern TAutoConsoleVariable<int32> CVarSwuiDebugShowDirtyRects;
extern TAutoConsoleVariable<int32> CVarSwuiDebugForceFullFrameUploadEveryFrame;

// Profiling helpers
extern TAutoConsoleVariable<int32> CVarSwuiNoTextureUpload;
extern TAutoConsoleVariable<int32> CVarSwuiVerbosePaint;
