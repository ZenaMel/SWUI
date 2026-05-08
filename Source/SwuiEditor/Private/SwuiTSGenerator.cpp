#include "SwuiTSGenerator.h"
#include "Swui.h"
#include "SwuiBindingSource.h"
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