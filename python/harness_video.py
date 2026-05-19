"""
Standalone video test harness for J2K V1sion DLL.

Usage:
    python test_video.py <input_video> [output_video] [--every N] [--scale S]

Renders YOLO overlay boxes (player, ball, stamina, fire zone) on every frame
and writes the annotated result to output_video (default: <input>_annotated.mp4).

--every N  : process every Nth frame (default 1 = all frames).
             The DLL still sees every frame but YOLO only fires on heavy frames.
--scale S  : resize input to S fraction before passing to DLL (default 1.0).

Env overrides (same as production):
  J2K_YOLO_EVERY_N  — override DLL's YOLO cadence
  J2K_GCV_DLL       — explicit DLL path
  J2K_DRAW_OVERLAY  — 0 to skip drawing (timing only)
"""
import os
import sys
import time

import cv2

# ── path setup ──────────────────────────────────────────────────────────────
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

# Force YOLO to run on every frame in test mode unless caller overrides.
if not os.environ.get("J2K_YOLO_EVERY_N"):
    os.environ["J2K_YOLO_EVERY_N"] = "1"

# ── imports from run_cpp (overlay + DLL loader) ──────────────────────────────
from run_cpp import GCVWorker  # noqa: E402


def run(input_path: str, output_path: str, every: int = 1, scale: float = 1.0) -> None:
    cap = cv2.VideoCapture(input_path)
    if not cap.isOpened():
        print(f"[test_video] Cannot open: {input_path}", file=sys.stderr)
        sys.exit(1)

    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    fps_in = cap.get(cv2.CAP_PROP_FPS) or 30.0
    w_in = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h_in = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))

    w_proc = max(1, int(round(w_in * scale)))
    h_proc = max(1, int(round(h_in * scale)))

    print(f"[test_video] Input : {input_path}")
    print(f"             Size  : {w_in}x{h_in}  fps={fps_in:.2f}  frames={total_frames}")
    print(f"             Scale : {scale}  → proc {w_proc}x{h_proc}")
    print(f"             Every : {every}  (DLL sees every frame; YOLO cadence from J2K_YOLO_EVERY_N={os.environ.get('J2K_YOLO_EVERY_N')})")
    print(f"[test_video] Output: {output_path}")

    # Use the full input resolution for the output video (overlay drawn on original size).
    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    out_fps = fps_in / max(1, every)
    writer = cv2.VideoWriter(output_path, fourcc, out_fps, (w_in, h_in))

    # Initialise DLL with the processing resolution.
    os.environ.setdefault("J2K_PROCESS_SCALE", str(scale))
    try:
        worker = GCVWorker(w_in, h_in)
    except Exception as e:
        print(f"[test_video] GCVWorker init failed: {e}", file=sys.stderr)
        sys.exit(1)

    frame_i = 0
    written = 0
    t0 = time.perf_counter()

    while True:
        ret, bgr = cap.read()
        if not ret:
            break
        frame_i += 1
        if (frame_i % every) != 0:
            continue

        out_frame, gcv = worker.process(bgr)
        writer.write(out_frame)
        written += 1

        if written % 30 == 0:
            elapsed = time.perf_counter() - t0
            pct = 100.0 * frame_i / max(1, total_frames)
            fps_proc = written / max(0.001, elapsed)
            print(f"\r  frame {frame_i}/{total_frames} ({pct:.0f}%)  "
                  f"{fps_proc:.1f} frames/s written={written}   ", end="", flush=True)

    print()
    cap.release()
    writer.release()
    worker.close()

    elapsed = time.perf_counter() - t0
    print(f"[test_video] Done — {written} frames written in {elapsed:.1f}s → {output_path}")


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description="J2K DLL video test harness")
    ap.add_argument("input", help="Input video path")
    ap.add_argument("output", nargs="?", default=None, help="Output video path (default: input_annotated.mp4)")
    ap.add_argument("--every", type=int, default=1, help="Process every Nth frame (default 1)")
    ap.add_argument("--scale", type=float, default=1.0, help="Processing scale (default 1.0)")
    args = ap.parse_args()

    in_path = args.input
    if args.output:
        out_path = args.output
    else:
        base, _ = os.path.splitext(in_path)
        out_path = base + "_annotated.mp4"

    run(in_path, out_path, every=args.every, scale=args.scale)
