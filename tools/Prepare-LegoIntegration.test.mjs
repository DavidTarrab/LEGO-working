import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import { hookEdits, prepareProject, syncProjectSource } from './Prepare-LegoIntegration.mjs';

test('stage CLI accepts --apply without mistaking it for an engine path', t => {
  const root = fixture(t);
  const script = fileURLToPath(new URL('./Prepare-LegoIntegration.mjs', import.meta.url));
  const result = spawnSync(process.execPath,[script,'stage','--apply'],{cwd:root,env:{...process.env,LEGO_ENGINE_ROOT:root},encoding:'utf8'});
  // The fixture is not an authorized pinned engine checkout. Rejection should
  // name the environment-selected engine, rather than a directory named --apply.
  assert.equal(result.status,1);
  assert.ok(result.stderr.includes(root),result.stderr);
  assert.match(result.stderr,/reviewed for Epic/);
  assert.ok(!fs.readFileSync(path.join(root,'Engine/Source/Runtime/RHI/RHI.Build.cs'),'utf8').includes('LEGO_HOOK'));
});

function fixture(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'lego-port-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const files = {
    'RHI/RHI.Build.cs': 'PrivateDependencyModuleNames.Add("Core");',
    'RHI/Private/RHICommandList.cpp': '#include "CoreMinimal.h"\nvoid FRHICommandListExecutor::ExecuteInner_DoExecute(FRHICommandListBase& CmdList)\n{\n}',
    'D3D12RHI/D3D12RHI.Build.cs': '"RenderCore",',
    'D3D12RHI/Private/D3D12CommandContext.cpp': '#include "D3D12RHIPrivate.h"\nvoid FD3D12CommandContextBase::RHIBeginFrame()\n{\n}\nvoid FD3D12CommandContextBase::RHIEndFrame()\n{\n}',
    'D3D12RHI/Private/D3D12DirectCommandListManager.cpp': '#include "D3D12RHIPrivate.h"\nvoid FD3D12CommandListManager::Destroy()\n{\n\t// Wait for the queue to empty\n\tWaitForCommandQueueFlush();\n}\nvoid FD3D12CommandListManager::ExecuteCommandLists(TArray<FD3D12CommandListHandle>& Lists, bool WaitForCompletion)\n{\n}\nvoid Done()\n{\n\tif (WaitForCompletion)\n\t{\n\t\tcheck(SyncPoint.IsComplete());\n\t}\n}',
    'D3D12RHI/Private/D3D12DirectCommandListManager.h': '#include "CoreMinimal.h"\nTRefCountPtr<FD3D12CommandListFence>\tCommandListFence;'
  };
  for (const [name, text] of Object.entries(files)) {
    const file = path.join(root, 'Engine/Source/Runtime', name);
    fs.mkdirSync(path.dirname(file), { recursive: true }); fs.writeFileSync(file, text.replaceAll('\n', '\r\n'));
  }
  return root;
}
test('hook planning is nonmutating, captures fence ownership, and is idempotent after application', t => {
  const root = fixture(t), edits = hookEdits(root);
  assert.equal(edits.length, 6);
  for (const edit of edits) {
    assert.equal(fs.readFileSync(edit.file, 'utf8'), edit.original);
    assert.ok(edit.updated.includes('LEGO_HOOK'));
    fs.writeFileSync(edit.file, edit.updated);
  }
  const gpu = edits.find(e => e.file.endsWith('D3D12DirectCommandListManager.cpp')).updated;
  assert.ok(gpu.includes('TRefCountPtr<FD3D12CommandListFence> Fence'));
  assert.ok(!gpu.includes('&CommandListFence'));
  assert.ok(gpu.includes('while (LegoCompletionTasks.GetValue() > 0)'));
  for (const edit of hookEdits(root)) assert.equal(edit.original, edit.updated);
});
test('hook planning refuses ambiguous engine anchors', t => {
  const root = fixture(t), file = path.join(root, 'Engine/Source/Runtime/RHI/RHI.Build.cs');
  fs.appendFileSync(file, 'PrivateDependencyModuleNames.Add("Core");');
  assert.throws(() => hookEdits(root), /refusing an unreviewed merge/);
});
test('project preparation preserves source baseline and refuses existing destinations', t => {
  const root = fixture(t), baseline = path.join(root, 'baseline'), destination = path.join(root, 'DialogueDemo');
  fs.mkdirSync(path.join(baseline, 'Content'), { recursive: true });
  fs.writeFileSync(path.join(baseline, 'Content/map.uasset'), 'fixture map');
  const model = path.join(root, 'model.gguf'); fs.writeFileSync(model, 'fixture');
  const project = prepareProject(destination, baseline, model);
  assert.equal(JSON.parse(fs.readFileSync(project)).Modules[0].Name, 'DialogueDemo');
  assert.equal(fs.readFileSync(path.join(destination, 'Content/map.uasset'), 'utf8'), 'fixture map');
  assert.equal(fs.readFileSync(path.join(baseline, 'Content/map.uasset'), 'utf8'), 'fixture map');
  assert.ok(fs.existsSync(path.join(destination, 'Source/DialogueDemoEditor.Target.cs')));
  assert.ok(fs.readFileSync(path.join(destination, 'Config/DefaultGame.ini'), 'utf8').includes('OperationPolicy=2'));
  assert.throws(() => prepareProject(destination, baseline, model), /refusing overwrite/);
});
test('project source synchronization preserves user settings and refuses independent source edits', t => {
  const root = fixture(t), baseline = path.join(root, 'baseline'), destination = path.join(root, 'project');
  fs.mkdirSync(path.join(baseline, 'Content'), {recursive: true});
  const model = path.join(root, 'model.gguf'); fs.writeFileSync(model, 'fixture');
  const projectFile = prepareProject(destination, baseline, model);
  const project = JSON.parse(fs.readFileSync(projectFile)); project.EngineAssociation = 'preserve-this';
  fs.writeFileSync(projectFile, JSON.stringify(project));
  syncProjectSource(destination, true);
  assert.equal(JSON.parse(fs.readFileSync(projectFile)).EngineAssociation, 'preserve-this');
  const source = path.join(destination, 'Source/DialogueDemo/Private/LlamaDialogueWidget.cpp');
  fs.appendFileSync(source, '\n// independent user edit');
  assert.throws(() => syncProjectSource(destination, true), /independent changes/);
  assert.ok(fs.readFileSync(source, 'utf8').endsWith('// independent user edit'));
});
