// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "mkldnn_roi_align_node.h"
#include <mkldnn.hpp>
#include <string>
#include <vector>
#include <math.h>
#include <mkldnn_extension_utils.h>
#include <mkldnn_types.h>
#include <utils/bfloat16.hpp>
#include <cpu/x64/cpu_isa_traits.hpp>
#include "ie_parallel.hpp"
#include <mkldnn_selective_build.h>
#include <ngraph/opsets/opset3.hpp>

#include <cpu/x64/jit_generator.hpp>
#include "emitters/jit_load_store_emitters.hpp"

using namespace MKLDNNPlugin;
using namespace InferenceEngine;
using namespace mkldnn;
using namespace mkldnn::impl;
using namespace mkldnn::impl::cpu;
using namespace mkldnn::impl::cpu::x64;
using namespace mkldnn::impl::utils;
using namespace Xbyak;

using ngPoolingMode = ngraph::op::v3::ROIAlign::PoolingMode;

#define GET_OFF(field) offsetof(jit_roi_align_call_args, field)

template <cpu_isa_t isa>
struct jit_uni_roi_align_kernel_f32 : public jit_uni_roi_align_kernel, public jit_generator {
    DECLARE_CPU_JIT_AUX_FUNCTIONS(jit_uni_roi_align_kernel_f32);

    explicit jit_uni_roi_align_kernel_f32(jit_roi_align_params jcp) : jit_uni_roi_align_kernel(jcp), jit_generator() {}

    void create_ker() override {
        jit_generator::create_kernel();
        ker_ = (decltype(ker_))jit_ker();
    };

    void generate() override {
        load_emitter.reset(new jit_load_emitter(this, isa));
        store_emitter.reset(new jit_store_emitter(this, isa));

        this->preamble();

        uni_vpxor(vmm_zero, vmm_zero, vmm_zero);

        load_pool_gpr_idxs = {static_cast<size_t>(reg_load_store_mask.getIdx()), static_cast<size_t>(reg_load_table.getIdx())};
        store_pool_gpr_idxs = {static_cast<size_t>(reg_load_store_mask.getIdx())};
        store_pool_vec_idxs = {static_cast<size_t>(vmm_zero.getIdx())};

        if (jcp_.layout == ROIAlignLayoutType::ncsp) {
            roi_align_planar();
        } else {
            roi_align_cgather();
        }

        this->postamble();

        load_emitter->emit_data();
        store_emitter->emit_data();
    }

private:
    using Vmm = typename conditional3<isa == cpu::x64::sse41, Xbyak::Xmm, isa == cpu::x64::avx2,
            Xbyak::Ymm, Xbyak::Zmm>::type;

    const int v_len = cpu_isa_traits<isa>::vlen;
    const int x_len = cpu_isa_traits<sse41>::vlen;
    const int v_step = v_len / sizeof(float);
    const int x_step = x_len / sizeof(float);

    using reg64_t = const Xbyak::Reg64;
    using reg32_t = const Xbyak::Reg32;
    using reg16_t = const Xbyak::Reg16;

    reg64_t reg_src_address = r8;
    // reg_srcx is used after abi parse finised
    reg64_t reg_src0        = r11;
    reg64_t reg_src1        = r12;
    reg64_t reg_src2        = rcx;
    reg64_t reg_src3        = rdi;
    reg64_t reg_weights     = r13;

    reg64_t reg_buf  = r9;
    reg64_t reg_src_stride  = r15;

    reg64_t reg_work_amount = r14;
    reg64_t reg_num_samples = r10;

    reg64_t reg_load_table  = rax;
    reg64_t reg_load_store_mask = rbx;

    reg64_t reg_tmp_64 = rbp;
    reg32_t reg_tmp_32 = ebp;
    reg16_t reg_tmp_16 = bp;

    // [0] for reg_buf
    // [1] for reg_dst
    Xmm xmm_args_pool = Xmm(15);

    std::unique_ptr<jit_load_emitter> load_emitter = nullptr;
    std::vector<size_t> load_pool_gpr_idxs;

    std::unique_ptr<jit_store_emitter> store_emitter = nullptr;
    std::vector<size_t> store_pool_gpr_idxs;
    std::vector<size_t> store_pool_vec_idxs;

    Vmm vmm_zero = Vmm(0);

    // assign for cgather
    Vmm vmm_src0 = Vmm(1);
    Vmm vmm_src1 = Vmm(2);
    Vmm vmm_src2 = Vmm(3);
    Vmm vmm_src3 = Vmm(4);

    Vmm vmm_weights0 = Vmm(5);
    Vmm vmm_weights1 = Vmm(6);
    Vmm vmm_weights2 = Vmm(7);
    Vmm vmm_weights3 = Vmm(8);

    Vmm vmm_sample = Vmm(9);
    Vmm vmm_buf = Vmm(10);
    Vmm vmm_scale = Vmm(11);

    // assign for planar
    reg64_t reg_src = reg_src0;
    reg64_t reg_dst = reg_src1;

    Vmm vmm_weights = vmm_weights0;
    Xmm xmm_weights = Xmm(vmm_weights.getIdx());
    Vmm vmm_src = vmm_src0;
    Xmm xmm_src = Xmm(vmm_src.getIdx());
    Xmm xmm_buf = Xmm(vmm_buf.getIdx());

    Vmm vmm_dst = vmm_src1;
    Xmm xmm_dst = Xmm(vmm_dst.getIdx());
    Vmm vmm_dst_tail = vmm_src2;
    Xmm xmm_dst_tail = Xmm(vmm_dst_tail.getIdx());

    Vmm vmm_temp1 = vmm_weights1;
    Xmm xmm_temp1 = Xmm(vmm_temp1.getIdx());
    Vmm vmm_temp2 = vmm_weights2;
    Xmm xmm_temp2 = Xmm(vmm_temp2.getIdx());

    Vmm vmm_mask = vmm_weights3;

    Opmask k_mask = Opmask(7);

    reg64_t reg_params = abi_param1;

    void roi_align_cgather() {
        mov(reg_src_address, ptr[reg_params + GET_OFF(src)]);
        mov(reg_weights, ptr[reg_params + GET_OFF(weights)]);

        mov(reg_num_samples, ptr[reg_params + GET_OFF(num_samples)]);
        mov(reg_work_amount, ptr[reg_params + GET_OFF(work_amount)]);
        if (jcp_.alg == Algorithm::ROIAlignAvg) {
            mov(reg_tmp_64, ptr[reg_params + GET_OFF(scale)]);
            uni_vbroadcastss(vmm_scale, ptr[reg_tmp_64]);
        }
        mov(reg_tmp_64, ptr[reg_params + GET_OFF(buffer)]);
        uni_vpinsrq(xmm_args_pool, xmm_args_pool, reg_tmp_64, 0);
        mov(reg_tmp_64, ptr[reg_params + GET_OFF(dst)]);
        uni_vpinsrq(xmm_args_pool, xmm_args_pool, reg_tmp_64, 1);

        if (jcp_.layout == ROIAlignLayoutType::nspc) {
            int src_stride = v_step * jcp_.data_size;
            mov(reg_src_stride, src_stride);
        } else if (jcp_.layout == ROIAlignLayoutType::blk) {
            mov(reg_src_stride, ptr[reg_params + GET_OFF(src_stride)]);
            imul(reg_src_stride, reg_src_stride, jcp_.data_size);
        }

        auto store = [&](Vmm vmm_dst, Xbyak::Reg64 reg_dst, int elt_num) {
                    store_emitter->emit_code({static_cast<size_t>(vmm_dst.getIdx())}, {static_cast<size_t>(reg_dst.getIdx())},
                    std::make_shared<store_emitter_context>(Precision::FP32, jcp_.data_prc, elt_num),
                    {store_pool_vec_idxs}, {store_pool_gpr_idxs});
        };

        auto load_buf = [&](Xbyak::Reg64 reg_src, Vmm vmm_src, int elt_num) {
                    load_emitter->emit_code({static_cast<size_t>(reg_src.getIdx())}, {static_cast<size_t>(vmm_src.getIdx())},
                    std::make_shared<load_emitter_context>(Precision::FP32, Precision::FP32, elt_num),
                    {}, {load_pool_gpr_idxs});
        };
        auto store_buf = [&](Vmm vmm_dst, Xbyak::Reg64 reg_dst, int elt_num) {
                    store_emitter->emit_code({static_cast<size_t>(vmm_dst.getIdx())}, {static_cast<size_t>(reg_dst.getIdx())},
                    std::make_shared<store_emitter_context>(Precision::FP32, Precision::FP32, elt_num),
                    {store_pool_vec_idxs}, {store_pool_gpr_idxs});
        };

        // out loop for samples in bin
        Xbyak::Label out_loop_label;
        Xbyak::Label out_loop_end_label;

        L(out_loop_label);
        {
            cmp(reg_num_samples, 1);
            jl(out_loop_end_label, T_NEAR);

            // get 4 src address and 4 vmm_weights
            get_src();
            get_weights();

            // inner loop for channels of one sample
            Xbyak::Label in_loop_main_label;
            Xbyak::Label in_loop_main_end_label;
            Xbyak::Label in_loop_tail_label;
            Xbyak::Label in_loop_tail_end_label;

            uni_vpextrq(reg_buf, xmm_args_pool, 0);
            // do not spoil reg_work_amount(channels), which is needed in store rountine
            mov(reg_tmp_64, reg_work_amount);

            L(in_loop_main_label);
            {
                cmp(reg_tmp_64, v_step);
                jl(in_loop_main_end_label, T_NEAR);

                generate_samples(v_step);
                // now this sample value across channel reside in vmm_sample
                // compute with other samples in vmm_buf
                load_buf(reg_buf, vmm_buf, v_step);
                if (jcp_.alg == Algorithm::ROIAlignAvg) {
                    uni_vaddps(vmm_buf, vmm_buf, vmm_sample);
                } else {
                    uni_vmaxps(vmm_buf, vmm_buf, vmm_sample);
                }
                store_buf(vmm_buf, reg_buf, v_step);

                if ((isa == cpu::x64::sse41) && (jcp_.layout == ROIAlignLayoutType::blk)) {
                    add(reg_src0, x_step * jcp_.data_size);
                    add(reg_src1, x_step * jcp_.data_size);
                    add(reg_src2, x_step * jcp_.data_size);
                    add(reg_src3, x_step * jcp_.data_size);
                    add(reg_buf, x_step * sizeof(float));

                    generate_samples(x_step);
                    load_buf(reg_buf, vmm_buf, x_step);
                    if (jcp_.alg == Algorithm::ROIAlignAvg) {
                        uni_vaddps(vmm_buf, vmm_buf, vmm_sample);
                    } else {
                        uni_vmaxps(vmm_buf, vmm_buf, vmm_sample);
                    }
                    store_buf(vmm_buf, reg_buf, x_step);

                    sub(reg_src0, x_step * jcp_.data_size);
                    sub(reg_src1, x_step * jcp_.data_size);
                    sub(reg_src2, x_step * jcp_.data_size);
                    sub(reg_src3, x_step * jcp_.data_size);
                    // reg_buf no need reset back, buf is continious

                    sub(reg_tmp_64, x_step);
                }

                add(reg_src0, reg_src_stride);
                add(reg_src1, reg_src_stride);
                add(reg_src2, reg_src_stride);
                add(reg_src3, reg_src_stride);
                add(reg_buf, v_step * sizeof(float));

                sub(reg_tmp_64, v_step);

                jmp(in_loop_main_label, T_NEAR);
            }
            L(in_loop_main_end_label);

            int tail_step = 1;
            L(in_loop_tail_label);
            {
                cmp(reg_tmp_64, tail_step);
                jl(in_loop_tail_end_label, T_NEAR);

                generate_samples(tail_step);
                load_buf(reg_buf, vmm_buf, tail_step);
                if (jcp_.alg == Algorithm::ROIAlignAvg) {
                    uni_vaddps(vmm_buf, vmm_buf, vmm_sample);
                } else {
                    uni_vmaxps(vmm_buf, vmm_buf, vmm_sample);
                }
                store_buf(vmm_buf, reg_buf, tail_step);

                int tail_src_stride = tail_step * jcp_.data_size;
                add(reg_src0, tail_src_stride);
                add(reg_src1, tail_src_stride);
                add(reg_src2, tail_src_stride);
                add(reg_src3, tail_src_stride);
                add(reg_buf, tail_step * sizeof(float));

                sub(reg_tmp_64, tail_step);

                jmp(in_loop_tail_label, T_NEAR);
            }
            L(in_loop_tail_end_label);

            sub(reg_num_samples, 1);

            jmp(out_loop_label, T_NEAR);
        }
        L(out_loop_end_label);

        // store
        Xbyak::Label store_loop_main_label;
        Xbyak::Label store_loop_main_end_label;
        Xbyak::Label store_loop_tail_label;
        Xbyak::Label store_loop_tail_end_label;

        // EOL for reg_src0-reg_src3
        reg64_t reg_dst = reg_src0;
        uni_vpextrq(reg_dst, xmm_args_pool, 1);
        uni_vpextrq(reg_buf, xmm_args_pool, 0);

        reg64_t reg_dst_stride = reg_src1;
        if (jcp_.layout == ROIAlignLayoutType::nspc) {
            int dst_stride = v_step * jcp_.data_size;
            mov(reg_dst_stride, dst_stride);
        } else if (jcp_.layout == ROIAlignLayoutType::blk) {
            int blk_size = (isa == cpu::x64::sse41) ? v_step * 2 : v_step;
            int dst_stride = blk_size * jcp_.pooled_h * jcp_.pooled_w * jcp_.data_size;
            mov(reg_dst_stride, dst_stride);
        }

        L(store_loop_main_label);
        {
            cmp(reg_work_amount, v_step);
            jl(store_loop_main_end_label, T_NEAR);

            load_buf(reg_buf, vmm_buf, v_step);
            if (jcp_.alg == Algorithm::ROIAlignAvg) {
                uni_vmulps(vmm_buf, vmm_buf, vmm_scale);
            }
            store(vmm_buf, reg_dst, v_step);

            if ((isa == cpu::x64::sse41) && (jcp_.layout == ROIAlignLayoutType::blk)) {
                add(reg_buf, x_step * sizeof(float));
                add(reg_dst, x_step * jcp_.data_size);

                load_buf(reg_buf, vmm_buf, x_step);
                if (jcp_.alg == Algorithm::ROIAlignAvg) {
                    uni_vmulps(vmm_buf, vmm_buf, vmm_scale);
                }
                store(vmm_buf, reg_dst, x_step);

                sub(reg_dst, x_step * jcp_.data_size);

                sub(reg_work_amount, x_step);
            }

            add(reg_buf, v_step * sizeof(float));
            add(reg_dst, reg_dst_stride);

            sub(reg_work_amount, v_step);

            jmp(store_loop_main_label, T_NEAR);
        }
        L(store_loop_main_end_label);

        int tail_step = 1;
        L(store_loop_tail_label);
        {
            cmp(reg_work_amount, tail_step);
            jl(store_loop_tail_end_label, T_NEAR);

            load_buf(reg_buf, vmm_buf, tail_step);
            if (jcp_.alg == Algorithm::ROIAlignAvg) {
                uni_vmulps(vmm_buf, vmm_buf, vmm_scale);
            }
            store(vmm_buf, reg_dst, tail_step);

            add(reg_buf, tail_step * sizeof(float));
            add(reg_dst, tail_step * jcp_.data_size);

            sub(reg_work_amount, tail_step);

            jmp(store_loop_tail_label, T_NEAR);
        }
        L(store_loop_tail_end_label);
    }

    void get_src() {
        mov(reg_src0, ptr[reg_src_address + 0 * sizeof(void*)]);
        mov(reg_src1, ptr[reg_src_address + 1 * sizeof(void*)]);
        mov(reg_src2, ptr[reg_src_address + 2 * sizeof(void*)]);
        mov(reg_src3, ptr[reg_src_address + 3 * sizeof(void*)]);
        add(reg_src_address, 4 * sizeof(void*));
    }

    void get_weights() {
        uni_vbroadcastss(vmm_weights0, ptr[reg_weights + 0 * sizeof(float)]);
        uni_vbroadcastss(vmm_weights1, ptr[reg_weights + 1 * sizeof(float)]);
        uni_vbroadcastss(vmm_weights2, ptr[reg_weights + 2 * sizeof(float)]);
        uni_vbroadcastss(vmm_weights3, ptr[reg_weights + 3 * sizeof(float)]);
        add(reg_weights, 4 * sizeof(float));
    }

    void generate_samples(int num) {
        auto load = [&](Xbyak::Reg64 reg_src, Vmm vmm_src, int elt_num) {
                    load_emitter->emit_code({static_cast<size_t>(reg_src.getIdx())}, {static_cast<size_t>(vmm_src.getIdx())},
                    std::make_shared<load_emitter_context>(jcp_.data_prc, Precision::FP32, elt_num),
                    {}, {load_pool_gpr_idxs});
        };

        uni_vpxor(vmm_sample, vmm_sample, vmm_sample);
        load(reg_src0, vmm_src0, num);
        uni_vfmadd231ps(vmm_sample, vmm_src0, vmm_weights0);
        load(reg_src1, vmm_src1, num);
        uni_vfmadd231ps(vmm_sample, vmm_src1, vmm_weights1);
        load(reg_src2, vmm_src2, num);
        uni_vfmadd231ps(vmm_sample, vmm_src2, vmm_weights2);
        load(reg_src3, vmm_src3, num);
        uni_vfmadd231ps(vmm_sample, vmm_src3, vmm_weights3);
    }

    void roi_align_planar() {
        mov(reg_src, ptr[this->reg_params + GET_OFF(src)]);
        mov(reg_buf, ptr[this->reg_params + GET_OFF(buffer)]);
        mov(reg_weights, ptr[this->reg_params + GET_OFF(weights)]);

        mov(reg_dst, ptr[this->reg_params + GET_OFF(dst)]);
        mov(reg_num_samples, ptr[reg_params + GET_OFF(num_samples)]);

        if (jcp_.alg == Algorithm::ROIAlignAvg) {
            mov(reg_tmp_64, ptr[reg_params + GET_OFF(scale)]);
            uni_vbroadcastss(vmm_scale, ptr[reg_tmp_64]);
        }

        auto load_idx = [&](Xbyak::Reg64 reg_idx, Vmm vmm_idx, int elt_num) {
                    load_emitter->emit_code({static_cast<size_t>(reg_idx.getIdx())}, {static_cast<size_t>(vmm_idx.getIdx())},
                    std::make_shared<load_emitter_context>(Precision::I32, Precision::I32, elt_num),
                    {}, {load_pool_gpr_idxs});
        };

        Xbyak::Label main_loop_label;
        Xbyak::Label main_loop_end_label;
        Xbyak::Label tail_loop_label;
        Xbyak::Label tail_loop_end_label;

        int lane = v_len / cpu_isa_traits<sse41>::vlen;
        uni_vpxor(vmm_dst, vmm_dst, vmm_dst);
        L(main_loop_label);
        {
            cmp(reg_num_samples, lane);
            jl(main_loop_end_label, T_NEAR);

            load_idx(reg_buf, vmm_buf, v_step);

            if (jcp_.data_prc == Precision::FP32)
                gather_f32(vmm_src, reg_src, vmm_buf);
            else if (jcp_.data_prc == Precision::BF16)
                gather_bf16_to_f32_zmm(vmm_src, reg_src, vmm_buf);

            uni_vmovups(vmm_weights, ptr[reg_weights]);

            if (jcp_.alg == Algorithm::ROIAlignAvg) {
                uni_vfmadd231ps(vmm_dst, vmm_src, vmm_weights);
            } else {
                uni_vmulps(vmm_src, vmm_src, vmm_weights);
                // horizontal add for each lane
                // xmm_dst[0] hold the max
                if (isa == cpu::x64::avx512_common) {
                    for (int i = 0; i < lane; i++) {
                        vextractf32x4(xmm_temp1, Xbyak::Zmm(vmm_src.getIdx()), i);
                        horizontal_add_xmm(xmm_temp1, xmm_temp2);
                        uni_vmaxps(xmm_dst, xmm_dst, xmm_temp1);
                    }
                } else if (isa == cpu::x64::avx2) {
                    for (int i = 0; i < lane; i++) {
                        vextractf128(xmm_temp1, Xbyak::Ymm(vmm_src.getIdx()), i);
                        horizontal_add_xmm(xmm_temp1, xmm_temp2);
                        uni_vmaxps(xmm_dst, xmm_dst, xmm_temp1);
                    }
                } else {
                    horizontal_add_xmm(xmm_src, xmm_temp2);
                    uni_vmaxps(xmm_dst, xmm_dst, xmm_src);
                }
            }

            add(reg_buf, v_len);
            add(reg_weights, v_len);
            sub(reg_num_samples, lane);

            jmp(main_loop_label, T_NEAR);
        }
        L(main_loop_end_label);

        if (jcp_.alg == Algorithm::ROIAlignAvg)
            uni_vpxor(vmm_dst_tail, vmm_dst_tail, vmm_dst_tail);

        lane = 1;
        L(tail_loop_label);
        {
            cmp(reg_num_samples, 1);
            jl(tail_loop_end_label, T_NEAR);

            load_idx(reg_buf, vmm_buf, x_step);

            if (jcp_.data_prc == Precision::FP32)
                gather_f32_xmm(xmm_src, reg_src, xmm_buf);
            else if (jcp_.data_prc == Precision::BF16)
                gather_bf16_to_f32_xmm(xmm_src, reg_src, xmm_buf);

            uni_vmovups(xmm_weights, ptr[reg_weights]);
            if (jcp_.alg == Algorithm::ROIAlignAvg) {
                // as vex instruction will zero upper bit for xmm version, store result in seperate xmm_dst_tail
                uni_vfmadd231ps(xmm_dst_tail, xmm_src, xmm_weights);
            } else {
                uni_vmulps(xmm_src, xmm_src, xmm_weights);
                horizontal_add_xmm(xmm_src, xmm_temp2);
                uni_vmaxps(xmm_dst, xmm_dst, xmm_src);
            }

            add(reg_buf, x_len);
            add(reg_weights, x_len);
            sub(reg_num_samples, lane);

            jmp(tail_loop_label, T_NEAR);
        }
        L(tail_loop_end_label);

        if (jcp_.alg == Algorithm::ROIAlignAvg) {
            uni_vaddps(vmm_dst, vmm_dst, vmm_dst_tail);
            horizontal_add();  // xmm_dst[0] is the dst value
            uni_vmulps(vmm_dst, vmm_dst, vmm_scale);
        }

        // xmm_dst[0] of f32 is the dst value
        if (jcp_.data_prc == Precision::FP32)
            uni_vpextrd(ptr[reg_dst], xmm_dst, 0);
        else if (jcp_.data_prc == Precision::BF16)
            uni_vpextrw(ptr[reg_dst], xmm_dst, 1);
    }

    // gather f32 data from reg_src with vmm_idx(data_size) to vmm_src with f32 precision
    inline void gather_f32(Vmm &vmm_src, const reg64_t &reg_src, const Vmm &vmm_idx) {
        constexpr bool is_ymm = std::is_same<Vmm, Xbyak::Ymm>::value;
        constexpr bool is_zmm = std::is_same<Vmm, Xbyak::Zmm>::value;

        if (is_zmm) {
            kxnord(k_mask, k_mask, k_mask);
            vgatherdps(vmm_src | k_mask, ptr[reg_src + vmm_idx * jcp_.data_size]);
        } else if (is_ymm) {
            uni_vpcmpeqd(vmm_mask, vmm_mask, vmm_mask);
            vgatherdps(vmm_src, ptr[reg_src + vmm_idx * jcp_.data_size], vmm_mask);
        } else {
            gather_f32_xmm(Xbyak::Xmm(vmm_src.getIdx()), reg_src, Xbyak::Xmm(vmm_idx.getIdx()));
        }
    }

    inline void gather_f32_xmm(Xbyak::Xmm xmm_src, const reg64_t reg_src, const Xbyak::Xmm xmm_idx) {
        sub(rsp, x_len);
        uni_vmovdqu(ptr[rsp], xmm_idx);
        for (int i = 0; i < x_step; i++) {
            mov(reg_tmp_32, ptr[rsp + i * sizeof(int)]);       // sizeof(int)  index_size
            mov(reg_tmp_32, ptr[reg_src + reg_tmp_64 * jcp_.data_size]);  // scale: sizeof(float)   value_size
            mov(ptr[rsp + i * sizeof(int)], reg_tmp_32);
        }
        uni_vmovups(xmm_src, ptr[rsp]);
        add(rsp, x_len);
    }

    // gather bf16 data from reg_src with vmm_idx(data_size) to vmm_src with f32 precision
    // bf16 is needed from avx512_core
    inline void gather_bf16_to_f32_zmm(Vmm vmm_src, const reg64_t reg_src, const Vmm vmm_idx) {
        if (!std::is_same<Vmm, Xbyak::Zmm>::value)
            IE_THROW() << "bf16 is only supported from avx512_core platform for ROIAlign node.";
        sub(rsp, v_len);
        uni_vmovdqu(ptr[rsp], vmm_idx);
        for (int i = 0; i < v_step; i++) {
            mov(reg_tmp_32, ptr[rsp + i * sizeof(int)]);       // sizeof(int)  index_size
            mov(reg_tmp_16, word[reg_src + reg_tmp_64 * jcp_.data_size]);  // scale: sizeof(bf16)   value_size
            mov(ptr[rsp + i * sizeof(int)], reg_tmp_16);
        }
        uni_vmovups(vmm_src, ptr[rsp]);    // |_ x|_ x|_ x|_ x|
        uni_vpslld(vmm_src, vmm_src, 16);  // |x 0|x 0|x 0|x 0|

        add(rsp, v_len);
    }

    inline void gather_bf16_to_f32_xmm(Xbyak::Xmm xmm_src, const reg64_t reg_src, const Xbyak::Xmm xmm_idx) {
        sub(rsp, x_len);
        uni_vmovdqu(ptr[rsp], xmm_idx);
        for (int i = 0; i < x_step; i++) {
            mov(reg_tmp_32, ptr[rsp + i * sizeof(int)]);
            mov(reg_tmp_16, ptr[reg_src + reg_tmp_64 * jcp_.data_size]);
            mov(ptr[rsp + i * sizeof(int)], reg_tmp_16);
        }
        uni_vmovups(xmm_src, ptr[rsp]);    // |_ x|_ x|_ x|_ x|
        uni_vpslld(xmm_src, xmm_src, 16);  // |x 0|x 0|x 0|x 0|

        add(rsp, x_len);
    }

    inline void horizontal_add_xmm(const Xbyak::Xmm &xmm_dst, const Xbyak::Xmm &xmm_aux) {
        uni_vmovshdup(xmm_aux, xmm_dst);              //  dst:1,2,3,4; aux:2,2,4,4
        uni_vaddps(xmm_dst, xmm_dst, xmm_aux);        //  dst:1+2,2+2,3+4,4+4
        uni_vmovhlps(xmm_aux, xmm_aux, xmm_dst);      //  aux:3+4,4+4,4,4
        uni_vaddps(xmm_dst, xmm_dst, xmm_aux);        //  dst:1+2+3+4,...
    }

    // horizontal add for vmm_dst, temp1 and temp2 as aux
    inline void horizontal_add() {
        Xbyak::Xmm xmm_dst = Xbyak::Xmm(vmm_dst.getIdx());
        Xbyak::Xmm xmm_temp1 = Xbyak::Xmm(vmm_temp1.getIdx());
        Xbyak::Xmm xmm_temp2 = Xbyak::Xmm(vmm_temp2.getIdx());
        if (isa == cpu::x64::sse41) {
            horizontal_add_xmm(xmm_dst, xmm_temp1);
        } else if (isa == cpu::x64::avx2) {
            Xbyak::Ymm ymm_dst = Xbyak::Ymm(vmm_dst.getIdx());
            vextractf128(xmm_temp1, ymm_dst, 0);
            vextractf128(xmm_temp2, ymm_dst, 1);
            uni_vaddps(xmm_dst, xmm_temp1, xmm_temp2);
            horizontal_add_xmm(xmm_dst, xmm_temp1);
        } else {
            Xbyak::Zmm zmm_dst = Xbyak::Zmm(vmm_dst.getIdx());
            vextractf32x4(xmm_temp1, zmm_dst, 0);
            vextractf32x4(xmm_temp2, zmm_dst, 1);
            uni_vaddps(xmm_temp1, xmm_temp1, xmm_temp2);
            vextractf32x4(xmm_temp2, zmm_dst, 2);
            vextractf32x4(xmm_dst, zmm_dst, 3);
            uni_vaddps(xmm_dst, xmm_dst, xmm_temp2);
            uni_vaddps(xmm_dst, xmm_dst, xmm_temp1);
            horizontal_add_xmm(xmm_dst, xmm_temp1);
        }
    }
};

bool MKLDNNROIAlignNode::isSupportedOperation(const std::shared_ptr<const ngraph::Node>& op, std::string& errorMessage) noexcept {
    try {
        auto roiAlign = ngraph::as_type_ptr<const ngraph::opset3::ROIAlign>(op);
        if (!roiAlign) {
            errorMessage = "Only opset3 ROIAlign operation is supported";
            return false;
        }

        const ngPoolingMode mode = roiAlign->get_mode();
        if (mode != ngPoolingMode::AVG && mode != ngPoolingMode::MAX) {
            errorMessage = "Doesn't support mode: " + ngraph::as_string(mode);
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

MKLDNNROIAlignNode::MKLDNNROIAlignNode(const std::shared_ptr<ngraph::Node>& op, const mkldnn::engine& eng,
                                       MKLDNNWeightsSharing::Ptr &cache) : MKLDNNNode(op, eng, cache) {
    std::string errorMessage;
    if (isSupportedOperation(op, errorMessage)) {
        errorPrefix = "ROIPooling layer with name '" + getName() + "' ";

        auto roiAlign = ngraph::as_type_ptr<const ngraph::opset3::ROIAlign>(op);
        pooledH = roiAlign->get_pooled_h();
        pooledW = roiAlign->get_pooled_w();
        spatialScale = roiAlign->get_spatial_scale();
        samplingRatio = roiAlign->get_sampling_ratio();
        const ngPoolingMode m = roiAlign->get_mode();
        if (m == ngPoolingMode::MAX) {
            algorithm = Algorithm::ROIAlignMax;
        } else if (m == ngPoolingMode::AVG) {
            algorithm = Algorithm::ROIAlignAvg;
        }
    } else {
        IE_THROW(NotImplemented) << errorMessage;
    }
}

void MKLDNNROIAlignNode::getSupportedDescriptors() {
    if (!descs.empty())
        return;

    if (getParentEdges().size() != 3)
        IE_THROW() << errorPrefix << "has incorrect number of input edges: " << getParentEdges().size();
    if (getChildEdges().empty())
        IE_THROW() << errorPrefix << "has incorrect number of output edges: " << getChildEdges().size();

    if (getInputShapeAtPort(0).getRank() != 4) {
        IE_THROW() << errorPrefix << "doesn't support 0th input with rank: " << getInputShapeAtPort(0).getRank();
    }

    if (getInputShapeAtPort(1).getRank() != 2) {
        IE_THROW() << errorPrefix << "doesn't support 1st input with rank: " << getInputShapeAtPort(1).getRank();
    }

    if (getInputShapeAtPort(2).getRank() != 1) {
        IE_THROW() << errorPrefix << "doesn't support 2nd input with rank: " << getInputShapeAtPort(2).getRank();
    }

    if (getOutputShapeAtPort(0).getRank() != 4) {
        IE_THROW() << errorPrefix << "doesn't support output with rank: " << getOutputShapeAtPort(0).getRank();
    }

    const auto& proposalsDims = getInputShapeAtPort(1).getDims();
    if (proposalsDims[1] != 4) {
        IE_THROW() << errorPrefix << "has invalid shape on 1st input: [" << proposalsDims[0] << "," << proposalsDims[1] << "]";
    }

    const auto& indexesDims = getInputShapeAtPort(2).getDims();
    if (!dimsEqualWeak(proposalsDims[0], indexesDims[0])) {
        IE_THROW() << errorPrefix << "has different sizes of inputs for proposals ("
                   << proposalsDims[0] << ") and indexes (" << indexesDims[0] << ")";
    }
}

void MKLDNNROIAlignNode::createJitKernel(const InferenceEngine::Precision& dataPrec, const ROIAlignLayoutType& selectLayout) {
    auto jcp = jit_roi_align_params();
    jcp.alg = algorithm;
    jcp.data_prc = dataPrec;
    jcp.data_size = dataPrec.size();
    jcp.layout = selectLayout;
    jcp.pooled_h = pooledH;
    jcp.pooled_w = pooledW;

    if (mayiuse(cpu::x64::avx512_common)) {
        roi_align_kernel.reset(new jit_uni_roi_align_kernel_f32<cpu::x64::avx512_common>(jcp));
    } else if (mayiuse(cpu::x64::avx2)) {
        roi_align_kernel.reset(new jit_uni_roi_align_kernel_f32<cpu::x64::avx2>(jcp));
    } else if (mayiuse(cpu::x64::sse41)) {
        roi_align_kernel.reset(new jit_uni_roi_align_kernel_f32<cpu::x64::sse41>(jcp));
    }

    if (roi_align_kernel)
        roi_align_kernel->create_ker();
}

void MKLDNNROIAlignNode::initSupportedPrimitiveDescriptors() {
    if (!supportedPrimitiveDescriptors.empty())
        return;

    Precision inputPrec0 = getOriginalInputPrecisionAtPort(0);
    Precision outputPrec = getOriginalOutputPrecisionAtPort(0);

    if (inputPrec0 != Precision::FP32 || outputPrec != Precision::FP32) {
        if ((outputPrec == Precision::BF16 || inputPrec0 == Precision::BF16) && mayiuse(avx512_core)) {
            outputPrec = inputPrec0 = Precision::BF16;
        } else {
            outputPrec = inputPrec0 = Precision::FP32;
        }
    }

    NodeConfig config;
    config.dynBatchSupport = false;
    config.inConfs.resize(3);
    config.outConfs.resize(1);

    impl_desc_type impl_type;
    if (mayiuse(cpu::x64::avx512_common)) {
        impl_type = impl_desc_type::jit_avx512;
    } else if (mayiuse(cpu::x64::avx2)) {
        impl_type = impl_desc_type::jit_avx2;
    } else if (mayiuse(cpu::x64::sse41)) {
        impl_type = impl_desc_type::jit_sse42;
    } else {
        impl_type = impl_desc_type::ref;
    }

    std::vector<std::pair<LayoutType, LayoutType>> supportedFormats {
            {LayoutType::ncsp, LayoutType::ncsp}
    };

    if (mayiuse(cpu::x64::sse41)) {
        supportedFormats.push_back(std::make_pair(LayoutType::nspc, LayoutType::nspc));
        if (impl_desc_type::jit_avx512 == impl_type) {
            supportedFormats.push_back(std::make_pair(LayoutType::nCsp16c, LayoutType::nCsp16c));
        } else {
            supportedFormats.push_back(std::make_pair(LayoutType::nCsp8c, LayoutType::nCsp8c));
        }
    }

    for (auto fmts : supportedFormats) {
        addSupportedPrimDesc({{fmts.first, inputPrec0},
                              {LayoutType::ncsp, Precision::FP32},
                              {LayoutType::ncsp, Precision::I32}},
                             {{fmts.second, outputPrec}},
                              impl_type);
    }
}

void MKLDNNROIAlignNode::createPrimitive() {
    auto& srcMemPtr = getParentEdgeAt(0)->getMemoryPtr();
    auto& dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
    if (!srcMemPtr || !srcMemPtr->isAllocated())
        IE_THROW() << errorPrefix << " did not allocate input memory";
    if (!dstMemPtr || !dstMemPtr->isAllocated())
        IE_THROW() << errorPrefix << " did not allocate destination memory";

    if (!roi_align_kernel) {
        ROIAlignLayoutType selectedLayout = ROIAlignLayoutType::nspc;

        if (srcMemPtr->getDesc().hasLayoutType(LayoutType::ncsp)) {
            selectedLayout = ROIAlignLayoutType::ncsp;
        } else if (srcMemPtr->getDesc().hasLayoutType(LayoutType::nCsp8c) ||
                   srcMemPtr->getDesc().hasLayoutType(LayoutType::nCsp16c)) {
            selectedLayout = ROIAlignLayoutType::blk;
        }
        createJitKernel(srcMemPtr->getDesc().getPrecision(), selectedLayout);
    }
}

namespace {
struct ROIAlignContext {
    MKLDNNROIAlignNode &node;
};
}

template<typename T>
struct MKLDNNROIAlignNode::ROIAlignExecute {
    using srcT = typename std::tuple_element<0, T>::type;
    using dstT = typename std::tuple_element<1, T>::type;

    void operator()(ROIAlignContext & ctx) {
        ctx.node.executeSpecified<srcT, dstT>();
    }
};
void MKLDNNROIAlignNode::execute(mkldnn::stream strm) {
    auto inputPrec = getParentEdgeAt(0)->getMemory().GetDataType();
    auto outputPrec = getChildEdgeAt(0)->getMemory().GetDataType();
    if (!((inputPrec == mkldnn_bf16 && outputPrec == mkldnn_bf16) ||
          (inputPrec == mkldnn_f32 && outputPrec == mkldnn_f32)))
        IE_THROW() <<"ROIAlign doesn't support demanded precisions";

    ROIAlignContext ctx = {
            *this
    };

    OV_SWITCH(MKLDNNPlugin, ROIAlignExecute, ctx, std::tie(inputPrec, outputPrec),
              OV_CASE2(mkldnn_f32, mkldnn_f32, float, float),
              OV_CASE2(mkldnn_bf16, mkldnn_bf16, bfloat16_t, bfloat16_t))
}

template <typename inputType, typename outputType>
void MKLDNNROIAlignNode::executeSpecified() {
    auto &srcMemory0 = getParentEdgeAt(0)->getMemory();
    auto &srcMemory1 = getParentEdgeAt(1)->getMemory();
    auto &dstMemory = getChildEdgeAt(0)->getMemory();

    auto srcBlockDesc = srcMemory0.GetDescWithType<BlockedMemoryDesc>();
    auto dstBlockDesc = dstMemory.GetDescWithType<BlockedMemoryDesc>();

    auto isPlainFmt = srcBlockDesc->hasLayoutType(LayoutType::ncsp);

    const auto *srcData = reinterpret_cast<const inputType *>(getParentEdgeAt(0)->getMemoryPtr()->GetPtr());
    const auto *srcRoi = reinterpret_cast<const float *>(getParentEdgeAt(1)->getMemoryPtr()->GetPtr());
    const auto *srcRoiIdx = reinterpret_cast<const int *>(getParentEdgeAt(2)->getMemoryPtr()->GetPtr());
    auto *dst = reinterpret_cast<outputType *>(getChildEdgeAt(0)->getMemoryPtr()->GetPtr());

    auto nominalRoiCount = static_cast<int>(srcMemory1.getStaticDims()[0]);
    int realRois = 0;
    auto inputDimVector = srcMemory0.getStaticDims();
    const int C = static_cast<int>(inputDimVector[1]);
    const int H = static_cast<int>(inputDimVector[2]);
    const int W = static_cast<int>(inputDimVector[3]);

    const int binCount = pooledH * pooledW;

    const auto &srcStrides = srcBlockDesc->getStrides();
    const auto &dstStrides = dstBlockDesc->getStrides();

    const int batchInputStride = srcStrides[0];
    const int batchOutputStride = dstStrides[0];
    const int lastBlockDim = srcBlockDesc->getBlockDims().back();
    // bilinear interpolate parameters number
    const int BLIParamsNum = 4;

    for (; realRois < nominalRoiCount; realRois++) {
        auto roiBatchInd = srcRoiIdx[realRois];
        if (roiBatchInd == -1) {
            break;
        }
    }

    std::vector<int> numSamples(realRois);
    std::vector<std::vector<float>> weightsTbl(realRois);
    std::vector<std::vector<size_t>> srcAddressListTbl;
    std::vector<std::vector<int>> srcIndexTbl;
    if (!isPlainFmt)
        srcAddressListTbl.resize(realRois);
    else
        srcIndexTbl.resize(realRois);

    parallel_for(realRois, [&](size_t n) {
        int roiOff = n * 4;
        const float* srcRoiPtr = &srcRoi[roiOff];
        int roiBatchInd = srcRoiIdx[n];
        if (roiBatchInd < -1) {  // -1 means switched off region
            IE_THROW() << "Batch index cannot be less, than -1";
        } else if (roiBatchInd >= inputDimVector[0]) {
            IE_THROW() << "Demanded batch (id = " << roiBatchInd << ") doesn't exist";
        }

        float x1 = srcRoiPtr[0] * spatialScale;
        float y1 = srcRoiPtr[1] * spatialScale;
        float x2 = srcRoiPtr[2] * spatialScale;
        float y2 = srcRoiPtr[3] * spatialScale;

        float roiHeight = std::max(y2 - y1, 1.0f);
        float roiWidth = std::max(x2 - x1, 1.0f);
        float binHeight = roiHeight / pooledH;
        float binWidth = roiWidth / pooledW;

        auto samplingRatioX = samplingRatio == 0 ? static_cast<int>(ceil(binWidth)) : samplingRatio;
        auto samplingRatioY = samplingRatio == 0 ? static_cast<int>(ceil(binHeight)) : samplingRatio;

        uint64_t numSamplesInBin = static_cast<uint64_t>(samplingRatioX) * samplingRatioY;
        numSamples[n] = numSamplesInBin;

        float sampleDistanceX = binWidth / samplingRatioX;
        float sampleDistanceY = binHeight / samplingRatioY;
        // prepare arrays for sampling points and weights
        size_t paramsSize = BLIParamsNum * numSamplesInBin * binCount;
        weightsTbl[n] = std::vector<float>(paramsSize, 0.f);
        if (!isPlainFmt)
            srcAddressListTbl[n] = std::vector<size_t>(paramsSize, 0);
        else
            srcIndexTbl[n] = std::vector<int>(paramsSize, 0);

        size_t batchSrcOffset = roiBatchInd * batchInputStride;
        int idxIter = 0;

        // |__|__|     |     |
        // |__|__|__ __|__ __|
        // |     | bin |     |
        // |__ __|__ __|__ __|
        // |     |     |     |
        // |__ __|__ __|__ __|
        for (int yBinInd = 0; yBinInd < pooledH; ++yBinInd) {
            for (int xBinInd = 0; xBinInd < pooledW; ++xBinInd) {
                // run into bin
                for (int ySampleInd = 0; ySampleInd < samplingRatioY; ySampleInd++) {
                    float sampleY = y1 + yBinInd * binHeight + sampleDistanceY * (0.5f + ySampleInd);
                    for (int xSampleInd = 0; xSampleInd < samplingRatioX; xSampleInd++) {
                        float sampleX = x1 + xBinInd * binWidth + sampleDistanceX * (0.5f + xSampleInd);
                        if (sampleX < -1.0 || sampleX > W ||
                            sampleY < -1.0 || sampleY > H) {
                            // For this sample we save 4 index of (0,0) and 4 weight of 0
                            if (!isPlainFmt) {
                                auto startPoint = reinterpret_cast<size_t>(&srcData[batchSrcOffset]);
                                for (int i = 0; i < BLIParamsNum; i++)
                                    srcAddressListTbl[n][idxIter + i] = startPoint;
                            } else {
                                for (int i = 0; i < BLIParamsNum; i++)
                                    srcIndexTbl[n][idxIter + i] = 0;
                            }
                            for (int i = 0; i < BLIParamsNum; i++)
                                weightsTbl[n][idxIter + i] = 0.f;
                            idxIter += BLIParamsNum;
                            continue;
                        }
                        sampleX = std::max(sampleX, float{0});
                        sampleY = std::max(sampleY, float{0});

                        auto sampleYLow = static_cast<unsigned int>(sampleY);
                        auto sampleXLow = static_cast<unsigned int>(sampleX);
                        unsigned int sampleYHigh;
                        unsigned int sampleXHigh;
                        if (sampleYLow >= H - 1) {
                            sampleYHigh = sampleYLow = H - 1;
                            sampleY = static_cast<float>(sampleYLow);
                        } else {
                            sampleYHigh = sampleYLow + 1;
                        }
                        if (sampleXLow >= W - 1) {
                            sampleXHigh = sampleXLow = W - 1;
                            sampleX = static_cast<float>(sampleXLow);
                        } else {
                            sampleXHigh = sampleXLow + 1;
                        }

                        if (!isPlainFmt) {
                            size_t srcOffset = batchSrcOffset + sampleYLow * W * lastBlockDim + sampleXLow * lastBlockDim;
                            srcAddressListTbl[n][idxIter] = reinterpret_cast<size_t>(&srcData[srcOffset]);

                            srcOffset = batchSrcOffset + sampleYLow * W * lastBlockDim + sampleXHigh * lastBlockDim;
                            srcAddressListTbl[n][idxIter + 1] = reinterpret_cast<size_t>(&srcData[srcOffset]);

                            srcOffset = batchSrcOffset + sampleYHigh * W * lastBlockDim + sampleXLow * lastBlockDim;
                            srcAddressListTbl[n][idxIter + 2] = reinterpret_cast<size_t>(&srcData[srcOffset]);

                            srcOffset = batchSrcOffset + sampleYHigh * W * lastBlockDim + sampleXHigh * lastBlockDim;
                            srcAddressListTbl[n][idxIter + 3] = reinterpret_cast<size_t>(&srcData[srcOffset]);
                        } else {
                            srcIndexTbl[n][idxIter] = sampleYLow  * W + sampleXLow;
                            srcIndexTbl[n][idxIter + 1] = sampleYLow  * W + sampleXHigh;
                            srcIndexTbl[n][idxIter + 2] = sampleYHigh * W + sampleXLow;
                            srcIndexTbl[n][idxIter + 3] = sampleYHigh * W + sampleXHigh;
                        }

                        // weight calculation for bilinear interpolation
                        auto ly = sampleY - sampleYLow;
                        auto lx = sampleX - sampleXLow;
                        auto hy = 1.0f - ly;
                        auto hx = 1.0f - lx;

                        weightsTbl[n][idxIter] = hy * hx;
                        weightsTbl[n][idxIter + 1] = hy * lx;
                        weightsTbl[n][idxIter + 2] = ly * hx;
                        weightsTbl[n][idxIter + 3] = ly * lx;

                        idxIter += BLIParamsNum;
                    }
                }
            }
        }
    });

    if (realRois == 0) {
        IE_THROW() << "realRois must be greater than 0";
    }

    if (roi_align_kernel) {
        if (!isPlainFmt) {
            std::vector<float> workingBuf;
            int bufSize = rnd_up(C, 16);
            size_t threadsNum = parallel_get_num_threads();
            workingBuf.resize(bufSize * threadsNum, 0.f);
            parallel_for3d(realRois, pooledH, pooledW, [&](int n, int yBinInd, int xBinInd) {
                int numSamplesROI = numSamples[n];
                // each sample have 4 values for srcAddressList and weight
                size_t binOffset = numSamplesROI * BLIParamsNum * pooledW * yBinInd + numSamplesROI * BLIParamsNum * xBinInd;

                auto arg = jit_roi_align_call_args();
                arg.src = static_cast<const void*>(&srcAddressListTbl[n][binOffset]);
                arg.weights = static_cast<const float*>(&weightsTbl[n][binOffset]);
                arg.work_amount = C;
                arg.num_samples = numSamplesROI;
                float numSamplesInBinInvert = 1.f / numSamplesROI;
                arg.scale = static_cast<const float*>(&numSamplesInBinInvert);
                float *threadBuf = static_cast<float*>(&workingBuf[parallel_get_thread_num() * bufSize]);
                memset(threadBuf, 0, bufSize * sizeof(float));
                arg.buffer = threadBuf;
                size_t dstOffset = n * batchOutputStride + yBinInd * pooledW * lastBlockDim + xBinInd * lastBlockDim;
                arg.dst = static_cast<void*>(&dst[dstOffset]);
                arg.src_stride = lastBlockDim * W * H; // only valid for blk, nspc generate inside
                (*roi_align_kernel)(&arg);
            });
        } else {
            // one lane for one sample generation, then pooling all samples.
            parallel_for4d(realRois, C, pooledH, pooledW, [&](int n, int cIdx, int yBinInd, int xBinInd) {
                size_t batchSrcOffset = srcRoiIdx[n] * batchInputStride;
                size_t channelSrcOffset = batchSrcOffset + cIdx * H * W;
                size_t binOffset = yBinInd * pooledW + xBinInd;
                size_t binDstOffset = n * batchOutputStride + cIdx * binCount + binOffset;
                int numSamplesROI = numSamples[n];
                size_t paramOffset = binOffset * BLIParamsNum * numSamplesROI;

                auto arg = jit_roi_align_call_args();
                arg.src = static_cast<const void*>(&srcData[channelSrcOffset]);
                arg.dst = static_cast<void*>(&dst[binDstOffset]);
                // buffer with absolute index
                arg.buffer = static_cast<void*>(&srcIndexTbl[n][paramOffset]);
                arg.weights = static_cast<const float*>(&weightsTbl[n][paramOffset]);
                float numSamplesInBinInvert = 1.f / numSamplesROI;
                arg.scale = static_cast<const float*>(&numSamplesInBinInvert);
                arg.num_samples = numSamplesROI;
                (*roi_align_kernel)(&arg);
            });
        }
    } else {
        // ref with planar
        parallel_for4d(realRois, C, pooledH, pooledW, [&](int n, int cIdx, int yBinInd, int xBinInd) {
            int numSamplesROI = numSamples[n];
            size_t batchSrcOffset = srcRoiIdx[n] * batchInputStride;
            size_t channelSrcOffset = batchSrcOffset + cIdx * H * W;
            size_t binOffset = yBinInd * pooledW + xBinInd;
            size_t binDstOffset = n * batchOutputStride + cIdx * binCount + binOffset;
            int paramOffset = binOffset * BLIParamsNum * numSamplesROI;
            float numSamplesInBinInvert = 1.f / numSamplesROI;

            float pooledValue = 0;
            for (unsigned int binSampleInd = 0; binSampleInd < numSamplesROI; binSampleInd++) {
                float src0 = srcData[channelSrcOffset + srcIndexTbl[n][paramOffset]];
                float src1 = srcData[channelSrcOffset + srcIndexTbl[n][paramOffset + 1]];
                float src2 = srcData[channelSrcOffset + srcIndexTbl[n][paramOffset + 2]];
                float src3 = srcData[channelSrcOffset + srcIndexTbl[n][paramOffset + 3]];

                float sampleValue =
                        weightsTbl[n][paramOffset] * src0 +
                        weightsTbl[n][paramOffset + 1] * src1 +
                        weightsTbl[n][paramOffset + 2] * src2 +
                        weightsTbl[n][paramOffset + 3] * src3;
                paramOffset += BLIParamsNum;

                switch (getAlgorithm()) {
                    case Algorithm::ROIAlignMax:
                    {
                        pooledValue = sampleValue > pooledValue ? sampleValue : pooledValue;
                        break;
                    }
                    case Algorithm::ROIAlignAvg:
                    default:
                    {
                        pooledValue += sampleValue * numSamplesInBinInvert;
                    }
                }
                dst[binDstOffset] = pooledValue;
            }
        });
    }
}

bool MKLDNNROIAlignNode::created() const {
    return getType() == ROIAlign;
}

bool MKLDNNROIAlignNode::needPrepareParams() const {
    return false;
}

void MKLDNNROIAlignNode::executeDynamicImpl(mkldnn::stream strm) {
    execute(strm);
}

REG_MKLDNN_PRIM_FOR(MKLDNNROIAlignNode, ROIAlign)
