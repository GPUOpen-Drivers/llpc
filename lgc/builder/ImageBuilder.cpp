/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019-2020 Advanced Micro Devices, Inc. All Rights Reserved.
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
 * @file  ImageBuilder.cpp
 * @brief LLPC source file: implementation of Builder methods for image operations
 ***********************************************************************************************************************
 */
#include "BuilderImpl.h"
#include "lgc/state/TargetInfo.h"
#include "lgc/util/Internal.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicsAMDGPU.h"

#define DEBUG_TYPE "llpc-builder-impl-image"

using namespace lgc;
using namespace llvm;

// Intrinsic ID table for getresinfo
static const Intrinsic::ID ImageGetResInfoIntrinsicTable[] = {
    Intrinsic::amdgcn_image_getresinfo_1d,      Intrinsic::amdgcn_image_getresinfo_2d,
    Intrinsic::amdgcn_image_getresinfo_3d,      Intrinsic::amdgcn_image_getresinfo_cube,
    Intrinsic::amdgcn_image_getresinfo_1darray, Intrinsic::amdgcn_image_getresinfo_2darray,
    Intrinsic::amdgcn_image_getresinfo_2dmsaa,  Intrinsic::amdgcn_image_getresinfo_2darraymsaa};

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
    Intrinsic::amdgcn_struct_buffer_atomic_xor};

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
};

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
    newTy = VectorType::get(newTy, cast<VectorType>(origTy)->getNumElements());
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
Value *ImageBuilder::CreateImageLoad(Type *resultTy, unsigned dim, unsigned flags, Value *imageDesc, Value *coord,
                                     Value *mipLevel, const Twine &instName) {
  getPipelineState()->getShaderResourceUsage(m_shaderStage)->resourceRead = true;
  assert(coord->getType()->getScalarType()->isIntegerTy(32));
  imageDesc = patchCubeDescriptor(imageDesc, dim);
  coord = handleFragCoordViewIndex(coord, flags, dim);

  unsigned dmask = 1;
  Type *origTexelTy = resultTy;
  if (auto structResultTy = dyn_cast<StructType>(resultTy))
    origTexelTy = structResultTy->getElementType(0);

  Type *texelTy = origTexelTy;
  if (origTexelTy->isIntOrIntVectorTy(64)) {
    // Only load the first component for 64-bit texel, casted to <2 x i32>
    texelTy = VectorType::get(getInt32Ty(), 2);
  }

  if (auto vectorResultTy = dyn_cast<VectorType>(texelTy))
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
  Value *result = nullptr;
  unsigned imageDescArgIndex = 0;
  if (imageDesc->getType() == getImageDescTy()) {
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
    args.push_back(getInt32(((flags & ImageFlagCoherent) ? 1 : 0) | ((flags & ImageFlagVolatile) ? 2 : 0)));

    // Get the intrinsic ID from the load intrinsic ID table and call it.
    auto table = mipLevel ? &ImageLoadMipIntrinsicTable[0] : &ImageLoadIntrinsicTable[0];
    result = CreateIntrinsic(table[dim], {intrinsicDataTy, coords[0]->getType()}, args, nullptr, instName);
  } else {
    // Texel buffer descriptor. Use the buffer instruction.
    imageDescArgIndex = args.size();
    args.push_back(imageDesc);
    args.push_back(coords[0]);
    args.push_back(getInt32(0));
    args.push_back(getInt32(0));
    args.push_back(getInt32(0));
    result = CreateIntrinsic(Intrinsic::amdgcn_struct_buffer_load_format, intrinsicDataTy, args, nullptr, instName);
  }

  // For 64-bit texel, only the first component is loaded, other components are filled in with (0, 0, 1). This
  // operation could be viewed as supplement of the intrinsic call.
  if (origTexelTy->isIntOrIntVectorTy(64)) {
    Value *texel = result;
    if (isa<StructType>(resultTy))
      texel = CreateExtractValue(result, uint64_t(0));
    texel = CreateBitCast(texel, getInt64Ty()); // Casted to i64

    if (origTexelTy->isVectorTy()) {
      texel = CreateInsertElement(UndefValue::get(origTexelTy), texel, uint64_t(0));

      SmallVector<Value *, 3> defaults = {getInt64(0), getInt64(0), getInt64(1)};
      for (unsigned i = 1; i < cast<VectorType>(origTexelTy)->getNumElements(); ++i)
        texel = CreateInsertElement(texel, defaults[i - 1], i);
    }

    if (isa<StructType>(resultTy)) {
      // TFE
      intrinsicDataTy = StructType::get(origTexelTy->getContext(), {origTexelTy, getInt32Ty()});
      result = CreateInsertValue(CreateInsertValue(UndefValue::get(intrinsicDataTy), texel, uint64_t(0)),
                                 CreateExtractValue(result, 1), 1);
    } else {
      intrinsicDataTy = origTexelTy;
      result = texel;
    }
  }

  // Add a waterfall loop if needed.
  if (flags & ImageFlagNonUniformImage)
    result = createWaterfallLoop(cast<Instruction>(result), imageDescArgIndex);

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
Value *ImageBuilder::CreateImageLoadWithFmask(Type *resultTy, unsigned dim, unsigned flags, Value *imageDesc,
                                              Value *fmaskDesc, Value *coord, Value *sampleNum, const Twine &instName) {
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
  Value *fmaskTexel = CreateImageLoad(VectorType::get(getInt32Ty(), 4), fmaskDim, flags, fmaskDesc, coord, nullptr,
                                      instName + ".fmaskload");

  // Calculate the sample number we would use if the F-mask descriptor format is valid.
  Value *calcSampleNum = CreateExtractElement(fmaskTexel, uint64_t(0));
  Value *shiftSampleNum = CreateShl(sampleNum, getInt32(2));
  calcSampleNum = CreateLShr(calcSampleNum, shiftSampleNum);
  calcSampleNum = CreateAnd(calcSampleNum, getInt32(15));

  // Check whether the F-mask descriptor has a BUF_DATA_FORMAT_INVALID (0) format (dword[1].bit[20-25]).
  Value *fmaskFormat = CreateExtractElement(fmaskDesc, 1);
  fmaskFormat = CreateAnd(fmaskFormat, getInt32(63 << 20));
  Value *fmaskValidFormat = CreateICmpNE(fmaskFormat, getInt32(0));

  // Use that to select the calculated sample number or the provided one, then append that to the coordinates.
  sampleNum = CreateSelect(fmaskValidFormat, calcSampleNum, sampleNum);
  sampleNum = CreateInsertElement(UndefValue::get(coord->getType()), sampleNum, uint64_t(0));
  static const int Idxs[] = {0, 1, 2, 3};
  coord = CreateShuffleVector(coord, sampleNum, ArrayRef<int>(Idxs).slice(0, dim == Dim2DArrayMsaa ? 4 : 3));

  // Now do the normal load.
  return dyn_cast<Instruction>(CreateImageLoad(resultTy, dim, flags, imageDesc, coord, nullptr, instName));
}

// =====================================================================================================================
// Create an image store.
//
// @param texel : Texel value to store; v4i16, v4i32, v4i64, v4f16 or v4f32
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor
// @param coord : Coordinates: scalar or vector i32
// @param mipLevel : Mipmap level if doing load_mip, otherwise nullptr
// @param instName : Name to give instruction(s)
Value *ImageBuilder::CreateImageStore(Value *texel, unsigned dim, unsigned flags, Value *imageDesc, Value *coord,
                                      Value *mipLevel, const Twine &instName) {
  getPipelineState()->getShaderResourceUsage(m_shaderStage)->resourceWrite = true;
  assert(coord->getType()->getScalarType()->isIntegerTy(32));
  imageDesc = patchCubeDescriptor(imageDesc, dim);
  coord = handleFragCoordViewIndex(coord, flags, dim);

  // For 64-bit texel, only the first component is stored
  if (texel->getType()->isIntOrIntVectorTy(64)) {
    if (texel->getType()->isVectorTy())
      texel = CreateExtractElement(texel, uint64_t(0));
    texel = CreateBitCast(texel, VectorType::get(getFloatTy(), 2)); // Casted to <2 x float>
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

  Type *texelTy = texel->getType();
  SmallVector<Value *, 16> args;
  Instruction *imageStore = nullptr;
  unsigned imageDescArgIndex = 0;
  if (imageDesc->getType() == getImageDescTy()) {
    // Not texel buffer; use image store instruction.
    // Build the intrinsic arguments.
    unsigned dmask = 1;
    if (auto vectorTexelTy = dyn_cast<VectorType>(texelTy))
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
    args.push_back(getInt32(((flags & ImageFlagCoherent) ? 1 : 0) | ((flags & ImageFlagVolatile) ? 2 : 0)));

    // Get the intrinsic ID from the store intrinsic ID table and call it.
    auto table = mipLevel ? &ImageStoreMipIntrinsicTable[0] : &ImageStoreIntrinsicTable[0];
    imageStore = CreateIntrinsic(table[dim], {texelTy, coords[0]->getType()}, args, nullptr, instName);
  } else {
    // Texel buffer descriptor. Use the buffer instruction.
    // First widen texel to vec4 if necessary.
    if (auto vectorTexelTy = dyn_cast<VectorType>(texelTy)) {
      if (vectorTexelTy->getNumElements() != 4) {
        texel = CreateShuffleVector(texel, Constant::getNullValue(texelTy), ArrayRef<int>{0, 1, 2, 3});
      }
    } else
      texel = CreateInsertElement(Constant::getNullValue(VectorType::get(texelTy, 4)), texel, uint64_t(0));

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

  // Add a waterfall loop if needed.
  if (flags & ImageFlagNonUniformImage)
    createWaterfallLoop(imageStore, imageDescArgIndex);

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
Value *ImageBuilder::CreateImageSample(Type *resultTy, unsigned dim, unsigned flags, Value *imageDesc,
                                       Value *samplerDesc, ArrayRef<Value *> address, const Twine &instName) {
  Value *coord = address[ImageAddressIdxCoordinate];
  assert(coord->getType()->getScalarType()->isFloatTy() || coord->getType()->getScalarType()->isHalfTy());

  // Find the {set,desc} for the sampler and see if it is a YCbCr converting sampler.
  // This is a hack to cope with the design of the Vulkan YCbCr conversion extension, in which
  // you cannot see in the SPIR-V that a sampler is a converting sampler. It does not work
  // if the sampler pointer is passed through a non-inlined function arg or a phi node.
  // If using BuilderRecorder, it relies on the order in which recorded builder calls are
  // processed in BuilderReplayer.
  if (m_pipelineState->haveConvertingSampler()) {
    if (auto loadFromPtr = dyn_cast<CallInst>(samplerDesc)) {
      if (Function *loadFromPtrFunc = loadFromPtr->getCalledFunction()) {
        if (loadFromPtrFunc->getName().startswith(lgcName::DescriptorLoadFromPtr)) {
          Value *descPtr = loadFromPtr->getOperand(0);
          if (auto descPtrCall = dyn_cast<CallInst>(descPtr)) {
            if (Function *descPtrCallFunc = descPtrCall->getCalledFunction()) {
              if (descPtrCallFunc->getName().startswith(lgcName::DescriptorGetSamplerPtr)) {
                // We have found the call that tells us the descriptor set and binding.
                unsigned descSet = cast<ConstantInt>(descPtrCall->getArgOperand(0))->getZExtValue();
                unsigned binding = cast<ConstantInt>(descPtrCall->getArgOperand(1))->getZExtValue();
                const ResourceNode *node =
                    m_pipelineState->findResourceNode(ResourceNodeType::DescriptorYCbCrSampler, descSet, binding)
                        .second;
                if (node) {
                  // It is a YCbCr converting sampler. Get the immutable converter and call the
                  // appropriate function for a converting image sample.
                  return CreateImageSampleConvert(resultTy, dim, flags, imageDesc, node->immutableValue, address,
                                                  instName);
                }
              }
            }
          }
        }
      }
    }
  }

  // Normal image sample.
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
// @param imageDesc : Image descriptor
// @param convertingSamplerDesc : Converting sampler descriptor (v8i32)
// @param address : Address and other arguments
// @param instName : Name to give instruction(s)
Value *ImageBuilder::CreateImageSampleConvert(Type *resultTy, unsigned dim, unsigned flags, Value *imageDesc,
                                              Value *convertingSamplerDesc, ArrayRef<Value *> address,
                                              const Twine &instName) {
  llvm_unreachable("Not yet implemented");
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
Value *ImageBuilder::CreateImageGather(Type *resultTy, unsigned dim, unsigned flags, Value *imageDesc,
                                       Value *samplerDesc, ArrayRef<Value *> address, const Twine &instName) {
  Value *coord = address[ImageAddressIdxCoordinate];
  assert(coord->getType()->getScalarType()->isFloatTy() || coord->getType()->getScalarType()->isHalfTy());

  // Check whether we are being asked for integer texel component type.
  Value *needDescPatch = nullptr;
  Type *texelTy = resultTy;
  if (auto structResultTy = dyn_cast<StructType>(resultTy))
    texelTy = structResultTy->getElementType(0);
  Type *texelComponentTy = texelTy->getScalarType();
  Type *gatherTy = resultTy;

  if (texelComponentTy->isIntegerTy()) {
    // Handle integer texel component type.
    gatherTy = VectorType::get(getFloatTy(), 4);
    if (resultTy != texelTy)
      gatherTy = StructType::get(getContext(), {gatherTy, getInt32Ty()});

    // For integer gather on pre-GFX9, patch descriptor or coordinate.
    needDescPatch = preprocessIntegerImageGather(dim, imageDesc, coord);
  }

  // Only the first 4 DWORDs are sampler descriptor, we need to extract these values under any condition
  samplerDesc = CreateShuffleVector(samplerDesc, samplerDesc, ArrayRef<int>{0, 1, 2, 3});

  Value *result = nullptr;
  Value *addrOffset = address[ImageAddressIdxOffset];
  if (addrOffset && isa<ArrayType>(addrOffset->getType())) {
    // We implement a gather with independent offsets (SPIR-V ConstantOffsets) as four separate gathers.
    Value *residency = nullptr;
    SmallVector<Value *, ImageAddressCount> modifiedAddress;
    modifiedAddress.insert(modifiedAddress.begin(), address.begin(), address.end());
    auto gatherStructTy = dyn_cast<StructType>(gatherTy);
    result = UndefValue::get(gatherStructTy ? gatherStructTy->getElementType(0) : gatherTy);
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
      result = CreateInsertValue(UndefValue::get(gatherTy), result, 0);
      result = CreateInsertValue(result, residency, 1);
    }
  } else {
    // No independent offsets. Do the single image gather.
    result = CreateImageSampleGather(gatherTy, dim, flags, coord, imageDesc, samplerDesc, address, instName, false);
  }

  if (needDescPatch) {
    // For integer gather on pre-GFX9, post-process the result.
    result = postprocessIntegerImageGather(needDescPatch, flags, imageDesc, texelTy, result);
  }

  // Bitcast returned texel from v4f32 to v4i32. (It would be easier to call the gather
  // intrinsic with the right return type, but we do it this way to match the code generated
  // before the image rework.)
  if (isa<StructType>(result->getType())) {
    // TFE: Need to extract texel from the struct, convert it, and re-insert it.
    Value *texel = CreateExtractValue(result, 0);
    Value *tfe = CreateExtractValue(result, 1);
    texel = cast<Instruction>(CreateBitCast(texel, texelTy));
    result = UndefValue::get(StructType::get(getContext(), {texel->getType(), tfe->getType()}));
    result = CreateInsertValue(result, texel, 0);
    result = CreateInsertValue(result, tfe, 1);
  } else
    result = cast<Instruction>(CreateBitCast(result, texelTy));

  return result;
}

// =====================================================================================================================
// Implement pre-GFX9 integer gather workaround to patch descriptor or coordinate, depending on format in descriptor
// Returns nullptr for GFX9+, or a bool value that is true if the descriptor was patched or false if the
// coordinate was modified.
//
// @param dim : Image dimension
// @param [in/out] imageDesc : Image descriptor
// @param [in/out] coord : Coordinate
Value *ImageBuilder::preprocessIntegerImageGather(unsigned dim, Value *&imageDesc, Value *&coord) {
  if (getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 9) {
    // GFX9+: Workaround not needed.
    return nullptr;
  }

  // Check whether the descriptor needs patching. It does if it does not have format 32, 32_32 or 32_32_32_32.
  Value *descDword1 = CreateExtractElement(imageDesc, 1);
  Value *dataFormat = CreateIntrinsic(Intrinsic::amdgcn_ubfe, getInt32Ty(), {descDword1, getInt32(20), getInt32(6)});
  Value *isDataFormat32 = CreateICmpEQ(dataFormat, getInt32(IMG_DATA_FORMAT_32));
  Value *isDataFormat3232 = CreateICmpEQ(dataFormat, getInt32(IMG_DATA_FORMAT_32_32));
  Value *isDataFormat32323232 = CreateICmpEQ(dataFormat, getInt32(IMG_DATA_FORMAT_32_32_32_32));
  Value *cond = CreateOr(isDataFormat3232, isDataFormat32);
  cond = CreateOr(isDataFormat32323232, cond);
  Value *needDescPatch = CreateXor(cond, getInt1(true));

  // Create the if..else..endif, where the condition is whether the descriptor needs patching.
  InsertPoint savedInsertPoint = saveIP();
  BranchInst *branch = createIf(needDescPatch, true, "before.int.gather");

  // Inside the "then": patch the descriptor: change NUM_FORMAT from SINT to SSCALE.
  Value *descDword1A = CreateExtractElement(imageDesc, 1);
  descDword1A = CreateSub(descDword1A, getInt32(0x08000000));
  Value *patchedImageDesc = CreateInsertElement(imageDesc, descDword1A, 1);

  // On to the "else": patch the coordinates: add (-0.5/width, -0.5/height) to the x,y coordinates.
  SetInsertPoint(branch->getSuccessor(1)->getTerminator());
  Value *zero = getInt32(0);
  dim = dim == DimCubeArray ? DimCube : dim;
  Value *resInfo = CreateIntrinsic(ImageGetResInfoIntrinsicTable[dim], {VectorType::get(getFloatTy(), 4), getInt32Ty()},
                                   {getInt32(15), zero, imageDesc, zero, zero});
  resInfo = CreateBitCast(resInfo, VectorType::get(getInt32Ty(), 4));

  Value *widthHeight = CreateShuffleVector(resInfo, resInfo, ArrayRef<int>{0, 1});
  widthHeight = CreateSIToFP(widthHeight, VectorType::get(getFloatTy(), 2));
  Value *valueToAdd = CreateFDiv(ConstantFP::get(widthHeight->getType(), -0.5), widthHeight);
  unsigned coordCount = cast<VectorType>(coord->getType())->getNumElements();
  if (coordCount > 2) {
    valueToAdd = CreateShuffleVector(valueToAdd, Constant::getNullValue(valueToAdd->getType()),
                                     ArrayRef<int>({0, 1, 2, 3}).slice(0, coordCount));
  }
  Value *patchedCoord = CreateFAdd(coord, valueToAdd);

  // Restore insert point to after the if..else..endif, and add the phi nodes.
  restoreIP(savedInsertPoint);
  PHINode *imageDescPhi = CreatePHI(imageDesc->getType(), 2);
  imageDescPhi->addIncoming(patchedImageDesc, branch->getSuccessor(0));
  imageDescPhi->addIncoming(imageDesc, branch->getSuccessor(1));
  imageDesc = imageDescPhi;

  PHINode *coordPhi = CreatePHI(coord->getType(), 2);
  coordPhi->addIncoming(coord, branch->getSuccessor(0));
  coordPhi->addIncoming(patchedCoord, branch->getSuccessor(1));
  coord = coordPhi;

  return needDescPatch;
}

// =====================================================================================================================
// Implement pre-GFX9 integer gather workaround to modify result.
// Returns possibly modified result.
//
// @param needDescPatch : Bool value that is true if descriptor was patched
// @param flags : Flags passed to CreateImageGather
// @param imageDesc : Image descriptor
// @param texelTy : Type of returned texel
// @param result : Returned texel value, or struct containing texel and TFE
Value *ImageBuilder::postprocessIntegerImageGather(Value *needDescPatch, unsigned flags, Value *imageDesc,
                                                   Type *texelTy, Value *result) {
  // Post-processing of result for integer return type.
  // Create the if..endif, where the condition is whether the descriptor was patched. If it was,
  // then we need to convert the texel from float to i32.
  InsertPoint savedInsertPoint = saveIP();
  BranchInst *branch = createIf(needDescPatch, false, "after.int.gather");

  // Process the returned texel.
  Value *texel = result;
  bool tfe = isa<StructType>(result->getType());
  if (tfe) {
    // TFE: Need to extract texel from the struct, convert it, and re-insert it.
    texel = CreateExtractValue(result, 0);
  }
  if (flags & ImageFlagSignedResult)
    texel = CreateFPToSI(texel, texelTy);
  else
    texel = CreateFPToUI(texel, texelTy);
  Value *patchedResult = CreateBitCast(texel, VectorType::get(getFloatTy(), 4));
  if (tfe)
    patchedResult = CreateInsertValue(result, patchedResult, 0);

  patchedResult = CreateSelect(needDescPatch, patchedResult, result);

  // Restore insert point to after the if..endif, and add the phi node.
  BasicBlock *thenBlock = GetInsertBlock();
  restoreIP(savedInsertPoint);
  PHINode *resultPhi = CreatePHI(result->getType(), 2);
  resultPhi->addIncoming(patchedResult, thenBlock);
  resultPhi->addIncoming(result, branch->getParent());

  return resultPhi;
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
Value *ImageBuilder::CreateImageSampleGather(Type *resultTy, unsigned dim, unsigned flags, Value *coord,
                                             Value *imageDesc, Value *samplerDesc, ArrayRef<Value *> address,
                                             const Twine &instName, bool isSample) {
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
      if (cast<VectorType>(offsetVal->getType())->getNumElements() >= 2) {
        singleOffsetVal = CreateOr(
            singleOffsetVal, CreateShl(CreateAnd(CreateExtractElement(offsetVal, 1), getInt32(0x3F)), getInt32(8)));
        if (cast<VectorType>(offsetVal->getType())->getNumElements() >= 3) {
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
  args.push_back(getInt1(false));

  // i32 tfe/lwe bits
  bool tfe = isa<StructType>(resultTy);
  args.push_back(getInt32(tfe));

  // glc/slc bits
  args.push_back(getInt32(((flags & ImageFlagCoherent) ? 1 : 0) | ((flags & ImageFlagVolatile) ? 2 : 0)));

  // Search the intrinsic ID table.
  auto table = isSample ? &ImageSampleIntrinsicTable[0] : &ImageGather4IntrinsicTable[0];
  for (;; ++table) {
    assert(table->matchMask != 0 && "Image sample/gather intrinsic ID not found");
    if (table->matchMask == addressMask)
      break;
  }
  Intrinsic::ID intrinsicId = table->ids[dim];

  // Create the intrinsic.
  Instruction *imageOp = CreateIntrinsic(intrinsicId, overloadTys, args, nullptr, instName);

  // Add a waterfall loop if needed.
  SmallVector<unsigned, 2> nonUniformArgIndexes;
  if (flags & ImageFlagNonUniformImage)
    nonUniformArgIndexes.push_back(imageDescArgIndex);
  if (flags & ImageFlagNonUniformSampler)
    nonUniformArgIndexes.push_back(imageDescArgIndex + 1);
  if (!nonUniformArgIndexes.empty())
    imageOp = createWaterfallLoop(imageOp, nonUniformArgIndexes);
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
Value *ImageBuilder::CreateImageAtomic(unsigned atomicOp, unsigned dim, unsigned flags, AtomicOrdering ordering,
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
Value *ImageBuilder::CreateImageAtomicCompareSwap(unsigned dim, unsigned flags, AtomicOrdering ordering,
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
Value *ImageBuilder::CreateImageAtomicCommon(unsigned atomicOp, unsigned dim, unsigned flags, AtomicOrdering ordering,
                                             Value *imageDesc, Value *coord, Value *inputValue, Value *comparatorValue,
                                             const Twine &instName) {
  getPipelineState()->getShaderResourceUsage(m_shaderStage)->resourceWrite = true;
  assert(coord->getType()->getScalarType()->isIntegerTy(32));
  coord = handleFragCoordViewIndex(coord, flags, dim);

  switch (ordering) {
  case AtomicOrdering::Release:
  case AtomicOrdering::AcquireRelease:
  case AtomicOrdering::SequentiallyConsistent:
    CreateFence(AtomicOrdering::Release, SyncScope::System);
    break;
  default:
    break;
  }

  // Prepare the coordinate, which might also change the dimension.
  SmallVector<Value *, 4> coords;
  SmallVector<Value *, 6> derivatives;
  dim = prepareCoordinate(dim, coord, nullptr, nullptr, nullptr, coords, derivatives);

  SmallVector<Value *, 8> args;
  Instruction *atomicInst = nullptr;
  unsigned imageDescArgIndex = 0;
  if (imageDesc->getType() == getImageDescTy()) {
    // Resource descriptor. Use the image atomic instruction.
    imageDesc = patchCubeDescriptor(imageDesc, dim);
    args.push_back(inputValue);
    if (atomicOp == AtomicOpCompareSwap)
      args.push_back(comparatorValue);
    args.insert(args.end(), coords.begin(), coords.end());
    imageDescArgIndex = args.size();
    args.push_back(imageDesc);
    args.push_back(getInt32(0));
    args.push_back(getInt32(0));

    // Get the intrinsic ID from the load intrinsic ID table, and create the intrinsic.
    Intrinsic::ID intrinsicId = ImageAtomicIntrinsicTable[atomicOp][dim];
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
  if (flags & ImageFlagNonUniformImage)
    atomicInst = createWaterfallLoop(atomicInst, imageDescArgIndex);

  switch (ordering) {
  case AtomicOrdering::Acquire:
  case AtomicOrdering::AcquireRelease:
  case AtomicOrdering::SequentiallyConsistent:
    CreateFence(AtomicOrdering::Acquire, SyncScope::System);
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
Value *ImageBuilder::CreateImageQueryLevels(unsigned dim, unsigned flags, Value *imageDesc, const Twine &instName) {
  dim = dim == DimCubeArray ? DimCube : dim;
  Value *zero = getInt32(0);
  Instruction *resInfo = CreateIntrinsic(ImageGetResInfoIntrinsicTable[dim], {getFloatTy(), getInt32Ty()},
                                         {getInt32(8), UndefValue::get(getInt32Ty()), imageDesc, zero, zero});
  if (flags & ImageFlagNonUniformImage)
    resInfo = createWaterfallLoop(resInfo, 2);
  return CreateBitCast(resInfo, getInt32Ty(), instName);
}

// =====================================================================================================================
// Create a query of the number of samples in an image. Returns an i32 value.
//
// @param dim : Image dimension
// @param flags : ImageFlag* flags
// @param imageDesc : Image descriptor or texel buffer descriptor
// @param instName : Name to give instruction(s)
Value *ImageBuilder::CreateImageQuerySamples(unsigned dim, unsigned flags, Value *imageDesc, const Twine &instName) {
  // Extract LAST_LEVEL (SQ_IMG_RSRC_WORD3, [19:16])
  Value *descWord3 = CreateExtractElement(imageDesc, 3);
  Value *lastLevel = CreateIntrinsic(Intrinsic::amdgcn_ubfe, getInt32Ty(), {descWord3, getInt32(16), getInt32(4)});
  // Sample number = 1 << LAST_LEVEL
  Value *sampleNumber = CreateShl(getInt32(1), lastLevel);

  // Extract TYPE(SQ_IMG_RSRC_WORD3, [31:28])
  Value *imageType = CreateIntrinsic(Intrinsic::amdgcn_ubfe, getInt32Ty(), {descWord3, getInt32(28), getInt32(4)});

  // Check if resource type is 2D MSAA or 2D MSAA array, 14 = SQ_RSRC_IMG_2D_MSAA, 15 = SQ_RSRC_IMG_2D_MSAA_ARRAY
  Value *isMsaa = CreateOr(CreateICmpEQ(imageType, getInt32(14)), CreateICmpEQ(imageType, getInt32(15)));

  // Return sample number if resource type is 2D MSAA or 2D MSAA array. Otherwise, return 1.
  return CreateSelect(isMsaa, sampleNumber, getInt32(1), instName);
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
Value *ImageBuilder::CreateImageQuerySize(unsigned dim, unsigned flags, Value *imageDesc, Value *lod,
                                          const Twine &instName) {
  if (imageDesc->getType() == getTexelBufferDescTy()) {
    // Texel buffer.
    // Extract NUM_RECORDS (SQ_BUF_RSRC_WORD2)
    Value *numRecords = CreateExtractElement(imageDesc, 2);

    if (getPipelineState()->getTargetInfo().getGfxIpVersion().major == 8) {
      // GFX8 only: extract STRIDE (SQ_BUF_RSRC_WORD1 [29:16]) and divide into NUM_RECORDS.
      Value *stride = CreateIntrinsic(Intrinsic::amdgcn_ubfe, getInt32Ty(),
                                      {CreateExtractElement(imageDesc, 1), getInt32(16), getInt32(14)});
      numRecords = CreateUDiv(numRecords, stride);
    }
    if (!instName.isTriviallyEmpty())
      numRecords->setName(instName);
    return numRecords;
  }

  // Proper image.
  unsigned modifiedDim = dim == DimCubeArray ? DimCube : change1DTo2DIfNeeded(dim);
  Value *zero = getInt32(0);
  Instruction *resInfo =
      CreateIntrinsic(ImageGetResInfoIntrinsicTable[modifiedDim], {VectorType::get(getFloatTy(), 4), getInt32Ty()},
                      {getInt32(15), lod, imageDesc, zero, zero});
  if (flags & ImageFlagNonUniformImage)
    resInfo = createWaterfallLoop(resInfo, 2);
  Value *intResInfo = CreateBitCast(resInfo, VectorType::get(getInt32Ty(), 4));

  unsigned sizeComponentCount = getImageQuerySizeComponentCount(dim);

  if (sizeComponentCount == 1)
    return CreateExtractElement(intResInfo, uint64_t(0), instName);

  if (dim == DimCubeArray) {
    Value *slices = CreateExtractElement(intResInfo, 2);
    slices = CreateSDiv(slices, getInt32(6));
    intResInfo = CreateInsertElement(intResInfo, slices, 2);
  }

  if (dim == Dim1DArray && modifiedDim == Dim2DArray) {
    // For a 1D array on gfx9+ that we treated as a 2D array, we want components 0 and 2.
    return CreateShuffleVector(intResInfo, intResInfo, ArrayRef<int>{0, 2}, instName);
  }
  return CreateShuffleVector(intResInfo, intResInfo, ArrayRef<int>({0, 1, 2}).slice(0, sizeComponentCount), instName);
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
Value *ImageBuilder::CreateImageGetLod(unsigned dim, unsigned flags, Value *imageDesc, Value *samplerDesc, Value *coord,
                                       const Twine &instName) {
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

  // Only the first 4 DWORDs are sampler descriptor, we need to extract these values under any condition
  samplerDesc = CreateShuffleVector(samplerDesc, samplerDesc, ArrayRef<int>{0, 1, 2, 3});

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
                                        {VectorType::get(getFloatTy(), 2), getFloatTy()}, args, nullptr, instName);
  // Add a waterfall loop if needed.
  SmallVector<unsigned, 2> nonUniformArgIndexes;
  if (flags & ImageFlagNonUniformImage)
    nonUniformArgIndexes.push_back(imageDescArgIndex);
  if (flags & ImageFlagNonUniformSampler)
    nonUniformArgIndexes.push_back(imageDescArgIndex + 1);

  if (!nonUniformArgIndexes.empty())
    result = createWaterfallLoop(result, nonUniformArgIndexes);
  return result;
}

// =====================================================================================================================
// Change 1D or 1DArray dimension to 2D or 2DArray if needed as a workaround on GFX9+
//
// @param dim : Image dimension
unsigned ImageBuilder::change1DTo2DIfNeeded(unsigned dim) {
  if (getPipelineState()->getTargetInfo().getGpuWorkarounds().gfx9.treat1dImagesAs2d) {
    switch (dim) {
    case Dim1D:
      return Dim2D;
    case Dim1DArray:
      return Dim2DArray;
    default:
      break;
    }
  }
  return dim;
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
unsigned ImageBuilder::prepareCoordinate(unsigned dim, Value *coord, Value *projective, Value *derivativeX,
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
    assert(getImageNumCoords(dim) == cast<VectorType>(coordTy)->getNumElements());

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

  // For 1D or 1DArray on GFX9+, change to 2D or 2DArray and add the extra component. The
  // extra component is 0 for int or 0.5 for FP.
  unsigned origDim = dim;
  bool needExtraDerivativeDim = false;
  dim = change1DTo2DIfNeeded(dim);
  if (dim != origDim) {
    Value *extraComponent = getInt32(0);
    needExtraDerivativeDim = true;
    if (!coordScalarTy->isIntegerTy())
      extraComponent = ConstantFP::get(coordScalarTy, 0.5);

    if (dim == Dim2D)
      outCoords.push_back(extraComponent);
    else {
      outCoords.push_back(outCoords.back());
      outCoords[1] = extraComponent;
    }
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
    if (auto vectorDerivativeXTy = dyn_cast<VectorType>(derivativeX->getType())) {
      for (unsigned i = 0; i != vectorDerivativeXTy->getNumElements(); ++i)
        outDerivatives.push_back(CreateExtractElement(derivativeX, i));
    } else
      outDerivatives.push_back(derivativeX);

    if (needExtraDerivativeDim) {
      // GFX9+ 1D -> 2D: need extra derivative too.
      outDerivatives.push_back(Constant::getNullValue(outDerivatives[0]->getType()));
    }

    // Derivatives by Y
    if (auto vectorDerivativeYTy = dyn_cast<VectorType>(derivativeY->getType())) {
      for (unsigned i = 0; i != vectorDerivativeYTy->getNumElements(); ++i)
        outDerivatives.push_back(CreateExtractElement(derivativeY, i));
    } else
      outDerivatives.push_back(derivativeY);

    if (needExtraDerivativeDim) {
      // GFX9+ 1D -> 2D: need extra derivative too.
      outDerivatives.push_back(Constant::getNullValue(outDerivatives[0]->getType()));
    }
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
void ImageBuilder::combineCubeArrayFaceAndSlice(Value *coord, SmallVectorImpl<Value *> &coords) {
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
// Patch descriptor with cube dimension for image load/store/atomic for GFX8 and earlier
//
// @param desc : Descriptor before patching
// @param dim : Image dimensions
Value *ImageBuilder::patchCubeDescriptor(Value *desc, unsigned dim) {
  if ((dim != DimCube && dim != DimCubeArray) || getPipelineState()->getTargetInfo().getGfxIpVersion().major >= 9)
    return desc;

  // Extract the depth.
  Value *elem4 = CreateExtractElement(desc, 4);
  Value *depth = CreateAnd(elem4, getInt32(0x1FFF));

  // Change to depth * 6 + 5
  depth = CreateMul(depth, getInt32(6));
  depth = CreateAdd(depth, getInt32(5));
  elem4 = CreateAnd(elem4, getInt32(0xFFFFE000));
  elem4 = CreateOr(elem4, depth);

  // Change resource type to 2D array (0xD)
  Value *elem3 = CreateExtractElement(desc, 3);
  elem3 = CreateAnd(elem3, getInt32(0x0FFFFFFF));
  elem3 = CreateOr(elem3, getInt32(0xD0000000));

  // Reassemble descriptor.
  desc = CreateInsertElement(desc, elem4, 4);
  desc = CreateInsertElement(desc, elem3, 3);
  return desc;
}

// =====================================================================================================================
// Handle cases where we need to add the FragCoord x,y to the coordinate, and use ViewIndex as the z coordinate.
//
// @param coord : Coordinate, scalar or vector i32
// @param flags : Image flags
// @param [in,out] dim : Image dimension
Value *ImageBuilder::handleFragCoordViewIndex(Value *coord, unsigned flags, unsigned &dim) {
  bool useViewIndex = false;
  if (flags & ImageFlagCheckMultiView) {
    if (getPipelineState()->getInputAssemblyState().enableMultiView) {
      useViewIndex = true;
      dim = Dim2DArray;
      unsigned coordCount = cast<VectorType>(coord->getType())->getNumElements();
      if (coordCount < 3) {
        const static int Indexes[] = {0, 1, 1};
        coord = CreateShuffleVector(coord, Constant::getNullValue(coord->getType()), Indexes);
      }
    }
  }

  if (flags & ImageFlagAddFragCoord) {
    // Get FragCoord, convert to signed i32, and add its x,y to the coordinate.
    // For now, this just generates a call to llpc.input.import.builtin. A future commit will
    // change it to use a Builder call to read the built-in.
    getPipelineState()->getShaderResourceUsage(m_shaderStage)->builtInUsage.fs.fragCoord = true;

    const static unsigned BuiltInFragCoord = 15;
    std::string callName = lgcName::InputImportBuiltIn;
    Type *builtInTy = VectorType::get(getFloatTy(), 4);
    addTypeMangling(builtInTy, {}, callName);
    Value *fragCoord = emitCall(callName, builtInTy, getInt32(BuiltInFragCoord), {}, &*GetInsertPoint());
    fragCoord->setName("FragCoord");
    fragCoord = CreateShuffleVector(fragCoord, fragCoord, ArrayRef<int>{0, 1});
    fragCoord = CreateFPToSI(fragCoord, VectorType::get(getInt32Ty(), 2));
    unsigned coordCount = cast<VectorType>(coord->getType())->getNumElements();
    if (coordCount > 2) {
      const static int Indexes[] = {0, 1, 2, 3};
      fragCoord = CreateShuffleVector(fragCoord, Constant::getNullValue(fragCoord->getType()),
                                      ArrayRef<int>(Indexes).slice(0, coordCount));
    }
    coord = CreateAdd(coord, fragCoord);
  }

  if (useViewIndex) {
    // Get ViewIndex and use it as the z coordinate.
    // For now, this just generates a call to llpc.input.import.builtin. A future commit will
    // change it to use a Builder call to read the built-in.
    auto &builtInUsage = getPipelineState()->getShaderResourceUsage(m_shaderStage)->builtInUsage;
    switch (m_shaderStage) {
    case ShaderStageVertex:
      builtInUsage.vs.viewIndex = true;
      break;
    case ShaderStageTessEval:
      builtInUsage.tes.viewIndex = true;
      break;
    case ShaderStageGeometry:
      builtInUsage.gs.viewIndex = true;
      break;
    case ShaderStageFragment:
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
    Value *viewIndex = emitCall(callName, builtInTy, getInt32(BuiltInViewIndex), {}, &*GetInsertPoint());
    viewIndex->setName("ViewIndex");
    coord = CreateInsertElement(coord, viewIndex, 2);
  }

  return coord;
}
