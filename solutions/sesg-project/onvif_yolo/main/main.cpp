/**
 * @file main.cpp
 * @brief ONVIF + YOLO AI IPC：单进程同时提供
 *        1) 摄像头 -> YOLO11n(NPU) 检测 -> 设备端画框 -> H.264 -> RTSP 推流；
 *        2) ONVIF 协议（WS-Discovery + Device/Media SOAP），让本机表现为标准 ONVIF IP 摄像机。
 *
 * 架构（复用 sscma-example-sg200x 现有能力）：
 *   - 相机 CH0：RGB888 物理地址 -> YOLO11n 推理（CVI NPU，DMA 零拷贝）
 *   - 相机 CH2：H264 -> RTSP 推流（SesgRtspVpssStreamer），检测框走硬件 RGN/OSD 叠加
 *   - ONVIF：OnvifService 后台线程（UDP 3702 发现 + mongoose SOAP）
 *
 * 用法：
 *   ./onvif_yolo <yolo11n_detection.cvimodel> [threshold] [skip] [rtsp_port] [rtsp_session] [onvif_port]
 *
 * 示例：
 *   sudo ./onvif_yolo ./model/yolo11n_detection_cv181x_int8.cvimodel 0.60 2 8554 live 8080
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
#include "font5x7.h"
#include "onvif_service.h"

using namespace ma;

#define TAG "onvif_yolo"

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

// 按类别 id 取一个稳定的醒目颜色（ARGB8888）。
static inline uint32_t color_for_class(int cls) {
    static const uint32_t kPalette[8] = {
        0xFFFF3030u, 0xFF30FF30u, 0xFF3080FFu, 0xFFFFD030u,
        0xFFFF30FFu, 0xFF30FFFFu, 0xFFFF8030u, 0xFF80FF30u,
    };
    if (cls < 0) cls = 0;
    return kPalette[cls % 8];
}

// 追加一个"填充小矩形"到 overlay 列表（归一化坐标）。利用 thickness≈min(w,h) 让边框退化成填充。
static inline void push_filled_px(std::vector<sesg::stream_rtsp::OverlayRect>& out,
                                  int px, int py, int pw, int ph, uint32_t argb) {
    if (pw <= 0 || ph <= 0) return;
    if (px >= (int)OVERLAY_W || py >= (int)OVERLAY_H) return;
    if (px + pw <= 0 || py + ph <= 0) return;
    sesg::stream_rtsp::OverlayRect r;
    r.x = (float)px / (float)OVERLAY_W;
    r.y = (float)py / (float)OVERLAY_H;
    r.w = (float)pw / (float)OVERLAY_W;
    r.h = (float)ph / (float)OVERLAY_H;
    r.argb = argb;
    r.thickness = (uint16_t)std::max(1, std::min(pw, ph));  // 退化成填充
    out.push_back(r);
}

// 在 (px,py) 处用 5x7 点阵渲染一段文字（每个点放大成 scale×scale 像素块）。
// 返回渲染宽度（像素）。文字直接以填充小矩形进 RGN overlay。
static int draw_text_px(std::vector<sesg::stream_rtsp::OverlayRect>& out, int px, int py,
                        const char* text, uint32_t argb, int scale) {
    int cx = px;
    for (const char* p = text; *p; ++p) {
        const uint8_t* cols = font5x7::columns(*p);
        for (int c = 0; c < 5; ++c) {
            for (int row = 0; row < 7; ++row) {
                if (cols[c] & (1u << row)) {
                    push_filled_px(out, cx + c * scale, py + row * scale, scale, scale, argb);
                }
            }
        }
        cx += 6 * scale;  // 5 列 + 1 列间距
    }
    return cx - px;
}

// 把一帧检测结果转换成 RTSP 通道的 RGN/OSD：检测框 + 类别文字标签。
static void build_overlay_rects(const std::vector<detector_utils::Detection>& dets,
                                std::vector<sesg::stream_rtsp::OverlayRect>& out) {
    out.clear();
    for (const auto& d : dets) {
        const float x1 = d.x - d.w * 0.5f;
        const float y1 = d.y - d.h * 0.5f;

        sesg::stream_rtsp::OverlayRect r;
        r.x = std::clamp(x1, 0.0f, 1.0f);
        r.y = std::clamp(y1, 0.0f, 1.0f);
        r.w = std::clamp(d.w, 0.0f, 1.0f);
        r.h = std::clamp(d.h, 0.0f, 1.0f);
        const uint32_t col = color_for_class(d.target);
        r.argb = col;
        r.thickness = 3;

        if (r.w <= 0.0f || r.h <= 0.0f) continue;
        if (r.x >= 1.0f || r.y >= 1.0f) continue;
        if (r.x + r.w <= 0.0f || r.y + r.h <= 0.0f) continue;
        out.push_back(r);

        // 文字标签："CLASS NN"（类别名 + 置信度百分比），画在框左上角上方。
        char label[40];
        int pct = (int)(d.score * 100.0f + 0.5f);
        if (pct > 99) pct = 99;
        snprintf(label, sizeof(label), "%s %d", coco_label(d.target), pct);

        const int scale = 2;            // 点阵放大倍数
        const int text_h = 7 * scale;   // 文字高度
        int tx = (int)(r.x * OVERLAY_W) + 1;
        int ty = (int)(r.y * OVERLAY_H) - text_h - 2;  // 框上方
        if (ty < 0) ty = (int)(r.y * OVERLAY_H) + 2;    // 贴顶时改画框内上沿

        // 文字底色条（不透明深色，ARGB1555 只有 1bit alpha，必须用不透明），提升可读性。
        int label_w = (int)strlen(label) * 6 * scale;
        push_filled_px(out, tx - 1, ty - 1, label_w + 2, text_h + 2, 0xFF101010u);
        draw_text_px(out, tx, ty, label, col, scale);
    }
}
int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage:\n");
        printf("  %s <yolo11n_detection.cvimodel> [threshold] [skip] [rtsp_port] [rtsp_session] [onvif_port]\n", argv[0]);
        printf("    threshold    : detection threshold (default 0.60)\n");
        printf("    skip         : infer every N frames (default 2)\n");
        printf("    rtsp_port    : RTSP server port (default 8554)\n");
        printf("    rtsp_session : RTSP path (default live)\n");
        printf("    onvif_port   : ONVIF SOAP HTTP port (default 8080)\n");
        printf("\nExample:\n");
        printf("  sudo %s ./model/yolo11n_detection_cv181x_int8.cvimodel 0.60 2 8554 live 8080\n", argv[0]);
        return -1;
    }

    const char* yolo_model_path = argv[1];
    float threshold = (argc > 2) ? atof(argv[2]) : 0.60f;
    int skip_interval = (argc > 3) ? atoi(argv[3]) : 2;
    int rtsp_port = (argc > 4) ? atoi(argv[4]) : DEFAULT_RTSP_PORT;
    const char* rtsp_session = (argc > 5 && strlen(argv[5]) > 0) ? argv[5] : DEFAULT_RTSP_SESSION;
    int onvif_port = (argc > 6) ? atoi(argv[6]) : DEFAULT_ONVIF_PORT;
    if (skip_interval < 1) skip_interval = 1;

    printf("\n========== ONVIF + YOLO AI IPC ==========\n");
    printf("YOLO model : %s\n", yolo_model_path);
    printf("threshold  : %.2f | skip: %d\n", threshold, skip_interval);
    printf("RTSP       : :%d/%s\n", rtsp_port, rtsp_session);
    printf("ONVIF SOAP : :%d\n", onvif_port);
    printf("=========================================\n\n");

    // ---------- 解析对外 IP（用于回填 ONVIF XAddr / Stream URI）----------
    // 优先用环境变量 DEVICE_IP，否则探测默认网卡地址；最终回退到 reCamera USB 默认网关。
    std::string device_ip;
    if (const char* env_ip = getenv("DEVICE_IP")) device_ip = env_ip;
    if (device_ip.empty()) {
        FILE* fp = popen("ip -4 -o addr show 2>/dev/null | awk '!/ lo /{sub(/\\/.*/,\"\",$4); print $4; exit}'", "r");
        if (fp) {
            char ipbuf[64] = {0};
            if (fgets(ipbuf, sizeof(ipbuf), fp)) {
                ipbuf[strcspn(ipbuf, "\r\n")] = 0;
                device_ip = ipbuf;
            }
            pclose(fp);
        }
    }
    if (device_ip.empty()) device_ip = "192.168.42.1";
    printf("[%s] device_ip = %s\n", TAG, device_ip.c_str());

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

    // ---------- 启动 RTSP（复用同一套 VI/VPSS/VENC，CH2 H264）----------
    sesg::stream_rtsp::SesgRtspVpssStreamer streamer;
    bool rtsp_ok = false;
    {
        sesg::stream_rtsp::ServerConfig server_cfg;
        server_cfg.port = rtsp_port;

        sesg::stream_rtsp::ChannelConfig ch2;
        ch2.ch = VIDEO_CH2;
        ch2.format = VIDEO_FORMAT_H264;
        ch2.width = OVERLAY_W;
        ch2.height = OVERLAY_H;
        ch2.fps = 25;
        ch2.enable_rgn_overlay = true;   // 设备端硬件画框
        ch2.path = rtsp_session;

        if (streamer.startAttached(server_cfg, {ch2}, 1)) {
            rtsp_ok = true;
            printf("[%s] RTSP streaming: rtsp://%s:%d/%s\n", TAG, device_ip.c_str(), rtsp_port, rtsp_session);
        } else {
            MA_LOGE(TAG, "streamer.startAttached failed (port=%d session=%s)", rtsp_port, rtsp_session);
        }
    }

    // ---------- 启动 ONVIF 服务 ----------
    onvif::OnvifService onvif_svc;
    {
        onvif::OnvifConfig oc;
        oc.device_ip = device_ip;
        oc.onvif_port = onvif_port;
        oc.rtsp_port = rtsp_port;
        oc.rtsp_session = rtsp_session;
        oc.width = OVERLAY_W;
        oc.height = OVERLAY_H;
        oc.fps = 25;
        onvif_svc.start(oc);
    }

    camera->startStream(Camera::StreamMode::kRefreshOnReturn);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // ---------- 主循环 ----------
    printf("\n[%s] running. RTSP: rtsp://%s:%d/%s | ONVIF discovery on udp/3702, SOAP on :%d\n\n",
           TAG, device_ip.c_str(), rtsp_port, rtsp_session, onvif_port);

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

            // 设备端叠加检测框到 RTSP 画面。
            if (rtsp_ok) {
                std::vector<sesg::stream_rtsp::OverlayRect> rects;
                build_overlay_rects(results, rects);
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
    onvif_svc.stop();
    if (rtsp_ok) streamer.stopAttached();
    camera->stopStream();
    for (auto& sensor : device->getSensors()) sensor->deInit();
    delete square_detector;
    ma::ModelFactory::remove(yolo_model);
    delete yolo_engine;
    printf("[%s] done\n", TAG);
    return 0;
}
