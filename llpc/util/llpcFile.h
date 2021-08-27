/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  llpcFile.h
 * @brief LLPC header file: contains definitions of utility collection class Llpc::File.
 ***********************************************************************************************************************
 */
#pragma once

#include "llpc.h"
#include <climits>
#include <cstdio>

namespace Llpc {

// On Linux, NAME_MAX is the maximum filename length, while PATH_MAX defines the maximum path length.
// On Windows, maximum file and path lengths are defined by _MAX_FNAME and MAX_PATH, respectively.
#if defined(__unix__)
constexpr size_t MaxFilenameLen = NAME_MAX;
constexpr size_t MaxPathLen = PATH_MAX;
#else
constexpr size_t MaxFilenameLen = _MAX_FNAME;
constexpr size_t MaxPathLen = _MAX_PATH;
#endif

// We add 1 to accommodate a null terminator.
// Note that PathBufferLen already considers the full path length, including the file name part, so
// there's no need to add them when creating buffers.
constexpr size_t FilenameBufferLen = MaxFilenameLen + 1;
constexpr size_t PathBufferLen = MaxPathLen + 1;

// Enumerates access modes that may be required on an opened file. Can be bitwise ORed together to specify multiple
// simultaneous modes.
enum FileAccessMode : unsigned {
  FileAccessRead = 0x1,        ///< Read access.
  FileAccessWrite = 0x2,       ///< Write access.
  FileAccessAppend = 0x4,      ///< Append access.
  FileAccessBinary = 0x8,      ///< Binary access.
  FileAccessReadUpdate = 0x10, ///< Read&Update access.
};

// =====================================================================================================================
// Exposes simple file I/O functionality by encapsulating standard C runtime file I/O functions like fopen, fwrite, etc.
class File {
public:
  File() : m_fileHandle(nullptr) {}

  // Closes the file if it is still open.
  ~File() { close(); }

  static size_t getFileSize(const char *filename);
  static bool exists(const char *filename);

  Result open(const char *filename, unsigned accessFlags);
  void close();
  Result write(const void *buffer, size_t bufferSize);
  Result read(void *buffer, size_t bufferSize, size_t *bytesRead);
  Result readLine(void *buffer, size_t bufferSize, size_t *bytesRead);
  Result printf(const char *formatStr, ...) const;
  Result vPrintf(const char *formatStr, va_list argList);
  Result flush() const;
  void rewind();
  void seek(int offset, bool fromOrigin);

  // Returns true if the file is presently open.
  bool isOpen() const { return (m_fileHandle); }
  // Gets handle of the file
  const std::FILE *getHandle() const { return m_fileHandle; }

private:
  std::FILE *m_fileHandle; // File handle
};

} // namespace Llpc
