# R00 基础骨架

## 目标

建立独立 Parcel 项目、CMake 构建、核心基础设施和测试框架，为后续模块提供稳定底座。

## 前置依赖

无。

## 范围

- 独立目录 `E:\Learn\Parcel`。
- C++20 + CMake 3.24+ + MSVC 2022。
- `parcel_core`：Result/Error、日志、hash、路径、时间、线程池。
- Operation/Scheduler 框架。
- 平台文件接口（Windows 实现）。
- 单元测试框架和 CI 骨架。

## 任务清单

### 仓库与构建

- [x] 创建 `cpp/include/parcel/core`、`cpp/src/core`、`cpp/tests`。
- [x] 根 `CMakeLists.txt`、`CMakePresets.json`（Debug/Release/ASan）。
- [x] `parcel_core` 静态库 target。
- [x] `parcel_tests` target（Catch2/GoogleTest）。
- [x] 统一 warning：`/W4 /permissive- /Zc:preprocessor`。
- [x] Debug/Release 均通过；`parcel_core` 支持 no-exceptions/no-rtti 构建。
- [x] `.clang-format`/`.editorconfig`（可选）。

### Result/Error

- [x] `ErrorCode` 枚举：InvalidArgument、NotFound、Io、Corrupt、HashMismatch、Timeout、Canceled、Unsupported、Internal、Http 等。
- [x] `Error { code, message, package, bundle, asset, path, httpStatus }`。
- [x] `Result<T>` / `Result<void>`，支持 move、错误传播、无异常。
- [x] `PARCEL_TRY`、`PARCEL_ASSERT` 宏。
- [x] 单元测试：成功/失败/移动/void。

### 日志

- [x] `ILogger` 接口；默认 stderr logger；可注入。
- [x] 级别：Trace/Debug/Info/Warn/Error/Fatal。
- [x] 线程安全；避免热路径格式化。
- [x] ILogger 可注入；spdlog 适配为可选，首版未实现。

### Hash/Path/Time

- [x] SHA-256、CRC32；xxHash 可选，首版未实现。
- [x] 文件/内存 hash API。
- [x] 路径规范化、拼接、扩展名、相对路径安全检查。
- [x] 禁止 `..`/绝对路径穿越的校验工具。
- [x] `std::chrono` 时间工具；可注入 fake clock（测试）。

### 线程池

- [x] `IThreadPool` 接口：`Post(Task)`、`PostDelayed`（可选）。
- [x] `ThreadPool` 实现：N worker、任务队列、停止/排空。
- [x] 任务取消标记（`std::stop_token` 或自定义 token）。
- [x] worker 线程名称/ID（调试）。
- [x] 测试：并发、停止、异常隔离（测试 target 可开异常）。

### 平台文件

- [x] `IPlatformFile`：OpenRead/OpenWrite/Size/ReadAt/Write/Delete/Rename/Exists/CreateDirs/Stat/FreeSpace。
- [x] Windows 实现：`std::filesystem` + error_code；长路径/Unicode。
- [x] 只读/原子 rename 语义。
- [x] 测试：临时目录、长路径、权限错误（可跳过）。

### Operation/Scheduler

- [x] `OperationBase`：Start/Tick/Cancel/AddChild/OnComplete/Status/Progress/Error。
- [x] `OperationScheduler`：优先级、时间片、每帧预算、稳定排序。
- [x] 子操作环检测；取消传播；dispose 一次。
- [x] 完成回调帧末执行；允许回调重入。
- [x] `WaitForCompletion`（仅工具）与死锁说明。
- [x] 测试：优先级、时间片、取消、子操作失败、回调重入。

## 验收标准

- Windows Debug/Release 构建通过。
- `parcel_tests` 全绿。
- no-exceptions/no-rtti 构建通过。
- 调度器在 4ms 预算下可中断/恢复，无死循环。
- 文件/路径/hash 基础 API 有测试覆盖。
- 代码无 Hobi/hdrp-bgfx 依赖。

## 实施状态

- 已完成（2026-11-20）。
- Debug/Release、no-exceptions/no-RTTI、ASan 测试通过。
- 另验证了 exceptions/RTTI 开启的 `parcel_core` 构建，线程池异常隔离测试通过。
- 可选未实现：xxHash、spdlog 适配。

## 估算

约 1 周（1 人）。
