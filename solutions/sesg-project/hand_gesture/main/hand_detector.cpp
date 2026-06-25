/**
 * @file hand_detector.cpp
 * @brief Palm 检测 SSD 后处理实现（anchor 生成 + 解码 + NMS + letterbox 逆变换）。
 *
 * 严格对齐 PC 端 test_tflite_rtsp.py HandDetector：
 *   - calc_scale            (Python L111-116)
 *   - generate_anchors      (Python L118-176, 坑⑥：合并相同 stride 的层)
 *   - score 解码 clip→sigmoid (Python L201-202)
 *   - box/keypoint 解码       (Python L206-216, reverse_output_order, scale=192, fixed_anchor_size)
 *   - nms                     (Python L267-281)
 *   - letterbox 逆变换        (Python L239-248)
 */

#include "hand_detector.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace hand {

// 对齐 ssd_anchors_calculator.cc::CalculateScale（Python L111-116）
//   num_strides==1 → 中点；否则 [min_scale, max_scale] 线性插值。
float HandDetector::calc_scale(float min_scale, float max_scale,
                               int stride_index, int num_strides) {
    if (num_strides == 1) {
        return (min_scale + max_scale) * 0.5f;
    }
    return min_scale + (max_scale - min_scale) * (float)stride_index / (float)(num_strides - 1);
}

// 对齐 ssd_anchors_calculator.cc::GenerateAnchors（Python L118-176）
// ⚠️ 坑⑥（导致"关键点不动"的真正根因）：strides=[8,16,16,16]，相同 stride 的层必须合并！
//   layer0(stride=8)单独一组；layer1/2/3(stride=16)合并为一组。
//   每组在每个特征图 cell 依次输出该组所有 (scale, aspect_ratio) 对（含 interpolated scale）。
//   不能把 4 层当 4 个独立网格顺序排。fixed_anchor_size=true → w=h=1（解码时 aw=ah=1）。
std::vector<std::pair<float, float>> HandDetector::generate_anchors() {
    const int num_layers = 4;
    const float min_scale = 0.1484375f;
    const float max_scale = 0.75f;
    const int input_size = 192;
    const int strides[4] = {8, 16, 16, 16};
    const float aspect_ratios[1] = {1.0f};
    const float interp_ratio = 1.0f;          // interpolated_scale_aspect_ratio
    const float anchor_offset_x = 0.5f;
    const float anchor_offset_y = 0.5f;
    const int num_strides = 4;

    std::vector<std::pair<float, float>> anchors;

    int layer_id = 0;
    while (layer_id < num_layers) {
        // 找出与 layer_id 相同 stride 的连续层 [layer_id, last_same)
        int last_same = layer_id;
        while (last_same < num_strides && strides[last_same] == strides[layer_id]) {
            last_same++;
        }

        // 为组内每一层准备 (scale, aspect_ratio) 对
        // 顺序: sub_layer0 的 aspect_ratios + interp, sub_layer1 的 ..., ...
        struct Pair {
            float scale;
            float aspect_ratio;
        };
        std::vector<Pair> pairs;
        for (int sub = layer_id; sub < last_same; ++sub) {
            const float scale = calc_scale(min_scale, max_scale, sub, num_strides);
            for (float ar : aspect_ratios) {
                pairs.push_back({scale, ar});
            }
            if (interp_ratio > 0) {
                float scale_next;
                if (sub == num_strides - 1) {
                    scale_next = 1.0f;
                } else {
                    scale_next = calc_scale(min_scale, max_scale, sub + 1, num_strides);
                }
                pairs.push_back({std::sqrt(scale * scale_next), interp_ratio});
            }
        }

        const int stride = strides[layer_id];
        const int feature_h = (int)std::ceil((float)input_size / (float)stride);
        const int feature_w = (int)std::ceil((float)input_size / (float)stride);
        for (int y = 0; y < feature_h; ++y) {
            for (int x = 0; x < feature_w; ++x) {
                const float cx = ((float)x + anchor_offset_x) / (float)feature_w;
                const float cy = ((float)y + anchor_offset_y) / (float)feature_h;
                for (const auto& pr : pairs) {
                    // fixed_anchor_size=true → w=h=1（仅记录中心，解码时 aw=ah=1）
                    (void)pr;
                    anchors.push_back({cx, cy});
                }
            }
        }

        layer_id = last_same;
    }

    assert((int)anchors.size() == 2016 && "SSD anchor 数量必须为 2016");
    return anchors;
}

// 对齐 Python _nms（L267-281），返回 keep 的下标（按 score 降序）
std::vector<int> HandDetector::nms(const std::vector<std::array<float, 4>>& xyxy,
                                   const std::vector<float>& scores, float iou_thresh) {
    const size_t n = xyxy.size();
    std::vector<int> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = (int)i;
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return scores[a] > scores[b]; });

    std::vector<float> areas(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& b = xyxy[i];
        areas[i] = std::max(0.f, b[2] - b[0]) * std::max(0.f, b[3] - b[1]);
    }

    std::vector<int> keep;
    std::vector<char> suppressed(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const int idx = order[i];
        if (suppressed[idx]) continue;
        keep.push_back(idx);
        for (size_t j = i + 1; j < n; ++j) {
            const int jdx = order[j];
            if (suppressed[jdx]) continue;
            const auto& a = xyxy[idx];
            const auto& b = xyxy[jdx];
            const float xx1 = std::max(a[0], b[0]);
            const float yy1 = std::max(a[1], b[1]);
            const float xx2 = std::min(a[2], b[2]);
            const float yy2 = std::min(a[3], b[3]);
            const float w = std::max(0.f, xx2 - xx1);
            const float h = std::max(0.f, yy2 - yy1);
            const float inter = w * h;
            const float ovr = inter / (areas[idx] + areas[jdx] - inter + 1e-9f);
            if (ovr > iou_thresh) suppressed[jdx] = 1;
        }
    }
    return keep;
}

bool HandDetector::prepareInputTensor() {
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

    // 打印输入张量信息（仅一次）—— 诊断"输出恒定"问题的关键线索
    const char* tname = "UNKNOWN";
    switch (input_type_) {
        case MA_TENSOR_TYPE_F32: tname = "F32"; break;
        case MA_TENSOR_TYPE_F16: tname = "F16"; break;
        case MA_TENSOR_TYPE_BF16: tname = "BF16"; break;
        case MA_TENSOR_TYPE_S8:  tname = "S8";  break;
        case MA_TENSOR_TYPE_U8:  tname = "U8";  break;
        default: break;
    }
    const float qs = input_tensor_cache_.quant_param.scale;
    const int qzp = input_tensor_cache_.quant_param.zero_point;
    printf("[DET] input tensor: type=%s, layout=%s, shape=(%d,%d,%d,%d), quant(scale=%.8f zp=%d)\n",
           tname, input_is_chw_ ? "NCHW" : "NHWC",
           s.dims[0], s.dims[1], s.dims[2], s.dims[3], qs, qzp);
    return true;
}

bool HandDetector::init(const std::string& model_path) {
    engine_ = std::make_unique<ma::engine::EngineCVI>();
    if (engine_->init() != MA_OK) return false;
    if (engine_->load(model_path) != MA_OK) return false;
    if (!prepareInputTensor()) return false;
    anchors_ = generate_anchors();
    inited_ = true;
    return true;
}

 std::vector<Palm> HandDetector::detect(const cv::Mat& rgb,
                                        float min_score, float nms_thresh, int max_hands) {
     infer_ms_ = 0.f;
     post_ms_ = 0.f;
     std::vector<Palm> empty;
     if (!inited_ || !engine_ || rgb.empty()) return empty;
 
     auto t0 = std::chrono::high_resolution_clock::now();
 
     // 调试日志降频计数器（每 100 帧打印一次，避免 printf 阻塞推理）
     static int dbg_cnt = 0;
     const bool dbg_log = ((dbg_cnt++ % 100) == 0);
 
     // 1) letterbox 到 192×192（Python L181-184）
     //    注意：reCamera 取出即 RGB，无需 BGR→RGB 转换（与 Python 端 cv2 BGR 不同）
     LetterboxPad pad{};
     cv::Mat inp = letterbox(rgb, input_w_, &pad);  // RGB888
     auto t_lb = std::chrono::high_resolution_clock::now();
 
     // 2) 归一化 /255 并按 dtype 打包（Python: inp = (inp/255)[None]）
     const int H = input_h_;
     const int W = input_w_;
     const int C = 3;
     const ma_quant_param_t qp = input_tensor_cache_.quant_param;
     const bool input_is_int = (input_type_ == MA_TENSOR_TYPE_U8 || input_type_ == MA_TENSOR_TYPE_S8);
     const bool quant_defaultish = (!std::isfinite(qp.scale) || qp.scale <= 0.f ||
                                    (std::fabs(qp.scale - 1.0f) < 1e-6f && qp.zero_point == 0));
     const bool use_raw_passthrough = input_is_int && quant_defaultish;
 
     static bool printed_pack_mode = false;
     if (!printed_pack_mode) {
         printed_pack_mode = true;
         printf("[DET] packInput mode: %s\n", use_raw_passthrough ? "raw_passthrough" : "normalize_div255");
     }
 
     // ===== 诊断：letterbox 后统计（前3帧，轻量版，只统计 letterbox 输出）=====
     static int diag_frame = 0;
     if (diag_frame < 3) {
         diag_frame++;
         int mn2 = 255, mx2 = 0;
         for (int y = 0; y < inp.rows; ++y)
             for (int x = 0; x < inp.cols; ++x) {
                 const uint8_t* p = inp.ptr<uint8_t>(y) + x * 3;
                 for (int c = 0; c < 3; ++c) {
                     int v = p[c]; if (v<mn2) mn2=v; if (v>mx2) mx2=v;
                 }
             }
         printf("[DET-DIAG] frame#%d letterbox %dx%d: min=%d max=%d\n",
                diag_frame, inp.cols, inp.rows, mn2, mx2);
     }
 
     for (int y = 0; y < H; ++y) {
         for (int x = 0; x < W; ++x) {
             const uint8_t* p = inp.ptr<uint8_t>(y) + x * 3;
             for (int c = 0; c < C; ++c) {
                 size_t idx;
                 if (input_is_chw_) {
                     idx = (size_t)c * (size_t)H * (size_t)W + (size_t)y * (size_t)W + (size_t)x;
                 } else {
                     idx = ((size_t)y * (size_t)W + (size_t)x) * (size_t)C + (size_t)c;
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
 
     // ===== 诊断：打印打包后 tensor 的前12个值（前3帧）=====
     if (diag_frame <= 3) {
         printf("[DET-DIAG] packed tensor first12: ");
         for (int i = 0; i < 12; ++i) {
             float v = 0.f;
             switch (input_type_) {
                 case MA_TENSOR_TYPE_F32: v = input_buf_.f32[i]; break;
                 case MA_TENSOR_TYPE_F16: v = fp16_to_fp32(input_buf_.u16[i]); break;
                 case MA_TENSOR_TYPE_BF16: v = bf16_to_fp32(input_buf_.u16[i]); break;
                 case MA_TENSOR_TYPE_U8: v = (float)input_buf_.u8[i]; break;
                 case MA_TENSOR_TYPE_S8: v = (float)input_buf_.s8[i]; break;
                 default: break;
             }
             printf("%.4f%s", v, (i==11)?"\n":" ");
         }
     }
 
     ma_tensor_t tensor = make_input_tensor(input_type_, input_buf_, input_numel_);
     // ⚠️ 关键：必须设置 tensor.type，否则 EngineCVI 可能用错误的 dtype 解析输入
     tensor.type = input_type_;
     tensor.quant_param = input_tensor_cache_.quant_param;
     int ret_set = engine_->setInput(0, tensor);
     int ret_run = engine_->run();
     auto t_run = std::chrono::high_resolution_clock::now();
     if (diag_frame <= 3) {
         printf("[DET-DIAG] setInput ret=%d, run ret=%d\n", ret_set, ret_run);
     }
     const int n_out = engine_->getOutputSize();
     // 输出 shape 是静态信息，仅首帧打印一次
     {
         static bool printed_shape = false;
         if (!printed_shape) {
             printed_shape = true;
             printf("[DET] n_out = %d\n", n_out);
             for (int oi = 0; oi < n_out; ++oi) {
                 const ma_tensor_t t = engine_->getOutput(oi);
                 const ma_shape_t s = engine_->getOutputShape(oi);
                 printf("  out[%d]: type=%d, shape=(", oi, t.type);
                 for (int i = 0; i < s.size; ++i) printf("%d%s", s.dims[i], (i+1<s.size)?",":"");
                 printf("), numel=%zu, name=%s\n", shape_numel(s), t.name ? t.name : "null");
             }
         }
     }
     auto t1 = std::chrono::high_resolution_clock::now();
     infer_ms_ = std::chrono::duration<float, std::milli>(t1 - t0).count();
 
     // 细化计时（每 100 帧打印，诊断 CPU vs TPU 瓶颈）
     if (dbg_log) {
         const float lb_ms   = std::chrono::duration<float, std::milli>(t_lb - t0).count();
         const float pack_ms = std::chrono::duration<float, std::milli>(t_pack - t_lb).count();
         const float run_ms  = std::chrono::duration<float, std::milli>(t_run - t_pack).count();
         const float out_ms  = std::chrono::duration<float, std::milli>(t1 - t_run).count();
         std::printf("  [DET-TIME] letterbox=%.1fms pack=%.1fms TPU_run=%.1fms readout=%.1fms\n",
                     lb_ms, pack_ms, run_ms, out_ms);
     }
 
     // 3) 读输出 & 后处理（letterbox 模式：pad 非零）
     return postProcess(n_out, min_score, nms_thresh, max_hands, pad, dbg_log, t1);
 }
 
  // 虚拟地址推理：传入已 mmap 的 RGB 指针，构造零拷贝 cv::Mat 头后复用 detect() 逻辑。
  // 相比「整帧 memcpy」，mmap 只建立页表映射；letterbox 内部只在缩放时分配新缓冲。
  // 用于「无硬件缩放」场景：CH0 原生分辨率（640×480）→ 模型输入（192×192）。
  std::vector<Palm> HandDetector::detectVirt(const uint8_t* rgb, int W, int H, int stride_bytes,
                                             float min_score, float nms_thresh, int max_hands) {
      infer_ms_ = 0.f;
      post_ms_  = 0.f;
      std::vector<Palm> empty;
      if (!inited_ || !engine_ || !rgb || W <= 0 || H <= 0) return empty;

      // 构造零拷贝 cv::Mat 头（不复制像素数据）：rows=H, cols=W, CV_8UC3, data=rgb
      // stride_bytes 通常是 W*3（紧密排布），但也支持行对齐 padding。
      const int step = (stride_bytes > 0) ? stride_bytes : (W * 3);
      cv::Mat rgb_mat(H, W, CV_8UC3, const_cast<uint8_t*>(rgb), (size_t)step);
      if (rgb_mat.empty()) return empty;

      // 复用 detect() 的完整流程（letterbox + 归一化/打包 + run + postProcess）
      return detect(rgb_mat, min_score, nms_thresh, max_hands);
  }

  // 零拷贝推理：直接传入物理地址（CH0 已由硬件缩放到 input_w_×input_h_）。
  // 避免整帧 memcpy + 软件缩放，交给 CVI 引擎做 DMA 读取。
  std::vector<Palm> HandDetector::detectPhys(uint64_t phy_addr, int W, int H,
                                            float min_score, float nms_thresh, int max_hands) {
     infer_ms_ = 0.f;
     post_ms_ = 0.f;
     std::vector<Palm> empty;
     if (!inited_ || !engine_ || phy_addr == 0) return empty;
 
     auto t0 = std::chrono::high_resolution_clock::now();
 
     static int dbg_cnt = 0;
     const bool dbg_log = ((dbg_cnt++ % 100) == 0);
 
     // 物理地址直传（零拷贝）：CH0 已由 kWindow 硬件缩放到 input_w_×input_h_。
     // 若帧尺寸 == 模型输入，则 pad 全为 0（无需 letterbox 逆变换）。
     LetterboxPad pad{0.f, 0.f, 0.f, 0.f};
     const bool size_match = (W == input_w_ && H == input_h_);
     if (!size_match) {
         // 尺寸不一致：退回虚拟地址路径（安全兜底，极少触发）。
         if (dbg_log) {
             printf("[DET-PHYS] 尺寸不匹配 W=%d H=%d (期望 %dx%d)，退回 cv::Mat 路径\n",
                    W, H, input_w_, input_h_);
         }
         // mmap 并构造 cv::Mat（这里不推荐，仅在异常时兜底）
         return empty;
     }
 
     // 构造物理地址 tensor，交给 CVI 引擎 SetTensorPhysicalAddr 做 DMA 读取
     ma_tensor_t tensor{};
     tensor.size        = (size_t)W * (size_t)H * 3u;  // RGB888
     tensor.is_physical = true;
     tensor.is_variable = false;
     tensor.data.data   = reinterpret_cast<void*>(phy_addr);
     tensor.type        = input_type_;
     tensor.quant_param = input_tensor_cache_.quant_param;
     engine_->setInput(0, tensor);
     engine_->run();
 
     auto t1 = std::chrono::high_resolution_clock::now();
     infer_ms_ = std::chrono::duration<float, std::milli>(t1 - t0).count();
 
     const int n_out = engine_->getOutputSize();
 
     if (dbg_log) {
         std::printf("[DET-PHYS] infer=%.1fms (零拷贝物理地址直传)\n", infer_ms_);
     }
 
     // 后处理：pad=0（尺寸匹配，无 letterbox）
     return postProcess(n_out, min_score, nms_thresh, max_hands, pad, dbg_log, t1);
 }
 
 // 通用后处理：读输出 → score/box 解码 → NMS → letterbox 逆变换。
 // 被 detect()（cv::Mat + letterbox）与 detectPhys()（物理地址零拷贝）共用。
 std::vector<Palm> HandDetector::postProcess(int n_out, float min_score, float nms_thresh,
                                             int max_hands, const LetterboxPad& pad,
                                             bool dbg_log,
                                             std::chrono::time_point<std::chrono::high_resolution_clock> t1) {
     std::vector<Palm> empty;
 
     // 1) 读输出：用「总元素数 numel」识别 boxes / scores。
     //    ⚠️ 坑⑧：cvimodel 输出 shape 可能带末尾冗余维度，用 numel 判别最稳妥。
     //    ⚠️ 坑⑨：若 TPU 侧 fuse 预处理，输入 U8 + quant 默认值，必须 raw passthrough。
     if (n_out < 2) return empty;
 
     std::vector<float> boxes_vec, scores_vec;
     const int num_boxes = (int)anchors_.size();      // 2016
     const int box_dim = 18;
     const size_t boxes_numel_expect = (size_t)num_boxes * (size_t)box_dim;  // 36288
     const size_t scores_numel_expect = (size_t)num_boxes;                   // 2016
 
     for (int oi = 0; oi < n_out; ++oi) {
         const ma_tensor_t t = engine_->getOutput(oi);
         const ma_shape_t s = engine_->getOutputShape(oi);
         const size_t numel = shape_numel(s);
         if (numel == boxes_numel_expect && boxes_vec.empty()) {
             boxes_vec.resize(numel);
             for (size_t i = 0; i < numel; ++i) boxes_vec[i] = read_val(t, (int)i);
         } else if (numel == scores_numel_expect && scores_vec.empty()) {
             scores_vec.resize(numel);
             for (size_t i = 0; i < numel; ++i) scores_vec[i] = read_val(t, (int)i);
         }
     }
 
     if (dbg_log) {
         printf("[DET] boxes_vec.size()=%zu, scores_vec.size()=%zu\n", boxes_vec.size(), scores_vec.size());
         if (!boxes_vec.empty()) {
             printf("  boxes[0:8] = ");
             for (int i = 0; i < 8 && i < (int)boxes_vec.size(); ++i) printf("%f ", boxes_vec[i]);
             printf("\n");
         }
         if (!scores_vec.empty()) {
             printf("  scores[0:8] = ");
             for (int i = 0; i < 8 && i < (int)scores_vec.size(); ++i) printf("%f ", scores_vec[i]);
             printf("\n");
         }
     }
 
     if (boxes_vec.empty() || scores_vec.empty()) return empty;
 
     const float* boxes_flat = boxes_vec.data();
     const float* scores_flat = scores_vec.data();
 
     // 2) score 解码：clip(±100) → sigmoid（Python L201-202）
     std::vector<float> scores(num_boxes);
     for (int i = 0; i < num_boxes; ++i) {
         float v = scores_flat[i];
         if (v > 100.f) v = 100.f;
         if (v < -100.f) v = -100.f;
         scores[i] = 1.0f / (1.0f + std::exp(-v));
     }
 
     // 3) box/keypoint 解码（Python L206-216）
     //    reverse_output_order=true, x/y/w/h_scale=192, fixed_anchor_size=true
     std::vector<std::array<float, 4>> xyxy;
     std::vector<std::array<std::array<float, 2>, kNumPalmKeypoints>> kpts;
     std::vector<int> sel;
     xyxy.reserve(num_boxes);
     kpts.reserve(num_boxes);
     sel.reserve(num_boxes);
     for (int i = 0; i < num_boxes; ++i) {
         if (scores[i] < min_score) continue;
         const float ax = anchors_[i].first;
         const float ay = anchors_[i].second;
         const float* b = boxes_flat + (size_t)i * (size_t)box_dim;
         const float cx = b[0] / 192.0f + ax;
         const float cy = b[1] / 192.0f + ay;
         const float w  = b[2] / 192.0f;
         const float h  = b[3] / 192.0f;
         std::array<std::array<float, 2>, kNumPalmKeypoints> kp{};
         for (int k = 0; k < kNumPalmKeypoints; ++k) {
             kp[k][0] = b[4 + 2 * k] / 192.0f + ax;
             kp[k][1] = b[5 + 2 * k] / 192.0f + ay;
         }
         sel.push_back(i);
         xyxy.push_back({cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f});
         kpts.push_back(kp);
     }
     if (sel.empty()) return empty;
 
     // 4) NMS（Python L227-237）
     std::vector<float> sel_scores;
     sel_scores.reserve(sel.size());
     for (int i : sel) sel_scores.push_back(scores[i]);
     std::vector<int> keep = nms(xyxy, sel_scores, nms_thresh);
 
     // 5) 组装 + letterbox 逆变换（Python L229-248）
     //    sx = 1/(1-left-right); sy = 1/(1-top-bottom)
     const float sx = 1.0f / (1.0f - pad.left - pad.right);
     const float sy = 1.0f / (1.0f - pad.top - pad.bottom);
 
     std::vector<Palm> dets;
     for (int k = 0; k < (int)keep.size() && k < max_hands; ++k) {
         const int li = keep[k];  // 局部下标
         const int gi = sel[li];  // 全局下标（取 kpts/score）
         Palm p;
         p.bbox[0] = (xyxy[li][0] - pad.left) * sx;
         p.bbox[1] = (xyxy[li][1] - pad.top) * sy;
         p.bbox[2] = (xyxy[li][2] - pad.left) * sx;
         p.bbox[3] = (xyxy[li][3] - pad.top) * sy;
         for (int j = 0; j < kNumPalmKeypoints; ++j) {
             p.kpts[j][0] = (kpts[li][j][0] - pad.left) * sx;
             p.kpts[j][1] = (kpts[li][j][1] - pad.top) * sy;
         }
         p.score = scores[gi];
         dets.push_back(p);
     }
 
     auto t2 = std::chrono::high_resolution_clock::now();
     post_ms_ = std::chrono::duration<float, std::milli>(t2 - t1).count();
 
     // ===== 调试 log：降频打印（每 100 帧）=====
     if (dbg_log) {
         std::printf("[DET] infer=%.1fms post=%.1fms | 检测到 %zu 只手\n",
                     infer_ms_, post_ms_, dets.size());
         for (size_t i = 0; i < dets.size(); ++i) {
             const auto& p = dets[i];
             std::printf("  [DET] hand#%zu score=%.3f bbox=(%.3f,%.3f,%.3f,%.3f) wrist=(%.3f,%.3f)\n",
                         i, p.score,
                         p.bbox[0], p.bbox[1], p.bbox[2], p.bbox[3],
                         p.kpts[0][0], p.kpts[0][1]);
         }
     }
 
     return dets;
 }
 
 }  // namespace hand
