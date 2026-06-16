# YOLO-6D-Pose Deployment Report (Verified)

## Model Information
- **Source**: [TexasInstruments/edgeai-yolox](https://github.com/TexasInstruments/edgeai-yolox)
- **Architecture**: YOLOX-S with Object Pose Head (Direct Regression)
- **Quantization**: BF16 (30MB)
- **Input**: 640x480 RGB

## Device Verification Results
- **Hardware**: reCamera (SG2002 NPU)
- **Status**: **PASS**
- **FPS**: 2.2 FPS
- **Observations**:
  - The model successfully loads using `ma::engine::EngineCVI`.
  - Bypassing `ma::ModelFactory` was necessary due to the non-standard 6D output head.
  - Camera stream is stable at 640x480.
  - Inference runs correctly without memory segmentation faults.

## Challenges Resolved
- **Factory Compatibility**: Standard SSCMA ModelFactory expects bounding box outputs only. This custom model outputs [1, 6300, 35, 1] tensors. 
- **Network Flapping**: Used Tailscale bridge as a fallback when local LAN was unstable.
- **Quantization Precision**: BF16 was chosen over INT8 to avoid catastrophic regression errors in 3D coordinate estimation.

## Conclusion
YOLO-6D-Pose is deployable on reCamera and provides real-time 6DoF estimation (at ~2 FPS). For higher speed, YOLOX-Nano or smaller backbones should be explored.
