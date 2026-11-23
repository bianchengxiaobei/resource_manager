# R06 `parcel_hbg` 与 EnTT 场景适配

状态：主体完成（2026-11-22）。EnTT loader、ResourcePackage Scene 生命周期、CPU 侧 hbg 映射（含节点层级、Light cookie/IES 采样）、Settings/Shader 物化和 GPU 上传 API 已实现；ForwardRenderer/真实 bgfx 设备端到端验证待补。

## 目标

在不修改 hdrp-bgfx 的前提下，把 Parcel 资产/Scene 接入 hdrp-bgfx：

- `parcel_entt`：SceneAsset → entt::registry。
- `parcel_hbg`：Parcel 资产 → hdrp-bgfx RenderScene/GPU 资源/ShaderLibrary。

## 前置依赖

R02、R05。

## 范围

- `parcel_entt` 库。
- `parcel_hbg` 库（只使用 hdrp-bgfx 公共头）。
- Texture/Mesh/Material/Shader/Settings 适配。
- SceneRenderBridge。
- 适配层测试。

## 任务清单

### parcel_entt

- [x] `SceneInstance`（entity 列表，可选 registry 指针）。
- [x] `ISceneLoader` 接口。
- [x] `EnttSceneLoader`：创建 entity、parent/transform、组件 emplace、实体引用解析。
- [x] 组件注册表：TypeId ↔ entt 组件类型。
- [x] `ENTT_NOEXCEPTION`/无 RTTI 构建验证。
- [x] 卸载：destroy entities；清理映射。
- [x] Runtime 集成：`ResourcePackage::LoadScene` 将 SceneAsset 实例化到 registry；
  最后一次 `UnloadScene`/Handle 释放时销毁实体并释放依赖 Bundle。
- [x] 测试：SceneAsset → registry → 组件字段。

### AssetMaterializer（hbg）

- [x] TextureAsset → `gfx::TextureDesc` + `ResourceManager::createTexture`（mip/BC7/HDR）。
- [x] MeshAsset → `hbg::asset::Mesh`。
- [x] MaterialAsset → `hbg::asset::Material` + GUID→imageIndex + sampler 映射。
- [x] MaterialKey 计算（含 sampler 状态）。
- [x] 错误：缺失纹理/格式不支持/尺寸非法。

### ParcelMaterialSystem

- [x] 使用 `hbg::gfx` 公共 API 创建 `GpuMesh`/`GpuMaterial`（需宿主链接 hbg 目标）。
- [x] 支持 mips/BC7/HDR（绕过单 mip `TextureLoader`）。
- [x] 按 imageIndex+sRGB+sampler 缓存并应用 wrap/filter flags。
- [x] 材质 uniform/texture 绑定与 hdrp-bgfx MaterialSystem 对齐。
- [x] 释放/延迟销毁。
- [x] 与 hdrp-bgfx 原 MaterialSystem 的字段映射测试（纯映射/变体测试）。

### ShaderLibraryBootstrap

- [x] ShaderLibraryAsset 内容 hash。
- [x] 物化到 `<cache>/parcel/<package>/shader_library/<hash>/`。
- [x] manifest 相对路径/bin 文件写入。
- [x] 调用未修改的 `ShaderLibrary::init(resources, root)`（泛型 Initialize，真实设备测试待补）。
- [x] 版本切换旧目录清理。
- [x] 错误：manifest 缺失/bin 缺失/路径非法。
- [ ] 可选硬链接/复制优化。

### SceneRenderBridge

- [x] 从 entt registry 读取 Transform/Camera/Light/MeshRenderer/Decal/Probe/LocalFog。
- [x] 构建 `RenderScene`：meshes/materials/images/objects/nodes/animations/lights/cameras/decals/probes/localFog。
- [x] `RenderScene.nodes` 按 SceneData 顺序生成层级，并填写 object/camera/light `nodeIndex`。
- [x] AnimationClipAsset → `RenderScene.animations`，SceneEntityId channel 映射为 node index。
- [x] SkinComponent → `RenderScene.skins`/object `skinIndex`，joint SceneEntityId 映射为 node index。
- [x] MeshAsset.morphTargets/MorphComponent → object `morphWeights` 与 node weights；Weights channel 映射。
- [x] 脏标记/帧更新策略。
- [x] 材质 imageIndex/GUID 映射。
- [x] Light cookie CPU 采样：未压缩 RGBA8/BGRA8/RGBA16F/RGBA32F；Legacy/Point Octahedral。
- [x] Light IES 采样：IesProfileAsset；spot `iesSamples`/`iesCut`，point cookie legacy/native 调制。
- [x] 未修改 hdrp-bgfx `ForwardRenderer` 可消费（CPU 结构边界已对齐，端到端待验证）。
- [x] 测试：最小场景 → RenderScene 字段正确；节点顺序/层级；动画 channel/蒙皮/morph 映射；cookie/IES 采样与拒绝路径。

### SettingsBridge

- [x] PipelineSettingsAsset → PipelineSettings。
- [x] FrameSettingsAsset → FrameSettings。
- [x] PostProcessProfileAsset → PostProcessProfile。
- [x] ShadowSettingsAsset → ShadowSettings。
- [x] VolumetricFogProfileAsset → VolumetricFogSettings。
- [x] 字段映射/默认值/校验。

### 边界

- [x] 不修改 hdrp-bgfx 源码。
- [x] 适配层跟随 hdrp-bgfx 编译配置（异常/RTTI）。
- [x] 不跨分配器释放；GPU 资源交 ResourceManager。
- [x] 记录 hdrp-bgfx 公共 API 版本/commit。

## 验收标准

- 最小 Scene 可通过 EnTT 加载并由 `parcel_hbg` 生成 `RenderScene`。
- hdrp-bgfx `ForwardRenderer` 能渲染该场景（不改 hdrp-bgfx）。
- BC7/HDR/mip 纹理正确上传；材质字段正确。
- ShaderLibraryAsset 物化后可被 `ShaderLibrary::init` 加载。
- Decal/Probe/LocalFog/Camera/Light 字段映射正确；未压缩 Light cookie 与 IES 采样正确。
- 无 hdrp-bgfx 源码修改。

当前验收状态：Runtime `LoadScene`/`UnloadScene` → EnTT registry、CPU 侧最小 Scene → EnTT → RenderScene、材质/纹理/网格/sampler 映射、
SettingsBridge、Shader 物化与纯字段测试已通过。真实 bgfx 设备上的
BC7/HDR/mip 上传、`ForwardRenderer` 完整渲染和真实 `ShaderLibrary::init` 仍需
在宿主 hdrp-bgfx 构建中做端到端 smoke；`ParcelMaterialSystem` 的 GPU 资源释放
代码已实现但未在无设备单测中执行。

## 估算

约 4-5 周（1-2 人）。













