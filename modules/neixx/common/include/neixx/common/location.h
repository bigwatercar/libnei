#pragma once

#ifndef NEI_COMMON_LOCATION_H
#define NEI_COMMON_LOCATION_H

#include <cstdint>
#include <string>

#include <nei/macros/nei_export.h>

namespace nei {

class NEI_API Location final {
public:
  constexpr Location() noexcept = default;

  constexpr Location(const char *file_name, std::int32_t line, const char *function_name) noexcept
      : file_name_(file_name)
      , function_name_(function_name)
      , line_(line) {
  }

  static constexpr Location Current(const char *file_name, std::int32_t line, const char *function_name) noexcept {
    return Location(file_name, line, function_name);
  }

  static constexpr Location Unknown() noexcept {
    return Location();
  }

  constexpr const char *file_name() const noexcept {
    return file_name_;
  }

  constexpr const char *function_name() const noexcept {
    return function_name_;
  }

  constexpr std::int32_t line() const noexcept {
    return line_;
  }

  constexpr bool is_null() const noexcept {
    return file_name_ == nullptr;
  }

  // Returns a human-readable representation: "function@file:line".
  // Returns "unknown" if the location was default-constructed.
  std::string ToString() const {
    if (is_null()) {
      return "unknown";
    }
    return std::string(function_name_) + "@" +
           std::string(file_name_) + ":" + std::to_string(line_);
  }

private:
  const char *file_name_ = nullptr;
  const char *function_name_ = nullptr;
  std::int32_t line_ = 0;
  std::int32_t reserved_ = 0;
};

static_assert(sizeof(Location) == sizeof(const char *) * 2 + sizeof(std::int32_t) * 2,
              "Location layout must stay fixed for ABI stability.");

} // namespace nei

#define FROM_HERE ::nei::Location::Current(__FILE__, __LINE__, __FUNCTION__)

#endif // NEI_COMMON_LOCATION_H
