"""Idempotent config migrator for python/j2k_config.json.

Each version bump describes what changed in `MIGRATIONS` below. Run with:
    python python/utils/config_migrate.py [--config <path>] [--dry-run]

Behavior:
- Reads the canonical config (defaults to <repo>/python/j2k_config.json).
- Checks the `config_schema_version` field (treats missing as 0).
- Applies migrations in order until the schema reaches CURRENT_SCHEMA_VERSION.
- Writes the result back atomically (.tmp → rename) unless --dry-run.

Safe to run on any version; running on an up-to-date config is a no-op.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any, Callable, Dict


CURRENT_SCHEMA_VERSION = 1


def _migration_v0_to_v1(cfg: Dict[str, Any]) -> Dict[str, Any]:
    """Initial schema. Adds `config_schema_version` and strips known PII fields
    that older configs may have contained (`ps5_ip`, `psn_user`, `cont_guid`).
    Personal calibration values are preserved."""
    cfg["config_schema_version"] = 1
    for pii_field in ("ps5_ip", "psn_user", "cont_guid"):
        if pii_field in cfg and isinstance(cfg[pii_field], str) and cfg[pii_field]:
            cfg[pii_field] = ""
    return cfg


MIGRATIONS: Dict[int, Callable[[Dict[str, Any]], Dict[str, Any]]] = {
    0: _migration_v0_to_v1,
}


def _resolve_default_config() -> Path:
    return Path(__file__).resolve().parent.parent / "j2k_config.json"


def migrate(cfg: Dict[str, Any]) -> tuple[Dict[str, Any], int]:
    """Return (migrated_cfg, applied_count)."""
    applied = 0
    while True:
        ver = int(cfg.get("config_schema_version", 0) or 0)
        if ver >= CURRENT_SCHEMA_VERSION:
            return cfg, applied
        if ver not in MIGRATIONS:
            raise RuntimeError(
                f"No migration registered from schema v{ver} → v{ver + 1}. "
                f"Update MIGRATIONS in {__file__}."
            )
        cfg = MIGRATIONS[ver](cfg)
        new_ver = int(cfg.get("config_schema_version", 0) or 0)
        if new_ver <= ver:
            raise RuntimeError(
                f"Migration v{ver}→v{ver + 1} did not bump config_schema_version; aborting."
            )
        applied += 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--config", type=Path, default=_resolve_default_config())
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if not args.config.is_file():
        print(f"[config_migrate] not found: {args.config}", file=sys.stderr)
        return 1

    raw = args.config.read_text(encoding="utf-8")
    cfg = json.loads(raw)
    if not isinstance(cfg, dict):
        print(f"[config_migrate] root is not a JSON object", file=sys.stderr)
        return 2

    cfg, applied = migrate(cfg)
    if applied == 0:
        print(f"[config_migrate] {args.config} already at schema v{CURRENT_SCHEMA_VERSION}")
        return 0

    if args.dry_run:
        print(f"[config_migrate] would apply {applied} migration(s) → v{CURRENT_SCHEMA_VERSION} (dry-run)")
        return 0

    tmp = args.config.with_suffix(args.config.suffix + ".tmp")
    tmp.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, args.config)
    print(f"[config_migrate] applied {applied} migration(s) → v{CURRENT_SCHEMA_VERSION}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
