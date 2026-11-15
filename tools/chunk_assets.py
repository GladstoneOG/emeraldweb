#!/usr/bin/env python3
"""Split large web/WASM asset files into <20 MiB chunks for Cloudflare Workers.

Cloudflare Workers Static Assets imposes a 25 MiB per-file limit on the Free plan.
This script splits any target asset(s) into <19 MiB `.partXX` files inside
`public/chunks/` and generates JSON manifests compatible with IndexedDB caching.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Dict, List


DEFAULT_CHUNK_SIZE_MB = 19.0
DEFAULT_THRESHOLD_MB = 20.0
DEFAULT_SCAN_EXTENSIONS = {".wasm", ".data", ".bin", ".pck"}


def sha256_file(path: Path) -> str:
    """Compute hex SHA-256 of a file using streaming reads."""
    h = hashlib.sha256()
    with path.open("rb") as fh:
        while True:
            block = fh.read(4 * 1024 * 1024)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def clean_old_parts(out_dir: Path, asset_name: str) -> None:
    """Remove stale `.partXX` chunks for a given asset filename."""
    prefix = f"{asset_name}.part"
    if not out_dir.exists():
        return
    for entry in out_dir.iterdir():
        if entry.is_file() and entry.name.startswith(prefix):
            entry.unlink()


def verify_chunks(chunk_paths: List[Path], expected_sha256: str, expected_bytes: int) -> None:
    """Re-read written chunks in order and verify byte length and SHA-256."""
    h = hashlib.sha256()
    total = 0
    for cp in chunk_paths:
        with cp.open("rb") as fh:
            while True:
                block = fh.read(4 * 1024 * 1024)
                if not block:
                    break
                h.update(block)
                total += len(block)
    if total != expected_bytes:
        raise RuntimeError(f"Chunk verification byte mismatch: {total} != {expected_bytes}")
    actual_sha = h.hexdigest()
    if actual_sha != expected_sha256:
        raise RuntimeError(f"Chunk verification SHA-256 mismatch: {actual_sha} != {expected_sha256}")


def chunk_single_file(
    src_path: Path,
    out_dir: Path,
    public_dir: Path,
    chunk_size_bytes: int,
    verify: bool = True,
    remove_source: bool = False,
) -> Dict[str, object]:
    """Split `src_path` into numbered `.partXX` chunks and write its manifest."""
    if not src_path.is_file():
        raise FileNotFoundError(f"Input asset not found: {src_path}")

    out_dir.mkdir(parents=True, exist_ok=True)
    asset_name = src_path.name
    total_bytes = src_path.stat().st_size
    full_sha256 = sha256_file(src_path)
    version = full_sha256[:12]

    clean_old_parts(out_dir, asset_name)

    written_paths: List[Path] = []
    chunk_rel_urls: List[str] = []

    with src_path.open("rb") as src_fh:
        idx = 0
        while True:
            remaining = chunk_size_bytes
            part_name = f"{asset_name}.part{idx:02d}"
            part_path = out_dir / part_name
            wrote_any = False

            with part_path.open("wb") as part_fh:
                while remaining > 0:
                    block = src_fh.read(min(4 * 1024 * 1024, remaining))
                    if not block:
                        break
                    part_fh.write(block)
                    wrote_any = True
                    remaining -= len(block)

            if not wrote_any:
                part_path.unlink(missing_ok=True)
                break

            written_paths.append(part_path)
            try:
                rel_url = part_path.relative_to(public_dir).as_posix()
            except ValueError:
                rel_url = f"chunks/{part_name}"
            chunk_rel_urls.append(rel_url)
            idx += 1

    if total_bytes == 0:
        # Preserve an empty 0-byte single chunk if needed
        part_name = f"{asset_name}.part00"
        part_path = out_dir / part_name
        part_path.write_bytes(b"")
        written_paths.append(part_path)
        chunk_rel_urls.append(f"chunks/{part_name}")

    if verify:
        verify_chunks(written_paths, full_sha256, total_bytes)

    manifest = {
        "file": asset_name,
        "version": version,
        "sha256": full_sha256,
        "totalBytes": total_bytes,
        "chunkSize": chunk_size_bytes,
        "chunks": chunk_rel_urls,
    }

    per_asset_manifest = out_dir / f"{asset_name}.manifest.json"
    with per_asset_manifest.open("w", encoding="utf-8") as mf:
        json.dump(manifest, mf, indent=2)
        mf.write("\n")

    if remove_source:
        src_path.unlink()

    return manifest


def find_oversized_assets(public_dir: Path, threshold_bytes: int) -> List[Path]:
    """Scan top-level `public/` directory for binary assets exceeding threshold."""
    if not public_dir.exists():
        return []
    targets: List[Path] = []
    for entry in sorted(public_dir.iterdir()):
        if not entry.is_file():
            continue
        if entry.suffix.lower() not in DEFAULT_SCAN_EXTENSIONS:
            continue
        if entry.stat().st_size >= threshold_bytes:
            targets.append(entry)
    return targets


def main(argv: List[str] | None = None) -> int:
    repo_root = Path(__file__).resolve().parent.parent
    default_public = repo_root / "public"
    default_out = default_public / "chunks"

    parser = argparse.ArgumentParser(
        description="Split large WASM/data assets into <20 MiB Cloudflare-safe chunks."
    )
    parser.add_argument(
        "--input",
        "-i",
        action="append",
        default=[],
        help="Specific asset file(s) to chunk. If omitted, scans public/ for files >= threshold.",
    )
    parser.add_argument(
        "--public-dir",
        default=str(default_public),
        help="Root public/ directory (default: <repo>/public).",
    )
    parser.add_argument(
        "--output-dir",
        "-o",
        default=str(default_out),
        help="Destination directory for chunks and manifests (default: public/chunks).",
    )
    parser.add_argument(
        "--chunk-size-mb",
        type=float,
        default=DEFAULT_CHUNK_SIZE_MB,
        help="Maximum chunk size in MiB (default: 19.0).",
    )
    parser.add_argument(
        "--threshold-mb",
        type=float,
        default=DEFAULT_THRESHOLD_MB,
        help="Auto-scan threshold in MiB when --input is omitted (default: 20.0).",
    )
    parser.add_argument(
        "--remove-source",
        action="store_true",
        help="Remove original unsplit file after SHA-256 chunk verification succeeds.",
    )
    parser.add_argument(
        "--no-verify",
        action="store_true",
        help="Skip post-write SHA-256 verification of generated chunks.",
    )

    args = parser.parse_args(argv)
    if args.chunk_size_mb <= 0:
        parser.error("--chunk-size-mb must be positive")

    public_dir = Path(args.public_dir).resolve()
    out_dir = Path(args.output_dir).resolve()
    chunk_size_bytes = int(args.chunk_size_mb * 1024 * 1024)
    threshold_bytes = int(args.threshold_mb * 1024 * 1024)

    if args.input:
        inputs = [Path(p).resolve() for p in args.input]
    else:
        inputs = find_oversized_assets(public_dir, threshold_bytes)
        if not inputs:
            print(
                f"No assets >= {args.threshold_mb:.1f} MiB found in {public_dir}; nothing to chunk."
            )
            return 0

    index_manifest_path = out_dir / "assets.manifest.json"
    combined: Dict[str, Dict[str, object]] = {}
    if index_manifest_path.is_file():
        try:
            existing = json.loads(index_manifest_path.read_text(encoding="utf-8"))
            if isinstance(existing, dict) and isinstance(existing.get("assets"), dict):
                combined = existing["assets"]
        except Exception:
            combined = {}

    for src in inputs:
        manifest = chunk_single_file(
            src_path=src,
            out_dir=out_dir,
            public_dir=public_dir,
            chunk_size_bytes=chunk_size_bytes,
            verify=not args.no_verify,
            remove_source=args.remove_source,
        )
        combined[manifest["file"]] = manifest
        size_mb = manifest["totalBytes"] / (1024 * 1024)
        print(
            f"Chunked {manifest['file']} ({size_mb:.2f} MiB) -> "
            f"{len(manifest['chunks'])} part(s), version={manifest['version']}"
        )

    out_dir.mkdir(parents=True, exist_ok=True)
    index_manifest_path.write_text(
        json.dumps({"assets": combined}, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"Wrote combined manifest: {index_manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
