# R03 导入管线

## 目标

实现从源资源到 cooked asset 的导入管线：`.parcelmeta`、ImportDB、FBX/assimp、glTF/GLB、纹理、材质、Shader、Scene sidecar、CLI。

## 前置依赖

R02。

## 范围

- `parcel_import` 库 + `parcel_import` CLI。
- `.parcelmeta`、ImportDB。
- FBX/assimp 导入。
- glTF/GLB 导入（内置 cgltf）。
- 纹理 PNG/TGA/JPG/HDR + BC1/BC3/BC5/BC7。
- ShaderLibraryAsset 打包。
- `.parcel.json` 场景合并。
- 增量与确定性。

## 任务清单

### 元数据/DB

- [x] `.parcelmeta` JSON schema + 读写。
- [x] 源 GUID/资产 GUID/子资产 GUID 生成规则。
- [x] ImportDB（二进制缓存）：source hash/settings hash/importer version/
      output hash/deps。
- [x] 事务更新/失败回滚。
- [x] 增量判断：跳过/重导。
- [x] import_report.json。

### FBX/assimp

- [x] assimp 依赖接入（`PARCEL_ASSIMP_ROOT` 可选）。
- [x] 缩放转换；assimp 输入按 RH/Y-up/CCW 约定处理（轴转换由源资产设置保证）。
- [x] 提取 mesh/vertex/index/material/texture path/node/camera/light。
- [x] 提取 FBX node animation 的 TRS 通道，生成 AnimationClipAsset 和根实体 AnimationPlayerComponent。
- [x] 提取 `aiBone` 顶点权重与 offset matrix，生成 MeshAsset.skinVertices/SkinComponent。
- [x] 提取 `aiMesh::mAnimMeshes` 生成 MeshAsset.morphTargets；`mMorphMeshChannels` 生成 Weights 通道并映射到实体。
- [x] MeshAsset 生成（含 SubMesh/bounds/attribute 标志/蒙皮顶点/morph targets）。
- [x] MaterialAsset 生成（PBR 字段/纹理引用/sampler）。
- [x] SceneAsset 生成（节点层级/Transform/Camera/Light/AnimationPlayer/Skin）。
- [x] 去重（mesh/material 内容 hash；纹理按内容+设置跨源复用 canonical TextureAsset）。
- [x] meshoptimizer 优化（顶点去重/缓存/可选 overdraw/fetch；蒙皮或 morph mesh 只重排索引）。
- [x] 错误/告警：缺失纹理、未知材质、空 mesh。

### glTF/GLB

- [x] `.gltf`/`.glb` 解析（cgltf；external/data URI buffer）。
- [x] mesh/material/node/camera/KHR_lights_punctual 导入。
- [x] glTF texture sampler（wrapS/T、min/mag/mip filter）写入 TextureRef。
- [x] `EXT_meshopt_compression`：ATTRIBUTES/TRIANGLES/INDICES 与 NONE/Octahedral/Quaternion/Exponential filter 解码。
- [x] `KHR_materials_variants`：按 `materialVariant` 设置选择变体材质，未覆盖 primitive 回退默认材质。
- [x] glTF scene 选择：`scene` 支持名称/0-based 索引，空值使用默认 scene，未声明默认 scene 时回退全部 node；仅选中 scene 可达节点创建渲染组件。
- [x] skin、morph targets、TRS/Weights 动画导入。
- [x] sidecar settings/camera/light override 与额外场景组件。
- [x] 无 skin mesh/material 内容去重；纹理 canonical GUID 复用。
- [x] embedded image（bufferView/data URI）解码与纹理子资产导入。
- [x] GLB、sidecar、完整 Import 流水线集成测试。

### 纹理

- [x] PNG/TGA/JPG/HDR 解码（内置 stb_image）。
- [x] RGBA8/BGRA8/RGBA16F/RGBA32F（非压缩格式可配置）。
- [x] BC1/BC3/BC5/BC7 编码（stb_dxt + bc7enc；compression=none/auto/bc1/bc3/bc5/bc7）。
- [x] mip 生成；sRGB 标记。
- [x] TextureAsset + BlobTable 写入。
- [x] 导入器版本/settings hash/纹理格式写入 meta/report。

### Shader

- [x] 读取 hdrp-bgfx `shaders/manifest.json` + `build/<backend>/*.bin`。
- [x] 生成 ShaderLibraryAsset（backend/pass/variant 索引）。
- [x] 校验 manifest 路径/缺失文件/重复 variant。
- [x] 可选：调用 hdrp-bgfx `shader_build.py`（外部工具，不修改 hdrp-bgfx）。

### Scene sidecar

- [x] `.parcel.json` 读取/校验。
- [x] FBX 节点 + sidecar 合并；sidecar 优先（settings/cameras/lights）。
- [x] Lights 映射到 LightComponent；Decals/Probes/LocalFogVolumes 映射到独立实体组件。
- [x] Settings 资产引用。
- [x] 错误上下文（文件/节点/字段）。

### CLI/流水线

- [x] `parcel_import` 参数/退出码。
- [x] 并行导入（std::thread worker）+ 确定性输出。
- [x] 输出目录布局 `<guid>.prca`。
- [x] 导入报告（成功/跳过/失败/告警）；CLI 输出耗时。
- [x] 编码全部成功后才提交输出；失败源不写 DB/meta。

## 验收标准

- 一个真实 FBX + 纹理 + `.parcel.json` 可导入成功。
- `.gltf`/`.glb` + 纹理 + sidecar 可导入成功，重复导入全部跳过。
- 重复导入（无变化）全部跳过；修改设置/源文件后正确重导。
- 缺失纹理/不支持格式/空 mesh 有明确错误。
- 导入输出可被 R01/R02 解析。
- ShaderLibraryAsset 包含所有 hdrp-bgfx pass/variant。
- 确定性：同一输入/工具版本输出字节一致。

## 估算

约 3-4 周（1-2 人）。

## 当前状态

主体已实现并通过单元/CLI smoke 测试：

- `.parcelmeta` + `schemas/parcel.meta.schema.json`
- 二进制 ImportDB + 原子提交 + 增量判断
- Texture（PNG/TGA/JPG/HDR + mip）、RGBA8/BGRA8/RGBA16F/RGBA32F、
  BC1/BC3/BC5/BC7（stb_dxt/bc7enc）、ShaderLibrary、Text/Raw
- assimp FBX（`PARCEL_ASSIMP_ROOT` 启用）：mesh/material/node/camera/light/animation、
  骨骼/skin 与 morph targets/Weights、meshoptimizer（顶点去重/缓存/可选 overdraw/fetch）、
  sidecar settings/camera/light override、decals/probes/localFogVolumes 组件、
  mesh/material 内容去重；纹理按内容+设置跨源复用 canonical TextureAsset
- 可选 shader build：`--shader-build-*` 调用 hdrp-bgfx `shader_build.py`，失败中止；成功后强制重导 Shader manifest
- `parcel_import` CLI、并行导入、`import_report.json`
- 真实 YooAsset FBX smoke：Material/Mesh/Scene + 1 个共享 Texture PRCA（3 个同内容纹理源复用），二次导入 skip
- glTF/GLB smoke：triangle 场景导入 mesh/material/scene/skin/morph/animation；sidecar 覆盖；GLB 二进制 buffer；embedded bufferView/data URI 图片；sampler wrap/filter 映射；KHR_materials_variants 选择/回退；EXT_meshopt_compression 解码；默认/命名/索引 scene 选择；完整 Import 流水线纹理复用与二次 skip

R03 已完成。



















