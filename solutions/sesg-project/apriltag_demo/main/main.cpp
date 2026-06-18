#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sscma.h>
#include <video.h>

#include <sesg/stream_udp.h>

extern "C" {
#include <apriltag.h>
#include <tag36h11.h>
#include <tagStandard41h12.h>
}

using namespace ma;

#define TAG "apriltag_demo"

static std::atomic<bool> g_running(true);
static void sig_handler(int) { g_running.store(false); }

struct UDPAprilTagResult {
    float center_x;
    float center_y;
    float corners[8];  // 4 corners x 2 coords
    int tag_id;
    float decision_margin;
};

struct Args {
    bool enable_udp = false;
    std::string udp_ip = "192.168.2.101";
    int udp_port = 5001;

    int cam_w = 640;
    int cam_h = 480;

    int jpeg_ch = 1;
    int jpeg_w = 320;
    int jpeg_h = 240;
    int jpeg_fps = 10;

    float decimate = 1.0f;
    float blur = 0.0f;
    int threads = 1;
    bool debug = false;
};

static void print_usage(const char* argv0) {
    std::printf(
        "Usage: %s [udp_ip udp_port]\n"
        "\n"
        "Examples:\n"
        "  %s                        # 视频流推理（无UDP推流）\n"
        "  %s 192.168.2.101 5001     # 视频流推理并发送到UDP\n",
        argv0, argv0, argv0);
}

static bool parse_int(const char* s, int* out) {
    if (!s || !out) return false;
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (!end || *end != '\0') return false;
    *out = static_cast<int>(v);
    return true;
}

static bool parse_args(int argc, char** argv, Args* a) {
    if (!a) return false;

    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "-h" || arg1 == "--help") {
            print_usage(argv[0]);
            return false;
        }
    }

    if (argc == 3) {
        a->enable_udp = true;
        a->udp_ip = argv[1];
        if (!parse_int(argv[2], &a->udp_port)) {
            std::fprintf(stderr, "错误：端口号必须是数字\n");
            print_usage(argv[0]);
            return false;
        }
    } else if (argc == 1) {
        a->enable_udp = false;
    } else {
        std::fprintf(stderr, "错误：参数数量不正确\n");
        print_usage(argv[0]);
        return false;
    }

    return true;
}

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, &args)) {
        return (argc > 1) ? 1 : 0;
    }

    Signal::install({SIGINT, SIGTERM, SIGQUIT}, [](int) { sig_handler(0); });

    std::printf(
        "[apriltag_demo] 视频流模式 | UDP推流: %s, 目标地址=%s:%d, 摄像头=%dx%d, JPEG(通道=%d)=%dx%d@%dfps\n",
        args.enable_udp ? "启用" : "关闭",
        args.udp_ip.c_str(),
        args.udp_port,
        args.cam_w,
        args.cam_h,
        args.jpeg_ch,
        args.jpeg_w,
        args.jpeg_h,
        args.jpeg_fps);

    // Initialize AprilTag detector
    apriltag_detector* td = apriltag_detector_create();
    apriltag_family* tf = tag36h11_create();
    apriltag_detector_add_family(td, tf);
    td->quad_decimate = args.decimate;
    td->quad_sigma = args.blur;
    td->nthreads = args.threads;
    td->debug = args.debug ? 1 : 0;
    td->refine_edges = 1;

    std::printf("[apriltag_demo] AprilTag 检测器已初始化 (tag36h11)\n");

    Device* device = Device::getInstance();
    Camera* camera = nullptr;

    for (auto& sensor : device->getSensors()) {
        if (sensor->getType() == ma::Sensor::Type::kCamera) {
            camera = static_cast<Camera*>(sensor);
            camera->init(0);

            Camera::CtrlValue v;
            v.i32 = 0;
            camera->commandCtrl(Camera::CtrlType::kChannel, Camera::CtrlMode::kWrite, v);

            v.u16s[0] = static_cast<uint16_t>(args.cam_w);
            v.u16s[1] = static_cast<uint16_t>(args.cam_h);
            camera->commandCtrl(Camera::CtrlType::kWindow, Camera::CtrlMode::kWrite, v);

            v.i32 = 0;
            camera->commandCtrl(Camera::CtrlType::kPhysical, Camera::CtrlMode::kWrite, v);
            break;
        }
    }

    if (!camera) {
        MA_LOGE(TAG, "未找到摄像头设备");
        apriltag_detector_destroy(td);
        tag36h11_destroy(tf);
        return 1;
    }

    sesg::stream_udp::JpegChannelConfig jpeg_cfg;
    jpeg_cfg.ch = args.jpeg_ch;
    jpeg_cfg.width = args.jpeg_w;
    jpeg_cfg.height = args.jpeg_h;
    jpeg_cfg.fps = args.jpeg_fps;
    sesg::stream_udp::SesgJpegUdpStreamer::configureJpegChannel(*camera, jpeg_cfg);

    camera->startStream(Camera::StreamMode::kRefreshOnReturn);

    auto camera_shared = std::shared_ptr<Camera>(camera, [](Camera*) {});
    sesg::stream_udp::SesgJpegUdpStreamer udp_streamer(args.udp_ip, (uint16_t)args.udp_port);

    if (args.enable_udp) {
        if (!udp_streamer.start(camera_shared, jpeg_cfg, /*init_vpss=*/false)) {
            MA_LOGE(TAG, "UDP推流器启动失败");
            camera->stopStream();
            camera->deInit();
            apriltag_detector_destroy(td);
            tag36h11_destroy(tf);
            return 1;
        }
        std::printf("[apriltag_demo] UDP推流器已启动\n");
    }

    constexpr uint32_t MAGIC_APTG = 0x41505447;  // "APTG"

    std::vector<uint8_t> safe_frame;

    int frame_count = 0;
    int detect_count = 0;
    int ok_count = 0;
    int64_t detect_ms_sum = 0;
    auto stat_start = std::chrono::steady_clock::now();

    std::printf("[apriltag_demo] 开始 AprilTag 检测循环...\n");

    while (g_running.load()) {
        ma_img_t frame;
        if (camera->retrieveFrame(frame, MA_PIXEL_FORMAT_GRAYSCALE) != MA_OK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        if (!frame.data || frame.size == 0 || frame.width == 0 || frame.height == 0) {
            camera->returnFrame(frame);
            continue;
        }

        if (safe_frame.size() < frame.size) {
            safe_frame.resize(frame.size);
        }
        std::memcpy(safe_frame.data(), frame.data, frame.size);
        camera->returnFrame(frame);

        // Create AprilTag image
        image_u8_t im = {
            .width = (int)frame.width,
            .height = (int)frame.height,
            .stride = (int)frame.width,
            .buf = safe_frame.data()
        };

        auto d0 = std::chrono::steady_clock::now();
        zarray_t* detections = apriltag_detector_detect(td, &im);
        auto d1 = std::chrono::steady_clock::now();
        const int64_t dms = std::chrono::duration_cast<std::chrono::milliseconds>(d1 - d0).count();
        detect_ms_sum += dms;
        detect_count++;

        std::vector<UDPAprilTagResult> results;

        for (int i = 0; i < zarray_size(detections); i++) {
            apriltag_detection* det;
            zarray_get(detections, i, &det);

            ok_count++;

            UDPAprilTagResult r{};
            r.center_x = (float)det->c[0];
            r.center_y = (float)det->c[1];
            for (int j = 0; j < 4; j++) {
                r.corners[j * 2] = (float)det->p[j][0];
                r.corners[j * 2 + 1] = (float)det->p[j][1];
            }
            r.tag_id = det->id;
            r.decision_margin = (float)det->decision_margin;
            results.push_back(r);

            std::printf("[AprilTag] ID=%d center=(%.1f, %.1f) decision_margin=%.2f\n",
                det->id, det->c[0], det->c[1], det->decision_margin);
        }

        apriltag_detections_destroy(detections);

        if (args.enable_udp) {
            udp_streamer.sendLatest(MAGIC_APTG, results);
        }

        frame_count++;
        const auto stat_now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(stat_now - stat_start).count();

        if (elapsed >= 1000) {
            const double fps = (frame_count * 1000.0) / (double)elapsed;
            const double detect_fps = (detect_count * 1000.0) / (double)elapsed;
            const double avg_detect_ms = (detect_count > 0) ? ((double)detect_ms_sum / (double)detect_count) : 0.0;
            std::printf(
                "[性能] 采集FPS=%.2f | 检测FPS=%.2f | 平均检测耗时=%.1fms | 检测成功=%d次\n",
                fps,
                detect_fps,
                avg_detect_ms,
                ok_count);

            frame_count = 0;
            detect_count = 0;
            ok_count = 0;
            detect_ms_sum = 0;
            stat_start = stat_now;
        }
    }

    if (args.enable_udp) {
        udp_streamer.stop(/*deinit_vpss=*/false);
    }
    camera->stopStream();
    camera->deInit();

    apriltag_detector_destroy(td);
    tag36h11_destroy(tf);

    std::printf("[apriltag_demo] 程序正常退出\n");
    return 0;
}
