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
#include "lgc/ElfNoteEntryInsertionUtil.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/BinaryByteStream.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/BinaryStreamWriter.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace lgc;

// The implementation of ELF rewriting is based on "Linux Programmer's Manual ELF(5)".
// In particular, see "Notes (Nhdr)" for the document of the note section.

namespace {

// It is similar to struct NoteHeader defined in llpc/util/vkgcElfReader.h, but
// it allows us to use Elf_Nhdr_Impl<object::ELF64LE>::Align and it does not
// have the size limitation of note name.
using NoteHeader = object::Elf_Nhdr_Impl<object::ELF64LE>;

// An array of zeros. We use it for the note alignment.
constexpr char ZerosForNoteAlign[NoteHeader::Align] = {'\0'};

// =====================================================================================================================
// Contents of a section to be shifted and its new offset.
struct SectionShiftInfo {
  SmallString<64> section;
  ELF::Elf64_Off newOffset;
};

// =====================================================================================================================
// Generates a StringRef from the given SmallVector.
//
// @param input : The SmallVector to be converted to StringRef.
// @returns     : The generated StringRef.
template <typename T> StringRef stringRefFromSmallVector(const SmallVectorImpl<T> &input) {
  return StringRef(reinterpret_cast<const char *>(input.data()), sizeof(T) * input.size());
}

// =====================================================================================================================
// Add an entry to the note section.
//
// Reference: Linux Programmer's Manual ELF(5) "Notes (Nhdr)".
//
// @param noteEntry             : The note entry to be added to the note section.
// @param [out] noteEntryWriter : The .note section where the note entry will be added to.
void addNoteEntry(const NoteEntry &noteEntry, BinaryStreamWriter &noteEntryWriter) {
  NoteHeader noteHeader = {};
  noteHeader.n_namesz = noteEntry.name.size() + 1;
  noteHeader.n_descsz = noteEntry.desc.size();
  noteHeader.n_type = static_cast<ELF::Elf64_Word>(noteEntry.type);
  cantFail(noteEntryWriter.writeObject(noteHeader));

  // Write the note name terminated by zero and zeros for the alignment.
  cantFail(noteEntryWriter.writeCString(noteEntry.name));
  cantFail(noteEntryWriter.writeFixedString(StringRef(
      ZerosForNoteAlign, offsetToAlignment(noteEntryWriter.getLength(), Align::Constant<NoteHeader::Align>()))));

  // Write the note description and zeros for the alignment.
  cantFail(noteEntryWriter.writeBytes(noteEntry.desc));
  cantFail(noteEntryWriter.writeFixedString(StringRef(
      ZerosForNoteAlign, offsetToAlignment(noteEntryWriter.getLength(), Align::Constant<NoteHeader::Align>()))));
}

// =====================================================================================================================
// Writes note entries to a byte-stream.
//
// @param notes                 : The array of note entries to be added to the note section.
// @param newNoteEntryOffset    : The offset in ELF where the note entries will be added.
// @param [out] noteEntryStream : The byte-stream to be filled with the note entries.
void writeNoteEntriesToByteStream(ArrayRef<NoteEntry> notes, const ELF::Elf64_Off newNoteEntryOffset,
                                  AppendingBinaryByteStream &noteEntryStream) {
  BinaryStreamWriter noteEntryWriter(noteEntryStream);
  cantFail(noteEntryWriter.writeFixedString(
      StringRef(ZerosForNoteAlign, offsetToAlignment(newNoteEntryOffset, Align::Constant<NoteHeader::Align>()))));

  // Write the note entries.
  for (const auto &note : notes)
    addNoteEntry(note, noteEntryWriter);
}

// =====================================================================================================================
// Updates the offsets of sections to their new offsets to be shifted. After updating offsets
// it returns the contents of sections and their new offsets.
//
// @param elf                       : The input ELF.
// @param shiftStartingOffset       : The first offset of ELF contents that will be shifted.
//                                  All sections after this offset will be shifted.
// @param lengthToBeShifted         : The length how much sections after shiftStartingOffset will be shift.
// @param sectionHeaders            : The array of section headers.
// @param [out] sectionAndNewOffset : The array of the contents of sections to be shifted and their new
//                                  offset sorted by the new offset in the increasing order.
void updateSectionOffsetsForShift(const SmallVectorImpl<char> &elf, const ELF::Elf64_Off shiftStartingOffset,
                                  ELF::Elf64_Off lengthToBeShifted, MutableArrayRef<ELF::Elf64_Shdr> sectionHeaders,
                                  SmallVectorImpl<SectionShiftInfo> &sectionAndNewOffset) {
  // If a section is located after shiftStartingOffset, it must be shifted.
  for (auto &sectionHeader : sectionHeaders) {
    if (sectionHeader.sh_offset < shiftStartingOffset)
      continue;
    const auto newOffset = alignTo(sectionHeader.sh_offset + lengthToBeShifted, Align(sectionHeader.sh_addralign));
    sectionAndNewOffset.push_back(
        {SmallString<64>(StringRef(elf.data() + sectionHeader.sh_offset, sectionHeader.sh_size)), newOffset});
    lengthToBeShifted = newOffset - sectionHeader.sh_offset;

    // Update the offset of section pointed by the section header to its new offset.
    sectionHeader.sh_offset = newOffset;
  }

  // Sort sectionAndNewOffset by the new offset of each section in the increasing order.
  sort(sectionAndNewOffset,
       [](const SectionShiftInfo &i0, const SectionShiftInfo &i1) { return i0.newOffset < i1.newOffset; });
}

// =====================================================================================================================
// Inserts the new contents to the given ELF.
//
// @param [in/out] elf        : The ELF to insert the new contents.
// @param insertionOffset     : The offset of the new contents to be inserted.
// @param elfContentStream    : The ELF contents to be inserted.
// @param sectionAndNewOffset : The sections to be shifted and their new offsets.
void insertContentsToELF(SmallVectorImpl<char> &elf, const ELF::Elf64_Off insertionOffset,
                         AppendingBinaryByteStream &elfContentStream,
                         const SmallVectorImpl<SectionShiftInfo> &sectionAndNewOffset) {
  // Strip sections after the insertion offset of the new contents.
  elf.resize(insertionOffset);

  // Write the new contents.
  raw_svector_ostream elfStream(elf);
  ArrayRef<uint8_t> contents;
  cantFail(elfContentStream.readBytes(0, elfContentStream.getLength(), contents));
  elfStream << toStringRef(contents);

  // Write the sections after the insertion offset.
  for (const auto &sectionAndNewOffsetInfo : sectionAndNewOffset) {
    elfStream.write_zeros(sectionAndNewOffsetInfo.newOffset - elfStream.str().size());
    elfStream << sectionAndNewOffsetInfo.section.str();
  }
}

// =====================================================================================================================
// Write the section header table to the given offset.
//
// @param [in/out] elf             : The ELF to write the section header table.
// @param sectionHeaderTableOffset : The offset where it will write the section header table.
// @param sectionHeaderTable       : The section header table to be written to the ELF.
void writeSectionHeaderTable(SmallVectorImpl<char> &elf, const ELF::Elf64_Off sectionHeaderTableOffset,
                             const SmallVectorImpl<ELF::Elf64_Shdr> &sectionHeaderTable) {
  if (sectionHeaderTable.size() == 0)
    return;

  raw_svector_ostream elfStream(elf);
  const unsigned minElfSizeForSectionHeaders =
      sectionHeaderTableOffset + sizeof(ELF::Elf64_Shdr) * sectionHeaderTable.size();
  if (minElfSizeForSectionHeaders > elf.size())
    elfStream.write_zeros(minElfSizeForSectionHeaders - elf.size());
  auto sectionHeaderTableInString = stringRefFromSmallVector(sectionHeaderTable);
  elfStream.pwrite(sectionHeaderTableInString.data(), sectionHeaderTableInString.size(), sectionHeaderTableOffset);
}

} // anonymous namespace

namespace lgc {

// =====================================================================================================================
// Adds the given note entries to the note section with the given section name in the given ELF.
// If the note section with the given name does not exist, it uses any other note section.
//
// @param [in/out] elf    : ELF to be updated with the new note entries.
// @param notes           : An array of note entries to be inserted to the existing note section.
// @param noteSectionName : The name of note section to where note entries will be inserted.
void addNotesToElf(SmallVectorImpl<char> &elf, ArrayRef<NoteEntry> notes, const char *noteSectionName) {
  // Get ELF header that contains information for section header table offset
  // and the number of section headers.
  //
  // Reference: http://www.skyfree.org/linux/references/ELF_Format.pdf
  ELF::Elf64_Ehdr *ehdr = reinterpret_cast<ELF::Elf64_Ehdr *>(elf.data());

  // Get the section headers and the existing note section whose name is noteSectionName.
  MutableArrayRef<ELF::Elf64_Shdr> sectionHeaders(reinterpret_cast<ELF::Elf64_Shdr *>(elf.data() + ehdr->e_shoff),
                                                  ehdr->e_shnum);
  auto existingNoteSection =
      find_if(sectionHeaders, [&elf, ehdr, &sectionHeaders, noteSectionName](const ELF::Elf64_Shdr &sectionHeader) {
        if (sectionHeader.sh_type != ELF::SHT_NOTE)
          return false;
        const char *stringTableForSectionNames = elf.data() + sectionHeaders[ehdr->e_shstrndx].sh_offset;
        return !strcmp(noteSectionName, &stringTableForSectionNames[sectionHeader.sh_name]);
      });
  // If a note section with noteSectionName does not exist, use any other note section.
  if (existingNoteSection == sectionHeaders.end()) {
    existingNoteSection = find_if(
        sectionHeaders, [](const ELF::Elf64_Shdr &sectionHeader) { return sectionHeader.sh_type == ELF::SHT_NOTE; });
  }

  // We assume that the given ELF already contains a note section. Since AMD GPU
  // accepts only ELFs with AMD related metadata, the assumption will be satisfied.
  assert(existingNoteSection != sectionHeaders.end());

  // Prepare the new note entries to be added to the existing note section.
  const ELF::Elf64_Off newNoteEntryOffset = existingNoteSection->sh_offset + existingNoteSection->sh_size;
  AppendingBinaryByteStream noteEntryStream(support::little);
  writeNoteEntriesToByteStream(notes, newNoteEntryOffset, noteEntryStream);

  // Get the last section located just before the section header table.
  auto sectionBeforeSectionHeaderTable =
      std::max_element(sectionHeaders.begin(), sectionHeaders.end(),
                       [ehdr](const ELF::Elf64_Shdr &largest, const ELF::Elf64_Shdr &current) {
                         if (current.sh_offset > ehdr->e_shoff)
                           return false;
                         return current.sh_offset > largest.sh_offset;
                       });

  // Update the offset information of sections after the offset of new note entries.
  // The new offset should be the offset where each section will be shifted to.
  SmallVector<SectionShiftInfo> sectionAndNewOffset;
  updateSectionOffsetsForShift(elf, newNoteEntryOffset, noteEntryStream.getLength(), sectionHeaders,
                               sectionAndNewOffset);

  // Increase the size of the existing note section to include new note entries.
  existingNoteSection->sh_size += noteEntryStream.getLength();

  // Prepare the section header table shift if we have to shift it. Note that inserting note entries requires
  // rewriting sections, which results in overwriting the section header table. Therefore, the contents
  // pointed by the MutableArrayRef sectionHeaders will not be the section header table. In that case, we
  // have to create a backup for the section header table.
  SmallVector<ELF::Elf64_Shdr> sectionHeaderTableBackup;
  if (ehdr->e_shoff > newNoteEntryOffset) {
    ehdr->e_shoff = sectionBeforeSectionHeaderTable->sh_offset + sectionBeforeSectionHeaderTable->sh_size;
    sectionHeaderTableBackup.append(sectionHeaders.begin(), sectionHeaders.end());
  }

  // Insert the stream of note entries to the ELF.
  insertContentsToELF(elf, newNoteEntryOffset, noteEntryStream, sectionAndNewOffset);

  // Write section header table.
  writeSectionHeaderTable(elf, ehdr->e_shoff, sectionHeaderTableBackup);
}

} // namespace lgc
