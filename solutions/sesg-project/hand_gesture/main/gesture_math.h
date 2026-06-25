/**
 * @file gesture_math.h
 * @brief 公共数学：normalize_radians、letterbox、RotatedRect 透视变换。
 *
 * 严格对齐 PC 端 `test_tflite_rtsp.py`：
 *   - normalize_radians         (Python L92-94)
 *   - letterbox(keep_aspect + 居中黑边)  (Python L252-264)
 *   - build_rotated_roi_warp    (Python L354-378, image_to_tensor_converter_opencv.cc)
 *
 * 使用 OpenCV（cv::RotatedRect / getPerspectiveTransform / warpPerspective）以保证
 * 与 Python 端数值完全一致。
 */

#pragma once

#include <cstdint>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <utility>

namespace hand {

// 归一化到 [-π, π]（对齐 RectTransformationCalculator::NormalizeRadians，Python L92-94）
float normalize_radians(float angle);

// Letterbox 的归一化 padding：返回 (top, bottom, left, right) 均为相对 size 的比例。
// 对齐 ImageToTensorCalculator（keep_aspect_ratio + BORDER_ZERO 居中，Python L252-264）。
struct LetterboxPad {
    float top;
    float bottom;
    float left;
    float right;
};
// 把 src(HxW RGB888) 等比缩放居中放入 size×size 黑边画布，返回画布 + padding 比例。
cv::Mat letterbox(const cv::Mat& src_rgb, int size, LetterboxPad* pad);

// 构建旋转 ROI 的透视变换并 warp 到 size×size。
// 严格对齐 image_to_tensor_converter_opencv.cc / Python L354-378：
//   cv::RotatedRect(center, (w_px,h_px), rot*180/π) → boxPoints 得 src 四角，
//   dst 四角固定为 [[0,size],[0,0],[size,0],[size,size]] (bl,tl,tr,br)。
// 返回 warp 后的 size×size RGB888。
cv::Mat build_rotated_roi_warp(const cv::Mat& src_rgb,
                               float cx_px, float cy_px,
                               float w_px, float h_px,
                               float rot_rad, int size);

}  // namespace hand