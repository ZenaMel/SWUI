#include "SwuiBindingCollector.h"
#include "Swui.h"
#include "SwuiNavigation.h"
#include "SwuiBindingSource.h"
#include "SwuiTSGenerator.h"

#include "EngineUtils.h"
#include "Editor.h"
#include "UObject/UObjectIterator.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

// ── Internal helpers ──────────────────────────────────────────────────────

static FString SwuiNS(UClass* Src)
{
	if (!Src) return TEXT("swui");
	FString N = Src->GetName();
	if (N.StartsWith(TEXT("A")) || N.StartsWith(TEXT("U"))) N = N.RightChop(1);
	return N.ToLower();
}

static FString SwuiON(UClass* Src)
{
	if (!Src) return TEXT("Swui");
	FString N = Src->GetName();
	if (N.EndsWith(TEXT("_C"))) N = N.LeftChop(2);
	if ((N.StartsWith(TEXT("A")) || N.StartsWith(TEXT("U"))) && N.Len() > 1 && FChar::IsUpper(N[1]))
		N = N.RightChop(1);
	return N;
}

// ── Effective binding collection ───────────────────────────────────────────

FSwuiEffectiveBindings SwuiCollectEffectiveBindings(USwui* Bridge)
{
	FSwuiEffectiveBindings Result;
	if (!Bridge) return Result;

	for (int32 i = 0; i < Bridge->BindingSources.Num(); ++i)
	{
		const FSwuiBindingSource& Source = Bridge->BindingSources[i];
		UClass* SourceClass = Source.SourceClass;
		if (!SourceClass) continue;

		// ── Validate that no UFUNCTIONS are misusing SwuiExpose ───────────
		for (TFieldIterator<UFunction> FnIt(SourceClass, EFieldIteratorFlags::ExcludeSuper); FnIt; ++FnIt)
		{
			if (FnIt->HasMetaData(TEXT("SwuiExpose")))
			{
				Result.Warnings.Add(FString::Printf(
					TEXT("SWUI: SwuiExpose is not supported on UFUNCTION/BlueprintEvent '%s'. ")
					TEXT("Use SwuiExpose on a UPROPERTY(BlueprintVisible) or ")
					TEXT("multicast delegate property instead. Generation BLOCKED."),
					*FnIt->GetName()));
				// Report the error but don't block generation entirely — log and continue
			}
		}

		// ── Validate class-level SwuiExpose ──────────────────────────────
		if (SourceClass->HasMetaData(TEXT("SwuiExpose")))
		{
			Result.Warnings.Add(FString::Printf(
				TEXT("SWUI: SwuiExpose on class '%s' metadata is not supported. ")
				TEXT("Use SwuiExpose on individual UPROPERTY members instead. Generation BLOCKED."),
				*SourceClass->GetName()));
		}

		FSwuiEffectiveSource EffSrc;
		EffSrc.SourceClass = SourceClass;
		EffSrc.ObjectName  = SwuiON(SourceClass);
		EffSrc.Namespace   = SwuiNS(SourceClass);

		// ── Code-exposed properties ──────────────────────────────────────
		TSet<FName> CodeExposedProps;
		for (TFieldIterator<FProperty> It(SourceClass); It; ++It)
		{
			if (!It->HasMetaData(TEXT("SwuiExpose"))) continue;
			if (!It->HasAnyPropertyFlags(CPF_BlueprintVisible))
			{
				Result.Warnings.Add(FString::Printf(
					TEXT("SWUI: Ignored SwuiExpose on %s — only BlueprintVisible properties are supported."),
					*It->GetName()));
				continue;
			}
			if (SwuiGetTSType(*It).IsEmpty())
			{
				Result.Warnings.Add(FString::Printf(
					TEXT("SWUI: Ignored SwuiExpose on %s — type %s is not supported."),
					*It->GetName(), *It->GetClass()->GetName()));
				continue;
			}

			const FName PN = It->GetFName();
			CodeExposedProps.Add(PN);
			FSwuiEffectiveProperty& EP = EffSrc.Properties.AddDefaulted_GetRef();
			EP.PropName = PN;
			EP.Origin   = Source.Properties.Contains(PN)
				? ESwuiBindingOrigin::Both
				: ESwuiBindingOrigin::CodeExposed;
		}

		// ── Code-exposed delegates ───────────────────────────────────────
		TSet<FName> CodeExposedDelegates;
		for (TFieldIterator<FMulticastDelegateProperty> It(SourceClass); It; ++It)
		{
			if (!It->HasMetaData(TEXT("SwuiExpose"))) continue;
			if (!It->HasAnyPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintAssignable))
			{
				Result.Warnings.Add(FString::Printf(
					TEXT("SWUI: Ignored SwuiExpose on %s — only BlueprintVisible/BlueprintAssignable delegates are supported."),
					*It->GetName()));
				continue;
			}

			const FName DN = It->GetFName();
			CodeExposedDelegates.Add(DN);
			FSwuiEffectiveDelegate& ED = EffSrc.Delegates.AddDefaulted_GetRef();
			ED.DelegateName = DN;
			ED.Origin       = Source.Delegates.Contains(DN)
				? ESwuiBindingOrigin::Both
				: ESwuiBindingOrigin::CodeExposed;
		}

		// ── Manual-only properties (not already code-exposed) ────────────
		for (const FName& PN : Source.Properties)
		{
			if (CodeExposedProps.Contains(PN)) continue;
			FProperty* Prop = SourceClass->FindPropertyByName(PN);
			if (!Prop || SwuiGetTSType(Prop).IsEmpty()) continue;
			FSwuiEffectiveProperty& EP = EffSrc.Properties.AddDefaulted_GetRef();
			EP.PropName = PN;
			EP.Origin   = ESwuiBindingOrigin::Manual;
		}

		// ── Manual-only delegates ────────────────────────────────────────
		for (const FName& DN : Source.Delegates)
		{
			if (CodeExposedDelegates.Contains(DN)) continue;
			FSwuiEffectiveDelegate& ED = EffSrc.Delegates.AddDefaulted_GetRef();
			ED.DelegateName = DN;
			ED.Origin       = ESwuiBindingOrigin::Manual;
		}

		if (!EffSrc.Properties.IsEmpty() || !EffSrc.Delegates.IsEmpty())
		{
			Result.Sources.Add(MoveTemp(EffSrc));
		}
	}

	// ── Auto-discover classes with SwuiExpose members not in BindingSources ──
	// These become effective sources without modifying the saved BindingSources.
	TSet<UClass*> BoundClasses;
	for (const auto& Src : Bridge->BindingSources)
		if (Src.SourceClass) BoundClasses.Add(Src.SourceClass);

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Cls = *It;
		if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;
		if (Cls->GetName().StartsWith(TEXT("SKEL_")) || Cls->GetName().StartsWith(TEXT("REINST_"))) continue;
		if (BoundClasses.Contains(Cls)) continue;
		if (!Cls->IsChildOf<AActor>() && !Cls->IsChildOf<UActorComponent>()) continue;

		// ── SwuiEvent on UFUNCTION ────────────────────────────
		// Collected by the navigation event generator separately.
		// SwuiEvent="onev.rooms.host" declares the navigation event tag inline.
		for (TFieldIterator<UFunction> FnIt(Cls, EFieldIteratorFlags::ExcludeSuper); FnIt; ++FnIt)
		{
			if (FnIt->HasMetaData(TEXT("SwuiEvent")))
			{
				UE_LOG(LogTemp, Verbose, TEXT("SWUI: SwuiEvent='%s' on UFUNCTION '%s' — will emit as navigation event."),
					*FnIt->GetMetaData(TEXT("SwuiEvent")), *FnIt->GetName());
			}
		}

		// ── Validate: class-level SwuiExpose is invalid ─────────────
		if (Cls->HasMetaData(TEXT("SwuiExpose")))
		{
			Result.Warnings.Add(FString::Printf(
				TEXT("SWUI: SwuiExpose on class '%s' metadata is not supported. ")
				TEXT("Use SwuiExpose on individual UPROPERTY members instead. Generation BLOCKED."),
				*Cls->GetName()));
		}

		// Scan for any SwuiExpose member on supported item types (properties, delegates, functions)
		bool bHasExpose = false;
		for (TFieldIterator<FProperty> PIt(Cls); PIt && !bHasExpose; ++PIt)
			if (PIt->HasMetaData(TEXT("SwuiExpose"))) bHasExpose = true;
		for (TFieldIterator<FMulticastDelegateProperty> DIt(Cls); DIt && !bHasExpose; ++DIt)
			if (DIt->HasMetaData(TEXT("SwuiExpose"))) bHasExpose = true;
		for (TFieldIterator<UFunction> FIt(Cls, EFieldIteratorFlags::ExcludeSuper); FIt && !bHasExpose; ++FIt)
			if (FIt->HasMetaData(TEXT("SwuiEvent"))) bHasExpose = true;

		if (!bHasExpose) continue;

		FSwuiEffectiveSource AutoSrc;
		AutoSrc.SourceClass = Cls;
		AutoSrc.ObjectName  = SwuiON(Cls);
		AutoSrc.Namespace   = SwuiNS(Cls);
		AutoSrc.bAutoDiscovered = true;

		// Collect exposed members
		for (TFieldIterator<FProperty> PIt(Cls); PIt; ++PIt)
		{
			if (!PIt->HasMetaData(TEXT("SwuiExpose"))) continue;
			if (!PIt->HasAnyPropertyFlags(CPF_BlueprintVisible)) continue;
			if (SwuiGetTSType(*PIt).IsEmpty()) continue;
			FSwuiEffectiveProperty& EP = AutoSrc.Properties.AddDefaulted_GetRef();
			EP.PropName = PIt->GetFName();
			EP.Origin   = ESwuiBindingOrigin::CodeExposed;
		}
		for (TFieldIterator<FMulticastDelegateProperty> DIt(Cls); DIt; ++DIt)
		{
			if (!DIt->HasMetaData(TEXT("SwuiExpose"))) continue;
			if (!DIt->HasAnyPropertyFlags(CPF_BlueprintVisible | CPF_BlueprintAssignable)) continue;
			FSwuiEffectiveDelegate& ED = AutoSrc.Delegates.AddDefaulted_GetRef();
			ED.DelegateName = DIt->GetFName();
			ED.Origin       = ESwuiBindingOrigin::CodeExposed;
		}

		Result.Sources.Add(MoveTemp(AutoSrc));
	}

	return Result;
}

// ── Asset discovery ────────────────────────────────────────────────────────

TArray<USwui*> SwuiFindAllSwuiAssets()
{
	TArray<USwui*> Results;
	TSet<USwui*> Seen;

	// 1. Editor-world actor components
	if (GEditor)
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (World)
		{
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (USwui* C = It->FindComponentByClass<USwui>())
				{
					if (!Seen.Contains(C)) { Seen.Add(C); Results.Add(C); }
				}
			}
		}
	}

	// 2. All loaded USwui instances (non-template, non-CDO, non-REINST)
	for (TObjectIterator<USwui> It; It; ++It)
	{
		USwui* C = *It;
		if (C->IsTemplate()) continue;
		if (C->HasAnyFlags(RF_ClassDefaultObject)) continue;
		if (C->GetOutermost()->GetName().StartsWith(TEXT("/Temp/"))) continue;
		if (Seen.Contains(C)) continue;
		Seen.Add(C);
		Results.Add(C);
	}

	return Results;
}

// ── Per-asset regeneration ────────────────────────────────────────────────

FSwuiGenerationResult SwuiRegenerateAsset(USwui* Bridge)
{
	FSwuiGenerationResult R;
	if (!Bridge)
	{
		R.AssetName = TEXT("(null)");
		R.bSuccess  = false;
		return R;
	}

	R.AssetName = Bridge->GetPathName();
	const bool bOK = FSwuiTSGenerator::Generate(Bridge);

	// Collect warnings from effective bindings
	FSwuiEffectiveBindings B = SwuiCollectEffectiveBindings(Bridge);
	R.bHadWarnings = !B.Warnings.IsEmpty();
	R.Messages = B.Warnings;

	// Also generate navigation bindings for any sibling USwuiNavigation
	if (Bridge->GetOwner())
	{
		if (USwuiNavigation* Nav = Bridge->GetOwner()->FindComponentByClass<USwuiNavigation>())
		{
			bool bNavOK = FSwuiTSGenerator::GenerateNavigation(Bridge, Nav->NavigationEvents);
			if (bNavOK)
			{
				UE_LOG(LogTemp, Log, TEXT("SWUI: Generated navigation bindings for '%s'."), *Nav->GetPathName());
			}
		}
	}

	R.bSuccess = bOK;
	return R;
}

// ── Regenerate all ────────────────────────────────────────────────────────

bool SwuiRegenerateAllBindings()
{
	TArray<USwui*> AllAssets = SwuiFindAllSwuiAssets();
	if (AllAssets.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("SWUI: No USwui assets found to regenerate."));
		return false;
	}

	int32 SuccessCount = 0;
	int32 FailCount    = 0;
	int32 WarnCount    = 0;

	for (USwui* Asset : AllAssets)
	{
		if (!Asset->InterfaceName.IsEmpty())
		{
			FSwuiGenerationResult R = SwuiRegenerateAsset(Asset);
			if (R.bSuccess)
			{
				++SuccessCount;
				if (R.bHadWarnings) ++WarnCount;
				for (const FString& W : R.Messages)
					UE_LOG(LogTemp, Warning, TEXT("%s"), *W);
			}
			else
			{
				++FailCount;
				UE_LOG(LogTemp, Error, TEXT("SWUI: Generation failed for %s"), *R.AssetName);
			}
		}
	}

	if (FailCount > 0)
	{
		UE_LOG(LogTemp, Error, TEXT("SWUI: Regenerated %d OK (%d with warnings), %d FAILED out of %d total."),
			SuccessCount, WarnCount, FailCount, AllAssets.Num());
		return false;
	}

	if (WarnCount > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("SWUI: Regenerated %d assets successfully with %d warning(s)."),
			SuccessCount, WarnCount);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("SWUI: Regenerated %d assets successfully."), SuccessCount);
	}

	// ── Navigation bindings: iterate all USwuiNavigation instances ─────────
	for (TObjectIterator<USwuiNavigation> It; It; ++It)
	{
		USwuiNavigation* Nav = *It;
		if (Nav->IsTemplate()) continue;
		if (Nav->HasAnyFlags(RF_ClassDefaultObject)) continue;
		if (Nav->GetOutermost()->GetName().StartsWith(TEXT("/Temp/"))) continue;

		// Find the sibling USwui for this navigation component
		USwui* Swui = nullptr;
		if (AActor* Owner = Nav->GetOwner())
		{
			Swui = Owner->FindComponentByClass<USwui>();
		}

		if (!Swui || Swui->InterfaceName.IsEmpty()) continue;

		// Skip if already generated via SwuiRegenerateAsset sibling path
		if (AllAssets.Contains(Swui)) continue;

		bool bNavOK = FSwuiTSGenerator::GenerateNavigation(Swui, Nav->NavigationEvents);
		if (bNavOK)
		{
			UE_LOG(LogTemp, Log, TEXT("SWUI: Generated navigation bindings for '%s'."), *Nav->GetPathName());
		}
	}

	return FailCount == 0;
}

// ── Debounced auto-regeneration ────────────────────────────────────────────

static FTSTicker::FDelegateHandle GSwuiAutoRegenHandle;
static bool GSwuiAutoRegenPending = false;

bool GSwuiPendingRegenerationAfterPIE = false;

void SwuiScheduleAutoRegeneration(const FString& Reason)
{
	UE_LOG(LogTemp, Log, TEXT("SWUI: Auto-regeneration triggered: %s"), *Reason);
	GSwuiAutoRegenPending = true;

	if (GSwuiAutoRegenHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GSwuiAutoRegenHandle);
	}

	constexpr float Debounce = 0.8f;
	GSwuiAutoRegenHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([](float) -> bool
		{
			if (!GSwuiAutoRegenPending) return false;
			GSwuiAutoRegenPending = false;
			GSwuiAutoRegenHandle.Reset();

			if (!GEditor) return false;

			// Defer regeneration if PIE is active; flush on EndPIE
			if (GEditor->PlayWorld)
			{
				GSwuiPendingRegenerationAfterPIE = true;
				UE_LOG(LogTemp, Log, TEXT("SWUI: Deferring regeneration until after PIE ends."));
				return false;
			}

			SwuiRegenerateAllBindings();
			return false;
		}),
		Debounce);
}

// ── AngelScript project detection ───────────────────────────────────────────

bool SwuiProjectLooksLikeAngelScriptProject()
{
	const FString ScriptDir = FPaths::ProjectDir() / TEXT("Script");

	if (!FPaths::DirectoryExists(ScriptDir))
	{
		return false;
	}

	TArray<FString> ScriptFiles;
	IFileManager::Get().FindFilesRecursive(
		ScriptFiles,
		*ScriptDir,
		TEXT("*.as"),
		true,
		false
	);

	return ScriptFiles.Num() > 0;
}
