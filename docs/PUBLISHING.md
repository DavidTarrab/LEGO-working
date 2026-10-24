# Publication scope

The public tree contains source code, documentation, original research data and
a deliberately selected portable validation summary. It excludes the licensed
engine checkout/template assets, model weights, toolchains, compiled binaries,
raw local runs, IDE state, credentials and machine-specific setup notes.

Before committing a release snapshot:

```powershell
node --test .\tools\Analyze-ConcurrentRun.test.mjs `
  .\tools\Select-LegoMeasurement.test.mjs .\tools\Prepare-LegoIntegration.test.mjs
node .\tools\Check-Publish.mjs
git diff --check
git status --short
```

Review newly added files and staged changes as well. The automated audit is a
limited heuristic, not proof that no private data exists. It checks the current
Git-visible tree; it does not inspect ignored data or sanitize commit history.
Never force-add `local/`, `runs/`, engine assets or model weights. Raw reports
record absolute paths and launch metadata; export only selected sanitized fields
when sharing results.

The inherited repository history can still contain obsolete engine copies,
binaries and original machine-specific examples removed from the current tree.
This cleanup does not rewrite that history, change remotes, create a public
repository or push anything. For a distribution that must exclude historical
files, use a fresh source snapshot after reviewing/committing the cleaned tree,
or plan an explicit history migration separately. A cleaned working tree alone
does not remove old blobs from Git.

The original adaptor training and gameplay evaluation frameworks are retained
for research provenance, with their upstream notices. Their experiments were
not validated by the Windows scheduling runs and should not be represented as
successful reproductions. Their model/dataset settings need explicit inputs.

The portable result record omits usernames, paths, process IDs and timestamps.
It preserves the hardware/software configuration, workload description and
measured statistics. The original model checksum was not recorded, and the
short run has no confidence intervals; both limitations must travel with results.

Consult [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). The project license
does not grant rights to redistribute Unreal, CUDA, model weights, ROMs or the
publisher's PDF.
