# R04 打包管线

## 目标

实现 Collector、依赖分析、Bundle 分配、PRCL/PRCM 生成、构建报告、增量/确定性打包和 `parcel_pack` CLI。

## 前置依赖

R01、R02、R03。

## 范围

- `parcel_pack` 库 + CLI。
- Collector 规则（main/static/depend、地址、标签、打包规则）。
- 依赖分析（资产→Bundle）。
- Bundle 分配/共享资源/零引用剔除。
- Manifest/版本/hash 输出。
- 构建报告、增量、确定性。

## 任务清单

### Collector

- [x] 收集配置 schema（JSON；TOML 延后）。
- [x] MainAssetCollector/StaticAssetCollector/DependAssetCollector。
- [x] 地址规则：Address/AssetPath/FileName/GUID/Prefix。
- [x] 标签规则。
- [x] 打包规则：目录/显式列表/共享资源策略。
- [x] 收集结果模型 + 错误/告警。

### 依赖分析

- [x] 从 AssetFile ExternalRefTable 构建资产依赖。
- [x] 资产 → Bundle 依赖图。
- [x] 环检测；缺失依赖；重复 GUID。
- [x] 零引用 DependAsset 剔除。
- [x] 共享资源独立 Bundle/并入引用者策略。
- [x] Shader/Raw 独立 Bundle 规则。

### Bundle 构建

- [x] PRCL BundleBuilder：TOC/BlockTable/压缩/hash（复用 R01 WriteBundle）。
- [x] 资源数据 logical stream 布局。
- [x] 内容寻址 BundleGuid/文件名。
- [x] 共享 Bundle 去重。
- [x] 压缩选项（None/LZ4/Zstd）与块大小。
- [ ] 加密 hook（延后；PRCL v1 无加密字段）。

### Manifest

- [x] PRCM ManifestBuilder：Asset/Bundle/Tag/Dependency。
- [x] `manifest.bytes` / `manifest.hash` / `<package>.version`。
- [x] `builtin_catalog.bin`：Bundle GUID/FileName、package/version；默认生成，`writeBuiltinCatalog=false` 可关闭。
- [x] Manifest flags（addressable/extensionless/lowercase/guid）。
- [x] 输出目录 `target/package/version`。

### 报告/校验

- [x] BuildReport JSON：总大小、Bundle 大小/资源数/依赖、重复资源、未使用资源、零引用、告警/错误、Shader 变体、纹理格式。
- [x] 构建失败非零退出码。
- [x] 校验：重复地址/GUID、循环依赖、缺失依赖、路径控制字符、Raw 多文件、空 Bundle。
- [ ] 构建前后 hash 对比（可选，延后）。

### 增量/确定性

- [x] BuildDB：资产 hash + Bundle 成员 + 工具版本。
- [x] 未变化 Bundle 复用；Manifest 总重建。
- [x] 稳定排序/GUID；无时间戳；字节级可复现。
- [x] 清理旧输出（`--clean`）。

### CLI

- [x] `parcel_pack --project --package --version --target --cooked --output --config`。
- [x] 并行构建 Bundle payload；进度/日志由 stdout 输出。
- [x] 退出码/报告路径。

## 验收标准

- [x] 一个 cooked 目录可打包出 Bundle + Manifest + version/hash。
- [x] Manifest 可被 `serialize::ManifestView`/`LoadManifestFile` 加载；依赖图正确。
- [x] 重复打包字节级一致；增量构建跳过未变化 Bundle。
- [x] 循环依赖/重复地址/缺失依赖构建失败并有清晰报告。
- [x] 共享资源不会重复打包（可配置）。
- [x] 报告可用于定位体积/依赖问题（资产类型、纹理格式、Shader、未使用/重复/零引用）。

## 估算

约 3-4 周（1-2 人）。

## 实施状态

完成（2026-11-22）。

- 新增 `parcel_pack` 静态库、`parcel_pack` CLI（CMake target `parcel_pack_cli`）。
- 配置 schema：`schemas/parcel.pack.schema.json`；collector 支持 main/static/depend、
  directory/explicit files、assetPath/fileName/guid/prefix/address、tags、bundle、share。
- 依赖来自 PRCA ExternalRefTable；支持缺失依赖、重复 GUID、重复地址、循环检测、
  Depend 零引用剔除、共享 Bundle、Shader/Raw 独立 Bundle。
- Bundle GUID/文件名内容寻址；payload 8 字节对齐；LZ4/Zstd 可选；Bundle payload
  按 `jobs` 并行编码；未变化 Bundle 依据 `build_db.json` 复用。
- 输出 `<output>/<target>/<package>/<version>/`：`bundles/<guid>.bundle`、
  `manifest.bytes`、`manifest.hash`、`<package>.version`、`build_report.json`、
  `build_db.json`、`builtin_catalog.bin`。
- Manifest 只包含 Main 资产；Static/Depend 参与 Bundle 但不可通过 AssetList 加载。
- `build_report.json` 包含资产/ Bundle 明细、资产类型、纹理格式、Shader
  variant/backend、未使用资产、零引用、重复内容、内建目录字节数和 warnings/errors。
- 测试：Pack 配置解析、确定性输出、共享 Bundle、增量复用、缺失依赖、循环依赖、
  Raw 多文件、None/LZ4/Zstd、Shader 自动收集、BuiltinCatalog 编解码/确定性。
  当前总测试 206 cases / 5803 assertions。

实现约定：

- cooked 文件名 `<guid>.prca` 是 GUID 权威来源；Manifest `asset_path` 默认使用
  cooked 相对路径。`.parcelmeta` 源路径映射尚未接入，需要显式 Collector 地址规则
  或后续 Pack 配置扩展。
- `build_db.json` 为可读 JSON（非二进制），记录工具版本、Bundle 成员、file hash。
- 当前不做 main+subasset 强制组包；同一目录 / 显式 `bundle` 决定 Bundle 归属。
- 加密、构建前后 hash 对比、TOML 配置、复杂 glob/archive bundle 留到后续加固/扩展。














