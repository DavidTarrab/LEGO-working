#!/usr/bin/env node
// Inspect the current Git-visible tree; never print suspected secret values.
import fs from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const result = spawnSync('git',['ls-files','-c','-o','--exclude-standard','-z'],{cwd:root,encoding:'utf8',maxBuffer:32*1024*1024});
if (result.status !== 0) throw new Error('Cannot enumerate Git-visible files.');
const files = [...new Set(result.stdout.split('\0').filter(Boolean))];
const findings = [];
let checked = 0;
const escapedUser = (process.env.USERNAME || process.env.USER || '__unknown__').replace(/[.*+?^${}()|[\]\\]/g,'\\$&');
const localUserPath = new RegExp('[A-Z]:[\\\\/]+Users[\\\\/]+' + escapedUser + '(?:[\\\\/]|\\b)','i');
const textExtensions = /\.(md|txt|json|jsonl|yaml|yml|toml|ini|cfg|xml|config|ps1|bat|sh|py|mjs|js|ts|cpp|h|hpp|c|cs|vcxproj|sln|ipynb)$/i;
const forbidden = /(^|\/)(local|runs|__pycache__|node_modules|\.idea|\.vs)(\/|$)|\.(exe|dll|lib|pdb|obj|ilk|pyc|pfx|pem|gguf|utrace|etl)$/i;
for (const file of files) {
  const full = path.join(root,file);
  if (!fs.existsSync(full) || !fs.statSync(full).isFile()) continue;
  ++checked;
  if (forbidden.test(file)) findings.push({file,issue:'local/private/generated artifact'});
  if (/^\.env(?:\.|$)/.test(path.basename(file)) && path.basename(file) !== '.env.example') findings.push({file,issue:'private environment configuration'});
  if (fs.statSync(full).size > 50*1024*1024) findings.push({file,issue:'file exceeds 50 MiB; review distribution'});
  if (!textExtensions.test(file) || fs.statSync(full).size > 8*1024*1024) continue;
  // Also recognize JSON-escaped paths in metadata and notebooks.
  const text = fs.readFileSync(full,'utf8').replace(/\\\\/g,'\\');
  const rules = [
    ['personal Windows path',/[A-Z]:[\\/](?:dev[\\/]LEGO|Program Files[\\/]UE_4\.27)/i],
    ['current user profile path',localUserPath],
    ['original private cluster path',/\/state\/partition\//i],
    ['private key',/-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----/],
    ['credential token',/\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}|AKIA[A-Z0-9]{16})\b/]
  ];
  for (const [issue,pattern] of rules) if (pattern.test(text)) findings.push({file,issue});
}
for (const finding of findings) console.error(`${finding.file}: ${finding.issue}`);
console.log(`Checked ${checked} existing Git-visible files: ${findings.length} findings. Local ignored files and Git history are outside this check.`);
process.exitCode = findings.length ? 1 : 0;
