# 03 资产与 Scene/ECS Schema

## 1. 基础类型

```cpp
using AssetGuid = std::array<std::byte, 16>;   // 128-bit
using TypeId    = uint32_t;

class AssetObject {
public:
    virtual ~AssetObject() = default;
    virtual TypeId Type() const noexcept = 0;
};

class ITypeSerializer {
public:
    virtual TypeId Type() const noexcept = 0;
    virtual std::string_view TypeName() const noexcept = 0;
    virtual uint32_t Version() const noexcept = 0;
    virtual Result<std::shared_ptr<AssetObject>> Create() const = 0;
    virtual Result<void> Serialize(const AssetObject&, BinaryWriter&,
                                   IAssetWriteContext&) const = 0;
    virtual Result<std::shared_ptr<AssetObject>> Deserialize(
        BinaryReader&, IAssetReadContext&) const = 0;
    virtual Result<void> CollectReferences(const AssetObject&,
                                           std::vector<AssetGuid>& out) const = 0;
};
```

- `IAssetWriteContext::AddBlob` 分配 blob_id；`IAssetReadContext::Blob` 读取 blob。
- `IAssetReadContext` 继承 `IObjectResolver`，用于跨资产引用。

- 每个资产类型有稳定 type name + 版本。
- 跨资产引用用 `AssetGuid`，禁止裸指针。
- `CollectReferences` 供导入器/打包器生成 ExternalRefTable。

## 2. TextureAsset

```cpp
enum class TextureFormat : uint8_t {
    RGBA8, BGRA8, RGBA16F, RGBA32F,
    BC1, BC3, BC5, BC7,
    // ASTC 等 hdrp-bgfx 支持后追加
};

struct TextureMip {
    uint32_t width;
    uint32_t height;
    uint32_t rowPitch;
    std::vector<std::byte> data;
};

class TextureAsset : public AssetObject {
public:
    TextureFormat format;
    bool sRGB;
    uint32_t mipCount;
    std::vector<TextureMip> mips;
};
```

- 与 `hbg::gfx::TextureFormat` 对齐。
- 首期：RGBA8/BGRA8/RGBA16F/RGBA32F/BC1/BC3/BC5/BC7；ASTC 后置。
- importer 支持 RGBA8/BGRA8/RGBA16F/RGBA32F，以及 BC1/BC3/BC5/BC7；
  BC7 使用 bc7enc，BC1/BC3/BC5 使用 stb_dxt，HDR 默认 RGBA32F。
- mip 完整存储；运行时通过 `gfx::TextureDesc` 的 `mipCount` 创建。

## 3. MeshAsset

对齐 `hbg::asset::Mesh`：

```cpp
struct MeshVertex {
    Vec3 position;
    Vec3 normal{0, 0, 1};
    Vec4 tangent{0, 0, 0, 1};
    Vec2 texcoord0;
    Vec2 texcoord1;
    Vec4 color{1, 1, 1, 1};
};

struct SubMesh {
    uint32_t indexOffset;
    uint32_t indexCount;
    uint32_t materialIndex;
    Bounds bounds;
};

struct SkinVertex {
    std::array<uint16_t, 4> joints;
    std::array<float, 4> weights;
};

struct MorphTarget {
    std::string name;
    std::vector<Vec3> positionDeltas;
    std::vector<Vec3> normalDeltas;
    std::vector<Vec4> tangentDeltas;
    bool hasNormals;
    bool hasTangents;
};

class MeshAsset : public AssetObject {
public:
    std::string name;
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SkinVertex> skinVertices;
    std::vector<MorphTarget> morphTargets;
    std::vector<float> morphWeights;
    std::vector<SubMesh> subMeshes;
    Bounds bounds;
    bool hasNormals;
    bool hasTangents;
    bool hasUv0;
    bool hasUv1;
    bool hasColors;
};
```

- 坐标系：右手 Y-up、CCW 正面。
- FBX/assimp 导入，meshoptimizer 优化；有蒙皮影响或 morph target 的 mesh 只重排索引，
  不合并顶点，避免丢失不同骨骼权重或变形增量。
- 顶点/索引/蒙皮权重/morph delta 放 BlobTable；SubMesh/元数据放版本化二进制 payload。
- MeshAsset serializer_version 3；`skinVertices` 为空或与 `vertices` 等长；
  morph target 数为 0..256，总 delta 顶点数有上限；全部 position delta 必须与顶点等长，
  normal/tangent delta 按 flag 与顶点等长；`morphWeights` 为空或与 target 数一致且有限。

## 4. MaterialAsset

对齐 `hbg::asset::Material`，纹理引用改为 `AssetGuid`：

```cpp
enum class TextureWrap { Repeat, Clamp, Mirror };
enum class TextureFilter { Linear, Nearest };
enum class TextureMipFilter { Linear, Nearest };

struct TextureSampler {
    TextureWrap wrap_s = TextureWrap::Clamp;
    TextureWrap wrap_t = TextureWrap::Clamp;
    TextureFilter min_filter = TextureFilter::Linear;
    TextureFilter mag_filter = TextureFilter::Linear;
    TextureMipFilter mip_filter = TextureMipFilter::Linear;
};

struct TextureRef {
    AssetGuid texture;
    int uv_set = 0;
    TextureSampler sampler;
};

class MaterialAsset : public AssetObject {
public:
    std::string name;
    MaterialType type;           // Lit / Unlit
    AlphaMode alphaMode;         // Opaque / Mask / Blend
    bool doubleSided;
    uint32_t features;
    int renderQueue;

    Vec4 baseColorFactor;
    float metallic;
    float roughness;
    Vec3 emissiveFactor;
    float emissiveStrength;
    float normalScale;
    float occlusionStrength;
    float alphaCutoff;

    TextureRef baseColorTexture;
    TextureRef metallicRoughnessTexture;
    TextureRef normalTexture;
    TextureRef occlusionTexture;
    TextureRef emissiveTexture;

    std::string diffusionProfile;
    bool maskMapPacked;

    float clearCoat;
    float clearCoatRoughness;
    float clearCoatNormalScale;
    float transmission;
    float thickness;
    float ior;
    float iridescence;
    float iridescenceIor;
    float iridescenceThickness;
    float anisotropy;
    float anisotropyRotation;
    float subsurfaceMask;
};
```

- `TextureRef.sampler` 保存每个纹理引用的 wrap/filter 状态；默认 Clamp/Linear。
  glTF 导入器对无 sampler 的纹理引用使用 glTF 默认 Repeat/Linear。
- `MaterialKey` 由适配层根据 GUID→运行时 imageIndex 与 sampler 状态计算。
- `MaterialAsset` serializer_version 为 `2`（TextureRef 携带 sampler）。
- 纹理引用必须出现在 ExternalRefTable 中。

## 5. ShaderLibraryAsset

```text
ShaderLibraryAsset
  version
  backends:
    dxbc / spirv:
      pass entries:
        forward / forwardAdvanced / forwardInstanced / ...
        vertex / fragments / bufferFragments / compute / bufferCompute
  BlobTable: shader binaries
  materialVariant / pipelineKey metadata
```

- 打包 hdrp-bgfx 的 `shaders/manifest.json` + `.bin`。
- 单个资产包含所有 backend；运行时按当前 backend 选择。
- 不在运行时编译 Shader。
- `parcel_hbg` 负责物化到磁盘或未来内存加载。

## 6. Settings 资产

- `PipelineSettingsAsset` → `hbg::render::PipelineSettings`
- `FrameSettingsAsset` → `hbg::render::FrameSettings`
- `PostProcessProfileAsset` → `PostProcessProfile`（Bloom/DOF/MotionBlur/TAA/FXAA/ColorGrading/Exposure/Vignette）
- `ShadowSettingsAsset` → `ShadowSettings`
- `VolumetricFogProfileAsset` → `VolumetricFogSettings`

Scene 通过 AssetGuid 引用这些设置；相机可覆盖 FrameSettings。

## 7. SceneAsset

```cpp
struct SceneEntityId { uint64_t value; };

struct SceneEntityRecord {
    SceneEntityId id;
    SceneEntityId parent;       // 0 表示根
    std::string name;
    TransformData transform;
    uint32_t layer;
    uint32_t flags;
};

struct ComponentChunk {
    TypeId type;
    uint32_t schema_version;
    std::vector<SceneEntityId> owners;
    std::vector<std::vector<std::byte>> payloads;   // versioned component payloads
};

class SceneAsset : public AssetObject {
public:
    std::string name;
    SceneData scene;             // entities/components/settings 引用
};

struct SceneData {
    std::vector<SceneEntityRecord> entities;
    std::vector<ComponentChunk> components;
    AssetGuid pipelineSettings;
    AssetGuid frameSettings;
    AssetGuid postProcessProfile;
    AssetGuid shadowSettings;
    AssetGuid volumetricFogProfile;
};
```

### 7.1 IesProfileAsset

```cpp
class IesProfileAsset : public AssetObject {
public:
    std::string name;
    std::vector<float> vertical_angles;
    std::vector<float> horizontal_angles;
    std::vector<float> values;   // vertical-major
};
```

- TypeId 13；由 `ies` importer 解析 `TILT=NONE` 的 IES 文件，保留原始角度/
  candela 表并通过 `ValidateIesProfile` 校验有限值、排序和正 candela。
- `LightComponent.ies` 引用该资产；hbg 桥接层生成 spot 的 `iesSamples`/`iesCut`，
  point 光源则用 profile 调制 cookie legacy/native 数据。
- IES 文件属于普通源资产，参与 import DB、依赖分析和打包。

### 7.2 AnimationClipAsset

```cpp
class AnimationClipAsset : public AssetObject {
public:
    std::string name;
    float duration;
    std::vector<AnimationChannel> channels;
};

struct AnimationChannel {
    SceneEntityId target;
    AnimationPath path;                 // Translation/Rotation/Scale/Weights
    AnimationInterpolation interpolation; // Step/Linear/CubicSpline
    uint32_t target_index;
    std::vector<float> times;
    std::vector<Vec4> values;
    std::vector<Vec4> in_tangents;
    std::vector<Vec4> out_tangents;
};
```

- TypeId 14；channel 以 SceneEntityId 而非运行时 node index 定位，桥接层再映射到
  `RenderScene.nodes`。
- `ValidateAnimationClip` 校验 duration、target、key 数量、时间单调性和所有浮点值。
- FBX 导入器按 `aiAnimation` 生成一个 clip；导入 node 的
  translation/rotation/scale 通道，以及 `mMorphMeshChannels` 拆出的
  `AnimationPath::Weights` 通道（`target_index` 指向 mesh morph target）。

## 8. 标准组件

| 组件 | 字段 |
|---|---|
| TransformComponent | position / rotation / scale / parent |
| CameraComponent | projection / yfov / aspect / near / far / xmag / ymag / clearColor / cullingMask / priority |
| LightComponent | hdrp Light 全字段：type、color、intensity、range、inner/outer cone、area shape/size、cookie、IES、shadow settings |
| MeshRendererComponent | mesh GUID、material GUIDs、submesh material、cast/receive shadows、sortingLayer/order、rendererPriority、lightLayerMask、decalLayerMask、lightmap atlas/ST/size |
| DecalComponent | hdrp Decal 全字段，texture 引用改 AssetGuid |
| ReflectionProbeComponent | hdrp ReflectionProbe 全字段，image 改 AssetGuid |
| LocalFogVolumeComponent | hdrp LocalFogVolume 全字段，mask image 改 AssetGuid |
| AudioSourceComponent | clip GUID / volume / pitch / loop / playOnAwake / spatial / min-max distance / rolloff |
| AudioListenerComponent | enabled |
| RigidBodyComponent | motion type / mass / drag / angularDrag / gravity / constraints / interpolation |
| ColliderComponent | shape / size / offset / trigger / friction / restitution / layer / mask |
| ScriptComponent | language / script GUID / class/module / enabled / property bag |
| AnimationPlayerComponent | animation clip GUID list |
| SkinComponent | joint SceneEntityId list + inverse bind matrices |
| MorphComponent | per-object morph target weights（数量与 mesh morph target 一致） |

- 组件 TypeId 稳定；自定义组件通过 `ITypeSerializer` 注册。
- 组件内实体引用用 `SceneEntityId`；资产引用用 `AssetGuid`。

## 9. .parcel.json sidecar

```json
{
  "version": 1,
  "source": "level01.fbx",
  "coordinateSystem": "rh-y-up-ccw",
  "settings": {
    "pipeline": "settings/pipeline.parcel",
    "frame": "settings/frame.parcel",
    "postProcess": "settings/post.parcel",
    "shadows": "settings/shadows.parcel",
    "volumetricFog": "settings/fog.parcel"
  },
  "lights": [],
  "decals": [],
  "reflectionProbes": [],
  "localFogVolumes": [],
  "cameras": []
}
```

- 字段兼容 hdrp-bgfx `.hdrp.json`，增加 Parcel 资产引用。
- JSON Schema：`schemas/parcel.scene.schema.json`。
- 导入器负责把 FBX 节点/材质/纹理与 sidecar 合并为 SceneAsset/组件。
- lights/cameras 按名称覆盖节点组件；decals/reflectionProbes/localFogVolumes 生成带
  TransformComponent 的独立实体，组件字段按 hdrp-bgfx 规则 sanitize。
- 纹理字段（decal texture/normalTexture/maskTexture、probe texture、fog maskTexture、
  light cookie、IES profile）转为 AssetGuid；路径优先相对源文件目录，其次
  project-relative。light 还支持 cookieMode/cookieType/cookieScale/cookieOffset；
  `ies` 指向 `.ies` 源文件对应的 IesProfileAsset。

## 10. EnTT 加载

```cpp
struct SceneInstance {
    std::vector<SceneEntityId> entities;
    void* registry = nullptr;
    std::shared_ptr<void> context;
};

class ISceneLoader {
public:
    virtual Result<SceneInstance> Instantiate(const SceneData& scene,
                                               void* registry) = 0;
    virtual Result<void> Destroy(SceneInstance& instance) = 0;
};
```

- R02 冻结 `parcel::scene::ISceneLoader` 接口；R06 已实现
  `parcel_entt::EnttSceneLoader`（`cpp/include/parcel/entt/scene_loader.hpp`）。
- 两阶段：创建 entity → 解析 parent/transform → emplace 组件 → 解析实体引用。
- `ENTT_NOEXCEPTION`；可与无 RTTI 共存。
- `SceneInstance` 持有 entity 列表；销毁时 `registry.destroy`。R06 增加
  `std::shared_ptr<void> context`，由 `EnttSceneLoader` 保存仅属于该实例的
  `entt::entity` 列表，避免误销毁宿主的其他 entity。
- `parcel_hbg::SceneRenderBridge` 从 registry 构建 `hbg::render::RenderScene`
  （R06 已实现，见 `docs/07-hbg-adapter.md`）。
- `ResourcePackage::LoadScene` 加载 SceneAsset 后调用 `ISceneLoader::Instantiate`；
  `UnloadScene` 在最后一个共享 Handle 释放时调用 `Destroy` 并释放依赖 Bundle。

## 11. Schema 版本

- 每个资产类型有 serializer_version；MeshAsset 为 `3`（含蒙皮顶点与 morph targets），
  MaterialAsset 为 `2`（TextureRef 携带 sampler），其余标准类型为 `1`。
- Scene/组件 schema 变更必须升级 Scene 版本或组件 schema_version。
- 旧版本必须提供迁移或明确拒绝；构建报告记录 schema 版本。
- 当前实现采用严格匹配：未知组件 TypeId、未知 schema_version、未知序列化器版本
  一律返回 `Unsupported`，不做静默降级。

## 12. R02 实现契约

### 12.1 构建目标

```text
parcel_scene  → parcel_core + parcel_serialize
parcel_assets → parcel_scene + parcel_serialize
```

两个 target 默认 `/EHs-c- /GR-` 或 `-fno-exceptions -fno-rtti`，不依赖
Hobi、hdrp-bgfx 或 EnTT。EnTT 只通过 `parcel/scene/scene_loader.hpp` 的
`ISceneLoader` 契约接入。

### 12.2 资产 TypeId

| TypeId | 类型名 | 数据位置 |
|---:|---|---|
| 1 | TextureAsset | mip 数据在 BlobTable |
| 2 | MeshAsset | vertex/index/skin/morph 在 BlobTable |
| 3 | MaterialAsset | metadata + AssetGuid 纹理引用 + TextureSampler |
| 4 | ShaderLibraryAsset | pass 二进制在 BlobTable |
| 5 | RawFileAsset | 文件数据在 BlobTable |
| 6 | TextAsset | metadata |
| 7 | PipelineSettingsAsset | metadata |
| 8 | FrameSettingsAsset | metadata（128-bit bitset） |
| 9 | PostProcessProfileAsset | metadata |
| 10 | ShadowSettingsAsset | metadata |
| 11 | VolumetricFogProfileAsset | metadata |
| 12 | SceneAsset | PRCS scene payload 在 BlobTable |
| 13 | IesProfileAsset | 角度/candela metadata |
| 14 | AnimationClipAsset | channel/key metadata |

- 资产 object metadata 采用版本化小端二进制，由 `ITypeSerializer` 写入；
  `TypeTable.serializer_version` 决定是否可解析。FlatBuffers 仍可用于自定义
  serializer，但不是标准资产类型的强制要求。
- `CollectReferences` 生成 PRCA `ExternalRefTable`；SceneAsset 会遍历组件
  payload，收集纹理、mesh、material、音频 clip、animation clip、脚本和 settings 引用。

### 12.3 SceneData（PRCS v1）

```text
Header (128B):
  char magic[4]              "PRCS"
  uint16 version             1
  uint16 header_size         128
  uint32 flags
  uint32 entity_count
  uint32 component_chunk_count
  uint32 reserved
  AssetGuid pipeline/frame/postProcess/shadow/volumetricFog  (5 * 16B)
  byte reserved[24]

EntityRecord:
  uint64 id
  uint64 parent              0 = root
  string name                uint32 length + UTF-8
  TransformData              3 float position + 4 float quaternion + 3 float scale
  uint32 layer
  uint32 flags

ComponentChunk:
  uint32 type
  uint32 schema_version
  uint32 owner_count
  uint32 reserved
  uint64 owners[owner_count]
  payload[owner_count]:
    uint32 payload_size
    byte[payload_size]
```

- `SceneData` 校验：entity id 唯一且非 0；parent 必须存在且无环；component
  TypeId/schema_version 必须注册；owners 必须存在且同一 chunk 内唯一；
  owner 与 payload 数量一致；payload 非空；不允许重复 component chunk。
- `SceneCodec` 提供 `EncodeScene` / `DecodeScene`；SceneAsset 将其编码结果放入
  BlobTable，metadata 只保存名字和 blob_id。
- 实体引用通过 `SceneData::FindEntity` 解析；`ValidateEntityReferences` 校验
  `TransformComponent.parent` 和 `SkinComponent.joints`（非 0 时必须存在）。
  Scene 记录中的 `parent` 是层级权威来源。`MorphComponent` 只保存有限的
  浮点权重，不持有实体引用。

### 12.4 标准组件

| TypeId | 组件 | schema_version |
|---:|---|---:|
| 1 | TransformComponent | 1 |
| 2 | CameraComponent | 1 |
| 3 | LightComponent | 1 |
| 4 | MeshRendererComponent | 1 |
| 5 | DecalComponent | 1 |
| 6 | ReflectionProbeComponent | 1 |
| 7 | LocalFogVolumeComponent | 1 |
| 8 | AudioSourceComponent | 1 |
| 9 | AudioListenerComponent | 1 |
| 10 | RigidBodyComponent | 1 |
| 11 | ColliderComponent | 1 |
| 12 | ScriptComponent | 1 |
| 13 | AnimationPlayerComponent | 1 |
| 14 | SkinComponent | 1 |
| 15 | MorphComponent | 1 |

- `ComponentCodec<T>` 提供类型、名称、schema_version、`Encode`、`Decode`。
- `ComponentRegistry` 接受自定义 `ComponentTypeInfo`，可附加
  `CollectComponentReferencesFn`；自定义组件使用稳定 TypeId 和版本。
- 组件字段采用固定小端顺序（枚举为 uint8，计数为 uint32）；读取时校验
  枚举范围、数量上限、payload 尾随字节。字段顺序变更必须提升 schema_version。

### 12.5 `.parcel.json`

- Schema：`schemas/parcel.scene.schema.json`。
- `ParseSceneSidecar` / `LoadSceneSidecarFile` 校验 `version`、settings 字符串、
  cameras 类型和数值范围；错误包含字段上下文与行列信息。
- `lights` / `decals` / `reflectionProbes`（兼容 `probes`）/
  `localFogVolumes` 保留为 `JsonValue` override，导入器负责与 FBX 节点合并。
- `settings.*` 是资产路径；导入器将其解析为 `SceneData` 中的 `AssetGuid`。

### 12.6 EnTT 两阶段加载

1. 创建 registry 和全部 entity；`SceneInstance.entities` 记录创建顺序。
2. 解析 parent：parent 必须已存在；建立父子关系。
3. 遍历 component chunks，按 owner 反向索引，解码 payload 并 emplace 到 entity。
4. 解析组件内 `SceneEntityId` 资产/实体引用；引用不存在则整体回滚。
5. 失败路径：销毁已创建 entity，返回带 entity/component 上下文的错误。
6. `SceneInstance` 析构/`Destroy` 只销毁自己创建的 entity；不拥有 registry。

### 12.7 hdrp-bgfx 字段映射

| Parcel | hdrp-bgfx |
|---|---|
| MaterialAsset | `hbg::asset::Material`；TextureRef 的 AssetGuid 映射到 imageIndex，sampler 映射到 `hbg::asset::TextureSampler` |
| CameraComponent | `hbg::render::Camera` + ViewState；clearColor/cullingMask/priority 由桥接层保存 |
| LightComponent | `hbg::render::Light` + `LightShadowSettings`；cookie GUID 映射 imageIndex 并填充未压缩 CPU 采样；IES GUID 解析 IesProfileAsset，生成 spot `iesSamples`/`iesCut`，point 时调制 cookie legacy/native 数据 |
| AnimationPlayerComponent | 解析 AnimationClipAsset，按 SceneEntityId 映射 `hbg::asset::AnimationChannel.node` 并写入 `RenderScene.animations` |
| SkinComponent | 把 joint SceneEntityId 映射为 node index，连同 inverse bind matrices 写入 `RenderScene.skins` |
| MeshRendererComponent | `hbg::render::RenderObject` + mesh/material index；有 SkinComponent 时设置 `skinIndex` |
| DecalComponent | `hbg::render::Decal`；texture GUID 映射到 imageIndex |
| ReflectionProbeComponent | `hbg::render::ReflectionProbe` |
| LocalFogVolumeComponent | `hbg::render::LocalFogVolume` |
| PipelineSettingsAsset | `hbg::render::PipelineSettings` |
| FrameSettingsAsset | `hbg::render::FrameSettings` |
| PostProcessProfileAsset | `hbg::render::PostProcessProfile` |
| ShadowSettingsAsset | `hbg::render::ShadowSettings` |
| VolumetricFogProfileAsset | `hbg::render::VolumetricFogSettings` |

适配层只做字段映射和 GUID→索引解析；不修改 hdrp-bgfx。










