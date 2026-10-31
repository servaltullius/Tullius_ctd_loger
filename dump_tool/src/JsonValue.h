#pragma once

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool {

// obj[key] as T, or `fallback` when obj is no object, the key is missing, or
// it holds another type. nlohmann's value() throws in that last case, and a
// sidecar written by another version or damaged on disk (the incident
// manifest, the plugin scan, MO2 state) must not cost the whole analysis or
// output over one field.
template <class T>
T JsonValueOr(const nlohmann::json& obj, const char* key, T fallback)
{
  if (!obj.is_object()) {
    return fallback;
  }
  const auto it = obj.find(key);
  if (it == obj.end()) {
    return fallback;
  }
  try {
    return it->get<T>();
  } catch (const nlohmann::json::exception&) {
    return fallback;
  }
}

}  // namespace skydiag::dump_tool
