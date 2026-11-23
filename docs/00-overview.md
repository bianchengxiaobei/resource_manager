# 00 目标、范围与术语

## 1. 目标

Parcel 为 Hobi 引擎（以及未来的 hdrp-bgfx 渲染器）提供完整的资源管线：

1. 将 FBX/glTF/纹理/材质/场景等源资源导入为可序列化的 cooked asset。
2. 将 cooked asset 按规则打成自定义 Bundle，并生成 Manifest/版本/hash。
3. 在运行时按 Manifest 加载、引用计数、卸载、热更新。
4. 通过 Go 服务端提供版本、灰度、鉴权、签名 URL 与分发能力。
5. 通过 Vue 管理端完成包/版本/灰度/回滚/审计操作。

## 2. 范围

### 首期包含

- Windows + MSVC + CMake。
- C++20 运行时与工具，Go 单节点服务端，Vue 管理端。
- 自定义容器 `PRCL`、资产文件 `PRCA`、清单 `PRCM`。
- 版本化元数据 + BlobTable 大块数据（FlatBuffers 可选）。
- FBX/assimp 与 glTF/GLB 导入，PNG/TGA/JPG/HDR 纹理，RGBA8/BGRA8/RGBA16F/32F 与
  hdrp-bgfx ShaderLibrary 打包；BC1/BC3/BC5/BC7 编码已接入。
- EnTT/ECS 场景加载；`parcel_entt` 独立 target。
- `parcel_hbg` 适配层；不修改 hdrp-bgfx。
- 完整 Runtime API 表面；依赖宿主能力的接口返回 `Unsupported`。
- 完整热更链路：版本、Manifest、下载、续传、重试、校验、修复。
- CDN 多 URL、Token、签名 URL、灰度、回滚。
- 物理/音频/脚本组件的数据 schema；运行时不绑定具体后端。

### 首期不包含

- Android/iOS/主机/Web 构建。
- ASTC 纹理编码（等 hdrp-bgfx 的 TextureFormat 支持后接入）。
- 资源加密、签名、防篡改（只做 SHA-256）。
- 物理/音频/脚本的具体运行时实现。
- Editor UI（后续里程碑）。
- 多节点/HA 服务端（单节点 MVP 之后演进）。
- Hobi 工程改造；Hobi 仅作为历史参考。

## 3. 术语

| 术语 | 含义 |
|---|---|
| Cooked Asset | 导入后、可被运行时直接反序列化的资产 |
| AssetGuid | 128-bit 资产稳定标识 |
| SceneEntityId | Scene 内部的 64-bit 实体稳定标识 |
| PRCA | Parcel Asset File，单个资产文件格式 |
| PRCL | Parcel Bundle，资源包容器 |
| PRCM | Parcel Manifest，包清单 |
| BlobTable | 大块二进制表（顶点/索引/像素/Shader） |
| parcel_hbg | hdrp-bgfx 适配层 |
| parcel_entt | EnTT/ECS 场景加载适配层 |
| Sidecar | 与源资源同目录的 `.parcelmeta` / `.parcel.json` |

## 4. 约束

- `parcel_core` 不依赖 Hobi、hdrp-bgfx、EnTT。
- `parcel_core` 禁用异常和 RTTI；公开 API 使用 `Result<T>` + `ErrorCode`。
- 适配层（`parcel_entt`、`parcel_hbg`）可以跟随宿主的编译配置。
- 所有格式必须版本化、边界检查、可 fuzz。
- 主线程持有管理器状态；worker 只做 IO/解压/hash。
- 资产/场景/设置 schema 变更必须升版本并写迁移说明。
- hdrp-bgfx 不修改；适配层只使用公共头文件和 `gfx` 公共 API。

## 5. 成功标准

- 一个 FBX/glTF + 纹理 + `.parcel.json` 场景能通过 CLI 导入、打包，并被 Runtime 加载。
- Scene 能加载到 EnTT registry，并通过 `parcel_hbg` 生成 hdrp-bgfx `RenderScene`。
- 具备首次安装、增量更新、回滚、断点续传、损坏修复的端到端能力。
- Go 服务端可通过 Vue 管理端完成包/版本/灰度/回滚操作。
- 目标规模下无句柄/Provider/BundleLoader 泄漏，错误可定位。




