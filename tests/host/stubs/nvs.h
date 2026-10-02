#pragma once
#include <Preferences.h>

using esp_err_t = int;
struct FakeNvsHandle { std::string nameSpace; };
using nvs_handle_t = FakeNvsHandle *;

#ifndef ESP_OK
constexpr esp_err_t ESP_OK = 0;
#endif
constexpr esp_err_t ESP_FAIL = 1;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 2;
constexpr esp_err_t ESP_ERR_NVS_TYPE_MISMATCH = 3;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 4;
constexpr int NVS_READWRITE = 0;
constexpr int NVS_READONLY = 1;

inline esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
  if (fake::nvsOpenFail) return ESP_FAIL;
  if (mode == NVS_READONLY) {
    const std::string prefix = std::string(name) + "/";
    bool namespaceExists = false;
    for (const auto& entry : fake::nvs) {
      if (entry.first.compare(0, prefix.size(), prefix) == 0) {
        namespaceExists = true;
        break;
      }
    }
    if (!namespaceExists) return ESP_ERR_NVS_NOT_FOUND;
  }
  *handle = new FakeNvsHandle{name};
  return ESP_OK;
}
inline void nvs_close(nvs_handle_t handle) { delete handle; }
inline esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length) {
  if (fake::nvsShouldFailRead(handle->nameSpace, key)) return ESP_FAIL;
  const auto it = fake::nvs.find(handle->nameSpace + "/" + key);
  if (it == fake::nvs.end()) return ESP_ERR_NVS_NOT_FOUND;
  const bool identityKey = std::strcmp(key, "identity") == 0;
  if (identityKey && fake::nvsIdentityTypeMismatch) return ESP_ERR_NVS_TYPE_MISMATCH;
  if (!identityKey && std::strcmp(key, "energy-v1") != 0 && std::strcmp(key, "energy-v2") != 0 &&
      std::strcmp(key, "sc-journal") != 0)
    return ESP_ERR_NVS_TYPE_MISMATCH;
  if (!value) {
    *length = it->second.size();
    return ESP_OK;
  }
  if (fake::nvsBlobDataReadFail) return ESP_FAIL;
  if (*length < it->second.size()) return ESP_ERR_NVS_INVALID_LENGTH;
  std::memcpy(value, it->second.data(), it->second.size());
  *length = it->second.size();
  return ESP_OK;
}
inline esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *value) {
  if (fake::nvsShouldFailRead(handle->nameSpace, key)) return ESP_FAIL;
  if (std::strcmp(key, "serial") != 0) return ESP_ERR_NVS_TYPE_MISMATCH;
  const auto it = fake::nvs.find(handle->nameSpace + "/" + key);
  if (it == fake::nvs.end()) return ESP_ERR_NVS_NOT_FOUND;
  if (it->second.size() != sizeof(*value)) return ESP_ERR_NVS_TYPE_MISMATCH;
  std::memcpy(value, it->second.data(), sizeof(*value));
  return ESP_OK;
}
inline esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value) {
  if (fake::nvsShouldFailRead(handle->nameSpace, key)) return ESP_FAIL;
  if (std::strcmp(key, "provisioned") != 0)
    return ESP_ERR_NVS_TYPE_MISMATCH;
  const auto it = fake::nvs.find(handle->nameSpace + "/" + key);
  if (it == fake::nvs.end()) return ESP_ERR_NVS_NOT_FOUND;
  if (it->second.size() != sizeof(*value)) return ESP_ERR_NVS_TYPE_MISMATCH;
  *value = it->second[0];
  return ESP_OK;
}
inline esp_err_t nvs_get_u64(nvs_handle_t handle, const char *key, uint64_t *value) {
  if (fake::nvsShouldFailRead(handle->nameSpace, key)) return ESP_FAIL;
  if (std::strcmp(key, "etotal") != 0) return ESP_ERR_NVS_TYPE_MISMATCH;
  const auto it = fake::nvs.find(handle->nameSpace + "/" + key);
  if (it == fake::nvs.end()) return ESP_ERR_NVS_NOT_FOUND;
  if (it->second.size() != sizeof(*value)) return ESP_ERR_NVS_TYPE_MISMATCH;
  std::memcpy(value, it->second.data(), sizeof(*value));
  return ESP_OK;
}
inline esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length) {
  ++fake::nvsWrites;
  if (fake::nvsShouldFailWrite(handle->nameSpace, key)) return ESP_FAIL;
  const auto *bytes = static_cast<const uint8_t *>(value);
  fake::nvs[handle->nameSpace + "/" + key] = {bytes, bytes + length};
  return ESP_OK;
}
inline esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
  ++fake::nvsWrites;
  if(fake::nvsShouldFailWrite(handle->nameSpace,key)) return ESP_FAIL;
  fake::nvs[handle->nameSpace+"/"+key]={value};return ESP_OK;
}
inline esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
  if(fake::nvsFail || fake::nvsFailEraseKey==handle->nameSpace+"/"+key) return ESP_FAIL;
  auto it=fake::nvs.find(handle->nameSpace+"/"+key);
  if(it==fake::nvs.end()) return ESP_ERR_NVS_NOT_FOUND;
  fake::nvs.erase(it);return ESP_OK;
}
inline esp_err_t nvs_commit(nvs_handle_t) { return fake::nvsFail ? ESP_FAIL : ESP_OK; }
