#include "SwuiCVars.h"

// HUD / frame driving
TAutoConsoleVariable<int32> CVarSwuiHudLockstep(
	TEXT("swui.hud.Lockstep"),
	-1,
	TEXT("Override HUD lockstep mode. -1 = use instance setting, 0 = off, 1 = on."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiHudExternalBeginFrames(
	TEXT("swui.hud.ExternalBeginFrames"),
	-1,
	TEXT("Override external begin frames. -1 = use instance setting, 0 = off, 1 = on."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiHudSendExternalBeginFrameFromTick(
	TEXT("swui.hud.SendExternalBeginFrameFromTick"),
	-1,
	TEXT("Override sending external begin frames from SWUI tick. -1 = use instance setting, 0 = off, 1 = on."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiHudFlushBeforeFrame(
	TEXT("swui.hud.FlushBeforeFrame"),
	-1,
	TEXT("Override HUD flush-before-frame behavior. -1 = use instance setting, 0 = off, 1 = on."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiHudMaxBrowserFPS(
	TEXT("swui.hud.MaxBrowserFPS"),
	-1,
	TEXT("Override HUD browser FPS cap. -1 = use instance setting."),
	ECVF_Default);

// Paint / upload
TAutoConsoleVariable<int32> CVarSwuiPaintHybridDirtyUpload(
	TEXT("swui.paint.HybridDirtyUpload"),
	-1,
	TEXT("Override hybrid dirty upload. -1 = use instance setting, 0 = off, 1 = on."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiPaintTileDiffLargeRects(
	TEXT("swui.paint.TileDiffLargeRects"),
	-1,
	TEXT("Override tile diff for large rects. -1 = use instance setting, 0 = off, 1 = on."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiPaintUploadBudget(
	TEXT("swui.paint.UploadBudget"),
	-1,
	TEXT("Override upload budget enable. -1 = use instance setting, 0 = off, 1 = on."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiPaintMaxNormalUploadBytes(
	TEXT("swui.paint.MaxNormalUploadBytes"),
	-1,
	TEXT("Override normal upload budget in bytes per frame. -1 = use instance setting."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiPaintTileWidth(
	TEXT("swui.paint.TileWidth"),
	-1,
	TEXT("Override tile width. -1 = use instance setting."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiPaintTileHeight(
	TEXT("swui.paint.TileHeight"),
	-1,
	TEXT("Override tile height. -1 = use instance setting."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiPaintMinDirtyRectWidth(
	TEXT("swui.paint.MinDirtyRectWidth"),
	-1,
	TEXT("Override min dirty rect width. -1 = use instance setting."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiPaintMinDirtyRectHeight(
	TEXT("swui.paint.MinDirtyRectHeight"),
	-1,
	TEXT("Override min dirty rect height. -1 = use instance setting."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiPaintCenterCritical(
	TEXT("swui.paint.CenterCritical"),
	-1,
	TEXT("Override center-critical rect processing. -1 = use instance setting, 0 = off, 1 = on."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiPaintCenterCriticalWidth(
	TEXT("swui.paint.CenterCriticalWidth"),
	-1,
	TEXT("Override center-critical rect width. -1 = use instance setting."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiPaintCenterCriticalHeight(
	TEXT("swui.paint.CenterCriticalHeight"),
	-1,
	TEXT("Override center-critical rect height. -1 = use instance setting."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiPaintRotatingCursor(
	TEXT("swui.paint.RotatingCursor"),
	-1,
	TEXT("Override rotating deferred cursor. -1 = use instance setting, 0 = off, 1 = on."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiPaintFullBaseline(
	TEXT("swui.paint.FullBaseline"),
	-1,
	TEXT("Override force full baseline upload on first paint. -1 = use instance setting, 0 = off, 1 = on."),
	ECVF_Default);

// Debug
TAutoConsoleVariable<int32> CVarSwuiDebugLogPaintStats(
	TEXT("swui.debug.LogPaintStats"),
	-1,
	TEXT("Override paint stats logging. -1 = use instance setting, 0 = off, 1 = on."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiDebugShowDirtyRects(
	TEXT("swui.debug.ShowDirtyRects"),
	-1,
	TEXT("Override dirty rect debug overlay. -1 = use instance setting, 0 = off, 1 = on."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiDebugForceFullFrameUploadEveryFrame(
	TEXT("swui.DebugForceFullFrameUploadEveryFrame"),
	0,
	TEXT("Force full-frame upload every UE frame, bypassing dirty rects, tile priorities, ")
	TEXT("center-critical rects, transition heuristics, and upload cooldowns. ")
	TEXT("Also forces browser frame pumping. 0=off, 1=on."),
	ECVF_Default);

// UI resolution presets
TAutoConsoleVariable<int32> CVarSwuiUiResolutionPreset(
	TEXT("swui.hud.UiResolutionPreset"),
	-1,
	TEXT("Override UI resolution preset. -1 = use instance setting, ")
	TEXT("0=720p, 1=900p, 2=1080p, 3=1440p, 4=native, 5=custom."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiCustomUiWidth(
	TEXT("swui.hud.CustomUiWidth"),
	-1,
	TEXT("Override custom UI width when preset is Custom. -1 = use instance setting."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiCustomUiHeight(
	TEXT("swui.hud.CustomUiHeight"),
	-1,
	TEXT("Override custom UI height when preset is Custom. -1 = use instance setting."),
	ECVF_Default);

// Profiling helpers
TAutoConsoleVariable<int32> CVarSwuiNoTextureUpload(
	TEXT("swui.prof.NoTextureUpload"),
	0,
	TEXT("Debug: skip RHIUpdateTexture2D (isolate GPU upload cost). 0=normal, 1=skip upload."),
	ECVF_Default);

TAutoConsoleVariable<int32> CVarSwuiVerbosePaint(
	TEXT("swui.prof.VerbosePaint"),
	0,
	TEXT("Debug: log per-paint upload strategy details. 0=off, 1=on."),
	ECVF_Default);
