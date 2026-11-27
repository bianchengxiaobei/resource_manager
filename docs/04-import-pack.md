# 04 导入与打包

## 1. Import 目标

把源资源转换为 cooked asset，并生成可被 `parcel_pack` 消费的元数据：

```text
Source Assets
  → 扫描
  → .parcelmeta（GUID/importer/settings/source hash）
  → ImportDB（增量判断）
  → Importer
  → Cooked Asset（PRCA）+ 依赖 + 子资产
```

## 2. .parcelmeta

每个源文件旁边一个 JSON：

```json
{
  "version": 1,
  "sourceGuid": "…",
  "importer": "fbx",
  "importerVersion": 8,
  "sourceHash": "sha256:…",
  "settings": {
    "scale": 1.0,
    "coordinateSystem": "rh-y-up-ccw",
    "generateTangents": true
  },
  "assets": [
    { "guid": "…", "type": "SceneAsset", "role": "scene" },
    { "guid": "…", "type": "MeshAsset", "role": "mesh" },
    { "guid": "…", "type": "MaterialAsset", "role": "material" }
  ],
  "dependencies": ["guid-of-texture", "guid-of-shader"]
}
```

- GUID 首次导入生成，之后稳定；移动文件通过 meta 跟随。
- `sourceHash + settingsHash + importerVersion + dependencyOutputHash` 决定是否重导。
- 子资产 GUID 由 `sourceGuid + local_id` 派生，稳定。

## 3. ImportDB

- 建议 SQLite（单文件、事务、并发读）；若必须零依赖，可先用自定义二进制缓存。
- 记录：source path、sourceGuid、asset guids、importer、settings hash、source hash、
  texture index hash、output hash、dependencies、last build time；当前 DB version 2。
- 事务更新；失败不写入半成品。
- DB 可删除重建，meta 是权威。

## 4. Importer 接口

```cpp
class IAssetImporter {
public:
    virtual std::string_view Name() const = 0;
    virtual bool Supports(std::string_view extension) const = 0;
    virtual Result<ImportResult> Import(const ImportRequest&) = 0;
};

struct ImportResult {
    std::vector<CookedAsset> assets;   // 主资产 + 子资产
    std::vector<AssetGuid> dependencies;
    std::vector<std::string> warnings;
};
```

首批 Importer：

- `fbx`：assimp 读取 mesh/material/texture/node/camera/light/animation/bone/blend shape；生成 MeshAsset/MaterialAsset/AnimationClipAsset/SceneAsset 和 SkinComponent。
- `gltf`：内置 cgltf 读取 `.gltf`/`.glb`，支持 mesh/material/texture/node/camera/KHR_lights_punctual、KHR_materials_variants、KHR_texture_transform（UV0；共享 transform 烘网格，逐贴图 transform 烘纹理变体）、EXT_meshopt_compression、scene 选择、skin、morph targets 与 TRS/Weights 动画。
- `texture`：PNG/TGA/JPG/HDR → TextureAsset；BC1/BC3/BC5/BC7、RGBA16F/32F、mip。
- `shader`：hdrp-bgfx manifest + bin → ShaderLibraryAsset。
- `scene settings`：hdrp-bgfx JSON → Parcel Settings 资产。
- `ies`：`TILT=NONE` IES 光度文件 → IesProfileAsset（spot/point Light 采样数据）。
- `text/json`：TextAsset/RawFileAsset。

## 5. FBX/assimp 规则

- 坐标系：右手 Y-up、CCW 正面；与 hdrp-bgfx 一致。
- 单位/缩放：导入设置可覆盖。
- 材质：assimp 材质 → MaterialAsset；纹理路径转 AssetGuid。
- 节点：FBX 层级 → Scene entities + TransformComponent。
- 动画：`aiAnimation` 的 node translation/rotation/scale 通道 → AnimationClipAsset；
  根实体写入 AnimationPlayerComponent；`aiBone` 顶点权重写入 MeshAsset.skinVertices，骨骼节点写入 SkinComponent；
  `aiMesh::mAnimMeshes` 写入 MeshAsset.morphTargets（默认权重为 0，避免 FBX `mWeight=1` 直接应用所有 shape），
  `aiAnimation::mMorphMeshChannels` 按 target 拆成 AnimationPath::Weights 通道并映射到 mesh 所属实体。
- 相机/灯光：assimp 支持的部分映射到 Camera/Light；其余用 `.parcel.json` 覆盖。
- morph 限制：FBX 的 channel index 直接作为 target_index，按 Assimp 的 AnimMesh
  顺序导入；单个 channel 含多个 in-between shape 或复杂多 BlendShape 时可能不精确。
- 去重：同一 FBX 导入内，相同 mesh/material 内容按 hash 去重并保留一个 AssetGuid；
  带蒙皮顶点的 mesh 不跨节点去重；纹理引用按“文件内容 + 纹理设置”跨源复用
  canonical TextureAsset GUID，不同设置分开。
- meshoptimizer（内置 0.18）：默认 `optimizeMesh=true` 做顶点去重、缓存和 fetch 优化；
  带蒙皮或 morph target 的 mesh 只做索引重排，不合并顶点；`optimizeOverdraw=false`，
  可设 `overdrawThreshold`（默认 1.05）。

## 5.1 glTF/GLB 规则

- 入口：`.gltf` 与 `.glb`；`cgltf` 解析 JSON/GLB chunk、外部 buffer 与 data URI buffer。
- 网格：triangle primitive 合并为一个 MeshAsset；多 primitive 生成多个 submesh，
  材质映射到 MeshRenderer；支持 POSITION/NORMAL/TANGENT/TEXCOORD_0/1/COLOR_0、
  JOINTS_0..3/WEIGHTS_0..3（最多取 4 个影响）。
- 场景：node 层级、TRS/matrix（matrix 分解为 TRS）、camera、KHR_lights_punctual
  point/spot/directional；导入设置 `scene` 支持场景名或 0-based 索引，留空使用 glTF 默认 scene，
  未声明默认 scene 时回退全部 node。仅选中 scene 可达节点创建
  Camera/Light/MeshRenderer/Skin/Morph 组件，全部 node 的 Transform 保留供
  skin/animation 引用。
- 材质：PBR metallic-roughness、alphaMode/cutoff、doubleSided、emissiveStrength、
  clearcoat/transmission/ior/iridescence/anisotropy/diffuseTransmission 字段映射；
  纹理引用保留 glTF sampler 的 wrapS/T、min/mag/mip filter，无 sampler 时使用
  glTF 默认 Repeat/Linear。
- 材质变体：读取 `KHR_materials_variants`；导入设置 `materialVariant` 选择命名变体，
  未设置时使用 primitive 默认材质；变体未覆盖某 primitive 时回退默认材质，
  未知变体名返回 `InvalidArgument`。
- 动画：node translation/rotation/scale 与 weights channel；Step/Linear/CubicSpline。
- Skin：`skin.joints` 映射到 SceneEntity；`JOINTS_n/WEIGHTS_n` 写入 MeshAsset；
  逆绑定矩阵写入 SkinComponent。
- Morph：primitive targets 合并为 mesh-level morph targets，默认权重取 mesh.weights，
  node.weights 写入 MorphComponent；morph 动画拆成 Weights channel。
- sidecar：`.parcel.json` 的 settings/camera/light override 与 decals/probes/localFog
  合并逻辑与 FBX 一致。
- 纹理：external image 沿用 canonical texture GUID；embedded image（bufferView/data URI）
  解码为 TextureAsset 子资产，GUID 由 glTF 源 + image index 派生，Material 引用并写入
  ImportDB 依赖；SceneRenderBridge 经 `MaterializeTexture` 保留完整 mip 链与 format，
  hdrp-bgfx 的 MaterialSystem/ParcelMaterialSystem 均可直接上传。
- 去重：无 skin 的 mesh 按内容 hash 去重；material 按内容 hash 去重；外部纹理沿用
  canonical texture GUID（文件内容 + 纹理设置）。
- 压缩：`EXT_meshopt_compression` 支持 ATTRIBUTES/TRIANGLES/INDICES 三种 mode 与
  NONE/Octahedral/Quaternion/Exponential filter；使用内置 meshoptimizer 解码后
  覆盖 bufferView 数据，单 bufferView 与累计解码均受 512 MiB 限制。
- KHR_texture_transform：仅使用 UV0。所有已映射纹理共享同一 transform 时，把
  `offset/rotation/scale` 在导入期烘入 `TEXCOORD_0`，保持 hdrp-bgfx 原 shader
  只采样 UV0；逐贴图 transform 不同时，改为按 `(源图, transform, sampler, sRGB)`
  生成烘焙后的 TextureAsset 变体，网格 UV0 保持原值。使用 `texCoord != 0` 时保留
  原始 UV0 并追加 warning。逐贴图烘焙会增加 cook 时间、纹理内存和包体；变体使用
  默认 texture 编码设置（不继承外部纹理的 `.parcelmeta` 覆盖）。
- 限制：Draco/basisu/WebP 的 required extension 返回 Unsupported；未选中 scene
  的 mesh/material 仍会 cook。
  外部 buffer/image 可包含 `..` 父目录段，但解析后的路径必须仍位于 project root
  内；外部纹理需与 glTF 同批导入以建立 canonical GUID。多个 node 复用同一
  skinned mesh 且 skin joint 顺序不同时按各自 SkinComponent 解释，建议使用同一
  skeleton。

## 6. 纹理

- 解码：bimg（hdrp-bgfx 已有）/ lodepng / stb。
- 格式：RGBA8/BGRA8/RGBA16F/RGBA32F/BC1/BC3/BC5/BC7；ASTC 后置。
- 非压缩 `format`：`auto`/`rgba8`/`bgra8`/`rgba16f`/`rgba32f`；
  HDR `auto` 为 RGBA32F，可显式转为 RGBA16F。
- `format` 仅用于未压缩输出；与 `compression!=none` 同时设置会报错。
- 压缩 `compression`：`none`/`auto`/`bc1`/`bc3`/`bc5`/`bc7`；
  `auto` 对不透明 LDR 选 BC1、含 alpha 选 BC3；HDR 不支持 BC。
- BC7：bc7enc；BC1/BC3/BC5：stb_dxt；`compressionQuality` 为
  `fast`/`balanced`/`best`，写入 settings hash。
- mip：默认生成完整链；HDR 保留 float；压缩块按 4x4 边缘复制填充。
- sRGB：按用途（baseColor/emissive 为 sRGB，normal/mask 为 linear）设置。
- 大图/图集后续；首期单图。

## 7. Shader

- 输入：hdrp-bgfx `shaders/manifest.json` + `build/<backend>/*.bin`。
- 输出：一个 `ShaderLibraryAsset`，包含所有 backend 与 pass/variant 二进制。
- 不运行时编译；默认要求先运行 hdrp-bgfx 的 `shader_build.py`。
- 可选：`parcel_import` 在导入前调用原始 `shader_build.py`（不修改脚本）：

```powershell
parcel_import --project <root> --source <root> --output <cooked> `
  --shader-build-python py `
  --shader-build-script <hdrp-bgfx>/tools/shader_build.py `
  --shader-build-shaderc <shaderc> --shader-build-bgfx-src <bgfx-src>
```

  `--shader-build-root` 默认 project root，输出固定为
  `<root>/shaders/manifest.json`；`--shader-build-jobs`、`--shader-build-timeout`
  和 `--shader-build-force` 透传。省略 `--shader-build-python` 时直接执行脚本/包装器。
  build 失败会中止导入；`--dry-run` 不执行 build；build 成功后强制重导 Shader
  manifest，避免只改 `.bin` 时增量跳过。
- ShaderLibraryAsset 单独打 Bundle，便于首包/更新分离。

## 8. Scene sidecar

- `.parcel.json` 与源 FBX 同目录或显式指定；Schema 为
  `schemas/parcel.scene.schema.json`。
- 字段兼容 hdrp-bgfx `.hdrp.json`：lights/decals/probes/localFogVolumes/cameras/settings。
- 导入器把 FBX 节点与 sidecar 合并；settings/cameras/lights 按节点名覆盖，
  decals/probes/localFogVolumes 生成独立实体与对应组件。lights 支持
  cookie/cookieMode/cookieType/cookieScale/cookieOffset 与 ies；cookie 路径复用纹理解析与去重，
  ies 路径解析为 IesProfileAsset GUID 并进入 SceneAsset 依赖。
- 组件字段对齐 hdrp-bgfx：decal 的 opacity/angleFade/normalBlend/ao/layerMask/channelMask，
  probe 的 boxExtents/blendDistance/intensity，local fog 的 shape/falloffMode/maskPlane/
  positiveFade/negativeFade/sanitize 规则。
- sidecar 纹理路径优先相对源文件目录解析，再尝试 project-relative；缺失/非法字段产生 warning，
  probe 无有效 texture 时跳过该 probe。
- 设置资产可复用同一份 pipeline/frame/postprocess/shadow/fog 配置。

## 9. Import CLI

```bash
parcel_import \
  --project <project-root> \
  --source <assets-dir> \
  --output <cooked-dir> \
  --db <import.db> \
  --target windows \
  --jobs 8
```

输出：

```text
cooked/
  <asset-guid>.prca
  import_report.json
```

退出码：0 成功；非 0 失败（参数/IO/导入错误/依赖缺失）。

## 10. Pack 目标

```text
Cooked Assets + .parcelmeta + ImportDB
  → Collector（地址/标签/打包规则）
  → DependencyAnalyzer（资产→Bundle 依赖，环检测）
  → BundleBuilder（PRCL）
  → ManifestBuilder（PRCM）
  → Verify → Report → Output
```

## 11. Collector 规则

- MainAssetCollector：可被代码加载，写入 Manifest AssetList。
- StaticAssetCollector：参与打包，不写入可加载列表。
- DependAssetCollector：只作为依赖参与；零引用则剔除。
- 地址规则：Address / AssetPath / GUID 三选一或组合。
- 过滤规则 `filterRule`：按 cooked 资产类型选择，默认 `CollectAll`。
  - `CollectAll`、`CollectScene`、`CollectShader`、`CollectRaw`、`CollectVideo`、
    `CollectTexture`、`CollectMaterial`、`CollectMesh`、`CollectAnimation`、
    `CollectText`、`CollectIES`。
  - `CollectVideo` 仅匹配源路径为视频扩展名的 RawFileAsset。
  - `CollectPrefab`、`CollectSprite`、`CollectShaderVariants` 对 Parcel cooked 资产不适用，
    配置后 pack 返回 `Unsupported`。
- 打包规则 `packRule`：默认 `Default` 保持原有目录/依赖/Shader/Raw 行为。
  - `PackSeparately`：每个资产单独 Bundle，名为 `separate/<guid>`。
  - `PackDirectory`：资产所在目录。
  - `PackTopDirectory`：资产路径第一级目录。
  - `PackCollector`：collector 名；`PackGroup`：`group`，未设置时退回 collector 名。
  - `PackRawFile` / `PackVideoFile`：匹配 RawFileAsset（视频规则再检查源扩展名），
    名为 `raw/<guid>`。
  - `PackShader`：ShaderLibraryAsset 进入 `shaders/<collector>`。
  - `PackShaderVariants` 不支持，pack 返回 `Unsupported`。
  - 显式 `bundle` 始终优先于 `packRule`；`.parcelmeta` source group 也会强制同组。
- 标签：流向 Bundle tags，用于首包/按标签下载。
- Shader 自动独立 Bundle；Raw 文件默认单独 Bundle，`archive=true` collector 可将多个 raw 写入同一 archive Bundle。
- Pack 可选扫描 `source`（默认 project root）下的 `.parcelmeta`，将 cooked GUID 映射回
  源相对路径；`useSourcePath=true`（默认）时主资产 Manifest path/AssetPath 地址优先使用
  源路径。

## 12. Bundle 分配

- 主资产 + 子资产同 Bundle，避免拆散；有 `.parcelmeta` 时按 `sourceGuid` 强制归组。
- 组内显式 `bundle` 必须一致，否则构建失败；Raw 文件不能与其它资产同组。
- 共享资源被多个 Bundle 引用时独立共享 Bundle；单引用可并入引用者。
- Raw Bundle：默认一个文件一个 Bundle；archive collector 例外。
- Bundle 依赖图必须无环；Packer 构建期校验。
- Bundle 粒度：可按目录/Collector 配置；避免一个资源一个 Bundle 或超大 Bundle。

## 13. Manifest/输出

```text
output/
  bundles/<bundle_guid>.bundle
  manifest.bytes
  manifest.hash
  <package>.version
  build_report.json
  builtin_catalog.bin（默认生成；writeBuiltinCatalog=false 可关闭）
```

- Manifest 包含 Asset/Bundle/Tag/Dependency 表。
- `manifest.hash` 为 SHA-256。
- BuildReport：总大小、每 Bundle 大小/资源数/依赖、重复资源、未使用资源、零引用、
  告警/错误、Shader 变体、纹理格式统计、内建目录字节数。
- `builtin_catalog.bin`：`magic=0x0133C5EE`、`version=1`；U16 长度 UTF-8 字符串；
  记录 package/version 和 BundleGuid+FileName，条目按 FileName/GUID 稳定排序。
  运行时 BuiltinFS 优先按目录定位，缺失时回退目录探测。

## 14. 确定性/增量

- 稳定 GUID、稳定排序；Manifest/Bundle 无时间戳。
- 内容寻址 Bundle 文件名；同一输入字节级可复现。
- 增量：ImportDB + BuildDB；资产 hash + Bundle 成员 + 工具版本决定是否重建。
- 文件级增量：BuildDB 记录资产路径、size/mtime、hash、类型、依赖和报告统计；未变化资产
  不重新读取/解析。Bundle 需要重建时，按需读取并校验 hash 后再写入 payload。
- Bundle 成员变化会触发 Bundle 重建；Manifest 每次重新生成。
- `build_report.json` 的 `reusedBundleCount`/`reusedAssetCount` 可用于确认增量命中。
- 构建报告记录工具版本/编码器版本/参数。

## 15. Pack CLI

```bash
parcel_pack \
  --project <project-root> \
  --package game \
  --version 1.0.0 \
  --target windows \
  --cooked <cooked-dir> \
  --output <build-dir> \
  --config <pack-config.json|toml> \
  --verify-output
```

退出码非 0 表示构建失败；CI 必须检查；`--verify-output` 可选。


## 16. R03 已实现契约

### 16.1 `.parcelmeta`

- `<source>.parcelmeta`，JSON Schema：`schemas/parcel.meta.schema.json`。
- 字段：`version/sourceGuid/importer/importerVersion/sourceHash/settingsHash/settings/assets/dependencies`。
- `sourceHash` 为源文件 SHA-256；若存在同名 `.parcel.json`，与 sidecar hash 组合后写入，
  保证 sidecar 修改会触发重导。
- `assets[]` 包含 `guid/type/role/localId`；`localId` 是子资产稳定派生键。
- 源码 GUID：`SHA-256("parcel.source.v1:" + project-relative-path)[0..15]`；
  子资产 GUID：`SHA-256("parcel.sub.v1:" + sourceGuid + role + localId)[0..15]`；
  首次写入 meta 后以 meta 为权威。

### 16.2 ImportDB

- 零依赖二进制缓存，头部 `PRID` + version 2 + record table。
- 每条记录保存 source path/GUID、importer/version、source/settings hash、
  texture index hash、outputs（GUID+output hash）、dependencies（GUID+output hash）。
- DB 按 source path 稳定排序；写临时文件后原子 rename。
- 增量条件：sourceHash + settingsHash + importerVersion + texture index hash（FBX/glTF）+
  所有 output 文件存在且 hash 匹配 + dependency output hash 匹配；否则重导。
- 失败源不写入 DB；成功源统一提交。DB 可删除重建，meta 是权威。

### 16.3 `parcel_import`

- target：`parcel_import` 静态库、`parcel_import` CLI（CMake target
  `parcel_import_cli`）。
- 内置 importer：`texture`（PNG/TGA/JPG/HDR）、`shader`（hdrp-bgfx manifest）、
  `ies`、`text`、`raw`、`gltf`，以及启用 assimp 时的 `fbx`。
- `.parcelmeta` 与 sidecar 在源目录；cooked 输出 `<guid>.prca`；
  `import_report.json` 记录 status/importer/version/settings hash/assets/warnings；
  耗时由 CLI stdout 输出。
- 多个源可并行处理；提交阶段按稳定顺序写文件、meta、DB；编码失败不会写半成品。
- texture importer v3：默认 `format=auto`/`compression=none`；LDR 输出 RGBA8，
  HDR 输出 RGBA32F；可选 `bgra8`/`rgba16f`/`rgba32f` 与 `bc1/bc3/bc5/bc7`。
  `compressionQuality=fast/balanced/best` 参与 settings hash。
- shader build 可选：`--shader-build-python/script/shaderc/bgfx-src/root/jobs/timeout/force`
  调用原始 `shader_build.py`；成功后强制重导 Shader manifest，失败中止导入。
- FBX 使用 assimp 可选依赖：

```powershell
cmake --preset windows-debug -DPARCEL_ASSIMP_ROOT=E:/Learn/assimp
```

  未配置时 `.fbx` 会被明确报告为不支持；配置后 CLI/tests 会自动复制
  `assimp-vc143-mt.dll`。
- FBX importer v8：meshoptimizer 优化；sidecar decals/reflectionProbes/localFogVolumes 映射为独立实体组件；
  lights 支持 cookie/ies；节点动画输出 AnimationClipAsset 并挂到根实体；骨骼权重输出
  MeshAsset.skinVertices/SkinComponent；blend shape geometry 输出 MeshAsset.morphTargets，
  morph 权重曲线输出 AnimationPath::Weights 通道；字段校验失败/缺失纹理/IES/未知动画节点/
  无法解析的骨骼/越界 morph target 追加 warning，probe 无有效 texture 时跳过。
- 同一 FBX 导入内，material/mesh 按编码内容 hash 去重；纹理源按“内容 + 设置”选择
  canonical TextureAsset GUID，所有引用共享同一 cooked 资产。
- glTF importer v9：内置 cgltf，支持 `.gltf`/`.glb`、external/data URI buffer、
  mesh/material/node/camera/KHR_lights_punctual、KHR_materials_variants、
  KHR_texture_transform（共享 UV0 transform 烘入 `TEXCOORD_0`，逐贴图 transform
  烘焙纹理变体）、EXT_meshopt_compression、scene 选择、skin、morph targets、
  TRS/Weights 动画与 sidecar override；external buffer/image 的父目录相对路径在
  project root 内可解析；embedded image 解码为纹理子资产；无 skin mesh/material
  按内容去重，外部纹理复用 canonical GUID。

## 17. R04 已实现契约

### 17.1 配置与采集

- `parcel_pack` 静态库；CLI CMake target `parcel_pack_cli`，输出
  `parcel_pack.exe`。
- 配置 schema：`schemas/parcel.pack.schema.json`；CLI 同时接受 `.json` 和 `.toml`，
  两种格式共享同一字段语义。
- 顶层字段：`package/version/target/source/cooked/output`、压缩/块大小、manifest flags、
  `autoCollectShaders`、`writeBuiltinCatalog`、`enableShareBundle`、
  `useSourcePath`、`forceSubAssetGrouping`、`singleReferencedPackAlone`、`clean`、`verifyOutput`、
  `outputNameStyle`、`buildBundleType`。
- 构建输出参数：
  - `outputNameStyle=0/1`（HashName）：`<bundleGuid>.bundle`，保持旧行为。
  - `outputNameStyle=2`（BundleName）：bundle 名中的路径分隔符和非文件名字符转为 `_`，再追加 `.bundle`。
  - `outputNameStyle=3`（BundleNameHashName）：`<sanitized-bundle-name>_<bundleGuid>.bundle`。
  - `buildBundleType=0/1`（AssetBundle）：常规 PRCL，archive 由 collector `archive` 控制。
  - `buildBundleType=3`（Archive）：所有 RawFileAsset 强制走 archive raw；`2`（RawFile）暂不支持，返回 `Unsupported`。
- `collectors[]` 支持 `main/static/depend`、目录或 `files` 显式列表、递归、
  `include`/`exclude` glob、`assetPath/fileName/guid/prefix/address` 地址规则、
  `addressPrefix`/`address`、`tags`、显式 `bundle`、`share`、`archive`；
  以及 `group`、`packRule`、`filterRule`。规则名大小写不敏感，schema 使用
  YooAsset 风格 PascalCase（如 `PackDirectory`、`CollectMaterial`）。
- `include`/`exclude` 模式相对 collector 的 `path`（默认 cooked root），大小写敏感；
  支持 `*`（不跨 `/`）、`**`（跨 `/`，`**/` 可匹配零层目录）、`?`、`[abc]`、
  `[!abc]`。`include` 会递归扫描目录，`exclude` 在文件列表/目录扫描/include 之后过滤；
  `files` 仍按精确相对路径处理。
- 从 cooked 目录读取 `<guid>.prca`；GUID 以文件名为权威。PRCA 必须恰好包含一个
  object；object 的 TypeTable 名称作为 manifest `typeName`。
- 资产依赖只从 PRCA `ExternalRefTable` 读取；缺失依赖报错。Depend 资产未被
  Main/Static 可达引用时剔除并进入 report。
- 重复 GUID、重复地址/资产路径、循环资产依赖均由 pack 阶段校验失败。

### 17.2 Bundle 分配

- 默认按资产所在目录分 Bundle；collector 的 `bundle` 可显式覆盖。
- `packRule` 可覆盖默认 Bundle 名：`PackSeparately`、`PackDirectory`、
  `PackTopDirectory`、`PackCollector`、`PackGroup`、`PackRawFile`、`PackVideoFile`、
  `PackShader`。显式 `bundle` 和 source group 优先于 `packRule`。
- ShaderLibraryAsset 默认进入 `shaders` 共享 Bundle；RawFileAsset 默认每个资产单独
  Bundle，非 archive 的 Raw Bundle 多文件会失败。
- collector 设置 `archive=true` 时，其 RawFileAsset 不再写 PRCA payload，而是把原始
  raw bytes 写入 PRCL TOC 的 `raw_files`；同一 archive bundle 可包含多个 raw 文件。
  bundle 名优先使用显式 `bundle`，否则为 `archive/<collector.name>`。Raw 文件名取
  manifest `assetPath`（`replaceAssetPathWithAddress=true` 时为 address），必须唯一且为
  安全相对路径。Archive raw 的 Manifest asset 设置 asset flag bit 0，运行时
  `LoadRawFile` 从 TOC 读取；`OpenRawFile` 可直接按 range 读取 TOC raw。非 archive raw
  行为不变，`OpenRawFile` 会物化一次后提供 range 读取。
- Depend 资产被多个 Bundle 引用时进入 `shared/<目录>`；单引用且
  `singleReferencedPackAlone=false` 时并入引用者 Bundle。
- Bundle GUID 由 package、bundle 名、压缩参数、工具版本、成员 GUID+资产 hash
  派生；文件名 `<bundle-guid>.bundle`。
- 每个非 archive PRCA 作为 Bundle 的一个 payload，按成员 GUID 稳定排序并 8 字节对齐；
  payload 的 `mainAssetGuid` 为 cooked 资产 GUID；archive raw 与 payload 共用逻辑流但
  使用独立的 TOC 范围。
- Bundle 依赖图来自资产 ExternalRefTable 的跨 Bundle 边；构建期检查循环依赖。

### 17.3 源路径映射与子资产组包

- `parcel_pack` 新增 `--source <dir>`；配置顶层 `source` 可覆盖扫描根，默认使用
  `project root`。Pack 递归扫描其中 `*.parcelmeta`，按 meta 文件位置推导项目相对源路径。
- `.parcelmeta` 的 `sourceGuid` 作为源组；`assets[].guid` 全部登记到该组。主资产按
  `scene > texture > shader > mesh > material > animation > raw > text > ies` 选择；
  `useSourcePath=true`（默认）时主资产的 Manifest `asset_path` 与 `assetPath` 地址使用
  源路径，子资产保留 cooked 路径以避免同源多资产路径冲突。
- `forceSubAssetGrouping=true`（默认）时，同源资产强制进入同一个 Bundle；组内显式
  `bundle` 必须一致。组名默认取源路径父目录，全部为 Shader 时使用 `shaders`。
- Raw 资产不能与同源其它资产组包；构建返回 `InvalidArgument`。
- `sourcePath` 写入 `build_report.json`；没有 meta 映射的资产行为不变。
- 非法/不可读 `.parcelmeta` 记录 warning 并跳过；重复 GUID 或同一源组路径冲突会失败。

### 17.4 输出

CLI 参数：

```bash
parcel_pack \
  --project <project-root> \
  --package game \
  --version 1.0.0 \
  --target windows \
  --source <source-dir> \
  --cooked <cooked-dir> \
  --output <build-root> \
  --config <pack-config.json|toml>
```

配置示例：

```json
{
  "package": "game",
  "version": "1.0.0",
  "target": "windows",
  "source": "assets",
  "cooked": "cooked",
  "output": "build",
  "useSourcePath": true,
  "forceSubAssetGrouping": true,
  "verifyOutput": true,
  "compression": "lz4",
  "blockSize": 262144,
  "collectors": [
    {
      "name": "main",
      "type": "main",
      "path": "assets",
      "include": ["**/*.prca"],
      "exclude": ["**/debug/**"],
      "addressRule": "assetPath",
      "tags": ["base"],
      "filterRule": "CollectAll",
      "packRule": "PackDirectory"
    },
    {
      "name": "textures",
      "type": "depend",
      "path": "textures",
      "addressRule": "fileName",
      "filterRule": "CollectTexture",
      "packRule": "PackTopDirectory"
    }
  ]
}
```

也可以使用 `.toml` 配置（顶层键值 + `[[collectors]]`）：

```toml
package = "game"
version = "1.0.0"
cooked = "cooked"
output = "build"
verifyOutput = true

[[collectors]]
name = "main"
type = "main"
path = "assets"
include = ["**/*.prca"]
exclude = ["**/debug/**"]
addressRule = "assetPath"
tags = ["base"]
```

TOML 支持基本/字面量字符串、十进制整数、布尔、数组、注释和多行数组；collector 使用
`[[collectors]]`。不支持多行字符串、dotted keys、inline tables 和 `[section]`。
顶层字段应写在第一个 `[[collectors]]` 之前。

`--output` 是构建根目录，最终包目录为
`<output>/<target>/<package>/<version>/`：

```text
bundles/<bundle-guid>.bundle    # outputNameStyle 0/1；2/3 见构建输出参数
manifest.bytes
manifest.hash                 # PRCM content hash，hex + newline
<package>.version
build_report.json
build_db.json
builtin_catalog.bin          # writeBuiltinCatalog=false 可关闭
```

- Manifest 只写 Main 资产；Static/Depend 参与 Bundle，但不进入可加载 AssetList。
  Asset 记录包含 address/path/type/bundle/payload/localObject/dependency bundle/tags。
  Archive raw 的 asset flags bit 0 为 `ArchiveRaw`，此时 `payload/localObject` 为 0，
  `assetPath` 即 TOC raw 文件名；`LoadRawFile` 返回原始 raw bytes。
- `manifest.hash` 为 `ManifestHeader.content_hash`，即 `[header_size,file_size)`
  的 SHA-256，与 R01 的 `LoadManifestFile` 校验语义一致。
- `build_db.json`（version 2）记录工具版本、Bundle 名/GUID/文件名/file hash、成员 GUID+hash，
  以及资产的相对路径、size/mtime、hash、类型、依赖和纹理/Shader 报告统计。
  未变化 Bundle 校验文件 hash 后复用；未变化资产直接复用元数据/统计，Manifest 每次重建。
- `build_report.json` 记录工具版本/压缩参数、总大小、Bundle/资产依赖、资产类型、纹理格式、Shader
  variant/backend、未使用资产、零引用、重复内容、增量命中统计、输出校验状态（`outputVerified`/`verifiedBundleCount`）、告警和错误。
- Bundle payload 压缩会并行构建；Manifest 输出稳定排序，重复打包字节级一致。
- 失败构建返回非零并写 `build_report.json`；`--clean` 删除新计划之外的旧
  `.bundle` 文件。
- 可选输出校验：`verifyOutput=true` 或 CLI `--verify-output` 会在写完后重新读取
  `manifest.bytes`/`manifest.hash`/`<package>.version`/全部 Bundle（以及存在时的
  `builtin_catalog.bin`），对比大小、SHA-256、CRC32 和 catalog 条目；不一致返回
  `HashMismatch` 并写入失败报告。`parcel::pack::VerifyPackOutput()` 可单独调用。
- 已实现 Bundle + 文件级增量、collector `include`/`exclude` glob、JSON/TOML 配置、
  archive bundle、`packRule`/`filterRule`/`group`；未实现/延后：加密 hook。




















