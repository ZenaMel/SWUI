#include "SwuiBlueprintFunctionLibrary.h"
#include "SwuiJsonObj.h"


USwuiBlueprintFunctionLibrary::USwuiBlueprintFunctionLibrary(const class FObjectInitializer& PCIP)
: Super(PCIP)
{

}

USwuiEye* USwuiBlueprintFunctionLibrary::NewSwuiEye(UObject* WorldContextObject)
{

	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	USwuiEye* Eye = NewObject<USwuiEye>(WorldContextObject);

	return Eye;

}

USwuiJsonObj* USwuiBlueprintFunctionLibrary::NewSwuiJsonObj(UObject* WorldContextObject)
{

	USwuiJsonObj* JsonObj = NewObject<USwuiJsonObj>(GetTransientPackage(), USwuiJsonObj::StaticClass());
	JsonObj->Init("{}");
	
	return JsonObj;

}

void USwuiBlueprintFunctionLibrary::RunSwuiEventLoop()
{
	SwuiManager::DoSwuiMessageLoop();
}

USwuiJsonObj* USwuiBlueprintFunctionLibrary::ParseJSON(const FString& JSONString)
{

	USwuiJsonObj* JsonObj = NewObject<USwuiJsonObj>(GetTransientPackage(), USwuiJsonObj::StaticClass());
	JsonObj->Init(JSONString);

	return JsonObj;

}

FString USwuiBlueprintFunctionLibrary::JSONToString(USwuiJsonObj *ObjectToParse)
{

	// Create the JSON reader
	FString ReturnString;
	TSharedRef<TJsonWriter<TCHAR>> writer = TJsonWriterFactory<TCHAR>::Create(&ReturnString);

	// Convert the JSON object to an FString
	FJsonSerializer::Serialize(ObjectToParse->GetJsonObj().ToSharedRef(), writer);

	return ReturnString;

}

FCharacterEvent USwuiBlueprintFunctionLibrary::ToKeyEvent(FKey Key)
{
	FModifierKeysState KeyState;

	FCharacterEvent CharEvent = FCharacterEvent(Key.GetFName().ToString().ToUpper().GetCharArray()[0], KeyState, 0, 0);

	return CharEvent;
}



FString USwuiBlueprintFunctionLibrary::GameRootDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
}


bool USwuiBlueprintFunctionLibrary::HasSubstring(const FString& SearchIn, const FString& Substring, ESearchCase::Type SearchCase /*= ESearchCase::IgnoreCase*/, ESearchDir::Type SearchDir /*= ESearchDir::FromStart*/)
{
	return SearchIn.Contains(Substring, SearchCase, SearchDir);
}