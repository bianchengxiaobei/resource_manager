# Requirements 执行索引

## 依赖关系

```text
R00 基础骨架
  └─ R01 序列化与格式
       └─ R02 资产与 Scene/ECS Schema
            ├─ R03 导入管线
            │    └─ R04 打包管线
            └─ R05 Runtime 核心
                 ├─ R06 hbg 适配 + EnTT
                 └─ R07 热更 / CDN / 灰度
                      └─ R08 Go 服务端 + Vue
                           └─ R09 稳固 / 测试 / 发布
```

## 建议顺序

1. **R00**：先建立可构建、可测试的骨架。
2. **R01**：格式是所有资产/打包/运行时的基础。
3. **R02**：冻结资产、组件、Scene schema。
4. **R03**：FBX/assimp + glTF/GLB 导入，产出 PRCA。
5. **R04**：Collector/Bundle/Manifest，产出可分发产物。
6. **R05**：Runtime 核心，跑通单资源/Scene 加载。
7. **R06**：EnTT + hdrp-bgfx 适配，跑通渲染。
8. **R07**：热更/下载/CDN/鉴权/灰度。
9. **R08**：Go 服务端 + Vue，端到端分发。
10. **R09**：稳定性/性能/安全/发布。

R03/R04 可与 R05 部分并行；R08 可在 R07 协议冻结后并行。

## 完成定义（DoD）

每个需求完成必须满足：

- 任务清单全部勾选，或明确记录延期原因。
- 验收标准全部满足。
- 单元/集成测试通过。
- 文档与实现一致（格式/schema/API）。
- 无 Hobi 依赖、无 hdrp-bgfx 源码修改。
- 错误码/日志可定位。
- 代码通过 CMake/CI 构建。

## 状态

| 需求 | 状态 |
|---|---|
| R00 | 已完成 |
| R01 | 已完成 |
| R02 | 已完成（EnTT concrete loader 延至 R06；AnimationClip/Player/Skin/Morph 已补） |
| R03 | 完成（BC1/3/5/7、meshoptimizer、shader build、纹理跨源复用、sidecar 组件映射、FBX 节点动画/蒙皮/morph、glTF/GLB、KHR_texture_transform 共享 UV0/逐贴图纹理变体） |
| R04 | 完成（加密延后；TOML、BuiltinCatalog、archive bundle 已实现） |
| R05 | 完成（Scene 生命周期、BuiltinCatalog、footprint/操作计时、Unpacker/Importer；下载修复由 R07 提供） |
| R06 | 主体完成（Runtime Scene 生命周期、节点层级、动画 channel、蒙皮、morph、Light cookie/IES 采样完成；合成/真实 Parcel 包资产的 bgfx/ForwardRenderer GPU smoke 已在 Windows/D3D12/D3D11/Vulkan 验证，包内 ShaderLibraryAsset 经 Bootstrap 内容寻址物化（blob 跨版本复用，硬链接/复制回退）后驱动真实 dxbc/spirv 库，10000 静态实例在 D3D12 验证，Proland 60067-node 和 OceanSphere 真实资产完成导入/打包及三后端 smoke；相对原 hdrp render-test 的帧预算和轻量 fractal_lod 场景绝对 60 fps 门禁已通过；baked terrain LOD 选择、CPU heightmap tile cache（精确 node bounds/camera ground）、CPU shared-grid 顶点变形、1km mesh + 500m submesh 的 60k vegetation frustum/5000m 距离剔除及 near/far LOD、CPU water FFT、CPU river mesh/terrain carve、river material 纹理、逐帧 wave normal、屏幕空间 Poisson foam 播种和 CPU foam 逐帧动画已支持，GPU shader 顶点变形、material tiles、逐实例 GPU instancing/imposter、GPU FFT 纹理和 GPU river particles/refraction 未覆盖） |
| R07 | 主体完成（WinHTTP loopback + 真实 CDN/TLS 传输与完整安装 smoke + watchdog + 每帧配额 + 共享调度 + SignedUrlProvider + 真实服务端签名 URL smoke；真实 CDN 边缘签名/鉴权待补） |
| R08 | 主体完成（Docker 容器启动、大规模压力测试延后；Playwright 管理端和 Windows nginx TLS 代理 smoke 已通过；包/版本重命名/删除与 CLI 灰度更新/删除已补） |
| R09 | 进行中（fuzz/压力/ASan/基准/安全/发布基线完成；1h 长跑 + 格式/字符串 30 分钟 fuzz + 真实 CDN/TLS 传输/完整安装 + 真实服务端签名 URL smoke 已完成；TSan 需 Linux/CI，真实 CDN 边缘签名/鉴权待补） |

## 使用方式

- 每个需求文档是一个可独立验收的工作包。
- 实现时按任务清单顺序推进，完成一项勾选一项。
- 发现设计冲突时，先更新 `docs/`，再更新对应需求文档，最后改代码。
- 任何格式/schema 变更必须同步更新 `docs/02-format.md`、`docs/03-assets-scene.md`
  与相关测试。

























