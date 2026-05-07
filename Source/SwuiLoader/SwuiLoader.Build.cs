using UnrealBuildTool;
using System.IO;
using System;

public class SwuiLoader : ModuleRules
{
	public SwuiLoader(ReadOnlyTargetRules Target) : base(Target)
    {
        PublicDependencyModuleNames.AddRange(
			new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Projects"
		});
	}
}
