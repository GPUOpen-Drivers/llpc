/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2024 Advanced Micro Devices, Inc. All Rights Reserved.
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
/**
 ***********************************************************************************************************************
 * @file  ImageBuilder.cpp
 * @brief LLPC source file: implementation of Builder methods for image operations
 ***********************************************************************************************************************
 */
#include "YCbCrConverter.h"
#include "lgc/LgcContext.h"
#include "lgc/LgcDialect.h"
#include "lgc/builder/BuilderImpl.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "lgc-builder-impl-image"

using namespace lgc;
using namespace llvm;

// Intrinsic ID table for getlod
static const Intrinsic::ID ImageGetLodIntrinsicTable[] = {
    Intrinsic::amdgcn_image_getlod_1d, Intrinsic::amdgcn_image_getlod_2d,
    Intrinsic::amdgcn_image_getlod_3d, Intrinsic::amdgcn_image_getlod_cube,
    Intrinsic::not_intrinsic,          Intrinsic::not_intrinsic,
    Intrinsic::not_intrinsic,          Intrinsic::not_intrinsic};

// Intrinsic ID table for image load
static const Intrinsic::ID ImageLoadIntrinsicTable[] = {
    Intrinsic::amdgcn_image_load_1d,     Intrinsic::amdgcn_image_load_2d,         Intrinsic::amdgcn_image_load_3d,
    Intrinsic::amdgcn_image_load_cube,   Intrinsic::amdgcn_image_load_1darray,    Intrinsic::amdgcn_image_load_2darray,
    Intrinsic::amdgcn_image_load_2dmsaa, Intrinsic::amdgcn_image_load_2darraymsaa};

// Intrinsic ID table for image load mip
static const Intrinsic::ID ImageLoadMipIntrinsicTable[] = {
    Intrinsic::amdgcn_image_load_mip_1d,      Intrinsic::amdgcn_image_load_mip_2d,
    Intrinsic::amdgcn_image_load_mip_3d,      Intrinsic::amdgcn_image_load_mip_cube,
    Intrinsic::amdgcn_image_load_mip_1darray, Intrinsic::amdgcn_image_load_mip_2darray,
};

// Intrinsic ID table for image store
static const Intrinsic::ID ImageStoreIntrinsicTable[] = {
    Intrinsic::amdgcn_image_store_1d,      Intrinsic::amdgcn_image_store_2d,
    Intrinsic::amdgcn_image_store_3d,      Intrinsic::amdgcn_image_store_cube,
    Intrinsic::amdgcn_image_store_1darray, Intrinsic::amdgcn_image_store_2darray,
    Intrinsic::amdgcn_image_store_2dmsaa,  Intrinsic::amdgcn_image_store_2darraymsaa};

// Intrinsic ID table for image store mip
static const Intrinsic::ID ImageStoreMipIntrinsicTable[] = {
    Intrinsic::amdgcn_image_store_mip_1d,      Intrinsic::amdgcn_image_store_mip_2d,
    Intrinsic::amdgcn_image_store_mip_3d,      Intrinsic::amdgcn_image_store_mip_cube,
    Intrinsic::amdgcn_image_store_mip_1darray, Intrinsic::amdgcn_image_store_mip_2darray,
};

// Table entry in image sample and image gather tables
struct IntrinsicTableEntry {
  unsigned matchMask;
  Intrinsic::ID ids[6];
};

// Intrinsic ID table for image gather.
// There are no entries for _lz variants; a _l variant with lod of constant 0 gets optimized
// later on into _lz.
// There are no entries for _cd variants; the Builder interface does not expose coarse derivatives.
static const IntrinsicTableEntry ImageGather4IntrinsicTable[] = {
    {(1U << Builder::ImageAddressIdxCoordinate),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_cube, Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLodBias),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_b_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_b_cube, Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_b_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLodBias) |
         (1U << Builder::ImageAddressIdxLodClamp),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_b_cl_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_b_cl_cube, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_b_cl_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLodBias) |
         (1U << Builder::ImageAddressIdxLodClamp) | (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_b_cl_o_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_b_cl_o_cube, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_b_cl_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLodBias) |
         (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_b_o_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_b_o_cube, Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_b_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_c_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_cube, Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_c_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxLodBias),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_c_b_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_b_cube, Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_c_b_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxLodBias) | (1U << Builder::ImageAddressIdxLodClamp),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_c_b_cl_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_b_cl_cube, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_b_cl_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxLodBias) | (1U << Builder::ImageAddressIdxLodClamp) |
         (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_c_b_cl_o_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_b_cl_o_cube, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_b_cl_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxLodBias) | (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_c_b_o_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_b_o_cube, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_b_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxLodClamp),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_c_cl_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_cl_cube, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_cl_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxLodClamp) | (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_c_cl_o_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_cl_o_cube, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_cl_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxLod),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_c_l_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_l_cube, Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_c_l_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxLod) | (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_c_l_o_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_l_o_cube, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_l_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_c_o_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_c_o_cube, Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_c_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLodClamp),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_cl_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_cl_cube, Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_cl_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLodClamp) |
         (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_cl_o_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_cl_o_cube, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_cl_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLod),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_l_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_l_cube, Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_l_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLod) |
         (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_l_o_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_l_o_cube, Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_l_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_o_2d, Intrinsic::not_intrinsic,
      Intrinsic::amdgcn_image_gather4_o_cube, Intrinsic::not_intrinsic, Intrinsic::amdgcn_image_gather4_o_2darray}},
    {0}};

// Intrinsic ID table for image sample.
// There are no entries for _lz variants; a _l variant with lod of constant 0 gets optimized
// later on into _lz.
// There are no entries for _cd variants; the Builder interface does not expose coarse derivatives.
static const IntrinsicTableEntry ImageSampleIntrinsicTable[] = {
    {(1U << Builder::ImageAddressIdxCoordinate),
     {
         Intrinsic::amdgcn_image_sample_1d,
         Intrinsic::amdgcn_image_sample_2d,
         Intrinsic::amdgcn_image_sample_3d,
         Intrinsic::amdgcn_image_sample_cube,
         Intrinsic::amdgcn_image_sample_1darray,
         Intrinsic::amdgcn_image_sample_2darray,
     }},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLodBias),
     {Intrinsic::amdgcn_image_sample_b_1d, Intrinsic::amdgcn_image_sample_b_2d, Intrinsic::amdgcn_image_sample_b_3d,
      Intrinsic::amdgcn_image_sample_b_cube, Intrinsic::amdgcn_image_sample_b_1darray,
      Intrinsic::amdgcn_image_sample_b_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLodBias) |
         (1U << Builder::ImageAddressIdxLodClamp),
     {Intrinsic::amdgcn_image_sample_b_cl_1d, Intrinsic::amdgcn_image_sample_b_cl_2d,
      Intrinsic::amdgcn_image_sample_b_cl_3d, Intrinsic::amdgcn_image_sample_b_cl_cube,
      Intrinsic::amdgcn_image_sample_b_cl_1darray, Intrinsic::amdgcn_image_sample_b_cl_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLodBias) |
         (1U << Builder::ImageAddressIdxLodClamp) | (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::amdgcn_image_sample_b_cl_o_1d, Intrinsic::amdgcn_image_sample_b_cl_o_2d,
      Intrinsic::amdgcn_image_sample_b_cl_o_3d, Intrinsic::amdgcn_image_sample_b_cl_o_cube,
      Intrinsic::amdgcn_image_sample_b_cl_o_1darray, Intrinsic::amdgcn_image_sample_b_cl_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLodBias) |
         (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::amdgcn_image_sample_b_o_1d, Intrinsic::amdgcn_image_sample_b_o_2d,
      Intrinsic::amdgcn_image_sample_b_o_3d, Intrinsic::amdgcn_image_sample_b_o_cube,
      Intrinsic::amdgcn_image_sample_b_o_1darray, Intrinsic::amdgcn_image_sample_b_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare),
     {Intrinsic::amdgcn_image_sample_c_1d, Intrinsic::amdgcn_image_sample_c_2d, Intrinsic::amdgcn_image_sample_c_3d,
      Intrinsic::amdgcn_image_sample_c_cube, Intrinsic::amdgcn_image_sample_c_1darray,
      Intrinsic::amdgcn_image_sample_c_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxLodBias),
     {Intrinsic::amdgcn_image_sample_c_b_1d, Intrinsic::amdgcn_image_sample_c_b_2d,
      Intrinsic::amdgcn_image_sample_c_b_3d, Intrinsic::amdgcn_image_sample_c_b_cube,
      Intrinsic::amdgcn_image_sample_c_b_1darray, Intrinsic::amdgcn_image_sample_c_b_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxLodBias) | (1U << Builder::ImageAddressIdxLodClamp),
     {Intrinsic::amdgcn_image_sample_c_b_cl_1d, Intrinsic::amdgcn_image_sample_c_b_cl_2d,
      Intrinsic::amdgcn_image_sample_c_b_cl_3d, Intrinsic::amdgcn_image_sample_c_b_cl_cube,
      Intrinsic::amdgcn_image_sample_c_b_cl_1darray, Intrinsic::amdgcn_image_sample_c_b_cl_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxLodBias) | (1U << Builder::ImageAddressIdxLodClamp) |
         (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::amdgcn_image_sample_c_b_cl_o_1d, Intrinsic::amdgcn_image_sample_c_b_cl_o_2d,
      Intrinsic::amdgcn_image_sample_c_b_cl_o_3d, Intrinsic::amdgcn_image_sample_c_b_cl_o_cube,
      Intrinsic::amdgcn_image_sample_c_b_cl_o_1darray, Intrinsic::amdgcn_image_sample_c_b_cl_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxLodBias) | (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::amdgcn_image_sample_c_b_o_1d, Intrinsic::amdgcn_image_sample_c_b_o_2d,
      Intrinsic::amdgcn_image_sample_c_b_o_3d, Intrinsic::amdgcn_image_sample_c_b_o_cube,
      Intrinsic::amdgcn_image_sample_c_b_o_1darray, Intrinsic::amdgcn_image_sample_c_b_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxLodClamp),
     {Intrinsic::amdgcn_image_sample_c_cl_1d, Intrinsic::amdgcn_image_sample_c_cl_2d,
      Intrinsic::amdgcn_image_sample_c_cl_3d, Intrinsic::amdgcn_image_sample_c_cl_cube,
      Intrinsic::amdgcn_image_sample_c_cl_1darray, Intrinsic::amdgcn_image_sample_c_cl_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxLodClamp) | (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::amdgcn_image_sample_c_cl_o_1d, Intrinsic::amdgcn_image_sample_c_cl_o_2d,
      Intrinsic::amdgcn_image_sample_c_cl_o_3d, Intrinsic::amdgcn_image_sample_c_cl_o_cube,
      Intrinsic::amdgcn_image_sample_c_cl_o_1darray, Intrinsic::amdgcn_image_sample_c_cl_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxDerivativeX) | (1U << Builder::ImageAddressIdxDerivativeY),
     {Intrinsic::amdgcn_image_sample_c_d_1d, Intrinsic::amdgcn_image_sample_c_d_2d,
      Intrinsic::amdgcn_image_sample_c_d_3d, Intrinsic::amdgcn_image_sample_c_d_cube,
      Intrinsic::amdgcn_image_sample_c_d_1darray, Intrinsic::amdgcn_image_sample_c_d_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxDerivativeX) | (1U << Builder::ImageAddressIdxDerivativeY) |
         (1U << Builder::ImageAddressIdxLodClamp),
     {Intrinsic::amdgcn_image_sample_c_d_cl_1d, Intrinsic::amdgcn_image_sample_c_d_cl_2d,
      Intrinsic::amdgcn_image_sample_c_d_cl_3d, Intrinsic::amdgcn_image_sample_c_d_cl_cube,
      Intrinsic::amdgcn_image_sample_c_d_cl_1darray, Intrinsic::amdgcn_image_sample_c_d_cl_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxDerivativeX) | (1U << Builder::ImageAddressIdxDerivativeY) |
         (1U << Builder::ImageAddressIdxLodClamp) | (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::amdgcn_image_sample_c_d_cl_o_1d, Intrinsic::amdgcn_image_sample_c_d_cl_o_2d,
      Intrinsic::amdgcn_image_sample_c_d_cl_o_3d, Intrinsic::amdgcn_image_sample_c_d_cl_o_cube,
      Intrinsic::amdgcn_image_sample_c_d_cl_o_1darray, Intrinsic::amdgcn_image_sample_c_d_cl_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxDerivativeX) |
         (1U << Builder::ImageAddressIdxDerivativeY) | (1U << Builder::ImageAddressIdxZCompare) |
         (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::amdgcn_image_sample_c_d_o_1d, Intrinsic::amdgcn_image_sample_c_d_o_2d,
      Intrinsic::amdgcn_image_sample_c_d_o_3d, Intrinsic::amdgcn_image_sample_c_d_o_cube,
      Intrinsic::amdgcn_image_sample_c_d_o_1darray, Intrinsic::amdgcn_image_sample_c_d_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLod) |
         (1U << Builder::ImageAddressIdxZCompare),
     {Intrinsic::amdgcn_image_sample_c_l_1d, Intrinsic::amdgcn_image_sample_c_l_2d,
      Intrinsic::amdgcn_image_sample_c_l_3d, Intrinsic::amdgcn_image_sample_c_l_cube,
      Intrinsic::amdgcn_image_sample_c_l_1darray, Intrinsic::amdgcn_image_sample_c_l_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxOffset) |
         (1U << Builder::ImageAddressIdxLod) | (1U << Builder::ImageAddressIdxZCompare),
     {Intrinsic::amdgcn_image_sample_c_l_o_1d, Intrinsic::amdgcn_image_sample_c_l_o_2d,
      Intrinsic::amdgcn_image_sample_c_l_o_3d, Intrinsic::amdgcn_image_sample_c_l_o_cube,
      Intrinsic::amdgcn_image_sample_c_l_o_1darray, Intrinsic::amdgcn_image_sample_c_l_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxOffset) |
         (1U << Builder::ImageAddressIdxZCompare),
     {Intrinsic::amdgcn_image_sample_c_o_1d, Intrinsic::amdgcn_image_sample_c_o_2d,
      Intrinsic::amdgcn_image_sample_c_o_3d, Intrinsic::amdgcn_image_sample_c_o_cube,
      Intrinsic::amdgcn_image_sample_c_o_1darray, Intrinsic::amdgcn_image_sample_c_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLodClamp),
     {Intrinsic::amdgcn_image_sample_cl_1d, Intrinsic::amdgcn_image_sample_cl_2d, Intrinsic::amdgcn_image_sample_cl_3d,
      Intrinsic::amdgcn_image_sample_cl_cube, Intrinsic::amdgcn_image_sample_cl_1darray,
      Intrinsic::amdgcn_image_sample_cl_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLodClamp) |
         (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::amdgcn_image_sample_cl_o_1d, Intrinsic::amdgcn_image_sample_cl_o_2d,
      Intrinsic::amdgcn_image_sample_cl_o_3d, Intrinsic::amdgcn_image_sample_cl_o_cube,
      Intrinsic::amdgcn_image_sample_cl_o_1darray, Intrinsic::amdgcn_image_sample_cl_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxDerivativeX) |
         (1U << Builder::ImageAddressIdxDerivativeY),
     {Intrinsic::amdgcn_image_sample_d_1d, Intrinsic::amdgcn_image_sample_d_2d, Intrinsic::amdgcn_image_sample_d_3d,
      Intrinsic::amdgcn_image_sample_d_cube, Intrinsic::amdgcn_image_sample_d_1darray,
      Intrinsic::amdgcn_image_sample_d_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxDerivativeX) |
         (1U << Builder::ImageAddressIdxDerivativeY) | (1U << Builder::ImageAddressIdxLodClamp),
     {Intrinsic::amdgcn_image_sample_d_cl_1d, Intrinsic::amdgcn_image_sample_d_cl_2d,
      Intrinsic::amdgcn_image_sample_d_cl_3d, Intrinsic::amdgcn_image_sample_d_cl_cube,
      Intrinsic::amdgcn_image_sample_d_cl_1darray, Intrinsic::amdgcn_image_sample_d_cl_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxDerivativeX) |
         (1U << Builder::ImageAddressIdxDerivativeY) | (1U << Builder::ImageAddressIdxLodClamp) |
         (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::amdgcn_image_sample_d_cl_o_1d, Intrinsic::amdgcn_image_sample_d_cl_o_2d,
      Intrinsic::amdgcn_image_sample_d_cl_o_3d, Intrinsic::amdgcn_image_sample_d_cl_o_cube,
      Intrinsic::amdgcn_image_sample_d_cl_o_1darray, Intrinsic::amdgcn_image_sample_d_cl_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxDerivativeX) |
         (1U << Builder::ImageAddressIdxDerivativeY) | (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::amdgcn_image_sample_d_o_1d, Intrinsic::amdgcn_image_sample_d_o_2d,
      Intrinsic::amdgcn_image_sample_d_o_3d, Intrinsic::amdgcn_image_sample_d_o_cube,
      Intrinsic::amdgcn_image_sample_d_o_1darray, Intrinsic::amdgcn_image_sample_d_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLod),
     {Intrinsic::amdgcn_image_sample_l_1d, Intrinsic::amdgcn_image_sample_l_2d, Intrinsic::amdgcn_image_sample_l_3d,
      Intrinsic::amdgcn_image_sample_l_cube, Intrinsic::amdgcn_image_sample_l_1darray,
      Intrinsic::amdgcn_image_sample_l_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxLod) |
         (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::amdgcn_image_sample_l_o_1d, Intrinsic::amdgcn_image_sample_l_o_2d,
      Intrinsic::amdgcn_image_sample_l_o_3d, Intrinsic::amdgcn_image_sample_l_o_cube,
      Intrinsic::amdgcn_image_sample_l_o_1darray, Intrinsic::amdgcn_image_sample_l_o_2darray}},
    {(1U << Builder::ImageAddressIdxCoordinate) | (1U << Builder::ImageAddressIdxOffset),
     {Intrinsic::amdgcn_image_sample_o_1d, Intrinsic::amdgcn_image_sample_o_2d, Intrinsic::amdgcn_image_sample_o_3d,
      Intrinsic::amdgcn_image_sample_o_cube, Intrinsic::amdgcn_image_sample_o_1darray,
      Intrinsic::amdgcn_image_sample_o_2darray}},
    {0}};

// Intrinsic ID table for struct buffer atomic
static const Intrinsic::ID StructBufferAtomicIntrinsicTable[] = {
    Intrinsic::amdgcn_struct_buffer_atomic_swap, Intrinsic::amdgcn_struct_buffer_atomic_cmpswap,
    Intrinsic::amdgcn_struct_buffer_atomic_add,  Intrinsic::amdgcn_struct_buffer_atomic_sub,
    Intrinsic::amdgcn_struct_buffer_atomic_smin, Intrinsic::amdgcn_struct_buffer_atomic_umin,
    Intrinsic::amdgcn_struct_buffer_atomic_smax, Intrinsic::amdgcn_struct_buffer_atomic_umax,
    Intrinsic::amdgcn_struct_buffer_atomic_and,  Intrinsic::amdgcn_struct_buffer_atomic_or,
    Intrinsic::amdgcn_struct_buffer_atomic_xor,  Intrinsic::amdgcn_struct_buffer_atomic_fmin,
    Intrinsic::amdgcn_struct_buffer_atomic_fmax, Intrinsic::amdgcn_struct_buffer_atomic_fadd};

// Intrinsic ID table for image atomic
static const Intrinsic::ID ImageAtomicIntrinsicTable[][8] = {
    {Intrinsic::amdgcn_image_atomic_swap_1d, Intrinsic::amdgcn_image_atomic_swap_2d,
     Intrinsic::amdgcn_image_atomic_swap_3d, Intrinsic::amdgcn_image_atomic_swap_cube,
     Intrinsic::amdgcn_image_atomic_swap_1darray, Intrinsic::amdgcn_image_atomic_swap_2darray,
     Intrinsic::amdgcn_image_atomic_swap_2dmsaa, Intrinsic::amdgcn_image_atomic_swap_2darraymsaa},
    {Intrinsic::amdgcn_image_atomic_cmpswap_1d, Intrinsic::amdgcn_image_atomic_cmpswap_2d,
     Intrinsic::amdgcn_image_atomic_cmpswap_3d, Intrinsic::amdgcn_image_atomic_cmpswap_cube,
     Intrinsic::amdgcn_image_atomic_cmpswap_1darray, Intrinsic::amdgcn_image_atomic_cmpswap_2darray,
     Intrinsic::amdgcn_image_atomic_cmpswap_2dmsaa, Intrinsic::amdgcn_image_atomic_cmpswap_2darraymsaa},
    {Intrinsic::amdgcn_image_atomic_add_1d, Intrinsic::amdgcn_image_atomic_add_2d,
     Intrinsic::amdgcn_image_atomic_add_3d, Intrinsic::amdgcn_image_atomic_add_cube,
     Intrinsic::amdgcn_image_atomic_add_1darray, Intrinsic::amdgcn_image_atomic_add_2darray,
     Intrinsic::amdgcn_image_atomic_add_2dmsaa, Intrinsic::amdgcn_image_atomic_add_2darraymsaa},
    {Intrinsic::amdgcn_image_atomic_sub_1d, Intrinsic::amdgcn_image_atomic_sub_2d,
     Intrinsic::amdgcn_image_atomic_sub_3d, Intrinsic::amdgcn_image_atomic_sub_cube,
     Intrinsic::amdgcn_image_atomic_sub_1darray, Intrinsic::amdgcn_image_atomic_sub_2darray,
     Intrinsic::amdgcn_image_atomic_sub_2dmsaa, Intrinsic::amdgcn_image_atomic_sub_2darraymsaa},
    {Intrinsic::amdgcn_image_atomic_smin_1d, Intrinsic::amdgcn_image_atomic_smin_2d,
     Intrinsic::amdgcn_image_atomic_smin_3d, Intrinsic::amdgcn_image_atomic_smin_cube,
     Intrinsic::amdgcn_image_atomic_smin_1darray, Intrinsic::amdgcn_image_atomic_smin_2darray,
     Intrinsic::amdgcn_image_atomic_smin_2dmsaa, Intrinsic::amdgcn_image_atomic_smin_2darraymsaa},
    {Intrinsic::amdgcn_image_atomic_umin_1d, Intrinsic::amdgcn_image_atomic_umin_2d,
     Intrinsic::amdgcn_image_atomic_umin_3d, Intrinsic::amdgcn_image_atomic_umin_cube,
     Intrinsic::amdgcn_image_atomic_umin_1darray, Intrinsic::amdgcn_image_atomic_umin_2darray,
     Intrinsic::amdgcn_image_atomic_umin_2dmsaa, Intrinsic::amdgcn_image_atomic_umin_2darraymsaa},
    {Intrinsic::amdgcn_image_atomic_smax_1d, Intrinsic::amdgcn_image_atomic_smax_2d,
     Intrinsic::amdgcn_image_atomic_smax_3d, Intrinsic::amdgcn_image_atomic_smax_cube,
     Intrinsic::amdgcn_image_atomic_smax_1darray, Intrinsic::amdgcn_image_atomic_smax_2darray,
     Intrinsic::amdgcn_image_atomic_smax_2dmsaa, Intrinsic::amdgcn_image_atomic_smax_2darraymsaa},
    {Intrinsic::amdgcn_image_atomic_umax_1d, Intrinsic::amdgcn_image_atomic_umax_2d,
     Intrinsic::amdgcn_image_atomic_umax_3d, Intrinsic::amdgcn_image_atomic_umax_cube,
     Intrinsic::amdgcn_image_atomic_umax_1darray, Intrinsic::amdgcn_image_atomic_umax_2darray,
     Intrinsic::amdgcn_image_atomic_umax_2dmsaa, Intrinsic::amdgcn_image_atomic_umax_2darraymsaa},
    {Intrinsic::amdgcn_image_atomic_and_1d, Intrinsic::amdgcn_image_atomic_and_2d,
     Intrinsic::amdgcn_image_atomic_and_3d, Intrinsic::amdgcn_image_atomic_and_cube,
     Intrinsic::amdgcn_image_atomic_and_1darray, Intrinsic::amdgcn_image_atomic_and_2darray,
     Intrinsic::amdgcn_image_atomic_and_2dmsaa, Intrinsic::amdgcn_image_atomic_and_2darraymsaa},
    {Intrinsic::amdgcn_image_atomic_or_1d, Intrinsic::amdgcn_image_atomic_or_2d, Intrinsic::amdgcn_image_atomic_or_3d,
     Intrinsic::amdgcn_image_atomic_or_cube, Intrinsic::amdgcn_image_atomic_or_1darray,
     Intrinsic::amdgcn_image_atomic_or_2darray, Intrinsic::amdgcn_image_atomic_or_2dmsaa,
     Intrinsic::amdgcn_image_atomic_or_2darraymsaa},
    {Intrinsic::amdgcn_image_atomic_xor_1d, Intrinsic::amdgcn_image_atomic_xor_2d,
     Intrinsic::amdgcn_image_atomic_xor_3d, Intrinsic::amdgcn_image_atomic_xor_cube,
     Intrinsic::amdgcn_image_atomic_xor_1darray, Intrinsic::amdgcn_image_atomic_xor_2darray,
     Intrinsic::amdgcn_image_atomic_xor_2dmsaa, Intrinsic::amdgcn_image_atomic_xor_2darraymsaa},
    {Intrinsic::amdgcn_image_atomic_fmin_1d, Intrinsic::amdgcn_image_atomic_fmin_2d,
     Intrinsic::amdgcn_image_atomic_fmin_3d, Intrinsic::amdgcn_image_atomic_fmin_cube,
     Intrinsic::amdgcn_image_atomic_fmin_1darray, Intrinsic::amdgcn_image_atomic_fmin_2darray,
     Intrinsic::amdgcn_image_atomic_fmin_2dmsaa, Intrinsic::amdgcn_image_atomic_fmin_2darraymsaa},
    {Intrinsic::amdgcn_image_atomic_fmax_1d, Intrinsic::amdgcn_image_atomic_fmax_2d,
     Intrinsic::amdgcn_image_atomic_fmax_3d, Intrinsic::amdgcn_image_atomic_fmax_cube,
     Intrinsic::amdgcn_image_atomic_fmax_1darray, Intrinsic::amdgcn_image_atomic_fmax_2darray,
     Intrinsic::amdgcn_image_atomic_fmax_2dmsaa, Intrinsic::amdgcn_image_atomic_fmax_2darraymsaa}};

// =====================================================================================================================
// Convert an integer or vector of integer type to the equivalent (vector of) half/float/double
//
// @param origTy : Original type
static Type *convertToFloatingPointType(Type *origTy) {
  assert(origTy->isIntOrIntVectorTy());
  Type *newTy = origTy;
  switch (newTy->getScalarType()->getIntegerBitWidth()) {
  case 16:
    newTy = Type::getHalfTy(newTy->getContext());
    break;
  case 32:
    newTy = Type::getFloatTy(newTy->getContext());
    break;
  default:
    llvm_unreachable("Should never be called!");
  }
  if (isa<VectorType>(origTy))
    newTy = FixedVectorType::get(newTy, cast<FixedVectorType>(origTy)->getNumElements());
  return newTy;
}

// =====================================================================================================================
// Create an image load.
//
// @param resultTy : Result type
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor
// @param coord : Coordinates: scalar or vector i32
// @param mipLevel : Mipmap level if doing load_mip, otherwise nullptr
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageLoad(Type *resultTy, unsigned dim, unsigned flags, Value *imageDesc, Value *coord,
                                    Value *mipLevel, const Twine &instName) {
  if (isa<PoisonValue>(imageDesc))
    return PoisonValue::get(resultTy);

  Type *origTexelTy = resultTy;
  if (auto structResultTy = dyn_cast<StructType>(resultTy))
    origTexelTy = structResultTy->getElementType(0);

  Type *texelTy = origTexelTy;
  if (origTexelTy->isIntOrIntVectorTy(64)) {
    // Only load the first component for 64-bit texel, casted to <2 x i32>
    texelTy = FixedVectorType::get(getInt32Ty(), 2);
  } else if (origTexelTy->isIntOrIntVectorTy(16)) {
    // Treat i16 load as f16 load and cast it back later.
    texelTy = FixedVectorType::get(getHalfTy(), 4);
  }

  bool isTexelBuffer = (dim == Dim1DBuffer || dim == Dim1DArrayBuffer);
  bool needFullDesc = texelTy != origTexelTy && origTexelTy->isIntOrIntVectorTy(64) && origTexelTy->isVectorTy() &&
                      m_pipelineState->getOptions().allowNullDescriptor;
  imageDesc = transformImageDesc(imageDesc, needFullDesc, isTexelBuffer, resultTy);
  const bool isVecTyDesc = imageDesc->getType()->isVectorTy();
  if (isVecTyDesc)
    imageDesc = fixImageDescForRead(imageDesc);
  // Mark usage of images, to allow the compute workgroup reconfiguration optimization.
  getPipelineState()->getShaderResourceUsage(m_shaderStage.value())->useImages = true;
  getPipelineState()->getShaderResourceUsage(m_shaderStage.value())->resourceRead = true;
  assert(coord->getType()->getScalarType()->isIntegerTy(32));
  coord = handleFragCoordViewIndex(coord, flags, dim);

  unsigned dmask = 1;

  if (auto vectorResultTy = dyn_cast<FixedVectorType>(texelTy))
    dmask = (1U << vectorResultTy->getNumElements()) - 1;

  // Prepare the coordinate, which might also change the dimension.
  SmallVector<Value *, 4> coords;
  SmallVector<Value *, 6> derivatives;
  dim = prepareCoordinate(dim, coord, nullptr, nullptr, nullptr, coords, derivatives);

  Type *intrinsicDataTy = nullptr;
  if (isa<StructType>(resultTy)) {
    // TFE
    intrinsicDataTy = StructType::get(texelTy->getContext(), {texelTy, getInt32Ty()});
  } else
    intrinsicDataTy = texelTy;

  SmallVector<Value *, 16> args;
  Instruction *imageInst = nullptr;
  unsigned imageDescArgIndex = 0;
  if (!isTexelBuffer) {
    // Not texel buffer; use image load instruction.
    // Build the intrinsic arguments.
    bool tfe = isa<StructType>(intrinsicDataTy);
    args.push_back(getInt32(dmask));
    args.insert(args.end(), coords.begin(), coords.end());

    if (mipLevel)
      args.push_back(mipLevel);
    imageDescArgIndex = args.size();
    args.push_back(imageDesc);
    args.push_back(getInt32(tfe));

    // glc/dlc bits
    CoherentFlag coherent = {};
    if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
      if (flags & (ImageFlagCoherent | ImageFlagVolatile)) {
        coherent.bits.glc = true;
        coherent.bits.dlc = true;
      }
    }

    args.push_back(getInt32(coherent.u32All));

    // Get the intrinsic ID from the load intrinsic ID table and call it.
    auto table = mipLevel ? &ImageLoadMipIntrinsicTable[0] : &ImageLoadIntrinsicTable[0];

    // Rectangle image uses the same Intrinsic ID with 2D image.
    Intrinsic::ID intrinsicId = (dim == DimRect) ? table[Dim2D] : table[dim];
    imageInst = CreateIntrinsic(intrinsicId, {intrinsicDataTy, coords[0]->getType()}, args, nullptr, instName);
  } else {
    // Texel buffer descriptor. Use the buffer instruction.
    imageDescArgIndex = args.size();
    args.push_back(imageDesc);
    args.push_back(coords[0]);
    args.push_back(getInt32(0));
    args.push_back(getInt32(0));
    args.push_back(getInt32(0));
    imageInst = CreateIntrinsic(Intrinsic::amdgcn_struct_buffer_load_format, intrinsicDataTy, args, nullptr, instName);
  }

  // Mark it as an invariant load if possible.
  if (flags & ImageFlagInvariant)
    imageInst->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(getContext(), {}));

  // Add a waterfall loop if needed.
  Value *result = imageInst;
  if (imageDesc->getType()->isVectorTy()) {
    if (flags & ImageFlagNonUniformImage)
      result = createWaterfallLoop(imageInst, imageDescArgIndex,
                                   getPipelineState()->getShaderOptions(m_shaderStage.value()).scalarizeWaterfallLoads);
    else if (flags & ImageFlagEnforceReadFirstLaneImage)
      enforceReadFirstLane(imageInst, imageDescArgIndex);
  }

  if (texelTy != origTexelTy) {
    Value *texel = result;
    bool tfe = isa<StructType>(resultTy);
    if (tfe)
      texel = CreateExtractValue(result, 0);

    // For 64-bit texel, only the first component is loaded, other components are filled in with (0, 0, 1). This
    // operation could be viewed as supplement of the intrinsic call.
    if (origTexelTy->isIntOrIntVectorTy(64)) {
      texel = CreateBitCast(texel, getInt64Ty()); // Casted to i64

      if (origTexelTy->isVectorTy()) {
        texel = CreateInsertElement(PoisonValue::get(origTexelTy), texel, uint64_t(0));

        SmallVector<Value *, 3> defaults = {getInt64(0), getInt64(0), getInt64(1)};
        // The default of W channel is set to 0 if allowNullDescriptor is on and image descriptor is a null descriptor
        if (m_pipelineState->getOptions().allowNullDescriptor) {
          // Check dword3 against 0 for a null descriptor
          Value *descWord3 = CreateExtractElement(imageDesc, 3);
          if (m_pipelineState->getOptions().maskOffNullDescriptorTypeField) {
            GfxIpVersion gfxIp = getPipelineState()->getTargetInfo().getGfxIpVersion();
            SqImgRsrcRegHandler proxySqRsrcRegHelper(this, imageDesc, &gfxIp);
            unsigned typeMask = proxySqRsrcRegHelper.getRegMask(SqRsrcRegs::Type);
            // Mask off the type bits for the null descriptor
            descWord3 = CreateAnd(descWord3, getInt32(~typeMask));
          }
          Value *isNullDesc = CreateICmpEQ(descWord3, getInt32(0));
          defaults[2] = CreateSelect(isNullDesc, getInt64(0), getInt64(1));
        }
        for (unsigned i = 1; i < cast<FixedVectorType>(origTexelTy)->getNumElements(); ++i)
          texel = CreateInsertElement(texel, defaults[i - 1], i);
      }
    } else if (origTexelTy->isIntOrIntVectorTy(16)) {
      texel = CreateBitCast(texel, origTexelTy);
    }

    if (tfe) {
      intrinsicDataTy = StructType::get(origTexelTy->getContext(), {origTexelTy, getInt32Ty()});
      result = CreateInsertValue(CreateInsertValue(PoisonValue::get(intrinsicDataTy), texel, 0),
                                 CreateExtractValue(result, 1), 1);
    } else {
      result = texel;
    }
  }

  return result;
}

// =====================================================================================================================
// Create an image load with fmask. Dim must be 2DMsaa or 2DArrayMsaa. If the F-mask descriptor has a valid
// format field, then it reads "fmask_texel_R", the R component of the texel read from the given coordinates
// in the F-mask image, and calculates the sample number to use as the sample'th nibble (where sample=0 means
// the least significant nibble) of fmask_texel_R. If the F-mask descriptor has an invalid format, then it
// just uses the supplied sample number. The calculated sample is then appended to the supplied coordinates
// for a normal image load.
//
// @param resultTy : Result type
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor
// @param fmaskDesc : Fmask descriptor
// @param coord : Coordinates: scalar or vector i32, exactly right width for given dimension excluding sample
// @param sampleNum : Sample number, i32
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageLoadWithFmask(Type *resultTy, unsigned dim, unsigned flags, Value *imageDesc,
                                             Value *fmaskDesc, Value *coord, Value *sampleNum, const Twine &instName) {
  if (isa<PoisonValue>(imageDesc))
    return PoisonValue::get(resultTy);
  // Load texel from F-mask image.
  unsigned fmaskDim = dim;
  switch (dim) {
  case Dim2DMsaa:
    fmaskDim = Dim2D;
    break;
  case Dim2DArrayMsaa:
    fmaskDim = Dim3D;
    break;
  default:
    llvm_unreachable("Should never be called!");
    break;
  }

  // When the shadow table is disabled, we don't need to load F-mask descriptor
  if (m_pipelineState->getOptions().enableFmask && !isa<PoisonValue>(fmaskDesc)) {
    Value *fmaskTexel = CreateImageLoad(FixedVectorType::get(getInt32Ty(), 4), fmaskDim, flags, fmaskDesc, coord,
                                        nullptr, instName + ".fmaskload");

    // Calculate the sample number we would use if the F-mask descriptor format is valid.
    Value *calcSampleNum = CreateExtractElement(fmaskTexel, uint64_t(0));
    Value *shiftSampleNum = CreateShl(sampleNum, getInt32(2));
    calcSampleNum = CreateLShr(calcSampleNum, shiftSampleNum);
    calcSampleNum = CreateAnd(calcSampleNum, getInt32(15));

    // Check whether the F-mask descriptor has a BUF_DATA_FORMAT_INVALID (0) format (dword[1].bit[20-25]).
    if (!fmaskDesc->getType()->isVectorTy()) {
      auto callInst = cast<CallInst>(fmaskTexel);
      unsigned argIdx = callInst->arg_size() == 5 ? 0 : callInst->arg_size() - 3;
      fmaskDesc = callInst->getArgOperand(argIdx);
    }
    Value *fmaskFormat = CreateExtractElement(fmaskDesc, 1);
    fmaskFormat = CreateAnd(fmaskFormat, getInt32(63 << 20));
    Value *fmaskValidFormat = CreateICmpNE(fmaskFormat, getInt32(0));

    // Use that to select the calculated sample number or the provided one, then append that to the coordinates.
    sampleNum = CreateSelect(fmaskValidFormat, calcSampleNum, sampleNum);
  }
  sampleNum = CreateInsertElement(PoisonValue::get(coord->getType()), sampleNum, uint64_t(0));
  static const int Idxs[] = {0, 1, 2, 3};
  coord = CreateShuffleVector(coord, sampleNum, ArrayRef<int>(Idxs).slice(0, dim == Dim2DArrayMsaa ? 4 : 3));

  // Now do the normal load.
  return dyn_cast<Instruction>(CreateImageLoad(resultTy, dim, flags, imageDesc, coord, nullptr, instName));
}

// =====================================================================================================================
// Create an image store.
//
// @param texel : Texel value to store
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor
// @param coord : Coordinates: scalar or vector i32
// @param mipLevel : Mipmap level if doing load_mip, otherwise nullptr
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageStore(Value *texel, unsigned dim, unsigned flags, Value *imageDesc, Value *coord,
                                     Value *mipLevel, const Twine &instName) {
  if (isa<PoisonValue>(imageDesc))
    return PoisonValue::get(texel->getType());
  // Mark usage of images, to allow the compute workgroup reconfiguration optimization.
  getPipelineState()->getShaderResourceUsage(m_shaderStage.value())->resourceWrite = true;
  assert(coord->getType()->getScalarType()->isIntegerTy(32));
  coord = handleFragCoordViewIndex(coord, flags, dim);

  // For 64-bit texel, only the first component is stored
  if (texel->getType()->isIntOrIntVectorTy(64)) {
    if (texel->getType()->isVectorTy())
      texel = CreateExtractElement(texel, uint64_t(0));
    texel = CreateBitCast(texel, FixedVectorType::get(getFloatTy(), 2)); // Casted to <2 x float>
  }

  // The intrinsics insist on an FP data type, so we need to bitcast from an integer data type.
  Type *intrinsicDataTy = texel->getType();
  if (intrinsicDataTy->isIntOrIntVectorTy()) {
    intrinsicDataTy = convertToFloatingPointType(intrinsicDataTy);
    texel = CreateBitCast(texel, intrinsicDataTy);
  }

  // Prepare the coordinate, which might also change the dimension.
  SmallVector<Value *, 4> coords;
  SmallVector<Value *, 6> derivatives;
  dim = prepareCoordinate(dim, coord, nullptr, nullptr, nullptr, coords, derivatives);

  bool isTexelBuffer = (dim == Dim1DBuffer || dim == Dim1DArrayBuffer);
  imageDesc = transformImageDesc(imageDesc, false, isTexelBuffer, texel->getType());

  Type *texelTy = texel->getType();
  SmallVector<Value *, 16> args;
  Instruction *imageStore = nullptr;
  unsigned imageDescArgIndex = 0;
  if (!isTexelBuffer) {
    // Not texel buffer; use image store instruction.
    // Build the intrinsic arguments.
    unsigned dmask = 1;
    if (auto vectorTexelTy = dyn_cast<FixedVectorType>(texelTy))
      dmask = (1U << vectorTexelTy->getNumElements()) - 1;

    // Build the intrinsic arguments.
    args.push_back(texel);
    args.push_back(getInt32(dmask));
    args.insert(args.end(), coords.begin(), coords.end());
    if (mipLevel)
      args.push_back(mipLevel);
    imageDescArgIndex = args.size();
    args.push_back(imageDesc);
    args.push_back(getInt32(0)); // tfe/lwe

    // glc bit
    CoherentFlag coherent = {};
    if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
      if (flags & (ImageFlagCoherent | ImageFlagVolatile)) {
        coherent.bits.glc = true;
      }
    }

    args.push_back(getInt32(coherent.u32All));

    // Get the intrinsic ID from the store intrinsic ID table and call it.
    auto table = mipLevel ? &ImageStoreMipIntrinsicTable[0] : &ImageStoreIntrinsicTable[0];

    // Rectangle image uses the same Intrinsic ID with 2D image.
    Intrinsic::ID intrinsicId = (dim == DimRect) ? table[Dim2D] : table[dim];
    imageStore = CreateIntrinsic(intrinsicId, {texelTy, coords[0]->getType()}, args, nullptr, instName);
  } else {
    // Texel buffer descriptor. Use the buffer instruction.
    // First widen texel to vec4 if necessary.
    if (auto vectorTexelTy = dyn_cast<FixedVectorType>(texelTy)) {
      if (vectorTexelTy->getNumElements() != 4) {
        texel = CreateShuffleVector(texel, Constant::getNullValue(texelTy), ArrayRef<int>{0, 1, 2, 3});
      }
    } else
      texel = CreateInsertElement(Constant::getNullValue(FixedVectorType::get(texelTy, 4)), texel, uint64_t(0));

    // Do the buffer store.
    args.push_back(texel);
    imageDescArgIndex = args.size();
    args.push_back(imageDesc);
    args.push_back(coords[0]);
    args.push_back(getInt32(0));
    args.push_back(getInt32(0));
    args.push_back(getInt32(0));
    imageStore =
        CreateIntrinsic(Intrinsic::amdgcn_struct_buffer_store_format, texel->getType(), args, nullptr, instName);
  }

  if (imageDesc->getType()->isVectorTy()) {
    // Add a waterfall loop if needed.
    if (flags & ImageFlagNonUniformImage)
      createWaterfallLoop(imageStore, imageDescArgIndex,
                          getPipelineState()->getShaderOptions(m_shaderStage.value()).scalarizeWaterfallLoads);
    else if (flags & ImageFlagEnforceReadFirstLaneImage)
      enforceReadFirstLane(imageStore, imageDescArgIndex);
  }

  return imageStore;
}

// =====================================================================================================================
// Create an image sample.
// The caller supplies all arguments to the image sample op in "address", in the order specified
// by the indices defined as ImageIndex* below.
//
// @param resultTy : Result type
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor
// @param samplerDesc : Sampler descriptor
// @param address : Address and other arguments
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageSample(Type *resultTy, unsigned dim, unsigned flags, Value *imageDesc,
                                      Value *samplerDesc, ArrayRef<Value *> address, const Twine &instName) {
  Value *coord = address[ImageAddressIdxCoordinate];
  assert(coord->getType()->getScalarType()->isFloatTy() || coord->getType()->getScalarType()->isHalfTy());
  return CreateImageSampleGather(resultTy, dim, flags, coord, imageDesc, samplerDesc, address, instName, true);
}

// =====================================================================================================================
// Create an image sample with a converting sampler.
// The caller supplies all arguments to the image sample op in "address", in the order specified
// by the indices defined as ImageIndex* below.
//
// @param resultTy : Result type
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDescArray : Image descriptor, or array of up to three descriptors for multi-plane
// @param convertingSamplerDesc : Converting sampler descriptor (v10i32)
// @param address : Address and other arguments
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageSampleConvert(Type *resultTy, unsigned dim, unsigned flags, Value *imageDescArray,
                                             Value *convertingSamplerDesc, ArrayRef<Value *> address,
                                             const Twine &instName) {
  return CreateImageSampleConvertYCbCr(resultTy, dim, flags, imageDescArray, convertingSamplerDesc, address, instName);
}

// =====================================================================================================================
// Create an image sample with a converting sampler.
// The caller supplies all arguments to the image sample op in "address", in the order specified
// by the indices defined as ImageAddressIdx* below.
//
// @param resultTy : Result type
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDescArray : Image descriptor, or array of up to three descriptors for multi-plane
// @param convertingSamplerDesc : Converting sampler descriptor (v10i32)
// @param address : Address and other arguments
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageSampleConvertYCbCr(Type *resultTy, unsigned dim, unsigned flags, Value *imageDescArray,
                                                  Value *convertingSamplerDesc, ArrayRef<Value *> address,
                                                  const Twine &instName) {
  // Mark usage of images, to allow the compute workgroup reconfiguration optimization.
  getPipelineState()->getShaderResourceUsage(m_shaderStage.value())->useImages = true;
  Value *result = nullptr;

  // Helper function to extract YCbCr meta data from ycbcrSamplerDesc
  auto getYCbCrMetaElement = [this, &convertingSamplerDesc](unsigned idx) -> unsigned {
    return cast<ConstantInt>(CreateExtractElement(convertingSamplerDesc, idx))->getZExtValue();
  };

  // Extract YCbCr meta data, which is the last 4 dwords of convertingSamplerDesc
  SamplerYCbCrConversionMetaData yCbCrMetaData;
  yCbCrMetaData.word0.u32All = getYCbCrMetaElement(4);
  yCbCrMetaData.word1.u32All = getYCbCrMetaElement(5);
  yCbCrMetaData.word2.u32All = getYCbCrMetaElement(6);
  yCbCrMetaData.word3.u32All = getYCbCrMetaElement(7);
  yCbCrMetaData.word4.u32All = getYCbCrMetaElement(8);
  yCbCrMetaData.word5.u32All = getYCbCrMetaElement(9);

  // Only the first 4 dwords are sampler descriptor, we need to extract these values under any condition
  // Init sample descriptor for luma channel
  Value *samplerDescLuma = CreateShuffleVector(convertingSamplerDesc, convertingSamplerDesc, ArrayRef<int>{0, 1, 2, 3});

  // If we have an array of image descriptors, extract the first one.
  Value *imageDesc = imageDescArray;
  if (isa<ArrayType>(imageDescArray->getType()))
    imageDesc = CreateExtractValue(imageDescArray, 0);
  if (isa<PoisonValue>(imageDesc))
    imageDesc = PoisonValue::get(FixedVectorType::get(getInt32Ty(), 8));
  else
    imageDesc = transformImageDesc(imageDesc, true, false, resultTy);
  imageDesc = fixImageDescForRead(imageDesc);

  YCbCrSampleInfo sampleInfoLuma = {resultTy, dim, flags, imageDesc, samplerDescLuma, address, instName.str(), true};

  GfxIpVersion gfxIp = getPipelineState()->getTargetInfo().getGfxIpVersion();

  // Init YCbCrConverterer
  YCbCrConverter YCbCrConverter(this, yCbCrMetaData, &sampleInfoLuma, &gfxIp);

  // Set image descriptor for chroma channel
  for (unsigned planeIdx = 1; planeIdx < yCbCrMetaData.word1.planes; ++planeIdx) {
    imageDesc = CreateExtractValue(imageDescArray, planeIdx);
    if (isa<PoisonValue>(imageDesc))
      imageDesc = PoisonValue::get(FixedVectorType::get(getInt32Ty(), 8));
    else
      imageDesc = transformImageDesc(imageDesc, true, false, resultTy);
    imageDesc = fixImageDescForRead(imageDesc);
    YCbCrConverter.SetImgDescChroma(planeIdx, imageDesc);
  }

  // Sample image source data
  YCbCrConverter.sampleYCbCrData();
  // Convert from YCbCr to RGB
  result = YCbCrConverter.convertColorSpace();

  return static_cast<Instruction *>(result);
}

// =====================================================================================================================
// Create an image gather.
// The caller supplies all arguments to the image sample op in "address", in the order specified
// by the indices defined as ImageIndex* below.
//
// @param resultTy : Result type
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor
// @param samplerDesc : Sampler descriptor
// @param address : Address and other arguments
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageGather(Type *resultTy, unsigned dim, unsigned flags, Value *imageDesc,
                                      Value *samplerDesc, ArrayRef<Value *> address, const Twine &instName) {
  if (isa<PoisonValue>(imageDesc) || isa<PoisonValue>(samplerDesc))
    return PoisonValue::get(resultTy);

  Value *coord = address[ImageAddressIdxCoordinate];
  assert(coord->getType()->getScalarType()->isFloatTy() || coord->getType()->getScalarType()->isHalfTy());

  // Check whether we are being asked for integer texel component type.
  Type *texelTy = resultTy;
  if (auto structResultTy = dyn_cast<StructType>(resultTy))
    texelTy = structResultTy->getElementType(0);
  Type *texelComponentTy = texelTy->getScalarType();
  Type *gatherTy = resultTy;

  if (texelComponentTy->isIntegerTy()) {
    // Handle integer texel component type.
    Type *replacedTy = texelComponentTy->getIntegerBitWidth() == 16 ? getHalfTy() : getFloatTy();
    gatherTy = FixedVectorType::get(replacedTy, 4);
    if (resultTy != texelTy)
      gatherTy = StructType::get(getContext(), {gatherTy, getInt32Ty()});
  }

  samplerDesc = transformSamplerDesc(samplerDesc);

  if (m_pipelineState->getOptions().disableTruncCoordForGather) {
    samplerDesc = modifySamplerDescForGather(samplerDesc);
  }

  Value *result = nullptr;
  Value *addrOffset = address[ImageAddressIdxOffset];
  if (addrOffset && isa<ArrayType>(addrOffset->getType())) {
    // We implement a gather with independent offsets (SPIR-V ConstantOffsets) as four separate gathers.
    Value *residency = nullptr;
    SmallVector<Value *, ImageAddressCount> modifiedAddress;
    modifiedAddress.insert(modifiedAddress.begin(), address.begin(), address.end());
    auto gatherStructTy = dyn_cast<StructType>(gatherTy);
    result = PoisonValue::get(gatherStructTy ? gatherStructTy->getElementType(0) : gatherTy);
    for (unsigned index = 0; index < 4; ++index) {
      modifiedAddress[ImageAddressIdxOffset] = CreateExtractValue(addrOffset, index);
      Value *singleResult = CreateImageSampleGather(gatherTy, dim, flags, coord, imageDesc, samplerDesc,
                                                    modifiedAddress, instName, false);
      if (gatherStructTy) {
        residency = CreateExtractValue(singleResult, 1);
        singleResult = CreateExtractValue(singleResult, 0);
      }
      result = CreateInsertElement(result, CreateExtractElement(singleResult, 3), index);
    }
    if (residency) {
      result = CreateInsertValue(PoisonValue::get(gatherTy), result, 0);
      result = CreateInsertValue(result, residency, 1);
    }
  } else {
    // No independent offsets. Do the single image gather.
    result = CreateImageSampleGather(gatherTy, dim, flags, coord, imageDesc, samplerDesc, address, instName, false);
  }

  // Bitcast returned texel from v4f32 to v4i32. (It would be easier to call the gather
  // intrinsic with the right return type, but we do it this way to match the code generated
  // before the image rework.)
  if (isa<StructType>(result->getType())) {
    // TFE: Need to extract texel from the struct, convert it, and re-insert it.
    Value *texel = CreateExtractValue(result, 0);
    Value *tfe = CreateExtractValue(result, 1);
    texel = cast<Instruction>(CreateBitCast(texel, texelTy));
    result = PoisonValue::get(StructType::get(getContext(), {texel->getType(), tfe->getType()}));
    result = CreateInsertValue(result, texel, 0);
    result = CreateInsertValue(result, tfe, 1);
  } else
    result = cast<Instruction>(CreateBitCast(result, texelTy));

  return result;
}

// =====================================================================================================================
// Common code to create an image sample or gather.
// The caller supplies all arguments to the image sample op in "address", in the order specified
// by the indices defined as ImageIndex* below.
//
// @param resultTy : Result type
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param coord : Coordinates (the one in address is ignored in favor of this one)
// @param imageDesc : Image descriptor
// @param samplerDesc : Sampler descriptor
// @param address : Address and other arguments
// @param instName : Name to give instruction(s)
// @param isSample : Is sample rather than gather
Value *BuilderImpl::CreateImageSampleGather(Type *resultTy, unsigned dim, unsigned flags, Value *coord,
                                            Value *imageDesc, Value *samplerDesc, ArrayRef<Value *> address,
                                            const Twine &instName, bool isSample) {
  if (isa<PoisonValue>(imageDesc) || isa<PoisonValue>(samplerDesc))
    return PoisonValue::get(resultTy);

  imageDesc = transformImageDesc(imageDesc, false, false, resultTy);
  const bool isVecTyDesc = imageDesc->getType()->isVectorTy();
  if (isVecTyDesc)
    imageDesc = fixImageDescForRead(imageDesc);

  samplerDesc = transformSamplerDesc(samplerDesc);

  // Mark usage of images, to allow the compute workgroup reconfiguration optimization.
  getPipelineState()->getShaderResourceUsage(m_shaderStage.value())->useImages = true;
  // Set up the mask of address components provided, for use in searching the intrinsic ID table
  unsigned addressMask = 0;
  for (unsigned i = 0; i != ImageAddressCount; ++i) {
    unsigned addressMaskBit = address[i] ? 1 : 0;
    addressMask |= addressMaskBit << i;
  }
  addressMask &= ~(1U << ImageAddressIdxProjective);
  addressMask &= ~(1U << ImageAddressIdxComponent);

  // Prepare the coordinate and derivatives, which might also change the dimension.
  SmallVector<Value *, 4> coords;
  SmallVector<Value *, 6> derivatives;
  Value *projective = address[ImageAddressIdxProjective];
  if (projective)
    projective = CreateFDiv(ConstantFP::get(projective->getType(), 1.0), projective);

  dim = prepareCoordinate(dim, coord, projective, address[ImageAddressIdxDerivativeX],
                          address[ImageAddressIdxDerivativeY], coords, derivatives);

  // Build the intrinsic arguments and overloaded types.
  SmallVector<Value *, 16> args;
  SmallVector<Type *, 4> overloadTys;
  overloadTys.push_back(resultTy);

  // Dmask.
  unsigned dmask = 15;
  if (address[ImageAddressIdxZCompare])
    dmask = 1;
  else if (!isSample) {
    dmask = 1;
    if (!address[ImageAddressIdxZCompare])
      dmask = 1U << cast<ConstantInt>(address[ImageAddressIdxComponent])->getZExtValue();
  }
  args.push_back(getInt32(dmask));

  // Offset: Supplied to us as a scalar or vector of i32, but need to be three 6-bit fields
  // X=[5:0] Y=[13:8] Z=[21:16] in a single i32.
  if (Value *offsetVal = address[ImageAddressIdxOffset]) {
    Value *singleOffsetVal = nullptr;
    if (isa<VectorType>((offsetVal)->getType())) {
      singleOffsetVal = CreateAnd(CreateExtractElement(offsetVal, uint64_t(0)), getInt32(0x3F));
      if (cast<FixedVectorType>(offsetVal->getType())->getNumElements() >= 2) {
        singleOffsetVal = CreateOr(
            singleOffsetVal, CreateShl(CreateAnd(CreateExtractElement(offsetVal, 1), getInt32(0x3F)), getInt32(8)));
        if (cast<FixedVectorType>(offsetVal->getType())->getNumElements() >= 3) {
          singleOffsetVal = CreateOr(
              singleOffsetVal, CreateShl(CreateAnd(CreateExtractElement(offsetVal, 2), getInt32(0x3F)), getInt32(16)));
        }
      }
    } else
      singleOffsetVal = CreateAnd(offsetVal, getInt32(0x3F));
    args.push_back(singleOffsetVal);
  }

  // Bias: float
  if (Value *biasVal = address[ImageAddressIdxLodBias]) {
    args.push_back(biasVal);
    overloadTys.push_back(biasVal->getType());
  }

  // ZCompare (dref)
  if (Value *zCompareVal = address[ImageAddressIdxZCompare]) {
    if (projective)
      zCompareVal = CreateFMul(zCompareVal, projective);
    args.push_back(zCompareVal);
  }

  // Grad (explicit derivatives)
  if (!derivatives.empty()) {
    args.insert(args.end(), derivatives.begin(), derivatives.end());
    overloadTys.push_back(derivatives[0]->getType());
  }

  // Coordinate
  args.insert(args.end(), coords.begin(), coords.end());
  overloadTys.push_back(coords[0]->getType());

  // LodClamp
  if (Value *lodClampVal = address[ImageAddressIdxLodClamp])
    args.push_back(lodClampVal);

  // Lod
  if (Value *lodVal = address[ImageAddressIdxLod])
    args.push_back(lodVal);

  // Image and sampler
  unsigned imageDescArgIndex = args.size();
  args.push_back(imageDesc);
  args.push_back(samplerDesc);

  // i32 Unorm
  args.push_back(getInt1(dim == DimRect));

  // i32 tfe/lwe bits
  bool tfe = isa<StructType>(resultTy);
  args.push_back(getInt32(tfe));

  // glc/dlc bits
  CoherentFlag coherent = {};
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
    if (flags & (ImageFlagCoherent | ImageFlagVolatile)) {
      coherent.bits.glc = true;
      coherent.bits.dlc = true;
    }
  }

  args.push_back(getInt32(coherent.u32All));

  // Search the intrinsic ID table.
  auto table = isSample ? &ImageSampleIntrinsicTable[0] : &ImageGather4IntrinsicTable[0];
  for (;; ++table) {
    assert(table->matchMask != 0 && "Image sample/gather intrinsic ID not found");
    if (table->matchMask == addressMask)
      break;
  }

  // Rectangle texture uses the same Intrinsic ID with 2D texture.
  Intrinsic::ID intrinsicId = (dim == DimRect) ? table->ids[Dim2D] : table->ids[dim];

  // Create the intrinsic.
  Instruction *imageOp = CreateIntrinsic(intrinsicId, overloadTys, args, nullptr, instName);

  // Add a waterfall loop if needed.
  SmallVector<unsigned, 2> nonUniformArgIndexes;
  if (imageDesc->getType()->isVectorTy()) {
    if (flags & ImageFlagNonUniformImage)
      nonUniformArgIndexes.push_back(imageDescArgIndex);
    else if (flags & ImageFlagEnforceReadFirstLaneImage)
      enforceReadFirstLane(imageOp, imageDescArgIndex);
  }

  if (samplerDesc->getType()->isVectorTy()) {
    const unsigned samplerDescArgIndex = imageDescArgIndex + 1;
    if (flags & ImageFlagNonUniformSampler) {
      nonUniformArgIndexes.push_back(samplerDescArgIndex);
    } else {
      // TODO: Re-add the condition once backend fix the waterfall loop bug.
      enforceReadFirstLane(imageOp, samplerDescArgIndex);
    }
  }

  if (!nonUniformArgIndexes.empty())
    imageOp = createWaterfallLoop(imageOp, nonUniformArgIndexes,
                                  getPipelineState()->getShaderOptions(m_shaderStage.value()).scalarizeWaterfallLoads);
  return imageOp;
}

// =====================================================================================================================
// Create an image atomic operation other than compare-and-swap.
//
// @param atomicOp : Atomic op to create
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param ordering : Atomic ordering
// @param imageDesc : Image descriptor
// @param coord : Coordinates: scalar or vector i32
// @param inputValue : Input value: i32
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageAtomic(unsigned atomicOp, unsigned dim, unsigned flags, AtomicOrdering ordering,
                                      Value *imageDesc, Value *coord, Value *inputValue, const Twine &instName) {
  return CreateImageAtomicCommon(atomicOp, dim, flags, ordering, imageDesc, coord, inputValue, nullptr, instName);
}

// =====================================================================================================================
// Create an image atomic compare-and-swap.
//
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param ordering : Atomic ordering
// @param imageDesc : Image descriptor
// @param coord : Coordinates: scalar or vector i32
// @param inputValue : Input value: i32
// @param comparatorValue : Value to compare against: i32
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageAtomicCompareSwap(unsigned dim, unsigned flags, AtomicOrdering ordering,
                                                 Value *imageDesc, Value *coord, Value *inputValue,
                                                 Value *comparatorValue, const Twine &instName) {
  return CreateImageAtomicCommon(AtomicOpCompareSwap, dim, flags, ordering, imageDesc, coord, inputValue,
                                 comparatorValue, instName);
}

// =====================================================================================================================
// Common code for CreateImageAtomic and CreateImageAtomicCompareSwap
//
// @param atomicOp : Atomic op to create
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param ordering : Atomic ordering
// @param imageDesc : Image descriptor
// @param coord : Coordinates: scalar or vector i32
// @param inputValue : Input value: i32
// @param comparatorValue : Value to compare against: i32; ignored if not compare-swap
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageAtomicCommon(unsigned atomicOp, unsigned dim, unsigned flags, AtomicOrdering ordering,
                                            Value *imageDesc, Value *coord, Value *inputValue, Value *comparatorValue,
                                            const Twine &instName) {
  if (isa<PoisonValue>(imageDesc))
    return PoisonValue::get(inputValue->getType());
  getPipelineState()->getShaderResourceUsage(m_shaderStage.value())->resourceWrite = true;
  assert(coord->getType()->getScalarType()->isIntegerTy(32));
  coord = handleFragCoordViewIndex(coord, flags, dim);

  switch (ordering) {
  case AtomicOrdering::Release:
  case AtomicOrdering::AcquireRelease:
  case AtomicOrdering::SequentiallyConsistent:
    CreateFence(AtomicOrdering::Release, getContext().getOrInsertSyncScopeID("agent"));
    break;
  default:
    break;
  }

  // Prepare the coordinate, which might also change the dimension.
  SmallVector<Value *, 4> coords;
  SmallVector<Value *, 6> derivatives;
  dim = prepareCoordinate(dim, coord, nullptr, nullptr, nullptr, coords, derivatives);

  bool isTexelBuffer = (dim == Dim1DBuffer || dim == Dim1DArrayBuffer);
  imageDesc = transformImageDesc(imageDesc, false, isTexelBuffer, nullptr);

  SmallVector<Value *, 8> args;
  Instruction *atomicInst = nullptr;
  unsigned imageDescArgIndex = 0;
  if (!isTexelBuffer) {
    // Resource descriptor. Use the image atomic instruction.
    args.push_back(inputValue);
    if (atomicOp == AtomicOpCompareSwap)
      args.push_back(comparatorValue);
    args.insert(args.end(), coords.begin(), coords.end());
    imageDescArgIndex = args.size();
    args.push_back(imageDesc);
    args.push_back(getInt32(0));
    args.push_back(getInt32(0));

    // Get the intrinsic ID from the load intrinsic ID table, and create the intrinsic.
    // Rectangle image uses the same Intrinsic ID with 2D image.
    Intrinsic::ID intrinsicId =
        (dim == DimRect) ? ImageAtomicIntrinsicTable[atomicOp][Dim2D] : ImageAtomicIntrinsicTable[atomicOp][dim];
    atomicInst = CreateIntrinsic(intrinsicId, {inputValue->getType(), coord->getType()->getScalarType()}, args, nullptr,
                                 instName);
  } else {
    // Texel buffer descriptor. Use the buffer atomic instruction.
    args.push_back(inputValue);
    if (atomicOp == AtomicOpCompareSwap)
      args.push_back(comparatorValue);
    imageDescArgIndex = args.size();
    args.push_back(imageDesc);
    args.push_back(coords[0]);
    args.push_back(getInt32(0));
    args.push_back(getInt32(0));
    args.push_back(getInt32(0));
    atomicInst =
        CreateIntrinsic(StructBufferAtomicIntrinsicTable[atomicOp], inputValue->getType(), args, nullptr, instName);
  }
  if (imageDesc->getType()->isVectorTy()) {
    if (flags & ImageFlagNonUniformImage)
      atomicInst =
          createWaterfallLoop(atomicInst, imageDescArgIndex,
                              getPipelineState()->getShaderOptions(m_shaderStage.value()).scalarizeWaterfallLoads);
    else if (flags & ImageFlagEnforceReadFirstLaneImage)
      enforceReadFirstLane(atomicInst, imageDescArgIndex);
  }

  switch (ordering) {
  case AtomicOrdering::Acquire:
  case AtomicOrdering::AcquireRelease:
  case AtomicOrdering::SequentiallyConsistent:
    CreateFence(AtomicOrdering::Acquire, getContext().getOrInsertSyncScopeID("agent"));
    break;
  default:
    break;
  }

  return atomicInst;
}

// =====================================================================================================================
// Create a query of the number of mipmap levels in an image. Returns an i32 value.
//
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor or texel buffer descriptor
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageQueryLevels(unsigned dim, unsigned flags, Value *imageDesc, const Twine &instName) {
  if (isa<PoisonValue>(imageDesc))
    return PoisonValue::get(getInt32Ty());
  dim = dim == DimCubeArray ? DimCube : dim;

  imageDesc = transformImageDesc(imageDesc, true, false, nullptr);

  Value *numMipLevel = nullptr;
  GfxIpVersion gfxIp = getPipelineState()->getTargetInfo().getGfxIpVersion();
  SqImgRsrcRegHandler proxySqRsrcRegHelper(this, imageDesc, &gfxIp);
  if (dim == Dim2DMsaa || dim == Dim2DArrayMsaa)
    numMipLevel = getInt32(1);
  else {
    Value *lastLevel = proxySqRsrcRegHelper.getReg(SqRsrcRegs::LastLevel);
    Value *baseLevel = proxySqRsrcRegHelper.getReg(SqRsrcRegs::BaseLevel);
    numMipLevel = CreateSub(lastLevel, baseLevel);
    numMipLevel = CreateAdd(numMipLevel, getInt32(1));
  }

  // Set to 0 if allowNullDescriptor is on and image descriptor is a null descriptor
  if (m_pipelineState->getOptions().allowNullDescriptor) {
    // Check dword3 against 0 for a null descriptor
    Value *descWord3 = CreateExtractElement(imageDesc, 3);
    if (m_pipelineState->getOptions().maskOffNullDescriptorTypeField) {
      // Mask off the type bits for the null descriptor
      unsigned typeMask = proxySqRsrcRegHelper.getRegMask(SqRsrcRegs::Type);
      descWord3 = CreateAnd(descWord3, getInt32(~typeMask));
    }
    Value *isNullDesc = CreateICmpEQ(descWord3, getInt32(0));
    numMipLevel = CreateSelect(isNullDesc, getInt32(0), numMipLevel);
  }

  return numMipLevel;
}

// =====================================================================================================================
// Create a query of the number of samples in an image. Returns an i32 value.
//
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor or texel buffer descriptor
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageQuerySamples(unsigned dim, unsigned flags, Value *imageDesc, const Twine &instName) {
  if (isa<PoisonValue>(imageDesc))
    return PoisonValue::get(getInt32Ty());

  imageDesc = transformImageDesc(imageDesc, true, false, nullptr);
  Value *descWord3 = CreateExtractElement(imageDesc, 3);
  Value *lastLevel = nullptr;
  if (m_pipelineState->getTargetInfo().getGfxIpVersion().major <= 11) {
    // Extract LAST_LEVEL (SQ_IMG_RSRC_WORD3, [19:16])
    lastLevel = CreateIntrinsic(Intrinsic::amdgcn_ubfe, getInt32Ty(), {descWord3, getInt32(16), getInt32(4)});
  }
  // Sample number = 1 << LAST_LEVEL
  Value *sampleNumber = CreateShl(getInt32(1), lastLevel);

  // Extract TYPE(SQ_IMG_RSRC_WORD3, [31:28])
  Value *imageType = CreateIntrinsic(Intrinsic::amdgcn_ubfe, getInt32Ty(), {descWord3, getInt32(28), getInt32(4)});

  // Check if resource type is 2D MSAA or 2D MSAA array, 14 = SQ_RSRC_IMG_2D_MSAA, 15 = SQ_RSRC_IMG_2D_MSAA_ARRAY
  Value *isMsaa = CreateOr(CreateICmpEQ(imageType, getInt32(14)), CreateICmpEQ(imageType, getInt32(15)));

  // Return sample number if resource type is 2D MSAA or 2D MSAA array. Otherwise, return 1.
  sampleNumber = CreateSelect(isMsaa, sampleNumber, getInt32(1), instName);

  // The sampler number is clamped 0 if allowNullDescriptor is on and image descriptor is a null descriptor
  if (m_pipelineState->getOptions().allowNullDescriptor) {
    if (m_pipelineState->getOptions().maskOffNullDescriptorTypeField) {
      GfxIpVersion gfxIp = getPipelineState()->getTargetInfo().getGfxIpVersion();
      SqImgRsrcRegHandler proxySqRsrcRegHelper(this, imageDesc, &gfxIp);
      unsigned typeMask = proxySqRsrcRegHelper.getRegMask(SqRsrcRegs::Type);
      // Mask off the type bits for the null descriptor
      descWord3 = CreateAnd(descWord3, getInt32(~typeMask));
    }
    // Check dword3 against 0 for a null descriptor
    Value *isNullDesc = CreateICmpEQ(descWord3, getInt32(0));
    sampleNumber = CreateSelect(isNullDesc, getInt32(0), sampleNumber);
  }

  if (flags & ImageFlagSamplePatternOffset) {
    Value *descWord6 = CreateExtractElement(imageDesc, 6);
    // sample pattern index is bit203-206, which is bit11-14 of word 6.
    Value *samplePatternOffset =
        CreateIntrinsic(Intrinsic::amdgcn_ubfe, getInt32Ty(), {descWord6, getInt32(11), getInt32(4)});
    // This is offset in entries, and now it is converted to offset in dwords (each entry has 16 dwords).
    samplePatternOffset = CreateMul(samplePatternOffset, getInt32(16));
    // Or the offset to the high 16 bit of sampleNumber.
    sampleNumber = CreateOr(CreateShl(samplePatternOffset, 16), sampleNumber);
  }

  return sampleNumber;
}

// =====================================================================================================================
// Create a query of size of an image.
// Returns an i32 scalar or vector of the width given by GetImageQuerySizeComponentCount.
//
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor or texel buffer descriptor
// @param lod : LOD
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageQuerySize(unsigned dim, unsigned flags, Value *imageDesc, Value *lod,
                                         const Twine &instName) {
  if (isa<PoisonValue>(imageDesc))
    return PoisonValue::get(getInt32Ty());
  bool isTexelBuffer = (dim == Dim1DBuffer || dim == Dim1DArrayBuffer);
  imageDesc = transformImageDesc(imageDesc, true, isTexelBuffer, nullptr);
  if (isTexelBuffer) {
    // Texel buffer.
    // Extract NUM_RECORDS (SQ_BUF_RSRC_WORD2)
    Value *numRecords = CreateExtractElement(imageDesc, 2);

    if (!instName.isTriviallyEmpty())
      numRecords->setName(instName);
    return numRecords;
  }

  // Proper image.
  unsigned modifiedDim = dim == DimCubeArray ? DimCube : dim;
  Value *resInfo = nullptr;

  GfxIpVersion gfxIp = getPipelineState()->getTargetInfo().getGfxIpVersion();
  SqImgRsrcRegHandler proxySqRsrcRegHelper(this, imageDesc, &gfxIp);
  Value *width = proxySqRsrcRegHelper.getReg(SqRsrcRegs::Width);
  Value *height = proxySqRsrcRegHelper.getReg(SqRsrcRegs::Height);
  Value *depth = proxySqRsrcRegHelper.getReg(SqRsrcRegs::Depth);
  Value *baseLevel = proxySqRsrcRegHelper.getReg(SqRsrcRegs::BaseLevel);

  if (dim == Dim2DMsaa || dim == Dim2DArrayMsaa)
    baseLevel = getInt32(0);

  Value *curLevel = CreateAdd(baseLevel, lod);

  // Size of the level
  width = CreateLShr(width, curLevel);
  width = CreateSelect(CreateICmpEQ(width, getInt32(0)), getInt32(1), width);
  height = CreateLShr(height, curLevel);
  height = CreateSelect(CreateICmpEQ(height, getInt32(0)), getInt32(1), height);

  if (dim == Dim3D) {
    Value *mipDepth = CreateLShr(depth, curLevel);
    mipDepth = CreateSelect(CreateICmpEQ(mipDepth, getInt32(0)), getInt32(1), mipDepth);

    Value *arrayPitch = proxySqRsrcRegHelper.getReg(SqRsrcRegs::ArrayPitch);
    Value *baseArray = proxySqRsrcRegHelper.getReg(SqRsrcRegs::BaseArray);
    Value *sliceDepth = CreateSub(depth, baseArray);
    Value *isSlice = CreateTrunc(arrayPitch, getInt1Ty());
    depth = CreateSelect(isSlice, sliceDepth, mipDepth);
  }

  // Set to 0 if allowNullDescriptor is on and image descriptor is a null descriptor
  if (m_pipelineState->getOptions().allowNullDescriptor) {
    // Check dword3 against 0 for a null descriptor
    Value *descWord3 = CreateExtractElement(imageDesc, 3);
    if (m_pipelineState->getOptions().maskOffNullDescriptorTypeField) {
      // Mask off the type bits for the null descriptor
      unsigned typeMask = proxySqRsrcRegHelper.getRegMask(SqRsrcRegs::Type);
      descWord3 = CreateAnd(descWord3, getInt32(~typeMask));
    }
    Value *isNullDesc = CreateICmpEQ(descWord3, getInt32(0));
    width = CreateSelect(isNullDesc, getInt32(0), width);
    height = CreateSelect(isNullDesc, getInt32(0), height);
    depth = CreateSelect(isNullDesc, getInt32(0), depth);
  }

  resInfo = CreateInsertElement(PoisonValue::get(FixedVectorType::get(getInt32Ty(), 4)), width, uint64_t(0));
  if (dim == Dim1DArray)
    resInfo = CreateInsertElement(resInfo, depth, 1);
  else
    resInfo = CreateInsertElement(resInfo, height, 1);

  resInfo = CreateInsertElement(resInfo, depth, 2);

  unsigned sizeComponentCount = getImageQuerySizeComponentCount(dim);

  if (sizeComponentCount == 1)
    return CreateExtractElement(resInfo, uint64_t(0), instName);

  if (dim == DimCubeArray) {
    Value *slices = CreateExtractElement(resInfo, 2);
    slices = CreateSDiv(slices, getInt32(6));
    resInfo = CreateInsertElement(resInfo, slices, 2);
  }

  if (dim == Dim1DArray && modifiedDim == Dim2DArray) {
    // For a 1D array on gfx9+ that we treated as a 2D array, we want components 0 and 2.
    return CreateShuffleVector(resInfo, resInfo, ArrayRef<int>{0, 2}, instName);
  }
  return CreateShuffleVector(resInfo, resInfo, ArrayRef<int>({0, 1, 2}).slice(0, sizeComponentCount), instName);
}

// =====================================================================================================================
// Create a get of the LOD that would be used for an image sample with the given coordinates
// and implicit LOD. Returns a v2f32 containing the layer number and the implicit level of
// detail relative to the base level.
//
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor
// @param samplerDesc : Sampler descriptor
// @param coord : Coordinates: scalar or vector f32, exactly right width without array layer
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageGetLod(unsigned dim, unsigned flags, Value *imageDesc, Value *samplerDesc, Value *coord,
                                      const Twine &instName) {
  if (isa<PoisonValue>(imageDesc) || isa<PoisonValue>(samplerDesc))
    return PoisonValue::get(FixedVectorType::get(getFloatTy(), 2));

  // Remove array from dimension if any.
  switch (dim) {
  case Dim1DArray:
    dim = Dim1D;
    break;
  case Dim2DArray:
    dim = Dim2D;
    break;
  case DimCubeArray:
    dim = DimCube;
    break;
  default:
    assert(dim <= DimCube);
    break;
  }

  // Prepare the coordinate, which might also change the dimension.
  SmallVector<Value *, 4> coords;
  SmallVector<Value *, 6> derivatives;
  dim = prepareCoordinate(dim, coord, nullptr, nullptr, nullptr, coords, derivatives);

  imageDesc = transformImageDesc(imageDesc, false, false, nullptr);
  if (isa<FixedVectorType>(samplerDesc->getType())) {
    // Only the first 4 dwords are sampler descriptor, we need to extract these values under any condition
    samplerDesc = CreateShuffleVector(samplerDesc, ArrayRef<int>{0, 1, 2, 3});
  } else {
    samplerDesc = transformSamplerDesc(samplerDesc);
  }

  SmallVector<Value *, 9> args;
  args.push_back(getInt32(3)); // dmask
  args.insert(args.end(), coords.begin(), coords.end());
  unsigned imageDescArgIndex = args.size();
  args.push_back(imageDesc);      // image desc
  args.push_back(samplerDesc);    // sampler desc
  args.push_back(getInt1(false)); // unorm
  args.push_back(getInt32(0));    // tfe/lwe
  args.push_back(getInt32(0));    // glc/slc

  Instruction *result = CreateIntrinsic(ImageGetLodIntrinsicTable[dim],
                                        {FixedVectorType::get(getFloatTy(), 2), getFloatTy()}, args, nullptr, instName);

  SmallVector<unsigned, 2> nonUniformArgIndexes;
  if (imageDesc->getType()->isVectorTy()) {
    // Add a waterfall loop if needed.
    if (flags & ImageFlagNonUniformImage)
      nonUniformArgIndexes.push_back(imageDescArgIndex);
    else if (flags & ImageFlagEnforceReadFirstLaneImage)
      enforceReadFirstLane(result, imageDescArgIndex);
  }

  if (samplerDesc->getType()->isVectorTy()) {
    const unsigned samplerDescArgIndex = imageDescArgIndex + 1;
    if (flags & ImageFlagNonUniformSampler)
      nonUniformArgIndexes.push_back(samplerDescArgIndex);
    else if (flags & ImageFlagEnforceReadFirstLaneSampler)
      enforceReadFirstLane(result, samplerDescArgIndex);
  }
  if (!nonUniformArgIndexes.empty())
    result = createWaterfallLoop(result, nonUniformArgIndexes,
                                 getPipelineState()->getShaderOptions(m_shaderStage.value()).scalarizeWaterfallLoads);

  return result;
}

// =====================================================================================================================
// Create a query of the sample position of given sample id in an image. Returns an v2f32 value.
//
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor or texel buffer descriptor
// @param sampleId : Sample ID
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageGetSamplePosition(unsigned dim, unsigned flags, Value *imageDesc, Value *sampleId,
                                                 const Twine &instName) {
  // Add ImageFlagSamplePatternOffset to query back both sample count and sample pattern offset.
  Value *sampleInfo = CreateImageQuerySamples(dim, flags | ImageFlagSamplePatternOffset, imageDesc);
  Value *sampleCount = CreateAnd(sampleInfo, 0xFFFF);
  Value *samplePatternOffset = CreateLShr(sampleInfo, 16);

  Value *validOffset = CreateAdd(samplePatternOffset, sampleId);
  // offset = (sampleCount > sampleId) ? (samplePatternOffset + sampleId) : 0
  Value *sampleValid = CreateICmpUGT(sampleCount, sampleId);
  Value *offset = CreateSelect(sampleValid, validOffset, getInt32(0));

  // Load sample position descriptor.
  Type *descTy = getDescTy(ResourceNodeType::DescriptorBuffer);
  Value *desc = create<LoadDriverTableEntryOp>(descTy, SiDrvTableSamplepos);
  // Load the value using the descriptor.
  offset = CreateShl(offset, getInt32(4));

  Type *samplePosTy = FixedVectorType::get(getFloatTy(), 2);
  return CreateIntrinsic(Intrinsic::amdgcn_raw_buffer_load, samplePosTy, {desc, offset, getInt32(0), getInt32(0)});
}

// =====================================================================================================================
// Create a ray intersect result with specified node in BVH buffer
//
// @param nodePtr : BVH node pointer
// @param extent : The valid range on which intersections can occur
// @param origin : Intersect ray origin
// @param direction : Intersect ray direction
// @param invDirection : The inverse of direction
// @param imageDesc : Image descriptor
// @param instName : Name to give instruction(s)
Value *BuilderImpl::CreateImageBvhIntersectRay(Value *nodePtr, Value *extent, Value *origin, Value *direction,
                                               Value *invDirection, Value *imageDesc, const Twine &instName) {
  imageDesc = fixImageDescForRead(imageDesc);

  SmallVector<Value *, 6> args;
  args.push_back(nodePtr);
  args.push_back(extent);
  args.push_back(origin);
  args.push_back(direction);
  args.push_back(invDirection);
  args.push_back(imageDesc);

  return CreateIntrinsic(FixedVectorType::get(getInt32Ty(), 4), Intrinsic::amdgcn_image_bvh_intersect_ray, args);
}

// =====================================================================================================================
// Prepare coordinate and explicit derivatives, pushing the separate components into the supplied vectors, and
// modifying if necessary.
// Returns possibly modified image dimension.
//
// @param dim : Image dimension
// @param coord : Scalar or vector coordinate value
// @param projective : Value to multiply into each coordinate component; nullptr if none
// @param derivativeX : Scalar or vector X derivative value, nullptr if none
// @param derivativeY : Scalar or vector Y derivative value, nullptr if none
// @param [out] outCoords : Vector to push coordinate components into
// @param [out] outDerivatives : Vector to push derivative components into
unsigned BuilderImpl::prepareCoordinate(unsigned dim, Value *coord, Value *projective, Value *derivativeX,
                                        Value *derivativeY, SmallVectorImpl<Value *> &outCoords,
                                        SmallVectorImpl<Value *> &outDerivatives) {
  // Push the coordinate components.
  Type *coordTy = coord->getType();
  Type *coordScalarTy = coordTy->getScalarType();

  if (coordTy == coordScalarTy) {
    // Push the single component.
    assert(getImageNumCoords(dim) == 1);
    outCoords.push_back(coord);
  } else {
    // Push the components.
    for (unsigned i = 0; i != getImageNumCoords(dim); ++i)
      outCoords.push_back(CreateExtractElement(coord, i));
  }

  // Divide the projective value into each component.
  // (We need to do this before we add an extra component for GFX9+.)
  if (projective) {
    for (unsigned i = 0; i != outCoords.size(); ++i)
      outCoords[i] = CreateFMul(outCoords[i], projective);
  }

  if (coordScalarTy->isIntegerTy()) {
    // Integer components (image load/store/atomic).
    assert(!derivativeX && !derivativeY);

    if (dim == DimCubeArray) {
      // For a cubearray, combine the face and slice into a single component.
      combineCubeArrayFaceAndSlice(coord, outCoords);
      dim = DimCube;
    }
    return dim;
  }

  // FP coordinates, possibly with explicit derivatives.
  // Round the array slice.
  if (dim == Dim1DArray || dim == Dim2DArray || dim == DimCubeArray)
    outCoords.back() = CreateIntrinsic(Intrinsic::rint, coordScalarTy, outCoords.back());

  Value *cubeSc = nullptr;
  Value *cubeTc = nullptr;
  Value *cubeMa = nullptr;
  Value *cubeId = nullptr;
  if (dim == DimCube || dim == DimCubeArray) {
    // For a cube or cubearray, transform the coordinates into s,t,faceid.
    cubeSc = CreateIntrinsic(Intrinsic::amdgcn_cubesc, {}, {outCoords[0], outCoords[1], outCoords[2]});
    cubeTc = CreateIntrinsic(Intrinsic::amdgcn_cubetc, {}, {outCoords[0], outCoords[1], outCoords[2]});
    cubeMa = CreateIntrinsic(Intrinsic::amdgcn_cubema, {}, {outCoords[0], outCoords[1], outCoords[2]});
    cubeId = CreateIntrinsic(Intrinsic::amdgcn_cubeid, {}, {outCoords[0], outCoords[1], outCoords[2]});

    Value *absMa = CreateIntrinsic(Intrinsic::fabs, getFloatTy(), cubeMa);
    Value *recipAbsMa = CreateFDiv(ConstantFP::get(getFloatTy(), 1.0), absMa);
    Value *sc = CreateFMul(cubeSc, recipAbsMa);
    sc = CreateFAdd(sc, ConstantFP::get(getFloatTy(), 1.5));
    Value *tc = CreateFMul(cubeTc, recipAbsMa);
    tc = CreateFAdd(tc, ConstantFP::get(getFloatTy(), 1.5));

    outCoords[0] = sc;
    outCoords[1] = tc;
    outCoords[2] = cubeId;

    // For a cubearray, combine the face and slice into a single component.
    if (dim == DimCubeArray) {
      Value *face = outCoords[2];
      Value *slice = outCoords[3];
      Constant *multiplier = ConstantFP::get(face->getType(), 8.0);
      Value *combined = CreateFMul(slice, multiplier);
      combined = CreateFAdd(combined, face);
      outCoords[2] = combined;
      outCoords.pop_back();
      dim = DimCube;
    }

    // Round the cube face ID.
    outCoords[2] = CreateIntrinsic(Intrinsic::rint, getFloatTy(), outCoords[2]);
  }

  // Push the derivative components.
  if (derivativeX) {
    // Derivatives by X
    if (auto vectorDerivativeXTy = dyn_cast<FixedVectorType>(derivativeX->getType())) {
      for (unsigned i = 0; i != vectorDerivativeXTy->getNumElements(); ++i)
        outDerivatives.push_back(CreateExtractElement(derivativeX, i));
    } else
      outDerivatives.push_back(derivativeX);

    // Derivatives by Y
    if (auto vectorDerivativeYTy = dyn_cast<FixedVectorType>(derivativeY->getType())) {
      for (unsigned i = 0; i != vectorDerivativeYTy->getNumElements(); ++i)
        outDerivatives.push_back(CreateExtractElement(derivativeY, i));
    } else
      outDerivatives.push_back(derivativeY);
  }
  if (outDerivatives.empty() || dim != DimCube)
    return dim;

  // When sampling cubemap with explicit gradient value, API supplied gradients are cube vectors,
  // need to transform them to face gradients for the selected face.
  // Mapping of MajorAxis, U-Axis, V-Axis is (according to DXSDK doc and refrast):
  //   faceId  | MajorAxis | FaceUAxis | FaceVAxis
  //   0       | +X        | -Z        | -Y
  //   1       | -X        | +Z        | -Y
  //   2       | +Y        | +X        | +Z
  //   3       | -Y        | +X        | -Z
  //   4       | +Z        | +X        | -Y
  //   5       | -Z        | -X        | -Y
  //   (Major Axis is defined by enum D3D11_TEXTURECUBE_FACE in d3d ddk header file (d3d11.h in DX11DDK).)
  //
  // Parameters used to convert cube gradient vector to face gradient (face ids are in floats because hardware
  // returns floats):
  //   faceId  | faceIdPos    | faceNeg   | flipU | flipV
  //   0.0     | 0.0          | false     | true  | true
  //   1.0     | 0.0          | true      | false | true
  //   2.0     | 1.0          | false     | false | false
  //   3.0     | 1.0          | true      | false | true
  //   4.0     | 2.0          | false     | false | true
  //   5.0     | 2.0          | true      | true  | true

  Value *faceCoordX = cubeSc;
  Value *faceCoordY = cubeTc;
  Value *faceId = cubeId;

  Value *gradXx = outDerivatives[0];
  Value *gradXy = outDerivatives[1];
  Value *gradXz = outDerivatives[2];
  Value *gradYx = outDerivatives[3];
  Value *gradYy = outDerivatives[4];
  Value *gradYz = outDerivatives[5];

  outDerivatives.resize(4);

  Constant *negOne = ConstantFP::get(faceId->getType(), -1.0);
  Constant *zero = Constant::getNullValue(faceId->getType());
  Constant *half = ConstantFP::get(faceId->getType(), 0.5);
  Constant *one = ConstantFP::get(faceId->getType(), 1.0);
  Constant *two = ConstantFP::get(faceId->getType(), 2.0);
  Constant *five = ConstantFP::get(faceId->getType(), 5.0);

  // faceIdHalf = faceId * 0.5
  Value *faceIdHalf = CreateFMul(faceId, half);
  // faceIdPos = round_zero(faceIdHalf)
  //   faceIdPos is: 0.0 (X axis) when face ID is 0.0 or 1.0;
  //                 1.0 (Y axis) when face ID is 2.0 or 3.0;
  //                 2.0 (Z axis) when face ID is 4.0 or 5.0;
  Value *faceIdPos = CreateIntrinsic(Intrinsic::trunc, faceIdHalf->getType(), faceIdHalf);
  // faceNeg = (faceIdPos != faceIdHalf)
  //   faceNeg is true when major axis is negative, this corresponds to             face ID being 1.0, 3.0, or 5.0
  Value *faceNeg = CreateFCmpONE(faceIdPos, faceIdHalf);
  // faceIsY = (faceIdPos == 1.0);
  Value *faceIsY = CreateFCmpOEQ(faceIdPos, one);
  // flipU is true when U-axis is negative, this corresponds to face ID being 0.0 or 5.0.
  Value *flipU = CreateOr(CreateFCmpOEQ(faceId, five), CreateFCmpOEQ(faceId, zero));
  // flipV is true when V-axis is negative, this corresponds to face ID being             anything other than 2.0.
  // flipV = (faceId != 2.0);
  Value *flipV = CreateFCmpONE(faceId, two);
  // major2.x = 1/major.x * 1/major.x * 0.5;
  //          = 1/(2*major.x) * 1/(2*major.x) * 2
  Value *recipMa = CreateFDiv(one, cubeMa);
  Value *major2X = CreateFMul(CreateFMul(recipMa, recipMa), two);

  Value *gradx = gradXx;
  Value *grady = gradXy;
  Value *gradz = gradXz;
  for (unsigned i = 0; i < 2; ++i) {
    // majorDeriv.x = (faceIdPos == 0.0) ? grad.x : grad.z;
    Value *majorDerivX = CreateSelect(CreateFCmpOEQ(faceIdPos, zero), gradx, gradz);
    // majorDeriv.x = (faceIsY == 0) ? majorDeriv.x : grad.y;
    majorDerivX = CreateSelect(faceIsY, grady, majorDerivX);
    // majorDeriv.x = (faceNeg == 0.0) ? majorDeriv.x : (-majorDeriv.x);
    majorDerivX = CreateSelect(faceNeg, CreateFMul(majorDerivX, negOne), majorDerivX);
    // faceDeriv.x = (faceIdPos == 0.0) ? grad.z : grad.x;
    Value *faceDerivX = CreateSelect(CreateFCmpOEQ(faceIdPos, zero), gradz, gradx);
    // faceDeriv.x = (flipU == 0) ? faceDeriv.x : (-faceDeriv.x);
    faceDerivX = CreateSelect(flipU, CreateFMul(faceDerivX, negOne), faceDerivX);
    // faceDeriv.y = (faceIsY == 0) ? grad.y : grad.z;
    Value *faceDerivY = CreateSelect(faceIsY, gradz, grady);
    // faceDeriv.y = (flipV == 0) ? faceDeriv.y : (-faceDeriv.y);
    faceDerivY = CreateSelect(flipV, CreateFMul(faceDerivY, negOne), faceDerivY);
    // faceDeriv.xy = major.xx * faceDeriv.xy;
    Value *halfMa = CreateFMul(cubeMa, half);
    faceDerivX = CreateFMul(faceDerivX, halfMa);
    faceDerivY = CreateFMul(faceDerivY, halfMa);
    // faceDeriv.xy = (-faceCrd.xy) * majorDeriv.xx + faceDeriv.xy;
    Value *negFaceCoordX = CreateFMul(faceCoordX, negOne);
    Value *negFaceCoordY = CreateFMul(faceCoordY, negOne);
    Value *faceDerivIncX = CreateFMul(negFaceCoordX, majorDerivX);
    Value *faceDerivIncY = CreateFMul(negFaceCoordY, majorDerivX);
    faceDerivX = CreateFAdd(faceDerivIncX, faceDerivX);
    faceDerivY = CreateFAdd(faceDerivIncY, faceDerivY);
    // grad.xy = faceDeriv.xy * major2.xx;
    outDerivatives[i * 2] = CreateFMul(faceDerivX, major2X);
    outDerivatives[i * 2 + 1] = CreateFMul(faceDerivY, major2X);

    gradx = gradYx;
    grady = gradYy;
    gradz = gradYz;
  }

  return dim;
}

// =====================================================================================================================
// For a cubearray with integer coordinates, combine the face and slice into a single component.
// In this case, the frontend may have generated code to separate the
// face and slice out of a single component, so we look for that code first.
//
// @param coord : Coordinate as vector value
// @param [in/out] coords : Coordinate components
void BuilderImpl::combineCubeArrayFaceAndSlice(Value *coord, SmallVectorImpl<Value *> &coords) {
  // See if we can find the face and slice components in a chain of insertelements.
  Constant *multiplier = getInt32(6);
  Value *face = nullptr;
  Value *slice = nullptr;
  Value *partialCoord = coord;
  while (auto insert = dyn_cast<InsertElementInst>(partialCoord)) {
    unsigned index = cast<ConstantInt>(insert->getOperand(2))->getZExtValue();
    switch (index) {
    case 2:
      face = !face ? insert->getOperand(1) : face;
      break;
    case 3:
      slice = !slice ? insert->getOperand(1) : slice;
      break;
    }
    partialCoord = insert->getOperand(0);
  }

  Value *combined = nullptr;
  if (face && slice) {
    if (auto sliceDiv = dyn_cast<BinaryOperator>(slice)) {
      if (auto faceRem = dyn_cast<BinaryOperator>(face)) {
        if (sliceDiv->getOpcode() == Instruction::UDiv && faceRem->getOpcode() == Instruction::URem &&
            sliceDiv->getOperand(1) == multiplier && faceRem->getOperand(1) == multiplier &&
            sliceDiv->getOperand(0) == faceRem->getOperand(0)) {
          // This is the case that the slice and face were extracted from a combined value using
          // the same multiplier. That happens with SPIR-V with multiplier 6.
          combined = sliceDiv->getOperand(0);
        }
      }
    }
  }

  if (!combined) {
    // We did not find the div and rem generated by the frontend to separate the face and slice.
    face = coords[2];
    slice = coords[3];
    combined = CreateMul(slice, multiplier);
    combined = CreateAdd(combined, face);
  }
  coords[2] = combined;
  coords.pop_back();
}

// =====================================================================================================================
// Handle cases where we need to add the FragCoord x,y to the coordinate, and use ViewIndex as the z coordinate.
//
// @param coord : Coordinate, scalar or vector i32
// @param flags : Image flags
// @param [in/out] dim : Image dimension
Value *BuilderImpl::handleFragCoordViewIndex(Value *coord, unsigned flags, unsigned &dim) {
  bool useViewIndex = false;
  if (flags & ImageFlagCheckMultiView) {
    if (getPipelineState()->getInputAssemblyState().multiView != MultiViewMode::Disable) {
      useViewIndex = true;
      dim = Dim2DArray;
      unsigned coordCount = cast<FixedVectorType>(coord->getType())->getNumElements();
      if (coordCount < 3) {
        const static int Indexes[] = {0, 1, 1};
        coord = CreateShuffleVector(coord, Constant::getNullValue(coord->getType()), Indexes);
      }
    }
  }

  if (flags & ImageFlagAddFragCoord) {
    // Get FragCoord, convert to signed i32, and add its x,y to the coordinate.
    // For now, this just generates a call to lgc.input.import.builtin. A future commit will
    // change it to use a Builder call to read the built-in.
    getPipelineState()->getShaderResourceUsage(m_shaderStage.value())->builtInUsage.fs.fragCoord = true;

    const static unsigned BuiltInFragCoord = 15;
    std::string callName = lgcName::InputImportBuiltIn;
    Type *builtInTy = FixedVectorType::get(getFloatTy(), 4);
    addTypeMangling(builtInTy, {}, callName);
    Value *fragCoord = CreateNamedCall(callName, builtInTy, getInt32(BuiltInFragCoord), {});
    fragCoord->setName("FragCoord");
    fragCoord = CreateShuffleVector(fragCoord, fragCoord, ArrayRef<int>{0, 1});
    fragCoord = CreateFPToSI(fragCoord, FixedVectorType::get(getInt32Ty(), 2));
    unsigned coordCount = cast<FixedVectorType>(coord->getType())->getNumElements();
    if (coordCount > 2) {
      const static int Indexes[] = {0, 1, 2, 3};
      fragCoord = CreateShuffleVector(fragCoord, Constant::getNullValue(fragCoord->getType()),
                                      ArrayRef<int>(Indexes).slice(0, coordCount));
    }
    coord = CreateAdd(coord, fragCoord);
  }

  if (useViewIndex) {
    // Get ViewIndex and use it as the z coordinate.
    // For now, this just generates a call to lgc.input.import.builtin. A future commit will
    // change it to use a Builder call to read the built-in.
    auto &builtInUsage = getPipelineState()->getShaderResourceUsage(m_shaderStage.value())->builtInUsage;
    switch (m_shaderStage.value()) {
    case ShaderStage::Vertex:
      builtInUsage.vs.viewIndex = true;
      break;
    case ShaderStage::TessControl:
      builtInUsage.tcs.viewIndex = true;
      break;
    case ShaderStage::TessEval:
      builtInUsage.tes.viewIndex = true;
      break;
    case ShaderStage::Geometry:
      builtInUsage.gs.viewIndex = true;
      break;
    case ShaderStage::Mesh:
      builtInUsage.mesh.viewIndex = true;
      break;
    case ShaderStage::Fragment:
      builtInUsage.fs.viewIndex = true;
      break;
    default:
      llvm_unreachable("Should never be called!");
      break;
    }

    const static unsigned BuiltInViewIndex = 4440;
    std::string callName = lgcName::InputImportBuiltIn;
    Type *builtInTy = getInt32Ty();
    addTypeMangling(builtInTy, {}, callName);
    Value *rtLayer = CreateNamedCall(callName, builtInTy, getInt32(BuiltInViewIndex), {});
    if (getPipelineState()->getInputAssemblyState().multiView == MultiViewMode::PerView)
      rtLayer = CreateLShr(rtLayer, getInt32(8)); // RT layer id is in the high 24 bits of view index.
    rtLayer->setName("Layer");
    coord = CreateInsertElement(coord, rtLayer, 2);
  }

  return coord;
}

// =====================================================================================================================
// Fix image descriptor before an operation that reads the image, as long as we really have an image descriptor,
// not a buffer descriptor for a texel buffer.
//
// @param imageDesc : Original image descriptor
// @returns Image descriptor, modified if necessary
Value *BuilderImpl::fixImageDescForRead(Value *imageDesc) {
  if (getPipelineState()->getTargetInfo().getGpuWorkarounds().gfx10.waClearWriteCompressBit) {
    if (cast<FixedVectorType>(imageDesc->getType())->getNumElements() == 8) {
      // Need to clear the write_compress_enable bit, which is bit 212, or bit 20 of dword 6.
      // I am hard-coding it here as it is only needed on a limited range of chips.
      Value *dword6 = CreateExtractElement(imageDesc, 6);
      dword6 = CreateAnd(dword6, getInt32(0xFFEFFFFF));
      imageDesc = CreateInsertElement(imageDesc, dword6, 6);
    }
  }
  return imageDesc;
}

// =====================================================================================================================
// Enforce readfirstlane on the given descriptor.
//
// @param imageInst : the image instruction
// @param descIdx : the index of the descriptor to put readfirstlane on
void BuilderImpl::enforceReadFirstLane(Instruction *imageInst, unsigned descIdx) {
  InsertPointGuard guard(*this);
  SetInsertPoint(imageInst);
  Value *origDesc = imageInst->getOperand(descIdx);
  const unsigned elemCount = cast<FixedVectorType>(origDesc->getType())->getNumElements();
  Value *newDesc = PoisonValue::get(FixedVectorType::get(getInt32Ty(), elemCount));
  for (unsigned elemIdx = 0; elemIdx < elemCount; ++elemIdx) {
    Value *elem = CreateExtractElement(origDesc, elemIdx);
    elem = CreateIntrinsic(getInt32Ty(), Intrinsic::amdgcn_readfirstlane, elem);
    newDesc = CreateInsertElement(newDesc, elem, elemIdx);
  }
  imageInst->setOperand(descIdx, newDesc);
}

// =====================================================================================================================
// Modify sampler descriptor to force set trunc_coord as 0 for gather4 instruction.
//
// @param imageDesc : Original sampler descriptor
// @returns Sampler descriptor, modified if necessary
Value *BuilderImpl::modifySamplerDescForGather(Value *samplerDesc) {
  // Need to clear the trunc_coord bit for gather4, which is bit 27 of dword 0.
  Value *dword0 = CreateExtractElement(samplerDesc, uint64_t(0));
  dword0 = CreateAnd(dword0, getInt32(0xF7FFFFFF));
  samplerDesc = CreateInsertElement(samplerDesc, dword0, uint64_t(0));

  return samplerDesc;
}

// =====================================================================================================================
// Transform image descriptor pointer to a i32 type or a descriptor load instruction.
//
// @param imageDesc : image descriptor pointer
// @param mustLoad : Whether to load image descriptor from the pointer
// @param isTexelBuffer : Whether it is a texel buffer
// @param texelType : The type of the texel
// @returns The transformed descriptor
Value *BuilderImpl::transformImageDesc(Value *imageDesc, bool mustLoad, bool isTexelBuffer, Type *texelType) {
  assert(!isa<PoisonValue>(imageDesc));
  if (isa<FixedVectorType>(imageDesc->getType()))
    return imageDesc;

  // Explicitly load the descriptor from the descriptor pointer
  Type *descType = FixedVectorType::get(getInt32Ty(), isTexelBuffer ? 4 : 8);
  Value *desc = CreateLoad(descType, imageDesc);
  cast<Instruction>(desc)->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(getContext(), {}));
  return desc;
}

// =====================================================================================================================
// Transform sampler descriptor pointer to a i32 type or a descriptor load instruction.
//
// @param samplerDesc : descriptor pointer or a full descriptor
// @returns Transformed sampler descriptor
Value *BuilderImpl::transformSamplerDesc(Value *samplerDesc) {
  assert(!isa<PoisonValue>(samplerDesc));
  if (isa<FixedVectorType>(samplerDesc->getType()))
    return samplerDesc;

  // Explicitly load the descriptor from the descriptor pointer
  Type *descType = FixedVectorType::get(getInt32Ty(), 4);
  Value *desc = CreateLoad(descType, samplerDesc);
  cast<Instruction>(desc)->setMetadata(LLVMContext::MD_invariant_load, MDNode::get(getContext(), {}));
  return desc;
}
