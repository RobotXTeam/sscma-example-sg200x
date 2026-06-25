/**
 * @file engine_utils.h
 * @brief EngineCVI 通用工具：dtype 转换、shape numel、输入打包、输出读取。

 */

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <sscma.h>
#include <vector>

namespace hand {

// ---- dtype 转换（对齐 age_gender_race_runner.cpp）----
inline float bf16_to_fp32(uint16_t v) {
    uint32_t u = (uint32_t)v << 16;
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

inline float fp16_to_fp32(uint16_t v) {
    const uint32_t sign = (uint32_t)(v & 0x8000u) << 16;
    const uint32_t exp  = (v & 0x7C00u) >> 10;
    const uint32_t mant = (v & 0x03FFu);

    uint32_t out;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;
        } else {
            uint32_t m = mant;
            uint32_t e = 0;
            while ((m & 0x0400u) == 0) {
                m <<= 1;
                e++;
            }
            m &= 0x03FFu;
            out = sign | ((127u - 15u - e) << 23) | (m << 13);
        }
    } else if (exp == 0x1Fu) {
        out = sign | 0x7F800000u | (mant << 13);
    } else {
        out = sign | ((exp + (127u - 15u)) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &out, sizeof(f));
    return f;
}

inline uint16_t fp32_to_bf16(float v) {
    uint32_t u;
    std::memcpy(&u, &v, sizeof(u));
    return (uint16_t)(u >> 16);
}

inline uint16_t fp32_to_fp16(float v) {
    uint32_t u;
    std::memcpy(&u, &v, sizeof(u));
    const uint32_t sign = (u >> 31) & 1u;
    int exp = (int)((u >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = u & 0x7FFFFFu;

    if (exp <= 0) {
        if (exp < -10) return (uint16_t)(sign << 15);
        mant |= 0x800000u;
        const int shift = 14 - exp;
        return (uint16_t)((sign << 15) | (mant >> shift));
    }
    if (exp >= 31) {
        return (uint16_t)((sign << 15) | 0x7C00u);
    }
    return (uint16_t)((sign << 15) | ((uint16_t)exp << 10) | (uint16_t)(mant >> 13));
}

inline size_t tensor_elem_size(ma_tensor_type_t t) {
    switch (t) {
        case MA_TENSOR_TYPE_F32: return 4;
        case MA_TENSOR_TYPE_F16: return 2;
        case MA_TENSOR_TYPE_BF16: return 2;
        case MA_TENSOR_TYPE_S8: return 1;
        case MA_TENSOR_TYPE_U8: return 1;
        default: return 0;
    }
}

inline size_t shape_numel(const ma_shape_t& s) {
    if (s.size <= 0) return 0;
    size_t n = 1;
    for (int i = 0; i < s.size; ++i) {
        if (s.dims[i] <= 0) return 0;
        n *= (size_t)s.dims[i];
    }
    return n;
}

inline const float* reinterpret_to_float(const ma_tensor_t& t) {
    if (t.type == MA_TENSOR_TYPE_F32) return t.data.f32;
    return nullptr;
}

// 读取输出 tensor 的第 idx 个值为 float（含反量化，对齐 read_val）
inline float read_val(const ma_tensor_t& t, int idx) {
    switch (t.type) {
        case MA_TENSOR_TYPE_F32: return t.data.f32[idx];
        case MA_TENSOR_TYPE_F16: return fp16_to_fp32(t.data.u16[idx]);
        case MA_TENSOR_TYPE_BF16: return bf16_to_fp32(t.data.u16[idx]);
        case MA_TENSOR_TYPE_S8: {
            const float scale = t.quant_param.scale;
            const int zp = t.quant_param.zero_point;
            return ((int)t.data.s8[idx] - zp) * scale;
        }
        case MA_TENSOR_TYPE_U8: {
            const float scale = t.quant_param.scale;
            const int zp = t.quant_param.zero_point;
            return ((int)t.data.u8[idx] - zp) * scale;
        }
        default: return 0.f;
    }
}

// 输入打包缓冲：根据 type 自动选择底层 vector。
struct InputBuf {
    std::vector<uint8_t>  u8;
    std::vector<int8_t>   s8;
    std::vector<uint16_t> u16;  // F16 / BF16
    std::vector<float>    f32;

    void resize_for(ma_tensor_type_t t, size_t n) {
        u8.clear(); s8.clear(); u16.clear(); f32.clear();
        switch (t) {
            case MA_TENSOR_TYPE_U8: u8.assign(n, 0); break;
            case MA_TENSOR_TYPE_S8: s8.assign(n, 0); break;
            case MA_TENSOR_TYPE_F16:
            case MA_TENSOR_TYPE_BF16: u16.assign(n, 0); break;
            case MA_TENSOR_TYPE_F32: f32.assign(n, 0.f); break;
            default: break;
        }
    }

    void* data_for(ma_tensor_type_t t) {
        switch (t) {
            case MA_TENSOR_TYPE_U8: return u8.data();
            case MA_TENSOR_TYPE_S8: return s8.data();
            case MA_TENSOR_TYPE_F16:
            case MA_TENSOR_TYPE_BF16: return u16.data();
            case MA_TENSOR_TYPE_F32: return f32.data();
            default: return nullptr;
        }
    }
};

// 将一个 float 实值按 tensor 的 type/quant_param 写入底层 buffer 第 idx 位。
inline void store_val(InputBuf& buf, ma_tensor_type_t t,
                      const ma_quant_param_t& qp, size_t idx, float real) {
    switch (t) {
        case MA_TENSOR_TYPE_F32: buf.f32[idx] = real; break;
        case MA_TENSOR_TYPE_BF16: buf.u16[idx] = fp32_to_bf16(real); break;
        case MA_TENSOR_TYPE_F16: buf.u16[idx] = fp32_to_fp16(real); break;
        case MA_TENSOR_TYPE_S8: {
            const float scale = qp.scale;
            const int zp = qp.zero_point;
            const float inv = (scale > 0.f) ? (1.0f / scale) : 0.f;
            int q = (int)std::lround(real * inv) + zp;
            if (q < -128) q = -128;
            if (q > 127) q = 127;
            buf.s8[idx] = (int8_t)q;
            break;
        }
        case MA_TENSOR_TYPE_U8: {
            const float scale = qp.scale;
            const int zp = qp.zero_point;
            const float inv = (scale > 0.f) ? (1.0f / scale) : 0.f;
            int q = (int)std::lround(real * inv) + zp;
            if (q < 0) q = 0;
            if (q > 255) q = 255;
            buf.u8[idx] = (uint8_t)q;
            break;
        }
        default: break;
    }
}

// 构造 ma_tensor_t 以提交 setInput（绑定到 InputBuf）。
inline ma_tensor_t make_input_tensor(ma_tensor_type_t t, InputBuf& buf, size_t numel) {
    ma_tensor_t tensor{};
    tensor.size = numel * tensor_elem_size(t);
    tensor.is_physical = false;
    tensor.is_variable = false;
    tensor.data.data = buf.data_for(t);
    return tensor;
}

}  // namespace hand