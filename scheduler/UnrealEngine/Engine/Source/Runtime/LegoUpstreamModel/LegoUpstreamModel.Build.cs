using UnrealBuildTool;
using System;
using System.IO;

public class LegoUpstreamModel : ModuleRules
{
    public LegoUpstreamModel(ReadOnlyTargetRules Target) : base(Target)
    {
        if (Target.Platform != UnrealTargetPlatform.Win64) throw new BuildException("LEGO upstream integration requires Win64.");
        PublicDependencyModuleNames.Add("Core");
        PrivateDependencyModuleNames.AddRange(new[] { "CoreUObject", "Engine", "Json", "TaskScheduler", "TaskDetector", "PerformanceDetector" });
        string Backend = Environment.GetEnvironmentVariable("LEGO_UPSTREAM_BACKEND_DIR");
        if (String.IsNullOrWhiteSpace(Backend)) throw new BuildException("Load tools/Enter-LegoEnvironment.ps1 and build tools/Build-LegoInference.ps1 first.");
        Backend = Path.GetFullPath(Backend);
        foreach (string Required in new[] { "lego-inference.lib", "lego-inference.dll", "include/LegoInference.h", "build-info.json" })
            if (!File.Exists(Path.Combine(Backend, Required))) throw new BuildException("Missing upstream backend artifact: " + Required);
        PrivateIncludePaths.Add(Path.Combine(Backend, "include"));
        PublicAdditionalLibraries.Add(Path.Combine(Backend, "lego-inference.lib"));
        foreach (string Required in new[] { "lego-inference.dll", "llama.dll", "ggml.dll", "ggml-base.dll", "ggml-cpu.dll", "ggml-cuda.dll", "cudart64_12.dll", "cublas64_12.dll", "cublasLt64_12.dll" })
        {
            string FilePath = Path.Combine(Backend, Required);
            if (!File.Exists(FilePath)) throw new BuildException("Missing upstream runtime dependency: " + Required);
            RuntimeDependencies.Add("$(BinaryOutputDir)/" + Required, FilePath);
        }
        RuntimeDependencies.Add("$(BinaryOutputDir)/lego-backend-build-info.json", Path.Combine(Backend, "build-info.json"));
        RuntimeDependencies.Add("$(BinaryOutputDir)/llama-LICENSE.txt", Path.Combine(Backend, "llama-LICENSE.txt"));
    }
}
