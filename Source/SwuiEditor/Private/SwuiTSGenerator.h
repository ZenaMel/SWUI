#pragma once

#include "UObject/UnrealType.h"
#include "UObject/Field.h"

class USwui;

// Maps a UE FProperty type to its TypeScript equivalent.
// Returns an empty string for types we don't support syncing.
static FString SwuiGetTSType(const FProperty* Prop)
{
	if (Prop->IsA<FFloatProperty>()  ||
		Prop->IsA<FDoubleProperty>() ||
		Prop->IsA<FIntProperty>()    ||
		Prop->IsA<FInt64Property>()  ||
		Prop->IsA<FByteProperty>())    return TEXT("number");

	if (Prop->IsA<FBoolProperty>())    return TEXT("boolean");

	if (Prop->IsA<FStrProperty>()  ||
		Prop->IsA<FNameProperty>() ||
		Prop->IsA<FTextProperty>()) return TEXT("string");

	return FString();
}

class FSwuiTSGenerator
{
public:
	// Generates Content/UI/generated/<InterfaceName>.generated.ts from the
	// component's ExposedProperties list. Safe to call from any editor context.
	static void Generate(USwui* Swui);
};
