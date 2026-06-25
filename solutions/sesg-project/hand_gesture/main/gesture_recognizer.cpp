/**
 * @file gesture_recognizer.cpp
 * @brief GestureEmbedder + CannedClassifier 后处理实现（坑④⑤，cvimodel 适配）。
 *
 * 严格对齐 PC 端 test_tflite_rtsp.py GestureRecognizer：
 *   - normalize_landmark_aspect_ratio (Python L430-439)
 *   - normalize_object                (Python L455-464)
 *   - embedder 打包（坑④: rotation=0，不旋转 landmarks，Python L466-501）
 *
 */

#include "gesture_recognizer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace hand {

// 对齐 LandmarksToMatrixCalculator::NormalizeLandmarkAspectRatio（Python L430-439）
//   按 max_dim 归一化，缩放 x/y（z 不变）。
void GestureRecognizer::normalize_landmark_aspect_ratio(const float lm_in[21 * 3],
                                                        float lm_out[21 * 3],
                                                        int img_w, int img_h) {
    const float width    = (float)img_w;
    const float height   = (float)img_h;
    const float max_dim  = std::max(width, height);
    const float ws       = width / max_dim;
    const float hs       = height / max_dim;
    for (int i = 0; i < 21; ++i) {
        lm_out[i * 3 + 0] = (lm_in[i * 3 + 0] - 0.5f) * ws + 0.5f;
        lm_out[i * 3 + 1] = (lm_in[i * 3 + 1] - 0.5f) * hs + 0.5f;
        lm_out[i * 3 + 2] = lm_in[i * 3 + 2];  // z 不变
    }
}

// 对齐 LandmarksToMatrixCalculator::NormalizeObject（Python L455-464）
//   以第 origin_offset 个关键点为原点，按包围盒长边缩放（含 z）。
void GestureRecognizer::normalize_object(float lm[21 * 3], int origin_offset) {
    // origin = lm[origin_offset]（3 元素）
    const float ox = lm[origin_offset * 3 + 0];
    const float oy = lm[origin_offset * 3 + 1];
    const float oz = lm[origin_offset * 3 + 2];

    // out = out - origin（每个点的 x/y/z 分别减去 origin 的 x/y/z）
    for (int i = 0; i < 21; ++i) {
        lm[i * 3 + 0] -= ox;
        lm[i * 3 + 1] -= oy;
        lm[i * 3 + 2] -= oz;
    }

    // range_x = max(x) - min(x); range_y = max(y) - min(y)
    float min_x = lm[0], max_x = lm[0];
    float min_y = lm[1], max_y = lm[1];
    for (int i = 0; i < 21; ++i) {
        const float vx = lm[i * 3 + 0];
        const float vy = lm[i * 3 + 1];
        if (vx < min_x) min_x = vx;
        if (vx > max_x) max_x = vx;
        if (vy < min_y) min_y = vy;
        if (vy > max_y) max_y = vy;
    }
    const float range_x = max_x - min_x;
    const float range_y = max_y - min_y;
    const float scale   = std::max(range_x, range_y) + 1e-5f;

    // out /= scale（全部 63 个值）
    const float inv = 1.0f / scale;
    for (int i = 0; i < 21 * 3; ++i) {
        lm[i] *= inv;
    }
}

// 缓存 embedder 3 个输入信息，按 name 匹配 hand/world/handedness（Python L493-500）
bool GestureRecognizer::prepareEmbedderInputs() {
    const int n_in = emb_->getInputSize();
    emb_in_hand_idx_   = -1;
    emb_in_world_idx_  = -1;
    emb_in_handed_idx_ = -1;

    for (int i = 0; i < n_in; ++i) {
        const ma_tensor_t t   = emb_->getInput(i);
        const ma_shape_t  s   = emb_->getInputShape(i);
        const size_t numel    = shape_numel(s);

        std::string name = (t.name != nullptr) ? std::string(t.name) : std::string();
        for (auto& c : name) c = (char)std::tolower((unsigned char)c);

        TensorInfo* info = nullptr;
        if (name.find("handedness") != std::string::npos) {
            emb_in_handed_idx_ = i;
            info = &emb_in_handed_;
        } else if (name.find("world") != std::string::npos) {
            emb_in_world_idx_ = i;
            info = &emb_in_world_;
        } else {
            // 默认当作 hand landmarks
            emb_in_hand_idx_ = i;
            info = &emb_in_hand_;
        }
        info->type  = t.type;
        info->quant = t.quant_param;
        info->numel = numel;
    }

    // cvimodel 转换裁剪了 dead inputs（world/handedness），仅保留 hand 输入（见 REPORT 4.1）。
    // 至少找到 hand 输入即可。
    return (emb_in_hand_idx_ >= 0);
}

// 缓存 classifier 输入信息（embedding 1x128）
bool GestureRecognizer::prepareClassifierInput() {
    if (clf_->getInputSize() < 1) return false;
    const ma_tensor_t t = clf_->getInput(0);
    const ma_shape_t  s = clf_->getInputShape(0);
    clf_in_.type  = t.type;
    clf_in_.quant = t.quant_param;
    clf_in_.numel = shape_numel(s);
    // classifier 输入应为 128（embedding 维度）
    return (clf_in_.numel == 128);
}

bool GestureRecognizer::init(const std::string& embedder_path, const std::string& classifier_path) {
    emb_ = std::make_unique<ma::engine::EngineCVI>();
    if (emb_->init() != MA_OK) return false;
    if (emb_->load(embedder_path) != MA_OK) return false;

    clf_ = std::make_unique<ma::engine::EngineCVI>();
    if (clf_->init() != MA_OK) return false;
    if (clf_->load(classifier_path) != MA_OK) return false;

    if (!prepareEmbedderInputs())   return false;
    if (!prepareClassifierInput())  return false;

    inited_ = true;
    return true;
}

// 按 dtype 打包一个 [1,N,3] 的关键点 tensor（F32 期望），并 setInput 到 embedder
void GestureRecognizer::pack_landmarks_tensor(int input_idx, const float lm[21 * 3]) {
    // cvimodel 可能裁剪 world/handedness 输入（REPORT 4.1），未找到时跳过打包。
    if (input_idx < 0) return;
    TensorInfo* info = nullptr;
    if (input_idx == emb_in_hand_idx_) {
        info = &emb_in_hand_;
    } else if (input_idx == emb_in_world_idx_) {
        info = &emb_in_world_;
    } else {
        return;
    }
    info->buf.resize_for(info->type, info->numel);
    // landmarks 为 21*3=63 个值，按行优先写入
    const size_t n = std::min(info->numel, (size_t)(21 * 3));
    for (size_t i = 0; i < n; ++i) {
        store_val(info->buf, info->type, info->quant, i, lm[i]);
    }
    // 剩余位置填 0（如果 numel > 63，不应发生）
    for (size_t i = n; i < info->numel; ++i) {
        store_val(info->buf, info->type, info->quant, i, 0.f);
    }
    ma_tensor_t t   = make_input_tensor(info->type, info->buf, info->numel);
    t.type          = info->type;
    t.quant_param   = info->quant;
    emb_->setInput(input_idx, t);
}

// 按 dtype 打包一个 [1,1] 的 handedness tensor
void GestureRecognizer::pack_handedness_tensor(int input_idx, float handedness) {
    // cvimodel 可能裁剪 handedness 输入（REPORT 4.1），未找到时跳过打包。
    if (input_idx < 0) return;
    TensorInfo* info = &emb_in_handed_;
    info->buf.resize_for(info->type, info->numel);
    store_val(info->buf, info->type, info->quant, 0, handedness);
    ma_tensor_t t   = make_input_tensor(info->type, info->buf, info->numel);
    t.type          = info->type;
    t.quant_param   = info->quant;
    emb_->setInput(input_idx, t);
}

// recognize（严格对齐 Python L466-512）
bool GestureRecognizer::recognize(const HandResult& in, int img_w, int img_h, HandResult& out) {
    emb_ms_ = 0.f;
    clf_ms_ = 0.f;
    if (!inited_ || !emb_ || !clf_ || !in.landmark_valid) return false;

    // 调试日志降频计数器（每 100 帧打印一次，避免 printf 阻塞推理）
    static int dbg_cnt = 0;
    const bool dbg_log = ((dbg_cnt++ % 100) == 0);

    // ===== (a) hand landmarks: aspect → object normalize（坑④：不旋转！）=====
    // ⚠️ 坑④（导致"手势永远 None"的根因）：embedder 的 rotation 必须为 0。
    //   官方 hand_gesture_recognizer_graph.cc 传入 embedder 子图的 norm_rect.rotation=0，
    //   RotateLandmarks 是恒等变换。绝对不要把 rot 传进去旋转 landmarks。
    float hand_lm[21 * 3];
    for (int i = 0; i < 21; ++i) {
        hand_lm[i * 3 + 0] = in.landmarks[i][0];
        hand_lm[i * 3 + 1] = in.landmarks[i][1];
        hand_lm[i * 3 + 2] = in.landmarks[i][2];
    }
    float tmp[21 * 3];
    normalize_landmark_aspect_ratio(hand_lm, tmp, img_w, img_h);  // aspect normalize
    normalize_object(tmp, 0);                                      // object normalize（以 wrist 为原点）

    // ===== (b) world landmarks: object normalize（跳过 aspect 与旋转）=====
    float world_lm[21 * 3];
    for (int i = 0; i < 21; ++i) {
        world_lm[i * 3 + 0] = in.world[i][0];
        world_lm[i * 3 + 1] = in.world[i][1];
        world_lm[i * 3 + 2] = in.world[i][2];
    }
    normalize_object(world_lm, 0);

    // ===== (c) handedness: P(right hand) → 1x1 =====

    // ===== embedder 打包 + 推理（Python L493-502）=====
    auto t0 = std::chrono::high_resolution_clock::now();

    pack_landmarks_tensor(emb_in_hand_idx_, tmp);
    pack_landmarks_tensor(emb_in_world_idx_, world_lm);
    pack_handedness_tensor(emb_in_handed_idx_, in.handedness);
    emb_->run();

    auto t1 = std::chrono::high_resolution_clock::now();
    emb_ms_ = std::chrono::duration<float, std::milli>(t1 - t0).count();

    // 读 embedding（第 0 个输出，1x128）
    const ma_tensor_t t_emb = emb_->getOutput(0);
    float embedding[128];
    const ma_shape_t s_emb = emb_->getOutputShape(0);
    const size_t emb_numel = std::min(shape_numel(s_emb), (size_t)128);
    for (size_t i = 0; i < emb_numel; ++i) embedding[i] = read_val(t_emb, (int)i);

    // ===== classifier 打包 + 推理（Python L505-509）=====
    auto t2 = std::chrono::high_resolution_clock::now();

    clf_in_.buf.resize_for(clf_in_.type, clf_in_.numel);
    for (size_t i = 0; i < clf_in_.numel && i < (size_t)128; ++i) {
        store_val(clf_in_.buf, clf_in_.type, clf_in_.quant, i, embedding[i]);
    }
    ma_tensor_t t_clf  = make_input_tensor(clf_in_.type, clf_in_.buf, clf_in_.numel);
    t_clf.type         = clf_in_.type;
    t_clf.quant_param  = clf_in_.quant;
    clf_->setInput(0, t_clf);
    clf_->run();

    auto t3 = std::chrono::high_resolution_clock::now();
    clf_ms_ = std::chrono::duration<float, std::milli>(t3 - t2).count();

    // ===== 读 8 logits 并补做 softmax（REPORT 4.2：cvimodel Softmax 被截断，输出原始 logits）=====
    //   canned_gesture_classifier 原末层为 Softmax，但 cvimodel 转换中 Softmax 被截断，
    //   模型输出的是 logits（不是概率）。必须在 C++ 端补做 softmax：
    //     - argmax(logits) ≡ argmax(softmax(logits))，故手势类别正确；
    //     - 但置信度必须用 softmax 后的概率，否则 clip[0,1] 会把 logits 压成 0/1。
    const ma_tensor_t t_probs = clf_->getOutput(0);
    const ma_shape_t  s_probs = clf_->getOutputShape(0);
    const size_t prob_numel  = std::min(shape_numel(s_probs), (size_t)kNumGestures);

    // 读取 logits
    float logits[kNumGestures] = {0.f};
    for (size_t i = 0; i < prob_numel; ++i) {
        logits[i] = read_val(t_probs, (int)i);
    }

    // softmax（数值稳定：减去最大值）
    float mx = logits[0];
    for (int i = 1; i < kNumGestures; ++i) {
        if (logits[i] > mx) mx = logits[i];
    }
    float sum = 0.f;
    float probs[kNumGestures];
    for (int i = 0; i < kNumGestures; ++i) {
        probs[i] = std::exp(logits[i] - mx);
        sum += probs[i];
    }
    for (int i = 0; i < kNumGestures; ++i) {
        probs[i] /= sum;
    }

    int idx = 0;
    for (int i = 1; i < kNumGestures; ++i) {
        if (probs[i] > probs[idx]) idx = i;
    }

    // ===== 调试 log：降频打印（每 100 帧）=====
    if (dbg_log) {
        std::printf("[REC] emb=%.1fms clf=%.1fms | embedding[0:8]=",
                    emb_ms_, clf_ms_);
        for (int i = 0; i < 8; ++i) {
            std::printf("%.3f ", embedding[i]);
        }
        std::printf("...\n");
        std::printf("  [REC] logits=");
        for (int i = 0; i < kNumGestures; ++i) {
            std::printf("%.3f ", logits[i]);
        }
        std::printf("\n  [REC] probs  =");
        for (int i = 0; i < kNumGestures; ++i) {
            std::printf("%.3f ", probs[i]);
        }
        std::printf("\n  [REC] → %s (%.1f%%)\n", kGestureNames[idx], probs[idx] * 100.f);
    }

    out            = in;
    out.gesture_idx  = idx;
    out.gesture_conf = probs[idx];
    return true;
}

}  // namespace hand
