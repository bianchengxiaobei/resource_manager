# 07 `parcel_hbg`：hdrp-bgfx 适配层

## 1. 约束

- **不修改 hdrp-bgfx**。
- 只 include hdrp-bgfx 公共头文件，使用公共 `hbg::gfx` / `hbg::asset` /
  `hbg::render` / `hbg::shader` API。
- 如果公共 API 不够，优先在适配层复制最小逻辑，不回改 hdrp-bgfx。
- 适配层可以跟随 hdrp-bgfx 的编译配置（例如异常/nlohmann）。

## 2. 结构

```text
parcel_hbg
  ├─ AssetMaterializer
  │    ├─ TextureAsset → gfx::TextureDesc + ResourceManager::createTexture
  │    ├─ MeshAsset → hbg::asset::Mesh
  │    ├─ MaterialAsset → hbg::asset::Material + imageIndex/sampler 映射
  │    └─ ShaderLibraryAsset → 内容寻址缓存目录（manifest + bin）
  ├─ ParcelMaterialSystem
  │    └─ 用 hbg::gfx 公共 API 构建 GpuMesh/GpuMaterial
  ├─ SceneRenderBridge
  │    └─ entt::registry → hbg::render::RenderScene
  ├─ ShaderLibraryBootstrap
  │    └─ ShaderLibrary::init(resources, materializedRoot)
  └─ SettingsBridge
       └─ Parcel 设置资产 → hdrp-bgfx settings 结构
```

## 3. 纹理

- Parcel `TextureAsset` 包含完整 mip、BC1/BC3/BC5/BC7/HDR 格式。
- 适配层直接构造 `gfx::TextureDesc`：
  - `width/height`
  - `mipCount`
  - `format`（BC1/BC3/BC5/BC7/RGBA16F/32F/…）
  - `data` 指向全部 mip 连续数据
  - `dataSize`
  - `flags`（sRGB、clamp 等）
- 调用 `ResourceManager::createTexture`，不经过 `TextureLoader` 的单 mip 转换。
- BC1/BC3/BC5/BC7 数据直接上传；HDR 保持 float。
- 旧 `MaterialSystem` 的 `TextureLoader::createTexture` 只支持单 mip；适配层不使用它。

## 4. Mesh / Material

- `MeshAsset` → `hbg::asset::Mesh`（顶点/索引/morph 字段一一对应；蒙皮权重按
  hdrp-bgfx 当前 `SkinVertex`/`SkinInfluence` 流打包）。
- `MaterialAsset` → `hbg::asset::Material`：
  - TextureRef 的 AssetGuid 映射到 `RenderScene.images` 的 imageIndex。
  - TextureRef 的 sampler 映射到 `hbg::asset::TextureSampler`；MaterialKey 包含
    sampler 状态。
- 适配层实现 `ParcelMaterialSystem`：
  - 使用 `hbg::gfx::ResourceManager` 创建 `GpuMesh`/`GpuMaterial`。
  - `GpuMesh` / `GpuMaterial` / `MaterialSamplers` 是 hdrp-bgfx 公共结构。
  - 支持 mips/BC/HDR；按 imageIndex + sRGB + sampler 状态缓存并上传纹理，
    descriptor flags 使用对应 wrap/filter 状态；不修改原 `MaterialSystem`。
- 后续如果 hdrp-bgfx 的 `MaterialSystem` 支持 mips，可切换到原实现。

## 5. ShaderLibraryAsset

`hbg::shader::ShaderLibrary` 只从磁盘目录加载 `manifest.json` + bin。适配层：

1. 计算 ShaderLibraryAsset 内容 hash。
2. 物化到：
   ```text
   <cache>/parcel/<package>/shader_library/<hash>/
     manifest.json
     build/dxbc/*.bin
     build/spirv/*.bin
   ```
3. 调用 `ShaderLibrary::init(resources, root)`。
4. 版本切换后旧目录延迟清理（无引用/无当前场景）。
5. 可选优化：内置包中的 shader 文件用硬链接/复制，避免重复解压。

限制：

- 每次新版本都要物化；磁盘占用可控但存在。
- 如果未来 hdrp-bgfx 支持从内存加载，删除物化逻辑。

## 6. SceneRenderBridge

```cpp
class SceneRenderBridge {
public:
    Result<void> Update(const entt::registry& registry,
                        hbg::render::RenderScene& scene);
};
```

- 从 ECS 读取 Transform/Camera/Light/MeshRenderer/Decal/Probe/LocalFog。
- 构建：
  - `scene.meshes` / `materials` / `images`（来自 Parcel 资产）
  - `scene.objects`（RenderObject）
  - `scene.lights`
  - `scene.cameras`
  - `scene.decals` / `probes` / `localFogVolumes`
- `RenderScene` 只在脏标记/帧更新时重建；避免每帧全量复制。
- hdrp-bgfx `ForwardRenderer` 继续使用未修改的 `RenderScene`/`DrawList`。

## 7. SettingsBridge

- `PipelineSettingsAsset` → `hbg::render::PipelineSettings`
- `FrameSettingsAsset` → `hbg::render::FrameSettings`
- `PostProcessProfileAsset` → `PostProcessProfile`
- `ShadowSettingsAsset` → `ShadowSettings`
- `VolumetricFogProfileAsset` → `VolumetricFogSettings`

- 适配层只做字段映射；不做 capability 推断，调用 hdrp-bgfx 的 `sanitize`。
- 相机级 FrameSettings 覆盖由适配层合并。

## 8. 资源生命周期

- Parcel 资产对象由 Runtime Handle/Provider 持有。
- 适配层创建的 `gfx::Texture`/`Buffer`/`Program` 由 `ResourceManager` 管理。
- Scene 卸载时：
  - `UnloadScene` 在最后一个共享 Handle 释放时调用 `ISceneLoader::Destroy`，
    释放 EnTT entities；
  - 释放适配层 GPU 资源（交给 `ResourceManager` 延迟销毁）；
  - 释放 Parcel Handle 及其依赖 Bundle。
- Shader 物化目录在 ShaderLibrary 释放后清理。

## 9. 已知限制

- Shader 必须物化到磁盘；首次启动有额外 IO。
- `ParcelMaterialSystem` 复制了 hdrp-bgfx 部分材质构建逻辑；hdrp-bgfx 改字段时需同步适配层。
- hdrp-bgfx 的 `ImageAsset` 无 mip 字段，适配层绕过其 `TextureLoader`。
- `RenderScene.nodes` 由 Parcel Scene 的实体层级生成，按 `SceneEntityComponent.scene_order`
  （即 `SceneData.entities` 顺序）稳定排序；`RenderObject`/`Camera`/`Light.nodeIndex`
  指向对应节点。
- `RenderScene.animations` 来自 `AnimationPlayerComponent` 引用的 AnimationClipAsset；
  channel 的 SceneEntityId 在桥接时映射为 node index，包含 FBX 的
  translation/rotation/scale 与 morph `Weights` 通道。桥接层只生成
  `RenderScene.animations`；播放时间/循环/混合由宿主动画系统驱动。
- `RenderScene.skins` 来自 `SkinComponent`：joint SceneEntityId 映射为 node index，
  inverse bind matrices 原样转换；MeshAsset.skinVertices 在适配层打包为 hdrp-bgfx
  的 `SkinVertex.influenceOffset/Count` + `skinInfluences` 流，供 CPU 蒙皮使用。
  skinned object 使用 mesh bounds，且不作为 static shadow caster。
- MeshAsset.morphTargets/morphWeights 原样转换到 `hbg::asset::Mesh`；
  `MorphComponent`（可选）覆盖 per-object 初始权重，否则使用 mesh 默认权重，
  再缺省为 0。桥接层同步 `RenderScene.nodes[].weights/baseWeights` 与
  `RenderObject.morphWeights`，使宿主的 Weights 动画通道可直接驱动畸形。
- 适配层依赖 hdrp-bgfx 公共结构稳定；不保证二进制 ABI，只保证源码级集成。

## 10. 给 hdrp-bgfx 的建议（不要求）

- 增加内存版 ShaderLibrary（manifest + binary span）。
- 增加 `ImageAsset` mip 链/压缩格式字段。
- 将 MaterialSystem 的纹理创建策略抽象成接口。
- 这些改动可以简化适配层，但首期不做。

## 11. R06 实现状态

参考 hdrp-bgfx 公共头版本：commit `9157bccf1d3792c736338135bb606d0f784f64ee`。

已实现：

- `parcel_entt`：`EnttSceneLoader`、`EnttComponentRegistry`、
  `SceneEntityComponent`/`ParentEntityComponent`、两阶段实例化、失败回滚、
  仅销毁自身 entity；`SceneInstance.context` 保存 loader 私有 ownership。
- `parcel_hbg::AssetMaterializer`：Texture（完整 mip 链、BC1/BC3/BC5/BC7/HDR、sRGB）、
  Mesh、Material（GUID→imageIndex、sampler→TextureSampler）、MaterialKey。
- `parcel_hbg::ParcelMaterialSystem`：用 `hbg::gfx::ResourceManager` 创建
  `TextureDesc`/`GpuMesh`/`GpuMaterial`/samplers，按 imageIndex+sRGB+sampler
  缓存纹理并写入 wrap/filter flags，填充与 hdrp-bgfx `MaterialSystem` 对齐的
  uniforms/variant，提供显式 `Shutdown`。
- `parcel_hbg::ShaderLibraryBootstrap`：内容 hash、内容寻址物化、
  `manifest.json`/bin 写入、旧版本清理；`Initialize` 模板直接调用未修改的
  `ShaderLibrary::init(resources, root)`。
- `parcel_hbg::SceneRenderBridge`：从 registry 构建 `RenderScene`，处理
  Transform 层级、Camera/Light/MeshRenderer/Decal/Probe/LocalFog、
  `RenderScene.nodes` 与 object/camera/light `nodeIndex`、AnimationClipAsset → `RenderScene.animations`、
  SkinComponent → `RenderScene.skins`/object `skinIndex`、MorphComponent/mesh morph targets → object `morphWeights` 与 node weights、image/material 去重、
  dirty 重建；Camera 的 clearColor/cullingMask/priority
  与 Light cookie imageIndex 通过 bridge sidecar 暴露；未压缩 cookie 纹理
  （RGBA8/BGRA8/RGBA16F/RGBA32F）生成 Legacy/Point Octahedral 采样与 native 数据；
  IesProfileAsset 生成 spot `iesSamples`/`iesCut`，point 时调制 cookie legacy/native。
- `parcel_hbg::SettingsBridge`：Pipeline/Frame/PostProcess/Shadow/VolumetricFog
  字段映射与基础校验。
- 测试：`test_entt.cpp`、`test_hbg.cpp`，包含最小 scene → RenderScene、节点顺序/层级、动画 channel 映射、
  material/transform/settings/shader 物化、回滚/卸载、错误路径。当前全量
  223 cases / 6095 assertions，Debug/Release/no-exceptions/UBSan 通过；ASan 由 R09 构建覆盖。

构建：

```powershell
cmake --preset windows-debug -DPARCEL_ENTT_ROOT=E:/Learn/entt `
  -DPARCEL_HBG_ROOT=E:/Learn/hdrp-bgfx `
  -DPARCEL_HBG_BGFX_INCLUDE_DIR=E:/Learn/hdrp-bgfx/build/vs2022/_deps/bgfx-src/include
```

未完成/限制：

- 无 bgfx 设备时无法做真实 `ForwardRenderer` 渲染 smoke；BC7/HDR/mip
  上传和真实 `ShaderLibrary::init` 仍需宿主 hdrp-bgfx 端到端验证。
- `parcel_hbg` 的 CPU 结构测试不链接 hbg 图形库；使用
  `ParcelMaterialSystem` 的宿主需链接 hdrp-bgfx target（若 target 已存在，
  CMake 会自动链接 `hbg_app`/`hbg_gfx`）。
- `SceneRenderBridge.images` 是 hdrp-bgfx 旧 `ImageAsset` 兼容视图，仅保留
  base mip；ParcelMaterialSystem 应使用 `MaterializedTexture` 的完整 mip/BC7/HDR。
- 同一 image 被不同 sampler 引用时会按状态重复上传纹理副本；默认
  Clamp/Linear 且 sRGB 用途一致时复用基础上传，decal 兼容视图继续使用基础上传。
- Light cookie 的 GUID→imageIndex 与采样数据已填充；当前仅支持未压缩
  RGBA8/BGRA8/RGBA16F/RGBA32F，BC 压缩 cookie 返回 `Unsupported`。IES 通过
  IesProfileAsset 提供 spot 8 样本/`iesCut`，point 光源以 profile 调制 cookie
  legacy/native 数据；IES 不再作为贴图 imageIndex 暴露。仅支持 `TILT=NONE`，其他 tilt 表在导入阶段拒绝。
- Shader manifest 以 asset pass 名作为 hdrp-bgfx manifest key；非标准 pass 名
  会物化但不会被原 `ShaderLibrary` 读取。
- 可选硬链接/复制优化延后。



























