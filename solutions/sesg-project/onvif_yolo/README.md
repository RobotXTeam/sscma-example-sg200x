# onvif_yolo

reCamera 上的 **ONVIF + YOLO AI IPC** 演示：单进程 C++ 程序，把 reCamera 变成一台标准 ONVIF 网络摄像机，画面带 YOLO11n 实时目标检测框和类别标签。

主机端（PC / seeed）可以用任何标准 ONVIF 客户端发现设备、获取 RTSP 地址并拉流验收。

## 功能

- **ONVIF 协议（C++ 实现）**
  - WS-Discovery：UDP 组播 `239.255.255.250:3702`，让客户端自动发现设备
  - Device/Media SOAP 服务（mongoose HTTP，端口 `8080`）：`GetDeviceInformation` / `GetCapabilities` / `GetServices` / `GetProfiles` / `GetStreamUri`
- **YOLO11n 目标检测**：摄像头 → NPU 推理（COCO 80 类）→ 设备端硬件画框 + 类别/置信度文字标签
- **RTSP 推流**：H.264 1280x720，`rtsp://<device-ip>:8554/live`，检测框走硬件 RGN/OSD 叠加（不重编码）

整条 `采集 → 推理 → 画框 → 编码 → 推流 → ONVIF 发现/描述` 流水线在一个原生进程内完成，贴近商用 AI IPC 固件形态。

## 硬件 / 软件需求

- reCamera 一台（reCamera OS 0.2.3+）
- 主机交叉编译工具链 `riscv64-unknown-linux-musl-`
- SG200X SDK（`SG200X_SDK_PATH`）
- 验收端：任意 ONVIF 客户端 / `ffmpeg` / VLC（PC 需要 `python3` 跑 `onvif_client.py`）

## 模型下载

本演示使用的 `.cvimodel` 不放在 GitHub 仓库，请从 Google Drive 下载：

- Google Drive 根目录：<https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link>
- 模型路径：`/reCamera_Shared/Wiki/onvif_yolo/model/`
  - `yolo11n_detection_cv181x_int8.cvimodel` — YOLO11n 检测模型（COCO 80 类，cv181x INT8）

> 该模型也是 reCamera 系统自带模型，位于设备 `/usr/share/supervisor/models/yolo11n_detection_cv181x_int8.cvimodel`。

## 快速运行（开箱即跑，无需编译）

不想编译的话，直接从 Google Drive 拉运行包：

- 运行包：`/reCamera_Shared/Wiki/onvif_yolo/run/`（`onvif_yolo` 可执行 + `README.md`）
- 模型：`/reCamera_Shared/Wiki/onvif_yolo/model/`

把 `run/` 和 `model/` 拉到 reCamera 同一目录，按 `run/README.md` 跑即可，详见该说明。

## 编译

```bash
export SG200X_SDK_PATH=<path-to-sg2002-sdk>
export PATH=<path-to-toolchain>/bin:$PATH

cd solutions/sesg-project/onvif_yolo
rm -rf build && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

产物：`build/onvif_yolo`

## 部署与运行

```bash
# 1) 上传可执行文件和模型到 reCamera
scp build/onvif_yolo recamera@<device-ip>:/home/recamera/onvif_yolo/
scp model/yolo11n_detection_cv181x_int8.cvimodel recamera@<device-ip>:/home/recamera/onvif_yolo/model/

# 2) 在 reCamera 上停止占用摄像头的默认服务
sudo /etc/init.d/S03node-red stop
sudo /etc/init.d/S91sscma-node stop
sudo /etc/init.d/S93sscma-supervisor stop

# 3) 运行
cd /home/recamera/onvif_yolo
sudo env LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:$LD_LIBRARY_PATH \
  ./onvif_yolo ./model/yolo11n_detection_cv181x_int8.cvimodel 0.50 2 8554 live 8080
```

### 参数

| 位置 | 参数 | 说明 | 默认 |
|------|------|------|------|
| 1 | model | YOLO11n 检测 `.cvimodel` 路径 | 必填 |
| 2 | threshold | 检测置信度阈值 | 0.50 |
| 3 | skip | 每 N 帧推理 1 次 | 2 |
| 4 | rtsp_port | RTSP 端口 | 8554 |
| 5 | rtsp_session | RTSP path | live |
| 6 | onvif_port | ONVIF SOAP 端口 | 8080 |

> 关闭程序用 `Ctrl-C` 或 `kill -TERM <pid>`（会优雅释放 VPSS/RTSP/RGN）。**不要用 `kill -9`**，否则可能残留 VPSS 资源，下次启动报 `CVI_VPSS_CreateGrp failed`，需要重启设备恢复。

## 主机端验收

PC 端运行 `onvif_client.py`（纯标准库 + opencv-python）：

```bash
# 1) WS-Discovery 发现设备 + SOAP 拿设备信息和 RTSP 地址
python3 onvif_client.py --discover

# 2) 发现后直接拉流显示（带检测框）
python3 onvif_client.py --discover --play

# 3) 直连已知 IP
python3 onvif_client.py --host <device-ip> --onvif-port 8080 --play
```

也可以用任意标准工具拉流：

```bash
ffplay -rtsp_transport tcp rtsp://<device-ip>:8554/live
ffmpeg -rtsp_transport tcp -i rtsp://<device-ip>:8554/live -t 20 -c copy out.mp4
```

## 证据

完整证据图片和视频在 Google Drive：

- Google Drive 根目录：<https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link>
- 证据图片：`/reCamera_Shared/Wiki/onvif_yolo/evidence/image/`
  - `demo.gif` — 演示动图（带检测框和类别标签）
  - `frame_detection_01.png` / `frame_detection_02.png` — 关键帧
  - `onvif_acceptance.txt` — ONVIF 发现 + SOAP 验收日志
- 证据视频：`/reCamera_Shared/Wiki/onvif_yolo/evidence/video/`
  - `onvif_yolo_demo.mp4` — RTSP 拉流录制（1280x720 H.264）

## 已知问题与说明

- 本 BSP 的 RGN/OSD 硬件叠加只能 attach 到 VPSS（不能 attach 到 VENC），且 overlay 的 `u32CanvasNum` 必须 ≥1。仓库内 `components/SeSg/stream_rtsp` 已按此修正。
- ONVIF 实现为 Profile S 核心子集，覆盖发现 + 设备信息 + 视频 profile + RTSP 取流，未实现 PTZ / 鉴权 / 事件订阅。
- 安全说明：ONVIF SOAP 和 RTSP 当前**无鉴权**，仅适用于可信局域网演示环境；生产部署需加 WS-UsernameToken 和 RTSP 鉴权。
