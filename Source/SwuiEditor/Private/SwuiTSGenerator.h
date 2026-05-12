#pragma once

#include "Containers/Array.h"
#include "Containers/Set.h"
#include "UObject/UnrealType.h"
#include "UObject/Field.h"

class USwui;
struct FSwuiNavigationEvent;

// Maps a UE FProperty type to its TypeScript equivalent.
// Returns an empty string for types we don't support syncing.
static FString SwuiGetTSType(const FProperty* Prop)
{
	if (Prop->IsA<FFloatProperty>()  ||
		Prop->IsA<FDoubleProperty>() ||
		Prop->IsA<FIntProperty>()    ||
		Prop->IsA<FInt64Property>())    return TEXT("number");

	if (Prop->IsA<FEnumProperty>())
	{
		if (const FEnumProperty* EnumProp = CastField<const FEnumProperty>(Prop))
		{
			if (const UEnum* EnumDef = EnumProp->GetEnum())
			{
				return EnumDef->GetName();
			}
		}
		return TEXT("enum");
	}

	if (const FByteProperty* ByteProp = CastField<const FByteProperty>(Prop))
	{
		if (ByteProp->Enum)
		{
			return ByteProp->Enum->GetName();
		}
		return TEXT("number");
	}

	if (Prop->IsA<FBoolProperty>())    return TEXT("boolean");

	if (Prop->IsA<FStrProperty>()  ||
		Prop->IsA<FNameProperty>() ||
		Prop->IsA<FTextProperty>()) return TEXT("string");

	if (const FStructProperty* StructProp = CastField<const FStructProperty>(Prop))
	{
		if (!StructProp->Struct) return FString();
		const FString StructName = StructProp->Struct->GetName();
		if (StructName == TEXT("GameplayTag")) return TEXT("GameplayTag");
		if (StructName == TEXT("Vector2D"))   return TEXT("FVector2D");
		if (StructName == TEXT("Vector"))     return TEXT("FVector");
		if (StructName == TEXT("Rotator"))    return TEXT("FRotator");
		if (StructName == TEXT("LinearColor")) return TEXT("FLinearColor");
		if (StructName == TEXT("Color"))      return TEXT("FColor");
		// Generic struct — return reflected name. Recursive child validation
		// happens during struct-type generation in the cpp.
		return StructName;
	}

	if (const FArrayProperty* ArrayProp = CastField<const FArrayProperty>(Prop))
	{
		const FString InnerType = SwuiGetTSType(ArrayProp->Inner);
		return InnerType.IsEmpty() ? FString() : InnerType + TEXT("[]");
	}

	if (const FMapProperty* MapProp = CastField<const FMapProperty>(Prop))
	{
		const FString KeyType = SwuiGetTSType(MapProp->KeyProp);
		const FString ValType = SwuiGetTSType(MapProp->ValueProp);
		if (KeyType != TEXT("string")) return FString();
		return ValType.IsEmpty() ? FString() : TEXT("Record<string, ") + ValType + TEXT(">");
	}

	if (Prop->IsA<FObjectPropertyBase>() && !Prop->IsA<FSoftObjectProperty>() && !Prop->IsA<FSoftClassProperty>())
	{
		return TEXT("SwuiObjectRef");
	}

	if (Prop->IsA<FSoftObjectProperty>() || Prop->IsA<FSoftClassProperty>())
	{
		return TEXT("SwuiAssetRef");
	}

	return FString();
}

class FSwuiTSGenerator
{
public:
	static bool Generate(USwui* Swui);
	static bool GenerateNavigation(USwui* Swui, const TArray<FSwuiNavigationEvent>& NavigationEvents);
};
