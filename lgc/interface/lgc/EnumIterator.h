/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021 Google LLC. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  EnumIterator.h
 * @brief LGC header file: defines utilities for iterating over enums
 ***********************************************************************************************************************
 */
#pragma once

#include "llvm/ADT/Sequence.h"
#include <cassert>
#include <type_traits>

namespace lgc {

// Common LGC traits for enums with continuous values, i.e., `first, first + 1, ..., end - 1`.
// This is intended to extend `llvm::enum_iteration_traits`, so that we can use it both with `lgc::enumRange`,
// and, internally, with `llvm::enum_seq`.
template <typename EnumT, EnumT first, EnumT end> struct continuous_enum_iteration_traits {
  // LLVM traits for `llvm::enum_iteration_traits`.
  static_assert(std::is_enum<EnumT>::value, "enum_traits can only be used with enums");
  static constexpr bool is_iterable = true; // NOLINT

  // Custom LGC traits not present in LLVM. We put them in a nested struct to avoid potential clashes with future
  // members added to `llvm::enum_iteration_traits`.
  struct lgc_traits {
    using underlying_type = std::underlying_type_t<EnumT>;

    static constexpr EnumT first_value = first;                                                    // NOLINT
    static constexpr underlying_type first_underlying_value = static_cast<underlying_type>(first); // NOLINT
    static constexpr EnumT end_value = end;                                                        // NOLINT
    static constexpr underlying_type end_underlying_value = static_cast<underlying_type>(end);     // NOLINT
  };
};

// Common enum_iteration_traits for continuous enums that start at 0 and have a `::Count` value indicating the number of
// values.
template <typename EnumT>
struct default_continuous_enum_iteration_traits : continuous_enum_iteration_traits<EnumT, EnumT{}, EnumT::Count> {};

// Convenience macro to make an enum iterable. Requires the enum type to start at 0 and have a `::Count` value
// indicating the number of values. Must be used in the `llvm` namespace.
#define LGC_DEFINE_DEFAULT_ITERABLE_ENUM(ENUM_TYPE)                                                                    \
  template <> struct enum_iteration_traits<ENUM_TYPE> : lgc::default_continuous_enum_iteration_traits<ENUM_TYPE> {}

// Convenience macro to make an enum iterable. Requires the enum type to start at zero and have a COUNT_VALUE value
// indicating the number of values. Must be used in the `llvm` namespace.
#define LGC_DEFINE_ZERO_BASED_ITERABLE_ENUM(ENUM_TYPE, COUNT_VALUE)                                                    \
  template <>                                                                                                          \
  struct enum_iteration_traits<ENUM_TYPE>                                                                              \
      : lgc::continuous_enum_iteration_traits<ENUM_TYPE, ENUM_TYPE{}, COUNT_VALUE> {}

// =====================================================================================================================
// Converts enum to its underlying integer value.
//
// @param value : The enum value.
// @returns : Copy of the enum value converted to its integer type.
template <typename EnumT> constexpr std::underlying_type_t<EnumT> toUnderlying(EnumT value) {
  return static_cast<std::underlying_type_t<EnumT>>(value);
}

// =====================================================================================================================
// Returns the number of enum values in an iterable enum. Typically, this matches the `::Count` value.
//
// @returns : The number of enum values in `EnumT`.
template <typename EnumT> constexpr size_t enumCount() {
  return static_cast<size_t>(llvm::enum_iteration_traits<EnumT>::lgc_traits::end_underlying_value -
                             llvm::enum_iteration_traits<EnumT>::lgc_traits::first_underlying_value);
}

// =====================================================================================================================
// Creates the range of enum values: `[begin, end)`. By default, this will range over all enum values (except
// `::Count`). EnumT must provide the `enum_iteration_traits::lgc_traits` traits.
// Has the same semantics as `llvm::enum_seq` but performs extra safety checks.
// See https://llvm.org/doxygen/Sequence_8h.html for usage details and examples.
//
// @param begin : The first value in the range. By default, this is the enum value `0`.
// @param end : The one-past the last value in the range. By default, this is the enum value `::Count`.
// @returns : The enum range: `begin, begin + 1, ..., end - 1`.
template <typename EnumT> llvm::iota_range<EnumT> enumRange(EnumT begin, EnumT end) {
  using LgcTraitsT = typename llvm::enum_iteration_traits<EnumT>::lgc_traits;
  constexpr auto firstValue = LgcTraitsT::first_underlying_value;
  constexpr auto endValue = LgcTraitsT::end_underlying_value;
  (void)firstValue;
  (void)endValue;
  assert(toUnderlying(begin) <= toUnderlying(end) && "Invalid enum range");
  assert(toUnderlying(begin) >= firstValue && "Invalid enum value");
  assert(toUnderlying(end) <= endValue && "Invalid enum value");
  return llvm::enum_seq(begin, end);
}

// =====================================================================================================================
// Creates the range of enum values: `[first_val, end)`. See `enumRange(EnumT begin, EnumT end)` above.
//
// @param end : The one-past the last value in the range. By default, this is the enum value `::Count`.
// @returns : The enum range: `first_value, first_value + 1, ..., end - 1`.
template <typename EnumT> llvm::iota_range<EnumT> enumRange(EnumT end) {
  return enumRange(llvm::enum_iteration_traits<EnumT>::lgc_traits::first_value, end);
}

// =====================================================================================================================
// Creates the range of all enum values of `EnumT`. See `enumRange(EnumT begin, EnumT end)` above.
//
// @returns : The enum range: `first_value, first_value + 1, ..., end_value - 1`.
template <typename EnumT> llvm::iota_range<EnumT> enumRange() {
  return enumRange(llvm::enum_iteration_traits<EnumT>::lgc_traits::first_value,
                   llvm::enum_iteration_traits<EnumT>::lgc_traits::end_value);
}

} // namespace lgc
