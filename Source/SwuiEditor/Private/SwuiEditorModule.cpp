#include "Modules/ModuleManager.h"
#include "Modules/ModuleInterface.h"
#include "PropertyEditorModule.h"
#include "LevelEditor.h"
#include "ToolMenus.h"
#include "EngineUtils.h"

#include "Swui.h"
#include "SwuiDetails.h"
#include "SwuiTSGenerator.h"

#define LOCTEXT_NAMESPACE "SwuiEditor"

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

		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FSwuiEditorModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);

		if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
		{
			FPropertyEditorModule& PropertyModule =
				FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
			PropertyModule.UnregisterCustomClassLayout(USwui::StaticClass()->GetFName());
		}
	}

private:
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
				"Re-generates TypeScript bindings for all SwuiBridge components in the current level."),
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

		UE_LOG(LogTemp, Log, TEXT("SWUI: Refreshed JS bindings for %d SwuiBridge component(s)."), Count);
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSwuiEditorModule, SwuiEditor)
