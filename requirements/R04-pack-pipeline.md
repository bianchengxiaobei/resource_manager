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

- [x] 收集配置 schema（JSON/TOML；TOML 使用顶层键值 + `[[collectors]]`）。
- [x] Collector `include`/`exclude` glob（`*`、`**`、`?`、字符类）。
- [x] MainAssetCollector/StaticAssetCollector/DependAssetCollector。
- [x] Collector `packRule`/`filterRule`/`group`；支持 YooAsset 风格规则名。
- [x] FilterRule：按 cooked 类型过滤；多 collector 命中时过滤失败回退下一个 collector。
- [x] PackRule：Separately/Directory/TopDirectory/Collector/Group/RawFile/VideoFile/Shader；
      ShaderVariants 和不适用的 FilterRule 返回 `Unsupported`。
- [x] 地址规则：Address/AssetPath/FileName/GUID/Prefix。
- [x] 标签规则。
- [x] 打包规则：目录/显式列表/共享资源策略。
- [x] `.parcelmeta` 源路径映射（`source`/`useSourcePath`）。
- [x] Main+SubAsset 按 `sourceGuid` 强制同 Bundle（`forceSubAssetGrouping`）。
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
- [x] `outputNameStyle`：HashName/BundleName/BundleName_HashName；重名检测。
- [x] `buildBundleType`：AssetBundle/Archive；RawFile 明确返回 `Unsupported`。
- [x] 共享 Bundle 去重。
- [x] Archive bundle：collector `archive=true` 将多个 RawFileAsset 原始 bytes 写入
  PRCL TOC `raw_files`；Manifest asset flag `ArchiveRaw`，运行时 `LoadRawFile` 读取。
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
- [x] 构建前后 hash 对比（可选；`verifyOutput` / `--verify-output`，校验 manifest、version、Bundle、catalog）。

### 增量/确定性

- [x] BuildDB：资产 hash/元数据/统计/文件 stat + Bundle 成员 + 工具版本。
- [x] 文件级增量：未变化资产跳过重读/解析，Bundle 重建时按需校验加载 payload。
- [x] 未变化 Bundle 复用；Manifest 总重建。
- [x] 稳定排序/GUID；Manifest/Bundle 无时间戳；字节级可复现。
- [x] 清理旧输出（`--clean`）。

### CLI

- [x] `parcel_pack --project --package --version --target --source --cooked --output --config`。
- [x] 并行构建 Bundle payload；进度/日志由 stdout 输出。
- [x] 退出码/报告路径。

## 验收标准

- [x] 一个 cooked 目录可打包出 Bundle + Manifest + version/hash。
- [x] Manifest 可被 `serialize::ManifestView`/`LoadManifestFile` 加载；依赖图正确。
- [x] 重复打包字节级一致；增量构建跳过未变化 Bundle 和未变化资产。
- [x] 循环依赖/重复地址/缺失依赖构建失败并有清晰报告。
- [x] 共享资源不会重复打包（可配置）。
- [x] 报告可用于定位体积/依赖问题（资产类型、纹理格式、Shader、未使用/重复/零引用）。

## 估算

约 3-4 周（1-2 人）。

## 实施状态

完成（2026-11-22）。

- 新增 `parcel_pack` 静态库、`parcel_pack` CLI（CMake target `parcel_pack_cli`）。
- 配置 schema：`schemas/parcel.pack.schema.json`；CLI 支持 JSON/TOML，TOML 使用
  顶层键值 + `[[collectors]]`；collector 支持 main/static/depend、
  directory/explicit files/include/exclude glob、assetPath/fileName/guid/prefix/address、
  tags、bundle、share、archive、group、packRule、filterRule。
- 依赖来自 PRCA ExternalRefTable；支持缺失依赖、重复 GUID、重复地址、循环检测、
  Depend 零引用剔除、共享 Bundle、Shader/Raw 独立 Bundle。
- Bundle GUID/文件名内容寻址；payload 8 字节对齐；LZ4/Zstd 可选；Bundle payload
  按 `jobs` 并行编码；未变化 Bundle/资产依据 `build_db.json` 复用，未变化资产统计直接复用。
- 输出 `<output>/<target>/<package>/<version>/`：`bundles/<guid>.bundle`（或
  `outputNameStyle=2/3` 的名称）、`manifest.bytes`、`manifest.hash`、
  `<package>.version`、`build_report.json`、`build_db.json`、`builtin_catalog.bin`。
- `outputNameStyle` 实际驱动 Bundle 文件名，并对路径分隔符/非文件名字符做确定性替换、
  检测重名；`buildBundleType=3` 强制 RawFileAsset archive，`2` 返回 `Unsupported`。
- Manifest 只包含 Main 资产；Static/Depend 参与 Bundle 但不可通过 AssetList 加载。
- Archive raw：`archive=true` collector 将 RawFileAsset 的原始 bytes 写入 PRCL TOC
  `raw_files`，raw 名取 Manifest assetPath/address；Main archive raw 的 Manifest
  asset flags bit 0 标记 `ArchiveRaw`，`payload_id/local_object_id=0`；Runtime
  `LoadRawFile` 从 TOC 读取并构造 `RawFileAsset`。Archive 模式参与 Bundle GUID，
  切换 archive 会触发重建。
- `build_report.json` 包含资产/ Bundle 明细、资产类型、纹理格式、Shader
  variant/backend、未使用资产、零引用、重复内容、内建目录字节数、输出校验状态和 warnings/errors。
- 可选输出校验：写入后重读 manifest/hash/version/bundle/catalog，校验大小、SHA-256、CRC32 与 catalog 条目；可通过 `verifyOutput`/`--verify-output` 开启，`VerifyPackOutput()` 也可单独调用。
- 测试：Pack JSON/TOML 配置解析、确定性输出、共享 Bundle、Bundle/文件级增量复用、输出 hash 校验、collector 包含/排除 glob、
  packRule/filterRule 分组与类型过滤、多 collector 过滤回退、outputNameStyle 三种文件名、
  buildBundleType Archive/RawFile 校验、缺失依赖、循环依赖、
  Raw 多文件、archive raw bundle、None/LZ4/Zstd、Shader 自动收集、BuiltinCatalog 编解码/确定性、
  `.parcelmeta` 源路径映射/Main+SubAsset 强制组包。
  当前全量测试 284 cases / 15730 assertions（Release 282 / 13072）。

实现约定：

- cooked 文件名 `<guid>.prca` 是 GUID 权威来源；Pack 可扫描 `source`（默认 project
  root）下 `.parcelmeta`，将主资产 Manifest `asset_path`/`assetPath` 地址映射为源相对路径；
  `useSourcePath=false` 时回退 cooked 相对路径。
- 同 `sourceGuid` 的资产强制进入同一 Bundle（`forceSubAssetGrouping` 可关闭）；显式
  `bundle` 冲突或 Raw 与其它资产同组会失败。
- `build_db.json` 为可读 JSON（非二进制），记录工具版本、Bundle 成员、file hash，以及资产的 path/stat/hash/类型/依赖/报告统计；stat 用于文件级增量失效判断。
- 加密留到后续加固/扩展；文件级增量、可选输出 hash 校验、collector glob、TOML 配置、
  archive bundle、packRule/filterRule/group、outputNameStyle/buildBundleType 已完成。














