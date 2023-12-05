/******************************************************************************
 * Copyright (c) 2022 Intel Corporation - All rights reserved.                *
 *                                                                            *
 * For information on the license, see the LICENSE file.                      *
 * Further information: https://github.com/libxsmm/tpp-pytorch-extension/     *
 * SPDX-License-Identifier: BSD-3-Clause                                      *
 ******************************************************************************/
/* Author: Dhiraj Kalamkar (Intel Corp.)
 ******************************************************************************/

#ifndef _EXT_TPP_H_
#define _EXT_TPP_H_

#include "timing.h"
#include "xsmm_functors.h"

namespace tpp {

template <typename Tin, typename Tout>
class BrgemmExtTPP {
 public:
  BrgemmExtTPP() {}
  BrgemmExtTPP(
      long M,
      long N,
      long K,
      long str_a,
      long str_b,
      float beta, // = 1.0,
      XformTPP::XFORM_TYPE c_trans, // = XformTPP::XFORM_NONE_TPP,
      int a_trans, // = 0,
      int unroll_hint)
      : M(M),
        N(N),
        K(K),
        beta(beta),
        c_trans(c_trans),
        brgemm(),
        xform(),
        add() {
    // auto dt_in = XsmmDtype<Tin>();
    auto dt_out = XsmmDtype<Tout>();
    if (dt_out == LIBXSMM_DATATYPE_F32 && c_trans == XformTPP::XFORM_N2V_TPP) {
      printf(
          "Warning: reseting c_trans flag from N2V to None for FP32 output\n");
      c_trans = XformTPP::XFORM_NONE_TPP;
    }
    auto beta_ = beta;

    if (c_trans != XformTPP::XFORM_NONE_TPP) {
      beta_ = 0.0;
      xform = XformExtTPP<Tout>(M, N, c_trans);
    }
    brgemm = BrgemmTPP<Tin, Tout>(
        M, N, K, str_a, str_b, beta_, a_trans, unroll_hint);
    if (beta_ != beta) {
      add = AddTPP<Tout, Tout>(M, N);
    }
    xform_type = c_trans == XformTPP::XFORM_N2V_TPP ? VNNI : XPOSE;
  }

  void operator()(
      Tin* A,
      Tin* B,
      Tout* C,
      long count,
      bool no_tile_cfg = false) {
    if (c_trans == XformTPP::XFORM_NONE_TPP) {
      ScopedTimer _t(BRGEMM, 2 * M * N * K * count);
      brgemm(A, B, C, count, no_tile_cfg);
    } else {
      Tout tmp_C[M * N];
      {
        ScopedTimer _t(BRGEMM, 2 * M * N * K * count);
        brgemm(A, B, tmp_C, count, no_tile_cfg);
      }
      if (beta == 0.0) {
        ScopedTimer _t(xform_type);
        xform(tmp_C, C);
      } else {
        Tout tmp[M * N];
        {
          ScopedTimer _t(xform_type);
          xform(tmp_C, tmp);
        }
        {
          ScopedTimer _t(EW_ADD);
          add(C, tmp, C);
        }
      }
    }
  }

  void ref(Tin* A, Tin* B, Tout* C, long count, bool no_tile_cfg = false) {
    if (c_trans == XformTPP::XFORM_NONE_TPP) {
      ScopedTimer _t(BRGEMM, 2 * M * N * K * count);
      brgemm.ref(A, B, C, count, no_tile_cfg);
    } else {
      Tout tmp_C[M * N];
      {
        ScopedTimer _t(BRGEMM, 2 * M * N * K * count);
        brgemm.ref(A, B, tmp_C, count, no_tile_cfg);
      }
      if (beta == 0.0) {
        ScopedTimer _t(xform_type);
        xform.ref(tmp_C, C);
      } else {
        Tout tmp[M * N];
        {
          ScopedTimer _t(xform_type);
          xform.ref(tmp_C, tmp);
        }
        {
          ScopedTimer _t(EW_ADD);
          add.ref(C, tmp, C);
        }
      }
    }
  }

  void config() {
    brgemm.config();
  }

  void release() {
    brgemm.release();
  }

 private:
  long M, N, K;
  float beta;
  XformTPP::XFORM_TYPE c_trans;
  BrgemmTPP<Tin, Tout> brgemm;
  XformExtTPP<Tout> xform;
  AddTPP<Tout, Tout> add;
  DebugTimer xform_type;
};

} // namespace tpp

template <typename Tin, typename Tout, int impl>
class ScopedTPP<tpp::SpmmTPP<Tin, Tout>, impl> {
 public:
  ScopedTPP() {}
  ScopedTPP(tpp::SpmmTPP<Tin, Tout> func) : func(std::move(func)) {}
  void operator()(
      Tin* A,
      const tensor_bcsc_t& B,
      unsigned long long B_n_cols,
      unsigned long long B_col_offs,
      Tout* C,
      bool no_tile_cfg = false) {
    ScopedTimer _t(
        BRGEMM, func.flops(), func.bytes_C_moved() + func.bytes_AB_moved());
    if (impl == 0) {
      func(A, B, B_n_cols, B_col_offs, C, no_tile_cfg);
    } /*else if (impl == 1) {
      func.ref(A, B, C, count, no_tile_cfg);
    } */ else {
      printf("invalid impl requested\n");
      exit(1);
    }
  }

  void config() {
    func.config();
  }

  void release() {
    func.release();
  }

 private:
  tpp::SpmmTPP<Tin, Tout> func;
};

template <typename Tin, typename Tout, int impl>
class ScopedTPP<tpp::BrgemmTPP<Tin, Tout>, impl> {
 public:
  ScopedTPP() {}
  ScopedTPP(tpp::BrgemmTPP<Tin, Tout> func) : func(std::move(func)) {}
  void operator()(
      Tin* A,
      Tin* B,
      Tout* C,
      long count,
      bool no_tile_cfg = false) {
    ScopedTimer _t(
        BRGEMM,
        func.flops() * count,
        func.bytes_C_moved() + func.bytes_AB_moved() * count);
    if (impl == 0) {
      func(A, B, C, count, no_tile_cfg);
    } else if (impl == 1) {
      func.ref(A, B, C, count, no_tile_cfg);
    } else {
      printf("invalid impl requested\n");
      exit(1);
    }
  }

  void config() {
    func.config();
  }

  void release() {
    func.release();
  }

 private:
  tpp::BrgemmTPP<Tin, Tout> func;
};

template <typename Tin, typename Tout, int impl>
class ScopedTPP<tpp::GemmTPP<Tin, Tout>, impl> {
 public:
  ScopedTPP() {}
  ScopedTPP(tpp::GemmTPP<Tin, Tout> func) : func(std::move(func)) {}
  void operator()(
      Tin* A,
      Tin* B,
      Tout* C,
      char* B_bitmap,
      bool no_tile_cfg = false) {
    ScopedTimer _t(
        BRGEMM, func.flops(), func.bytes_C_moved() + func.bytes_AB_moved());
    if (impl == 0) {
      func(A, B, C, B_bitmap, no_tile_cfg);
    } else {
      printf("invalid impl requested\n");
      exit(1);
    }
  }

  void config() {
    func.config();
  }

  void release() {
    func.release();
  }

 private:
  tpp::GemmTPP<Tin, Tout> func;
};

template <typename Tin, typename Tout, int impl>
class ScopedTPP<tpp::BrgemmExtTPP<Tin, Tout>, impl> {
 public:
  ScopedTPP() {}
  ScopedTPP(tpp::BrgemmExtTPP<Tin, Tout> func) : func(std::move(func)) {}
  void operator()(
      Tin* A,
      Tin* B,
      Tout* C,
      long count,
      bool no_tile_cfg = false) {
    if (impl == 0) {
      func(A, B, C, count, no_tile_cfg);
    } else if (impl == 1) {
      func.ref(A, B, C, count, no_tile_cfg);
    } else {
      printf("invalid impl requested\n");
      exit(1);
    }
  }

  void config() {
    func.config();
  }

  void release() {
    func.release();
  }

 private:
  tpp::BrgemmExtTPP<Tin, Tout> func;
};

// #define TCBrgemmTPP BrgemmTPP

#endif // _EXT_TPP_H_
