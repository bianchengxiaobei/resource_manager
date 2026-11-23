# 02 格式：PRCA / PRCL / PRCM

## 1. 总览

| 格式 | Magic | Version | Header | 用途 |
|---|---|---|---|---|
| PRCA | `PRCA` | 1 | 128B | 单个 cooked asset 文件 |
| PRCL | `PRCL` | 1 | 128B | Bundle 容器 |
| PRCM | `PRCM` | 1 | 160B | Package Manifest |

所有格式小端、固定宽度字段、版本化、边界检查、CRC/SHA-256 校验。
读取不支持的版本返回 `Unsupported`，所有表偏移/长度都必须做溢出与边界检查。

## 2. PRCA Asset File

### 2.1 Header（128B）

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `PRCA` |
| 4 | 2 | version |
| 6 | 2 | header_size = 128 |
| 8 | 4 | flags |
| 12 | 8 | file_size |
| 20 | 8 | type_table_offset |
| 28 | 4 | type_count |
| 32 | 8 | object_table_offset |
| 40 | 4 | object_count |
| 44 | 8 | external_ref_offset |
| 52 | 4 | external_ref_count |
| 56 | 8 | sub_asset_offset |
| 64 | 4 | sub_asset_count |
| 68 | 8 | flatbuffer_offset |
| 76 | 4 | flatbuffer_size |
| 80 | 8 | blob_table_offset |
| 88 | 4 | blob_count |
| 92 | 32 | content_hash |
| 124 | 4 | reserved0 |

`content_hash` = SHA-256 of `[header_size, file_size)`。Header 本身不在 hash 内。

### 2.2 Tables

```text
TypeEntry (16B):
  uint32 type_id
  uint32 type_name_offset       // absolute file offset, UTF-8 NUL terminated
  uint32 serializer_version
  uint32 flags

ObjectEntry (40B):
  uint32 local_id
  uint32 type_id
  uint64 metadata_offset        // versioned serializer payload, absolute
  uint64 metadata_size
  uint64 blob_id                // UINT64_MAX = none
  uint32 flags
  uint32 reserved0

ExternalRefEntry (20B):
  AssetGuid asset_guid          // 16B
  uint32 flags

SubAssetEntry (12B):
  uint32 name_offset            // absolute file offset, UTF-8 NUL terminated
  uint32 local_object_id
  uint32 flags

BlobEntry (24B):
  uint64 offset
  uint64 size                   // stored size
  uint32 crc32                  // CRC32 of stored bytes
  uint8  codec                  // 0=None, 1=LZ4, 2=Zstd
  uint8  flags
  uint16 reserved0
```

### 2.3 Rules

- `local_id` 只在文件内有效；跨资产引用必须使用 `AssetGuid`。
- `ExternalRefTable` 是依赖分析唯一来源；Packer 不解析 object metadata。
- `metadata` 由 `ITypeSerializer` 负责，当前 R02 类型使用版本化小端二进制
  payload；FlatBuffers 仍是可选实现方式。Mesh/Texture/Shader 大块数据放 Blob。
- metadata 区域中每个 object metadata 按 8 字节对齐；对象之间可以共享内容，
  但当前 writer 为每个 object 独立写入。
- Blob 数据按 8 字节对齐；blob 之间、blob 与 header/table 不允许重叠。
- PRCA v1 只接受 `codec=None`。后续若要压缩 blob，需要先在格式版本中增加
  uncompressed size 字段。
- `TypeTable` 的 type_id/name 必须唯一；`ObjectTable` 的 local_id 必须唯一；
  `SubAsset.local_object_id` 必须存在；`ExternalRef.asset_guid` 不能为 0。
- 两阶段反序列化：先创建所有对象，再 Deserialize/解析引用。

## 3. PRCL Bundle

### 3.1 Header（128B）

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `PRCL` |
| 4 | 2 | version |
| 6 | 2 | header_size = 128 |
| 8 | 4 | flags |
| 12 | 8 | file_size |
| 20 | 8 | block_table_offset |
| 28 | 4 | block_table_size |
| 32 | 4 | block_count |
| 36 | 8 | toc_offset |
| 44 | 4 | toc_compressed_size |
| 48 | 4 | toc_raw_size |
| 52 | 4 | block_size |
| 56 | 1 | toc_codec |
| 57 | 1 | file_codec |
| 58 | 2 | reserved0 |
| 60 | 32 | file_hash |
| 92 | 4 | header_crc |
| 96 | 32 | reserved |

- `flags` bit 0：TOC 使用了压缩。
- `header_crc` = CRC32 of first 128 bytes with `header_crc` zeroed。
- `file_hash` = SHA-256 of entire file with `file_hash` and `header_crc` zeroed。
- `block_size` 默认 256KB，最大 64MB。
- `block_table_size` 必须等于 `block_count * 24`。

### 3.2 BlockEntry（24B）

```text
uint64 data_offset
uint32 compressed_size
uint32 uncompressed_size
uint32 crc32                 // CRC32 of uncompressed bytes
uint16 codec                 // 0=None, 1=LZ4, 2=Zstd
uint16 flags
```

Block 按顺序覆盖逻辑流；除最后一个 block 外，`uncompressed_size` 必须等于
`block_size`；最后一个 block 必须非空且不超过 `block_size`。
`ReadLogical` 只解压覆盖请求范围的 block，并校验这些 block 的 CRC。

### 3.3 TOC raw layout

```text
uint32 toc_version (1)
AssetGuid bundle_guid
uint32 string_count
  [string] (uint32 length + bytes) * string_count
uint32 bundle_name_index
uint32 dependency_count
  AssetGuid * dependency_count
uint32 payload_count
  { uint32 payload_id, uint64 logical_offset, uint64 size,
    AssetGuid main_asset_guid, uint32 flags } * payload_count
uint32 raw_count
  { uint32 name_index, uint64 logical_offset, uint64 size, uint32 flags } * raw_count
```

- TOC 以 `toc_codec` 压缩；Header 记录压缩/原始大小。若压缩不比原始小，writer
  会自动回退 `None`。
- `bundle_guid` 非 0；dependency 非 0、无重复、不能自依赖。
- `payload_id` 唯一；payload/raw 范围必须落在逻辑流内且互不重叠。
- Raw file name 必须是安全相对路径，拒绝绝对路径、`..` 穿越和 NUL。
- 加密 hook 预留；首期不实现。

## 4. PRCM Manifest

### 4.1 Header（160B）

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `PRCM` |
| 4 | 2 | version |
| 6 | 2 | header_size = 160 |
| 8 | 4 | flags |
| 12 | 8 | file_size |
| 20 | 8 | string_table_offset |
| 28 | 4 | string_count |
| 32 | 8 | asset_table_offset |
| 40 | 4 | asset_count |
| 44 | 8 | bundle_table_offset |
| 52 | 4 | bundle_count |
| 56 | 8 | tag_table_offset |
| 64 | 4 | tag_count |
| 68 | 8 | dependency_table_offset |
| 76 | 4 | dependency_count |
| 80 | 4 | package_name_index |
| 84 | 4 | package_version_index |
| 88 | 4 | output_name_style |
| 92 | 4 | build_bundle_type |
| 96 | 32 | content_hash |
| 128 | 32 | reserved |

`content_hash` = SHA-256 of `[header_size, file_size)`。

`LoadManifestFile(".../manifest.bytes")` 优先读取 `manifest.bytes.hash`；若不存在，
也接受同目录的 `manifest.hash`（Packer 输出使用该名称）。

Flags：

- bit 0 `EnableAddressable`
- bit 1 `SupportExtensionless`
- bit 2 `LocationToLower`
- bit 3 `IncludeAssetGuid`
- bit 4 `ReplaceAssetPathWithAddress`

`Addressable + LocationToLower` 和 `ReplaceAssetPathWithAddress` 缺少
`EnableAddressable` 都会被拒绝。

### 4.2 StringTable

```text
uint32 string_count
[string] * string_count         // uint32 length + UTF-8 bytes
```

所有 asset/bundle 名称、address、path、type name 与 tag 都进入 string table；
字符串在 ManifestView 中通过 `string_view` 零拷贝引用。

### 4.3 AssetRecord（60B）

```text
AssetGuid asset_guid
uint32 address_index
uint32 path_index
uint32 type_name_index
uint32 bundle_index
uint32 payload_id
uint32 local_object_id
uint32 dependency_offset
uint32 dependency_count
uint32 tag_offset
uint32 tag_count
uint32 flags
```

### 4.4 BundleRecord（96B）

```text
AssetGuid bundle_guid
uint32 name_index
uint32 file_name_index
uint64 file_size
Hash256 file_sha256
uint32 file_crc
uint32 flags
uint32 dependency_offset
uint32 dependency_count
uint32 referrer_offset
uint32 referrer_count
uint32 tag_offset
uint32 tag_count
```

### 4.5 Tag / Dependency Tables

- `TagTable` 是 flat `uint32 string_index[]`。每个 Asset/Bundle 的 `tag_offset/count`
  指向该数组的一段；同一 tag 可以出现在多个记录中。
- `DependencyTable` 是 flat `uint32 bundle_index[]`。Asset 的依赖、Bundle 的依赖和
  引用列表都指向该数组。
- `ManifestView::Tags()` 返回全局去重后的 `string_view` 列表。
- 重复 address/path/GUID/Bundle name/file name、越界 index、缺失依赖、表重叠
  都会拒绝。

## 5. 压缩与校验

```text
Codec: 0 None, 1 LZ4, 2 Zstd
```

- PRCL block 压缩：逐 block 尝试，若压缩后不更小则回退 `None`。
- TOC 压缩：同样允许回退。
- PRCA v1 Blob 暂不支持压缩。
- SHA-256 仅保证完整性，不防篡改；首期不签名。
- CRC32 用于快速损坏检测；SHA-256 用于文件/内容完整性。

## 6. 确定性

- Import/Pack 输出必须字节级可复现：稳定排序、稳定 GUID、固定时间戳或去掉时间戳。
- 压缩参数、编码器版本写入构建报告；同一输入 + 同一工具版本必须一致。
- `cpp/tests/golden` 保存最小 PRCA/PRCL/PRCM 回归文件；由
  `parcel_golden_generate` 生成。
