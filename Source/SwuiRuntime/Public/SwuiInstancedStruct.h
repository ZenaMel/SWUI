#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "SwuiInstancedStruct.generated.h"

USTRUCT(BlueprintType)
struct FSwuiInstancedStruct
{
	GENERATED_BODY()

	UScriptStruct* GetScriptStruct() const { return StructDef; }
	void* GetMutableMemory() { return Data.GetData(); }
	const void* GetMemory() const { return Data.GetData(); }

	void InitializeAs(UScriptStruct* InStruct, const uint8* SrcData = nullptr)
	{
		StructDef = InStruct;
		if (InStruct)
		{
			const int32 Size = InStruct->GetStructureSize();
			const int32 MinAlign = InStruct->GetMinAlignment();
			Data.SetNum(Size + MinAlign);
			uint8* Aligned = Align(Data.GetData(), MinAlign);
			if (SrcData)
				FMemory::Memcpy(Aligned, SrcData, Size);
			else
				FMemory::Memzero(Aligned, Size);
		}
		else
		{
			Data.Empty();
		}
	}

	void Reset()
	{
		Data.Empty();
		StructDef = nullptr;
	}

	bool IsValid() const { return StructDef != nullptr && Data.Num() > 0; }

	UPROPERTY()
	TArray<uint8> Data;

	UPROPERTY()
	TObjectPtr<UScriptStruct> StructDef = nullptr;
};
