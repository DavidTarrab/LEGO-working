"""Download the immutable reviewed snapshot; never execute model code here."""
import argparse
import hashlib
import json
from pathlib import Path

from runtime import MODEL_ID, MODEL_REVISION


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    if args.manifest.exists():
        raise FileExistsError("Choose a new download manifest")
    from huggingface_hub import HfApi, snapshot_download
    upstream = HfApi().model_info(MODEL_ID, revision=MODEL_REVISION, files_metadata=True)
    if upstream.sha != MODEL_REVISION:
        raise ValueError("Upstream revision changed")
    expected = {entry.rfilename: entry.lfs.sha256 for entry in upstream.siblings if entry.lfs}
    snapshot = Path(snapshot_download(MODEL_ID, revision=MODEL_REVISION,
                                     cache_dir=args.cache_dir, max_workers=2))
    records = {}
    for path in sorted(snapshot.iterdir()):
        if path.is_file():
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
                    digest.update(block)
            records[path.name] = {"bytes": path.stat().st_size, "sha256": digest.hexdigest()}
            if path.name in expected and digest.hexdigest() != expected[path.name]:
                raise ValueError(f"Upstream LFS checksum mismatch: {path.name}")
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps({"model_id": MODEL_ID, "revision": MODEL_REVISION,
                                         "files": records, "upstream_lfs_hashes_verified": True}, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"success": True, "revision": MODEL_REVISION,
                      "files": len(records)}), flush=True)


if __name__ == "__main__":
    main()
