#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "Input/Events.h"
#include "SwuiEye.h"

#include "SwuiBlueprintFunctionLibrary.generated.h"

UCLASS(ClassGroup = Swui, Blueprintable)
class SWUIRUNTIME_API USwuiBlueprintFunctionLibrary : public UBlueprintFunctionLibrary
{

	GENERATED_UCLASS_BODY()

	UFUNCTION(BlueprintPure, meta = (HidePin = "WorldContextObject", DefaultToSelf = "WorldContextObject", DisplayName = "Create SwuiEye", CompactNodeTitle = "SwuiEye", Keywords = "new create Swui eye SWUI"), Category = Swui)
	static USwuiEye* NewSwuiEye(UObject* WorldContextObject);

	UFUNCTION(BlueprintPure, meta = (HidePin = "WorldContextObject", DefaultToSelf = "WorldContextObject", DisplayName = "Create SwuiJSON Obj", CompactNodeTitle = "JSON", Keywords = "new create Swui eye SWUI json"), Category = Swui)
	static USwuiJsonObj* NewSwuiJsonObj(UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Run SWUI Tick", Keywords = "SWUI Swui eye SWUI tick"), Category = Swui)
	static void RunSwuiEventLoop();

	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Parse JSON String", Keywords = "SWUI Swui eye json parse"), Category = Swui)
	static USwuiJsonObj* ParseJSON(const FString& JSONString);

	UFUNCTION(BlueprintCallable, meta = (DisplayName = "JSON To String", Keywords = "SWUI Swui eye json parse string"), Category = Swui)
	static FString JSONToString(USwuiJsonObj *ObjectToParse);

	/** convert regular key events into char event which you can use char press*/
	UFUNCTION(BlueprintPure, meta = (DisplayName = "To CharacterEvent (Key)", BlueprintAutocast), Category = Swui)
	static FCharacterEvent ToKeyEvent(FKey Key);

	//Utility functions taken from Victory Plugin
	UFUNCTION(BlueprintPure, Category = "Swui Utility")
	static FString GameRootDirectory();

	/**
	* Returns whether or not the SearchIn string contains the supplied Substring.
	* 	Ex: "cat" is a contained within "concatenation" as a substring.
	* @param SearchIn The string to search within
	* @param Substring The string to look for in the SearchIn string
	* @param bUseCase Whether or not to be case-sensitive
	* @param bSearchFromEnd Whether or not to start the search from the end of the string instead of the beginning
	*/
	UFUNCTION(BlueprintPure, Category = "Swui Utility")
	static bool HasSubstring(const FString& SearchIn, const FString& Substring, ESearchCase::Type SearchCase = ESearchCase::IgnoreCase, ESearchDir::Type SearchDir = ESearchDir::FromStart);

};