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

#include "lgc/util/MsgPackScanner.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include "gmock/gmock.h"

using namespace lgc;
using namespace llvm;

TEST(MsgPackScanner, TestReadUpdateInt) {
  static const struct { MsgPackScanner::Item top = {MsgPackScanner::ItemType::Scalar}; } spec;
  MsgPackScanner::Spec scannerSpec(&spec);
  MsgPackScanner scanner(scannerSpec);
  StringRef blob = StringRef("\xd0\x2a", 2);
  Error err = scanner.scan(blob);
  ASSERT_FALSE(err);
  ASSERT_EQ(scanner.asInt(spec.top), 0x2a);
  // Update the top item.
  scanner.set(spec.top, 0x12a);
  ASSERT_EQ(scanner.asInt(spec.top), 0x12a);
  // Write the updated MsgPack.
  SmallString<0> output;
  raw_svector_ostream stream(output);
  scanner.write(stream);
  ASSERT_EQ(output, "\xcd\x01\x2a");
}

TEST(MsgPackScanner, TestReadBinary) {
  static const struct { MsgPackScanner::Item top = {MsgPackScanner::ItemType::Scalar}; } spec;
  MsgPackScanner::Spec scannerSpec(&spec);
  MsgPackScanner scanner(scannerSpec);
  StringRef blob = StringRef("\xC4\x4\x1\x2\x3\x4", 6);
  Error err = scanner.scan(blob);
  ASSERT_FALSE(err);
  ASSERT_EQ(scanner.asString(spec.top), "\x1\x2\x3\x4");
}

TEST(MsgPackScanner, TestReadUpdateArray) {
  // clang-format off
  static const struct {
    MsgPackScanner::Item top = {MsgPackScanner::ItemType::Array};
    MsgPackScanner::Item   element0 = {MsgPackScanner::ItemType::Scalar};
    MsgPackScanner::Item   element1 = {MsgPackScanner::ItemType::Scalar};
    MsgPackScanner::Item   element2 = {MsgPackScanner::ItemType::Scalar};
    MsgPackScanner::Item end = {MsgPackScanner::ItemType::EndContainer};
  } spec;
  // clang-format on
  MsgPackScanner::Spec scannerSpec(&spec);
  MsgPackScanner scanner(scannerSpec);
  StringRef blob = StringRef("\x92\x2b\x2c");
  Error err = scanner.scan(blob);
  ASSERT_FALSE(err);
  ASSERT_EQ(scanner.asInt(spec.element0), 0x2b);
  ASSERT_EQ(scanner.asInt(spec.element1), 0x2c);
  ASSERT_FALSE(scanner.isSet(spec.element2));
  // Update element 0.
  scanner.set(spec.element0, 0x12b);
  ASSERT_EQ(scanner.asInt(spec.element0), 0x12b);
  // Update element 2. This was not present before, so it extends the array.
  scanner.set(spec.element2, 0x22b);
  ASSERT_EQ(scanner.asInt(spec.element2), 0x22b);
  // Write the updated MsgPack.
  SmallString<0> output;
  raw_svector_ostream stream(output);
  scanner.write(stream);
  ASSERT_EQ(output, "\x93\xcd\x01\x2b\x2c\xcd\x02\x2b");
}

TEST(MsgPackScanner, TestReadUpdateMap) {
  // clang-format off
  static const struct {
    MsgPackScanner::Item top = {MsgPackScanner::ItemType::Map};
    MsgPackScanner::Item   bar = {MsgPackScanner::ItemType::Scalar, "bar"};
    MsgPackScanner::Item   cad = {MsgPackScanner::ItemType::Scalar, "cad"};
    MsgPackScanner::Item   foo = {MsgPackScanner::ItemType::Scalar, "foo"};
    MsgPackScanner::Item end = {MsgPackScanner::ItemType::EndContainer};
  } spec;
  // clang-format on
  MsgPackScanner::Spec scannerSpec(&spec);
  MsgPackScanner scanner(scannerSpec);
  StringRef blob = StringRef("\x82\xa3"
                             "foo"
                             "\xd0\x2d\xa3"
                             "bar"
                             "\xd0\x2e");
  Error err = scanner.scan(blob);
  ASSERT_FALSE(err);
  ASSERT_EQ(scanner.asInt(spec.bar), 0x2e);
  ASSERT_EQ(scanner.asInt(spec.foo), 0x2d);
  ASSERT_FALSE(scanner.isSet(spec.cad));
  // Update foo.
  scanner.set(spec.foo, 0x12d);
  // Set cad. This was not present before, so it extends the map.
  scanner.set(spec.cad, "wibble");
  // Write the updated MsgPack.
  SmallString<0> output;
  raw_svector_ostream stream(output);
  scanner.write(stream);
  ASSERT_EQ(output, StringRef("\x83\xa3"
                              "foo"
                              "\xcd\x01\x2d\xa3"
                              "bar"
                              "\xd0\x2e\xa3"
                              "cad"
                              "\xa6"
                              "wibble"));
}

TEST(MsgPackScanner, TestNestedCreateMapFromEmpty) {
  // clang-format off
  static const struct {
    MsgPackScanner::Item top = {MsgPackScanner::ItemType::Map};
    MsgPackScanner::Item   bar = {MsgPackScanner::ItemType::Scalar, "bar"};
    MsgPackScanner::Item   map2 = {MsgPackScanner::ItemType::Map, "map2"};
    MsgPackScanner::Item     foo = {MsgPackScanner::ItemType::Scalar, "foo"};
    MsgPackScanner::Item     map3 = {MsgPackScanner::ItemType::Map, "map3"};
    MsgPackScanner::Item       cat = {MsgPackScanner::ItemType::Scalar, "cat"};
    MsgPackScanner::Item     endMap3 = {MsgPackScanner::ItemType::EndContainer};
    MsgPackScanner::Item   endMap2 = {MsgPackScanner::ItemType::EndContainer};
    MsgPackScanner::Item end = {MsgPackScanner::ItemType::EndContainer};
  } spec;
  // clang-format on
  MsgPackScanner::Spec scannerSpec(&spec);
  MsgPackScanner scanner(scannerSpec);
  // Set "cat", resulting in the creation of all three levels of map.
  scanner.set(spec.cat, "mouse");
  ASSERT_EQ(scanner.asString(spec.cat), StringRef("mouse"));
  ASSERT_FALSE(scanner.isSet(spec.bar));
  ASSERT_FALSE(scanner.isSet(spec.foo));
  // Write the updated MsgPack.
  SmallString<0> output;
  raw_svector_ostream stream(output);
  scanner.write(stream);
  // Check it by parsing with msgpack::Document and converting to YAML text.
  msgpack::Document doc;
  doc.readFromBlob(output, /*Multi=*/false);
  SmallString<0> yaml;
  raw_svector_ostream yamlStream(yaml);
  doc.toYAML(yamlStream);
  ASSERT_EQ(yaml, StringRef("---\n"
                            "map2:\n"
                            "  map3:\n"
                            "    cat:             mouse\n"
                            "...\n"));
}

TEST(MsgPackScanner, TestNestedCreateMap) {
  // clang-format off
  static const struct {
    MsgPackScanner::Item top = {MsgPackScanner::ItemType::Map};
    MsgPackScanner::Item   bar = {MsgPackScanner::ItemType::Scalar, "bar"};
    MsgPackScanner::Item   map2 = {MsgPackScanner::ItemType::Map, "map2"};
    MsgPackScanner::Item     foo = {MsgPackScanner::ItemType::Scalar, "foo"};
    MsgPackScanner::Item     map3 = {MsgPackScanner::ItemType::Map, "map3"};
    MsgPackScanner::Item       cat = {MsgPackScanner::ItemType::Scalar, "cat"};
    MsgPackScanner::Item     endMap3 = {MsgPackScanner::ItemType::EndContainer};
    MsgPackScanner::Item   endMap2 = {MsgPackScanner::ItemType::EndContainer};
    MsgPackScanner::Item end = {MsgPackScanner::ItemType::EndContainer};
  } spec;
  // clang-format on
  MsgPackScanner::Spec scannerSpec(&spec);
  // Create initial MsgPack blob using msgpack::Document to parse YAML text.
  const char yaml[] = "---\n"
                      "bar: barrow\n"
                      "...\n";
  msgpack::Document doc;
  doc.fromYAML(yaml);
  std::string blob;
  doc.writeToBlob(blob);
  // Scan blob into MsgPackScanner.
  MsgPackScanner scanner(scannerSpec);
  Error err = scanner.scan(blob);
  ASSERT_FALSE(err);
  ASSERT_EQ(scanner.asString(spec.bar), StringRef("barrow"));
  // Set "cat", resulting in the creation of map2 and map3.
  scanner.set(spec.cat, "mouse");
  ASSERT_EQ(scanner.asString(spec.cat), StringRef("mouse"));
  ASSERT_FALSE(scanner.isSet(spec.foo));
  // Change the value of "bar", changing its size.
  scanner.set(spec.bar, "barycentric");
  // Write the updated MsgPack.
  SmallString<0> output;
  raw_svector_ostream stream(output);
  scanner.write(stream);
  // Check it by parsing with msgpack::Document and converting to YAML text.
  msgpack::Document doc2;
  doc2.readFromBlob(output, /*Multi=*/false);
  SmallString<0> yaml2;
  raw_svector_ostream yamlStream2(yaml2);
  doc2.toYAML(yamlStream2);
  ASSERT_EQ(yaml2, StringRef("---\n"
                             "bar:             barycentric\n"
                             "map2:\n"
                             "  map3:\n"
                             "    cat:             mouse\n"
                             "...\n"));
}

TEST(MsgPackScanner, TestReduceSize) {
  // clang-format off
  static const struct {
    MsgPackScanner::Item top = {MsgPackScanner::ItemType::Map};
    MsgPackScanner::Item   map2 = {MsgPackScanner::ItemType::Map, "map2"};
    MsgPackScanner::Item     map3 = {MsgPackScanner::ItemType::Map, "map3"};
    MsgPackScanner::Item       cat = {MsgPackScanner::ItemType::Scalar, "cat"};
    MsgPackScanner::Item     endMap3 = {MsgPackScanner::ItemType::EndContainer};
    MsgPackScanner::Item   endMap2 = {MsgPackScanner::ItemType::EndContainer};
    MsgPackScanner::Item end = {MsgPackScanner::ItemType::EndContainer};
  } spec;
  // clang-format on
  MsgPackScanner::Spec scannerSpec(&spec);
  // Create initial MsgPack blob using msgpack::Document to parse YAML text.
  const char yaml[] = "---\n"
                      "bar: barycentric\n"
                      "map2:\n"
                      "  map3:\n"
                      "    cat: mouse\n"
                      "  squirrel: nut\n"
                      "...\n";
  msgpack::Document doc;
  doc.fromYAML(yaml);
  std::string blob;
  doc.writeToBlob(blob);
  // Scan blob into MsgPackScanner.
  MsgPackScanner scanner(scannerSpec);
  Error err = scanner.scan(blob);
  ASSERT_FALSE(err);
  ASSERT_EQ(scanner.asString(spec.cat), StringRef("mouse"));
  // Set "cat" to "ox", a shorter string.
  scanner.set(spec.cat, "ox");
  ASSERT_EQ(scanner.asString(spec.cat), StringRef("ox"));
  // Write the updated MsgPack.
  SmallString<0> output;
  raw_svector_ostream stream(output);
  scanner.write(stream);
  // Check it by parsing with msgpack::Document and converting to YAML text.
  msgpack::Document doc2;
  doc2.readFromBlob(output, /*Multi=*/false);
  SmallString<0> yaml2;
  raw_svector_ostream yamlStream2(yaml2);
  doc2.toYAML(yamlStream2);
  ASSERT_EQ(yaml2, StringRef("---\n"
                             "bar:             barycentric\n"
                             "map2:\n"
                             "  map3:\n"
                             "    cat:             ox\n"
                             "  squirrel:        nut\n"
                             "...\n"));
}
