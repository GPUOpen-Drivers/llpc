/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2024 Advanced Micro Devices, Inc. All Rights Reserved.
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

// MsgPackScanner class to read, write and incrementally update MsgPack.
//
// For the case that the caller has a small number of elements it wants to look at and it knows their names
// and positions in the MsgPack document hierarchy, MsgPackScanner provides a more efficient way of doing
// it than MsgPackDocument. MsgPackDocument builds the whole document hierarchy with maps, and thus has a
// lot of malloc traffic, even for parts of the document that the caller is not interested in. MsgPackScanner
// only creates a single map of the elements that the caller is interested in, so has a lot less malloc
// traffic.
//
// A future project could be to change the LLVM AMDGPU PALMetadata class to use this, in the case that we are
// compiling (it is being called from AsmPrinter), rather than assembling or disassembling. Then, this
// code would need to be upstreamed in LLVM, with some tests.
//
// TODO: Does not yet implement deleting an element.
//
// Usage:
//
// 1. Write a spec for the structure of the MsgPack document and items within it that you want to read, modify
//    or write. That is done with a static struct variable containing variables of type MsgPackScanner::Item
//    Where an item appears in a map, you give it the key name to match.
//
// 2. Construct a MsgPackScanner::Spec object, passing it a pointer to the struct in (1).
//    This can be done one time for multiple MsgPackScanners, to common up the processing it does (constructing
//    a map of the item names).
//
// 3. Construct a MsgPackScanner object, passing it the MsgPackScanner::Spec from (2).
//
// 4. Scan a MsgPack blob using MsgPackScanner::scan() (optional, to handle the case that your code is constructing
//    a new MsgPack blob). You can give scan() a callback function, called when an item in your spec has just
//    been found; with that, your spec can have an anonymous item in a map, and the callback gets called for
//    each found entry in the corresponding map in the MsgPack blob.
//
// 5. Use isSet() to tell if an item is set (if it was matched in the scan), and asBool(), asInt(), asString() to
//    get a item's value.
//
// 6. Use setBool() and set() to update an item to a new value. If the item does not already exist in the MsgPack
//    blob, this creates it, and any parent maps and arrays that need creating, right up to the top-level item
//    if this is the first setBool()/set() and you are creating a new MsgPack blob.
//
// 7. Use write() to write the updated MsgPack blob.

#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

namespace lgc {

// =====================================================================================================================
// MsgPackScanner class to read, write and incrementally update MsgPack.
class MsgPackScanner {
public:
  // Item types in the item array that the caller passes to the MsgPackScanner::Spec constructor.
  enum class ItemType : unsigned {
    First = 1489898298, // Arbitrarily chosen magic number
    Scalar = First,
    EndContainer,
    Map,
    Array,
    Last = Array
  };

  // Struct for item in spec. The MsgPackScanner is passed an array of Item, each with an ItemType and optional
  // name, which the caller can construct as a struct.
  // MsgPackScanner treats it as a tree, where Map or Array contain further items until the matching EndContainer.
  // Items directly contained in a Map item can be named, to match the MsgPack map key, or unnamed, in which case
  // it matches any key. The latter is only useful if the caller provides a callback function to scan() so it gets
  // called each time an item is matched.
  // The whole spec is either a single ItemType::Scalar, or an ItemType::Map/ItemType::Array with child
  // elements, terminated by ItemType::EndContainer. Nested maps/arrays must also be correctly terminated.
  struct Item {
    ItemType itemType;
    const char *name;
  };

  // A representation of the spec set up for MsgPackScanner. A client might want to set up one of these in a
  // static variable, then use it to create and use multiple MsgPackScanner objects, possibly concurrently.
  class Spec {
  public:
    // Constructor given pointer to caller's struct containing Items. The supplied struct must remain valid
    // for the lifetime of the MsgPackScanner::Spec, which must remain valid for the lifetime of any
    // MsgPackScanner using it.
    Spec(const void *itemStruct);

    // Accessors.
    size_t size() const { return m_itemArray.size(); }
    const Item &operator[](size_t idx) const { return m_itemArray[idx]; }
    llvm::ArrayRef<Item>::iterator begin() const { return m_itemArray.begin(); }
    llvm::ArrayRef<Item>::iterator end() const { return m_itemArray.end(); }
    // Look up a {key, parent item index}, giving an item index. Key is one of:
    // - FNV-1a hash of name for map; or
    // - 0 for anonymous map entry; or
    // - index for array entry.
    std::optional<unsigned> lookup(unsigned key, unsigned parentItemIndex) const;
    // Given an item index, get the parent index, or UINT_MAX if none (it is the root item).
    unsigned getParentIndex(unsigned index) const { return m_parentIndices[index]; }

  private:
    // Supplied spec.
    llvm::ArrayRef<Item> m_itemArray;
    // Map from {name, parent item index} to item index.
    llvm::DenseMap<std::pair<unsigned, unsigned>, unsigned> m_itemMap;
    // Parent item index for each item.
    llvm::SmallVector<unsigned> m_parentIndices;
  };

  // Constructor given Spec object.
  MsgPackScanner(const Spec &spec);

  // Scan a MsgPack blob. Returns error for illegal MsgPack format, but tolerates empty blob.
  // Cam only be called once for this MsgPackScanner object, and must be called before other methods.
  // The StringRef for the blob is retained, as it is used in subsequent method calls.
  // Each item that is matched has its position in the MsgPack blob remembered, so that the caller can make
  // subsequent isSet(), asBool(), asInt(), asString(), setBool(), set() calls on it.
  // The callback is called just after finding an item in the item array, allowing the caller to accumulate
  // a value from an item that occurs multiple times.
  llvm::Error scan(llvm::StringRef blob, llvm::function_ref<llvm::Error(MsgPackScanner &, const Item &)> callback = {});

  // Subsequent methods specify a particular item in the spec by passing a const reference to that item in
  // the struct that the caller passed to the MsgPackScanner::Spec constructor.

  // Determine whether an item is set.
  bool isSet(const Item &item) const;

  // Get an item as a bool. Returns {} if the item has some other type, or was not found.
  std::optional<bool> asBool(const Item &item) const;

  // Get an item as an integer. Returns {} if the item has some other type, or was not found.
  std::optional<uint64_t> asInt(const Item &item) const;

  // Get an item as a StringRef. Works for a string or binary object.
  // Returns {} if the item has some other type, or was not found.
  std::optional<llvm::StringRef> asString(const Item &item) const;

  // Set an item as a bool. This gets a different name to avoid implicit conversions from other types to bool.
  // If the item does not exist, it is created, increasing the size of its parent map/array. If the parent
  // map/array does not exist, it is created, and so on.
  void setBool(const Item &item, bool value);

  // Set an item as an unsigned integer.
  // If the item does not exist, it is created, increasing the size of its parent map/array. If the parent
  // map/array does not exist, it is created, and so on.
  void set(const Item &item, uint64_t value);

  // Set an item as a string.
  // If the item does not exist, it is created, increasing the size of its parent map/array. If the parent
  // map/array does not exist, it is created, and so on.
  void set(const Item &item, llvm::StringRef value);

  // Write the whole MsgPack to the stream, as modified by any set() and setBool() calls made on it.
  void write(llvm::raw_ostream &stream);

private:
  // Get size of next object.
  llvm::Expected<unsigned> getObjectSize() const;

  // Get an item's MsgPack-encoded value.
  llvm::StringRef getValue(const Item &item) const;

  // Set an item to the new value that has just been written in MsgPack format to m_newData.
  size_t setValue(const Item &item, size_t newOffset, size_t newSize);

  // Item info gathered during scan, one for each item in the supplied spec.
  struct ItemInfo {
    constexpr static const size_t NoNewOffset = ~size_t(0);
    constexpr static const size_t NoReplacementNewSize = ~size_t(0);

    size_t keyOffset;                  // Offset of key, only if this item is a map entry
    size_t offset;                     // Offset of value
    size_t size;                       // Size of value (just the header for map/array)
    size_t endOffset;                  // End offset, only for map or array
    size_t newKeyOffset = NoNewOffset; // Offset of new key in m_newData, or NoNewOffset
    size_t newKeySize;                 // Size of new key
    size_t newOffset = NoNewOffset;    // Offset of new value (from set()) in m_newData, or NoNewOffset
    size_t newSize;                    // Size of new value (from set()), NoReplacementNewSize if deleting old item
                                       //  without replacing it
    unsigned gen;                      // Generation of new data, used to ensure that we action multiple inserts at
                                       //  the same offset in the order we created them
  };

  const Spec &m_spec;
#ifndef NDEBUG
  bool m_inUse = false; // For asserting if user calls scan() after set() or setBool() or scan().
#endif
  llvm::StringRef m_blob;
  size_t m_next;
  llvm::SmallVector<ItemInfo> m_itemInfos;
  llvm::SmallString<64> m_newData;
  // Generation of new data, used to ensure that we action multiple inserts at the same offset in the
  // order we created them.
  unsigned m_gen = 0;
};

} // namespace lgc
