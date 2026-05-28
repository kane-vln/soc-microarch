#include "json.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

Json::Json() : value_(nullptr) {}
Json::Json(std::nullptr_t) : value_(nullptr) {}
Json::Json(bool value) : value_(value) {}
Json::Json(int value) : value_(static_cast<std::int64_t>(value)) {}
Json::Json(std::int64_t value) : value_(value) {}
Json::Json(std::uint64_t value) : value_(value) {}
Json::Json(double value) : value_(value) {}
Json::Json(const char* value) : value_(std::string(value)) {}
Json::Json(std::string value) : value_(std::move(value)) {}
Json::Json(array_t value) : value_(std::move(value)) {}
Json::Json(object_t value) : value_(std::move(value)) {}

Json Json::array() { return Json(array_t{}); }
Json Json::object() { return Json(object_t{}); }

Json& Json::operator[](const std::string& key) {
  if (!std::holds_alternative<object_t>(value_)) {
    value_ = object_t{};
  }
  return std::get<object_t>(value_)[key];
}

const Json& Json::operator[](const std::string& key) const {
  if (!std::holds_alternative<object_t>(value_)) {
    throw std::logic_error("JSON value is not an object");
  }
  const auto& object = std::get<object_t>(value_);
  const auto it = object.find(key);
  if (it == object.end()) {
    throw std::out_of_range("JSON object key not found: " + key);
  }
  return it->second;
}

void Json::push_back(Json value) {
  if (!std::holds_alternative<array_t>(value_)) {
    value_ = array_t{};
  }
  std::get<array_t>(value_).push_back(std::move(value));
}

std::string Json::dump(int indent) const { return dump_impl(indent, 0); }

std::string Json::dump_impl(int indent, int depth) const {
  const std::string pad(static_cast<std::size_t>(indent * depth), ' ');
  const std::string child_pad(static_cast<std::size_t>(indent * (depth + 1)), ' ');

  if (std::holds_alternative<std::nullptr_t>(value_)) {
    return "null";
  }
  if (std::holds_alternative<bool>(value_)) {
    return std::get<bool>(value_) ? "true" : "false";
  }
  if (std::holds_alternative<std::int64_t>(value_)) {
    return std::to_string(std::get<std::int64_t>(value_));
  }
  if (std::holds_alternative<std::uint64_t>(value_)) {
    return std::to_string(std::get<std::uint64_t>(value_));
  }
  if (std::holds_alternative<double>(value_)) {
    std::ostringstream out;
    out << std::setprecision(10) << std::get<double>(value_);
    return out.str();
  }
  if (std::holds_alternative<std::string>(value_)) {
    return "\"" + escape(std::get<std::string>(value_)) + "\"";
  }
  if (std::holds_alternative<array_t>(value_)) {
    const auto& array = std::get<array_t>(value_);
    if (array.empty()) {
      return "[]";
    }
    std::ostringstream out;
    out << "[\n";
    for (std::size_t i = 0; i < array.size(); ++i) {
      out << child_pad << array[i].dump_impl(indent, depth + 1);
      if (i + 1 != array.size()) {
        out << ",";
      }
      out << "\n";
    }
    out << pad << "]";
    return out.str();
  }

  const auto& object = std::get<object_t>(value_);
  if (object.empty()) {
    return "{}";
  }
  std::ostringstream out;
  out << "{\n";
  std::size_t index = 0;
  for (const auto& [key, value] : object) {
    out << child_pad << "\"" << escape(key) << "\": " << value.dump_impl(indent, depth + 1);
    if (++index != object.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << pad << "}";
  return out.str();
}

std::string Json::escape(const std::string& input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\b':
        out << "\\b";
        break;
      case '\f':
        out << "\\f";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (ch < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
        } else {
          out << ch;
        }
        break;
    }
  }
  return out.str();
}
