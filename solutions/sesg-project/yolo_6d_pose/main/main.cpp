#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "sscma.h"

using namespace ma;

static std::atomic<bool> g_running{true};
static void sig_handler(int sig) { g_running.store(false); }

#define TAG "main"

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <model_path> [threshold]\n", argv[0]);
        return 1;
    }

    const char* model_path = argv[1];
    float threshold = (argc > 2) ? atof(argv[2]) : 0.60f;

    printf("model: %s\n", model_path);
    printf("threshold: %.2f\n", threshold);

    auto* engine = new ma::engine::EngineCVI();
    if (engine->init() != MA_OK || engine->load(model_path) != MA_OK) {
        printf("E engine init/load failed\n");
        delete engine;
        return 1;
    }

    printf("Engine loaded successfully. Input size: %d, Output size: %d\n", 
           engine->getInputSize(), engine->getOutputSize());

    for (int i = 0; i < engine->getOutputSize(); ++i) {
        ma_shape_t shape = engine->getOutputShape(i);
        printf("Output %d shape: [", i);
        for (uint32_t s = 0; s < shape.size; ++s) printf("%d%s", shape.dims[s], s == shape.size-1 ? "" : ",");
        printf("]\n");
    }

    int input_w = 640;
    int input_h = 480;

    ma::Device* device = ma::Device::getInstance();
    ma::Camera* camera = nullptr;
    for (auto& sensor : device->getSensors()) {
        if (sensor->getType() == ma::Sensor::Type::kCamera) {
            camera = static_cast<ma::Camera*>(sensor);
            camera->init(0);
            ma::Camera::CtrlValue val;

            val.i32 = 0;
            camera->commandCtrl(ma::Camera::CtrlType::kChannel, ma::Camera::CtrlMode::kWrite, val);
            val.u16s[0] = input_w;
            val.u16s[1] = input_h;
            camera->commandCtrl(ma::Camera::CtrlType::kWindow, ma::Camera::CtrlMode::kWrite, val);
            val.i32 = 1;
            camera->commandCtrl(ma::Camera::CtrlType::kPhysical, ma::Camera::CtrlMode::kWrite, val);
            break;
        }
    }

    if (!camera) { 
        printf("E No camera found\n"); 
        return 1; 
    }

    camera->startStream(ma::Camera::StreamMode::kRefreshOnReturn);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("Starting inference loop...\n");

    auto fps_start = std::chrono::high_resolution_clock::now();
    int fps_frame_count = 0;

    while (g_running.load()) {
        ma_img_t frame;
        if (camera->retrieveFrame(frame, MA_PIXEL_FORMAT_RGB888) != MA_OK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        fps_frame_count++;

        ma_tensor_t tensor;
        memset(&tensor, 0, sizeof(tensor));
        tensor.size = frame.size;
        tensor.is_physical = true;
        tensor.is_variable = false;
        tensor.data.data = reinterpret_cast<void*>(frame.data);
        
        engine->setInput(0, tensor);

        if (engine->run() != MA_OK) {
            printf("E Engine run failed\n");
            break;
        }

        ma_tensor_t out = engine->getOutput(0);
        float* data = (float*)out.data.data;
        
        camera->returnFrame(frame);

        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - fps_start).count();
        if (elapsed_ms >= 5000) {
            const float fps = (elapsed_ms > 0) ? (fps_frame_count * 1000.0f / (float)elapsed_ms) : 0.0f;
            printf("[stats] fps=%.1f. Output[0] sample: %.4f, %.4f, %.4f\n", fps, data[0], data[1], data[2]);
            fps_start = now;
            fps_frame_count = 0;
        }
    }

    camera->stopStream();
    for (auto& sensor : device->getSensors()) sensor->deInit();
    delete engine;

    printf("Demo finished.\n");
    return 0;
}
