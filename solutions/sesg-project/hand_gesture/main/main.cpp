/**
 * @file main.cpp
 * @brief reCamera 手势识别入口：物理地址 mmap 零拷贝 cv::Mat + skip_interval 帧复用 + UDP 推流。
 *
 * 命令行：
 *   ./hand_gesture <palm.cvimodel> <landmark.cvimodel> <embedder.cvimodel> <classifier.cvimodel>
 *                  [min_score] [udp_ip] [udp_port] [jpeg_w] [jpeg_h] [jpeg_fps]
 *                  [skip_multi] [skip_single]
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <sscma.h>
#include <video.h>

#include <sesg/stream_udp.h>

#include "gesture_recognizer.h"
#include "hand_detector.h"
#include "hand_landmarker.h"
#include "hand_types.h"

using namespace ma;

#define TAG "hand_gesture"

// UDP 魔数："HAND"
constexpr uint32_t MAGIC_HAND = 0x48414E44;

static std::atomic<bool> g_running(true);
static void sig_handler(int) { g_running.store(false); }

// 命令行参数（对齐 PROMPT 第七章）
struct Args {
    std::string palm_model;
    std::string landmark_model;
    std::string embedder_model;
    std::string classifier_model;

    float min_score = 0.5f;   // 不再配置 cam_w/cam_h：使用摄像头原生分辨率（640×480）

    bool enable_udp = false;
    std::string udp_ip   = "192.168.2.101";
    int udp_port         = 5001;

    int jpeg_ch  = 1;
    int jpeg_w   = 320;
    int jpeg_h   = 240;
    int jpeg_fps = 10;

    // skip_interval：多手时默认 3，单手时默认 1
    int skip_multi  = 3;
    int skip_single = 1;
};

static void print_usage(const char* argv0) {
    std::printf(
        "Usage: %s <palm.cvimodel> <landmark.cvimodel> <embedder.cvimodel> <classifier.cvimodel>\n"
        "       [min_score] [udp_ip] [udp_port] [jpeg_w] [jpeg_h] [jpeg_fps]\n"
        "       [skip_multi] [skip_single]\n\n"
        "Examples:\n"
        "  %s m1.cvimodel m2.cvimodel m3.cvimodel m4.cvimodel\n"
        "  %s m1.cvimodel m2.cvimodel m3.cvimodel m4.cvimodel 0.5 192.168.2.101 5001 320 240 10\n"
        "  %s m1.cvimodel m2.cvimodel m3.cvimodel m4.cvimodel 0.5 192.168.2.101 5001 320 240 10 3 1\n",
        argv0, argv0, argv0, argv0);
}

static bool parse_int(const char* s, int* out) {
    if (!s || !out) return false;
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (!end || *end != '\0') return false;
    *out = static_cast<int>(v);
    return true;
}

static bool parse_float(const char* s, float* out) {
    if (!s || !out) return false;
    char* end = nullptr;
    float v = std::strtof(s, &end);
    if (!end || *end != '\0') return false;
    *out = v;
    return true;
}

static bool parse_args(int argc, char** argv, Args* a) {
    if (argc < 5) {
        std::fprintf(stderr, "[错误] 前 4 个模型路径必填\n");
        print_usage(argv[0]);
        return false;
    }
    a->palm_model       = argv[1];
    a->landmark_model   = argv[2];
    a->embedder_model   = argv[3];
    a->classifier_model = argv[4];

    // [min_score]
    if (argc > 5 && !parse_float(argv[5], &a->min_score)) {
        std::fprintf(stderr, "[错误] min_score 必须是数字\n"); return false;
    }

    // [udp_ip] [udp_port] → 同时给出才启用 UDP
    if (argc > 7) {
        a->enable_udp = true;
        a->udp_ip   = argv[6];
        if (!parse_int(argv[7], &a->udp_port)) {
            std::fprintf(stderr, "[错误] udp_port 必须是数字\n"); return false;
        }
        if (argc > 8 && !parse_int(argv[8], &a->jpeg_w)) {
            std::fprintf(stderr, "[错误] jpeg_w 必须是数字\n"); return false;
        }
        if (argc > 9 && !parse_int(argv[9], &a->jpeg_h)) {
            std::fprintf(stderr, "[错误] jpeg_h 必须是数字\n"); return false;
        }
        if (argc > 10 && !parse_int(argv[10], &a->jpeg_fps)) {
            std::fprintf(stderr, "[错误] jpeg_fps 必须是数字\n"); return false;
        }
    }
    // [skip_multi] [skip_single]
    if (argc > 11 && !parse_int(argv[11], &a->skip_multi)) {
        std::fprintf(stderr, "[错误] skip_multi 必须是数字\n"); return false;
    }
    if (argc > 12 && !parse_int(argv[12], &a->skip_single)) {
        std::fprintf(stderr, "[错误] skip_single 必须是数字\n"); return false;
    }
    return true;
}

// 把 HandResult 转成 UDP POD（对齐 hand_types.h UDPHandResult）
static hand::UDPHandResult to_udp(const hand::HandResult& r) {
    hand::UDPHandResult u{};
    u.palm_x1    = r.palm.bbox[0];
    u.palm_y1    = r.palm.bbox[1];
    u.palm_x2    = r.palm.bbox[2];
    u.palm_y2    = r.palm.bbox[3];
    u.palm_score = r.palm.score;
    u.handedness = r.handedness;
    u.gesture_idx    = r.gesture_idx;
    u.gesture_conf   = r.gesture_conf;
    u.landmark_valid = r.landmark_valid ? 1 : 0;
    for (int i = 0; i < hand::kNumLandmarks; ++i) {
        u.landmarks[i * 3 + 0] = r.landmarks[i][0];
        u.landmarks[i * 3 + 1] = r.landmarks[i][1];
        u.landmarks[i * 3 + 2] = r.landmarks[i][2];
    }
    return u;
}

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, &args)) {
        return (argc > 1) ? 1 : 0;
    }

    Signal::install({SIGINT, SIGTERM, SIGQUIT}, [](int) { sig_handler(0); });

    std::printf("[hand_gesture] 启动中...\n");
    std::printf("  palm:       %s\n", args.palm_model.c_str());
    std::printf("  landmark:   %s\n", args.landmark_model.c_str());
    std::printf("  embedder:   %s\n", args.embedder_model.c_str());
    std::printf("  classifier: %s\n", args.classifier_model.c_str());
    std::printf("  摄像头: 原生分辨率 (物理地址 mmap), min_score=%.2f\n", args.min_score);
    std::printf("  skip_interval: multi=%d, single=%d\n", args.skip_multi, args.skip_single);
    std::printf("  UDP: %s", args.enable_udp ? "启用" : "关闭");
    if (args.enable_udp) {
        std::printf(" → %s:%d, JPEG CH%d %dx%d@%dfps",
                    args.udp_ip.c_str(), args.udp_port,
                    args.jpeg_ch, args.jpeg_w, args.jpeg_h, args.jpeg_fps);
    }
    std::printf("\n");

    // ===== 1. 初始化 4 个模型 runner =====
    hand::HandDetector detector;
    if (!detector.init(args.palm_model)) {
        MA_LOGE(TAG, "palm detector 初始化失败: %s", args.palm_model.c_str());
        return 1;
    }
    std::printf("[OK] palm detector 已加载\n");

    hand::HandLandmarker landmarker;
    if (!landmarker.init(args.landmark_model)) {
        MA_LOGE(TAG, "landmarker 初始化失败: %s", args.landmark_model.c_str());
        return 1;
    }
    std::printf("[OK] hand landmarker 已加载\n");

    hand::GestureRecognizer recognizer;
    if (!recognizer.init(args.embedder_model, args.classifier_model)) {
        MA_LOGE(TAG, "gesture recognizer 初始化失败");
        return 1;
    }
    std::printf("[OK] gesture recognizer 已加载\n");

    // ===== 2. 初始化摄像头（CH0 物理地址模式，使用原生分辨率）=====
    Device* device = Device::getInstance();
    Camera* camera = nullptr;

    for (auto& sensor : device->getSensors()) {
        if (sensor->getType() == ma::Sensor::Type::kCamera) {
            camera = static_cast<Camera*>(sensor);
            camera->init(0);

            Camera::CtrlValue v;
            // CH0（推理用）
            v.i32 = 0;
            camera->commandCtrl(Camera::CtrlType::kChannel, Camera::CtrlMode::kWrite, v);

            // ⚠️ 关键：必须设置 kWindow，否则驱动报「Channel 0 is not configured」
            // 且 retrieveFrame 会因无帧产生而永久阻塞。
            // 640×480（VGA）在 VPSS 硬件缩放器支持范围内（face_udp 用 640×640 亦可），
            // VPSS 会从 sensor 原生分辨率缩放到此。palm/landmarker 内部再软件 letterbox 到 192×192/224×224。
            // （palm 模型所需 192×192 低于 VPSS 最小缩放分辨率，故 CH0 不用 192×192）
            v.u16s[0] = 640;
            v.u16s[1] = 480;
            camera->commandCtrl(Camera::CtrlType::kWindow, Camera::CtrlMode::kWrite, v);

            // 物理地址模式（frame.data 即物理地址，避免 CPU 拷贝）
            v.i32 = 1;
            camera->commandCtrl(Camera::CtrlType::kPhysical, Camera::CtrlMode::kWrite, v);
            break;
        }
    }

    if (!camera) {
        MA_LOGE(TAG, "未找到摄像头设备");
        return 1;
    }
    std::printf("[OK] 摄像头已初始化 (原生分辨率, 物理地址模式)\n");

    // ===== 3. 配置 JPEG 通道（仅启用 UDP 时）=====
    sesg::stream_udp::JpegChannelConfig jpeg_cfg;
    jpeg_cfg.ch    = args.jpeg_ch;
    jpeg_cfg.width  = args.jpeg_w;
    jpeg_cfg.height = args.jpeg_h;
    jpeg_cfg.fps    = args.jpeg_fps;

    if (args.enable_udp) {
        sesg::stream_udp::SesgJpegUdpStreamer::configureJpegChannel(*camera, jpeg_cfg);
    }

    // 启动摄像头数据流
    camera->startStream(Camera::StreamMode::kRefreshOnReturn);

    // ===== 4. 启动 UDP 推流器（仅启用 UDP 时）=====
    auto camera_shared = std::shared_ptr<Camera>(camera, [](Camera*) {});
    sesg::stream_udp::SesgJpegUdpStreamer udp_streamer(args.udp_ip, (uint16_t)args.udp_port);

    if (args.enable_udp) {
        if (!udp_streamer.start(camera_shared, jpeg_cfg, /*init_vpss=*/false)) {
            MA_LOGE(TAG, "UDP 推流器启动失败");
            camera->stopStream();
            camera->deInit();
            return 1;
        }
        std::printf("[OK] UDP 推流器已启动 → %s:%d\n", args.udp_ip.c_str(), args.udp_port);
    }

    // ===== 5. 顺序循环：取帧 → 推理 → 发送（单线程）=====
    std::printf("[hand_gesture] 开始手势识别循环...\n");

    // 性能统计
    int frame_count = 0;
    int infer_count = 0;   // 实际推理帧数（用于计算「推理 FPS」）
    auto stat_start = std::chrono::steady_clock::now();

    // skip_interval 状态：上次推理结果缓存
    std::vector<hand::HandResult> last_results;   // 上次完整推理结果
    int skip_counter = 0;                          // 距上次推理的帧数

    while (g_running.load()) {
        // ---- 1) CH0 取 RGB888 帧（物理地址模式）----
        // 定位「卡死」的首帧心跳：确认 retrieveFrame 是否返回
        static std::atomic<bool> hb_first{false};
        if (!hb_first.exchange(true)) {
            std::printf("[心跳] 首次 retrieveFrame(RGB888) 调用前...\n");
            std::fflush(stdout);
        }
        ma_img_t frame;
        if (camera->retrieveFrame(frame, MA_PIXEL_FORMAT_RGB888) != MA_OK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (!frame.data || frame.size == 0 || frame.width == 0 || frame.height == 0) {
            camera->returnFrame(frame);
            continue;
        }
        {
            static std::atomic<bool> hb2{false};
            if (!hb2.exchange(true)) {
                std::printf("[心跳] retrieveFrame 返回: %dx%d size=%zu data=%p\n",
                            frame.width, frame.height, (size_t)frame.size, (void*)frame.data);
                std::fflush(stdout);
            }
        }

        // 物理地址（零拷贝）：frame.data 即物理地址
        const uint64_t phy_addr = (uint64_t)frame.data;
        const int W = (int)frame.width;
        const int H = (int)frame.height;

        // 决定本帧是否推理（skip_interval）
        // 上次结果手数决定 interval：≥2 只手用 skip_multi，否则 skip_single
        const int last_n = (int)last_results.size();
        const int interval = (last_n >= 2) ? args.skip_multi
                          : (last_n == 1 ? args.skip_single : 1);
        const bool do_infer = (skip_counter == 0);

        std::vector<hand::HandResult> results;
        if (do_infer) {
            // ---- 2a) mmap 物理地址成虚拟地址（det + lm 共用同一次 mmap）----
            // 无硬件缩放场景：det 也需要虚拟地址做软件 letterbox。
            // mmap 只建立页表映射，不复制像素；省掉 640×480×3≈900KB 的逐帧 memcpy。
            void* p = CVI_SYS_Mmap(phy_addr, frame.size);
            uint8_t* vir_addr = reinterpret_cast<uint8_t*>(p);

            // RGB888 常见存在行对齐 padding：优先用 size/height 推断 stride（bytes/row）
            // ⚠️ 关键：若 stride 错误（硬编码 W*3），零拷贝 cv::Mat 会越界读取 → 卡死/段错误
            int stride_bytes = W * 3;
            if (H > 0 && frame.size >= (size_t)H) {
                const size_t s = frame.size / (size_t)H;
                if (s >= (size_t)W * 3) stride_bytes = (int)s;
            }

            // 首帧诊断：打印帧信息 + 前 8 像素 RGB（验证 mmap 成功 + stride 正确）
            static std::atomic<bool> printed_first{false};
            if (vir_addr && !printed_first.exchange(true)) {
                const uint8_t* rgb = vir_addr;
                std::printf("[帧诊断] CH0: %dx%d size=%zu stride=%d bytes | 前8px RGB: ",
                            W, H, (size_t)frame.size, stride_bytes);
                for (int i = 0; i < 8; ++i) {
                    const int off = i * 3;
                    std::printf("(%u,%u,%u) ", rgb[off + 0], rgb[off + 1], rgb[off + 2]);
                }
                std::printf("\n");
            }

            // ---- 2b) 模型 1：Palm 检测（虚拟地址零拷贝 cv::Mat 头 + 软件 letterbox）----
            std::vector<hand::Palm> palms;
            if (vir_addr) {
                palms = detector.detectVirt(vir_addr, W, H, stride_bytes,
                                            args.min_score, 0.3f, 2);
            }

            // 每只手：模型 2（关键点）+ 模型 3/4（手势识别）
            results.reserve(palms.size());
            for (const auto& palm : palms) {
                hand::HandResult hr{};
                hr.palm = palm;

                // 模型 2：21 关键点（虚拟地址零拷贝 cv::Mat 头）
                bool lm_ok = false;
                if (vir_addr) {
                    lm_ok = landmarker.detectVirt(vir_addr, W, H, stride_bytes, palm, hr);
                }

                // 模型 3+4：embedder + classifier（仅关键点有效时）
                if (lm_ok) {
                    hand::HandResult recognized{};
                    if (recognizer.recognize(hr, W, H, recognized)) {
                        results.push_back(recognized);
                    } else {
                        results.push_back(hr);
                    }
                } else {
                    results.push_back(hr);
                }
            }

            // 归还 mmap（若已 mmap）
            if (vir_addr) {
                CVI_SYS_Munmap(vir_addr, frame.size);
            }

            // 缓存本次结果
            last_results = results;
            skip_counter = interval;   // 重置计数器
            infer_count++;
        } else {
            // ---- 2c) 跳过推理，复用上次结果 ----
            results = last_results;
            skip_counter--;
            if (skip_counter < 0) skip_counter = 0;
        }

        // 尽快归还帧（推理或复用完成后）
        camera->returnFrame(frame);

        // ---- 3) stdout 打印检测结果（每 N 帧一次，避免 printf 阻塞）----
        static int print_cnt = 0;
        if ((print_cnt++ % std::max(1, interval)) == 0 && !results.empty()) {
            for (const auto& r : results) {
                const char* gname = hand::kGestureNames[r.gesture_idx];
                const char* hand_side = (r.handedness > 0.5f) ? "R" : "L";
                if (r.landmark_valid) {
                    std::printf("[手势] %s (%.0f%%) [%s] palm=(%.2f,%.2f,%.2f,%.2f) score=%.2f\n",
                                gname, r.gesture_conf * 100.f, hand_side,
                                r.palm.bbox[0], r.palm.bbox[1], r.palm.bbox[2], r.palm.bbox[3],
                                r.palm.score);
                } else {
                    std::printf("[手势] (关键点失败) palm=(%.2f,%.2f,%.2f,%.2f) score=%.2f\n",
                                r.palm.bbox[0], r.palm.bbox[1], r.palm.bbox[2], r.palm.bbox[3],
                                r.palm.score);
                }
            }
        }

        // ---- 4) UDP 推流：本帧（无论推理或复用）都发送最新结果 ----
        std::vector<hand::UDPHandResult> udp_results;
        udp_results.reserve(results.size());
        for (const auto& r : results) {
            udp_results.push_back(to_udp(r));
        }

        if (args.enable_udp) {
            udp_streamer.sendLatest(MAGIC_HAND, udp_results);
        }

        // ---- 5) 性能统计（每秒打印）----
        frame_count++;
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - stat_start).count();
        if (elapsed >= 1000) {
            const double fps = (frame_count * 1000.0) / (double)elapsed;
            const double infer_fps = (infer_count * 1000.0) / (double)elapsed;
            const double det_ms = detector.lastInferenceMs() + detector.lastPostMs();
            const double lm_ms  = landmarker.lastInferenceMs() + landmarker.lastPostMs();
            const double rec_ms = recognizer.lastEmbedderMs() + recognizer.lastClassifierMs();
            std::printf("[性能] FPS=%.2f (推理=%.2f) | palm=%.1fms | landmark=%.1fms | gesture=%.1fms | "
                        "总耗时=%.1fms | 平均手数=%.2f\n",
                        fps, infer_fps, det_ms, lm_ms, rec_ms, det_ms + lm_ms + rec_ms,
                        elapsed > 0 ? (double)results.size() : 0.0);

            frame_count = 0;
            infer_count = 0;
            stat_start = now;
        }
    }

    // ===== 6. 清理 =====
    if (args.enable_udp) {
        udp_streamer.stop(/*deinit_vpss=*/false);
    }
    camera->stopStream();
    camera->deInit();

    std::printf("[hand_gesture] 程序正常退出\n");
    return 0;
}
/*
sudo ./hand_gesture hand_detector_cv181x_int8.cvimodel hand_landmarks_detector_cv181x_int8.cvimodel gesture_embedder_cv181x_int8.cvimodel canned_gesture_classifier_cv181x_int8.cvimodel 0.5 192.168.4.48 5001 320 240 10 3 1
*/
