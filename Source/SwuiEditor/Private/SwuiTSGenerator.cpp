#include "SwuiTSGenerator.h"
#include "SwuiNavigation.h"
#include "Swui.h"
#include "SwuiBindingSource.h"
#include "SwuiBindingCollector.h"
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

static FString SwuiFormatTemplate(FString Template, const TMap<FString, FString>& Vars)
{
	for (const TPair<FString, FString>& Pair : Vars)
	{
		Template.ReplaceInline(*FString::Printf(TEXT("${%s}"), *Pair.Key), *Pair.Value);
	}
	return Template;
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

static FString SwuiBuildRuntimeBlock()
{
	return TEXT(R"TS(export interface ISwuiRuntimeInfo {
	fps: number;
	deltaTime: number;
	cefFps: number;
	width: number;
	height: number;
}

/** Always-available runtime info pushed by the SWUI subsystem each CEF frame. */
export const SwuiRuntime = {
	get fps(): number { return (window as any).__SWUI__?._runtime?.fps ?? 0; },
	get deltaTime(): number { return (window as any).__SWUI__?._runtime?.dt ?? 0; },
	get cefFps(): number { return (window as any).__SWUI__?._runtime?.cefFps ?? 0; },
	get width(): number { return (window as any).__SWUI__?._runtime?.width ?? window.innerWidth; },
	get height(): number { return (window as any).__SWUI__?._runtime?.height ?? window.innerHeight; },

	onTick(fn: (info: ISwuiRuntimeInfo) => void): () => void {
		const h = (e: Event) => fn((e as CustomEvent<ISwuiRuntimeInfo>).detail);
		document.addEventListener('swui:tick', h);
		return () => document.removeEventListener('swui:tick', h);
	},
};
)TS");
}

static FString SwuiBuildKeyEntry(const FSwuiGeneratedPropertyInfo& P)
{
	TMap<FString, FString> Vars;
	Vars.Add(TEXT("PropName"), P.PropName);
	Vars.Add(TEXT("FullKey"), P.FullKey);
	return SwuiFormatTemplate(TEXT("\t${PropName}: '${FullKey}' as const,\n"), Vars);
}

static FString SwuiBuildStateSubscriptionHelper(const FSwuiGeneratedPropertyInfo& P)
{
	TMap<FString, FString> Vars;
	Vars.Add(TEXT("PropName"), P.PropName);
	Vars.Add(TEXT("TSType"), P.TSType);
	return SwuiFormatTemplate(TEXT(R"TS(	on${PropName}(fn: (v: ${TSType}) => void): () => void {
		return Swui.on(this.${PropName}, fn);
	},
)TS"), Vars);
}

static FString SwuiBuildEventHelper(const FSwuiGeneratedEventInfo& E)
{
	TMap<FString, FString> Vars;
	Vars.Add(TEXT("DelegateName"), E.DelegateName);
	Vars.Add(TEXT("FullKey"), E.FullKey);
	return SwuiFormatTemplate(TEXT(R"TS(	${DelegateName}(fn: () => void): () => void {
		document.addEventListener('${FullKey}', fn);
		return () => document.removeEventListener('${FullKey}', fn);
	},
)TS"), Vars);
}

static FString SwuiBuildGeneratedFile(
	const FString& InterfaceName,
	const FString& SourcesList,
	const FString& RuntimeBlock,
	const FString& BaseTypesBlock,
	const FString& StructTypesBlock,
	const FString& EnumBlock,
	const FString& ObjectsBody,
	const FString& StateTypesBlock,
	const FString& ContractBlock)
{
	TMap<FString, FString> Vars;
	Vars.Add(TEXT("InterfaceName"), InterfaceName);
	Vars.Add(TEXT("SourcesList"), SourcesList);
	Vars.Add(TEXT("RuntimeBlock"), RuntimeBlock);
	Vars.Add(TEXT("BaseTypesBlock"), BaseTypesBlock);
	Vars.Add(TEXT("StructTypesBlock"), StructTypesBlock);
	Vars.Add(TEXT("EnumBlock"), EnumBlock);
	Vars.Add(TEXT("ObjectsBody"), ObjectsBody);
	Vars.Add(TEXT("StateTypesBlock"), StateTypesBlock);
	Vars.Add(TEXT("ContractBlock"), ContractBlock);
	return SwuiFormatTemplate(TEXT(R"TS(// ${InterfaceName}.generated.ts
// AUTO-GENERATED by SimpleWebUI — do not edit manually.
// Re-generate via: Tools > SimpleWebUI > Refresh JS Bindings
// Sources: ${SourcesList}

import Swui from '@simplewebui/client';

${RuntimeBlock}

${BaseTypesBlock}${StructTypesBlock}${EnumBlock}${ObjectsBody}${StateTypesBlock}${ContractBlock})TS"), Vars);
}

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
		OutType = SwuiGetTSType(Prop);

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

		if (EnumDefinition->IsValidEnumValue(EnumValue))
		{
			OutDefaultValueLiteral = SwuiQuotePreviewString(EnumDefinition->GetNameStringByValue(EnumValue));
		}
		else
		{
			// Fall back to the first valid option
			for (int32 i = 0; i < EnumDefinition->NumEnums(); ++i)
			{
				if (!SwuiShouldSkipEnumValue(EnumDefinition, i))
				{
					OutDefaultValueLiteral = SwuiQuotePreviewString(EnumDefinition->GetNameStringByIndex(i));
					break;
				}
			}
		}

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
		OutType = SwuiGetTSType(Prop);
		const bool bDefaultValue = DefaultsObject
			? BoolProp->GetPropertyValue_InContainer(DefaultsObject)
			: false;
		OutDefaultValueLiteral = bDefaultValue ? TEXT("true") : TEXT("false");
		return;
	}

	if (const FStrProperty* StrProp = CastField<const FStrProperty>(Prop))
	{
		OutType = SwuiGetTSType(Prop);
		const FString DefaultValue = DefaultsObject
			? StrProp->GetPropertyValue_InContainer(DefaultsObject)
			: FString();
		OutDefaultValueLiteral = SwuiQuotePreviewString(DefaultValue);
		return;
	}

	if (const FNameProperty* NameProp = CastField<const FNameProperty>(Prop))
	{
		OutType = SwuiGetTSType(Prop);
		const FName DefaultValue = DefaultsObject
			? NameProp->GetPropertyValue_InContainer(DefaultsObject)
			: NAME_None;
		OutDefaultValueLiteral = SwuiQuotePreviewString(DefaultValue.ToString());
		return;
	}

	if (const FTextProperty* TextProp = CastField<const FTextProperty>(Prop))
	{
		OutType = SwuiGetTSType(Prop);
		const FText DefaultValue = DefaultsObject
			? TextProp->GetPropertyValue_InContainer(DefaultsObject)
			: FText::GetEmpty();
		OutDefaultValueLiteral = SwuiQuotePreviewString(DefaultValue.ToString());
		return;
	}

	if (const FStructProperty* StructProp = CastField<const FStructProperty>(Prop))
	{
		OutType = SwuiGetTSType(Prop);
		const FString StructName = StructProp->Struct->GetName();

		if (StructName == TEXT("GameplayTag"))
		{
			OutType = TEXT("GameplayTag");
			OutDefaultValueLiteral = TEXT("''");
		}
		else if (StructName == TEXT("Vector2D"))
		{
			OutDefaultValueLiteral = TEXT("{x:0,y:0}");
		}
		else if (StructName == TEXT("Vector"))
		{
			OutDefaultValueLiteral = TEXT("{x:0,y:0,z:0}");
		}
		else if (StructName == TEXT("Rotator"))
		{
			OutDefaultValueLiteral = TEXT("{pitch:0,yaw:0,roll:0}");
		}
		else if (StructName == TEXT("LinearColor"))
		{
			OutDefaultValueLiteral = TEXT("{r:0,g:0,b:0,a:1}");
		}
		else if (StructName == TEXT("Color"))
		{
			OutDefaultValueLiteral = TEXT("{r:0,g:0,b:0,a:255}");
		}
		else
		{
			// Generic struct — build default from children
			TArray<FString> Fields;
			for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
			{
				FString ChildType, ChildDefault, ChildMin, ChildMax, ChildStep;
				TArray<FString> ChildOptions;
				SwuiPopulateFieldMetadata(nullptr, *It, It->GetName(), ChildType, ChildDefault, ChildMin, ChildMax, ChildStep, ChildOptions);
				if (ChildType.IsEmpty() || ChildType == TEXT("unknown")) continue;
				Fields.Add(It->GetName() + TEXT(":") + ChildDefault);
			}
			OutDefaultValueLiteral = TEXT("{") + FString::Join(Fields, TEXT(",")) + TEXT("}");
		}
		return;
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

	if (const FArrayProperty* ArrayProp = CastField<const FArrayProperty>(Prop))
	{
		OutType = SwuiGetTSType(Prop);
		OutDefaultValueLiteral = TEXT("[]");
		return;
	}

	if (const FMapProperty* MapProp = CastField<const FMapProperty>(Prop))
	{
		OutType = SwuiGetTSType(Prop);
		OutDefaultValueLiteral = TEXT("{}");
		return;
	}

	if (Prop->IsA<FObjectPropertyBase>())
	{
		OutType = SwuiGetTSType(Prop);
		OutDefaultValueLiteral = TEXT("null");
		return;
	}

	if (Prop->IsA<FSoftObjectProperty>() || Prop->IsA<FSoftClassProperty>())
	{
		OutType = SwuiGetTSType(Prop);
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
		if (EnumOptions.Num() > 0)
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
			if (Prop.EnumOptions.Num() > 0)
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

static FString SwuiBuildContractBlock(const TArray<FSwuiGeneratedSourceEntry>& Sources)
{
	TMap<FString, FString> Vars;
	Vars.Add(TEXT("StateBody"), SwuiBuildContractStateBody(Sources));
	Vars.Add(TEXT("EventsBody"), SwuiBuildContractEventsBody(Sources));
	return SwuiFormatTemplate(TEXT(R"TS(export const SwuiContract = {
	state: {
${StateBody}
	},
	events: {
${EventsBody}
	},
} as const;
)TS"), Vars);
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

	// Single source of truth: SwuiBindingCollector merges manual + code-exposed (SwuiExpose)
	FSwuiEffectiveBindings Effective = SwuiCollectEffectiveBindings(Bridge);
	if (Effective.Sources.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("SWUI: No effective bindings to generate for '%s'. Skipping."), *Bridge->InterfaceName);
		return false;
	}

	for (const FString& W : Effective.Warnings)
	{
		UE_LOG(LogTemp, Warning, TEXT("%s"), *W);
	}

	TArray<FSwuiGeneratedSourceEntry> Sources;
	TArray<FString>      Namespaces;
	TMap<FString, FSwuiCollectedEnum> CollectedEnumsByPath;

	for (const FSwuiEffectiveSource& EffSrc : Effective.Sources)
	{
		UClass* SourceClass = EffSrc.SourceClass;
		if (!SourceClass) continue;
		if (EffSrc.Properties.IsEmpty() && EffSrc.Delegates.IsEmpty()) continue;

		const FString Namespace  = EffSrc.Namespace;
		const FString ObjectName = EffSrc.ObjectName;
		const UObject* DefaultsObject = SourceClass->GetDefaultObject();
		Namespaces.AddUnique(Namespace);

		FSwuiGeneratedSourceEntry Entry;
		Entry.ObjectName = ObjectName;
		Entry.Namespace  = Namespace;

		for (const FSwuiEffectiveProperty& EP : EffSrc.Properties)
		{
			const FName PropFName = EP.PropName;
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

		for (const FSwuiEffectiveDelegate& ED : EffSrc.Delegates)
		{
			const FName DelegateFName = ED.DelegateName;

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
		const FSwuiCollectedEnum& CollectedEnum = Pair.Value;
		ShortNameToEnumPaths.FindOrAdd(CollectedEnum.ShortName).Add(CollectedEnum.EnumPath);
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
			if (P.EnumOptions.IsEmpty()) continue;

			const UEnum* EnumDef = nullptr;
			UClass* ResolvedSourceClass = nullptr;
			for (const FSwuiBindingSource& BS : Bridge->BindingSources)
			{
				if (BS.SourceClass && SwuiComputeObjectName(BS.SourceClass) == Src.ObjectName)
				{
					ResolvedSourceClass = BS.SourceClass;
					break;
				}
			}
			if (ResolvedSourceClass)
			{
				FProperty* Prop = ResolvedSourceClass->FindPropertyByName(FName(*P.PropName));
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
	if (bHasFVector2D) BaseTypesBlock += TEXT(R"TS(
export type FVector2D = {
	x: number;
	y: number;
};

)TS");
	if (bHasFVector) BaseTypesBlock += TEXT(R"TS(
export type FVector = {
	x: number;
	y: number;
	z: number;
};

)TS");
	if (bHasFRotator) BaseTypesBlock += TEXT(R"TS(
export type FRotator = {
	pitch: number;
	yaw: number;
	roll: number;
};

)TS");
	if (bHasFLinearColor) BaseTypesBlock += TEXT(R"TS(
export type FLinearColor = {
	r: number;
	g: number;
	b: number;
	a: number;
};

)TS");
	if (bHasFColor) BaseTypesBlock += TEXT(R"TS(
export type FColor = {
	r: number;
	g: number;
	b: number;
	a: number;
};

)TS");
	if (bHasGameplayTag) BaseTypesBlock += TEXT(R"TS(
export type GameplayTag = string;

)TS");
	if (bHasObjectRef) BaseTypesBlock += TEXT(R"TS(
export type SwuiObjectRef = {
	name: string;
	className: string;
	path?: string;
};

)TS");
	if (bHasAssetRef) BaseTypesBlock += TEXT(R"TS(
export type SwuiAssetRef = {
	path: string;
	name?: string;
	className?: string;
};

)TS");

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
		TMap<FString, FString> SVars;
		SVars.Add(TEXT("StructName"), StructName);
		SVars.Add(TEXT("Body"), SwuiBuildStructTypeBody(const_cast<UScriptStruct*>(StructDef)));
		StructTypesBlock += SwuiFormatTemplate(TEXT(R"TS(
export type ${StructName} = {
${Body}};

)TS"), SVars);
	}

	// ── Build enum code ──────────────────────────────────────────────────
	FString EnumBlock;
	for (const FSwuiResolvedEnum& R : ResolvedEnums)
	{
		FString EnumValues;
		for (const FString& ValName : R.ValueNames)
		{
			EnumValues += FString::Printf(TEXT("\t%s: \"%s\",\n"), *ValName, *ValName);
		}
		{
			TMap<FString, FString> EVars;
			EVars.Add(TEXT("Canonical"), R.PrimaryCanonicalName);
			EVars.Add(TEXT("Values"), EnumValues);
			EnumBlock += SwuiFormatTemplate(TEXT(R"TS(
export const ${Canonical} = {
${Values}} as const;

export type ${Canonical} = typeof ${Canonical}[keyof typeof ${Canonical}];

)TS"), EVars);
		}

		// Source-specific aliases (skip the primary source — it's already the canonical)
		for (const auto& SrcPair : R.SourceCanonicalNames)
		{
			const FString& SrcName = SrcPair.Key;
			const FString& Canonical = SrcPair.Value;
			if (Canonical == R.PrimaryCanonicalName) continue;

			TMap<FString, FString> AVars;
			AVars.Add(TEXT("Alias"), Canonical);
			AVars.Add(TEXT("Canonical"), R.PrimaryCanonicalName);
			EnumBlock += SwuiFormatTemplate(TEXT(R"TS(
export const ${Alias} = ${Canonical};
export type ${Alias} = ${Canonical};

)TS"), AVars);
		}

		// Short alias when unique
		if (R.bShortNameUnique)
		{
			TMap<FString, FString> ShVars;
			ShVars.Add(TEXT("Short"), R.ShortName);
			ShVars.Add(TEXT("Canonical"), R.PrimaryCanonicalName);
			EnumBlock += SwuiFormatTemplate(TEXT(R"TS(
export const ${Short} = ${Canonical};
export type ${Short} = ${Canonical};

)TS"), ShVars);
		}
	}

	// ── Build state types ────────────────────────────────────────────────
	FString StateTypesBlock;
	for (const FSwuiGeneratedSourceEntry& Src : Sources)
	{
		if (Src.Props.IsEmpty()) continue;

		FString StateFields;
		for (const FSwuiGeneratedPropertyInfo& P : Src.Props)
		{
			StateFields += FString::Printf(TEXT("\t%s: %s;\n"), *P.PropName, *P.TSType);
		}
		TMap<FString, FString> StVars;
		StVars.Add(TEXT("ObjectName"), Src.ObjectName);
		StVars.Add(TEXT("Fields"), StateFields);
		StateTypesBlock += SwuiFormatTemplate(TEXT(R"TS(
export type ${ObjectName}State = {
${Fields}};

)TS"), StVars);
	}

	const FString IName  = Bridge->InterfaceName;
	const FString NsList = FString::Join(Namespaces, TEXT(", "));

	// ── One const object per source class ────────────────────────────────────
	FString ObjectsBody;
	for (const FSwuiGeneratedSourceEntry& Src : Sources)
	{
		FString ObjectInner;

		for (const FSwuiGeneratedPropertyInfo& P : Src.Props)
			ObjectInner += SwuiBuildKeyEntry(P);

		if (!Src.Props.IsEmpty())
			ObjectInner += TEXT("\n");

		for (const FSwuiGeneratedPropertyInfo& P : Src.Props)
			ObjectInner += SwuiBuildStateSubscriptionHelper(P);

		for (const FSwuiGeneratedEventInfo& E : Src.Events)
			ObjectInner += SwuiBuildEventHelper(E);

		TMap<FString, FString> OVars;
		OVars.Add(TEXT("ObjectName"), Src.ObjectName);
		OVars.Add(TEXT("Inner"), ObjectInner);
		ObjectsBody += SwuiFormatTemplate(TEXT("export const ${ObjectName} = {\n${Inner}};\n\n"), OVars);
	}

	const FString RuntimeBlock = SwuiBuildRuntimeBlock();
	const FString ContractBlock = SwuiBuildContractBlock(Sources);

	FString Output = SwuiBuildGeneratedFile(IName, NsList,
		RuntimeBlock, BaseTypesBlock, StructTypesBlock, EnumBlock,
		ObjectsBody, StateTypesBlock, ContractBlock);

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