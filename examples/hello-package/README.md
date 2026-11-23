# Hello Package

Minimal end-to-end source example: texture + text asset, import, pack, manifest.

## Prerequisites

Build the CLIs from the repository root:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --target parcel_import_cli parcel_pack_cli
```

## Import

```powershell
& build/windows-debug/bin/Debug/parcel_import.exe `
  --project examples/hello-package `
  --source examples/hello-package/Assets `
  --output examples/hello-package/cooked `
  --db examples/hello-package/import.db `
  --target windows `
  --jobs 2
```

Expected output:

```text
imported Assets/checker.tga
imported Assets/hello.txt
imported=2 skipped=0 failed=0
```

`.parcelmeta` files are deterministic and can be committed with the source assets.
`cooked/`, `build/` and `import.db` are generated and ignored.

## Pack

```powershell
& build/windows-debug/bin/Debug/parcel_pack.exe `
  --project examples/hello-package `
  --config examples/hello-package/pack.json
```

Expected output:

```text
output: examples/hello-package/build/windows/hello/1.0.0
```

The output contains `manifest.bytes`, `manifest.hash`, `hello.version`,
`bundles/*.bundle`, `build_report.json` and `build_db.json`.

## Runtime

Point `ResourcePackageOptions.builtin_root` at the version directory, then load by
address. The manifest records `Assets/checker.tga` and `Assets/hello.txt`.

## FBX + `.parcel.json`

When assimp is available (`-DPARCEL_ASSIMP_ROOT=<path>`), add an `.fbx` source under
`Assets/` and a matching `.parcel.json` scene sidecar. The sidecar schema is
`schemas/parcel.scene.schema.json`; camera/light/settings overrides in the sidecar
take precedence over importer defaults.

Example sidecar shape:

```json
{
  "version": 1,
  "settings": "Settings/hello.parcel.json",
  "cameras": [],
  "lights": []
}
```

Import and pack commands are unchanged. If assimp is not configured, `.fbx` sources
are skipped with an importer warning; the texture/text example still runs.
