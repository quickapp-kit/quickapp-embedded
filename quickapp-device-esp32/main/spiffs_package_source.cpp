#include "spiffs_package_source.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_spiffs.h"

static const char* TAG = "rpk_source";

namespace quickapp::device {

namespace qp = core::package;

struct SpiffsPackageSource::Impl {
  FILE* file{nullptr};
  std::uint64_t file_size{0};
  bool closed{false};
  const char* partition_label{nullptr};
};

std::shared_ptr<SpiffsPackageSource> SpiffsPackageSource::create(
    const char* partition_label, const char* file_path) {
  auto source = std::shared_ptr<SpiffsPackageSource>(new SpiffsPackageSource());
  source->impl_ = std::make_unique<Impl>();
  source->impl_->partition_label = partition_label;

  // 挂载 SPIFFS 分区
  esp_vfs_spiffs_conf_t spiffs_conf = {};
  spiffs_conf.base_path = "/rpk";
  spiffs_conf.partition_label = partition_label;
  spiffs_conf.max_files = 2;
  spiffs_conf.format_if_mount_failed = false;

  esp_err_t err = esp_vfs_spiffs_register(&spiffs_conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to mount SPIFFS partition '%s': %s",
             partition_label, esp_err_to_name(err));
    return nullptr;
  }

  // 检查分区信息
  size_t total = 0, used = 0;
  esp_spiffs_info(partition_label, &total, &used);
  ESP_LOGI(TAG, "SPIFFS partition '%s': total=%zu, used=%zu",
           partition_label, total, used);

  // 打开 RPK 文件
  char full_path[64];
  snprintf(full_path, sizeof(full_path), "/rpk/%s", file_path);

  source->impl_->file = fopen(full_path, "rb");
  if (source->impl_->file == nullptr) {
    ESP_LOGE(TAG, "Cannot open RPK file: %s", full_path);
    esp_vfs_spiffs_unregister(partition_label);
    return nullptr;
  }

  // 获取文件大小
  fseek(source->impl_->file, 0, SEEK_END);
  source->impl_->file_size = static_cast<std::uint64_t>(ftell(source->impl_->file));
  fseek(source->impl_->file, 0, SEEK_SET);

  ESP_LOGI(TAG, "RPK file opened: %s, size=%llu bytes",
           full_path, (unsigned long long)source->impl_->file_size);

  return source;
}

SpiffsPackageSource::~SpiffsPackageSource() {
  if (impl_ && !impl_->closed) {
    close();
  }
}

core::RuntimeResult<std::uint64_t> SpiffsPackageSource::size() noexcept {
  if (!impl_ || impl_->closed) {
    return core::RuntimeResult<std::uint64_t>::failure(
        core::RuntimeError::simple(
            core::RuntimeErrorCode::kPackageIoError, "source closed"));
  }
  return core::RuntimeResult<std::uint64_t>::success(impl_->file_size);
}

core::EnqueueResult SpiffsPackageSource::read_at(
    qp::PackageReadRequest request,
    qp::PackageReadCompletion completion) noexcept {
  if (!impl_ || impl_->closed || impl_->file == nullptr) {
    return core::EnqueueResult::failure(core::RuntimeError::simple(
        core::RuntimeErrorCode::kPackageIoError, "source closed"));
  }

  if (!completion) {
    return core::EnqueueResult::failure(core::RuntimeError::simple(
        core::RuntimeErrorCode::kPackageIoError, "null completion"));
  }

  if (request.offset > impl_->file_size ||
      request.length > impl_->file_size - request.offset) {
    return core::EnqueueResult::failure(core::RuntimeError::simple(
        core::RuntimeErrorCode::kPackageIoError, "read out of bounds"));
  }

  // 同步读取 (嵌入式环境, SPIFFS 是阻塞 IO)
  fseek(impl_->file, static_cast<long>(request.offset), SEEK_SET);

  auto bytes = std::make_shared<qp::Bytes>(static_cast<std::size_t>(request.length));
  size_t read = fread(bytes->data(), 1, static_cast<size_t>(request.length),
                      impl_->file);

  if (read != static_cast<size_t>(request.length)) {
    ESP_LOGE(TAG, "SPIFFS read error: requested %llu, got %zu",
             (unsigned long long)request.length, read);
    completion(qp::PackageReadResult{
        std::move(request.request_id),
        core::RuntimeResult<qp::ImmutableBytes>::failure(
            core::RuntimeError::simple(
                core::RuntimeErrorCode::kPackageIoError, "read failed"))});
    return core::EnqueueResult::success(core::Accepted{});
  }

  completion(qp::PackageReadResult{
      std::move(request.request_id),
      core::RuntimeResult<qp::ImmutableBytes>::success(std::move(bytes))});

  return core::EnqueueResult::success(core::Accepted{});
}

void SpiffsPackageSource::close() noexcept {
  if (!impl_ || impl_->closed) return;

  if (impl_->file != nullptr) {
    fclose(impl_->file);
    impl_->file = nullptr;
  }

  if (impl_->partition_label != nullptr) {
    esp_vfs_spiffs_unregister(impl_->partition_label);
  }

  impl_->closed = true;
  ESP_LOGI(TAG, "RPK source closed");
}

}  // namespace quickapp::device
