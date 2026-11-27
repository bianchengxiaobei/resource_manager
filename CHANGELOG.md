# Changelog

All notable changes are recorded here. The project follows a pre-1.0 format.

## Unreleased

### Added

- Pack file-level incremental builds: `build_db.json` v2 stores each asset's relative
  path, size/mtime, content hash, type, dependencies and report statistics. Unchanged
  assets are not reopened or reparsed, and are payload-loaded/verified lazily only when
  their bundle must be rebuilt. `build_report.json` exposes `reusedAssetCount` alongside
  `reusedBundleCount`.
- Optional Pack output verification: `verifyOutput=true` / `parcel_pack --verify-output` re-reads the written manifest, hash sidecar, version file, bundle files and builtin catalog, comparing size, SHA-256, CRC32 and catalog entries. Mismatches fail the build and are reported; `parcel::pack::VerifyPackOutput()` is public.
- Collector `include`/`exclude` glob patterns for `parcel_pack`, relative to the collector
  path, with `*`, `**`, `?` and character classes.
- TOML pack configuration (`--config pack.toml`) with top-level keys and `[[collectors]]`;
  JSON remains supported. The TOML subset covers strings, integers, booleans, arrays,
  comments and multiline arrays.
- Collector `packRule`, `filterRule` and `group`: type-based filtering with
  multi-collector fallback, and bundle grouping by separately/directory/top-directory/
  collector/group/raw/video/shader. Explicit `bundle` and source groups retain priority.
  Unsupported Unity-only rules return `Unsupported`.
- Runtime `PlayMode` (`Auto`/`EditorSimulate`/`Offline`/`Host`) with strict FileSystem
  validation. `Offline` is builtin-only; `Host` requires a sandbox; `EditorSimulate`
  scans a simulate root and loads loose cooked PRCA files directly, with a direct
  resolver for cross-asset references and no PRCL/BundleLoader usage; bundle, download,
  cache, unpacker and importer APIs return `Unsupported`.
- Pack `outputNameStyle` now controls bundle file names (`HashName`, `BundleName`,
  `BundleName_HashName`) with deterministic sanitization and collision checks.
  `buildBundleType=3` forces raw assets into archive bundles; `RawFile` is explicitly
  unsupported.
- Archive raw bundles: collector `archive=true` stores multiple RawFileAsset payloads as
  PRCL TOC `raw_files` with archive manifest flags; runtime `LoadRawFile` reads the TOC
  directly while non-archive raw assets keep the existing PRCA payload path.
- `ResourcePackage::OpenRawFile` returns a `RawFileReader` for synchronous asset-level
  range reads. Archive raw reads bundle logical ranges without creating a Provider or
  copying the full payload; non-archive raw materializes once and serves ranges from the
  retained copy. The reader owns a bundle reference until destruction.
- Server package/version management: admin-only `PATCH` rename and `DELETE` with a
  confirmation value; version deletion is rejected while a channel or gray rule references
  it, package deletion cascades versions/channels/rules, and unreferenced content-addressed
  blobs are removed. Manifest/hash artifact names follow package and version renames.
  `parcel-admin` gains `rename-package`, `delete-package`, `rename-version`,
  `delete-version`, `gray-update`, and `gray-delete`; the Vue admin exposes the same
  package/version actions.
- Pack `.parcelmeta` source mapping: `parcel_pack` scans `source` (default project root) for
  `*.parcelmeta`, maps primary asset GUIDs back to project-relative source paths, and uses
  those paths for manifest `asset_path`/`assetPath` addresses by default (`useSourcePath`).
  Same-source Main/SubAssets are forced into one bundle (`forceSubAssetGrouping`); conflicting
  explicit bundle hints and Raw assets grouped with other assets fail. `build_report.json`
  includes `sourcePath`, and `parcel_pack` accepts `--source`.
- Runtime bundle transfer operations: `ResourceUnpacker` copies missing builtin bundles into
  the writable Sandbox/Custom file system with optional tag filtering, concurrency, progress,
  cancellation, size/SHA-256/CRC verification, and atomic temp installation. `ResourceImporter`
  imports external `.bundle` files by bundle name/GUID with the same validation and lifecycle.
  `ResourcePackage::CreateUnpacker` / `CreateImporter` now return working operations instead of
  `Unsupported`; empty importer options return `InvalidArgument`. New runtime test coverage
  unpacks the fixture package, imports an external bundle, verifies bytes, and checks a missing
  source failure.
- Optional `parcel_hbg_gpu_smoke` Windows target for the hdrp-bgfx host build:
  hidden GLFW window + real bgfx device, Parcel Scene → RenderScene →
  unmodified `ForwardRenderer`, full-mip/BC7/RGBA16F `ParcelMaterialSystem`
  uploads, draw-call validation, screenshot and GPU-resource leak checks.
  `--package` loads a built Parcel package through `ResourcePackage` and
  `EnttSceneLoader`; `--scene` selects its scene address (defaults to the first
  `SceneAsset`). `--objects=N` builds a shared static mesh grid for scale tests
  (10000 instances validated on D3D12). `--shader-cache` materializes a package
  `ShaderLibraryAsset` through `ShaderLibraryBootstrap`, reinitializes the
  renderer's unmodified `ShaderLibrary`, and validates the Forward program
  (dxbc/spirv).
- Hidden WinHTTP real-CDN smoke (`[.cdn]` + `scripts/run-cdn-smoke.ps1`):
  Cloudflare HTTPS 8 MiB, certificate validation, cold/warm throughput, and
  WinHTTP connection reuse. Debug measured 692.8-840.7 ms / 9.52-11.55 MiB/s
  cold and 304.6-594.8 ms / 13.45-26.26 MiB/s warm with one connection created
  and one reused.
- OpenAPI contract tests for the Go server: every documented method/path must be
  registered and every registered API route documented; top-level JSON fields
  for the main schemas are checked against Go struct tags. The `/files` route now
  uses the documented Go wildcard form, and the generated TS schema plus
  `vue-tsc` build pass without drift.
- Hidden real-CDN full-install smoke (`[.cdn-install]` +
  `scripts/run-cdn-bundle-smoke.ps1`): downloads the committed 328 B
  `minimal.prcl` fixture from GitHub raw over HTTPS, builds a matching Parcel
  manifest, and verifies SHA-256/CRC, sandbox install, `VerifyCache`, and zero
  remaining download size.
- Hidden live-server signed URL smoke (`[.signed]` +
  `scripts/run-signed-url-smoke.ps1`): starts a temporary `parcel-server`,
  uploads/publishes a bundle, issues a device token, then uses WinHTTP +
  `SignedUrlProvider` to download it. Verifies exact bytes/SHA-256, unsigned and
  tampered requests return 403, unauthenticated signing returns 401, and an
  expired signature returns 403 (32 assertions).
- Terrain LOD selection adapter (`parcel_hbg::TerrainLodController`): ports the
  original `TerrainLodController`/`TerrainQuadTree` split, frustum, horizon, and
  child-order behavior without modifying hdrp-bgfx. The GPU smoke discovers the
  packaged `.terrain.json` TextAsset and updates `TerrainLOD_*` visibility. A
  baked 341-node Proland terrain (`gen_terrain_lod.py --baked-terrain`) packs to
  356 assets / 3 bundles / 196,413,756 bytes and yields `leaves=85 visible=67
  bound=341` on D3D11/D3D12/Vulkan; D3D11 renders 468 draws / 1,275,816
  triangles and 64 textures. A CPU `HeightmapTileCache` (ported from the
  original synchronous tile cache) reads the packaged `HBGHMAP1` `heightmap.bin`,
  supplies exact node bounds ahead of sidecar `node_bounds`, and bilinearly
  resamples camera ground height after 0.1 m movement; the smoke reports
  `2048x2048 scale=0.076295 ground=442.926 hits=113 misses=113 tiles=113
  evictions=49`. `BuildVegetation` parses the packaged `trees.json` and merges
  60000 deciduous/conifer billboards into 1 km species tiles (1481 meshes,
  240000 triangles) with packaged alpha-mask tree textures;
  `VegetationController` applies per-tile frustum and 5000 m
  `distanceToBounds` culling (`visible=53`, `visible_tiles=27 visible_submeshes=153`,
  `frustum_culled=1014 frustum_culled_submeshes=32`, `distance_culled=414 distance_culled_submeshes=8`) and toggles `meshIndex` without
  modifying hdrp-bgfx. D3D11 total is 1823 objects / 468 draws / 26 passes /
  1,275,816 triangles / 64 textures. The 1 km mesh tile is required by bgfx's 4096 static
  vertex/index buffer limit; each 1 km mesh is split into 500 m submeshes for finer culling,
  with near/far LOD (single tree card beyond 1500 m). Shared-grid sidecars (`terrain_coarse_mesh`/
  `terrain_fine_mesh`, as produced by the original non-baked `fractal_lod.gltf`)
  now build 341 CPU-displaced world-space meshes (33/65 grids, world UVs and
  skirts): 1,460,841 vertices / 2,833,664 triangles, `leaves=85 visible=67
  bound=341`; `scripts/run-r06-shared-terrain-smoke.ps1 -Rebuild` stages the
  original non-baked asset (19 assets / 3 bundles / 96,264,476 bytes), and the
  D3D11 shared-terrain smoke renders 344 objects / 162 draws / 26 passes /
  1,200,580 triangles / 64 textures. This is CPU geometry, not the original GPU
  shader displacement or heightmap tile array; material tiles, per-instance
  vegetation GPU instancing/impostors and GPU FFT texture remain pending; CPU river mesh/terrain carve, material textures, CPU foam animation, and screen-space Poisson foam seeding are supported.
- CPU water FFT (`parcel_hbg::WaterSurface`): ports the original `OceanFFT`
  spectrum, four 5488/392/28/2 m grids, inverse FFT, mip sampling, and the
  `applyToVertices`/`applyToScreenGrid` height/slope/choppy paths. It rebuilds
  the original `WaterScreenGrid` (8 px cells, 1.25 overscan) and ocean-sphere ray
  grid, attaches a `WaterFFT` morph target, and lets the unmodified hdrp-bgfx CPU
  morph/dynamic-vertex path upload results. All three backends report
  `water_fft=active mesh=0/2 vertices=8750 updates=1 morph_updates=1
  screen_grid=1 grid=125x70`; OceanSphere is 28 draws / 26 passes, baked terrain
  468/26, and shared terrain 162/26. GPU FFT texture and GPU river particles remain
  pending; CPU river mesh/terrain carve, material textures, CPU foam animation, and screen-space Poisson foam seeding are supported.
- CPU river mesh/terrain carve (`parcel_hbg::RiverScene`): ports the original
  `RiverGraph::meander`, `HydroFlow`, `buildRiverMesh` (320x12 ribbon), and
  `computeRiverTerrainDelta`/`applyRiverTerrainDelta` paths. The smoke discovers
  the packaged `river` object, creates a translucent Lit fallback material, and
  rebuilds normals/tangents on the carved `RiverTerrain` mesh. All three backends
  report `river=active vertices=4173 indices=23040 flow_samples=319
  speed=[0.0188,0.0226]`, `river_terrain=mesh='RiverTerrain' vertices=66049
  changed=6166 delta=[-5.060,2.550]`, `river_texture=size=256 base=0 normal=1 frames=8`,
  `river_normal_frame=frame=0/8 time=0.100 step=0.500`, and
  `river_foam=particles=512 vertices=2048 indices=3072 texture=64 image=9
  fade=[0.000,1.000]`; `river_foam_update=updates=6 time=0.100 max_delta=1613.910
  fade=[0.000,1.000]`; `river_foam_screen=particles=482`; D3D11 renders 3 objects
  / 31 draws / 27 passes / 271892 triangles / 76 textures. The
  `RiverBaseColor`/`RiverNormal` textures follow the original
  `RiverParticleSystem`
  `buildWaveFrames`/`buildBaseColorPixels`/`buildBaseNormalFrame` paths, use a
  Repeat sampler and normalScale 0.35, and generate the base color plus 8
  wave-normal frames. Each normal frame has its own image/material/mesh;
  `RiverScene::Update` switches `RenderObject.meshIndex` using the original
  `time / waveTimeStep` rule (31-frame smoke verified `frame=1/8`). The CPU
  foam overlay ports `riverLifecycleFade`, flow/speed-driven placement, particle
  capacity, and the original screen-space particle idea: `RiverScene::Update`
  accepts a `RiverView`, seeds an adaptive Poisson-disk cloud inside the river
  screen bounds, maps samples back to the water plane, reprojects/respawns
  particles that leave the viewport or river, and drives a `RiverFoamFlow` morph
  target. The 64² `RiverFoam` overlay uses alpha-test cutout
  (`kRenderQueueAlphaTest`, alphaCutoff 0.2) because hdrp-bgfx leaks one uniform
  on the transparent dynamic morph path. Depth-snapshot sampling, the Poisson
  neighborhood grid, depth/normal splatting, and screen-aligned foam sprites
  remain pending; the original `maxParticles=5000` cloud is reduced to 512
  world-space quads by the bgfx static buffer limits. GPU river particles,
  refraction/absorption/reflection, and the original river water shader remain
  pending.
- OceanSphere project asset smoke (`scripts/run-r06-ocean-smoke.ps1`): stages
  the original `../../assets` layout, imports/packs 18 assets into 2 bundles
  (92,187,880 bytes), and renders through Parcel's unmodified ForwardRenderer on
  D3D11/D3D12/Vulkan at 800x450 (`objects=1`, `draws=28`, `passes=26`,
  `textures=59`, `triangles=34224-34244`). hdrp's render-test also draws
  unreferenced terrain LOD nodes, so this is an import/pack/backend smoke, not a
  draw-count parity gate; dynamic terrain LOD, CPU river mesh/terrain carve, river
  material textures, and CPU foam animation is covered separately by R06 smokes.
- Large loopback download benchmark (`[.download-bench]` +
  `scripts/run-download-bench.ps1`): downloads 512 bundles (31.94 MiB total,
  8 concurrent) through WinHTTP from a temporary Python server, verifies
  SHA-256, atomic install, `VerifyCache`, and zero remaining download size.
  Debug 4864 ms / 6.57 MiB/s; Release 4079-4455 ms / 7.17-7.83 MiB/s; 512 requests,
  8 connections, 504 reused.
- Playwright admin smoke (`server/web/e2e/admin.spec.ts` +
  `scripts/run-webui-smoke.ps1`): builds and embeds the Vue UI, starts a
  temporary server, then uses system Edge headless to verify login, package
  creation, two version uploads, publish, rollback, gray-rule
  creation/edit/delete, audit pagination, metrics views, and logout with no
  page errors.
- Windows nginx TLS proxy smoke (`scripts/run-nginx-smoke.ps1`): downloads
  nginx 1.27.5, generates a local CA and server certificate, starts a temporary
  Parcel server, verifies HTTP→HTTPS redirect, HTTPS health/static UI, admin
  login, and package creation through nginx. A Go TLS probe verifies the local
  CA chain and hostname. Docker container startup and public CA trust remain
  unverified.
- R06 frame budget tooling: `--warmup`, avg/median/p95 frame stats, optional
  `--gpu-light-list`/`--motion-vectors` workload matching, and
  `scripts/run-r06-performance.ps1` comparing the Parcel package path against the
  unmodified hdrp `--render-test` with three alternating pairs. Proland 1080p D3D11
  median Parcel average was 291.804 ms vs hdrp 301.689 ms (ratio 0.967) with
  identical draws/passes/triangles; this 60k-node stress scene is not gated on
  absolute 60 fps because the original hdrp path is also below it.
- Added `-MinFps` for a lightweight absolute 60 fps gate. `fractal_lod` at
  D3D11 800x450 passes with Parcel 1.634 ms (612.0 fps) vs hdrp 7.304 ms
  (136.9 fps), ratio 0.224, over 3x120 frames. `-MinFps` requires 120 frames
  because the original `render_test.cpp` includes pipeline warmup in its
  in-process average; 20 frames underreported hdrp at 50.4 fps.
- SceneRenderBridge deduplicates static meshes by asset GUID plus final submesh
  material indices and reuses materials by GUID; skinned and morph meshes remain
  per-object to preserve independent deformation state. Real-asset smoke with a
  60067-node Proland scene imported from its original `../../assets` layout
  produced 15449 draws / 1.56M visible triangles from 79 packaged assets on
  D3D12, D3D11 and Vulkan (Debug, 320x240).
- gLTF `KHR_texture_transform` import support for the hdrp-compatible UV0 path:
  when all mapped material textures share one transform, `offset`, `rotation` and
  `scale` are baked into `TEXCOORD_0`; when per-texture transforms differ, each
  non-identity transform is baked into a TextureAsset variant keyed by source
  image, transform, sampler and sRGB. `texCoord != 0` keeps the original UV0 and
  emits a warning. glTF importer is now v9.
- gLTF/GLB `EXT_meshopt_compression` decode for ATTRIBUTES/TRIANGLES/INDICES
  with NONE/Octahedral/Quaternion/Exponential filters and 512 MiB budgets.
- gLTF `scene` importer setting (name or 0-based index).
- `ShaderLibraryBootstrap` stores shader pass binaries in a content-addressed
  `.blobs/<sha256>.bin` cache. Version directories link them with same-volume
  hard links and fall back to copies when linking is unavailable;
  `CleanupOldVersions` skips the blob directory and removes unreferenced blobs.
  `ShaderMaterializeOptions::use_hard_links` can disable linking.

### Fixed

- `parcel_pack --config` now applies `writeBuiltinCatalog`; disabling the catalog removes a
  stale `builtin_catalog.bin` from the same output directory.
- `cpp/include/parcel/import/importer.hpp` includes `texture_asset.hpp` so a
  clean rebuild of the texture fallback alias no longer depends on transitive
  includes.
- `ShaderLibraryBootstrap` now uses process-unique temporary names and
  idempotent hardlink/copy commits for cached shader binaries and the generated
  manifest. Concurrent processes sharing one shader cache no longer collide on
  temporary or output files; the hbg package shader smoke passes with D3D12 and
  Vulkan running concurrently against the same cache.
- `OperationBase::Start` now uses an atomic one-shot guard so concurrent
  scheduler and `StartSync` starts cannot invoke `OnStart` twice on the same
  operation; this fixes the ASan heap-use-after-free seen in concurrent runtime
  asset loading.
- `SceneRenderBridge` images now preserve the complete mip chain and texture
  format instead of uploading only the base mip.
- `ShaderLibraryBootstrap` emits legacy singular `fragment`/`bufferFragment`
  aliases for single-fragment passes so hdrp-bgfx's `ShaderLibrary` accepts
  materialized manifests.
- glTF importer v9 resolves parent-relative external buffer/image paths that
  remain inside the project root and supports shared and per-texture
  `KHR_texture_transform`; v8 metadata is invalidated so affected scenes are
  reimported.

## 0.1.0 - 2026-11-21

### Added

- Deterministic `builtin_catalog.bin` with package/version and Bundle GUID/file
  mapping; BuiltinFileSystem loads it for fast lookup and falls back to directory
  probing for legacy packs.
- Light cookie support: sidecar cookie paths resolve through texture dedup; the
  SceneRenderBridge fills hdrp-bgfx legacy/point-octahedral cookie samples and
  native data for uncompressed RGBA8/BGRA8/RGBA16F/RGBA32F textures.
- RenderScene node hierarchy: the SceneRenderBridge emits nodes in SceneData order
  and assigns `nodeIndex` on RenderObjects, Cameras and Lights.
- Animation clips: FBX node TRS animations cook to AnimationClipAsset, scenes carry
  AnimationPlayerComponent references, and the bridge remaps SceneEntityId channels
  to RenderScene node indices. FBX morph channels cook to AnimationPath::Weights with
  targetIndex.
- Morph targets: FBX blend shapes cook to MeshAsset morph targets/zero default weights,
  MorphComponent overrides per-object weights, and the bridge fills RenderScene node
  weights and object morphWeights. Morph meshes skip vertex merging, and MeshAsset
  serializer version is now 3.
- Skinning: FBX aiBone weights cook to MeshAsset skin vertices, SkinComponent stores
  joint SceneEntityIds and inverse bind matrices, and the bridge emits RenderScene.skins
  with node indices and object skinIndex. Skinned meshes skip vertex merging.
  Fixed the MeshAsset metadata count guard for blob-backed large vertex/index tables.
- IES profile support: the `ies` importer cooks TILT=NONE angle/candela tables
  into an IesProfileAsset; sidecar light paths resolve to it, spot lights receive
  `iesSamples`/`iesCut`, and point lights modulate cookie legacy/native data.
- C++20 import, pack, runtime, remote update and hbg adapter libraries.
- PRCA/PRCL/PRCM formats with compression, hashes and zero-copy manifest lookups.
- glTF/GLB import through vendored cgltf 1.15: mesh/material/node/camera/KHR_lights_punctual,
  skin, morph targets, TRS/Weights animations, sidecar overrides, external buffers/images,
  GLB binary buffers, embedded bufferView/data URI images decoded into texture subassets,
  texture sampler wrap/filter mapping, `KHR_materials_variants` material selection,
  `EXT_meshopt_compression` attribute/index decoding, glTF scene selection by name/index
  (default scene when unset), and canonical texture reuse. glTF importer is v6.
- Material TextureRef now carries a TextureSampler (wrap/filter); MaterialAsset serializer
  version is 2, and ParcelMaterialSystem uploads per-sampler texture variants for hdrp-bgfx.
  FBX importer is v8 so existing metas reimport.
- EnTT Scene loading, asset codecs, sidecars and deterministic import/pack.
- `ResourcePackage::LoadScene`/`LoadSceneAsync` instantiate SceneAsset through
  `ISceneLoader` on the scheduler; the last shared SceneHandle release calls
  `Destroy` and releases provider bundles. YooAsset SceneHandle semantics used as reference.
- Downloader with range resume, retries, cancellation and cache repair.
- Go single-node server, Vue admin console, OpenAPI contract, Docker/nginx files.
- R09 hardening: independent format fuzz targets, stress tests, security tests,
  ASan build, performance benchmarks (including 10k-file cache scan) and release packaging.
- Batch cache verification (`VerifyCacheFiles`) with Metadata/Size/Content levels,
  corruption cleanup, missing-bundle skip and per-scan statistics. R09 soak script included.
- ClangCL UBSan build support and `scripts/run-ubsan.ps1`; all tests pass with zero UBSan
  runtime errors on Clang 17 / MSVC STL (alignment check disabled for an STL false positive).
- Disk-full commit injection: transient ENOSPC is retried, persistent failure returns `Io`,
  and a later download recovers without a stale cache entry.
- Budgeted unload now processes small Provider/Bundle batches per call; a 1k-bundle
  stress case shows a 1.6 ms p95 call with a 4 ms budget; OS scheduling outliers are
  reported separately. Performance tests run separately.
- WinHTTP transport now pools session/connection handles and exposes request/created/
  reused statistics; the loopback smoke verifies handle reuse.
- On-demand loads and explicit downloaders now share an in-flight gate keyed by the
  temporary path, so a bundle is transferred once and waiters reuse the owner result.
- `RetryFailed` skips succeeded bundles and only restarts failed tasks.
- Downloader request starts are throttled by `max_requests_per_frame` (default 8) over a
  16 ms scheduler window; `scheduler_frame_interval<=0` disables the window limit.
- Downloader watchdog now tracks transport byte progress and aborts stalled transfers via
  `HttpRequest::should_abort`; WinHTTP reports bytes during its read loop.
- Cache verification reads bundles with up to 8 workers by default
  (`CacheVerifyOptions::max_concurrency`, clamped to 1-32); invalidation and report
  aggregation stay serial, matching YooAsset `FileVerifyMaxConcurrency`.
- `parcel_bench` now measures cache verification: 10k 1-KiB bundles take 14.8 s
  serially and 9.1 s with 8 workers on the release baseline.
- WinHTTP loopback smoke (`scripts/run-winhttp-smoke.ps1`) downloads and verifies a
  bundle through a real local HTTP server; the hidden `[.winhttp]` test is opt-in.
- 60-minute R09 soak: 1103 stress iterations (4 cases / 1092 assertions each),
  0 failures in 60m01s.
- Windows ClangCL libFuzzer support (`PARCEL_FUZZ_LIBFUZZER=ON`, `scripts/run-fuzz.ps1`);
  30-minute combined format run: 222,311,162 executions, 0 artifacts/crashes.
- Added `parcel_fuzz_sidecar` (`.parcel.json` / `.parcelmeta` / remote config); 30-minute
  run: 12,304,085 executions, 0 artifacts/crashes.
- Added `remote::SignedUrlProvider`: exchanges `/api/v1/client/url` with device auth,
  caches signed queries until expiry, and re-signs after 403/410 responses.
- `/client/config` now exposes `sign_url`, and the client parses it for automatic
  signed-URL provider setup.
- Sandbox app footprint/watermark cleanup (`app_build_id` + None/Bundles/Manifests/All)
  runs before manifest loading; `OperationBase::Elapsed` and
  `DownloadSummary::elapsed/bytes_per_second` expose timing and throughput.
- FBX import now merges sidecar decals/reflection probes/local fog volumes into
  standalone scene entities, validates/clamps fields like hdrp-bgfx, and resolves
  texture paths relative to the source first. FBX content dedup reuses one
  mesh/material GUID per import and canonicalizes texture references by file hash.
- Texture assets are canonicalized across sources by source bytes plus import
  settings; identical aliases share one cooked asset while different settings stay
  distinct. Texture importer is v3 and FBX is v8 so existing metas reimport;
  ImportDB v2 stores the texture index hash so FBX references invalidate when
  mappings change.
- Optional `--shader-build-*` options invoke hdrp-bgfx `shader_build.py` before
  import without modifying it. Build failures abort, dry-run skips, and successful
  builds force shader library reimport when only binaries changed.
- FBX meshes run through built-in meshoptimizer 0.18: vertex remap, vertex cache,
  optional overdraw and vertex-fetch compaction; `optimizeMesh`, `optimizeOverdraw`
  and `overdrawThreshold` are part of importer settings.
- Texture import supports configurable RGBA8/BGRA8/RGBA16F/RGBA32F output and
  BC1/BC3/BC5/BC7 compression with fast/balanced/best quality, mip-chain encoding,
  auto BC1/BC3 selection, and HDR-to-half conversion.

### Changed

- Manifest hash is defined as SHA-256 over the PRCM payload after the 160-byte
  header, matching `parcel_pack` and the Go server.
- Scheduler updates are serialized and provider handles are registered while the
  package lock is held, making concurrent load/release and unload paths safe.
- Provider start is idempotent across concurrent loaders.
- Bundles downloaded concurrently by multiple downloaders are serialized per temp path;
  after acquiring the lock the downloader rechecks the cache and skips redundant network I/O.

### Fixed

- R07/R08 hash contract mismatch for downloaded manifests.
- Concurrent `UnloadUnusedAssets` could invalidate a provider between selection
  and handle creation.
- Concurrent `WaitForCompletion` calls could race scheduler updates.
- Concurrent publish/rollback writes could hit SQLite deferred-transaction lock errors;
  write transactions now use `BEGIN IMMEDIATE`.
- Multiple downloaders could write the same `<bundle-guid>.part`; temp writes are now serialized.
- Oversized or orphaned partial downloads are discarded or cleared explicitly.



































