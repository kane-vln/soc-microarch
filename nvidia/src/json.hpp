#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

class Json {
 public:
  using array_t = std::vector<Json>;
  using object_t = std::map<std::string, Json>;

  Json();
  Json(std::nullptr_t);
  Json(bool value);
  Json(int value);
  Json(std::int64_t value);
  Json(std::uint64_t value);
  Json(double value);
  Json(const char* value);
  Json(std::string value);

  static Json array();
  static Json object();

  Json& operator[](const std::string& key);
  const Json& operator[](const std::string& key) const;
  void push_back(Json value);

  std::string dump(int indent = 2) const;

 private:
  using value_t = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t,
                               double, std::string, array_t, object_t>;

  explicit Json(array_t value);
  explicit Json(object_t value);

  std::string dump_impl(int indent, int depth) const;
  static std::string escape(const std::string& input);

  value_t value_;
};
