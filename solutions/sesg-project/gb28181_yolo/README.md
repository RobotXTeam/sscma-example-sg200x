# gb28181_yolo

reCamera 上的 **GB/T 28181 国标 AI 网络摄像机**演示：reCamera 作为符合国标的前端设备（IPC），向国标平台（SRS / WVP 等）注册，响应点播，把带 YOLO11n 检测框的 H.264 以 **PS(Program Stream) over RTP** 推送上去。

GB/T 28181 是中国安防视频监控联网的国家标准，国标平台/NVR/视频专网普遍要求设备走 28181 接入。本 demo 让 reCamera 成为一台带 AI 检测的国标设备端。

## 架构

```
reCamera (gb28181_client + YOLO 引擎)                 国标平台 (SRS)
┌────────────────────────────────────────┐
│ 相机 → YOLO11n(NPU) → RGN/OSD 画框 → H264 │
│   └→ 本地 RTSP 127.0.0.1:8554/live        │
│ ffmpeg 拉 RTSP → Annex-B 裸流              │
│   ↓                                        │   SIP REGISTER (TCP 5060)
│ gb28181_client:                            │ ──────────────────────────→ 注册
│  · SIP(eXosip2/TCP) 注册+心跳+INVITE应答    │ ←──────────────────────────  INVITE
│  · 手写 PS 封装 → RTP(PT=96)               │   PS/RTP over TCP (媒体 9000)
│    → RFC4571 长度前缀 → TCP                │ ──────────────────────────→ 媒体
└────────────────────────────────────────┘                              ↓
                                              SRS 转 RTMP/HTTP-FLV: /live/<设备GBID>
                                                         ↓ 拉流验收
                                              ffplay / VLC / 国标客户端
```

检测框由设备端 RGN/OSD 硬件叠加烧进 H.264，PS 封装不重编码。

## 硬件 / 软件需求

- reCamera 一台（reCamera OS 0.2.3+，自带 ffmpeg）
- 主机交叉编译工具链 `riscv64-unknown-linux-musl-` + SG200X SDK
- GB28181 国标平台：SRS 5（`--gb28181=on`）/ WVP-pro 等
- SIP 库：osip2 5.3.1 + eXosip2 5.3.0（交叉编译，见 build.sh）

## 模型与依赖库下载

模型和运行时 SIP 库不放 GitHub，从 Google Drive 下载：

- Google Drive 根目录：<https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link>
- 模型路径：`/reCamera_Shared/Wiki/gb28181_yolo/model/`
  - `yolo11n_detection_cv181x_int8.cvimodel`（COCO 80 类，设备自带）
- 运行时库路径：`/reCamera_Shared/Wiki/gb28181_yolo/model/lib/`
  - `libeXosip2.so.15` / `libosip2.so.15` / `libosipparser2.so.15`（riscv64-musl）

> `libcares` / `libssl` / `libcrypto` 设备自带，无需下载。

## 快速运行（开箱即跑，无需编译）

从 Google Drive 拉运行包（含可执行 + SIP 运行时库，无需自己编译 eXosip2/osip2）：

- 运行包：`/reCamera_Shared/Wiki/gb28181_yolo/run/`（`gb28181_client` + `lib/`(SIP 库) + `run_on_device.sh` + `README.md`）
- 模型：`/reCamera_Shared/Wiki/gb28181_yolo/model/`
- 还需 rtmp_yolo 运行包提供本地带框 RTSP：`/reCamera_Shared/Wiki/rtmp_yolo/run/`

按 `run/README.md` 跑即可（注意给 `lib/` 重建软链）。

## 交叉编译

### 1) 编译 SIP 库（osip2 + eXosip2）

```bash
export PATH=<toolchain>/bin:$PATH
SR=<sysroot>
PREFIX=<install-prefix>
# osip2 5.3.1 (ftp.gnu.org/gnu/osip/)
./configure --host=riscv64-unknown-linux-musl CC=riscv64-unknown-linux-musl-gcc --prefix=$PREFIX --enable-shared --disable-static
make -j4 && make install
# eXosip2 5.3.0 (download.savannah.nongnu.org/releases/exosip/libexosip2-5.3.0.tar.gz)
export osip2_CFLAGS="-I$PREFIX/include" osip2_LIBS="-L$PREFIX/lib -losip2 -losipparser2"
./configure --host=riscv64-unknown-linux-musl CC=riscv64-unknown-linux-musl-gcc --prefix=$PREFIX --enable-shared --disable-static \
  CPPFLAGS="-I$PREFIX/include -I$SR/usr/include" LDFLAGS="-L$PREFIX/lib -L$SR/usr/lib" LIBS="-lcares -lssl -lcrypto"
make -j4 && make install
```

### 2) 编译客户端

```bash
cd solutions/sesg-project/gb28181_yolo
./build.sh    # 见脚本，链接 eXosip2/osip2/cares/ssl/crypto
# 产物: gb28181_client
```

## 部署与运行

```bash
# 上传客户端、SIP 库、启动脚本到 reCamera
scp gb28181_client run_on_device.sh recamera@<device-ip>:/home/recamera/gb28181_yolo/
scp <PREFIX>/lib/lib{eXosip2,osip2,osipparser2}.so.* recamera@<device-ip>:/home/recamera/gb28181_yolo/lib/
# YOLO 检测引擎复用 rtmp_yolo（提供本地 RTSP）

# 一键启动（停服务 → 起 YOLO 引擎 → 起 GB28181 客户端）
cd /home/recamera/gb28181_yolo
chmod +x gb28181_client run_on_device.sh
sudo ./run_on_device.sh
```

### gb28181_client 参数

```
./gb28181_client <设备GBID> <平台IP> <平台SIP端口> <设备IP> <平台GBID> <RTSP地址>
# 例：
./gb28181_client 34020000001320000001 192.168.2.113 5060 192.168.2.249 \
  34020000002000000001 rtsp://127.0.0.1:8554/live
```

> 关闭用 `Ctrl-C` / `kill -TERM`。**不要 `kill -9`** YOLO 引擎（残留 VPSS 需 reboot）。

## 搭建 SRS 国标平台（验收端）

```bash
# srs.conf 启用 gb28181 (sip 5060, media 9000, candidate=<srs-ip>)
docker run -d --name srs-gb --network host \
  -v /path/to/srs.gb.conf:/usr/local/srs/conf/srs.conf \
  ossrs/srs:5 ./objs/srs -c conf/srs.conf
```

注意：SRS 5 的 GB28181 走 **TCP**（SIP 5060 + 媒体 9000 都是 TCP）。

## PC 端验收

```bash
# SRS 把国标流转成 RTMP / HTTP-FLV，stream 名为设备 GBID
ffplay rtmp://<srs-ip>:1935/live/34020000001320000001
ffplay http://<srs-ip>:8080/live/34020000001320000001.flv

# 查看注册和流状态
curl http://<srs-ip>:1985/api/v1/streams/
```

## 证据

- Google Drive 根目录：<https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link>
- 证据图片：`/reCamera_Shared/Wiki/gb28181_yolo/evidence/image/`（demo.gif、frame_detection_*.png、gb28181_acceptance.txt）
- 证据视频：`/reCamera_Shared/Wiki/gb28181_yolo/evidence/video/gb28181_yolo_demo.mp4`

## 实现说明与已知问题

- **PS 封装为手写**（MPEG Program Stream：pack header 0xBA + system header 0xBB + PSM 0xBC + PES 0xE0），ffmpeg 仅用于把 RTSP 转成 Annex-B 裸流。
- SIP 鉴权：SRS 5 简化版无需鉴权（密码为空）；对接需鉴权的平台（如 WVP）需补 MD5 摘要。
- SRS 端偶发 `write_h264_ipb_frame errno=11` 是 EAGAIN 流控回退，非致命，不影响录制画面。
- 媒体线程暂不自动重连：YOLO 引擎重启导致 RTSP 断开后，需重启客户端触发新 INVITE（后续可加断线重连）。
- 安全：GB28181 信令/媒体为明文，仅适用于可信专网；生产需按国标做鉴权与网络隔离。
