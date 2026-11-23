# R02 资产与 Scene/ECS Schema

## 目标

定义并实现 Parcel 的资产类型、序列化器、Scene/ECS 组件 schema、`.parcel.json` 和 EnTT 加载所需的数据契约。

## 前置依赖

R01。

## 范围

- AssetObject/ITypeSerializer/TypeRegistry。
- Texture/Mesh/Material/Shader/Scene/Settings 资产。
- 组件 schema。
- EnTT 加载器接口（不依赖 entt 的核心部分）。
- schema 版本与校验。

## 任务清单

### 类型系统

- [x] `AssetObject`、`TypeId`、`ITypeSerializer`。
- [x] `TypeRegistry`：按名称/ID 注册、查找、序列化器版本。
- [x] `IObjectResolver`：AssetGuid → AssetObject。
- [x] `CollectReferences` 接口。
- [x] 类型注册宏/模板辅助。

### 资产类型

- [x] `TextureAsset`：格式/ mip / sRGB / BlobTable。
- [x] `MeshAsset`：MeshVertex/SkinVertex/MorphTarget/SubMesh/Bounds/字段标志。
- [x] `MaterialAsset`：hdrp-bgfx Material 字段 + TextureRef(AssetGuid + sampler)。
- [x] `ShaderLibraryAsset`：backend/pass/variant/binary 索引。
- [x] `RawFileAsset` / `TextAsset`。
- [x] Settings：Pipeline/Frame/PostProcess/Shadow/VolumetricFog。
- [x] SceneAsset：entities/components/settings 引用。
- [x] `IesProfileAsset`：TILT=NONE IES 角度/candela 表，`LightComponent.ies` 引用。
- [x] `AnimationClipAsset`：SceneEntityId 目标、TRS/Weights channel、插值与切线。

### Scene/组件

- [x] `SceneEntityId`、`SceneEntityRecord`、`ComponentChunk`。
- [x] 标准组件：Transform/Camera/Light/MeshRenderer/Decal/ReflectionProbe/LocalFogVolume。
- [x] AudioSource/AudioListener。
- [x] RigidBody/Collider。
- [x] ScriptComponent + PropertyBag。
- [x] AnimationPlayerComponent：AnimationClipAsset 引用集合。
- [x] SkinComponent：joint SceneEntityId + inverse bind matrices。
- [x] MorphComponent：per-object morph weights（有限浮点，数量上限）。
- [x] 组件 schema（版本化二进制 meta；`ComponentCodec<T>`）。
- [x] 实体引用 `SceneEntityId` 的解析规则。
- [x] 组件版本与迁移策略。

### Sidecar

- [x] `.parcel.json` schema（JSON Schema 文件）。
- [x] 字段兼容 hdrp-bgfx `.hdrp.json`。
- [x] 加载/校验/错误上下文。
- [x] 设置资产引用。

### EnTT 对接契约

- [x] `SceneInstance`（entity 列表 + 可选 registry 指针）。
- [x] `ISceneLoader` 接口。
- [x] `parcel_entt::EnttSceneLoader` 具体实现（R06 完成；R02 冻结 `ISceneLoader` 契约与组件注册表）。
- [x] 两阶段加载流程文档。
- [x] `parcel_hbg::SceneRenderBridge` 所需字段映射表。

### 测试

- [x] 每个资产类型序列化/反序列化。
- [x] 组件 schema 默认值/边界值。
- [x] Scene 两阶段加载（entity/component/entity ref）。
- [x] 未知 TypeId/版本拒绝。
- [x] `.parcel.json` 解析/错误。

## 验收标准

- 所有资产/组件 schema 有版本和文档。
- 序列化往返一致；未知版本明确失败。
- Scene 可被 EnTT 加载器消费（用 mock 或 entt 测试）。
- `.parcel.json` schema 与 hdrp-bgfx 字段对齐。
- 组件字段覆盖 hdrp-bgfx 的 Camera/Light/Material/RenderObject/Decal/Probe/LocalFog。
- 资产/组件 API 不依赖 Hobi/hdrp-bgfx/entt。

## 估算

约 2-3 周（1-2 人）。

## 实施状态

R02 已完成。已实现 `parcel_scene`、`parcel_assets`、标准组件 codec、Scene PRCS v1、`.parcel.json` 解析与 schema、资产/组件/Scene 测试。`parcel_entt::EnttSceneLoader` 的具体实现属于 R06；R02 冻结了 `ISceneLoader` 契约并用 mock 覆盖两阶段加载。`parcel_hbg::SceneRenderBridge` 字段映射表见 `docs/03-assets-scene.md` 第 12.7 节。

已知边界：`TransformComponent.parent` 是可选镜像引用，Scene 记录中的 `parent` 仍是权威层级；组件迁移当前严格拒绝未知 schema_version，后续版本迁移器在 R09 之前补充。






