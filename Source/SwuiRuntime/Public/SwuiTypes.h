#pragma once

#include "CoreMinimal.h"
#include "SwuiTypes.generated.h"

struct FTickEventLoopData
{
	FTSTicker::FDelegateHandle DelegateHandle;
	int32 EyeCount;
	bool bShouldTickEventLoop;

	FTickEventLoopData()
	{
		DelegateHandle = FTSTicker::FDelegateHandle();
		EyeCount = 0;
		bShouldTickEventLoop = true;
	}
};

struct FSwuiTextureParams
{
	// Pointer to our Texture's resource
	FTexture2DResource* Texture2DResource;
};

struct FUpdateTextureRegionsData
{
	FTextureResource* Texture2DResource;
	uint32 NumRegions;
	FUpdateTextureRegion2D* Regions;
	uint32 SrcPitch;
	uint32 SrcBpp;
	TArray<uint8> SrcData;
};

UENUM(BlueprintType)
enum ESwuiSpecialKeys
{
	backspacekey = 8 UMETA(DisplayName = "Backspace"),
	tabkey = 9 UMETA(DisplayName = "Tab"),
	enterkey = 13 UMETA(DisplayName = "Enter"),
	pausekey = 19 UMETA(DisplayName = "Pause"),
	escapekey = 27 UMETA(DisplayName = "Escape"),
	pageupkey = 33 UMETA(DisplayName = "Page Up"),
	pagedownkey = 34 UMETA(DisplayName = "Page Down"),
	endkey = 35 UMETA(DisplayName = "End"),
	homekey = 36 UMETA(DisplayName = "Home"),
	leftarrowkey = 37 UMETA(DisplayName = "Left Arrow"),
	rightarrowkey = 39 UMETA(DisplayName = "Right Arrow"),
	downarrowkey = 40 UMETA(DisplayName = "Down Arrow"),
	uparrowkey = 38 UMETA(DisplayName = "Up Arrow"),
	insertkey = 45 UMETA(DisplayName = "Insert"),
	deletekey = 46 UMETA(DisplayName = "Delete"),
	numlockkey = 144 UMETA(DisplayName = "Num Lock"),
	scrolllockkey = 145 UMETA(DisplayName = "Scroll Lock"),
	unknownkey = 0,
};


USTRUCT(BlueprintType)
struct FSwuiEyeSettings
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SwuiSettings")
	float FrameRate;

	/** Should this be rendered in game to be transparent? */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SwuiRuntime")
	bool bIsTransparent;

	/** Width(X) and Height(Y) of the view resolution */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SwuiRuntime")
	FVector2D ViewSize;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SwuiRuntime")
	bool bEnableWebGL;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SwuiRuntime")
	bool bAudioMuted;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SwuiRuntime")
	bool bAutoPlayEnabled;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SwuiRuntime")
	bool bDebugLogTick;

	FSwuiEyeSettings();
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FScriptEvent, const FString&, EventName, const FString&, EventMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLogEvent, const FString&, LogText);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FDownloadCompleteSignature, FString, url);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FDownloadUpdatedSignature, FString, url, float, percentage);
//DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDownloadComplete);