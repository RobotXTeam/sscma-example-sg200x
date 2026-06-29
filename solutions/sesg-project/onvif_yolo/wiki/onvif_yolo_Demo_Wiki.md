---
title: 使用 reCamera 的 ONVIF AI 网络摄像机演示
description: 本文档介绍了使用 reCamera 的基于 AI 的 ONVIF 网络摄像机演示，展示了 ONVIF 协议（设备发现 + RTSP 取流）与 YOLO11n 实时目标检测在单个 C++ 程序中的集成。
keywords:
  - ONVIF
  - YOLO
  - RTSP
  - AI IPC
  - reCamera
  - AI Edge Vision
  - Object Detection
slug: /recamera_onvif_yolo
sku: 102991897,102991896
image: https://files.seeedstudio.com/wiki/reCamera/onvif_yolo/demo.gif
sidebar_position: 30
last_update:
  date: 2026-06-11T00:00:00.000Z
  author: Steven
createdAt: '2026-06-11'
updatedAt: '2026-06-11'
url: https://wiki.seeedstudio.com/cn/recamera_onvif_yolo/
---

# 使用 reCamera 的 ONVIF AI 网络摄像机演示

## 简介

ONVIF 是网络摄像机行业的开放标准，几乎所有 NVR（网络录像机）、VMS（视频管理软件）和监控平台都支持通过 ONVIF 自动发现摄像机并拉取视频流。本演示把 reCamera 变成一台**标准 ONVIF AI 网络摄像机（IP Camera）**：在标准的设备发现和 RTSP 取流之上，叠加了 YOLO11n 实时目标检测，视频画面直接带有检测框和类别标签。

整个流水线——摄像头采集、NPU 推理、画框、H.264 编码、RTSP 推流、ONVIF 发现与描述——全部在 reCamera 上的**单个 C++ 程序**内完成，贴近真正的商用 AI IPC 固件形态。

本项目提供了一个开箱即用的演示，专注于以下应用功能：

- **ONVIF 设备发现**：通过 WS-Discovery（UDP 组播），局域网内的 ONVIF 客户端能自动发现 reCamera。
- **ONVIF SOAP 服务**：提供 `GetDeviceInformation`、`GetCapabilities`、`GetProfiles`、`GetStreamUri` 等核心接口，客户端据此获取设备信息和 RTSP 地址。
- **AI 目标检测 RTSP 流**：摄像头画面经 YOLO11n（COCO 80 类）检测，检测框和类别/置信度标签通过硬件叠加到 H.264 视频流上。

<div align="center"><img width={600} src="https://files.seeedstudio.com/wiki/reCamera/onvif_yolo/demo.gif" /></div>

## 硬件准备

要运行此演示，只需要**一台 reCamera 设备**。支持所有 reCamera 变体。

您可以根据部署需求选择**任何版本的 reCamera**：

- reCamera 2002 系列（Wi-Fi）
- reCamera Gimbal（云台）
- reCamera HQ PoE（以太网 + PoE）

<table align="center">
 <tr>
  <th>reCamera 2002 系列</th>
  <th>reCamera Gimbal</th>
  <th>reCamera HQ PoE</th>
 </tr>
 <tr>
  <td>
    <div style={{textAlign:'center'}}>
      <img src="https://files.seeedstudio.com/wiki/reCamera/recamera_banner.png" style={{width:300, height:'auto'}}/>
    </div>
  </td>
  <td>
    <div style={{textAlign:'center'}}>
      <img src="https://files.seeedstudio.com/wiki/reCamera/Gimbal/reCamera-Gimbal.png" style={{width:300, height:'auto'}}/>
    </div>
  </td>
  <td>
    <div style={{textAlign:'center'}}>
      <img src="https://files.seeedstudio.com/wiki/reCamera/reCamera_hq_poe/1-100029708-reCamera-2002-HQ-PoE-8GB.jpg" style={{width:300, height:'auto'}}/>
    </div>
  </td>
 </tr>
 <tr>
  <td>
    <div class="get_one_now_container" style={{textAlign: 'center'}}>
      <a class="get_one_now_item" href="https://www.seeedstudio.com/reCamera-2002w-8GB-p-6250.html" target="_blank">
        <strong><span><font color={'FFFFFF'} size={"4"}> 立即购买 </font></span></strong>
      </a>
    </div>
  </td>
  <td>
    <div class="get_one_now_container" style={{textAlign: 'center'}}>
      <a class="get_one_now_item" href="https://www.seeedstudio.com/reCamera-gimbal-2002w-optional-accessories.html" target="_blank" rel="noopener noreferrer">
        <strong><span><font color={'FFFFFF'} size={"4"}> 立即购买 </font></span></strong>
      </a>
    </div>
  </td>
  <td>
    <div class="get_one_now_container" style={{textAlign: 'center'}}>
      <a class="get_one_now_item" href="https://www.seeedstudio.com/reCamera-2002-HQ-PoE-64GB-p-6557.html" target="_blank" rel="noopener noreferrer">
        <strong><span><font color={'FFFFFF'} size={"4"}> 立即购买 </font></span></strong>
      </a>
    </div>
  </td>
 </tr>
</table>

## 软件准备

- reCamera OS 0.2.3+
- 主机工具链（用于交叉编译 C++ 程序，`riscv64-unknown-linux-musl-`）
- SG200X SDK
- Python 3.x（用于 PC 端 ONVIF 客户端 / 验收脚本）

:::note
本演示使用的模型文件已提供在 [Google Drive](https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link)，请进入 `/reCamera_Shared/Wiki/onvif_yolo/model/` 下载，无需自行转换。该模型也是 reCamera 系统自带的检测模型。

如需自行转换模型，请参考：[reCamera AI 模型部署指南](https://wiki.seeedstudio.com/cn/recamera_ai_model_deployment/)
:::

## 搭建演示

### 步骤 1：配置 reCamera

首先，请按照官方入门指南完成 reCamera 的基本配置：[reCamera 基本配置](https://wiki.seeedstudio.com/cn/recamera_getting_started/)

完成初始设置后，确保设备已通电并正确连接到网络。记下设备的 IP 地址（后面客户端会用到，ONVIF 发现也会自动获取）。

:::warning
在运行 C++ 程序之前，必须停止默认的 Node-RED 等服务，因为它们会占用相机资源。请通过 SSH 运行以下命令：
:::

```bash
sudo /etc/init.d/S03node-red stop
sudo /etc/init.d/S91sscma-node stop
sudo /etc/init.d/S93sscma-supervisor stop
```

### 步骤 2：下载模型和代码

从 [Google Drive](https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link) 进入 `/reCamera_Shared/Wiki/onvif_yolo/model/`，下载本演示所需的模型文件：

- `yolo11n_detection_cv181x_int8.cvimodel` - YOLO11n 目标检测模型（COCO 80 类，cv181x INT8）

从 GitHub 克隆代码仓库：

```bash
git clone https://github.com/RobotXTeam/sscma-example-sg200x.git
cd sscma-example-sg200x/solutions/sesg-project/onvif_yolo
mkdir -p model
# 将下载的 .cvimodel 放入 model/
```

### 步骤 3：编译 C++ 程序

设置交叉编译环境：

```bash
export SG200X_SDK_PATH=<SDK路径>
export PATH=<工具链路径>/bin:$PATH
```

编译项目：

```bash
rm -rf build && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

编译后的可执行文件位于：`build/onvif_yolo`

### 步骤 4：部署到 reCamera

将可执行文件和模型上传到 reCamera：

```bash
scp build/onvif_yolo recamera@<device-ip>:/home/recamera/onvif_yolo/
scp model/yolo11n_detection_cv181x_int8.cvimodel recamera@<device-ip>:/home/recamera/onvif_yolo/model/
```

设备上的目录结构：

```text
/home/recamera/onvif_yolo/
├── onvif_yolo                # 可执行文件
└── model/
    └── yolo11n_detection_cv181x_int8.cvimodel
```

### 步骤 5：运行演示

SSH 登录 reCamera 并运行：

```bash
cd /home/recamera/onvif_yolo
chmod +x onvif_yolo
sudo env LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:$LD_LIBRARY_PATH \
  ./onvif_yolo ./model/yolo11n_detection_cv181x_int8.cvimodel 0.60 2 8554 live 8080
```

#### 参数说明

| 参数 | 描述 | 默认值 |
|------|------|--------|
| `model` | YOLO11n 检测模型路径 | 必填 |
| `threshold` | 检测置信度阈值 | 0.60 |
| `skip` | 每 N 帧推理 1 次 | 2 |
| `rtsp_port` | RTSP 端口 | 8554 |
| `rtsp_session` | RTSP path | live |
| `onvif_port` | ONVIF SOAP 端口 | 8080 |

:::warning
关闭程序请用 `Ctrl-C` 或 `kill -TERM <pid>`，程序会优雅释放摄像头/VPSS/RTSP 资源。**不要使用 `kill -9`**，否则可能残留 VPSS 资源，下次启动报错，需要重启设备恢复。
:::

## 预期输出

### 在 reCamera 终端上

运行成功后，终端将显示：

```text
========== ONVIF + YOLO AI IPC ==========
YOLO model : ./model/yolo11n_detection_cv181x_int8.cvimodel
threshold  : 0.60 | skip: 2
RTSP       : :8554/live
ONVIF SOAP : :8080
=========================================
[onvif_yolo] device_ip = 192.168.x.x
[onvif_yolo] YOLO11n detector ready (SSCMA multi-output)
[onvif_yolo] RTSP streaming: rtsp://192.168.x.x:8554/live
[onvif] WS-Discovery listening on 239.255.255.250:3702
[onvif] SOAP HTTP service on http://0.0.0.0:8080 (device_service / media_service)
[onvif_yolo] FPS=26.7 | infer=27 (avg 38.0 ms) | last detections=2
```

### 在 PC 端

PC 端用 `onvif_client.py`（纯标准库 + opencv-python）完成标准 ONVIF 接入流程：

```bash
# 1) WS-Discovery 发现设备，并拉取设备信息和 RTSP 地址
python3 onvif_client.py --discover

# 2) 发现后直接拉流显示（带检测框和类别标签）
python3 onvif_client.py --discover --play

# 3) 直连已知设备 IP
python3 onvif_client.py --host <device-ip> --onvif-port 8080 --play
```

发现 + SOAP 阶段的输出示例：

```text
[discover] 发现设备 192.168.x.x -> http://192.168.x.x:8080/onvif/device_service
[onvif] 设备信息:
    Manufacturer: Seeed Studio
    Model: reCamera
    FirmwareVersion: 1.0-onvif-yolo
    SerialNumber: RECAMERA-0001
    HardwareId: SG2002
[onvif] RTSP Stream URI = rtsp://192.168.x.x:8554/live
```

您也可以用任意标准工具直接拉流（无需脚本）：

```bash
ffplay -rtsp_transport tcp rtsp://<device-ip>:8554/live
```

<div align="center"><img width={600} src="https://files.seeedstudio.com/wiki/reCamera/onvif_yolo/demo.gif" /></div>

### 证据文件

本演示的完整证据图片和视频已上传到 Google Drive：

- Google Drive 根目录：[Google Drive](https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link)
- 证据图片路径：`/reCamera_Shared/Wiki/onvif_yolo/evidence/image/`
- 证据视频路径：`/reCamera_Shared/Wiki/onvif_yolo/evidence/video/`

关键文件：

- `demo.gif` - 演示动图（带检测框和类别标签）
- `frame_detection_01.png` / `frame_detection_02.png` - 检测关键帧
- `onvif_acceptance.txt` - ONVIF 发现 + SOAP 验收日志
- `onvif_yolo_demo.mp4` - RTSP 拉流录制视频（1280x720 H.264）

## 故障排查

### 相机访问错误

如果看到 "No camera" 错误：

- 确保 Node-RED 等服务已停止（参见步骤 1）
- 检查相机连接

### `CVI_VPSS_CreateGrp failed`

通常是上次用 `kill -9` 强杀程序残留了 VPSS 资源。解决：

```bash
sudo reboot
```

重启后重新停服务再运行。

### 客户端发现不到设备

- 确认 PC 和 reCamera 在同一局域网，且组播未被路由器/防火墙拦截
- 确认设备端日志有 `WS-Discovery listening on 239.255.255.250:3702`
- 也可以跳过发现，用 `--host <device-ip>` 直连 ONVIF SOAP

### RTSP 拉流报 PPS/SPS 错误

刚连接时偶发，是因为接入点恰好在关键帧之间。等待 1-2 秒或使用 `-rtsp_transport tcp` 即可稳定。

## 安全说明

本演示的 ONVIF SOAP 和 RTSP 服务**未启用鉴权**，仅适用于可信局域网的演示/评估环境。如需生产部署，请补充 ONVIF WS-UsernameToken 鉴权和 RTSP 鉴权，并限制服务监听范围。

## 恢复服务

演示完成后，恢复默认服务：

```bash
sudo /etc/init.d/S03node-red start
sudo /etc/init.d/S91sscma-node start
curl -sS http://127.0.0.1/api/version
```

## 技术支持与产品讨论

感谢您选择我们的产品！如果您需要特定定制目标的指导或想要进一步扩展工作流，请随时联系我们。我们在这里为您提供不同的支持，确保您使用我们产品的体验尽可能顺畅。我们提供多种沟通渠道以满足不同的偏好和需求。

<div class="button_tech_support_container">
<a href="https://forum.seeedstudio.com/" class="button_forum"></a>
<a href="https://www.seeedstudio.com/contacts" class="button_email"></a>
</div>

<div class="button_tech_support_container">
<a href="https://discord.gg/eWkprNDMU7" class="button_discord"></a>
<a href="https://github.com/Seeed-Studio/wiki-documents/discussions/69" class="button_discussion"></a>
</div>
