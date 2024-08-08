/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2020-2024 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 *  deal in the Software without restriction, including without limitation the
 *  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 *  sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *  IN THE SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  LoweringPointerTupleMap.h
 * @brief A key-value map from keys to tuples of pointers that is optimized for value and type lowering uses
 ***********************************************************************************************************************
 */

#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include <type_traits>
#include <vector>

namespace CompilerUtils {

/// @brief A key-value map from pointer keys to tuples of pointers that is optimized for value and type lowering uses
///
/// This map is optimized using two main assumptions:
///
///  1. The vast majority of keys are mapped to a single pointer.
///  2. Keys can be updated, but this happens rarely, and there is no need to reclaim memory except when the map as a
///     whole is destroyed.
///
/// Pointers and references into the map are @em not stable.
///
/// The map does not distinguish between missing entries and entries mapped to an empty tuple.
///
/// The map can optionally track all places in which a value appears, for an efficient implementation of
/// @ref replaceAllUsesOfWith.
template <typename KeyT, typename ValueT, bool TrackReverse> class LoweringPointerTupleMap {
  template <typename T> struct AlignOfPointee;

  template <typename PointeeT> struct AlignOfPointee<PointeeT *> { static constexpr size_t value = alignof(PointeeT); };

  static_assert(std::is_pointer_v<ValueT>);
  static_assert(AlignOfPointee<ValueT>::value >= 2);

  static_assert(std::is_pointer_v<KeyT>);
  static_assert(AlignOfPointee<KeyT>::value >= 2);

  using Map = llvm::DenseMap<KeyT, uintptr_t>;

  // Tracking the reverse mapping takes up some space which we only use if it was requested.
  struct Empty {};
  using ReverseMap = std::conditional_t<TrackReverse, llvm::DenseMap<ValueT, llvm::SmallVector<uintptr_t>>, Empty>;

  /// If requested, track the locations in which each value is mentioned.
  ReverseMap m_reverseMap;

  /// Map keys to values.
  ///
  /// For 1-1 mappings, this stores a value pointer.
  /// For 1-N mappings, this stores ((index << 1) | 1), where index is the index
  /// into m_extra.
  Map m_map;

  /// (size, values...) sequences stored contiguously for every 1-N mapping.
  std::vector<uintptr_t> m_extra;

  llvm::ArrayRef<ValueT> decode(const uintptr_t *encoded) const {
    if ((*encoded & 1) == 0)
      return llvm::ArrayRef(reinterpret_cast<const ValueT *>(encoded), 1);

    size_t index = *encoded >> 1;
    assert(index < m_extra.size());
    size_t count = m_extra[index];
    assert(count >= 2);
    assert(count < m_extra.size() - index);
    const ValueT *begin = reinterpret_cast<const ValueT *>(&m_extra[index + 1]);
    return llvm::ArrayRef(begin, begin + count);
  }

public:
  bool empty() const { return m_map.empty(); }
  size_t size() const { return m_map.size(); }

  void clear() {
    m_map.clear();
    m_extra.clear();

    if constexpr (TrackReverse)
      m_reverseMap.clear();
  }

  llvm::ArrayRef<ValueT> lookup(KeyT key) const {
    auto it = m_map.find(key);
    if (it == m_map.end())
      return {};

    return decode(&it->second);
  }

  KeyT lookupUniqueKey(ValueT value) const {
    static_assert(TrackReverse);

    auto it = m_reverseMap.find(value);
    if (it == m_reverseMap.end())
      return nullptr;

    uintptr_t keyValue = it->second[0];
    if (keyValue & 1u) // 1-N mapping, keyValue is N number but not key ptr
      return nullptr;

    return reinterpret_cast<KeyT>(it->second[0]);
  }

  llvm::ArrayRef<ValueT> set(KeyT key, llvm::ArrayRef<ValueT> value) {
    if (value.empty()) {
      auto it = m_map.find(key);
      if (it == m_map.end())
        return {};

      if constexpr (TrackReverse)
        this->clearReverseMap(it);
      m_map.erase(it);
      return {};
    }

    auto [it, inserted] = m_map.try_emplace(key);
    if constexpr (TrackReverse) {
      if (!inserted)
        this->clearReverseMap(it);
    }

    uintptr_t encoded;

    if (value.size() == 1) {
      encoded = reinterpret_cast<uintptr_t>(value[0]);
      assert((encoded & 1) == 0);

      if constexpr (TrackReverse)
        m_reverseMap[value[0]].push_back(reinterpret_cast<uintptr_t>(key));
    } else {
      size_t index = m_extra.size();

      m_extra.push_back(value.size());
      for (size_t i = 0; i < value.size(); ++i) {
        // Use a memcpy to store the value because even though m_extra is a
        // vector of uintptr_t, we will access it via ValueT pointers later.
        m_extra.emplace_back();
        memcpy(&m_extra.back(), &value[i], sizeof(uintptr_t));
        if constexpr (TrackReverse)
          m_reverseMap[value[i]].push_back(((index + 1 + i) << 1) | 1);
      }

      encoded = (index << 1) | 1;
      assert((encoded >> 1) == index);
    }

    it->second = encoded;

    return decode(&it->second);
  }

  // Replace a value that may have previously been recorded as part of a mapping
  // with another value.
  //
  // If the values in the map are `llvm::Value *`s, this can be used if RAUW is
  // performed on IR, as in:
  // @code
  //   toReplace->replaceAllUsesWith(with);
  //   map.replaceAllUsesWith(toReplace, with);
  // @endcode
  //
  // Note that this method is only available when the TrackReverse template
  // parameter is set to true.
  //
  // @param toReplace : the mapping value to be replaced
  // @param with : the new value to replace it with in all mappings in which
  //               it appears
  void replaceAllUsesOfWith(ValueT toReplace, ValueT with) {
    static_assert(TrackReverse);

    if (toReplace == with)
      return;

    auto toReplaceIt = m_reverseMap.find(toReplace);
    if (toReplaceIt == m_reverseMap.end())
      return;

    llvm::SmallVector<uintptr_t> occurrences = std::move(toReplaceIt->second);
    m_reverseMap.erase(toReplaceIt);

    for (uintptr_t occurrence : occurrences) {
      if (occurrence & 1) {
        // Use a memcpy to store the value because even though m_extra is a
        // vector of uintptr_t, we will access it via ValueT pointers later.
        memcpy(&m_extra[occurrence >> 1], &with, sizeof(uintptr_t));
      } else {
        m_map.find(reinterpret_cast<KeyT>(occurrence))->second = reinterpret_cast<uintptr_t>(with);
      }
    }

    auto withIt = m_reverseMap.find(with);
    if (withIt != m_reverseMap.end()) {
      withIt->second.append(occurrences);
    } else {
      m_reverseMap.try_emplace(with, std::move(occurrences));
    }
  }

private:
  // Clear the reverse map for all entries in mapIt.
  void clearReverseMap(typename Map::iterator mapIt) {
    auto clearEntry = [&](ValueT oldValue, uintptr_t occurrence) {
      auto it = m_reverseMap.find(oldValue);
      assert(it != m_reverseMap.end());
      if (it->second.size() == 1) {
        assert(it->second[0] == occurrence);
        m_reverseMap.erase(it);
      } else {
        *llvm::find(it->second, occurrence) = it->second.back();
        it->second.pop_back();
      }
    };

    if ((mapIt->second & 1) == 0) {
      ValueT oldValue = reinterpret_cast<ValueT>(mapIt->second);
      clearEntry(oldValue, reinterpret_cast<uintptr_t>(mapIt->first));
    } else {
      size_t index = mapIt->second >> 1;
      size_t count = m_extra[index];
      for (size_t i = 0; i != count; ++i)
        clearEntry(reinterpret_cast<ValueT>(m_extra[index + 1 + i]), ((index + 1 + i) << 1) | 1);
    }
  }
};

} // namespace CompilerUtils
