#include "detector_utils.h"

namespace detector_utils {

// ========== SquareDetector ==========
SquareDetector::SquareDetector(ma::model::Detector* detector, int input_w, int input_h)
    : detector_(detector), input_w_(input_w), input_h_(input_h) {
}

void SquareDetector::setThreshold(float threshold) {
    detector_->setConfig(MA_MODEL_CFG_OPT_THRESHOLD, threshold);
}

PerfStats SquareDetector::run(void* frame_data, size_t frame_size, std::vector<Detection>& results) {
    (void)frame_data;
    (void)frame_size;
    // 使用物理地址运行推理
    detector_->run(nullptr);
    
    // 获取结果
    auto sscma_results = detector_->getResults();
    results.clear();
    for (auto& r : sscma_results) {
        Detection det;
        det.x = r.x;
        det.y = r.y;
        det.w = r.w;
        det.h = r.h;
        det.score = r.score;
        det.target = r.target;
        results.push_back(det);
    }
    
    // 获取性能数据
    auto perf_data = detector_->getPerf();
    PerfStats perf;
    perf.preprocess_ms = perf_data.preprocess;
    perf.inference_ms = perf_data.inference;
    perf.postprocess_ms = perf_data.postprocess;
    
    return perf;
}

} // namespace detector_utils
