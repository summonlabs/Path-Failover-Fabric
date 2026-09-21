// Path Failover Fabric - deterministic canonical byte encoding.
//
// ByteWriter produces one canonical little-endian encoding per value; ByteReader
// is a total, sticky-failure decoder. Every decoder built on these primitives is
// required to reject trailing bytes, impossible lengths and out-of-range enums
// rather than silently defaulting.
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pff/limits.hpp"
#include "pff/status.hpp"

namespace pff {

class ByteWriter {
 public:
  explicit ByteWriter(std::size_t reserve_hint = 0,
                      std::size_t max_size = limits::max_record_payload);

  void u8(std::uint8_t v);
  void u16(std::uint16_t v);
  void u32(std::uint32_t v);
  void u64(std::uint64_t v);
  void i64(std::int64_t v);
  void boolean(bool v);
  void bytes(std::span<const std::byte> v);
  void string(std::string_view v, std::size_t max_bytes = limits::max_name_bytes);

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] std::span<const std::byte> data() const noexcept {
    return std::span<const std::byte>(buffer_.data(), buffer_.size());
  }
  [[nodiscard]] std::vector<std::byte> take() && { return std::move(buffer_); }

 private:
  void fail(Code code, Reason reason);

  std::vector<std::byte> buffer_;
  std::size_t max_size_;
  Status status_;
};

class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> data) noexcept : data_(data) {}

  std::uint8_t u8();
  std::uint16_t u16();
  std::uint32_t u32();
  std::uint64_t u64();
  std::int64_t i64();
  bool boolean();
  Status bytes(std::span<std::byte> out);          // exact-length copy
  Result<std::span<const std::byte>> bytes(std::size_t n);
  Result<std::string_view> string(std::size_t max_bytes = limits::max_name_bytes);

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] bool at_end() const noexcept { return offset_ == data_.size(); }

  // Marks the reader as failed with the supplied status and returns it.
  Status fail(Code code, Reason reason);
  // Requires the reader to be exhausted; otherwise records trailing_garbage.
  Status require_end();

 private:
  Status fail(Code code, Reason reason, const char* detail);

  std::span<const std::byte> data_;
  std::size_t offset_ = 0;
  Status status_;
};

// Canonical hex rendering of a byte sequence, used for digests and diagnostics.
[[nodiscard]] std::string to_hex(std::span<const std::byte> data);

}  // namespace pff
