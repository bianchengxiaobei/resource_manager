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

## 3. 回归阈值

首期阈值（超过即需要调查，不直接判定失败）：

- 100k PRCM 解析 <= 900 ms，地址查找 <= 1.5 us。
- 8 MiB PRCL 打开 <= 3 ms，全量读取 <= 35 ms，随机 4 KiB <= 300 us。
- 10k PRCA 打开 <= 15 ms。
- 10k cache 文件扫描 <= 40 ms；超过时检查目录层级/磁盘状态。
- 10k/1KiB cache verify 记录串行/并行基线；并行收益受磁盘和杀毒扫描影响。
- Unload p95 <= 4 ms（1k Bundle，`parcel_performance`；OS 调度离群点仅记录）。

## 4. 未覆盖

- 真实网络连接复用/吞吐（下载并发已由 MockTransport/压力测试覆盖）。
- 内存峰值和不同 block size 的系统矩阵。

这些项在 R09 后续补齐，现有数字用于回归比较。







