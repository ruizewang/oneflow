/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#ifndef ONEFLOW_CORE_COMMON_CONVERTER_H_
#define ONEFLOW_CORE_COMMON_CONVERTER_H_

#include <cuda_runtime.h>
#include <cstdint>
#include <limits>
#include <type_traits>
#include "oneflow/core/common/data_type.h"

namespace oneflow {

template<typename T>
struct IsFloatingOrHalf {
  static constexpr bool value = IsFloating<T>::value || IsFloat16<T>::value;
};

template<typename T>
struct IsArithmeticOrHalf {
  static constexpr bool value = std::is_arithmetic<T>::value || IsFloat16<T>::value;
};

template<typename From, typename To>
struct NeedsClamp {
  static constexpr bool from_fp = IsFloatingOrHalf<From>::value;
  static constexpr bool to_fp = IsFloatingOrHalf<To>::value;
  static constexpr bool from_unsigned = std::is_unsigned<From>::value;
  static constexpr bool to_unsigned = std::is_unsigned<To>::value;
  static constexpr bool value =
      // to smaller type of same kind (fp, int)
      (from_fp == to_fp && sizeof(To) < sizeof(From)) ||
      // fp32 has range in excess of (u)int64
      (from_fp && !to_fp) ||
      // converting to unsigned requires clamping negatives to zero
      (!from_unsigned && to_unsigned) ||
      // zero-extending signed unsigned integers requires more bits
      (from_unsigned && !to_unsigned && sizeof(To) <= sizeof(From));
};

template<typename To>
struct NeedsClamp<bool, To> {
  static constexpr bool value = false;
};

template<typename T>
struct ret_type {
  constexpr ret_type() = default;
};

// floating-point and signed integer -> floating-point and signed integer
template<typename T, typename U>
OF_DEVICE_FUNC constexpr std::enable_if_t<
    NeedsClamp<U, T>::value && std::is_signed<U>::value && std::is_signed<T>::value, T>
Clamp(U value, ret_type<T>) {
  return value <= GetMinVal<T>() ? GetMinVal<T>()
                                 : value >= GetMaxVal<T>() ? GetMaxVal<T>() : static_cast<T>(value);
}

// floating-point -> unsigned types
template<typename T, typename U>
OF_DEVICE_FUNC constexpr std::enable_if_t<NeedsClamp<U, T>::value && std::is_signed<U>::value
                                              && IsFloatingOrHalf<U>::value
                                              && std::is_unsigned<T>::value,
                                          T>
Clamp(U value, ret_type<T>) {
  return value <= GetMinVal<T>() ? GetMinVal<T>()
                                 : value >= GetMaxVal<T>() ? GetMaxVal<T>() : static_cast<T>(value);
}

// signed integer types -> unsigned types
template<typename T, typename U>
OF_DEVICE_FUNC constexpr std::enable_if_t<NeedsClamp<U, T>::value && std::is_signed<U>::value
                                              && std::is_integral<U>::value
                                              && std::is_unsigned<T>::value,
                                          T>
Clamp(U value, ret_type<T>) {
  return value <= 0 ? 0
                    : static_cast<std::make_unsigned_t<U>>(value) >= GetMaxVal<T>()
                          ? GetMaxVal<T>()
                          : static_cast<T>(value);
}

// unsigned types -> any types
template<typename T, typename U>
OF_DEVICE_FUNC constexpr std::enable_if_t<NeedsClamp<U, T>::value && std::is_unsigned<U>::value, T>
Clamp(U value, ret_type<T>) {
  return value >= GetMaxVal<T>() ? GetMaxVal<T>() : static_cast<T>(value);
}

// not clamp
template<typename T, typename U>
OF_DEVICE_FUNC constexpr std::enable_if_t<!NeedsClamp<U, T>::value, T> Clamp(U value, ret_type<T>) {
  return value;
}

OF_DEVICE_FUNC constexpr int32_t Clamp(uint32_t value, ret_type<int32_t>) {
  return value & 0x80000000u ? 0x7fffffff : value;
}

OF_DEVICE_FUNC constexpr uint32_t Clamp(int32_t value, ret_type<uint32_t>) {
  return value < 0 ? 0u : value;
}

OF_DEVICE_FUNC constexpr int32_t Clamp(int64_t value, ret_type<int32_t>) {
  return value < static_cast<int64_t>(GetMinVal<int32_t>())
             ? GetMinVal<int32_t>()
             : value > static_cast<int64_t>(GetMaxVal<int32_t>()) ? GetMaxVal<int32_t>()
                                                                  : static_cast<int32_t>(value);
}

template<>
OF_DEVICE_FUNC constexpr int32_t Clamp(uint64_t value, ret_type<int32_t>) {
  return value > static_cast<uint64_t>(GetMaxVal<int32_t>()) ? GetMaxVal<int32_t>()
                                                             : static_cast<int32_t>(value);
}

template<>
OF_DEVICE_FUNC constexpr uint32_t Clamp(int64_t value, ret_type<uint32_t>) {
  return value < 0
             ? 0
             : value > static_cast<int64_t>(GetMaxVal<uint32_t>()) ? GetMaxVal<uint32_t>()
                                                                   : static_cast<uint32_t>(value);
}

template<>
OF_DEVICE_FUNC constexpr uint32_t Clamp(uint64_t value, ret_type<uint32_t>) {
  return value > static_cast<uint64_t>(GetMaxVal<uint32_t>()) ? GetMaxVal<uint32_t>()
                                                              : static_cast<uint32_t>(value);
}

template<typename T>
OF_DEVICE_FUNC constexpr bool Clamp(T value, ret_type<bool>) {
  return static_cast<bool>(value);
}

template<typename T>
OF_DEVICE_FUNC constexpr float16 Clamp(T value, ret_type<float16>) {
  return static_cast<float16>(Clamp(value, ret_type<float>()) < GetMinVal<float16>()
                                  ? GetMinVal<float16>()
                                  : Clamp(value, ret_type<float>()) > GetMaxVal<float16>()
                                        ? GetMaxVal<float16>()
                                        : Clamp(value, ret_type<float>()));
}

template<typename T>
OF_DEVICE_FUNC constexpr T Clamp(float16 value, ret_type<T>) {
  return Clamp(static_cast<float>(value), ret_type<T>());
}

OF_DEVICE_FUNC constexpr float16 Clamp(float16 value, ret_type<float16>) { return value; }

template<typename T, typename U>
OF_DEVICE_FUNC constexpr T Clamp(U value) {
  return Clamp(value, ret_type<T>());
}

namespace {
#ifdef __CUDA_ARCH__

inline __device__ int cuda_round_helper(float f, int) { return __float2int_rn(f); }

inline __device__ unsigned cuda_round_helper(float f, unsigned) { return __float2uint_rn(f); }

inline __device__ long long cuda_round_helper(float f, long long) {
  return __float2ll_rd(f + 0.5f);
}

inline __device__ unsigned long long cuda_round_helper(float f, unsigned long long) {
  return __float2ull_rd(f + 0.5f);
}

inline __device__ long cuda_round_helper(float f, long) {
  return sizeof(long) == sizeof(int) ? __float2int_rn(f) : __float2ll_rd(f + 0.5f);
}

inline __device__ unsigned long cuda_round_helper(float f, unsigned long) {
  return sizeof(unsigned long) == sizeof(unsigned int) ? __float2uint_rn(f)
                                                       : __float2ull_rd(f + 0.5f);
}

inline __device__ int cuda_round_helper(double f, int) { return __double2int_rn(f); }

inline __device__ unsigned cuda_round_helper(double f, unsigned) { return __double2uint_rn(f); }

inline __device__ long long cuda_round_helper(double f, long long) {
  return __double2ll_rd(f + 0.5f);
}

inline __device__ unsigned long long cuda_round_helper(double f, unsigned long long) {
  return __double2ull_rd(f + 0.5f);
}

inline __device__ long cuda_round_helper(double f, long) {
  return sizeof(long) == sizeof(int) ? __double2int_rn(f) : __double2ll_rd(f + 0.5f);
}

inline __device__ unsigned long cuda_round_helper(double f, unsigned long) {
  return sizeof(unsigned long) == sizeof(unsigned int) ? __double2uint_rn(f)
                                                       : __double2ull_rd(f + 0.5f);
}
#endif

template<typename Out, typename In, bool OutIsFp = IsFloatingOrHalf<Out>::value,
         bool InIsFp = IsFloatingOrHalf<In>::value>
struct ConverterBase;

template<typename Out, typename In>
struct Converter : ConverterBase<Out, In> {
  static_assert(IsArithmeticOrHalf<Out>::value && IsArithmeticOrHalf<In>::value,
                "Default ConverterBase can only be used with arithmetic types.");
};

// Converts between two FP types
template<typename Out, typename In>
struct ConverterBase<Out, In, true, true> {
  OF_DEVICE_FUNC static constexpr Out Convert(In value) { return value; }
  OF_DEVICE_FUNC static constexpr Out ConvertNorm(In value) { return value; }
  OF_DEVICE_FUNC static constexpr Out ConvertSat(In value) { return value; }
  OF_DEVICE_FUNC static constexpr Out ConvertSatNorm(In value) { return value; }
};

// Converts integral to FP type
template<typename Out, typename In>
struct ConverterBase<Out, In, true, false> {
  OF_DEVICE_FUNC static constexpr Out Convert(In value) { return value; }
  OF_DEVICE_FUNC static constexpr Out ConvertSat(In value) { return value; }
  OF_DEVICE_FUNC static constexpr Out ConvertNorm(In value) {
    return value * (Out(1) / (GetMaxVal<In>()));
  }
  OF_DEVICE_FUNC static constexpr Out ConvertSatNorm(In value) {
    return value * (Out(1) / (GetMaxVal<In>()));
  }
};

// Converts integral to float16
template<typename In>
struct ConverterBase<float16, In, true, false> {
  OF_DEVICE_FUNC static constexpr float16 Convert(In value) {
    auto out = ConverterBase<float, In, true, false>::Convert(value);
    return static_cast<float16>(out);
  }

  OF_DEVICE_FUNC static constexpr float16 ConvertSat(In value) {
    auto out = ConverterBase<float, In, true, false>::ConvertSat(value);
    return static_cast<float16>(out);
  }

  OF_DEVICE_FUNC static constexpr float16 ConvertNorm(In value) {
    auto out = ConverterBase<float, In, true, false>::ConvertNorm(value);
    return static_cast<float16>(out);
  }

  OF_DEVICE_FUNC static constexpr float16 ConvertSatNorm(In value) {
    auto out = ConverterBase<float, In, true, false>::ConvertSatNorm(value);
    return static_cast<float16>(out);
  }
};

// Converts FP to integral type
template<typename Out, typename In>
struct ConverterBase<Out, In, false, true> {
  OF_DEVICE_FUNC static constexpr Out Convert(In value) {
#ifdef __CUDA_ARCH__
    return Clamp<Out>(cuda_round_helper(value, Out()));
#else
    return Clamp<Out>(std::round(value));
#endif
  }

  OF_DEVICE_FUNC static constexpr Out ConvertSat(In value) {
#ifdef __CUDA_ARCH__
    return Clamp<Out>(cuda_round_helper(value, Out()));
#else
    return Clamp<Out>(std::round(value));
#endif
  }

  OF_DEVICE_FUNC static constexpr Out ConvertNorm(In value) {
#ifdef __CUDA_ARCH__
    return Clamp<Out>(cuda_round_helper(value * GetMaxVal<Out>(), Out()));
#else
    return std::round(value * GetMaxVal<Out>());
#endif
  }

  OF_DEVICE_FUNC static constexpr Out ConvertSatNorm(In value) {
#ifdef __CUDA_ARCH__
    return std::is_signed<Out>::value
               ? Clamp<Out>(cuda_round_helper(value * GetMaxVal<Out>(), Out()))
               : cuda_round_helper(GetMaxVal<Out>() * __saturatef(value), Out());
#else
    return Clamp<Out>(std::round(value * GetMaxVal<Out>()));
#endif
  }
};

// Converts signed to signed, unsigned to unsigned or unsigned to signed
template<typename Out, typename In, bool IsOutSigned = std::is_signed<Out>::value,
         bool IsInSigned = std::is_signed<In>::value>
struct ConvertIntInt {
  OF_DEVICE_FUNC static constexpr Out Convert(In value) { return value; }
  OF_DEVICE_FUNC static constexpr Out ConvertNorm(In value) {
    return Converter<Out, float>::Convert(value * (1.0f * GetMaxVal<Out>() / GetMaxVal<In>()));
  }
  OF_DEVICE_FUNC static constexpr Out ConvertSat(In value) { return Clamp<Out>(value); }
  OF_DEVICE_FUNC static constexpr Out ConvertSatNorm(In value) { return ConvertNorm(value); }
};

// Converts signed to unsigned integer
template<typename Out, typename In>
struct ConvertIntInt<Out, In, false, true> {
  OF_DEVICE_FUNC static constexpr Out Convert(In value) { return value; }
  OF_DEVICE_FUNC static constexpr Out ConvertNorm(In value) {
    return Converter<Out, float>::Convert(value * (1.0f * GetMaxVal<Out>() / GetMaxVal<In>()));
  }
  OF_DEVICE_FUNC static constexpr Out ConvertSat(In value) { return Clamp<Out>(value); }
  OF_DEVICE_FUNC static constexpr Out ConvertSatNorm(In value) {
#ifdef __CUDA_ARCH__
    return cuda_round_helper(__saturatef(value * (1.0f / GetMaxVal<In>())) * GetMaxVal<Out>());
#else
    return value < 0 ? 0 : ConvertNorm(value);
  }
#endif
  };

  // Converts between integral types
  template<typename Out, typename In>
  struct ConverterBase<Out, In, false, false> : ConvertIntInt<Out, In> {
    static_assert(IsArithmeticOrHalf<Out>::value && IsArithmeticOrHalf<In>::value,
                  "Default ConverterBase can only be used with arithmetic types.");
  };

  // Pass-through conversion
  template<typename T>
  struct Converter<T, T> {
    static OF_DEVICE_FUNC constexpr T Convert(T value) { return value; }
    static OF_DEVICE_FUNC constexpr T ConvertSat(T value) { return value; }
    static OF_DEVICE_FUNC constexpr T ConvertNorm(T value) { return value; }
    static OF_DEVICE_FUNC constexpr T ConvertSatNorm(T value) { return value; }
  };

  template<typename raw_out, typename raw_in>
  using converter_t =
      Converter<std::remove_cv_t<raw_out>, std::remove_cv_t<std::remove_reference_t<raw_in>>>;

}  // namespace

template<typename Out, typename In>
OF_DEVICE_FUNC constexpr Out Convert(In value) {
  return converter_t<Out, In>::Convert(value);
}

template<typename Out, typename In>
OF_DEVICE_FUNC constexpr Out ConvertNorm(In value) {
  return converter_t<Out, In>::ConvertNorm(value);
}

template<typename Out, typename In>
OF_DEVICE_FUNC constexpr Out ConvertSat(In value) {
  return converter_t<Out, In>::ConvertSat(value);
}

template<typename Out, typename In>
OF_DEVICE_FUNC constexpr Out ConvertSatNorm(In value) {
  return converter_t<Out, In>::ConvertSatNorm(value);
}

}  // namespace oneflow

#endif  // ONEFLOW_CORE_COMMON_CONVERTER_H_