# Changelog

All notable changes are recorded here. The project follows a pre-1.0 format.

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



































