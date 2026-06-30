/**
 * @file main.cpp
 * @brief RTMP + YOLO AI IPC：单进程：
 *        摄像头 -> YOLO11n(NPU) 检测 -> 设备端画框(RGN/OSD) -> H.264 -> RTMP 推流。
 *
 * 架构（复用 sscma-example-sg200x 现有能力）：
 *   - 相机 CH0：RGB888 物理地址 -> YOLO11n 推理（CVI NPU，DMA 零拷贝）
 *   - 相机 CH2：H264 -> 检测框走硬件 RGN/OSD 叠加；VENC 输出经 libavformat 封装 FLV 推 RTMP
 *   - RTMP 推流不重编码：VENC 已把检测框烧进 H.264，只做 FLV 封装 + RTMP 发送
 *
 * 用法：
 *   ./rtmp_yolo <yolo11n_detection.cvimodel> <rtmp_url> [threshold] [skip]
 *
 * 示例：
 *   sudo ./rtmp_yolo ./model/yolo11n_detection_cv181x_int8.cvimodel rtmp://192.168.2.113:1935/live/recamera 0.60 2
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>
#include <sys/wait.h>

#include <sscma.h>
#include <video.h>

#include <sesg/stream_rtsp.h>

extern "C" {
#include <app_ipcam_sys.h>
#include <app_ipcam_venc.h>
#include <app_ipcam_vpss.h>
}

#include "coco_labels.h"
#include "detector_utils.h"
#include "sesg/osd_utils.hpp"

using namespace ma;

#define TAG "rtmp_yolo"

static constexpr int DEFAULT_RTSP_PORT = 8554;
static constexpr const char* DEFAULT_RTSP_SESSION = "live";
static constexpr int DEFAULT_ONVIF_PORT = 8080;

// 推流/叠加画布分辨率（与 RTSP 通道一致）。
static constexpr uint32_t OVERLAY_W = 1280;
static constexpr uint32_t OVERLAY_H = 720;

struct LowLatencyDefaults {
    uint32_t vb_blk_num = 3;
    uint32_t vpss_depth = 1;
    uint32_t gop = 25;
    uint32_t bitrate_kbps = 1500;
    VENC_RC_MODE_E rc_mode = VENC_RC_MODE_H264CBR;
};

static void apply_low_latency_defaults(video_ch_index_t ch, uint32_t fps, const LowLatencyDefaults& d) {
    if (auto* sys = app_ipcam_Sys_Param_Get()) {
        if (ch < sys->vb_pool_num) sys->vb_pool[ch].vb_blk_num = d.vb_blk_num;
    }
    if (auto* vpss = app_ipcam_Vpss_Param_Get()) {
        if (vpss->u32GrpCnt > 0) {
            auto& grp0 = vpss->astVpssGrpCfg[0];
            if (ch < VPSS_MAX_PHY_CHN_NUM) grp0.astVpssChnAttr[ch].u32Depth = d.vpss_depth;
        }
    }
    if (auto* venc = app_ipcam_Venc_Param_Get()) {
        if (ch < venc->s32VencChnCnt) {
            auto& vcfg = venc->astVencChnCfg[ch];
            vcfg.u32Gop = d.gop;
            vcfg.enRcMode = d.rc_mode;
            vcfg.u32BitRate = d.bitrate_kbps;
            vcfg.u32MaxBitRate = d.bitrate_kbps;
            vcfg.statTime = 1;
            vcfg.u32SrcFrameRate = fps;
            vcfg.u32DstFrameRate = fps;
        }
    }
}

static std::atomic<bool> g_running(true);
static void sig_handler(int) { g_running.store(false); }

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage:\n");
        printf("  %s <yolo11n_detection.cvimodel> <rtmp_url> [threshold] [skip]\n", argv[0]);
        printf("    rtmp_url     : e.g. rtmp://192.168.2.113:1935/live/recamera\n");
        printf("    threshold    : detection threshold (default 0.60)\n");
        printf("    skip         : infer every N frames (default 2)\n");
        printf("\nExample:\n");
        printf("  sudo %s ./model/yolo11n_detection_cv181x_int8.cvimodel rtmp://192.168.2.113:1935/live/recamera 0.60 2\n", argv[0]);
        return -1;
    }

    const char* yolo_model_path = argv[1];
    const char* rtmp_url = argv[2];
    float threshold = (argc > 3) ? atof(argv[3]) : 0.60f;
    int skip_interval = (argc > 4) ? atoi(argv[4]) : 2;
    if (skip_interval < 1) skip_interval = 1;

    printf("\n========== RTMP + YOLO AI IPC ==========\n");
    printf("YOLO model : %s\n", yolo_model_path);
    printf("RTMP url   : %s\n", rtmp_url);
    printf("threshold  : %.2f | skip: %d\n", threshold, skip_interval);
    printf("========================================\n\n");

    // ---------- 初始化 YOLO 引擎 ----------
    auto* yolo_engine = new ma::engine::EngineCVI();
    if (yolo_engine->init() != MA_OK || yolo_engine->load(yolo_model_path) != MA_OK) {
        MA_LOGE(TAG, "yolo engine init/load failed: %s", yolo_model_path);
        return 1;
    }

    int input_w = 0, input_h = 0;
    {
        auto s = yolo_engine->getInputShape(0);
        if (s.size == 4) {
            if (s.dims[3] == 3 || s.dims[3] == 1) { input_h = s.dims[1]; input_w = s.dims[2]; }
            else { input_h = s.dims[2]; input_w = s.dims[3]; }
        }
        if (input_w <= 0 || input_h <= 0) { input_w = 640; input_h = 640; }
    }
    printf("[%s] model input = %dx%d\n", TAG, input_w, input_h);

    // YOLO11n detection 是多输出模型，走 SSCMA 原生 detector 后处理。
    ma::Model* yolo_model = ma::ModelFactory::create(yolo_engine);
    if (!yolo_model) {
        MA_LOGE(TAG, "ModelFactory::create failed (need detector bbox model)");
        return 1;
    }
    if (yolo_model->getInputType() != MA_INPUT_TYPE_IMAGE || yolo_model->getOutputType() != MA_OUTPUT_TYPE_BBOX) {
        MA_LOGE(TAG, "model is not a detector bbox model");
        return 1;
    }
    auto* detector = static_cast<ma::model::Detector*>(yolo_model);
    detector->setConfig(MA_MODEL_CFG_OPT_THRESHOLD, threshold);
    auto* square_detector = new detector_utils::SquareDetector(detector, input_w, input_h);
    printf("[%s] YOLO11n detector ready (SSCMA multi-output)\n", TAG);

    // ---------- 初始化相机 ----------
    Device* device = Device::getInstance();
    Camera* camera = nullptr;
    for (auto& sensor : device->getSensors()) {
        if (sensor->getType() == ma::Sensor::Type::kCamera) {
            camera = static_cast<Camera*>(sensor);
            camera->init(0);
            Camera::CtrlValue val;
            val.i32 = 0;  // CH0 用于推理
            camera->commandCtrl(Camera::CtrlType::kChannel, Camera::CtrlMode::kWrite, val);
            val.u16s[0] = input_w;
            val.u16s[1] = input_h;
            camera->commandCtrl(Camera::CtrlType::kWindow, Camera::CtrlMode::kWrite, val);
            val.i32 = 1;  // 物理地址（DMA 零拷贝喂 NPU）
            camera->commandCtrl(Camera::CtrlType::kPhysical, Camera::CtrlMode::kWrite, val);
            break;
        }
    }
    if (!camera) { MA_LOGE(TAG, "No camera"); return 1; }

    LowLatencyDefaults ll;
    apply_low_latency_defaults(VIDEO_CH0, 25, ll);
    apply_low_latency_defaults(VIDEO_CH2, 25, ll);

    // ---------- 启动 VPSS/VENC/RGN 管线（attached 模式，CH2 H264 + 硬件画框）----------
    // 复用 SesgRtspVpssStreamer 来配置 VENC 通道和 RGN/OSD 叠加；它内部也会起一个
    // RTSP server（端口 8554），这里作为额外可选输出，主输出是 RTMP。
    sesg::stream_rtsp::SesgRtspVpssStreamer streamer;
    bool pipe_ok = false;
    {
        sesg::stream_rtsp::ServerConfig server_cfg;
        server_cfg.port = DEFAULT_RTSP_PORT;

        sesg::stream_rtsp::ChannelConfig ch2;
        ch2.ch = VIDEO_CH2;
        ch2.format = VIDEO_FORMAT_H264;
        ch2.width = OVERLAY_W;
        ch2.height = OVERLAY_H;
        ch2.fps = 25;
        ch2.enable_rgn_overlay = true;   // 设备端硬件画框
        ch2.path = DEFAULT_RTSP_SESSION;

        if (streamer.startAttached(server_cfg, {ch2}, 1)) {
            pipe_ok = true;
            printf("[%s] VENC/RGN pipeline up (aux RTSP: rtsp://%s:%d/%s)\n",
                   TAG, "<device-ip>", DEFAULT_RTSP_PORT, DEFAULT_RTSP_SESSION);
        } else {
            MA_LOGE(TAG, "streamer.startAttached failed");
            return 1;
        }
    }

    // ---------- RTMP 推流说明 ----------
    // 设备运行时 libavformat 为裁剪版（FLV/AVCC 序列头不完整），不在进程内推 RTMP；
    // 改由设备自带的 ffmpeg 可执行文件做 RTSP->RTMP 转封装（-c copy，不重编码）。
    // run_rtmp.sh 会在本程序起来后自动拉起该 ffmpeg relay。
    printf("[%s] aux RTSP ready. RTMP target = %s\n", TAG, rtmp_url);
    printf("[%s] relay cmd: ffmpeg -rtsp_transport tcp -i rtsp://127.0.0.1:%d/%s -c copy -f flv %s\n",
           TAG, DEFAULT_RTSP_PORT, DEFAULT_RTSP_SESSION, rtmp_url);


    camera->startStream(Camera::StreamMode::kRefreshOnReturn);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // ---------- 主循环 ----------
    printf("\n[%s] running. RTMP -> %s\n\n", TAG, rtmp_url);

    int frame_counter = 0;
    int infer_count = 0;
    auto fps_start = std::chrono::high_resolution_clock::now();
    int fps_frame_count = 0;
    double win_inf_ms = 0;
    int win_infer = 0;
    size_t last_det_count = 0;

    while (g_running.load()) {
        ma_img_t frame;
        if (camera->retrieveFrame(frame, MA_PIXEL_FORMAT_RGB888) != MA_OK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        frame_counter++;
        fps_frame_count++;

        const bool do_inference = (skip_interval <= 1) || (frame_counter % skip_interval == 0);
        if (do_inference) {
            // 物理地址直喂 NPU，零拷贝。
            ma_tensor_t tensor = {};
            tensor.size = frame.size;
            tensor.is_physical = true;
            tensor.is_variable = false;
            tensor.data.data = reinterpret_cast<void*>(frame.data);
            yolo_engine->setInput(0, tensor);

            std::vector<detector_utils::Detection> results;
            detector_utils::PerfStats perf = square_detector->run(frame.data, frame.size, results);

            // 设备端硬件叠加检测框（同时影响 RTMP 和辅助 RTSP 输出）。
            if (pipe_ok) {
                std::vector<sesg::stream_rtsp::OverlayRect> rects;
                sesg::osd::build_overlay_rects(results, rects, [](int target) { return coco_label(target); }, OVERLAY_W, OVERLAY_H);
                (void)streamer.updateOverlayRects(VIDEO_CH2, rects);
            }

            // 诊断：定期打印原始检测坐标和类别（排查框对齐问题）。
            if (infer_count % 40 == 0 && !results.empty()) {
                for (size_t i = 0; i < results.size() && i < 6; ++i) {
                    const auto& d = results[i];
                    printf("[det] cls=%d(%s) score=%.2f cx=%.3f cy=%.3f w=%.3f h=%.3f\n",
                           d.target, coco_label(d.target), d.score, d.x, d.y, d.w, d.h);
                }
            }

            infer_count++;
            win_infer++;
            win_inf_ms += perf.inference_ms;
            last_det_count = results.size();
        }

        camera->returnFrame(frame);

        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - fps_start).count();
        if (elapsed >= 2000) {
            float video_fps = fps_frame_count * 1000.0f / elapsed;
            printf("[%s] FPS=%.1f | infer=%d (avg %.1f ms) | last detections=%zu\n",
                   TAG, video_fps, win_infer,
                   win_infer > 0 ? win_inf_ms / win_infer : 0.0, last_det_count);
            win_infer = 0;
            win_inf_ms = 0;
            fps_frame_count = 0;
            fps_start = now;
        }
    }

    // ---------- 清理 ----------
    printf("\n[%s] stopping...\n", TAG);
    if (pipe_ok) streamer.stopAttached();
    camera->stopStream();
    for (auto& sensor : device->getSensors()) sensor->deInit();
    delete square_detector;
    ma::ModelFactory::remove(yolo_model);
    delete yolo_engine;
    printf("[%s] done\n", TAG);
    return 0;
}
