/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021 Advanced Micro Devices, Inc. All Rights Reserved.
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
  **********************************************************************************************************************
  * @file  spv_khr_integer_dot_product.h
  * @brief Export SPV_KHR_integer_dot_product before it is released for internal use
  **********************************************************************************************************************
  */
#ifndef SPV_KHR_integer_dot_product_H_
#define SPV_KHR_integer_dot_product_H_

namespace spv {

    enum Capability;

    static const Capability CapabilityDotProductInputAllKHR = static_cast<Capability>(6016);
    static const Capability CapabilityDotProductInput4x8BitKHR = static_cast<Capability>(6017);
    static const Capability CapabilityDotProductInput4x8BitPackedKHR = static_cast<Capability>(6018);
    static const Capability CapabilityDotProductUnsignedKHR = static_cast<Capability>(6019);
    static const Capability CapabilityDotProductMixedSignednessKHR = static_cast<Capability>(6020);
    static const Capability CapabilityDotProductSignedKHR = static_cast<Capability>(6021);
    static const Capability CapabilityDotProductAccSatUnsignedKHR = static_cast<Capability>(6022);
    static const Capability CapabilityDotProductAccSatMixedSignednessKHR = static_cast<Capability>(6023);
    static const Capability CapabilityDotProductAccSatSignedKHR = static_cast<Capability>(6024);

    enum Op;
    static const Op OpSDotKHR = static_cast<Op>(4450);
    static const Op OpUDotKHR = static_cast<Op>(4451);
    static const Op OpSUDotKHR = static_cast<Op>(4452);
    static const Op OpSDotAccSatKHR = static_cast<Op>(4453);
    static const Op OpUDotAccSatKHR = static_cast<Op>(4454);
    static const Op OpSUDotAccSatKHR = static_cast<Op>(4455);

    enum PackedVectorFormat {
        PackedVectorFormat4x8BitKHR = 0,
        PackedVectorFormatMax       = 0x7fffffff,
    };
}  // end namespace spv

#endif
