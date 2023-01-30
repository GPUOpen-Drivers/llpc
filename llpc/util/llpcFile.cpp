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
 * @file  llpcFile.h
 * @brief LLPC source file: contains implementation of utility collection class Llpc::File.
 ***********************************************************************************************************************
 */
#include "llpcFile.h"
#include <cassert>
#include <stdarg.h>
#include <sys/stat.h>

#define DEBUG_TYPE "llpc-file"

namespace Llpc {

// =====================================================================================================================
// Opens a file stream for read, write or append access.
//
// @param filename : Name of file to open
// @param accessFlags : ORed mask of FileAccessMode values describing how the file will be used
Result File::open(const char *filename, unsigned accessFlags) {
  Result result = Result::Success;

  if (m_fileHandle)
    result = Result::ErrorUnavailable;
  else if (!filename)
    result = Result::ErrorInvalidPointer;
  else {
    char fileMode[5] = {};

    switch (accessFlags) {
    case FileAccessRead:
      fileMode[0] = 'r';
      break;
    case FileAccessWrite:
      fileMode[0] = 'w';
      break;
    case FileAccessAppend:
      fileMode[0] = 'a';
      break;
    case (FileAccessRead | FileAccessWrite):
      // NOTE: Both r+ and w+ modes might apply here: r+ requires that the file exists beforehand, while w+ does
      // not. w+ will create the file if it doesn't exist, like w,a,a+. w+, like w, will discard existing
      // contents of the file. If we need to expose r+ mode, adding another flag to indicate 'don't overwrite the
      // file'.
      fileMode[0] = 'w';
      fileMode[1] = '+';
      break;
    case (FileAccessRead | FileAccessAppend):
      fileMode[0] = 'a';
      fileMode[1] = '+';
      break;
    case (FileAccessRead | FileAccessBinary):
      fileMode[0] = 'r';
      fileMode[1] = 'b';
      break;
    case (FileAccessWrite | FileAccessBinary):
      fileMode[0] = 'w';
      fileMode[1] = 'b';
      break;
    case (FileAccessRead | FileAccessWrite | FileAccessBinary):
      fileMode[0] = 'w';
      fileMode[1] = 'b';
      fileMode[2] = '+';
      fileMode[3] = 'R';
      break;
    case (FileAccessRead | FileAccessAppend | FileAccessBinary):
      fileMode[0] = 'a';
      fileMode[1] = 'b';
      fileMode[2] = '+';
      fileMode[3] = 'R';
      break;
    case FileAccessReadUpdate:
      fileMode[0] = 'r';
      fileMode[1] = '+';
      break;
    case (FileAccessReadUpdate | FileAccessBinary):
      fileMode[0] = 'r';
      fileMode[1] = '+';
      fileMode[2] = 'b';
      break;
    default:
      assert(0 && "Should never be called!");
      result = Result::ErrorInvalidValue;
      break;
    }

    if (result == Result::Success) {
      m_fileHandle = fopen(filename, &fileMode[0]);
      if (!m_fileHandle)
        result = Result::ErrorUnknown;
    }
  }

  return result;
}

// =====================================================================================================================
// Closes the file handle if still open.
void File::close() {
  if (m_fileHandle) {
    fclose(m_fileHandle);
    m_fileHandle = nullptr;
  }
}

// =====================================================================================================================
// Writes a stream of bytes to the file.
//
// @param buffer : Buffer to write to the file
// @param bufferSize : Size of the buffer in bytes
Result File::write(const void *buffer, size_t bufferSize) {
  Result result = Result::Success;

  if (!m_fileHandle)
    result = Result::ErrorUnavailable;
  else if (!buffer)
    result = Result::ErrorInvalidPointer;
  else if (bufferSize == 0)
    result = Result::ErrorInvalidValue;
  else {
    if (fwrite(buffer, 1, bufferSize, m_fileHandle) != bufferSize)
      result = Result::ErrorUnknown;
  }

  return result;
}

// =====================================================================================================================
// Reads a stream of bytes from the file.
//
// @param [out] buffer : Buffer to read the file into
// @param bufferSize : Size of buffer in bytes
// @param [out] bytesReadOut : Number of bytes actually read (can be nullptr)
Result File::read(void *buffer, size_t bufferSize, size_t *bytesReadOut) {
  Result result = Result::Success;

  if (!m_fileHandle)
    result = Result::ErrorUnavailable;
  else if (!buffer)
    result = Result::ErrorInvalidPointer;
  else if (bufferSize == 0)
    result = Result::ErrorInvalidValue;
  else {
    const size_t bytesRead = fread(buffer, 1, bufferSize, m_fileHandle);

    if (bytesRead != bufferSize)
      result = Result::ErrorUnknown;

    if (bytesReadOut)
      *bytesReadOut = bytesRead;
  }

  return result;
}

// =====================================================================================================================
// Reads a single line (until the next newline) of bytes from the file.
//
// @param [out] buffer : Buffer to read the file into
// @param bufferSize : Size of buffer in bytes
// @param [out] bytesReadOut : Number of bytes actually read (can be nullptr)
Result File::readLine(void *buffer, size_t bufferSize, size_t *bytesReadOut) {
  Result result = Result::ErrorInvalidValue;

  if (!m_fileHandle)
    result = Result::ErrorUnavailable;
  else if (!buffer)
    result = Result::ErrorInvalidPointer;
  else if (bufferSize == 0)
    result = Result::ErrorInvalidValue;
  else {
    size_t bytesRead = 0;
    char *charBuffer = static_cast<char *>(buffer);

    while (bytesRead < bufferSize) {
      int c = getc(m_fileHandle);
      if (c == '\n') {
        result = Result::Success;
        break;
      }
      if (c == EOF) {
        result = Result::ErrorUnknown;
        break;
      }
      charBuffer[bytesRead] = static_cast<char>(c);
      bytesRead++;
    }

    if (bytesReadOut)
      *bytesReadOut = bytesRead;
  }

  return result;
}

// =====================================================================================================================
// Prints a formatted string to the file.
//
// @param formatStr : Printf-style format string
Result File::printf(const char *formatStr,
                    ... // Printf-style argument list
) const {
  Result result = Result::ErrorUnavailable;

  if (m_fileHandle) {
    va_list argList;
    va_start(argList, formatStr);

#if defined(_WIN32)
    // MS compilers provide vfprintf_s, which is supposedly "safer" than traditional vfprintf.
    if (vfprintf_s(m_fileHandle, formatStr, argList) != -1)
#else
    // Just use the traditional vfprintf.
    if (vfprintf(m_fileHandle, formatStr, argList) >= 0)
#endif
      result = Result::Success;
    else
      result = Result::ErrorUnknown;

    va_end(argList);
  }

  return result;
}

// =====================================================================================================================
// Prints a formatted string to the file.
//
// @param formatStr : Printf-style format string
// @param argList : Pre-started variable argument list
Result File::vPrintf(const char *formatStr, va_list argList) {
  Result result = Result::ErrorUnavailable;

  if (m_fileHandle) {
#if defined(_WIN32)
    // MS compilers provide vfprintf_s, which is supposedly "safer" than traditional vfprintf.
    if (vfprintf_s(m_fileHandle, formatStr, argList) != -1)
#else
    // Just use the traditional vfprintf.
    if (vfprintf(m_fileHandle, formatStr, argList) >= 0)
#endif
      result = Result::Success;
    else
      result = Result::ErrorUnknown;
  }

  return result;
}

// =====================================================================================================================
// Flushes pending I/O to the file.
Result File::flush() const {
  Result result = Result::Success;

  if (!m_fileHandle)
    result = Result::ErrorUnavailable;
  else
    fflush(m_fileHandle);

  return result;
}

// =====================================================================================================================
// Sets the file position to the beginning of the file.
void File::rewind() {
  if (m_fileHandle)
    ::rewind(m_fileHandle);
}

// =====================================================================================================================
// Sets the file position to the beginning of the file.
//
// @param offset : Number of bytes to offset
// @param fromOrigin : If true, the seek will be relative to the file origin; if false, it will be from the current
// position
void File::seek(int offset, bool fromOrigin) {
  if (m_fileHandle) {
    int ret = fseek(m_fileHandle, offset, fromOrigin ? SEEK_SET : SEEK_CUR);

    assert(ret == 0);
    (void(ret)); // unused
  }
}

// =====================================================================================================================
// Returns the size of the file, or 0 if the file is inaccessible.
//
// @param filename : Name of the file to check
// @returns : The size of the file `filename` in bytes, or 0 if it is inaccessible
size_t File::getFileSize(const char *filename) {
#if defined(_WIN32)
  // On MS compilers the function and structure to retrieve/store file status information is named '_stat' (with
  // underbar)...
  struct _stat fileStatus = {};
  const int result = _stat(filename, &fileStatus);
#else
  // ...however, on other compilers, they are named 'stat' (no underbar).
  struct stat fileStatus = {};
  const int result = stat(filename, &fileStatus);
#endif
  // If the function call to retrieve file status information fails (returns 0), then the file does not exist (or is
  // inaccessible in some other manner).
  return result == 0 ? fileStatus.st_size : 0;
}

// =====================================================================================================================
// Returns true if a file with the given name exists.
//
// @param filename : Name of the file to check
bool File::exists(const char *filename) {
#if defined(_WIN32)
  // On MS compilers the function and structure to retrieve/store file status information is named '_stat' (with
  // underbar)...
  struct _stat fileStatus = {};
  const int result = _stat(filename, &fileStatus);
#else
  // ...however, on other compilers, they are named 'stat' (no underbar).
  struct stat fileStatus = {};
  const int result = stat(filename, &fileStatus);
#endif
  // If the function call to retrieve file status information fails (returns -1), then the file does not exist (or is
  // inaccessible in some other manner).
  return result != -1;
}

} // namespace Llpc
