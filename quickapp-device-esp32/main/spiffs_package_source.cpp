#include "spiffs_package_source.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <utility>

#include "esp_log.h"
#include "esp_spiffs.h"

static const char* TAG = "rpk_source";

namespace quickapp::device {

namespace qp = core::package;

// 真机策略:
// 1) 开机一次性把整个 RPK 从 SPIFFS 读进内存(PSRAM), read_at 从内存切片。
//    (SPIFFS 逐次 fread 太慢, 会拖垮 zip 解析)
// 2) **打破同步递归**: PackageLoader 的 validate_artifact 以
//    "读成员 → 回调里读下一个成员" 方式迭代。若 read_at 内联执行回调,
//    N 个成员就会嵌套 N 层调用栈(每层含 sha256/IR 解析的大局部变量),
//    真机内部 RAM 栈扛不住 → 崩溃。
//    这里用 trampoline(蹦床): read_at 只把 (请求,回调) 入队并立即返回;
//    最外层的那次 read_at 负责把队列逐个执行完 —— 递归被拉平成迭代,
//    调用栈深度恒定, 且回调触发时机对 loader 而言仍是"同步内"完成。
struct SpiffsPackageSource::Impl {
  std::shared_ptr<const qp::Bytes> bytes;  // 整包内存副本
  bool closed{false};

  // 蹦床队列: 待执行的完成回调
  struct Pending {
    qp::PackageReadCompletion completion;
    qp::PackageReadResult result;
  };
  std::deque<Pending> queue;
  bool draining{false};  // 是否正在最外层排空队列
};

std::shared_ptr<SpiffsPackageSource> SpiffsPackageSource::create(
    const char* partition_label, const char* file_path) {
  auto source = std::shared_ptr<SpiffsPackageSource>(new SpiffsPackageSource());
  source->impl_ = std::make_unique<Impl>();

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

  size_t total = 0, used = 0;
  esp_spiffs_info(partition_label, &total, &used);
  ESP_LOGI(TAG, "SPIFFS partition '%s': total=%zu, used=%zu",
           partition_label, total, used);

  char full_path[64];
  snprintf(full_path, sizeof(full_path), "/rpk/%s", file_path);
  FILE* file = fopen(full_path, "rb");
  if (file == nullptr) {
    ESP_LOGE(TAG, "Cannot open RPK file: %s", full_path);
    esp_vfs_spiffs_unregister(partition_label);
    return nullptr;
  }

  fseek(file, 0, SEEK_END);
  const long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (file_size <= 0) {
    ESP_LOGE(TAG, "RPK file empty or unreadable: %s", full_path);
    fclose(file);
    esp_vfs_spiffs_unregister(partition_label);
    return nullptr;
  }

  auto buffer = std::make_shared<qp::Bytes>(static_cast<std::size_t>(file_size));
  const size_t got =
      fread(buffer->data(), 1, static_cast<size_t>(file_size), file);
  fclose(file);
  esp_vfs_spiffs_unregister(partition_label);

  if (got != static_cast<size_t>(file_size)) {
    ESP_LOGE(TAG, "RPK read short: wanted %ld, got %zu", file_size, got);
    return nullptr;
  }

  source->impl_->bytes = std::move(buffer);
  ESP_LOGI(TAG, "RPK loaded into memory: %s, size=%ld bytes",
           full_path, file_size);
  return source;
}

SpiffsPackageSource::~SpiffsPackageSource() {
  if (impl_ && !impl_->closed) {
    close();
  }
}

core::RuntimeResult<std::uint64_t> SpiffsPackageSource::size() noexcept {
  if (!impl_ || impl_->closed || !impl_->bytes) {
    return core::RuntimeResult<std::uint64_t>::failure(
        core::RuntimeError::simple(
            core::RuntimeErrorCode::kPackageIoError, "source closed"));
  }
  return core::RuntimeResult<std::uint64_t>::success(impl_->bytes->size());
}

core::EnqueueResult SpiffsPackageSource::read_at(
    qp::PackageReadRequest request,
    qp::PackageReadCompletion completion) noexcept {
  if (!impl_ || impl_->closed || !impl_->bytes) {
    return core::EnqueueResult::failure(core::RuntimeError::simple(
        core::RuntimeErrorCode::kPackageIoError, "source closed"));
  }
  if (!completion) {
    return core::EnqueueResult::failure(core::RuntimeError::simple(
        core::RuntimeErrorCode::kPackageIoError, "null completion"));
  }

  const auto& data = *impl_->bytes;
  if (request.offset > data.size() ||
      request.length > data.size() - request.offset) {
    return core::EnqueueResult::failure(core::RuntimeError::simple(
        core::RuntimeErrorCode::kPackageIoError, "read out of bounds"));
  }

  // 从内存切片, 组装完成结果
  auto value = std::make_shared<qp::Bytes>(
      data.begin() + static_cast<std::ptrdiff_t>(request.offset),
      data.begin() +
          static_cast<std::ptrdiff_t>(request.offset + request.length));
  Impl::Pending pending{
      std::move(completion),
      qp::PackageReadResult{
          std::move(request.request_id),
          core::RuntimeResult<qp::ImmutableBytes>::success(std::move(value))}};
  impl_->queue.push_back(std::move(pending));

  // 蹦床: 若已在排空中(说明当前正处于某个回调内部, 是递归再入),
  // 只入队, 交给最外层循环处理 → 调用栈不再累积。
  if (impl_->draining) {
    return core::EnqueueResult::success(core::Accepted{});
  }

  // 最外层: 逐个执行队列, 直到清空。
  // 回调内部若再次 read_at, 会命中上面的 draining 分支, 只入队不递归。
  impl_->draining = true;
  while (!impl_->queue.empty()) {
    Impl::Pending item = std::move(impl_->queue.front());
    impl_->queue.pop_front();
    item.completion(std::move(item.result));
  }
  impl_->draining = false;

  return core::EnqueueResult::success(core::Accepted{});
}

void SpiffsPackageSource::close() noexcept {
  if (!impl_ || impl_->closed) return;
  impl_->queue.clear();
  impl_->bytes.reset();
  impl_->closed = true;
  ESP_LOGI(TAG, "RPK source closed");
}

}  // namespace quickapp::device
