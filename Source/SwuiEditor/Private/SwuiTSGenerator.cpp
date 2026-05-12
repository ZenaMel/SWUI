#include "SwuiTSGenerator.h"
#include "SwuiNavigation.h"
#include "Swui.h"
#include "SwuiBindingSource.h"
#include "SwuiTypes.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"

static FString SwuiComputeNamespace(UClass* SourceClass)
{
	if (!SourceClass) return TEXT("swui");
	FString Name = SourceClass->GetName();
	if (Name.StartsWith(TEXT("A")) || Name.StartsWith(TEXT("U")))
		Name = Name.RightChop(1);
	return Name.ToLower();
}

// Object name for the generated TS export: strip A/U prefix and _C suffix, keep PascalCase.
static FString SwuiComputeObjectName(UClass* SourceClass)
{
	if (!SourceClass) return TEXT("Swui");
	FString Name = SourceClass->GetName();
	// Strip Blueprint-generated class suffix
	if (Name.EndsWith(TEXT("_C")))
		Name = Name.LeftChop(2);
	// Strip leading A/U only if followed by an uppercase letter (avoid mangling e.g. "UE4")
	if ((Name.StartsWith(TEXT("A")) || Name.StartsWith(TEXT("U"))) && Name.Len() > 1 && FChar::IsUpper(Name[1]))
		Name = Name.RightChop(1);
	return Name;
}

static FString SwuiMakeIdentifierSegment(const FString& Raw)
{
	FString Result;
	bool bUpperNext = true;

	for (TCHAR Char : Raw)
	{
		if (!FChar::IsAlnum(Char))
		{
			bUpperNext = true;
			continue;
		}

		Result.AppendChar(bUpperNext ? FChar::ToUpper(Char) : Char);
		bUpperNext = false;
	}

	if (Result.IsEmpty())
	{
		Result = TEXT("Event");
	}

	if (!FChar::IsAlpha(Result[0]) && Result[0] != TEXT('_'))
	{
		Result = TEXT("Event") + Result;
	}

	return Result;
}

static FString SwuiMakeNavigationIdentifier(const FString& EventName)
{
	TArray<FString> Segments;
	EventName.ParseIntoArray(Segments, TEXT("."), true);

	if (Segments.Num() > 0 && Segments[0].Equals(TEXT("swui"), ESearchCase::IgnoreCase))
	{
		Segments.RemoveAt(0);
	}

	FString Identifier;
	for (const FString& Segment : Segments)
	{
		Identifier += SwuiMakeIdentifierSegment(Segment);
	}

	if (Identifier.IsEmpty())
	{
		Identifier = SwuiMakeIdentifierSegment(EventName);
	}

	return Identifier;
}

static FString SwuiMakeNavigationConstantIdentifier(const FString& EventName)
{
	FString Identifier = SwuiMakeNavigationIdentifier(EventName);
	if (!Identifier.IsEmpty())
	{
		Identifier[0] = FChar::ToLower(Identifier[0]);
	}
	return Identifier;
}

static FString SwuiEscapeTsStringLiteral(const FString& Value)
{
	FString Escaped = Value;
	Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Escaped.ReplaceInline(TEXT("'"), TEXT("\\'"));
	Escaped.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Escaped.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	Escaped.ReplaceInline(TEXT("\t"), TEXT("\\t"));
	return Escaped;
}

static FString SwuiQuotePreviewString(const FString& Value)
{
	return TEXT("'") + SwuiEscapeTsStringLiteral(Value) + TEXT("'");
}

static bool SwuiIsExcludedNavigationPlaceholder(const FString& TagName)
{
	return TagName == TEXT("swui.menu")
		|| TagName == TEXT("swui.navigation")
		|| TagName == TEXT("swui.pointer");
}

struct FSwuiGeneratedNavInfo
{
	FString Identifier;
	FString HandlerSuffix;
	FString EventName;
	FString Category;
	bool bDefaultEvent = false;
};

struct FSwuiGeneratedPropertyInfo
{
	FString ObjectName;
	FString Namespace;
	FString FullKey;
	FString PropName;
	FString TSType;
	FString Label;
	FString ContractType;
	FString DefaultValueLiteral;
	FString MinVal;
	FString MaxVal;
	FString StepVal;
	TArray<FString> EnumOptions;
	const void* StructDef = nullptr; // UScriptStruct* for generic struct props
};

struct FSwuiGeneratedEventInfo
{
	FString ObjectName;
	FString Namespace;
	FString FullKey;
	FString DelegateName;
	FString PayloadBody;
};

struct FSwuiGeneratedSourceEntry
{
	FString ObjectName;
	FString Namespace;
	TArray<FSwuiGeneratedPropertyInfo> Props;
	TArray<FSwuiGeneratedEventInfo> Events;
};

struct FSwuiCollectedEnum
{
	const UEnum* EnumDef = nullptr;
	FString EnumPath;
	FString ShortName;
	TArray<FString> SourceNames;
	TArray<FString> ValueNames;
};

struct FSwuiResolvedEnum
{
	const UEnum* EnumDef = nullptr;
	FString EnumPath;
	FString ShortName;
	FString PrimaryCanonicalName;
	bool bShortNameUnique;
	TArray<FString> ValueNames;
	TMap<FString, FString> SourceCanonicalNames;
};

static TArray<FString> SwuiBuildStructChildFieldTypes(UScriptStruct* Struct); // forward

static FString SwuiGetNavigationCategory(const FString& EventName, bool bDefaultEvent)
{
	if (!bDefaultEvent)
	{
		return TEXT("custom");
	}

	TArray<FString> Segments;
	EventName.ParseIntoArray(Segments, TEXT("."), true);
	if (Segments.Num() >= 2 && Segments[0].Equals(TEXT("swui"), ESearchCase::IgnoreCase))
	{
		return Segments[1].ToLower();
	}

	return TEXT("custom");
}

static bool SwuiTryGetEnumDefinition(const FProperty* Prop, const UEnum*& OutEnum)
{
	OutEnum = nullptr;

	if (const FEnumProperty* EnumProp = CastField<const FEnumProperty>(Prop))
	{
		OutEnum = EnumProp->GetEnum();
		return OutEnum != nullptr;
	}

	if (const FByteProperty* ByteProp = CastField<const FByteProperty>(Prop))
	{
		OutEnum = ByteProp->Enum;
		return OutEnum != nullptr;
	}

	return false;
}

static bool SwuiShouldSkipEnumValue(const UEnum* EnumDef, int32 Index)
{
	if (!EnumDef) return true;

	const FString ValueName = EnumDef->GetNameStringByIndex(Index);
	const FString EnumName  = EnumDef->GetName();

	return EnumDef->HasMetaData(TEXT("Hidden"), Index)
		|| ValueName.IsEmpty()
		|| ValueName == EnumName + TEXT("_MAX");
}

static void SwuiCollectEnumFromProperty(const FProperty* Prop, const FString& ObjectName, TMap<FString, FSwuiCollectedEnum>& OutEnums)
{
	const UEnum* EnumDef = nullptr;
	if (!SwuiTryGetEnumDefinition(Prop, EnumDef) || !EnumDef) return;

	const FString EnumPath = EnumDef->GetPathName();
	const FString ShortName = EnumDef->GetName();

	FSwuiCollectedEnum& Entry = OutEnums.FindOrAdd(EnumPath);

	if (!Entry.EnumDef)
	{
		Entry.EnumDef = EnumDef;
		Entry.EnumPath = EnumPath;
		Entry.ShortName = ShortName;
		for (int32 i = 0; i < EnumDef->NumEnums(); ++i)
		{
			if (SwuiShouldSkipEnumValue(EnumDef, i)) continue;
			Entry.ValueNames.Add(EnumDef->GetNameStringByIndex(i));
		}
	}

	Entry.SourceNames.AddUnique(ObjectName);
}

static TArray<FString> SwuiBuildStructChildFieldTypes(UScriptStruct* Struct)
{
	TArray<FString> FieldTypes;
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		const FString FieldTSType = SwuiGetTSType(*It);
		if (FieldTSType.IsEmpty()) continue;
		FieldTypes.Add(It->GetName() + TEXT(":") + FieldTSType);
	}
	return FieldTypes;
}

static FString SwuiBuildStructTypeBody(UScriptStruct* Struct)
{
	TArray<FString> FieldTypes = SwuiBuildStructChildFieldTypes(Struct);
	TArray<FString> Lines;
	for (const FString& FT : FieldTypes)
	{
		FString FieldName, FieldType;
		FT.Split(TEXT(":"), &FieldName, &FieldType);
		Lines.Add(FString::Printf(TEXT("\t%s: %s;"), *FieldName, *FieldType));
	}
	return FString::Join(Lines, TEXT("\n"));
}

static bool SwuiIsGenericStruct(const FStructProperty* StructProp)
{
	if (!StructProp || !StructProp->Struct) return false;
	const FString Name = StructProp->Struct->GetName();
	return Name != TEXT("GameplayTag")
		&& Name != TEXT("Vector2D")
		&& Name != TEXT("Vector")
		&& Name != TEXT("Rotator")
		&& Name != TEXT("LinearColor")
		&& Name != TEXT("Color");
}

static FString SwuiReadFirstMetadataValue(const FProperty* Prop, std::initializer_list<const TCHAR*> Keys)
{
	for (const TCHAR* Key : Keys)
	{
		if (Prop->HasMetaData(Key))
		{
			const FString Value = Prop->GetMetaData(Key).TrimStartAndEnd();
			if (!Value.IsEmpty())
			{
				return Value;
			}
		}
	}

	return FString();
}

static FString SwuiFormatNumericLiteral(double Value, bool bInteger)
{
	if (bInteger)
	{
		return FString::Printf(TEXT("%lld"), static_cast<long long>(Value));
	}

	FString Literal = FString::SanitizeFloat(Value);
	if (!Literal.Contains(TEXT(".")) && !Literal.Contains(TEXT("e")) && !Literal.Contains(TEXT("E")))
	{
		Literal += TEXT(".0");
	}

	return Literal;
}

static void SwuiApplyNumericFallbacks(const FString& FieldName, bool bInteger, FString& InOutMin, FString& InOutMax, FString& InOutStep)
{
	const FString Name = FieldName.ToLower();

	if (InOutStep.IsEmpty())
	{
		InOutStep = bInteger ? TEXT("1") : TEXT("0.1");
	}

	if (Name.Contains(TEXT("angle")) || Name.Contains(TEXT("yaw")))
	{
		if (InOutMin.IsEmpty()) InOutMin = TEXT("0");
		if (InOutMax.IsEmpty()) InOutMax = TEXT("360");
		if (InOutStep.IsEmpty() || InOutStep == TEXT("0.1")) InOutStep = TEXT("1");
		return;
	}

	if (Name.Contains(TEXT("percent")) || Name.Contains(TEXT("percentage")))
	{
		if (InOutMin.IsEmpty()) InOutMin = TEXT("0");
		if (InOutMax.IsEmpty()) InOutMax = TEXT("100");
		if (InOutStep.IsEmpty() || InOutStep == TEXT("0.1")) InOutStep = TEXT("1");
		return;
	}

	if (Name.Contains(TEXT("ammo")) || Name.Contains(TEXT("count")) || Name.Contains(TEXT("size")) || Name.Contains(TEXT("reserve")))
	{
		if (InOutMin.IsEmpty()) InOutMin = TEXT("0");
		if (InOutMax.IsEmpty()) InOutMax = TEXT("999");
		if (InOutStep.IsEmpty() || InOutStep == TEXT("0.1")) InOutStep = TEXT("1");
	}
}

static void SwuiPopulateFieldMetadata(
	const UObject* DefaultsObject,
	const FProperty* Prop,
	const FString& FieldName,
	FString& OutType,
	FString& OutDefaultValueLiteral,
	FString& OutMinVal,
	FString& OutMaxVal,
	FString& OutStepVal,
	TArray<FString>& OutEnumOptions)
{
	OutType.Reset();
	OutDefaultValueLiteral.Reset();
	OutMinVal.Reset();
	OutMaxVal.Reset();
	OutStepVal.Reset();
	OutEnumOptions.Reset();

	const UEnum* EnumDefinition = nullptr;
	if (SwuiTryGetEnumDefinition(Prop, EnumDefinition) && EnumDefinition)
	{
		OutType = TEXT("enum");

		int64 EnumValue = 0;
		if (DefaultsObject)
		{
			if (const FEnumProperty* EnumProp = CastField<const FEnumProperty>(Prop))
			{
				const void* ValuePtr = EnumProp->ContainerPtrToValuePtr<void>(DefaultsObject);
				EnumValue = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			}
			else if (const FByteProperty* ByteProp = CastField<const FByteProperty>(Prop))
			{
				const void* ValuePtr = ByteProp->ContainerPtrToValuePtr<void>(DefaultsObject);
				EnumValue = ByteProp->GetSignedIntPropertyValue(ValuePtr);
			}
		}

		OutDefaultValueLiteral = SwuiQuotePreviewString(EnumDefinition->GetNameStringByValue(EnumValue));

		for (int32 Index = 0; Index < EnumDefinition->NumEnums(); ++Index)
		{
			if (SwuiShouldSkipEnumValue(EnumDefinition, Index))
			{
				continue;
			}

			OutEnumOptions.Add(SwuiQuotePreviewString(EnumDefinition->GetNameStringByIndex(Index)));
		}

		return;
	}

	if (const FBoolProperty* BoolProp = CastField<const FBoolProperty>(Prop))
	{
		OutType = TEXT("boolean");
		const bool bDefaultValue = DefaultsObject
			? BoolProp->GetPropertyValue_InContainer(DefaultsObject)
			: false;
		OutDefaultValueLiteral = bDefaultValue ? TEXT("true") : TEXT("false");
		return;
	}

	if (const FStrProperty* StrProp = CastField<const FStrProperty>(Prop))
	{
		OutType = TEXT("string");
		const FString DefaultValue = DefaultsObject
			? StrProp->GetPropertyValue_InContainer(DefaultsObject)
			: FString();
		OutDefaultValueLiteral = SwuiQuotePreviewString(DefaultValue);
		return;
	}

	if (const FNameProperty* NameProp = CastField<const FNameProperty>(Prop))
	{
		OutType = TEXT("string");
		const FName DefaultValue = DefaultsObject
			? NameProp->GetPropertyValue_InContainer(DefaultsObject)
			: NAME_None;
		OutDefaultValueLiteral = SwuiQuotePreviewString(DefaultValue.ToString());
		return;
	}

	if (const FTextProperty* TextProp = CastField<const FTextProperty>(Prop))
	{
		OutType = TEXT("string");
		const FText DefaultValue = DefaultsObject
			? TextProp->GetPropertyValue_InContainer(DefaultsObject)
			: FText::GetEmpty();
		OutDefaultValueLiteral = SwuiQuotePreviewString(DefaultValue.ToString());
		return;
	}

	if (const FStructProperty* StructProp = CastField<const FStructProperty>(Prop))
	{
		const FString StructName = StructProp->Struct->GetName();
		if (StructName == TEXT("GameplayTag"))
		{
			OutType = TEXT("string");
			OutDefaultValueLiteral = TEXT("''");
			return;
		}
	}

	if (const FNumericProperty* NumericProp = CastField<const FNumericProperty>(Prop))
	{
		const bool bInteger = NumericProp->IsInteger();
		OutType = TEXT("number");
		if (DefaultsObject)
		{
			const void* ValuePtr = NumericProp->ContainerPtrToValuePtr<void>(DefaultsObject);
			OutDefaultValueLiteral = bInteger
				? SwuiFormatNumericLiteral(static_cast<double>(NumericProp->GetSignedIntPropertyValue(ValuePtr)), true)
				: SwuiFormatNumericLiteral(NumericProp->GetFloatingPointPropertyValue(ValuePtr), false);
		}
		else
		{
			OutDefaultValueLiteral = bInteger ? TEXT("0") : TEXT("0.0");
		}

		OutMinVal = SwuiReadFirstMetadataValue(Prop, { TEXT("ClampMin"), TEXT("UIMin") });
		OutMaxVal = SwuiReadFirstMetadataValue(Prop, { TEXT("ClampMax"), TEXT("UIMax") });
		OutStepVal = SwuiReadFirstMetadataValue(Prop, { TEXT("Delta"), TEXT("Multiple"), TEXT("Step") });
		SwuiApplyNumericFallbacks(FieldName, bInteger, OutMinVal, OutMaxVal, OutStepVal);
		return;
	}

	if (const FStructProperty* StructProp = CastField<const FStructProperty>(Prop))
	{
		OutType = TEXT("object");
		OutDefaultValueLiteral = TEXT("null");
		return;
	}

	if (const FArrayProperty* ArrayProp = CastField<const FArrayProperty>(Prop))
	{
		OutType = TEXT("array");
		OutDefaultValueLiteral = TEXT("null");
		return;
	}

	if (const FMapProperty* MapProp = CastField<const FMapProperty>(Prop))
	{
		OutType = TEXT("map");
		OutDefaultValueLiteral = TEXT("null");
		return;
	}

	if (Prop->IsA<FObjectPropertyBase>())
	{
		OutType = TEXT("object");
		OutDefaultValueLiteral = TEXT("null");
		return;
	}

	if (Prop->IsA<FSoftObjectProperty>() || Prop->IsA<FSoftClassProperty>())
	{
		OutType = TEXT("object");
		OutDefaultValueLiteral = TEXT("null");
		return;
	}

	OutType = TEXT("unknown");
	OutDefaultValueLiteral = TEXT("null");
}

static FString SwuiBuildPayloadFieldsBody(const UFunction* SignatureFunction, const FString& Indent)
{
	if (!SignatureFunction)
	{
		return FString();
	}

	FString PayloadBody;
	for (TFieldIterator<const FProperty> It(SignatureFunction); It; ++It)
	{
		const FProperty* Param = *It;
		if (!Param->HasAnyPropertyFlags(CPF_Parm) || Param->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			continue;
		}

		FString FieldType;
		FString DefaultValueLiteral;
		FString MinVal;
		FString MaxVal;
		FString StepVal;
		TArray<FString> EnumOptions;
		const FString ParamName = Param->GetName();
		SwuiPopulateFieldMetadata(nullptr, Param, ParamName, FieldType, DefaultValueLiteral, MinVal, MaxVal, StepVal, EnumOptions);

		if (FieldType.IsEmpty() || FieldType == TEXT("unknown"))
		{
			continue;
		}

		PayloadBody += FString::Printf(TEXT("%s%s: {\n"), *Indent, *SwuiQuotePreviewString(ParamName));
		PayloadBody += FString::Printf(TEXT("%s\tlabel: %s,\n"), *Indent, *SwuiQuotePreviewString(ParamName));
		PayloadBody += FString::Printf(TEXT("%s\ttype: %s,\n"), *Indent, *SwuiQuotePreviewString(FieldType));
		PayloadBody += FString::Printf(TEXT("%s\tdefaultValue: %s,\n"), *Indent, *DefaultValueLiteral);
		if (!MinVal.IsEmpty()) PayloadBody += FString::Printf(TEXT("%s\tmin: %s,\n"), *Indent, *MinVal);
		if (!MaxVal.IsEmpty()) PayloadBody += FString::Printf(TEXT("%s\tmax: %s,\n"), *Indent, *MaxVal);
		if (!StepVal.IsEmpty()) PayloadBody += FString::Printf(TEXT("%s\tstep: %s,\n"), *Indent, *StepVal);
		if (FieldType == TEXT("enum") && EnumOptions.Num() > 0)
		{
			PayloadBody += FString::Printf(TEXT("%s\toptions: [%s],\n"), *Indent, *FString::Join(EnumOptions, TEXT(", ")));
		}
		PayloadBody += FString::Printf(TEXT("%s},\n"), *Indent);
	}

	return PayloadBody;
}

static FString SwuiBuildContractStateBody(const TArray<FSwuiGeneratedSourceEntry>& Sources)
{
	FString StateBody;
	for (const FSwuiGeneratedSourceEntry& Source : Sources)
	{
		for (const FSwuiGeneratedPropertyInfo& Prop : Source.Props)
		{
			StateBody += FString::Printf(TEXT("\t\t[%s.%s]: {\n"), *Prop.ObjectName, *Prop.PropName);
			StateBody += FString::Printf(TEXT("\t\t\tlabel: %s,\n"), *SwuiQuotePreviewString(Prop.Label));
			StateBody += FString::Printf(TEXT("\t\t\tsource: %s,\n"), *SwuiQuotePreviewString(Prop.ObjectName));
			StateBody += FString::Printf(TEXT("\t\t\tproperty: %s,\n"), *SwuiQuotePreviewString(Prop.PropName));
			StateBody += FString::Printf(TEXT("\t\t\ttype: %s,\n"), *SwuiQuotePreviewString(Prop.ContractType));
			StateBody += FString::Printf(TEXT("\t\t\tdefaultValue: %s,\n"), *Prop.DefaultValueLiteral);
			if (!Prop.MinVal.IsEmpty()) StateBody += FString::Printf(TEXT("\t\t\tmin: %s,\n"), *Prop.MinVal);
			if (!Prop.MaxVal.IsEmpty()) StateBody += FString::Printf(TEXT("\t\t\tmax: %s,\n"), *Prop.MaxVal);
			if (!Prop.StepVal.IsEmpty()) StateBody += FString::Printf(TEXT("\t\t\tstep: %s,\n"), *Prop.StepVal);
			if (Prop.ContractType == TEXT("enum") && Prop.EnumOptions.Num() > 0)
			{
				StateBody += FString::Printf(TEXT("\t\t\toptions: [%s],\n"), *FString::Join(Prop.EnumOptions, TEXT(", ")));
			}
			StateBody += TEXT("\t\t},\n");
		}
	}

	return StateBody;
}

static FString SwuiBuildContractEventsBody(const TArray<FSwuiGeneratedSourceEntry>& Sources)
{
	FString EventsBody;
	for (const FSwuiGeneratedSourceEntry& Source : Sources)
	{
		for (const FSwuiGeneratedEventInfo& Event : Source.Events)
		{
			EventsBody += FString::Printf(TEXT("\t\t%s: {\n"), *SwuiQuotePreviewString(Event.FullKey));
			EventsBody += FString::Printf(TEXT("\t\t\tlabel: %s,\n"), *SwuiQuotePreviewString(Event.DelegateName));
			EventsBody += FString::Printf(TEXT("\t\t\tsource: %s,\n"), *SwuiQuotePreviewString(Event.ObjectName));
			EventsBody += FString::Printf(TEXT("\t\t\tevent: %s,\n"), *SwuiQuotePreviewString(Event.DelegateName));
			if (Event.PayloadBody.IsEmpty())
			{
				EventsBody += TEXT("\t\t\tpayload: {},\n");
			}
			else
			{
				EventsBody += TEXT("\t\t\tpayload: {\n");
				EventsBody += Event.PayloadBody;
				EventsBody += TEXT("\t\t\t},\n");
			}
			EventsBody += TEXT("\t\t},\n");
		}
	}

	return EventsBody;
}

static TArray<FSwuiGeneratedNavInfo> SwuiCollectGeneratedNavigationEvents(const TArray<FSwuiNavigationEvent>& NavigationEvents)
{
	TMap<FString, FSwuiGeneratedNavInfo> EventsByName;

	for (const FName& BuiltInTagName : FSwuiNavTags::GetAllBuiltInTagNames())
	{
		const FString EventName = BuiltInTagName.ToString();
		if (EventName.IsEmpty() || SwuiIsExcludedNavigationPlaceholder(EventName))
		{
			continue;
		}

		EventsByName.Add(EventName, {
			SwuiMakeNavigationConstantIdentifier(EventName),
			SwuiMakeNavigationIdentifier(EventName),
			EventName,
			SwuiGetNavigationCategory(EventName, true),
			true,
		});
	}

	for (const FSwuiNavigationEvent& NavEvent : NavigationEvents)
	{
		if (!NavEvent.Event.IsValid() || !NavEvent.bForwardToJS)
		{
			continue;
		}

		const FString EventName = NavEvent.GetEffectiveJsEventName();
		if (EventName.IsEmpty() || SwuiIsExcludedNavigationPlaceholder(EventName))
		{
			continue;
		}

		if (EventsByName.Contains(EventName))
		{
			continue;
		}

		EventsByName.Add(EventName, {
			SwuiMakeNavigationConstantIdentifier(EventName),
			SwuiMakeNavigationIdentifier(EventName),
			EventName,
			TEXT("custom"),
			false,
		});
	}

	TArray<FSwuiGeneratedNavInfo> Events;
	Events.Reserve(EventsByName.Num());
	EventsByName.GenerateValueArray(Events);

	Events.Sort([](const FSwuiGeneratedNavInfo& A, const FSwuiGeneratedNavInfo& B)
	{
		return A.EventName < B.EventName;
	});

	TSet<FString> UsedIdentifiers;
	for (FSwuiGeneratedNavInfo& Event : Events)
	{
		if (!UsedIdentifiers.Contains(Event.Identifier))
		{
			UsedIdentifiers.Add(Event.Identifier);
			continue;
		}

		const FString BaseIdentifier = Event.Identifier;
		const FString BaseHandlerSuffix = Event.HandlerSuffix;
		int32 Suffix = 2;
		while (UsedIdentifiers.Contains(Event.Identifier))
		{
			Event.Identifier = FString::Printf(TEXT("%s%d"), *BaseIdentifier, Suffix);
			Event.HandlerSuffix = FString::Printf(TEXT("%s%d"), *BaseHandlerSuffix, Suffix);
			++Suffix;
		}

		UsedIdentifiers.Add(Event.Identifier);
	}

	return Events;
}

bool FSwuiTSGenerator::Generate(USwui* Bridge)
{
	if (!Bridge || Bridge->InterfaceName.IsEmpty()) return false;
	if (Bridge->BindingSources.IsEmpty()) return false;

	TArray<FSwuiGeneratedSourceEntry> Sources;
	TArray<FString>      Namespaces;
	TMap<FString, FSwuiCollectedEnum> CollectedEnumsByPath;

	for (const FSwuiBindingSource& Source : Bridge->BindingSources)
	{
		UClass* SourceClass = Source.SourceClass;
		if (!SourceClass) continue;
		if (Source.Properties.IsEmpty() && Source.Delegates.IsEmpty()) continue;

		const FString Namespace  = SwuiComputeNamespace(SourceClass);
		const FString ObjectName = SwuiComputeObjectName(SourceClass);
		const UObject* DefaultsObject = SourceClass->GetDefaultObject();
		Namespaces.AddUnique(Namespace);

		FSwuiGeneratedSourceEntry Entry;
		Entry.ObjectName = ObjectName;
		Entry.Namespace  = Namespace;

		for (const FName& PropFName : Source.Properties)
		{
			FProperty* Prop = SourceClass->FindPropertyByName(PropFName);
			if (!Prop) continue;

			const FString TSType = SwuiGetTSType(Prop);
			if (TSType.IsEmpty()) continue;

			FSwuiGeneratedPropertyInfo Info;
			Info.ObjectName = ObjectName;
			Info.Namespace  = Namespace;
			Info.PropName   = PropFName.ToString();
			Info.FullKey    = Namespace + TEXT(".") + Info.PropName;
			Info.TSType     = TSType;
			Info.Label      = Prop->HasMetaData(TEXT("DisplayName")) ? Prop->GetMetaData(TEXT("DisplayName")) : FString();
			if (Info.Label.IsEmpty()) Info.Label = Info.PropName;
			SwuiPopulateFieldMetadata(
				DefaultsObject,
				Prop,
				Info.PropName,
				Info.ContractType,
				Info.DefaultValueLiteral,
				Info.MinVal,
				Info.MaxVal,
				Info.StepVal,
				Info.EnumOptions);

			SwuiCollectEnumFromProperty(Prop, ObjectName, CollectedEnumsByPath);

			if (const FStructProperty* StructProp = CastField<const FStructProperty>(Prop))
			{
				if (SwuiIsGenericStruct(StructProp))
				{
					Info.StructDef = StructProp->Struct;
				}
			}

			Entry.Props.Add(MoveTemp(Info));
		}

		for (const FName& DelegateFName : Source.Delegates)
		{
			FSwuiGeneratedEventInfo EInfo;
			EInfo.ObjectName    = ObjectName;
			EInfo.Namespace     = Namespace;
			EInfo.DelegateName  = DelegateFName.ToString();
			EInfo.FullKey       = Namespace + TEXT(".") + EInfo.DelegateName;

			const FProperty* DelegateProp = SourceClass->FindPropertyByName(DelegateFName);
			const UFunction* SignatureFunction = nullptr;
			if (const FMulticastDelegateProperty* MulticastDelegateProp = CastField<const FMulticastDelegateProperty>(DelegateProp))
			{
				SignatureFunction = MulticastDelegateProp->SignatureFunction;
			}
			else if (const FDelegateProperty* SingleDelegateProp = CastField<const FDelegateProperty>(DelegateProp))
			{
				SignatureFunction = SingleDelegateProp->SignatureFunction;
			}

			if (SignatureFunction)
			{
				for (TFieldIterator<const FProperty> ParamIt(SignatureFunction); ParamIt; ++ParamIt)
				{
					if (ParamIt->HasAnyPropertyFlags(CPF_Parm) && !ParamIt->HasAnyPropertyFlags(CPF_ReturnParm))
					{
						SwuiCollectEnumFromProperty(*ParamIt, ObjectName, CollectedEnumsByPath);
					}
				}
			}

			EInfo.PayloadBody = SwuiBuildPayloadFieldsBody(SignatureFunction, TEXT("\t\t\t\t"));
			Entry.Events.Add(MoveTemp(EInfo));
		}

		Sources.Add(MoveTemp(Entry));
	}

	if (Sources.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("SWUI: No supported properties or events to generate for '%s'. Skipping."), *Bridge->InterfaceName);
		return false;
	}

	// ── Resolve enums: short-name collisions, canonical names ─────────────
	TMap<FString, TSet<FString>> ShortNameToEnumPaths;
	for (const auto& Pair : CollectedEnumsByPath)
	{
		const FSwuiCollectedEnum& Entry = Pair.Value;
		ShortNameToEnumPaths.FindOrAdd(Entry.ShortName).Add(Entry.EnumPath);
	}

	TArray<FSwuiResolvedEnum> ResolvedEnums;
	TMap<FString, FSwuiResolvedEnum*> ResolvedByPath;

	for (const auto& Pair : CollectedEnumsByPath)
	{
		const FSwuiCollectedEnum& Collected = Pair.Value;

		FSwuiResolvedEnum Resolved;
		Resolved.EnumDef = Collected.EnumDef;
		Resolved.EnumPath = Collected.EnumPath;
		Resolved.ShortName = Collected.ShortName;
		Resolved.bShortNameUnique = ShortNameToEnumPaths[Collected.ShortName].Num() == 1;
		Resolved.ValueNames = Collected.ValueNames;

		// Primary canonical: first source's source-prefixed name
		Resolved.PrimaryCanonicalName = Collected.SourceNames[0] + Collected.ShortName;

		// Source-specific canonical aliases
		for (const FString& SrcName : Collected.SourceNames)
		{
			Resolved.SourceCanonicalNames.Add(SrcName, SrcName + Collected.ShortName);
		}

		ResolvedEnums.Add(MoveTemp(Resolved));
	}

	for (FSwuiResolvedEnum& R : ResolvedEnums)
	{
		ResolvedByPath.Add(R.EnumPath, &R);
	}

	// ── Override TSType for enum properties ──────────────────────────────
	for (FSwuiGeneratedSourceEntry& Src : Sources)
	{
		for (FSwuiGeneratedPropertyInfo& P : Src.Props)
		{
			if (P.ContractType != TEXT("enum")) continue;

			const UEnum* EnumDef = nullptr;
			UClass* SourceClass = nullptr;
			for (const FSwuiBindingSource& BS : Bridge->BindingSources)
			{
				if (BS.SourceClass && SwuiComputeObjectName(BS.SourceClass) == Src.ObjectName)
				{
					SourceClass = BS.SourceClass;
					break;
				}
			}
			if (SourceClass)
			{
				FProperty* Prop = SourceClass->FindPropertyByName(FName(*P.PropName));
				if (Prop && SwuiTryGetEnumDefinition(Prop, EnumDef) && EnumDef)
				{
					FSwuiResolvedEnum** Found = ResolvedByPath.Find(EnumDef->GetPathName());
					if (Found)
					{
						const FSwuiResolvedEnum* R = *Found;
						P.TSType = R->bShortNameUnique ? R->ShortName : R->SourceCanonicalNames[Src.ObjectName];
					}
				}
			}
		}
	}

	// ── Detect which types are used ───────────────────────────────────────
	TSet<FString> UsedTypes;
	for (const FSwuiGeneratedSourceEntry& Src : Sources)
	{
		for (const FSwuiGeneratedPropertyInfo& P : Src.Props)
		{
			UsedTypes.Add(P.TSType);
		}
	}
	bool bHasFVector2D     = UsedTypes.Contains(TEXT("FVector2D"));
	bool bHasFVector       = UsedTypes.Contains(TEXT("FVector"));
	bool bHasFRotator      = UsedTypes.Contains(TEXT("FRotator"));
	bool bHasFLinearColor  = UsedTypes.Contains(TEXT("FLinearColor"));
	bool bHasFColor        = UsedTypes.Contains(TEXT("FColor"));
	bool bHasGameplayTag   = UsedTypes.Contains(TEXT("GameplayTag"));
	bool bHasObjectRef     = UsedTypes.Contains(TEXT("SwuiObjectRef"));
	bool bHasAssetRef      = UsedTypes.Contains(TEXT("SwuiAssetRef"));

	// ── Build base TS types (generated once when used) ───────────────────
	FString BaseTypesBlock;
	if (bHasFVector2D)
	{
		BaseTypesBlock += TEXT(
			"export type FVector2D = {\n"
			"\tx: number;\n"
			"\ty: number;\n"
			"};\n\n"
		);
	}
	if (bHasFVector)
	{
		BaseTypesBlock += TEXT(
			"export type FVector = {\n"
			"\tx: number;\n"
			"\ty: number;\n"
			"\tz: number;\n"
			"};\n\n"
		);
	}
	if (bHasFRotator)
	{
		BaseTypesBlock += TEXT(
			"export type FRotator = {\n"
			"\tpitch: number;\n"
			"\tyaw: number;\n"
			"\troll: number;\n"
			"};\n\n"
		);
	}
	if (bHasFLinearColor)
	{
		BaseTypesBlock += TEXT(
			"export type FLinearColor = {\n"
			"\tr: number;\n"
			"\tg: number;\n"
			"\tb: number;\n"
			"\ta: number;\n"
			"};\n\n"
		);
	}
	if (bHasFColor)
	{
		BaseTypesBlock += TEXT(
			"export type FColor = {\n"
			"\tr: number;\n"
			"\tg: number;\n"
			"\tb: number;\n"
			"\ta: number;\n"
			"};\n\n"
		);
	}
	if (bHasGameplayTag)
	{
		BaseTypesBlock += TEXT("export type GameplayTag = string;\n\n");
	}
	if (bHasObjectRef)
	{
		BaseTypesBlock += TEXT(
			"export type SwuiObjectRef = {\n"
			"\tname: string;\n"
			"\tclassName: string;\n"
			"\tpath?: string;\n"
			"};\n\n"
		);
	}
	if (bHasAssetRef)
	{
		BaseTypesBlock += TEXT(
			"export type SwuiAssetRef = {\n"
			"\tpath: string;\n"
			"\tname?: string;\n"
			"\tclassName?: string;\n"
			"};\n\n"
		);
	}

	// ── Collect and build generic struct types ───────────────────────────
	FString StructTypesBlock;
	TSet<const void*> CollectedStructDefs;
	TArray<const UScriptStruct*> StructDefOrder;
	for (const FSwuiGeneratedSourceEntry& Src : Sources)
	{
		for (const FSwuiGeneratedPropertyInfo& P : Src.Props)
		{
			if (!P.StructDef || CollectedStructDefs.Contains(P.StructDef)) continue;
			CollectedStructDefs.Add(P.StructDef);
			StructDefOrder.Add(static_cast<const UScriptStruct*>(P.StructDef));
		}
	}

	for (const UScriptStruct* StructDef : StructDefOrder)
	{
		const FString StructName = StructDef->GetName();
		StructTypesBlock += FString::Printf(TEXT("export type %s = {\n"), *StructName);
		StructTypesBlock += SwuiBuildStructTypeBody(const_cast<UScriptStruct*>(StructDef));
		StructTypesBlock += TEXT("\n};\n\n");
	}

	// ── Build enum code ──────────────────────────────────────────────────
	FString EnumBlock;
	for (const FSwuiResolvedEnum& R : ResolvedEnums)
	{
		// Primary as-const object + type
		EnumBlock += FString::Printf(TEXT("export const %s = {\n"), *R.PrimaryCanonicalName);
		for (const FString& ValName : R.ValueNames)
		{
			EnumBlock += FString::Printf(TEXT("\t%s: \"%s\",\n"), *ValName, *ValName);
		}
		EnumBlock += TEXT("} as const;\n\n");

		EnumBlock += FString::Printf(TEXT("export type %s = typeof %s[keyof typeof %s];\n\n"),
			*R.PrimaryCanonicalName, *R.PrimaryCanonicalName, *R.PrimaryCanonicalName);

		// Source-specific aliases (skip the primary source — it's already the canonical)
		for (const auto& SrcPair : R.SourceCanonicalNames)
		{
			const FString& SrcName = SrcPair.Key;
			const FString& Canonical = SrcPair.Value;
			if (Canonical == R.PrimaryCanonicalName) continue;

			EnumBlock += FString::Printf(TEXT("export const %s = %s;\n"), *Canonical, *R.PrimaryCanonicalName);
			EnumBlock += FString::Printf(TEXT("export type %s = %s;\n\n"), *Canonical, *R.PrimaryCanonicalName);
		}

		// Short alias when unique
		if (R.bShortNameUnique)
		{
			EnumBlock += FString::Printf(TEXT("export const %s = %s;\n"), *R.ShortName, *R.PrimaryCanonicalName);
			EnumBlock += FString::Printf(TEXT("export type %s = %s;\n\n"), *R.ShortName, *R.PrimaryCanonicalName);
		}
	}

	// ── Build state types ────────────────────────────────────────────────
	FString StateTypesBlock;
	for (const FSwuiGeneratedSourceEntry& Src : Sources)
	{
		if (Src.Props.IsEmpty()) continue;

		StateTypesBlock += FString::Printf(TEXT("export type %sState = {\n"), *Src.ObjectName);
		for (const FSwuiGeneratedPropertyInfo& P : Src.Props)
		{
			StateTypesBlock += FString::Printf(TEXT("\t%s: %s;\n"), *P.PropName, *P.TSType);
		}
		StateTypesBlock += TEXT("};\n\n");
	}

	const FString IName  = Bridge->InterfaceName;
	const FString NsList = FString::Join(Namespaces, TEXT(", "));

	// ── One const object per source class ────────────────────────────────────
	FString ObjectsBody;
	for (const FSwuiGeneratedSourceEntry& Src : Sources)
	{
		ObjectsBody += FString::Printf(TEXT("export const %s = {\n"), *Src.ObjectName);

		// Key string entries
		for (const FSwuiGeneratedPropertyInfo& P : Src.Props)
			ObjectsBody += FString::Printf(TEXT("\t%s: '%s' as const,\n"), *P.PropName, *P.FullKey);

		if (!Src.Props.IsEmpty() && (!Src.Props.IsEmpty() || !Src.Events.IsEmpty()))
			ObjectsBody += TEXT("\n");

		// State subscription helpers: onHealth(fn)
		for (const FSwuiGeneratedPropertyInfo& P : Src.Props)
		{
			ObjectsBody += FString::Printf(
				TEXT("\ton%s(fn: (v: %s) => void): () => void {\n")
				TEXT("\t\treturn Swui.on(this.%s, fn);\n")
				TEXT("\t},\n"),
				*P.PropName, *P.TSType, *P.PropName);
		}

		// Event helpers: OnFired(fn) — UE name as-is, no extra prefix
		for (const FSwuiGeneratedEventInfo& E : Src.Events)
		{
			ObjectsBody += FString::Printf(
				TEXT("\t%s(fn: () => void): () => void {\n")
				TEXT("\t\tdocument.addEventListener('%s', fn);\n")
				TEXT("\t\treturn () => document.removeEventListener('%s', fn);\n")
				TEXT("\t},\n"),
				*E.DelegateName, *E.FullKey, *E.FullKey);
		}

		ObjectsBody += TEXT("};\n\n");
	}

	// SwuiRuntime — always-present base object (fps, dt, dimensions, tick event)
	const FString RuntimeBlock = TEXT(
		"export interface ISwuiRuntimeInfo {\n"
		"  fps: number; deltaTime: number; cefFps: number; width: number; height: number;\n"
		"}\n"
		"\n"
		"/** Always-available runtime info pushed by the SWUI subsystem each CEF frame. */\n"
		"export const SwuiRuntime = {\n"
		"  get fps()       : number { return (window as any).__SWUI__?._runtime?.fps ?? 0; },\n"
		"  get deltaTime() : number { return (window as any).__SWUI__?._runtime?.dt  ?? 0; },\n"
		"  get cefFps()    : number { return (window as any).__SWUI__?._runtime?.cefFps ?? 0; },\n"
		"  get width()     : number { return (window as any).__SWUI__?._runtime?.width ?? window.innerWidth; },\n"
		"  get height()    : number { return (window as any).__SWUI__?._runtime?.height ?? window.innerHeight; },\n"
		"  onTick(fn: (info: ISwuiRuntimeInfo) => void): () => void {\n"
		"    const h = (e: Event) => fn((e as CustomEvent<ISwuiRuntimeInfo>).detail);\n"
		"    document.addEventListener('swui:tick', h);\n"
		"    return () => document.removeEventListener('swui:tick', h);\n"
		"  },\n"
		"};\n"
	);

	const FString ContractBlock = FString::Printf(TEXT(
		"export const SwuiContract = {\n"
		"\tstate: {\n"
		"%s"
		"\t},\n"
		"\tevents: {\n"
		"%s"
		"\t},\n"
		"} as const;\n"),
		*SwuiBuildContractStateBody(Sources),
		*SwuiBuildContractEventsBody(Sources));

	FString Output = FString::Printf(TEXT(
		"// %s.generated.ts\n"
		"// AUTO-GENERATED by SimpleWebUI \u2014 do not edit manually.\n"
		"// Re-generate via: Tools > SimpleWebUI > Refresh JS Bindings\n"
		"// Sources: %s\n"
		"\n"
		"import Swui from '@simplewebui/client';\n"
		"\n"
		"%s"
		"\n"
		"%s"
		"%s"
		"%s"
		"%s"
		"%s"
		"%s"
	),
		*IName,
		*NsList,
		*RuntimeBlock,
		*BaseTypesBlock,
		*StructTypesBlock,
		*EnumBlock,
		*ObjectsBody,
		*StateTypesBlock,
		*ContractBlock
	);

	const FString OutDir  = FPaths::ProjectContentDir() / TEXT("UI/generated");
	const FString OutFile = OutDir / IName + TEXT(".generated.ts");

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*OutDir))
		PlatformFile.CreateDirectoryTree(*OutDir);

	if (FFileHelper::SaveStringToFile(Output, *OutFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogTemp, Log, TEXT("SWUI: Generated '%s'"), *OutFile);
		return true;
	}

	UE_LOG(LogTemp, Error, TEXT("SWUI: Failed to write '%s'"), *OutFile);
	return false;
}

bool FSwuiTSGenerator::GenerateNavigation(USwui* Bridge, const TArray<FSwuiNavigationEvent>& NavigationEvents)
{
	if (!Bridge)
	{
		UE_LOG(LogTemp, Error, TEXT("SWUI: Navigation JS bindings generation failed because the target USwui is null."));
		return false;
	}

	if (Bridge->InterfaceName.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("SWUI: Navigation JS bindings generation failed because InterfaceName is empty on '%s'."), *Bridge->GetPathName());
		return false;
	}

	const FString InterfaceName = Bridge->InterfaceName;
	const FString ObjectName = TEXT("SwuiNavigationEvents");
	const TArray<FSwuiGeneratedNavInfo> Events = SwuiCollectGeneratedNavigationEvents(NavigationEvents);

	FString ObjectBody = FString::Printf(TEXT("export const %s = {\n"), *ObjectName);
	for (const FSwuiGeneratedNavInfo& Event : Events)
		ObjectBody += FString::Printf(TEXT("\t%s: '%s' as const,\n"), *Event.Identifier, *SwuiEscapeTsStringLiteral(Event.EventName));

	if (Events.Num() > 0)
		ObjectBody += TEXT("\n");

	for (const FSwuiGeneratedNavInfo& Event : Events)
	{
		ObjectBody += FString::Printf(
			TEXT("\ton%s(fn: (detail: unknown) => void): () => void {\n")
			TEXT("\t\treturn Swui.onEvent(%s.%s, fn);\n")
			TEXT("\t},\n"),
			*Event.HandlerSuffix,
			*ObjectName,
			*Event.Identifier);
	}
	ObjectBody += TEXT("};\n\n");

	FString ContractBody = TEXT("export const SwuiNavigationContract = {\n\tnavigation: {\n");
	for (const FSwuiGeneratedNavInfo& Event : Events)
	{
		ContractBody += FString::Printf(
			TEXT("\t\t[%s.%s]: {\n")
			TEXT("\t\t\tlabel: %s,\n")
			TEXT("\t\t\ttag: %s.%s,\n")
			TEXT("\t\t\tcategory: %s,\n")
			TEXT("\t\t\tdefaultEvent: %s,\n")
			TEXT("\t\t},\n"),
			*ObjectName,
			*Event.Identifier,
			*SwuiQuotePreviewString(Event.Identifier),
			*ObjectName,
			*Event.Identifier,
			*SwuiQuotePreviewString(Event.Category),
			Event.bDefaultEvent ? TEXT("true") : TEXT("false"));
	}
	ContractBody += TEXT("\t},\n} as const;\n\n");
	ObjectBody += ContractBody;
	ObjectBody += FString::Printf(TEXT("export default %s;\n"), *ObjectName);

	const FString Header = FString::Printf(
		TEXT("// %s.navigation.generated.ts\n")
		TEXT("// AUTO-GENERATED by SimpleWebUI — do not edit manually.\n")
		TEXT("// Re-generate via: SwuiNavigation > Refresh Navigation Events JS Bindings\n\n")
		TEXT("import Swui from '@simplewebui/client';\n\n"),
		*InterfaceName);

	const FString Output = Header + ObjectBody;

	const FString OutDir = FPaths::ProjectContentDir() / TEXT("UI/generated");
	const FString OutFile = OutDir / InterfaceName + TEXT(".navigation.generated.ts");

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*OutDir))
	{
		PlatformFile.CreateDirectoryTree(*OutDir);
	}

	if (FFileHelper::SaveStringToFile(Output, *OutFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogTemp, Log, TEXT("SWUI: Generated navigation bindings '%s' with %d JS-forwarded event(s)."), *OutFile, Events.Num());
		return true;
	}

	UE_LOG(LogTemp, Error, TEXT("SWUI: Failed to write navigation bindings '%s'."), *OutFile);
	return false;
}