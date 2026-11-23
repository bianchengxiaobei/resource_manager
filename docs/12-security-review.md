# 12 安全审查

审查范围：C++ 解析/打包/运行时、Go 服务端、Vue 管理端、部署文件。目标是首期生产 MVP
的输入边界、权限、日志和供应链风险，不包含加密/DRM。

## 1. 边界与威胁模型

- 不可信输入：网络下载的 PRCM/PRCL、上传的 Manifest/Bundle、`.parcel.json`、
  HTTP Header/Query、管理端表单。
- 信任边界：服务端进程、SQLite、本地 blob 目录；CDN 只负责传输，不作为可信源。
- 不做：资源加密、客户端防篡改、服务端多租户隔离、任意代码执行沙箱。

## 2. 检查结果

| 项 | 结果 | 证据/位置 |
|---|---|---|
| 路径穿越（资产/包/版本/Bundle/Raw） | 通过 | `core::path::IsSafeRelative`、`pack` 的 root 越界检查、服务端路径归一化与测试 |
| PRCL 逻辑/压缩块炸弹 | 通过 | `block_size <= 64 MiB`、`compressed_size <= 64 MiB`、块表/文件范围/CRC 校验；`test_security.cpp` |
| PRCM 表计数/范围 | 通过 | `kMaxTableEntries`、`CheckTableRange`、依赖/标签索引校验；`test_security.cpp` |
| PRCA 表计数/Blob 范围 | 通过 | 表计数上限、metadata/blob 范围校验；`test_security.cpp` |
| AnimationClipAsset metadata | 通过 | channel/key 数量上限、时间单调/range、target 非 0、有限浮点校验；`test_assets.cpp` |
| MeshAsset 蒙皮顶点 | 通过 | skin 数量与顶点数一致、权重有限/非负；大数量仍由 Blob 长度校验；`test_assets.cpp`、`test_import.cpp` |
| MeshAsset morph targets | 通过 | target 数量/总 delta 上限、delta 数量与顶点一致、normal/tangent flag 与实际数组一致、delta/默认权重有限；`test_assets.cpp`、`test_import.cpp` |
| glTF/GLB 解析 | 通过 | cgltf parse/load/validate、required Draco/basisu/WebP 拒绝、EXT_meshopt_compression source range/单 bufferView 与累计 512 MiB 解码预算、mesh 顶点/索引/morph budget、buffer 大小上限/路径限制在 project root、embedded image 大小/尺寸/decode 校验；`test_gltf.cpp` |
| SkinComponent | 通过 | joint/matrix 数量一致、joint 非 0、矩阵有限、joint 实体存在性校验；`test_scene.cpp`、`test_hbg.cpp` |
| MorphComponent | 通过 | 权重数量上限、浮点有限；桥接时校验与 mesh morph target 数量一致；`test_scene.cpp`、`test_hbg.cpp` |
| 上传大小/类型/哈希 | 通过 | multipart 流式大小限制、PRCM/PRCL magic/header/content hash 校验 |
| 管理端 CSRF/权限/限流 | 通过 | Session + CSRF、API Key 角色、登录/上传/下载限流；Go API 测试 |
| Token/签名 URL | 通过 | 设备 JWT、HMAC `key_id\npath\nexpires`、过期校验、密钥轮换；bad signature 403；客户端 `SignedUrlProvider` 不持有密钥，403/410 重签 |
| 日志脱敏 | 通过 | 不记录密码/Token/HMAC；配置解析错误不输出 secret |
| 依赖许可证 | 见 `THIRD_PARTY_NOTICES.md` | 内置依赖保留 LICENSE，外部依赖由宿主提供 |
| 服务端上传并发 | 通过 | `TestConcurrentUploadsAndDownloads` 覆盖 4 上传 + 8×25 下载/config/未签名请求 |
| 缓存完整性 | 通过 | `VerifyCacheFiles` 按 Metadata/Size/Content 校验当前 Manifest；内容级读文件核对 SHA-256/CRC32，损坏项删除并继续；`test_remote.cpp` |
| UBSan/ASan | 通过 | MSVC ASan 48.65s (RelWithDebInfo, 206 cases/5803 assertions)；ClangCL 17 UBSan 225 cases/6162 assertions、0 runtime error；`scripts/run-ubsan.ps1` |
| WinHTTP/真实 CDN TLS | 部分验证 | WinHTTP loopback 下载与 session/connection 句柄复用通过 `scripts/run-winhttp-smoke.ps1`；真实 CDN/TLS/吞吐待宿主 |
| Docker/nginx TLS 启动 | 待宿主验证 | 本机无 Docker；配置已静态审查 |

## 3. 已实施的硬化

- Manifest/Asset/Bundle 解析先做固定头、尺寸、范围、计数和重叠校验，再分配/解压。
- 服务端上传先写 `.tmp`，校验成功后原子 rename；拒绝路径穿越和控制字符。
- 签名 URL 的 path 参与 HMAC；过期时间有上限；错误统一 403，不区分 key/路径。
- 客户端仅缓存服务端签名 query 至过期，不保存 HMAC 密钥；403/410 清除缓存并重签。
- 会话 Cookie HttpOnly/SameSite，CSRF 使用独立 token；登录失败不泄露用户存在性。
- 服务端请求日志只记录 path，不记录 Query/Authorization。
- 并发加载通过 package 锁注册 Handle；provider 启动幂等；scheduler update 串行化。
- 同一 Bundle 的下载按临时路径串行化，获得锁后复查缓存；避免并发覆盖 `.part`。
  批量缓存校验默认读取数据并核对 SHA-256/CRC32，损坏项删除后继续扫描，避免坏缓存拖垮后续加载。


## 4. 残留风险

- 解压仍有 CPU/内存成本；64 MiB 块上限能阻止单块膨胀，但大量合法块仍可造成压力，
  依赖限流/超时/监控缓解。
- 服务端上传上限默认 4 GiB，入口 nginx 也有限制；需按部署环境调整。
- 未做服务端病毒扫描、内容类型深度识别或 CDN 私钥托管。
- 未做分布式限流/多节点一致性；当前单节点 SQLite。
- 未验证真实 WinHTTP TLS、Docker 镜像和 nginx 证书链。
- 依赖许可证清单是工程审查，不替代法务结论。

## 5. 复现命令

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --target parcel_tests
& build/windows-debug/bin/Debug/parcel_tests.exe "[security]"
powershell -ExecutionPolicy Bypass -File .\scripts\run-ubsan.ps1
cd server
go test ./...
```


























































