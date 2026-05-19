import argparse
import csv
import ctypes
import json
import os
import time
from pathlib import Path

import cv2


GCV_SIZE = 48
OFF_PLAYER = 12
OFF_BALL = 24
OFF_FIRE = 32
OFF_STAMINA = 40


def _u16be(buf: bytes, off: int) -> int:
    return int.from_bytes(buf[off : off + 2], "big")


def _read_box_px(buf: bytes, off: int, w: int, h: int):
    x1n = _u16be(buf, off + 0)
    y1n = _u16be(buf, off + 2)
    x2n = _u16be(buf, off + 4)
    y2n = _u16be(buf, off + 6)
    if x2n <= x1n or y2n <= y1n:
        return None
    x1 = int(x1n * w / 65535)
    y1 = int(y1n * h / 65535)
    x2 = int(x2n * w / 65535)
    y2 = int(y2n * h / 65535)
    return (x1, y1, x2, y2)


def _draw_box(frame, box, color, label):
    if box is None:
        return
    x1, y1, x2, y2 = box
    cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
    cv2.putText(
        frame,
        label,
        (x1, max(14, y1 - 6)),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.45,
        color,
        1,
        cv2.LINE_AA,
    )


def _load_dll(dll_path: str):
    dll = ctypes.CDLL(dll_path)
    dll.r.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_char_p]
    dll.r.restype = ctypes.c_int
    dll.p.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    dll.p.restype = ctypes.c_int
    dll.g.argtypes = []
    dll.g.restype = ctypes.c_void_p
    dll.x.argtypes = []
    dll.x.restype = None
    return dll


def run(video_path: str, out_dir: str, max_frames: int = 0) -> dict:
    vp = Path(video_path)
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    base = vp.stem
    out_video = out / f"{base}_cpp_authority_annotated.mp4"
    out_csv = out / f"{base}_cpp_authority_frames.csv"
    out_json = out / f"{base}_cpp_authority_report.json"
    fail_dir = out / f"{base}_cpp_authority_failure_frames"
    fail_dir.mkdir(parents=True, exist_ok=True)

    cap = cv2.VideoCapture(str(vp))
    if not cap.isOpened():
        raise RuntimeError(f"Cannot open video: {video_path}")
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH) or 1920)
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT) or 1080)
    fps = float(cap.get(cv2.CAP_PROP_FPS) or 30.0)
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)

    dll_env = os.environ.get("J2K_GCV_DLL", "").strip()
    dll_path = Path(dll_env) if dll_env else (Path(__file__).resolve().parent / "bin" / "j2k_ch.dll")
    if not dll_path.exists():
        dll_path = Path(__file__).resolve().parent / "bin" / "j2k_ch.dll"
    dll = _load_dll(str(dll_path))
    script_dir = str(Path(__file__).resolve().parent)
    rc = dll.r(w, h, script_dir.encode("utf-8"))
    if rc != 0:
        raise RuntimeError(f"dll.r failed: {rc}")

    writer = cv2.VideoWriter(str(out_video), cv2.VideoWriter_fourcc(*"mp4v"), fps, (w, h))
    rows = []
    fail_frames = []
    counts = {
        "player_visible_frames": 0,
        "player_lost_frames": 0,
        "ball_visible_frames": 0,
        "ball_lost_frames": 0,
        "stamina_visible_frames": 0,
        "stamina_lost_frames": 0,
    }
    prev_c = {"player": None, "ball": None, "stamina": None}

    t_start = time.perf_counter()
    i = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        i += 1
        if max_frames > 0 and i > max_frames:
            break
        t0 = time.perf_counter()
        rr = dll.p(frame.ctypes.data, w, h, int(frame.strides[0]))
        if rr < 0:
            continue
        g_ptr = dll.g()
        gcv = ctypes.string_at(g_ptr, GCV_SIZE)
        frame_ms = (time.perf_counter() - t0) * 1000.0

        player = _read_box_px(gcv, OFF_PLAYER, w, h)
        ball = _read_box_px(gcv, OFF_BALL, w, h)
        stamina = _read_box_px(gcv, OFF_STAMINA, w, h)
        fire = _read_box_px(gcv, OFF_FIRE, w, h)

        # Requested colors: blue=ball, green=stamina, purple=player, red=firezone.
        _draw_box(frame, player, (255, 0, 255), "PLAYER")
        _draw_box(frame, ball, (255, 0, 0), "BALL")
        _draw_box(frame, stamina, (0, 255, 0), "STAMINA")
        _draw_box(frame, fire, (0, 0, 255), "FIREZONE")

        fps_live = i / max(1e-6, time.perf_counter() - t_start)
        cv2.putText(
            frame,
            f"FPS={fps_live:.1f} FRAMEms={frame_ms:.1f}",
            (12, 20),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.55,
            (225, 225, 225),
            2,
            cv2.LINE_AA,
        )

        fails = []
        for key, box in (("player", player), ("ball", ball), ("stamina", stamina)):
            if box is None:
                counts[f"{key}_lost_frames"] += 1
                fails.append(f"{key.upper()}_LOST")
                prev_c[key] = None
                continue
            counts[f"{key}_visible_frames"] += 1
            cx = (box[0] + box[2]) * 0.5
            cy = (box[1] + box[3]) * 0.5
            if prev_c[key] is not None:
                jump = ((cx - prev_c[key][0]) ** 2 + (cy - prev_c[key][1]) ** 2) ** 0.5
                if key == "player" and jump > 220:
                    fails.append("PLAYER_JUMP")
                if key == "ball" and jump > 280:
                    fails.append("BALL_JUMP")
                if key == "stamina" and jump > 180:
                    fails.append("STAMINA_JUMP")
            prev_c[key] = (cx, cy)

        if fails:
            cv2.putText(frame, "FAIL: " + ",".join(fails[:4]), (12, 44), cv2.FONT_HERSHEY_SIMPLEX, 0.52, (0, 0, 255), 2)
            if len(fail_frames) < 200:
                fail_frames.append(i)
                cv2.imwrite(str(fail_dir / f"frame_{i:06d}.png"), frame)
        else:
            cv2.putText(frame, "STATUS: stable", (12, 44), cv2.FONT_HERSHEY_SIMPLEX, 0.52, (0, 200, 120), 2)

        rows.append(
            {
                "frame": i,
                "player_visible": int(player is not None),
                "ball_visible": int(ball is not None),
                "stamina_visible": int(stamina is not None),
                "frame_ms": round(frame_ms, 3),
                "fails": ";".join(fails),
            }
        )
        writer.write(frame)
        if (i % 120) == 0:
            print(f"\r[{i}/{max(1,total)}] fps={fps_live:.1f} fails={len(fail_frames)}", end="", flush=True)

    print()
    cap.release()
    writer.release()
    dll.x()

    elapsed = max(1e-6, time.perf_counter() - t_start)
    avg_fps = i / elapsed
    report = {
        "video": str(vp),
        "total_frames": i,
        "input_frames": total,
        "average_fps": avg_fps,
        "player_visible_frames": counts["player_visible_frames"],
        "player_predicted_frames": 0,
        "player_lost_frames": counts["player_lost_frames"],
        "ball_visible_frames": counts["ball_visible_frames"],
        "ball_predicted_frames": 0,
        "ball_lost_frames": counts["ball_lost_frames"],
        "stamina_visible_frames": counts["stamina_visible_frames"],
        "stamina_predicted_frames": 0,
        "stamina_lost_frames": counts["stamina_lost_frames"],
        "failure_frame_count": len(fail_frames),
        "worst_failure_frames": fail_frames[:50],
        "annotated_video": str(out_video),
        "frame_csv": str(out_csv),
        "failure_frame_dir": str(fail_dir),
        "overall_pass": len(fail_frames) <= max(15, int(i * 0.01)),
    }

    with open(out_json, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    if rows:
        with open(out_csv, "w", encoding="utf-8", newline="") as f:
            cw = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            cw.writeheader()
            cw.writerows(rows)

    print(f"[cpp-validate] annotated -> {out_video}")
    print(f"[cpp-validate] report    -> {out_json}")
    print(f"[cpp-validate] csv       -> {out_csv}")
    print(f"[cpp-validate] pass={report['overall_pass']} failures={len(fail_frames)} avg_fps={avg_fps:.2f}")
    return report


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("--out", default=None)
    ap.add_argument("--max-frames", type=int, default=0)
    args = ap.parse_args()
    out_dir = args.out or str(Path(args.input).resolve().parent)
    r = run(args.input, out_dir, max_frames=max(0, int(args.max_frames)))
    raise SystemExit(0 if r.get("overall_pass") else 1)
