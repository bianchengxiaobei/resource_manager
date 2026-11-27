# 10 里程碑与验收

## 估算前提

- 2-3 名熟悉 C++/Go 的工程师并行。
- Windows 首期；hdrp-bgfx 作为未来渲染器；不修改 hdrp-bgfx。
- 总计约 **28-35 周到 M4**；单人全栈约 12-18 个月。

## M0 基础骨架（约 1 周）

状态：已完成（2026-11-20）。

交付：

- 独立仓库/目录、CMakePresets、目录结构。
- `parcel_core`：Result/Error、日志、hash、路径、线程池。
- Operation/Scheduler 骨架。
- 单元测试框架。
- 文档索引。

验收：

- Windows Debug/Release 构建通过。
- 调度/取消/时间片/子操作环检测测试通过。
- no-exceptions/no-rtti 构建通过。

## M1 格式 + 资产 + 导入 + 打包（约 8-10 周）

状态：已完成（R01/R02/R03/R04 完成；R04 加密为明确延期项，TOML/archive bundle 已实现）。

交付：

- PRCA/PRCL/PRCM 编解码 + 版本化 metadata + BlobTable。
- Texture/Mesh/Material/Shader/Scene/Settings schema；组件 PRCS v1。
- `.parcelmeta`、ImportDB、FBX/assimp 与 glTF/GLB 导入。
- 纹理 PNG/TGA/JPG/HDR、RGBA16F/32F 与 BC1/BC3/BC5/BC7 编码。
- ShaderLibraryAsset 打包。
- Collector/依赖/Bundle/Manifest/Report（含 JSON/TOML、glob、archive raw、PackRule/FilterRule/group、outputNameStyle/buildBundleType）。
- `parcel_import` / `parcel_pack` CLI。

验收：

- 一个 FBX + 纹理 + `.parcel.json` 可导入、打包。
- 重复构建字节级一致。
- 损坏/越界/版本不支持输入全部拒绝。
- 构建报告包含大小/依赖/告警。

## M2 Runtime 核心（约 5-6 周）

状态：进行中（R05 完成，含 Auto/EditorSimulate/Offline/Host 运行时模式与松散 cooked PRCA 直读；R06 Runtime Scene 生命周期、Light cookie/IES 采样与 EnTT Scene 加载完成，合成/真实包/Proland 真实资产 GPU 端到端 smoke 已通过；R07 已提供下载修复）。

交付：

- AssetSystem/ResourcePackage/ResourceManager/Provider/Handle/BundleLoader。
- Builtin/Sandbox FS、缓存 sidecar、验证/清理。
- PRCA 反序列化、版本化 metadata/BlobTable、IObjectResolver。
- Full API 表面：LoadAsset/Sub/All/Raw/BundleFile/Info/Unload/多 Package。
- `parcel_entt` + SceneAsset 加载；`ResourcePackage::LoadScene`/`UnloadScene` 实例化与销毁。
- 基础诊断。

验收：

- 1000 资源随机释放无泄漏。
- 同步/异步结果一致。
- Scene 加载到 EnTT registry，组件字段正确。
- 缓存损坏可检测/修复。
- LoadingLock/UnloadAll/DestroyPackage 测试通过。

## M3 热更 + 服务端 + Vue（约 8-10 周）

状态：主体完成（R07 客户端/下载/Manifest/灰度、R08 Go 服务端/Vue/CLI/OpenAPI/Docker 文件、WinHTTP 真实网络、Playwright 管理端 smoke（登录/建包/上传/发布/回滚/灰度创建编辑删除/审计分页/指标/登出）和 Windows nginx TLS 代理 smoke 已完成；Docker 容器启动和大规模压力测试延后）。

交付：

- 版本/Manifest 缓存 + hash 校验。
- Downloader：并发/续传/重试/watchdog/原子安装/失败列表。
- 按需下载、缓存修复、清理。
- CDN 多 URL、Token、签名 URL、灰度、回滚客户端。
- Go 服务端：SQLite/本地存储、包/版本/渠道/灰度/回滚/审计/指标。
- Vue 管理端：登录/包/版本/上传/灰度/回滚/审计。
- Docker + nginx TLS + 备份。

验收：

- 首装、增量更新、回滚、断点续传、404/500/超时/磁盘满全部覆盖。
- 多 CDN 失败切换；401 刷新；签名 URL 过期重签。
- 灰度百分比/白名单/平台/App 版本正确；回滚生效。
- 管理端完成上传/发布/灰度/回滚闭环。

## M4 hdrp-bgfx 适配 + 运行时扩展（约 6-8 周）

状态：进行中（R06 CPU 适配/Scene/Shader/Settings、节点动画、蒙皮与 morph 主体完成；R09 硬化基线已开始；合成与真实 Parcel 包资产的 ForwardRenderer GPU smoke 已在 Windows/D3D12/D3D11/Vulkan 验证，包内 ShaderLibraryAsset 经 Bootstrap 内容寻址物化（blob 跨版本复用，硬链接/复制回退）后已驱动真实 dxbc/spirv ShaderLibrary，10000 静态实例已在 D3D12 验证，Proland 60067-node 和 OceanSphere 真实资产已完成导入/打包/D3D12/D3D11/Vulkan smoke；相对原 hdrp render-test 的帧预算和轻量 fractal_lod 场景绝对 60 fps 门禁已通过；baked terrain LOD 选择、heightmap tile cache 的精确 node bounds/ground、CPU shared-grid 顶点变形、1km mesh + 500m submesh 的 60k vegetation frustum/5000m 距离剔除及 near/far LOD、CPU water FFT、CPU river mesh/terrain carve、river material 纹理、逐帧 wave normal、屏幕空间 Poisson foam 播种和 CPU foam 逐帧动画已支持，GPU shader 顶点变形、material tiles、逐实例 vegetation GPU instancing、GPU FFT 纹理和 GPU river particles/refraction 未覆盖）。

交付：

- `parcel_hbg`：AssetMaterializer、ParcelMaterialSystem、Shader 物化、SceneRenderBridge（节点层级/动画 channel/蒙皮/morph weights）、SettingsBridge。
- Audio/Physics/Script 运行时接入（按选定 backend）。
- Meshoptimizer/纹理优化；性能/安全/fuzz。
- 完整诊断/泄漏报告。

验收：

- Parcel Scene 能驱动 hdrp-bgfx `RenderScene` 渲染（合成场景与真实 Parcel 包资产在 D3D12/D3D11/Vulkan GPU smoke 已验证，10000 静态实例在 D3D12 已验证，Proland 60067-node 真实资产三后端已验证）。
- BC7/HDR/mip 纹理正确；ShaderLibrary 正常加载（GPU smoke 已验证；包内 ShaderLibraryAsset 经 Bootstrap 内容寻址物化（blob + 硬链接/复制回退）后已驱动未修改的 dxbc/spirv ShaderLibrary）。
- 材质/灯光/相机/Decal/Probe/Fog 字段对齐；节点动画映射到 RenderScene.animations，
  蒙皮 joint/逆绑定矩阵映射到 RenderScene.skins；morph targets 与 Weights 通道映射到 mesh/object/node weights。
- ASan/TSan（可行时）通过；目标规模性能达标（10000 静态实例结构验证已通过，Proland 60067-node 真实资产已完成 D3D12/D3D11/Vulkan smoke；相对原 hdrp render-test 的帧预算和轻量 fractal_lod 绝对 60 fps 已通过，其他项目资产待测）。

## R09 稳固/发布（进行中）

已完成基线：

- PRCA/PRCL/PRCM 独立 fuzz 目标 + golden smoke corpus。
- PRCM/PRCA/PRCL 安全边界测试；服务端并发上传/下载测试。
- Runtime LoadingLock/UnloadAll/Destroy 与并发 Load/Release 压力测试。
- MSVC ASan 全量测试通过；CMake UBSan/TSan 选项。
- Release 性能基准 `parcel_bench` 与 `docs/13-benchmarks.md`。
- `docs/12-security-review.md`、`THIRD_PARTY_NOTICES.md`、`CHANGELOG.md`。
- `examples/hello-package` 全链路示例与 `scripts/package-release.ps1`。

待补：TSan 实机（Windows Clang 不支持，需 Linux/CI）、真实 CDN 边缘签名/鉴权流程、
Docker 容器启动。WinHTTP loopback smoke 已通过 `scripts/run-winhttp-smoke.ps1`；
Cloudflare TLS/连接复用/吞吐、GitHub raw 完整安装、真实服务端签名 URL（含
403/401/过期）、Playwright 管理端和 Windows nginx TLS 代理 smoke 已通过
`scripts/run-cdn-smoke.ps1` / `scripts/run-cdn-bundle-smoke.ps1` /
`scripts/run-signed-url-smoke.ps1` / `scripts/run-webui-smoke.ps1` /
`scripts/run-nginx-smoke.ps1`。

## M5 Editor（后续）

- Collector UI、构建面板、报告/Debugger、导入设置编辑。
- 复用 `parcel_import`/`parcel_pack` 核心。

















