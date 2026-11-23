# Third-party dependencies

These libraries are vendored to keep the serialization layer buildable offline.

| Directory | Version | Origin | License |
|---|---|---|---|
| `catch2` | v3.16.0 amalgamated | hdrp-bgfx / Catch2 | BSL-1.0 |
| `flatbuffers` | 23.5.26 runtime headers | FlatBuffers | Apache-2.0 |
| `lz4` | 1.8.0 | FlaxEngine ThirdParty / LZ4 | BSD-2-Clause |
| `zstd` | 1.5.7 | Godot thirdparty / zstd | BSD-3-Clause |

`flatbuffers` is used as a header-only runtime. Generated schema headers are checked in;
the `flatc` compiler is not required for a normal build. `zstd` and `lz4` are built as
separate static libraries and linked only into `parcel_serialize`.

The `parcel_serialize` implementation references the architecture of YooAsset
(`PackageManifest` / bundle reader / writer) but does not copy YooAsset source code.
