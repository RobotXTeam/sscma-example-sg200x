/**
 * @file hand_landmarker.h
 * @brief 手部 21 关键点检测 (224×224, 旋转 ROI)。
 *
 * 严格对齐 PC 端 `test_tflite_rtsp.py` HandLandmarker（坑①②③）。
 */

#pragma once

#include <memory>
#include <opencv2/core.hpp>
#include <sscma.h>
#include <utility>
#include <vector>

#include "engine_utils.h"
#include "gesture_math.h"
#include "hand_types.h"

namespace hand {

// 旋转 ROI 计算的中间结果（用于与 Python 对齐调试）。
struct RotatedRectNorm {
    float xc = 0.f;   // 归一化中心 x
    float yc = 0.f;   // 归一化中心 y
    float w  = 0.f;   // 归一化宽
    float h  = 0.f;   // 归一化高
    float rot = 0.f;  // 旋转角 (rad)
};

class HandLandmarker {
public:
    HandLandmarker() = default;
    ~HandLandmarker() = default;

    bool init(const std::string& model_path);

     // 输入：原图 RGB888（reCamera 取出即 RGB，不要再 BGR→RGB）+ 一个 palm 检测结果。
     // 成功时填充 out 并返回 true（landmark_valid=true）。
     bool detect(const cv::Mat& rgb, const Palm& palm, HandResult& out);
 
     // 虚拟地址（mmap 自物理地址）推理：直接传入 CPU 可读的 RGB 指针与 stride。
     // 内部仍需软件裁剪/旋转 ROI（手部 ROI 无法用硬件做），但省掉整帧 memcpy。
     bool detectVirt(const uint8_t* rgb_ptr, int W, int H, int stride_bytes,
                     const Palm& palm, HandResult& out);

     float lastInferenceMs() const { return infer_ms_; }
     float lastPostMs() const { return post_ms_; }

private:
    // 计算旋转角（坑①，Python L310-319）
    static float compute_rotation(const Palm& palm, int img_w, int img_h);

    // Rect 变换（坑②，顺序 shift→square_long→scale，Python L328-349）
    static RotatedRectNorm transform_rect(const Palm& palm, float rot, int img_w, int img_h);

    bool prepareInputTensor();

private:
    std::unique_ptr<ma::engine::EngineCVI> engine_;
    bool inited_ = false;

    int input_h_ = 224;
    int input_w_ = 224;

    ma_tensor_type_t input_type_ = MA_TENSOR_TYPE_NONE;
    bool input_is_chw_ = false;
    size_t input_numel_ = 0;
    ma_tensor_t input_tensor_cache_{};
    InputBuf input_buf_;

    float infer_ms_ = 0.f;
    float post_ms_  = 0.f;
};

}  // namespace hand