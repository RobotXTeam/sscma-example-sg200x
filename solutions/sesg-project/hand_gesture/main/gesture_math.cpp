/**
 * @file gesture_math.cpp
 * @brief 公共数学实现：normalize_radians、letterbox、RotatedRect 透视变换。
 *
 */

#include "gesture_math.h"

#include <algorithm>
#include <cmath>

namespace hand {

// 归一化到 [-π, π]（对齐 RectTransformationCalculator::NormalizeRadians，Python L92-94）
//   angle - 2π·floor((angle - (-π)) / (2π))
float normalize_radians(float angle) {
    constexpr float kPi    = 3.14159265358979323846f;
    constexpr float kTwoPi = 6.28318530717958647692f;
    return angle - kTwoPi * std::floor((angle - (-kPi)) / kTwoPi);
}

// Letterbox（对齐 ImageToTensorCalculator keep_aspect_ratio + BORDER_ZERO 居中，Python L252-264）
cv::Mat letterbox(const cv::Mat& src_rgb, int size, LetterboxPad* pad) {
    const int H = src_rgb.rows;
    const int W = src_rgb.cols;
    if (H <= 0 || W <= 0 || size <= 0) {
        if (pad) *pad = {0, 0, 0, 0};
        return cv::Mat::zeros(std::max(1, size), std::max(1, size), CV_8UC3);
    }

    // 等比缩放，使长边恰好 == size
    const float s = (float)size / (float)std::max(H, W);
    const int nh  = (int)std::lround((float)H * s);
    const int nw  = (int)std::lround((float)W * s);

    static int lb_diag = 0;
    if (lb_diag < 2) {
        lb_diag++;
        printf("[LB-DIAG] #%d src=%dx%d size=%d s=%.4f nh=%d nw=%d\n",
               lb_diag, W, H, size, s, nh, nw);
    }
    cv::Mat canvas = cv::Mat::zeros(size, size, CV_8UC3);

    const float sx = (float)nw / (float)W;  // x 缩放比
    const float sy = (float)nh / (float)H;  // y 缩放比
    const float tx = (float)(size - nw) * 0.5f;  // x 平移（居中）
    const float ty = (float)(size - nh) * 0.5f;  // y 平移（居中）

    if (lb_diag <= 2) {
        printf("[LB-DIAG] #%d warpAffine sx=%.4f sy=%.4f tx=%.1f ty=%.1f\n",
               lb_diag, sx, sy, tx, ty);
    }

    // M = [sx  0  tx]
    //     [0  sy  ty]
    cv::Mat M = (cv::Mat_<float>(2, 3) <<
        sx, 0.f, tx,
        0.f, sy, ty);

    cv::warpAffine(src_rgb, canvas, M, cv::Size(size, size),
                   cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));

    if (lb_diag <= 2) {
        // 统计 canvas 非零像素
        int nz = 0, mn = 255, mx = 0;
        double sum = 0;
        for (int y = 0; y < canvas.rows; ++y)
            for (int x = 0; x < canvas.cols; ++x) {
                const uint8_t* p = canvas.ptr<uint8_t>(y) + x * 3;
                for (int c = 0; c < 3; ++c) {
                    int v = p[c]; sum += v;
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                    if (v > 0) nz++;
                }
            }
        printf("[LB-DIAG] #%d canvas %dx%d: nonzero=%d min=%d max=%d mean=%.1f\n",
               lb_diag, canvas.cols, canvas.rows, nz, mn, mx,
               sum / (canvas.rows * canvas.cols * 3.0));
    }

    const int top   = (size - nh) / 2;
    const int left  = (size - nw) / 2;
    if (pad) {
        pad->top    = (float)top / (float)size;
        pad->bottom = (float)(size - top - nh) / (float)size;
        pad->left   = (float)left / (float)size;
        pad->right  = (float)(size - left - nw) / (float)size;
    }
    return canvas;
}
cv::Mat build_rotated_roi_warp(const cv::Mat& src_rgb,
                               float cx_px, float cy_px,
                               float w_px, float h_px,
                               float rot_rad, int size) {
    cv::Mat out = cv::Mat::zeros(size, size, CV_8UC3);
    if (src_rgb.empty() || size <= 0) return out;

    // OpenCV 的 RotatedRect.angle 单位为度，MediaPipe 直接传入 rot*180/π。
    cv::RotatedRect rot_rect(cv::Point2f(cx_px, cy_px),
                             cv::Size2f(w_px, h_px),
                             (float)(rot_rad * 180.0 / CV_PI));

    cv::Point2f src_ptsf[4];
    rot_rect.points(src_ptsf);  // 顺序: bl, tl, tr, br（MediaPipe boxPoints 约定）

    cv::Point2f src_pts[4];
    for (int i = 0; i < 4; ++i) src_pts[i] = src_ptsf[i];

    cv::Point2f dst_pts[4] = {
        cv::Point2f(0.0f,             (float)size),  // bl
        cv::Point2f(0.0f,             0.0f),         // tl
        cv::Point2f((float)size,      0.0f),         // tr
        cv::Point2f((float)size,      (float)size),  // br
    };

    cv::Mat M = cv::getPerspectiveTransform(src_pts, dst_pts);
    cv::warpPerspective(src_rgb, out, M, cv::Size(size, size),
                        cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    return out;
}

}  // namespace hand