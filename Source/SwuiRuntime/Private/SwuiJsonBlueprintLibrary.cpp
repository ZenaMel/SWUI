#include "SwuiJsonBlueprintLibrary.h"

#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "JsonObjectConverter.h"

FSwuiInstancedStruct USwuiJsonBlueprintLibrary::JsonToStruct(const FString& JsonPayload, const FString& StructPath)
{
	UScriptStruct* StructDef = FindObject<UScriptStruct>(nullptr, *StructPath);
	if (!StructDef)
	{
		UE_LOG(LogTemp, Warning, TEXT("SWUI: JsonToStruct - Struct '%s' not found."), *StructPath);
		return FSwuiInstancedStruct();
	}

	FSwuiInstancedStruct Result;
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

void USwuiJsonBlueprintLibrary::GetSwuiInstancedStructValue(const FSwuiInstancedStruct& InstancedStruct, int32& OutValue)
{
	if (InstancedStruct.IsValid())
	{
		UScriptStruct* StructDef = InstancedStruct.GetScriptStruct();
		StructDef->CopyScriptStruct(&OutValue, InstancedStruct.GetMemory(), 1);
	}
}

// ── Per-field JSON extractors ──────────────────────────────────────────────

static TSharedPtr<FJsonObject> SwuiParseJsonObject(const FString& JsonPayload)
{
	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonPayload);
	if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
		return JsonObj;
	return nullptr;
}

FString USwuiJsonBlueprintLibrary::ExtractStringField(const FString& JsonPayload, const FString& FieldName)
{
	TSharedPtr<FJsonObject> JsonObj = SwuiParseJsonObject(JsonPayload);
	if (!JsonObj) return FString();
	FString Value;
	JsonObj->TryGetStringField(FieldName, Value);
	return Value;
}

int32 USwuiJsonBlueprintLibrary::ExtractIntField(const FString& JsonPayload, const FString& FieldName)
{
	TSharedPtr<FJsonObject> JsonObj = SwuiParseJsonObject(JsonPayload);
	if (!JsonObj) return 0;
	if (JsonObj->HasTypedField<EJson::Number>(FieldName))
		return (int32)JsonObj->GetNumberField(FieldName);
	return FCString::Atoi(*JsonObj->GetStringField(FieldName));
}

float USwuiJsonBlueprintLibrary::ExtractFloatField(const FString& JsonPayload, const FString& FieldName)
{
	TSharedPtr<FJsonObject> JsonObj = SwuiParseJsonObject(JsonPayload);
	if (!JsonObj) return 0.f;
	if (JsonObj->HasTypedField<EJson::Number>(FieldName))
		return (float)JsonObj->GetNumberField(FieldName);
	return 0.f;
}

bool USwuiJsonBlueprintLibrary::ExtractBoolField(const FString& JsonPayload, const FString& FieldName)
{
	TSharedPtr<FJsonObject> JsonObj = SwuiParseJsonObject(JsonPayload);
	if (!JsonObj) return false;
	bool Value = false;
	JsonObj->TryGetBoolField(FieldName, Value);
	return Value;
}
