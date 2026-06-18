# AprilTag Demo for reCamera

## 概述

AprilTag 是一个广泛使用的视觉标记检测库，由 AprilRobotics 开发。本 demo 在 reCamera (SG2002) 上运行 AprilTag 检测器，实时识别摄像头画面中的 AprilTag 标记，可用于机器人定位、姿态估计和增强现实等应用场景。

## 能力

- 实时检测 AprilTag tag36h11 标记
- 输出标记 ID、中心坐标和决策裕度
- 支持 UDP 推流模式
- 纯 CPU 运行，无需 NPU

## 性能

| 指标 | 数值 |
|------|------|
| 采集 FPS | ~2 |
| 检测 FPS | ~2 |
| 平均检测耗时 | ~500ms |
| 分辨率 | 640x480 |
| 标记族 | tag36h11 |

## 资产清单

### Google Drive

Wiki 根目录: https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link

| 类型 | 路径 | 文件 |
|------|------|------|
| 运行包 | `/reCamera_Shared/Wiki/apriltag_demo/run/` | `apriltag_demo`, `README.md` |
| 模型 | 无 | AprilTag 不需要模型文件 |

### GitHub

仓库: https://github.com/RobotXTeam/sscma-example-sg200x

| 类型 | 路径 |
|------|------|
| 源码 | `solutions/sesg-project/apriltag_demo/main/main.cpp` |
| 构建配置 | `solutions/sesg-project/apriltag_demo/CMakeLists.txt` |
| README | `solutions/sesg-project/apriltag_demo/README.md` |
| Wiki | `solutions/sesg-project/apriltag_demo/wiki/` |

## 构建

### 环境要求

- RISC-V 交叉编译工具链: `riscv64-unknown-linux-musl-gcc`
- SG200X SDK

### 编译步骤

```bash
# 设置环境变量
export SG200X_SDK_PATH=/path/to/sg2002_recamera_emmc
export PATH=/path/to/riscv64-linux-musl-x86_64/bin:$PATH

# 克隆仓库
git clone https://github.com/RobotXTeam/sscma-example-sg200x.git
cd sscma-example-sg200x

# 下载 AprilTag 库
wget https://github.com/AprilRobotics/apriltag/archive/refs/heads/master.tar.gz
tar xzf master.tar.gz
mv apriltag-master apriltag_lib

# 编译
cd solutions/sesg-project/apriltag_demo
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### 验证产物

```bash
file apriltag_demo
# 应显示: ELF 64-bit LSB executable, UCB RISC-V, RVC, double-float ABI
```

## 部署

### 1. 停止占用摄像头的服务

```bash
printf 'recamera.1\n' | sudo -S /etc/init.d/S03node-red stop
printf 'recamera.1\n' | sudo -S /etc/init.d/S91sscma-node stop
printf 'recamera.1\n' | sudo -S /etc/init.d/S93sscma-supervisor stop
```

### 2. 上传到 reCamera

```bash
scp apriltag_demo recamera@192.168.42.1:/tmp/
```

### 3. 运行

```bash
ssh recamera@192.168.42.1
chmod +x /tmp/apriltag_demo

# 无 UDP 推流
sudo env LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:$LD_LIBRARY_PATH \
  /tmp/apriltag_demo

# 带 UDP 推流
sudo env LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:$LD_LIBRARY_PATH \
  /tmp/apriltag_demo 192.168.2.101 5001
```

## 验收

1. 程序启动后显示: `[apriltag_demo] AprilTag 检测器已初始化 (tag36h11)`
2. 摄像头初始化成功: `CameraSG200X::startStream`
3. 每秒输出性能统计: `[性能] 采集FPS=X.XX | 检测FPS=X.XX | 平均检测耗时=XXXms | 检测成功=X次`
4. 对准 AprilTag 标记时显示: `[AprilTag] ID=X center=(X.X, X.X) decision_margin=X.XX`

## AprilTag 标记生成

使用 tag36h11 标记族。可从以下地址下载或生成标记:
- https://github.com/AprilRobotics/apriltag-imgs/tree/master/tag36h11

## 故障排除

| 问题 | 解决方案 |
|------|----------|
| 摄像头无法初始化 | 确保已停止 node-red 和 sscma-node 服务 |
| 检测速度慢 | 正常现象，AprilTag 在 CPU 上运行 |
| 无法检测到标记 | 确保标记清晰可见，光线充足 |
| VPSS 错误 | 可忽略，不影响功能 |

## 许可证

AprilTag 库遵循 BSD 许可证。
