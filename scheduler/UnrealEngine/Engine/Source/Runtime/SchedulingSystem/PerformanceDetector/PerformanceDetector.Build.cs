using UnrealBuildTool;

public class PerformanceDetector: ModuleRules
{
	public PerformanceDetector(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "TaskDetector"});


		bEnableUndefinedIdentifierWarnings = false;
	}
}
