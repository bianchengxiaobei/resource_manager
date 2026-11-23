# 01 总体架构

## 1. 模块分层

```text
Game / Hobi / hdrp-bgfx
  ├─ parcel_hbg            hdrp-bgfx 适配层（可选）
  ├─ parcel_entt           EnTT Scene Loader（可选）
  ├─ parcel_remote         Version/Manifest cache / CDN / Auth / Gray
  └─ parcel_runtime        AssetSystem / ResourcePackage / FileSystem / Cache / Downloader
       ├─ parcel_assets     Texture/Mesh/Material/Shader/Scene/Settings
       ├─ parcel_scene      ECS-neutral SceneData / 组件 schema
       ├─ parcel_serialize  PRCA/PRCL/PRCM + 版本化 metadata + BlobTable
       └─ parcel_core       Result/Error/Log/Hash/ThreadPool/Operation
```

离线工具：

```text
parcel_import  →  FBX/assimp、纹理、Shader、sidecar → Cooked Assets
parcel_pack    →  Collector/依赖/Bundle/Manifest/Report → Build Output
```

服务端：

```text
parcel-server（Go）→ 控制面 + 数据面 + Vue Web UI
parcel-admin（Go） → 上传/发布/回滚/灰度 CLI
```

## 2. 依赖方向

```text
parcel_core ← parcel_serialize ← parcel_scene ← parcel_assets
parcel_core ← parcel_runtime
parcel_entt → parcel_assets / parcel_scene / parcel_runtime + entt
parcel_hbg  → parcel_runtime / parcel_assets + hdrp-bgfx 公共头
parcel_remote → parcel_runtime / parcel_assets / parcel_serialize
parcel_import → parcel_assets / parcel_serialize / parcel_scene
                + stb_image + optional assimp
parcel_pack   → parcel_assets / parcel_serialize
```

约束：

- `parcel_core` 不依赖任何第三方引擎。
- `parcel_hbg` 不修改 hdrp-bgfx；只使用公共头文件与 `gfx::ResourceManager`。
- `parcel_entt` 不把 EnTT 类型泄漏到 `parcel_scene` 公共 API。
- 工具（import/pack）可以依赖第三方库；运行时核心保持最小依赖。

## 3. 线程与所有权

### 主线程

- 持有 `AssetSystem`、`ResourcePackage`、`ResourceManager`、`FileSystemHost`。
- 执行 Operation 调度、Provider/BundleLoader 状态迁移、EnTT 组件创建、回调。
- 每帧调用 `AssetSystem::Update()`，默认时间片 4ms。

### Worker

- 只做文件 IO、解压、hash、HTTP 下载。
- 不调用引擎对象 API，不修改管理器状态。
- 结果通过任务完成队列交回主线程 `Integrate`。

### 所有权

- `ObjectGraph` 拥有资产对象；`BundleHandle` 拥有 ObjectGraph。
- `HandleState` 共享句柄；最后一个拷贝或 `Release()` 释放 Provider 引用。
- `Provider` 强持有主 Bundle + 依赖 Bundle 的 `BundleLoader`。
- `BundleLoader` 弱引用 Provider；`ResourceManager` 弱引用两者，避免循环。
- 适配层负责把 Parcel 对象转成 hdrp-bgfx/EnTT 对象，不跨分配器释放。

## 4. 构建目标

```text
parcel_core
parcel_serialize
parcel_assets
parcel_scene
parcel_entt
parcel_import
parcel_pack
parcel_runtime
parcel_remote
parcel_hbg
parcel_tests

parcel_import_cli
parcel_pack_cli
```

命名约定：`parcel_import` / `parcel_pack` 是库 target；`parcel_import_cli` /
`parcel_pack_cli` 是可执行 target，输出文件名为 `parcel_import` / `parcel_pack`。

- CMake 3.24+，C++20。
- `parcel_core`：`/GR- /EHs-c-`（MSVC）或 `-fno-exceptions -fno-rtti`。
- `parcel_entt` / `parcel_hbg`：跟随 EnTT/hdrp-bgfx 的编译配置。
- 测试 target 可单独启用异常。
- 默认静态库，避免 DLL 边界 ABI 问题。

## 5. 目录建议

```text
cpp/include/parcel/
  core/ async/ serialize/ assets/ scene/ import/ pack/ runtime/ remote/
cpp/src/...
cpp/src/import/cli_main.cpp
cpp/src/pack/cli_main.cpp
cpp/src/runtime/
cpp/tests/
server/cmd/parcel-server/
server/cmd/parcel-admin/
server/internal/
server/web/
api/openapi.yaml
docs/
requirements/
```

## 6. 主循环接入

- Parcel 不绑定 Hobi 的 `EngineManager`。
- 宿主每帧调用 `parcel::AssetSystem::Update()`；也可通过 `IUpdateSink` 适配。
- `AssetSystem::Shutdown()` 必须停止下载、取消 Operation、释放句柄、卸载 Package。
- EnTT/hdrp-bgfx 集成由 `parcel_entt` / `parcel_hbg` 适配层完成。



