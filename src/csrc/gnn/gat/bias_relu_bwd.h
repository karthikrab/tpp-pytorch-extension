/******************************************************************************
 * Copyright (c) 2022 Intel Corporation - All rights reserved.                *
 *                                                                            *
 * For information on the license, see the LICENSE file.                      *
 * Further information: https://github.com/libxsmm/tpp-pytorch-extension/     *
 * SPDX-License-Identifier: BSD-3-Clause                                      *
 ******************************************************************************/
/* Authors: Ramanarayan Mohanty, Sasikanth Avancha (Intel Corp.)
 ******************************************************************************/

RECORD_FUNCTION("bias_relu_bwd", std::vector<c10::IValue>());

int i = 0;
auto t_grad_out = inputs[i++].contiguous();
auto t_relu_mask = inputs[i++];

auto in_sizes = t_grad_out.sizes();
auto N = in_sizes[0];
long K = in_sizes[1];
auto dK = (K + 15) / 16;

at::Tensor t_grad_out_f32 = t_grad_out;
if (t_grad_out.dtype() == at::kBFloat16)
  t_grad_out_f32 = at::empty({N, K});

auto t_grad_in = t_grad_out.new_empty({N, K});

auto t_grad_bias = at::empty({K});
auto grad_out = GetVLAPtr<T>(t_grad_out, {K});
auto relu_mask = GetVLAPtr<short>(t_relu_mask, {dK});
auto grad_bias = GetVLAPtr<float>(t_grad_bias, {K});
auto grad_out_f32 = GetVLAPtr<float>(t_grad_out_f32, {K});
auto grad_in = GetVLAPtr<T>(t_grad_in, {K});

auto relu_bwd_tpp = SCOPEIT((ReLUBwdTPP<T, float>(1, K, true)), ACT);
auto grad_bias_tpp = SCOPEIT(GradBiasTPP<float>(1, K), BIAS);
auto cvt_tpp = SCOPEIT((ConvertTPP<float, T>(1, K)), EW_COPY);
auto set_zero_tpp = SCOPEIT(SetZeroTPP<float>(K), EW_ZERO);

int threads = omp_get_max_threads();

{
  RECORD_SCOPE(gdo_bias_relu, {t_grad_out});
  {
    tensor_set_zero(1, K, t_grad_bias);
    float* bias_ptrs[threads];
    RECORD_FUNCTION("parallel_for", std::vector<c10::IValue>());
#pragma omp parallel  
    {
      int tid = omp_get_thread_num();
      float prv_grad_bias[1][K];
      bias_ptrs[tid] = prv_grad_bias[0];
      set_zero_tpp(prv_grad_bias[0]);
#pragma omp for
      for (int n = 0; n < N; n++) {
          relu_bwd_tpp(grad_out[n], grad_out_f32[n], (T*)NULL, relu_mask[n]);
          grad_bias_tpp(grad_out_f32[n], prv_grad_bias[0]);
          cvt_tpp(grad_out_f32[n], grad_in[n]);
      }
      omp_reduce_buf(threads, K, bias_ptrs, grad_bias[0]);
    }
  }
}

return {t_grad_in, t_grad_bias};
