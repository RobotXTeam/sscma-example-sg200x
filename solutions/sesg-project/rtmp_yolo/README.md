# rtmp_yolo

reCamera 上的 **RTMP + YOLO AI IPC** 演示：摄像头经 YOLO11n 实时目标检测、设备端硬件画框后，把带检测框的 H.264 通过 RTMP 推送到流媒体服务器（如 SRS / nginx-rtmp），PC 端用任意 RTMP/HTTP-FLV 播放器或 `rtmp_receiver.py` 验收。

## 架构

```
reCamera (rtmp_yolo)                                seeed / 流媒体服务器
┌──────────────────────────────────────┐
│ 相机 CH0 RGB888 ─DMA→ YOLO11n (NPU)   │
│                         ↓ 检测框        │
│ 相机 CH2 H264 ← RGN/OSD 硬件画框        │
│   ↓ 本地 RTSP 127.0.0.1:8554/live      │
│ ffmpeg -c copy (不重编码)               │   RTMP    ┌─────────────┐
│   └────────────────────────────────────┼─────────→ │ SRS :1935   │
└──────────────────────────────────────┘           │ HTTP-FLV    │
                                                     │  :8080      │
                              PC: ffplay / rtmp_receiver.py ◄───────┘
```

关键点：检测框由设备端 RGN/OSD **硬件叠加**烧进 H.264 码流，RTMP 推流用 `-c copy` 不重编码，零画质损失、低 CPU。

> 设计说明：reCamera 运行时的 libavformat 为裁剪版（FLV/AVCC 序列头不完整），不适合在进程内直接推 RTMP；改用设备自带的 `ffmpeg` 可执行文件做 RTSP→RTMP 转封装，这也是工业 IPC 接入 CDN/流媒体的常见做法。`run_rtmp.sh` 把检测引擎和 ffmpeg relay 封装成一键启动。

## 硬件 / 软件需求

- reCamera 一台（reCamera OS 0.2.3+，自带 `ffmpeg`）
- 主机交叉编译工具链 `riscv64-unknown-linux-musl-` + SG200X SDK
- 流媒体服务器：SRS / nginx-rtmp / ZLMediaKit 等（验收端）

## 模型下载

模型不放 GitHub，从 Google Drive 下载：

- Google Drive 根目录：<https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link>
- 模型路径：`/reCamera_Shared/Wiki/rtmp_yolo/model/`
  - `yolo11n_detection_cv181x_int8.cvimodel` — YOLO11n 检测（COCO 80 类，cv181x INT8，设备自带）

## 快速运行（开箱即跑，无需编译）

从 Google Drive 拉运行包：

- 运行包：`/reCamera_Shared/Wiki/rtmp_yolo/run/`（`rtmp_yolo` 可执行 + `run_rtmp.sh` + `README.md`）
- 模型：`/reCamera_Shared/Wiki/rtmp_yolo/model/`

把 `run/` 和 `model/` 拉到 reCamera 同一目录，按 `run/README.md` 跑即可。

## 编译

```bash
export SG200X_SDK_PATH=<path-to-sg2002-sdk>
export PATH=<path-to-toolchain>/bin:$PATH

cd solutions/sesg-project/rtmp_yolo
rm -rf build && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

产物：`build/rtmp_yolo`

## 部署与运行

```bash
# 1) 上传可执行文件、模型、启动脚本到 reCamera
scp build/rtmp_yolo run_rtmp.sh recamera@<device-ip>:/home/recamera/rtmp_yolo/
scp model/yolo11n_detection_cv181x_int8.cvimodel recamera@<device-ip>:/home/recamera/rtmp_yolo/model/

# 2) 停止占用摄像头的默认服务
sudo /etc/init.d/S03node-red stop
sudo /etc/init.d/S91sscma-node stop
sudo /etc/init.d/S93sscma-supervisor stop

# 3) 一键启动（检测引擎 + RTMP relay）
cd /home/recamera/rtmp_yolo
chmod +x rtmp_yolo run_rtmp.sh
sudo ./run_rtmp.sh rtmp://<srs-ip>:1935/live/recamera 0.60 2
```

### run_rtmp.sh 参数

| 位置 | 参数 | 说明 | 默认 |
|------|------|------|------|
| 1 | rtmp_url | 目标 RTMP 地址 | rtmp://127.0.0.1:1935/live/recamera |
| 2 | threshold | 检测置信度阈值 | 0.60 |
| 3 | skip | 每 N 帧推理 1 次 | 2 |

> 关闭用 `Ctrl-C` / `kill -TERM`，会优雅停止 ffmpeg relay 和检测引擎并释放 VPSS。**不要 `kill -9`**（残留 VPSS 需 reboot）。

## 搭建 SRS 流媒体服务器（验收端示例）

```bash
docker run -d --name srs --network host ossrs/srs:5 \
  ./objs/srs -c conf/srs.conf
# RTMP 1935 / HTTP-FLV 8080 / API 1985
```

## PC 端验收

```bash
# 方式 1：通用播放器
ffplay rtmp://<srs-ip>:1935/live/recamera
ffplay http://<srs-ip>:8080/live/recamera.flv      # HTTP-FLV

# 方式 2：本仓库接收脚本（可录制）
python3 rtmp_receiver.py --url rtmp://<srs-ip>:1935/live/recamera
python3 rtmp_receiver.py --url http://<srs-ip>:8080/live/recamera.flv --record out.mp4 --seconds 15
```

查看 SRS 是否收到流：

```bash
curl http://<srs-ip>:1985/api/v1/streams/
```

## 证据

- Google Drive 根目录：<https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link>
- 证据图片：`/reCamera_Shared/Wiki/rtmp_yolo/evidence/image/`
  - `demo.gif` — 演示动图（带检测框，夜间场景已提亮）
  - `frame_detection_01.png` / `frame_detection_bright.png` — 关键帧
  - `rtmp_acceptance.txt` / `rtmp_srs_api.txt` — RTMP 验收日志与 SRS API 状态
- 证据视频：`/reCamera_Shared/Wiki/rtmp_yolo/evidence/video/rtmp_yolo_demo.mp4`

## 安全说明

RTMP 默认无鉴权，演示环境用。生产部署请用带 token/鉴权的推流地址（SRS 支持 `on_publish` HTTP 回调鉴权）并限制服务监听范围。
