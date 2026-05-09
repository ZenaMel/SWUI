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

#include "Swui.h"
#include "SwuiNavigation.h"
#include "SwuiDetails.h"
#include "SwuiNavigationDetails.h"
#include "SwuiTSGenerator.h"
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
	}

private:
	TSharedPtr<FSwuiNodeFactory> NodeFactory;

	void MaybeLaunchDevServer()
	{
		// Opt-out: add  [SimpleWebUI]  AutoLaunchDevServer=False  to DefaultEditor.ini
		bool bAutoLaunch = true;
		GConfig->GetBool(TEXT("SimpleWebUI"), TEXT("AutoLaunchDevServer"), bAutoLaunch, GEditorIni);
		if (!bAutoLaunch) return;

		// Expect Content/UI to contain a package.json with a "dev" script.
		const FString ContentUI = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("Content/UI"));

		if (!FPaths::DirectoryExists(ContentUI / TEXT("../.."))) return; // sanity

		// Spawn: start "SWUI Dev" /D "<ContentUI>" cmd.exe /k pnpm dev
		const FString Cmd = FString::Printf(
			TEXT("start \"SWUI Dev\" /D \"%s\" cmd.exe /k pnpm dev"),
			*ContentUI);

		FPlatformProcess::CreateProc(
			TEXT("cmd.exe"),
			*FString::Printf(TEXT("/c %s"), *Cmd),
			/*bLaunchDetached=*/true,
			/*bLaunchHidden=*/false,
			/*bLaunchReallyHidden=*/false,
			/*OutProcessID=*/nullptr,
			/*PriorityModifier=*/0,
			/*OptionalWorkingDirectory=*/nullptr,
			/*PipeWriteChild=*/nullptr
		);

		UE_LOG(LogTemp, Log, TEXT("SWUI: Launched dev server in %s"), *ContentUI);
	}

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
			"Re-generates TypeScript bindings for all USwui components in the current level."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FSwuiEditorModule::OnRefreshAllBindings))
		);
	}

	void OnRefreshAllBindings()
	{
		if (!GEditor) return;
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World) return;

		int32 Count = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (USwui* Bridge = It->FindComponentByClass<USwui>())
			{
				FSwuiTSGenerator::Generate(Bridge);
				++Count;
			}
		}

		UE_LOG(LogTemp, Log, TEXT("SWUI: Refreshed JS bindings for %d USwui component(s)."), Count);
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSwuiEditorModule, SwuiEditor)
