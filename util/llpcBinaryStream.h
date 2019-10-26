/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
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
  * @file  llpcBinaryStream.h
  * @brief LLPC header file: contains the define of LLPC template utility class BinaryIStream and BinaryOStream.
  ***********************************************************************************************************************
  */

namespace Llpc
{

// =====================================================================================================================
//  Represents the template stream class which reads data in binary format.
template<class Stream>
class BinaryIStream
{
public:
    BinaryIStream(Stream& stream) : m_stream(stream) {}

    // Read obj from Stream m_ss with binary format
    template<class T>
    BinaryIStream& operator >>(T& object)
    {
        m_stream.read(reinterpret_cast<char*>(&object), sizeof(T));
        return *this;
    }

    // Read set object to BinaryIStream
    BinaryIStream& operator >> (
        std::unordered_set<uint64_t>& set)  // [out] set object
    {
        uint32_t setSize = 0;
        *this >> setSize;
        for (uint32_t i = 0; i < setSize; ++i)
        {
            uint64_t item;
            *this >> item;
            set.insert(item);
        }
        return *this;
    }

    // Read map object to BinaryIStream
    BinaryIStream& operator >> (
        std::map<uint32_t, uint32_t>& map)   // [out] map object
    {
        uint32_t mapSize = 0;
        *this >> mapSize;
        for (uint32_t i = 0; i < mapSize; ++i)
        {
            uint32_t first;
            uint32_t second;
            *this >> first;
            *this >> second;
            map[first] = second;
        }
        return *this;
    }

private:
    Stream& m_stream;   // Stream for binary data read/write
};

// =====================================================================================================================
//  Represents the template stream class which writes data in binary format.
template<class Stream>
class BinaryOStream
{
public:
    BinaryOStream(Stream& stream) : m_stream(stream) {}

    // Write obj to Stream m_ss with binary format
    template<class T>
    BinaryOStream& operator <<(const T& object)
    {
        m_stream.write(reinterpret_cast<const char*>(&object), sizeof(T));
        return *this;
    }

    // Write set object to BinaryOStream
    BinaryOStream& operator << (
        const std::unordered_set<uint64_t>& set)   // [in] set object
    {
        uint32_t setSize = set.size();
        *this << setSize;
        for (auto item : set)
        {
            *this << item;
        }
        return *this;
    }

    // Write map object to BinaryOStream
    BinaryOStream& operator << (
        const std::map<uint32_t, uint32_t>& map)  // [in] map object
    {
        uint32_t mapSize = map.size();
        *this << mapSize;
        for (auto item : map)
        {
            *this << item.first;
            *this << item.second;
        }
        return *this;
    }

private:
    Stream& m_stream;   // Stream for binary data read/write
};

// =====================================================================================================================
// Output resource usage to stream out with binary format.
//
// NOTE: This function must keep same order with IStream& operator >> (IStream& in, ResourceUsage& resUsage)
template <class OStream>
OStream& operator << (
    OStream&             out,          // [out] Output stream
    const ResourceUsage& resUsage)     // [in] Resource usage object
{
    BinaryOStream<OStream> binOut(out);

    binOut << resUsage.descPairs;
    binOut << resUsage.pushConstSizeInBytes;
    binOut << resUsage.resourceWrite;
    binOut << resUsage.resourceRead;
    binOut << resUsage.perShaderTable;
    binOut << resUsage.numSgprsAvailable;
    binOut << resUsage.numVgprsAvailable;
    binOut << resUsage.builtInUsage.perStage.u64All;
    binOut << resUsage.builtInUsage.allStage.u64All;

    // Map from shader specified locations to tightly packed locations
    binOut << resUsage.inOutUsage.inputLocMap;
    binOut << resUsage.inOutUsage.outputLocMap;
    binOut << resUsage.inOutUsage.perPatchInputLocMap;
    binOut << resUsage.inOutUsage.perPatchOutputLocMap;
    binOut << resUsage.inOutUsage.builtInInputLocMap;
    binOut << resUsage.inOutUsage.builtInOutputLocMap;
    binOut << resUsage.inOutUsage.perPatchBuiltInInputLocMap;
    binOut << resUsage.inOutUsage.perPatchBuiltInOutputLocMap;

    for (uint32_t i = 0; i < MaxTransformFeedbackBuffers; ++i)
    {
        binOut << resUsage.inOutUsage.xfbStrides[i];
    }

    binOut << resUsage.inOutUsage.enableXfb;
    for (uint32_t i = 0; i < MaxGsStreams; ++i)
    {
        binOut << resUsage.inOutUsage.streamXfbBuffers[i];
    }

    binOut << resUsage.inOutUsage.inputMapLocCount;
    binOut << resUsage.inOutUsage.outputMapLocCount;
    binOut << resUsage.inOutUsage.perPatchInputMapLocCount;
    binOut << resUsage.inOutUsage.perPatchOutputMapLocCount;
    binOut << resUsage.inOutUsage.expCount;

    binOut << resUsage.inOutUsage.gs.rasterStream;
    binOut << resUsage.inOutUsage.gs.xfbOutsInfo;
    for (uint32_t i = 0; i < MaxColorTargets; ++i)
    {
        binOut << static_cast<uint32_t>(resUsage.inOutUsage.fs.outputTypes[i]);
    }
    return out;
}

// =====================================================================================================================
// Read resUsage from stream in with binary format.
//
// NOTE: This function must keep same order with OStream& operator << (OStream& out, const ResourceUsage& resUsage)
template <class IStream>
IStream& operator >> (
    IStream&       in,        // [out] Input stream
    ResourceUsage& resUsage)  // [out] Resource usage object
{
    BinaryIStream<IStream> binIn(in);

    binIn >> resUsage.descPairs;
    binIn >> resUsage.pushConstSizeInBytes;
    binIn >> resUsage.resourceWrite;
    binIn >> resUsage.resourceRead;
    binIn >> resUsage.perShaderTable;
    binIn >> resUsage.numSgprsAvailable;
    binIn >> resUsage.numVgprsAvailable;
    binIn >> resUsage.builtInUsage.perStage.u64All;
    binIn >> resUsage.builtInUsage.allStage.u64All;

    binIn >> resUsage.inOutUsage.inputLocMap;
    binIn >> resUsage.inOutUsage.outputLocMap;
    binIn >> resUsage.inOutUsage.perPatchInputLocMap;
    binIn >> resUsage.inOutUsage.perPatchOutputLocMap;
    binIn >> resUsage.inOutUsage.builtInInputLocMap;
    binIn >> resUsage.inOutUsage.builtInOutputLocMap;
    binIn >> resUsage.inOutUsage.perPatchBuiltInInputLocMap;
    binIn >> resUsage.inOutUsage.perPatchBuiltInOutputLocMap;

    for (uint32_t i = 0; i < MaxTransformFeedbackBuffers; ++i)
    {
        binIn >> resUsage.inOutUsage.xfbStrides[i];
    }

    binIn >> resUsage.inOutUsage.enableXfb;
    for (uint32_t i = 0; i < MaxGsStreams; ++i)
    {
        binIn >> resUsage.inOutUsage.streamXfbBuffers[i];
    }

    binIn >> resUsage.inOutUsage.inputMapLocCount;
    binIn >> resUsage.inOutUsage.outputMapLocCount;
    binIn >> resUsage.inOutUsage.perPatchInputMapLocCount;
    binIn >> resUsage.inOutUsage.perPatchOutputMapLocCount;
    binIn >> resUsage.inOutUsage.expCount;

    binIn >> resUsage.inOutUsage.gs.rasterStream;
    binIn >> resUsage.inOutUsage.gs.xfbOutsInfo;
    for (uint32_t i = 0; i < MaxColorTargets; ++i)
    {
        uint32_t outType;
        binIn >> outType;
        resUsage.inOutUsage.fs.outputTypes[i] = static_cast<BasicType>(outType);
    }
    return in;
}

} // Llpc
