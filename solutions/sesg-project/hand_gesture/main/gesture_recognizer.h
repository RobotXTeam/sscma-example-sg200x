/**
 * @file gesture_recognizer.h
 * @brief GestureEmbedder + CannedClassifier（坑④⑤）。
 *
 * 严格对齐 PC 端 `test_tflite_rtsp.py` GestureRecognizer：
 *   - normalize_landmark_aspect_ratio (Python L430-439)
 *   - normalize_object                (Python L455-464)
 *   - embedder 打包（坑④: rotation=0，不旋转 landmarks）
 *   - classifier 解析（坑⑤: 末层已带 sigmoid，不额外 softmax）
 */

#pragma once

#include <memory>
#include <string>
#include <sscma.h>

#include "engine_utils.h"
#include "hand_types.h"

namespace hand {

class GestureRecognizer {
public:
    GestureRecognizer() = default;
    ~GestureRecognizer() = default;

    // 分别加载 embedder 与 classifier 模型。
    bool init(const std::string& embedder_path, const std::string& classifier_path);

    // recognize（Python L466-512）
    //   landmarks_norm: 21x3 归一化图像坐标（来自 HandLandmarker）
    //   world:           21x3 原始模型输出（米）
    //   handedness:      P(right hand)
    //   img_w/img_h:     原图宽高（用于 aspect normalize）
    // 成功时填充 out.gesture_idx / out.gesture_conf 并返回 true。
    bool recognize(const HandResult& in, int img_w, int img_h, HandResult& out);

    float lastEmbedderMs() const { return emb_ms_; }
    float lastClassifierMs() const { return clf_ms_; }

private:
    // 对齐 LandmarksToMatrixCalculator::NormalizeLandmarkAspectRatio（Python L430-439）
    static void normalize_landmark_aspect_ratio(const float lm_in[21 * 3],
                                                float lm_out[21 * 3],
                                                int img_w, int img_h);
    // 对齐 LandmarksToMatrixCalculator::NormalizeObject（Python L455-464）
    static void normalize_object(float lm[21 * 3], int origin_offset = 0);

    // 按 dtype 打包一个 [1,N,3] 的关键点 tensor（F32 期望）
    void pack_landmarks_tensor(int input_idx, const float lm[21 * 3]);

    // 按 dtype 打包一个 [1,1] 的 handedness tensor
    void pack_handedness_tensor(int input_idx, float handedness);

    bool prepareEmbedderInputs();
    bool prepareClassifierInput();

private:
    std::unique_ptr<ma::engine::EngineCVI> emb_;
    std::unique_ptr<ma::engine::EngineCVI> clf_;
    bool inited_ = false;

    // embedder 3 个输入的索引（按 name 匹配：hand/world/handedness）
    int emb_in_hand_idx_ = -1;
    int emb_in_world_idx_ = -1;
    int emb_in_handed_idx_ = -1;

    // 每个 embedder 输入的 type/quant/numel 缓存
    struct TensorInfo {
        ma_tensor_type_t type = MA_TENSOR_TYPE_NONE;
        ma_quant_param_t quant{};
        size_t numel = 0;
        InputBuf buf;
    };
    TensorInfo emb_in_hand_;
    TensorInfo emb_in_world_;
    TensorInfo emb_in_handed_;

    // classifier 1 个输入（embedding 1x128）+ 输出（8 概率）
    TensorInfo clf_in_;
    TensorInfo clf_out_;

    float emb_ms_ = 0.f;
    float clf_ms_ = 0.f;
};

}  // namespace hand