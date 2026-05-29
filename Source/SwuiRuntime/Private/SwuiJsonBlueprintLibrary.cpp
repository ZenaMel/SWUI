#include "SwuiJsonBlueprintLibrary.h"

#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "JsonObjectConverter.h"
#include "StructUtils/InstancedStruct.h"

FInstancedStruct USwuiJsonBlueprintLibrary::JsonToStruct(const FString& JsonPayload, const FString& StructPath)
{
	UScriptStruct* StructDef = FindObject<UScriptStruct>(nullptr, *StructPath);
	if (!StructDef)
	{
		UE_LOG(LogTemp, Warning, TEXT("SWUI: JsonToStruct - Struct '%s' not found."), *StructPath);
		return FInstancedStruct();
	}

	FInstancedStruct Result;
	Result.InitializeAs(StructDef, nullptr);
	void* Mem = Result.GetMutableMemory();
	if (!Mem)
	{
		return Result;
	}

	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonPayload);
	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("SWUI: JsonToStruct - Failed to parse JSON for '%s'. payload=%s"), *StructPath, *JsonPayload);
		Result.Reset();
	}
	else if (!FJsonObjectConverter::JsonObjectToUStruct(JsonObj.ToSharedRef(), StructDef, Mem, 0, 0, false, nullptr))
	{
		UE_LOG(LogTemp, Warning, TEXT("SWUI: JsonToStruct - Failed to deserialize JSON into '%s'. payload=%s"), *StructPath, *JsonPayload);
		Result.Reset();
	}

	return Result;
}
