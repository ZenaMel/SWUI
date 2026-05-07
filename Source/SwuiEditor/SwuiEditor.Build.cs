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
			"SwuiRuntime"
		});

		PrivateIncludePaths.Add("SwuiEditor/Private");
	}
}
