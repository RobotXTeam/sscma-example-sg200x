# GB/T 28181 + YOLO AI IPC 部署报告

- **Demo 名称**：gb28181_yolo
- **日期**：2026-06-11
- **设备**：reCamera（SG2002 / cv181x，RISC-V，reCamera OS 0.2.4）
- **目标**：reCamera 作为 GB/T 28181 国标设备端，向国标平台注册并点播推流，画面带 YOLO11n 检测框；seeed 上用 SRS 5 国标平台验收。

## 1. 结论

端到端跑通，五个环节全过：

1. **SIP 注册**：REGISTER 成功（TCP 5060），设备 GBID `34020000001320000001`，SRS 域 `34020000002000000001`，realm `3402000000`，SRS5 简化版无鉴权。
2. **INVITE 点播**：SRS 主动 INVITE，设备回 100 Trying + 200 OK（SDP：sendonly / PS/90000 / y=ssrc）。
3. **媒体推送**：设备拉本地带框 H.264 → 手写 PS 封装 → RTP(PT=96) → RFC4571 长度前缀 → TCP 连 SRS:9000。
4. **SRS 解码**：收到国标流转 RTMP，`/live/34020000001320000001`，H264 Baseline 1280x720，drop=0。
5. **带框录制**：SRS HTTP-FLV/RTMP 拉流录制成功，32/45 抽样帧含 YOLO 检测框。

## 2. 架构

```
reCamera:
  相机 → YOLO11n(NPU) → RGN/OSD 硬件画框 → H.264 → 本地 RTSP 8554
  ffmpeg -c copy -bsf:v h264_mp4toannexb -f h264  (RTSP → Annex-B 裸流)
  gb28181_client (eXosip2):
    SIP/TCP: REGISTER + keepalive + answer INVITE
    手写 PS muxer: pack(0xBA)+system(0xBB)+PSM(0xBC)+PES(0xE0)
    RTP(PT=96) → RFC4571(2字节长度) → TCP → SRS:9000
SRS 5 (seeed, docker, host net):
    SIP-TCP 5060 / GB-TCP 媒体 9000 / 转 RTMP 1935 / HTTP-FLV 8080 / API 1985
```

## 3. 复用与新增

| 模块 | 来源 |
|------|------|
| 相机+YOLO+RGN 画框+本地 RTSP | 复用 rtmp_yolo 引擎 |
| SIP 协议栈 | osip2 5.3.1 + eXosip2 5.3.0（交叉编译到 riscv64-musl） |
| **GB28181 设备端客户端** | **新增 `gb28181_client.c`**：SIP 注册/心跳/INVITE 应答 + 手写 PS muxer + RTP/RFC4571/TCP |
| 国标平台 | SRS 5（`--gb28181=on`，TCP 模式） |

## 4. 关键技术点与踩坑

1. **SRS 5 GB28181 是 TCP-only**：SIP 5060 和媒体 9000 都走 TCP（不是传统 UDP）。客户端 SIP 用 eXosip2 TCP transport，媒体用 RTP-over-TCP（RFC4571 2 字节长度前缀）。
2. **PS 封装手写**：ffmpeg 无 PS muxer，且国标要求 PS（非 TS）。自写 MPEG Program Stream：关键帧前插 system header + PSM，PES 携带 PTS，PS 切片成 ≤1400B 的 RTP 包，末片置 RTP M 位。
3. **ffmpeg 子进程库污染**：从 RTSP 取流的 ffmpeg 子进程必须 `unset LD_LIBRARY_PATH`，否则会加载到 demo 自带 lib 目录里不兼容的 ffmpeg .so。
4. **HTTP-FLV 录制要在关键帧接入**：用大 probesize + 重试解决首帧 SPS/PPS 问题。
5. **`write_h264_ipb_frame errno=11`**：EAGAIN 流控回退（下游 socket 缓冲瞬时满），非致命，不影响入流和录制画面。
6. **依赖库**：eXosip2 运行时依赖 libcares/libssl/libcrypto（设备自带）+ libosip2/libosipparser2（随客户端一起放设备 lib/）。

## 5. 运维注意

- 启动用 `run_on_device.sh`：停服务 → 起 YOLO 引擎（RTSP 8554）→ 起 gb28181_client。
- 退出用 `Ctrl-C` / `kill -TERM`，不要 `kill -9` YOLO 引擎。
- 媒体线程暂不自动重连：RTSP 断开后需重启客户端触发新 INVITE（已知缺口）。

## 6. 真实环境路径（仅内部）

- seeed 仓库：`/home/steven/sscma-example-sg200x/solutions/sesg-project/gb28181_yolo`
- SIP 库交叉编译安装：`/home/seeed/gb28181/install`
- 工具链：`/home/seeed/zsz/TOOL/riscv64-linux-musl-x86_64/bin`
- SDK：`/home/seeed/桌面/sg2002_recamera_emmc`
- 设备运行目录：`/home/recamera/gb28181_yolo`（含 `lib/` 放 SIP .so）
- SRS：seeed docker 容器 `srs-gb`，host 网络，192.168.2.113

## 7. 云端资产

- Google Drive 根目录：https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link
- 模型：`/reCamera_Shared/Wiki/gb28181_yolo/model/yolo11n_detection_cv181x_int8.cvimodel`
- 运行时 SIP 库：`/reCamera_Shared/Wiki/gb28181_yolo/model/lib/lib{eXosip2,osip2,osipparser2}.so.15`
- 证据图片：`/reCamera_Shared/Wiki/gb28181_yolo/evidence/image/`
- 证据视频：`/reCamera_Shared/Wiki/gb28181_yolo/evidence/video/gb28181_yolo_demo.mp4`
