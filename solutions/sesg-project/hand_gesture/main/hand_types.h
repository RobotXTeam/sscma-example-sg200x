/**
 * @file hand_types.h
 * @brief 手势识别公共数据结构：Palm / HandResult / UDPHandResult + 手势类别常量。
 *
 * 严格对齐 PC 端 `test_tflite_rtsp.py` 的后处理契约（见该文件 GESTURE_NAMES / HAND_CONNECTIONS）。
 * UDPHandResult 为 POD，C++/Python 双端对齐（小端，#pragma pack(1)），用于 SesgJpegUdpStreamer 打包。
 */

#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace hand {

// 手势类别（canned_gesture_classifier 8 类，顺序固定，对齐 test_tflite_rtsp.py L68-77）
//   0:None 1:Closed_Fist 2:Open_Palm 3:Pointing_Up
//   4:Thumb_Down 5:Thumb_Up 6:Victory 7:ILoveYou
constexpr int kNumGestures   = 8;
constexpr int kGestureNone   = 0;
constexpr const char* kGestureNames[kNumGestures] = {
    "None",          // 0
    "Closed_Fist",   // 1
    "Open_Palm",     // 2
    "Pointing_Up",   // 3
    "Thumb_Down",    // 4
    "Thumb_Up",      // 5
    "Victory",       // 6
    "ILoveYou",      // 7
};

// 手部 21 关键点连接关系（对齐 test_tflite_rtsp.py L80-87 HAND_CONNECTIONS，仅用于可视化）
constexpr int kHandConnections[][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 4},        // thumb
    {0, 5}, {5, 6}, {6, 7}, {7, 8},        // index
    {5, 9}, {9, 10}, {10, 11}, {11, 12},   // middle
    {9, 13}, {13, 14}, {14, 15}, {15, 16}, // ring
    {13, 17}, {17, 18}, {18, 19}, {19, 20},// pinky
    {0, 17},                               // palm base
};
constexpr int kNumHandConnections = sizeof(kHandConnections) / sizeof(kHandConnections[0]);

constexpr int kNumLandmarks = 21;     // 21 关键点
constexpr int kNumPalmKeypoints = 7;  // palm 检测输出 7 个关键点

// Palm 检测结果（归一化坐标，对齐 test_tflite_rtsp.py HandDetector.detect 返回 dict）
struct Palm {
    std::array<float, 4> bbox{};                       // 归一化 xyxy
    std::array<std::array<float, 2>, kNumPalmKeypoints> kpts{}; // 7x2 归一化 (x,y)
    float score = 0.f;
};

// 单手完整识别结果（对齐 test_tflite_rtsp.py HandGesturePipeline.run 返回 dict）
struct HandResult {
    Palm palm;
    std::array<std::array<float, 3>, kNumLandmarks> landmarks{}; // 21x3 归一化图像坐标 [0,1] (x,y,z)
    std::array<std::array<float, 3>, kNumLandmarks> world{};     // 21x3 原始模型输出(米)
    float handedness = 0.f;     // P(right hand) ∈ [0,1]
    float hand_flag   = 0.f;    // 模型 presence
    float rot         = 0.f;    // ROI 旋转角(rad)
    int   gesture_idx = 0;      // 0..7
    float gesture_conf = 0.f;   // 该类别概率
    bool  landmark_valid = false; // 关键点是否有效（landmarker 成功）
};

// UDP 结果结构体（POD，C++/Python 双端对齐）。
// 对齐 PROMPT_recamera_hand_gesture.md 第八章 8.2：
//   5f palm(xyxy) + f palm_score + f handedness + i gesture_idx + f gesture_conf + 63f landmarks + i landmark_valid
// 总大小 = 4*5 + 4 + 4 + 4 + 21*3*4 + 4 = 20 + 4 + 4 + 4 + 252 + 4 = 288 bytes
#pragma pack(push, 1)
struct UDPHandResult {
    float   palm_x1;            // palm bbox 归一化 xyxy
    float   palm_y1;
    float   palm_x2;
    float   palm_y2;
    float   palm_score;         // palm 置信度
    float   handedness;         // P(right hand)
    int32_t gesture_idx;        // 0..7（见手势类别表）
    float   gesture_conf;       // 该类别概率
    float   landmarks[21 * 3];  // 21 个归一化关键点 (x,y,z) 交错
    int32_t landmark_valid;     // 1=关键点有效，0=只有 palm 框
};
#pragma pack(pop)
static_assert(sizeof(UDPHandResult) == 4 * 5 + 4 + 4 + 4 + 21 * 3 * 4 + 4,
              "UDPHandResult size mismatch (expected 288 bytes)");

}  // namespace hand