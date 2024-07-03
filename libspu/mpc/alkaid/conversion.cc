// Copyright 2021 Ant Group Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "libspu/mpc/alkaid/conversion.h"

#include <functional>
#include <iostream>
#include <utility>

#include "yacl/utils/platform_utils.h"

#include "libspu/core/parallel_utils.h"
#include "libspu/core/prelude.h"
#include "libspu/core/trace.h"
#include "libspu/mpc/ab_api.h"
#include "libspu/mpc/alkaid/type.h"
#include "libspu/mpc/alkaid/value.h"
#include "libspu/mpc/common/communicator.h"
#include "libspu/mpc/common/prg_state.h"
#include "libspu/mpc/common/pv2k.h"
#include "libspu/mpc/utils/ring_ops.h"

// TODO: it shows incorrect result that defines EQ_USE_PRG_STATE and undefines EQ_USE_OFFLINE. Fix it.
// #define EQ_USE_OFFLINE
// #define EQ_USE_PRG_STATE

namespace spu::mpc::alkaid {

static NdArrayRef wrap_add_bb(SPUContext* ctx, const NdArrayRef& x,
                              const NdArrayRef& y) {
  SPU_ENFORCE(x.shape() == y.shape());
  return UnwrapValue(add_bb(ctx, WrapValue(x), WrapValue(y)));
}

// Reference:
// ALKAID: A Mixed Protocol Framework for Machine Learning
// P16 5.3 Share Conversions, Bit Decomposition
// https://eprint.iacr.org/2018/403.pdf
//
// Latency: 2 + log(nbits) from 1 rotate and 1 ppa.
NdArrayRef A2B::proc(KernelEvalContext* ctx, const NdArrayRef& in) const {
  return A2BMultiFanIn(ctx, in);
}

NdArrayRef B2ASelector::proc(KernelEvalContext* ctx,
                             const NdArrayRef& in) const {
  const auto* in_ty = in.eltype().as<BShrTy>();
  const size_t in_nbits = in_ty->nbits();

  // PPA: latency=3+log(k), comm = 2*k*log(k) +3k
  // OT:  latency=2, comm=K*K
  if (in_nbits <= 8) {
    return B2AByOT().proc(ctx, in);
  } else {
    return B2AByPPA().proc(ctx, in);
  }
}

// Reference:
// 5.3 Share Conversions
// https://eprint.iacr.org/2018/403.pdf
//
// In the semi-honest setting, this can be further optimized by having party 2
// provide (−x2−x3) as private input and compute
//   [x1]B = [x]B + [-x2-x3]B
// using a parallel prefix adder. Regardless, x1 is revealed to parties
// 1,3 and the final sharing is defined as
//   [x]A := (x1, x2, x3)
// Overall, the conversion requires 1 + log k rounds and k + k log k gates.
//
// TODO: convert to single share, will reduce number of rotate.
NdArrayRef B2AByPPA::proc(KernelEvalContext* ctx, const NdArrayRef& in) const {
  const auto field = ctx->getState<Z2kState>()->getDefaultField();
  const auto* in_ty = in.eltype().as<BShrTy>();
  const size_t in_nbits = in_ty->nbits();

  SPU_ENFORCE(in_nbits <= SizeOf(field) * 8, "invalid nbits={}", in_nbits);
  const auto out_ty = makeType<AShrTy>(field);
  NdArrayRef out(out_ty, in.shape());

  auto numel = in.numel();

  if (in_nbits == 0) {
    // special case, it's known to be zero.
    DISPATCH_ALL_FIELDS(field, "_", [&]() {
      NdArrayView<std::array<ring2k_t, 2>> _out(out);
      pforeach(0, numel, [&](int64_t idx) {
        _out[idx][0] = 0;
        _out[idx][1] = 0;
      });
    });
    return out;
  }

  auto* comm = ctx->getState<Communicator>();
  auto* prg_state = ctx->getState<PrgState>();

  DISPATCH_UINT_PT_TYPES(in_ty->getBacktype(), "_", [&]() {
    using bshr_t = std::array<ScalarT, 2>;
    NdArrayView<bshr_t> _in(in);

    DISPATCH_ALL_FIELDS(field, "_", [&]() {
      using ashr_el_t = ring2k_t;
      using ashr_t = std::array<ashr_el_t, 2>;

      // first expand b share to a share length.
      const auto expanded_ty = makeType<BShrTy>(
          calcBShareBacktype(SizeOf(field) * 8), SizeOf(field) * 8);
      NdArrayRef x(expanded_ty, in.shape());
      NdArrayView<ashr_t> _x(x);

      pforeach(0, numel, [&](int64_t idx) {
        const auto& v = _in[idx];
        _x[idx][0] = v[0];
        _x[idx][1] = v[1];
      });

      // P1 & P2 local samples ra, note P0's ra is not used.
      std::vector<ashr_el_t> ra0(numel);
      std::vector<ashr_el_t> ra1(numel);
      std::vector<ashr_el_t> rb0(numel);
      std::vector<ashr_el_t> rb1(numel);

      prg_state->fillPrssPair(ra0.data(), ra1.data(), ra0.size(),
                              PrgState::GenPrssCtrl::Both);
      prg_state->fillPrssPair(rb0.data(), rb1.data(), rb0.size(),
                              PrgState::GenPrssCtrl::Both);

      pforeach(0, numel, [&](int64_t idx) {
        const auto zb = rb0[idx] ^ rb1[idx];
        if (comm->getRank() == 1) {
          rb0[idx] = zb ^ (ra0[idx] + ra1[idx]);
        } else {
          rb0[idx] = zb;
        }
      });
      rb1 = comm->rotate<ashr_el_t>(rb0, "b2a.rand");  // comm => 1, k

      // compute [x+r]B
      NdArrayRef r(expanded_ty, in.shape());
      NdArrayView<ashr_t> _r(r);
      pforeach(0, numel, [&](int64_t idx) {
        _r[idx][0] = rb0[idx];
        _r[idx][1] = rb1[idx];
      });

      // comm => log(k) + 1, 2k(logk) + k
      auto x_plus_r = wrap_add_bb(ctx->sctx(), x, r);
      NdArrayView<ashr_t> _x_plus_r(x_plus_r);

      // reveal
      std::vector<ashr_el_t> x_plus_r_2(numel);
      if (comm->getRank() == 0) {
        x_plus_r_2 = comm->recv<ashr_el_t>(2, "reveal.x_plus_r.to.P0");
      } else if (comm->getRank() == 2) {
        std::vector<ashr_el_t> x_plus_r_0(numel);
        pforeach(0, numel,
                 [&](int64_t idx) { x_plus_r_0[idx] = _x_plus_r[idx][0]; });
        comm->sendAsync<ashr_el_t>(0, x_plus_r_0, "reveal.x_plus_r.to.P0");
      }

      // P0 hold x+r, P1 & P2 hold -r, reuse ra0 and ra1 as output
      auto self_rank = comm->getRank();
      pforeach(0, numel, [&](int64_t idx) {
        if (self_rank == 0) {
          const auto& x_r_v = _x_plus_r[idx];
          ra0[idx] = x_r_v[0] ^ x_r_v[1] ^ x_plus_r_2[idx];
        } else {
          ra0[idx] = -ra0[idx];
        }
      });

      ra1 = comm->rotate<ashr_el_t>(ra0, "b2a.rotate");

      NdArrayView<ashr_t> _out(out);
      pforeach(0, numel, [&](int64_t idx) {
        _out[idx][0] = ra0[idx];
        _out[idx][1] = ra1[idx];
      });
    });
  });
  return out;
}

template <typename T>
static std::vector<bool> bitDecompose(const NdArrayRef& in, size_t nbits) {
  auto numel = in.numel();
  // decompose each bit of an array of element.
  // FIXME: this is not thread-safe.
  std::vector<bool> dep(numel * nbits);

  NdArrayView<T> _in(in);

  pforeach(0, numel, [&](int64_t idx) {
    const auto& v = _in[idx];
    for (size_t bit = 0; bit < nbits; bit++) {
      size_t flat_idx = idx * nbits + bit;
      dep[flat_idx] = static_cast<bool>((v >> bit) & 0x1);
    }
  });
  return dep;
}

template <typename T>
static std::vector<T> bitCompose(absl::Span<T const> in, size_t nbits) {
  SPU_ENFORCE(in.size() % nbits == 0);
  std::vector<T> out(in.size() / nbits, 0);
  pforeach(0, out.size(), [&](int64_t idx) {
    for (size_t bit = 0; bit < nbits; bit++) {
      size_t flat_idx = idx * nbits + bit;
      out[idx] += in[flat_idx] << bit;
    }
  });
  return out;
}

// Reference:
// 5.4.1 Semi-honest Security
// https://eprint.iacr.org/2018/403.pdf
//
// Latency: 2.
//
// Alkaid paper algorithm reference.
//
// P1 & P3 locally samples c1.
// P2 & P3 locally samples c3.
//
// P3 (the OT sender) defines two messages.
//   m{i} := (i^b1^b3)−c1−c3 for i in {0, 1}
// P2 (the receiver) defines his input to be b2 in order to learn the message
//   c2 = m{b2} = (b2^b1^b3)−c1−c3 = b − c1 − c3.
// P1 (the helper) also knows b2 and therefore the three party OT can be used.
//
// However, to make this a valid 2-out-of-3 secret sharing, P1 needs to learn
// c2.
//
// Current implementation
// - P2 could send c2 resulting in 2 rounds and 4k bits of communication.
//
// TODO:
// - Alternatively, the three-party OT procedure can be repeated (in parallel)
// with again party 3 playing the sender with inputs m0,mi so that party 1
// (the receiver) with input bit b2 learns the message c2 (not m[b2]) in the
// first round, totaling 6k bits and 1 round.
NdArrayRef B2AByOT::proc(KernelEvalContext* ctx, const NdArrayRef& in) const {
  const auto field = ctx->getState<Z2kState>()->getDefaultField();
  const auto* in_ty = in.eltype().as<BShrTy>();
  const size_t in_nbits = in_ty->nbits();

  SPU_ENFORCE(in_nbits <= SizeOf(field) * 8, "invalid nbits={}", in_nbits);

  NdArrayRef out(makeType<AShrTy>(field), in.shape());
  auto numel = in.numel();

  if (in_nbits == 0) {
    // special case, it's known to be zero.
    DISPATCH_ALL_FIELDS(field, "_", [&]() {
      NdArrayView<std::array<ring2k_t, 2>> _out(out);
      pforeach(0, numel, [&](int64_t idx) {
        _out[idx][0] = 0;
        _out[idx][1] = 0;
      });
    });
    return out;
  }

  auto* comm = ctx->getState<Communicator>();
  auto* prg_state = ctx->getState<PrgState>();

  // P0 as the helper/dealer, helps to prepare correlated randomness.
  // P1, P2 as the receiver and sender of OT.
  size_t pivot;
  prg_state->fillPubl(absl::MakeSpan(&pivot, 1));
  size_t P0 = pivot % 3;
  size_t P1 = (pivot + 1) % 3;
  size_t P2 = (pivot + 2) % 3;

  DISPATCH_UINT_PT_TYPES(in_ty->getBacktype(), "_", [&]() {
    using bshr_el_t = ScalarT;
    using bshr_t = std::array<bshr_el_t, 2>;
    NdArrayView<bshr_t> _in(in);

    DISPATCH_ALL_FIELDS(field, "_", [&]() {
      using ashr_el_t = ring2k_t;
      using ashr_t = std::array<ashr_el_t, 2>;

      NdArrayView<ashr_t> _out(out);

      const size_t total_nbits = numel * in_nbits;
      std::vector<ashr_el_t> r0(total_nbits);
      std::vector<ashr_el_t> r1(total_nbits);
      prg_state->fillPrssPair(r0.data(), r1.data(), r0.size(),
                              PrgState::GenPrssCtrl::Both);

      if (comm->getRank() == P0) {
        // the helper
        auto b2 = bitDecompose<bshr_el_t>(getShare(in, 1), in_nbits);

        // gen masks with helper.
        std::vector<ashr_el_t> m0(total_nbits);
        std::vector<ashr_el_t> m1(total_nbits);
        prg_state->fillPrssPair<ashr_el_t>(m0.data(), nullptr, m0.size(),
                                           PrgState::GenPrssCtrl::First);
        prg_state->fillPrssPair<ashr_el_t>(m1.data(), nullptr, m1.size(),
                                           PrgState::GenPrssCtrl::First);

        // build selected mask
        SPU_ENFORCE(b2.size() == m0.size() && b2.size() == m1.size());
        pforeach(0, total_nbits,
                 [&](int64_t idx) { m0[idx] = !b2[idx] ? m0[idx] : m1[idx]; });

        // send selected masked to receiver.
        comm->sendAsync<ashr_el_t>(P1, m0, "mc");

        auto c1 = bitCompose<ashr_el_t>(r0, in_nbits);
        auto c2 = comm->recv<ashr_el_t>(P1, "c2");

        pforeach(0, numel, [&](int64_t idx) {
          _out[idx][0] = c1[idx];
          _out[idx][1] = c2[idx];
        });
      } else if (comm->getRank() == P1) {
        // the receiver
        prg_state->fillPrssPair<ashr_el_t>(nullptr, nullptr, r0.size(),
                                           PrgState::GenPrssCtrl::None);
        prg_state->fillPrssPair<ashr_el_t>(nullptr, nullptr, r0.size(),
                                           PrgState::GenPrssCtrl::None);

        auto b2 = bitDecompose<bshr_el_t>(getShare(in, 0), in_nbits);

        // ot.recv
        auto mc = comm->recv<ashr_el_t>(P0, "mc");
        auto m0 = comm->recv<ashr_el_t>(P2, "m0");
        auto m1 = comm->recv<ashr_el_t>(P2, "m1");

        // rebuild c2 = (b1^b2^b3)-c1-c3
        pforeach(0, total_nbits, [&](int64_t idx) {
          mc[idx] = !b2[idx] ? m0[idx] ^ mc[idx] : m1[idx] ^ mc[idx];
        });
        auto c2 = bitCompose<ashr_el_t>(mc, in_nbits);
        comm->sendAsync<ashr_el_t>(P0, c2, "c2");
        auto c3 = bitCompose<ashr_el_t>(r1, in_nbits);

        pforeach(0, numel, [&](int64_t idx) {
          _out[idx][0] = c2[idx];
          _out[idx][1] = c3[idx];
        });
      } else if (comm->getRank() == P2) {
        // the sender.
        auto c3 = bitCompose<ashr_el_t>(r0, in_nbits);
        auto c1 = bitCompose<ashr_el_t>(r1, in_nbits);

        // c3 = r0, c1 = r1
        // let mi := (i^b1^b3)−c1−c3 for i in {0, 1}
        // reuse r's memory for m
        pforeach(0, numel, [&](int64_t idx) {
          const auto x = _in[idx];
          auto xx = x[0] ^ x[1];
          for (size_t bit = 0; bit < in_nbits; bit++) {
            size_t flat_idx = idx * in_nbits + bit;
            ashr_el_t t = r0[flat_idx] + r1[flat_idx];
            r0[flat_idx] = ((xx >> bit) & 0x1) - t;
            r1[flat_idx] = ((~xx >> bit) & 0x1) - t;
          }
        });

        // gen masks with helper.
        std::vector<ashr_el_t> m0(total_nbits);
        std::vector<ashr_el_t> m1(total_nbits);
        prg_state->fillPrssPair<ashr_el_t>(nullptr, m0.data(), m0.size(),
                                           PrgState::GenPrssCtrl::Second);
        prg_state->fillPrssPair<ashr_el_t>(nullptr, m1.data(), m1.size(),
                                           PrgState::GenPrssCtrl::Second);
        pforeach(0, total_nbits, [&](int64_t idx) {
          m0[idx] ^= r0[idx];
          m1[idx] ^= r1[idx];
        });

        comm->sendAsync<ashr_el_t>(P1, m0, "m0");
        comm->sendAsync<ashr_el_t>(P1, m1, "m1");

        pforeach(0, numel, [&](int64_t idx) {
          _out[idx][0] = c3[idx];
          _out[idx][1] = c1[idx];
        });
      } else {
        SPU_THROW("expected party=3, got={}", comm->getRank());
      }
    });
  });

  return out;
}

// TODO: Accelerate bit scatter.
// split even and odd bits. e.g.
//   xAyBzCwD -> (xyzw, ABCD)
[[maybe_unused]] std::pair<NdArrayRef, NdArrayRef> bit_split(
    const NdArrayRef& in) {
  constexpr std::array<uint128_t, 6> kSwapMasks = {{
      yacl::MakeUint128(0x2222222222222222, 0x2222222222222222),  // 4bit
      yacl::MakeUint128(0x0C0C0C0C0C0C0C0C, 0x0C0C0C0C0C0C0C0C),  // 8bit
      yacl::MakeUint128(0x00F000F000F000F0, 0x00F000F000F000F0),  // 16bit
      yacl::MakeUint128(0x0000FF000000FF00, 0x0000FF000000FF00),  // 32bit
      yacl::MakeUint128(0x00000000FFFF0000, 0x00000000FFFF0000),  // 64bit
      yacl::MakeUint128(0x0000000000000000, 0xFFFFFFFF00000000),  // 128bit
  }};
  constexpr std::array<uint128_t, 6> kKeepMasks = {{
      yacl::MakeUint128(0x9999999999999999, 0x9999999999999999),  // 4bit
      yacl::MakeUint128(0xC3C3C3C3C3C3C3C3, 0xC3C3C3C3C3C3C3C3),  // 8bit
      yacl::MakeUint128(0xF00FF00FF00FF00F, 0xF00FF00FF00FF00F),  // 16bit
      yacl::MakeUint128(0xFF0000FFFF0000FF, 0xFF0000FFFF0000FF),  // 32bit
      yacl::MakeUint128(0xFFFF00000000FFFF, 0xFFFF00000000FFFF),  // 64bit
      yacl::MakeUint128(0xFFFFFFFF00000000, 0x00000000FFFFFFFF),  // 128bit
  }};

  const auto* in_ty = in.eltype().as<BShrTy>();
  const size_t in_nbits = in_ty->nbits();
  SPU_ENFORCE(in_nbits != 0 && in_nbits % 2 == 0, "in_nbits={}", in_nbits);
  const size_t out_nbits = in_nbits / 2;
  const auto out_backtype = calcBShareBacktype(out_nbits);
  const auto out_type = makeType<BShrTy>(out_backtype, out_nbits);

  NdArrayRef lo(out_type, in.shape());
  NdArrayRef hi(out_type, in.shape());

  DISPATCH_UINT_PT_TYPES(in_ty->getBacktype(), "_", [&]() {
    using in_el_t = ScalarT;
    using in_shr_t = std::array<in_el_t, 2>;
    NdArrayView<in_shr_t> _in(in);

    DISPATCH_UINT_PT_TYPES(out_backtype, "_", [&]() {
      using out_el_t = ScalarT;
      using out_shr_t = std::array<out_el_t, 2>;

      NdArrayView<out_shr_t> _lo(lo);
      NdArrayView<out_shr_t> _hi(hi);

      if constexpr (sizeof(out_el_t) <= 8) {
        pforeach(0, in.numel(), [&](int64_t idx) {
          constexpr uint64_t S = 0x5555555555555555;  // 01010101
          const out_el_t M = (out_el_t(1) << (in_nbits / 2)) - 1;

          const auto& r = _in[idx];

          _lo[idx][0] = yacl::pext_u64(r[0], S) & M;
          _hi[idx][0] = yacl::pext_u64(r[0], ~S) & M;
          _lo[idx][1] = yacl::pext_u64(r[1], S) & M;
          _hi[idx][1] = yacl::pext_u64(r[1], ~S) & M;
        });
      } else {
        pforeach(0, in.numel(), [&](int64_t idx) {
          auto r = _in[idx];
          // algorithm:
          //      0101010101010101
          // swap  ^^  ^^  ^^  ^^
          //      0011001100110011
          // swap   ^^^^    ^^^^
          //      0000111100001111
          // swap     ^^^^^^^^
          //      0000000011111111
          for (int k = 0; k + 1 < Log2Ceil(in_nbits); k++) {
            auto keep = static_cast<in_el_t>(kKeepMasks[k]);
            auto move = static_cast<in_el_t>(kSwapMasks[k]);
            int shift = 1 << k;

            r[0] = (r[0] & keep) ^ ((r[0] >> shift) & move) ^
                   ((r[0] & move) << shift);
            r[1] = (r[1] & keep) ^ ((r[1] >> shift) & move) ^
                   ((r[1] & move) << shift);
          }
          in_el_t mask = (in_el_t(1) << (in_nbits / 2)) - 1;
          _lo[idx][0] = static_cast<out_el_t>(r[0]) & mask;
          _hi[idx][0] = static_cast<out_el_t>(r[0] >> (in_nbits / 2)) & mask;
          _lo[idx][1] = static_cast<out_el_t>(r[1]) & mask;
          _hi[idx][1] = static_cast<out_el_t>(r[1] >> (in_nbits / 2)) & mask;
        });
      }
    });
  });

  return std::make_pair(hi, lo);
}

[[maybe_unused]] std::pair<NdArrayRef, NdArrayRef> bit_split_mss(
    const NdArrayRef& in) {
  constexpr std::array<uint128_t, 6> kSwapMasks = {{
      yacl::MakeUint128(0x2222222222222222, 0x2222222222222222),  // 4bit
      yacl::MakeUint128(0x0C0C0C0C0C0C0C0C, 0x0C0C0C0C0C0C0C0C),  // 8bit
      yacl::MakeUint128(0x00F000F000F000F0, 0x00F000F000F000F0),  // 16bit
      yacl::MakeUint128(0x0000FF000000FF00, 0x0000FF000000FF00),  // 32bit
      yacl::MakeUint128(0x00000000FFFF0000, 0x00000000FFFF0000),  // 64bit
      yacl::MakeUint128(0x0000000000000000, 0xFFFFFFFF00000000),  // 128bit
  }};
  constexpr std::array<uint128_t, 6> kKeepMasks = {{
      yacl::MakeUint128(0x9999999999999999, 0x9999999999999999),  // 4bit
      yacl::MakeUint128(0xC3C3C3C3C3C3C3C3, 0xC3C3C3C3C3C3C3C3),  // 8bit
      yacl::MakeUint128(0xF00FF00FF00FF00F, 0xF00FF00FF00FF00F),  // 16bit
      yacl::MakeUint128(0xFF0000FFFF0000FF, 0xFF0000FFFF0000FF),  // 32bit
      yacl::MakeUint128(0xFFFF00000000FFFF, 0xFFFF00000000FFFF),  // 64bit
      yacl::MakeUint128(0xFFFFFFFF00000000, 0x00000000FFFFFFFF),  // 128bit
  }};

  const auto* in_ty = in.eltype().as<BShrTyMss>();
  const size_t in_nbits = in_ty->nbits();
  SPU_ENFORCE(in_nbits != 0 && in_nbits % 2 == 0, "in_nbits={}", in_nbits);
  const size_t out_nbits = in_nbits / 2;
  const auto out_backtype = calcBShareBacktype(out_nbits);
  const auto out_type = makeType<BShrTyMss>(out_backtype, out_nbits);

  NdArrayRef lo(out_type, in.shape());
  NdArrayRef hi(out_type, in.shape());

  DISPATCH_UINT_PT_TYPES(in_ty->getBacktype(), "_", [&]() {
    using in_el_t = ScalarT;
    using in_shr_t = std::array<in_el_t, 3>;
    NdArrayView<in_shr_t> _in(in);

    DISPATCH_UINT_PT_TYPES(out_backtype, "_", [&]() {
      using out_el_t = ScalarT;
      using out_shr_t = std::array<out_el_t, 3>;

      NdArrayView<out_shr_t> _lo(lo);
      NdArrayView<out_shr_t> _hi(hi);

      // if constexpr (sizeof(out_el_t) <= 8) {
      //   pforeach(0, in.numel(), [&](int64_t idx) {
      //     constexpr uint64_t S = 0x5555555555555555;  // 01010101
      //     const out_el_t M = (out_el_t(1) << (in_nbits / 2)) - 1;

      //     const auto& r = _in[idx];

      //     _lo[idx][0] = yacl::pext_u64(r[0], S) & M;
      //     _hi[idx][0] = yacl::pext_u64(r[0], ~S) & M;
      //     _lo[idx][1] = yacl::pext_u64(r[1], S) & M;
      //     _hi[idx][1] = yacl::pext_u64(r[1], ~S) & M;
      //     _lo[idx][2] = yacl::pext_u64(r[1], S) & M;
      //     _hi[idx][2] = yacl::pext_u64(r[1], ~S) & M;
      //   });
      // } else {
        pforeach(0, in.numel(), [&](int64_t idx) {
          auto r = _in[idx];
          // algorithm:
          //      0101010101010101
          // swap  ^^  ^^  ^^  ^^
          //      0011001100110011
          // swap   ^^^^    ^^^^
          //      0000111100001111
          // swap     ^^^^^^^^
          //      0000000011111111
          for (int k = 0; k + 1 < Log2Ceil(in_nbits); k++) {
            auto keep = static_cast<in_el_t>(kKeepMasks[k]);
            auto move = static_cast<in_el_t>(kSwapMasks[k]);
            int shift = 1ull << k;

            r[0] = (r[0] & keep) ^ ((r[0] >> shift) & move) ^
                   ((r[0] & move) << shift);
            r[1] = (r[1] & keep) ^ ((r[1] >> shift) & move) ^
                   ((r[1] & move) << shift);
            r[2] = (r[2] & keep) ^ ((r[2] >> shift) & move) ^
                   ((r[2] & move) << shift);

            // assert(r[1] == 0 && r[2] == 0);
          }
          in_el_t mask = (in_el_t(1) << (in_nbits / 2)) - 1;
          _lo[idx][0] = static_cast<out_el_t>(r[0]) & mask;
          _hi[idx][0] = static_cast<out_el_t>(r[0] >> (in_nbits / 2)) & mask;
          _lo[idx][1] = static_cast<out_el_t>(r[1]) & mask;
          _hi[idx][1] = static_cast<out_el_t>(r[1] >> (in_nbits / 2)) & mask;
          _lo[idx][2] = static_cast<out_el_t>(r[2]) & mask;
          _hi[idx][2] = static_cast<out_el_t>(r[2] >> (in_nbits / 2)) & mask;

          // assert(_lo[idx][1] == 0 && _lo[idx][2] == 0);
          // assert(_hi[idx][1] == 0 && _hi[idx][2] == 0);
        });
      // }
    });
  });

  return std::make_pair(hi, lo);
}

NdArrayRef MsbA2B::proc(KernelEvalContext* ctx, const NdArrayRef& in) const {
  // size_t numel = in.numel();
  // size_t elsize = in.elsize();

  // NdArrayRef res(makeType<BShrTy>(calcBShareBacktype(1), 1), in.shape());

  // for (size_t p = 0; p < 3; p++)
  // {
  //   size_t offset = p * numel / 3;
  //   size_t op_numel = p == 2 ? numel - offset : numel / 3;
  //   NdArrayRef op(in.eltype(), in.shape());
  //   auto src_ptr = (uint8_t *) in.cbegin().getRawPtr() + offset * elsize;
  //   auto res_ptr = (uint8_t *) res.cbegin().getRawPtr() + offset;
  //   auto* op_ptr = static_cast<std::byte*>(op.data());
  //   std::memcpy(op_ptr, src_ptr, elsize * op_numel);
  //   auto tmp = MsbA2BMultiFanIn(ctx, op, p);
  //   auto* tmp_ptr = static_cast<std::byte*>(tmp.data());
  //   std::memcpy(res_ptr, tmp_ptr, op_numel);
  // }

  return MsbA2BMultiFanIn(ctx, in);
}

// Reference:
// New Primitives for Actively-Secure MPC over Rings with Applications to
// Private Machine Learning
// P8 IV.D protocol eqz
// https://eprint.iacr.org/2019/599.pdf
//
// Improved Primitives for MPC over Mixed Arithmetic-Binary Circuits
// https://eprint.iacr.org/2020/338.pdf
//
// P0 as the helper/dealer, samples r, deals [r]a and [r]b.
// P1 and P2 get new share [a]
//   P1: [a] = x2 + x3
//   P2: [a] = x1
// reveal c = [a]+[r]a
// check [a] == 0  <=> c == r
// c == r <=> ~c ^ rb  to be bit wise all 1
// then eqz(a) = bit_wise_and(~c ^ rb)
NdArrayRef eqz(KernelEvalContext* ctx, const NdArrayRef& in) {
  auto* prg_state = ctx->getState<PrgState>();
  auto* comm = ctx->getState<Communicator>();

  const auto field = in.eltype().as<AShrTy>()->field();
  const PtType in_bshr_btype = calcBShareBacktype(SizeOf(field) * 8);
  const auto numel = in.numel();

  NdArrayRef out(makeType<BShrTy>(calcBShareBacktype(8), 8), in.shape());

  size_t pivot;
  prg_state->fillPubl(absl::MakeSpan(&pivot, 1));
  size_t P0 = pivot % 3;
  size_t P1 = (pivot + 1) % 3;
  size_t P2 = (pivot + 2) % 3;

  DISPATCH_ALL_FIELDS(field, "_", [&]() {
    using ashr_el_t = ring2k_t;
    using ashr_t = std::array<ashr_el_t, 2>;
    DISPATCH_UINT_PT_TYPES(in_bshr_btype, "_", [&]() {
      using bshr_el_t = ScalarT;
      std::vector<bshr_el_t> zero_flag_3pc_0(numel);
      std::vector<bshr_el_t> zero_flag_3pc_1(numel);

      // algorithm begins
      if (comm->getRank() == P0) {
        std::vector<ashr_el_t> r(numel);
        prg_state->fillPriv(absl::MakeSpan(r));

        std::vector<ashr_el_t> r_arith_0(numel);
        prg_state->fillPrssPair<ashr_el_t>({}, r_arith_0.data(), numel,
                                           PrgState::GenPrssCtrl::Second);
        std::vector<bshr_el_t> r_bool_0(numel);
        prg_state->fillPrssPair<bshr_el_t>({}, r_bool_0.data(), numel,
                                           PrgState::GenPrssCtrl::Second);

        std::vector<ashr_el_t> r_arith_1(numel);
        pforeach(0, numel, [&](int64_t idx) {
          r_arith_1[idx] = r[idx] - r_arith_0[idx];
        });
        comm->sendAsync<ashr_el_t>(P2, r_arith_1, "r_arith");

        std::vector<bshr_el_t> r_bool_1(numel);
        pforeach(0, numel,
                 [&](int64_t idx) { r_bool_1[idx] = r[idx] ^ r_bool_0[idx]; });
        comm->sendAsync<bshr_el_t>(P2, r_bool_1, "r_bool");

        // back to 3 pc
        // P0 zero_flag = (rb1, rz)
        pforeach(0, numel,
                 [&](int64_t idx) { zero_flag_3pc_0[idx] = r_bool_1[idx]; });

        prg_state->fillPrssPair<bshr_el_t>({}, zero_flag_3pc_1.data(), numel,
                                           PrgState::GenPrssCtrl::Second);

      } else {
        std::vector<ashr_el_t> a_s(numel);
        NdArrayView<ashr_t> _in(in);
        std::vector<ashr_el_t> r_arith(numel);
        std::vector<bshr_el_t> r_bool(numel);

        if (comm->getRank() == P1) {
          pforeach(0, numel,
                   [&](int64_t idx) { a_s[idx] = _in[idx][0] + _in[idx][1]; });

          prg_state->fillPrssPair<ashr_el_t>(r_arith.data(), {}, numel,
                                             PrgState::GenPrssCtrl::First);
          prg_state->fillPrssPair<bshr_el_t>(r_bool.data(), {}, numel,
                                             PrgState::GenPrssCtrl::First);
        } else {
          pforeach(0, numel, [&](int64_t idx) { a_s[idx] = _in[idx][1]; });
          prg_state->fillPrssPair<ashr_el_t>({}, {}, numel,
                                             PrgState::GenPrssCtrl::None);
          prg_state->fillPrssPair<bshr_el_t>({}, {}, numel,
                                             PrgState::GenPrssCtrl::None);
          r_arith = comm->recv<ashr_el_t>(P0, "r_arith");
          r_bool = comm->recv<bshr_el_t>(P0, "r_bool");
        }

        // c in secret share
        std::vector<ashr_el_t> c_s(numel);
        pforeach(0, numel,
                 [&](int64_t idx) { c_s[idx] = r_arith[idx] + a_s[idx]; });

        std::vector<bshr_el_t> zero_flag_2pc(numel);
        if (comm->getRank() == P1) {
          auto c_p = comm->recv<ashr_el_t>(P2, "c_s");

          // reveal c
          pforeach(0, numel,
                   [&](int64_t idx) { c_p[idx] = c_p[idx] + c_s[idx]; });
          // P1 zero_flag = (rz, not(c_p xor [r]b0)^ rz)
          std::vector<bshr_el_t> r_z(numel);
          prg_state->fillPrssPair<bshr_el_t>(r_z.data(), {}, numel,
                                             PrgState::GenPrssCtrl::First);
          pforeach(0, numel, [&](int64_t idx) {
            zero_flag_2pc[idx] = ~(c_p[idx] ^ r_bool[idx]) ^ r_z[idx];
          });

          comm->sendAsync<bshr_el_t>(P2, zero_flag_2pc, "flag_split");

          pforeach(0, numel, [&](int64_t idx) {
            zero_flag_3pc_0[idx] = r_z[idx];
            zero_flag_3pc_1[idx] = zero_flag_2pc[idx];
          });
        } else {
          comm->sendAsync<ashr_el_t>(P1, c_s, "c_s");
          // P1 zero_flag = (not(c_p xor [r]b0)^ rz, rb1)
          pforeach(0, numel,
                   [&](int64_t idx) { zero_flag_3pc_1[idx] = r_bool[idx]; });
          prg_state->fillPrssPair<bshr_el_t>({}, {}, numel,
                                             PrgState::GenPrssCtrl::None);

          auto flag_split = comm->recv<bshr_el_t>(P1, "flag_split");
          pforeach(0, numel, [&](int64_t idx) {
            zero_flag_3pc_0[idx] = flag_split[idx];
          });
        }
      }

      // Reference:
      // Improved Primitives for Secure Multiparty Integer Computation
      // P10 4.1 k-ary
      // https://link.springer.com/chapter/10.1007/978-3-642-15317-4_13
      //
      // if a == 0, zero_flag supposed to be all 1
      // do log k round bit wise and
      // in each round, bit wise split zero_flag in half
      // compute  and(left_half, right_half)
      auto cur_bytes = SizeOf(field) * numel;
      auto cur_bits = cur_bytes * 8;
      auto cur_numel = (unsigned long)numel;
      std::vector<std::byte> round_res_0(cur_bytes);
      std::memcpy(round_res_0.data(), zero_flag_3pc_0.data(), cur_bytes);
      std::vector<std::byte> round_res_1(cur_bytes);
      std::memcpy(round_res_1.data(), zero_flag_3pc_1.data(), cur_bytes);
      while (cur_bits != cur_numel) {
        // byte num per element
        auto byte_num_el = cur_bytes == cur_numel ? 1 : (cur_bytes / numel);
        // byte num of left/right_bits
        auto half_num_bytes =
            cur_bytes == cur_numel ? cur_numel : (cur_bytes / 2);

        // break into left_bits and right_bits
        std::vector<std::vector<std::byte>> left_bits(
            2, std::vector<std::byte>(half_num_bytes));
        std::vector<std::vector<std::byte>> right_bits(
            2, std::vector<std::byte>(half_num_bytes));

        // cur_bits <= 8, use rshift to split in half
        if (cur_bytes == cur_numel) {
          pforeach(0, numel, [&](int64_t idx) {
            left_bits[0][idx] =
                round_res_0[idx] >> (cur_bits / (cur_numel * 2));
            left_bits[1][idx] =
                round_res_1[idx] >> (cur_bits / (cur_numel * 2));
            right_bits[0][idx] = round_res_0[idx];
            right_bits[1][idx] = round_res_1[idx];
          });
          // cur_bits > 8
        } else {
          pforeach(0, numel, [&](int64_t idx) {
            auto cur_byte_idx = idx * byte_num_el;
            for (size_t i = 0; i < (byte_num_el / 2); i++) {
              left_bits[0][cur_byte_idx / 2 + i] =
                  round_res_0[cur_byte_idx + i];
              left_bits[1][cur_byte_idx / 2 + i] =
                  round_res_1[cur_byte_idx + i];
            }
            for (size_t i = 0; i < (byte_num_el / 2); i++) {
              right_bits[0][cur_byte_idx / 2 + i] =
                  round_res_0[cur_byte_idx + byte_num_el / 2 + i];
              right_bits[1][cur_byte_idx / 2 + i] =
                  round_res_1[cur_byte_idx + byte_num_el / 2 + i];
            }
          });
        }

        // compute and(left_half, right_half)
        std::vector<std::byte> r0(half_num_bytes);
        std::vector<std::byte> r1(half_num_bytes);
        prg_state->fillPrssPair<std::byte>(r0.data(), r1.data(), half_num_bytes,
                                           PrgState::GenPrssCtrl::Both);

        // z1 = (x1 & y1) ^ (x1 & y2) ^ (x2 & y1) ^ (r0 ^ r1);
        pforeach(0, half_num_bytes, [&](int64_t idx) {
          r0[idx] = (left_bits[0][idx] & right_bits[0][idx]) ^
                    (left_bits[0][idx] & right_bits[1][idx]) ^
                    (left_bits[1][idx] & right_bits[0][idx]) ^
                    (r0[idx] ^ r1[idx]);
        });

        auto temp = comm->rotate<std::byte>(r0, "andbb");
        r1.assign(temp.begin(), temp.end());

        cur_bytes = cur_bytes == cur_numel ? cur_numel : (cur_bytes / 2);
        cur_bits /= 2;
        round_res_0.assign(r0.begin(), r0.end());
        round_res_1.assign(r1.begin(), r1.end());
      }

      NdArrayView<std::array<std::byte, 2>> _out(out);

      pforeach(0, numel, [&](int64_t idx) {
        _out[idx][0] = round_res_0[idx];
        _out[idx][1] = round_res_1[idx];
      });
    });
  });

  return out;
}

NdArrayRef EqualAA::proc(KernelEvalContext* ctx, const NdArrayRef& lhs,
                         const NdArrayRef& rhs) const {
  const auto* lhs_ty = lhs.eltype().as<AShrTy>();
  const auto* rhs_ty = rhs.eltype().as<AShrTy>();

  SPU_ENFORCE(lhs_ty->field() == rhs_ty->field());
  const auto field = lhs_ty->field();
  NdArrayRef out(makeType<AShrTy>(field), lhs.shape());

  DISPATCH_ALL_FIELDS(field, "_", [&]() {
    using shr_t = std::array<ring2k_t, 2>;
    NdArrayView<shr_t> _out(out);
    NdArrayView<shr_t> _lhs(lhs);
    NdArrayView<shr_t> _rhs(rhs);

    pforeach(0, lhs.numel(), [&](int64_t idx) {
      _out[idx][0] = _lhs[idx][0] - _rhs[idx][0];
      _out[idx][1] = _lhs[idx][1] - _rhs[idx][1];
    });
  });

  return eqz(ctx, out);
}

NdArrayRef EqualAP::proc(KernelEvalContext* ctx, const NdArrayRef& lhs,
                         const NdArrayRef& rhs) const {
  auto* comm = ctx->getState<Communicator>();
  const auto* lhs_ty = lhs.eltype().as<AShrTy>();
  const auto* rhs_ty = rhs.eltype().as<Pub2kTy>();

  SPU_ENFORCE(lhs_ty->field() == rhs_ty->field());
  const auto field = lhs_ty->field();
  NdArrayRef out(makeType<AShrTy>(field), lhs.shape());

  auto rank = comm->getRank();

  DISPATCH_ALL_FIELDS(field, "_", [&]() {
    using el_t = ring2k_t;
    using shr_t = std::array<el_t, 2>;

    NdArrayView<shr_t> _out(out);
    NdArrayView<shr_t> _lhs(lhs);
    NdArrayView<el_t> _rhs(rhs);

    pforeach(0, lhs.numel(), [&](int64_t idx) {
      _out[idx][0] = _lhs[idx][0];
      _out[idx][1] = _lhs[idx][1];
      if (rank == 0) _out[idx][1] -= _rhs[idx];
      if (rank == 1) _out[idx][0] -= _rhs[idx];
    });
    return out;
  });

  return eqz(ctx, out);
}

void CommonTypeV::evaluate(KernelEvalContext* ctx) const {
  const Type& lhs = ctx->getParam<Type>(0);
  const Type& rhs = ctx->getParam<Type>(1);

  SPU_TRACE_MPC_DISP(ctx, lhs, rhs);

  const auto* lhs_v = lhs.as<Priv2kTy>();
  const auto* rhs_v = rhs.as<Priv2kTy>();

  ctx->setOutput(makeType<AShrTy>(std::max(lhs_v->field(), rhs_v->field())));
}

// Xor gate for ASS.
NdArrayRef AssXor2(KernelEvalContext* ctx, const NdArrayRef& lhs,
                       const NdArrayRef& rhs) {
  const auto* lhs_ty = lhs.eltype().as<BShrTy>();
  const auto* rhs_ty = rhs.eltype().as<BShrTy>();

  const size_t out_nbits = std::min(lhs_ty->nbits(), rhs_ty->nbits());
  const PtType out_btype = calcBShareBacktype(out_nbits);
  NdArrayRef out(makeType<BShrTy>(out_btype, out_nbits), lhs.shape());

  return DISPATCH_UINT_PT_TYPES(rhs_ty->getBacktype(), "_", [&]() {
    using rhs_el_t = ScalarT;
    using rhs_shr_t = std::array<rhs_el_t, 2>;
    NdArrayView<rhs_shr_t> _rhs(rhs);

    return DISPATCH_UINT_PT_TYPES(lhs_ty->getBacktype(), "_", [&]() {
      using lhs_el_t = ScalarT;
      using lhs_shr_t = std::array<lhs_el_t, 2>;
      NdArrayView<lhs_shr_t> _lhs(lhs);

      return DISPATCH_UINT_PT_TYPES(out_btype, "_", [&]() {
        using out_el_t = ScalarT;
        // mss(x) = (Dx, dx0, dx1), x = Dx ^ dx0 ^ dx1
        using out_shr_t = std::array<out_el_t, 2>;
        NdArrayView<out_shr_t> _out(out);

        // online.
        pforeach(0, lhs.numel(), [&](int64_t idx) {
          const auto& l = _lhs[idx];
          const auto& r = _rhs[idx];
          out_shr_t& o = _out[idx];
          o[0] = l[0] ^ r[0];
        });
        return out;
      });
    });
  });
}

// Xor gate for RSS.
NdArrayRef RssXor2(KernelEvalContext* ctx, const NdArrayRef& lhs,
                       const NdArrayRef& rhs) {

  const auto* lhs_ty = lhs.eltype().as<BShrTy>();
  const auto* rhs_ty = rhs.eltype().as<BShrTy>();

  const size_t out_nbits = std::min(lhs_ty->nbits(), rhs_ty->nbits());
  const PtType out_btype = calcBShareBacktype(out_nbits);
  NdArrayRef out(makeType<BShrTy>(out_btype, out_nbits), lhs.shape());

  return DISPATCH_UINT_PT_TYPES(rhs_ty->getBacktype(), "_", [&]() {
    using rhs_el_t = ScalarT;
    using rhs_shr_t = std::array<rhs_el_t, 2>;
    NdArrayView<rhs_shr_t> _rhs(rhs);

    return DISPATCH_UINT_PT_TYPES(lhs_ty->getBacktype(), "_", [&]() {
      using lhs_el_t = ScalarT;
      using lhs_shr_t = std::array<lhs_el_t, 2>;
      NdArrayView<lhs_shr_t> _lhs(lhs);

      return DISPATCH_UINT_PT_TYPES(out_btype, "_", [&]() {
        using out_el_t = ScalarT;
        // mss(x) = (Dx, dx0, dx1), x = Dx ^ dx0 ^ dx1
        using out_shr_t = std::array<out_el_t, 2>;
        NdArrayView<out_shr_t> _out(out);

        // online.
        pforeach(0, lhs.numel(), [&](int64_t idx) {
          const auto& l = _lhs[idx];
          const auto& r = _rhs[idx];
          out_shr_t& o = _out[idx];
          o[0] = l[0] ^ r[0];
          o[1] = l[1] ^ r[1];
        });
        return out;
      });
    });
  });
}

// Xor gate for MSS.
NdArrayRef MssXor2(KernelEvalContext* ctx, const NdArrayRef& lhs,
                       const NdArrayRef& rhs) {

  const auto* lhs_ty = lhs.eltype().as<BShrTyMss>();
  const auto* rhs_ty = rhs.eltype().as<BShrTyMss>();

  const size_t out_nbits = std::min(lhs_ty->nbits(), rhs_ty->nbits());
  const PtType out_btype = calcBShareBacktype(out_nbits);
  NdArrayRef out(makeType<BShrTyMss>(out_btype, out_nbits), lhs.shape());

  return DISPATCH_UINT_PT_TYPES(rhs_ty->getBacktype(), "_", [&]() {
    using rhs_el_t = ScalarT;
    using rhs_shr_t = std::array<rhs_el_t, 3>;
    NdArrayView<rhs_shr_t> _rhs(rhs);

    return DISPATCH_UINT_PT_TYPES(lhs_ty->getBacktype(), "_", [&]() {
      using lhs_el_t = ScalarT;
      using lhs_shr_t = std::array<lhs_el_t, 3>;
      NdArrayView<lhs_shr_t> _lhs(lhs);

      return DISPATCH_UINT_PT_TYPES(out_btype, "_", [&]() {
        using out_el_t = ScalarT;
        // mss(x) = (Dx, dx0, dx1), x = Dx ^ dx0 ^ dx1
        using out_shr_t = std::array<out_el_t, 3>;
        NdArrayView<out_shr_t> _out(out);

        // online.
        pforeach(0, lhs.numel(), [&](int64_t idx) {
          const auto& l = _lhs[idx];
          const auto& r = _rhs[idx];
          out_shr_t& o = _out[idx];
          o[0] = l[0] ^ r[0];
          o[1] = l[1] ^ r[1];
          o[2] = l[2] ^ r[2];
        });
        return out;
      });
    });
  });
}

// And gate for RSS which outputs ASS result (no comunication).
NdArrayRef RssAnd2NoComm(KernelEvalContext* ctx, const NdArrayRef& lhs,
                       const NdArrayRef& rhs) {
  auto* prg_state = ctx->getState<PrgState>();

  const auto* lhs_ty = lhs.eltype().as<BShrTy>();
  const auto* rhs_ty = rhs.eltype().as<BShrTy>();

  const size_t out_nbits = std::min(lhs_ty->nbits(), rhs_ty->nbits());
  const PtType out_btype = calcBShareBacktype(out_nbits);
  NdArrayRef out(makeType<BShrTy>(out_btype, out_nbits), lhs.shape());

  return DISPATCH_UINT_PT_TYPES(rhs_ty->getBacktype(), "_", [&]() {
    using rhs_el_t = ScalarT;
    using rhs_shr_t = std::array<rhs_el_t, 2>;
    NdArrayView<rhs_shr_t> _rhs(rhs);

    return DISPATCH_UINT_PT_TYPES(lhs_ty->getBacktype(), "_", [&]() {
      using lhs_el_t = ScalarT;
      using lhs_shr_t = std::array<lhs_el_t, 2>;
      NdArrayView<lhs_shr_t> _lhs(lhs);

      return DISPATCH_UINT_PT_TYPES(out_btype, "_", [&]() {
        using out_el_t = ScalarT;
        // mss(x) = (Dx, dx0, dx1), x = Dx ^ dx0 ^ dx1
        using out_shr_t = std::array<out_el_t, 2>;
        NdArrayView<out_shr_t> _out(out);

        // correlated randomness for RSS based multiplication.
        std::vector<out_el_t> r0(lhs.numel(), 0);
        std::vector<out_el_t> r1(lhs.numel(), 0);
        
        prg_state->fillPrssPair(r0.data(), r1.data(), r0.size(),
                                PrgState::GenPrssCtrl::Both);
        #ifndef EQ_USE_PRG_STATE
        std::fill(r0.begin(), r0.end(), 0);
        std::fill(r1.begin(), r1.end(), 0);
        #endif

        // online.
        // dxy = dx & dy = (dx0 & dy0) ^ (dx0 & dy1) ^ (dx1 & dy0);
        // r0 is dxy0, r1 is dxy1.
        pforeach(0, lhs.numel(), [&](int64_t idx) {
          const auto& l = _lhs[idx];
          const auto& r = _rhs[idx];
          out_shr_t& o = _out[idx];
          o[0] = (l[0] & r[0]) ^ (l[0] & r[1]) ^ (l[1] & r[0]) ^
                    (r0[idx] ^ r1[idx]);
        });
        return out;
      });
    });
  });
}

// And gate for MSS which outputs RSS result (no comunication).
NdArrayRef MssAnd2NoComm(KernelEvalContext* ctx, const NdArrayRef& lhs,
                       const NdArrayRef& rhs) {
  auto* prg_state = ctx->getState<PrgState>();
  auto* comm = ctx->getState<Communicator>();

  const auto* lhs_ty = lhs.eltype().as<BShrTyMss>();
  const auto* rhs_ty = rhs.eltype().as<BShrTyMss>();

  const size_t out_nbits = std::min(lhs_ty->nbits(), rhs_ty->nbits());
  const PtType out_btype = calcBShareBacktype(out_nbits);
  NdArrayRef out(makeType<BShrTy>(out_btype, out_nbits), lhs.shape());

  return DISPATCH_UINT_PT_TYPES(rhs_ty->getBacktype(), "_", [&]() {
    using rhs_el_t = ScalarT;
    using rhs_shr_t = std::array<rhs_el_t, 3>;
    NdArrayView<rhs_shr_t> _rhs(rhs);

    return DISPATCH_UINT_PT_TYPES(lhs_ty->getBacktype(), "_", [&]() {
      using lhs_el_t = ScalarT;
      using lhs_shr_t = std::array<lhs_el_t, 3>;
      NdArrayView<lhs_shr_t> _lhs(lhs);

      return DISPATCH_UINT_PT_TYPES(out_btype, "_", [&]() {
        using out_el_t = ScalarT;
        // mss(x) = (Dx, dx0, dx1), x = Dx ^ dx0 ^ dx1
        using out_shr_t = std::array<out_el_t, 2>;

        // correlated randomness for RSS based multiplication.
        std::vector<out_el_t> r0(lhs.numel(), 0);
        std::vector<out_el_t> r1(lhs.numel(), 0);
        prg_state->fillPrssPair(r0.data(), r1.data(), r0.size(),
                                PrgState::GenPrssCtrl::Both);

        // offline.
        
        #if !defined(EQ_USE_PRG_STATE) || !defined(EQ_USE_OFFLINE)
        std::fill(r0.begin(), r0.end(), 0);
        std::fill(r1.begin(), r1.end(), 0);
        comm->addCommStatsManually(0, 0);     // deal with unused-variable warning. 
        #endif
        #ifdef EQ_USE_OFFLINE
        // dxy = dx & dy = (dx0 & dy0) ^ (dx0 & dy1) ^ (dx1 & dy0);
        // r0 is dxy0, r1 is dxy1.
        pforeach(0, lhs.numel(), [&](int64_t idx) {
          const auto& l = _lhs[idx];
          const auto& r = _rhs[idx];
          r0[idx] = (l[1] & r[1]) ^ (l[1] & r[2]) ^ (l[2] & r[1]) ^
                    (r0[idx] ^ r1[idx]);
        });

        r1 = comm->rotate<out_el_t>(r0, "MssAndBB, offline");  // comm => 1, k
        // comm->addCommStatsManually(-1, -r0.size() * sizeof(out_el_t));        
        #endif

        // online, compute [out] locally.
        NdArrayView<out_shr_t> _out(out);
        pforeach(0, lhs.numel(), [&](int64_t idx) {
          const auto& l = _lhs[idx];
          const auto& r = _rhs[idx];

          out_shr_t& o = _out[idx];
          // z = x & y = (Dx ^ dx) & (Dy ^ dy) = Dx & Dy ^ Dx & dy ^ dx & Dy ^ dxy
          // o[0] = ((comm->getRank() == 0) * (l[0] & r[0])) ^ (l[0] & r[1]) ^ (l[1] & r[0]) ^ r0[idx];   // r0 is dxy0
          // o[1] = ((comm->getRank() == 2) * (l[0] & r[0])) ^ (l[0] & r[2]) ^ (l[2] & r[0]) ^ r1[idx];   // r1 is dxy1
          o[0] = ((l[0] & r[0])) ^ (l[0] & r[1]) ^ (l[1] & r[0]) ^ r0[idx];   // r0 is dxy0
          o[1] = ((l[0] & r[0])) ^ (l[0] & r[2]) ^ (l[2] & r[0]) ^ r1[idx];   // r1 is dxy1
        });
        return out;
      });
    });
  });
}

// And gate for MSS which outputs ASS result (no comunication).
NdArrayRef MssAnd3NoComm(KernelEvalContext* ctx, const NdArrayRef& op1,
                       const NdArrayRef& op2, const NdArrayRef& op3) {

    auto lo_res = MssAnd2NoComm(ctx, op1, op2);
    auto hi_res = ResharingMss2Rss(ctx, op3);
    auto out = RssAnd2NoComm(ctx, lo_res, hi_res);
    
    return out;
}

// And gate for MSS which outputs ASS result (no comunication).
NdArrayRef MssAnd4NoComm(KernelEvalContext* ctx, const NdArrayRef& op1,
                       const NdArrayRef& op2, const NdArrayRef& op3, const NdArrayRef& op4) {

    auto lo_res = MssAnd2NoComm(ctx, op1, op2);
    auto hi_res = MssAnd2NoComm(ctx, op3, op4);
    auto out = RssAnd2NoComm(ctx, lo_res, hi_res);
    
    return out;
}

// Resharing protocol from RSS to MSS.
NdArrayRef ResharingRss2Mss(KernelEvalContext* ctx, const NdArrayRef& in) {
  auto* prg_state = ctx->getState<PrgState>();
  auto* comm = ctx->getState<Communicator>();

  const auto* in_ty = in.eltype().as<BShrTy>();

  const size_t out_nbits = in_ty->nbits();
  const PtType out_btype = calcBShareBacktype(out_nbits);
  NdArrayRef out(makeType<BShrTyMss>(out_btype, out_nbits), in.shape());

    return DISPATCH_UINT_PT_TYPES(in_ty->getBacktype(), "_", [&]() {
      using in_el_t = ScalarT;
      using in_shr_t = std::array<in_el_t, 2>;
      NdArrayView<in_shr_t> _in(in);

      return DISPATCH_UINT_PT_TYPES(out_btype, "_", [&]() {
        using out_el_t = ScalarT;
        // mss(x) = (Dx, dx0, dx1), x = Dx ^ dx0 ^ dx1
        using out_shr_t = std::array<out_el_t, 3>;
        NdArrayView<out_shr_t> _out(out);

        // correlated randomness for RSS based multiplication.
        std::vector<out_el_t> r0(in.numel(), 0);
        std::vector<out_el_t> r1(in.numel(), 0);
        prg_state->fillPrssPair(r0.data(), r1.data(), r0.size(),
                                PrgState::GenPrssCtrl::Both);
        #if !defined(EQ_USE_OFFLINE) || !defined(EQ_USE_PRG_STATE)
        std::fill(r0.begin(), r0.end(), 0);
        std::fill(r1.begin(), r1.end(), 0);
        #endif

        // online.
        pforeach(0, in.numel(), [&](int64_t idx) {
          in_shr_t& i = _in[idx];
          out_shr_t& o = _out[idx];
          o[1] = r0[idx];
          o[2] = r1[idx];
          r0[idx] = i[0] ^ r0[idx];
        });

        r0 = comm->rotateR<out_el_t>(r0, "Resharing RSS to MSS, online");  // comm => 1, k

        pforeach(0, in.numel(), [&](int64_t idx) {
          in_shr_t& i = _in[idx];
          out_shr_t& o = _out[idx];

          o[0] = i[0] ^ i[1] ^ o[1] ^ o[2] ^ r0[idx];
        });
        return out;
      });
    });
}

// Resharing protocol from ASS to RSS.
// using RSS container to hold ASS.
NdArrayRef ResharingAss2Rss(KernelEvalContext* ctx, const NdArrayRef& in) {
  auto* prg_state = ctx->getState<PrgState>();
  auto* comm = ctx->getState<Communicator>();

  const auto* in_ty = in.eltype().as<BShrTy>();

  const size_t out_nbits = in_ty->nbits();
  const PtType out_btype = calcBShareBacktype(out_nbits);
  NdArrayRef out(makeType<BShrTy>(out_btype, out_nbits), in.shape());

    return DISPATCH_UINT_PT_TYPES(in_ty->getBacktype(), "_", [&]() {
      using in_el_t = ScalarT;
      using in_shr_t = std::array<in_el_t, 2>;
      NdArrayView<in_shr_t> _in(in);

      return DISPATCH_UINT_PT_TYPES(out_btype, "_", [&]() {
        using out_el_t = ScalarT;
        // mss(x) = (Dx, dx0, dx1), x = Dx ^ dx0 ^ dx1
        using out_shr_t = std::array<out_el_t, 2>;
        NdArrayView<out_shr_t> _out(out);

        // correlated randomness for RSS based multiplication.
        std::vector<out_el_t> r0(in.numel(), 0);
        std::vector<out_el_t> r1(in.numel(), 0);
        prg_state->fillPrssPair(r0.data(), r1.data(), r0.size(),
                                PrgState::GenPrssCtrl::Both);
        #ifndef EQ_USE_PRG_STATE
        std::fill(r0.begin(), r0.end(), 0);
        std::fill(r1.begin(), r1.end(), 0);
        #endif

        // online.
        pforeach(0, in.numel(), [&](int64_t idx) {
          in_shr_t& i = _in[idx];
          out_shr_t& o = _out[idx];
          o[0] = i[0] ^ r0[idx] ^ r1[idx];
          r0[idx] = i[0] ^ r0[idx] ^ r1[idx];
        });

        // TODO: not safe. should add a mask to r1.
        r0 = comm->rotate<out_el_t>(r0, "Resharing ASS to RSS, online");  // comm => 1, k

        pforeach(0, in.numel(), [&](int64_t idx) {
          out_shr_t& o = _out[idx];

          o[1] = r0[idx];
        });
        return out;
      });
    });
}

// Resharing protocol from ASS to MSS.
// using RSS container to hold ASS.
NdArrayRef ResharingAss2Mss(KernelEvalContext* ctx, const NdArrayRef& in) {
  auto* prg_state = ctx->getState<PrgState>();
  auto* comm = ctx->getState<Communicator>();

  const auto* in_ty = in.eltype().as<BShrTy>();

  const size_t out_nbits = in_ty->nbits();
  const PtType out_btype = calcBShareBacktype(out_nbits);
  NdArrayRef out(makeType<BShrTyMss>(out_btype, out_nbits), in.shape());

    return DISPATCH_UINT_PT_TYPES(in_ty->getBacktype(), "_", [&]() {
      using in_el_t = ScalarT;
      using in_shr_t = std::array<in_el_t, 2>;
      NdArrayView<in_shr_t> _in(in);

      return DISPATCH_UINT_PT_TYPES(out_btype, "_", [&]() {
        using out_el_t = ScalarT;
        // mss(x) = (Dx, dx0, dx1), x = Dx ^ dx0 ^ dx1
        using out_shr_t = std::array<out_el_t, 3>;
        NdArrayView<out_shr_t> _out(out);

        // correlated randomness for RSS based multiplication.
        std::vector<out_el_t> r0(in.numel());
        std::vector<out_el_t> r1(in.numel());
        prg_state->fillPrssPair(r0.data(), r1.data(), r0.size(),
                                PrgState::GenPrssCtrl::Both);
        #if !defined(EQ_USE_OFFLINE) || !defined(EQ_USE_PRG_STATE)
        std::fill(r0.begin(), r0.end(), 0);
        std::fill(r1.begin(), r1.end(), 0);
        #endif

        // online.
        pforeach(0, in.numel(), [&](int64_t idx) {
          in_shr_t& i = _in[idx];
          out_shr_t& o = _out[idx];
          o[1] = r0[idx];
          o[2] = r1[idx];
          r0[idx] = i[0] ^ r0[idx];
          r1[idx] = i[0];
        });

        // TODO: not safe. should add a mask to r1.
        r0 = comm->rotateR<out_el_t>(r0, "Resharing ASS to MSS, online, message 1");  // comm => 1, k
        r1 = comm->rotate<out_el_t>(r1, "Resharing ASS to MSS, online, message 2");  // comm => 1, k
        comm->addCommStatsManually(-1, 0);

        pforeach(0, in.numel(), [&](int64_t idx) {
          in_shr_t& i = _in[idx];
          out_shr_t& o = _out[idx];

          o[0] = i[0] ^ o[1] ^ o[2] ^ r0[idx] ^ r1[idx];
        });
        return out;
      });
    });
}

// Resharing protocol from MSS to RSS.
NdArrayRef ResharingMss2Rss(KernelEvalContext* ctx, const NdArrayRef& in) {

  const auto* in_ty = in.eltype().as<BShrTyMss>();

  const size_t out_nbits = in_ty->nbits();
  const PtType out_btype = calcBShareBacktype(out_nbits);
  NdArrayRef out(makeType<BShrTy>(out_btype, out_nbits), in.shape());

    return DISPATCH_UINT_PT_TYPES(in_ty->getBacktype(), "_", [&]() {
      using in_el_t = ScalarT;
      using in_shr_t = std::array<in_el_t, 3>;
      NdArrayView<in_shr_t> _in(in);

      return DISPATCH_UINT_PT_TYPES(out_btype, "_", [&]() {
        using out_el_t = ScalarT;
        // mss(x) = (Dx, dx0, dx1), x = Dx ^ dx0 ^ dx1
        using out_shr_t = std::array<out_el_t, 2>;
        NdArrayView<out_shr_t> _out(out);

        // online.
        pforeach(0, in.numel(), [&](int64_t idx) {
          in_shr_t& i = _in[idx];
          out_shr_t& o = _out[idx];
          o[0] = i[0] ^ i[1];
          o[1] = i[0] ^ i[2];

          // assert(i[1] == 0 && i[2] == 0);
        });

        return out;
      });
    });
}

// Resharing protocol from RSS to ASS.
NdArrayRef ResharingRss2Ass(KernelEvalContext* ctx, const NdArrayRef& in) {

  const auto* in_ty = in.eltype().as<BShrTy>();

  const size_t out_nbits = in_ty->nbits();
  const PtType out_btype = calcBShareBacktype(out_nbits);
  NdArrayRef out(makeType<BShrTy>(out_btype, out_nbits), in.shape());

    return DISPATCH_UINT_PT_TYPES(in_ty->getBacktype(), "_", [&]() {
      using in_el_t = ScalarT;
      using in_shr_t = std::array<in_el_t, 2>;
      NdArrayView<in_shr_t> _in(in);

      return DISPATCH_UINT_PT_TYPES(out_btype, "_", [&]() {
        using out_el_t = ScalarT;
        // mss(x) = (Dx, dx0, dx1), x = Dx ^ dx0 ^ dx1
        using out_shr_t = std::array<out_el_t, 2>;
        NdArrayView<out_shr_t> _out(out);

        // online.
        pforeach(0, in.numel(), [&](int64_t idx) {
          in_shr_t& i = _in[idx];
          out_shr_t& o = _out[idx];
          o[0] = i[0];
          o[1] = 0;
        });

        return out;
      });
    });
}

uint64_t lshift(uint64_t x, size_t shift) {
  return x << shift;
}

uint64_t rshift(uint64_t x, size_t shift) {
  return x >> shift;
}


uint64_t select(uint64_t x, uint64_t mask, uint64_t offset, size_t idx) {
  return (x & (mask << (idx * offset))) << ((3 - idx) * offset);
}

// Select substring of x corresponding to mask and lshift it stride bits.
uint64_t SelectAndRotate(uint64_t x, uint64_t mask, uint64_t stride) {
  return (x & mask) << stride;
}

NdArrayRef pack_2_bitvec_ass(const NdArrayRef& lo, const NdArrayRef& hi) {
  const auto* lo_ty = lo.eltype().as<BShrTy>();
  const auto* hi_ty = hi.eltype().as<BShrTy>();

  assert(lo_ty->nbits() == hi_ty->nbits());
  const size_t out_nbits = lo_ty->nbits() + hi_ty->nbits();
  const PtType out_btype = calcBShareBacktype(out_nbits);
  NdArrayRef out(makeType<BShrTy>(out_btype, out_nbits), lo.shape());

  return DISPATCH_UINT_PT_TYPES(hi_ty->getBacktype(), "_", [&]() {
    using hi_el_t = ScalarT;
    using hi_shr_t = std::array<hi_el_t, 2>;
    NdArrayView<hi_shr_t> _hi(hi);

    return DISPATCH_UINT_PT_TYPES(lo_ty->getBacktype(), "_", [&]() {
      using lo_el_t = ScalarT;
      using lo_shr_t = std::array<lo_el_t, 2>;
      NdArrayView<lo_shr_t> _lo(lo);

      return DISPATCH_UINT_PT_TYPES(out_btype, "_", [&]() {
        using out_el_t = ScalarT;
        using out_shr_t = std::array<out_el_t, 2>;
        NdArrayView<out_shr_t> _out(out);

        pforeach(0, lo.numel(), [&](int64_t idx) {
          const auto& l = _lo[idx];
          const auto& h = _hi[idx];
          out_shr_t& o = _out[idx];
          o[0] = l[0] | (static_cast<out_el_t>(h[0]) << lo_ty->nbits());
        });
        return out;
      });
    });
  });
}

NdArrayRef pack_2_bitvec_rss(const NdArrayRef& lo, const NdArrayRef& hi) {
  const auto* lo_ty = lo.eltype().as<BShrTy>();
  const auto* hi_ty = hi.eltype().as<BShrTy>();

  assert(lo_ty->nbits() == hi_ty->nbits());
  const size_t out_nbits = lo_ty->nbits() + hi_ty->nbits();
  const PtType out_btype = calcBShareBacktype(out_nbits);
  NdArrayRef out(makeType<BShrTy>(out_btype, out_nbits), lo.shape());

  return DISPATCH_UINT_PT_TYPES(hi_ty->getBacktype(), "_", [&]() {
    using hi_el_t = ScalarT;
    using hi_shr_t = std::array<hi_el_t, 2>;
    NdArrayView<hi_shr_t> _hi(hi);

    return DISPATCH_UINT_PT_TYPES(lo_ty->getBacktype(), "_", [&]() {
      using lo_el_t = ScalarT;
      using lo_shr_t = std::array<lo_el_t, 2>;
      NdArrayView<lo_shr_t> _lo(lo);

      return DISPATCH_UINT_PT_TYPES(out_btype, "_", [&]() {
        using out_el_t = ScalarT;
        using out_shr_t = std::array<out_el_t, 2>;
        NdArrayView<out_shr_t> _out(out);

        pforeach(0, lo.numel(), [&](int64_t idx) {
          const auto& l = _lo[idx];
          const auto& h = _hi[idx];
          out_shr_t& o = _out[idx];
          o[0] = l[0] | (static_cast<out_el_t>(h[0]) << lo_ty->nbits());
          o[1] = l[1] | (static_cast<out_el_t>(h[1]) << lo_ty->nbits());
        });
        return out;
      });
    });
  });
}

NdArrayRef pack_2_bitvec_mss(const NdArrayRef& lo, const NdArrayRef& hi) {
  const auto* lo_ty = lo.eltype().as<BShrTyMss>();
  const auto* hi_ty = hi.eltype().as<BShrTyMss>();

  assert(lo_ty->nbits() == hi_ty->nbits());
  const size_t out_nbits = lo_ty->nbits() + hi_ty->nbits();
  const PtType out_btype = calcBShareBacktype(out_nbits);
  NdArrayRef out(makeType<BShrTyMss>(out_btype, out_nbits), lo.shape());

  return DISPATCH_UINT_PT_TYPES(hi_ty->getBacktype(), "_", [&]() {
    using hi_el_t = ScalarT;
    using hi_shr_t = std::array<hi_el_t, 3>;
    NdArrayView<hi_shr_t> _hi(hi);

    return DISPATCH_UINT_PT_TYPES(lo_ty->getBacktype(), "_", [&]() {
      using lo_el_t = ScalarT;
      using lo_shr_t = std::array<lo_el_t, 3>;
      NdArrayView<lo_shr_t> _lo(lo);

      return DISPATCH_UINT_PT_TYPES(out_btype, "_", [&]() {
        using out_el_t = ScalarT;
        using out_shr_t = std::array<out_el_t, 3>;
        NdArrayView<out_shr_t> _out(out);

        pforeach(0, lo.numel(), [&](int64_t idx) {
          const auto& l = _lo[idx];
          const auto& h = _hi[idx];
          out_shr_t& o = _out[idx];
          o[0] = l[0] | (static_cast<out_el_t>(h[0]) << lo_ty->nbits());
          o[1] = l[1] | (static_cast<out_el_t>(h[1]) << lo_ty->nbits());
          o[2] = l[2] | (static_cast<out_el_t>(h[2]) << lo_ty->nbits());
        });
        return out;
      });
    });
  });
}

std::pair<NdArrayRef, NdArrayRef> unpack_2_bitvec_ass(const NdArrayRef& in) {
  const auto* in_ty = in.eltype().as<BShrTy>();
  assert(in_ty->nbits() != 0 && in_ty->nbits() % 2 == 0);

  const size_t lo_nbits = in_ty->nbits() / 2;
  const size_t hi_nbits = in_ty->nbits() - lo_nbits;
  const PtType lo_btype = calcBShareBacktype(lo_nbits);
  const PtType hi_btype = calcBShareBacktype(hi_nbits);
  NdArrayRef lo(makeType<BShrTy>(lo_btype, lo_nbits), in.shape());
  NdArrayRef hi(makeType<BShrTy>(hi_btype, hi_nbits), in.shape());

  return DISPATCH_UINT_PT_TYPES(in_ty->getBacktype(), "_", [&]() {
    using in_el_t = ScalarT;
    using in_shr_t = std::array<in_el_t, 2>;
    NdArrayView<in_shr_t> _in(in);

    return DISPATCH_UINT_PT_TYPES(lo_btype, "_", [&]() {
      using lo_el_t = ScalarT;
      using lo_shr_t = std::array<lo_el_t, 2>;
      NdArrayView<lo_shr_t> _lo(lo);

      return DISPATCH_UINT_PT_TYPES(hi_btype, "_", [&]() {
        using hi_el_t = ScalarT;
        using hi_shr_t = std::array<hi_el_t, 2>;
        NdArrayView<hi_shr_t> _hi(hi);

        pforeach(0, in.numel(), [&](int64_t idx) {
          const auto& i = _in[idx];
          lo_shr_t& l = _lo[idx];
          hi_shr_t& h = _hi[idx];
          l[0] = i[0] & ((1 << lo_nbits) - 1);
          h[0] = (i[0] >> lo_nbits) & ((1 << hi_nbits) - 1);
        });
        return std::make_pair(hi, lo);
      });
    });
  });
}

std::pair<NdArrayRef, NdArrayRef> unpack_2_bitvec_rss(const NdArrayRef& in) {
  const auto* in_ty = in.eltype().as<BShrTy>();
  assert(in_ty->nbits() != 0 && in_ty->nbits() % 2 == 0);

  const size_t lo_nbits = in_ty->nbits() / 2;
  const size_t hi_nbits = in_ty->nbits() - lo_nbits;
  const PtType lo_btype = calcBShareBacktype(lo_nbits);
  const PtType hi_btype = calcBShareBacktype(hi_nbits);
  NdArrayRef lo(makeType<BShrTy>(lo_btype, lo_nbits), in.shape());
  NdArrayRef hi(makeType<BShrTy>(hi_btype, hi_nbits), in.shape());

  return DISPATCH_UINT_PT_TYPES(in_ty->getBacktype(), "_", [&]() {
    using in_el_t = ScalarT;
    using in_shr_t = std::array<in_el_t, 2>;
    NdArrayView<in_shr_t> _in(in);

    return DISPATCH_UINT_PT_TYPES(lo_btype, "_", [&]() {
      using lo_el_t = ScalarT;
      using lo_shr_t = std::array<lo_el_t, 2>;
      NdArrayView<lo_shr_t> _lo(lo);

      return DISPATCH_UINT_PT_TYPES(hi_btype, "_", [&]() {
        using hi_el_t = ScalarT;
        using hi_shr_t = std::array<hi_el_t, 2>;
        NdArrayView<hi_shr_t> _hi(hi);

        pforeach(0, in.numel(), [&](int64_t idx) {
          const auto& i = _in[idx];
          lo_shr_t& l = _lo[idx];
          hi_shr_t& h = _hi[idx];
          l[0] = i[0] & ((1 << lo_nbits) - 1);
          l[1] = i[1] & ((1 << lo_nbits) - 1);
          h[0] = (i[0] >> lo_nbits) & ((1 << hi_nbits) - 1);
          h[1] = (i[1] >> lo_nbits) & ((1 << hi_nbits) - 1);
        });
        return std::make_pair(hi, lo);
      });
    });
  });
}

std::pair<NdArrayRef, NdArrayRef> unpack_2_bitvec_mss(const NdArrayRef& in) {
  const auto* in_ty = in.eltype().as<BShrTyMss>();
  assert(in_ty->nbits() != 0 && in_ty->nbits() % 2 == 0);

  const size_t lo_nbits = in_ty->nbits() / 2;
  const size_t hi_nbits = in_ty->nbits() - lo_nbits;
  const PtType lo_btype = calcBShareBacktype(lo_nbits);
  const PtType hi_btype = calcBShareBacktype(hi_nbits);
  NdArrayRef lo(makeType<BShrTyMss>(lo_btype, lo_nbits), in.shape());
  NdArrayRef hi(makeType<BShrTyMss>(hi_btype, hi_nbits), in.shape());

  return DISPATCH_UINT_PT_TYPES(in_ty->getBacktype(), "_", [&]() {
    using in_el_t = ScalarT;
    using in_shr_t = std::array<in_el_t, 3>;
    NdArrayView<in_shr_t> _in(in);

    return DISPATCH_UINT_PT_TYPES(lo_btype, "_", [&]() {
      using lo_el_t = ScalarT;
      using lo_shr_t = std::array<lo_el_t, 3>;
      NdArrayView<lo_shr_t> _lo(lo);

      return DISPATCH_UINT_PT_TYPES(hi_btype, "_", [&]() {
        using hi_el_t = ScalarT;
        using hi_shr_t = std::array<hi_el_t, 3>;
        NdArrayView<hi_shr_t> _hi(hi);

        pforeach(0, in.numel(), [&](int64_t idx) {
          const auto& i = _in[idx];
          lo_shr_t& l = _lo[idx];
          hi_shr_t& h = _hi[idx];
          l[0] = i[0] & ((1 << lo_nbits) - 1);
          l[1] = i[1] & ((1 << lo_nbits) - 1);
          l[2] = i[2] & ((1 << lo_nbits) - 1);
          h[0] = (i[0] >> lo_nbits) & ((1 << hi_nbits) - 1);
          h[1] = (i[1] >> lo_nbits) & ((1 << hi_nbits) - 1);
          h[2] = (i[2] >> lo_nbits) & ((1 << hi_nbits) - 1);
        });
        return std::make_pair(hi, lo);
      });
    });
  });
}

NdArrayRef MsbA2BMultiFanIn(KernelEvalContext* ctx, const NdArrayRef& in, size_t start_rank) {
  const auto field = in.eltype().as<AShrTy>()->field();
  const auto numel = in.numel();
  auto* comm = ctx->getState<Communicator>();
  auto* prg_state = ctx->getState<PrgState>();

  const size_t start_rank_next = (start_rank + 1) % 3;

  #define EQ_U64(x) static_cast<uint64_t>(x)

  // First construct 2 boolean shares.
  // Let
  //   X = [(x0, x1), (x1, x2), (x2, x0)] as input.
  //   Z = (z0, z1, z2) as boolean zero share.
  //
  // Construct edabitsB = [(ebb0, ebb1), (ebb1, ebb2), (ebb2, ebb0)] as boolean shares,
  //   edabitsA = [(eba0, eba1), (eba1, eba2), (eba2, eba0)] as arithmetic shares,
  //   where edabitsA = edabitsB.
  //
  // Open mask = x - edabitsA.
  //
  // That
  //  mask + edabitsB = x0 + x1 + x2 = X
  const Type rss_ashr_type =
      makeType<AShrTy>(field);
  const Type rss_bshr_type =
      makeType<BShrTy>(GetStorageType(field), SizeOf(field) * 8);
  const Type mss_bshr_type =
      makeType<BShrTyMss>(GetStorageType(field), SizeOf(field) * 8);

  NdArrayRef m(mss_bshr_type, in.shape());
  NdArrayRef n(mss_bshr_type, in.shape());
  NdArrayRef p(mss_bshr_type, in.shape());
  NdArrayRef g(mss_bshr_type, in.shape());
  NdArrayRef out(rss_bshr_type, in.shape());

  return DISPATCH_ALL_FIELDS(field, "alkaid.msb.split", [&]() {
    using el_t = ring2k_t;
    using rss_shr_t = std::array<el_t, 2>;
    using mss_shr_t = std::array<el_t, 3>;

    NdArrayView<rss_shr_t> _in(in);           // rss
    NdArrayView<mss_shr_t> _m(m);
    NdArrayView<mss_shr_t> _n(n);
    NdArrayView<rss_shr_t> _out(out);

    /**
     * 1. Convert RSS-shared x into MSS-shared m (Dm, RSS(dm)) and n (Dn, RSS(dn)).
    */
    // generate (compressed) correlated randomness: ((dm0, dm1), (dm1, dn2), (dn2, dm0)). 
    std::vector<el_t> r0(numel, 0);
    std::vector<el_t> r1(numel, 0);
    
    prg_state->fillPrssPair(r0.data(), r1.data(), r0.size(),
                            PrgState::GenPrssCtrl::Both);
    #ifndef EQ_USE_PRG_STATE
    std::fill(r0.begin(), r0.end(), 0);
    std::fill(r1.begin(), r1.end(), 0);
    #endif

    // copy the correlated randomness into m and n
    pforeach(0, numel, [&](int64_t idx) {
      if (comm->getRank() == start_rank) 
      {
        // Wait for x2 ^ dn2 from P1.
        _m[idx][1] = r0[idx];                               // dm0
        _m[idx][2] = r1[idx];                               // dm1
        r1[idx] ^= r0[idx] ^ (_in[idx][0] + _in[idx][1]);     
        _m[idx][0] = r1[idx];                               // Dm = (x0 + x1) ^ dm0 ^ dm1

        _n[idx][1] = 0;
        _n[idx][2] = 0;
      } 
      else if (comm->getRank() == start_rank_next) 
      {
        // Wait for Dm from P0.
        _m[idx][1] = r0[idx];                               // dm1
        _n[idx][2] = r1[idx];                               // dn2
        r1[idx] ^= _in[idx][1];                             // dn2 ^ x2
        _n[idx][0] = r1[idx];                               // Dn = x2 ^ dn2

        _m[idx][2] = 0;
        _n[idx][1] = 0;
      }
      else
      {
        // Wait for Dm from P0.
        _n[idx][1] = r0[idx];                               // dn2
        _m[idx][2] = r1[idx];                               // dm0
        _n[idx][0] = _in[idx][0] ^ r0[idx];                 // Dn = x2 ^ dn2

        _m[idx][1] = 0;
        _n[idx][2] = 0;
      }
    });

    // rotate k bits
    r0 = comm->bcast<el_t>(r1, 0, "MsbA2B, special resharing from ASS to MSS, broadcast Dm");
    if (comm->getRank() == start_rank) 
    {
      r0 = comm->recv<el_t>(start_rank_next, "MsbA2B, special resharing from ASS to MSS, get dn2");
      // comm->addCommStatsManually(0, -sizeof(el_t) * numel);   
    }
    else if (comm->getRank() == start_rank_next) 
    {
      comm->sendAsync<el_t>(start_rank, r1, "MsbA2B, special resharing from ASS to MSS, send dn2");
      // comm->addCommStatsManually(-1, 0);
    }

    // compute external value Dm, Dn
    pforeach(0, numel, [&](int64_t idx) {
      if (comm->getRank() == start_rank) 
      {
        _n[idx][0] = r0[idx];                               // Dn = x2 + dn2
      } 
      else if (comm->getRank() == start_rank_next) 
      {
        _m[idx][0] = r0[idx];                              // Dm = (x0 + x1) ^ dm0 ^ dm1
      }
      else
      {
        _m[idx][0] = r0[idx];                            
      }
    });

    // 4. generate signal p and g.
    NdArrayView<mss_shr_t> _p(p);
    NdArrayView<mss_shr_t> _g(g);

    auto sig_g_rss = MssAnd2NoComm(ctx, m, n);
    auto sig_g_mss = ResharingAss2Mss(ctx, ResharingRss2Ass(ctx, sig_g_rss));
    NdArrayView<mss_shr_t> _g_mss(sig_g_mss);
    pforeach(0, numel, [&](int64_t idx) {
      _p[idx][0] = _m[idx][0] ^ _n[idx][0];
      _p[idx][1] = _m[idx][1] ^ _n[idx][1];
      _p[idx][2] = _m[idx][2] ^ _n[idx][2];
      _g[idx][0] = _g_mss[idx][0];
      _g[idx][1] = _g_mss[idx][1];
      _g[idx][2] = _g_mss[idx][2];
    });

    // 5. PPA.
    // we dont use the carryout circuit from aby 2.0. By limitting p's msb to be 1 and g's msb to be 0,
    // we could build a simpler carryout circuit.
    size_t nbits = SizeOf(field) * 8 - 1;
    size_t k = nbits;
    
    pforeach(0, numel, [&](int64_t idx) {
      _out[idx][0]  = (_p[idx][0] ^ _p[idx][1]) >> nbits;
      _out[idx][1]  = (_p[idx][0] ^ _p[idx][2]) >> nbits;
      _p[idx][0]    = ( 1ull << nbits     ) | _p[idx][0];    
      _p[idx][1]    = ((1ull << nbits) - 1) & _p[idx][1];
      _p[idx][2]    = ((1ull << nbits) - 1) & _p[idx][2];
      _g[idx][0]    = ((1ull << nbits) - 1) & _g[idx][0];
      _g[idx][1]    = ((1ull << nbits) - 1) & _g[idx][1];
      _g[idx][2]    = ((1ull << nbits) - 1) & _g[idx][2];
    });

    while (k > 1) 
    {
      NdArrayRef pops[4];
      NdArrayRef gops[4];

      auto [g_hi, g_lo] = bit_split_mss(g);
      std::tie(gops[3], gops[1]) = bit_split_mss(g_hi);
      std::tie(gops[2], gops[0]) = bit_split_mss(g_lo);
      auto [p_hi, p_lo] = bit_split_mss(p);
      std::tie(pops[3], pops[1]) = bit_split_mss(p_hi);
      std::tie(pops[2], pops[0]) = bit_split_mss(p_lo);

      auto p_res      = MssAnd4NoComm(ctx, pops[0], pops[1], pops[2], pops[3]);
      auto g_res_3    = ResharingRss2Ass(ctx, ResharingMss2Rss(ctx, gops[3]));
      auto g_res_2    = ResharingRss2Ass(ctx, MssAnd2NoComm(ctx, gops[2], pops[3]));
      auto g_res_1    = MssAnd3NoComm(ctx, gops[1], pops[3], pops[2]);
      auto g_res_0    = MssAnd4NoComm(ctx, gops[0], pops[3], pops[2], pops[1]);
      auto g_combined = AssXor2(ctx, AssXor2(ctx, g_res_0, g_res_1), AssXor2(ctx, g_res_2, g_res_3));

      // online communication
      k /= 4;
      if (k > 1)
      {
        auto pg = pack_2_bitvec_ass(p_res, g_combined);
        pg = ResharingAss2Mss(ctx, pg);
        std::tie(g, p) = unpack_2_bitvec_mss(pg);
        // p = ResharingAss2Mss(ctx, p_res);
        // g = ResharingAss2Mss(ctx, g_combined);
      } else {
        auto pg = pack_2_bitvec_ass(p_res, g_combined);
        pg = ResharingAss2Rss(ctx, pg);
        std::tie(g, p) = unpack_2_bitvec_rss(pg);
        // p = ResharingAss2Rss(ctx, p_res);
        // g = ResharingAss2Rss(ctx, g_combined);
      }
    }

    NdArrayView<std::array<uint8_t, 2>> _g_rss(g);
    pforeach(0, numel, [&](size_t idx) {
      _out[idx][0] ^= (static_cast<uint64_t>(_g_rss[idx][0]));
      _out[idx][1] ^= (static_cast<uint64_t>(_g_rss[idx][1]));
    });

    return out;
  });  
}

/**
 * A 4 fan-in 4 outputs protocol for black cell in PPA.
 */
std::pair<NdArrayRef, NdArrayRef> PGCell_4FanIn4Out(KernelEvalContext* ctx, 
  const NdArrayRef& p0, const NdArrayRef& p1, const NdArrayRef& p2, const NdArrayRef& p3,
  const NdArrayRef& g0, const NdArrayRef& g1, const NdArrayRef& g2, const NdArrayRef& g3) 

// std::array<NdArrayRef, 8> PGCell_4FanIn4Out(KernelEvalContext* ctx, 
//   const NdArrayRef& p, const NdArrayRef& g, const size_t nbits, const size_t mask, const size_t stride) 
{
  /**
   *  p3    p2    p1    p0
   *  g3    g2    g1    g0
   * --------------------------------
   *  g'3   g'2   g'1   g'0
   *  p'3   p'2   p'1   p'0
   * where 
   * p'3 = (p0 & p1) & (p2 & p3)
   * p'2 = (p0 & p1) & p2
   * p'1 = (p0 & p1)
   * p'0 = p0
   * g'3 = g3 ^ g2 & p3 ^ g1 & (p2 & p3) ^ (g0 & p1) & (p2 & p3)
   * g'2 = g2 ^ g1 & p2 ^ (g0 & p1) & p2
   * g'1 = g1 ^ (g0 & p1)
   * g'0 = g0.
   * 
   * All the AND gates is concluded here:
   * AND2 in MSS:
   *  p01_rss = p0 & p1, p23_rss = p2 & p3, g0p1_rss = g0 & p1
   * AND2 in RSS:
   *  p0123_ass = p01_rss & p23_rss, p012_ass = p01_rss & p2_rss
   *  g2p3_ass = g2_rss & p3_rss, g1p23_ass = g1_rss & p23_rss, g0p123_ass = g0p1_rss & p23_rss
   *  g1p2_ass = g1_rss & p2_rss, g0p12_ass = g0p1_rss & p2_rss
   *  
   * All the Resharing steps is here:
   *  p3 -> p3_rss, p2 -> p2_rss, g2 -> g2_rss, g1 -> g1_rss              (down)
   *  p01_rss -> p01_mss, p012_ass -> p012_mss, p0123_ass -> p0123_mss    (up)
   *  gr3_ass -> gr3_mss, gr2_ass -> gr2_mss, gr1_rss -> gr1_mss          (up)
   */

  const auto* in_ty = p0.eltype().as<BShrTyMss>();
  // const size_t out_nbits = in_ty->nbits();
  // const PtType out_btype = calcBShareBacktype(out_nbits);

  const auto numel = p0.numel();
  auto* comm = ctx->getState<Communicator>();

  return DISPATCH_UINT_PT_TYPES(in_ty->getBacktype(), "_", [&]() {
    using el_t = ScalarT;
    using rss_shr_t = std::array<el_t, 2>;

    auto p3_rss = ResharingMss2Rss(ctx, p3);
    auto p2_rss = ResharingMss2Rss(ctx, p2);
    auto g2_rss = ResharingMss2Rss(ctx, g2);
    auto g1_rss = ResharingMss2Rss(ctx, g1);

    auto p01_rss = MssAnd2NoComm(ctx, p0, p1);
    auto p23_rss = MssAnd2NoComm(ctx, p2, p3);
    auto g0p1_rss = MssAnd2NoComm(ctx, g0, p1);

    auto p0123_ass = RssAnd2NoComm(ctx, p01_rss, p23_rss);
    auto p012_ass = RssAnd2NoComm(ctx, p01_rss, p2_rss);
    auto g2p3_ass = RssAnd2NoComm(ctx, g2_rss, p3_rss);
    auto g1p23_ass = RssAnd2NoComm(ctx, g1_rss, p23_rss);
    auto g0p123_ass = RssAnd2NoComm(ctx, g0p1_rss, p23_rss);
    auto g1p2_ass = RssAnd2NoComm(ctx, g1_rss, p2_rss);
    auto g0p12_ass = RssAnd2NoComm(ctx, g0p1_rss, p2_rss);
    
    // gr3 = g3 ^ gr3_ass
    auto gr3_ass = AssXor2(ctx, g2p3_ass, AssXor2(ctx, g1p23_ass, g0p123_ass));
    auto gr2_ass = AssXor2(ctx, g1p2_ass, g0p12_ass);
    auto gr1_ass = ResharingRss2Ass(ctx, g0p1_rss);
    auto gr0_ass = ResharingRss2Ass(ctx, ResharingMss2Rss(ctx, g0));
    auto pr3_ass = p0123_ass;
    auto pr2_ass = p012_ass;
    auto pr1_ass = ResharingRss2Ass(ctx, p01_rss);
    auto pr0_ass = ResharingRss2Ass(ctx, ResharingMss2Rss(ctx, p0));
    auto g3_ass = ResharingRss2Ass(ctx, ResharingMss2Rss(ctx, g3));
    auto g2_ass = ResharingRss2Ass(ctx, g2_rss);
    auto g1_ass = ResharingRss2Ass(ctx, g1_rss);

    NdArrayView<rss_shr_t> _gr3(gr3_ass);
    NdArrayView<rss_shr_t> _gr2(gr2_ass);
    NdArrayView<rss_shr_t> _gr1(gr1_ass);
    NdArrayView<rss_shr_t> _gr0(gr0_ass);
    NdArrayView<rss_shr_t> _pr3(pr3_ass);
    NdArrayView<rss_shr_t> _pr2(pr2_ass);
    NdArrayView<rss_shr_t> _pr1(pr1_ass);
    NdArrayView<rss_shr_t> _pr0(pr0_ass);
    NdArrayView<rss_shr_t> _g3(g3_ass);
    NdArrayView<rss_shr_t> _g2(g2_ass);
    NdArrayView<rss_shr_t> _g1(g1_ass);
    pforeach(0, numel, [&](int64_t idx) {

      _gr3[idx][0] ^= rshift(_gr2[idx][0], 1) ^ rshift(_gr1[idx][0], 2) ^ rshift(_gr0[idx][0], 3) \
        ^ _g3[idx][0] ^ rshift(_g2[idx][0], 1) ^ rshift(_g1[idx][0], 2);
      _gr3[idx][1] ^= rshift(_gr2[idx][1], 1) ^ rshift(_gr1[idx][1], 2) ^ rshift(_gr0[idx][1], 3) \
        ^ _g3[idx][0] ^ rshift(_g2[idx][0], 1) ^ rshift(_g1[idx][0], 2);
      _pr3[idx][0] ^= rshift(_pr2[idx][0], 1) ^ rshift(_pr1[idx][0], 2) ^ rshift(_pr0[idx][0], 3);
      _pr3[idx][1] ^= rshift(_pr2[idx][1], 1) ^ rshift(_pr1[idx][1], 2) ^ rshift(_pr0[idx][0], 3);
    });

    std::pair<NdArrayRef, NdArrayRef> result;
    result.first = ResharingAss2Mss(ctx, gr3_ass);
    result.second = ResharingAss2Mss(ctx, pr3_ass);
    comm->addCommStatsManually(-1, 0);

    return result;
  });
}

/**
 * A 4 fan-in 1 output protocol for black cell in PPA.
 */
std::pair<NdArrayRef, NdArrayRef> PGCell_4FanIn1Out(KernelEvalContext* ctx, 
  const NdArrayRef& p0, const NdArrayRef& p1, const NdArrayRef& p2, const NdArrayRef& p3,
  const NdArrayRef& g0, const NdArrayRef& g1, const NdArrayRef& g2, const NdArrayRef& g3) 

// std::array<NdArrayRef, 8> PGCell_4FanIn4Out(KernelEvalContext* ctx, 
//   const NdArrayRef& p, const NdArrayRef& g, const size_t nbits, const size_t mask, const size_t stride) 
{
  /**
   *  p3    p2    p1    p0
   *  g3    g2    g1    g0
   * --------------------------------
   *  g'3   g2    g1    g0 
   *  p'3   p2    p1    p0
   * where 
   * p'3 = (p0 & p1) & (p2 & p3)
   * g'3 = g3 ^ g2 & p3 ^ g1 & (p2 & p3) ^ (g0 & p1) & (p2 & p3)
   * 
   * All the AND gates is concluded here:
   * AND2 in MSS:
   *  p01_rss = p0 & p1, p23_rss = p2 & p3, g0p1_rss = g0 & p1
   * AND2 in RSS:
   *  p0123_ass = p01_rss & p23_rss, p012_ass = p01_rss & p2_rss
   *  g2p3_ass = g2_rss & p3_rss, g1p23_ass = g1_rss & p23_rss, g0p123_ass = g0p1_rss & p23_rss
   *  g1p2_ass = g1_rss & p2_rss, g0p12_ass = g0p1_rss & p2_rss
   *  
   * All the Resharing steps is here:
   *  p3 -> p3_rss, p2 -> p2_rss, g2 -> g2_rss, g1 -> g1_rss              (down)
   *  p01_rss -> p01_mss, p012_ass -> p012_mss, p0123_ass -> p0123_mss    (up)
   *  gr3_ass -> gr3_mss, gr2_ass -> gr2_mss, gr1_rss -> gr1_mss          (up)
   */

  // // const size_t out_nbits = in_ty->nbits();
  // // const PtType out_btype = calcBShareBacktype(out_nbits);
  // const auto numel = p0.numel();
  auto* comm = ctx->getState<Communicator>();
  // const auto* in_ty = p0.eltype().as<BShrTyMss>();
  // const size_t in_nbits = in_ty->nbits();
  // SPU_ENFORCE(in_nbits != 0 && in_nbits % 4 == 0, "in_nbits={}", in_nbits);
  // const size_t comm_nbits = in_nbits / 4;
  // const auto comm_backtype = calcBShareBacktype(comm_nbits);
  // const auto comm_type = makeType<BShrTyMss>(comm_backtype, comm_nbits);

  // return DISPATCH_UINT_PT_TYPES(in_ty->getBacktype(), "_", [&]() {

  //   using el_t = ScalarT;
  //   using mss_shr_t = std::array<el_t, 3>;

    auto p3_rss = ResharingMss2Rss(ctx, p3);
    auto g2_rss = ResharingMss2Rss(ctx, g2);
    auto g1_rss = ResharingMss2Rss(ctx, g1);

    auto p01_rss = MssAnd2NoComm(ctx, p0, p1);
    auto p23_rss = MssAnd2NoComm(ctx, p2, p3);
    auto g0p1_rss = MssAnd2NoComm(ctx, g0, p1);

    auto p0123_ass = RssAnd2NoComm(ctx, p01_rss, p23_rss);
    auto g2p3_ass = RssAnd2NoComm(ctx, g2_rss, p3_rss);
    auto g1p23_ass = RssAnd2NoComm(ctx, g1_rss, p23_rss);
    auto g0p123_ass = RssAnd2NoComm(ctx, g0p1_rss, p23_rss);

    auto g3_ass = ResharingRss2Ass(ctx, ResharingMss2Rss(ctx, g3));
    
    // gr3 = g3 ^ gr3_ass
    auto gr3_ass = AssXor2(ctx, AssXor2(ctx, g3_ass, g2p3_ass), AssXor2(ctx, g1p23_ass, g0p123_ass));
    auto pr3_ass = p0123_ass;

    auto gr3_mss = ResharingAss2Mss(ctx, gr3_ass);
    auto pr3_mss = ResharingAss2Mss(ctx, pr3_ass);
    comm->addCommStatsManually(-1, 0);

    // NdArrayView<mss_shr_t> _gr3(gr3_mss);
    // NdArrayView<mss_shr_t> _pr3(pr3_mss);
    // NdArrayView<mss_shr_t> _g2(g2);
    // NdArrayView<mss_shr_t> _g1(g1);
    // NdArrayView<mss_shr_t> _g0(g0);
    // NdArrayView<mss_shr_t> _p2(p2);
    // NdArrayView<mss_shr_t> _p1(p1);
    // NdArrayView<mss_shr_t> _p0(p0);
    // pforeach(0, numel, [&](int64_t idx) {

    //   _gr3[idx][0] ^= rshift(_g2[idx][0], 1) ^ rshift(_g1[idx][0], 2) ^ rshift(_g0[idx][0], 3);
    //   _gr3[idx][1] ^= rshift(_g2[idx][1], 1) ^ rshift(_g1[idx][1], 2) ^ rshift(_g0[idx][1], 3);
    //   _gr3[idx][2] ^= rshift(_g2[idx][2], 1) ^ rshift(_g1[idx][2], 2) ^ rshift(_g0[idx][2], 3);

    //   _pr3[idx][0] ^= rshift(_p2[idx][0], 1) ^ rshift(_p1[idx][0], 2) ^ rshift(_p0[idx][0], 3);
    //   _pr3[idx][1] ^= rshift(_p2[idx][1], 1) ^ rshift(_p1[idx][1], 2) ^ rshift(_p0[idx][1], 3);
    //   _pr3[idx][2] ^= rshift(_p2[idx][2], 1) ^ rshift(_p1[idx][2], 2) ^ rshift(_p0[idx][2], 3);
    // });

    std::pair<NdArrayRef, NdArrayRef> result;
    result.first = gr3_mss;
    result.second = pr3_mss;

    return result;
  // });
}

NdArrayRef A2BMultiFanIn(KernelEvalContext* ctx, const NdArrayRef& in) {
  const auto field = in.eltype().as<AShrTy>()->field();
  const auto numel = in.numel();
  auto* comm = ctx->getState<Communicator>();
  auto* prg_state = ctx->getState<PrgState>();
  #define EQ_U64(x) static_cast<uint64_t>(x)

  // First construct 2 boolean shares.
  // Let
  //   X = [(x0, x1), (x1, x2), (x2, x0)] as input.
  //   Z = (z0, z1, z2) as boolean zero share.
  //
  // Construct edabitsB = [(ebb0, ebb1), (ebb1, ebb2), (ebb2, ebb0)] as boolean shares,
  //   edabitsA = [(eba0, eba1), (eba1, eba2), (eba2, eba0)] as arithmetic shares,
  //   where edabitsA = edabitsB.
  //
  // Open mask = x - edabitsA.
  //
  // That
  //  mask + edabitsB = x0 + x1 + x2 = X
  const Type rss_ashr_type =
      makeType<AShrTy>(field);
  const Type rss_bshr_type =
      makeType<BShrTy>(GetStorageType(field), SizeOf(field) * 8);
  const Type mss_bshr_type =
      makeType<BShrTyMss>(GetStorageType(field), SizeOf(field) * 8);

  NdArrayRef m(mss_bshr_type, in.shape());
  NdArrayRef n(mss_bshr_type, in.shape());

  NdArrayRef p(mss_bshr_type, in.shape());
  NdArrayRef g(mss_bshr_type, in.shape());
  NdArrayRef c(rss_bshr_type, in.shape());
  NdArrayRef out(rss_bshr_type, in.shape());
  return DISPATCH_ALL_FIELDS(field, "alkaid.msb.split", [&]() {
    using el_t = ring2k_t;
    using rss_shr_t = std::array<el_t, 2>;
    using mss_shr_t = std::array<el_t, 3>;

    NdArrayView<rss_shr_t> _in(in);           // rss
    
    // NdArrayView<rss_shr_t> _eba(edabitsA);    
    // NdArrayView<mss_shr_t> _ebb(edabitsB);    

    NdArrayView<mss_shr_t> _m(m);
    NdArrayView<mss_shr_t> _n(n);
    NdArrayView<rss_shr_t> _out(out);

    /**
     * 1. Convert RSS-shared x into MSS-shared m (Dm, RSS(dm)) and n (Dn, RSS(dn)).
    */
    // generate (compressed) correlated randomness: ((dm0, dm1), (dm1, dn2), (dn2, dm0)). 
    std::vector<el_t> r0(numel, 0);
    std::vector<el_t> r1(numel, 0);
    prg_state->fillPrssPair(r0.data(), r1.data(), r0.size(),
                            PrgState::GenPrssCtrl::Both);
    #ifndef EQ_USE_PRG_STATE
    std::fill(r0.begin(), r0.end(), 0);
    std::fill(r1.begin(), r1.end(), 0);
    #endif

    // copy the correlated randomness into m and n
    pforeach(0, numel, [&](int64_t idx) {
      if (comm->getRank() == 0) 
      {
        // Wait for x2 ^ dn2 from P1.
        _m[idx][1] = r0[idx];                               // dm0
        _m[idx][2] = r1[idx];                               // dm1
        r1[idx] ^= r0[idx] ^ (_in[idx][0] + _in[idx][1]);     
        _m[idx][0] = r1[idx];                               // Dm = (x0 + x1) ^ dm0 ^ dm1

        _n[idx][1] = 0;
        _n[idx][2] = 0;
      } 
      else if (comm->getRank() == 1) 
      {
        // Wait for Dm from P0.
        _m[idx][1] = r0[idx];                               // dm1
        _n[idx][2] = r1[idx];                               // dn2
        r1[idx] ^= _in[idx][1];                             // dn2 ^ x2
        _n[idx][0] = r1[idx];                               // Dn = x2 ^ dn2

        _m[idx][2] = 0;
        _n[idx][1] = 0;
      }
      else
      {
        // Wait for Dm from P0.
        _n[idx][1] = r0[idx];                               // dn2
        _m[idx][2] = r1[idx];                               // dm0
        _n[idx][0] = _in[idx][0] ^ r0[idx];                 // Dn = x2 ^ dn2

        _m[idx][1] = 0;
        _n[idx][2] = 0;
      }
    });

    // rotate k bits
    r0 = comm->bcast<el_t>(r1, 0, "MsbA2B, special resharing from ASS to MSS, broadcast Dm");
    if (comm->getRank() == 0) 
    {
      r0 = comm->recv<el_t>(1, "MsbA2B, special resharing from ASS to MSS, get dn2");
      comm->addCommStatsManually(-1, 0);
    }
    else if (comm->getRank() == 1) 
    {
      comm->sendAsync<el_t>(0, r1, "MsbA2B, special resharing from ASS to MSS, send dn2");
      comm->addCommStatsManually(-1, 0);
    }

    // compute external value Dm, Dn
    pforeach(0, numel, [&](int64_t idx) {
      if (comm->getRank() == 0) 
      {
        _n[idx][0] = r0[idx];                               // Dn = x2 + dn2
      } 
      else if (comm->getRank() == 1) 
      {
        _m[idx][0] = r0[idx];                              // Dm = (x0 + x1) ^ dm0 ^ dm1
      }
      else
      {
        _m[idx][0] = r0[idx];                            
      }
    });

    // if (comm->getRank() == 0) std::cout << "PPA: m " << _m[0][0] << " " << _m[1][0] << std::endl;
    // if (comm->getRank() == 0) std::cout << "PPA: n " << _n[0][0] << " " << _n[1][0] << std::endl;

    // 4. generate signal p and g.
    NdArrayView<mss_shr_t> _p(p);
    NdArrayView<mss_shr_t> _g(g);

    auto sig_g_rss = MssAnd2NoComm(ctx, m, n);
    auto sig_g_mss = ResharingAss2Mss(ctx, ResharingRss2Ass(ctx, sig_g_rss));
    NdArrayView<mss_shr_t> _g_mss(sig_g_mss);
    // if (comm->getRank() == 0) std::cout << "PPA: sig_g_mss " << _g_mss[0][0] << " " << _g_mss[1][0] << std::endl;
    pforeach(0, numel, [&](int64_t idx) {
      _p[idx][0] = _m[idx][0] ^ _n[idx][0];
      _p[idx][1] = _m[idx][1] ^ _n[idx][1];
      _p[idx][2] = _m[idx][2] ^ _n[idx][2];
      _g[idx][0] = _g_mss[idx][0];
      _g[idx][1] = _g_mss[idx][1];
      _g[idx][2] = _g_mss[idx][2];
    });
    // assert(_p[0][1] == 0 && _p[1][1] == 0 && _p[2][1] == 0 && _p[0][2] == 0 && _p[1][2] == 0 && _p[2][2] == 0);
    // assert(_g[0][1] == 0 && _g[1][1] == 0 && _g[2][1] == 0 && _g[0][2] == 0 && _g[1][2] == 0 && _g[2][2] == 0);

    // if (comm->getRank() == 0) std::cout << "PPA: generate signal p and signal g." << std::endl;
    // if (comm->getRank() == 0) std::cout << "PPA: signal p." << _p[0][0] << " " << _p[1][0] << std::endl;
    // if (comm->getRank() == 0) std::cout << "PPA: signal g." << _g[0][0] << " " << _g[1][0] << std::endl;

    // 5. PPA.
    // we dont use the carryout circuit from aby 2.0. By limitting p's msb to be 1 and g's msb to be 0,
    // we could build a simpler carryout circuit.
    // size_t nbits = SizeOf(field) * 8 - 1;
    // size_t k = nbits;
    
    pforeach(0, numel, [&](int64_t idx) {
      _out[idx][0] = _p[idx][0] ^ _p[idx][1];
      _out[idx][1] = _p[idx][0] ^ _p[idx][2];
    });

    // uint64_t ONLY. do not use el_t as it involves lshift error.
    std::array<uint64_t, 3> bit_mask = {
      0x1111111111111111ull,
      0x8888888888888888ull,
      0x8888888888888888ull
    };
    std::array<uint64_t, 3> bit_offset = {
      1ull, 4ull, 16ull 
    };

    // if (comm->getRank() == 0) std::cout << "PPA: initialize." << std::endl;

    // Construnction from aby 2.0. See https://eprint.iacr.org/2020/1225
    size_t lev;

    // Level 0. Use 4 fan-in and 4 outputs cell. 
    // p3, p2, p1, p0 -> p3 & p2 & p1 & p0, p2 & p1 & p0, p1 & p0, p0
    // g works in the same way.
    {
      lev = 0;
      // if (comm->getRank() == 0) std::cout << "PPA: " << k << " bits, level " << lev << std::endl;

      NdArrayRef pops[4];
      NdArrayRef gops[4];

      for (int i = 0; i < 4; i++) {
        pops[i] = NdArrayRef(mss_bshr_type, in.shape());
        gops[i] = NdArrayRef(mss_bshr_type, in.shape());
        NdArrayView<mss_shr_t> _pops(pops[i]);
        NdArrayView<mss_shr_t> _gops(gops[i]);

        pforeach(0, numel, [&](int64_t idx) {
          _pops[idx][0] = select(_p[idx][0], bit_mask[lev], bit_offset[lev], i);
          _pops[idx][1] = select(_p[idx][1], bit_mask[lev], bit_offset[lev], i);
          _pops[idx][2] = select(_p[idx][2], bit_mask[lev], bit_offset[lev], i);
          _gops[idx][0] = select(_g[idx][0], bit_mask[lev], bit_offset[lev], i);
          _gops[idx][1] = select(_g[idx][1], bit_mask[lev], bit_offset[lev], i);
          _gops[idx][2] = select(_g[idx][2], bit_mask[lev], bit_offset[lev], i);

          // assert(_pops[idx][0] % debug_offset == 0 && _pops[idx][1] % debug_offset == 0 && _pops[idx][2] % debug_offset == 0);
          // assert(_gops[idx][0] % debug_offset == 0 && _gops[idx][1] % debug_offset == 0 && _gops[idx][2] % debug_offset == 0);
        });
      }

      std::tie(g, p) = PGCell_4FanIn4Out(ctx, pops[0], pops[1], pops[2], pops[3], gops[0], gops[1], gops[2], gops[3]);

      // if (comm->getRank() == 0) std::cout << "PPA: generate signal p and signal g." << std::endl;
      // if (comm->getRank() == 0) std::cout << "PPA: signal p." << _p[0][0] << " " << _p[1][0] << std::endl;
      // if (comm->getRank() == 0) std::cout << "PPA: signal g." << _g[0][0] << " " << _g[1][0] << std::endl;
    }

    // assert(_p[0][1] == 0 && _p[1][1] == 0 && _p[2][1] == 0 && _p[0][2] == 0 && _p[1][2] == 0 && _p[2][2] == 0);
    // assert(_g[0][1] == 0 && _g[1][1] == 0 && _g[2][1] == 0 && _g[0][2] == 0 && _g[1][2] == 0 && _g[2][2] == 0);

    // Level 1. Use 4 fan-in and 1 output cell. 
    // p3, p2, p1, p0 -> p3 & p2 & p1 & p0
    // g works in the same way.
    {
      lev = 1;
      // if (comm->getRank() == 0) std::cout << "PPA: " << k << " bits, level " << lev << std::endl;

      NdArrayRef pops[4];
      NdArrayRef gops[4];

      for (int i = 0; i < 4; i++) {
        pops[i] = NdArrayRef(mss_bshr_type, in.shape());
        gops[i] = NdArrayRef(mss_bshr_type, in.shape());
        NdArrayView<mss_shr_t> _pops(pops[i]);
        NdArrayView<mss_shr_t> _gops(gops[i]);

        pforeach(0, numel, [&](int64_t idx) {
          _pops[idx][0] = SelectAndRotate(_p[idx][0], bit_mask[lev], bit_offset[lev]*(3-i));
          _gops[idx][0] = SelectAndRotate(_g[idx][0], bit_mask[lev], bit_offset[lev]*(3-i));
          _pops[idx][1] = SelectAndRotate(_p[idx][1], bit_mask[lev], bit_offset[lev]*(3-i));
          _gops[idx][1] = SelectAndRotate(_g[idx][1], bit_mask[lev], bit_offset[lev]*(3-i));
          _pops[idx][2] = SelectAndRotate(_p[idx][2], bit_mask[lev], bit_offset[lev]*(3-i));
          _gops[idx][2] = SelectAndRotate(_g[idx][2], bit_mask[lev], bit_offset[lev]*(3-i));

          // assert(_pops[idx][0] % debug_offset == 0 && _pops[idx][1] % debug_offset == 0 && _pops[idx][2] % debug_offset == 0);
          // assert(_gops[idx][0] % debug_offset == 0 && _gops[idx][1] % debug_offset == 0 && _gops[idx][2] % debug_offset == 0);
        });
      }

      std::tie(gops[0], pops[0]) = PGCell_4FanIn1Out(ctx, pops[0], pops[1], pops[2], pops[3], gops[0], gops[1], gops[2], gops[3]);
      NdArrayView<mss_shr_t> _pops(pops[0]);
      NdArrayView<mss_shr_t> _gops(gops[0]);
      pforeach(0, numel, [&](int64_t idx) {
        _g[idx][0] = (_g[idx][0] & 0x7777777777777777) ^ _gops[idx][0];
        _g[idx][1] = (_g[idx][1] & 0x7777777777777777) ^ _gops[idx][1];
        _g[idx][2] = (_g[idx][2] & 0x7777777777777777) ^ _gops[idx][2];
        _p[idx][0] = (_p[idx][0] & 0x7777777777777777) ^ _pops[idx][0];
        _p[idx][1] = (_p[idx][1] & 0x7777777777777777) ^ _pops[idx][1];
        _p[idx][2] = (_p[idx][2] & 0x7777777777777777) ^ _pops[idx][2];
      });

      // if (comm->getRank() == 0) std::cout << "PPA: generate signal p and signal g." << std::endl;
      // if (comm->getRank() == 0) std::cout << "PPA: signal p." << _p[0][0] << " " << _p[1][0] << std::endl;
      // if (comm->getRank() == 0) std::cout << "PPA: signal g." << _g[0][0] << " " << _g[1][0] << std::endl;
    }

    // assert(_p[0][1] == 0 && _p[1][1] == 0 && _p[2][1] == 0 && _p[0][2] == 0 && _p[1][2] == 0 && _p[2][2] == 0);
    // assert(_g[0][1] == 0 && _g[1][1] == 0 && _g[2][1] == 0 && _g[0][2] == 0 && _g[1][2] == 0 && _g[2][2] == 0);

    // Level 2. Use 4 fan-in and 1 output cell. 
    // p3, p2, p1, p0 -> p3 & p2 & p1 & p0
    // g works in the same way.
    {
      lev = 2;
      // if (comm->getRank() == 0) std::cout << "PPA: " << k << " bits, level " << lev << std::endl;

      NdArrayRef pops[4];
      NdArrayRef gops[4];

      for (int i = 0; i < 4; i++) {
        pops[i] = NdArrayRef(mss_bshr_type, in.shape());
        gops[i] = NdArrayRef(mss_bshr_type, in.shape());
        NdArrayView<mss_shr_t> _pops(pops[i]);
        NdArrayView<mss_shr_t> _gops(gops[i]);

        pforeach(0, numel, [&](int64_t idx) {
          _pops[idx][0] = SelectAndRotate(_p[idx][0], bit_mask[lev], bit_offset[lev]*(3-i));
          _gops[idx][0] = SelectAndRotate(_g[idx][0], bit_mask[lev], bit_offset[lev]*(3-i));
          _pops[idx][1] = SelectAndRotate(_p[idx][1], bit_mask[lev], bit_offset[lev]*(3-i));
          _gops[idx][1] = SelectAndRotate(_g[idx][1], bit_mask[lev], bit_offset[lev]*(3-i));
          _pops[idx][2] = SelectAndRotate(_p[idx][2], bit_mask[lev], bit_offset[lev]*(3-i));
          _gops[idx][2] = SelectAndRotate(_g[idx][2], bit_mask[lev], bit_offset[lev]*(3-i));

          // assert(_pops[idx][0] % debug_offset == 0 && _pops[idx][1] % debug_offset == 0 && _pops[idx][2] % debug_offset == 0);
          // assert(_gops[idx][0] % debug_offset == 0 && _gops[idx][1] % debug_offset == 0 && _gops[idx][2] % debug_offset == 0);
        });
      }

      std::tie(gops[0], pops[0]) = PGCell_4FanIn1Out(ctx, pops[0], pops[1], pops[2], pops[3], gops[0], gops[1], gops[2], gops[3]);
      NdArrayView<mss_shr_t> _pops(pops[0]);
      NdArrayView<mss_shr_t> _gops(gops[0]);
      pforeach(0, numel, [&](int64_t idx) {
        _g[idx][0] = (_g[idx][0] & 0x7777777777777777ull) ^ _gops[idx][0];
        _g[idx][1] = (_g[idx][1] & 0x7777777777777777ull) ^ _gops[idx][1];
        _g[idx][2] = (_g[idx][2] & 0x7777777777777777ull) ^ _gops[idx][2];
        _p[idx][0] = (_p[idx][0] & 0x7777777777777777ull) ^ _pops[idx][0];
        _p[idx][1] = (_p[idx][1] & 0x7777777777777777ull) ^ _pops[idx][1];
        _p[idx][2] = (_p[idx][2] & 0x7777777777777777ull) ^ _pops[idx][2];
      });

      // if (comm->getRank() == 0) std::cout << "PPA: generate signal p and signal g." << std::endl;
      // if (comm->getRank() == 0) std::cout << "PPA: signal p." << _p[0][0] << " " << _p[1][0] << std::endl;
      // if (comm->getRank() == 0) std::cout << "PPA: signal g." << _g[0][0] << " " << _g[1][0] << std::endl;
    }

    // assert(_p[0][1] == 0 && _p[1][1] == 0 && _p[2][1] == 0 && _p[0][2] == 0 && _p[1][2] == 0 && _p[2][2] == 0);
    // assert(_g[0][1] == 0 && _g[1][1] == 0 && _g[2][1] == 0 && _g[0][2] == 0 && _g[1][2] == 0 && _g[2][2] == 0);

    // Level 3. Use 2 fan-in and 1 output cell. 
    // p3, p2, p1, p0 -> p3 & p2 & p1 & p0
    // g works in the same way.
    {
      lev = 3;
      // if (comm->getRank() == 0) std::cout << "PPA: " << k << " bits, level " << lev << std::endl;

      NdArrayRef pops = NdArrayRef(mss_bshr_type, in.shape());
      NdArrayRef gops0 = NdArrayRef(mss_bshr_type, in.shape());
      NdArrayRef gops1 = NdArrayRef(mss_bshr_type, in.shape());
      NdArrayView<mss_shr_t> _pops(pops);
      NdArrayView<mss_shr_t> _gops0(gops0);
      NdArrayView<mss_shr_t> _gops1(gops1);

      pforeach(0, numel, [&](int64_t idx) {
        _gops0[idx][0] =  SelectAndRotate(_g[idx][0], 0x8888888888888888ull, 1) ^ \
                            SelectAndRotate(_g[idx][0], 0x8888888888888888ull, 2) ^ \
                              SelectAndRotate(_g[idx][0], 0x8888888888888888ull, 3); 
        _gops0[idx][1] =  SelectAndRotate(_g[idx][1], 0x8888888888888888ull, 1) ^ \
                            SelectAndRotate(_g[idx][1], 0x8888888888888888ull, 2) ^ \
                              SelectAndRotate(_g[idx][1], 0x8888888888888888ull, 3); 
        _gops0[idx][2] =  SelectAndRotate(_g[idx][2], 0x8888888888888888ull, 1) ^ \
                            SelectAndRotate(_g[idx][2], 0x8888888888888888ull, 2) ^ \
                              SelectAndRotate(_g[idx][2], 0x8888888888888888ull, 3); 
        _gops1[idx][0] =  _g[idx][0];
        _gops1[idx][1] =  _g[idx][1];
        _gops1[idx][2] =  _g[idx][2];
        _pops[idx][0]  =  SelectAndRotate(_p[idx][0], 0x7777777777777777ull, 0);
        _pops[idx][1]  =  SelectAndRotate(_p[idx][1], 0x7777777777777777ull, 0);
        _pops[idx][2]  =  SelectAndRotate(_p[idx][2], 0x7777777777777777ull, 0);
      });

      // if (comm->getRank() == 0) std::cout << "PPA: gops0 " << _gops0[0][0] << " " << _gops0[1][0] << std::endl;
      // if (comm->getRank() == 0) std::cout << "PPA: gops1 " << _gops1[0][0] << " " << _gops1[1][0] << std::endl;
      // if (comm->getRank() == 0) std::cout << "PPA: pops " << _pops[0][0] << " " << _pops[1][0] << std::endl;
      // auto temp0 = MssAnd2NoComm(ctx, gops0, pops);
      // auto temp1 = ResharingMss2Rss(ctx, gops1);
      // NdArrayView<rss_shr_t> _t0(temp0);
      // NdArrayView<rss_shr_t> _t1(temp1);
      // if (comm->getRank() == 0) std::cout << "PPA: temp0 " << _t0[0][0] << " " << _t0[1][0] << std::endl;
      // if (comm->getRank() == 0) std::cout << "PPA: temp1 " << _t1[0][0] << " " << _t1[1][0] << std::endl;
      c = RssXor2(ctx, ResharingMss2Rss(ctx, gops1), MssAnd2NoComm(ctx, gops0, pops));      
    }

    // assert(_p[0][1] == 0 && _p[1][1] == 0 && _p[2][1] == 0 && _p[0][2] == 0 && _p[1][2] == 0 && _p[2][2] == 0);
    // assert(_g[0][1] == 0 && _g[1][1] == 0 && _g[2][1] == 0 && _g[0][2] == 0 && _g[1][2] == 0 && _g[2][2] == 0);

    NdArrayView<rss_shr_t> _c(c);
    // if (comm->getRank() == 0) std::cout << "PPA: generate signal c." << std::endl;
    // if (comm->getRank() == 0) std::cout << "PPA: signal c." << _c[0][0] << " " << _c[1][0] << std::endl;
    pforeach(0, numel, [&](int64_t idx) {

      _out[idx][0] ^= lshift(_c[idx][0], 1);
      _out[idx][1] ^= lshift(_c[idx][1], 1);
    });
    // if (comm->getRank() == 0) std::cout << "PPA: carry " << _g_rss[0][0] << " " << _g_rss[1][0] << std::endl;

    return out;
  });  
}

}  // namespace spu::mpc::alkaid
