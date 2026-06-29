#pragma once

#include <sscma.h>

namespace detector_utils {

// 检测结果结构
struct Detection {
    float x, y, w, h;  // 归一化坐标 [0,1]
    float score;
    int32_t target;    // 类别 ID
};

// 性能统计结构
struct PerfStats {
    double preprocess_ms;
    double inference_ms;
    double postprocess_ms;
    
    double total() const {
        return preprocess_ms + inference_ms + postprocess_ms;
    }
};

/**
 * @brief 正方形模型推理 (使用SSCMA原生后处理)
 */
class SquareDetector {
public:
    SquareDetector(ma::model::Detector* detector, int input_w, int input_h);
    
    /**
     * @brief 运行推理 (使用物理地址输入)
     * @param frame_data 物理地址
     * @param frame_size 数据大小
     * @param results 输出检测结果
     * @return 性能统计
     */
    PerfStats run(void* frame_data, size_t frame_size, std::vector<Detection>& results);
    
    void setThreshold(float threshold);
    
private:
    ma::model::Detector* detector_;
    int input_w_;
    int input_h_;
};

} // namespace detector_utils
