# Third-Party Notices

This file is an engineering inventory, not legal advice. Vendored license texts are
under `third_party/`; distribution must retain them.

## Vendored

| Component | Version | License | Location |
|---|---|---|---|
| Catch2 | 3.16.0 amalgamated | BSL-1.0 | `third_party/catch2/LICENSE.txt` |
| FlatBuffers runtime | 23.5.26 headers | Apache-2.0 | `third_party/flatbuffers/LICENSE.txt` |
| LZ4 | 1.8.0 | BSD-2-Clause | `third_party/lz4/LICENSE.txt` |
| Zstandard | 1.5.7 | BSD-3-Clause | `third_party/zstd/LICENSE` |
| stb_image / stb_image_resize | vendored snapshot | MIT or public domain (dual) | `third_party/stb/README.md` |
| cgltf | 1.15 | MIT | `third_party/cgltf/cgltf.h` (license in `third_party/cgltf/LICENSE.txt`) |

## Optional host dependencies

| Component | License | Source |
|---|---|---|
| EnTT | MIT | https://github.com/skypjack/entt |
| assimp | BSD-3-Clause | https://github.com/assimp/assimp |
| bgfx | BSD-2-Clause | https://github.com/bkaradzic/bgfx |
| hdrp-bgfx | see upstream repository | host-supplied, not modified by Parcel |

## Server and admin

Go modules: `modernc.org/sqlite` (BSD-3-Clause), `golang.org/x/crypto`
(BSD-3-Clause). Vue, Vite, TypeScript and Element Plus are MIT-licensed; their
license texts are in `server/web/node_modules` after `npm ci` and in the npm
package metadata.

## References

YooAsset is used as an architectural reference for manifest/bundle concepts. No
YooAsset source code is copied into Parcel; YooAsset is Apache-2.0. Hobi and Unity
source are not copied.
