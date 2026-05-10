using UnrealBuildTool;

public class SwuiUncookedOnly : ModuleRules
{
	public SwuiUncookedOnly(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"BlueprintGraph",
			"KismetCompiler",
			"SwuiRuntime",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"Kismet",
			"GameplayTags",
			"GraphEditor",
			"Slate",
			"SlateCore",
		});

	}
}
