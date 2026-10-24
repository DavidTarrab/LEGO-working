import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const hash = data => crypto.createHash('sha256').update(data).digest('hex');
function writeChanged(file, data) {
  if (!fs.existsSync(file) || hash(fs.readFileSync(file)) !== hash(data)) fs.writeFileSync(file, data);
}
function walk(root, rel = '') {
  return fs.readdirSync(path.join(root, rel), { withFileTypes: true }).flatMap(e => {
    if (e.isSymbolicLink()) throw new Error(`Symlink not supported: ${e.name}`);
    const name = path.join(rel, e.name);
    return e.isDirectory() ? walk(root, name) : [name];
  });
}
function replaceOnce(text, before, after, file) {
  if (text.split(before).length !== 2) throw new Error(`Expected one hook anchor in ${file}; refusing an unreviewed merge.`);
  return text.replace(before, after);
}

export function hookEdits(engine) {
  const runtime = path.join(engine, 'Engine/Source/Runtime');
  const transformations = {
    'RHI/RHI.Build.cs': text => replaceOnce(text, 'PrivateDependencyModuleNames.Add("Core");',
      'PrivateDependencyModuleNames.Add("Core");\n        PrivateDependencyModuleNames.Add("TaskDetector"); // LEGO_HOOK', 'RHI.Build.cs'),
    'RHI/Private/RHICommandList.cpp': text => {
      text = replaceOnce(text, '#include "CoreMinimal.h"', '#include "CoreMinimal.h"\n#include "TaskDetector.h" // LEGO_HOOK', 'RHICommandList.cpp');
      return replaceOnce(text, 'void FRHICommandListExecutor::ExecuteInner_DoExecute(FRHICommandListBase& CmdList)\n{',
        'void FRHICommandListExecutor::ExecuteInner_DoExecute(FRHICommandListBase& CmdList)\n{\n    FLegoRHIActivityScope LegoRHIActivity;', 'RHICommandList.cpp');
    },
    'D3D12RHI/D3D12RHI.Build.cs': text => replaceOnce(text, '"RenderCore",',
      '"RenderCore",\n                "TaskDetector", // LEGO_HOOK\n                "PerformanceDetector",', 'D3D12RHI.Build.cs'),
    'D3D12RHI/Private/D3D12CommandContext.cpp': text => {
      text = replaceOnce(text, '#include "D3D12RHIPrivate.h"', '#include "D3D12RHIPrivate.h"\n#include "TaskDetector.h" // LEGO_HOOK\n#include "PerformanceDetector.h"', 'D3D12CommandContext.cpp');
      text = replaceOnce(text, 'void FD3D12CommandContextBase::RHIBeginFrame()\n{',
        'void FD3D12CommandContextBase::RHIBeginFrame()\n{\n    FPerformanceDetector::GetInstance().SetFrameBeginTime(0);\n    FTaskDetector::GetInstance().SetIsInFrameRendering(true);', 'D3D12CommandContext.cpp');
      return replaceOnce(text, 'void FD3D12CommandContextBase::RHIEndFrame()\n{',
        `void FD3D12CommandContextBase::RHIEndFrame()\n{\n    // CPU frame submission closes here; pending GPU fences remain busy.\n    struct FLegoFrameEndScope { ~FLegoFrameEndScope() { FTaskDetector::GetInstance().SetIsInFrameRendering(false); } } LegoFrameEnd;`, 'D3D12CommandContext.cpp');
    },
    'D3D12RHI/Private/D3D12DirectCommandListManager.cpp': text => {
      text = replaceOnce(text, '#include "D3D12RHIPrivate.h"',
        `#include "D3D12RHIPrivate.h"\n#include "TaskDetector.h" // LEGO_HOOK\n#include "Async/AsyncWork.h"\n\n// The manager drains these tasks before destroying the fence's internal cores.\nclass FLegoFenceCompletion : public FNonAbandonableTask\n{\n    friend class FAutoDeleteAsyncTask<FLegoFenceCompletion>;\n    TRefCountPtr<FD3D12CommandListFence> Fence;\n    uint64 Value;\n    FThreadSafeCounter* Pending;\npublic:\n    FLegoFenceCompletion(TRefCountPtr<FD3D12CommandListFence> InFence, uint64 InValue, FThreadSafeCounter* InPending) : Fence(InFence), Value(InValue), Pending(InPending) {}\n    void DoWork() { Fence->WaitForFence(Value); FTaskDetector::GetInstance().EndGPUWork(); Pending->Decrement(); }\n    FORCEINLINE TStatId GetStatId() const { RETURN_QUICK_DECLARE_CYCLE_STAT(FLegoFenceCompletion, STATGROUP_ThreadPoolAsyncTasks); }\n};`, 'D3D12DirectCommandListManager.cpp');
      text = replaceOnce(text, 'void FD3D12CommandListManager::Destroy()\n{\n\t// Wait for the queue to empty\n\tWaitForCommandQueueFlush();',
        'void FD3D12CommandListManager::Destroy()\n{\n\t// Wait for the queue to empty\n\tWaitForCommandQueueFlush();\n    while (LegoCompletionTasks.GetValue() > 0) { FPlatformProcess::SleepNoStats(.001f); }', 'D3D12DirectCommandListManager.cpp');
      text = replaceOnce(text, 'void FD3D12CommandListManager::ExecuteCommandLists(TArray<FD3D12CommandListHandle>& Lists, bool WaitForCompletion)\n{',
        'void FD3D12CommandListManager::ExecuteCommandLists(TArray<FD3D12CommandListHandle>& Lists, bool WaitForCompletion)\n{\n    FTaskDetector::GetInstance().BeginGPUWork(); // Includes queued CPU submission delay conservatively.', 'D3D12DirectCommandListManager.cpp');
      return replaceOnce(text, '\t\tcheck(SyncPoint.IsComplete());\n\t}\n}',
        '\t\tcheck(SyncPoint.IsComplete());\n\t}\n    LegoCompletionTasks.Increment();\n    (new FAutoDeleteAsyncTask<FLegoFenceCompletion>(CommandListFence, SignaledFenceValue, &LegoCompletionTasks))->StartBackgroundTask();\n}', 'D3D12DirectCommandListManager.cpp');
    },
    'D3D12RHI/Private/D3D12DirectCommandListManager.h': text => {
      text = replaceOnce(text, '#include "CoreMinimal.h"', '#include "CoreMinimal.h"\n#include "HAL/ThreadSafeCounter.h"', 'D3D12DirectCommandListManager.h');
      return replaceOnce(text, 'TRefCountPtr<FD3D12CommandListFence>\tCommandListFence;',
        'TRefCountPtr<FD3D12CommandListFence>\tCommandListFence;\n    FThreadSafeCounter LegoCompletionTasks; // LEGO_HOOK', 'D3D12DirectCommandListManager.h');
    }
  };
  return Object.entries(transformations).map(([relative, edit]) => {
    const file = path.join(runtime, relative), original = fs.readFileSync(file, 'utf8');
    // Idempotence: do not overwrite subsequent hand edits to a hooked engine.
    const normalized = original.replaceAll('\r\n', '\n');
    const updated = normalized.includes('LEGO_HOOK') ? original : edit(normalized);
    return { file, original, updated };
  });
}

export function stageModules(engine, apply = false) {
  const result = spawnSync('git', ['-C', engine, 'rev-parse', 'HEAD'], { encoding: 'utf8' });
  if (result.status !== 0 || result.stdout.trim() !== 'd94b38ae3446da52224bedd2568c078f828b4039')
    throw new Error(`Engine hooks are reviewed for Epic 4.27.2 commit d94b38ae only: ${engine}`);
  const source = path.join(repo, 'scheduler/UnrealEngine/Engine/Source/Runtime');
  const dest = path.join(engine, 'Engine/Source/Runtime');
  const manifestFile = path.join(engine, '.lego-staged-files.json');
  const previous = fs.existsSync(manifestFile) ? JSON.parse(fs.readFileSync(manifestFile, 'utf8')) : {};
  const planned = [];
  for (const module of ['LegoUpstreamModel', 'SchedulingSystem']) for (const rel of walk(path.join(source, module))) {
    // Source assets only; do not stage trained predictor experiments or bundled
    // unverified import libraries as if they were a recovered CUDA build.
    if (rel.split(path.sep).some(p => ['Libs', 'Resources'].includes(p))) continue;
    const key = path.join(module, rel), target = path.join(dest, key), data = fs.readFileSync(path.join(source, key));
    if (fs.existsSync(target) && hash(fs.readFileSync(target)) !== previous[key] && hash(fs.readFileSync(target)) !== hash(data))
      throw new Error(`Destination has independent changes: ${target}`);
    planned.push({ key, target, data });
  }
  // Validate all hooks and owned destinations before any mutation.
  const edits = hookEdits(engine);
  if (apply) {
    const manifest = { ...previous };
    for (const item of planned) {
      fs.mkdirSync(path.dirname(item.target), { recursive: true }); writeChanged(item.target, item.data);
      manifest[item.key] = hash(item.data);
    }
    for (const edit of edits) if (edit.updated !== edit.original) fs.writeFileSync(edit.file, edit.updated);
    fs.writeFileSync(manifestFile, JSON.stringify(manifest, null, 2));
  }
  return { moduleFiles: planned.length, hookFiles: edits.filter(e => e.updated !== e.original).map(e => path.relative(engine, e.file)), applied: apply };
}

export function prepareProject(destination, baseline, model) {
  if (fs.existsSync(destination)) throw new Error(`Project destination already exists; refusing overwrite: ${destination}`);
  if (!fs.existsSync(path.join(baseline, 'Content'))) throw new Error('Baseline Content directory is required.');
  if (!fs.existsSync(model)) throw new Error('Model file does not exist.');
  const iniPath = p => p.replaceAll('\\', '/');
  fs.mkdirSync(destination, { recursive: true });
  fs.cpSync(path.join(baseline, 'Content'), path.join(destination, 'Content'), { recursive: true });
  fs.cpSync(path.join(repo, 'scheduler/DialogueDemo/Source'), path.join(destination, 'Source'), { recursive: true });
  fs.mkdirSync(path.join(destination, 'Config'));
  fs.writeFileSync(path.join(destination, 'DialogueDemo.uproject'), JSON.stringify({ FileVersion: 3, EngineAssociation: '', DisableEnginePluginsByDefault: true,
    Modules: [{ Name: 'DialogueDemo', Type: 'Runtime', LoadingPhase: 'Default' },
      ...['TaskDetector', 'PerformanceDetector', 'DurationPredictor', 'TaskScheduler', 'LegoUpstreamModel']
        .map(Name => ({ Name, Type: 'Runtime', LoadingPhase: 'PreDefault' }))] }, null, 2));
  for (const [target, type] of [['DialogueDemo', 'Game'], ['DialogueDemoEditor', 'Editor']])
    fs.writeFileSync(path.join(destination, 'Source', `${target}.Target.cs`),
      `using UnrealBuildTool;\npublic class ${target}Target : TargetRules { public ${target}Target(TargetInfo Target) : base(Target) { Type = TargetType.${type}; DefaultBuildSettings = BuildSettingsVersion.V2; ExtraModuleNames.Add("DialogueDemo"); } }\n`);
  fs.writeFileSync(path.join(destination, 'Config', 'DefaultEngine.ini'),
    `[/Script/EngineSettings.GameMapsSettings]\nEditorStartupMap=/Game/ThirdPersonBP/Maps/ThirdPersonExampleMap\nGameDefaultMap=/Game/ThirdPersonBP/Maps/ThirdPersonExampleMap\nGlobalDefaultGameMode=/Script/DialogueDemo.LegoExperimentGameMode\n\n[/Script/WindowsTargetPlatform.WindowsTargetSettings]\nDefaultGraphicsRHI=DefaultGraphicsRHI_DX12\n`);
  fs.writeFileSync(path.join(destination, 'Config', 'DefaultGame.ini'),
    `[LEGO]\nModelPath=${iniPath(path.resolve(model))}\nSeconds=120\nAPM=200\nFPS=60\nTokens=16\nContext=2048\nOperationPolicy=2\nAdmissionTimeoutSeconds=2\n`);
  return path.join(destination, 'DialogueDemo.uproject');
}

export function syncProjectSource(destination, localModules = false) {
  const descriptor = path.join(destination, 'DialogueDemo.uproject');
  const project = JSON.parse(fs.readFileSync(descriptor, 'utf8'));
  if (!Array.isArray(project.Modules)) throw new Error('Project module descriptor is invalid.');
  const manifestPath = path.join(destination, '.lego-source-files.json');
  const previous = fs.existsSync(manifestPath) ? JSON.parse(fs.readFileSync(manifestPath, 'utf8')) : {};
  const runtime = path.join(repo, 'scheduler/UnrealEngine/Engine/Source/Runtime');
  const sources = [['DialogueDemo', path.join(repo, 'scheduler/DialogueDemo/Source/DialogueDemo')]];
  if (localModules) {
    sources.push(['LegoUpstreamModel', path.join(runtime, 'LegoUpstreamModel')]);
    for (const name of ['TaskDetector', 'PerformanceDetector', 'DurationPredictor', 'TaskScheduler'])
      sources.push([name, path.join(runtime, 'SchedulingSystem', name)]);
  }
  const planned = [];
  for (const [name, root] of sources) for (const rel of walk(root)) {
    if (rel.split(path.sep).includes('Resources')) continue;
    const key = path.join('Source', name, rel), target = path.join(destination, key), data = fs.readFileSync(path.join(root, rel));
    if (fs.existsSync(target) && hash(fs.readFileSync(target)) !== previous[key] && hash(fs.readFileSync(target)) !== hash(data))
      throw new Error(`Project file has independent changes: ${target}`);
    planned.push({key, target, data});
  }
  for (const {key, target, data} of planned) {
    fs.mkdirSync(path.dirname(target), {recursive: true}); writeChanged(target, data); previous[key] = hash(data);
  }
  for (const Name of ['TaskDetector', 'PerformanceDetector', 'DurationPredictor', 'TaskScheduler', 'LegoUpstreamModel'])
    if (!project.Modules.some(m => m.Name === Name)) project.Modules.push({Name, Type: 'Runtime', LoadingPhase: 'PreDefault'});
  writeChanged(descriptor, JSON.stringify(project, null, 2));
  fs.writeFileSync(manifestPath, JSON.stringify(previous, null, 2));
  return {files: planned.length, localModules};
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const args = process.argv.slice(2), action = args.shift();
    if (action === 'stage') console.log(JSON.stringify(stageModules(path.resolve(args.find(arg => !arg.startsWith('--')) || process.env.LEGO_ENGINE_ROOT || 'local/UE427Source'), args.includes('--apply')), null, 2));
    else if (action === 'project') console.log(prepareProject(path.resolve(args[0] || 'local/DialogueUpstream'), path.resolve(args[1] || 'local/LegoBaseline'), path.resolve(args[2] || process.env.LEGO_MODEL_PATH || 'local/models/model.gguf')));
    else if (action === 'sync-project') console.log(syncProjectSource(path.resolve(args[0] || 'local/DialogueUpstream'), args.includes('--local-modules')));
    else throw new Error('Usage: node tools/Prepare-LegoIntegration.mjs stage [engine] [--apply] | project [destination] [baseline] [model] | sync-project [destination] [--local-modules]');
  } catch (e) { console.error(e.message); process.exitCode = 1; }
}
