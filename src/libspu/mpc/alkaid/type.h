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

#pragma once

#include "libspu/core/type.h"

namespace spu::mpc::alkaid {

// The MRSS interface.
//
// The value of this type is a MRSS-sharing.
class MrssShare {
 public:
  virtual ~MrssShare() = default;
};

// Arithmetic sharing in RSS.
class AShrTy : public TypeImpl<AShrTy, RingTy, Secret, AShare> {
  using Base = TypeImpl<AShrTy, RingTy, Secret, AShare>;

 public:
  using Base::Base;
  static std::string_view getStaticId() { return "alkaid.AShr"; }

  explicit AShrTy(FieldType field) { field_ = field; }

  size_t size() const override { return SizeOf(GetStorageType(field_)) * 2; }
};

// Arithmetic sharing in MRSS.
class AShrTyMrss
    : public TypeImpl<AShrTyMrss, RingTy, Secret, AShare, MrssShare> {
  using Base = TypeImpl<AShrTyMrss, RingTy, Secret, AShare, MrssShare>;

 public:
  using Base::Base;
  static std::string_view getStaticId() { return "alkaid.AShrMrss"; }

  explicit AShrTyMrss(FieldType field) { field_ = field; }

  size_t size() const override {
    return SizeOf(GetStorageType(field_)) * 3;
  }  // mx, <rx>
};

class OShrTy : public TypeImpl<OShrTy, RingTy, Secret, OShare> {
  using Base = TypeImpl<OShrTy, RingTy, Secret, OShare>;

 public:
  using Base::Base;
  static std::string_view getStaticId() { return "aby3.OShr"; }

  explicit OShrTy(FieldType field) { field_ = field; }

  // two shares in oram rep share of two different values
  size_t size() const override { return SizeOf(GetStorageType(field_)) * 2; }
};

class OPShrTy : public TypeImpl<OPShrTy, RingTy, Secret, OPShare> {
  using Base = TypeImpl<OPShrTy, RingTy, Secret, OPShare>;

 public:
  using Base::Base;
  static std::string_view getStaticId() { return "aby3.OShr"; }

  explicit OPShrTy(FieldType field) { field_ = field; }

  // two shares in oram rep share of two different values
  size_t size() const override { return SizeOf(GetStorageType(field_)); }
};

// Boolean sharing in RSS.
class BShrTy : public TypeImpl<BShrTy, TypeObject, Secret, BShare> {
  using Base = TypeImpl<BShrTy, TypeObject, Secret, BShare>;
  PtType back_type_ = PT_INVALID;

 public:
  using Base::Base;
  explicit BShrTy(PtType back_type, size_t nbits) {
    SPU_ENFORCE(SizeOf(back_type) * 8 >= nbits,
                "backtype={} has not enough bits={}", back_type, nbits);
    back_type_ = back_type;
    nbits_ = nbits;
  }

  PtType getBacktype() const { return back_type_; }

  static std::string_view getStaticId() { return "alkaid.BShr"; }

  void fromString(std::string_view detail) override {
    auto comma = detail.find_first_of(',');
    auto back_type_str = detail.substr(0, comma);
    auto nbits_str = detail.substr(comma + 1);
    SPU_ENFORCE(PtType_Parse(std::string(back_type_str), &back_type_),
                "parse failed from={}", detail);
    nbits_ = std::stoul(std::string(nbits_str));
  }

  std::string toString() const override {
    return fmt::format("{},{}", PtType_Name(back_type_), nbits_);
  }

  size_t size() const override { return SizeOf(back_type_) * 2; }

  bool equals(TypeObject const* other) const override {
    auto const* derived_other = dynamic_cast<BShrTy const*>(other);
    SPU_ENFORCE(derived_other);
    return getBacktype() == derived_other->getBacktype() &&
           nbits() == derived_other->nbits();
  }
};

// Boolean sharing in MRSS.
class BShrTyMrss
    : public TypeImpl<BShrTyMrss, TypeObject, Secret, BShare, MrssShare> {
  using Base = TypeImpl<BShrTyMrss, TypeObject, Secret, BShare, MrssShare>;
  PtType back_type_ = PT_INVALID;

 public:
  using Base::Base;
  explicit BShrTyMrss(PtType back_type, size_t nbits) {
    SPU_ENFORCE(SizeOf(back_type) * 8 >= nbits,
                "backtype={} has not enough bits={}", back_type, nbits);
    back_type_ = back_type;
    nbits_ = nbits;
  }

  PtType getBacktype() const { return back_type_; }

  static std::string_view getStaticId() { return "alkaid.BShrMrss"; }

  void fromString(std::string_view detail) override {
    auto comma = detail.find_first_of(',');
    auto back_type_str = detail.substr(0, comma);
    auto nbits_str = detail.substr(comma + 1);
    SPU_ENFORCE(PtType_Parse(std::string(back_type_str), &back_type_),
                "parse failed from={}", detail);
    nbits_ = std::stoul(std::string(nbits_str));
  }

  std::string toString() const override {
    return fmt::format("{},{}", PtType_Name(back_type_), nbits_);
  }

  size_t size() const override { return SizeOf(back_type_) * 3; }

  bool equals(TypeObject const* other) const override {
    auto const* derived_other = dynamic_cast<BShrTyMrss const*>(other);
    SPU_ENFORCE(derived_other);
    return getBacktype() == derived_other->getBacktype() &&
           nbits() == derived_other->nbits();
  }
};

// Permutation share
class PShrTy : public TypeImpl<PShrTy, RingTy, Secret, PShare> {
  using Base = TypeImpl<PShrTy, RingTy, Secret, PShare>;

 public:
  using Base::Base;
  static std::string_view getStaticId() { return "alkaid.PShr"; }

  explicit PShrTy() { field_ = FieldType::FM64; }

  size_t size() const override { return SizeOf(GetStorageType(field_)) * 2; }
};

void registerTypes();

}  // namespace spu::mpc::alkaid
