#!/bin/sh
# run_rtmp.sh - 在 reCamera 上一键启动 RTMP AI IPC
#
# 做两件事：
#   1) 启动 rtmp_yolo：摄像头 -> YOLO11n 检测 -> 设备端画框 -> H.264 -> 本地 RTSP(127.0.0.1:8554/live)
#   2) 用设备自带 ffmpeg 把本地 RTSP 转封装(-c copy)推到 RTMP 服务器，断线自动重连
#
# 用法：
#   sudo ./run_rtmp.sh <rtmp_url> [threshold] [skip]
# 示例：
#   sudo ./run_rtmp.sh rtmp://192.168.2.113:1935/live/recamera 0.50 2

RTMP_URL="${1:-rtmp://127.0.0.1:1935/live/recamera}"
THRESHOLD="${2:-0.50}"
SKIP="${3:-2}"

DIR="$(cd "$(dirname "$0")" && pwd)"
MODEL="$DIR/model/yolo11n_detection_cv181x_int8.cvimodel"
export LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:$LD_LIBRARY_PATH

cleanup() {
    echo "[run_rtmp] stopping..."
    [ -n "$FF_PID" ] && kill -TERM "$FF_PID" 2>/dev/null
    [ -n "$YOLO_PID" ] && kill -TERM "$YOLO_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup INT TERM EXIT

echo "[run_rtmp] starting detection + RTSP engine..."
"$DIR/rtmp_yolo" "$MODEL" "$RTMP_URL" "$THRESHOLD" "$SKIP" &
YOLO_PID=$!

# 等 RTSP 起来并产出关键帧
sleep 5

# relay 循环：断线自动重连，直到 detection 进程退出
# 注意：ffmpeg 必须用 env -u LD_LIBRARY_PATH 清掉本脚本设的库路径，
# 否则会加载到 /mnt/system 里与系统 ffmpeg 不兼容的 libavfilter.so.7（symbol not found）。
echo "[run_rtmp] starting ffmpeg RTSP->RTMP relay -> $RTMP_URL"
(
  while kill -0 "$YOLO_PID" 2>/dev/null; do
    env -u LD_LIBRARY_PATH ffmpeg -rtsp_transport tcp -i rtsp://127.0.0.1:8554/live -c copy -f flv "$RTMP_URL"
    echo "[run_rtmp] relay exited, retry in 2s..."
    sleep 2
  done
) &
FF_PID=$!

wait "$YOLO_PID"

