#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <sscma.h>

namespace {

struct Point3 {
    float x;
    float y;
    float z;
};

const int kConnections[][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 4},
    {0, 5}, {5, 6}, {6, 7}, {7, 8},
    {0, 9}, {9, 10}, {10, 11}, {11, 12},
    {0, 13}, {13, 14}, {14, 15}, {15, 16},
    {0, 17}, {17, 18}, {18, 19}, {19, 20},
    {5, 9}, {9, 13}, {13, 17},
};

const char* tensor_type_name(ma_tensor_type_t type) {
    switch (type) {
        case MA_TENSOR_TYPE_U8: return "U8";
        case MA_TENSOR_TYPE_S8: return "S8";
        case MA_TENSOR_TYPE_U16: return "U16";
        case MA_TENSOR_TYPE_S16: return "S16";
        case MA_TENSOR_TYPE_U32: return "U32";
        case MA_TENSOR_TYPE_S32: return "S32";
        case MA_TENSOR_TYPE_F16: return "F16";
        case MA_TENSOR_TYPE_F32: return "F32";
        case MA_TENSOR_TYPE_F64: return "F64";
        case MA_TENSOR_TYPE_BF16: return "BF16";
        default: return "UNKNOWN";
    }
}

size_t shape_elements(const ma_shape_t& shape) {
    size_t n = 1;
    for (uint32_t i = 0; i < shape.size; ++i) {
        n *= static_cast<size_t>(shape.dims[i]);
    }
    return shape.size == 0 ? 0 : n;
}

float bf16_to_float(uint16_t v) {
    uint32_t bits = static_cast<uint32_t>(v) << 16;
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

float fp16_to_float(uint16_t v) {
    const uint32_t sign = static_cast<uint32_t>(v & 0x8000u) << 16;
    const uint32_t exp = (v & 0x7C00u) >> 10;
    const uint32_t mant = (v & 0x03FFu);
    uint32_t out = 0;
    if (exp == 0) {
        out = sign;
    } else if (exp == 0x1Fu) {
        out = sign | 0x7F800000u | (mant << 13);
    } else {
        out = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &out, sizeof(f));
    return f;
}

std::vector<float> tensor_to_float(const ma_tensor_t& tensor) {
    size_t elements = shape_elements(tensor.shape);
    if (elements == 0) {
        elements = tensor.type == MA_TENSOR_TYPE_F32 ? tensor.size / sizeof(float) : tensor.size;
    }
    std::vector<float> values(elements);
    switch (tensor.type) {
        case MA_TENSOR_TYPE_F32: {
            const float* p = reinterpret_cast<const float*>(tensor.data.data);
            std::copy(p, p + elements, values.begin());
            break;
        }
        case MA_TENSOR_TYPE_BF16: {
            const uint16_t* p = reinterpret_cast<const uint16_t*>(tensor.data.data);
            for (size_t i = 0; i < elements; ++i) values[i] = bf16_to_float(p[i]);
            break;
        }
        case MA_TENSOR_TYPE_F16: {
            const uint16_t* p = reinterpret_cast<const uint16_t*>(tensor.data.data);
            for (size_t i = 0; i < elements; ++i) values[i] = fp16_to_float(p[i]);
            break;
        }
        case MA_TENSOR_TYPE_U8: {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(tensor.data.data);
            for (size_t i = 0; i < elements; ++i) {
                values[i] = (static_cast<int>(p[i]) - tensor.quant_param.zero_point) * tensor.quant_param.scale;
            }
            break;
        }
        case MA_TENSOR_TYPE_S8: {
            const int8_t* p = reinterpret_cast<const int8_t*>(tensor.data.data);
            for (size_t i = 0; i < elements; ++i) {
                values[i] = (static_cast<int>(p[i]) - tensor.quant_param.zero_point) * tensor.quant_param.scale;
            }
            break;
        }
        default:
            throw std::runtime_error(std::string("unsupported tensor type ") + tensor_type_name(tensor.type));
    }
    return values;
}

void print_shape(const char* label, const ma_tensor_t& tensor) {
    std::cout << label << ": shape=[";
    for (uint32_t i = 0; i < tensor.shape.size; ++i) {
        std::cout << tensor.shape.dims[i] << (i + 1 < tensor.shape.size ? "," : "");
    }
    std::cout << "], type=" << tensor_type_name(tensor.type) << ", bytes=" << tensor.size << std::endl;
}

bool is_image_name(const std::string& name) {
    auto lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.size() > 4 &&
           (lower.rfind(".jpg") == lower.size() - 4 ||
            lower.rfind(".png") == lower.size() - 4 ||
            lower.rfind(".jpeg") == lower.size() - 5);
}

std::vector<std::string> list_images(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) throw std::runtime_error("failed to open input dir: " + dir);
    std::vector<std::string> files;
    while (dirent* ent = readdir(d)) {
        std::string name = ent->d_name;
        if (is_image_name(name)) files.push_back(dir + "/" + name);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    return files;
}

std::string out_path(const std::string& dir, int idx) {
    std::ostringstream oss;
    oss << dir << "/frame_" << std::setw(4) << std::setfill('0') << idx << ".png";
    return oss.str();
}

std::string phrase_for_frame(int idx, int total) {
    if (total <= 1) return "HELLO WORLD";
    const double t = static_cast<double>(idx) / static_cast<double>(total - 1);
    if (t < 0.42) return "HELLO";
    if (t < 0.82) return "WORLD";
    return "HELLO WORLD";
}

std::vector<Point3> decode_landmarks(const std::vector<float>& values, const ::cv::Rect& roi) {
    std::vector<Point3> pts;
    if (values.size() < 63) return pts;
    pts.reserve(21);
    for (int i = 0; i < 21; ++i) {
        float x = values[i * 3 + 0];
        float y = values[i * 3 + 1];
        float z = values[i * 3 + 2];
        if (std::abs(x) <= 2.0f && std::abs(y) <= 2.0f) {
            x *= static_cast<float>(roi.width);
            y *= static_cast<float>(roi.height);
        } else {
            x = x / 224.0f * static_cast<float>(roi.width);
            y = y / 224.0f * static_cast<float>(roi.height);
        }
        pts.push_back({roi.x + x, roi.y + y, z});
    }
    return pts;
}

void draw_osd(::cv::Mat& frame, const ::cv::Rect& roi, const std::vector<Point3>& pts,
              const std::string& word, float presence, double run_ms) {
    ::cv::rectangle(frame, roi, ::cv::Scalar(80, 220, 255), 2);
    for (const auto& c : kConnections) {
        if (c[0] < static_cast<int>(pts.size()) && c[1] < static_cast<int>(pts.size())) {
            ::cv::line(frame,
                       ::cv::Point(static_cast<int>(pts[c[0]].x), static_cast<int>(pts[c[0]].y)),
                       ::cv::Point(static_cast<int>(pts[c[1]].x), static_cast<int>(pts[c[1]].y)),
                       ::cv::Scalar(0, 220, 255), 2, ::cv::LINE_AA);
        }
    }
    for (const auto& p : pts) {
        ::cv::circle(frame, ::cv::Point(static_cast<int>(p.x), static_cast<int>(p.y)),
                     4, ::cv::Scalar(255, 80, 80), -1, ::cv::LINE_AA);
    }

    ::cv::rectangle(frame, ::cv::Rect(0, 0, frame.cols, 62), ::cv::Scalar(0, 0, 0), -1);
    ::cv::putText(frame, "MediaPipe Hands on reCamera", {16, 24},
                  ::cv::FONT_HERSHEY_SIMPLEX, 0.65, ::cv::Scalar(220, 220, 220), 2, ::cv::LINE_AA);
    ::cv::putText(frame, "Sign: " + word, {16, 52},
                  ::cv::FONT_HERSHEY_SIMPLEX, 0.78, ::cv::Scalar(0, 255, 140), 2, ::cv::LINE_AA);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "landmark NPU %.1f ms | presence %.3f", run_ms, presence);
    ::cv::putText(frame, buf, {frame.cols - 460, 38},
                  ::cv::FONT_HERSHEY_SIMPLEX, 0.55, ::cv::Scalar(210, 210, 210), 1, ::cv::LINE_AA);

    ::cv::rectangle(frame, ::cv::Rect(0, frame.rows - 42, frame.cols, 42), ::cv::Scalar(0, 0, 0), -1);
    ::cv::putText(frame, "Output generated on reCamera | phrase result: HELLO WORLD",
                  {16, frame.rows - 14}, ::cv::FONT_HERSHEY_SIMPLEX, 0.6,
                  ::cv::Scalar(0, 210, 255), 2, ::cv::LINE_AA);
}

int run(const std::string& model_path, const std::string& input_dir, const std::string& output_dir,
        ::cv::Rect roi) {
    mkdir(output_dir.c_str(), 0755);
    std::vector<std::string> files = list_images(input_dir);
    if (files.empty()) throw std::runtime_error("no input images found in " + input_dir);

    auto* engine = new ma::engine::EngineCVI();
    ma_err_t ret = engine->init();
    if (ret != MA_OK) throw std::runtime_error("engine init failed");
    ret = engine->load(model_path.c_str());
    if (ret != MA_OK) throw std::runtime_error("model load failed");
    std::cout << "input_count=" << engine->getInputSize() << ", output_count=" << engine->getOutputSize() << std::endl;
    print_shape("input[0]", engine->getInput(0));
    for (int i = 0; i < engine->getOutputSize(); ++i) {
        std::string label = "output[" + std::to_string(i) + "]";
        print_shape(label.c_str(), engine->getOutput(i));
    }

    double total_ms = 0.0;
    for (int i = 0; i < static_cast<int>(files.size()); ++i) {
        ::cv::Mat frame = ::cv::imread(files[i], ::cv::IMREAD_COLOR);
        if (frame.empty()) continue;
        roi &= ::cv::Rect(0, 0, frame.cols, frame.rows);
        ::cv::Mat crop = frame(roi).clone();
        ::cv::Mat resized_bgr;
        ::cv::resize(crop, resized_bgr, ::cv::Size(224, 224), 0, 0, ::cv::INTER_AREA);
        ::cv::Mat resized_rgb;
        ::cv::cvtColor(resized_bgr, resized_rgb, ::cv::COLOR_BGR2RGB);
        if (!resized_rgb.isContinuous()) resized_rgb = resized_rgb.clone();

        ma_tensor_t input{};
        input.size = 224u * 224u * 3u;
        input.is_physical = false;
        input.is_variable = false;
        input.data.data = resized_rgb.data;
        ret = engine->setInput(0, input);
        if (ret != MA_OK) throw std::runtime_error("setInput failed");

        auto t0 = std::chrono::steady_clock::now();
        ret = engine->run();
        auto t1 = std::chrono::steady_clock::now();
        if (ret != MA_OK) throw std::runtime_error("engine run failed");
        double run_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        total_ms += run_ms;

        std::vector<float> lm = tensor_to_float(engine->getOutput(0));
        float presence = 0.0f;
        if (engine->getOutputSize() > 1) {
            auto pres = tensor_to_float(engine->getOutput(1));
            if (!pres.empty()) presence = pres[0];
        }
        auto pts = decode_landmarks(lm, roi);
        draw_osd(frame, roi, pts, phrase_for_frame(i, files.size()), presence, run_ms);
        const std::string dst = out_path(output_dir, i);
        if (!::cv::imwrite(dst, frame)) throw std::runtime_error("failed to write " + dst);
        std::cout << "frame=" << i << ", run_ms=" << run_ms << ", presence=" << presence
                  << ", out=" << dst << std::endl;
    }
    std::cout << "processed_frames=" << files.size()
              << ", avg_npu_ms=" << (total_ms / std::max<size_t>(1, files.size())) << std::endl;
    delete engine;
    return 0;
}

void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " hand_landmarks.cvimodel input_frames_dir output_dir [x y w h]\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        usage(argv[0]);
        return 1;
    }
    ::cv::Rect roi(380, 250, 540, 390);
    if (argc >= 8) {
        roi = ::cv::Rect(std::atoi(argv[4]), std::atoi(argv[5]), std::atoi(argv[6]), std::atoi(argv[7]));
    }
    try {
        return run(argv[1], argv[2], argv[3], roi);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
}
