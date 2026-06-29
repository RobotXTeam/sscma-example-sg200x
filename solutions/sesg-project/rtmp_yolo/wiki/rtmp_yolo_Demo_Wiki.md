---
title: 使用 reCamera 的 RTMP AI 直播推流演示
description: 本文档介绍了使用 reCamera 的基于 AI 的 RTMP 推流演示，展示了 YOLO11n 实时目标检测 + 设备端硬件画框 + RTMP 推流到流媒体服务器的完整链路。
keywords:
  - RTMP
  - YOLO
  - AI IPC
  - SRS
  - reCamera
  - AI Edge Vision
  - Object Detection
slug: /recamera_rtmp_yolo
sku: 102991897,102991896
image: https://files.seeedstudio.com/wiki/reCamera/rtmp_yolo/demo.gif
sidebar_position: 31
last_update:
  date: 2026-06-11T00:00:00.000Z
  author: Steven
createdAt: '2026-06-11'
updatedAt: '2026-06-11'
url: https://wiki.seeedstudio.com/cn/recamera_rtmp_yolo/
---

# 使用 reCamera 的 RTMP AI 直播推流演示

## 简介

RTMP 是直播和流媒体领域应用最广的推流协议，几乎所有直播平台、CDN、流媒体服务器（SRS、nginx-rtmp、ZLMediaKit）都支持。本演示让 reCamera 成为一台**带 AI 目标检测的 RTMP 推流摄像机**：摄像头画面经 YOLO11n 实时检测，检测框和类别标签由设备端硬件叠加烧进 H.264 视频，再通过 RTMP 推送到流媒体服务器，任何标准播放器都能拉流观看带检测框的实时画面。

本项目提供了一个开箱即用的演示，专注于以下应用功能：

- **AI 目标检测**：YOLO11n（COCO 80 类），NPU 推理约 38 ms/帧。
- **设备端硬件画框**：检测框 + 类别/置信度标签通过 RGN/OSD 硬件叠加，不占用 CPU 重编码。
- **RTMP 推流**：H.264 经 `-c copy` 转封装推到 RTMP 服务器，零画质损失、低延时。

<div align="center"><img width={600} src="https://files.seeedstudio.com/wiki/reCamera/rtmp_yolo/demo.gif" /></div>

## 硬件准备

要运行此演示，只需要**一台 reCamera 设备**。支持所有 reCamera 变体。

<table align="center">
 <tr>
  <th>reCamera 2002 系列</th>
  <th>reCamera Gimbal</th>
  <th>reCamera HQ PoE</th>
 </tr>
 <tr>
  <td><div style={{textAlign:'center'}}><img src="https://files.seeedstudio.com/wiki/reCamera/recamera_banner.png" style={{width:300, height:'auto'}}/></div></td>
  <td><div style={{textAlign:'center'}}><img src="https://files.seeedstudio.com/wiki/reCamera/Gimbal/reCamera-Gimbal.png" style={{width:300, height:'auto'}}/></div></td>
  <td><div style={{textAlign:'center'}}><img src="https://files.seeedstudio.com/wiki/reCamera/reCamera_hq_poe/1-100029708-reCamera-2002-HQ-PoE-8GB.jpg" style={{width:300, height:'auto'}}/></div></td>
 </tr>
 <tr>
  <td><div class="get_one_now_container" style={{textAlign: 'center'}}><a class="get_one_now_item" href="https://www.seeedstudio.com/reCamera-2002w-8GB-p-6250.html" target="_blank"><strong><span><font color={'FFFFFF'} size={"4"}> 立即购买 </font></span></strong></a></div></td>
  <td><div class="get_one_now_container" style={{textAlign: 'center'}}><a class="get_one_now_item" href="https://www.seeedstudio.com/reCamera-gimbal-2002w-optional-accessories.html" target="_blank" rel="noopener noreferrer"><strong><span><font color={'FFFFFF'} size={"4"}> 立即购买 </font></span></strong></a></div></td>
  <td><div class="get_one_now_container" style={{textAlign: 'center'}}><a class="get_one_now_item" href="https://www.seeedstudio.com/reCamera-2002-HQ-PoE-64GB-p-6557.html" target="_blank" rel="noopener noreferrer"><strong><span><font color={'FFFFFF'} size={"4"}> 立即购买 </font></span></strong></a></div></td>
 </tr>
</table>

## 软件准备

- reCamera OS 0.2.3+（设备自带 `ffmpeg`）
- 主机交叉编译工具链 + SG200X SDK
- 流媒体服务器：SRS / nginx-rtmp（验收端）
- PC 端：ffplay / VLC，或本仓库 `rtmp_receiver.py`（需 `pip install opencv-python`）

:::note
本演示使用的模型文件已提供在 [Google Drive](https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link)，请进入 `/reCamera_Shared/Wiki/rtmp_yolo/model/` 下载。该模型也是 reCamera 系统自带的检测模型。
:::

## 搭建演示

### 步骤 1：搭建 RTMP 流媒体服务器

在一台服务器（或 PC）上用 Docker 启动 SRS：

```bash
docker run -d --name srs --network host ossrs/srs:5 ./objs/srs -c conf/srs.conf
```

SRS 提供 RTMP（1935）、HTTP-FLV（8080）、HTTP-API（1985）。记下服务器 IP，例如 `192.168.2.113`。

### 步骤 2：配置 reCamera

按官方入门指南完成 reCamera 基本配置：[reCamera 基本配置](https://wiki.seeedstudio.com/cn/recamera_getting_started/)

:::warning
运行 C++ 程序前，必须停止占用相机的默认服务：
:::

```bash
sudo /etc/init.d/S03node-red stop
sudo /etc/init.d/S91sscma-node stop
sudo /etc/init.d/S93sscma-supervisor stop
```

### 步骤 3：下载模型和代码

```bash
git clone https://github.com/RobotXTeam/sscma-example-sg200x.git
cd sscma-example-sg200x/solutions/sesg-project/rtmp_yolo
mkdir -p model
# 从 Google Drive /reCamera_Shared/Wiki/rtmp_yolo/model/ 下载 cvimodel 放入 model/
```

### 步骤 4：编译

```bash
export SG200X_SDK_PATH=<SDK路径>
export PATH=<工具链路径>/bin:$PATH
rm -rf build && mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

产物：`build/rtmp_yolo`

### 步骤 5：部署到 reCamera

```bash
scp build/rtmp_yolo run_rtmp.sh recamera@<device-ip>:/home/recamera/rtmp_yolo/
scp model/*.cvimodel recamera@<device-ip>:/home/recamera/rtmp_yolo/model/
```

### 步骤 6：运行演示

```bash
cd /home/recamera/rtmp_yolo
chmod +x rtmp_yolo run_rtmp.sh
sudo ./run_rtmp.sh rtmp://<srs-ip>:1935/live/recamera 0.60 2
```

`run_rtmp.sh` 会同时启动检测引擎和 RTMP 推流。

#### 参数说明

| 参数 | 描述 | 默认值 |
|------|------|--------|
| rtmp_url | 目标 RTMP 地址 | rtmp://127.0.0.1:1935/live/recamera |
| threshold | 检测置信度阈值 | 0.60 |
| skip | 每 N 帧推理 1 次 | 2 |

:::warning
关闭用 `Ctrl-C` 或 `kill -TERM`。**不要 `kill -9`**，否则残留 VPSS 资源需重启设备恢复。
:::

## 预期输出

### 在 reCamera 终端上

```text
========== RTMP + YOLO AI IPC ==========
YOLO model : ./model/yolo11n_detection_cv181x_int8.cvimodel
RTMP url   : rtmp://<srs-ip>:1935/live/recamera
========================================
[rtmp_yolo] aux RTSP ready. RTMP target = rtmp://<srs-ip>:1935/live/recamera
[run_rtmp] starting ffmpeg RTSP->RTMP relay -> rtmp://<srs-ip>:1935/live/recamera
[rtmp_yolo] FPS=26.7 | infer=27 (avg 38.0 ms) | last detections=2
```

### 在 PC 端

```bash
# 通用播放器
ffplay rtmp://<srs-ip>:1935/live/recamera
ffplay http://<srs-ip>:8080/live/recamera.flv

# 本仓库接收脚本（可录制）
python3 rtmp_receiver.py --url rtmp://<srs-ip>:1935/live/recamera
```

确认 SRS 收到流：

```bash
curl http://<srs-ip>:1985/api/v1/streams/
# 应看到 "/live/recamera"，video codec H264 1280x720
```

<div align="center"><img width={600} src="https://files.seeedstudio.com/wiki/reCamera/rtmp_yolo/demo.gif" /></div>

### 证据文件

- Google Drive 根目录：[Google Drive](https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link)
- 证据图片：`/reCamera_Shared/Wiki/rtmp_yolo/evidence/image/`
- 证据视频：`/reCamera_Shared/Wiki/rtmp_yolo/evidence/video/`

关键文件：

- `demo.gif` - 演示动图（带检测框）
- `frame_detection_01.png` / `frame_detection_bright.png` - 检测关键帧
- `rtmp_yolo_demo.mp4` - RTMP 拉流录制视频

## 故障排查

### SRS 收不到流

- 确认设备能 ping 通 SRS 服务器，RTMP 1935 端口可达
- 看 `run_rtmp.sh` 输出里 ffmpeg relay 是否在推（有 `frame=` 递增）
- `curl http://<srs-ip>:1985/api/v1/streams/` 看 SRS 端是否登记了流

### 画面无检测框

- 画面里要有 COCO 80 类目标（人、椅子、车等）才会画框
- 暗光场景检测率下降，可降低 threshold（如 0.30）

### `CVI_VPSS_CreateGrp failed`

上次用 `kill -9` 残留 VPSS，`sudo reboot` 后重来。

## 恢复服务

```bash
sudo /etc/init.d/S03node-red start
sudo /etc/init.d/S91sscma-node start
curl -sS http://127.0.0.1/api/version
```

## 技术支持与产品讨论

感谢您选择我们的产品！如果您需要特定定制目标的指导或想要进一步扩展工作流，请随时联系我们。

<div class="button_tech_support_container">
<a href="https://forum.seeedstudio.com/" class="button_forum"></a>
<a href="https://www.seeedstudio.com/contacts" class="button_email"></a>
</div>

<div class="button_tech_support_container">
<a href="https://discord.gg/eWkprNDMU7" class="button_discord"></a>
<a href="https://github.com/Seeed-Studio/wiki-documents/discussions/69" class="button_discussion"></a>
</div>
