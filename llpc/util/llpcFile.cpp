/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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

namespace Llpc
{

// =====================================================================================================================
// Opens a file stream for read, write or append access.
Result File::Open(
    const char*  filename,     // [in] Name of file to open
    uint32_t     accessFlags)  // ORed mask of FileAccessMode values describing how the file will be used
{
    Result result = Result::Success;

    if (m_fileHandle != nullptr)
    {
        result = Result::ErrorUnavailable;
    }
    else if (filename == nullptr)
    {
        result = Result::ErrorInvalidPointer;
    }
    else
    {
        char fileMode[5] = { };

        switch (accessFlags)
        {
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
            // contents of thefile. If we need to expose r+ mode, adding another flag to indicate 'don't overwrite the
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
        case (FileAccessWrite | FileAccessBinary) :
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

        if (result == Result::Success)
        {
            // Just use the traditional fopen.
            m_fileHandle = fopen(filename, &fileMode[0]);
            if (m_fileHandle == nullptr)
            {
                result = Result::ErrorUnknown;
            }
        }
    }

    return result;
}

// =====================================================================================================================
// Closes the file handle if still open.
void File::Close()
{
    if (m_fileHandle != nullptr)
    {
        fclose(m_fileHandle);
        m_fileHandle = nullptr;
    }
}

// =====================================================================================================================
// Writes a stream of bytes to the file.
Result File::Write(
    const void* pBuffer,     // [in] Buffer to write to the file
    size_t      bufferSize)  // Size of the buffer in bytes
{
    Result result = Result::Success;

    if (m_fileHandle == nullptr)
    {
        result = Result::ErrorUnavailable;
    }
    else if (pBuffer == nullptr)
    {
        result = Result::ErrorInvalidPointer;
    }
    else if (bufferSize == 0)
    {
        result = Result::ErrorInvalidValue;
    }
    else
    {
        if (fwrite(pBuffer, 1, bufferSize, m_fileHandle) != bufferSize)
        {
            result = Result::ErrorUnknown;
        }
    }

    return result;
}

// =====================================================================================================================
// Reads a stream of bytes from the file.
Result File::Read(
    void*   pBuffer,        // [out] Buffer to read the file into
    size_t  bufferSize,     // Size of buffer in bytes
    size_t* pBytesReadOut)  // [out] Number of bytes actually read (can be nullptr)
{
    Result result = Result::Success;

    if (m_fileHandle == nullptr)
    {
        result = Result::ErrorUnavailable;
    }
    else if (pBuffer == nullptr)
    {
        result = Result::ErrorInvalidPointer;
    }
    else if (bufferSize == 0)
    {
        result = Result::ErrorInvalidValue;
    }
    else
    {
        const size_t bytesRead = fread(pBuffer, 1, bufferSize, m_fileHandle);

        if (bytesRead != bufferSize)
        {
            result = Result::ErrorUnknown;
        }

        if (pBytesReadOut != nullptr)
        {
            *pBytesReadOut = bytesRead;
        }
    }

    return result;
}

// =====================================================================================================================
// Reads a single line (until the next newline) of bytes from the file.
Result File::ReadLine(
    void*   pBuffer,        // [out] Buffer to read the file into
    size_t  bufferSize,     // Size of buffer in bytes
    size_t* pBytesReadOut)  // [out] Number of bytes actually read (can be nullptr)
{
    Result result = Result::ErrorInvalidValue;

    if (m_fileHandle == nullptr)
    {
        result = Result::ErrorUnavailable;
    }
    else if (pBuffer == nullptr)
    {
        result = Result::ErrorInvalidPointer;
    }
    else if (bufferSize == 0)
    {
        result = Result::ErrorInvalidValue;
    }
    else
    {
        size_t bytesRead = 0;
        char* pCharBuffer = static_cast<char*>(pBuffer);

        while (bytesRead < bufferSize)
        {
            int32_t c = getc(m_fileHandle);
            if (c == '\n')
            {
                result = Result::Success;
                break;
            }
            else if (c == EOF)
            {
                result = Result::ErrorUnknown;
                break;
            }
            pCharBuffer[bytesRead] = static_cast<char>(c);
            bytesRead++;
        }

        if (pBytesReadOut != nullptr)
        {
            *pBytesReadOut = bytesRead;
        }
    }

    return result;
}

// =====================================================================================================================
// Prints a formatted string to the file.
Result File::Printf(
    const char* formatStr,   // [in] Printf-style format string
    ...                      // Printf-style argument list
    ) const
{
    Result result = Result::ErrorUnavailable;

    if (m_fileHandle != nullptr)
    {
        va_list argList;
        va_start(argList, formatStr);

        // Just use the traditional vfprintf.
        if (vfprintf(m_fileHandle, formatStr, argList) >= 0)
        {
            result = Result::Success;
        }
        else
        {
            result = Result::ErrorUnknown;
        }

        va_end(argList);
    }

    return result;
}

// =====================================================================================================================
// Prints a formatted string to the file.
Result File::VPrintf(
    const char* formatStr,   // [in] Printf-style format string
    va_list     argList)     // Pre-started variable argument list
{
    Result result = Result::ErrorUnavailable;

    if (m_fileHandle != nullptr)
    {
        // Just use the traditional vfprintf.
        if (vfprintf(m_fileHandle, formatStr, argList) >= 0)
        {
            result = Result::Success;
        }
        else
        {
            result = Result::ErrorUnknown;
        }
    }

    return result;
}

// =====================================================================================================================
// Flushes pending I/O to the file.
Result File::Flush() const
{
    Result result = Result::Success;

    if (m_fileHandle == nullptr)
    {
        result = Result::ErrorUnavailable;
    }
    else
    {
        fflush(m_fileHandle);
    }

    return result;
}

// =====================================================================================================================
// Sets the file position to the beginning of the file.
void File::Rewind()
{
    if (m_fileHandle != nullptr)
    {
        ::rewind(m_fileHandle);
    }
}

// =====================================================================================================================
// Sets the file position to the beginning of the file.
void File::Seek(
    int32_t offset,         // Number of bytes to offset
    bool   fromOrigin)      // If true, the seek will be relative to the file origin;
                            // if false, it will be from the current position
{
    if (m_fileHandle != nullptr)
    {
        int32_t ret = fseek(m_fileHandle, offset, fromOrigin ? SEEK_SET : SEEK_CUR);

        assert(ret == 0);
        (void(ret)); // unused
    }
}

// =====================================================================================================================
// Returns true if a file with the given name exists.
size_t File::GetFileSize(
    const char* filename)     // [in] Name of the file to check
{
    // ...however, on other compilers, they are named 'stat' (no underbar).
    struct stat fileStatus = {};
    const int32_t result = stat(filename, &fileStatus);
    // If the function call to retrieve file status information fails (returns 0), then the file does not exist (or is
    // inaccessible in some other manner).
    return (result == 0) ? fileStatus.st_size : 0;
}

// =====================================================================================================================
// Returns true if a file with the given name exists.
bool File::Exists(
    const char* filename)      // [in] Name of the file to check
{
    // ...however, on other compilers, they are named 'stat' (no underbar).
    struct stat fileStatus = {};
    const int32_t result = stat(filename, &fileStatus);
    // If the function call to retrieve file status information fails (returns -1), then the file does not exist (or is
    // inaccessible in some other manner).
    return (result != -1);
}

} // Llpc
