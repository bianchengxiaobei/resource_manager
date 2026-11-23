# 11 决策记录与风险

## 决策记录

| # | 决策 | 原因 |
|---|---|---|
| D1 | 独立项目 Parcel，不修改 Hobi | Hobi 临时；Parcel 需要独立演进 |
| D2 | 未来渲染器 hdrp-bgfx，只做适配层 | 避免将 Parcel 与渲染器耦合；hdrp-bgfx 不改 |
| D3 | Scene 直接 EnTT/ECS | 用户选择；避免 GameObject 兼容层 |
| D4 | PRCA/PRCL/PRCM 自定义格式 | 控制布局、压缩、hash、热更 |
| D5 | 版本化元数据 + BlobTable（FlatBuffers 可选） | 小数据可演进；大 buffer 零拷贝/少复制 |
| D6 | FBX/assimp 优先，glTF/GLB 已补 | 用户选择；cgltf 内置，保持外部依赖为零 |
| D7 | 纹理格式跟随 hdrp-bgfx | RGBA8/BGRA8/16F/32F/BC1/3/5/7；ASTC 后置 |
| D8 | 单个 ShaderLibraryAsset | pass/variant 多，按 backend 索引 |
| D9 | 设置独立资产 | Scene 引用，便于复用/热更 |
| D10 | parcel_entt 独立 target | Core 不依赖 EnTT |
| D11 | 物理/音频/脚本先数据后运行时 | backend 未定；避免过早绑定 |
| D12 | Go 单节点服务端 + Vue | 用户选择；先生产 MVP |
| D13 | 只做 SHA-256，不做签名/加密 | 首期范围；HTTPS 信任 |
| D14 | no-exceptions/no-rtti 核心 | 游戏引擎常见；Result/TypeId |
| D15 | CMake only | 用户要求；不改 HoBi |
| D16 | SceneHandle 共享 Provider，最后一个句柄释放时销毁场景 | 避免一个句柄卸载影响其他句柄；对齐 YooAsset 引用计数语义 |
| D17 | BuiltinCatalog 沿用 YooAsset 二进制布局（GUID/FileName、U16 UTF8） | 复用成熟格式；文件名用 `builtin_catalog.bin`，缺失时兼容旧包探测 |

## 主要风险

### R1 hdrp-bgfx 不改导致适配层复杂

- ShaderLibrary 只能从磁盘加载 → 需要物化缓存。
- MaterialSystem 只支持单 mip → 适配层复制材质构建逻辑。
- RenderScene 需要每帧/脏标记同步。
- 缓解：适配层隔离；建议未来 hdrp-bgfx 增加内存 ShaderLibrary/mip 支持。

### R2 FBX/assimp 元数据复杂

- 材质/纹理路径、坐标系、单位、相机/灯光映射。
- 缓解：`.parcel.json` 覆盖；导入报告；先支持最小场景。
- 当前 assimp 为可选本地依赖（`PARCEL_ASSIMP_ROOT`），R03 已覆盖 mesh/material/
  node/camera/light；纹理路径引用可解析到独立 texture 源，侧车 settings/camera/light 可覆盖。
- mesh/material 内容去重、纹理按内容+设置跨源复用、meshoptimizer、
  sidecar decals/probes/localFog 映射已实现。

### R3 Shader 变体与字段演进

- hdrp-bgfx pass/variant 多；Material/Light 字段可能变。
- 缓解：ShaderLibraryAsset 按 manifest 原样打包；asset schema 版本化。

### R4 纹理编码

- BC7 质量/速度；HDR 格式；mip；sRGB。
- 缓解：编码器参数写入 settings hash；HDR 不允许 BC；首期只 Windows。

### R5 物理/音频/脚本 backend 未定

- 组件 schema 可能与最终 backend 不匹配。
- 缓解：先最小字段 + 自定义扩展；运行时后置。

### R6 服务端 + Vue 范围大

- 鉴权、DB、存储、灰度、回滚、审计、监控、部署。
- 缓解：单节点 MVP；SQLite/本地；OpenAPI；Docker。

### R7 异常/RTTI 边界

- Core 禁用；适配层可能启用。
- 缓解：核心头文件不依赖异常/RTTI；适配层单独 target。

### R8 格式兼容/演进

- PRCA/Scene/Material schema 变更。
- 缓解：版本号 + 拒绝不兼容；构建报告记录版本。

### R9 合规

- YooAsset Apache-2.0 参考需 NOTICE；Unity 4.7 源码专有，不复制。
- hdrp-bgfx/bgfx/assimp/bc7enc/stb_dxt/meshoptimizer 等许可证保留。

## 待定但不阻塞

- 物理/音频/脚本具体 backend。
- ASTC 接入时间。
- Editor UI 形态。
- 服务端多节点/HA 演进。
- 是否增加 Manifest/Bundle 签名。








