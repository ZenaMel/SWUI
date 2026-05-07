using UnrealBuildTool;

public class SwuiEditor : ModuleRules
{
	public SwuiEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"PropertyEditor",
			"Slate",
			"SlateCore",
			"LevelEditor",
			"ToolMenus",
			"SwuiRuntime",
			// Custom K2 nodes
			"BlueprintGraph",
			"KismetCompiler",
			"GraphEditor",
			"Kismet",
			// Class picker widget in Details panel
			"ClassViewer",
		});

		PrivateIncludePaths.Add("SwuiEditor/Private");
	}
}
