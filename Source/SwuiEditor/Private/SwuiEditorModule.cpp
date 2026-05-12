#include "Modules/ModuleManager.h"
#include "Modules/ModuleInterface.h"
#include "PropertyEditorModule.h"
#include "LevelEditor.h"
#include "ToolMenus.h"
#include "EngineUtils.h"
#include "EdGraphUtilities.h"
#include "SGraphNode.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"
#include "Editor.h"
// FCoreUObjectDelegates::ReloadCompleteDelegate, OnObjectPropertyChanged, OnAssetLoaded
#include "UObject/UObjectGlobals.h"

#if SWUI_WITH_ANGELSCRIPT
// AngelScript compile delegates (conditional: gated by SWUI_WITH_ANGELSCRIPT)
#include "AngelscriptCodeModule.h"
#endif

#include "Swui.h"
#include "SwuiNavigation.h"
#include "SwuiDetails.h"
#include "SwuiNavigationDetails.h"
#include "SwuiTSGenerator.h"
#include "SwuiBindingCollector.h"
#include "K2Node_SwuiObserve.h"
#include "K2Node_SwuiObserveEvent.h"

#define LOCTEXT_NAMESPACE "SwuiEditor"

class FSwuiNodeFactory : public FGraphPanelNodeFactory
{
public:
	virtual TSharedPtr<SGraphNode> CreateNode(UEdGraphNode* Node) const override
	{
		if (UK2Node_SwuiObserve*      N = Cast<UK2Node_SwuiObserve>(Node))      return N->CreateVisualWidget();
		if (UK2Node_SwuiObserveEvent* N = Cast<UK2Node_SwuiObserveEvent>(Node)) return N->CreateVisualWidget();
		return nullptr;
	}
};

class FSwuiEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FPropertyEditorModule& PropertyModule =
			FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

		PropertyModule.RegisterCustomClassLayout(
			USwui::StaticClass()->GetFName(),
			FOnGetDetailCustomizationInstance::CreateStatic(&FSwuiDetails::MakeInstance));

		PropertyModule.RegisterCustomClassLayout(
			USwuiNavigation::StaticClass()->GetFName(),
			FOnGetDetailCustomizationInstance::CreateStatic(&FSwuiNavigationDetails::MakeInstance));

		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FSwuiEditorModule::RegisterMenus));

		NodeFactory = MakeShareable(new FSwuiNodeFactory);
		FEdGraphUtilities::RegisterVisualNodeFactory(NodeFactory);

		MaybeLaunchDevServer();

		// ── Auto-regeneration hooks ────────────────────────────────────────
		// All hooks feed into the unified SwuiScheduleAutoRegeneration path.

		// Primary C++ reload hook: covers Hot Reload + Live Coding patch.
		FCoreUObjectDelegates::ReloadCompleteDelegate.AddRaw(
			this,
			&FSwuiEditorModule::OnReloadComplete);

		// React to property changes on any USwui asset (BindingSources, InterfaceName, etc.)
		FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(
			this,
			&FSwuiEditorModule::OnObjectPropertyChanged);

		// React to asset load completion
		FCoreUObjectDelegates::OnAssetLoaded.AddRaw(
			this,
			&FSwuiEditorModule::OnAssetLoaded);

		// Flush deferred regeneration after PIE ends
		FEditorDelegates::EndPIE.AddRaw(
			this,
			&FSwuiEditorModule::OnEndPIE);

#if SWUI_WITH_ANGELSCRIPT
		// ── Conditional AngelScript compile hook ───────────────────────────
		if (SwuiProjectLooksLikeAngelScriptProject())
		{
			// Dynamic module lookup — module is linked but may not be loaded yet
			FAngelscriptCodeModule* ASModule =
				FModuleManager::GetModulePtr<FAngelscriptCodeModule>(TEXT("AngelscriptCode"));
			if (ASModule)
			{
				// Bind to success-only post-compile delegate
				ASModule->GetPostCompile().AddRaw(this, &FSwuiEditorModule::OnAngelScriptPostCompile);

				// Bind failure handler for logging only (no regeneration)
				ASModule->GetReloadHadErrors().AddRaw(this, &FSwuiEditorModule::OnAngelScriptCompileFailed);

				UE_LOG(LogTemp, Log, TEXT("SWUI: Bound AngelScript compile success/failure hooks."));
			}
			else
			{
				UE_LOG(LogTemp, Log, TEXT("SWUI: AngelScript project detected but AngelscriptCode module not yet loaded — will retry on module load."));
				FModuleManager::Get().OnModulesChanged().AddRaw(this, &FSwuiEditorModule::OnModulesChanged);
				bPendingASBind = true;
			}
		}
#endif // SWUI_WITH_ANGELSCRIPT
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);

		if (NodeFactory.IsValid())
		{
			FEdGraphUtilities::UnregisterVisualNodeFactory(NodeFactory);
			NodeFactory.Reset();
		}

		if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
		{
			FPropertyEditorModule& PropertyModule =
				FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
			PropertyModule.UnregisterCustomClassLayout(USwui::StaticClass()->GetFName());
			PropertyModule.UnregisterCustomClassLayout(USwuiNavigation::StaticClass()->GetFName());
		}

		FCoreUObjectDelegates::ReloadCompleteDelegate.RemoveAll(this);
		FCoreUObjectDelegates::OnObjectPropertyChanged.RemoveAll(this);
		FCoreUObjectDelegates::OnAssetLoaded.RemoveAll(this);
		FEditorDelegates::EndPIE.RemoveAll(this);

#if SWUI_WITH_ANGELSCRIPT
		if (bPendingASBind)
		{
			FModuleManager::Get().OnModulesChanged().RemoveAll(this);
		}
#endif
	}

private:
	TSharedPtr<FSwuiNodeFactory> NodeFactory;

#if SWUI_WITH_ANGELSCRIPT
	bool bPendingASBind = false;
#endif

	// ── Auto-regeneration event handlers ──────────────────────────────────

	void OnReloadComplete(EReloadCompleteReason Reason)
	{
		SwuiScheduleAutoRegeneration(TEXT("C++ reload complete"));
	}

	void OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
	{
		if (!Object || !Object->IsA<USwui>()) return;
		if (!Event.MemberProperty) return;

		const FName PropName = Event.MemberProperty->GetFName();
		if (PropName == GET_MEMBER_NAME_CHECKED(USwui, InterfaceName) ||
			PropName == GET_MEMBER_NAME_CHECKED(USwui, BindingSources))
		{
			SwuiScheduleAutoRegeneration(FString::Printf(TEXT("property change: %s"), *PropName.ToString()));
		}
	}

	void OnAssetLoaded(UObject* Asset)
	{
		if (Asset && Asset->IsA<USwui>())
		{
			SwuiScheduleAutoRegeneration(TEXT("asset loaded"));
		}
	}

	void OnEndPIE(bool bSimulating)
	{
		if (GSwuiPendingRegenerationAfterPIE)
		{
			GSwuiPendingRegenerationAfterPIE = false;
			SwuiScheduleAutoRegeneration(TEXT("deferred after PIE"));
		}
	}

#if SWUI_WITH_ANGELSCRIPT
	void OnAngelScriptPostCompile()
	{
		SwuiScheduleAutoRegeneration(TEXT("AngelScript compile success"));
	}

	void OnAngelScriptCompileFailed()
	{
		UE_LOG(LogTemp, Warning, TEXT("SWUI: Skipping JS binding regeneration because AngelScript compile failed."));
	}

	void OnModulesChanged(FName ModuleName, EModuleChangeReason Reason)
	{
		if (!bPendingASBind) return;
		if (Reason != EModuleChangeReason::ModuleLoaded) return;
		if (ModuleName != TEXT("AngelscriptCode")) return;

		// Retry binding now that AngelscriptCode is loaded
		bPendingASBind = false;
		FModuleManager::Get().OnModulesChanged().RemoveAll(this);

		FAngelscriptCodeModule* ASModule =
			FModuleManager::GetModulePtr<FAngelscriptCodeModule>(TEXT("AngelscriptCode"));
		if (ASModule)
		{
			ASModule->GetPostCompile().AddRaw(this, &FSwuiEditorModule::OnAngelScriptPostCompile);
			ASModule->GetReloadHadErrors().AddRaw(this, &FSwuiEditorModule::OnAngelScriptCompileFailed);
			UE_LOG(LogTemp, Log, TEXT("SWUI: Bound AngelScript hooks (deferred on module load)."));
		}
	}
#endif // SWUI_WITH_ANGELSCRIPT

	// ── Dev server ────────────────────────────────────────────────────────

	void MaybeLaunchDevServer()
	{
		bool bAutoLaunch = true;
		GConfig->GetBool(TEXT("SimpleWebUI"), TEXT("AutoLaunchDevServer"), bAutoLaunch, GEditorIni);
		if (!bAutoLaunch) return;

		const FString ContentUI = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("Content/UI"));

		if (!FPaths::DirectoryExists(ContentUI / TEXT("../.."))) return;

		const FString Cmd = FString::Printf(
			TEXT("start \"SWUI Dev\" /D \"%s\" cmd.exe /k pnpm dev"),
			*ContentUI);

		FPlatformProcess::CreateProc(
			TEXT("cmd.exe"),
			*FString::Printf(TEXT("/c %s"), *Cmd),
			true, false, false, nullptr, 0, nullptr, nullptr);

		UE_LOG(LogTemp, Log, TEXT("SWUI: Launched dev server in %s"), *ContentUI);
	}

	// ── Menu ──────────────────────────────────────────────────────────────

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);

		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
		FToolMenuSection& Section = Menu->FindOrAddSection("SimpleWebUI");
		Section.Label = LOCTEXT("MenuSectionLabel", "SimpleWebUI");

		Section.AddMenuEntry(
			"RefreshJSBindings",
			LOCTEXT("RefreshJSBindings", "Refresh JS Bindings"),
			LOCTEXT("RefreshJSBindingsTip",
				"Re-generates TypeScript bindings for all USwui components in the project."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FSwuiEditorModule::OnRefreshAllBindings)));
	}

	void OnRefreshAllBindings()
	{
		const bool bOK = SwuiRegenerateAllBindings();
		UE_LOG(LogTemp, Log, TEXT("SWUI: Manual refresh completed %s."), bOK ? TEXT("successfully") : TEXT("with errors"));
	}
};

#undef LOCTEXT_NAMESPACE
IMPLEMENT_MODULE(FSwuiEditorModule, SwuiEditor)
