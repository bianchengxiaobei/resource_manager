# R01 序列化与格式（PRCA/PRCL/PRCM）

## 目标

实现 Parcel 的三种二进制格式、FlatBuffers 元数据、BlobTable、解析器/写入器和 fuzz 基础。

## 前置依赖

R00。

## 范围

- PRCA Asset File：对象表、类型表、FlatBuffers、BlobTable、ExternalRefTable、子资产。
- PRCL Bundle：Header/BlockTable/TOC/压缩。
- PRCM Manifest：Asset/Bundle/Tag/Dependency 表。
- 版本化、边界检查、CRC/SHA-256。
- 测试与 fuzz。

## 任务清单

### 通用

- [x] `BinaryWriter`/`BinaryReader`：小端、定长、string_view、span、边界检查。
- [x] `Hash256`/`Guid128` 类型与序列化。
- [x] 版本常量与兼容检查工具。
- [x] 文件头 magic/version/header_size 校验。
- [x] 表偏移/长度溢出检查工具。

### PRCA

- [x] Header 读写。
- [x] TypeTable：type_id/type_name/serializer_version。
- [x] ObjectTable：local_id/type_id/FlatBuffers offset+size/blob_id/flags。
- [x] ExternalRefTable：AssetGuid + flags。
- [x] SubAssetTable：name/local_object_id/flags。
- [x] BlobTable：offset/size/crc/codec/flags。
- [x] FlatBuffers verifier 集成（可选）。
- [x] 两阶段读取 API：`AssetFileReader::ReadHeader/Objects/Blobs`。
- [x] 写入 API：`AssetFileWriter`。
- [x] 校验：重复 local_id、未知 type、越界、blob 重叠、ref 非法。

### PRCL

- [x] BundleHeader 128B 读写。
- [x] BlockTable：offset/compressed/uncompressed/crc/codec/flags。
- [x] TOC：bundle_guid/name/deps/payload/raw/string table。
- [x] TOC 压缩（zstd/lz4）。
- [x] 块压缩/解压：None/LZ4/Zstd；按块读取。
- [x] 逻辑流读取 API：`BundleReader::ReadLogical(offset,size)`。
- [x] 校验：magic/version/block 越界/CRC/文件 hash。
- [x] 路径穿越/文件名非法拒绝。

### PRCM

- [x] Header 读写。
- [x] StringTable、AssetTable、BundleTable、TagTable、DependencyTable。
- [x] `ManifestView` 零拷贝：string_view + 查找表。
- [x] AssetGuid → AssetRecord、address/path/guid 查找。
- [x] BundleGuid → BundleRecord、依赖/引用列表。
- [x] Manifest hash/version 文件读写。
- [x] 校验：重复地址/GUID/Bundle 名、依赖缺失、表越界。

### 压缩

- [x] zstd 封装（级别可配、错误码）。
- [x] lz4 封装（块压缩）。
- [x] 压缩/解压单元测试 + 大 buffer。
- [x] 压缩失败/不支持的 codec 返回错误。

### 测试/Fuzz

- [x] Golden files：最小 PRCA/PRCL/PRCM。
- [x] 往返测试：写入 → 读取 → 字段一致。
- [x] 截断/越界/随机字节 fuzz（libFuzzer 或简单 fuzz harness）。
- [x] 版本不兼容拒绝。
- [x] 大表（10k 资产/1k Bundle）性能基线。

## 验收标准

- 三种格式往返测试通过。
- 损坏/截断/越界输入不崩溃、不 UB、返回错误。
- Manifest 查找 API 零拷贝，10k 资产解析在可接受时间内。
- PRCL 按块读取只解压覆盖区域。
- fuzz 目标可运行，至少 10 分钟无崩溃。
- 格式文档与实现一致（docs/02-format.md）。

## 估算

约 2-3 周（1-2 人）。

## 实施状态

- 已完成（2026-11-20）。
- 新增 parcel_serialize：BinaryReader/Writer、Guid128、PRCA/PRCL/PRCM、压缩、ManifestView。
- 内置 FlatBuffers 23.5.26 runtime headers、zstd 1.5.7、lz4 1.8.0。
- 测试覆盖往返、截断、随机/损坏输入、版本拒绝、大表 10k/1k、golden files。
- ASan、no-exceptions/no-RTTI、Debug/Release 测试通过。
- 可选 parcel_fuzz 目标已提供；10 分钟长跑 fuzz 留到 R09。
