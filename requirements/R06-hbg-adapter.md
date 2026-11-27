# R06 `parcel_hbg` 与 EnTT 场景适配

状态：主体完成（2026-11-26）。EnTT loader、ResourcePackage Scene 生命周期、CPU 侧 hbg 映射（含节点层级、Light cookie/IES 采样）、Settings/Shader 物化、GPU 上传 API、合成场景/真实 Parcel 包资产的 bgfx `ForwardRenderer` GPU smoke 已实现；包内 `ShaderLibraryAsset` 经 `ShaderLibraryBootstrap` 内容寻址物化（blob + 硬链接/复制回退）后可驱动未修改的 `ShaderLibrary`；10000 静态实例已在 D3D12 验证，Proland 60067-node、OceanSphere 和 river 真实资产已完成导入/打包及 D3D12/D3D11/Vulkan smoke；相对原 hdrp render-test 的帧预算和轻量 `fractal_lod` 场景的绝对 60 fps 门禁均已通过；baked terrain LOD 选择、CPU heightmap tile cache（精确 node bounds/camera ground）、CPU shared-grid 顶点变形、按 1km mesh + 500m submesh 的 60k vegetation frustum/5000m 距离剔除及 near/far LOD、CPU water FFT、CPU river mesh/terrain carve、river material 纹理、逐帧 wave normal、屏幕空间 Poisson foam 播种和 CPU foam 逐帧动画已支持；GPU shader 顶点变形、material tiles、逐实例 GPU instancing/imposter、GPU FFT 纹理、GPU river particles/refraction 仍未覆盖。

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
- [x] manifest 相对路径/bin 文件写入；单 fragment/bufferFragment 同时写 legacy 单数字段。
- [x] 调用未修改的 `ShaderLibrary::init(resources, root)`（泛型 Initialize；GPU smoke 已用真实 dxbc/spirv 库验证）。
- [x] 版本切换旧目录清理。
- [x] 错误：manifest 缺失/bin 缺失/路径非法。
- [x] 可选硬链接/复制优化（内容寻址 `.blobs/<sha256>.bin`；同卷硬链接，失败回退复制；旧版本清理跳过 blob 目录并回收无引用 blob）。
- [x] 并发物化安全：进程唯一临时名、幂等硬链接/复制提交和 manifest 原子替换；D3D12/Vulkan 同时共享同一 shader cache 的 smoke 已通过。

### SceneRenderBridge

- [x] 从 entt registry 读取 Transform/Camera/Light/MeshRenderer/Decal/Probe/LocalFog。
- [x] 构建 `RenderScene`：meshes/materials/images/objects/nodes/animations/lights/cameras/decals/probes/localFog。
- [x] `RenderScene.nodes` 按 SceneData 顺序生成层级，并填写 object/camera/light `nodeIndex`。
- [x] AnimationClipAsset → `RenderScene.animations`，SceneEntityId channel 映射为 node index。
- [x] SkinComponent → `RenderScene.skins`/object `skinIndex`，joint SceneEntityId 映射为 node index。
- [x] MeshAsset.morphTargets/MorphComponent → object `morphWeights` 与 node weights；Weights channel 映射。
- [x] 脏标记/帧更新策略。
- [x] 材质 imageIndex/GUID 映射。
- [x] 静态 mesh/material/image 按 GUID 与子网格材质去重；skin/morph mesh 保留每对象副本。
- [x] Light cookie CPU 采样：未压缩 RGBA8/BGRA8/RGBA16F/RGBA32F；Legacy/Point Octahedral。
- [x] Light IES 采样：IesProfileAsset；spot `iesSamples`/`iesCut`，point cookie legacy/native 调制。
- [x] 未修改 hdrp-bgfx `ForwardRenderer` 可消费（CPU 结构边界已对齐）。
- [x] 测试：最小场景 → RenderScene 字段正确；节点顺序/层级；动画 channel/蒙皮/morph 映射；cookie/IES 采样与拒绝路径；image 完整 mip 链保留；共享静态 mesh/material 去重且 skin/morph 保持独立。
- [x] GPU smoke：`parcel_hbg_gpu_smoke` 使用隐藏 GLFW 窗口、真实 bgfx Device、未修改 `ShaderLibrary`/`ForwardRenderer`；验证 `ParcelMaterialSystem` 完整 mip/BC7/RGBA16F 上传、draw call 与资源释放（Windows/D3D12/D3D11/Vulkan 已实测）。`--package` 模式通过 `ResourcePackage::LoadScene` → `EnttSceneLoader` 读取真实 PRCA/Bundle（已用 glTF→import→pack 包验证）；包内 `ShaderLibraryAsset` 经 `ShaderLibraryBootstrap` 物化到 `--shader-cache`，再重初始化 `ShaderLibrary` 并创建 Forward program（dxbc/spirv 已实测）；`--objects=10000` 共享静态网格实例已通过 D3D12 smoke；Proland 60067-node 真实资产按原始 `../../assets` 布局（staging）直接导入（79 assets、168MB bundle，15449 draws/1.56M triangles），已通过 D3D12/D3D11/Vulkan smoke。
- [x] 帧预算：smoke 增加 warmup/avg/median/p95 和可选 `--gpu-light-list`/`--motion-vectors`；`scripts/run-r06-performance.ps1` 交替执行 3 组配对，与未修改 hdrp `--render-test` 同场景/同后端/同配置对比。D3D11 1920x1080：Parcel avg 291.804ms vs hdrp 301.689ms（0.967，40226 draws/24 passes/2501870 triangles 完全一致）；D3D11/D3D12/Vulkan 320x240 比率 1.048/0.989/0.752。门限为不慢于原实现 10%；该 60k-node 场景绝对仅约 3-4 fps，原 hdrp 同样低于 60 fps。
- [x] 轻量场景绝对 60 fps：`fractal_lod.gltf`（346 nodes/98306 triangles/12 draws/23 passes）和 `proland_light/1.0.0` 包，D3D11 800x450、每组 120 帧、3 组配对：原 hdrp 7.304ms（136.9fps），Parcel 1.634ms（612.0fps），比率 0.224，draw/pass/triangle 完全一致。原 hdrp `render_test.cpp` 从第 0 帧开始计时，20 帧包含首帧 pipeline 编译而低估到 50.4fps，因此 `-MinFps` 要求 `Frames >= 120`；D3D12/Vulkan 单组 120 帧也分别达到 100.1/202.6 fps。
- [x] 第二项目资产：`ocean_sphere.gltf` 按原始 `../../assets` 布局导入/打包（18 assets、2 bundles、92187880 bytes），`scripts/run-r06-ocean-smoke.ps1` 在 D3D11/D3D12/Vulkan 800x450 验证 `objects=1 draws=28 passes=26 textures=59`、`triangles=34224-34244` 并输出截图。原 hdrp `--render-test` 会加载 glTF 中未被 scene 引用的 terrain LOD 节点（draws=12/triangles=98306），因此该资产只作导入/打包/三后端渲染 smoke，不作 draw/pass 一致性门禁。
- [x] Heightmap LOD bounds/ground：新增 `parcel_hbg::HeightmapTileCache`，按原 `HeightmapTileCache` 的 `HBGHMAP1` 头、`heightmap_scale`、同步 node bounds 查询和 LRU capacity 解析 `heightmap.bin` RawFileAsset；`TerrainLodController` 以 `heightmap_cache_tiles` 建立 provider，优先于 sidecar `node_bounds` 计算精确 `zmin/zmax`，并在相机移动超过 0.1m 时 bilinear 采样 `ground_height`（原 `TerrainLodController::update` 行为）。GPU smoke 在 D3D11/D3D12/Vulkan 读取包内 2048² heightmap：`scale=0.076295`、`ground=442.926`、`hits=113 misses=113 tiles=113 evictions=49`，LOD 结果保持 `leaves=85 visible=67 bound=341`；新增 2 个 cache 单元测试和 heightmap controller 测试。限制：CPU cache 只服务 LOD bounds/ground；shared-grid 顶点变形由 `TerrainLodController` 在 CPU 上额外生成，不是原 GPU shader displacement，也没有 GPU tile array；material tiles、逐实例 vegetation GPU instancing、GPU FFT 纹理和 GPU river particles/refraction 仍需后续移植；CPU river mesh/terrain carve、material 纹理、逐帧 wave normal、屏幕空间 Poisson foam 播种和 CPU foam 逐帧动画已支持；1km mesh + 500m submesh/frustum/5000m 距离剔除及 near/far LOD 已由 `VegetationController` 支持。
- [x] Shared-grid CPU 顶点变形：当 sidecar 含 `terrain_coarse_mesh`/`terrain_fine_mesh`（原 `fractal_lod.gltf` 路径）时，`TerrainLodController` 按原 `add_terrain_node` 的 33/65 grid、world-space UV、heightmap 双线性高度和 `skirt_depth` 语义，为每个 `TerrainLOD_{level}_{tx}_{ty}` 节点生成独立的 world-space mesh；原共享的 `TerrainSharedGrid33/65` 模板只作为材质来源，不修改 hdrp-bgfx shader。`scripts/run-r06-shared-terrain-smoke.ps1` 从原始 `fractal_lod.gltf`/`fractal_lod.terrain.json`/`heightmap.bin` 构建 19 assets、3 bundles、96264476 bytes 包；三后端均为 `leaves=85 visible=67 bound=341`、`terrain_shared=meshes=341 vertices=1460841 triangles=2833664 coarse=33 fine=65 fine_from=3`、`terrain_heightmap=2048x2048 scale=0.076295 ground=442.926 hits=113 misses=113 tiles=113 evictions=49`；D3D11 `objects=344 draws=162 passes=26 triangles=1200580 textures=64`，D3D12/Vulkan triangles 分别为 1200560/1200580，截图 `build/proland_shared_terrain_smoke_d3d11.png`。限制：这是 CPU 烘出的每节点 mesh，仍需额外 1.46M 顶点显存；原 GPU shader displacement、GPU heightmap tile array、material tiles、逐实例 vegetation、GPU FFT 纹理和 GPU river particles/refraction 未移植；CPU water FFT、CPU river mesh/terrain carve、material 纹理、逐帧 wave normal、屏幕空间 Poisson foam 播种和 CPU foam 逐帧动画已支持。
- [x] CPU water FFT：新增 `parcel_hbg::WaterSurface`，按原 `OceanFFT` 的 TMA/JONSWAP 频谱、4 级 grid（5488/392/28/2m）、逆 FFT、mip 采样和原 `applyToVertices`/`applyToScreenGrid` 的高度/斜率/choppy 语义，并通过原 `WaterScreenGrid`（8px、1.25 overscan）和 ocean-sphere ray grid 在 CPU 上更新 `Water` mesh；通过给 water mesh 附加 `WaterFFT` morph target、由未修改的 hdrp-bgfx 公共 morph 路径逐帧上传 dynamic vertex buffer，不修改 hdrp-bgfx。三后端 smoke 均为 `water_fft=active mesh=0/2 vertices=8750 updates=1 morph_updates=1 screen_grid=1 grid=125x70`；OceanSphere 当前为 `draws=28 passes=26`，baked terrain 为 `draws=468 passes=26`，shared terrain 为 `draws=162 passes=26`。新增 `test_water_surface.cpp` 覆盖 morph 绑定、首帧位移、12Hz 节流和缺失 Water/空 mesh 错误。限制：当前由 Parcel 在 CPU 上生成屏幕网格和 FFT 顶点；GPU FFT 纹理未移植。
- [x] CPU river mesh/terrain carve：新增 `parcel_hbg::RiverScene`，按原 `RiverGraph::meander`、`HydroFlow`、`buildRiverMesh` 和 `computeRiverTerrainDelta`/`applyRiverTerrainDelta` 语义生成 320×12 水面 ribbon、采样 flow speed，并对 `RiverTerrain` mesh 做 CPU 河床/河岸雕刻与法线/切线重建；`hbg_gpu_smoke` 自动发现包内 `.hdrp.json` 的 `river` 对象，`scripts/run-r06-river-smoke.ps1` 导入/打包原始 `river.gltf`/`river.bin`/`river.hdrp.json`（5 assets、2 bundles、11075479 bytes）。三后端均为 `river=active vertices=4173 indices=23040 flow_samples=319 speed=[0.0188,0.0226]`、`river_terrain=mesh='RiverTerrain' vertices=66049 changed=6166 delta=[-5.060,2.550]`、`river_texture=size=256 base=0 normal=1 frames=8`、`river_normal_frame=frame=0/8 time=0.100 step=0.500`、`river_foam=particles=512 vertices=2048 indices=3072 texture=64 image=9 fade=[0.000,1.000]`、`river_foam_update=updates=6 time=0.100 max_delta=1613.910 fade=[0.000,1.000]`、`river_foam_screen=particles=482`；D3D11 `objects=3 draws=31 passes=27 triangles=271892 textures=76`（D3D12/Vulkan triangles 分别为 271872/271892），截图 `build/proland_river_smoke_d3d11.png`。纹理按原 `RiverParticleSystem` 的 `buildWaveFrames`/`buildBaseColorPixels`/`buildBaseNormalFrame` 语义生成 256² 基础色和 `waveTimeLoop=8` 帧 wave normal map；每帧 normal 使用独立的 `RiverNormalFrame*` image、material 和 `RiverSurface.Frame*` mesh，`RiverScene::Update` 按原 `update` 的 `time / waveTimeStep` 规则切换 `RenderObject.meshIndex`，从而在未修改 hdrp-bgfx 的情况下实现逐帧 wave normal 动画（smoke 中 `waveTimeStep=0.5`，31 帧验证 `frame=1/8`）。另按原 `RiverParticleSystem` 的 `riverLifecycleFade`、flow/speed、粒子容量和屏幕空间粒子语义生成 512 个 CPU foam particles；`RiverScene::Update` 接收 `RiverView`（view-proj、inv view-proj、camera、viewport），按原 `ScreenParticleLayer`/`buildWaveFrames` 的屏幕空间思路，将 river 可见屏幕包围盒内自适应 Poisson-disk 样本映射到水面并过滤进河流，首帧以 `river_foam_screen=particles` 输出；之后每帧 project 到屏幕、越界或离开河流时按屏幕重新 respawn，并通过预采样 flow field 推进、驱动 `RiverFoamFlow` morph target。`RiverFoam` 使用 64² alpha-test Lit overlay（1024 个三角形、`kRenderQueueAlphaTest`、alphaCutoff 0.2）；受 bgfx 静态 buffer 限制，原 `maxParticles=5000` 的屏幕粒子云在此缩减到 512 个世界空间 foam quad。原始 depth snapshot、屏幕空间 Poisson 邻域网格、depth/normal splat 和屏幕对齐 sprite 仍未移植；alpha-test cutout 而非 Blend 是因为 hdrp-bgfx 透明队列 + dynamic morph 会泄漏一个 uniform。新增 `test_river.cpp` 覆盖 ribbon/flow/terrain carve/texture generation/foam overlay/lifecycle/morph update/wave normal frames/screen-space foam seed/sidecar 解析（10 cases / 590 assertions）。限制：wave normal 帧在 Apply 时预生成，并由每帧 mesh/material 切换呈现，不是 GPU texture array 逐帧上传；基础 hdrp-bgfx 没有 river water shader，当前用半透明 Lit 材质近似水面，未移植 GPU river particles、refraction/absorption/reflection 和 GPU river 变形；foam 为 alpha-test cutout，screen-space 分布仍是世界空间 quad，没有 depth snapshot/Poisson 邻域/depth-normal splat/屏幕对齐 sprite，soft blending 待 hdrp 修复。
- [x] 地形 LOD 选择：移植原 `TerrainLodController`/`TerrainQuadTree` 的 `splitDistance`、frustum、horizon culling、child order 和节点名匹配语义到 `parcel_hbg::TerrainLodController`；`parcel_hbg_gpu_smoke` 从包内同名 `TextAsset` 自动读取 `.terrain.json`，对 `TerrainLOD_{level}_{tx}_{ty}` 对象做可见性与排序更新。用原 `gen_terrain_lod.py --baked-terrain --no-trees` 生成 341 节点 baked 资产（356 assets、3 bundles、196413756 bytes），D3D11/D3D12/Vulkan 800x450 均得到 `leaves=85 visible=67 bound=341`，D3D11 `objects=1823 draws=468 passes=26 triangles=1275816 textures=64`，截图 `build/proland_terrain_smoke_d3d11.png`；单位测试覆盖 split/merge、动态相机、node bounds/frustum、water-only 和 malformed sidecar。限制：baked 资产仍走静态几何；带 `terrain_coarse_mesh`/`terrain_fine_mesh` 的 shared sidecar 已走 CPU 变形路径；GPU shader 顶点变形、terrain material tiles、逐实例 vegetation GPU instancing、GPU FFT 纹理和 GPU river particles 仍未移植；CPU water FFT、CPU river mesh/terrain carve、material 纹理、逐帧 wave normal、屏幕空间 Poisson foam 播种和 CPU foam 逐帧动画已支持。
- [x] Vegetation tile culling：`parcel_hbg::BuildVegetation` 解析包内 `trees.json` TextAsset，按原 `TerrainLodController` 的 species/position/scale/rotation 数据和原 `buildVegetationTiles` 的 tile 语义，将 60000 棵树合并为 species/tile mesh（默认 1km tile，使用包内 `tree_deciduous.png`/`tree_conifer.png` 与 alpha mask）；新增 `VegetationController::Bind/Update`，对每 tile 做 frustum + 5000m `distanceToBounds` 剔除，并以 `meshIndex=UINT32_MAX` 切换可见性。三后端 smoke 均为 `vegetation=60000 deciduous=30892 conifer=29108 meshes=1481 triangles=240000 textures=2 objects=1481 tiles=1273 submeshes=5003`，`vegetation_visibility=objects=1481 tiles=1273 submeshes=5003 visible=53 visible_tiles=27 visible_submeshes=153 frustum_culled=1014 frustum_culled_submeshes=32 distance_culled=414 distance_culled_submeshes=8`；D3D11 总计 `objects=1823 draws=468 passes=26 triangles=1275816 textures=64`。限制：bgfx 静态 vertex/index buffer 上限为 4096，无法为 60k 每实例对象各建 buffer；原 `vegetation_tile_size=500` 在 smoke 中提升为 1km；500m submesh 第二阶段剔除及 near/far LOD（默认 1500m 外单面树卡）已支持，逐实例第二阶段剔除、GPU imposter 和 3D atlas 动画仍未移植。


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
SettingsBridge、Shader 物化与纯字段测试已通过。`parcel_hbg_gpu_smoke` 已在
Windows/D3D12/D3D11/Vulkan 上用真实 bgfx Device 完成合成 Scene → `ForwardRenderer` 渲染，
并已通过 `--package` 加载 glTF→import→pack 生成的真实 PRCA/Bundle 场景；验证了完整
mip 链、BC7、RGBA16F 上传、draw call 和 GPU 资源释放。包内 ShaderLibraryAsset
经 `ShaderLibraryBootstrap` 内容寻址物化（跨版本 blob 复用，硬链接/复制回退）后驱动未修改的 `ShaderLibrary`，在 D3D12/D3D11
（dxbc）和 Vulkan（spirv）完成 Forward program 创建与渲染。10000 个共享静态网格实例
的 D3D12 smoke 已通过；Proland 60067-node 真实资产已完成导入/打包和
D3D12/D3D11/Vulkan smoke，bridge 不再按对象复制 mesh/material GPU 资源。相对原
hdrp render-test 的帧预算已通过（D3D11 1080 三组配对中位数：0.967，
draw/pass/triangle 完全一致）；该压力场景不设绝对 60 fps 门禁，轻量
`fractal_lod.gltf` 场景的 D3D11 800x450 绝对 60 fps 已另行通过。OceanSphere
和 river 场景已完成导入/打包和 D3D11/D3D12/Vulkan smoke；baked terrain LOD
选择、CPU water FFT、CPU river mesh/terrain carve、river material 纹理、逐帧 wave normal、屏幕空间 Poisson foam 播种和 CPU foam 逐帧动画已支持，
GPU shader 顶点变形、GPU FFT 纹理和 GPU river particles/refraction 仍未覆盖。

## 估算

约 4-5 周（1-2 人）。













