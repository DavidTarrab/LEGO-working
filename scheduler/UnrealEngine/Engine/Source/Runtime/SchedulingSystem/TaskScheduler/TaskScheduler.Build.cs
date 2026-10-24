using UnrealBuildTool;

public class TaskScheduler: ModuleRules
{
	public TaskScheduler(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "TaskDetector", "PerformanceDetector", "DurationPredictor" });

		bEnableUndefinedIdentifierWarnings = false;
	}
}