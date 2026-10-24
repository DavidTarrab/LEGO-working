// Fetch only Epic's corrected 4.27.2 dependency manifest. Git credentials stay
// in memory; never write/print tokens or dump credential-helper output.
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const engine = path.resolve(process.argv[2] || 'local/UE427Source');
const git = spawnSync('git', ['credential', 'fill'], {
  input: 'protocol=https\nhost=github.com\n\n', encoding: 'utf8',
  env: { ...process.env, GIT_TERMINAL_PROMPT: '0', GCM_INTERACTIVE: 'never' }
});
if (git.status !== 0) throw new Error('GitHub sign-in required; run git ls-remote for EpicGames/UnrealEngine first.');
const entries = Object.fromEntries(git.stdout.trim().split(/\r?\n/).map(line => {
  const index = line.indexOf('='); return [line.slice(0, index), line.slice(index + 1)];
}));
if (!entries.password) throw new Error('No GitHub credential available.');
const headers = { Authorization: `Bearer ${entries.password}`, 'User-Agent': 'LEGO-local-setup' };
const metadataResponse = await fetch('https://api.github.com/repos/EpicGames/UnrealEngine/releases/tags/4.27.2-release', { headers });
if (!metadataResponse.ok) throw new Error(`Epic release metadata unavailable: HTTP ${metadataResponse.status}`);
const release = await metadataResponse.json();
const asset = release.assets.find(item => item.name === 'Commit.gitdeps.xml');
if (!asset || !asset.url.startsWith('https://api.github.com/repos/EpicGames/UnrealEngine/releases/assets/'))
  throw new Error('Corrected manifest asset not found in the official Epic release.');
const response = await fetch(asset.url, { headers: { ...headers, Accept: 'application/octet-stream' } });
if (!response.ok) throw new Error(`Epic manifest download failed: HTTP ${response.status}`);
const original = await response.text();
if (!original.includes('<DependencyManifest') || !original.includes('<Files>'))
  throw new Error('Downloaded asset is not a dependency manifest.');
const target = path.join(engine, 'Engine/Build/Commit.gitdeps.xml');
fs.writeFileSync(target, original.replace('BaseUrl="http://cdn.unrealengine.com/', 'BaseUrl="https://cdn.unrealengine.com/'));
const notes = path.join(path.dirname(engine), 'integration-notes');
fs.mkdirSync(notes, { recursive: true });
fs.writeFileSync(path.join(notes, 'dependency-manifest-source.json'), JSON.stringify({
  release: release.html_url, asset: asset.name, asset_id: asset.id,
  downloaded_utc: new Date().toISOString(), transport_change: 'HTTP base URL upgraded to HTTPS'
}, null, 2));
console.log(`Installed corrected Epic manifest: ${target}`);
