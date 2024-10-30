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

#include "lgc/util/MsgPackScanner.h"
#include "llvm/BinaryFormat/MsgPack.h"
#include "llvm/BinaryFormat/MsgPackReader.h"
#include "llvm/BinaryFormat/MsgPackWriter.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"

#define DEBUG_TYPE "msgpack-scanner"

using namespace lgc;
using namespace llvm;
using namespace llvm::msgpack;
using namespace llvm::support;

// =====================================================================================================================
// Generate a run-time 32-bit hash of the specified string using the FNV-1a hash algorithm.
static uint32_t fnv1aHash(llvm::StringRef str) {
  // FNV-1a hash offset
  static constexpr uint32_t Fnv1aOffset = 2166136261u;
  // FNV-1a hash prime
  static constexpr uint32_t Fnv1aPrime = 16777619u;

  uint32_t hash = Fnv1aOffset;
  for (char c : str) {
    hash ^= uint8_t(c);
    hash *= Fnv1aPrime;
  }
  return hash;
}

// =====================================================================================================================
// Spec object constructor given reference to caller's struct containing Items. The supplied struct must remain valid
// for the lifetime of the MsgPackScanner::Spec, which must remain valid for the lifetime of any
// MsgPackScanner using it.
MsgPackScanner::Spec::Spec(const void *itemStruct) {
  // Scan to find the size of the array by looking at nested maps and arrays.
  const Item *items = static_cast<const Item *>(itemStruct);
  unsigned level = 0;
  for (unsigned idx = 0;; ++idx) {
    const Item &item = items[idx];
    assert(item.itemType >= ItemType::First && item.itemType <= ItemType::Last);
    if (item.itemType == ItemType::EndContainer) {
      if (--level == 0) {
        m_itemArray = ArrayRef<Item>(items, idx + 1);
        break;
      }
      continue;
    }
    if (item.itemType == ItemType::Array || item.itemType == ItemType::Map) {
      ++level;
      continue;
    }
    if (level == 0) {
      // Spec has only one item if it is not map or array.
      assert(idx == 0);
      m_itemArray = ArrayRef<Item>(items, 1);
      break;
    }
  }

  m_parentIndices.resize(m_itemArray.size());
  m_parentIndices[0] = UINT_MAX;
  // Add items to map so they can be found when scanning msgpack.
  struct StackLevel {
    unsigned itemIndex;
    bool isMap;
    unsigned childIndex;
  };
  SmallVector<StackLevel> stack;
  stack.push_back({});
  for (unsigned itemIndex = 1; itemIndex != m_itemArray.size(); ++itemIndex) {
    const Item &item = m_itemArray[itemIndex];
    m_parentIndices[itemIndex] = stack.back().itemIndex;
    if (item.itemType != ItemType::EndContainer) {
      if (item.name) {
        // Item has a map key.
        bool inserted = m_itemMap.insert({{fnv1aHash(item.name), stack.back().itemIndex}, itemIndex}).second;
        assert(inserted && "Duplicate name at this level in MsgPackScanner spec");
        (void)inserted;
        LLVM_DEBUG(dbgs() << "Item " << itemIndex << " is name " << item.name << " parent index "
                          << stack.back().itemIndex << "\n");
      } else {
        // No map key; make up our own array index.
        m_itemMap.insert({{stack.back().childIndex, stack.back().itemIndex}, itemIndex});
        LLVM_DEBUG(dbgs() << "Item " << itemIndex << " is name " << stack.back().childIndex << " parent index "
                          << stack.back().itemIndex << "\n");
      }
    }
    // Only increment childIndex for an array. Anonymous map entry items always get index 0, meaning that we
    // can map multiple map entries against it.
    if (!stack.back().isMap)
      ++stack.back().childIndex;
    if (item.itemType == ItemType::Map || item.itemType == ItemType::Array)
      stack.push_back({itemIndex});
    else if (item.itemType == ItemType::EndContainer) {
      stack.pop_back();
      assert((!stack.empty() || itemIndex == m_itemArray.size() - 1) && "Bad MsgPackScanner spec");
    }
  }
}

// =====================================================================================================================
// Look up a {key, parent item index}, giving an item index. Key is one of:
// - FNV-1a hash of name for map; or
// - 0 for anonymous map entry; or
// - index for array entry.
std::optional<unsigned> MsgPackScanner::Spec::lookup(unsigned key, unsigned parentItemIndex) const {
  auto it = m_itemMap.find({key, parentItemIndex});
  if (it == m_itemMap.end())
    return {};
  return it->second;
}

// =====================================================================================================================
// Constructor given Spec object.
MsgPackScanner::MsgPackScanner(const Spec &spec) : m_spec(spec) {
  m_itemInfos.resize(m_spec.size());
}

// =====================================================================================================================
// Scan a MsgPack blob. Returns error for illegal MsgPack format.
// The callback is called just after finding an item in the item array, allowing the caller to accumulate
// a value from an item that occurs multiple times (typically as a named child of a map where the map is specified
// as an anonymous child of an outer map).
Error MsgPackScanner::scan(StringRef blob, function_ref<Error(MsgPackScanner &, const Item &)> callback) {
#ifndef NDEBUG
  assert(!m_inUse);
  m_inUse = true;
#endif
  m_blob = blob;

  // The top of stack StackLevel represents:
  // - an in-progress skipping array or skipping or non-skipping map, if remaining is non-zero;
  // - otherwise, an item about to be filled in.
  struct StackLevel {
    unsigned itemIndex;
    size_t childCount;
    bool isMap;
    size_t childIndex;
  };
  SmallVector<StackLevel> stack;
  stack.push_back({UINT_MAX, 1});

  unsigned itemIndex = 0; // First object always attached to item index 0 in spec
  unsigned objectSize = 0;
  m_next = 0;
  while (!stack.empty()) {
    m_next += objectSize;
    if (m_next == m_blob.size())
      return make_error<StringError>("MsgPack truncated", std::make_error_code(std::errc::invalid_argument));
    // Get size of next object.
    Expected<unsigned> objectSizeOr = getObjectSize();
    if (Error err = objectSizeOr.takeError())
      return err;
    objectSize = *objectSizeOr;
    // Get next object.
    Object obj;
    {
      Reader reader(m_blob.drop_front(m_next));
      if (Error err = reader.read(obj).takeError())
        return err;
    }

    LLVM_DEBUG({
      for (unsigned i = 0; i != stack.size(); ++i)
        dbgs() << "  ";
      dbgs() << m_next << ": ";
      switch (obj.Kind) {
      case Type::Int:
        dbgs() << "int " << obj.Int;
        break;
      case Type::UInt:
        dbgs() << "uint " << obj.UInt;
        break;
      case Type::Nil:
        dbgs() << "nil";
        break;
      case Type::Boolean:
        dbgs() << "boolean " << obj.Bool;
        break;
      case Type::Float:
        dbgs() << "float " << obj.Float;
        break;
      case Type::String:
        dbgs() << "string " << obj.Raw;
        break;
      case Type::Binary:
        dbgs() << "binary " << obj.Raw;
        break;
      case Type::Array:
        dbgs() << "array " << obj.Length;
        break;
      case Type::Map:
        dbgs() << "map " << obj.Length;
        break;
      case Type::Extension:
        dbgs() << "extension " << obj.Extension.Type << " " << obj.Extension.Bytes;
        break;
      default:
        dbgs() << "unknown";
        break;
      }
    });

    if (itemIndex != UINT_MAX) {
      // itemIndex is already set, either because this is the first time round the loop (the first object is
      // always attached to item index 0), or because the previous loop read a map key that matched one we
      // are looking for, so we are now on the value for that key.
    } else if (stack.back().itemIndex != UINT_MAX) {
      // Set itemIndex to the item matching the current object in the parent map or array being scanned.
      if (stack.back().isMap) {
        // Check for this object being the key in a map. (A map has Length*2 children, where, counting from 0,
        // the even numbered ones are the keys and the odd numbered ones are the values.)
        if (stack.back().childIndex % 2 == 0) {
          if (obj.Kind == Type::String) {
            unsigned key = fnv1aHash(obj.Raw);
            LLVM_DEBUG(dbgs() << " (checking name " << key << " parent " << stack.back().itemIndex << ")");
            std::optional<unsigned> found = m_spec.lookup(key, stack.back().itemIndex);
            if (found) {
              itemIndex = found.value();
              m_itemInfos[itemIndex].keyOffset = m_next;
              ++stack.back().childIndex;
              LLVM_DEBUG(dbgs() << ": key for item " << itemIndex << "\n");
              continue; // Loop back for the value corresponding to this key.
            }
          }
          // Check for a match for an anonymous item in a map.
          LLVM_DEBUG(dbgs() << " (checking name 0 parent " << stack.back().itemIndex << ")");
          std::optional<unsigned> found = m_spec.lookup(0, stack.back().itemIndex);
          if (found) {
            itemIndex = found.value();
            m_itemInfos[itemIndex].keyOffset = m_next;
            ++stack.back().childIndex;
            LLVM_DEBUG(dbgs() << ": key for item " << itemIndex << "\n");
            continue; // Loop back for the value corresponding to this key.
          }
        }
      }
      if (!stack.back().isMap) {
        // Check for this object being an array element.
        unsigned key = stack.back().childIndex;
        LLVM_DEBUG(dbgs() << " (checking name " << key << " parent " << stack.back().itemIndex << ")");
        std::optional<unsigned> found = m_spec.lookup(key, stack.back().itemIndex);
        if (found)
          itemIndex = found.value();
      }
    }

    if (itemIndex != UINT_MAX) {
      // This object is being attached to an item in the spec.
      m_itemInfos[itemIndex].offset = m_next;
      m_itemInfos[itemIndex].size = objectSize;
      LLVM_DEBUG(dbgs() << ": item " << itemIndex);
      if (itemIndex != UINT_MAX && callback) {
        if (Error err = callback(*this, m_spec[itemIndex]))
          return err;
      }
    }
    LLVM_DEBUG(dbgs() << "\n");

    if (obj.Kind == Type::Map && obj.Length != 0) {
      // Start a new map. It has Length {key,value} pairs of entries, thus Length*2 entries.
      stack.push_back({itemIndex, obj.Length * 2, /*isMap=*/true});
    } else if (obj.Kind == Type::Array && obj.Length != 0) {
      // Start a new array.
      stack.push_back({itemIndex, obj.Length, /*isMap=*/false});
    } else {
      // Increment count on current container; pop if at end.
      while (++stack.back().childIndex == stack.back().childCount) {
        unsigned poppingItemIndex = stack.back().itemIndex;
        if (poppingItemIndex != UINT_MAX)
          m_itemInfos[poppingItemIndex].endOffset = m_next + objectSize;
        stack.pop_back();
        LLVM_DEBUG({
          for (unsigned i = 0; i != stack.size(); ++i)
            dbgs() << "  ";
          dbgs() << "pop\n";
        });
        if (stack.empty())
          break;
      }
    }
    itemIndex = UINT_MAX;
  }
  LLVM_DEBUG(dbgs() << "Finished msgpack scan\n");
  return Error::success();
}

// =====================================================================================================================
// Get size of next object.
// We use MsgPackReader to read the next object, but it does not tell us how big the object is, so we have
// to figure that out for ourselves. For an array or map, the object size does not include the enclosed
// elements.
// If we upstream this code into LLVM, then we could instead add a public method to MsgPackReader to get
// its next pointer.
Expected<unsigned> MsgPackScanner::getObjectSize() const {
  unsigned firstByte = uint8_t(m_blob[m_next]);
  switch (firstByte) {

  case FirstByte::Int8:
  case FirstByte::UInt8:
    return 1 + sizeof(int8_t);

  case FirstByte::Int16:
  case FirstByte::UInt16:
    return 1 + sizeof(int16_t);

  case FirstByte::Int32:
  case FirstByte::UInt32:
    return 1 + sizeof(int32_t);

  case FirstByte::Int64:
  case FirstByte::UInt64:
    return 1 + sizeof(int64_t);

  case FirstByte::Float32:
    return 1 + sizeof(float);

  case FirstByte::Float64:
    return 1 + sizeof(double);

  case FirstByte::Str8:
  case FirstByte::Bin8:
    if (m_next + 1 + sizeof(uint8_t) > m_blob.size())
      return make_error<StringError>("MsgPack truncated", std::make_error_code(std::errc::invalid_argument));
    return 1 + sizeof(uint8_t) + endian::read<uint8_t, Endianness>(m_blob.data() + m_next + 1);

  case FirstByte::Str16:
  case FirstByte::Bin16:
    if (m_next + 1 + sizeof(uint16_t) > m_blob.size())
      return make_error<StringError>("MsgPack truncated", std::make_error_code(std::errc::invalid_argument));
    return 1 + sizeof(uint16_t) + endian::read<uint16_t, Endianness>(m_blob.data() + m_next + 1);

  case FirstByte::Str32:
  case FirstByte::Bin32:
    if (m_next + 1 + sizeof(uint32_t) > m_blob.size())
      return make_error<StringError>("MsgPack truncated", std::make_error_code(std::errc::invalid_argument));
    return 1 + sizeof(uint32_t) + endian::read<uint32_t, Endianness>(m_blob.data() + m_next + 1);

  case FirstByte::Array16:
  case FirstByte::Map16:
    return 1 + sizeof(uint16_t);

  case FirstByte::Array32:
  case FirstByte::Map32:
    return 1 + sizeof(uint32_t);

  case FirstByte::FixExt1:
    return 1 + 1 + 1;

  case FirstByte::FixExt2:
    return 1 + 1 + 2;

  case FirstByte::FixExt4:
    return 1 + 1 + 4;

  case FirstByte::FixExt8:
    return 1 + 1 + 8;

  case FirstByte::FixExt16:
    return 1 + 1 + 16;

  case FirstByte::Ext8:
    if (m_next + 1 + sizeof(uint8_t) > m_blob.size())
      return make_error<StringError>("MsgPack truncated", std::make_error_code(std::errc::invalid_argument));
    return 1 + 1 + sizeof(uint8_t) + endian::read<uint8_t, Endianness>(m_blob.data() + m_next + 1 + 1);

  case FirstByte::Ext16:
    if (m_next + 1 + sizeof(uint16_t) > m_blob.size())
      return make_error<StringError>("MsgPack truncated", std::make_error_code(std::errc::invalid_argument));
    return 1 + 1 + sizeof(uint16_t) + endian::read<uint16_t, Endianness>(m_blob.data() + m_next + 1 + 1);

  case FirstByte::Ext32:
    if (m_next + 1 + sizeof(uint32_t) > m_blob.size())
      return make_error<StringError>("MsgPack truncated", std::make_error_code(std::errc::invalid_argument));
    return 1 + 1 + sizeof(uint32_t) + endian::read<uint32_t, Endianness>(m_blob.data() + m_next + 1 + 1);

  default:
    if ((firstByte & FixBitsMask::String) == FixBits::String)
      return 1 + firstByte & 0x1f;
    return 1;
  }
}

// =====================================================================================================================
// Determine whether an item is set.
bool MsgPackScanner::isSet(const Item &item) const {
  return !getValue(item).empty();
}

// =====================================================================================================================
// Get an item as a bool. Returns {} if the item has some other type, or was not found.
std::optional<bool> MsgPackScanner::asBool(const Item &item) const {
  StringRef value = getValue(item);
  if (value.empty())
    return {}; // Item not found

  Object obj;
  Reader reader(value);
  void(bool(reader.read(obj))); // Check success, and assert on failure
  if (obj.Kind == Type::Boolean)
    return obj.Bool;
  return {};
}

// =====================================================================================================================
// Get an item as an integer. Returns {} if the item has some other type, or was not found.
std::optional<uint64_t> MsgPackScanner::asInt(const Item &item) const {
  StringRef value = getValue(item);
  if (value.empty())
    return {}; // Item not found

  Object obj;
  Reader reader(value);
  void(bool(reader.read(obj))); // Check success, and assert on failure
  if (obj.Kind == Type::UInt)
    return obj.UInt;
  if (obj.Kind == Type::Int)
    return obj.Int;
  return {};
}

// =====================================================================================================================
// Get an item as a StringRef. Returns {} if the item has some other type, or was not found.
std::optional<StringRef> MsgPackScanner::asString(const Item &item) const {
  StringRef value = getValue(item);
  if (value.empty())
    return {}; // Item not found

  Object obj;
  Reader reader(value);
  void(bool(reader.read(obj))); // Check success, and assert on failure
  if (obj.Kind == Type::String || obj.Kind == Type::Binary)
    return obj.Raw;
  return {};
}

// =====================================================================================================================
// Get an item's MsgPack-encoded value:
// - if it has been set(), gives the location in m_newData;
// - if it has not been set() but it is in the original blob, gives the location in the MsgPack blob;
// - otherwise, returns "".
// The returned StringRef has a length to take it up to the end of m_newData or the supplied MsgPack blob; that is
// OK because we know it is a well-formed MsgPack value that the caller can read.
StringRef MsgPackScanner::getValue(const Item &item) const {
  unsigned itemIndex = &item - &m_spec[0];
  assert(itemIndex < m_spec.size());
  const ItemInfo &itemInfo = m_itemInfos[itemIndex];
  if (itemInfo.newOffset != ItemInfo::NoNewOffset)
    return StringRef(m_newData).drop_front(itemInfo.newOffset);
  if (itemInfo.size != 0)
    return m_blob.drop_front(itemInfo.offset);
  return "";
}

// =====================================================================================================================
// Set an item as a bool. This gets a different name to avoid implicit conversions from other types to bool.
void MsgPackScanner::setBool(const Item &item, bool value) {
  // Write the new value into m_newData.
  size_t newOffset = m_newData.size();
  raw_svector_ostream stream(m_newData);
  msgpack::Writer writer(stream);
  writer.write(value);
  // Attach it to the item being set.
  setValue(item, newOffset, m_newData.size() - newOffset);
}

// =====================================================================================================================
// Set an item as an unsigned integer.
void MsgPackScanner::set(const Item &item, uint64_t value) {
  // Write the new value into m_newData.
  size_t newOffset = m_newData.size();
  raw_svector_ostream stream(m_newData);
  msgpack::Writer writer(stream);
  writer.write(value);
  // Attach it to the item being set.
  setValue(item, newOffset, m_newData.size() - newOffset);
}

// =====================================================================================================================
// Set an item as a string
void MsgPackScanner::set(const Item &item, StringRef value) {
  // Write the new value into m_newData.
  size_t newOffset = m_newData.size();
  raw_svector_ostream stream(m_newData);
  msgpack::Writer writer(stream);
  writer.write(value);
  // Attach it to the item being set.
  setValue(item, newOffset, m_newData.size() - newOffset);
}

// =====================================================================================================================
// Set an item to the new value that has just been written in MsgPack format to m_newData.
//
// @param item : Item to attach the new value to
// @param newOffset : Offset of new value in m_newData
// @param newSize : Size of new value in m_newData
//
// @return : The old offset to use when inserting children of the item.
//
size_t MsgPackScanner::setValue(const Item &item, size_t newOffset, size_t newSize) {
#ifndef NDEBUG
  m_inUse = true;
#endif
  unsigned itemIndex = &item - &m_spec[0];
  assert(itemIndex < m_spec.size());
  ItemInfo &itemInfo = m_itemInfos[itemIndex];
  size_t insertOffset = itemInfo.endOffset;
  if (insertOffset == 0)
    insertOffset = itemInfo.offset;
  if (itemInfo.newOffset == ItemInfo::NoNewOffset) {
    if (itemInfo.size == 0) {
      // Item does not yet exist and needs to be created.
      // Check the parent.
      unsigned parentIndex = m_spec.getParentIndex(itemIndex);
      if (parentIndex != UINT_MAX) {
        const Item &parentItem = m_spec[parentIndex];
        size_t parentNewOffset = m_newData.size();
        raw_svector_ostream stream(m_newData);
        msgpack::Writer writer(stream);
        // Determine the new length of the parent map/array: 1 if it did not already exist, otherwise one more
        // than its previous length.
        unsigned length = 1;
        StringRef parentValue = getValue(parentItem);
        if (!parentValue.empty()) {
          Object obj;
          Reader reader(parentValue);
          void(bool(reader.read(obj))); // Check success, and assert on failure
          length = obj.Length + 1;
        }
        // Write the new map/array header.
        if (parentItem.itemType == ItemType::Map)
          writer.writeMapSize(length);
        else
          writer.writeArraySize(length);
        insertOffset = setValue(parentItem, parentNewOffset, m_newData.size() - parentNewOffset);
        // If the parent is a map, we need to write the key.
        if (parentItem.itemType == ItemType::Map) {
          itemInfo.newKeyOffset = m_newData.size();
          writer.write(StringRef(item.name));
          itemInfo.newKeySize = m_newData.size() - itemInfo.newKeyOffset;
        }
        itemInfo.offset = insertOffset;
      }
    } else {
      // First time setting an existing item. Check if the value being set is the same as the old value.
      if (m_blob.drop_front(itemInfo.offset).substr(0, newSize) ==
          StringRef(m_newData).drop_front(newOffset).substr(0, newSize))
        return insertOffset; // No change in value; nothing to do.
    }
  }
  // Attach the new value to the item.
  // m_gen goes up by 2 to allow a possible new key to use gen - 1 in write(), to ensure that the new key
  // gets written before the new value.
  itemInfo.newOffset = newOffset;
  itemInfo.newSize = newSize;
  m_gen += 2;
  itemInfo.gen = m_gen;
  // For the case when this is a map or array being created or having its size updated ready to insert a child,
  // return the old offset to insert at.
  return insertOffset;
}

// =====================================================================================================================
// Write updated MsgPack to the stream.
void MsgPackScanner::write(raw_ostream &stream) {
  // Gather change records for points where data is removed, inserted or changed.
  struct Change {
    size_t oldOffset;
    size_t oldSize;
    size_t newOffset;
    size_t newSize;
    unsigned gen;
  };
  SmallVector<Change> changes;
  for (unsigned itemIndex = 0; itemIndex != m_spec.size(); ++itemIndex) {
    ItemInfo &itemInfo = m_itemInfos[itemIndex];
    Change change{};
    if (itemInfo.newSize == ItemInfo::NoReplacementNewSize) {
      // Deleting old item without replacing it.
      // TODO: There is no code yet to delete an item that would exercise this path. The idea is that the method to
      // delete an item would also take care of modifying the parent map/array header to change its child count.
      change.oldOffset = itemInfo.offset;
      change.oldSize = itemInfo.size;
      if (itemInfo.endOffset != 0) {
        // This item is a map or array; delete all the contents too.
        change.oldSize = itemInfo.endOffset - itemInfo.offset;
      }
      if (itemInfo.keyOffset != 0) {
        // This item is in a map; also delete the key.
        change.oldOffset = itemInfo.keyOffset;
        change.oldSize += itemInfo.offset - itemInfo.keyOffset;
      }
    } else if (itemInfo.newOffset != ItemInfo::NoNewOffset) {
      // Replacing or adding this item.
      if (itemInfo.newKeyOffset != ItemInfo::NoNewOffset) {
        // Also adding new key. Do that first.
        change.oldOffset = itemInfo.offset;
        change.oldSize = 0;
        change.newOffset = itemInfo.newKeyOffset;
        change.newSize = itemInfo.newKeySize;
        change.gen = itemInfo.gen - 1; // Key needs to go before value
        changes.push_back(change);
        LLVM_DEBUG({
          dbgs() << "Change (new key) gen=" << change.gen << " oldOffset=" << change.oldOffset
                 << " oldSize=" << change.oldSize << " new=";
          for (char ch : StringRef(m_newData).drop_front(change.newOffset).take_front(change.newSize))
            dbgs() << format("%2.2x ", (unsigned char)ch);
          dbgs() << "\n";
        });
      }
      change = {};
      change.oldOffset = itemInfo.offset;
      change.oldSize = itemInfo.size;
      change.newOffset = itemInfo.newOffset;
      change.newSize = itemInfo.newSize;
    } else {
      continue;
    }
    change.gen = itemInfo.gen;
    changes.push_back(change);
    LLVM_DEBUG({
      dbgs() << "Change gen=" << change.gen << " oldOffset=" << change.oldOffset << " oldSize=" << change.oldSize
             << " new=";
      for (char ch : StringRef(m_newData).drop_front(change.newOffset).take_front(change.newSize))
        dbgs() << format("%2.2x ", (unsigned char)ch);
      dbgs() << "\n";
    });
  }

  // Sort the change records by oldOffset then gen.
  std::sort(changes.begin(), changes.end(), [](const Change &lhs, const Change &rhs) {
    return std::tie(lhs.oldOffset, lhs.gen) < std::tie(rhs.oldOffset, rhs.gen);
  });

  // Write the new MsgPack blob.
  size_t oldOffset = 0;
  for (const Change &change : changes) {
    // Write old data up to the point of the change record.
    stream << m_blob.take_front(change.oldOffset).drop_front(oldOffset);
    // Skip old data being removed or replaced.
    oldOffset = change.oldOffset + change.oldSize;
    // Write new data.
    stream << StringRef(m_newData).drop_front(change.newOffset).take_front(change.newSize);
  }
  // Write remaining old data.
  stream << m_blob.drop_front(oldOffset);
}
