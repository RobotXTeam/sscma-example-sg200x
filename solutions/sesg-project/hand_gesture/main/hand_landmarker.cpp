/**
 * @file hand_landmarker.cpp
 * @brief 手部 21 关键点检测后处理实现（旋转角 + Rect 变换 + 透视变换 + 关键点投影）。
 *
 * 严格对齐 PC 端 test_tflite_rtsp.py HandLandmarker：
 *   - compute_rotation  (Python L310-319, 坑①: dy 必须带负号)
 *   - transform_rect    (Python L328-349, 坑②: shift→square_long→scale)
 *   - build_rotated_roi_warp (Python L354-378)
 *   - 输出读取           (Python L382-390)
 *   - 关键点归一化 + 投影 (Python L392-409, 坑③)
 */

#include "hand_landmarker.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace hand {

// ===== 1. 计算旋转角（DetectionsToRectsCalculator::ComputeRotation，Python L310-319）=====
// rotation_vector: wrist(0) → middle_mcp(2), target_angle=90°
float HandLandmarker::compute_rotation(const Palm& palm, int img_w, int img_h) {
    // 源码先把关键点转成像素坐标再算 atan2
    const float x0 = palm.kpts[0][0] * (float)img_w;  // wrist x
    const float y0 = palm.kpts[0][1] * (float)img_h;  // wrist y
    const float x1 = palm.kpts[2][0] * (float)img_w;  // middle_mcp x
    const float y1 = palm.kpts[2][1] * (float)img_h;  // middle_mcp y

    constexpr float kPi = 3.14159265358979323846f;
    // Y 轴朝下，dy 必须带负号：atan2(-(y1-y0), (x1-x0))
    const float rot = normalize_radians(kPi * 0.5f -
                                        std::atan2(-(y1 - y0), (x1 - x0)));
    return rot;
}

//  顺序必须为: shift(用原始 w/h) → square_long → scale
RotatedRectNorm HandLandmarker::transform_rect(const Palm& palm, float rot,
                                               int img_w, int img_h) {
    // ===== 2. Palm box → 归一化 rect（DetectionsToRectsCalculator, USE_BOUNDING_BOX）=====
    const float bx1 = palm.bbox[0];
    const float by1 = palm.bbox[1];
    const float bx2 = palm.bbox[2];
    const float by2 = palm.bbox[3];
    float width  = bx2 - bx1;            // 归一化宽
    float height = by2 - by1;            // 归一化高
    float xc = (bx1 + bx2) * 0.5f;       // 归一化中心
    float yc = (by1 + by2) * 0.5f;

    const float W = (float)img_w;
    const float H = (float)img_h;

    // shift（TransformNormalizedRect 的 rotation!=0 分支，考虑图像宽高比）
    const float SHIFT_X = 0.0f;
    const float SHIFT_Y = -0.5f;
    const float cos_r = std::cos(rot);
    const float sin_r = std::sin(rot);

    const float x_shift = (W * width * SHIFT_X * cos_r - H * height * SHIFT_Y * sin_r) / W;
    const float y_shift = (W * width * SHIFT_X * sin_r + H * height * SHIFT_Y * cos_r) / H;
    xc += x_shift;
    yc += y_shift;

    // square_long（像素空间取长边，再除回归一化）
    const float long_side = std::max(width * W, height * H);
    width  = long_side / W;
    height = long_side / H;

    // scale
    constexpr float SCALE_X = 2.6f;
    constexpr float SCALE_Y = 2.6f;
    width  *= SCALE_X;
    height *= SCALE_Y;

    RotatedRectNorm r;
    r.xc = xc;
    r.yc = yc;
    r.w  = width;
    r.h  = height;
    r.rot = rot;
    return r;
}

bool HandLandmarker::prepareInputTensor() {
    input_tensor_cache_ = engine_->getInput(0);
    input_type_ = input_tensor_cache_.type;

    const ma_shape_t s = engine_->getInputShape(0);
    if (s.size != 4) return false;

    if (s.dims[1] == 3 || s.dims[1] == 1) {
        input_is_chw_ = true;
        input_h_ = s.dims[2];
        input_w_ = s.dims[3];
    } else if (s.dims[3] == 3 || s.dims[3] == 1) {
        input_is_chw_ = false;
        input_h_ = s.dims[1];
        input_w_ = s.dims[2];
    } else {
        input_is_chw_ = true;
        input_h_ = s.dims[2];
        input_w_ = s.dims[3];
    }
    input_numel_ = shape_numel(s);
    if (input_numel_ == 0) return false;
    input_buf_.resize_for(input_type_, input_numel_);
    return true;
}

bool HandLandmarker::init(const std::string& model_path) {
    engine_ = std::make_unique<ma::engine::EngineCVI>();
    if (engine_->init() != MA_OK) return false;
    if (engine_->load(model_path) != MA_OK) return false;
    if (!prepareInputTensor()) return false;
    inited_ = true;
    return true;
}

 // 虚拟地址（mmap 自物理地址）推理：构造零拷贝 cv::Mat 头，委托给 detect()。
 // 内部 warpPerspective 直接读取 mmap 内存，省掉整帧 memcpy。
 bool HandLandmarker::detectVirt(const uint8_t* rgb_ptr, int W, int H, int stride_bytes,
                                 const Palm& palm, HandResult& out) {
     infer_ms_ = 0.f;
     post_ms_ = 0.f;
     if (!inited_ || !engine_ || !rgb_ptr || W <= 0 || H <= 0) return false;
     // 构造零拷贝 cv::Mat 头（不复制数据，仅包装指针）
     cv::Mat rgb(H, W, CV_8UC3, const_cast<uint8_t*>(rgb_ptr), (size_t)stride_bytes);
     if (rgb.empty()) return false;
     return detect(rgb, palm, out);
 }
 
 bool HandLandmarker::detect(const cv::Mat& rgb, const Palm& palm, HandResult& out) {
     infer_ms_ = 0.f;
     post_ms_ = 0.f;
     if (!inited_ || !engine_ || rgb.empty()) return false;

    const int H = rgb.rows;
    const int W = rgb.cols;
    if (H <= 0 || W <= 0) return false;

    auto t0 = std::chrono::high_resolution_clock::now();

    // 调试日志降频计数器（每 100 帧打印一次，避免 printf 阻塞推理）
    static int dbg_cnt = 0;
    const bool dbg_log = ((dbg_cnt++ % 100) == 0);

    // ===== 1. 计算旋转角（坑①，Python L310-319）=====
    const float rot = compute_rotation(palm, W, H);

    // ===== 2/3. Rect 变换（坑②，Python L328-349）=====
    const RotatedRectNorm rect = transform_rect(palm, rot, W, H);

    // ===== 4. 构建透视变换（Python L354-378, image_to_tensor_converter_opencv.cc）=====
    //   cv::RotatedRect(center, (w_px,h_px), rot*180/π) → boxPoints → dst 四角
    const float cx_px = rect.xc * (float)W;
    const float cy_px = rect.yc * (float)H;
    const float w_px  = rect.w  * (float)W;
    const float h_px  = rect.h  * (float)H;
    cv::Mat roi = build_rotated_roi_warp(rgb, cx_px, cy_px, w_px, h_px, rot, input_w_);
    auto t_warp = std::chrono::high_resolution_clock::now();

    // ===== 5. 按 dtype 打包输入（Python: roi = (roi/255)[None]）=====
    //    注意：reCamera 取出即 RGB，无需 BGR→RGB 转换（与 Python 端 cv2 BGR 不同！）
    //    当输入是 U8/S8 且 quant_param
    //    为默认值时，说明 TPU 侧 fuse 了预处理，应走 raw passthrough（填原始像素）。
    //    否则做 /255 归一化再量化回 U8 会把输入压成接近全 0 → 输出恒定/无效。
    const int Hi = input_h_;
    const int Wi = input_w_;
    const int C = 3;
    const ma_quant_param_t qp = input_tensor_cache_.quant_param;
    const bool input_is_int = (input_type_ == MA_TENSOR_TYPE_U8 || input_type_ == MA_TENSOR_TYPE_S8);
    const bool quant_defaultish = (!std::isfinite(qp.scale) || qp.scale <= 0.f ||
                                   (std::fabs(qp.scale - 1.0f) < 1e-6f && qp.zero_point == 0));
    const bool use_raw_passthrough = input_is_int && quant_defaultish;

    for (int y = 0; y < Hi; ++y) {
        for (int x = 0; x < Wi; ++x) {
            const uint8_t* p = roi.ptr<uint8_t>(y) + x * 3;
            for (int c = 0; c < C; ++c) {
                size_t idx;
                if (input_is_chw_) {
                    idx = (size_t)c * (size_t)Hi * (size_t)Wi + (size_t)y * (size_t)Wi + (size_t)x;
                } else {
                    idx = ((size_t)y * (size_t)Wi + (size_t)x) * (size_t)C + (size_t)c;
                }
                if (use_raw_passthrough) {
                    switch (input_type_) {
                        case MA_TENSOR_TYPE_U8: input_buf_.u8[idx] = p[c]; break;
                        case MA_TENSOR_TYPE_S8: input_buf_.s8[idx] = (int8_t)(std::max(-128, std::min(127, (int)p[c] - 128))); break;
                        default: break;
                    }
                } else {
                    const float real = (float)p[c] / 255.0f;
                    store_val(input_buf_, input_type_, qp, idx, real);
                }
            }
        }
    }
    auto t_pack = std::chrono::high_resolution_clock::now();

    ma_tensor_t tensor = make_input_tensor(input_type_, input_buf_, input_numel_);
    engine_->setInput(0, tensor);
    engine_->run();
    auto t_run = std::chrono::high_resolution_clock::now();

    auto t1 = std::chrono::high_resolution_clock::now();
    infer_ms_ = std::chrono::duration<float, std::milli>(t1 - t0).count();

    // 细化计时（每 100 帧打印，诊断 CPU vs TPU 瓶颈）
    if (dbg_log) {
        const float warp_ms = std::chrono::duration<float, std::milli>(t_warp - t0).count();
        const float pack_ms = std::chrono::duration<float, std::milli>(t_pack - t_warp).count();
        const float run_ms  = std::chrono::duration<float, std::milli>(t_run - t_pack).count();
        const float out_ms  = std::chrono::duration<float, std::milli>(t1 - t_run).count();
        std::printf("  [LM-TIME] warpPerspective=%.1fms pack=%.1fms TPU_run=%.1fms readout=%.1fms\n",
                    warp_ms, pack_ms, run_ms, out_ms);
    }

    // ===== 6. 读取输出（Python L382-390）=====
    //   outs[0] landmarks [1,21,3]（224 像素系）
    //   outs[1] hand_flag  [1,1]
    //   outs[2] handedness_raw [1,1] → sigmoid 得 P(right)
    //   outs[3] world [1,21,3]（米）
    const int n_out = engine_->getOutputSize();
    if (n_out < 4) {
        // 至少需要 landmarks + hand_flag + handedness + world
        return false;
    }

    // 读 landmarks（第 0 个输出，21*3=63 个值，224 系）
    const ma_tensor_t t_lm = engine_->getOutput(0);
    float landmarks[21 * 3];
    for (int i = 0; i < 21 * 3; ++i) landmarks[i] = read_val(t_lm, i);

    // hand_flag（第 1 个输出，1 个值）
    const ma_tensor_t t_flag = engine_->getOutput(1);
    const float hand_flag = read_val(t_flag, 0);

    // handedness_raw（第 2 个输出，1 个值 → sigmoid）
    const ma_tensor_t t_hand = engine_->getOutput(2);
    const float handedness_raw = read_val(t_hand, 0);

    // world（第 3 个输出，21*3=63 个值，米）
    const ma_tensor_t t_world = engine_->getOutput(3);
    float world[21 * 3];
    for (int i = 0; i < 21 * 3; ++i) world[i] = read_val(t_world, i);

    // ===== 7. handedness: sigmoid → P(right hand)（Python L390）=====
    //   binary_classification → sigmoid；handedness_raw > -100 才算有效
    float handedness = 0.f;
    if (handedness_raw > -100.f) {
        handedness = 1.0f / (1.0f + std::exp(-handedness_raw));
    }

    // ===== 8. TensorsToLandmarksCalculator（归一化到 ROI 局部，Python L392-396）=====
    //   lx=x/224, ly=y/224, lz=z/224/0.4
    constexpr float NORMALIZE_Z = 0.4f;
    float lx[21], ly[21], lz[21];
    for (int i = 0; i < 21; ++i) {
        lx[i] = landmarks[i * 3 + 0] / 224.0f;
        ly[i] = landmarks[i * 3 + 1] / 224.0f;
        lz[i] = landmarks[i * 3 + 2] / 224.0f / NORMALIZE_Z;
    }

    // ===== 9. LandmarkProjectionCalculator（NORM_RECT 分支，square ROI，Python L400-409, 坑③）=====
    //   new_x = (cos·(x-0.5) - sin·(y-0.5)) * width   + xc
    //   new_y = (sin·(x-0.5) + cos·(y-0.5)) * height  + yc
    //   new_z = z * width
    const float cos_r = std::cos(rot);
    const float sin_r = std::sin(rot);
    for (int i = 0; i < 21; ++i) {
        const float xl = lx[i] - 0.5f;
        const float yl = ly[i] - 0.5f;
        const float nx = (cos_r * xl - sin_r * yl) * rect.w + rect.xc;
        const float ny = (sin_r * xl + cos_r * yl) * rect.h + rect.yc;
        const float nz = lz[i] * rect.w;
        out.landmarks[i][0] = nx;
        out.landmarks[i][1] = ny;
        out.landmarks[i][2] = nz;
    }

    // world 原样保留（米）
    for (int i = 0; i < 21; ++i) {
        out.world[i][0] = world[i * 3 + 0];
        out.world[i][1] = world[i * 3 + 1];
        out.world[i][2] = world[i * 3 + 2];
    }

    out.palm = palm;
    out.handedness = handedness;
    out.hand_flag = hand_flag;
    out.rot = rot;
    out.landmark_valid = true;

    auto t2 = std::chrono::high_resolution_clock::now();
    post_ms_ = std::chrono::duration<float, std::milli>(t2 - t1).count();

    // ===== 调试 log：降频打印（每 100 帧）=====
    if (dbg_log) {
        std::printf("[LM] infer=%.1fms post=%.1fms | hand_flag=%.3f handedness=%.3f(R) rot=%.3f\n",
                    infer_ms_, post_ms_, hand_flag, handedness, rot);
        // 打印 21 点归一化坐标（每行 3 个点）
        for (int i = 0; i < 21; i += 3) {
            std::printf("  [LM] pt#%02d-#%02d: ", i, std::min(i + 2, 20));
            for (int j = i; j <= std::min(i + 2, 20); ++j) {
                std::printf("(%.3f,%.3f,%.3f) ", out.landmarks[j][0], out.landmarks[j][1], out.landmarks[j][2]);
            }
            std::printf("\n");
        }
    }

    return true;
}

}  // namespace hand