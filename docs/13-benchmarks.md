# 13 性能基线与阈值

## 1. 方法

`parcel_bench` 使用确定性输入，重复测量取中位数；默认测量：

- PRCM：10k 资产 + 1k Bundle，100k 资产 + 10k Bundle；写入、解析、带内容 hash 解析、
  地址查找。
- PRCL：8 MiB payload，64 KiB block，LZ4；写入、打开、全量读取、随机 4 KiB 逻辑读取。
- PRCA：10k object（64 B metadata）+ 1 MiB blob；写入、打开。
- Cache：临时目录生成 10k 个 `.bundle` 文件；测量 `SandboxFileSystem::QueryCache` 扫描/统计时间。
- Cache verify：生成 2k/10k 个 1 KiB Bundle 和 Manifest；分别用 1/8 worker 执行
  `ResourcePackage::VerifyCacheFiles(Content)`，统计串行/并行耗时。

构建：

```powershell
cmake -S . -B build/windows-bench -DPARCEL_BUILD_BENCHMARKS=ON
cmake --build build/windows-bench --config Release --target parcel_bench
& build/windows-bench/bin/Release/parcel_bench.exe
```

快速检查：

```powershell
& build/windows-bench/bin/Release/parcel_bench.exe --quick
```

## 2. 基线

Windows 11、MSVC 19.44、Release、默认参数（本机首次基线）：

| 场景 | 输入 | 结果 |
|---|---|---|
| PRCM 写入 | 10k 资产 / 1k Bundle / 1.17 MB | 81.9 ms |
| PRCM 解析 | 同上 | 48.1 ms |
| PRCM 解析 + hash | 同上 | 49.9 ms |
| 地址查找 | 10k 资产 | 401 ns/次 |
| PRCM 写入 | 100k 资产 / 10k Bundle / 11.9 MB | 627.8 ms |
| PRCM 解析 | 同上 | 508.7 ms |
| 地址查找 | 100k 资产 | 652 ns/次 |
| PRCL 写入 | 8 MiB / LZ4 / 64 KiB block | 19.5 ms |
| PRCL 打开 | 同上 | 1.2 ms |
| PRCL 全量读取 | 同上 | 18.4 ms |
| PRCL 随机 4 KiB | 同上 | 128.9 us/次 |
| PRCA 写入 | 10k object + 1 MiB blob | 21.7 ms |
| PRCA 打开 | 同上 | 7.5 ms |
| Cache 扫描 | 10k `.bundle` 文件 / 180 KB | 16.3 ms |
| Cache verify | 10k Bundle / 1 KiB / Content | 串行 14.8 s，8 worker 9.1 s |
| Unload 4ms 预算 | 1k Raw Bundle，每帧 4ms，Debug 性能用例 | p95 1.6 ms；偶发离群 5.2 ms |

### WinHTTP 真实 CDN/TLS

`scripts/run-cdn-smoke.ps1` 以 Cloudflare speed endpoint 下载 8 MiB，连续请求两次；
第一次包含 TLS 握手，第二次复用 WinHTTP session/connection。2026-11-24 Debug
多次运行（网络相关，仅记录）：

| 场景 | 输入 | 结果 |
|---|---|---|
| CDN cold | 8 MiB HTTPS，含 TLS 握手 | 692.8-840.7 ms / 9.52-11.55 MiB/s |
| CDN warm | 8 MiB HTTPS，复用连接 | 304.6-594.8 ms / 13.45-26.26 MiB/s |
| CDN bundle install | 328 B GitHub raw | 下载/校验/安装通过，2 requests |
| 连接统计 | 2 requests | created=1，reused=1 |

该结果覆盖 WinHTTP/Schannel TLS 传输、证书链、吞吐和完整安装路径；Range 续传
仍由 loopback/MockTransport 覆盖。`run-cdn-bundle-smoke.ps1` 的 328 B fixture 是
仓库 committed golden 文件，安装用例不解析其运行时 Bundle 内容。Cloudflare 的
speed endpoint 对单次 `bytes` 有上限，16 MiB 请求返回 403，因此默认固定为 8 MiB。

### 大规模 loopback 下载基准

`scripts/run-download-bench.ps1` 用临时 Python 静态 server 和真实 WinHTTP 下载
512 个 Bundle（默认每个 64 KiB，共 512 个请求），8 并发，校验 SHA-256、原子安装、
`VerifyCache` 和下载后大小；隐藏用例 `[.download-bench]` 不计入主测试数。

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run-download-bench.ps1 `
  -BuildDir build/windows-release -Config Release
```

| 配置 | 文件/字节 | 耗时 | 吞吐 | 连接 |
|---|---|---:|---:|---|
| Debug | 512 / 31.94 MiB | 4864 ms | 6.57 MiB/s | 8 created / 504 reused |
| Release | 512 / 31.94 MiB | 4079-4455 ms | 7.17-7.83 MiB/s | 8 created / 504 reused |

该基准覆盖下载器并发、WinHTTP loopback、校验和缓存安装的规模开销，不代表公网
CDN 吞吐；真实 CDN/TLS 见上一节。

### R06 Parcel 包 vs hdrp 原 render-test

同一 Proland `fractal.gltf`（60067 nodes）分别用 Parcel 包 smoke 和 hdrp-bgfx 原始
glTF `--render-test` 渲染；两者使用同一个未修改的 `ForwardRenderer` 与
`--no-shadows`、GPU light list、motion vectors。Parcel 先做 10 帧 warmup，原 hdrp
先做 10 帧 preflight；随后交替执行 3 组配对测量，每组取 20 帧平均，比较两组
平均值的中位数，避免首次 shader pipeline 编译和单次桌面调度抖动污染结果。硬件为
i7-10700 + RTX 2060 SUPER，Windows 11，RelWithDebInfo。

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run-r06-performance.ps1 `
  -Backend d3d11 -Width 1920 -Height 1080 -Frames 20 -Warmup 10 -PreflightFrames 10 -Repeats 3
```

| 后端/分辨率 | 原 hdrp avg 中位数 | Parcel avg / median / p95 中位数 | Parcel/hdrp | draws/passes/triangles |
|---|---:|---:|---:|---|
| D3D11 1920x1080 | 301.689 ms | 291.804 / 288.260 / 405.354 ms | 0.967 | 40226 / 24 / 2501870 |
| D3D11 320x240 | 226.745 ms | 237.725 / 206.001 / 343.670 ms | 1.048 | 30835 / 22 / 2347238 |
| D3D12 320x240 | 220.408 ms | 218.002 / 208.298 / 321.205 ms | 0.989 | 30835 / 22 / 2347236 |
| Vulkan 320x240 | 254.288 ms | 191.221 / 187.067 / 256.119 ms | 0.752 | 30835 / 22 / 2347238 |

该场景绝对帧率只有约 3-4 fps，原 hdrp 路径同样低于 60 fps；瓶颈是 hdrp 当前
prepass/motion vectors 未走 instanced/GPU-driven batch（15401/20096 个可见 item
逐个 draw），不是 Parcel 适配层。因此 R06 的相对预算定义为：**相同配置下 Parcel
包路径平均帧时间不得比原 hdrp render-test 慢 10% 以上，且 draw/pass/triangle
完全一致**；绝对 60 fps 使用单独的轻量场景门禁，不作为 60k-node 压力场景门禁。
`parcel_avg/hdrp_avg` 的 3 组中位数在高负载桌面仍有约 5% 波动，因此 D3D11
320x240 接近门限但未越界。

轻量场景绝对 60 fps 使用 `fractal_lod.gltf`（346 nodes、5 meshes、98306 triangles、
12 draws、23 passes）和 `build/proland-light-layout` 生成的
`proland_light/1.0.0` 包。原 hdrp 的 `src/app/render_test.cpp` 从第 0 帧开始计时，
`--frames` 的平均值包含首帧 pipeline 编译，单独的 preflight 进程无法把冷启动从
计时循环中排除；因此 20 帧会低估到 50.4 fps，门禁固定使用 120 帧/组、交替 3 组：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run-r06-performance.ps1 `
  -Backend d3d11 -Width 800 -Height 450 -Frames 120 -Warmup 10 `
  -PreflightFrames 10 -Repeats 3 -MinFps 60 `
  -Asset E:\Learn\hdrp-proland\content\proland\scenes\fractal\fractal_lod.gltf `
  -Package E:\Learn\Parcel\build\proland-light-layout\build\windows\proland_light\1.0.0
```

`proland_light/1.0.0` 由 `build/proland-light-layout` 中的原始 `../../assets` 布局
staging 经 `parcel_import`/`parcel_pack` 生成，和 hdrp render-test 使用同一 glTF。

| 后端/分辨率 | 原 hdrp avg | Parcel avg / median / p95 | Parcel/hdrp | Parcel fps |
|---|---:|---:|---:|---:|
| D3D11 800x450（3x120） | 7.304 ms / 136.9 fps | 1.634 / 0.517 / 9.882 ms | 0.224 | 612.0 |
| D3D12 800x450（1x120） | 13.448 ms / 74.4 fps | 9.988 / 6.745 / 22.281 ms | 0.743 | 100.1 |
| Vulkan 800x450（1x120） | 7.253 ms / 137.9 fps | 4.935 / 4.011 / 9.835 ms | 0.680 | 202.6 |

D3D11 三组均大于 60 fps，Parcel 中位平均 1.634 ms；draw/pass/triangle 与 hdrp
完全一致。D3D12/Vulkan 单组 120 帧的平均帧率也超过 60 fps；D3D12 p95 22.281 ms
记录为偶发离群，不做隐藏。`-MinFps` 同时要求原 hdrp 与 Parcel 的平均 fps 达标；
若原 hdrp 自身低于 60 fps，脚本会以“baseline unavailable”失败，避免用不可比
场景伪造通过。

## 3. 回归阈值

首期阈值（超过即需要调查，不直接判定失败）：

- 100k PRCM 解析 <= 900 ms，地址查找 <= 1.5 us。
- 8 MiB PRCL 打开 <= 3 ms，全量读取 <= 35 ms，随机 4 KiB <= 300 us。
- 10k PRCA 打开 <= 15 ms。
- 10k cache 文件扫描 <= 40 ms；超过时检查目录层级/磁盘状态。
- 10k/1KiB cache verify 记录串行/并行基线；并行收益受磁盘和杀毒扫描影响。
- Unload p95 <= 4 ms（1k Bundle，`parcel_performance`；OS 调度离群点仅记录）。
- R06 Proland 包路径平均帧时间 <= 原 hdrp render-test 的 1.10 倍，且 draw/pass/triangle
  完全一致（`scripts/run-r06-performance.ps1`；3 组配对中位数，首次 pipeline 编译通过
  warmup/preflight 排除）。该压力场景不设绝对 60 fps 门禁；原 hdrp 同样不达标。

## 4. 未覆盖

- 真实 CDN 签名 URL/鉴权流程（TLS、吞吐和完整安装已覆盖，签名 URL 仍由
  loopback/MockTransport 覆盖）。
- 内存峰值和不同 block size 的系统矩阵。

这些项在 R09 后续补齐，现有数字用于回归比较。







