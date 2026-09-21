// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
#include "pff/canonical.hpp"

#include <cstring>

namespace pff {
namespace {

constexpr std::size_t kMaxCanonicalString = 4096;

// One helper per width: a single width parameter next to a value parameter of a
// similar integer type is exactly the kind of interface where a swap compiles.
void append_u16(std::vector<std::byte>& out, std::uint16_t value) {
  out.push_back(static_cast<std::byte>(value & 0xFFu));
  out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
}

void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32u; shift += 8u) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
  }
}

void append_u64(std::vector<std::byte>& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64u; shift += 8u) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
  }
}

std::uint16_t read_u16_at(std::span<const std::byte> data, std::size_t offset) {
  return static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[offset]) |
                                    (static_cast<std::uint16_t>(
                                         static_cast<std::uint8_t>(data[offset + 1]))
                                     << 8));
}

std::uint32_t read_u32_at(std::span<const std::byte> data, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[offset + i])) << (8u * i);
  }
  return value;
}

std::uint64_t read_u64_at(std::span<const std::byte> data, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[offset + i])) << (8u * i);
  }
  return value;
}

}  // namespace

// ---- ByteWriter ------------------------------------------------------------

ByteWriter::ByteWriter(std::size_t reserve_hint, std::size_t max_size)
    : max_size_(max_size) {
  if (reserve_hint > max_size_) {
    reserve_hint = max_size_;
  }
  buffer_.reserve(reserve_hint);
}

void ByteWriter::fail(Code code, Reason reason) {
  if (status_.ok()) {
    status_ = Status(code, reason);
  }
}

void ByteWriter::u8(std::uint8_t v) {
  if (!status_.ok()) {
    return;
  }
  if (buffer_.size() + 1 > max_size_) {
    fail(Code::exhausted, Reason::allocation_denied);
    return;
  }
  buffer_.push_back(static_cast<std::byte>(v));
}

void ByteWriter::u16(std::uint16_t v) {
  if (!status_.ok()) {
    return;
  }
  if (buffer_.size() + 2 > max_size_) {
    fail(Code::exhausted, Reason::allocation_denied);
    return;
  }
  append_u16(buffer_, v);
}

void ByteWriter::u32(std::uint32_t v) {
  if (!status_.ok()) {
    return;
  }
  if (buffer_.size() + 4 > max_size_) {
    fail(Code::exhausted, Reason::allocation_denied);
    return;
  }
  append_u32(buffer_, v);
}

void ByteWriter::u64(std::uint64_t v) {
  if (!status_.ok()) {
    return;
  }
  if (buffer_.size() + 8 > max_size_) {
    fail(Code::exhausted, Reason::allocation_denied);
    return;
  }
  append_u64(buffer_, v);
}

void ByteWriter::i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }

void ByteWriter::boolean(bool v) { u8(v ? 1u : 0u); }

void ByteWriter::bytes(std::span<const std::byte> v) {
  if (!status_.ok()) {
    return;
  }
  if (v.size() > max_size_ - buffer_.size()) {
    fail(Code::exhausted, Reason::allocation_denied);
    return;
  }
  buffer_.insert(buffer_.end(), v.begin(), v.end());
}

void ByteWriter::string(std::string_view v, std::size_t max_bytes) {
  if (!status_.ok()) {
    return;
  }
  if (v.size() > max_bytes || v.size() > 0xFFFFu || v.size() > max_size_ - buffer_.size()) {
    fail(Code::invalid, Reason::length_invalid);
    return;
  }
  u16(static_cast<std::uint16_t>(v.size()));
  if (!status_.ok()) {
    return;
  }
  const auto* raw = reinterpret_cast<const std::byte*>(v.data());
  bytes(std::span<const std::byte>(raw, v.size()));
}

// ---- ByteReader ------------------------------------------------------------

Status ByteReader::fail(Code code, Reason reason, const char* detail) {
  if (status_.ok()) {
    status_ = Status(code, reason, detail == nullptr ? std::string() : std::string(detail));
  }
  return status_;
}

Status ByteReader::fail(Code code, Reason reason) {
  if (status_.ok()) {
    status_ = Status(code, reason);
  }
  return status_;
}

std::uint8_t ByteReader::u8() {
  if (!status_.ok()) {
    return 0;
  }
  if (remaining() < 1) {
    fail(Code::invalid, Reason::length_invalid, "short read: u8");
    return 0;
  }
  const auto value = static_cast<std::uint8_t>(data_[offset_]);
  offset_ += 1;
  return value;
}

std::uint16_t ByteReader::u16() {
  if (!status_.ok()) {
    return 0;
  }
  if (remaining() < 2) {
    fail(Code::invalid, Reason::length_invalid, "short read: u16");
    return 0;
  }
  const std::uint16_t value = read_u16_at(data_, offset_);
  offset_ += 2;
  return value;
}

std::uint32_t ByteReader::u32() {
  if (!status_.ok()) {
    return 0;
  }
  if (remaining() < 4) {
    fail(Code::invalid, Reason::length_invalid, "short read: u32");
    return 0;
  }
  const std::uint32_t value = read_u32_at(data_, offset_);
  offset_ += 4;
  return value;
}

std::uint64_t ByteReader::u64() {
  if (!status_.ok()) {
    return 0;
  }
  if (remaining() < 8) {
    fail(Code::invalid, Reason::length_invalid, "short read: u64");
    return 0;
  }
  const std::uint64_t value = read_u64_at(data_, offset_);
  offset_ += 8;
  return value;
}

std::int64_t ByteReader::i64() { return static_cast<std::int64_t>(u64()); }

bool ByteReader::boolean() {
  const std::uint8_t raw = u8();
  if (!status_.ok()) {
    return false;
  }
  if (raw > 1u) {
    fail(Code::invalid, Reason::enum_invalid, "boolean out of range");
    return false;
  }
  return raw == 1u;
}

Result<std::span<const std::byte>> ByteReader::bytes(std::size_t n) {
  if (!status_.ok()) {
    return status_;
  }
  if (n > remaining()) {
    fail(Code::invalid, Reason::length_invalid, "short read: bytes");
    return status_;
  }
  const auto out = data_.subspan(offset_, n);
  offset_ += n;
  return out;
}

Status ByteReader::bytes(std::span<std::byte> out) {
  auto read = bytes(out.size());
  if (!read.ok()) {
    return read.status();
  }
  std::memcpy(out.data(), read.value().data(), out.size());
  return ok_status();
}

Result<std::string_view> ByteReader::string(std::size_t max_bytes) {
  if (!status_.ok()) {
    return status_;
  }
  const std::uint16_t length = u16();
  if (!status_.ok()) {
    return status_;
  }
  if (length > max_bytes) {
    fail(Code::invalid, Reason::length_invalid, "string too long");
    return status_;
  }
  auto raw = bytes(length);
  if (!raw.ok()) {
    return raw.status();
  }
  return std::string_view(reinterpret_cast<const char*>(raw.value().data()), raw.value().size());
}

Status ByteReader::require_end() {
  if (!status_.ok()) {
    return status_;
  }
  if (!at_end()) {
    return fail(Code::invalid, Reason::trailing_garbage, "trailing bytes after value");
  }
  return ok_status();
}

std::string to_hex(std::span<const std::byte> data) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (const std::byte raw : data) {
    const auto value = static_cast<std::uint8_t>(raw);
    out.push_back(kDigits[value >> 4]);
    out.push_back(kDigits[value & 0x0Fu]);
  }
  return out;
}

}  // namespace pff
