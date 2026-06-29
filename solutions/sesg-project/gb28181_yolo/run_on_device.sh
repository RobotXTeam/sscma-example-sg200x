#!/bin/sh
# Run on reCamera. Starts YOLO engine (RTSP 8554) if needed, then the
# GB28181 client that registers to SRS and pushes PS/RTP-over-TCP media.
#
# Usage: run_on_device.sh
set -e
GBDIR=/home/recamera/gb28181_yolo
# 1) stop camera-occupying services
printf 'recamera.1\n' | sudo -S /etc/init.d/S03node-red stop 2>/dev/null || true
printf 'recamera.1\n' | sudo -S /etc/init.d/S91sscma-node stop 2>/dev/null || true
printf 'recamera.1\n' | sudo -S /etc/init.d/S93sscma-supervisor stop 2>/dev/null || true

# 2) start YOLO engine if RTSP 8554 not up
if ! (ss -tln 2>/dev/null || netstat -tln 2>/dev/null) | grep -q 8554; then
  cd "$GBDIR"
  printf 'recamera.1\n' | sudo -S env LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd \
    nohup stdbuf -oL -eL ./gb28181_engine ./model/yolo11n_detection_cv181x_int8.cvimodel \
    rtmp://127.0.0.1:1935/live/x 0.60 2 >/tmp/engine.log 2>&1 &
  sleep 8
fi

# 3) start GB28181 client
cd "$GBDIR"
export LD_LIBRARY_PATH="$GBDIR/lib:/mnt/system/lib:/usr/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd"
# args: dev_id srv_ip srv_port local_ip srv_id rtsp
exec ./gb28181_client \
  34020000001320000001 192.168.2.113 5060 192.168.2.249 34020000002000000001 \
  rtsp://127.0.0.1:8554/live
