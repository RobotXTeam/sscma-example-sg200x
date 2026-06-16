# reCamera YOLO-6D-Pose Demo

## 1. Introduction
本项目展示了如何在 reCamera (SG200X NPU) 上部署 6DoF 姿态估计模型。采用德州仪器的 **YOLOX-6D-Pose** 架构，实现了单阶段、端到端的 6D 姿态回归（3D 旋转 + 3D 平移）。

## 2. Model Assets
所有模型和运行包均已发布至 Google Drive。
- **Google Drive 根目录**: [Google Drive Link](https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link)
- **子路径**:
  - 运行包: `/reCamera_Shared/Wiki/yolo_6d_pose/run/`
  - 模型文件: `/reCamera_Shared/Wiki/yolo_6d_pose/model/yolox_s_6d_pose_no_decode_cv181x_bf16.cvimodel`

## 3. Deployment Steps

### Model Conversion
1. 获取 [TexasInstruments/edgeai-yolox](https://github.com/TexasInstruments/edgeai-yolox) 源码。
2. 修改 `yolo_object_pose_head.py`，禁用 `decode_in_inference`。
3. 导出为 ONNX 并使用 TPU-MLIR 转换为 `cvimodel` (BF16)。

### Device Run
1. 将 `.cvimodel` 上传至 reCamera 设备。
2. 停止摄像头相关服务：
   ```bash
   /etc/init.d/S03node-red stop
   /etc/init.d/S91sscma-node stop
   /etc/init.d/S93sscma-supervisor stop
   ```
3. 运行 C++ 推理程序（输出 UDP 流）。

### Receiver
1. 在 PC 端运行 `udp_receiver_6d.py`。
2. 接收 reCamera 的 raw tensor 数据并进行 3D 投影可视化。

## 4. Evidence
相关截图已上传至 Google Drive `/reCamera_Shared/Wiki/yolo_6d_pose/evidence/image/`。
