#include "SwuiJsonObj.h"
#include "ISwuiRuntime.h"
#include "Json.h"

USwuiJsonObj::USwuiJsonObj(const class FObjectInitializer& PCIP)
: Super(PCIP)
{

}

void USwuiJsonObj::Init(const FString &StringData)
{
	StrData = *StringData;

	TSharedRef<TJsonReader<TCHAR>> JsonReader = TJsonReaderFactory<TCHAR>::Create(StringData);
	DoParseJson(JsonReader);
}

FString USwuiJsonObj::GetStringValue(const FString& Index)
{
	return JsonParsed->GetStringField(Index);
}

bool USwuiJsonObj::GetBooleanValue(const FString &Index)
{
	return JsonParsed->GetBoolField(Index);
}

float USwuiJsonObj::GetNumValue(const FString &Index)
{
	return JsonParsed->GetNumberField(Index);
}

USwuiJsonObj* USwuiJsonObj::GetNestedObject(const FString &Index)
{
	TSharedPtr<FJsonObject> NewJson = JsonParsed->GetObjectField(Index);

	if (!NewJson.IsValid())
	{
		return nullptr;
	}

	// Make our new Temp obj
	USwuiJsonObj* TempObj = NewObject<USwuiJsonObj>(GetTransientPackage(), USwuiJsonObj::StaticClass());
	TempObj->SetJsonObj(NewJson);

	// return it
	return TempObj;
}

TArray<float> USwuiJsonObj::GetNumArray(const FString &Index)
{
	TArray<float> Temp;

	for (TSharedPtr<FJsonValue> Val : JsonParsed->GetArrayField(Index))
	{

		Temp.Add(Val->AsNumber());

	}

	return Temp;
}

TArray<bool> USwuiJsonObj::GetBooleanArray(const FString &Index)
{
	TArray<bool> Temp;

	for (TSharedPtr<FJsonValue> Val : JsonParsed->GetArrayField(Index))
	{

		Temp.Add(Val->AsBool());

	}

	return Temp;
}

TArray<FString> USwuiJsonObj::GetStringArray(const FString &Index)
{
	TArray<FString> Temp;

	for (TSharedPtr<FJsonValue> Val : JsonParsed->GetArrayField(Index))
	{

		Temp.Add(Val->AsString());

	}

	return Temp;
}


void USwuiJsonObj::SetStringValue(const FString &Value, const FString &Index)
{
	JsonParsed->SetStringField(Index, Value);
}

void USwuiJsonObj::SetNumValue(const float Value, const FString &Index)
{
	JsonParsed->SetNumberField(Index, Value);
}

void USwuiJsonObj::SetBooleanValue(const bool Value, const FString &Index)
{
	JsonParsed->SetBoolField(Index, Value);
}

void USwuiJsonObj::SetNestedObject(USwuiJsonObj *Value, const FString &Index)
{
	JsonParsed->SetObjectField(Index, Value->GetJsonObj());
}

void USwuiJsonObj::SetJsonObj(TSharedPtr<FJsonObject> NewJson)
{
	// Set our new stored JSON object
	JsonParsed = NewJson;
}

TSharedPtr<FJsonObject> USwuiJsonObj::GetJsonObj()
{
	return JsonParsed;
}

void USwuiJsonObj::DoParseJson(TSharedRef<TJsonReader<TCHAR>> JsonReader)
{
	if (!FJsonSerializer::Deserialize(JsonReader, JsonParsed))
	{
		UE_LOG(LogSwuiRuntime, Warning, TEXT("JSON STRING FAILED TO PARSE! WILL DEFAULT TO EMPTY OBJECT {}"));

		// Make an empty json object to prevent crashing
		DoParseJson(TJsonReaderFactory<TCHAR>::Create("{}"));
	}
}

// CUSTOM ADDED START
void USwuiJsonObj::SetStringArray(const TArray<FString> &Value, const FString &Index)
{
	TArray<TSharedPtr<FJsonValue>> ValueArray;

	for (FString Val : Value)
	{
		ValueArray.Add(MakeShareable(new FJsonValueString(Val)));
	}

	JsonParsed->SetArrayField(Index, ValueArray);
}

void USwuiJsonObj::SetBooleanArray(const TArray<bool> &Value, const FString &Index)
{
	TArray<TSharedPtr<FJsonValue>> ValueArray;

	for (bool Val : Value)
	{
		ValueArray.Add(MakeShareable(new FJsonValueBoolean(Val)));
	}

	JsonParsed->SetArrayField(Index, ValueArray);
}

void USwuiJsonObj::SetNumArray(const TArray<float> &Value, const FString &Index)
{
	TArray<TSharedPtr<FJsonValue>> ValueArray;

	for (float Val : Value)
	{
		ValueArray.Add(MakeShareable(new FJsonValueNumber(Val)));
	}

	JsonParsed->SetArrayField(Index, ValueArray);
}

void USwuiJsonObj::SetObjectArray(const TArray<USwuiJsonObj*> &Value, const FString &Index)
{
	TArray<TSharedPtr<FJsonValue>> ValueArray;

	for (USwuiJsonObj* Val : Value)
	{
		ValueArray.Add(MakeShareable(new FJsonValueObject(Val->GetJsonObj())));
	}

	JsonParsed->SetArrayField(Index, ValueArray);
}
// CUSTOM ADDED END
