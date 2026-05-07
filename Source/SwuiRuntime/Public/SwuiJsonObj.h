#pragma once

#include "SwuiJsonObj.generated.h"

UCLASS(ClassGroup = Swui, Blueprintable)
class SWUIRUNTIME_API USwuiJsonObj : public UObject
{

	GENERATED_UCLASS_BODY()

public:

	//// Get Values ////

	/* Gets a String Value for the key given */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	FString GetStringValue(const FString &Index);

	/* Gets a Numerical Value for the key given */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	float GetNumValue(const FString &Index);

	/* Gets a Boolean Value for the key given */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	bool GetBooleanValue(const FString &Index);

	/* Gets a Nested JSON Object Value for the key given */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	USwuiJsonObj* GetNestedObject(const FString &Index);

	//// Get Array Values ////

	/* Gets an Array of floats or numbers for the key given */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	TArray<float> GetNumArray(const FString &Index);

	/* Gets an Array of booleans for the key given */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	TArray<bool> GetBooleanArray(const FString &Index);

	/* Gets an Array of strings for the key given */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	TArray<FString> GetStringArray(const FString &Index);

	//// Set Values ////

	/* Sets or Adds a String Value to this JSON object */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void SetStringValue(const FString &Value, const FString &Index);

	/* Sets or Adds a Numerical Value to this JSON object */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void SetNumValue(const float Value, const FString &Index);

	/* Sets or Adds a Boolean Value to this JSON object */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void SetBooleanValue(const bool Value, const FString &Index);

	/* Sets or Adds a Nested JSON Object Value to this JSON object */
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void SetNestedObject(USwuiJsonObj *Value, const FString &Index);

	void Init(const FString &dataString);
	void SetJsonObj(TSharedPtr<FJsonObject> NewJson);
	
	TSharedPtr<FJsonObject> GetJsonObj();
	
	// CUSTOM ADDED START
	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void SetStringArray(const TArray<FString> &Value, const FString &Index);

	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void SetBooleanArray(const TArray<bool> &Value, const FString &Index);

	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void SetNumArray(const TArray<float> &Value, const FString &Index);

	UFUNCTION(BlueprintCallable, Category = "SwuiRuntime")
	void SetObjectArray(const TArray<USwuiJsonObj*> &Value, const FString &Index);
	// CUSTOM ADDED END

private:

	FString StrData;
	TSharedPtr<FJsonObject> JsonParsed;

	void DoParseJson(TSharedRef<TJsonReader<TCHAR>> JsonReader);
};