# reCamera YOLO-6D-Pose Run Package

## Verified Components
1. **yolo_6d_pose_minimal**: Bypasses SSCMA ModelFactory to support custom 6D output head.
2. **yolox_s_6d_pose_no_decode_cv181x_bf16.cvimodel**: 30MB, YCB-V mustard can class.

## Quick Start
1. Transfer files to reCamera:
   ```bash
   scp yolo_6d_pose_minimal yolox_s_6d_pose_no_decode_cv181x_bf16.cvimodel recamera@192.168.42.1:/tmp/
   ```
2. Run on reCamera:
   ```bash
   /etc/init.d/S03node-red stop
   cd /tmp
   export LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:$LD_LIBRARY_PATH
   ./yolo_6d_pose_minimal yolox_s_6d_pose_no_decode_cv181x_bf16.cvimodel 0.60
   ```
3. On PC (Receiver):
   ```bash
   python3 udp_receiver_6d.py
   ```

## Performance
- **FPS**: ~2.2 FPS (SG2002 NPU, BF16)
- **Input**: 640x480 RGB
