# 09 构建、测试与 CI

## 1. C++ 构建

- CMake 3.24+，C++20，MSVC 2022（Windows 首期）。
- 目标：

```text
parcel_core
parcel_serialize
parcel_assets
parcel_scene
parcel_stb (stb_image/stb_dxt)
parcel_bc7enc (BC7 encoder)
parcel_meshoptimizer
parcel_import
parcel_entt
parcel_pack
parcel_runtime
parcel_remote
parcel_hbg
parcel_tests
parcel_import_cli
parcel_pack_cli
parcel_bench（可选，PARCEL_BUILD_BENCHMARKS=ON）
```

- 依赖：
  - zstd 1.5.7、lz4 1.8.0（压缩；已内置于 `third_party`）
  - flatbuffers 23.5.26 runtime headers（可选 metadata；已内置）
  - entt 3.13（`parcel_entt`；`PARCEL_ENTT_ROOT`，当前默认探测 `E:/Learn/entt`）
  - assimp（FBX；R03 可选，`PARCEL_ASSIMP_ROOT`）
  - cgltf 1.15（glTF/GLB；R03 已内置 `third_party/cgltf`）
  - stb_image/stb_dxt（PNG/TGA/JPG/HDR + BC1/3/5；R03 已内置 `third_party/stb`）
  - hdrp-bgfx 公共头（`PARCEL_HBG_ROOT` + `PARCEL_HBG_BGFX_INCLUDE_DIR`；R06 可选）
  - meshoptimizer 0.18（Mesh 优化 + glTF EXT_meshopt_compression 解码；R03 已内置 `third_party/meshoptimizer`）
  - bc7enc（BC7 纹理压缩；R03 已内置 `third_party/bc7enc`）
  - Python 3（可选，仅 `--shader-build-*` 调用 hdrp-bgfx `shader_build.py`）
  - 内置轻量 JSON parser/serializer（sidecar/meta/report；无外部依赖）
  - Catch2 v3 amalgamated（测试；已内置）
  - WinHTTP（`parcel_remote` Windows HTTP 后端；系统库 winhttp）
- 依赖管理：vcpkg 或 CMake FetchContent；优先复用 hdrp-bgfx 已有副本，避免版本冲突。
- R00/R01/R02/R03 已内置 Catch2、FlatBuffers、zstd、lz4、stb_image/stb_dxt、bc7enc、meshoptimizer，构建与测试无需网络；
  FBX 需要可选 assimp 目录；glTF/GLB 使用内置 cgltf；BC7 使用内置 bc7enc，BC1/3/5 使用内置 stb_dxt，Mesh 使用内置 meshoptimizer。
- 编译选项：
  - `parcel_core` / `parcel_serialize` / `parcel_scene` / `parcel_assets`：
    `/GR- /EHs-c-` 或 `-fno-exceptions -fno-rtti`。
  - `parcel_entt` / `parcel_hbg` / `parcel_remote`：跟随宿主配置。
  - 警告：`/W4 /permissive- /Zc:preprocessor`（MSVC）。
- 测试 target 可单独启用异常，不污染生产库。
- hdrp-bgfx 是外部活动源；公共头变更后必须强制重建 `parcel_hbg` 与测试（必要时删除
  对应 `parcel_hbg.lib`），否则 Release/ASan 可能因 `RenderScene`/`RenderObject`
  布局不一致出现假失败或崩溃。

## 2. Go 构建

- Go 1.22+；当前验证 Go 1.27。
- `cd server && go build ./...`。
- `parcel-server`、`parcel-admin`。
- SQLite 使用纯 Go `modernc.org/sqlite`，Docker 构建无需 cgo/gcc。
- `go test ./...`；集成测试使用临时 DB/目录。
- Vue：`cd server/web && npm ci && npm run generate:api && npm run build`；产物写入
  `server/internal/webui/dist` 并 embed 到 Go binary。
- Docker/nginx/备份：`deploy/docker-compose.yml`、`deploy/nginx/nginx.conf`、
  `deploy/backup.sh`；本机当前未安装 Docker，镜像启动 smoke 待补。

### WinHTTP loopback smoke

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run-winhttp-smoke.ps1
```

脚本用临时 Python 静态 server 提供 Bundle，`parcel_tests.exe "[.winhttp]"` 通过
`WinHttpTransport` 完成真实 HTTP 下载、SHA-256 校验和缓存安装，并验证
session/connection 句柄复用统计；该用例默认隐藏，不计入主测试数。真实 CDN/TLS/吞吐仍需宿主环境。

### 发布打包

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\package-release.ps1 `
  -Version 0.1.0 -BuildDir build/windows-release
```

输出 `dist/parcel-<version>/` 与 zip，包含 CLI、静态库、头文件、文档、schema、deploy
文件、服务端二进制（若已构建）和管理端静态文件。

## 3. 测试分层

### 单元测试（C++）

当前已验证（hbg-disabled Debug，主套件 272 cases / 10252 assertions；性能用例另计 1 case / 5022 assertions）：Result/Error、日志、
路径、SHA-256/CRC32、文件、时间、线程池、Operation/Scheduler、Guid128、二进制读写、
压缩、PRCA/PRCL/PRCM、Manifest 零拷贝查找、FlatBuffers 可选 metadata 测试、golden
files、资产/组件/Scene schema、sidecar JSON/组件映射、内容 hash 去重、导入/打包（含 JSON/TOML 配置、collector include/exclude glob、archive raw bundle）、Runtime 同步/异步加载、archive/non-archive raw `OpenRawFile` range 读取、
Handle 生命周期、Runtime Scene 实例化/卸载、AnimationClip/AnimationPlayer、SkinComponent/蒙皮顶点、MorphComponent/morph target、Light cookie/IES 导入与采样、BuiltinCatalog 编解码/包名校验/回退、Bundle 缓存校验、Sandbox footprint/覆盖安装清理、操作耗时/下载吞吐、下载/续传/CDN/Token/灰度/Manifest 缓存、
Signed URL 缓存/403 重签、截断/随机 fuzz、安全边界拒绝、并发/卸载/重建压力。
- 结果/错误、路径、hash、线程池、Operation 调度。
- PRCA/PRCL/PRCM 编解码、边界、版本。
- FlatBuffers/BlobTable 往返与 R02 版本化 asset metadata。
- 标准资产（Texture/Mesh/Material/Shader/Raw/Text/Settings/Scene/IES/AnimationClip）往返；Material 覆盖 TextureRef sampler，Mesh 覆盖蒙皮顶点与 morph targets。
- 标准组件 codec、Scene PRCS v1、entity/parent/引用校验。
- 下载/Range/重试/多 CDN/Token/暂停取消/按需修复、Manifest 缓存/灰度/回滚。
- `.parcel.json` 解析、schema/错误上下文、设置路径与 camera override。
- `.parcelmeta` 往返、GUID 派生、ImportDB 原子读写与确定性。
- Import pipeline：PNG/TGA/HDR 解码、RGBA8/BGRA8/RGBA16F/RGBA32F、BC1/BC3/BC5/BC7、
  mip、meshoptimizer、纹理跨源内容+设置复用、增量 skip/reimport、glTF/GLB 变体/场景选择/
  EXT_meshopt_compression、Shader manifest 打包、
  shader build 调用/失败/强制重导、缺失二进制错误、并行 jobs。
- mock `ISceneLoader` 两阶段加载契约。
- Handle/Provider/BundleLoader 引用计数。
- Pack：配置解析、collector/地址/标签、共享 Bundle、Raw 多文件拒绝、缺失/循环依赖、
  确定性输出、`build_db.json` Bundle/文件级增量复用、输出 hash/catalog 校验、Manifest 加载。

直接运行：

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug --output-on-failure
```

启用 FBX（需要本地 assimp 构建/安装目录）：

```powershell
cmake --preset windows-debug -DPARCEL_ASSIMP_ROOT=E:/Learn/assimp
cmake --build --preset windows-debug --target parcel_import_cli
parcel_import --project <project> --source <assets> --output <cooked> --db <import.db>
```

打包 cooked 目录：

```powershell
cmake --build --preset windows-debug --target parcel_pack_cli
parcel_pack --project <project> --package game --version 1.0.0 --target windows `
  --cooked <cooked> --output <build-root> --config <pack-config.json|toml>
```

输出位于 `<build-root>/<target>/<package>/<version>/`。`--config` 可省略；JSON/TOML
字段语义见 `schemas/parcel.pack.schema.json` 和 `docs/04-import-pack.md`。

生成 golden 文件（仅在格式变更后需要）：

```powershell
cmake --build --preset windows-debug --target parcel_golden_generate
& build/windows-debug/bin/Debug/parcel_golden_generate.exe cpp/tests/golden
```

构建 fuzz 目标（独立 PRCA/PRCL/PRCM + 字符串/配置 + 合并目标）：

```powershell
cmake --preset windows-debug -DPARCEL_BUILD_FUZZER=ON
cmake --build --preset windows-debug --target parcel_fuzz parcel_fuzz_asset parcel_fuzz_bundle parcel_fuzz_manifest parcel_fuzz_sidecar
```

MSVC 目标为 standalone 文件输入；种子在 `cpp/fuzz/corpus/`，CTest 会执行
`parcel_fuzz_*_smoke`。Windows ClangCL 使用 `scripts/run-fuzz.ps1` 构建 libFuzzer 目标并执行 30 分钟长跑；Linux/Clang 下启用 libFuzzer + ASan。

Windows ClangCL libFuzzer（`-Target parcel_fuzz_sidecar` 可跑 `.parcel.json`/`.parcelmeta`/remote config）：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run-fuzz.ps1 -Minutes 30
```

构建性能基准：

```powershell
cmake -S . -B build/windows-bench -DPARCEL_BUILD_BENCHMARKS=ON
cmake --build build/windows-bench --config Release --target parcel_bench
& build/windows-bench/bin/Release/parcel_bench.exe
```

基线和回归阈值见 `docs/13-benchmarks.md`。

### 集成测试（C++）

- Runtime：`parcel_runtime` 的 Builtin/Sandbox/Custom FS、PRCL/PRCA 加载、
  Provider 去重、Raw/BundleFile、全量/子资源、LoadingLock/UnloadAll、缓存 sidecar 损坏。
- EnTT 场景加载；SceneAsset → registry → 组件；回滚/自定义组件/卸载。
- `parcel_hbg` 适配层：SceneRenderBridge（节点层级/动画 channel/morph weights）、AssetMaterializer（含 texture sampler 映射）、SettingsBridge、Shader 物化；GPU 设备端到端 smoke 需宿主 hdrp-bgfx。
- assimp FBX 动画/蒙皮：真实 `animation_with_skeleton.fbx` 导入 AnimationClipAsset、AnimationPlayerComponent、MeshAsset.skinVertices 与 SkinComponent；`morph_animation.fbx` 导入 morph targets 与 AnimationPath::Weights 通道（配置 assimp 时）。
- glTF/GLB：`triangle.gltf`/`triangle.glb` 导入 mesh/material/scene/skin/morph/TRS+Weights 动画，texture sampler wrap/filter 映射，embedded bufferView/data URI 图片导入纹理子资产，EXT_meshopt_compression 解码，默认/命名/索引 scene 选择，sidecar 覆盖与完整 Import 流水线（纹理 canonical 复用、二次导入 skip）。
- 跨 Bundle 引用、子资源、全部资源、RawFile。

### 故障注入

- HTTP：MockTransport 覆盖 401/403/404/500、慢响应、传输失败、Range；WinHTTP loopback
  smoke 通过 `scripts/run-winhttp-smoke.ps1`（隐藏 `[.winhttp]` 用例），真实 CDN/TLS 待宿主。
- 文件：截断、hash 不匹配、权限拒绝；磁盘满 commit ENOSPC 注入（重试/失败/恢复）。
- 缓存：sidecar 缺失、数据文件损坏、临时文件残留、批量校验删除损坏项。
- Manifest：版本不支持、表越界、字符串表损坏。

### Fuzz

- PRCA/PRCL/PRCM 解析器。
- BlobTable/metadata 边界与 FlatBuffers 可选路径。
- 目标：无崩溃、无 UB、错误可返回。
- `parcel_fuzz` / `parcel_fuzz_asset` / `parcel_fuzz_bundle` / `parcel_fuzz_manifest` / `parcel_fuzz_sidecar`：
  MSVC 下为 standalone 文件输入；Windows ClangCL 使用 `scripts/run-fuzz.ps1` 的
  libFuzzer；Linux/Clang 下启用 libFuzzer + ASan。
- 2026-11-21：Windows ClangCL libFuzzer 合并目标运行 1801s、222,311,162 次、
  0 artifacts/0 crash；覆盖 PRCA/PRCL/PRCM。
- 2026-11-21：`parcel_fuzz_sidecar` 运行 1801s、12,304,085 次、0 artifacts；覆盖
  `.parcel.json`、`.parcelmeta`、remote config 文本解析。

### 并发/稳定性

- 随机 Handle 释放、依赖链卸载、LoadingLock/UnloadAll/Destroy 压力。
- 并发 `LoadAsset`/`LoadRawFile`/`UnloadUnusedAssets`；服务端并发上传/下载/config。
- 长跑脚本：`scripts/soak-r09.ps1 -Minutes 60`，重复执行 `[stress]`
  （Runtime 并发/卸载/重建 + 跨 Downloader 缓存写）；默认每轮随机种子并记录日志。
  已执行 60 分钟：1103 轮、每轮 4 个 stress cases/1092 assertions、0 失败；总耗时 01:00:01.87。
- ASan（MSVC）与 UBSan（ClangCL）全量测试通过；TSan 本机不可用。
- UBSan：`scripts/run-ubsan.ps1` 使用 VS 环境 + Ninja + clang-cl，静态 CRT 匹配 UBSan runtime；
  Clang 17 / MSVC STL 14.44 下关闭 `alignment` 检查以规避 `wmemcmp` 平台误报。

### 安全

- PRCM/PRCA/PRCL 表计数、路径、块大小/压缩块边界；见 `cpp/tests/unit/test_security.cpp`。
- 服务端 CSRF/权限/限流/路径穿越/上传校验/签名 URL；见 Go API 测试。
- 清单和残留风险见 `docs/12-security-review.md`。

### 性能

- `parcel_bench` 覆盖 10k/100k Manifest、8 MiB Bundle LZ4、10k object PRCA、10k cache 扫描。
- 已记录 Release 基线和回归阈值：`docs/13-benchmarks.md`。
- 下载并发已覆盖（多 worker、跨 Downloader 同一 Bundle 串行化）；缓存批量校验支持 1-32 worker，已覆盖
  Metadata/Size/Content 级别、同名内容损坏删除、缺失 Bundle 跳过和跨 Bundle 继续扫描。
- 缓存批量校验：10k/1 KiB Content 校验串行 14.8s、8 worker 9.1s（`parcel_bench`，Release）。
- 主线程帧预算：`[performance]` 用例创建 1k 独立 Raw Bundle，按 4ms 预算分帧卸载，
  记录 p95 与最慢耗时（Debug 实测 p95 1.6ms，偶发 OS 调度离群点 5.2ms）；标准 CTest 单独运行
  `parcel_performance`，ASan/UBSan 排除该用例。

## 4. 确定性测试

- 同一输入 + 同一工具版本：Import/Pack 输出字节级一致。
- 固定时间戳/排序/GUID。
- Shader/纹理编码器版本写入报告；比较 hash。

## 5. CI

- Windows MSVC：Debug/Release、no-exceptions/no-rtti、ASan（已验证；Debug 主套件
  284 cases / 15730 assertions，性能 1 case / 5022 assertions；Release 282 / 13072；
  ASan 282 / 13072）。
- Windows ClangCL：UBSan（已验证，225 cases / 6162 assertions，0 runtime error；该构建启用 hbg 测试）。
- assimp 启用：227 cases / 8820 assertions（含真实 `animation_with_skeleton.fbx` 动画/蒙皮与 `morph_animation.fbx` morph 导入 smoke）。
- Clang/Linux：ASan/UBSan/TSan；CMake 选项 `PARCEL_ENABLE_ASAN/UBSAN/TSAN`（Windows ClangCL 走 UBSan 脚本），
  TSan 不能与 ASan/UBSan 同时启用。
- Go：`go vet`、`go test`；CI 执行 `go test -race`（本机无 cgo/gcc 时跳过）。
- Vue：typecheck、build。
- 文档/契约：OpenAPI lint、schema 版本检查。
- 构建产物：parcel_import、parcel_pack、parcel-server、parcel-admin（前两者由
  `parcel_import_cli` / `parcel_pack_cli` 生成）。
- 失败阻断：格式/解析/核心测试必须通过。

## 6. 目录约定

```text
cpp/tests/unit/
cpp/tests/integration/
cpp/fuzz/               # libFuzzer / standalone fuzz target
cpp/tests/golden/       # golden PRCA/PRCL/PRCM + generator
schemas/                # JSON Schema（scene/meta/pack）
server/internal/..._test.go
server/web/src/...
```






















































































