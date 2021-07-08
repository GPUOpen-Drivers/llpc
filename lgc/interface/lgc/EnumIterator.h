/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/iterator.h"
#include "llvm/ADT/iterator_range.h"
#include <array>
#include <cassert>
#include <iterator>
#include <type_traits>

namespace lgc {

// Traits providing information about how to iterate C++ enum types. By default, enums are not considered iterable.
// To make an enum type iterable, provide a specialization of this struct for you enum. This will allow you to use
// `lgc::enum_iterator` and `lgc::enum_range()`.
// For now, only continuous enums are supported.
template <typename EnumT> struct iterable_enum_traits {
  static_assert(std::is_enum<EnumT>::value, "enum_traits can only be used with enums");
  using underlying_type = std::underlying_type_t<EnumT>;
  static constexpr bool is_iteratable = false;

  /*
  Specializations must provide the following:
  static constexpr EnumT first_value = ...;
  static constexpr underlying_type first_underlying_value = ...;
  static constexpr EnumT end_value = ...;
  static constexpr underlying_type end_underlying_value = ...;
  */
};

// Common iterable_enum_traits for enums with continuous values, i.e., `first, first + 1, ..., end - 1`.
template <typename EnumT, EnumT first, EnumT end> struct continuous_iterable_enum_traits {
  static_assert(std::is_enum<EnumT>::value, "enum_traits can only be used with enums");
  using underlying_type = std::underlying_type_t<EnumT>;
  static constexpr bool is_iterable = true;

  static constexpr EnumT first_value = first;
  static constexpr underlying_type first_underlying_value = static_cast<underlying_type>(first);
  static constexpr EnumT end_value = end;
  static constexpr underlying_type end_underlying_value = static_cast<underlying_type>(end);
};

// Common iterable_enum_traits for continuous enums that start at 0 and have a `::Count` value indicating the number of
// values.
template <typename EnumT>
struct default_continuous_iterable_enum_traits : continuous_iterable_enum_traits<EnumT, EnumT{}, EnumT::Count> {};

// Convenience macro to make an enum iterable. Requires the enum type to start at 0 and have a `::Count` value
// indicating the number of values. Must be used in the `lgc` namespace.
#define LGC_DEFINE_DEFAULT_ITERABLE_ENUM(ENUM_TYPE)                                                                    \
  template <> struct iterable_enum_traits<ENUM_TYPE> : default_continuous_iterable_enum_traits<ENUM_TYPE> {}

// Convenience macro to make an enum iterable. Requires the enum type to start at zero and have a COUNT_VALUE value
// indicating the number of values. Must be used in the `lgc` namespace.
#define LGC_DEFINE_ZERO_BASED_ITERABLE_ENUM(ENUM_TYPE, COUNT_VALUE)                                                    \
  template <>                                                                                                          \
  struct iterable_enum_traits<ENUM_TYPE> : continuous_iterable_enum_traits<ENUM_TYPE, ENUM_TYPE{}, COUNT_VALUE> {}

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
  return static_cast<size_t>(iterable_enum_traits<EnumT>::end_underlying_value -
                             iterable_enum_traits<EnumT>::first_underlying_value);
}

// Random access iterator for iterating over enums. Enum types must implement the `iterable_enum_traits` by providing
// its specialization.
// Implementation inspired by `value_sequence_iterator` from `llvm/ADT/Sequence.h`.
template <typename EnumT>
class enum_iterator
    : public llvm::iterator_facade_base<enum_iterator<EnumT>, std::random_access_iterator_tag, const EnumT> {
  using BaseT = typename enum_iterator::iterator_facade_base;
  using TraitsT = iterable_enum_traits<EnumT>;
  static_assert(TraitsT::is_iterable, "enum not iterable or iterable_enum_traits missing");

public:
  using UnderlyingT = typename TraitsT::underlying_type;
  using DifferenceT = typename BaseT::difference_type;
  using ReferenceT = typename BaseT::reference;

  // Default constructor creates an end iterator.
  enum_iterator() = default;

  enum_iterator(EnumT value) : m_value(value) {
    assert(toUnderlying(m_value) >= TraitsT::first_underlying_value && "Invalid enum value");
    assert(toUnderlying(m_value) <= TraitsT::end_underlying_value && "Invalid enum value");
  }

  // Copyable and moveable.
  enum_iterator(const enum_iterator &) = default;
  enum_iterator &operator=(const enum_iterator &) = default;
  enum_iterator(enum_iterator &&) = default;
  enum_iterator &operator=(enum_iterator &&) = default;

  enum_iterator &operator+=(DifferenceT n) {
    m_value = toEnum(toUnderlying(m_value) + n);
    assert(toUnderlying(m_value) <= TraitsT::end_underlying_value && "Invalid enum value");
    return *this;
  }
  enum_iterator &operator-=(DifferenceT n) {
    m_value = toEnum(toUnderlying(m_value) - n);
    assert(toUnderlying(m_value) >= TraitsT::first_underlying_value && "Invalid enum value");
    return *this;
  }
  using BaseT::operator-;
  DifferenceT operator-(const enum_iterator &rhs) const { return toUnderlying(m_value) - toUnderlying(rhs.m_value); }

  bool operator==(const enum_iterator &rhs) const { return m_value == rhs.m_value; }
  bool operator<(const enum_iterator &rhs) const { return toUnderlying(m_value) < toUnderlying(rhs.m_value); }
  ReferenceT operator*() const { return m_value; }

private:
  static EnumT toEnum(UnderlyingT value) { return static_cast<EnumT>(value); }

  EnumT m_value = TraitsT::end_value;
};

// =====================================================================================================================
// Creates the range of enum values: `[begin, end)`. By default, this will range over all enum values (except
// `::Count`). EnumT must provide the `iterable_enum_traits` trait. E.g.:
// - for (MyEnum value : enumRange<MyEnum>())  // Iterates over all values of `MyEnum`.
// - for (MyEnum value : enumRange(MyEnum::C))  // Iterates over values of `MyEnum` until (but excluding `C`).
// - llvm::is_contained(enumRange(MyEnum::A, MyEnum::C), value)  // Checks if `value` is in `[A, C)` (without `C`).
// - llvm::is_contained(enumRange(MyEnum::A, enumInc(MyEnum::C)), value)  // Checks if `value` is in `[A, C]`.
//
// @param begin : The first value in the range. By default, this is the enum value `0`.
// @param end : The one-past the last value in the range. By default, this is the enum value `::Count`.
// @returns : The enum range: `begin, begin + 1, ..., end - 1`.
template <typename EnumT> llvm::iterator_range<enum_iterator<EnumT>> enumRange(EnumT begin, EnumT end) {
  return {enum_iterator<EnumT>(begin), enum_iterator<EnumT>(end)};
}

// =====================================================================================================================
// Creates the range of enum values: `[first_val, end)`. See `enumRange(EnumT begin, EnumT end)` above.
//
// @param end : The one-past the last value in the range. By default, this is the enum value `::Count`.
// @returns : The enum range: `first_value, first_value + 1, ..., end - 1`.
template <typename EnumT> llvm::iterator_range<enum_iterator<EnumT>> enumRange(EnumT end) {
  return enumRange<EnumT>(iterable_enum_traits<EnumT>::first_value, end);
}

// =====================================================================================================================
// Creates the range of all enum values of `EnumT`. See `enumRange(EnumT begin, EnumT end)` above.
//
// @returns : The enum range: `first_value, first_value + 1, ..., end_value - 1`.
template <typename EnumT> llvm::iterator_range<enum_iterator<EnumT>> enumRange() {
  return enumRange<EnumT>(iterable_enum_traits<EnumT>::first_value, iterable_enum_traits<EnumT>::end_value);
}

} // namespace lgc
