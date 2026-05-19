import os
import sys
from pathlib import Path

import cv2

from run_cpp import GCVWorker


def _u16be(buf: bytes, off: int) -> int:
    return int.from_bytes(buf[off : off + 2], "big")


def _box_present(buf: bytes, off: int) -> bool:
    x1 = _u16be(buf, off + 0)
    y1 = _u16be(buf, off + 2)
    x2 = _u16be(buf, off + 4)
    y2 = _u16be(buf, off + 6)
    return x2 > x1 and y2 > y1


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: python test_cpp_video_detection.py <video_path>")
        return 2
    video_path = Path(sys.argv[1])
    if not video_path.is_file():
        print(f"[ERR] missing video: {video_path}")
        return 2

    os.environ["J2K_NO_AUTO_UI"] = "1"
    os.environ["J2K_DET_DEBUG"] = "1"

    cap = cv2.VideoCapture(str(video_path))
    if not cap.isOpened():
        print(f"[ERR] cannot open video: {video_path}")
        return 2

    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH) or 1920)
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT) or 1080)
    worker = GCVWorker(w, h)

    frames = 0
    player_seen = 0
    ball_seen = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        frames += 1
        _, gcv = worker.process(frame)
        if len(gcv) >= 40:
            if _box_present(gcv, 12):
                player_seen += 1
            if _box_present(gcv, 24):
                ball_seen += 1

    worker.close()
    cap.release()

    print(f"[RESULT] frames={frames} player_box_frames={player_seen} ball_box_frames={ball_seen}")
    print("[NOTE] stamina class is reported via [J2K DET] debug lines from C++ stderr.")
    if frames <= 0:
        return 1
    if player_seen <= 0 or ball_seen <= 0:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
