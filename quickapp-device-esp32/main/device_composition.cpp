#include <array>
// QuickApp Device 真机运行时组合入口 (从 case001_lvgl.cpp 移植)
// 差异: 去掉 SDL 窗口/鼠标, 换成 ILI9341 显示 + XPT2046 触摸;
//       去掉命令行/showcase 分支/无头测试断言; RPK 从 SPIFFS 读取;
//       main() → app_main(); 交互主循环常驻 (service + 重绑 click handler)。
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <cstdio>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

#include <lvgl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include <pthread.h>

#include "display_driver.h"
#include "touch_driver.h"
#include "spiffs_package_source.h"

#include "quickapp/core/foundation/app_runtime_factory.h"
#include "quickapp/core/feature/module_registry.h"
#include "quickapp/core/package/package_loader.h"
#include "quickapp/core/event/event_router.h"
#include "quickapp/core/render/initial_render_pipeline.h"
#include "quickapp/core/surface/surface_controller.h"
#include "quickapp/core/timer/timer_registry.h"
#include "quickapp/js/abi/runtime_abi_service.h"
#include "quickapp/js/alpha/alpha_page_initialization_stage.h"
#include "quickapp/js/binding/alpha_initial_binding_stage.h"
#include "quickapp/js/engine/js_engine_service.h"
#if defined(QUICKAPP_EXAMPLES_USE_LIBUV_JS_BACKEND)
#include "quickapp/js/engine/libuv_event_loop_backend.h"
#endif
#include "quickapp/js/engine/observation.h"
#include "quickapp/js/engine/quickjs_engine_provider.h"
#include "quickapp/js/event/handler_registry.h"
#include "quickapp/js/framework/static_facade_catalog.h"
#include "quickapp/js/module/module_loader.h"
#include "quickapp/js/page/page_host_control.h"
#include "quickapp/js/render/alpha_initial_transaction_builder.h"
#include "quickapp/js/vm/vm_lifecycle_service.h"
#include "quickapp/lvgl/font/system_default_font_asset.h"
#include "quickapp/lvgl/feature/lvgl_feature_provider.h"
#include "quickapp/lvgl/foundation/owner_task_queue.h"
#include "quickapp/lvgl/integration/core_mount_bridge.h"
#include "quickapp/lvgl/measure/font_measure.h"
#include "quickapp/lvgl/mount/lvgl_mount_backend.h"
#include "quickapp/lvgl/mount/mount_host.h"
#include "quickapp/lvgl/surface/lvgl_page_root_backend.h"
#include "quickapp/lvgl/surface/surface_host.h"

static const char* TAG = "qa_device";

namespace qc = quickapp::core;
namespace qcf = quickapp::core::feature;
namespace qp = quickapp::core::package;
namespace qr = quickapp::core::render;
namespace qs = quickapp::core::surface;
namespace qj = quickapp::js;
namespace ja = quickapp::js::abi;
namespace qlf = quickapp::lvgl::foundation;
namespace qlfeat = quickapp::lvgl::feature;
namespace qli = quickapp::lvgl::integration;
namespace qlm = quickapp::lvgl::mount;
namespace qls = quickapp::lvgl::surface;
namespace qm = quickapp::lvgl::measure;

namespace {

constexpr qlf::OwnerToken kOwner{1};

std::optional<qc::RuntimeValue> toCoreRuntimeValue(const qj::RuntimeValue& value) {
  const auto& storage = value.storage();
  if (std::holds_alternative<std::nullptr_t>(storage)) return qc::RuntimeValue::null();
  if (const auto* boolean = std::get_if<bool>(&storage))
    return qc::RuntimeValue::boolean(*boolean);
  if (const auto* number = std::get_if<double>(&storage)) {
    auto converted = qc::RuntimeValue::finite_number(*number);
    return converted ? std::optional<qc::RuntimeValue>(converted.value()) : std::nullopt;
  }
  if (const auto* string = std::get_if<std::string>(&storage)) {
    auto converted = qc::RuntimeValue::utf8_string(*string);
    return converted ? std::optional<qc::RuntimeValue>(converted.value()) : std::nullopt;
  }
  if (const auto* array = std::get_if<qj::RuntimeValue::Array>(&storage)) {
    qc::RuntimeValue::Array converted;
    converted.reserve(array->size());
    for (const auto& item : *array) {
      auto child = toCoreRuntimeValue(item);
      if (!child) return std::nullopt;
      converted.push_back(std::move(*child));
    }
    auto result = qc::RuntimeValue::array(std::move(converted));
    return result ? std::optional<qc::RuntimeValue>(result.value()) : std::nullopt;
  }
  const auto& object = std::get<qj::RuntimeValue::Object>(storage);
  qc::RuntimeValue::Object converted;
  for (const auto& [key, item] : object) {
    auto child = toCoreRuntimeValue(item);
    if (!child) return std::nullopt;
    converted.emplace(key, std::move(*child));
  }
  auto result = qc::RuntimeValue::object(std::move(converted));
  return result ? std::optional<qc::RuntimeValue>(result.value()) : std::nullopt;
}

// 真机: 无交互模拟器概念, 但沿用 kInteractiveSimulator=true 的常驻交互循环路径
constexpr bool kInteractiveSimulator = true;

// 真机屏幕: ILI9341 240x320 竖屏, 矩形
float gRuntimeViewportWidth = 240.0F;
float gRuntimeViewportHeight = 320.0F;

enum class DisplayShape { kRect, kRound };
DisplayShape gDisplayShape = DisplayShape::kRect;

// 真机运行时组合 (对齐 quickapp-examples/runtime_composition.h)
inline qp::RuntimeComposition makeDeviceComposition() {
  std::set<std::string, std::less<>> components{
      "View",   "Text",  "Button", "Image", "Input",
      "Switch", "Slider", "Picker", "List",  "Scroll", "Tabs"};
  std::set<std::string, std::less<>> capabilities{
      "system.prompt", "system.router", "system.fetch", "system.file",
      "system.device", "system.shortcut"};
  return {"quickapp-kit-runtime-v1", "quickapp-kit-js-engine-v1",
          std::move(components), std::move(capabilities)};
}

qc::RequestId request(std::string value) { return qc::RequestId::parse(std::move(value)).value(); }
class SurfaceResults final : public qc::CoreIngressPort<qls::SurfaceResult> {
 public:
  explicit SurfaceResults(qli::CoreMountBridge& bridge) : bridge_(bridge) {}
  qc::EnqueueResult post(qls::SurfaceResult&& value) noexcept override { return bridge_.acceptSurfaceResult(std::move(value)); }
  void close() noexcept override {}
 private: qli::CoreMountBridge& bridge_;
};

class SurfaceContent final : public qls::SurfaceContentLifecyclePort {
 public:
  void bind(qlm::MountHost& mounts) noexcept { mounts_ = &mounts; }
  [[nodiscard]] qlf::LocalResult canRelease(
      const qc::SurfaceId&) noexcept override {
    return mounts_ ? qlf::LocalResult::success()
                   : qlf::LocalResult::failure(qlf::LocalError::kInvalidState);
  }
  void releaseNoFail(const qc::SurfaceId& surfaceId) noexcept override {
    if (mounts_ != nullptr)
      static_cast<void>(mounts_->releaseSurface(kOwner, surfaceId));
  }
  void resetNoFail(const qc::SurfaceId& surfaceId) noexcept override {
    releaseNoFail(surfaceId);
  }
 private:
  qlm::MountHost* mounts_{nullptr};
};

class AppState final : public qs::AppRuntimeStateView {
 public:
  [[nodiscard]] qc::lifecycle::AppRuntimeState state() const noexcept override {
    return qc::lifecycle::AppRuntimeState::kForeground;
  }
};

class PageResolver final : public qs::VerifiedPageResolver {
 public:
  PageResolver(qp::PackageLoader& loader,
               std::shared_ptr<const qp::VerifiedPackage> package) noexcept
      : loader_(loader), package_(std::move(package)) {}

  [[nodiscard]] qc::RuntimeResult<qs::VerifiedSurfacePage> resolve(
      std::string_view route, const qc::SurfaceId& surfaceId) noexcept override {
    const auto descriptor = package_->pages().find(std::string(route));
    if (descriptor == package_->pages().end()) {
      return fail(qc::RuntimeErrorCode::kRouteNotFound, "route is absent from RPK");
    }
    std::optional<qp::VerifiedModule> module;
    std::optional<qp::PageIrHandle> pageIr;
    std::optional<qc::RuntimeError> failure;
    if (!loader_.load_module({descriptor->second.module_id, surfaceId},
                             [&](auto result) {
          if (result) module = std::move(result).value();
          else failure = result.error();
        }) || failure || !module) {
      return fail(failure ? failure->code : qc::RuntimeErrorCode::kPackageIoError,
                  failure ? failure->message : "page module load was rejected");
    }
    if (!loader_.load_page_ir(std::string(route), [&](auto result) {
          if (result) pageIr = std::move(result).value();
          else failure = result.error();
        }) || failure || !pageIr) {
      return fail(failure ? failure->code : qc::RuntimeErrorCode::kPackageIoError,
                  failure ? failure->message : "page IR load was rejected");
    }
    return qc::RuntimeResult<qs::VerifiedSurfacePage>::success(
        {std::string(route), std::move(*module), std::move(*pageIr)});
  }

 private:
  static qc::RuntimeResult<qs::VerifiedSurfacePage> fail(
      qc::RuntimeErrorCode code, std::string_view message) noexcept {
    return qc::RuntimeResult<qs::VerifiedSurfacePage>::failure(
        qc::RuntimeError::simple(code, message));
  }

  qp::PackageLoader& loader_;
  std::shared_ptr<const qp::VerifiedPackage> package_;
};

class CoreSurfaceResultIngress final
    : public qc::CoreIngressPort<qls::SurfaceResult> {
 public:
  void bind(qs::SurfaceController& controller) noexcept { controller_ = &controller; }

  qc::EnqueueResult post(qls::SurfaceResult&& result) noexcept override {
    if (controller_ == nullptr) return rejected("SurfaceController is unavailable");
    auto converted = std::visit([](auto&& value) -> qs::SurfaceCommandResult {
      using T = std::decay_t<decltype(value)>;
      constexpr bool visibility = std::is_same_v<T, qls::SetSurfaceVisibilityResult>;
      constexpr bool push = std::is_same_v<T, qls::PresentPushSurfaceHostResult>;
      constexpr bool close = std::is_same_v<T, qls::CloseSurfaceHostResult>;
      qs::SurfaceCommandKind kind = qs::SurfaceCommandKind::kDestroy;
      bool completed = value.status != qls::SurfaceResultStatus::kFailed;
      if constexpr (std::is_same_v<T, qls::CreateSurfaceHostResult>) {
        kind = qs::SurfaceCommandKind::kCreate;
        completed = value.status == qls::SurfaceResultStatus::kCreated;
      } else if constexpr (std::is_same_v<T, qls::PresentRootSurfaceHostResult> || push) {
        kind = qs::SurfaceCommandKind::kPresent;
        completed = value.status == qls::SurfaceResultStatus::kPresented;
      } else if constexpr (visibility) {
        kind = qs::SurfaceCommandKind::kVisibility;
        completed = value.status == qls::SurfaceResultStatus::kCompleted;
      } else if constexpr (close) {
        kind = qs::SurfaceCommandKind::kClose;
        completed = value.status == qls::SurfaceResultStatus::kCompleted;
      } else {
        completed = value.status == qls::SurfaceResultStatus::kDestroyed;
      }
      std::optional<qc::SurfaceId> source;
      std::optional<qc::SurfaceId> reveal;
      std::optional<qc::lifecycle::SurfaceVisibility> coreVisibility;
      if constexpr (push) source = value.source_surface_id;
      if constexpr (close) {
        source = value.surface_id;
        reveal = value.reveal_surface_id;
      }
      if constexpr (visibility) {
        coreVisibility = value.visibility == qls::SurfaceVisibility::kVisible
                             ? qc::lifecycle::SurfaceVisibility::kVisible
                             : qc::lifecycle::SurfaceVisibility::kHidden;
      }
      return {value.request_id, kind, value.surface_id, std::move(source),
              std::move(reveal), coreVisibility, completed, value.error};
    }, std::move(result));
    return controller_->enqueue(std::move(converted));
  }

  void close() noexcept override {}

 private:
  static qc::EnqueueResult rejected(std::string_view message) noexcept {
    return qc::EnqueueResult::failure(qc::RuntimeError::simple(
        qc::RuntimeErrorCode::kPlatformRejected, message));
  }
  qs::SurfaceController* controller_{nullptr};
};

class SurfacePlatform final : public qs::SurfacePlatformPort {
 public:
  explicit SurfacePlatform(qls::SurfaceHostAdapter& surfaces) noexcept
      : surfaces_(surfaces) {}
  void failNextCreate() noexcept { fail_next_create_ = true; }
  void holdNextCreate() noexcept { hold_next_create_ = true; }
  [[nodiscard]] std::optional<qs::SurfaceCreateHostCommand>
  takeHeldCreate() noexcept {
    auto value = std::move(held_create_);
    held_create_.reset();
    return value;
  }

  qc::EnqueueResult post(qs::SurfaceCommand&& command) noexcept override {
    return std::visit([this](auto&& value) -> qc::EnqueueResult {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, qs::SurfaceCreateHostCommand>) {
        if (hold_next_create_) {
          hold_next_create_ = false;
          held_create_.emplace(std::move(value));
          return qc::EnqueueResult::success(qc::Accepted{});
        }
        if (fail_next_create_) {
          fail_next_create_ = false;
          return qc::EnqueueResult::failure(qc::RuntimeError::simple(
              qc::RuntimeErrorCode::kPlatformRejected,
              "injected Surface creation failure"));
        }
        return surfaces_.post(qls::CreateSurfaceHost{
            std::move(value.request_id), std::move(value.surface_id),
            {gRuntimeViewportWidth, gRuntimeViewportHeight}});
      } else if constexpr (std::is_same_v<T, qs::SurfacePresentCommand>) {
        if (value.mode == qs::SurfacePresentMode::kRoot) {
          return surfaces_.post(qls::PresentRootSurfaceHost{
              std::move(value.request_id), std::move(value.target)});
        }
        if (!value.source) return rejected("push source is absent");
        return surfaces_.post(qls::PresentPushSurfaceHost{
            std::move(value.request_id), std::move(value.target),
            std::move(*value.source)});
      } else if constexpr (std::is_same_v<T, qs::SurfaceVisibilityCommand>) {
        return surfaces_.post(qls::SetSurfaceVisibility{
            std::move(value.request_id), std::move(value.surface_id),
            value.visibility == qc::lifecycle::SurfaceVisibility::kVisible
                ? qls::SurfaceVisibility::kVisible
                : qls::SurfaceVisibility::kHidden});
      } else if constexpr (std::is_same_v<T, qs::SurfaceCloseCommand>) {
        return surfaces_.post(qls::CloseSurfaceHost{
            std::move(value.request_id), std::move(value.source),
            std::move(value.reveal)});
      } else {
        return surfaces_.post(qls::DestroySurfaceHost{
            std::move(value.request_id), std::move(value.surface_id)});
      }
    }, std::move(command));
  }

  void close() noexcept override {}

 private:
  static qc::EnqueueResult rejected(std::string_view message) noexcept {
    return qc::EnqueueResult::failure(qc::RuntimeError::simple(
        qc::RuntimeErrorCode::kAbiInvalidArgument, message));
  }
  qls::SurfaceHostAdapter& surfaces_;
  std::optional<qs::SurfaceCreateHostCommand> held_create_;
  bool fail_next_create_{false};
  bool hold_next_create_{false};
};

class PlatformBackPort {
 public:
  virtual ~PlatformBackPort() = default;
  [[nodiscard]] virtual qc::EnqueueResult post(
      qs::NavigationCloseRequest request) noexcept = 0;
};

class PlatformBackIngress final : public PlatformBackPort {
 public:
  void bind(qs::SurfaceController& controller) noexcept {
    controller_ = &controller;
  }

  qc::EnqueueResult post(qs::NavigationCloseRequest request) noexcept override {
    if (controller_ == nullptr) {
      return qc::EnqueueResult::failure(qc::RuntimeError::simple(
          qc::RuntimeErrorCode::kPlatformRejected,
          "Core SurfaceController is unavailable"));
    }
    std::fprintf(stderr,
                 "platform.back.captured request=%s source=%s\n",
                 request.request_id.wire().c_str(), request.source.wire().c_str());
    return controller_->enqueue(qs::SurfaceRequest(std::move(request)));
  }

 private:
  qs::SurfaceController* controller_{nullptr};
};

class ControllerInitialResults final : public qr::InitialContentResultSink {
 public:
  void bind(qs::SurfaceController& controller) noexcept { controller_ = &controller; }
  void complete(qs::InitialContentResult result) noexcept override {
    const bool prepared = result.prepared;
    completed_ = true;
    prepared_ = prepared;
    std::fprintf(stderr,
                 "core.initial.complete request=%s surface=%s completed=%d error=%s:%s\n",
                 result.request_id.wire().c_str(),
                 result.surface_id.wire().c_str(), result.prepared ? 1 : 0,
                 result.error.has_value()
                     ? std::string(qc::to_wire(result.error->code)).c_str()
                     : "none",
                 result.error.has_value() ? result.error->message.data() : "");
    if (controller_ != nullptr && (prepared || forwardFailures_)) {
      const auto forwarded = controller_->enqueue(std::move(result));
      std::fprintf(stderr, "core.initial.forwarded accepted=%d\n", forwarded ? 1 : 0);
    } else {
      std::fprintf(stderr, "core.initial.forwarded accepted=0 suppressed=1\n");
    }
  }
  void close() noexcept override {}
  void suppressFailureForwarding() noexcept { forwardFailures_ = false; }
  [[nodiscard]] bool completed() const noexcept { return completed_; }
  [[nodiscard]] bool prepared() const noexcept { return prepared_; }
 private:
  qs::SurfaceController* controller_{nullptr};
  bool completed_{false};
  bool prepared_{false};
  bool forwardFailures_{true};
};

class ControllerOperationResults final : public qs::SurfaceOperationResultSink {
 public:
  using Callback = std::function<void(qs::SurfaceOperationKind, qc::RequestId,
                                      std::optional<qc::SurfaceId>, bool,
                                      std::optional<qc::RuntimeError>)>;
  explicit ControllerOperationResults(Callback callback) noexcept
      : callback_(std::move(callback)) {}
  void complete(qs::SurfaceOperationKind kind, qc::RequestId requestId,
                std::optional<qc::SurfaceId> target, bool completed,
                std::optional<qc::RuntimeError> error) noexcept override {
    if (callback_) callback_(kind, std::move(requestId), std::move(target),
                             completed, std::move(error));
  }
  void close() noexcept override {}
 private:
  Callback callback_;
};

class ControllerStatus final : public qs::SurfaceStatusSink {
 public:
  void status(qs::SurfaceStatusChanged change) noexcept override {
    std::fprintf(stderr, "core.surface.status surface=%s lifecycle=%u revision=%llu\n",
                 change.surface_id.wire().c_str(),
                 static_cast<unsigned>(change.lifecycle),
                 static_cast<unsigned long long>(change.revision));
  }
  void close() noexcept override {}
};

class ControllerLifecycleResults final : public qs::SurfaceLifecycleResultSink {
 public:
  void complete(qc::lifecycle::SurfaceLifecycleResult result) noexcept override {
    std::fprintf(stderr, "core.surface.lifecycle request=%s completed=%d\n",
                 result.request_id.wire().c_str(), result.completed ? 1 : 0);
  }
  void close() noexcept override {}
};

class ControllerPageLifecycle final : public qs::PageLifecyclePort {
 public:
  using Handler = std::function<qc::EnqueueResult(qs::PageCommand&&)>;
  explicit ControllerPageLifecycle(Handler handler) noexcept
      : handler_(std::move(handler)) {}
  qc::EnqueueResult post(qs::PageCommand&& command) noexcept override {
    if (!handler_) return rejected();
    return handler_(std::move(command));
  }
  void close() noexcept override { handler_ = {}; }
 private:
  static qc::EnqueueResult rejected() noexcept {
    return qc::EnqueueResult::failure(qc::RuntimeError::simple(
        qc::RuntimeErrorCode::kPlatformRejected, "page lifecycle is closed"));
  }
  Handler handler_;
};

class ControllerInitialPipeline final : public qs::InitialSurfacePipeline {
 public:
  using Handler = std::function<qc::EnqueueResult(qs::InitialContentCommand&&)>;
  explicit ControllerInitialPipeline(Handler handler) noexcept
      : handler_(std::move(handler)) {}
  qc::EnqueueResult post(qs::InitialContentCommand&& command) noexcept override {
    if (!handler_) return rejected();
    return handler_(std::move(command));
  }
  void release_surface(const qc::SurfaceId& surfaceId) noexcept override {
    if (release_) release_(surfaceId);
  }
  void close() noexcept override { handler_ = {}; release_ = {}; }
  void onRelease(std::function<void(const qc::SurfaceId&)> release) noexcept {
    release_ = std::move(release);
  }
 private:
  static qc::EnqueueResult rejected() noexcept {
    return qc::EnqueueResult::failure(qc::RuntimeError::simple(
        qc::RuntimeErrorCode::kPlatformRejected, "initial pipeline is closed"));
  }
  Handler handler_;
  std::function<void(const qc::SurfaceId&)> release_;
};

class MountResults final : public qc::CoreIngressPort<qr::MountTransactionResult> {
 public:
  qc::EnqueueResult post(qr::MountTransactionResult&& value) noexcept override {
    std::fprintf(stderr,
                 "platform.mount.complete source=%s surface=%s attempt=%s revision=%llu mounted=%d error=%s message=%s transaction=none(initial)\n",
                 qr::render_source_wire(value.source_id).c_str(), value.surface_id.wire().c_str(),
                 value.mount_attempt_id.wire().c_str(),
                 static_cast<unsigned long long>(value.revision),
                 value.mounted ? 1 : 0,
                 value.error.has_value() ? std::string(qc::to_wire(value.error->code)).c_str() : "",
                 value.error.has_value() ? std::string(value.error->message).c_str() : "");
    return coordinator_ ? coordinator_->accept(std::move(value)) : qc::EnqueueResult::failure(qc::RuntimeError::simple(qc::RuntimeErrorCode::kPlatformRejected, "coordinator unavailable"));
  }
  void close() noexcept override {}
  void bind(qr::MountCoordinator& coordinator) noexcept { coordinator_ = &coordinator; }
 private: qr::MountCoordinator* coordinator_{nullptr};
};

class RenderResults final : public qr::RenderTransactionResultSink {
 public:
  void bind(ja::RuntimeAbiService& runtimeAbi) noexcept {
    runtimeAbi_ = &runtimeAbi;
  }

  void complete(qr::RenderTransactionResult result) noexcept override {
    last = result;
    completed = true;
    if (runtimeAbi_ == nullptr) return;
    std::optional<ja::MessageRuntimeError> error;
    if (result.error.has_value()) {
      error = ja::MessageRuntimeError{
          std::string(qc::to_wire(result.error->code)),
          std::string(result.error->message), result.error->retryable,
          result.surface_id.wire(), std::nullopt,
          result.transaction_id.wire(), std::nullopt};
    }
    static_cast<void>(runtimeAbi_->postCallback(
        ja::JsInboundMessage(ja::RenderTransactionResult{
            result.surface_id.wire(), result.transaction_id.wire(),
            result.presented ? "presented" : "presentationFailed",
            result.submitted_revision, result.committed_revision,
            std::move(error)})));
  }

  void close() noexcept override {}

  ja::RuntimeAbiService* runtimeAbi_{nullptr};
  std::optional<qr::RenderTransactionResult> last;
  bool completed{false};
};

class FontResults final : public qc::CoreIngressPort<qm::PlatformFontGenerationChanged> {
 public:
  qc::EnqueueResult post(qm::PlatformFontGenerationChanged&&) noexcept override { return qc::EnqueueResult::success(qc::Accepted{}); }
  void close() noexcept override {}
};

class InitialResults final : public qr::InitialContentResultSink {
 public:
  void complete(qc::surface::InitialContentResult result) noexcept override { prepared = result.prepared; }
  void close() noexcept override {}
  bool prepared{false};
};

class CoreMeasure final : public qr::MeasurePort {
 public:
  explicit CoreMeasure(qm::FontMeasureAdapter& measure) : measure_(measure) {}
  qr::MeasureResult measure(const qr::MeasureRequest& value) noexcept override {
    const auto role = value.role == qr::MeasureRole::kText ? qm::MeasureRole::kText : qm::MeasureRole::kButtonLabel;
    const auto constraint = [](qr::MeasureConstraint value) {
      return qm::MeasureConstraint{value.kind == qr::MeasureConstraintKind::kExactly ? qm::ConstraintKind::kExactly : value.kind == qr::MeasureConstraintKind::kAtMost ? qm::ConstraintKind::kAtMost : qm::ConstraintKind::kUnconstrained, value.value};
    };
    const auto result = measure_.measure({value.request_id.wire(), value.surface_id.wire(), value.node_id.wire(), value.content_revision, value.platform_font_generation, role, value.text, value.font_token, value.font_size, value.font_weight, constraint(value.width_constraint), constraint(value.height_constraint)});
    return {value.request_id, value.surface_id, value.node_id, value.content_revision, value.platform_font_generation, result.measured, result.width, result.height, result.error};
  }
 private: qm::FontMeasureAdapter& measure_;
};

class Clock final : public qj::MonotonicClock {
 public:
  std::uint64_t nowNs() const noexcept override {
    return tick_.fetch_add(100, std::memory_order_relaxed);
  }
 private:
  mutable std::atomic<std::uint64_t> tick_{1000};
};

class TraceSink final : public qj::TraceSink {
 public:
  void emit(const qj::TraceEvent&) noexcept override {}
};

class ModuleCompletion final : public qj::module::ModuleCompletionPort {
 public:
  qj::module::ModuleEnqueueResult post(
      const qj::module::ModuleLoadCompletion& completion) noexcept override {
    status = completion.status;
    error = completion.error ? completion.error->message : "";
    return {qj::module::ModuleEnqueueStatus::Accepted};
  }
  std::string status;
  std::string error;
};

class JsRequestIds final : public qj::framework::JsRequestIdAllocatorPort {
 public:
  std::string nextRequestId() noexcept override {
    return "req:j-" + std::to_string(next_++);
  }
 private:
  std::uint64_t next_{100};
};

class LvglClickToCore final {
 public:
  explicit LvglClickToCore(qc::event::EventRouter& router) noexcept
      : router_(router) {}
  static void callback(void* context, const qc::SurfaceId& surface,
                       const qc::NodeId& node, std::uint64_t timestamp) noexcept {
    auto* self = static_cast<LvglClickToCore*>(context);
    if (self == nullptr) return;
    try {
      std::fprintf(stderr,
                   "lvgl.event.clicked surface=%s node=%s timestamp=%llu\n",
                   surface.wire().c_str(), node.wire().c_str(),
                   static_cast<unsigned long long>(timestamp));
      const auto request = qc::RequestId::parse(
          "req:p-" + std::to_string(++self->sequence_));
      if (!request) return;
      const auto result = self->router_.dispatch(qc::event::PlatformInputMessage{
          request.value(), surface, node, qp::EventType::kClick, timestamp, {}});
      std::fprintf(stderr,
                   "lvgl.input.dispatch request=%s surface=%s node=%s accepted=%d error=%s\n",
                   request.value().wire().c_str(), surface.wire().c_str(),
                   node.wire().c_str(), result ? 1 : 0,
                   result ? "" : std::string(qc::to_wire(result.error().code)).c_str());
    } catch (const std::exception& error) {
      std::fprintf(stderr, "lvgl.input.exception=%s\n", error.what());
    } catch (...) {
      std::fprintf(stderr, "lvgl.input.exception=unknown\n");
    }
  }
 private:
  qc::event::EventRouter& router_;
  std::uint64_t sequence_{0};
};

class LvglInputToCore final {
 public:
  explicit LvglInputToCore(qc::event::EventRouter& router) noexcept : router_(router) {}
  static void callback(void* context, const qc::SurfaceId& surface,
                       const qc::NodeId& node, qp::EventType type,
                       const char* value, std::uint64_t timestamp) noexcept {
    auto* self = static_cast<LvglInputToCore*>(context);
    if (self == nullptr) return;
    const auto requestId = qc::RequestId::parse("req:p-" + std::to_string(++self->sequence_));
    if (!requestId) return;
    qc::RuntimeValue::Object payload;
    if (type == qp::EventType::kInput || type == qp::EventType::kChange) {
      auto encoded = qc::RuntimeValue::utf8_string(value == nullptr ? "" : value);
      if (!encoded) return;
      payload.emplace("value", std::move(encoded).value());
    } else {
      payload.emplace("focused", qc::RuntimeValue::boolean(true));
    }
    const auto result = self->router_.dispatch(qc::event::PlatformInputMessage{
        requestId.value(), surface, node, type, timestamp, std::move(payload)});
    if (result) ++self->acceptedEvents;
    std::fprintf(stderr, "lvgl.input.dispatch type=%s request=%s node=%s accepted=%d error=%s\n",
                 std::string(qc::event::event_type_wire(type)).c_str(), requestId.value().wire().c_str(),
                 node.wire().c_str(), result ? 1 : 0,
                 result ? "" : std::string(qc::to_wire(result.error().code)).c_str());
  }
 private:
  qc::event::EventRouter& router_;
  std::uint64_t sequence_{100};
 public:
  std::size_t acceptedEvents{0};
};

class LvglSwitchToCore final {
 public:
  explicit LvglSwitchToCore(qc::event::EventRouter& router) noexcept
      : router_(router) {}

  static void callback(void* context, const qc::SurfaceId& surface,
                       const qc::NodeId& node, bool checked,
                       std::uint64_t timestamp) noexcept {
    auto* self = static_cast<LvglSwitchToCore*>(context);
    if (self == nullptr) return;
    const auto requestId = qc::RequestId::parse(
        "req:p-" + std::to_string(++self->sequence_));
    if (!requestId) return;
    qc::RuntimeValue::Object payload;
    payload.emplace("checked", qc::RuntimeValue::boolean(checked));
    const auto result = self->router_.dispatch(qc::event::PlatformInputMessage{
        requestId.value(), surface, node, qp::EventType::kChange,
        timestamp, std::move(payload)});
    self->lastChecked = checked;
    self->dispatched = true;
    self->accepted = static_cast<bool>(result);
    std::fprintf(stderr,
                 "lvgl.switch.dispatch event=change checked=%d request=%s node=%s accepted=%d error=%s\n",
                 checked ? 1 : 0, requestId.value().wire().c_str(),
                 node.wire().c_str(), result ? 1 : 0,
                 result ? "" : std::string(qc::to_wire(result.error().code)).c_str());
  }

  qc::event::EventRouter& router_;
  std::uint64_t sequence_{200};
  bool dispatched{false};
  bool accepted{false};
  bool lastChecked{true};
};

qj::RuntimeValue toJsRuntimeValue(const qc::RuntimeValue& value) {
  return std::visit(
      [](const auto& stored) -> qj::RuntimeValue {
        using Stored = std::decay_t<decltype(stored)>;
        if constexpr (std::is_same_v<Stored, qc::RuntimeValue::Null>) {
          return qj::RuntimeValue(nullptr);
        } else if constexpr (std::is_same_v<Stored, bool>) {
          return qj::RuntimeValue(stored);
        } else if constexpr (std::is_same_v<Stored, std::int64_t> ||
                             std::is_same_v<Stored, double>) {
          return qj::RuntimeValue(static_cast<double>(stored));
        } else if constexpr (std::is_same_v<Stored, std::string>) {
          return qj::RuntimeValue(stored);
        } else if constexpr (std::is_same_v<Stored, std::shared_ptr<const qc::RuntimeValue::Array>>) {
          qj::RuntimeValue::Array array;
          if (stored != nullptr) {
            array.reserve(stored->size());
            for (const auto& child : *stored) array.push_back(toJsRuntimeValue(child));
          }
          return qj::RuntimeValue(std::move(array));
        } else {
          qj::RuntimeValue::Object object;
          if (stored != nullptr) {
            for (const auto& [key, child] : *stored) object.emplace(key, toJsRuntimeValue(child));
          }
          return qj::RuntimeValue(std::move(object));
        }
      },
      value.storage());
}

// This is the composition boundary: JS submits a typed initial template, and
// Core materializes it in the one authoritative RuntimeTreeStore.
class JsCoreIngress final : public ja::CoreIngressPort,
                            public qc::event::JsEventDispatchPort {
 public:
  explicit JsCoreIngress() = default;

  void bindCoordinator(qr::MountCoordinator& coordinator) noexcept {
    coordinator_ = &coordinator;
  }
  void bindPage(qc::SurfaceId surfaceId, qp::PageIrHandle page) noexcept {
    std::lock_guard lock(pagesMutex_);
    pages_[surfaceId.wire()] = std::move(page);
  }
  void bindSurfaceController(qs::SurfaceController& controller) noexcept {
    surfaceController_ = &controller;
  }
  void bindAbi(ja::RuntimeAbiService& runtimeAbi) noexcept {
    runtimeAbi_ = &runtimeAbi;
  }
  void bindFeatureRegistry(qcf::ModuleRegistry& registry) noexcept {
    featureRegistry_ = &registry;
  }
  void bindTimerRegistry(qc::timer::TimerRegistry& registry) noexcept {
    timerRegistry_ = &registry;
  }
  void bindJsServices(qj::module::ModuleLoader& modules,
                      qj::vm::VmLifecycleService& vm,
                      qj::event::HandlerRegistry& handlers) noexcept {
    modules_ = &modules;
    vm_ = &vm;
    handlerRegistry_ = &handlers;
  }

  [[nodiscard]] std::vector<std::string> blockHandlerIdsForSurface(
      const qc::SurfaceId& surfaceId) const {
    std::vector<std::string> result;
    for (const auto& [blockId, handlers] : blockHandlers_) {
      if (!blockId.starts_with("blk:" + surfaceId.wire() + "-")) continue;
      result.insert(result.end(), handlers.begin(), handlers.end());
    }
    return result;
  }

  qc::EnqueueResult post(qc::event::JsEventDispatch&& message) noexcept override {
    if (runtimeAbi_ == nullptr) {
      return qc::EnqueueResult::failure(qc::RuntimeError::simple(
          qc::RuntimeErrorCode::kPlatformRejected, "JS Runtime ABI is unavailable"));
    }
    std::fprintf(stderr,
                 "core.event.dispatch request=%s surface=%s node_owner=%s template_node=%llu handler=%s\n",
                 message.request_id.wire().c_str(),
                 message.surface_id.wire().c_str(),
                 qc::runtime_tree::owner_wire(message.target.owner).c_str(),
                 static_cast<unsigned long long>(
                     message.target.template_node_id.value()),
                 message.handler_id.wire().c_str());
    ja::JsEventDispatch typed{
        message.request_id.wire(), message.surface_id.wire(),
        message.handler_id.wire(), std::string(qc::event::event_type_wire(message.event_type)),
        message.phase,
        {qc::runtime_tree::owner_wire(message.target.owner),
         message.target.template_node_id.value()},
        {qc::runtime_tree::owner_wire(message.current_target.owner),
         message.current_target.template_node_id.value()},
        static_cast<double>(message.timestamp_ns), {}};
    for (const auto& [key, payload] : message.payload) {
      typed.payload.emplace(key, toJsRuntimeValue(payload));
    }
    const auto posted = runtimeAbi_->postCallback(
        ja::JsInboundMessage(std::move(typed)));
    return posted.ok
               ? qc::EnqueueResult::success(qc::Accepted{})
               : qc::EnqueueResult::failure(qc::RuntimeError::simple(
                     qc::RuntimeErrorCode::kQueueOverflow,
                     "JS event callback queue rejected"));
  }

  void close() noexcept override {}

  ja::EnqueueResult post(ja::CoreInboundMessage message) noexcept override {
    try {
      std::fprintf(stderr, "js.core.message kind=%zu\n", message.index());
      if (std::holds_alternative<ja::CompleteVmInitialization>(message)) {
        const auto& complete = std::get<ja::CompleteVmInitialization>(message);
        std::fprintf(stderr, "js.core.complete scope=%s status=%s phase=%s error=%s\n",
                     complete.scope.c_str(), complete.status.c_str(),
                     complete.failedPhase ? complete.failedPhase->c_str() : "",
                     complete.error ? complete.error->message.c_str() : "");
      }
      if (const auto* render = std::get_if<ja::SubmitRenderTransaction>(&message)) {
        const auto surface = qc::SurfaceId::parse(render->surfaceId);
        const auto transaction = qc::TransactionId::parse(render->transactionId);
        if (!surface || !transaction || render->revision == 0 ||
            coordinator_ == nullptr) {
          return rejectTransaction("invalid RenderTransaction identity",
                                  render->surfaceId, render->transactionId);
        }
        std::vector<qc::runtime_tree::BindingUpdate> updates;
        updates.reserve(render->operations.size());
        std::vector<qc::runtime_tree::InstantiateBlockRequest> blockInstantiates;
        std::vector<qc::BlockInstanceId> blockRemoves;
        std::vector<qc::runtime_tree::MoveBlockRequest> blockMoves;
        std::map<std::string, std::vector<std::string>, std::less<>> addedBlockHandlers;
        const auto parseOwner = [](const std::string& wire)
            -> std::optional<qc::OwnerInstanceId> {
          if (const auto component = qc::ComponentInstanceId::parse(wire)) {
            return qc::OwnerInstanceId(component.value());
          }
          if (const auto block = qc::BlockInstanceId::parse(wire)) {
            return qc::OwnerInstanceId(block.value());
          }
          return std::nullopt;
        };
        for (const auto& operation : render->operations) {
          if (const auto* update = std::get_if<ja::UpdateBindingOperation>(&operation)) {
            const auto owner = qc::ComponentInstanceId::parse(update->ownerInstanceId);
            if (!owner || update->templateBindingId == 0) {
              return rejectTransaction("invalid binding target",
                                      render->surfaceId, render->transactionId);
            }
            const auto value = std::visit(
                [](const auto& item) -> qc::runtime_tree::BindingValue {
                  return item;
                }, update->value);
            updates.push_back({owner.value(), update->templateBindingId, value});
            std::fprintf(stderr, "case002.render.op kind=updateBinding id=%llu\n",
                         static_cast<unsigned long long>(update->templateBindingId));
            continue;
          }
          if (const auto* instantiate = std::get_if<ja::InstantiateBlockOperation>(&operation)) {
            const auto blockId = qc::BlockInstanceId::parse(instantiate->blockInstanceId);
            const auto templateId = qc::TemplateBlockId::from(instantiate->templateBlockId);
            const auto parentTemplateId = qc::TemplateNodeId::from(instantiate->parent.templateNodeId);
            const auto parentOwner = parseOwner(instantiate->parent.ownerInstanceId);
            if (!blockId || !templateId || !parentTemplateId || !parentOwner) {
              return rejectTransaction("invalid block instantiation target",
                                      render->surfaceId, render->transactionId);
            }
            std::map<std::uint64_t, qc::runtime_tree::BindingValue> blockBindings;
            for (const auto& [id, value] : instantiate->initialBindings) {
              blockBindings.emplace(id, std::visit(
                  [](const auto& item) -> qc::runtime_tree::BindingValue { return item; }, value));
            }
            std::vector<qc::runtime_tree::HandlerRegistration> blockHandlers;
            for (const auto& binding : instantiate->handlers) {
              const auto owner = qc::BlockInstanceId::parse(binding.ownerInstanceId);
              const auto handlerTemplateId = qc::TemplateHandlerId::from(binding.templateHandlerId);
              const auto handlerId = qc::HandlerId::parse(binding.handlerId);
              if (!owner || !handlerTemplateId || !handlerId) {
                return rejectTransaction("invalid block handler identity",
                                        render->surfaceId, render->transactionId);
              }
              blockHandlers.push_back({owner.value(), handlerTemplateId.value(), handlerId.value()});
            }
            qc::runtime_tree::BlockKey key = std::string("");
            if (instantiate->key.has_value()) {
              key = std::visit([](const auto& value) -> qc::runtime_tree::BlockKey {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, std::string>) {
                  return value;
                } else {
                  return static_cast<std::int64_t>(value);
                }
              }, *instantiate->key);
            }
            blockInstantiates.push_back({templateId.value(), blockId.value(),
                                         {std::move(parentOwner.value()), parentTemplateId.value()},
                                         static_cast<std::size_t>(instantiate->index), key,
                                         std::move(blockBindings), std::move(blockHandlers)});
            std::fprintf(stderr, "case002.render.op kind=instantiateBlock id=%s\n",
                         instantiate->blockInstanceId.c_str());
            for (const auto& binding : instantiate->handlers)
              addedBlockHandlers[instantiate->blockInstanceId].push_back(binding.handlerId);
            continue;
          }
          if (const auto* remove = std::get_if<ja::RemoveBlockOperation>(&operation)) {
            const auto blockId = qc::BlockInstanceId::parse(remove->blockInstanceId);
            if (!blockId) return rejectTransaction("invalid block removal target",
                                                   render->surfaceId, render->transactionId);
            blockRemoves.push_back(blockId.value());
            std::fprintf(stderr, "case002.render.op kind=removeBlock id=%s\n",
                         remove->blockInstanceId.c_str());
            continue;
          }
          const auto* move = std::get_if<ja::MoveBlockOperation>(&operation);
          if (move == nullptr) return rejectTransaction("unknown render operation",
                                                        render->surfaceId, render->transactionId);
          const auto blockId = qc::BlockInstanceId::parse(move->blockInstanceId);
          const auto parentTemplateId = qc::TemplateNodeId::from(move->parent.templateNodeId);
          const auto parentOwner = parseOwner(move->parent.ownerInstanceId);
          if (!blockId || !parentTemplateId || !parentOwner) {
            return rejectTransaction("invalid block move target",
                                    render->surfaceId, render->transactionId);
          }
          blockMoves.push_back({blockId.value(),
                                {std::move(parentOwner.value()), parentTemplateId.value()},
                                static_cast<std::size_t>(move->index)});
          std::fprintf(stderr, "case002.render.op kind=moveBlock id=%s index=%llu\n",
                       move->blockInstanceId.c_str(),
                       static_cast<unsigned long long>(move->index));
        }
        std::optional<qc::RequestId> requestId;
        if (render->requestId.has_value()) {
          const auto parsed = qc::RequestId::parse(*render->requestId);
          if (!parsed) {
            return rejectTransaction("invalid causal request identity",
                                    render->surfaceId, render->transactionId);
          }
          requestId = parsed.value();
        }
        const auto accepted = coordinator_->submit(qr::RenderTransactionIntent{
            surface.value(), transaction.value(), render->revision, requestId,
            std::move(updates), std::move(blockInstantiates),
            std::move(blockRemoves), std::move(blockMoves)});
        if (!accepted) {
          return rejectTransaction("Core rejected RenderTransaction",
                                  render->surfaceId, render->transactionId);
        }
        bindBlockHandlers(render->surfaceId, addedBlockHandlers);
        for (const auto& blockId : blockRemoves) {
          const auto found = blockHandlers_.find(blockId.wire());
          if (found == blockHandlers_.end()) continue;
          for (const auto& handlerId : found->second)
            if (handlerRegistry_ != nullptr)
              handlerRegistry_->unbind(render->surfaceId, handlerId);
          blockHandlers_.erase(found);
        }
        return ja::EnqueueResult::accepted();
      }
      if (const auto* toast = std::get_if<ja::ShowToast>(&message)) {
        const auto requestId = qc::RequestId::parse(toast->requestId);
        const auto surfaceId = qc::SurfaceId::parse(toast->surfaceId);
        if (!requestId || !surfaceId || toast->message.empty() ||
            featureRegistry_ == nullptr) {
          return reject("invalid showToast request", toast->surfaceId,
                        toast->requestId);
        }
        const auto feature = featureRegistry_->invoke(qcf::Request{
            .request_id = requestId.value(),
            .surface_id = surfaceId.value(),
            .module = qcf::ModuleId::kSystemPrompt,
            .method = qcf::Method::kShowToast,
            .text = toast->message,
            .duration_ms = toast->durationMs});
        const auto status = std::string(qcf::status_wire(feature.status));
        const auto error = feature.error
                               ? std::optional<ja::MessageRuntimeError>(
                                     ja::MessageRuntimeError{
                                         feature.error->code,
                                         feature.error->message,
                                         feature.error->retryable,
                                         toast->surfaceId,
                                         toast->requestId,
                                         std::nullopt,
                                         std::nullopt})
                               : std::nullopt;
        ja::ShowToastResult result{toast->requestId, toast->surfaceId,
                                   status, error};
        return runtimeAbi_->postCallback(ja::JsInboundMessage(std::move(result))).ok
                   ? ja::EnqueueResult::accepted()
                   : reject("showToast result queue rejected", toast->surfaceId,
                            toast->requestId);
      }
      if (const auto* device = std::get_if<ja::DeviceGetInfo>(&message)) {
        const auto requestId = qc::RequestId::parse(device->requestId);
        const auto surfaceId = qc::SurfaceId::parse(device->surfaceId);
        if (!requestId || !surfaceId || featureRegistry_ == nullptr) {
          return reject("invalid device info request", device->surfaceId,
                        device->requestId);
        }
        const auto feature = featureRegistry_->invoke(qcf::Request{
            .request_id = requestId.value(),
            .surface_id = surfaceId.value(),
            .module = qcf::ModuleId::kSystemDevice,
            .method = qcf::Method::kGetInfo});
        std::optional<ja::DeviceInfo> info;
        if (feature.device_info) {
          const auto& value = *feature.device_info;
          info = ja::DeviceInfo{
              value.os_type, value.platform_version_name,
              value.platform_version_code, value.screen_density,
              value.screen_width, value.screen_height, value.window_width,
              value.window_height, value.device_type, std::nullopt,
              std::nullopt, std::nullopt, std::nullopt, std::nullopt,
              std::nullopt};
        }
        const auto error = feature.error
                               ? std::optional<ja::MessageRuntimeError>(
                                     ja::MessageRuntimeError{
                                         feature.error->code,
                                         feature.error->message,
                                         feature.error->retryable,
                                         device->surfaceId,
                                         device->requestId,
                                         std::nullopt,
                                         std::nullopt})
                               : std::nullopt;
        ja::DeviceGetInfoResult result{device->requestId, device->surfaceId,
                                       std::string(qcf::status_wire(feature.status)),
                                       std::move(info), error};
        return runtimeAbi_->postCallback(ja::JsInboundMessage(std::move(result))).ok
                   ? ja::EnqueueResult::accepted()
                   : reject("device info result queue rejected", device->surfaceId,
                            device->requestId);
      }
      if (const auto* timerStart = std::get_if<ja::TimerStart>(&message)) {
        const auto requestId = qc::RequestId::parse(timerStart->requestId);
        const auto surfaceId = qc::SurfaceId::parse(timerStart->surfaceId);
        if (!requestId || !surfaceId || timerRegistry_ == nullptr ||
            timerStart->delayMs > std::numeric_limits<std::uint64_t>::max() / 1'000'000ULL ||
            timerStart->periodMs > std::numeric_limits<std::uint64_t>::max() / 1'000'000ULL) {
          return reject("invalid timer start request", timerStart->surfaceId,
                        timerStart->requestId);
        }
        const auto result = timerRegistry_->start({
            requestId.value(), surfaceId.value(),
            timerStart->delayMs * 1'000'000ULL,
            timerStart->periodMs * 1'000'000ULL});
        ja::TimerStartResult callback{
            timerStart->requestId, timerStart->surfaceId,
            std::string(qc::timer::status_wire(result.status)),
            result.timer_id ? std::optional<std::string>(result.timer_id->wire())
                            : std::nullopt,
            result.error ? std::optional<ja::MessageRuntimeError>(ja::MessageRuntimeError{
                              std::string(qc::to_wire(result.error->code)),
                              std::string(result.error->message), result.error->retryable,
                              timerStart->surfaceId, timerStart->requestId,
                              std::nullopt, std::nullopt})
                         : std::nullopt};
        return runtimeAbi_->postCallback(ja::JsInboundMessage(std::move(callback))).ok
                   ? ja::EnqueueResult::accepted()
                   : reject("timer start result queue rejected", timerStart->surfaceId,
                            timerStart->requestId);
      }
      if (const auto* timerCancel = std::get_if<ja::TimerCancel>(&message)) {
        const auto requestId = qc::RequestId::parse(timerCancel->requestId);
        const auto surfaceId = qc::SurfaceId::parse(timerCancel->surfaceId);
        const auto timerId = qc::TimerId::parse(timerCancel->timerId);
        if (!requestId || !surfaceId || !timerId || timerRegistry_ == nullptr) {
          return reject("invalid timer cancel request", timerCancel->surfaceId,
                        timerCancel->requestId);
        }
        const auto result = timerRegistry_->cancel(
            {requestId.value(), surfaceId.value(), timerId.value()});
        ja::TimerCancelResult callback{
            timerCancel->requestId, timerCancel->surfaceId,
            std::string(qc::timer::status_wire(result.status)),
            timerCancel->timerId,
            result.error ? std::optional<ja::MessageRuntimeError>(ja::MessageRuntimeError{
                              std::string(qc::to_wire(result.error->code)),
                              std::string(result.error->message), result.error->retryable,
                              timerCancel->surfaceId, timerCancel->requestId,
                              std::nullopt, std::nullopt})
                         : std::nullopt};
        return runtimeAbi_->postCallback(ja::JsInboundMessage(std::move(callback))).ok
                   ? ja::EnqueueResult::accepted()
                   : reject("timer cancel result queue rejected", timerCancel->surfaceId,
                            timerCancel->requestId);
      }
      if (const auto* title = std::get_if<ja::SetTitleBar>(&message)) {
        const auto requestId = qc::RequestId::parse(title->requestId);
        const auto surfaceId = qc::SurfaceId::parse(title->surfaceId);
        if (!requestId || !surfaceId || title->text.empty() ||
            featureRegistry_ == nullptr) {
          return reject("invalid title bar request", title->surfaceId,
                        title->requestId);
        }
        const auto feature = featureRegistry_->invoke(qcf::Request{
            .request_id = requestId.value(),
            .surface_id = surfaceId.value(),
            .module = qcf::ModuleId::kPageHost,
            .method = qcf::Method::kSetTitleBar,
            .text = title->text});
        const auto error = feature.error
                               ? std::optional<ja::MessageRuntimeError>(
                                     ja::MessageRuntimeError{
                                         feature.error->code,
                                         feature.error->message,
                                         feature.error->retryable,
                                         title->surfaceId,
                                         title->requestId,
                                         std::nullopt,
                                         std::nullopt})
                               : std::nullopt;
        ja::SetTitleBarResult result{title->requestId, title->surfaceId,
                                     std::string(qcf::status_wire(feature.status)),
                                     error};
        return runtimeAbi_->postCallback(ja::JsInboundMessage(std::move(result))).ok
                   ? ja::EnqueueResult::accepted()
                   : reject("title result queue rejected", title->surfaceId,
                            title->requestId);
      }
      if (const auto* meta = std::get_if<ja::SetMeta>(&message)) {
        const auto requestId = qc::RequestId::parse(meta->requestId);
        const auto surfaceId = qc::SurfaceId::parse(meta->surfaceId);
        if (!requestId || !surfaceId ||
            (!meta->title && !meta->description) || featureRegistry_ == nullptr) {
          return reject("invalid meta request", meta->surfaceId,
                        meta->requestId);
        }
        const auto feature = featureRegistry_->invoke(qcf::Request{
            .request_id = requestId.value(),
            .surface_id = surfaceId.value(),
            .module = qcf::ModuleId::kPageHost,
            .method = qcf::Method::kSetMeta,
            .text = meta->title.value_or(""),
            .description = meta->description});
        const auto error = feature.error
                               ? std::optional<ja::MessageRuntimeError>(
                                     ja::MessageRuntimeError{
                                         feature.error->code,
                                         feature.error->message,
                                         feature.error->retryable,
                                         meta->surfaceId,
                                         meta->requestId,
                                         std::nullopt,
                                         std::nullopt})
                               : std::nullopt;
        ja::SetMetaResult result{meta->requestId, meta->surfaceId,
                                 std::string(qcf::status_wire(feature.status)),
                                 error};
        return runtimeAbi_->postCallback(ja::JsInboundMessage(std::move(result))).ok
                   ? ja::EnqueueResult::accepted()
                   : reject("meta result queue rejected", meta->surfaceId,
                            meta->requestId);
      }
      if (!std::holds_alternative<ja::InstantiateTemplate>(message)) {
        if (const auto* navigation = std::get_if<ja::NavigationPush>(&message)) {
          std::fprintf(stderr,
                       "js.core.navigation_push request=%s uri=%s source=%s\n",
                       navigation->requestId.c_str(), navigation->uri.c_str(),
                       navigation->sourceSurfaceId.c_str());
          const auto requestId = qc::RequestId::parse(navigation->requestId);
          const auto source = qc::SurfaceId::parse(navigation->sourceSurfaceId);
          if (!requestId || !source || surfaceController_ == nullptr) {
            return reject("invalid navigation push", navigation->sourceSurfaceId,
                          navigation->requestId);
          }
          qc::RuntimeValue::Object params;
          for (const auto& [key, value] : navigation->params) {
            auto converted = toCoreRuntimeValue(value);
            if (!converted) {
              return reject("invalid navigation params", navigation->sourceSurfaceId,
                            navigation->requestId);
            }
            params.emplace(key, std::move(*converted));
          }
          const auto paramsValue = qc::RuntimeValue::object(std::move(params));
          if (!paramsValue) {
            return reject("invalid navigation params", navigation->sourceSurfaceId,
                          navigation->requestId);
          }
          const auto* paramsObject = std::get_if<std::shared_ptr<const qc::RuntimeValue::Object>>(
              &paramsValue.value().storage());
          if (paramsObject == nullptr || !*paramsObject) {
            return reject("invalid navigation params", navigation->sourceSurfaceId,
                          navigation->requestId);
          }
          std::string goalParam;
          const auto goal = (*paramsObject)->find("goal");
          if (goal != (*paramsObject)->end()) {
            const auto* goalValue = std::get_if<std::string>(&goal->second.storage());
            if (goalValue != nullptr) goalParam = *goalValue;
          }
          std::fprintf(stderr, "core.navigation.params request=%s goal=%s count=%zu\n",
                       navigation->requestId.c_str(), goalParam.c_str(),
                       (*paramsObject)->size());
          const auto accepted = surfaceController_->enqueue(qs::SurfaceRequest(
              qs::NavigationPushRequest{requestId.value(), source.value(),
                                        navigation->uri, **paramsObject}));
          if (!accepted) {
            return reject("Core rejected navigation push",
                          navigation->sourceSurfaceId, navigation->requestId);
          }
          {
            std::lock_guard lock(navigationMutex_);
            navigationSources_[navigation->requestId] = navigation->sourceSurfaceId;
          }
          navigationPushes_.fetch_add(1, std::memory_order_relaxed);
        }
        if (const auto* navigation = std::get_if<ja::NavigationClose>(&message)) {
          std::fprintf(stderr,
                       "js.core.navigation_close request=%s source=%s\n",
                       navigation->requestId.c_str(),
                       navigation->sourceSurfaceId.c_str());
          const auto requestId = qc::RequestId::parse(navigation->requestId);
          const auto source = qc::SurfaceId::parse(navigation->sourceSurfaceId);
          if (!requestId || !source || surfaceController_ == nullptr) {
            return reject("invalid navigation close", navigation->sourceSurfaceId,
                          navigation->requestId);
          }
          const auto accepted = surfaceController_->enqueue(qs::SurfaceRequest(
              qs::NavigationCloseRequest{requestId.value(), source.value()}));
          if (!accepted) {
            return reject("Core rejected navigation close",
                          navigation->sourceSurfaceId, navigation->requestId);
          }
          std::fprintf(stderr, "js.core.navigation_close.enqueued=1\n");
        }
        return ja::EnqueueResult::accepted();
      }
      const auto& instantiate = std::get<ja::InstantiateTemplate>(message);
      std::fprintf(stderr,
                   "js.core.instantiate request=%s surface=%s template=%s bindings=%zu handlers=%zu\n",
                   instantiate.requestId.c_str(), instantiate.surfaceId.c_str(),
                   instantiate.templateId.c_str(), instantiate.initialBindings.size(),
                   instantiate.initialHandlers.size());
      const auto parsedSurface = qc::SurfaceId::parse(instantiate.surfaceId);
      const auto pageOwner = qc::ComponentInstanceId::parse(instantiate.ownerInstanceId);
      qp::PageIrHandle page;
      {
        std::lock_guard lock(pagesMutex_);
        const auto found = pages_.find(instantiate.surfaceId);
        if (found != pages_.end()) page = found->second;
      }
      if (!parsedSurface || !pageOwner || !page) {
        return reject("invalid initial template identity", instantiate.surfaceId,
                      instantiate.requestId);
      }
      std::map<std::uint64_t, qc::runtime_tree::BindingValue> bindings;
      for (const auto& [id, value] : instantiate.initialBindings) {
        bindings.emplace(id, std::visit(
            [](const auto& item) -> qc::runtime_tree::BindingValue { return item; },
            value));
      }
      std::vector<qc::runtime_tree::HandlerRegistration> handlers;
      for (const auto& binding : instantiate.initialHandlers) {
        const auto owner = qc::ComponentInstanceId::parse(binding.ownerInstanceId);
        const auto templateId = qc::TemplateHandlerId::from(binding.templateHandlerId);
        const auto handlerId = qc::HandlerId::parse(binding.handlerId);
        if (!owner || !templateId || !handlerId) {
          return reject("invalid initial handler identity", instantiate.surfaceId,
                        instantiate.requestId);
        }
        const auto* definition = page->find_handler(templateId.value().value());
        if (definition == nullptr || definition->scope_block_id.has_value()) {
          continue;
        }
        handlers.push_back({owner.value(), templateId.value(), handlerId.value()});
      }
      std::vector<qc::runtime_tree::InstantiateBlockRequest> initialBlocks;
      std::map<std::string, std::vector<std::string>, std::less<>> initialBlockHandlers;
      for (const auto& block : instantiate.initialBlocks) {
        const auto blockId = qc::BlockInstanceId::parse(block.blockInstanceId);
        const auto templateId = qc::TemplateBlockId::from(block.templateBlockId);
        const auto parentTemplateId = qc::TemplateNodeId::from(block.parent.templateNodeId);
        const auto parentComponent = qc::ComponentInstanceId::parse(block.parent.ownerInstanceId);
        const auto parentBlock = qc::BlockInstanceId::parse(block.parent.ownerInstanceId);
        if (!blockId || !templateId || !parentTemplateId ||
            (!parentComponent && !parentBlock)) {
          return reject("invalid initial block identity", instantiate.surfaceId,
                        instantiate.requestId);
        }
        qc::OwnerInstanceId parentOwner =
            parentComponent ? qc::OwnerInstanceId(parentComponent.value())
                            : qc::OwnerInstanceId(parentBlock.value());
        std::map<std::uint64_t, qc::runtime_tree::BindingValue> blockBindings;
        for (const auto& [id, value] : block.initialBindings) {
          blockBindings.emplace(id, std::visit(
              [](const auto& item) -> qc::runtime_tree::BindingValue { return item; },
              value));
        }
        std::vector<qc::runtime_tree::HandlerRegistration> blockHandlers;
        for (const auto& binding : block.handlers) {
          const auto owner = qc::BlockInstanceId::parse(binding.ownerInstanceId);
          const auto handlerTemplateId = qc::TemplateHandlerId::from(binding.templateHandlerId);
          const auto handlerId = qc::HandlerId::parse(binding.handlerId);
          if (!owner || !handlerTemplateId || !handlerId) {
            return reject("invalid initial block handler identity", instantiate.surfaceId,
                          instantiate.requestId);
          }
          blockHandlers.push_back({owner.value(), handlerTemplateId.value(), handlerId.value()});
          initialBlockHandlers[block.blockInstanceId].push_back(binding.handlerId);
        }
        std::optional<qc::runtime_tree::BlockKey> key;
        if (block.key.has_value()) {
          key = std::visit([](const auto& value) -> qc::runtime_tree::BlockKey {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, std::string>) {
              return value;
            } else {
              return static_cast<std::int64_t>(value);
            }
          }, *block.key);
        }
        initialBlocks.push_back({templateId.value(), blockId.value(),
                                 {std::move(parentOwner), parentTemplateId.value()},
                                 static_cast<std::size_t>(block.index), key.value_or(std::string("")),
                                 std::move(blockBindings), std::move(blockHandlers)});
      }
      if (coordinator_ == nullptr) {
        return reject("Core render pipeline is unavailable", instantiate.surfaceId,
                      instantiate.requestId);
      }
      const auto sourceId = qc::RequestId::parse(instantiate.requestId);
      if (!sourceId) {
        return reject("invalid initial render request identity",
                      instantiate.surfaceId, instantiate.requestId);
      }
      const auto submitted = coordinator_->submit(qr::InitialRenderIntent{
          parsedSurface.value(), sourceId.value(), pageOwner.value(), page,
          std::move(bindings), {gRuntimeViewportWidth, gRuntimeViewportHeight},
          std::move(handlers), std::move(initialBlocks)});
      if (!submitted) {
        return reject("Core rejected initial render", instantiate.surfaceId,
                      instantiate.requestId);
      }
      templateIds_[instantiate.surfaceId] = instantiate.templateId;
      bindBlockHandlers(instantiate.surfaceId, initialBlockHandlers);
      submitted_ = true;
      return ja::EnqueueResult::accepted();
    } catch (...) {
      return reject("out of memory while submitting initial render", "", "");
    }
  }

  [[nodiscard]] bool submitted() const noexcept { return submitted_; }
  [[nodiscard]] std::size_t navigationPushes() const noexcept {
    return navigationPushes_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::optional<std::string> takeNavigationSource(
      std::string_view requestId) noexcept {
    std::lock_guard lock(navigationMutex_);
    const auto found = navigationSources_.find(std::string(requestId));
    if (found == navigationSources_.end()) return std::nullopt;
    auto source = std::move(found->second);
    navigationSources_.erase(found);
    return source;
  }

 private:
  void bindBlockHandlers(
      std::string_view surfaceId,
      const std::map<std::string, std::vector<std::string>, std::less<>>& handlers) {
    if (modules_ == nullptr || vm_ == nullptr || handlerRegistry_ == nullptr) return;
    const auto templateFound = templateIds_.find(std::string(surfaceId));
    if (templateFound == templateIds_.end()) return;
    const auto definition = modules_->pageDefinitionForSurfaceOnExecutor(
        surfaceId, templateFound->second);
    if (!definition) return;
    const std::string handlerPrefix = "hdl:" + std::string(surfaceId) + "-";
    for (const auto& [blockId, ids] : handlers) {
      auto& registered = blockHandlers_[blockId];
      for (const auto& handlerId : ids) {
        if (!std::string_view(handlerId).starts_with(handlerPrefix)) continue;
        const auto templateStart = handlerPrefix.size();
        const auto templateSeparator = handlerId.find('-', templateStart);
        if (templateSeparator == std::string::npos ||
            templateSeparator == templateStart) continue;
        std::uint64_t templateId = 0;
        try {
          templateId = std::stoull(handlerId.substr(
              templateStart, templateSeparator - templateStart));
        } catch (...) {
          continue;
        }
        const auto method = modules_->handlerMethodNameOnExecutor(
            *definition, templateId);
        auto pageVm = vm_->pageVmOnExecutor(surfaceId);
        if (method && pageVm.ok() && handlerRegistry_->bind(
                          std::string(surfaceId), handlerId, *method,
                          std::move(pageVm).value())) {
          registered.push_back(handlerId);
        }
      }
    }
  }

  static ja::EnqueueResult reject(std::string message, std::string surfaceId,
                                  std::string requestId) noexcept {
    return ja::EnqueueResult::rejected({ja::AbiErrorCode::InvalidArgument,
                                        std::move(message), false,
                                        std::move(surfaceId),
                                        std::move(requestId), {}, {}});
  }

  static ja::EnqueueResult rejectTransaction(std::string message,
                                             std::string surfaceId,
                                             std::string transactionId) noexcept {
    return ja::EnqueueResult::rejected({ja::AbiErrorCode::InvalidArgument,
                                        std::move(message), false,
                                        std::move(surfaceId), std::nullopt,
                                        std::move(transactionId), std::nullopt});
  }

  qr::MountCoordinator* coordinator_{nullptr};
  std::mutex pagesMutex_;
  std::map<std::string, qp::PageIrHandle, std::less<>> pages_;
  qs::SurfaceController* surfaceController_{nullptr};
  std::mutex navigationMutex_;
  std::map<std::string, std::string, std::less<>> navigationSources_;
  ja::RuntimeAbiService* runtimeAbi_{nullptr};
  qj::module::ModuleLoader* modules_{nullptr};
  qj::vm::VmLifecycleService* vm_{nullptr};
  qj::event::HandlerRegistry* handlerRegistry_{nullptr};
  std::map<std::string, std::string, std::less<>> templateIds_;
  std::map<std::string, std::vector<std::string>, std::less<>> blockHandlers_;
  qcf::ModuleRegistry* featureRegistry_{nullptr};
  qc::timer::TimerRegistry* timerRegistry_{nullptr};
  bool submitted_{false};
  std::atomic<std::size_t> navigationPushes_{0};
};

class JsTimerCallbackPort final : public qc::timer::CallbackPort {
 public:
  void bind(ja::RuntimeAbiService& runtimeAbi) noexcept {
    runtimeAbi_ = &runtimeAbi;
  }

  qc::EnqueueResult post(qc::timer::Callback&& callback) noexcept override {
    if (runtimeAbi_ == nullptr) {
      return qc::EnqueueResult::failure(qc::RuntimeError::simple(
          qc::RuntimeErrorCode::kPlatformRejected,
          "Timer JS owner queue is unavailable"));
    }
    const auto result = runtimeAbi_->postCallback(ja::JsInboundMessage(
        ja::TimerFired{callback.surface_id.wire(), callback.timer_id.wire(),
                       callback.sequence, callback.missed_periods}));
    return result.ok
               ? qc::EnqueueResult::success(qc::Accepted{})
               : qc::EnqueueResult::failure(qc::RuntimeError::simple(
                     qc::RuntimeErrorCode::kQueueOverflow,
                     "Timer JS owner queue rejected callback"));
  }

 private:
  ja::RuntimeAbiService* runtimeAbi_{nullptr};
};

}  // namespace

// LVGL tick 定时器 (真机由 esp_timer 每 1ms 驱动)
static void device_lvgl_tick_cb(void*) { lv_tick_inc(1); }
static void device_start_lvgl_tick() {
  esp_timer_create_args_t args = {};
  args.callback = device_lvgl_tick_cb;
  args.name = "lvgl_tick";
  esp_timer_handle_t timer = nullptr;
  ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 1000));  // 1ms
}

static void device_runtime_main() {
  try {
    // 真机固定参数 (无命令行): sport-watch 走 showcase(push导航) 路径
    const bool binding001 = false;
    const bool case002 = false;
    const bool block001 = false;
    bool lvglP0 = false;
    const bool imageInputMissing = false;
    const bool imageInput001 = false;
    const bool s4Back = false;

    auto& factory = *new qc::AppRuntimeFactory();
    auto identity = std::move(factory.create()).value();

    // RPK 从 SPIFFS rpk_store 分区读取 (由 CMake 打包烧录)
    auto source = quickapp::device::SpiffsPackageSource::create(
        "rpk_store", "app.rpk");
    if (!source) throw std::runtime_error("SPIFFS RPK source unavailable");
    auto composition = makeDeviceComposition();
    auto loader = std::move(qp::PackageLoader::create(
        source, identity.request_ids(), std::move(composition))).value();
    std::shared_ptr<const qp::VerifiedPackage> package;
    std::string failure;
    ESP_LOGW(TAG, "STACK before open(): min_free=%u bytes",
             (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    if (!loader->open([&](auto result) { if (result) package = std::move(result).value(); else failure = result.error().message; })) throw std::runtime_error("RPK open enqueue failed");
    ESP_LOGW(TAG, "STACK after open(): min_free=%u bytes",
             (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    if (!package || !failure.empty()) throw std::runtime_error("RPK open failed: " + failure);
    ESP_LOGI(TAG, "RPK opened: package=%s entry=%s",
             package->package_id().c_str(), package->entry_route().c_str());
    const bool gallery001 = package->package_id() == "com.quickappkit.gallery001";
    [[maybe_unused]] const bool cardWallet =
        package->package_id() == "com.quickappkit.cardwallet";
    const bool controls001 = package->package_id() == "com.quickappkit.controls001";
    const bool controls002 = package->package_id() == "com.quickappkit.controls002";
    const bool list001 = package->package_id() == "com.quickappkit.list001";
    const bool tabs001 = package->package_id() == "com.quickappkit.tabs001";
    const bool platform001 = package->package_id() == "com.quickappkit.platform001";
    const bool mountOnlyRpk = controls002 || list001 || tabs001 || platform001;
    // 真机: 任意入口为 /pages/Home 的应用都按 showcase(push导航)处理
    const bool showcaseRpk = !mountOnlyRpk &&
        package->entry_route() == "/pages/Home";
    if (!mountOnlyRpk && package->entry_route() == "/pages/Home" &&
        !gallery001 && !controls001) {
      lvglP0 = true;
    }

    // LVGL 已在 app_main 里 init; 这里创建真机 display + 触摸 indev
    lv_display_t* display = display_driver_init();
    if (!display) throw std::runtime_error("ILI9341 display init failed");
    lv_display_set_default(display);
    lv_indev_t* mouse = touch_driver_init(display);
    if (!mouse) throw std::runtime_error("XPT2046 touch init failed");
    lv_indev_set_display(mouse, display);
    ESP_LOGI(TAG, "display+touch ready %dx%d",
             static_cast<int>(gRuntimeViewportWidth),
             static_cast<int>(gRuntimeViewportHeight));
    // 嵌入式适配: 所有大型装配对象改为堆分配(用引用别名, 下游代码零改动),
    // 避免全部塞进单个 102KB 栈帧导致 MCU 爆栈。设备常驻运行, 不回收(无泄漏问题)。
    auto& taskStorage = *new std::array<qlf::OwnerTask, 128>();
    auto& tasks = *new qlf::OwnerTaskQueue(taskStorage.data(), taskStorage.size(), 128, nullptr);
    if (!tasks.bindOwner(kOwner).ok()) throw std::runtime_error("owner bind failed");
    std::fprintf(stderr, "phase=display_ready\n");
    lv_obj_t* pageRootParent = lv_screen_active();
    auto& roots = *new qls::LvglPageRootBackend(pageRootParent);
    auto& featureProvider = *new qlfeat::LvglFeatureProvider(lv_screen_active());
    auto& featureRegistry = *new qcf::ModuleRegistry();
    if (!featureRegistry.register_provider(qcf::ModuleId::kSystemPrompt,
                                           featureProvider) ||
        !featureRegistry.register_provider(qcf::ModuleId::kSystemDevice,
                                           featureProvider) ||
        !featureRegistry.register_provider(qcf::ModuleId::kSystemFetch,
                                           featureProvider) ||
        !featureRegistry.register_provider(qcf::ModuleId::kSystemFile,
                                           featureProvider) ||
        !featureRegistry.register_provider(qcf::ModuleId::kPageHost,
                                           featureProvider)) {
      throw std::runtime_error("LVGL Feature provider registration failed");
    }
    auto& content = *new SurfaceContent();
    auto& mountResults = *new MountResults();
    auto bridge = std::make_unique<qli::CoreMountBridge>(kOwner, mountResults,
                                                         nullptr, false);
    auto* bridgeRaw = bridge.get();
    auto& surfaceResults = *new SurfaceResults(*bridgeRaw);
    auto& surfaces = *new qls::SurfaceHostAdapter(tasks, kOwner, roots, content, surfaceResults, qls::simulatorSurfaceHostLimits());
    auto& nativeRoots = *new qlm::LvglMountBackend(roots);
    auto mounts = std::make_unique<qlm::MountHost>(
        tasks, kOwner, surfaces, nativeRoots, *bridgeRaw,
        qlm::simulatorMountHostLimits());
    if (!imageInputMissing) {
      std::size_t loadedResources = 0;
      for (const auto& [resourcePath, descriptor] : package->resources()) {
        static_cast<void>(descriptor);
        if (!resourcePath.starts_with("assets/")) continue;
        std::shared_ptr<const qp::Bytes> resourceBytes;
        if (!loader->load_resource(resourcePath, [&](auto result) {
              if (result) {
                resourceBytes = std::make_shared<const qp::Bytes>(
                    std::move(result).value());
              }
            }) || resourceBytes == nullptr) {
          throw std::runtime_error("RPK resource could not be loaded: " + resourcePath);
        }
        mounts->setResource(resourcePath, resourceBytes);
        ++loadedResources;
        std::fprintf(stderr, "rpk.resource.loaded path=%s bytes=%zu\n",
                     resourcePath.c_str(), resourceBytes->size());
      }
      if ((imageInput001 || showcaseRpk) && loadedResources == 0) {
        throw std::runtime_error("Image RPK has no assets resource");
      }
    }
    content.bind(*mounts);
    auto& counters = *new qc::RuntimeCounters();
    auto& fontResults = *new FontResults();
    auto& publisher = *new qm::FontSnapshotPublisher(fontResults);
    if (!publisher.initialize(kOwner, qm::FontMetricsSnapshot::makeV1(1)).ok()) throw std::runtime_error("font initialization failed");
    auto& platformMeasure = *new qm::FontMeasureAdapter(publisher, qm::simulatorMeasureLimits());
    auto measure = std::make_unique<CoreMeasure>(platformMeasure);
    auto initialResults = std::make_unique<ControllerInitialResults>();
    auto* initialResultsRaw = initialResults.get();
    auto renderResults = std::make_unique<RenderResults>();
    auto* renderResultsRaw = renderResults.get();
    auto& coreIngress = *new JsCoreIngress();
    coreIngress.bindFeatureRegistry(featureRegistry);
    auto& timerCallbacks = *new JsTimerCallbackPort();
    auto& timerClock = *new qc::SteadyMonotonicClock();
    auto& timerRegistry = *new qc::timer::TimerRegistry(timerClock, timerCallbacks);
    coreIngress.bindTimerRegistry(timerRegistry);
    auto& eventRouter = *new qc::event::EventRouter(coreIngress);
    auto coordinatorResult = qr::MountCoordinator::create(
      {&identity.request_ids(), &counters, std::move(measure), std::move(bridge),
         std::move(initialResults), nullptr, nullptr, &eventRouter,
         std::move(renderResults)});
    if (!coordinatorResult) throw std::runtime_error("MountCoordinator create failed");
    auto coordinator = std::move(coordinatorResult).value();
    auto* coordinatorRaw = coordinator.get();
    coreIngress.bindCoordinator(*coordinatorRaw);
    mountResults.bind(*coordinator);
    std::fprintf(stderr, "phase=coordinator_ready\n");

    auto provider = std::make_unique<qj::QuickJsEngineProvider>();
    const auto descriptor = provider->describe();
    auto& clock = *new Clock();
    auto& traceSink = *new TraceSink();
    auto registration = qj::TraceSinkRegistration::admit(
        traceSink, {.nonblocking = true, .noReentry = true});
    if (!registration.ok()) throw std::runtime_error("trace registration failed");
    auto& engineConfig = *new qj::JsEngineConfig();
    engineConfig.expectedEngine = descriptor;
    engineConfig.limits.maxPendingTasks = 32;
    // MCU: ManualPump 单线程模式 — 不开 JS worker 线程(省第二个大栈),
    // JS 事件循环由本 runtime 线程在 service() 里 engine.pump(N) 驱动。
    engineConfig.executorMode = qj::ExecutorMode::ManualPump;
    const char* appRuntimeId = binding001 ? "app:binding001" : (case002 ? "app:case002" : (block001 ? "app:block001" : (lvglP0 ? "app:lvgl-p0" : "app:case001")));
    const char* observationName = binding001 ? "binding001-lvgl" : (case002 ? "case002-lvgl" : (block001 ? "block001-lvgl" : (lvglP0 ? "lvgl-p0" : "case001-lvgl")));
    std::unique_ptr<qj::EventLoopBackend> jsBackend;
#if defined(QUICKAPP_EXAMPLES_USE_LIBUV_JS_BACKEND)
    jsBackend = std::make_unique<qj::LibuvEventLoopBackend>(
        engineConfig.limits.maxPendingTasks);
#endif
    // ManualPump: 不再需要为 JS worker 线程预留第二个大栈。
    auto& engine = *new qj::JsEngineService(appRuntimeId, std::move(provider), engineConfig,
                               clock, std::move(registration).value(),
                               {false, observationName, "steady", 0},
                               std::move(jsBackend));
    std::promise<qj::ServiceResult> started;
    auto startedFuture = started.get_future();
    if (!engine.start([&](qj::ServiceResult result) {
          started.set_value(std::move(result));
        })) {
      throw std::runtime_error("QuickJS start failed");
    }
    // ManualPump: 必须由本线程驱动引擎, 否则 start 里 post 的初始化任务不会执行,
    // future 永远不就绪。pump 到 future ready。
    while (startedFuture.wait_for(std::chrono::seconds(0)) !=
           std::future_status::ready) {
      engine.pump(8);
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    auto startedResult = startedFuture.get();
    if (!startedResult.ok()) {
      std::fprintf(stderr, "quickjs.start.failed code=%s error=%s\n",
                   qj::runtimeErrorCodeName(startedResult.error().code).data(),
                   startedResult.error().message.c_str());
      std::promise<void> failedStop;
      if (engine.stop([] {}, [&] { failedStop.set_value(); }))
        failedStop.get_future().get();
      throw std::runtime_error("QuickJS start failed");
    }

    qj::JsEngineService* enginePtr = &engine;

    auto& completion = *new ModuleCompletion();
    auto& jsRequestIds = *new JsRequestIds();
    auto* facades = new qj::framework::StaticFacadeCatalog();
    qj::module::ModuleLoader* modules = nullptr;
    std::shared_ptr<qj::abi::RuntimeAbiService> runtimeAbi;
    qj::event::HandlerRegistry* handlerRegistry = nullptr;
    qj::page::PageHostControlInstaller* pageControls = nullptr;
    qj::binding::AlphaInitialBindingStage* bindingStage = nullptr;
    qj::render::AlphaInitialTransactionBuilder* transactionBuilder = nullptr;
    qj::alpha::AlphaPageInitializationStage* pageStage = nullptr;
    qj::vm::VmLifecycleService* vm = nullptr;
    qs::SurfaceController* controller = nullptr;
    std::uint64_t moduleRequestSequence = 0;
    std::uint64_t pageInitSequence = 0;

    auto& surfaceIngress = *new CoreSurfaceResultIngress();
    auto& appState = *new AppState();
    auto& statusSink = *new ControllerStatus();
    auto& lifecycleSink = *new ControllerLifecycleResults();
    auto& platformBack = *new PlatformBackIngress();
    std::atomic<bool> closeCompleted{false};
    std::optional<qc::RuntimeError> closeError;
    std::atomic<std::uint64_t> jsThreadHash{0};
    std::atomic<std::uint64_t> ownerThreadHash{0};

    auto pageLifecycle = std::make_unique<ControllerPageLifecycle>(
        [&](qs::PageCommand&& command) -> qc::EnqueueResult {
          if (enginePtr == nullptr || controller == nullptr) {
            return qc::EnqueueResult::failure(qc::RuntimeError::simple(
                qc::RuntimeErrorCode::kPlatformRejected,
                "JS page lifecycle is unavailable"));
          }
          return enginePtr->post(
              [&, command = std::move(command)](qj::JsEnginePort&,
                                                const qj::JsContextRef&) mutable {
                auto complete = [&](const qs::PageCommand& value,
                                    bool ok,
                                    std::optional<qc::RuntimeError> error = std::nullopt) {
                  const auto* start = std::get_if<qs::PageStartCommand>(&value);
                  const auto* hook = std::get_if<qs::PageHookCommand>(&value);
                  static_cast<void>(controller->enqueue(qs::PageLifecycleResult{
                      start ? start->request_id : hook->request_id,
                      start ? qs::PageCommandKind::kStart : qs::PageCommandKind::kHook,
                      start ? start->surface_id : hook->surface_id,
                      hook ? std::optional<qs::PageHook>(hook->hook) : std::nullopt,
                      ok, std::move(error)}));
                };
                try {
                  if (auto* start = std::get_if<qs::PageStartCommand>(&command)) {
                    const bool abiOpen = runtimeAbi != nullptr &&
                        runtimeAbi->openSurfaceOnExecutor(start->surface_id.wire()).ok();
                    const bool moduleOpen = modules != nullptr &&
                        modules->openSurfaceOnExecutor(start->surface_id.wire());
                    if (modules == nullptr || vm == nullptr || runtimeAbi == nullptr ||
                        !abiOpen || !moduleOpen) {
                      complete(command, false, qc::RuntimeError::simple(
                          qc::RuntimeErrorCode::kSurfaceNotFound,
                          "page JS surface could not open"));
                      return;
                    }
                    ja::LoadVerifiedModule message;
                    message.requestId = "req:j-" +
                                        std::to_string(++moduleRequestSequence);
                    const auto& module = start->page.module;
                    message.packageId = module.package_id();
                    message.moduleKind = "page";
                    message.moduleId = module.module_id();
                    message.cacheScope = "surface";
                    message.surfaceId = start->surface_id.wire();
                    message.dependencies = module.dependencies();
                    message.bundle = {module.descriptor().path,
                                      module.descriptor().byte_length,
                                      module.descriptor().sha256,
                                      std::make_shared<const std::vector<std::uint8_t>>(
                                          *module.bytes())};
                    message.expectedBootstrap = ja::BootstrapExpectation{
                        "page", module.module_id(), module.expected_template_id()};
                    message.expectedBindingIds = module.expected_binding_ids();
                    message.expectedHandlerIds = module.expected_handler_ids();
                    modules->onLoadVerifiedModule(message);
                    coreIngress.bindPage(start->surface_id, start->page.page_ir);
                    qj::RuntimeValue::Object pageParams;
                    for (const auto& [key, value] : start->params)
                      pageParams.emplace(key, toJsRuntimeValue(value));
                    std::string pageGoalParam;
                    const auto pageGoal = start->params.find("goal");
                    if (pageGoal != start->params.end()) {
                      const auto* goalValue = std::get_if<std::string>(
                          &pageGoal->second.storage());
                      if (goalValue != nullptr) pageGoalParam = *goalValue;
                    }
                    std::fprintf(stderr,
                                 "core.page.context_params surface=%s goal=%s count=%zu\n",
                                 start->surface_id.wire().c_str(), pageGoalParam.c_str(),
                                 start->params.size());
                    vm->onSurfaceContext({start->surface_id.wire(), package->package_id(),
                                          start->page.route,
                                          module.expected_template_id().value_or(""),
                                          std::move(pageParams), {"setTitleBar", "setMeta"},
                                          {gRuntimeViewportWidth,
                                           gRuntimeViewportHeight,
                                           "logical-px"}});
                    complete(command, completion.status == "loaded",
                             completion.status == "loaded"
                                 ? std::nullopt
                                 : std::optional<qc::RuntimeError>(qc::RuntimeError::simple(
                                       qc::RuntimeErrorCode::kModuleAbiUnsupported,
                                       completion.error)));
                    return;
                  }
                  auto* hook = std::get_if<qs::PageHookCommand>(&command);
                  if (hook == nullptr) return;
                  if (hook->hook == qs::PageHook::kOnDestroy) {
                    if (handlerRegistry != nullptr)
                      handlerRegistry->closeSurface(hook->surface_id.wire());
                    eventRouter.closeSurface(hook->surface_id);
                    if (vm != nullptr)
                      vm->closeSurfaceOnExecutor(hook->surface_id.wire());
                  }
                  complete(command, true);
                } catch (...) {
                  complete(command, false, qc::RuntimeError::simple(
                      qc::RuntimeErrorCode::kJsException,
                      "page lifecycle execution failed"));
                }
              }).status == qj::PostStatus::Accepted
                     ? qc::EnqueueResult::success(qc::Accepted{})
                     : qc::EnqueueResult::failure(qc::RuntimeError::simple(
                           qc::RuntimeErrorCode::kQueueOverflow,
                           "JS page lifecycle queue rejected"));
        });

    auto initialPipeline = std::make_unique<ControllerInitialPipeline>(
        [&](qs::InitialContentCommand&& command) -> qc::EnqueueResult {
          if (coordinatorRaw == nullptr || enginePtr == nullptr || vm == nullptr)
            return qc::EnqueueResult::failure(qc::RuntimeError::simple(
                qc::RuntimeErrorCode::kPlatformRejected,
                "initial render services are unavailable"));
          const auto surfaceId = command.surface_id;
          const auto pageIr = command.page_ir;
          auto posted = coordinatorRaw->post(std::move(command));
          if (!posted) return posted;
          const auto task = enginePtr->post(
              [&, surfaceId, pageIr](qj::JsEnginePort&, const qj::JsContextRef&) {
                vm->onVmInitialization({
                    "req:" + std::to_string(1000 + ++pageInitSequence),
                    "page", surfaceId.wire()});
                if (modules == nullptr || handlerRegistry == nullptr) return;
                const auto definition = modules->pageDefinitionForSurfaceOnExecutor(
                    surfaceId.wire(), pageIr->template_id());
                if (!definition) return;
                const auto handlers = modules->handlerBindingsOnExecutor(
                    *definition, "cmp:" + surfaceId.wire());
                if (!handlers.ok()) return;
                for (const auto& binding : handlers.value()) {
                  const auto* handlerDefinition =
                      pageIr->find_handler(binding.templateHandlerId);
                  if (handlerDefinition == nullptr ||
                      handlerDefinition->scope_block_id.has_value()) {
                    continue;
                  }
                  const auto method = modules->handlerMethodNameOnExecutor(
                      *definition, binding.templateHandlerId);
                  auto retained = vm->pageVmOnExecutor(surfaceId.wire());
                  if (method && retained.ok())
                    static_cast<void>(handlerRegistry->bind(
                        surfaceId.wire(), binding.handlerId, *method,
                        std::move(retained).value()));
                }
              });
          return task.status == qj::PostStatus::Accepted
                     ? qc::EnqueueResult::success(qc::Accepted{})
                     : qc::EnqueueResult::failure(qc::RuntimeError::simple(
                           qc::RuntimeErrorCode::kQueueOverflow,
                           "JS initial render queue rejected"));
        });
    initialPipeline->onRelease([&](const qc::SurfaceId& surfaceId) {
      if (coordinatorRaw != nullptr) coordinatorRaw->release_surface(surfaceId);
    });

    auto operationResults = std::make_unique<ControllerOperationResults>(
        [&](qs::SurfaceOperationKind kind, qc::RequestId requestId,
            std::optional<qc::SurfaceId> target, bool completed,
            std::optional<qc::RuntimeError> error) {
          if (kind == qs::SurfaceOperationKind::kClose) {
            closeCompleted.store(completed, std::memory_order_release);
            closeError = std::move(error);
            std::fprintf(stderr,
                         "core.navigation.close request=%s source=%s completed=%d\n",
                         requestId.wire().c_str(),
                         target ? target->wire().c_str() : "",
                         completed ? 1 : 0);
            return;
          }
          if (kind != qs::SurfaceOperationKind::kPush || runtimeAbi == nullptr) return;
          const auto source = coreIngress.takeNavigationSource(requestId.wire());
          if (!source) return;
          ja::NavigationPushResult result{
              requestId.wire(), *source, completed ? "completed" : "failed",
              target ? std::optional<std::string>(target->wire()) : std::nullopt,
              error ? std::optional<ja::MessageRuntimeError>(ja::MessageRuntimeError{
                         std::string(qc::to_wire(error->code)),
                         std::string(error->message),
                         error->retryable, std::nullopt, requestId.wire(), std::nullopt,
                         std::nullopt})
                    : std::nullopt};
          static_cast<void>(runtimeAbi->postCallback(
              ja::JsInboundMessage(std::move(result))));
        });

    auto pages = std::make_unique<PageResolver>(*loader, package);
    auto platform = std::make_unique<SurfacePlatform>(surfaces);
    [[maybe_unused]] auto* platformRaw = platform.get();
    qs::SurfaceLimits surfaceLimits;
    if (s4Back) surfaceLimits.ingress_capacity = 1;
    auto controllerResult = qs::SurfaceController::create(
        {&appState, &identity.request_ids(), std::move(pages), std::move(platform),
         std::move(pageLifecycle), std::move(initialPipeline),
         std::move(operationResults), std::make_unique<ControllerStatus>(),
         std::make_unique<ControllerLifecycleResults>(), &counters},
        surfaceLimits);
    if (!controllerResult) throw std::runtime_error("SurfaceController create failed");
    auto surfaceController = std::move(controllerResult).value();
    controller = surfaceController.get();
    coreIngress.bindSurfaceController(*controller);
    platformBack.bind(*controller);
    surfaceIngress.bind(*controller);
    initialResultsRaw->bind(*controller);
    bridgeRaw->bind(*mounts, surfaces, &surfaceIngress);
    std::promise<bool> baseReady;
    auto baseReadyFuture = baseReady.get_future();
    std::atomic<bool> baseReadyOnce{false};
    const auto finishBase = [&](bool value) {
      if (!baseReadyOnce.exchange(true)) baseReady.set_value(value);
    };
    if (engine.post([&](qj::JsEnginePort& js, const qj::JsContextRef& context) {
          jsThreadHash.store(static_cast<std::uint64_t>(
              std::hash<std::thread::id>{}(std::this_thread::get_id())),
              std::memory_order_relaxed);
          try {
            if (!facades->startOnExecutor(js, context)) { finishBase(false); return; }
            modules = new qj::module::ModuleLoader(
                engine, completion, binding001 ? "app:binding001" : (case002 ? "app:case002" : (block001 ? "app:block001" : (lvglP0 ? "app:lvgl-p0" : "app:case001"))), package->package_id(),
                qj::module::ModuleLoaderLimits{}, facades);
            if (!modules->startOnExecutor(js, context)) { finishBase(false); return; }
            runtimeAbi = std::make_shared<qj::abi::RuntimeAbiService>(
                engine, coreIngress, qj::abi::RuntimeAbiLimits{},
                qj::abi::CapabilitySupportSnapshot{});
            if (!runtimeAbi->startOnExecutor(js, context,
                                             qj::abi::kRuntimeAbiIdentity).ok()) {
              finishBase(false); return;
            }
            renderResultsRaw->bind(*runtimeAbi);
            coreIngress.bindAbi(*runtimeAbi);
            timerCallbacks.bind(*runtimeAbi);
            handlerRegistry = new qj::event::HandlerRegistry(engine);
            pageControls = new qj::page::PageHostControlInstaller(
                engine, *runtimeAbi, jsRequestIds);
            bindingStage = new qj::binding::AlphaInitialBindingStage(engine, *modules);
            transactionBuilder = new qj::render::AlphaInitialTransactionBuilder(
                engine, jsRequestIds);
            if (!handlerRegistry->startOnExecutor(js, context) ||
                !pageControls->startOnExecutor(js, context) ||
                !bindingStage->startOnExecutor(js, context) ||
                !transactionBuilder->startOnExecutor(js, context)) {
              finishBase(false); return;
            }
            pageStage = new qj::alpha::AlphaPageInitializationStage(
                *bindingStage, *transactionBuilder);
            vm = new qj::vm::VmLifecycleService(engine, *modules, *pageControls,
                                                 *pageStage, package->package_id());
            coreIngress.bindJsServices(*modules, *vm, *handlerRegistry);
            auto slots = modules->callbackSlots();
            auto vmSlots = vm->callbackSlots();
            slots.appContext = std::move(vmSlots.appContext);
            slots.surfaceContext = std::move(vmSlots.surfaceContext);
            slots.vmInitializationDispatch = std::move(vmSlots.vmInitializationDispatch);
            slots.jsEventDispatch = [handlerRegistry](const ja::JsEventDispatch& value) {
              const bool handled = handlerRegistry->dispatchOnExecutor(value);
              std::fprintf(stderr,
                           "js.event.handler surface=%s handler=%s handled=%d\n",
                           value.surfaceId.c_str(), value.handlerId.c_str(),
                           handled ? 1 : 0);
            };
            slots.timerStartResult = [facades](const ja::TimerStartResult& value) {
              static_cast<void>(facades->dispatchTimerStartResultOnExecutor(value));
            };
            slots.timerCancelResult = [facades](const ja::TimerCancelResult& value) {
              static_cast<void>(facades->dispatchTimerCancelResultOnExecutor(value));
            };
            slots.timerFired = [facades](const ja::TimerFired& value) {
              static_cast<void>(facades->dispatchTimerFiredOnExecutor(value));
              std::fprintf(stderr, "timer.fired surface=%s timer=%s sequence=%llu missed=%llu\n",
                           value.surfaceId.c_str(), value.timerId.c_str(),
                           static_cast<unsigned long long>(value.sequence),
                           static_cast<unsigned long long>(value.missedPeriods));
            };
            slots.renderTransactionResult = [](const ja::RenderTransactionResult& value) {
              std::fprintf(stderr,
                           "js.render.complete surface=%s transaction=%s status=%s revision=%llu\n",
                           value.surfaceId.c_str(), value.transactionId.c_str(),
                           value.status.c_str(),
                           static_cast<unsigned long long>(value.committedRevision));
            };
            if (!runtimeAbi->registerConsumersOnExecutor(std::move(slots)).ok() ||
                !vm->startOnExecutor(js, context)) { finishBase(false); return; }

            const auto send = [&](const qp::VerifiedModule& module,
                                  std::string kind,
                                  std::optional<ja::BootstrapExpectation> bootstrap) {
              ja::LoadVerifiedModule message;
              message.requestId = "req:j-" +
                                  std::to_string(++moduleRequestSequence);
              message.packageId = module.package_id();
              message.moduleKind = std::move(kind);
              message.moduleId = module.module_id();
              message.cacheScope = "appRuntime";
              message.dependencies = module.dependencies();
              message.bundle = {module.descriptor().path,
                                module.descriptor().byte_length,
                                module.descriptor().sha256,
                                std::make_shared<const std::vector<std::uint8_t>>(
                                    *module.bytes())};
              message.expectedBootstrap = std::move(bootstrap);
              modules->onLoadVerifiedModule(message);
            };
            static constexpr std::string_view kSharedModules[] = {
                "@quickapp-kit/framework-v1",
                "@quickapp-kit/shared/helper/utils",
                "@quickapp-kit/shared/helper/ajax",
                "@quickapp-kit/shared/helper/apis/example",
                "@quickapp-kit/shared/helper/apis/index"};
            for (const auto moduleId : kSharedModules) {
              const auto descriptorIt = package->modules().find(std::string(moduleId));
              if (descriptorIt == package->modules().end()) continue;
              std::optional<qp::VerifiedModule> shared;
              if (!loader->load_module({descriptorIt->first, std::nullopt},
                                       [&](auto result) {
                    if (result) shared = std::move(result).value();
                  }) || !shared) { finishBase(false); return; }
              send(*shared, "shared", std::nullopt);
            }
            std::optional<qp::VerifiedModule> app;
            if (!loader->load_module({"@quickapp-kit/app", std::nullopt},
                                     [&](auto result) {
                  if (result) app = std::move(result).value();
                }) || !app) { finishBase(false); return; }
            send(*app, "app",
                 ja::BootstrapExpectation{"app", app->module_id(), std::nullopt});
            vm->onAppContext({package->package_id(), "1.0.0", "1", 1,
                              {"system.router", "system.prompt", "system.device",
                               "system.fetch", "system.file"}});
            vm->onVmInitialization({"req:2", "app", std::nullopt});
            // An app module may validly omit lifecycle hooks; page loading does
            // not depend on an app VM instance being retained.
            finishBase(true);
          } catch (...) {
            finishBase(false);
          }
        }).status != qj::PostStatus::Accepted) {
      throw std::runtime_error("QuickJS base composition post failed");
    }
    // ManualPump: 驱动引擎执行上面 post 的 base composition 任务, 直到 future ready。
    while (baseReadyFuture.wait_for(std::chrono::seconds(0)) !=
           std::future_status::ready) {
      engine.pump(16);
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!baseReadyFuture.get()) {
      throw std::runtime_error("QuickJS base composition failed");
    }

    const auto service = [&]() {
      const auto ownerHash = static_cast<std::uint64_t>(
          std::hash<std::thread::id>{}(std::this_thread::get_id()));
      ownerThreadHash.store(ownerHash, std::memory_order_relaxed);
      // ManualPump: 每轮驱动 JS 引擎有限个任务(有限预算, 不饿死 LVGL/触摸)。
      engine.pump(16);
      if (!controller->drain()) throw std::runtime_error("Core surface drain failed");
      static_cast<void>(timerRegistry.service());
      if (!tasks.pump(kOwner, 128).ok())
        throw std::runtime_error("LVGL owner task pump failed");
      if (!mounts->service(kOwner, 128).ok())
        throw std::runtime_error("LVGL mount service failed");
      if (surfaces.service(kOwner, 128).error != qlf::LocalError::kNone)
        throw std::runtime_error("LVGL surface service failed");
      if (!bridgeRaw->service(kOwner, 128).ok())
        throw std::runtime_error("Core mount bridge service failed");
      if (!controller->drain()) throw std::runtime_error("Core result drain failed");
      if (!imageInputMissing) lv_timer_handler();
    };
    const auto waitFor = [&](auto predicate, std::string_view failureMessage) {
      for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        service();
        if (predicate()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      throw std::runtime_error(std::string(failureMessage));
    };

    const std::string rootRoute =
        (showcaseRpk || mountOnlyRpk)
            ? package->entry_route()
            : (binding001 ? "/pages/Binding"
                          : (case002 ? "/pages/Contract"
                                     : (block001 ? "/pages/Contract"
                                                  : (lvglP0 ? "/pages/Home"
                                                            : (imageInput001
                                                                   ? "/pages/ImageInput"
                                                                   : "/pages/Demo")))));
    if (!controller->enqueue(qs::SurfaceRequest(qs::RootSurfaceRequest{
            request("req:j-root"), rootRoute}))) {
      throw std::runtime_error("root navigation enqueue failed");
    }
    const auto surfaceFixture = qc::SurfaceId::parse("srf:1");
    if (!surfaceFixture) throw std::runtime_error("root SurfaceId fixture is invalid");
    auto surfaceId = surfaceFixture.value();
    bool rpkMounted = false;
    if (imageInputMissing) {
      waitFor([&] { return initialResultsRaw->completed(); },
              "missing Image resource failure did not settle");
      const auto failedMountObjects = mounts->liveObjectCount();
      const auto failedCoreNodes = coordinatorRaw->snapshot().committed_nodes;
      if (initialResultsRaw->prepared() || failedMountObjects != 0) {
        throw std::runtime_error("missing Image resource left partial mount objects");
      }
      waitFor([&] {
        const auto snapshot = controller->snapshot();
        return snapshot.records.empty() && snapshot.navigation_stack.empty() &&
               snapshot.pending_correlations == 0 &&
               coordinatorRaw->snapshot().committed_nodes == 0 &&
               eventRouter.handlerCount() == 0;
      }, "missing Image resource cleanup did not settle");
      std::fprintf(stderr,
                   "b3.image_failure rejected=1 partial_objects=0 mount_objects=0 core_nodes_before=%llu resources=stable\n",
                   static_cast<unsigned long long>(failedCoreNodes));
    } else if (mountOnlyRpk) {
      waitFor([&] { return initialResultsRaw->completed(); },
              "mount-only RPK initial mount did not settle");
      if (!initialResultsRaw->prepared()) {
        std::fprintf(stderr,
                     "rpk.lvgl_mount=false package=%s reason=platform_mount_rejected\n",
                     package->package_id().c_str());
      } else {
        waitFor([&] {
          const auto snapshot = controller->snapshot();
          return snapshot.navigation_stack.size() == 1 &&
                 !snapshot.navigation_active && snapshot.records.size() == 1 &&
                 snapshot.records.front().lifecycle == qs::SurfaceLifecycle::kVisible;
        }, "mount-only RPK surface did not become visible");
        const auto rootSnapshot = controller->snapshot();
        if (rootSnapshot.navigation_stack.empty())
          throw std::runtime_error("mount-only RPK navigation did not produce a SurfaceId");
        surfaceId = rootSnapshot.navigation_stack.front();
        rpkMounted = true;
        std::fprintf(stderr, "surface.root.visible=%s\n", surfaceId.wire().c_str());
      }
    } else {
      waitFor([&] {
        const auto snapshot = controller->snapshot();
        return snapshot.navigation_stack.size() == 1 && !snapshot.navigation_active &&
               snapshot.records.size() == 1 &&
               snapshot.records.front().lifecycle == qs::SurfaceLifecycle::kVisible;
      }, "root surface did not become visible");
      const auto rootSnapshot = controller->snapshot();
      if (rootSnapshot.navigation_stack.empty())
        throw std::runtime_error("root navigation did not produce a SurfaceId");
      surfaceId = rootSnapshot.navigation_stack.front();
      rpkMounted = true;
      std::fprintf(stderr, "surface.root.visible=%s\n", surfaceId.wire().c_str());
    }

    std::optional<qc::NodeId> buttonNode;
    std::optional<qc::SurfaceId> s4DetailSurface;
    if (!imageInputMissing && !mountOnlyRpk) {
    const auto handlerId = qc::HandlerId::parse("hdl:1");
    if (!handlerId) throw std::runtime_error("Case 001 handler id invalid");
    buttonNode = eventRouter.nodeForHandler(surfaceId, handlerId.value());
    if (!buttonNode && showcaseRpk) {
      // Showcase pages may put their first interaction inside a keyed block;
      // those handlers receive block-scoped IDs rather than hdl:1.
      for (const auto& handlerWire :
           coreIngress.blockHandlerIdsForSurface(surfaceId)) {
        const auto blockHandler = qc::HandlerId::parse(handlerWire);
        if (!blockHandler) continue;
        buttonNode = eventRouter.nodeForHandler(surfaceId, blockHandler.value());
        if (buttonNode) break;
      }
    }
    // A showcase RPK may intentionally be a read-only surface (for example a
    // long list). The interactive simulator still needs to keep its window
    // alive even when there is no Handler to bind.
    if (!buttonNode && !(kInteractiveSimulator && showcaseRpk)) {
      throw std::runtime_error(imageInput001 ? "B3 input handler node not registered" : "Case 001 button node not registered");
    }
    const auto missingNode = qc::NodeId::parse("node:999");
    if (!missingNode) throw std::runtime_error("negative NodeId is invalid");
    if (!kInteractiveSimulator) {
      const auto invalidHandler = eventRouter.dispatch(qc::event::PlatformInputMessage{
          request("req:p-900"), surfaceId, missingNode.value(),
          qp::EventType::kClick, 900, {}});
      if (invalidHandler ||
          invalidHandler.error().code != qc::RuntimeErrorCode::kHandlerNotFound) {
        throw std::runtime_error("invalid HandlerId input was not rejected");
      }
      std::fprintf(stderr,
                   "negative.invalid_handler rejected=1 error=HANDLER_NOT_FOUND\n");
    }
    auto& clickSink = *new LvglClickToCore(eventRouter);
    auto& inputSink = *new LvglInputToCore(eventRouter);
    auto& switchSink = *new LvglSwitchToCore(eventRouter);
    std::map<std::string, std::map<std::string, void*, std::less<>>, std::less<>>
        showcaseBoundObjects;
    const auto bindShowcaseClickHandlers = [&]() {
      if (!showcaseRpk || controls001) return;
      const auto snapshot = controller->snapshot();
      std::set<std::string, std::less<>> liveSurfaces;
      for (const auto& activeSurface : snapshot.navigation_stack) {
        liveSurfaces.insert(activeSurface.wire());
      }
      for (auto current = showcaseBoundObjects.begin();
           current != showcaseBoundObjects.end();) {
        if (!liveSurfaces.contains(current->first)) {
          current = showcaseBoundObjects.erase(current);
        } else {
          ++current;
        }
      }

      std::size_t installed = 0;
      for (const auto& activeSurface : snapshot.navigation_stack) {
        std::vector<std::string> handlerWires;
        handlerWires.reserve(64);
        for (std::size_t handlerIndex = 1; handlerIndex <= 64; ++handlerIndex) {
          handlerWires.push_back("hdl:" + std::to_string(handlerIndex));
        }
        const auto blockHandlers =
            coreIngress.blockHandlerIdsForSurface(activeSurface);
        handlerWires.insert(handlerWires.end(), blockHandlers.begin(),
                            blockHandlers.end());
        auto& boundObjects = showcaseBoundObjects[activeSurface.wire()];
        for (const auto& handlerWire : handlerWires) {
          const auto handler = qc::HandlerId::parse(handlerWire);
          if (!handler) continue;
          const auto node =
              eventRouter.nodeForHandler(activeSurface, handler.value());
          auto* object = node == std::nullopt
              ? nullptr
              : mounts->nativeObject(activeSurface, node.value());
          if (!node || object == nullptr) continue;
          const auto previous = boundObjects.find(handlerWire);
          if (previous != boundObjects.end() && previous->second == object) {
            continue;
          }
          if (mounts->installClickHandler(activeSurface, node.value(),
                                          &LvglClickToCore::callback,
                                          &clickSink)) {
            boundObjects[handlerWire] = object;
            auto* lvObject = static_cast<lv_obj_t*>(object);
            lv_area_t coordinates{};
            lv_obj_get_coords(lvObject, &coordinates);
            std::fprintf(
                stderr,
                "showcase.click_handler.bound surface=%s handler=%s node=%s "
                "rect=%d,%d,%d,%d clickable=%d hidden=%d\n",
                activeSurface.wire().c_str(), handlerWire.c_str(),
                node->wire().c_str(), static_cast<int>(coordinates.x1),
                static_cast<int>(coordinates.y1), static_cast<int>(coordinates.x2),
                static_cast<int>(coordinates.y2),
                lv_obj_has_flag(lvObject, LV_OBJ_FLAG_CLICKABLE) ? 1 : 0,
                lv_obj_has_flag(lvObject, LV_OBJ_FLAG_HIDDEN) ? 1 : 0);
            ++installed;
          }
        }
      }
      if (installed != 0) {
        std::fprintf(stderr, "showcase.click_handlers installed=%zu surfaces=%zu\n",
                     installed, snapshot.navigation_stack.size());
      }
    };
    [[maybe_unused]] const auto showcaseDetailHandler = [&]()
        -> std::optional<std::pair<qc::HandlerId, qc::NodeId>> {
      const auto pageHandler = qc::HandlerId::parse("hdl:2");
      const auto pageNode = pageHandler
          ? eventRouter.nodeForHandler(surfaceId, pageHandler.value())
          : std::nullopt;
      if (pageHandler && pageNode &&
          mounts->nativeObject(surfaceId, pageNode.value()) != nullptr) {
        return std::make_pair(pageHandler.value(), pageNode.value());
      }
      for (const auto& handlerWire :
           coreIngress.blockHandlerIdsForSurface(surfaceId)) {
        const auto handler = qc::HandlerId::parse(handlerWire);
        if (!handler) continue;
        const auto node = eventRouter.nodeForHandler(surfaceId, handler.value());
        if (node && mounts->nativeObject(surfaceId, node.value()) != nullptr)
          return std::make_pair(handler.value(), node.value());
      }
      return std::nullopt;
    };
    if (controls001) {
      const auto inputHandler = qc::HandlerId::parse("hdl:1");
      const auto switchHandler = qc::HandlerId::parse("hdl:4");
      const auto buttonHandler = qc::HandlerId::parse("hdl:5");
      const auto inputNode = inputHandler
          ? eventRouter.nodeForHandler(surfaceId, inputHandler.value())
          : std::nullopt;
      const auto switchNode = switchHandler
          ? eventRouter.nodeForHandler(surfaceId, switchHandler.value())
          : std::nullopt;
      const auto controlsButtonNode = buttonHandler
          ? eventRouter.nodeForHandler(surfaceId, buttonHandler.value())
          : std::nullopt;
      if (!inputNode || !switchNode || !controlsButtonNode ||
          mounts->nativeObject(surfaceId, inputNode.value()) == nullptr ||
          mounts->nativeObject(surfaceId, switchNode.value()) == nullptr ||
          mounts->nativeObject(surfaceId, controlsButtonNode.value()) == nullptr ||
          !mounts->installInputHandler(surfaceId, inputNode.value(),
                                       &LvglInputToCore::callback, &inputSink) ||
          !mounts->installSwitchHandler(surfaceId, switchNode.value(),
                                        &LvglSwitchToCore::callback, &switchSink) ||
          !mounts->installClickHandler(surfaceId, controlsButtonNode.value(),
                                       &LvglClickToCore::callback, &clickSink)) {
        throw std::runtime_error("B1 controls handlers are incomplete");
      }
      std::fprintf(stderr,
                   "b1.controls.handlers input=%s switch=%s button=%s\n",
                   inputNode->wire().c_str(), switchNode->wire().c_str(),
                   controlsButtonNode->wire().c_str());
    } else if (showcaseRpk) {
      bindShowcaseClickHandlers();
    } else if (imageInput001) {
      if (!mounts->installInputHandler(surfaceId, buttonNode.value(),
                                      &LvglInputToCore::callback, &inputSink)) {
        throw std::runtime_error("LVGL input handler install failed");
      }
      std::fprintf(stderr, "lvgl.input.node=%s\n", buttonNode->wire().c_str());
    } else if (lvglP0) {
      const auto detailHandler = qc::HandlerId::parse("hdl:2");
      const auto detailNode = detailHandler
          ? eventRouter.nodeForHandler(surfaceId, detailHandler.value())
          : std::nullopt;
      if (!detailHandler || !detailNode ||
          !mounts->installClickHandler(surfaceId, buttonNode.value(),
                                       &LvglClickToCore::callback, &clickSink) ||
          !mounts->installClickHandler(surfaceId, detailNode.value(),
                                       &LvglClickToCore::callback, &clickSink)) {
        throw std::runtime_error("LVGL P0 click handlers are incomplete");
      }
      std::fprintf(stderr, "lvgl.p0.nodes update=%s detail=%s\n",
                   buttonNode->wire().c_str(), detailNode->wire().c_str());
    } else {
      if (!mounts->installClickHandler(surfaceId, buttonNode.value(),
                                      &LvglClickToCore::callback, &clickSink)) {
        throw std::runtime_error("LVGL click handler install failed");
      }
      std::fprintf(stderr, "lvgl.button.node=%s\n", buttonNode->wire().c_str());
    }
    lv_timer_handler();
    auto* pageRoot = static_cast<lv_obj_t*>(roots.nativeObject(qls::PageRootHandle{1}));
    auto* hostRoot = pageRoot ? lv_obj_get_child(pageRoot, 0) : nullptr;
    auto* title = hostRoot ? lv_obj_get_child(hostRoot, 0) : nullptr;
    const auto expectedInitialTitle = gallery001
        ? std::string_view{"设备巡检"}
        : lvglP0
        ? std::string_view{"Home"}
        : imageInput001
        ? std::string_view{"Image/Input"}
        : binding001 || case002 || block001
        ? std::string_view{"0"}
        : std::string_view{"欢迎体验 quickapp 开发"};
    const auto rootText = title == nullptr ? std::string_view{} :
        (showcaseRpk ? std::string_view{} : std::string_view(lv_label_get_text(title)));
    const bool rootVisible = title != nullptr &&
        (showcaseRpk ? true : rootText == expectedInitialTitle);
    if (!rootVisible) throw std::runtime_error("Case root text is not visible");

    auto* buttonObject = static_cast<lv_obj_t*>(hostRoot ? lv_obj_get_child(hostRoot, 1) : nullptr);
    auto* inputObject = static_cast<lv_obj_t*>(hostRoot ? lv_obj_get_child(hostRoot, 2) : nullptr);
    if (lvglP0 || showcaseRpk) {
      buttonObject = hostRoot ? lv_obj_get_child(hostRoot, 2) : nullptr;
    }
    if (!imageInput001 && !controls001 && buttonObject == nullptr &&
        !(kInteractiveSimulator && showcaseRpk))
      throw std::runtime_error("Case 001 real LVGL button is absent");
    if (imageInput001 && inputObject == nullptr)
      throw std::runtime_error("B3 real LVGL input is absent");

    // ---- 真机常驻交互循环 ----
    // 沿用模拟器交互路径: 每帧 service() 推进运行时, 并持续绑定 click handler,
    // 以支持 showcase 的 push 导航 (Home -> Goals -> Detail)。
    ESP_LOGI(TAG, "runtime ready, surface=%s, entering device loop",
             surfaceId.wire().c_str());
    ESP_LOGI(TAG, "free heap=%lu PSRAM=%lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    while (true) {
      service();
      bindShowcaseClickHandlers();
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    }  // close: if (!imageInputMissing && !mountOnlyRpk)
  } catch (const std::exception& error) {
    ESP_LOGE(TAG, "device runtime error: %s", error.what());
    // 出错时显示提示并停在这里, 便于录像时排查
    if (lv_screen_active() != nullptr) {
      lv_obj_t* label = lv_label_create(lv_screen_active());
      lv_label_set_text_fmt(label, "Runtime error:\n%s", error.what());
      lv_obj_center(label);
    }
    while (true) {
      lv_timer_handler();
      vTaskDelay(pdMS_TO_TICKS(16));
    }
  }
}

// 运行时线程入口 (pthread)
static void* device_runtime_pthread(void*) {
  device_runtime_main();
  return nullptr;
}

extern "C" void app_main() {
  ESP_LOGI(TAG, "=== QuickApp Device (full runtime) starting ===");
  lv_init();
  device_start_lvgl_tick();

  // 必须用 pthread (而非裸 xTaskCreate) 创建运行时线程:
  // runtime/JS 引擎用了 C++ std 线程原语(std::promise/future/this_thread),
  // 这些依赖 ESP-IDF pthread 层; 裸 FreeRTOS 任务里调用会触发
  // "pthread_self: Failed to find current thread ID" assert。
  // 通过 esp_pthread 配置大栈(100KB, 内部RAM) + 绑定 core0。
  esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
  // ManualPump 单线程: 只此一个大栈, 无需给 JS worker 让内存。
  // 装配帧 ~44KB + QuickJS eval(同线程)调用深度余量 → 给 100KB。
  cfg.stack_size = 100 * 1024;
  cfg.prio = 5;
  cfg.thread_name = "qa_runtime";
  cfg.pin_to_core = 0;
  esp_pthread_set_cfg(&cfg);

  pthread_t tid;
  int rc = pthread_create(&tid, nullptr, device_runtime_pthread, nullptr);
  if (rc != 0) {
    ESP_LOGE(TAG, "qa_runtime pthread_create FAILED rc=%d", rc);
    return;
  }
  pthread_detach(tid);
}
