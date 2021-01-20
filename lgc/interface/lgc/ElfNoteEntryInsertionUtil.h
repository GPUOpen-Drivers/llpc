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
 * @file  ElfNoteEntryInsertionUtil.h
 * @brief LLPC header file: declaration of lgc::addNotesToElf interface
 *
 * @details The function addNotesToElf adds given note entries to the given ELF.
 ***********************************************************************************************************************
 */

#pragma once
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"

namespace lgc {

// Note entry that will be added to the note section.
struct NoteEntry {
  llvm::StringRef name;
  llvm::ArrayRef<uint8_t> desc;
  unsigned type;
};

// =====================================================================================================================
// Adds the given note entries to the given ELF if the given ELF has a note section.
// Otherwise, it does nothing.
//
// ELF layouts before/after adding new "note" entries to the existing note section:
//
// |-------------|             |-------------|
// | ELF header  |             | ELF header  |
// |-------------|             |-------------|
// | Sections    |             | Sections    |
// | ...         |             | ...         |
// |-------------|             |-------------|
// | Note section|     ==>     | Note section|
// | ...         |             |             |
// |-------------|<--(Will be  | + New note  |
// | ...         |    Shifted) |   entries   |
// |-------------| |        |  | ...         |
// | Section     | |        \->|-------------|
// | headers     | V           | ...         |
// | ...         |             |-------------|
//                             | Section     |
//                             | headers     |
//                             | ...         |
//
void addNotesToElf(llvm::SmallVectorImpl<char> &elf, llvm::ArrayRef<NoteEntry> notes, const char *noteSectionName);

} // namespace lgc
