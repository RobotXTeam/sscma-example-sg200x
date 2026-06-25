#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Receive SeSg stream_udp packets for hand_gesture (JPEG + hand results).

基于 qrcode-udp/tools/udp_receiver.py 改写，严格对齐 hand_types.h UDPHandResult。

Packet layout (little-endian), produced by `udp_service::UDPSender`:
- magic(u32)
- width(u32), height(u32), payload_size(u32), det_count(u32), fmt(u32)
- fid(u32)
- det_bytes (det_count * sizeof(UDPHandResult))
- payload bytes (JPEG if fmt==1 else RGB888)

UDPHandResult (288 bytes, #pragma pack(1)):
  float   palm_x1, palm_y1, palm_x2, palm_y2;  // palm bbox 归一化 xyxy
  float   palm_score;                          // palm 置信度
  float   handedness;                          // P(right hand)
  int32_t gesture_idx;                         // 0..7
  float   gesture_conf;                        // 该类别概率
  float   landmarks[21 * 3];                   // 21 个归一化关键点 (x,y,z) 交错
  int32_t landmark_valid;                      // 1=关键点有效，0=只有 palm 框
格式串: "<5f f i f 63f i"  (注意：palm_score 与 handedness 均为 float)

Usage:
  python3 udp_receiver.py --port 5001 --show --print-dets

Dependencies:
  pip install opencv-python numpy
"""

import argparse
import socket
import struct
import time
from typing import List, Optional, Tuple

# UDPHandResult 大小 = 288 bytes（必须与 hand_types.h static_assert 一致）
DET_SIZE = 288
DEFAULT_MAGIC = 0x48414E44  # "HAND"

# 手势类别（对齐 test_tflite_rtsp.py L68-77 / hand_types.h kGestureNames）
GESTURE_NAMES = [
    "None",          # 0
    "Closed_Fist",   # 1
    "Open_Palm",     # 2
    "Pointing_Up",   # 3
    "Thumb_Down",    # 4
    "Thumb_Up",      # 5
    "Victory",       # 6
    "ILoveYou",      # 7
]

# 手部 21 关键点连接关系（对齐 test_tflite_rtsp.py L80-87 / hand_types.h kHandConnections）
HAND_CONNECTIONS = [
    (0, 1), (1, 2), (2, 3), (3, 4),          # thumb
    (0, 5), (5, 6), (6, 7), (7, 8),          # index
    (5, 9), (9, 10), (10, 11), (11, 12),     # middle
    (9, 13), (13, 14), (14, 15), (15, 16),   # ring
    (13, 17), (17, 18), (18, 19), (19, 20),  # pinky
    (0, 17),                                 # palm base
]

# 单个 UDPHandResult 的 struct 格式（288 bytes）
#   5f(palm xyxy + score) + f(handedness) + i(gesture_idx) + f(gesture_conf) + 63f(landmarks) + i(landmark_valid)
_DET_FMT = "<5f f i f 63f i"
_DET_STRUCT = struct.Struct(_DET_FMT)
assert _DET_STRUCT.size == DET_SIZE, f"DET format size mismatch: {_DET_STRUCT.size} != {DET_SIZE}"


def parse_args():
    p = argparse.ArgumentParser(description="Receive hand_gesture frames (JPEG + hand results)")
    p.add_argument("--ip", default="0.0.0.0", help="listen ip (default: 0.0.0.0)")
    p.add_argument("--port", type=int, default=5001, help="listen port (default: 5001)")
    p.add_argument("--magic", type=lambda x: int(x, 0), default=DEFAULT_MAGIC,
                   help="magic u32 (default: 0x48414E44)")
    p.add_argument("--show", action="store_true", help="display frames (requires opencv-python)")
    p.add_argument("--print-dets", action="store_true", help="print gesture results")
    return p.parse_args()


def try_import_cv2():
    try:
        import cv2  # type: ignore
        import numpy as np  # type: ignore
        return cv2, np
    except Exception:
        return None, None


def find_magic(buf: bytearray, magic: int) -> int:
    m = struct.pack("<I", magic)
    return buf.find(m)


def parse_one_frame(buf: bytearray, magic: int) -> Tuple[Optional[dict], int]:
    """Try parse one frame from buf. Return (frame_dict_or_None, consumed_bytes)."""
    idx = find_magic(buf, magic)
    if idx < 0:
        if len(buf) > 4 * 1024 * 1024:
            return None, len(buf)
        return None, 0

    if idx > 0:
        return None, idx

    # Need at least 28 bytes for header (magic + 5*4 + fid)
    if len(buf) < 28:
        return None, 0

    magic_u32, w, h, payload_size, det_count, fmt, fid = struct.unpack_from("<7I", buf, 0)
    if magic_u32 != magic:
        return None, 4

    det_bytes_len = det_count * DET_SIZE
    total = 28 + det_bytes_len + payload_size
    if len(buf) < total:
        return None, 0

    det_bytes = bytes(buf[28:28 + det_bytes_len])
    payload = bytes(buf[28 + det_bytes_len: total])

    return {
        "magic": magic_u32,
        "w": w,
        "h": h,
        "payload_size": payload_size,
        "det_count": det_count,
        "fmt": fmt,
        "fid": fid,
        "det_bytes": det_bytes,
        "payload": payload,
    }, total


def parse_dets(det_bytes: bytes) -> List[dict]:
    """解析 det_count 个 UDPHandResult（288 bytes/个）。"""
    out: List[dict] = []
    if not det_bytes:
        return out
    n = len(det_bytes) // DET_SIZE
    for i in range(n):
        off = i * DET_SIZE
        (palm_x1, palm_y1, palm_x2, palm_y2, palm_score,
         handedness, gesture_idx, gesture_conf, *rest) = _DET_STRUCT.unpack_from(det_bytes, off)
        landmark_valid = int(rest[-1])
        # rest 前 63 个是 x,y,z 交错
        lm_flat = rest[:63]
        lm = [(lm_flat[j * 3], lm_flat[j * 3 + 1]) for j in range(21)]
        out.append({
            "palm_x1": palm_x1,
            "palm_y1": palm_y1,
            "palm_x2": palm_x2,
            "palm_y2": palm_y2,
            "palm_score": palm_score,
            "handedness": handedness,
            "gesture_idx": int(gesture_idx),
            "gesture_conf": gesture_conf,
            "landmarks": lm,
            "landmark_valid": landmark_valid,
        })
    return out


def overlay(img, dets, fid):
    """在 img 上叠加：palm 框（蓝）+ 21 关键点骨架（绿/红）+ gesture 文字（黄）。"""
    import cv2  # type: ignore

    H, W = img.shape[:2]
    for d in dets:
        # palm 框（蓝色）
        x1 = int(d["palm_x1"] * W)
        y1 = int(d["palm_y1"] * H)
        x2 = int(d["palm_x2"] * W)
        y2 = int(d["palm_y2"] * H)
        cv2.rectangle(img, (x1, y1), (x2, y2), (255, 0, 0), 2)

        # 手势文字 + handedness
        gname = GESTURE_NAMES[d["gesture_idx"]] if 0 <= d["gesture_idx"] < len(GESTURE_NAMES) else "?"
        conf = d["gesture_conf"]
        hand = "R" if d["handedness"] > 0.5 else "L"
        txt = f"{gname} ({conf * 100:.0f}%) [{hand}]"
        cv2.putText(img, txt, (x1, max(0, y1 - 10)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 255), 2)

        # 21 关键点骨架（仅 landmark_valid 时）
        if d["landmark_valid"]:
            lm = d["landmarks"]
            pts = [(int(p[0] * W), int(p[1] * H)) for p in lm]
            # 连线（绿色）
            for a, b in HAND_CONNECTIONS:
                cv2.line(img, pts[a], pts[b], (0, 255, 0), 2)
            # 关键点（红色）
            for pt in pts:
                cv2.circle(img, pt, 3, (0, 0, 255), -1)

    # 帧号
    cv2.putText(img, f"fid={fid}", (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)
    return img


def main():
    args = parse_args()
    cv2, np = try_import_cv2() if args.show else (None, None)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 50 * 1024 * 1024)
    sock.bind((args.ip, args.port))
    sock.settimeout(1.0)

    print(f"[UDP] listening on {args.ip}:{args.port}, magic=0x{args.magic:08X}, DET_SIZE={DET_SIZE}")

    buf = bytearray()
    last_fid = None
    first_pkt = True

    while True:
        try:
            data, _addr = sock.recvfrom(65535)
            if data:
                if first_pkt:
                    print(f"[UDP] first packet: {len(data)} bytes")
                    first_pkt = False
                buf.extend(data)

            # Try parse as many frames as possible
            while True:
                frame, consumed = parse_one_frame(buf, args.magic)
                if consumed > 0:
                    del buf[:consumed]
                if frame is None:
                    break

                fid = frame["fid"]
                if last_fid is not None and fid > last_fid + 1:
                    print(f"[UDP] dropped {fid - last_fid - 1} frames")
                last_fid = fid

                dets = parse_dets(frame["det_bytes"])
                if args.print_dets and dets:
                    print(f"[帧 {fid}] 手数={len(dets)}")
                    for d in dets:
                        gname = GESTURE_NAMES[d["gesture_idx"]] if 0 <= d["gesture_idx"] < len(GESTURE_NAMES) else "?"
                        hand = "R" if d["handedness"] > 0.5 else "L"
                        print(f"  - {gname} ({d['gesture_conf'] * 100:.0f}%) [{hand}] "
                              f"palm=({d['palm_x1']:.2f},{d['palm_y1']:.2f},{d['palm_x2']:.2f},{d['palm_y2']:.2f}) "
                              f"score={d['palm_score']:.2f} lm_valid={d['landmark_valid']}")

                if cv2 is not None and np is not None:
                    if frame["fmt"] == 1:
                        img = cv2.imdecode(np.frombuffer(frame["payload"], dtype=np.uint8), cv2.IMREAD_COLOR)
                    else:
                        # RGB888
                        arr = np.frombuffer(frame["payload"], dtype=np.uint8)
                        img = arr.reshape((frame["h"], frame["w"], 3))
                        img = cv2.cvtColor(img, cv2.COLOR_RGB2BGR)

                    if img is not None:
                        overlay(img, dets, fid)
                        cv2.imshow("hand_gesture", img)
                        if (cv2.waitKey(1) & 0xFF) == ord('q'):
                            raise KeyboardInterrupt

        except socket.timeout:
            continue
        except KeyboardInterrupt:
            break
        except Exception as e:
            print(f"[ERR] {e}")
            time.sleep(0.05)

    sock.close()
    if cv2 is not None:
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
