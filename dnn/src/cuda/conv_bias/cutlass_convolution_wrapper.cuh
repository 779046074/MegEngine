/**
 * \file dnn/src/cuda/conv_bias/cutlass_convolution_wrapper.cuh
 * MegEngine is Licensed under the Apache License, Version 2.0 (the "License")
 *
 * Copyright (c) 2014-2020 Megvii Inc. All rights reserved.
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT ARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.
 */
#pragma once
#include "cutlass/gemm/gemm.h"
#include "src/cuda/convolution_helper/parameter.cuh"
#include "src/cuda/utils.cuh"

namespace megdnn {
namespace cuda {
namespace cutlass_wrapper {

using GemmCoord = cutlass::gemm::GemmCoord;

template <typename Convolution>
void cutlass_convolution_wrapper(
        const int8_t* d_src, const int8_t* d_filter, const int32_t* d_bias,
        const int8_t* d_z, int8_t* d_dst, int* workspace,
        typename Convolution::ConvolutionParameter const& conv_param,
        typename Convolution::EpilogueOutputOp::Params const& epilogue,
        cudaStream_t stream);

template <bool NeedLoadFromConstMem>
void do_conv_bias_int8_implicit_gemm_imma_ncdiv32hw32(
        const int8_t* d_src, const int8_t* d_filter, const int32_t* d_bias,
        const int8_t* d_z, int8_t* d_dst, int* workspace,
        const convolution::ConvParam& param, uint32_t nonlinear_mode,
        float alpha, float beta, float gamma, float scale,
        const GemmCoord& threadblock_shape, const GemmCoord& warp_shape,
        cudaStream_t stream);

}  // namespace cutlass_wrapper
}  // namespace cuda
}  // namespace megdnn

// vim: syntax=cuda.doxygen