# Parcel

Parcel 是独立于 Hobi 引擎的资源管线项目，覆盖：

- 资源导入（Import）
- 资源打包（Pack）
- 运行时加载（Runtime）
- 热更新与分发（Update / CDN / Gray）
- Go 生产服务端 + Vue 管理端

Hobi 当前工程是临时的；本项目不修改 Hobi，也不依赖 Hobi 现有代码。未来渲染器以
`E:\Learn\hdrp-bgfx` 为目标，Parcel 通过独立适配层 `parcel_hbg` 接入，不修改
hdrp-bgfx。

## 关键决策

| 项 | 决策 |
|---|---|
| 语言 | C++20（Parcel）、Go（服务端/CLI）、TypeScript/Vue（管理端） |
| 构建 | 全部 CMake；Go modules；Vite |
| 命名空间 | `parcel` |
| Scene | 直接按 EnTT/ECS 设计，`parcel_entt` 独立适配 target |
| 资产 payload | 版本化二进制元数据 + BlobTable 大块数据（FlatBuffers 可选） |
| 容器/清单 | Parcel 自定义二进制（PRCA / PRCL / PRCM） |
| 首批导入 | FBX/assimp + glTF/GLB（cgltf） |
| 纹理 | 跟随 hdrp-bgfx：RGBA8/BGRA8/RGBA16F/RGBA32F/BC1/BC3/BC5/BC7；ASTC 后置 |
| Shader | hdrp-bgfx manifest + bin，打包成单个 ShaderLibraryAsset |
| 渲染器接入 | 只做 `parcel_hbg` 适配层，不修改 hdrp-bgfx |
| 物理/音频/脚本 | 先数据 schema，运行时后置 |
| 服务端 | Go 单节点生产 MVP，SQLite + 本地文件，JWT/HMAC，灰度/回滚 |
| 管理端 | Vue 3 + TypeScript + Vite + Element Plus |
| 安全 | 资产格式做 SHA-256 完整性校验，不做资产加密；传输层由服务端 HMAC 签名 URL 保护 |
| 异常/RTTI | `parcel_core` 禁用；适配层跟随宿主 |

## 仓库结构

```text
parcel/
  cpp/                    # C++20 库与工具
    include/parcel/
    src/
      import/
      pack/
      runtime/
      entt/
      hbg/
    tests/
  server/                 # Go 服务端与管理 CLI
    cmd/parcel-server/
    cmd/parcel-admin/
    internal/
    web/                  # Vue 3 管理端
    migrations/
    Dockerfile
  docs/                   # 设计文档
  requirements/           # 可执行需求拆分
  schemas/                # .parcel.json 等 JSON Schema
  examples/               # 全链路示例
  scripts/                # 发布/运维脚本
  CMakeLists.txt
  CMakePresets.json
```

## 文档入口

- [docs/00-overview.md](docs/00-overview.md)：目标、范围、术语
- [docs/01-architecture.md](docs/01-architecture.md)：模块、线程、构建
- [docs/02-format.md](docs/02-format.md)：PRCA/PRCL/PRCM 格式
- [docs/03-assets-scene.md](docs/03-assets-scene.md)：资产与 Scene/ECS schema
- [docs/04-import-pack.md](docs/04-import-pack.md)：导入与打包
- [docs/05-runtime.md](docs/05-runtime.md)：运行时加载与生命周期
- [docs/06-update-remote.md](docs/06-update-remote.md)：热更、CDN、鉴权、灰度
- [docs/07-hbg-adapter.md](docs/07-hbg-adapter.md)：hdrp-bgfx 适配层
- [docs/08-server-web.md](docs/08-server-web.md)：Go 服务端与 Vue 管理端
- [docs/09-build-test.md](docs/09-build-test.md)：构建、测试、CI
- [docs/10-milestones.md](docs/10-milestones.md)：里程碑与验收
- [docs/11-decisions-risks.md](docs/11-decisions-risks.md)：决策记录与风险
- [docs/12-security-review.md](docs/12-security-review.md)：安全审查清单
- [docs/13-benchmarks.md](docs/13-benchmarks.md)：性能基线与回归阈值
- [docs/14-versioning-compat.md](docs/14-versioning-compat.md)：API/错误码/版本兼容
- [schemas/parcel.scene.schema.json](schemas/parcel.scene.schema.json)：Scene sidecar JSON Schema
- [schemas/parcel.meta.schema.json](schemas/parcel.meta.schema.json)：`.parcelmeta` JSON Schema
- [schemas/parcel.pack.schema.json](schemas/parcel.pack.schema.json)：`parcel_pack` 配置 JSON Schema
- [examples/hello-package/README.md](examples/hello-package/README.md)：纹理/文本全链路示例

## 需求文档

- [requirements/README.md](requirements/README.md)：执行顺序与状态
- [requirements/R00-foundation.md](requirements/R00-foundation.md)
- [requirements/R01-serialization-format.md](requirements/R01-serialization-format.md)
- [requirements/R02-asset-scene-schema.md](requirements/R02-asset-scene-schema.md)
- [requirements/R03-import-pipeline.md](requirements/R03-import-pipeline.md)
- [requirements/R04-pack-pipeline.md](requirements/R04-pack-pipeline.md)
- [requirements/R05-runtime-core.md](requirements/R05-runtime-core.md)
- [requirements/R06-hbg-adapter.md](requirements/R06-hbg-adapter.md)
- [requirements/R07-update-remote.md](requirements/R07-update-remote.md)
- [requirements/R08-server-web.md](requirements/R08-server-web.md)
- [requirements/R09-hardening-release.md](requirements/R09-hardening-release.md)

## 构建与测试

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Release：

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

服务端与管理端：

```powershell
cd server
go test ./...
go build ./cmd/parcel-server
go build ./cmd/parcel-admin
cd web
npm ci
npm run generate:api
npm run build
```

- `parcel_core` / `parcel_serialize` / `parcel_scene` / `parcel_assets` /
  `parcel_import` / `parcel_pack` / `parcel_runtime` / `parcel_remote` 默认使用
  `/EHs-c- /GR-`（MSVC）或
  `-fno-exceptions -fno-rtti`。
- 测试使用仓库内置的 Catch2 v3 amalgamated、FlatBuffers、zstd、lz4、stb_image/stb_dxt、bc7enc、meshoptimizer，
  无需网络依赖；EnTT 通过 `PARCEL_ENTT_ROOT`（默认探测 `E:/Learn/entt`），
  hdrp-bgfx 适配通过 `PARCEL_HBG_ROOT` / `PARCEL_HBG_BGFX_INCLUDE_DIR` 可选启用；
  可选 assimp 用于 FBX；glTF/GLB 使用内置 cgltf。
- 可选：`windows-asan` 构建 AddressSanitizer 版本；UBSan 使用
  `scripts/run-ubsan.ps1`（VS + Ninja + clang-cl，静态 CRT，Clang 17 已验证）；
  CMake 另支持 `PARCEL_ENABLE_UBSAN` / `PARCEL_ENABLE_TSAN`（Clang/GCC，TSan 不可与 ASan 同开）。
- 可选：`-DPARCEL_BUILD_FUZZER=ON` 构建 `parcel_fuzz` / `parcel_fuzz_asset` /
  `parcel_fuzz_bundle` / `parcel_fuzz_manifest` / `parcel_fuzz_sidecar`；CTest 执行 golden smoke。
  Windows ClangCL 可用 `-DPARCEL_FUZZ_LIBFUZZER=ON` + `scripts/run-fuzz.ps1` 运行
  libFuzzer；Linux/Clang 使用 ASan + libFuzzer。
- 可选：`-DPARCEL_BUILD_BENCHMARKS=ON` 构建 `parcel_bench`，基线见
  `docs/13-benchmarks.md`。
- WinHTTP loopback smoke：`scripts/run-winhttp-smoke.ps1`（Python 静态 server + 连接句柄复用统计 +
  `[.winhttp]` 真实 HTTP 下载，默认不进 CTest）。
- 发布包：`scripts/package-release.ps1` 收集 CLI/静态库/头文件/文档/服务端产物。
- Golden 文件由 `parcel_golden_generate` 生成，位于 `cpp/tests/golden`。

## 当前状态

R00 基础骨架、R01 序列化格式、R02 资产与 Scene/ECS Schema、R03 导入管线、
R04 打包管线、R05 Runtime 核心、R06 EnTT/hbg 适配主体、R07 热更/下载/远程客户端主体、R08 Go 服务端/Vue 管理端主体已完成：

- `parcel_serialize`：BinaryReader/Writer、Guid128、PRCA/PRCL/PRCM 读写。
- 版本化 object metadata + BlobTable、ExternalRefTable、SubAssetTable。
- PRCL BlockTable/TOC、None/LZ4/Zstd 压缩、按块逻辑读取。
- PRCM ManifestView 零拷贝查找、hash/version 文件与 `.hash` sidecar。
- `parcel_scene`：SceneData PRCS v1、12 个标准组件 codec、注册表、
  `ISceneLoader` 契约、实体/引用校验。
- `parcel_assets`：Texture/Mesh/Material/Shader/Raw/Text/Settings/Scene
  资产类型、`TypeRegistry`、PRCA 编解码、跨资产引用收集。
- `.parcel.json` 解析与 `schemas/parcel.scene.schema.json`。
- `parcel_import`：`.parcelmeta`（`schemas/parcel.meta.schema.json`）、ImportDB、
  增量判断、PNG/TGA/JPG/HDR 纹理与 BC1/3/5/7、纹理跨源内容+设置复用、hdrp-bgfx Shader manifest
  与可选 `shader_build.py` 调用、IES profile、text/raw。
- 可选 assimp FBX：mesh/material/node/camera/light、骨骼权重/SkinComponent、morph targets/Weights 动画、sidecar 设置/camera/light override（含 light cookie/IES）、
  decals/probes/localFogVolumes 组件、同源内容去重。
- glTF/GLB（内置 cgltf）：mesh/material/node/camera/KHR_lights_punctual、skin、morph targets、
  TRS/Weights 动画、sidecar 覆盖；external buffer/image、embedded image 子资产、
  texture sampler（wrap/filter）、`KHR_materials_variants`、`EXT_meshopt_compression`、
  scene 选择与纹理 canonical 复用。
- `parcel_import` CLI：`--project --source --output --db --jobs`，输出
  `<guid>.prca` + `import_report.json`；支持并行导入和失败回滚。
- `parcel_pack`：collector（main/static/depend、目录/显式列表、地址/标签/共享策略）、
  ExternalRefTable 依赖分析、共享/Shader/Raw Bundle、PRCL/PRCM、`build_report.json`、
  `build_db.json`、`builtin_catalog.bin`、增量复用和 `--clean`。
- `parcel_pack` CLI：`--project --package --version --target --cooked --output --config`；
  输出 `<target>/<package>/<version>/bundles`、`manifest.bytes/hash`、version 文件。
- `parcel_runtime`：AssetSystem/ResourcePackage、Builtin/Sandbox/Custom FileSystem、
  PRCL BundleLoader、Handle/Provider 去重与引用计数、PRCA 多对象/子资源解码、
  IObjectResolver、sync/async 加载、LoadScene/UnloadScene 场景生命周期、Unload/UnloadAll、
  BuiltinCatalog、LoadingLock、缓存 sidecar、诊断/泄漏报告和全量 API 错误表面。
- `parcel_runtime` 支持按 `bundles/<file_name>` 查找 pack 输出，校验
  size/SHA-256/CRC32；Sandbox 缓存损坏会删除，并在配置下载器时按需重下修复。
- `parcel_entt`：`EnttSceneLoader`、组件注册表、两阶段实例化/回滚/卸载；
  Runtime SceneHandle 在最后一个共享句柄释放时销毁实体并释放依赖 Bundle。
- `parcel_hbg`：Texture/Mesh/Material Materializer、ParcelMaterialSystem（含
  per-texture sampler 上传）、ShaderLibraryBootstrap、SceneRenderBridge（含节点层级、
  object/camera/light nodeIndex、动画 channel/蒙皮/morph weights 映射、Light cookie/IES 采样）、
  SettingsBridge；真实 ForwardRenderer 与 bgfx 设备端到端 smoke 待宿主验证。
- `parcel_remote`：RemoteConfig/版本决策/灰度、Manifest hash+bytes 缓存与原子切换、
  多 CDN/Token、`SignedUrlProvider` 服务端签名 URL 缓存/403 重签、`ManifestCache`、`RemoteManifestService`、Windows WinHTTP 后端。
- `parcel_runtime` 下载：并发/每帧请求配额、无进度 watchdog、Range 续传、退避重试、暂停/取消/RetryFailed、
  原子安装、按需下载/缓存修复、OldVersions 清理。
- `parcel-server`：Go 标准库路由 + 纯 Go SQLite、内容寻址 blob、bcrypt Session+CSRF、
  设备 JWT、HMAC 签名 URL、包/版本/渠道/灰度/回滚/审计、Prometheus 指标、限流、
  OpenAPI 与 Docker/nginx/backup。
- `parcel-admin`：`upload/publish/rollback/gray/backup`。
- `parcel-admin-web`：Vue 3 + TypeScript + Vite + Element Plus，OpenAPI 生成 TS 类型。
- 边界/版本/CRC/SHA-256 校验、golden files、截断/随机 fuzz 测试；
  ASan/UBSan/no-exceptions 构建通过。

R09 稳固化基线已完成：独立 PRCA/PRCL/PRCM + sidecar/config fuzz 目标、安全边界测试、并发/重入
压力测试、分帧卸载预算、MSVC ASan、ClangCL UBSan、Release 性能基准和发布打包脚本已加入。剩余项包括 
TSan 实机验证、真实 CDN/TLS/吞吐、Docker/nginx smoke、Playwright、
大规模下载基准、R06 的真实 bgfx ForwardRenderer/ShaderLibrary 端到端验证、
R07 的真实 CDN/TLS、R08 的 Docker/真实 CDN 网络 smoke/Playwright。













































