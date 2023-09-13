/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2023 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  vkgcElfReader.h
 * @brief VKGC header file: contains declaration of VKGC ELF reading utilities.
 ***********************************************************************************************************************
 */
#pragma once

#include "g_palPipelineAbiMetadata.h"
#include "palPipelineAbi.h"
#include "vkgcUtil.h"
#include "llvm/BinaryFormat/MsgPackDocument.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace llvm {
template <unsigned InternalLen> class SmallString;
} // namespace llvm

namespace Vkgc {

// LLVM backend special section name
static const char AmdGpuDisasmName[] = ".AMDGPU.disasm"; // Name of ".AMDGPU.disasm" section
static const char AmdGpuCsdataName[] = ".AMDGPU.csdata"; // Name of ".AMDGPU.csdata" section
static const char AmdGpuConfigName[] = ".AMDGPU.config"; // Name of ".AMDGPU.config" section
static const char ColorExports[] = ".colorExports";      // Name of color export meta data
static const char DiscardState[] = ".discardState";      // Name of discard state

// e_ident size and indices
enum {
  EI_MAG0 = 0,       // File identification index
  EI_MAG1 = 1,       // File identification index
  EI_MAG2 = 2,       // File identification index
  EI_MAG3 = 3,       // File identification index
  EI_CLASS = 4,      // File class
  EI_DATA = 5,       // Data encoding
  EI_VERSION = 6,    // File version
  EI_OSABI = 7,      // OS/ABI identification
  EI_ABIVERSION = 8, // ABI version
  EI_PAD = 9,        // Start of padding bytes
  EI_NIDENT = 16     // Number of bytes in e_ident
};

// Object file classes
enum {
  ELFCLASSNONE = 0, // Invalid object file
  ELFCLASS32 = 1,   // 32-bit object file
  ELFCLASS64 = 2,   // 64-bit object file
};

// Object file byte orderings
enum {
  ELFDATANONE = 0, // Invalid data encoding
  ELFDATA2LSB = 1, // Little-endian object file
  ELFDATA2MSB = 2, // Big-endian object file
};

// Program header table type
enum {
  PT_LOAD = 1, // Loadable segment
};

// Machine architectures
enum {
  EM_AMDGPU = 224, // AMD GPU architecture
};

// Segment flag bits.
enum {
  PF_X = 0x1, // Execute
  PF_W = 0x2, // Write
  PF_R = 0x4, // Read
};

// ELF Symbol Table Binding: st_info.binding
enum {
  STB_LOCAL = 0,  // Not visible outside the object file.
  STB_GLOBAL = 1, // Global symbol, visible to all object files.
  STB_WEAK = 2    // Global scope, but with lower precedence than global symbols.
};

// ELF Symbol Table Type: st_info.type
enum {
  STT_NOTYPE = 0,  // No type specified (e.g., an absolute symbol).
  STT_OBJECT = 1,  // Data object.
  STT_FUNC = 2,    // Function entry point.
  STT_SECTION = 3, // Symbol is associated with a section.
  STT_FILE = 4,    // Source file associated with the object file.
};

// ELF file type
enum {
  ET_DYN = 3, // Shared object file
};

// Enumerates ELF Constants from GNU readelf indicating section type.
enum ElfSectionHeaderTypes : uint32_t {
  SHT_NULL = 0,     // No associated section (inactive entry)
  SHT_PROGBITS = 1, // Program-defined contents
  SHT_SYMTAB = 2,   // Symbol table
  SHT_STRTAB = 3,   // String table
  SHT_RELA = 4,     // Relocation entries; explicit addends
  SHT_HASH = 5,     // Symbol hash table
  SHT_DYNAMIC = 6,  // Information for dynamic linking
  SHT_NOTE = 7,     // Information about the file
};

// Enumerates ELF Section flags.
enum ElfSectionHeaderFlags : uint32_t {
  SHF_WRITE = 0x1,     // Section data should be writable during execution
  SHF_ALLOC = 0x2,     // Section occupies memory during program execution
  SHF_EXECINSTR = 0x4, // Section contains executable machine instructions
  SHF_MERGE = 0x10,    // The data in this section may be merged
  SHF_STRINGS = 0x20,  // The data in this section is null-terminated strings
};

static const uint32_t ElfMagic = 0x464C457F; // "/177ELF" in little-endian

// Section names used in PAL pipeline and LLVM back-end compiler
static const char TextName[] = ".text";         // Name of ".text" section (GPU ISA codes)
static const char DataName[] = ".data";         // Name of ".data" section
static const char RoDataName[] = ".rodata";     // Name of ".rodata" section
static const char ShStrTabName[] = ".shstrtab"; // Name of ".shstrtab" section
static const char StrTabName[] = ".strtab";     // Name of ".strtab" section
static const char SymTabName[] = ".symtab";     // Name of ".symtab" section
static const char NoteName[] = ".note";         // Name of ".note" section
static const char RelocName[] = ".rel.text";    // Name of ".reloc" section
static const char CommentName[] = ".comment";   // Name of ".comment" section

static const uint32_t NT_AMD_AMDGPU_ISA = 11; // Note type of AMDGPU ISA version

// Represents the layout of standard note header
struct NoteHeader {
  uint32_t nameSize; // Byte size of note name
  uint32_t descSize; // Descriptor size in byte
  uint32_t type;     // Note type
  char name[8];      // Note name, include padding
};
static_assert(sizeof(Util::Abi::AmdGpuVendorName) < 8, "");
static_assert(sizeof(Util::Abi::AmdGpuArchName) < 8, "");

#pragma pack(push, 1)
// Represents the layout of 32-bit ELF
struct Elf32 {
  // ELF file header
  struct FormatHeader {
    union {
      uint8_t e_ident[EI_NIDENT];        // ELF identification info
      uint32_t e_ident32[EI_NIDENT / 4]; // Bytes grouped for easy magic number setting
    };

    uint16_t e_type;      // 1 = relocatable, 3 = shared
    uint16_t e_machine;   // Machine architecture constant, 0x3FD = AMD GPU, 0xE0 = LLVM AMD GCN
    uint32_t e_version;   // ELF format version (1)
    uint32_t e_entry;     // Entry point if executable (0)
    uint32_t e_phoff;     // File offset of program header (unused, 0)
    uint32_t e_shoff;     // File offset of section header
    uint32_t e_flags;     // Architecture-specific flags
    uint16_t e_ehsize;    // Size of this ELF header
    uint16_t e_phentsize; // Size of an entry in program header (unused, 0)
    uint16_t e_phnum;
    uint16_t e_shentsize; // Size of an entry in section header
    uint16_t e_shnum;
    uint16_t e_shstrndx; // Section # that contains section name strings
  };

  // ELF section header (used to locate each data section)
  struct SectionHeader {
    uint32_t sh_name;      // Name (index into string table)
    uint32_t sh_type;      // Section type
    uint32_t sh_flags;     // Flag bits (SectionHeaderFlags enum)
    uint32_t sh_addr;      // Base memory address if loadable (0)
    uint32_t sh_offset;    // File position of start of section
    uint32_t sh_size;      // Size of section in bytes
    uint32_t sh_link;      // Section # with related info (unused, 0)
    uint32_t sh_info;      // More section-specific info
    uint32_t sh_addralign; // Alignment granularity in power of 2 (1)
    uint32_t sh_entsize;   // Size of entries if section is array
  };

  // ELF symbol table entry
  struct Symbol {
    uint32_t st_name;  // Symbol name (index into string table)
    uint32_t st_value; // Value or address associated with the symbol
    uint32_t st_size;  // Size of the symbol
    union {
      struct {
        uint8_t type : 4;    // Symbol Table Type
        uint8_t binding : 4; // Symbol Binding attributes
      };
      uint8_t all;
    } st_info;         // This field contains the symbol type and its binding attributes (that is,
                       //  its scope).
    uint8_t st_other;  // Must be zero, reserved
    uint16_t st_shndx; // Which section (header table index) it's defined in
  };

  // ELF relocation entry (without explicit append)
  struct Reloc {
    uint32_t r_offset; // Location (file byte offset, or program virtual address)
    union {
      uint32_t r_info; // Symbol table index and type of relocation to apply
      struct {
        uint32_t r_type : 8;    // Type of relocation
        uint32_t r_symbol : 24; // Index of the symbol in the symbol table
      };
    };
  };

  // ELF program header
  struct Phdr {
    uint32_t p_type;   // Type of segment
    uint32_t p_offset; // File offset where segment is located, in bytes
    uint32_t p_vaddr;  // Virtual address of beginning of segment
    uint32_t p_paddr;  // Physical address of beginning of segment (OS-specific)
    uint32_t p_filesz; // Num. of bytes in file image of segment (may be zero)
    uint32_t p_memsz;  // Num. of bytes in mem image of segment (may be zero)
    uint32_t p_flags;  // Segment flags
    uint32_t p_align;  // Segment alignment constraint
  };
};

// Represents the layout of 64-bit ELF
struct Elf64 {
  // ELF file header
  struct FormatHeader {
    union {
      uint8_t e_ident[EI_NIDENT];        // ELF identification info
      uint32_t e_ident32[EI_NIDENT / 4]; // Bytes grouped for easy magic number setting
    };

    uint16_t e_type;      // 1 = relocatable, 3 = shared
    uint16_t e_machine;   // Machine architecture constant, 0x3FD = AMD GPU, 0xE0 = LLVM AMD GCN
    uint32_t e_version;   // ELF format version (1)
    uint64_t e_entry;     // Entry point if executable (0)
    uint64_t e_phoff;     // File offset of program header (unused, 0)
    uint64_t e_shoff;     // File offset of section header
    uint32_t e_flags;     // Architecture-specific flags
    uint16_t e_ehsize;    // Size of this ELF header
    uint16_t e_phentsize; // Size of an entry in program header (unused, 0)
    uint16_t e_phnum;
    uint16_t e_shentsize; // Size of an entry in section header
    uint16_t e_shnum;
    uint16_t e_shstrndx; // Section # that contains section name strings
  };

  // ELF section header (used to locate each data section)
  struct SectionHeader {
    uint32_t sh_name;      // Name (index into string table)
    uint32_t sh_type;      // Section type
    uint64_t sh_flags;     // Flag bits (SectionHeaderFlags enum)
    uint64_t sh_addr;      // Base memory address if loadable (0)
    uint64_t sh_offset;    // File position of start of section
    uint64_t sh_size;      // Size of section in bytes
    uint32_t sh_link;      // Section # with related info (unused, 0)
    uint32_t sh_info;      // More section-specific info
    uint64_t sh_addralign; // Alignment granularity in power of 2 (1)
    uint64_t sh_entsize;   // Size of entries if section is array
  };

  // ELF symbol table entry
  struct Symbol {
    uint32_t st_name; // Symbol name (index into string table)
    union {
      struct {
        uint8_t type : 4;    // Symbol Table Type
        uint8_t binding : 4; // Symbol Binding attributes
      };
      uint8_t all;
    } st_info;         // This field contains the symbol type and its binding attributes (that is,
                       //  its scope).
    uint8_t st_other;  // Must be zero, reserved
    uint16_t st_shndx; // Which section (header table index) it's defined in
    uint64_t st_value; // Value or address associated with the symbol
    uint64_t st_size;  // Size of the symbol
  };

  // ELF relocation entry
  struct Reloc {
    uint64_t r_offset; // Location (file byte offset, or program virtual address)
    union {
      uint64_t r_info; // Symbol table index and type of relocation to apply
      struct {
        uint32_t r_type;   // Type of relocation
        uint32_t r_symbol; // Index of the symbol in the symbol table
      };
    };
  };

  // ELF program header
  struct Phdr {
    uint32_t p_type;   // Type of segment
    uint32_t p_flags;  // Segment flags
    uint64_t p_offset; // File offset where segment is located, in bytes
    uint64_t p_vaddr;  // Virtual address of beginning of segment
    uint64_t p_paddr;  // Physical addr of beginning of segment (OS-specific)
    uint64_t p_filesz; // Num. of bytes in file image of segment (may be zero)
    uint64_t p_memsz;  // Num. of bytes in mem image of segment (may be zero)
    uint64_t p_align;  // Segment alignment constraint
  };
};
#pragma pack(pop)

// Represents a named buffer to hold constant section data and metadata.
template <class ElfSectionHeader> struct ElfSectionBuffer {
  const uint8_t *data;      // Pointer to binary data buffer
  const char *name;         // Section name
  ElfSectionHeader secHead; // Section metadata
};

// Represents info of ELF symbol
struct ElfSymbol {
  const char *secName;  // Name of the section this symbol's defined in
  uint32_t secIdx;      // Index of the section this symbol's defined in
  const char *pSymName; // Name of this symbol
  uint32_t nameOffset;  // Symbol name offset in .strtab
  uint64_t size;        // Size of this symbol
  uint64_t value;       // Value associated with this symbol
  union {
    struct {
      uint8_t type : 4;    // Symbol Table Type
      uint8_t binding : 4; // Symbol Binding attributes
    };
    uint8_t all;
  } info; // This field contains the symbol type and its binding attributes (that is,
          //  its scope).
};

// Represents info of ELF relocation
struct ElfReloc {
  uint64_t offset;        // Location
  uint32_t symIdx;        // Index of this symbol in the symbol table
  uint32_t type;          // Type of this relocation
  bool useExplicitAddend; // Whether an explicit addend is used
  uint32_t addend;        // The value of the explicit addend
};

// Represents info of ELF note
struct ElfNote {
  NoteHeader hdr;      // Note header
  const uint8_t *data; // The content of the note
};

typedef llvm::SmallString<1024> ElfPackage;

// Represents the status of message packer iterator
enum MsgPackIteratorStatus : uint32_t {
  MsgPackIteratorNone,
  MsgPackIteratorMapKey,
  MsgPackIteratorMapValue,
  MsgPackIteratorArray,
  MsgPackIteratorArrayValue,
  MsgPackIteratorMapBegin,
  MsgPackIteratorMapPair,
  MsgPackIteratorMapEnd,
  MsgPackIteratorArrayEnd,
};

// Represents the struct of message packer iterator
struct MsgPackIterator {
  MsgPackIteratorStatus status;                            // Iterator status
  llvm::msgpack::MapDocNode::MapTy::iterator mapIt;        // Iterator of current map node
  llvm::msgpack::MapDocNode::MapTy::iterator mapEnd;       // End iterator of current map node
  llvm::msgpack::ArrayDocNode::ArrayTy::iterator arrayIt;  // Iterator of current array node
  llvm::msgpack::ArrayDocNode::ArrayTy::iterator arrayEnd; // End Iterator of current array node
  llvm::msgpack::DocNode *node;                            // Current node
};

// =====================================================================================================================
// Represents a reader for loading data from an Executable and Linkable Format (ELF) buffer.
//
// The client should call "ReadFromBuffer()" to initialize the context with the contents of an ELF, then
// "GetSectionData()" to retrieve the contents of a particular named section.
template <class Elf> class ElfReader {
public:
  typedef ElfSectionBuffer<typename Elf::SectionHeader> SectionBuffer;
  ElfReader(GfxIpVersion gfxIp);
  ~ElfReader();

  // Gets architecture-specific flags
  uint32_t getFlags() const { return m_header.e_flags; }

  // Gets graphics IP version info (used by ELF dump only)
  GfxIpVersion getGfxIpVersion() const { return m_gfxIp; }

  // Reads ELF data in from the given buffer into the context.
  // NOTE: Do not change the name or API of this method as it is used by AMD internal code and we need to
  // maintain compatibility.
  Result ReadFromBuffer(const void *buffer, size_t *bufSize);

  // Retrieves the section data for the specified section name, if it exists.
  // NOTE: Do not change the name or API of this method as it is used by AMD internal code and we need to
  // maintain compatibility.
  Result GetSectionData(const char *name, const void **ppData, size_t *dataLength) const;

  uint32_t getSectionCount();
  Result getSectionDataBySectionIndex(uint32_t secIdx, SectionBuffer **ppSectionData) const;
  Result getSectionDataBySortingIndex(uint32_t sortIdx, uint32_t *secIdx, SectionBuffer **ppSectionData) const;
  Result getTextSectionData(SectionBuffer **ppSectionData) const {
    return getSectionDataBySectionIndex(m_textSecIdx, ppSectionData);
  }

  // Determine if a section with the specified name is present in this ELF.
  bool isSectionPresent(const char *name) const { return (m_map.find(name) != m_map.end()); }

  uint32_t getSymbolCount() const;
  void getSymbol(uint32_t idx, ElfSymbol *symbol) const;

  bool isValidSymbol(const char *symbolName);

  ElfNote getNote(uint32_t noteType) const;

  // Gets all associated symbols by section index.
  // NOTE: Do not change the name or API of this method as it is used by AMD internal code and we need to
  // maintain compatibility.
  void GetSymbolsBySectionIndex(uint32_t secIndx, std::vector<ElfSymbol> &secSymbols) const;

  uint32_t getRelocationCount() const;
  void getRelocation(uint32_t idx, ElfReloc *reloc) const;

  // Gets the section index for the specified section name.
  // NOTE: Do not change the name or API of this method as it is used by AMD internal code and we need to
  // maintain compatibility.
  int32_t GetSectionIndex(const char *name) const {
    auto entry = m_map.find(name);
    return (entry != m_map.end()) ? entry->second : InvalidValue;
  }

  void initMsgPackDocument(const void *buffer, uint32_t sizeInBytes);

  bool getNextMsgNode();

  const llvm::msgpack::DocNode *getMsgNode() const;

  MsgPackIteratorStatus getMsgIteratorStatus() const;

  uint32_t getMsgMapLevel() const;

  const typename Elf::FormatHeader &getHeader() const { return m_header; }

  const std::map<std::string, uint32_t> &getMap() const { return m_map; }

  const std::vector<SectionBuffer *> &getSections() const { return m_sections; }

  int32_t getSymSecIdx() const { return m_symSecIdx; }

  int32_t getRelocSecIdx() const { return m_relocSecIdx; }

  int32_t getStrtabSecIdx() const { return m_strtabSecIdx; }

  int32_t getTextSecIdx() const { return m_textSecIdx; }

private:
  ElfReader(const ElfReader &) = delete;
  ElfReader &operator=(const ElfReader &) = delete;

  GfxIpVersion m_gfxIp; // Graphics IP version info (used by ELF dump only)

  typename Elf::FormatHeader m_header;     // ELF header
  std::map<std::string, uint32_t> m_map;   // Map between section name and section index
  std::vector<SectionBuffer *> m_sections; // List of section data and headers

  int32_t m_symSecIdx;    // Index of symbol section
  int32_t m_relocSecIdx;  // Index of relocation section
  int32_t m_strtabSecIdx; // Index of string table section
  int32_t m_textSecIdx;   // Index of string table section

  llvm::msgpack::Document m_document;           // MsgPack document
  std::vector<MsgPackIterator> m_iteratorStack; // MsgPack iterator stack
  uint32_t m_msgPackMapLevel;                   // The map level of current message item
};

// Dumps ELF package to out stream
template <class OStream, class Elf> OStream &operator<<(OStream &out, ElfReader<Elf> &reader);

} // namespace Vkgc
