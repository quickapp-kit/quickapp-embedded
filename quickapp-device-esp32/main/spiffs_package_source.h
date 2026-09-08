#pragma once

#include <cstdint>
#include <memory>

#include "quickapp/core/package/package_loader.h"

namespace quickapp::device {

/// SPIFFS-based PackageSource: 从 Flash 分区中读取 RPK 包
/// RPK 文件作为 app.rpk 写入 rpk_store SPIFFS 分区
class SpiffsPackageSource final
    : public core::package::PackageSource,
      public std::enable_shared_from_this<SpiffsPackageSource> {
 public:
  /// 创建 PackageSource
  /// @param partition_label SPIFFS 分区标签 (对应 partitions.csv 中的名字)
  /// @param file_path SPIFFS 中的 RPK 文件路径
  static std::shared_ptr<SpiffsPackageSource> create(
      const char* partition_label, const char* file_path);

  ~SpiffsPackageSource() override;

  core::RuntimeResult<std::uint64_t> size() noexcept override;
  core::EnqueueResult read_at(
      core::package::PackageReadRequest request,
      core::package::PackageReadCompletion completion) noexcept override;
  void close() noexcept override;

 private:
  SpiffsPackageSource() = default;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace quickapp::device
