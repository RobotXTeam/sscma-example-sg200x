# AprilTag Demo for reCamera

AprilTag 视觉标记检测 demo，在 reCamera (SG2002) 上实时检测 AprilTag 标记。

## 功能

- 实时检测 AprilTag tag36h11 标记
- 输出标记 ID、中心坐标和决策裕度
- 支持 UDP 推流模式
- 纯 CPU 运行，无需 NPU

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

## 资产

- Google Drive: https://drive.google.com/drive/folders/1GOQUMCel7fapbJCWzEEynDIvIt-6Wf5p?usp=drive_link
  - 运行包: `/reCamera_Shared/Wiki/apriltag_demo/run/`
  - 模型: 无（AprilTag 不需要模型文件）

## 许可证

AprilTag 库遵循 BSD 许可证。
