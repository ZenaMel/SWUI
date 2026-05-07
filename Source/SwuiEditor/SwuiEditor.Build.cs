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
		});

		PrivateIncludePaths.Add("SwuiEditor/Private");
	}
}
