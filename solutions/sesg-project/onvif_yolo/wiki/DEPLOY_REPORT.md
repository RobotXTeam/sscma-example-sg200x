# ONVIF + YOLO AI IPC 部署报告

- **Demo 名称**：onvif_yolo
- **日期**：2026-06-11
- **设备**：reCamera（SG2002 / cv181x，RISC-V，reCamera OS 0.2.4，Buildroot 2021.05，Linux 5.10.4）
- **目标**：用 C++ 在 reCamera 上实现 ONVIF 协议 + YOLO 目标检测的 AI IPC，单进程同时做 ONVIF 发现/描述 + 摄像头→YOLO→H.264→RTSP 推流；seeed 主机用标准 ONVIF/RTSP 工具验收。

## 1. 结论

全流程验收通过。reCamera 表现为一台标准 ONVIF IP 摄像机：

- WS-Discovery 可被发现，SOAP 返回设备信息和 RTSP 地址
- RTSP 流 1280x720 H.264 @ ~26.7 FPS
- YOLO11n COCO 80 类检测，~38 ms/帧，设备端硬件画框 + 类别/置信度文字标签
- seeed 用 `onvif_client.py`（WS-Discovery + SOAP）和 `ffmpeg` 拉流录制成功

## 2. 架构

单进程 C++，复用 `sscma-example-sg200x` 现有能力：

```
reCamera (单进程 onvif_yolo)
├── 相机 CH0: RGB888 640x640 物理地址 ──DMA──> YOLO11n (CVI NPU) ──> 检测结果
│                                                          │
│                                          归一化坐标 + 类别 + 文字标签
│                                                          ▼
├── 相机 CH2: H.264 1280x720 ──> RGN/OSD 硬件叠加检测框 ──> RTSP server (8554)
│
└── ONVIF 服务 (后台线程)
    ├── WS-Discovery: UDP 组播 239.255.255.250:3702
    └── Device/Media SOAP: mongoose HTTP :8080
```

- CH0 与 CH2 同属 VPSS grp0、同一 VI 源、同视场，各自缩放到不同尺寸；归一化坐标在两个通道间通用。
- 检测框 + 文字标签通过 `SesgRtspVpssStreamer::updateOverlayRects` 走设备端 RGN/OSD 硬件叠加，不做软件重编码。

## 3. 复用与新增

| 模块 | 来源 |
|------|------|
| 相机 + VPSS + VENC + RTSP 封装 | 复用 `components/SeSg/stream_rtsp`（`SesgRtspVpssStreamer`） |
| YOLO 检测后处理 | 复用 SSCMA `ma::model::Detector`（multi-output） |
| 主循环骨架 | 参考 `solutions/sesg-project/face_rtsp/main/main.cpp` |
| HTTP 服务 | 复用 `components/mongoose`（7.17） |
| **ONVIF WS-Discovery + SOAP** | **新增 `main/onvif_service.{h,cpp}`（POSIX UDP + mongoose）** |
| **YOLO 通用检测 + 文字标签渲染** | **新增 `main/main.cpp` + `main/font5x7.h`（5x7 点阵字库）+ `main/coco_labels.h`** |

## 4. 实测数据

- 视频流：1280x720，H.264 Constrained Baseline，~26.7 FPS
- YOLO11n 推理：~38 ms/帧（输入 640x640，INT8，cv181x NPU）
- 跳帧：skip=2（每 2 帧推理 1 次）
- 检测：办公室场景稳定检测 chair / tv / person 等，阈值 0.60

ONVIF 验收日志（seeed → 设备）：

```
[discover] 发现设备 192.168.2.249 -> http://192.168.2.249:8080/onvif/device_service
[onvif] 设备信息: Manufacturer=Seeed Studio Model=reCamera FirmwareVersion=1.0-onvif-yolo
        SerialNumber=RECAMERA-0001 HardwareId=SG2002
[onvif] RTSP Stream URI = rtsp://192.168.2.249:8554/live
```

## 5. 关键问题与修复

调试中发现 vendored `components/SeSg/stream_rtsp` 的 RGN/OSD 硬件叠加在本 BSP 上有两个真实 bug（影响任何用 overlay 的 demo），已修复：

1. **`u32CanvasNum=0` 导致 RGN 创建失败**
   - 现象：内核 `rgn_create:1217(): invalid u32CanvasNum(0)`，overlay 自动禁用，画面无框。
   - 修复：`attr.unAttr.stOverlay.u32CanvasNum = 2;`（双缓冲）。

2. **RGN attach 到 VENC 通道失败**
   - 现象：内核 `rgn_attach_to_chn:1526(): rgn can only be attached to vpss or vo.`
   - 根因：原代码 attach 到 `CVI_ID_VENC`；本 BSP 只允许 attach 到 VPSS/VO。
   - 修复：改为 attach 到 `CVI_ID_VPSS`（VencChn N 对应 VpssGrp0/VpssChn N）。

3. **C++17 标准未传递到组件静态库**
   - 现象：`is_same_v` / `if constexpr` 报错（gcc 10.2 默认 gnu++14）。
   - 修复：项目顶层 CMakeLists 显式 `set(CMAKE_CXX_FLAGS "... -std=gnu++17")`。

4. **检测调参**
   - 阈值 0.60 误检多（杂物误判 tv/chair）→ 提到 0.60，每帧框数从 5-7 降到真实的 1-2 个。
   - 补充 5x7 点阵文字标签（类别名 + 置信度），原 face_rtsp 仅有人脸 7 段数码管年龄。

## 6. 运维注意

- 退出用 `Ctrl-C` 或 `kill -TERM`，会优雅释放 VPSS/RTSP/RGN。
- `kill -9` 会残留 VPSS 资源，下次启动报 `CVI_VPSS_CreateGrp failed 0xc0068004`，需 `reboot` 恢复。
- 运行前必须停止 `S03node-red` / `S91sscma-node` / `S93sscma-supervisor`，否则摄像头被占用。
- 设备 IP：演示时设备自动选用有线 LAN IP（`192.168.2.249`）；程序通过 `ip addr` 自动探测并回填到 ONVIF XAddr / Stream URI，也可用环境变量 `DEVICE_IP` 覆盖。

## 7. 真实环境路径（仅内部）

- seeed 仓库：`/home/steven/sscma-example-sg200x/solutions/sesg-project/onvif_yolo`
- 工具链：`/home/seeed/zsz/TOOL/riscv64-linux-musl-x86_64/bin`
- SDK：`/home/seeed/桌面/sg2002_recamera_emmc`
- 设备运行目录：`/home/recamera/onvif_yolo`
- 设备自带模型：`/usr/share/supervisor/models/yolo11n_detection_cv181x_int8.cvimodel`

## 8. 云端资产

- Google Drive 根目录：https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link
- 模型：`/reCamera_Shared/Wiki/onvif_yolo/model/yolo11n_detection_cv181x_int8.cvimodel`
- 证据图片：`/reCamera_Shared/Wiki/onvif_yolo/evidence/image/`（demo.gif, frame_detection_01/02.png, onvif_acceptance.txt）
- 证据视频：`/reCamera_Shared/Wiki/onvif_yolo/evidence/video/onvif_yolo_demo.mp4`
