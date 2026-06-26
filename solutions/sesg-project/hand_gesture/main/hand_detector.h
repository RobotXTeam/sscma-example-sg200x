/**
 * @file hand_detector.h
 * @brief Palm 检测 (192×192, SSD, 2016 anchors)：anchor 生成 + 解码 + NMS。
 *
 * 严格对齐 PC 端 `test_tflite_rtsp.py` HandDetector（坑⑥最关键）。
 */

#pragma once

 #include <chrono>
 #include <memory>
 #include <opencv2/core.hpp>
 #include <sscma.h>
 #include <utility>
 #include <vector>

#include "engine_utils.h"
#include "gesture_math.h"
#include "hand_types.h"

namespace hand {

class HandDetector {
public:
    HandDetector() = default;
    ~HandDetector() = default;

    bool init(const std::string& model_path);

     // 输入：原图 RGB888（reCamera 取出即 RGB，不要再 BGR→RGB）。
     // 返回最多 max_hands 个 Palm（归一化坐标，已做 letterbox 逆变换）。
     std::vector<Palm> detect(const cv::Mat& rgb,
                              float min_score = 0.5f,
                              float nms_thresh = 0.3f,
                              int max_hands = 2);
 
     // 零拷贝推理：直接传入物理地址（CH0 已由硬件缩放到 input_w_×input_h_）。
     // 避免 CPU memcpy 与 letterbox 软件缩放，交给 CVI 引擎做 DMA 读取。
     // phy_addr: 帧物理地址；W/H: 帧宽高（应与模型输入一致）。
     std::vector<Palm> detectPhys(uint64_t phy_addr, int W, int H,
                                  float min_score = 0.5f,
                                  float nms_thresh = 0.3f,
                                  int max_hands = 2);

     // 虚拟地址推理：传入已 mmap 的 RGB 指针 + stride，内部构造零拷贝 cv::Mat 头并做软件 letterbox。
     // 用于「无硬件缩放」场景（CH0 原生分辨率 640×480 → 模型 192×192）。
     std::vector<Palm> detectVirt(const uint8_t* rgb, int W, int H, int stride_bytes,
                                  float min_score = 0.5f,
                                  float nms_thresh = 0.3f,
                                  int max_hands = 2);

     // 性能统计（单位 ms）
     float lastInferenceMs() const { return infer_ms_; }
     float lastPostMs() const { return post_ms_; }

private:
    // SSD anchor 生成（坑⑥：合并相同 stride 的层）。返回 anchor 数量必须 == 2016。
    static std::vector<std::pair<float, float>> generate_anchors();

    static float calc_scale(float min_scale, float max_scale,
                            int stride_index, int num_strides);

     static std::vector<int> nms(const std::vector<std::array<float, 4>>& xyxy,
                                 const std::vector<float>& scores, float iou_thresh);
 
     bool prepareInputTensor();

     // 通用后处理：读输出 → score/box 解码 → NMS → letterbox 逆变换。
     // t1 为推理结束时间点（用于统计 post_ms_）。
     std::vector<Palm> postProcess(int n_out, float min_score, float nms_thresh,
                                   int max_hands, const LetterboxPad& pad,
                                   bool dbg_log,
                                   std::chrono::time_point<std::chrono::high_resolution_clock> t1);

private:
    std::unique_ptr<ma::engine::EngineCVI> engine_;
    bool inited_ = false;

    int input_h_ = 192;
    int input_w_ = 192;

    ma_tensor_type_t input_type_ = MA_TENSOR_TYPE_NONE;
    bool input_is_chw_ = false;
    size_t input_numel_ = 0;
    ma_tensor_t input_tensor_cache_{};
    InputBuf input_buf_;

    std::vector<std::pair<float, float>> anchors_;  // (cx, cy)，fixed_anchor_size → w=h=1

    float infer_ms_ = 0.f;
    float post_ms_  = 0.f;
};

}  // namespace hand