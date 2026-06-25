# hand_gesture — reCamera 手势识别（4 模型串联 + UDP 推流）

在 reCamera（SG200X）上直接调用板载摄像头，串联 MediaPipe 官方 4 个 TFLite 模型（已转换为 `.cvimodel`）做完整手势识别，并通过 UDP 把识别结果 + JPEG 画面推送到 PC 显示。

## 流水线

```
摄像头 CH0 (RGB888)
    │
    ▼
[1] HandDetector (palm, 192×192, SSD) ── 坑⑥：SSD anchor 合并同 stride 层
    │
    ▼ (每只手)
[2] HandLandmarker (224×224, 旋转 ROI) ── 坑①②③：旋转角/Rect变换/投影
    │
    ▼
[3] GestureEmbedder (1×21×3 → 1×128) ── 坑④：rotation=0 不旋转；cvimodel 仅保留 hand 输入（REPORT 4.1）
    │
    ▼
[4] CannedGestureClassifier (1×128 → 8 logits → CPU softmax) ── 坑⑤：Softmax 被截断，C++ 补做 softmax（REPORT 4.2）
    │
    ▼
UDPHandResult (288 bytes/手) ── SesgJpegUdpStreamer ──► PC (CH1 JPEG + 结果)
```

## 工程结构

```
hand_gesture/
├── CMakeLists.txt                 # 顶层 project(hand_gesture)
├── README.md                      # 本文件
├── main/
│   ├── CMakeLists.txt             # component_register(SRCS *.cpp, PRIVATE_REQUIREDS sscma-micro stream_udp)
│   ├── main.cpp                   # 入口：参数解析 + 摄像头 CH0/CH1 + 主循环 + UDP 推流
│   ├── hand_types.h               # Palm / HandResult / UDPHandResult(288B, #pragma pack 1)
│   ├── engine_utils.h             # EngineCVI 通用工具：dtype 转换、shape numel、输入打包、输出读取
│   ├── gesture_math.h / .cpp      # 公共数学：normalize_radians、letterbox、RotatedRect 透视变换
│   ├── hand_detector.h / .cpp     # [模型1] palm 检测 + SSD anchor(2016) + 解码 + NMS
│   ├── hand_landmarker.h / .cpp   # [模型2] 旋转 ROI + 21 关键点 + LandmarkProjection
│   └── gesture_recognizer.h / .cpp# [模型3+4] embedder 打包(rotation=0) + classifier(不 softmax)
└── tools/
    └── udp_receiver.py            # PC 端接收：JPEG 解码 + 21 关键点骨架 + gesture 文字
```

## 后处理 7 大关键坑（与 PC 端 Python 参考逐条对齐）

| 坑 | 位置 | 说明 |
|----|------|------|
| ① 旋转角符号 | `hand_landmarker.cpp compute_rotation` | `atan2(-(y1-y0), x1-x0)` — Y 轴朝下 dy 带负号 |
| ② Rect 变换顺序 | `hand_landmarker.cpp transform_rect` | shift(原始 w/h) → square_long → scale(×2.6) |
| ③ 关键点投影 | `hand_landmarker.cpp` LandmarkProjection | `(cos·(lx-0.5) - sin·(ly-0.5))·w + xc` |
| ④ embedder rotation=0 | `gesture_recognizer.cpp recognize` | embedder 不旋转 landmarks，RotateLandmarks 恒等 |
| ⑤ classifier 补 softmax | `gesture_recognizer.cpp recognize` | cvimodel Softmax 被截断输出 logits，C++ 端补做 softmax（REPORT 4.2）|
| ⑥ SSD anchor 合并 | `hand_detector.cpp generate_anchors` | strides=[8,16,16,16] 同 stride 合并，总数=2016 |
| ⑦ **BF16 输出读取** | `hand_detector.cpp detect` | reCamera 裸机运行 `*_bf16.cvimodel` 时，输出张量类型为 `MA_TENSOR_TYPE_BF16`。`reinterpret_to_float()` 仅 F32 返回有效指针，BF16 返回 `nullptr` → 检测永远空（日志"平均手数=0.00"）。必须用 `read_val()` 逐元素读取（兼容 BF16/F16/F32/S8/U8）。PC 端不复现：`model_runner.py` 把 BF16 输出落盘为 `*_f32` 再读取。 |

> cvimodel 适配（详见 `/home/zxj/model/workspace/hand_gesture_convert/REPORT.md` 第 4 节）：
> - **4.1** `gesture_embedder` 的 world/handedness 为 dead inputs 被编译器裁剪，cvimodel 仅接受 `hand` 输入；
>   `prepareEmbedderInputs()` 仅要求找到 hand 输入，`pack_*` 在对应 idx<0 时跳过。
> - **4.2** `canned_gesture_classifier` 的 Softmax 末层被 TPU 截断（数值溢出），cvimodel 输出原始 logits；
>   C++ 端读取 logits 后补做数值稳定的 softmax 再 argmax，否则 clip[0,1] 会把置信度压成 0/1。

> PC 端权威参考实现：`/home/zxj/model/workspace/test_tflite_rtsp.py`

## 板端构建

```bash
cd /home/zxj/reCamera_cpp_github/sscma-example-sg200x/solutions/sesg-project/hand_gesture
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build
# 产物：build/hand_gesture (交叉编译到 riscv64)
```

## 板端运行

### 模型准备

将 4 个 TFLite 模型转换为 `.cvimodel`（本阶段先不关心转换，路径通过命令行传入）：

```
hand_detector.tflite          → hand_detector.cvimodel
hand_landmarks_detector.tflite→ hand_landmarks_detector.cvimodel
gesture_embedder.tflite       → gesture_embedder.cvimodel
canned_gesture_classifier.tflite → canned_gesture_classifier.cvimodel
```

### 只 stdout（不推流）

```bash
./build/hand_gesture \
    hand_detector.cvimodel \
    hand_landmarks_detector.cvimodel \
    gesture_embedder.cvimodel \
    canned_gesture_classifier.cvimodel \
    640 480 0.5
```

### 推流到 PC

```bash
./build/hand_gesture \
    hand_detector.cvimodel \
    hand_landmarks_detector.cvimodel \
    gesture_embedder.cvimodel \
    canned_gesture_classifier.cvimodel \
    640 480 0.5 \
    192.168.2.101 5001 320 240 10
```

参数顺序：

```
<palm> <landmark> <embedder> <classifier>
[cam_w] [cam_h] [min_score]
[udp_ip] [udp_port] [jpeg_w] [jpeg_h] [jpeg_fps]
```

- 前 4 个模型路径必填。
- `udp_ip` + `udp_port` 同时给出时启用 UDP 推流（CH1 JPEG 320×240@10fps）。

## PC 端接收

### 依赖

```bash
pip install opencv-python numpy
```

### 运行

```bash
python3 tools/udp_receiver.py --ip 0.0.0.0 --port 5001 --show --print-dets
```

- `--show`：弹窗显示 JPEG + 叠加（palm 框/关键点骨架/gesture 文字）。
- `--print-dets`：stdout 打印每只手的 gesture/conf/handedness。

### UDP 协议

线协议由 `sesg::udp_service::UDPSender` 产生（小端）：

```
magic(u32) | width(u32) | height(u32) | payload_size(u32) | det_count(u32) | fmt(u32) | fid(u32)
| det_bytes (det_count × 288)
| payload (JPEG 字节, fmt==1)
```

`UDPHandResult`（288 bytes，`#pragma pack(1)`）：

| 偏移 | 类型 | 字段 |
|------|------|------|
| 0   | 5×f32 | palm_x1, palm_y1, palm_x2, palm_y2, palm_score |
| 20  | f32   | handedness |
| 24  | i32   | gesture_idx (0..7) |
| 28  | f32   | gesture_conf |
| 32  | 63×f32| landmarks[21][3] (x,y,z 交错) |
| 284 | i32   | landmark_valid |

Python 格式串：`"<5f f i f 63f i"`（288 bytes）。

## 手势类别

```
0:None  1:Closed_Fist  2:Open_Palm  3:Pointing_Up
4:Thumb_Down  5:Thumb_Up  6:Victory  7:ILoveYou
```

## 性能统计

板端每秒打印：

```
[性能] FPS=12.50 | palm=15.2ms | landmark=22.3ms | gesture=8.1ms | 总耗时=45.6ms | 平均手数=1.20