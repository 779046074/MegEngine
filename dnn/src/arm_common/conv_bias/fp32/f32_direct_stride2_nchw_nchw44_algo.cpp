/**
 * \file
 dnn/src/arm_common/conv_bias/fp32/f32_direct_stride2_nchw_nchw44_algo.cpp
 * MegEngine is Licensed under the Apache License, Version 2.0 (the "License")
 *
 * Copyright (c) 2014-2020 Megvii Inc. All rights reserved.
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT ARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.
 */

#include "megdnn/oprs.h"
#include "src/arm_common/conv_bias/fp32/algos.h"
#include "src/arm_common/conv_bias/fp32/f32_direct_stride2_nchw_nchw44_kern.h"
#include "src/arm_common/conv_bias/fp32/strategy.h"
#include "src/arm_common/elemwise_op.h"
#include "src/common/opr_delegate.h"

#include "midout.h"

using namespace megdnn;
using namespace arm_common;
using conv_fun = std::function<void(
        WorkspaceBundle bundle, const ConvBiasImpl::NCBKernParam& kern_param,
        const ConvBiasImpl::NCBKernIndex& ncb_index,
        const CpuNDRange& workspace_ids, const CpuNDRange& ncb_range)>;
MIDOUT_DECL(megdnn_arm_common_conv_bias_fp32_nchw_nchw44_stride2)
namespace {
static inline int block_helper(const int nthread, const int amount,
                               const int per_unit_bytes) {
    MEGDNN_MARK_USED_VAR(per_unit_bytes);
    const int block_per_thread = div_ceil(amount, nthread);
    const int best_block = 16;
    const int max_block_num = div_ceil(block_per_thread, best_block);
    const int min_block_num = std::max(max_block_num - 1, 1);
    const int max_block = div_ceil(block_per_thread, max_block_num);
    const int min_block = div_ceil(block_per_thread, min_block_num);
    const int max_loss = std::abs(max_block_num * max_block - block_per_thread);
    const int min_loss = std::abs(min_block_num * min_block - block_per_thread);
    int block = max_loss > min_loss ? min_block : max_block;
    return block;
}
static inline size_t get_perthread_cache_bytes(const int ic, const int ih2,
                                               const int iw2) {
    // border_size is used to avoid read illegal memory
    int border_size = 64 * 2;
    return ic * ih2 * iw2 * sizeof(float) + border_size;
}
static void get_rectified_size(
        const megdnn::fallback::ConvBiasImpl::NCBKernSizeParam& param, int& ih2,
        int& iw2, int& oh2, int& ow2) {
    int iw = param.isz[1];
    int oh = param.osz[0];
    int ow = param.osz[1];

    oh2 = oh;
    ow2 = ow;
    constexpr int cacheline = 64 / sizeof(float);
    int block_oh = block_helper(param.nr_threads, oh, 0);
    auto&& fm = param.filter_meta;
    const int stride_h = static_cast<int>(fm.stride[0]);
    const int filter_h = static_cast<int>(fm.spatial[0]);
    ih2 = block_oh * stride_h + filter_h - stride_h;
    iw2 = round_up(iw + 2 * static_cast<int>(fm.padding[1]), cacheline);
}

static WorkspaceBundle get_bundle(const ConvBiasImpl::NCBKernSizeParam& param) {
    auto&& fm = param.filter_meta;
    int group = fm.group;
    int ic = fm.icpg;
    int oc = fm.ocpg;
    int fh = fm.spatial[0];
    int fw = fm.spatial[1];
    int ih2, iw2, oh2, ow2;
    get_rectified_size(param, ih2, iw2, oh2, ow2);

    int oh_block = block_helper(param.nr_threads, oh2, 0);
    megdnn_assert(oh_block != 0, "oh_block!=0");
    size_t src_size = get_perthread_cache_bytes(ic, ih2, iw2);
    size_t weight_size = group * oc * ic * fh * fw * sizeof(float);
    return {nullptr, {src_size * param.nr_threads, weight_size}};
};

static inline void copy_pad_src(float* sptr_base, const float* sptr_origin,
                                int ph, int pw, int pad_right, int ih, int iw,
                                int iw2, int pad_top, int pad_bottom, int ic,
                                int ic_stride) {
    MEGDNN_MARK_USED_VAR(ph);
    rep(ic_idx, ic) {
        const float* sptr = sptr_origin + ic_idx * ic_stride;
        memset(sptr_base, 0, sizeof(float) * iw2 * pad_top);
        sptr_base += iw2 * pad_top;
        rep(ih_idx, ih) {
            memset(sptr_base, 0, sizeof(float) * pw);
            sptr_base += pw;
            memcpy(sptr_base, sptr, sizeof(float) * iw);
            sptr_base += iw;
            sptr += iw;
            memset(sptr_base, 0, sizeof(float) * pad_right);
            sptr_base += pad_right;
        }
        memset(sptr_base, 0, sizeof(float) * iw2 * pad_bottom);
        sptr_base += iw2 * pad_bottom;
    }
}
static void pack_weight(WorkspaceBundle bundle,
                        const ConvBiasImpl::NCBKernParam& kern_param,
                        const ConvBiasImpl::NCBKernIndex& ncb_index) {
    bundle.set(kern_param.workspace_ptr);
    const int group_id = ncb_index.ndrange_id[0];
    int fh = kern_param.filter_meta.spatial[0];
    int fw = kern_param.filter_meta.spatial[1];
    int oc = kern_param.filter_meta.ocpg;
    int ic = kern_param.filter_meta.icpg;
    int oc_block = oc;
    int oc_idx = 0;
    const float* fptr =
            kern_param.filter<dt_float32>(group_id) + oc_idx * fh * fw * ic;
    auto packed_weight = reinterpret_cast<float*>(bundle.get(1)) +
                         group_id * oc * ic * fh * fw + oc_idx * ic * fh * fw;
    conv_bias::pack_weight_fp32_nchw_nchw44(fptr, packed_weight, oc_block, fh,
                                            fw, ic);
}

template <size_t filter, BiasMode bias_mode, typename Op>
static void do_conv_kern(WorkspaceBundle bundle,
                         const ConvBiasImpl::NCBKernParam& kern_param,
                         const ConvBiasImpl::NCBKernIndex& ncb_index,
                         const CpuNDRange&, const CpuNDRange&) {
    const int oh = kern_param.osz[0];
    const int ow = kern_param.osz[1];
    const int fh = kern_param.filter_meta.spatial[0];
    const int fw = kern_param.filter_meta.spatial[1];
    const int ic = kern_param.filter_meta.icpg;
    const int oc = kern_param.filter_meta.ocpg;
    const int ih = kern_param.isz[0];
    const int iw = kern_param.isz[1];
    const int stride_h = kern_param.filter_meta.stride[0];
    const int ph = kern_param.filter_meta.padding[0];
    const int pw = kern_param.filter_meta.padding[1];
    int ih2 = 0;
    int iw2 = 0;
    int oh2 = 0;
    int ow2 = 0;
    get_rectified_size(kern_param, ih2, iw2, oh2, ow2);
    bundle.set(kern_param.workspace_ptr);

    constexpr int pack_c = 4;
    const int batch_id = ncb_index.ndrange_id[0];
    const int group_id = ncb_index.ndrange_id[1];
    int oc_idx = 0;
    int oc_block = oc;
    int oh_block = block_helper(kern_param.nr_threads, oh2, 0);
    const int oh_idx = ncb_index.ndrange_id[2];
    const int oh_block_real = std::min(oh - oh_idx * oh_block, oh_block);
    const int ih_real = oh_block_real * stride_h + fh - stride_h;
    const int src_top_pad = std::max(ph - oh_idx * oh_block * stride_h, 0);
    const int src_bottom_pad = std::max(
            (oh_idx * oh_block + oh_block_real - 1) * stride_h + fh - ih - ph,
            0);
    const int remain_right_pad = std::max(iw2 - iw - pw, 0);
    const int src_offset = std::max(oh_idx * oh_block * stride_h - ph, 0) * iw;
    const float* origin_sptr = static_cast<const float*>(kern_param.src<float>(
                                       batch_id, group_id, 0, 1, 1)) +
                               src_offset;
    const size_t src_size = get_perthread_cache_bytes(ic, ih2, iw2);
    float* sptr = reinterpret_cast<float*>((int8_t*)bundle.get(0) +
                                           ncb_index.thread_id * src_size);

    copy_pad_src(sptr, origin_sptr, ph, pw, remain_right_pad,
                 ih_real - src_top_pad - src_bottom_pad, iw, iw2, src_top_pad,
                 src_bottom_pad, ic, ih * iw);
    // pack weight
    auto packed_weight = reinterpret_cast<float*>(bundle.get(1)) +
                         group_id * oc * ic * fh * fw + oc_idx * ic * fh * fw;
    // get param
    float_t* dst = kern_param.dst<float_t>(batch_id, group_id) +
                   oh_idx * oh_block * ow * pack_c;
    const float* bptr =
            kern_param.bias<dt_float32>(batch_id, group_id) + oc_idx;
    Op op;
#define KERN1_NCHW44_CONV(filter)                                             \
    conv_bias::conv_direct_stride2_##filter##x##filter##_fp32_nchw_nchw44<    \
                                                                              \
            bias_mode, Op>(sptr, packed_weight, bptr, nullptr, dst, oc_block, \
                           ic, ih_real, iw2, oh, oh_block_real, ow, op, ph,   \
                           pw)

    DISPATCH_FILTER(filter, KERN1_NCHW44_CONV);
#undef KERN1_NCHW44_CONV
}

}  // namespace

/* ===================== stride2 algo ===================== */
bool ConvBiasImpl::AlgoF32DirectStride2NCHWNCHW44::usable(
        fallback::ConvBiasImpl*, const NCBKernSizeParam& param,
        AlgoSelectionStrategy) const {
    auto&& fm = param.filter_meta;
    auto fh = fm.spatial[0];
    int oc = fm.ocpg;
    bool ok_type = ((param.src_type.enumv() == DTypeEnum::Float32 &&
                     param.filter_type.enumv() == DTypeEnum::Float32 &&
                     (param.dst_type.enumv() == DTypeEnum::Float32))) &&
                   (fm.format == param::Convolution::Format::NCHW44);
    bool ok_src_dst = fm.icpg < 4 && (oc % 4 == 0 && oc >= 4) && fm.group == 1;
    bool ok_filter = fm.spatial_ndim == 2 && fh == fm.spatial[1] &&
                     (fh == 3 || fh == 5 || fh == 7);
    bool ok_slide = fm.dilation[0] == 1 && fm.dilation[1] == 1 &&
                    fm.stride[0] == 2 && fm.stride[1] == 2;
    bool ok_conv = !fm.should_flip && param.bias_mode != BiasMode::BIAS;
    bool avaible = ok_type && ok_src_dst && ok_filter && ok_slide && ok_conv;
    return avaible;
}

size_t ConvBiasImpl::AlgoF32DirectStride2NCHWNCHW44::get_workspace(
        fallback::ConvBiasImpl*, const NCBKernSizeParam& param) const {
    return get_bundle(param).total_size_in_bytes();
}

SmallVector<ConvBiasImpl::NCBKern>
ConvBiasImpl::AlgoF32DirectStride2NCHWNCHW44::dispatch_kerns(
        fallback::ConvBiasImpl*, const NCBKernSizeParam& param) const {
    auto fm = param.filter_meta;
    const int batch = param.n;
    const int group = fm.group;
    WorkspaceBundle wbundle = get_bundle(param);
    conv_fun do_conv_fun = nullptr;
    // NOTE: remain_w is not used to gen hash of midout for compatible with
// shape runtime
#define DO_CONV_KERN_FUN(filter, bias_mode, op)                        \
    MIDOUT_BEGIN(megdnn_arm_common_conv_bias_fp32_nchw_nchw44_stride2, \
                 midout_iv(#filter #bias_mode #op##_hash)) {           \
        do_conv_fun = do_conv_kern<filter, bias_mode, op>;             \
    }                                                                  \
    MIDOUT_END();

#define GET_OP_PARAM(filter, bias_mode)                               \
    switch (param.nonlineMode) {                                      \
        case param::ConvBias::NonlineMode::IDENTITY:                  \
            DO_CONV_KERN_FUN(filter, bias_mode, NoneOp<dt_float32>)   \
            break;                                                    \
        case param::ConvBias::NonlineMode::RELU:                      \
            DO_CONV_KERN_FUN(filter, bias_mode, ReluOp<dt_float32>)   \
            break;                                                    \
        case param::ConvBias::NonlineMode::H_SWISH:                   \
            DO_CONV_KERN_FUN(filter, bias_mode, HSwishOp<dt_float32>) \
            break;                                                    \
        default:                                                      \
            megdnn_assert(0);                                         \
            break;                                                    \
    }
#define GET_BIAS_MODE_PARAM(filter)                                \
    switch (param.bias_mode) {                                     \
        case BiasMode::NO_BIAS:                                    \
            GET_OP_PARAM(filter, BiasMode::NO_BIAS)                \
            break;                                                 \
        case BiasMode::BROADCAST_CHANNEL_BIAS:                     \
            GET_OP_PARAM(filter, BiasMode::BROADCAST_CHANNEL_BIAS) \
            break;                                                 \
        default:                                                   \
            megdnn_assert(0);                                      \
            break;                                                 \
    }

#define DISPATCH_CONV_KERN()                \
    switch (param.filter_meta.spatial[0]) { \
        case 3:                             \
            GET_BIAS_MODE_PARAM(3)          \
            break;                          \
        case 5:                             \
            GET_BIAS_MODE_PARAM(5)          \
            break;                          \
        case 7:                             \
            GET_BIAS_MODE_PARAM(7)          \
            break;                          \
        default:                            \
            megdnn_assert(0);               \
            break;                          \
    }

    DISPATCH_CONV_KERN();

#undef DO_CONV_KERN_FUN
#undef GET_REMAIN_W_PARAM
#undef GET_OP_PARAM
#undef GET_BIAS_MODE_PARAM
#undef DISPATCH_CONV_KERN

    megdnn_assert(do_conv_fun);

    SmallVector<ConvBiasImpl::NCBKern> ret_kerns;
    WorkspaceBundle bundle = wbundle;
    int oh = param.osz[0];
    int oh_block = block_helper(param.nr_threads, oh, 0);
    auto do_pack_weight = [bundle](const NCBKernParam& kern_param,
                                   const NCBKernIndex& ncb_index) {
        pack_weight(bundle, kern_param, ncb_index);
    };
    ret_kerns.push_back({do_pack_weight, {static_cast<size_t>(group)}});
    CpuNDRange ncb_range = {static_cast<size_t>(batch),
                            static_cast<size_t>(group),
                            static_cast<size_t>(div_ceil(oh, oh_block))};
    auto do_conv = [bundle, do_conv_fun, ncb_range](
                           const NCBKernParam& kern_param,
                           const NCBKernIndex& ncb_index) {
        do_conv_fun(bundle, kern_param, ncb_index, ncb_index.ndrange_id,
                    ncb_range);
    };
    ret_kerns.push_back({do_conv, ncb_range});

    return ret_kerns;
}

// vim: syntax=cpp.doxygen