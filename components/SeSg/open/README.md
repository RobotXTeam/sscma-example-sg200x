# opencv_objdetect (shim)

这是一个“占位/胶水”组件，用来把你额外编译出来的 OpenCV `objdetect` 模块（头文件 + 库）放进仓库的 `components/SeSg` 下，方便其它 solution 通过 `REQUIREDS opencv_objdetect` 引用。

## 目录约定

把你编译出来的产物放到：

- `components/SeSg/opencv_objdetect/include/`
- `components/SeSg/opencv_objdetect/lib/`

然后业务工程里在 `component_register(... REQUIREDS ...)` 增加：

- `opencv_objdetect`

即可让编译时增加 include 路径与 link 路径，同时链接 `-lopencv_objdetect`。

## 注意

- `cv::QRCodeDetector` 需要 `opencv2/objdetect.hpp`，也需要运行时能找到 `libopencv_objdetect.so*`。
- 为了 ABI 一致，建议 `objdetect` 的 OpenCV 版本与当前 SDK 正在用的 OpenCV 版本一致（你这里 `libopencv_core.so` 指向 4.5.0）。

## 工具脚本

本目录的 `copy_lib.sh` 用于把本地 `./lib` 下的库上传到远端，并覆盖远端系统库。

流程：

1. 本地库先拷贝到远端暂存目录 `/home/recamera/sdk_libs`。
2. 如果 `/mnt/system/lib` 存在同名库，先备份到 `/home/recamera/lib_bk`。
3. 使用远端 `sudo` 将暂存目录中的库覆盖到 `/mnt/system/lib`。
4. 覆盖成功后清理 `/home/recamera/sdk_libs`，保证下次可继续使用。

注意：

- 远端需要允许 `sudo -S` 读取密码执行 `cp`。
- 密码以明文写在脚本里，仅适合内网/开发环境。
- 若 `/mnt/system/lib` 为只读挂载，脚本无法覆盖，可登录设备后手动执行：
	- 先把文件从 `/home/recamera/sdk_libs` 复制到目标目录：
		- `sudo mv /home/recamera/sdk_libs/* /mnt/system/lib/`
	- 然后确认 `/home/recamera/sdk_libs` 已清空，便于下次脚本复用。
