using UnrealBuildTool;

public class TaskDetector: ModuleRules
{
	public TaskDetector(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicDependencyModuleNames.AddRange(new string[] { "Core" });
		bEnableUndefinedIdentifierWarnings = false;
	}
}