#include "SwuiTSGenerator.h"
#include "Swui.h"
#include "SwuiBindingSource.h"
#include "SwuiTypes.h"
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

bool FSwuiTSGenerator::Generate(USwui* Bridge)
{
	if (!Bridge || Bridge->InterfaceName.IsEmpty()) return false;
	if (Bridge->BindingSources.IsEmpty()) return false;

	struct FPropInfo
	{
		FString ObjectName; // "BP_ThirdPersonCharacter"
		FString Namespace;
		FString FullKey;   // "namespace.PropName"
		FString PropName;  // "Health"
		FString TSType;    // "number"
		FString Label;
		FString Category;
		FString MinVal;
		FString MaxVal;
	};

	struct FEventInfo
	{
		FString ObjectName;   // "SwuiShootingComponent"
		FString Namespace;
		FString FullKey;      // "namespace.OnFired"
		FString DelegateName; // "OnFired"
	};

	// One entry per source class — tracks object name + its props + its events.
	struct FSourceEntry
	{
		FString ObjectName;
		FString Namespace;
		TArray<FPropInfo>  Props;
		TArray<FEventInfo> Events;
	};

	TArray<FSourceEntry> Sources;
	TArray<FString>      Namespaces;

	for (const FSwuiBindingSource& Source : Bridge->BindingSources)
	{
		UClass* SourceClass = Source.SourceClass;
		if (!SourceClass) continue;
		if (Source.Properties.IsEmpty() && Source.Delegates.IsEmpty()) continue;

		const FString Namespace  = SwuiComputeNamespace(SourceClass);
		const FString ObjectName = SwuiComputeObjectName(SourceClass);
		Namespaces.AddUnique(Namespace);

		FSourceEntry Entry;
		Entry.ObjectName = ObjectName;
		Entry.Namespace  = Namespace;

		for (const FName& PropFName : Source.Properties)
		{
			FProperty* Prop = SourceClass->FindPropertyByName(PropFName);
			if (!Prop) continue;

			const FString TSType = SwuiGetTSType(Prop);
			if (TSType.IsEmpty()) continue;

			auto GetMeta = [&](const TCHAR* Key) -> FString
			{
				return Prop->HasMetaData(Key) ? Prop->GetMetaData(Key) : FString();
			};

			FPropInfo Info;
			Info.ObjectName = ObjectName;
			Info.Namespace  = Namespace;
			Info.PropName   = PropFName.ToString();
			Info.FullKey    = Namespace + TEXT(".") + Info.PropName;
			Info.TSType     = TSType;
			Info.Label      = GetMeta(TEXT("DisplayName"));
			if (Info.Label.IsEmpty()) Info.Label = Info.PropName;
			Info.Category   = GetMeta(TEXT("Category"));
			Info.MinVal     = GetMeta(TEXT("ClampMin"));
			if (Info.MinVal.IsEmpty()) Info.MinVal = GetMeta(TEXT("UIMin"));
			Info.MaxVal     = GetMeta(TEXT("ClampMax"));
			if (Info.MaxVal.IsEmpty()) Info.MaxVal = GetMeta(TEXT("UIMax"));

			Entry.Props.Add(MoveTemp(Info));
		}

		for (const FName& DelegateFName : Source.Delegates)
		{
			FEventInfo EInfo;
			EInfo.ObjectName    = ObjectName;
			EInfo.Namespace     = Namespace;
			EInfo.DelegateName  = DelegateFName.ToString();
			EInfo.FullKey       = Namespace + TEXT(".") + EInfo.DelegateName;
			Entry.Events.Add(MoveTemp(EInfo));
		}

		Sources.Add(MoveTemp(Entry));
	}

	if (Sources.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("SWUI: No supported properties or events to generate for '%s'. Skipping."), *Bridge->InterfaceName);
		return false;
	}

	const FString IName  = Bridge->InterfaceName;
	const FString NsList = FString::Join(Namespaces, TEXT(", "));

	// ── One const object per source class ────────────────────────────────────
	FString ObjectsBody;
	for (const FSourceEntry& Src : Sources)
	{
		ObjectsBody += FString::Printf(TEXT("export const %s = {\n"), *Src.ObjectName);

		// Key string entries
		for (const FPropInfo& P : Src.Props)
			ObjectsBody += FString::Printf(TEXT("\t%s: '%s' as const,\n"), *P.PropName, *P.FullKey);

		if (!Src.Props.IsEmpty() && (!Src.Props.IsEmpty() || !Src.Events.IsEmpty()))
			ObjectsBody += TEXT("\n");

		// State subscription helpers: onHealth(fn)
		for (const FPropInfo& P : Src.Props)
		{
			ObjectsBody += FString::Printf(
				TEXT("\ton%s(fn: (v: %s) => void): () => void {\n")
				TEXT("\t\treturn Swui.on(this.%s, fn);\n")
				TEXT("\t},\n"),
				*P.PropName, *P.TSType, *P.PropName);
		}

		// Event helpers: OnFired(fn) — UE name as-is, no extra prefix
		for (const FEventInfo& E : Src.Events)
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
	),
		*IName,
		*NsList,
		*RuntimeBlock,
		*ObjectsBody
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

	struct FNavInfo
	{
		FString Identifier;
		FString EventName;
	};

	TArray<FNavInfo> Events;
	TSet<FString> UsedIdentifiers;

	for (const FSwuiNavigationEvent& NavEvent : NavigationEvents)
	{
		if (!NavEvent.Event.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("SWUI: Skipping navigation event with invalid GameplayTag while generating bindings for '%s'."), *Bridge->InterfaceName);
			continue;
		}

		if (!NavEvent.bForwardToJS)
		{
			continue;
		}

		const FString EventName = NavEvent.GetEffectiveJsEventName();
		if (EventName.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("SWUI: Skipping navigation event '%s' with empty JS event name while generating bindings for '%s'."), *NavEvent.Event.ToString(), *Bridge->InterfaceName);
			continue;
		}

		FString Identifier = SwuiMakeNavigationIdentifier(EventName);
		if (UsedIdentifiers.Contains(Identifier))
		{
			const FString BaseIdentifier = Identifier;
			int32 Suffix = 2;
			while (UsedIdentifiers.Contains(Identifier))
			{
				Identifier = FString::Printf(TEXT("%s%d"), *BaseIdentifier, Suffix++);
			}
		}

		UsedIdentifiers.Add(Identifier);
		Events.Add({ Identifier, EventName });
	}

	const FString InterfaceName = Bridge->InterfaceName;
	const FString ObjectName = TEXT("SwuiNavigationEvents");

	Events.Sort([](const FNavInfo& A, const FNavInfo& B)
	{
		return A.Identifier < B.Identifier;
	});

	FString ObjectBody = FString::Printf(TEXT("export const %s = {\n"), *ObjectName);
	for (const FNavInfo& Event : Events)
		ObjectBody += FString::Printf(TEXT("\t%s: '%s' as const,\n"), *Event.Identifier, *Event.EventName);

	if (Events.Num() > 0)
		ObjectBody += TEXT("\n");

	for (const FNavInfo& Event : Events)
	{
		ObjectBody += FString::Printf(
			TEXT("\ton%s(fn: (detail: unknown) => void): () => void {\n")
			TEXT("\t\treturn Swui.onEvent(%s.%s, fn);\n")
			TEXT("\t},\n"),
			*Event.Identifier,
			*ObjectName,
			*Event.Identifier);
	}
	ObjectBody += TEXT("};\n\n");
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

bool FSwuiTSGenerator::GeneratePreview(USwui* Bridge, const TArray<FSwuiNavigationEvent>& NavigationEvents)
{
	if (!Bridge || Bridge->InterfaceName.IsEmpty()) return false;
	if (Bridge->BindingSources.IsEmpty()) return false;

	// ── Helpers ───────────────────────────────────────────────────────────────

	auto QuoteStr = [](const FString& S) -> FString
	{
		return TEXT("'") + S + TEXT("'");
	};

	// ── State section ─────────────────────────────────────────────────────────

	FString StateBody;

	for (const FSwuiBindingSource& Source : Bridge->BindingSources)
	{
		UClass* SourceClass = Source.SourceClass;
		if (!SourceClass) continue;

		const FString Namespace = SwuiComputeNamespace(SourceClass);

		for (const FName& PropFName : Source.Properties)
		{
			FProperty* Prop = SourceClass->FindPropertyByName(PropFName);
			if (!Prop) continue;

			const FString TSType = SwuiGetTSType(Prop);
			if (TSType.IsEmpty()) continue;

			const FString PropName = PropFName.ToString();
			const FString FullKey  = Namespace + TEXT(".") + PropName;

			auto GetMeta = [&](const TCHAR* Key) -> FString
			{
				return Prop->HasMetaData(Key) ? Prop->GetMetaData(Key) : FString();
			};

			FString ClampMin = GetMeta(TEXT("ClampMin"));
			if (ClampMin.IsEmpty()) ClampMin = GetMeta(TEXT("UIMin"));
			FString ClampMax = GetMeta(TEXT("ClampMax"));
			if (ClampMax.IsEmpty()) ClampMax = GetMeta(TEXT("UIMax"));

			StateBody += FString::Printf(TEXT("\t\t%s: {\n"), *QuoteStr(FullKey));
			StateBody += FString::Printf(TEXT("\t\t\ttype: %s,\n"), *QuoteStr(TSType));

			// Default value
			if (TSType == TEXT("number"))
			{
				StateBody += TEXT("\t\t\tdefaultValue: 0,\n");
				if (!ClampMin.IsEmpty())
					StateBody += FString::Printf(TEXT("\t\t\tmin: %s,\n"), *ClampMin);
				if (!ClampMax.IsEmpty())
					StateBody += FString::Printf(TEXT("\t\t\tmax: %s,\n"), *ClampMax);
				StateBody += TEXT("\t\t\tstep: 1,\n");
			}
			else if (TSType == TEXT("boolean"))
			{
				StateBody += TEXT("\t\t\tdefaultValue: false,\n");
			}
			else // string
			{
				StateBody += TEXT("\t\t\tdefaultValue: '',\n");
			}

			StateBody += FString::Printf(TEXT("\t\t\tlabel: %s,\n"), *QuoteStr(PropName));
			StateBody += TEXT("\t\t},\n");
		}
	}

	// ── Events section ────────────────────────────────────────────────────────

	FString EventsBody;

	for (const FSwuiBindingSource& Source : Bridge->BindingSources)
	{
		UClass* SourceClass = Source.SourceClass;
		if (!SourceClass) continue;

		const FString Namespace = SwuiComputeNamespace(SourceClass);

		for (const FName& DelegateFName : Source.Delegates)
		{
			const FString DelegateName = DelegateFName.ToString();
			const FString FullKey      = Namespace + TEXT(".") + DelegateName;

			EventsBody += FString::Printf(TEXT("\t\t%s: {\n"), *QuoteStr(FullKey));
			EventsBody += FString::Printf(TEXT("\t\t\tlabel: %s,\n"), *QuoteStr(DelegateName));
			EventsBody += TEXT("\t\t\tpayload: {},\n");
			EventsBody += TEXT("\t\t},\n");
		}
	}

	// ── Navigation section ────────────────────────────────────────────────────

	FString NavBody;

	for (const FSwuiNavigationEvent& NavEvent : NavigationEvents)
	{
		if (!NavEvent.Event.IsValid() || !NavEvent.bForwardToJS)
			continue;

		const FString EventName = NavEvent.GetEffectiveJsEventName();
		if (EventName.IsEmpty()) continue;

		const FString Identifier = SwuiMakeNavigationIdentifier(EventName);

		NavBody += FString::Printf(TEXT("\t\t%s: {\n"), *QuoteStr(Identifier));
		NavBody += FString::Printf(TEXT("\t\t\ttag: %s,\n"), *QuoteStr(EventName));
		NavBody += FString::Printf(TEXT("\t\t\tlabel: %s,\n"), *QuoteStr(Identifier));
		NavBody += TEXT("\t\t\tpayload: {},\n");
		NavBody += TEXT("\t\t},\n");
	}

	// ── Assemble file ─────────────────────────────────────────────────────────

	const FString& IName = Bridge->InterfaceName;

	FString Output = FString::Printf(TEXT(
		"// %s.preview.generated.ts\n"
		"// AUTO-GENERATED by SimpleWebUI \u2014 do not edit manually.\n"
		"// Re-generate via: Tools > SimpleWebUI > Refresh JS Bindings\n"
		"\n"
		"import type { PreviewSchema } from '../src/swui-preview/types';\n"
		"\n"
		"export const %sPreviewSchema = {\n"
		"\tstate: {\n"
		"%s"
		"\t},\n"
		"\n"
		"\tevents: {\n"
		"%s"
		"\t},\n"
		"\n"
		"\tnavigation: {\n"
		"%s"
		"\t},\n"
		"} as const satisfies PreviewSchema;\n"
	),
		*IName,
		*IName,
		*StateBody,
		*EventsBody,
		*NavBody
	);

	const FString OutDir  = FPaths::ProjectContentDir() / TEXT("UI/generated");
	const FString OutFile = OutDir / IName + TEXT(".preview.generated.ts");

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*OutDir))
		PlatformFile.CreateDirectoryTree(*OutDir);

	if (FFileHelper::SaveStringToFile(Output, *OutFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogTemp, Log, TEXT("SWUI: Generated preview schema '%s'"), *OutFile);
		return true;
	}

	UE_LOG(LogTemp, Error, TEXT("SWUI: Failed to write preview schema '%s'"), *OutFile);
	return false;
}