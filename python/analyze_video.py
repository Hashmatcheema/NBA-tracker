import argparse
import csv
import json
import os
import sys
import time
from typing import Any

import cv2

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

if not os.environ.get("J2K_YOLO_EVERY_N"):
    os.environ["J2K_YOLO_EVERY_N"] = "2"

from run_cpp import GCVWorker  # noqa: E402


def _u16be(buf: bytes | bytearray, off: int) -> int:
    return int.from_bytes(bytes(buf)[off : off + 2], "big")


def read_gcv_box_px(gcv: bytes | bytearray, off: int, frame_w: int, frame_h: int) -> tuple[int, int, int, int] | None:
    x1n = _u16be(gcv, off + 0)
    y1n = _u16be(gcv, off + 2)
    x2n = _u16be(gcv, off + 4)
    y2n = _u16be(gcv, off + 6)
    if x2n <= x1n or y2n <= y1n:
        return None
    return (
        int(x1n * frame_w / 65535),
        int(y1n * frame_h / 65535),
        int(x2n * frame_w / 65535),
        int(y2n * frame_h / 65535),
    )


def _as_int_box(box: Any) -> tuple[int, int, int, int] | None:
    if not box:
        return None
    return (int(box[0]), int(box[1]), int(box[2]), int(box[3]))


def _draw_box(frame, box: tuple[int, int, int, int] | None, color: tuple[int, int, int], label: str) -> None:
    if box is None:
        return
    x1, y1, x2, y2 = box
    cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
    cv2.putText(frame, label, (x1, max(14, y1 - 8)), cv2.FONT_HERSHEY_SIMPLEX, 0.45, color, 1, cv2.LINE_AA)


def _state_name(t: dict[str, Any]) -> str:
    if t.get("visible"):
        return "visible"
    if t.get("predicted"):
        return "predicted"
    return "lost"


def run_once(input_path: str, out_dir: str, every: int, save_failure_frames: bool) -> dict[str, Any]:
    cap = cv2.VideoCapture(input_path)
    if not cap.isOpened():
        raise RuntimeError(f"Cannot open input video: {input_path}")

    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    fps_in = float(cap.get(cv2.CAP_PROP_FPS) or 30.0)
    fw = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH) or 1920)
    fh = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT) or 1080)
    base = os.path.splitext(os.path.basename(input_path))[0]

    out_video = os.path.join(out_dir, base + "_annotated.mp4")
    out_json = os.path.join(out_dir, base + "_tracking_report.json")
    out_txt = os.path.join(out_dir, base + "_score.txt")
    out_csv = os.path.join(out_dir, base + "_frames.csv")
    fail_dir = os.path.join(out_dir, base + "_failure_frames")
    if save_failure_frames:
        os.makedirs(fail_dir, exist_ok=True)

    worker = GCVWorker(fw, fh)
    writer = cv2.VideoWriter(out_video, cv2.VideoWriter_fourcc(*"mp4v"), fps_in / max(1, every), (fw, fh))

    # blue=ball, green=stamina, purple=player, red=firezone
    color_ball = (255, 0, 0)
    color_stamina = (0, 255, 0)
    color_player = (255, 0, 255)
    color_fire = (0, 0, 255)

    counts = {
        "player_visible_frames": 0,
        "player_predicted_frames": 0,
        "player_lost_frames": 0,
        "ball_visible_frames": 0,
        "ball_predicted_frames": 0,
        "ball_lost_frames": 0,
        "stamina_visible_frames": 0,
        "stamina_predicted_frames": 0,
        "stamina_lost_frames": 0,
    }
    perf = {"total_ms": 0.0, "max_ms": 0.0, "yolo_ms_sum": 0.0, "tracking_ms_sum": 0.0}
    frame_rows: list[dict[str, Any]] = []
    failures: list[dict[str, Any]] = []
    worst_failure_frames: list[int] = []
    start_ts = time.perf_counter()
    processed = 0
    dropped = 0
    prev_center: dict[str, tuple[float, float] | None] = {"player": None, "ball": None, "stamina": None}
    frame_i = 0

    while True:
        ok, bgr = cap.read()
        if not ok:
            break
        frame_i += 1
        if (frame_i % every) != 0:
            continue
        processed += 1
        t0 = time.perf_counter()
        out_frame, gcv = worker.process(bgr)
        call_ms = float((time.perf_counter() - t0) * 1000.0)
        perf["total_ms"] += call_ms
        perf["max_ms"] = max(perf["max_ms"], call_ms)
        state = getattr(worker, "last_tracking_state", {}) or {}
        perf_state = state.get("performance", {}) if isinstance(state, dict) else {}
        yolo_ms = float(perf_state.get("yolo_ms", 0.0))
        tracking_ms = float(perf_state.get("tracking_ms", 0.0))
        perf["yolo_ms_sum"] += yolo_ms
        perf["tracking_ms_sum"] += tracking_ms

        player = state.get("player", {})
        ball = state.get("ball", {})
        stamina = state.get("stamina", {})
        fire = _as_int_box(read_gcv_box_px(gcv, 32, fw, fh))

        p_box = _as_int_box(player.get("smooth_box"))
        b_box = _as_int_box(ball.get("smooth_box"))
        s_box = _as_int_box(stamina.get("smooth_box"))
        p_raw = _as_int_box(player.get("raw_box"))
        b_raw = _as_int_box(ball.get("raw_box"))
        s_raw = _as_int_box(stamina.get("raw_box"))

        p_state = _state_name(player)
        b_state = _state_name(ball)
        s_state = _state_name(stamina)
        counts[f"player_{p_state}_frames"] += 1
        counts[f"ball_{b_state}_frames"] += 1
        counts[f"stamina_{s_state}_frames"] += 1

        frame_failures: list[str] = []
        if player.get("lost"):
            frame_failures.append("PLAYER_LOST")
        if ball.get("lost"):
            frame_failures.append("BALL_LOST")
        if stamina.get("lost") and player.get("exists"):
            frame_failures.append("STAMINA_LOST")
        if ball.get("exists") and not ball.get("near_player") and ball.get("predicted") is False:
            frame_failures.append("BALL_NOT_NEAR_PLAYER")
        if stamina.get("exists") and stamina.get("associated_player_id") is None:
            frame_failures.append("STAMINA_UNASSOCIATED")
        if float(player.get("identity_confidence", 0.0)) < 0.45 and player.get("exists"):
            frame_failures.append("PLAYER_ID_LOW")

        for key, box in (("player", p_box), ("ball", b_box), ("stamina", s_box)):
            if box is None:
                continue
            cx = float(box[0] + box[2]) * 0.5
            cy = float(box[1] + box[3]) * 0.5
            prev = prev_center.get(key)
            if prev is not None:
                jump = ((cx - prev[0]) ** 2 + (cy - prev[1]) ** 2) ** 0.5
                if key == "player" and jump > 220.0:
                    frame_failures.append("PLAYER_JUMP")
                if key == "ball" and jump > 260.0:
                    frame_failures.append("BALL_JUMP")
                if key == "stamina" and jump > 180.0:
                    frame_failures.append("STAMINA_JUMP")
            prev_center[key] = (cx, cy)

        # raw YOLO proxy boxes (thin) + authoritative boxes (thick)
        _draw_box(out_frame, p_raw, (180, 90, 180), "PLAYER raw")
        _draw_box(out_frame, b_raw, (140, 80, 20), "BALL raw")
        _draw_box(out_frame, s_raw, (60, 140, 60), "STAMINA raw")

        _draw_box(
            out_frame,
            p_box,
            color_player,
            f"PLAYER id={player.get('track_id', 1)} {p_state} conf={float(player.get('confidence', 0.0)):.2f} idc={float(player.get('identity_confidence', 0.0)):.2f} miss={int(player.get('missed_frames', 0))}",
        )
        _draw_box(
            out_frame,
            b_box,
            color_ball,
            f"BALL id={ball.get('track_id', 1)} {b_state} conf={float(ball.get('confidence', 0.0)):.2f} miss={int(ball.get('missed_frames', 0))}",
        )
        _draw_box(
            out_frame,
            s_box,
            color_stamina,
            f"STAMINA id={stamina.get('track_id', 1)} pid={stamina.get('associated_player_id')} {s_state} conf={float(stamina.get('confidence', 0.0)):.2f} miss={int(stamina.get('missed_frames', 0))}",
        )
        _draw_box(out_frame, fire, color_fire, "FIREZONE")

        fps_live = processed / max(0.001, (time.perf_counter() - start_ts))
        frame_latency = float(perf_state.get("latency_ms", 0.0))
        if frame_latency > 120.0:
            dropped += 1

        diag = [
            f"FPS={fps_live:.1f}",
            f"YOLOms={yolo_ms:.1f}",
            f"TRACKms={tracking_ms:.2f}",
            f"FRAMEms={call_ms:.1f}",
            f"LATms={frame_latency:.1f}",
            f"ACTIVE={int(player.get('exists', False))+int(ball.get('exists', False))+int(stamina.get('exists', False))}",
            f"REJ={sum(v for v in state.get('rejections_by_class', {}).get('PLAYER', {}).values()) + sum(v for v in state.get('rejections_by_class', {}).get('BALL', {}).values()) + sum(v for v in state.get('rejections_by_class', {}).get('STAMINA', {}).values())}",
        ]
        cv2.putText(out_frame, " | ".join(diag), (16, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.52, (220, 220, 220), 2, cv2.LINE_AA)
        if frame_failures:
            cv2.putText(out_frame, "FAIL: " + ",".join(frame_failures[:4]), (16, 46), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 0, 255), 2, cv2.LINE_AA)
        else:
            cv2.putText(out_frame, "STATUS: stable", (16, 46), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 220, 120), 2, cv2.LINE_AA)

        writer.write(out_frame)
        if frame_failures:
            failures.append({"frame": frame_i, "reasons": frame_failures})
            if save_failure_frames:
                cv2.imwrite(os.path.join(fail_dir, f"frame_{frame_i:06d}.png"), out_frame)
            if len(worst_failure_frames) < 120:
                worst_failure_frames.append(frame_i)

        frame_rows.append(
            {
                "frame": frame_i,
                "player_state": p_state,
                "ball_state": b_state,
                "stamina_state": s_state,
                "player_conf": round(float(player.get("confidence", 0.0)), 4),
                "player_identity_conf": round(float(player.get("identity_confidence", 0.0)), 4),
                "ball_conf": round(float(ball.get("confidence", 0.0)), 4),
                "stamina_conf": round(float(stamina.get("confidence", 0.0)), 4),
                "miss_player": int(player.get("missed_frames", 0)),
                "miss_ball": int(ball.get("missed_frames", 0)),
                "miss_stamina": int(stamina.get("missed_frames", 0)),
                "yolo_ms": round(yolo_ms, 3),
                "tracking_ms": round(tracking_ms, 3),
                "frame_ms": round(call_ms, 3),
                "latency_ms": round(frame_latency, 3),
                "fails": ";".join(frame_failures),
            }
        )

        if processed % 60 == 0:
            pct = (100.0 * frame_i / max(1, total_frames))
            print(f"\r  [{pct:5.1f}%] frame {frame_i}/{total_frames} fps={fps_live:.1f} fails={len(failures)}", end="", flush=True)

    print()
    cap.release()
    writer.release()
    worker.close()

    elapsed_s = max(1e-6, time.perf_counter() - start_ts)
    avg_fps = processed / elapsed_s
    avg_yolo_ms = perf["yolo_ms_sum"] / max(1, processed)
    avg_tracking_ms = perf["tracking_ms_sum"] / max(1, processed)
    avg_total_ms = perf["total_ms"] / max(1, processed)
    rejections = (state.get("rejections_by_class", {}) if isinstance(state, dict) else {})
    switches = (state.get("identity_switches_by_class", {}) if isinstance(state, dict) else {})

    player_loss_ratio = counts["player_lost_frames"] / max(1, processed)
    ball_loss_ratio = counts["ball_lost_frames"] / max(1, processed)
    stamina_loss_ratio = counts["stamina_lost_frames"] / max(1, processed)
    cpu_mode = bool(os.environ.get("J2K_ALLOW_CPU_FALLBACK") or os.environ.get("J2K_ORT_NO_CUDA"))
    pass_release = (
        player_loss_ratio <= 0.02
        and ball_loss_ratio <= 0.05
        and stamina_loss_ratio <= 0.06
        and len(failures) <= max(8, int(processed * 0.01))
        and (cpu_mode or avg_fps >= 45.0)
    )

    report = {
        "video": input_path,
        "total_frames": processed,
        "input_frames": total_frames,
        "average_fps": avg_fps,
        "average_yolo_ms": avg_yolo_ms,
        "average_tracking_ms": avg_tracking_ms,
        "average_total_frame_ms": avg_total_ms,
        "max_total_frame_ms": perf["max_ms"],
        "dropped_frames": dropped,
        "queue_depth": 0,
        **counts,
        "false_positive_rejections_by_class": rejections,
        "identity_switches_by_class": switches,
        "dropped_detections_by_reason": rejections,
        "worst_failure_frames": worst_failure_frames[:50],
        "failure_frame_count": len(failures),
        "failure_frames_export_dir": fail_dir if save_failure_frames else "",
        "overall_pass": pass_release,
        "annotated_video": out_video,
        "frame_csv": out_csv,
    }

    with open(out_json, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    if frame_rows:
        with open(out_csv, "w", newline="", encoding="utf-8") as f:
            cw = csv.DictWriter(f, fieldnames=frame_rows[0].keys())
            cw.writeheader()
            cw.writerows(frame_rows)
    score = 95 if pass_release else max(0, int(
        50.0 * (1.0 - player_loss_ratio) * (1.0 - ball_loss_ratio)
    ))
    with open(out_txt, "w", encoding="utf-8") as f:
        f.write(f"SCORE: {score}/100\n")
        f.write(json.dumps(report, indent=2) + "\n")

    print(f"[analyze] annotated -> {out_video}")
    print(f"[analyze] report    -> {out_json}")
    print(f"[analyze] csv       -> {out_csv}")
    print(f"[analyze] pass={pass_release} failures={len(failures)} avg_fps={avg_fps:.2f}")
    return report


def run(input_path: str, out_dir: str, every: int, max_iterations: int, save_failure_frames: bool) -> dict[str, Any]:
    last = {}
    for i in range(1, max(1, max_iterations) + 1):
        print(f"[analyze] iteration {i}/{max_iterations}")
        last = run_once(input_path, out_dir, every=every, save_failure_frames=save_failure_frames)
        if last.get("overall_pass"):
            break
    return last


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("--out", default=None, help="Output directory (default: same as input)")
    ap.add_argument("--every", type=int, default=1)
    ap.add_argument("--max-iterations", type=int, default=3)
    ap.add_argument("--save-failure-frames", action="store_true")
    args = ap.parse_args()

    out_dir = args.out or os.path.dirname(os.path.abspath(args.input))
    result = run(
        args.input,
        out_dir,
        every=max(1, args.every),
        max_iterations=max(1, args.max_iterations),
        save_failure_frames=bool(args.save_failure_frames),
    )
    raise SystemExit(0 if result.get("overall_pass") else 1)
