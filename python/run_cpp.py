"""
Helios GCV entry — thin shell only (same ctypes pattern as ``_2k_Vision/2k_Vision.py``).

All behavior lives in native ``j2k_ch.dll`` (see ``cpp/src/dll/``). This file only loads the DLL.

Env:
- ``J2K_GCV_DLL`` — absolute path to the DLL.
- ``J2K_GCV_DLL_NAME`` — basename under ``python/bin/`` (default ``j2k_ch.dll``).
- ``J2K_NO_AUTO_UI`` — if ``1`` / ``true``, do not auto-open the PyQt panel when Helios embeds ``GCVWorker``.
- ``J2K_VISION_PANEL_CHILD`` — internal: set in the subprocess that only runs the tuning window.
- ``J2K_PANEL_PYTHON`` / ``J2K_PYTHON`` — optional: full path to a real ``python.exe`` for the tuning window.
  If unset, ``j2k_panel_python.path`` (written by ``scripts\\j2k.bat`` after pip) is used when it differs from
  the GCV host. Otherwise we detect Helios CVPython / bundled ``python.exe`` and try PATH ``python`` or ``py -3``.

CLI (``python run_cpp.py`` from ``python/``): opens the **J2K Vision** PyQt panel. Helios embeds
``GCVWorker`` without ``__main__`` (tuning is the Python UI only). Legacy: ``J2K_CLI_DLL_ONLY=1`` loads
the DLL from the console and waits for Enter. ``scripts\\j2k.bat run`` refreshes pip deps and ONNX only.
"""
import ctypes
import json
import os
import time
import shutil
import subprocess
import sys
import threading

__all__ = ["GCVWorker"]

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_DLL_NAME = (os.environ.get("J2K_GCV_DLL_NAME") or "j2k_ch.dll").strip() or "j2k_ch.dll"
_EXE_LOW = os.path.basename(sys.executable).lower()
_DRAW_OVERLAY_DEFAULT = "1"
_DRAW_OVERLAY = (os.environ.get("J2K_DRAW_OVERLAY") or _DRAW_OVERLAY_DEFAULT).strip().lower() not in (
    "0",
    "false",
    "no",
)
_PROCESS_SCALE_DEFAULT = "1.0"
try:
    _PROCESS_SCALE = float((os.environ.get("J2K_PROCESS_SCALE") or _PROCESS_SCALE_DEFAULT).strip())
except ValueError:
    _PROCESS_SCALE = 1.0
_PROCESS_SCALE = max(0.25, min(1.0, _PROCESS_SCALE))

_helios_ui_lock = threading.Lock()
_helios_ui_started = False
_helios_ui_proc = None
_DEBUG_SESSION_ID = os.environ.get("J2K_TRACK_DEBUG_SESSION", "runtime")
_DEBUG_LOG_PATH = os.path.normpath(
    os.environ.get("J2K_TRACK_DEBUG_LOG")
    or os.path.join(os.path.dirname(_SCRIPT_DIR), "track-debug-latest.jsonl")
)
_overlay_diag: dict[str, object] = {
    "prev_pb": None,
    "prev_fz": None,
    "prev_sb": None,
    "same_pb": 0,
    "same_fz": 0,
    "same_sb": 0,
    "first_pb_change_frame": -1,
    "first_fz_change_frame": -1,
    "boot_pb": None,
    "boot_fz": None,
    "stub_proxy_active": 0,
    "proxy_pb_prev": None,
    "proxy_fz_prev": None,
}
_process_diag: dict[str, object] = {
    "last_frame_ts": 0.0,
}
_process_perf_diag: dict[str, object] = {
    "last_return_ts": 0.0,
}
_input_diag: dict[str, int] = {
    "sq": -1,
    "r2": -1,
    "frame": -1,
}
_cuda_dll_dir_handles: list[object] = []

# Prepend python/bin to PATH and add it as a DLL directory at import time.
# ORT 1.26's CUDA/TRT provider DLLs dynamically load cuDNN9/cuBLAS12/TRT10 by filename;
# those DLLs are staged in python/bin but won't be found unless the dir is in PATH.
# This must happen before any DLL (including j2k_ch.dll) is loaded.
_bin_dir_early = os.path.join(_SCRIPT_DIR, "bin")
if os.path.isdir(_bin_dir_early):
    _sep = os.pathsep
    _cur_path = os.environ.get("PATH", "")
    if os.path.normcase(_bin_dir_early) not in [os.path.normcase(p) for p in _cur_path.split(_sep) if p]:
        os.environ["PATH"] = _bin_dir_early + _sep + _cur_path
    if hasattr(os, "add_dll_directory"):
        try:
            _cuda_dll_dir_handles.append(os.add_dll_directory(_bin_dir_early))
        except Exception:
            pass
del _bin_dir_early, _cur_path, _sep


def _dbg_log(hypothesis_id: str, location: str, message: str, data: dict, *, run_id: str = "pre-fix") -> None:
    enabled = (os.environ.get("J2K_TRACK_DEBUG") or "").strip().lower() in ("1", "true", "yes")
    if not enabled:
        return
    payload = {
        "sessionId": _DEBUG_SESSION_ID,
        "runId": run_id,
        "hypothesisId": hypothesis_id,
        "location": location,
        "message": message,
        "data": data,
        "timestamp": int(time.time() * 1000),
    }
    try:
        with open(_DEBUG_LOG_PATH, "a", encoding="utf-8") as f:
            f.write(json.dumps(payload, separators=(",", ":")) + "\n")
    except OSError:
        pass

def _agent_log(hypothesis_id: str, location: str, message: str, data: dict) -> None:
    # Debug instrumentation disabled after fix verification.
    return


# Helios CVPython entrypoints (some hosts call module-level `process(frame)` directly).
_helios_worker = None
_helios_frame_i = 0



def _pinned_panel_python(script_dir: str) -> str | None:
    path = os.path.join(script_dir, "j2k_panel_python.path")
    try:
        raw = open(path, encoding="utf-8").read().strip().strip('"')
    except OSError:
        return None
    if not raw or not os.path.isfile(raw):
        return None
    return os.path.normpath(raw)


def _host_is_embedded_gcvr_python(host: str) -> bool:
    """True when the GCV host is Helios CVPython / bundled python (panel must use another interpreter)."""
    base = os.path.basename(host).lower()
    low = host.lower().replace("\\", "/")
    if "cvpython" in base or "cvpython" in low:
        return True
    # Helios may use python.exe without "cvpython" in the name
    if base == "python.exe" and "helios" in low:
        return True
    return False


def _panel_subprocess_command(launcher: str) -> list[str] | None:
    """Build argv to run ``launch_vision_panel.py``. Helios CVPython rejects ``-c`` and ``script.py`` args."""
    script_dir = os.path.dirname(launcher)
    host = sys.executable
    host_abs = os.path.normcase(os.path.abspath(host))

    for key in ("J2K_PANEL_PYTHON", "J2K_PYTHON"):
        raw = (os.environ.get(key) or "").strip().strip('"')
        if raw and os.path.isfile(raw):
            return [raw, launcher]

    pinned = _pinned_panel_python(script_dir)
    if pinned:
        pin_abs = os.path.normcase(os.path.abspath(pinned))
        if pin_abs != host_abs:
            return [pinned, launcher]

    if not _host_is_embedded_gcvr_python(host):
        return [host, launcher]

    for name in ("python", "python3"):
        cand = shutil.which(name)
        if not cand:
            continue
        try:
            if os.path.normcase(os.path.abspath(cand)) != host_abs:
                return [cand, launcher]
        except OSError:
            return [cand, launcher]

    py_launch = shutil.which("py")
    if py_launch:
        return [py_launch, "-3", launcher]

    return None


def _maybe_open_helios_vision_ui() -> None:
    """After ``GCVWorker`` loads the DLL, open the PyQt tuner in a **subprocess** (Qt needs a real main thread)."""
    global _helios_ui_started, _helios_ui_proc
    if (os.environ.get("J2K_NO_AUTO_UI") or "").strip().lower() in ("1", "true", "yes"):
        return
    if (os.environ.get("J2K_VISION_PANEL_CHILD") or "").strip() == "1":
        return
    try:
        import j2k_vision_ui as _ui

        if getattr(_ui, "_standalone_cli_active", False):
            return
    except ImportError:
        pass
    with _helios_ui_lock:
        if _helios_ui_started:
            return
        _helios_ui_started = True
    script_dir = os.path.normpath(_SCRIPT_DIR)
    launcher = os.path.join(script_dir, "launch_vision_panel.py")
    if not os.path.isfile(launcher):
        _helios_ui_started = False
        return
    cmd = _panel_subprocess_command(launcher)
    if not cmd:
        _helios_ui_started = False
        return
    env = os.environ.copy()
    env["J2K_VISION_PANEL_CHILD"] = "1"
    env["J2K_PANEL_PARENT_PID"] = str(os.getpid())
    try:
        # #region agent log
        _agent_log(
            "H51",
            "run_cpp.py:_maybe_open_helios_vision_ui",
            "panel_subprocess_before_popen",
            {"cmd": cmd, "cwd": script_dir},
        )
        # #endregion
        _helios_ui_proc = subprocess.Popen(
            cmd,
            cwd=script_dir,
            env=env,
            close_fds=False,
        )
        # #region agent log
        _agent_log(
            "H51",
            "run_cpp.py:_maybe_open_helios_vision_ui",
            "panel_subprocess_popen_ok",
            {
                "cmd0": cmd[0] if len(cmd) > 0 else "",
                "childEnvFlag": env.get("J2K_VISION_PANEL_CHILD", ""),
                "childPid": int(getattr(_helios_ui_proc, "pid", 0) or 0),
            },
        )
        # #endregion
    except Exception:
        # #region agent log
        _agent_log(
            "H51",
            "run_cpp.py:_maybe_open_helios_vision_ui",
            "panel_subprocess_popen_failed",
            {"cmd": cmd},
        )
        # #endregion
        _helios_ui_started = False
        _helios_ui_proc = None


def _stop_helios_vision_ui() -> None:
    global _helios_ui_started, _helios_ui_proc
    proc = _helios_ui_proc
    _helios_ui_proc = None
    _helios_ui_started = False
    if proc is None:
        return
    try:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=1.5)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass
    except Exception:
        pass


def _resolved_dll_path() -> str:
    _env = (os.environ.get("J2K_GCV_DLL") or "").strip()
    if _env:
        return os.path.normpath(_env)
    primary = os.path.join(_SCRIPT_DIR, "bin", _DLL_NAME)
    staged = f"{primary}.new"
    # Auto-promote staged DLL to primary when staged is newer.
    # This runs at startup, before the DLL is loaded, so the file is not yet locked.
    if os.path.isfile(staged):
        try:
            staged_mtime = os.path.getmtime(staged)
            primary_mtime = os.path.getmtime(primary) if os.path.isfile(primary) else 0.0
            if staged_mtime > primary_mtime:
                import shutil as _shutil
                _shutil.copy2(staged, primary)
                os.remove(staged)
        except Exception:
            pass  # Promotion failed (locked or permissions) — fall through to existing logic
    # Prefer staged DLL when explicitly requested (helps when python/bin/j2k_ch.dll is locked).
    if (os.environ.get("J2K_USE_STAGED_DLL") or "").strip().lower() in ("1", "true", "yes"):
        if os.path.isfile(staged):
            return staged
    if os.path.isfile(primary):
        return primary
    if os.path.isfile(staged):
        return staged
    return primary


def _dll_visible_on_search_path(name: str) -> bool:
    probe_dirs = [os.path.join(_SCRIPT_DIR, "bin")]
    probe_dirs.extend([p for p in os.environ.get("PATH", "").split(os.pathsep) if p])
    seen = set()
    for d in probe_dirs:
        nd = os.path.normcase(os.path.normpath(d))
        if nd in seen:
            continue
        seen.add(nd)
        if os.path.isfile(os.path.join(d, name)):
            return True
    return False


def _dll_loadable(name: str) -> bool:
    try:
        ctypes.WinDLL(name)
        return True
    except Exception:
        return False


def _cuda_runtime_ready_for_ort() -> tuple[bool, list[str]]:
    # ORT 1.17 CUDA EP (CUDA 11 + cuDNN 8) can fail-fast if provider deps are only partially present.
    required = [
        "cudart64_110.dll",
        "cublas64_11.dll",
        "cublasLt64_11.dll",
        "cudnn64_8.dll",
        "cudnn_ops_infer64_8.dll",
        "cudnn_cnn_infer64_8.dll",
    ]
    missing = []
    for dll in required:
        if not _dll_visible_on_search_path(dll):
            missing.append(dll)
            continue
        if not _dll_loadable(dll):
            missing.append(dll)
    return (len(missing) == 0, missing)


def _candidate_cuda_dirs() -> list[str]:
    dirs: list[str] = []
    explicit = []
    for key in ("J2K_CUDA_BIN", "J2K_CUDNN_BIN"):
        raw = (os.environ.get(key) or "").strip().strip('"')
        if raw:
            explicit.append(raw)
    cuda_path = (os.environ.get("CUDA_PATH") or "").strip().strip('"')
    if cuda_path:
        explicit.append(os.path.join(cuda_path, "bin"))
    explicit.append(os.path.join(_SCRIPT_DIR, "bin"))
    for d in explicit:
        if d and os.path.isdir(d):
            dirs.append(os.path.normpath(d))

    for base in (os.environ.get("ProgramFiles") or "", os.environ.get("ProgramFiles(x86)") or ""):
        if not base:
            continue
        cuda_glob = os.path.join(base, "NVIDIA GPU Computing Toolkit", "CUDA")
        if os.path.isdir(cuda_glob):
            for name in os.listdir(cuda_glob):
                if name.lower().startswith("v11."):
                    b = os.path.join(cuda_glob, name, "bin")
                    if os.path.isdir(b):
                        dirs.append(os.path.normpath(b))
        cudnn_glob = os.path.join(base, "NVIDIA", "CUDNN")
        if os.path.isdir(cudnn_glob):
            for name in os.listdir(cudnn_glob):
                if name.lower().startswith("v8"):
                    b = os.path.join(cudnn_glob, name, "bin")
                    if os.path.isdir(b):
                        dirs.append(os.path.normpath(b))

    seen = set()
    out: list[str] = []
    for d in dirs:
        nd = os.path.normcase(d)
        if nd in seen:
            continue
        seen.add(nd)
        out.append(d)
    return out


def _inject_cuda_dll_dirs() -> list[str]:
    added: list[str] = []
    add_dir = getattr(os, "add_dll_directory", None)
    if add_dir is None:
        return added
    required = (
        "cudart64_110.dll",
        "cublas64_11.dll",
        "cublasLt64_11.dll",
        "cudnn64_8.dll",
        "cudnn_ops_infer64_8.dll",
        "cudnn_cnn_infer64_8.dll",
    )
    for d in _candidate_cuda_dirs():
        if not any(os.path.isfile(os.path.join(d, dll)) for dll in required):
            continue
        try:
            h = add_dir(d)
            _cuda_dll_dir_handles.append(h)
            added.append(d)
        except Exception:
            pass
    return added


def _read_gcv_box_u16(gcv: bytes, offset: int) -> tuple[int, int, int, int]:
    return (
        int.from_bytes(gcv[offset : offset + 2], "big"),
        int.from_bytes(gcv[offset + 2 : offset + 4], "big"),
        int.from_bytes(gcv[offset + 4 : offset + 6], "big"),
        int.from_bytes(gcv[offset + 6 : offset + 8], "big"),
    )


def _norm_box_to_px(
    x1: int, y1: int, x2: int, y2: int, fw: int, fh: int
) -> tuple[int, int, int, int] | None:
    if x2 <= x1 or y2 <= y1:
        return None
    return (
        int(x1 * fw / 65535),
        int(y1 * fh / 65535),
        int(x2 * fw / 65535),
        int(y2 * fh / 65535),
    )


def _draw_rect_bgr_numpy(frame, x1: int, y1: int, x2: int, y2: int, bgr: tuple[int, int, int], thickness: int = 2) -> None:
    """BGR rectangle outline without OpenCV (Helios CVPythonHost often has no cv2)."""
    try:
        import numpy as np
    except ImportError:
        return
    arr = np.asarray(frame)
    if arr.ndim < 2:
        return
    h, w = int(arr.shape[0]), int(arr.shape[1])
    x1 = max(0, min(w - 1, x1))
    x2 = max(0, min(w - 1, x2))
    y1 = max(0, min(h - 1, y1))
    y2 = max(0, min(h - 1, y2))
    if x2 <= x1 or y2 <= y1:
        return
    color = np.array(bgr, dtype=arr.dtype)
    t = max(1, int(thickness))
    for k in range(t):
        yy = y1 + k
        if yy <= y2:
            arr[yy, x1 : x2 + 1] = color
        yy = y2 - k
        if yy >= y1:
            arr[yy, x1 : x2 + 1] = color
    for k in range(t):
        xx = x1 + k
        if xx <= x2:
            arr[y1 : y2 + 1, xx] = color
        xx = x2 - k
        if xx >= x1:
            arr[y1 : y2 + 1, xx] = color


def _draw_gcv_overlay(frame, gcv: bytes, *, frame_i: int = 0):
    """No-op — the C++ DLL draws the overlay directly onto the frame buffer.

    Kept as a stub so existing callers (GCVWorker.process, Helios `process`)
    continue to work without changes. Previously this function decoded the
    GCV bytes and drew rectangles in Python; that work now lives in
    `J2kEngine::draw_overlay` (see cpp/src/dll/j2k_engine.cpp).
    """
    return frame


class GCVWorker:
    def __init__(self, w, h):
        _t_init0 = time.perf_counter()
        # CVPythonHost: keep YOLO cadence responsive by default; users can still override with J2K_YOLO_EVERY_N.
        _exe_low = os.path.basename(sys.executable).lower()
        # Large defaults (e.g. 160) make player/fire-zone appear frozen for multiple seconds.
        if ("cvpython" in _exe_low or "cv_python_host" in _exe_low) and not (
            os.environ.get("J2K_YOLO_EVERY_N") or ""
        ).strip():
            # CVPython default now scales with resolution to avoid recurring heavy-frame stutter at 1080p.
            # 1080p: n=5 reduces pressure vs n=4 while preserving continuity with carry/lock logic.
            # 720p:  n=4 keeps responsiveness; lower res can afford higher cadence.
            pixels = int(w) * int(h)
            if pixels >= 1920 * 1000:
                os.environ["J2K_YOLO_EVERY_N"] = "5"
            elif pixels >= 1280 * 720:
                os.environ["J2K_YOLO_EVERY_N"] = "4"
            else:
                os.environ["J2K_YOLO_EVERY_N"] = "3"
            # #region agent log
            _agent_log(
                "H30",
                "run_cpp.py:GCVWorker.__init__",
                "default_yolo_every_n_for_cvpython",
                {"J2K_YOLO_EVERY_N": os.environ.get("J2K_YOLO_EVERY_N")},
            )
            # #endregion
        if not (os.environ.get("J2K_YOLO_EVERY_N") or "").strip():
            # Public-release default: prioritize continuous tracking over sparse cadence.
            pixels = int(w) * int(h)
            if pixels >= 1920 * 1000:
                os.environ["J2K_YOLO_EVERY_N"] = "2"
            elif pixels >= 1280 * 720:
                os.environ["J2K_YOLO_EVERY_N"] = "3"
            else:
                os.environ["J2K_YOLO_EVERY_N"] = "4"
        # #region agent log
        _dbg_log(
            "H2",
            "run_cpp.py:GCVWorker.__init__",
            "runtime_cadence_config",
            {
                "frameW": int(w),
                "frameH": int(h),
                "processScale": float(_PROCESS_SCALE),
                "yoloEveryN": os.environ.get("J2K_YOLO_EVERY_N", ""),
            },
        )
        # #endregion

        _t_cuda0 = time.perf_counter()
        _cuda_prov = os.path.join(_SCRIPT_DIR, "bin", "onnxruntime_providers_cuda.dll")
        if not os.path.isfile(_cuda_prov):
            raise RuntimeError(
                "GPU-only mode: missing onnxruntime_providers_cuda.dll in python/bin."
            )
        cuda_ready = False
        missing_cuda: list[str] = []
        cuda_dirs_added: list[str] = []
        cuda_ready, missing_cuda = _cuda_runtime_ready_for_ort()
        if not cuda_ready:
            cuda_dirs_added = _inject_cuda_dll_dirs()
            if cuda_dirs_added:
                cuda_ready, missing_cuda = _cuda_runtime_ready_for_ort()
        if cuda_ready:
            os.environ["J2K_ORT_CUDA"] = "1"
            # #region agent log
            _agent_log(
                "H57",
                "run_cpp.py:GCVWorker.__init__",
                "cuda_ep_enabled_preflight_ok",
                {"providerDll": _cuda_prov},
            )
            # #endregion
        else:
            os.environ["J2K_ORT_CUDA"] = "0"
            # #region agent log
            _agent_log(
                "H57",
                "run_cpp.py:GCVWorker.__init__",
                "cuda_ep_disabled_missing_runtime_dlls",
                {"missing": missing_cuda},
            )
            # #endregion
        # #region agent log
        _dbg_log(
            "H10",
            "run_cpp.py:GCVWorker.__init__",
            "cuda_preflight_result",
            {
                "cudaProviderDllPresent": bool(os.path.isfile(_cuda_prov)),
                "cudaPreflightReady": bool(cuda_ready),
                "missingCudaDeps": missing_cuda,
                "cudaDirsAdded": cuda_dirs_added,
                "ortCudaEnv": os.environ.get("J2K_ORT_CUDA", ""),
                "processScaleEnv": os.environ.get("J2K_PROCESS_SCALE", ""),
                "procScale": float(_PROCESS_SCALE),
            },
        )
        # #endregion
        if not cuda_ready:
            miss = ", ".join(missing_cuda) if missing_cuda else "unknown CUDA/cuDNN dependency"
            raise RuntimeError(
                f"GPU-only mode: CUDA runtime not ready for ONNX Runtime (missing: {miss})."
            )
        _t_cuda1 = time.perf_counter()

        _dll = _resolved_dll_path()
        if not os.path.isfile(_dll):
            raise FileNotFoundError(
                f"J2K GCV DLL not found: {_dll!r} (set J2K_GCV_DLL or place {_DLL_NAME!r} in python/bin/)"
            )
        self._z = bytearray(1)
        # CDLL: MSVC C exports use cdecl; releases GIL during r/p/g so Helios threads are not blocked.
        # #region agent log
        _agent_log(
            "H50",
            "run_cpp.py:GCVWorker.__init__",
            "before_cdll_load",
            {"dll": _dll},
        )
        # #endregion
        self._d = ctypes.CDLL(_dll)
        # #region agent log
        _agent_log(
            "H53",
            "run_cpp.py:GCVWorker.__init__",
            "after_cdll_load",
            {"dll": _dll},
        )
        # #endregion
        self._d.r.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_char_p]
        self._d.r.restype = ctypes.c_int
        self._d.p.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        self._d.p.restype = ctypes.c_int
        self._d.g.argtypes = []
        self._d.g.restype = ctypes.c_void_p
        self._d.x.argtypes = []
        self._d.x.restype = None
        if hasattr(self._d, "u"):
            self._d.u.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
            self._d.u.restype = None
        self._u = getattr(self._d, "u", None)
        self._p = self._d.p
        self._g = self._d.g
        self._x = self._d.x
        try:
            import gtuner  # type: ignore[import-not-found]

            self._gtuner_get_actual = gtuner.get_actual
            self._btn_r2 = gtuner.BUTTON_5
            self._btn_sq = gtuner.BUTTON_17
        except (ImportError, AttributeError):
            self._gtuner_get_actual = None
        # #region agent log
        _agent_log(
            "H54",
            "run_cpp.py:GCVWorker.__init__",
            "before_dll_r",
            {"w": int(w), "h": int(h)},
        )
        # #endregion
        self._proc_scale = _PROCESS_SCALE
        self._proc_w = max(1, int(round(int(w) * self._proc_scale)))
        self._proc_h = max(1, int(round(int(h) * self._proc_scale)))
        self._frame_w = int(w)
        self._frame_h = int(h)
        self._prev_return_ts = 0.0
        _t_r0 = time.perf_counter()
        _rc = self._d.r(self._proc_w, self._proc_h, _SCRIPT_DIR.encode("utf-8"))
        _t_r1 = time.perf_counter()
        if _rc != 0:
            raise RuntimeError(f"DLL init failed with code {_rc}")
        # #region agent log
        _agent_log(
            "H1",
            "run_cpp.py:GCVWorker.__init__",
            "dll_init_ok",
            {"dll": _dll, "w": int(w), "h": int(h), "rc": int(_rc)},
        )
        # #endregion
        _maybe_open_helios_vision_ui()
        self._agent_frame_i = 0
        self.last_tracking_state: dict[str, object] = {
            "player": {"exists": False, "visible": False, "predicted": False, "lost": True, "smooth_box": None},
            "ball": {"exists": False, "visible": False, "predicted": False, "lost": True, "smooth_box": None},
            "stamina": {"exists": False, "visible": False, "predicted": False, "lost": True, "smooth_box": None},
            "performance": {"yolo_ms": 0.0, "tracking_ms": 0.0, "latency_ms": 0.0},
            "rejections_by_class": {"PLAYER": {}, "BALL": {}, "STAMINA": {}},
            "identity_switches_by_class": {"PLAYER": 0, "BALL": 0, "STAMINA": 0},
        }
        _t_init1 = time.perf_counter()
        _dbg_log(
            "H60",
            "run_cpp.py:GCVWorker.__init__",
            "startup_timing",
            {
                "cudaPreflightMs": float((_t_cuda1 - _t_cuda0) * 1000.0),
                "dllInitMs": float((_t_r1 - _t_r0) * 1000.0),
                "workerInitMs": float((_t_init1 - _t_init0) * 1000.0),
                "procW": int(self._proc_w),
                "procH": int(self._proc_h),
            },
        )

    def close(self):
        _x = getattr(self, "_x", None)
        if _x is not None:
            self._x = None
            _x()
        _stop_helios_vision_ui()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def process(self, frame):
        _call_t0 = time.perf_counter()
        self._agent_frame_i = int(getattr(self, "_agent_frame_i", 0)) + 1
        try:
            sq = -1
            r2 = -1
            if self._u is not None and self._gtuner_get_actual is not None:
                try:
                    sq = int(self._gtuner_get_actual(self._btn_sq))
                    r2 = int(self._gtuner_get_actual(self._btn_r2))
                    self._u(
                        ctypes.c_int(sq),
                        ctypes.c_int(r2),
                        ctypes.c_int(r2),
                    )
                except (OSError, TypeError, ValueError):
                    pass
            _input_diag["sq"] = int(sq)
            _input_diag["r2"] = int(r2)
            _input_diag["frame"] = int(self._agent_frame_i)
            if (self._agent_frame_i % 30) == 0:
                # #region agent log
                _dbg_log(
                    "H59",
                    "run_cpp.py:GCVWorker.process",
                    "controller_state",
                    {
                        "frame": int(self._agent_frame_i),
                        "sq": int(sq),
                        "r2": int(r2),
                    },
                )
                # #endregion

            if self._agent_frame_i == 1:
                # #region agent log
                _agent_log(
                    "H23",
                    "run_cpp.py:GCVWorker.process",
                    "before_dll_p_first_frame",
                    {
                        "shape": [int(frame.shape[0]), int(frame.shape[1]), int(frame.shape[2]) if len(frame.shape) > 2 else 1],
                        "stride": int(frame.strides[0]),
                        "dtype": str(getattr(frame, "dtype", "")),
                    },
                )
                # #endregion
            _t_p0 = time.perf_counter()
            proc_frame = frame
            if self._proc_scale < 0.999:
                try:
                    import cv2  # type: ignore[import-not-found]

                    proc_frame = cv2.resize(
                        frame,
                        (self._proc_w, self._proc_h),
                        interpolation=cv2.INTER_AREA,
                    )
                except Exception:
                    try:
                        import numpy as np

                        step = max(1, int(round(1.0 / self._proc_scale)))
                        proc_frame = np.ascontiguousarray(frame[::step, ::step, :])
                    except Exception:
                        proc_frame = frame
            _sz = self._p(
                proc_frame.ctypes.data,
                proc_frame.shape[1],
                proc_frame.shape[0],
                proc_frame.strides[0],
            )
            _t_p1 = time.perf_counter()
            if _sz < 0 or _sz > 4096:
                # #region agent log
                _agent_log(
                    "H52",
                    "run_cpp.py:GCVWorker.process",
                    "dll_p_return_size_suspicious",
                    {"i": self._agent_frame_i, "sz": int(_sz)},
                )
                # #endregion
            if self._agent_frame_i == 1:
                # #region agent log
                _agent_log(
                    "H24",
                    "run_cpp.py:GCVWorker.process",
                    "after_dll_p_first_frame",
                    {"sz": int(_sz), "pMs": float((_t_p1 - _t_p0) * 1000.0)},
                )
                # #endregion
            if (self._agent_frame_i % 60) == 0:
                # #region agent log
                _agent_log(
                    "H2",
                    "run_cpp.py:GCVWorker.process",
                    "process_tick",
                    {
                        "i": self._agent_frame_i,
                        "sz": int(_sz),
                        "shape": [int(frame.shape[0]), int(frame.shape[1]), int(frame.shape[2]) if len(frame.shape) > 2 else 1],
                        "stride": int(frame.strides[0]),
                        "dtype": str(getattr(frame, "dtype", "")),
                    },
                )
                # #endregion
            if (self._agent_frame_i % 30) == 0:
                # #region agent log
                _dbg_log(
                    "H3",
                    "run_cpp.py:GCVWorker.process",
                    "dll_process_runtime",
                    {
                        "frame": int(self._agent_frame_i),
                        "dllBytes": int(_sz),
                        "processMs": float((_t_p1 - _t_p0) * 1000.0),
                        "shape": [int(frame.shape[0]), int(frame.shape[1]), int(frame.shape[2]) if len(frame.shape) > 2 else 1],
                        "procShape": [
                            int(proc_frame.shape[0]),
                            int(proc_frame.shape[1]),
                            int(proc_frame.shape[2]) if len(proc_frame.shape) > 2 else 1,
                        ],
                    },
                )
                # #endregion
            if ((_t_p1 - _t_p0) * 1000.0) > 50.0:
                # #region agent log
                _dbg_log(
                    "H9",
                    "run_cpp.py:GCVWorker.process",
                    "slow_dll_process_spike",
                    {
                        "frame": int(self._agent_frame_i),
                        "processMs": float((_t_p1 - _t_p0) * 1000.0),
                        "procShape": [
                            int(proc_frame.shape[0]),
                            int(proc_frame.shape[1]),
                            int(proc_frame.shape[2]) if len(proc_frame.shape) > 2 else 1,
                        ],
                        "yoloEveryN": os.environ.get("J2K_YOLO_EVERY_N", ""),
                    },
                )
                # #endregion
            if self._agent_frame_i <= 180 and (self._agent_frame_i % 10) == 0:
                now_ts = time.perf_counter()
                last_ts = float(_process_diag.get("last_frame_ts", 0.0))
                frame_dt_ms = float((now_ts - last_ts) * 1000.0) if last_ts > 0.0 else 0.0
                _process_diag["last_frame_ts"] = now_ts
                # #region agent log
                _dbg_log(
                    "H5",
                    "run_cpp.py:GCVWorker.process",
                    "startup_frame_timing",
                    {
                        "frame": int(self._agent_frame_i),
                        "processMs": float((_t_p1 - _t_p0) * 1000.0),
                        "frameDeltaMs": frame_dt_ms,
                        "dllBytes": int(_sz),
                    },
                )
                # #endregion
            if self._agent_frame_i <= 12:
                # #region agent log
                _dbg_log(
                    "H8",
                    "run_cpp.py:GCVWorker.process",
                    "startup_first_frames",
                    {
                        "frame": int(self._agent_frame_i),
                        "processMs": float((_t_p1 - _t_p0) * 1000.0),
                        "dllBytes": int(_sz),
                    },
                )
                # #endregion
            if _sz <= 0:
                # #region agent log
                _agent_log(
                    "H3",
                    "run_cpp.py:GCVWorker.process",
                    "dll_returned_nonpositive",
                    {"i": self._agent_frame_i, "sz": int(_sz)},
                )
                # #endregion
                return frame, self._z

            _result = bytearray(_sz)
            ctypes.memmove((ctypes.c_ubyte * _sz).from_buffer(_result), self._g(), _sz)
            if (self._agent_frame_i % 60) == 0:
                # #region agent log
                _agent_log(
                    "H4",
                    "run_cpp.py:GCVWorker.process",
                    "gcv_bytes_ok",
                    {"i": self._agent_frame_i, "gcvLen": int(len(_result)), "drawOverlay": bool(_DRAW_OVERLAY)},
                )
                # #endregion
            now_rt = time.perf_counter()
            if self._prev_return_ts <= 0.0:
                latency_ms = 0.0
            else:
                latency_ms = float((now_rt - self._prev_return_ts) * 1000.0)
            self._prev_return_ts = now_rt
            player_box = _norm_box_to_px(*_read_gcv_box_u16(_result, 12), self._frame_w, self._frame_h)
            ball_box = _norm_box_to_px(*_read_gcv_box_u16(_result, 24), self._frame_w, self._frame_h)
            stamina_box = _norm_box_to_px(*_read_gcv_box_u16(_result, 40), self._frame_w, self._frame_h)
            self.last_tracking_state = {
                "player": {
                    "exists": player_box is not None,
                    "visible": player_box is not None,
                    "predicted": False,
                    "lost": player_box is None,
                    "smooth_box": list(player_box) if player_box is not None else None,
                },
                "ball": {
                    "exists": ball_box is not None,
                    "visible": ball_box is not None,
                    "predicted": False,
                    "lost": ball_box is None,
                    "smooth_box": list(ball_box) if ball_box is not None else None,
                },
                "stamina": {
                    "exists": stamina_box is not None,
                    "visible": stamina_box is not None,
                    "predicted": False,
                    "lost": stamina_box is None,
                    "smooth_box": list(stamina_box) if stamina_box is not None else None,
                },
                "performance": {
                    "yolo_ms": float((_t_p1 - _t_p0) * 1000.0),
                    "tracking_ms": 0.0,
                    "latency_ms": latency_ms,
                },
                "rejections_by_class": {"PLAYER": {}, "BALL": {}, "STAMINA": {}},
                "identity_switches_by_class": {"PLAYER": 0, "BALL": 0, "STAMINA": 0},
            }
            _draw_t0 = time.perf_counter()
            out_frame = _draw_gcv_overlay(frame, _result, frame_i=self._agent_frame_i)
            _draw_ms = float((time.perf_counter() - _draw_t0) * 1000.0)
            _call_ms = float((time.perf_counter() - _call_t0) * 1000.0)
            if self._agent_frame_i <= 180 and (self._agent_frame_i % 10) == 0:
                now_rt = time.perf_counter()
                prev_rt = float(_process_perf_diag.get("last_return_ts", 0.0))
                return_dt_ms = float((now_rt - prev_rt) * 1000.0) if prev_rt > 0.0 else 0.0
                _process_perf_diag["last_return_ts"] = now_rt
                # #region agent log
                _dbg_log(
                    "H6",
                    "run_cpp.py:GCVWorker.process",
                    "startup_end_to_end_timing",
                    {
                        "frame": int(self._agent_frame_i),
                        "callMs": _call_ms,
                        "drawMs": _draw_ms,
                        "returnDeltaMs": return_dt_ms,
                    },
                )
                # #endregion
            return out_frame, _result
        except Exception as e:
            # #region agent log
            _agent_log(
                "H5",
                "run_cpp.py:GCVWorker.process",
                "process_exception",
                {"i": self._agent_frame_i, "err": repr(e)},
            )
            # #endregion
            return frame, self._z


def init() -> None:
    """Optional Helios hook."""
    # #region agent log
    _agent_log("H17", "run_cpp.py:init", "helios_init_called", {})
    # #endregion


def process(frame):
    """Helios per-frame hook: run DLL and draw overlay."""
    global _helios_worker, _helios_frame_i
    _helios_frame_i += 1
    if _helios_worker is None:
        try:
            h = int(frame.shape[0])
            w = int(frame.shape[1])
        except Exception as e:
            # #region agent log
            _agent_log(
                "H18",
                "run_cpp.py:process",
                "frame_shape_error",
                {"i": _helios_frame_i, "err": repr(e)},
            )
            # #endregion
            return frame
        try:
            _helios_worker = GCVWorker(w, h)
            # #region agent log
            _agent_log("H19", "run_cpp.py:process", "helios_worker_created", {"w": w, "h": h})
            # #endregion
        except Exception as e:
            # #region agent log
            _agent_log(
                "H20",
                "run_cpp.py:process",
                "helios_worker_create_failed",
                {"w": w, "h": h, "err": repr(e)},
            )
            # #endregion
            return frame

    out_frame, gcv = _helios_worker.process(frame)
    if (_helios_frame_i % 60) == 0:
        # #region agent log
        _agent_log(
            "H21",
            "run_cpp.py:process",
            "helios_process_tick",
            {"i": _helios_frame_i, "gcvLen": int(len(gcv)) if hasattr(gcv, "__len__") else -1},
        )
        # #endregion
    return out_frame


def shutdown() -> None:
    """Optional Helios hook."""
    global _helios_worker
    if _helios_worker is not None:
        try:
            _helios_worker.close()
        except Exception:
            pass
        _helios_worker = None
    _stop_helios_vision_ui()
    # #region agent log
    _agent_log("H22", "run_cpp.py:shutdown", "helios_shutdown_called", {"i": int(_helios_frame_i)})
    # #endregion


if __name__ == "__main__":
    # Default: PyQt6 control panel + optional DLL via panel (skips native Win32 GUI).
    if (os.environ.get("J2K_CLI_DLL_ONLY") or "").strip().lower() not in (
        "1",
        "true",
        "yes",
    ):
        from j2k_vision_ui import run_app

        raise SystemExit(run_app())

    w, h = 640, 360
    if len(sys.argv) >= 3:
        try:
            w = int(sys.argv[1])
            h = int(sys.argv[2])
        except ValueError:
            pass

    dll_path = _resolved_dll_path()
    if not os.path.isfile(dll_path):
        print(
            f"[j2k] DLL not found: {dll_path!r}\n"
            "        Run: scripts\\j2k.bat build   (then close Helios if copy warns the file is locked.)",
            file=sys.stderr,
        )
        sys.exit(1)

    try:
        worker = GCVWorker(w, h)
    except (OSError, RuntimeError) as e:
        print(f"[j2k] Failed to initialize GCV / DLL: {e}", file=sys.stderr)
        sys.exit(1)

    print(
        "[j2k] DLL loaded (no C++ tuner in this build).\n"
        "      Press Enter here to unload the DLL, or use: python run_cpp.py for the J2K Vision panel."
    )
    try:
        input()
    finally:
        worker.close()
