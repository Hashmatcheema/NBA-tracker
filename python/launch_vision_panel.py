"""
J2K Vision — subprocess entry for Helios / CVPython.

Helios ``CVPython`` rejects ``python -c "..."``; the host spawns this file instead.
Parent sets ``J2K_VISION_PANEL_CHILD=1`` before ``Popen``.
"""
from __future__ import annotations

import os
import sys

_script_dir = os.path.dirname(os.path.abspath(__file__))
if _script_dir not in sys.path:
    sys.path.insert(0, _script_dir)

os.chdir(_script_dir)


def main() -> int:
    os.environ.setdefault("J2K_VISION_PANEL_CHILD", "1")
    from j2k_vision_ui import run_helios_panel_process

    return run_helios_panel_process()


if __name__ == "__main__":
    raise SystemExit(main())
