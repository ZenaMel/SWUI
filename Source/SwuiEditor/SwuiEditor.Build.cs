using System.IO;
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
			"AssetRegistry",
			"PropertyEditor",
			"Slate",
			"SlateCore",
			"LevelEditor",
			"ToolMenus",
			"ApplicationCore",
			"SwuiRuntime",
			"SwuiUncookedOnly",
			// Custom K2 nodes
			"BlueprintGraph",
			"KismetCompiler",
			"GraphEditor",
			"Kismet",
			// Class picker widget in Details panel
			"ClassViewer",
			"GameplayTags",
			"GameplayTagsEditor",
		});

		// ── Conditional AngelScript integration ──────────────────────────────
		// Detect whether the Angelscript plugin is present in the engine or
		// project at build time.  If found, compile AS hook support and define
		// SWUI_WITH_ANGELSCRIPT=1 so the C++ code can conditionally include the
		// AS headers and reference AS module types.
		//
		// Non-AS engine forks compile cleanly with SWUI_WITH_ANGELSCRIPT=0.
		// The runtime heuristic (SwuiProjectLooksLikeAngelScriptProject) still
		// gates hook *activation* — even AS-enabled builds only bind hooks when
		// the current project actually has Script/*.as files.

		string ProjectDir = Target.ProjectFile != null
			? Target.ProjectFile.Directory.FullName
			: null;

		bool bHasAngelscriptCode =
			Directory.Exists(Path.Combine(EngineDirectory, "Plugins", "Angelscript")) ||
			Directory.Exists(Path.Combine(EngineDirectory, "Plugins", "UnrealEngine-Angelscript"));

		if (!string.IsNullOrEmpty(ProjectDir))
		{
			bHasAngelscriptCode =
				bHasAngelscriptCode ||
				Directory.Exists(Path.Combine(ProjectDir, "Plugins", "Angelscript")) ||
				Directory.Exists(Path.Combine(ProjectDir, "Plugins", "UnrealEngine-Angelscript"));
		}

		if (bHasAngelscriptCode)
		{
			PrivateDependencyModuleNames.Add("AngelscriptCode");
			PublicDefinitions.Add("SWUI_WITH_ANGELSCRIPT=1");
		}
		else
		{
			PublicDefinitions.Add("SWUI_WITH_ANGELSCRIPT=0");
		}

		PrivateIncludePaths.Add("SwuiEditor/Private");
	}
}
