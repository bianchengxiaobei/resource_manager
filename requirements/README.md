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
| R03 | 完成（BC1/3/5/7、meshoptimizer、shader build、纹理跨源复用、sidecar 组件映射、FBX 节点动画/蒙皮/morph、glTF/GLB） |
| R04 | 完成（加密/TOML 延后；BuiltinCatalog 已实现） |
| R05 | 完成（Scene 生命周期、BuiltinCatalog、footprint/操作计时；下载修复由 R07 提供） |
| R06 | 主体完成（Runtime Scene 生命周期、节点层级、动画 channel、蒙皮、morph、Light cookie/IES 采样完成；真实 bgfx/ForwardRenderer 端到端验证延后） |
| R07 | 主体完成（WinHTTP loopback + watchdog + 每帧配额 + 共享调度 + SignedUrlProvider；真实 CDN/TLS 待补） |
| R08 | 主体完成（Docker/WinHTTP 真实网络 smoke/Playwright/大规模压力测试延后） |
| R09 | 进行中（fuzz/压力/ASan/基准/安全/发布基线完成；1h 长跑 + 格式/字符串 30 分钟 fuzz 已完成；TSan/真实 CDN smoke 待补） |

## 使用方式

- 每个需求文档是一个可独立验收的工作包。
- 实现时按任务清单顺序推进，完成一项勾选一项。
- 发现设计冲突时，先更新 `docs/`，再更新对应需求文档，最后改代码。
- 任何格式/schema 变更必须同步更新 `docs/02-format.md`、`docs/03-assets-scene.md`
  与相关测试。

























