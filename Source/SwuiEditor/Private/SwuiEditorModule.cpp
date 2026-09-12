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
#include "Misc/FileHelper.h"
#include "Editor.h"

#include "Swui.h"
#include "SwuiNavigation.h"
#include "SwuiDetails.h"
#include "SwuiNavigationDetails.h"
#include "SwuiTSGenerator.h"
#include "SwuiSettings.h"
#include "SwuiBindingCollector.h"
#include "K2Node_SwuiObserve.h"
#include "K2Node_SwuiObserveEvent.h"

#include "HAL/ConsoleManager.h"

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

		// Console command for code-only regeneration workflow.
		IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("SwuiRegenerateBindings"),
			TEXT("Regenerate all SWUI TypeScript bindings. Call from console, BP, or AS via ExecuteConsoleCommand."),
			FConsoleCommandDelegate::CreateRaw(this, &FSwuiEditorModule::OnRefreshAllBindings),
			ECVF_Default);

		MaybeLaunchDevServer();
		ExcludeContentUiFromAutoReimport();

		// NOTE: Auto-regeneration of TS bindings was removed because it caused
		// editor crashes when switching between Blueprint assets (the binding
		// collector + generator weren't safe against the editor GC / BP
		// compilation cycle). The original approach hooked into:
		//
		//   FCoreUObjectDelegates::ReloadCompleteDelegate   — C++ Hot Reload
		//   FCoreUObjectDelegates::OnObjectPropertyChanged  — property edits
		//   FCoreUObjectDelegates::OnAssetLoaded            — BP asset load
		//   FEditorDelegates::EndPIE                        — PIE end flush
		//   AngelScript PostCompile/ReloadHadErrors         — AS compile
		//
		// All fed into SwuiScheduleAutoRegeneration() with an 800ms debounce.
		// If re-enabling, re-add the `.AddRaw` bindings above and the
		// corresponding `.RemoveAll` in ShutdownModule, plus the handler
		// functions. For now, use Tools > SimpleWebUI > Refresh JS Bindings
		// to regenerate manually.
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);

		IConsoleManager::Get().UnregisterConsoleObject(TEXT("SwuiRegenerateBindings"));

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

	}

private:
	TSharedPtr<FSwuiNodeFactory> NodeFactory;

	// ── Auto-reimport exclusion for Content/UI ───────────────────────────

	/** Write a .uassetignore into Content/UI so UE's asset discovery skips it.
	 *  Also writes the editor ini setting so auto-reimport ignores it. */
	void ExcludeContentUiFromAutoReimport()
	{
		const FString UIDir = FPaths::ProjectContentDir() / TEXT("UI");
		const FString IgnoreFile = UIDir / TEXT(".uassetignore");
		if (!FPaths::FileExists(IgnoreFile))
		{
			FFileHelper::SaveStringToFile(TEXT("# Ignored by SWUI — web UI sources are not UE assets\n"), *IgnoreFile);
		}
	}

	// ── Dev server ────────────────────────────────────────────────────────

	void MaybeLaunchDevServer()
	{
		const USwuiSettings* Settings = GetDefault<USwuiSettings>();
		if (!Settings || !Settings->bAutoLaunchDevServer) return;

		const FString ContentUI = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("Content/UI"));

		if (!FPaths::DirectoryExists(ContentUI / TEXT("../.."))) return;

		// Kill any stale vite process on port 5173 so CEF can reconnect.
		FPlatformProcess::CreateProc(
			TEXT("pwsh.exe"),
			TEXT("-NoProfile -Command \"$p = Get-NetTCPConnection -LocalPort 5173 -ErrorAction SilentlyContinue | Select-Object -First 1; if ($p) { Stop-Process -Id $p.OwningProcess -Force }\""),
			true, false, false, nullptr, 0, nullptr, nullptr);

		FPlatformProcess::CreateProc(
			TEXT("cmd.exe"),
			TEXT("/c pnpm dev"),
			true, true, true, nullptr, 0, *ContentUI, nullptr);

		UE_LOG(LogTemp, Log, TEXT("SWUI: Launched dev server (hidden) in %s"), *ContentUI);
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
