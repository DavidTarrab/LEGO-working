using UnrealBuildTool;

public class DurationPredictor: ModuleRules
{
	public DurationPredictor(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.AddRange(new string[] { "Core" });


		bEnableUndefinedIdentifierWarnings = false;
	}
}
