/******************************************************************************
 * Copyright (c) 2022 Intel Corporation - All rights reserved.                *
 *                                                                            *
 * For information on the license, see the LICENSE file.                      *
 * Further information: https://github.com/libxsmm/tpp-pytorch-extension/     *
 * SPDX-License-Identifier: BSD-3-Clause                                      *
 ******************************************************************************/
/* Author: Dhiraj Kalamkar (Intel Corp.)
 ******************************************************************************/

#include <ATen/record_function.h>
// #include <torch/csrc/autograd/VariableTypeUtils.h>
#include <torch/extension.h>

#include <iostream>
#include <vector>
#include "ext_tpp.h"
#include "init.h"
#ifndef NO_PARLOOPER
#include "threaded_loops.h"
#endif
#include "timing.h"
#include "xsmm_functors.h"
#include <torch/csrc/distributed/c10d/comm.hpp>

using namespace tpp;
#include "tensor_helper.h"

static int my_rank = guess_mpi_rank();
static int my_size = 1;
static int large_cache_opt = false;
static int FT_OPT_SIZE = env2int("FT_OPT_SIZE", 256);
static int NCB_BLOCK_SIZE = env2int("NCB_BLOCK_SIZE", 64);
static int SK_BLOCK_SIZE = env2int("SK_BLOCK_SIZE", 64);
static int KV_CACHE_INC_SIZE = env2int("KV_CACHE_INC_SIZE", 128);
static const char *GEMM_LOOP_SCHEME = getenv("GEMM_LOOP_SCHEME") ? getenv("GEMM_LOOP_SCHEME") : "aCB";

REGISTER_LOCAL_SCOPE(b_emb, "b_emb");
REGISTER_LOCAL_SCOPE(pln_gemm, "pln_gemm");
REGISTER_LOCAL_SCOPE(qkv_gemm, "qkv_gemm");
REGISTER_LOCAL_SCOPE(mha, "mha");
REGISTER_LOCAL_SCOPE(ac_gemm1, "ac_gemm1");
REGISTER_LOCAL_SCOPE(ac_gemm2, "ac_gemm2");
REGISTER_LOCAL_SCOPE(o_gemm, "o_gemm");
REGISTER_LOCAL_SCOPE(i_gemm, "i_gemm");
REGISTER_LOCAL_SCOPE(lnorm, "lnorm");
REGISTER_LOCAL_SCOPE(rotary, "rotary");
REGISTER_LOCAL_SCOPE(reorder, "rorder");
REGISTER_LOCAL_SCOPE(allred, "allred");
REGISTER_LOCAL_SCOPE(barrier, "barrier");
REGISTER_LOCAL_SCOPE(concat, "concat");
REGISTER_LOCAL_SCOPE(fftkn, "fftkn");
REGISTER_LOCAL_SCOPE(k_trans, "k_trans");
REGISTER_LOCAL_SCOPE(pt_op, "pt_op");

static c10::intrusive_ptr<c10d::ProcessGroup> process_group;

void set_pg(c10::intrusive_ptr<c10d::ProcessGroup> process_group_) {
  process_group = process_group_;
  my_size = process_group->getSize();
  my_rank = process_group->getRank();
  printf("Setting PG: my_size = %d  my_rank = %d\n", my_size, my_rank);
}

inline void allreduce(at::Tensor t_in) {
  RECORD_SCOPE(allred, {t_in});
  if (!process_group) {
    printf("Missing process group when using model parallel, use set_pg()\n");
    exit(1);
  }
#if 0
  {
    RECORD_SCOPE(barrier, {});
    process_group->barrier()->wait();
  }
#endif
  std::vector<at::Tensor> temp_vec = {t_in};
  process_group->allreduce(temp_vec)->wait();
}

inline at::Tensor allgather(at::Tensor t_in, std::vector<long> &split_sizes) {
  RECORD_SCOPE(allred, {t_in});
  if (!process_group) {
    printf("Missing process group when using model parallel, use set_pg()\n");
    exit(1);
  }
#if 0
  {
    RECORD_SCOPE(barrier, {});
    process_group->barrier()->wait();
  }
#endif
  std::vector<std::vector<at::Tensor>> ag_vec(1);
  auto sz = t_in.sizes().vec();
  auto dim = t_in.dim() - 1;
  TPP_ASSERT(split_sizes.size() == my_size, "Length of split vector doesn't match group size");
  for (int i = 0; i < my_size; i++) {
    c10::InferenceMode guard(false);
    sz[dim] = split_sizes[i];
    ag_vec[0].push_back(t_in.new_empty(sz));
  }
  std::vector<at::Tensor> temp_vec = {t_in};
  process_group->allgather(ag_vec, temp_vec)->wait();
  auto t_out = at::cat(ag_vec[0], -1);
  return t_out;
}

template <typename T>
inline at::Tensor kv_concat(at::Tensor t_in1, at::Tensor t_in2, int dim, at::Tensor t_beam_idx) {
  RECORD_SCOPE(concat, {t_in1, t_in2});
  bool indirect = t_beam_idx.numel() > 0;
  auto ndim =  t_in1.dim();
  dim = dim >= 0 ? dim : dim+ndim;

  auto out_sizes = t_in1.sizes().vec();
  out_sizes[dim] += t_in2.size(dim);
  if (indirect) out_sizes[0] = t_beam_idx.size(0);
  auto t_out = t_in1.new_empty(out_sizes);

  auto B = out_sizes[0];
  auto N = out_sizes[1];
  auto S = out_sizes[2];
  auto F = out_sizes[3];
  TPP_ASSERT(B == t_in2.size(0), "Batch size mismatch\n");
  auto BNS = B * N * S;
  auto S1 = t_in1.size(dim);
  auto S2 = t_in2.size(dim);

  //auto cpy_tpp = SCOPEIT(CpyTPP<T>(F), EW_COPY);
  auto cpy_tpp = CpyTPP<T>(F);

  auto in1 = GetVLAPtr<T>(t_in1, {N, S1, F});
  auto in2 = GetVLAPtr<T>(t_in2, {N, S2, F});
  auto out = GetVLAPtr<T>(t_out, {F});
  auto beam_idx = GetVLAPtr<long>(t_beam_idx);
  //std::cout << "t_beam_idx.dtype: " << t_beam_idx.dtype() << std::endl;
  //auto beam_idx = (long*)t_beam_idx.data_ptr();
  T *ptrs[BNS];
  int p = 0;
  for (int j = 0; j < B; j++) {
    int j1 = indirect ? beam_idx[j] : j;
    for (int k = 0; k < N; k++) {
      for (int i = 0; i < S1; i++) {
        ptrs[p++] = in1[j1][k][i];
      }
      for (int i = 0; i < S2; i++) {
        ptrs[p++] = in2[j][k][i];
      }
    }
  }
  TPP_ASSERT(p == BNS, "Unmatched p=%d and BNS=%ld\n", p, BNS);
  {
    RECORD_OMP_TIME();
#pragma omp parallel for
    for (int i = 0; i < BNS; i++) {
      cpy_tpp(ptrs[i], out[i]);
    }
  }
  return t_out;
}

template <typename T>
inline void apply_rotary_pos_emb_gptj(
    at::Tensor t_in,
    at::Tensor t_emb_pos,
    at::Tensor t_pos,
    long N,
    long H) {
  RECORD_SCOPE(rotary, {t_in, t_emb_pos, t_pos});
  auto in_sizes = t_in.sizes(); // in[B][S][F]
  auto MP = t_emb_pos.size(0); // Max Pos
  auto HR = t_emb_pos.size(1); // rotary_dim
  auto B = in_sizes[0];
  auto S = in_sizes[1];
  auto COFF = HR / 2;

  auto in = GetVLAPtr<T>(t_in, {S, N, H}); // [B][S][N][H]
  auto emb_pos = GetVLAPtr<float>(t_emb_pos, {HR}); // [MP][HR]
  auto pos = GetVLAPtr<long>(t_pos, {S}); // [MB][S]
  // printf("MP=%ld HR=%ld B=%ld S=%ld N=%ld H=%ld\n", MP, HR, B, S, N, H);
  // std::cout << "pos: " << t_pos.sizes() << std::endl;
  // std::cout << "emb_pos: " << t_emb_pos.sizes() << std::endl;

  {
    RECORD_OMP_TIME();

#pragma omp parallel for collapse(3)
    for (int b = 0; b < B; b++) {
      for (int s = 0; s < S; s++) {
        for (int n = 0; n < N; n++) {
          for (int h = 0, h2 = 0; h < HR; h += 2, h2++) {
            float in0 = in[b][s][n][h];
            float in1 = in[b][s][n][h + 1];
            int p = pos[b][s];
	    if (p >= MP) continue;
	    //TPP_ASSERT(p < MP, "Invalid idx: %d (max %ld)\n", p, MP);
            float sin = emb_pos[p][h2];
            float cos = emb_pos[p][COFF + h2];
            float out0 = in0 * cos - in1 * sin;
            float out1 = in1 * cos + in0 * sin;
            in[b][s][n][h] = out0;
            in[b][s][n][h + 1] = out1;
            // if (b == 1 && s < 2 && n == 0 && h < 8) {
            //   printf("%d %d %d %d  %d: %g %g    %g %g\n", b, s, n, h, p, in0,
            //   out0, in1, out1);
            // }
          }
        }
      }
    }
  }
}

template <typename T>
inline void apply_rotary_pos_emb_llama(
    at::Tensor& t_in,
    at::Tensor& t_emb_pos,
    at::Tensor& t_pos,
    long N,
    long H) {
  RECORD_SCOPE(rotary, {t_in, t_emb_pos, t_pos});
  auto in_sizes = t_in.sizes(); // in[B][S][F]
  auto MP = t_emb_pos.size(1); // Max Pos
  auto HR = t_emb_pos.size(2); // rotary_dim
  auto B = in_sizes[0];
  auto S = in_sizes[1];
  auto COFF = HR / 2;

  auto in = GetVLAPtr<T>(t_in, {S, N, H}); // [B][S][N][H]
  auto emb_pos = GetVLAPtr<float>(t_emb_pos, {MP, HR}); // [MP][HR]
  auto pos = GetVLAPtr<long>(t_pos, {S}); // [MB][S]
  //printf("MP=%ld HR=%ld B=%ld S=%ld N=%ld H=%ld\n", MP, HR, B, S, N, H);

  {
    RECORD_OMP_TIME();

    #pragma omp parallel for collapse(3)
    for (int b = 0; b < B; b++) {
      for (int s = 0; s < S; s++) {
        for (int n = 0; n < N; n++) {
          for (int h2 = 0; h2 < HR/2; h2++) {
            float in0 = in[b][s][n][h2];
            float in1 = in[b][s][n][COFF + h2];
            int p = pos[b][s];
            float cos = emb_pos[0][p][h2];
            float sin = emb_pos[1][p][h2];
            float out0 = in0 * cos - in1 * sin;
            float out1 = in1 * cos + in0 * sin;
            in[b][s][n][h2] = out0;
            in[b][s][n][COFF + h2] = out1;
          }
        }
      }
    }
  }
}

template <typename T, typename LT = T>
inline void lyr_norm(
    at::Tensor t_in,
    at::Tensor t_gamma,
    at::Tensor t_beta,
    at::Tensor t_out,
    float eps) {
  RECORD_SCOPE(lnorm, {t_in, t_gamma, t_beta});
  auto in_sizes = t_in.sizes();
  auto BS = in_sizes[0] * in_sizes[1];
  auto K = in_sizes[2];

  auto in = GetVLAPtr<T>(t_in, {K});
  auto gamma = GetVLAPtr<LT>(t_gamma);
  auto beta = GetVLAPtr<LT>(t_beta);
  auto out = GetVLAPtr<T>(t_out, {K});

  auto layer_norm_fwd_tpp =
      SCOPEIT((LayerNormFwdTPP<T, LT>(1, 1, K, eps)), LAYER_NORM);

  {
    RECORD_OMP_TIME();
#pragma omp parallel for
    for (int b = 0; b < BS; b++) {
      layer_norm_fwd_tpp(in[b], gamma, beta, nullptr, nullptr, out[b]);
    }
  }
}

template <typename T, typename LT = T>
inline at::Tensor lyr_norm(
    at::Tensor t_in,
    at::Tensor t_gamma,
    at::Tensor t_beta,
    float eps) {
  auto t_out = at::empty_like(t_in);
  lyr_norm<T, LT>(t_in, t_gamma, t_beta, t_out, eps);
  return t_out;
}


template <typename T, typename LT = T>
inline void rms_norm(
    at::Tensor t_in,
    at::Tensor t_gamma,
    at::Tensor t_out,
    float eps) {
  RECORD_SCOPE(lnorm, {t_in, t_gamma});
  auto in_sizes = t_in.sizes();
  auto BS = in_sizes[0] * in_sizes[1];
  auto K = in_sizes[2];

  auto in = GetVLAPtr<T>(t_in, {K});
  auto gamma = GetVLAPtr<LT>(t_gamma);
  auto out = GetVLAPtr<T>(t_out, {K});

  auto rms_norm_fwd_tpp =
      SCOPEIT((RMSNormFwdTPP<T, LT>(1, 1, K, eps)), LAYER_NORM);

  {
    RECORD_OMP_TIME();

  #pragma omp parallel for
    for (int b = 0; b < BS; b++) {
      rms_norm_fwd_tpp(in[b], gamma, nullptr, out[b]);
    }
  }
}

template <typename T, typename LT = T>
inline at::Tensor llama_rms_norm(
    at::Tensor t_in,
    at::Tensor t_wt,
    float eps) {
  RECORD_SCOPE(lnorm, {t_in, t_wt});

  // auto orig_dt = t_in.dtype();
  // auto t_var = t_in.to(at::kFloat).pow(2).mean(-1, true);
  // auto t_tmp = t_in * at::rsqrt(t_var + eps);
  // auto ret = t_wt * t_tmp;
  // ret = ret.to(orig_dt);
  // return ret;

  auto t_out = at::empty_like(t_in);
  rms_norm<T, LT>(t_in, t_wt, t_out, eps);
  return t_out;
}

template <typename T>
inline void fc_plain(
    at::Tensor t_in,
    at::Tensor t_wt,
    at::Tensor t_bias,
    at::Tensor t_out) {
  RECORD_SCOPE(pln_gemm, {t_in, t_wt});
  auto in_sizes = t_in.sizes();
  auto wt_sizes = t_wt.sizes();
  auto BS = in_sizes[0] * in_sizes[1];
  auto C = in_sizes[2];

  auto Nc = wt_sizes[1];
  auto Hc = C / Nc;
  auto Nk = wt_sizes[0];
  auto Hk = wt_sizes[3];
  auto K = Nk * Hk;

  auto t_wt_V = wt_tensor_for_fwd(Nk, Hk, Nc, Hc, t_wt);

  t_in = t_in.contiguous();
  auto in = GetVLAPtr<T>(t_in, {Nc, Hc});
  auto wt_V = GetVLAPtr<T>(t_wt_V, {Nc, Hc * Hk});
  auto bias = GetVLAPtr<T>(t_bias, {Hk});
  auto out = GetVLAPtr<T>(t_out, {Nk, Hk});

  auto Ncb = Nc;
  auto BSb = 64L;
  auto rem = BS % 64;
  if (large_cache_opt) Ncb = NCB_BLOCK_SIZE;

  bool with_bias = (t_bias.numel() > 0);
  auto copy_bias_tpp = SCOPEIT(CpyBiasTPP<T>(BSb, Hk, K), BIAS);
  auto copy_bias_tpp_rem = SCOPEIT(CpyBiasTPP<T>(rem, Hk, K), BIAS);
  auto zero_tpp = SCOPEIT(SetZeroTPP<T>(BSb, Hk, K), EW_ZERO);
  auto zero_tpp_rem = SCOPEIT(SetZeroTPP<T>(rem, Hk, K), EW_ZERO);
  auto brgemm_tpp = SCOPEITGEMM(
      (BrgemmTPP<T, T>(BSb, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));
  auto brgemm_tpp_rem = SCOPEITGEMM(
      (BrgemmTPP<T, T>(rem, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));

  {
    RECORD_OMP_TIME();
    auto loop_scheme = large_cache_opt ? GEMM_LOOP_SCHEME : "aCb";
    auto ogemm_loop = ThreadedLoop<3>(
        {{0, Nc, Ncb, false}, {0L, BS, BSb}, {Nk}}, loop_scheme);
    ogemm_loop(
        [&](int* ind) {
          int nc = ind[0], s1 = ind[1], nk = ind[2];
          auto count = nc + Ncb < Nc ? Ncb : Nc - nc;
          bool is_rem = (s1 + BSb > BS);
          if (!is_rem) {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp(bias[nk], out[s1][nk]);
              } else {
                zero_tpp(out[s1][nk]);
              }
            }
            brgemm_tpp(in[s1][nc], wt_V[nk][nc], out[s1][nk], count, true);
          } else {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp_rem(bias[nk], out[s1][nk]);
              } else {
                zero_tpp_rem(out[s1][nk]);
              }
            }
            brgemm_tpp_rem(in[s1][nc], wt_V[nk][nc], out[s1][nk], count, false);
	    brgemm_tpp.config();
          }
        },
        [&]() { TimerStart();brgemm_tpp.config(); },
        [&]() { brgemm_tpp.release(); TimerEnd(); });
  }
}

template <typename T>
inline at::Tensor fc_plain(
    at::Tensor t_in,
    at::Tensor t_wt,
    at::Tensor t_bias) {
  auto sizes = t_in.sizes().vec();
  auto wt_sizes = t_wt.sizes();
  sizes[2] = wt_sizes[0] * wt_sizes[3];

  auto t_out = t_in.new_empty(sizes);
  fc_plain<T>(t_in, t_wt, t_bias, t_out);
  return t_out;
}

template<typename T>
inline at::Tensor wt_tensor_for_first_token(at::Tensor t) {
  RECORD_SCOPE(fftkn, {t});
  auto dim = t.dim();
  if (dim < 5) return t;
  auto sizes = t.sizes();
  constexpr long RBS = 4;
  auto K1 = sizes[0];
  if (K1 % RBS != 0) return t;
  auto C1 = sizes[1];
  auto C2 = sizes[2];
  auto K2 = sizes[3];
  auto C3 = sizes[4];
#if 0
  auto t_new = t.view({K1/RBS, RBS, C1, C2, K2, C3}).permute({0, 2, 3, 1, 4, 5}).contiguous().view({K1/RBS, C1, C2, RBS*K2, C3});
#else
  auto t_new = t.new_empty({K1/RBS, C1, C2, RBS*K2, C3});
  auto in = GetVLAPtr<T>(t, {RBS, C1, C2, K2 * C3});
  auto out = GetVLAPtr<T>(t_new, {C1, C2, RBS, K2 * C3});

#if 1
  auto cpy_tpp = SCOPEIT(CpyTPP<T>(C2, K2*C3, K2*C3, RBS*K2*C3), EW_COPY);

#pragma omp parallel for collapse (2)
  for (int i = 0; i < K1/RBS; i++) {
    for (int j = 0; j < C1; j++) {
      for (int k = 0; k < RBS; k++) {
        cpy_tpp(in[i][k][j][0], out[i][j][0][k]);
      }
    }
  }
#else
  auto cpy_tpp = SCOPEIT(CpyTPP<T>(RBS, K2*C3, C1*C2*K2*C3, K2*C3), EW_COPY);

#pragma omp parallel for collapse (2)
  for (int i = 0; i < K1/RBS; i++) {
    for (int j = 0; j < C1; j++) {
      for (int k = 0; k < C2; k++) {
        cpy_tpp(in[i][0][j][k], out[i][j][k][0]);
      }
    }
  }
#endif

#endif
  return t_new;
}

template <typename T>
inline void fc_mul(
    at::Tensor t_in,
    at::Tensor t_in1,
    at::Tensor t_wt,
    at::Tensor t_bias,
    at::Tensor t_out) {
  RECORD_SCOPE(o_gemm, {t_in, t_wt});
  auto in_sizes = t_in.sizes();
  auto BS = in_sizes[0] * in_sizes[1];
  if (BS > FT_OPT_SIZE) { // first token compute
    t_wt = wt_tensor_for_first_token<T>(t_wt);
  }
  auto wt_sizes = t_wt.sizes();
  auto C = in_sizes[2];

  auto Nc = wt_sizes[1];
  auto Hc = C / Nc;
  auto Nk = wt_sizes[0];
  auto Hk = wt_sizes[3];
  auto K = Nk * Hk;

  auto t_wt_V = wt_tensor_for_fwd(Nk, Hk, Nc, Hc, t_wt);

  auto in = GetVLAPtr<T>(t_in, {Nc, Hc});
  auto in1 = GetVLAPtr<T>(t_in1, {Nk, Hk});
  auto wt_V = GetVLAPtr<T>(t_wt_V, {Nc, Hc * Hk});
  auto bias = GetVLAPtr<T>(t_bias, {Hk});
  auto out = GetVLAPtr<T>(t_out, {Nk, Hk});

  auto Ncb = Nc;
  auto BSb = 64L;
  auto rem = BS % 64;
  if (large_cache_opt) Ncb = NCB_BLOCK_SIZE;

  bool with_bias = (t_bias.numel() > 0);
  auto copy_bias_tpp = SCOPEIT(CpyBiasTPP<T>(BSb, Hk, K), BIAS);
  auto copy_bias_tpp_rem = SCOPEIT(CpyBiasTPP<T>(rem, Hk, K), BIAS);
  auto zero_tpp = SCOPEIT(SetZeroTPP<T>(BSb, Hk, K), EW_ZERO);
  auto zero_tpp_rem = SCOPEIT(SetZeroTPP<T>(rem, Hk, K), EW_ZERO);
  auto brgemm_tpp = SCOPEITGEMM(
      (BrgemmTPP<T, T>(BSb, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));
  auto brgemm_tpp_rem = SCOPEITGEMM(
      (BrgemmTPP<T, T>(rem, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));
  auto mul_tpp = SCOPEIT((MulTPP<T, T>(BSb, Hk, K, K)), EW_MUL);
  auto mul_tpp_rem = SCOPEIT((MulTPP<T, T>(rem, Hk, K, K)), EW_MUL);

  {
    RECORD_OMP_TIME();
    // auto loop_scheme = large_cache_opt ? "acB" : "aBC";
    auto loop_scheme = large_cache_opt ? GEMM_LOOP_SCHEME : "aCb";
    auto ogemm_loop = ThreadedLoop<3>(
        {{0, Nc, Ncb, false}, {0L, BS, BSb}, {Nk}}, loop_scheme);
    ogemm_loop(
        [&](int* ind) {
          int nc = ind[0], s1 = ind[1], nk = ind[2];
          auto count = nc + Ncb < Nc ? Ncb : Nc - nc;
          bool is_rem = (s1 + BSb > BS);
          if (!is_rem) {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp(bias[nk], out[s1][nk]);
              } else {
                zero_tpp(out[s1][nk]);
              }
            }
            brgemm_tpp(in[s1][nc], wt_V[nk][nc], out[s1][nk], count, true);
            if (!(nc + Ncb < Nc)) { // last nc iter
              mul_tpp(in1[s1][nk], out[s1][nk], out[s1][nk]);
            }
          } else {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp_rem(bias[nk], out[s1][nk]);
              } else {
                zero_tpp_rem(out[s1][nk]);
              }
            }
            brgemm_tpp_rem(in[s1][nc], wt_V[nk][nc], out[s1][nk], count, false);
	    brgemm_tpp.config();
            if (!(nc + Ncb < Nc)) { // last nc iter
              mul_tpp_rem(in1[s1][nk], out[s1][nk], out[s1][nk]);
            }
          }
        },
        [&]() { TimerStart();brgemm_tpp.config(); },
        [&]() { brgemm_tpp.release(); TimerEnd(); });
  }
}

template <typename T>
inline at::Tensor fc_mul(
    at::Tensor t_in,
    at::Tensor t_in1,
    at::Tensor t_wt,
    at::Tensor t_bias) {
  auto t_out = at::empty_like(t_in1);
  fc_mul<T>(t_in, t_in1, t_wt, t_bias, t_out);
  return t_out;
}

template <typename T>
inline void fc_add_scale(
    at::Tensor t_in,
    at::Tensor t_in1,
    at::Tensor t_wt,
    at::Tensor t_bias,
    at::Tensor t_out,
    float scale) {
  RECORD_SCOPE(o_gemm, {t_in, t_wt});
  auto in_sizes = t_in.sizes();
  auto BS = in_sizes[0] * in_sizes[1];
  if (BS > FT_OPT_SIZE) { // first token compute
    t_wt = wt_tensor_for_first_token<T>(t_wt);
  }
  auto wt_sizes = t_wt.sizes();
  auto C = in_sizes[2];

  auto Nc = wt_sizes[1];
  auto Hc = C / Nc;
  auto Nk = wt_sizes[0];
  auto Hk = wt_sizes[3];
  auto K = Nk * Hk;

  auto t_wt_V = wt_tensor_for_fwd(Nk, Hk, Nc, Hc, t_wt);

  auto in = GetVLAPtr<T>(t_in, {Nc, Hc});
  auto in1 = GetVLAPtr<T>(t_in1, {Nk, Hk});
  auto wt_V = GetVLAPtr<T>(t_wt_V, {Nc, Hc * Hk});
  auto bias = GetVLAPtr<T>(t_bias, {Hk});
  auto out = GetVLAPtr<T>(t_out, {Nk, Hk});

  auto Ncb = Nc;
  auto BSb = 64L;
  auto rem = BS % 64;
  if (large_cache_opt) Ncb = NCB_BLOCK_SIZE;

  bool with_bias = (t_bias.numel() > 0);
  auto copy_bias_tpp = SCOPEIT(CpyBiasTPP<T>(BSb, Hk, K), BIAS);
  auto copy_bias_tpp_rem = SCOPEIT(CpyBiasTPP<T>(rem, Hk, K), BIAS);
  auto zero_tpp = SCOPEIT(SetZeroTPP<T>(BSb, Hk, K), EW_ZERO);
  auto zero_tpp_rem = SCOPEIT(SetZeroTPP<T>(rem, Hk, K), EW_ZERO);
  auto brgemm_tpp = SCOPEITGEMM(
      (BrgemmTPP<T, T>(BSb, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));
  auto brgemm_tpp_rem = SCOPEITGEMM(
      (BrgemmTPP<T, T>(rem, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));
  auto sadd_tpp = SCOPEIT((ScaleAddTPP<T, T>(BSb, Hk, K, K)), EW_ADD);
  auto sadd_tpp_rem = SCOPEIT((ScaleAddTPP<T, T>(rem, Hk, K, K)), EW_ADD);

  {
    RECORD_OMP_TIME();
    // auto loop_scheme = large_cache_opt ? "acB" : "aBC";
    auto loop_scheme = large_cache_opt ? GEMM_LOOP_SCHEME : "aCb";
    auto ogemm_loop = ThreadedLoop<3>(
        {{0, Nc, Ncb, false}, {0L, BS, BSb}, {Nk}}, loop_scheme);
    ogemm_loop(
        [&](int* ind) {
          int nc = ind[0], s1 = ind[1], nk = ind[2];
          auto count = nc + Ncb < Nc ? Ncb : Nc - nc;
          bool is_rem = (s1 + BSb > BS);
          if (!is_rem) {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp(bias[nk], out[s1][nk]);
              } else {
                zero_tpp(out[s1][nk]);
              }
            }
            brgemm_tpp(in[s1][nc], wt_V[nk][nc], out[s1][nk], count, true);
            if (!(nc + Ncb < Nc)) { // last nc iter
              sadd_tpp(in1[s1][nk], out[s1][nk], scale);
            }
          } else {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp_rem(bias[nk], out[s1][nk]);
              } else {
                zero_tpp_rem(out[s1][nk]);
              }
            }
            brgemm_tpp_rem(in[s1][nc], wt_V[nk][nc], out[s1][nk], count, false);
	    brgemm_tpp.config();
            if (!(nc + Ncb < Nc)) { // last nc iter
              sadd_tpp_rem(in1[s1][nk], out[s1][nk], scale);
            }
          }
        },
        [&]() { TimerStart();brgemm_tpp.config(); },
        [&]() { brgemm_tpp.release(); TimerEnd(); });
  }
}

template <typename T>
inline at::Tensor fc_add_scale(
    at::Tensor t_in,
    at::Tensor t_in1,
    at::Tensor t_wt,
    at::Tensor t_bias,
    float scale) {
  auto t_out = at::empty_like(t_in1);
  fc_add_scale<T>(t_in, t_in1, t_wt, t_bias, t_out, scale);
  return t_out;
}

template <typename T>
inline void fc_add2_scale(
    at::Tensor t_in,
    at::Tensor t_in1,
    at::Tensor t_in2,
    at::Tensor t_wt,
    at::Tensor t_bias,
    at::Tensor t_out,
    float scale) {
  RECORD_SCOPE(o_gemm, {t_in, t_wt});
  auto in_sizes = t_in.sizes();
  auto BS = in_sizes[0] * in_sizes[1];
  if (BS > FT_OPT_SIZE) { // first token compute
    t_wt = wt_tensor_for_first_token<T>(t_wt);
  }
  auto wt_sizes = t_wt.sizes();
  auto C = in_sizes[2];

  auto Nc = wt_sizes[1];
  auto Hc = C / Nc;
  auto Nk = wt_sizes[0];
  auto Hk = wt_sizes[3];
  auto K = Nk * Hk;

  auto t_wt_V = wt_tensor_for_fwd(Nk, Hk, Nc, Hc, t_wt);

  auto in = GetVLAPtr<T>(t_in, {Nc, Hc});
  auto in1 = GetVLAPtr<T>(t_in1, {Nk, Hk});
  auto in2 = GetVLAPtr<T>(t_in2, {Nk, Hk});
  auto wt_V = GetVLAPtr<T>(t_wt_V, {Nc, Hc * Hk});
  auto bias = GetVLAPtr<T>(t_bias, {Hk});
  auto out = GetVLAPtr<T>(t_out, {Nk, Hk});

  auto Ncb = Nc;
  auto BSb = 64L;
  auto rem = BS % 64;
  if (large_cache_opt) Ncb = NCB_BLOCK_SIZE;

  bool with_bias = (t_bias.numel() > 0);
  auto copy_bias_tpp = SCOPEIT(CpyBiasTPP<T>(BSb, Hk, K), BIAS);
  auto copy_bias_tpp_rem = SCOPEIT(CpyBiasTPP<T>(rem, Hk, K), BIAS);
  auto zero_tpp = SCOPEIT(SetZeroTPP<T>(BSb, Hk, K), EW_ZERO);
  auto zero_tpp_rem = SCOPEIT(SetZeroTPP<T>(rem, Hk, K), EW_ZERO);
  auto brgemm_tpp = SCOPEITGEMM(
      (BrgemmTPP<T, T>(BSb, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));
  auto brgemm_tpp_rem = SCOPEITGEMM(
      (BrgemmTPP<T, T>(rem, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));
  auto add_tpp = SCOPEIT((AddTPP<T, T>(BSb, Hk, K, K)), EW_ADD);
  auto add_tpp_rem = SCOPEIT((AddTPP<T, T>(rem, Hk, K, K)), EW_ADD);
  auto sadd_tpp = SCOPEIT((ScaleAddTPP<T, T>(BSb, Hk, K, K)), EW_ADD);
  auto sadd_tpp_rem = SCOPEIT((ScaleAddTPP<T, T>(rem, Hk, K, K)), EW_ADD);

  {
    RECORD_OMP_TIME();
    // auto loop_scheme = large_cache_opt ? "acB" : "aBC";
    auto loop_scheme = large_cache_opt ? GEMM_LOOP_SCHEME : "aCb";
    auto ogemm_loop = ThreadedLoop<3>(
        {{0, Nc, Ncb, false}, {0L, BS, BSb}, {Nk}}, loop_scheme);
    ogemm_loop(
        [&](int* ind) {
          int nc = ind[0], s1 = ind[1], nk = ind[2];
          auto count = nc + Ncb < Nc ? Ncb : Nc - nc;
          bool is_rem = (s1 + BSb > BS);
          if (!is_rem) {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp(bias[nk], out[s1][nk]);
              } else {
                zero_tpp(out[s1][nk]);
              }
            }
            brgemm_tpp(in[s1][nc], wt_V[nk][nc], out[s1][nk], count, true);
            if (!(nc + Ncb < Nc)) { // last nc iter
              add_tpp(out[s1][nk], in1[s1][nk], out[s1][nk]);
              sadd_tpp(in2[s1][nk], out[s1][nk], scale);
            }
          } else {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp_rem(bias[nk], out[s1][nk]);
              } else {
                zero_tpp_rem(out[s1][nk]);
              }
            }
            brgemm_tpp_rem(in[s1][nc], wt_V[nk][nc], out[s1][nk], count, false);
	    brgemm_tpp.config();
            if (!(nc + Ncb < Nc)) { // last nc iter
              add_tpp_rem(out[s1][nk], in1[s1][nk], out[s1][nk]);
              sadd_tpp_rem(in2[s1][nk], out[s1][nk], scale);
            }
          }
        },
        [&]() { TimerStart();brgemm_tpp.config(); },
        [&]() { brgemm_tpp.release(); TimerEnd(); });
  }
}

template <typename T>
inline at::Tensor fc_add2_scale(
    at::Tensor t_in,
    at::Tensor t_in1,
    at::Tensor t_in2,
    at::Tensor t_wt,
    at::Tensor t_bias,
    float scale) {
  auto t_out = at::empty_like(t_in1);
  fc_add2_scale<T>(t_in, t_in1, t_in2, t_wt, t_bias, t_out, scale);
  return t_out;
}

template <typename T>
inline void fc_gelu(
    at::Tensor t_in,
    at::Tensor t_wt,
    at::Tensor t_bias,
    at::Tensor t_out) {
  RECORD_SCOPE(i_gemm, {t_in, t_wt});
  auto in_sizes = t_in.sizes();
  auto BS = in_sizes[0] * in_sizes[1];
  if (BS > FT_OPT_SIZE) { // first token compute
    t_wt = wt_tensor_for_first_token<T>(t_wt);
  }
  auto wt_sizes = t_wt.sizes();
  auto C = in_sizes[2];

  auto Nc = wt_sizes[1];
  auto Hc = C / Nc;
  auto Nk = wt_sizes[0];
  auto Hk = wt_sizes[3];
  auto K = Nk * Hk;

  auto t_wt_V = wt_tensor_for_fwd(Nk, Hk, Nc, Hc, t_wt);

  auto in = GetVLAPtr<T>(t_in, {Nc, Hc});
  auto wt_V = GetVLAPtr<T>(t_wt_V, {Nc, Hc * Hk});
  auto bias = GetVLAPtr<T>(t_bias, {Hk});
  auto out = GetVLAPtr<T>(t_out, {Nk, Hk});

  auto Ncb = Nc;
  auto BSb = 64L;
  auto rem = BS % 64;
  if (large_cache_opt) Ncb = NCB_BLOCK_SIZE;

  bool with_bias = (t_bias.numel() > 0);
  auto copy_bias_tpp = SCOPEIT(CpyBiasTPP<T>(BSb, Hk, K), BIAS);
  auto copy_bias_tpp_rem = SCOPEIT(CpyBiasTPP<T>(rem, Hk, K), BIAS);
  auto zero_tpp = SCOPEIT(SetZeroTPP<T>(BSb, Hk, K), EW_ZERO);
  auto zero_tpp_rem = SCOPEIT(SetZeroTPP<T>(rem, Hk, K), EW_ZERO);
  auto brgemm_tpp = SCOPEITGEMM(
      (BrgemmTPP<T, T>(BSb, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));
  auto brgemm_tpp_rem = SCOPEITGEMM(
      (BrgemmTPP<T, T>(rem, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));
  auto gelu_fwd_tpp = SCOPEIT(GeluFwdTPP<T>(BSb, Hk, K, K), ACT);
  auto gelu_fwd_tpp_rem = SCOPEIT(GeluFwdTPP<T>(rem, Hk, K, K), ACT);

  {
    RECORD_OMP_TIME();
    // auto loop_scheme = large_cache_opt ? "acB" : "aBC";
    auto loop_scheme = large_cache_opt ? GEMM_LOOP_SCHEME : "aCb";
    auto igemm_loop =
        ThreadedLoop<3>({{0, Nc, Ncb, false}, {0, BS, BSb}, {Nk}}, loop_scheme);
    igemm_loop(
        [&](int* ind) {
          int nc = ind[0], s1 = ind[1], nk = ind[2];
          auto count = nc + Ncb < Nc ? Ncb : Nc - nc;
          bool is_rem = (s1 + BSb > BS);
          if (!is_rem) {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp(bias[nk], out[s1][nk]);
              } else {
                zero_tpp(out[s1][nk]);
              }
            }
            brgemm_tpp(in[s1][nc], wt_V[nk][nc], out[s1][nk], count, true);
            if (!(nc + Ncb < Nc)) { // last nc iter
              gelu_fwd_tpp(out[s1][nk], out[s1][nk]);
            }
          } else {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp_rem(bias[nk], out[s1][nk]);
              } else {
                zero_tpp_rem(out[s1][nk]);
              }
            }
            brgemm_tpp_rem(in[s1][nc], wt_V[nk][nc], out[s1][nk], count, false);
	    brgemm_tpp.config();
            if (!(nc + Ncb < Nc)) { // last nc iter
              gelu_fwd_tpp_rem(out[s1][nk], out[s1][nk]);
            }
          }
        },
        [&]() { TimerStart();brgemm_tpp.config(); },
        [&]() { brgemm_tpp.release(); TimerEnd(); });
  }
}

template <typename T>
inline at::Tensor fc_gelu(
    at::Tensor t_in,
    at::Tensor t_wt,
    at::Tensor t_bias) {
  auto sizes = t_in.sizes().vec();
  auto wt_sizes = t_wt.sizes();
  sizes[2] = wt_sizes[0] * wt_sizes[3];

  auto t_out = t_in.new_empty(sizes);
  fc_gelu<T>(t_in, t_wt, t_bias, t_out);
  return t_out;
}

template <typename T>
inline void fc_silu(
    at::Tensor t_in,
    at::Tensor t_wt,
    at::Tensor t_bias,
    at::Tensor t_out) {
  RECORD_SCOPE(i_gemm, {t_in, t_wt});
  auto in_sizes = t_in.sizes();
  auto BS = in_sizes[0] * in_sizes[1];
  if (BS > FT_OPT_SIZE) { // first token compute
    t_wt = wt_tensor_for_first_token<T>(t_wt);
  }
  auto wt_sizes = t_wt.sizes();
  auto C = in_sizes[2];

  auto Nc = wt_sizes[1];
  auto Hc = C / Nc;
  auto Nk = wt_sizes[0];
  auto Hk = wt_sizes[3];
  auto K = Nk * Hk;

  auto t_wt_V = wt_tensor_for_fwd(Nk, Hk, Nc, Hc, t_wt);

  auto in = GetVLAPtr<T>(t_in, {Nc, Hc});
  auto wt_V = GetVLAPtr<T>(t_wt_V, {Nc, Hc * Hk});
  auto bias = GetVLAPtr<T>(t_bias, {Hk});
  auto out = GetVLAPtr<T>(t_out, {Nk, Hk});

  auto Ncb = Nc;
  auto BSb = 64L;
  auto rem = BS % 64;
  if (large_cache_opt) Ncb = NCB_BLOCK_SIZE;

  bool with_bias = (t_bias.numel() > 0);
  auto copy_bias_tpp = SCOPEIT(CpyBiasTPP<T>(BSb, Hk, K), BIAS);
  auto copy_bias_tpp_rem = SCOPEIT(CpyBiasTPP<T>(rem, Hk, K), BIAS);
  auto zero_tpp = SCOPEIT(SetZeroTPP<T>(BSb, Hk, K), EW_ZERO);
  auto zero_tpp_rem = SCOPEIT(SetZeroTPP<T>(rem, Hk, K), EW_ZERO);
  auto brgemm_tpp = SCOPEITGEMM(
      (BrgemmTPP<T, T>(BSb, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));
  auto brgemm_tpp_rem = SCOPEITGEMM(
      (BrgemmTPP<T, T>(rem, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));
  auto silu_fwd_tpp = SCOPEIT(SiLUFwdTPP<T>(BSb, Hk, K, K), ACT);
  auto silu_fwd_tpp_rem = SCOPEIT(SiLUFwdTPP<T>(rem, Hk, K, K), ACT);

  {
    RECORD_OMP_TIME();
    // auto loop_scheme = large_cache_opt ? "acB" : "aBC";
    auto loop_scheme = large_cache_opt ? GEMM_LOOP_SCHEME : "aCb";
    auto igemm_loop =
        ThreadedLoop<3>({{0, Nc, Ncb, false}, {0, BS, BSb}, {Nk}}, loop_scheme);
    igemm_loop(
        [&](int* ind) {
          int nc = ind[0], s1 = ind[1], nk = ind[2];
          auto count = nc + Ncb < Nc ? Ncb : Nc - nc;
          bool is_rem = (s1 + BSb > BS);
          if (!is_rem) {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp(bias[nk], out[s1][nk]);
              } else {
                zero_tpp(out[s1][nk]);
              }
            }
            brgemm_tpp(in[s1][nc], wt_V[nk][nc], out[s1][nk], count, true);
            if (!(nc + Ncb < Nc)) { // last nc iter
              silu_fwd_tpp(out[s1][nk], out[s1][nk]);
            }
          } else {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp_rem(bias[nk], out[s1][nk]);
              } else {
                zero_tpp_rem(out[s1][nk]);
              }
            }
            brgemm_tpp_rem(in[s1][nc], wt_V[nk][nc], out[s1][nk], count, false);
	    brgemm_tpp.config();
            if (!(nc + Ncb < Nc)) { // last nc iter
              silu_fwd_tpp_rem(out[s1][nk], out[s1][nk]);
            }
          }
        },
        [&]() { TimerStart();brgemm_tpp.config(); },
        [&]() { brgemm_tpp.release(); TimerEnd(); });
  }
}

template <typename T>
inline at::Tensor fc_silu(
    at::Tensor t_in,
    at::Tensor t_wt,
    at::Tensor t_bias) {
  auto sizes = t_in.sizes().vec();
  auto wt_sizes = t_wt.sizes();
  sizes[2] = wt_sizes[0] * wt_sizes[3];

  auto t_out = t_in.new_empty(sizes);
  fc_silu<T>(t_in, t_wt, t_bias, t_out);
  return t_out;
}

template <typename T>
inline void fc_relu(
    at::Tensor t_in,
    at::Tensor t_wt,
    at::Tensor t_bias,
    at::Tensor t_out) {
  RECORD_SCOPE(i_gemm, {t_in, t_wt});
  auto in_sizes = t_in.sizes();
  auto BS = in_sizes[0] * in_sizes[1];
  if (BS > FT_OPT_SIZE) { // first token compute
    t_wt = wt_tensor_for_first_token<T>(t_wt);
  }
  auto wt_sizes = t_wt.sizes();
  auto C = in_sizes[2];

  auto Nc = wt_sizes[1];
  auto Hc = C / Nc;
  auto Nk = wt_sizes[0];
  auto Hk = wt_sizes[3];
  auto K = Nk * Hk;

  auto t_wt_V = wt_tensor_for_fwd(Nk, Hk, Nc, Hc, t_wt);

  auto in = GetVLAPtr<T>(t_in, {Nc, Hc});
  auto wt_V = GetVLAPtr<T>(t_wt_V, {Nc, Hc * Hk});
  auto bias = GetVLAPtr<T>(t_bias, {Hk});
  auto out = GetVLAPtr<T>(t_out, {Nk, Hk});

  auto Ncb = Nc;
  auto BSb = 64L;
  auto rem = BS % 64;
  if (large_cache_opt) Ncb = NCB_BLOCK_SIZE;

  bool with_bias = (t_bias.numel() > 0);
  auto copy_bias_tpp = SCOPEIT(CpyBiasTPP<T>(BSb, Hk, K), BIAS);
  auto copy_bias_tpp_rem = SCOPEIT(CpyBiasTPP<T>(rem, Hk, K), BIAS);
  auto zero_tpp = SCOPEIT(SetZeroTPP<T>(BSb, Hk, K), EW_ZERO);
  auto zero_tpp_rem = SCOPEIT(SetZeroTPP<T>(rem, Hk, K), EW_ZERO);
  auto brgemm_tpp = SCOPEITGEMM(
      (BrgemmTPP<T, T>(BSb, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));
  auto brgemm_tpp_rem = SCOPEITGEMM(
      (BrgemmTPP<T, T>(rem, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));
  auto relu_fwd_tpp = SCOPEIT(ReLUFwdTPP<T>(BSb, Hk, K, K, false), ACT);
  auto relu_fwd_tpp_rem = SCOPEIT(ReLUFwdTPP<T>(rem, Hk, K, K, false), ACT);

  {
    RECORD_OMP_TIME();
    // auto loop_scheme = large_cache_opt ? "acB" : "aBC";
    auto loop_scheme = large_cache_opt ? GEMM_LOOP_SCHEME : "aCb";
    auto igemm_loop =
        ThreadedLoop<3>({{0, Nc, Ncb, false}, {0, BS, BSb}, {Nk}}, loop_scheme);
    igemm_loop(
        [&](int* ind) {
          int nc = ind[0], s1 = ind[1], nk = ind[2];
          auto count = nc + Ncb < Nc ? Ncb : Nc - nc;
          bool is_rem = (s1 + BSb > BS);
          if (!is_rem) {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp(bias[nk], out[s1][nk]);
              } else {
                zero_tpp(out[s1][nk]);
              }
            }
            brgemm_tpp(in[s1][nc], wt_V[nk][nc], out[s1][nk], count, true);
            if (!(nc + Ncb < Nc)) { // last nc iter
              relu_fwd_tpp(out[s1][nk], out[s1][nk]);
            }
          } else {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp_rem(bias[nk], out[s1][nk]);
              } else {
                zero_tpp_rem(out[s1][nk]);
              }
            }
            brgemm_tpp_rem(in[s1][nc], wt_V[nk][nc], out[s1][nk], count, false);
	    brgemm_tpp.config();
            if (!(nc + Ncb < Nc)) { // last nc iter
              relu_fwd_tpp_rem(out[s1][nk], out[s1][nk]);
            }
          }
        },
        [&]() { TimerStart();brgemm_tpp.config(); },
        [&]() { brgemm_tpp.release(); TimerEnd(); });
  }
}

template <typename T>
inline at::Tensor fc_relu(
    at::Tensor t_in,
    at::Tensor t_wt,
    at::Tensor t_bias) {
  auto sizes = t_in.sizes().vec();
  auto wt_sizes = t_wt.sizes();
  sizes[2] = wt_sizes[0] * wt_sizes[3];

  auto t_out = t_in.new_empty(sizes);
  fc_relu<T>(t_in, t_wt, t_bias, t_out);
  return t_out;
}

template <typename T, typename Tout = T>
inline void qkv_gemm(at::Tensor t_in, at::Tensor t_wt, at::Tensor t_bias, at::Tensor t_out) {
  RECORD_SCOPE(qkv_gemm, {t_in, t_wt});
  auto in_sizes = t_in.sizes();
  auto BS = in_sizes[0] * in_sizes[1];
  if (BS > FT_OPT_SIZE) { // first token compute
    t_wt = wt_tensor_for_first_token<T>(t_wt);
  }
  auto wt_sizes = t_wt.sizes();
  auto C = in_sizes[2];

  auto Nc = wt_sizes[1];
  auto Hc = C / Nc;
  auto Nk = wt_sizes[0];
  auto Hk = wt_sizes[3];
  auto K = Nk * Hk;

  auto t_wt_V = wt_tensor_for_fwd(Nk, Hk, Nc, Hc, t_wt);

  auto in = GetVLAPtr<T>(t_in, {Nc, Hc});
  auto wt_V = GetVLAPtr<T>(t_wt_V, {Nc, Hc * Hk});
  auto bias = GetVLAPtr<T>(t_bias, {Hk});
  auto out = GetVLAPtr<Tout>(t_out, {Nk, Hk});

  auto Ncb = Nc;
  auto BSb = 64L;
  auto rem = BS % BSb;
  if (large_cache_opt) Ncb = NCB_BLOCK_SIZE;

  bool with_bias = (t_bias.numel() > 0);
  auto copy_bias_tpp = SCOPEIT(CpyBiasTPP<T>(BSb, Hk, K), BIAS);
  auto copy_bias_tpp_rem = SCOPEIT(CpyBiasTPP<T>(rem, Hk, K), BIAS);
  auto zero_tpp = SCOPEIT(SetZeroTPP<Tout>(BSb, Hk, K), EW_ZERO);
  auto zero_tpp_rem = SCOPEIT(SetZeroTPP<Tout>(rem, Hk, K), EW_ZERO);
  auto brgemm_tpp = SCOPEITGEMM(
      (BrgemmTPP<T, Tout>(BSb, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));
  auto brgemm_tpp_rem = SCOPEITGEMM(
      (BrgemmTPP<T, Tout>(rem, Hk, Hc, Hc, Hk * Hc, C, Hk, K, 1.0, 0, Ncb)));

  {
    RECORD_OMP_TIME();
    // auto loop_scheme = large_cache_opt ? "acB" : "aBC";
    auto loop_scheme = large_cache_opt ? GEMM_LOOP_SCHEME : "aCb";
    auto gemm_loop = ThreadedLoop<3>(
        {{0, Nc, Ncb, false}, {0, BS, BSb}, {Nk}}, loop_scheme);
    gemm_loop(
        [&](int* ind) {
          int nc = ind[0], s1 = ind[1], nk = ind[2];
          auto count = nc + Ncb < Nc ? Ncb : Nc - nc;
          bool is_rem = (s1 + BSb > BS);
          if (!is_rem) {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp(bias[nk], out[s1][nk]);
              } else {
                zero_tpp(out[s1][nk]);
              }
            }
            brgemm_tpp(in[s1][nc], wt_V[nk][nc], out[s1][nk], count, true);
          } else {
            if (nc == 0) {
              if (with_bias) {
                copy_bias_tpp_rem(bias[nk], out[s1][nk]);
              } else {
                zero_tpp_rem(out[s1][nk]);
              }
            }
            brgemm_tpp_rem(
                in[s1][nc], wt_V[nk][nc], out[s1][nk], count, false);
            brgemm_tpp.config();
          }
        },
        [&]() { TimerStart();brgemm_tpp.config(); },
        [&]() { brgemm_tpp.release(); TimerEnd(); });
  }
}

template <typename T, typename Tout = T>
inline at::Tensor qkv_gemm(at::Tensor t_in, at::Tensor t_wt, at::Tensor t_bias) {
  auto sizes = t_in.sizes().vec();
  auto wt_sizes = t_wt.sizes();
  sizes[2] = wt_sizes[0] * wt_sizes[3];
  auto t_out = t_in.new_empty(sizes);
  qkv_gemm<T, Tout>(t_in, t_wt, t_bias, t_out);
  return t_out;
}

template <typename T, typename Tv>
struct AttnKernels
{

  SCOPEIT_DECL(BrgemmTPP<T, float>) a_gemm_tpp;
  SCOPEIT_DECL(ScaleTPP<float, float>) scale_tpp;
  SCOPEIT_DECL(AddBiasTPP<T>) add_mask_tpp;
  SCOPEIT_DECL(AddTPP<T,float,float>) add_2dmask_tpp;
  SCOPEIT_DECL(VarSoftMaxFwdTPP<float, Tv>) softmax_fwd_tpp;
  SCOPEIT_DECL(BrgemmTPP<Tv, Tv>) c_gemm_tpp;
  SCOPEIT_DECL(ConvertTPP<Tv, T>) cvt_tpp;
  SCOPEIT_DECL(CpyTPP<T>) cpy_tpp;
  SCOPEIT_DECL(XformExtTPP<T>) xform_tpp;
  SCOPEIT_DECL(XformExtTPP<T>) vnni_tpp;
  SCOPEIT_DECL(SoftMaxFixUpTPP<T>) softmax_fixup;
  long Sqb_aligned;
  static constexpr long SQB_ALIGN = 16;

  AttnKernels(long Sqb, long Skb, long H, int pad, int kl_in_vnni, int vl_in_vnni, bool am_is_2d) {
    //printf("Sqb: %ld, Skb: %ld, H: %ld, psd: %d, kl: %d, vl: %d\n", Sqb, Skb, H, pad, kl_in_vnni, vl_in_vnni);
    if (Sqb == 0 || Skb == 0) return;
    Sqb_aligned = (Sqb + SQB_ALIGN - 1) & ~(SQB_ALIGN - 1);
    // [Sqb_aligned, H] * [H, Skb] = [Sqb_aligned, Skb]
    a_gemm_tpp = SCOPEITGEMM((BrgemmTPP<T, float>(
            Sqb_aligned, Skb, H, H, H * Skb, H, Skb, Skb, 0.0, 0, 1, kl_in_vnni)));
    // [Sqb_aligned, Skb]
    scale_tpp = SCOPEIT((ScaleTPP<float, float>(Sqb_aligned * Skb)), EW_SCL);
    if (!am_is_2d)
      add_mask_tpp = SCOPEIT(AddBiasTPP<T>(Sqb_aligned, Skb), EW_ADD);
    else
      add_2dmask_tpp = SCOPEIT((AddTPP<T,float,float>(Sqb, Skb)), EW_ADD);
    softmax_fwd_tpp =
      SCOPEIT((VarSoftMaxFwdTPP<float, Tv>(Sqb, Skb)), SOFTMAX);
    softmax_fixup =
      SCOPEIT((SoftMaxFixUpTPP<T>(Sqb, H)), EW_RCP);
    // [Sqb_aligned, Skb] * [Skb, H] = tmp[Sqb_aligned, H]
    c_gemm_tpp = SCOPEITGEMM((BrgemmTPP<Tv, Tv>(
            Sqb_aligned, H, Skb, Sqb_aligned * Skb, Skb * H, Skb, H, H, 0.0, 0, 1, vl_in_vnni)));
    // [Sqb, H] --> [Sqb, H]
    cvt_tpp = SCOPEIT((ConvertTPP<Tv, T>(Sqb, H, H, H)), EW_COPY);
    if (Sqb != Sqb_aligned)
      cpy_tpp = SCOPEIT(CpyTPP<T>(Sqb, H, H, H), EW_COPY);
    auto xform = XformTPP::XFORM_XPOSE_TPP;
    if (!std::is_same<T, float>::value && kl_in_vnni) {
      xform = XformTPP::XFORM_XPOSE_N2V_TPP;
    }
    // [Skb-pad, H] --> [H, Skb]
    xform_tpp =
      SCOPEIT(XformExtTPP<T>(Skb-pad, H, H, Skb, H, Skb, xform, true), XPOSE);
    if (vl_in_vnni != 0)
      vnni_tpp =
        SCOPEIT(XformExtTPP<T>(Skb-pad, H, Skb, H, H, H, XformTPP::XFORM_N2V_TPP, true), VNNI);
  }
};

template <typename T>
inline at::Tensor attn(
    at::Tensor t_QL,
    at::Tensor t_KL,
    at::Tensor t_AM,
    at::Tensor t_VL,
    at::Tensor t_KL_cache,
    at::Tensor t_VL_cache,
    VLAPtr<long, 1, long>& beam_idx,
    long offset) {
  RECORD_SCOPE(ac_gemm2, {t_QL, t_KL});
  auto t_CL = at::empty_like(t_QL);
  auto sizes = t_QL.sizes();
  long B = sizes[0];
  long N = sizes[1];
  long Sq = sizes[2];
  long H = sizes[3];
  float one_by_sqrt_H = 1.0 / sqrtf(H);
  auto ksizes = t_KL.sizes();
  long Sk = ksizes[2];
  // printf("Sq = %ld, Sk = %ld\n", Sq, Sk);
  // std::cout << "QL: " << t_QL.sizes() << std::endl;
  // std::cout << "KL: " << t_KL.sizes() << std::endl;
  TPP_ASSERT(Sq == 1 && Sk == 1, "Sq (%ld) and Sk (%ld) must be 1, offset (%ld)\n", Sq, Sk, offset);
  auto FSk = offset + Sk;
  auto FSk_aligned = (FSk + 0x3FL) & ~0x3FL;
  auto CSk = t_KL_cache.size(2);
  //printf("CSk = %d, FSk = %d\n", (int)CSk, (int)FSk);
  const bool am_valid = (t_AM.numel() > 0);

  auto QL = GetVLAPtr<T>(t_QL, {N, Sq, H});
  auto KL = GetVLAPtr<T>(t_KL, {N, Sk, H});
  auto VL = GetVLAPtr<T>(t_VL, {N, Sk, H});
  auto CL = GetVLAPtr<T>(t_CL, {N, Sq, H});
  auto AM = GetVLAPtr<T>(t_AM, {FSk});
  auto KL_C = GetVLAPtr<T>(t_KL_cache, {N, CSk, H});
  auto VL_C = GetVLAPtr<T>(t_VL_cache, {N, CSk, H});

  // Removing SCOPEIT due to very high overhead of timing these
  // auto dot_tpp = SCOPEIT((MulReduceTPP<T,T,float>(1, H)), EW_MUL);
  // auto scale_add_tpp = SCOPEIT((ScaleAddTPP<T, T>(H)), EW_ADD);
  // auto cpy_tpp = SCOPEIT(CpyTPP<T>(H), EW_COPY);
  // auto zero_tpp = SCOPEIT(SetZeroTPP<T>(H), EW_ZERO);
  auto dot_tpp = MulReduceTPP<float,T,float>(1, H);
  auto scale_add_tpp = ScaleAddTPP<T, float>(H);
  auto cpy_tpp = CpyTPP<T>(H);
  auto cvt_f2b_tpp = ConvertTPP<float,T>(H);
  auto cvt_b2f_tpp = ConvertTPP<T,float>(H);
  auto zero_tpp = SetZeroTPP<float>(H);
  auto softmax_fwd_tpp =
    SCOPEIT((SoftMaxFwdTPP<float, float>(1, 1, FSk_aligned)), SOFTMAX);
  if (FSk <= 256) {
    RECORD_OMP_TIME();
    {
#pragma omp parallel
      {
        int tid = omp_get_thread_num();
        //auto t00 = getTime();
        TimerStart();
#pragma omp for collapse(2) nowait
        for (int b = 0; b < B; b++) {
          for (int n = 0; n < N; n++) {
            float AS[FSk_aligned];
            //float *AS = GAS[tid]; //FSk];
            //auto t0 = getTime();
            {
              ScopedTimer t_(BRGEMM, 2 * FSk * H);
              float tmp_QL[H];
              cvt_b2f_tpp(QL[b][n][0], tmp_QL);
              for (int sk = 0; sk < FSk; sk++) {
                AS[sk] = 0.0f;
                if (sk < offset) {
                  int bid = beam_idx[b][sk];
                  //printf("b: %d n: %d sk: %d  bid = %d\n", b, n, sk, bid);
                  //dot_tpp(tmp_QL, KL_C[sk][bid][n], &AS[sk]);
                  dot_tpp(tmp_QL, KL_C[bid][n][sk], &AS[sk]);
                } else {
                  //printf("b: %d n: %d sk: %d \n", b, n, sk);
                  dot_tpp(tmp_QL, KL[b][n][0], &AS[sk]);
                  //cpy_tpp(KL[b][n][0], KL_C[sk][b][n]);
                  cpy_tpp(KL[b][n][0], KL_C[b][n][sk]);
                }
                AS[sk] *= one_by_sqrt_H;
                if (am_valid) {
                  AS[sk] += AM[b][sk];
                }
              }
              for (int sk = FSk; sk < FSk_aligned; sk++) {
                // pad AS to align for softmax
                AS[sk] = -1e9f;
              }
            }
            //auto t1 = getTime();
            softmax_fwd_tpp(AS, AS);
            //auto t2 = getTime();
            //printf("post softmax b: %d n: %d\n", b, n);
            {
              float tmp_CL[H];
              ScopedTimer t_(BRGEMM, 2 * FSk * H);
              zero_tpp(tmp_CL);
              for (int sk = 0; sk < FSk; sk++) {
                //printf("bmm2: b: %d n: %d sk: %d \n", b, n, sk);
                //if (b == 0&& n == 0) printf("AS[%d]: %g\n", sk, AS[sk]); 
                if (sk < offset) {
                  int bid = beam_idx[b][sk];
                  //scale_add_tpp(VL_C[sk][bid][n], tmp_CL, AS[sk]);
                  scale_add_tpp(VL_C[bid][n][sk], tmp_CL, AS[sk]);
                } else {
                  scale_add_tpp(VL[b][n][0], tmp_CL, AS[sk]);
                  //cpy_tpp(VL[b][n][0], VL_C[sk][b][n]);
                  cpy_tpp(VL[b][n][0], VL_C[b][n][sk]);
                }
              }
              cvt_f2b_tpp(tmp_CL, CL[b][n][0]);
            }
            //auto t3 = getTime();
            //if (tid == 0) printf("MHA: bns= %d %d %ld  %10g %10g %10g    %10g\n", b, n, FSk, (t1-t0)*1e6, (t2-t1)*1e6, (t3-t2)*1e6, (t3-t0)*1e6);
          }
        }
        TimerEnd();
        //auto t01 = getTime();
        //if (tid == 0) printf("MHA: s= %ld  %10g\n", FSk, (t01-t00)*1e6);
      }
    }
  } else {
    auto t_AS = t_QL.new_empty({B, N, FSk_aligned}, at::kFloat);
    // auto t_XL = t_QL.new_empty({B, N, H}, at::kFloat);
    auto t_XL = t_QL.to(at::kFloat);
    auto XL = GetVLAPtr<float>(t_XL, {N, H});
    auto AS = GetVLAPtr<float>(t_AS, {N, FSk_aligned});

    RECORD_OMP_TIME();
    {
#pragma omp parallel for collapse(3)
      for (int b = 0; b < B; b++) {
        for (int n = 0; n < N; n++) {
          for (int sk = 0; sk < FSk; sk++) {
            AS[b][n][sk] = 0.0f;
            if (sk < offset) {
              int bid = beam_idx[b][sk];
              //printf("b: %d n: %d sk: %d  bid = %d\n", b, n, sk, bid);
              //dot_tpp(tmp_QL, KL_C[sk][bid][n], &AS[sk]);
              dot_tpp(XL[b][n], KL_C[bid][n][sk], &AS[b][n][sk]);
            } else {
              //printf("b: %d n: %d sk: %d \n", b, n, sk);
              dot_tpp(XL[b][n], KL[b][n][0], &AS[b][n][sk]);
              cpy_tpp(KL[b][n][0], KL_C[b][n][sk]);
            }
            AS[b][n][sk] *= one_by_sqrt_H;
            if (am_valid) {
              AS[b][n][sk] += AM[b][sk];
            }
          }
        }
      }
#pragma omp parallel for collapse(2)
      for (int b = 0; b < B; b++) {
        for (int n = 0; n < N; n++) {
          for (int sk = FSk; sk < FSk_aligned; sk++) {
            // pad AS to align for softmax
            AS[b][n][sk] = -1e9f;
          }
          softmax_fwd_tpp(AS[b][n], AS[b][n]);
          zero_tpp(XL[b][n]);
          for (int sk = 0; sk < FSk; sk++) {
            if (sk < offset) {
              int bid = beam_idx[b][sk];
              scale_add_tpp(VL_C[bid][n][sk], XL[b][n], AS[b][n][sk]);
            } else {
              scale_add_tpp(VL[b][n][0], XL[b][n], AS[b][n][sk]);
              cpy_tpp(VL[b][n][0], VL_C[b][n][sk]);
            }
          }
        }
      }
    }
    t_CL = t_XL.to(t_CL.dtype());
  }
  return t_CL;
}

template <typename T, typename Tv>
inline at::Tensor attn(
    at::Tensor t_QL,
    at::Tensor t_KL,
    at::Tensor t_AM,
    at::Tensor t_VL) {
  RECORD_SCOPE(ac_gemm1, {t_QL, t_KL});
  auto t_CL = at::empty_like(t_QL);
  auto sizes = t_QL.sizes();
  long B = sizes[0];
  long N = sizes[1];
  long Sq = sizes[2];
  long H = sizes[3];
  float one_by_sqrt_H = 1.0 / sqrtf(H);
  auto ksizes = t_KL.sizes();
  long Sk = ksizes[2];
  long offset = Sk - Sq;
  constexpr long Sqb = 64;
  long qrem = Sq % Sqb;
  bool inline_trans = ((Sq+Sqb-1) / Sqb == 1);
  bool am_is_2d = t_AM.size(2) != 1;

  int vl_in_vnni = 1; //(Sk % 2 == 0 ? 1 : 0);
  const long VBS = (vl_in_vnni ? get_vnni_block_size<T>() : 1);
  const long SK_ALIGN = (VBS == 1 ? 1 : 64);
  long Sk_pad = (Sk + SK_ALIGN - 1) & ~(SK_ALIGN - 1);
  const long Skb = (!inline_trans ? 1024 : SK_BLOCK_SIZE); 
  long krem = Sk % Skb;
  int pad = Sk_pad - Sk;

  auto t_KL_TV = t_KL.new_empty({B, N, Sk_pad, H});
  auto t_VL_V = t_VL;
  if (VBS != 1) {
    t_VL_V = t_VL.new_empty({B, N, Sk_pad, H});
  }
  if (Sk != Sk_pad) {
    //TPP_ASSERT(am_is_2d == false, "2D AM not supported yet\n");
    if (!am_is_2d) {
      auto t_tmp = t_AM.new_empty({B, pad});
      t_tmp.fill_(-10000.0);
      t_AM = at::cat({t_AM.view({B, -1}), t_tmp}, -1);
    } else {
      auto t_tmp = t_AM.new_empty({B, 1, Sq, pad});
      t_tmp.fill_(-10000.0);
      t_AM = at::cat({t_AM, t_tmp}, -1);
    }
  }
  auto QL = GetVLAPtr<T>(t_QL, {N, Sq, H});
  auto KL = GetVLAPtr<T>(t_KL, {N, Sk, H});
  auto KL_TV = GetVLAPtr<T>(t_KL_TV, {N, Sk_pad, H});
  auto VL = GetVLAPtr<Tv>(t_VL, {N, Sk, H});
  auto VL_V = GetVLAPtr<Tv>(t_VL_V, {N, Sk_pad, H});
  auto CL = GetVLAPtr<T>(t_CL, {N, Sq, H});
  auto AM = GetVLAPtr<T>(t_AM, {Sk_pad});
  auto AM2 = GetVLAPtr<T>(t_AM, {Sq, Sk_pad});
  int kl_in_vnni = 1;


  AttnKernels<T, Tv> attn_kern[4] =  {
    AttnKernels<T,Tv>(Sqb, Skb, H, 0, kl_in_vnni, vl_in_vnni, am_is_2d),
    AttnKernels<T,Tv>(Sqb, krem+pad, H, pad, kl_in_vnni, vl_in_vnni, am_is_2d),
    AttnKernels<T,Tv>(qrem, Skb, H, 0, kl_in_vnni, vl_in_vnni, am_is_2d),
    AttnKernels<T,Tv>(qrem, krem+pad, H, pad, kl_in_vnni, vl_in_vnni, am_is_2d),
  };

  if (!inline_trans) {
    RECORD_SCOPE(k_trans, {t_QL, t_KL});
#pragma omp parallel for collapse(3)
    for (int n = 0; n < N; n++) {
      for (int b = 0; b < B; b++) {
        for (int sk = 0; sk < Sk; sk += Skb) {
          int kid = (sk + Skb > Sk) ? 1 : 0;
          attn_kern[kid].xform_tpp(KL[b][n][sk], KL_TV[b][n][sk]);
          if (VBS != 1)
            attn_kern[kid].vnni_tpp(VL[b][n][sk], VL_V[b][n][sk]);
        }
      }
    }
  }

  {
    RECORD_OMP_TIME();
    {
#pragma omp parallel for collapse(3)
      for (int b = 0; b < B; b++) {
        for (int n = 0; n < N; n++) {
          for (int sq = 0; sq < Sq; sq += Sqb) {
            long qbs = (Sq - sq >= Sqb ? Sqb : Sq - sq);
            int qid = (sq + Sqb > Sq) ? 1 : 0;
            long Sqb_aligned = attn_kern[qid*2].Sqb_aligned;
            T q_tmp[Sqb_aligned*H];
            T *q_ptr = QL[b][n][sq];
            if (Sqb_aligned != qbs) {
              attn_kern[qid*2].cpy_tpp(QL[b][n][sq], q_tmp);
              q_ptr = q_tmp;
            }
            float omax[qbs], osum[qbs], cmax[qbs], csum[qbs];
            for (int sk = 0; sk < Sk; sk += Skb) {
              long kbs = (Sk - sk >= Skb ? Skb : Sk_pad - sk);
              int kid = qid * 2 + ((sk + Skb > Sk) ? 1 : 0);
              auto &ak = attn_kern[kid];
              float AS[Sqb_aligned][kbs];
              Tv AST[Sqb_aligned][kbs];
              T*k_ptr = KL_TV[b][n][sk];
              T k_tmp[kbs*H];
              if (inline_trans) {
                ak.xform_tpp(KL[b][n][sk], k_tmp);
                k_ptr = k_tmp;
              }
              ak.a_gemm_tpp(q_ptr, k_ptr, AS[0], 1);
              for (int sq1 = 0; sq1 < qbs; sq1++) {
                auto qval = sq + sq1 + offset;
                for (int sk1 = qval + 1; sk1 < sk+kbs; sk1++) {
                  AS[sq1][sk1-sk] = -1e9f;
                }
              }
              ak.scale_tpp(AS[0], AS[0], one_by_sqrt_H);
              if (t_AM.numel() != 0) {
                if (am_is_2d)
                  ak.add_2dmask_tpp(&AM2[b][sq][sk], AS[0], AS[0]);
                else
                  ak.add_mask_tpp(&AM[b][sk], AS[0]);
              }
              float *pmax, *psum;
              if (sk == 0) {
                pmax = omax;
                psum = osum;
              } else {
                pmax = cmax;
                psum = csum;
              }
              ak.softmax_fwd_tpp(1, AS[0], AST[0], pmax, psum);
              Tv tmp[Sqb_aligned * H];
              Tv *v_ptr = VL_V[b][n][sk];
              Tv v_tmp[kbs*H];
              if (inline_trans && VBS != 1) {
                ak.vnni_tpp(VL[b][n][sk], v_tmp);
                v_ptr = v_tmp;
              }
              ak.c_gemm_tpp(AST[0], v_ptr, tmp, 1);
              if (sk == 0) {
                ak.cvt_tpp(tmp, CL[b][n][sq]);
              } else {
                ak.softmax_fixup(tmp, CL[b][n][sq], cmax, csum, omax, osum);
              }
            }
          }
        }
      }
    }
  }
  return t_CL;
}

template<typename cls>
struct LLMBlock : torch::CustomClassHolder {
 public:
   std::string name;
   at::Tensor t_dummy;
   at::Tensor t_dummy_int;
   caffe2::TypeMeta dt;
   caffe2::TypeMeta ldt;

  LLMBlock(std::string name, at::Tensor& t, at::Tensor& lt) : name(name), t_dummy(t.new_empty({0})), t_dummy_int(t.new_empty({0}, at::kLong)), dt(t.dtype()), ldt(lt.dtype()) { }

  std::vector<at::Tensor> forward_common(
      std::vector<at::Tensor> &t_inp,
      std::vector<at::Tensor> &t_cache,
      bool use_cache) {
    GlobalPass _gp(FWD);
    RECORD_FUNCTION(name, std::vector<c10::IValue>());
    std::vector<at::Tensor> ret;
    auto self = static_cast<cls*>(this);

    if (dt == at::kFloat && ldt == at::kFloat) {
      ret = self->template _forward<float, float>(
          t_inp,
          t_cache,
          use_cache);
    } else if (dt == at::kBFloat16 && ldt == at::kFloat) {
      ret = self->template _forward<bfloat16, float>(
          t_inp,
          t_cache,
          use_cache);
    } else if (dt == at::kBFloat16 && ldt == at::kBFloat16) {
      ret = self->template _forward<bfloat16, bfloat16>(
          t_inp,
          t_cache,
          use_cache);
    } else if (dt == at::kBFloat8 && ldt == at::kFloat) {
      ret = self->template _forward<bfloat8, float>(
          t_inp,
          t_cache,
          use_cache);
    } else if (dt == at::kBFloat8 && ldt == at::kBFloat16) {
      ret = self->template _forward<bfloat8, bfloat16>(
          t_inp,
          t_cache,
          use_cache);
    } else {
      std::cout << "Types: " << dt << "  " << ldt << std::endl;
      TPP_ASSERT(0, "Should not come here %s:%d\n", __FILE__, __LINE__);
    }
    // printf("Returning Layer \n");

    return ret;
  }

  template<typename T>
  std::vector<at::Tensor> self_mha(at::Tensor t_QL, at::Tensor t_KL, at::Tensor t_VL, at::Tensor t_am, std::vector<at::Tensor>& t_cache) {
    RECORD_SCOPE(mha, {t_QL, t_KL});
    auto self = static_cast<cls*>(this);
    auto t_key_past = this->t_dummy;
    auto t_value_past = this->t_dummy;
    auto t_beam_idx = this->t_dummy_int;
    auto t_offset = this->t_dummy_int;
    auto B = t_QL.size(0);
    auto S = t_QL.size(1);
    auto N = self->N;
    auto H = self->H;
    at::Tensor t_CL;
    long offset = 0;
    int csz = t_cache.size();
    if (csz > 0) t_key_past = t_cache[0];
    if (csz > 1) t_value_past = t_cache[1];
    if (csz > 2) t_beam_idx = t_cache[2].to(at::kLong);
    if (csz > 3) {
      t_offset = t_cache[3];
      offset = t_offset.item<long>();
      TPP_ASSERT(csz == 6, "Updated indirect kv_cache tuple should be of length 6\n");
      t_key_past = t_cache[4];
      t_value_past = t_cache[5];
    } else if (csz > 0) {
      offset = t_key_past.size(2);
    }

    t_QL = t_QL.view({B, S, N, H}).permute({0, 2, 1, 3}).contiguous();
    t_KL = t_KL.view({B, S, N, H}).permute({0, 2, 1, 3}).contiguous();
    t_VL = t_VL.view({B, S, N, H}).permute({0, 2, 1, 3}).contiguous();

    if (csz < 4) {
      if (t_key_past.numel() > 0) {
        t_KL = kv_concat<T>(t_key_past, t_KL, 2, t_beam_idx);
      }
      if (t_value_past.numel() > 0) {
        t_VL = kv_concat<T>(t_value_past, t_VL, 2, t_beam_idx);
      }

      t_CL = attn<T, T>(t_QL, t_KL, t_am, t_VL);
      t_CL = t_CL.view({B, N, S, H}).permute({0, 2, 1, 3}).contiguous().view({B, S, N * H});

      return {t_CL, t_KL, t_VL};

    } else if (offset == 0) {
      t_CL = attn<T, T>(t_QL, t_KL, t_am, t_VL);
      auto capacity = S + KV_CACHE_INC_SIZE;
      t_key_past = t_KL.new_zeros({B, N, capacity, H});
      t_value_past = t_VL.new_zeros({B, N, capacity, H});
      //t_beam_idx = t_beam_idx.new_zeros({capacity, B});
      t_beam_idx = at::arange(B).unsqueeze(0).expand({capacity, B}).contiguous();
      // if (my_rank == 0) std::cout << "t_beam_idx: " << t_beam_idx.sizes() << std::endl;
      t_offset = t_offset + S;
      t_key_past.slice(2, 0, S, 1).copy_(t_KL);
      t_value_past.slice(2, 0, S, 1).copy_(t_VL);
      t_CL = t_CL.view({B, N, S, H}).permute({0, 2, 1, 3}).contiguous().view({B, S, N * H});
      return {t_CL, t_KL, t_VL, t_beam_idx, t_offset, t_key_past, t_value_past};
      // printf("old offset = %d, new_offset = %ld\n", offset, t_offset.item<long>());
    } else {
      auto capacity = t_key_past.size(2);
      if (capacity <= offset) {
        printf("Warning: Reallocating kv cache, consider increasing KV_CACHE_INC_SIZE (%d)\n", KV_CACHE_INC_SIZE);
        auto new_capacity = offset + KV_CACHE_INC_SIZE;
        auto t_key_past_new = t_key_past.new_empty({B, N, new_capacity, H});
        t_key_past_new.slice(2, 0, offset, 1).copy_(t_key_past);
	t_key_past = t_key_past_new;

        auto t_value_past_new = t_value_past.new_empty({B, N, new_capacity, H});
        t_value_past_new.slice(2, 0, offset, 1).copy_(t_value_past);
	t_value_past = t_value_past_new;

        auto t_beam_idx_new = at::arange(B).unsqueeze(0).expand({new_capacity, B}).contiguous();
        t_beam_idx_new.slice(0, 0, offset, 1).copy_(t_beam_idx);
	t_beam_idx = t_beam_idx_new;
      }

      // if (my_rank == 0) std::cout << "t_beam_idx2: " << t_beam_idx.sizes() << std::endl;
      // std::cout << "t_key_past.shape:" << t_key_past.sizes() << std::endl;
      // std::cout << "t_beam_idx.shape:" << t_beam_idx.sizes() << std::endl;
      // std::cout << "t_offset:" << t_offset << std::endl;
      // std::cout << "B: " << B << " offset:" << offset << std::endl;
      auto t_new_beam_idx = t_beam_idx.new_empty({B, offset});
      auto beam_idx = GetVLAPtr<long>(t_new_beam_idx, {offset});
      auto b_ptr = GetVLAPtr<long>(t_beam_idx, {B});
      for(auto i = 0; i < B; i++) {
        beam_idx[i][offset-1] = b_ptr[offset-1][i];
        for(auto j = offset-2; j >= 0; j--) { //for the token of input, the target beam is alwarys 0
          beam_idx[i][j] = b_ptr[j][beam_idx[i][j+1]];
        }
      }
      t_CL = attn<T>(t_QL, t_KL, t_am, t_VL, t_key_past, t_value_past, beam_idx, offset);
      t_CL = t_CL.view({B, N, S, H}).permute({0, 2, 1, 3}).contiguous().view({B, S, N * H});
      t_offset = t_offset + 1;
      S = t_offset.item<long>();
      t_KL = t_key_past.slice(2, 0, S, 1);
      t_VL = t_value_past.slice(2, 0, S, 1);
      // printf("old offset = %d, new_offset = %ld\n", offset, t_offset.item<long>());
      return {t_CL, t_KL, t_VL, t_beam_idx, t_offset, t_key_past, t_value_past};
    }
  }
};

struct GPTJBlock : LLMBlock<GPTJBlock> {
 public:
  at::Tensor t_Wq, t_Wk, t_Wv, t_Wp;
  at::Tensor t_Wi, t_Wo;
  at::Tensor t_Bi, t_Bo;
  at::Tensor t_G, t_B;
  at::Tensor t_EP; // embed_positions
  float eps;
  long N, H;
  long max_positions, rotary_dim;

  GPTJBlock(
      std::vector<at::Tensor> params,
      double eps,
      long H,
      long max_positions,
      long rotary_dim)
      : LLMBlock("gptj_fwd", params[2], params[0]),
        eps(eps),
        H(H),
        max_positions(max_positions),
        rotary_dim(rotary_dim) {
    int i = 0;
    t_G =  params[i++]; // ln_gamma
    t_B =  params[i++]; // ln_beta

    t_Wq = params[i++]; // q_proj
    t_Wk = params[i++]; // k_proj
    t_Wv = params[i++]; // v_proj
    t_Wp = params[i++]; // out_proj

    t_Wi = params[i++]; // fc_in
    t_Bi = params[i++];

    t_Wo = params[i++]; // fc_out
    t_Bo = params[i++];

    t_EP = params[i++]; // embed_positions

    N = t_Wq.size(0) * t_Wq.size(3) / H;
    if (my_rank == 0) {
      std::cout << "my_size=" << my_size << " N=" << N << " H=" << H << std::endl;
    }
  }

  std::vector<at::Tensor> forward(
      std::vector<at::Tensor> t_inp,
      std::vector<at::Tensor> t_cache,
      bool use_cache) {
    return this->forward_common(t_inp, t_cache, use_cache);
  }

  template <typename T, typename LT = T>
  std::vector<at::Tensor> _forward(
      std::vector<at::Tensor>& t_inp,
      std::vector<at::Tensor>& t_cache,
      bool use_cache) {
    auto t_HS = t_inp[0];
    RECORD_SCOPE(pt_op, {t_HS});
    auto t_am = t_inp[1];
    auto t_pid = t_inp[2];
    auto sizes = t_HS.sizes();
    auto B = sizes[0];
    auto S = sizes[1];

    float scale = 1.0 / my_size;

    if (B*S / 64 > 4)
      large_cache_opt = true;
    else
      large_cache_opt = false;

    auto t_null = t_HS.new_empty({0});
    auto t_res = t_HS;
    t_HS = lyr_norm<T, LT>(t_HS, t_G, t_B, eps);

    auto t_QL = qkv_gemm<T>(t_HS, t_Wq, t_null);
    apply_rotary_pos_emb_gptj<T>(t_QL, t_EP, t_pid, N, H);

    auto t_KL = qkv_gemm<T>(t_HS, t_Wk, t_null);
    apply_rotary_pos_emb_gptj<T>(t_KL, t_EP, t_pid, N, H);

    auto t_VL = qkv_gemm<T>(t_HS, t_Wv, t_null);

    auto outputs = self_mha<T>(t_QL, t_KL, t_VL, t_am, t_cache);

    auto t_CL = outputs[0];
    auto t_SO = qkv_gemm<T>(t_CL, t_Wp, t_null);
    auto t_I = fc_gelu<T>(t_HS, t_Wi, t_Bi);
    auto t_Out = fc_add2_scale<T>(t_I, t_SO, t_res, t_Wo, t_Bo, scale);
    if (my_size > 1) {
      allreduce(t_Out);
    }

    outputs[0] = t_Out;

    if (use_cache) {
      return outputs;
    } else {
      return {t_Out};
    }
  }
};

struct OPTDecoderLayer : LLMBlock<OPTDecoderLayer> {
 public:
  at::Tensor t_Wq, t_Wk, t_Wv, t_Wp; // wt and bias for attn
  at::Tensor t_Bq, t_Bk, t_Bv, t_Bp;
  at::Tensor t_Wi, t_Wo; // wt and bias for fc1 and fc2
  at::Tensor t_Bi, t_Bo;
  at::Tensor t_G1, t_B1; // Gamma and Beta for attention layernorm
  at::Tensor t_G2, t_B2; // Gamma and Beta for MLP layernorm 
  float eps1, eps2;
  long N, H;
  bool do_layer_norm_before;

  OPTDecoderLayer(
      std::vector<at::Tensor> params,
      double eps1,
      double eps2,
      long H,
      bool do_layer_norm_before)
      : LLMBlock("opt_fwd", params[4], params[0]),
        eps1(eps1),
        eps2(eps2),
        H(H),
        do_layer_norm_before(do_layer_norm_before) {
    int i = 0;
    t_G1 = params[i++]; // ln_gamma, lnorm before attention
    t_B1 = params[i++]; // ln_beta
    t_G2 = params[i++]; // ln_gamma, lnorm before mlp
    t_B2 = params[i++]; // ln_beta

    t_Wq = params[i++]; // q_proj
    t_Bq = params[i++];
    t_Wk = params[i++]; // k_proj
    t_Bk = params[i++];
    t_Wv = params[i++]; // v_proj
    t_Bv = params[i++];
    t_Wp = params[i++]; // out_proj
    t_Bp = params[i++];

    t_Wi = params[i++]; // fc1
    t_Bi = params[i++];
    t_Wo = params[i++]; // fc2
    t_Bo = params[i++];

    N = t_Wq.size(0) * t_Wq.size(3) / H;
    if (my_rank == 0) {
      std::cout << "my_size=" << my_size << " N=" << N << " H=" << H << std::endl;
    }
  }

  std::vector<at::Tensor> forward(
      std::vector<at::Tensor> t_inp,
      std::vector<at::Tensor> t_cache,
      bool use_cache) {
    return this->forward_common(t_inp, t_cache, use_cache);
  }

  template <typename T, typename LT = T>
  std::vector<at::Tensor> _forward(
      std::vector<at::Tensor>& t_inp,
      std::vector<at::Tensor>& t_cache,
      bool use_cache) {
    auto t_HS = t_inp[0];
    RECORD_SCOPE(pt_op, {t_HS});
    auto t_am = t_inp[1];
    auto sizes = t_HS.sizes();
    auto B = sizes[0];
    auto S = sizes[1];

    float scale = 1.0 / my_size;

    if (B*S / 64 > 4)
      large_cache_opt = true;
    else
      large_cache_opt = false;

    auto t_null = t_HS.new_empty({0}); // at::Tensor().to(t_HS.dtype());

    auto t_res = t_HS;
    if (do_layer_norm_before) {
      t_HS = lyr_norm<T, LT>(t_HS, t_G1, t_B1, eps1);
    }

    auto t_QL = qkv_gemm<T>(t_HS, t_Wq, t_Bq);
    auto t_KL = qkv_gemm<T>(t_HS, t_Wk, t_Bk);
    auto t_VL = qkv_gemm<T>(t_HS, t_Wv, t_Bv);

    auto outputs = self_mha<T>(t_QL, t_KL, t_VL, t_am, t_cache);

    auto t_CL = outputs[0];

    t_HS = fc_add_scale<T>(t_CL, t_res, t_Wp, t_Bp, scale);
    if (my_size > 1) {
      allreduce(t_HS);
    }

    if (!do_layer_norm_before) {
      t_HS = lyr_norm<T, LT>(t_HS, t_G1, t_B1, eps1);
    }

    t_res = t_HS;

    if (do_layer_norm_before) {
      t_HS = lyr_norm<T, LT>(t_HS, t_G2, t_B2, eps2);
    }

    t_HS = fc_relu<T>(t_HS, t_Wi, t_Bi);
    t_HS = fc_add_scale<T>(t_HS, t_res, t_Wo, t_Bo, scale);

    if (my_size > 1) {
      allreduce(t_HS);
    }

    if (!do_layer_norm_before) {
      t_HS = lyr_norm<T, LT>(t_HS, t_G2, t_B2, eps2);
    }

    outputs[0] = t_HS;

    if (use_cache) {
      return outputs;
    } else {
      return {t_HS};
    }
  }
};

struct LlamaDecoderLayer : LLMBlock<LlamaDecoderLayer> {
 public:
  at::Tensor t_Wq, t_Wk, t_Wv, t_Wp;
  at::Tensor t_Wg, t_Wu, t_Wd;
  at::Tensor t_Gi, t_Gpa;
  at::Tensor t_EP; // embed_positions
  float eps;
  long N, H;
  long max_positions, rotary_dim;

  LlamaDecoderLayer(
      std::vector<at::Tensor> params,
      double eps,
      long H,
      long max_positions,
      long rotary_dim)
      : LLMBlock("llama_fwd", params[1], params[0]),
        eps(eps),
        H(H),
        max_positions(max_positions),
        rotary_dim(rotary_dim) {
    int i = 0;
    t_Gi = params[i++]; // input_ln_gamma

    t_Wq = params[i++]; // q_proj
    t_Wk = params[i++]; // k_proj
    t_Wv = params[i++]; // v_proj
    t_Wp = params[i++]; // out_proj

    t_Gpa = params[i++]; // post_attention_ln_gamma

    t_Wg = params[i++]; // fc_gate
    t_Wu = params[i++]; // fc_up
    t_Wd = params[i++]; // fc_down

    t_EP = params[i++]; // embed_positions

    N = t_Wq.size(0) * t_Wq.size(3) / H;
    if (my_rank == 0) {
      std::cout << "my_size=" << my_size << " N=" << N << " H=" << H << std::endl;
    }
  }

  std::vector<at::Tensor> forward(
      std::vector<at::Tensor> t_inp,
      std::vector<at::Tensor> t_cache,
      bool use_cache) {
    return this->forward_common(t_inp, t_cache, use_cache);
  }

  template <typename T, typename LT = T>
  std::vector<at::Tensor> _forward(
      std::vector<at::Tensor>& t_inp,
      std::vector<at::Tensor>& t_cache,
      bool use_cache) {
    auto t_HS = t_inp[0];
    RECORD_SCOPE(pt_op, {t_HS});
    auto t_am = t_inp[1];
    auto t_pid = t_inp[2];
    auto sizes = t_HS.sizes();
    auto B = sizes[0];
    auto S = sizes[1];

    float scale = 1.0 / my_size;

    if (B*S / 64 > 4)
      large_cache_opt = true;
    else
      large_cache_opt = false;

    auto t_null = t_HS.new_empty({0});
    auto t_res = t_HS;
    t_HS = llama_rms_norm<T, LT>(t_HS, t_Gi, eps);

    auto t_QL = qkv_gemm<T>(t_HS, t_Wq, t_null);
    apply_rotary_pos_emb_llama<T>(t_QL, t_EP, t_pid, N, H);

    auto t_KL = qkv_gemm<T>(t_HS, t_Wk, t_null);
    apply_rotary_pos_emb_llama<T>(t_KL, t_EP, t_pid, N, H);

    auto t_VL = qkv_gemm<T>(t_HS, t_Wv, t_null);

    auto outputs = self_mha<T>(t_QL, t_KL, t_VL, t_am, t_cache);

    auto t_CL = outputs[0];

    auto t_SO = fc_add_scale<T>(t_CL, t_res, t_Wp, t_null, scale);
    if (my_size > 1) {
      allreduce(t_SO);
    }

    t_res = t_SO;

    t_HS = llama_rms_norm<T, LT>(t_SO, t_Gpa, eps);

    auto t_I = fc_silu<T>(t_HS, t_Wg, t_null);
    t_I = fc_mul<T>(t_HS, t_I, t_Wu, t_null);
    auto t_Out = fc_add_scale<T>(t_I, t_res, t_Wd, t_null, scale);
    if (my_size > 1) {
      allreduce(t_Out);
    }

    outputs[0] = t_Out;

    if (use_cache) {
      return outputs;
    } else {
      return {t_Out};
    }
  }
};

static void apply_rotary_pos_emb_gptj_wrap(
    at::Tensor t_in,
    at::Tensor t_emb_pos,
    at::Tensor t_pos,
    long N,
    long H) {
  GlobalPass _gp(FWD);

  auto dt = t_in.dtype();
  if (dt == at::kFloat) {
    apply_rotary_pos_emb_gptj<float>(t_in, t_emb_pos, t_pos, N, H);
  } else if (dt == at::kBFloat16) {
    apply_rotary_pos_emb_gptj<bfloat16>(t_in, t_emb_pos, t_pos, N, H);
  } else if (dt == at::kBFloat8) {
    apply_rotary_pos_emb_gptj<bfloat8>(t_in, t_emb_pos, t_pos, N, H);
  } else {
    TPP_ASSERT(0, "Should not come here %s:%d\n", __FILE__, __LINE__);
  }
}

static at::Tensor lyr_norm_wrap(
    at::Tensor t_in,
    at::Tensor t_gamma,
    at::Tensor t_beta,
    double eps) {
  GlobalPass _gp(FWD);
  auto dt = t_in.dtype();
  auto ldt = t_gamma.dtype();
  auto t_out = at::empty_like(t_in);

  if (dt == at::kFloat && ldt == at::kFloat) {
    lyr_norm<float, float>(t_in, t_gamma, t_beta, t_out, eps);
  } else if (dt == at::kBFloat16 && ldt == at::kFloat) {
    lyr_norm<bfloat16, float>(t_in, t_gamma, t_beta, t_out, eps);
  } else if (dt == at::kBFloat16 && ldt == at::kBFloat16) {
    lyr_norm<bfloat16, bfloat16>(t_in, t_gamma, t_beta, t_out, eps);
  } else if (dt == at::kBFloat8 && ldt == at::kFloat) {
    lyr_norm<bfloat8, float>(t_in, t_gamma, t_beta, t_out, eps);
  } else if (dt == at::kBFloat8 && ldt == at::kBFloat8) {
    lyr_norm<bfloat8, bfloat8>(t_in, t_gamma, t_beta, t_out, eps);
  } else {
    TPP_ASSERT(0, "Should not come here %s:%d\n", __FILE__, __LINE__);
  }
  return t_out;
}

static at::Tensor fc_plain_wrap(
    at::Tensor t_in,
    at::Tensor t_wt,
    at::Tensor t_bias,
    long parallel_dim,
    std::vector<long> split_sizes) {
  GlobalPass _gp(FWD);
  if (parallel_dim == 1) {
    // t_in = t_in.chunk(my_size, -1)[my_rank].contiguous();
    t_in = t_in.split(split_sizes, -1)[my_rank].contiguous();
  }
	
  auto sizes = t_in.sizes().vec();
  auto wt_sizes = t_wt.sizes();
  sizes[2] = wt_sizes[0] * wt_sizes[3];

  auto t_out = t_in.new_empty(sizes);
  //std::cout << "YYY " << t_out.dtype() << "  " << t_in.dtype() << std::endl;
  auto dt = t_wt.dtype();
  if (dt == at::kFloat) {
    fc_plain<float>(t_in, t_wt, t_bias, t_out);
  } else if (dt == at::kBFloat16) {
    fc_plain<bfloat16>(t_in, t_wt, t_bias, t_out);
  } else if (dt == at::kBFloat8) {
    fc_plain<bfloat8>(t_in, t_wt, t_bias, t_out);
  } else {
    TPP_ASSERT(0, "Should not come here %s:%d\n", __FILE__, __LINE__);
  }
  if (my_size > 1) {
    if (parallel_dim == 0) {
      t_out = allgather(t_out, split_sizes);
    } else if (parallel_dim == 1) {
      allreduce(t_out);
    }
  }
  return t_out;
}

static at::Tensor fc_add2_scale_wrap(
    at::Tensor t_in,
    at::Tensor t_in1,
    at::Tensor t_in2,
    at::Tensor t_wt,
    at::Tensor t_bias,
    double scale) {
  GlobalPass _gp(FWD);
  auto t_out = at::empty_like(t_in1);
  auto dt = t_wt.dtype();
  if (dt == at::kFloat) {
    fc_add2_scale<float>(t_in, t_in1, t_in2, t_wt, t_bias, t_out, scale);
  } else if (dt == at::kBFloat16) {
    fc_add2_scale<bfloat16>(t_in, t_in1, t_in2, t_wt, t_bias, t_out, scale);
  } else if (dt == at::kBFloat8) {
    fc_add2_scale<bfloat8>(t_in, t_in1, t_in2, t_wt, t_bias, t_out, scale);
  } else {
    TPP_ASSERT(0, "Should not come here %s:%d\n", __FILE__, __LINE__);
  }
  return t_out;
}

static at::Tensor fc_gelu_wrap(
    at::Tensor t_in,
    at::Tensor t_wt,
    at::Tensor t_bias) {
  GlobalPass _gp(FWD);
  auto sizes = t_in.sizes().vec();
  auto wt_sizes = t_wt.sizes();
  sizes[2] = wt_sizes[0] * wt_sizes[3];

  auto t_out = t_in.new_empty(sizes);

  auto dt = t_wt.dtype();
  if (dt == at::kFloat) {
    fc_gelu<float>(t_in, t_wt, t_bias, t_out);
  } else if (dt == at::kBFloat16) {
    fc_gelu<bfloat16>(t_in, t_wt, t_bias, t_out);
  } else if (dt == at::kBFloat8) {
    fc_gelu<bfloat8>(t_in, t_wt, t_bias, t_out);
  } else {
    TPP_ASSERT(0, "Should not come here %s:%d\n", __FILE__, __LINE__);
  }
  return t_out;
}

REGISTER_SUBMODULE(_fused_llm_infer, m) {
  m.def("layer_norm", &lyr_norm_wrap, "TPP layer norm");
  m.def("fc_gelu", &fc_gelu_wrap, "TPP fc_gelu");
  m.def("fc_add2_scale", &fc_add2_scale_wrap, "TPP fc_add2_scale");
  m.def("fc_plain", &fc_plain_wrap, "TPP fc_plain");
  m.def("set_pg", &set_pg);
  m.def(
      "apply_rotary_pos_emb_gptj",
      &apply_rotary_pos_emb_gptj_wrap,
      "TPP apply_rotary_pos_emb_gptj");
  py::class_<GPTJBlock>(m, "GPTJBlock")
      .def(py::init<std::vector<at::Tensor>, double, long, long, long>())
      .def("forward", &GPTJBlock::forward);
  py::class_<OPTDecoderLayer>(m, "OPTDecoderLayer")
      .def(py::init<std::vector<at::Tensor>, double, double, long, bool>())
      .def("forward", &OPTDecoderLayer::forward);
  py::class_<LlamaDecoderLayer>(m, "LlamaDecoderLayer")
      .def(py::init<std::vector<at::Tensor>, double, long, long, long>())
      .def("forward", &LlamaDecoderLayer::forward);
}

TORCH_LIBRARY(tpp_llm, m) {
  m.def("layer_norm", &lyr_norm_wrap);
  m.def("fc_gelu", &fc_gelu_wrap);
  m.def("fc_add2_scale", &fc_add2_scale_wrap);
  m.def("fc_plain", &fc_plain_wrap);
  m.def("set_pg", &set_pg);
  m.class_<GPTJBlock>("GPTJBlock")
      .def(torch::init<std::vector<at::Tensor>, double, long, long, long>())
      .def("forward", &GPTJBlock::forward);
  m.class_<OPTDecoderLayer>("OPTDecoderLayer")
      .def(torch::init<std::vector<at::Tensor>, double, double, long, bool>())
      .def("forward", &OPTDecoderLayer::forward);
  m.class_<LlamaDecoderLayer>("LlamaDecoderLayer")
      .def(torch::init<std::vector<at::Tensor>, double, long, long, long>())
      .def("forward", &LlamaDecoderLayer::forward);
}
