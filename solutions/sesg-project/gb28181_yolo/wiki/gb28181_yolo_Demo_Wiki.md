---
title: 使用 reCamera 的 GB/T 28181 国标 AI 摄像机演示
description: 本文档介绍了使用 reCamera 的基于 AI 的 GB/T 28181 国标视频监控演示，展示了 YOLO11n 实时目标检测 + 设备端硬件画框 + 国标 SIP 注册/点播 + PS/RTP 推流到国标平台的完整链路。
keywords:
  - GB28181
  - GBT28181
  - YOLO
  - AI IPC
  - SRS
  - reCamera
  - AI Edge Vision
slug: /recamera_gb28181_yolo
sku: 102991897,102991896
image: https://files.seeedstudio.com/wiki/reCamera/gb28181_yolo/demo.gif
sidebar_position: 32
last_update:
  date: 2026-06-11T00:00:00.000Z
  author: Steven
createdAt: '2026-06-11'
updatedAt: '2026-06-11'
url: https://wiki.seeedstudio.com/cn/recamera_gb28181_yolo/
---

# 使用 reCamera 的 GB/T 28181 国标 AI 摄像机演示

## 简介

GB/T 28181 是中国安防视频监控联网的国家标准，国标平台、NVR、视频专网、雪亮工程等普遍要求前端设备通过 28181 协议接入。本演示让 reCamera 成为一台**符合 GB/T 28181 的 AI 国标摄像机**：设备向国标平台注册、响应点播（INVITE），把经 YOLO11n 实时检测、设备端硬件画框的 H.264 视频以国标要求的 **PS(Program Stream) over RTP** 方式推送到平台。

本项目提供了一个开箱即用的演示，专注于以下应用功能：

- **国标 SIP 接入**：SIP 注册、心跳保活、INVITE 点播应答（基于 eXosip2，走 TCP）。
- **国标 PS/RTP 媒体**：手写 MPEG Program Stream 封装 + RTP 打包 + RFC4571 over TCP 推流。
- **AI 目标检测**：YOLO11n（COCO 80 类），检测框 + 类别标签经设备端 RGN/OSD 硬件叠加烧进视频。

<div align="center"><img width={600} src="https://files.seeedstudio.com/wiki/reCamera/gb28181_yolo/demo.gif" /></div>

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

- reCamera OS 0.2.3+（设备自带 ffmpeg）
- 主机交叉编译工具链 + SG200X SDK
- GB28181 国标平台：SRS 5（`--gb28181=on`）或 WVP-pro
- SIP 库：osip2 5.3.1 + eXosip2 5.3.0（交叉编译）

:::note
本演示的模型文件和运行时 SIP 库已提供在 [Google Drive](https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link)：

- 模型：`/reCamera_Shared/Wiki/gb28181_yolo/model/yolo11n_detection_cv181x_int8.cvimodel`
- 运行时库：`/reCamera_Shared/Wiki/gb28181_yolo/model/lib/`（libeXosip2/libosip2/libosipparser2）
:::

## 搭建演示

### 步骤 1：搭建 GB28181 国标平台（SRS）

```bash
# srs.gb.conf 需启用 gb28181（sip listen 5060, media 9000, candidate=<srs-ip>）
docker run -d --name srs-gb --network host \
  -v /path/to/srs.gb.conf:/usr/local/srs/conf/srs.conf \
  ossrs/srs:5 ./objs/srs -c conf/srs.conf
```

:::tip
SRS 5 的 GB28181 走 **TCP**：SIP（5060）和媒体（9000）都是 TCP。
:::

### 步骤 2：配置 reCamera

按官方入门指南完成配置：[reCamera 基本配置](https://wiki.seeedstudio.com/cn/recamera_getting_started/)

:::warning
运行前停止占用相机的默认服务：
:::

```bash
sudo /etc/init.d/S03node-red stop
sudo /etc/init.d/S91sscma-node stop
sudo /etc/init.d/S93sscma-supervisor stop
```

### 步骤 3：下载模型/库和代码

```bash
git clone https://github.com/RobotXTeam/sscma-example-sg200x.git
cd sscma-example-sg200x/solutions/sesg-project/gb28181_yolo
mkdir -p model model/lib
# 从 Google Drive 下载 cvimodel 到 model/，SIP 库到 model/lib/
```

### 步骤 4：交叉编译

先编译 SIP 库（osip2 5.3.1 + eXosip2 5.3.0），再编译客户端：

```bash
export PATH=<工具链路径>/bin:$PATH
./build.sh    # 产物：gb28181_client
```

详细的 SIP 库交叉编译命令见仓库 README。

### 步骤 5：部署到 reCamera

```bash
scp gb28181_client run_on_device.sh recamera@<device-ip>:/home/recamera/gb28181_yolo/
scp model/lib/lib*.so.* recamera@<device-ip>:/home/recamera/gb28181_yolo/lib/
```

### 步骤 6：运行演示

```bash
cd /home/recamera/gb28181_yolo
chmod +x gb28181_client run_on_device.sh
sudo ./run_on_device.sh
```

`run_on_device.sh` 会停服务、起 YOLO 引擎（本地 RTSP）、起 GB28181 客户端。

#### 客户端参数

```
./gb28181_client <设备GBID> <平台IP> <平台SIP端口> <设备IP> <平台GBID> <RTSP地址>
```

| 参数 | 说明 | 示例 |
|------|------|------|
| 设备GBID | 本设备 20 位国标编码 | 34020000001320000001 |
| 平台IP / SIP端口 | 国标平台地址 | 192.168.2.113 / 5060 |
| 设备IP | 本设备 IP | 192.168.2.249 |
| 平台GBID | 平台 SIP 域 | 34020000002000000001 |
| RTSP地址 | 本地带框 H.264 源 | rtsp://127.0.0.1:8554/live |

:::warning
关闭用 `Ctrl-C` / `kill -TERM`。**不要 `kill -9`** YOLO 引擎（残留 VPSS 需重启设备）。
:::

## 预期输出

### 在 reCamera 终端上

```text
[sip] *** REGISTER SUCCESS ***
[sip] INVITE received -> 200 OK (SDP: PS/90000, ssrc=...)
[media] PS/RTP over TCP -> <srs-ip>:9000
```

### 在国标平台 / PC 端

SRS 把国标流转成 RTMP / HTTP-FLV，stream 名为设备 GBID：

```bash
ffplay rtmp://<srs-ip>:1935/live/34020000001320000001
ffplay http://<srs-ip>:8080/live/34020000001320000001.flv

# 查看注册和流状态
curl http://<srs-ip>:1985/api/v1/streams/
# 应看到 name=34020000001320000001, codec H264 1280x720, active=true
```

<div align="center"><img width={600} src="https://files.seeedstudio.com/wiki/reCamera/gb28181_yolo/demo.gif" /></div>

### 证据文件

- Google Drive 根目录：[Google Drive](https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link)
- 证据图片：`/reCamera_Shared/Wiki/gb28181_yolo/evidence/image/`
- 证据视频：`/reCamera_Shared/Wiki/gb28181_yolo/evidence/video/`

关键文件：

- `demo.gif` - 演示动图（带检测框）
- `frame_detection_01.png` / `frame_detection_bright.png` - 检测关键帧
- `gb28181_yolo_demo.mp4` - 国标平台拉流录制视频
- `gb28181_acceptance.txt` - 国标注册/点播/媒体验收日志

## 故障排查

### 注册失败

- 确认平台 SIP 端口（SRS 5 是 TCP 5060）可达，设备 GBID/域编码正确
- 对接需鉴权的平台（如 WVP）需补 SIP MD5 摘要鉴权（SRS5 简化版无需）

### 点播无流 / 平台收不到媒体

- SRS 5 媒体走 TCP 9000，确认可达
- 看客户端日志 INVITE 是否应答、媒体线程是否启动
- `curl http://<srs-ip>:1985/api/v1/streams/` 看平台是否登记流

### 画面无检测框

- 画面需有 COCO 80 类目标才画框；暗光场景可降低阈值

### `CVI_VPSS_CreateGrp failed`

上次 `kill -9` 残留 VPSS，`sudo reboot` 后重来。

## 安全说明

GB/T 28181 的 SIP 信令和 PS/RTP 媒体在本演示中为明文，仅适用于可信视频专网/局域网演示。生产部署请遵循国标的设备鉴权、SIP digest、媒体加密（如需）和网络隔离要求。

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
