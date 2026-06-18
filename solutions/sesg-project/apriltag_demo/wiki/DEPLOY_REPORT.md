# AprilTag Demo 部署报告

## 部署时间

2026-06-17

## 部署环境

- 本机: Steven (Ubuntu)
- 编译机: seeed (192.168.2.113)
- 目标设备: reCamera (SG2002, RISC-V)

## 部署步骤

### 1. 获取 AprilTag 源码

```bash
# 在 seeed 上下载 AprilTag 库
cd /home/seeed
wget https://github.com/AprilRobotics/apriltag/archive/refs/heads/master.tar.gz
tar xzf master.tar.gz
mv apriltag-master apriltag_lib
```

### 2. 创建 demo 项目

```bash
mkdir -p /home/seeed/sscma-example-sg200x/solutions/sesg-project/apriltag_demo/main
```

创建文件:
- `CMakeLists.txt` - 顶层构建配置
- `main/CMakeLists.txt` - 组件构建配置
- `main/main.cpp` - 主程序

### 3. 编译

```bash
cd /home/seeed/sscma-example-sg200x/solutions/sesg-project/apriltag_demo
export SG200X_SDK_PATH=/home/seeed/桌面/sg2002_recamera_emmc
export PATH=/home/seeed/桌面/host-tools/gcc/riscv64-linux-musl-x86_64/bin:$PATH
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

编译成功，产物: `apriltag_demo` (RISC-V ELF)

### 4. 部署到 reCamera

```bash
# 复制到本机
scp seeed:/home/seeed/sscma-example-sg200x/solutions/sesg-project/apriltag_demo/build/apriltag_demo /tmp/

# 上传到 reCamera
/home/steven/.claude/skills/ae/environments/seeed-recamera/scripts/recamera_scp_to.sh /tmp/apriltag_demo /tmp/apriltag_demo
```

### 5. 运行测试

```bash
/home/steven/.claude/skills/ae/environments/seeed-recamera/scripts/recamera_ssh.sh \
  "printf 'recamera.1\n' | sudo -S env LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd:\$LD_LIBRARY_PATH /tmp/apriltag_demo"
```

运行结果:
- 摄像头初始化成功
- AprilTag 检测器初始化成功
- 采集 FPS: ~2
- 检测 FPS: ~2
- 平均检测耗时: ~500ms
- 检测成功: 0次（无标记在视野中）

### 6. 上传到 Google Drive

```bash
rclone copy ~/work/reCamera_demo/apriltag_demo/run/ agent:reCamera_Shared/Wiki/apriltag_demo/run/ --progress
```

上传完成:
- `apriltag_demo` (281KB)
- `README.md` (2KB)

## 遇到的问题

1. **Git clone 失败**: GitHub 下载不稳定，改用 wget 下载 tarball
2. **工具链不在 PATH**: 需要手动添加工具链路径到 PATH
3. **C++17 未启用**: 需要在 CMakeLists.txt 中显式设置 CMAKE_CXX_STANDARD 17
4. **include 路径错误**: AprilTag 头文件直接在库目录下，不是子目录
5. **像素格式错误**: 使用 MA_PIXEL_FORMAT_GRAYSCALE 而非 MA_PIXEL_FORMAT_GRAY
6. **链接错误**: 需要在 REQUIREDS 中添加 apriltag_static

## 验证结果

✅ 编译成功
✅ 部署成功
✅ 运行成功
✅ 摄像头初始化成功
✅ AprilTag 检测器初始化成功
✅ 性能统计输出正常

## 后续步骤

1. 创建 Wiki 文档
2. 推送到 GitHub
3. 干净克隆验证
