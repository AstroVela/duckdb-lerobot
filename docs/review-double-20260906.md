# 当前改动的连续两轮审查（2026-09-06）

第一轮确认并修复 **2 项 bug**；修复后重新冻结输入，第二轮未发现新增可确认
bug。第一轮发现均有修复前失败、修复后通过的运行证据。
这不是对所有平台、所有依赖版本均无 bug 的保证。

## 范围与顺序

- 工作树：`/home/kaka/duckdb-lerobot-worktrees/review-assessment-20260905`
- 分支：`fix/review-correctness-20260905`
- 比较基线：`fab70ad4effa1e5e5dda43e58a99cc17d70bb5d4`，包括相对 HEAD 的
  已修改和未跟踪文件，不局限于上一批修复，也不是重新比较旧提交 `81db162`。
- DuckDB：`d8cdaa33fda8df955cc76ef58a280f68f4cd43fa`，v1.5.5。
- 两轮输入均为 58 个文件；此报告是审查输出，最终共 59 个改动文件。
  原始 `review-assessment-20260905.md` 保持不变。

| 范围 | 文件数 | 核查内容 |
| --- | ---: | --- |
| `src/` | 19 | 路径与缓存、会话和取消、时间戳索引、COPY/统计/编解码及媒体构建配置 |
| CMake 根目录、test CMake、Native CI | 3 | 源文件接入、FFmpeg ON/OFF、测试目标、sanitizer 选择与依赖 |
| `test/cpp/` | 7 | 故障/取消触发点、所有权、失败时测试本身能否退出、断言和重试 |
| `test/sql/` 中有改动的文件 | 9 | 输入边界、路径、会话、视觉/数值结果、参数恢复和执行条件 |
| `test/conformance/` | 7 | 原生/外部对照是否独立、fixture、版本、覆盖声明 |
| `test/python/` | 1 | 子进程资源统计和尾部 I/O 回归 |
| `benchmark/` | 8 | 计时与内存口径、结果校验、JSON 记录和文档 |
| 其余文档 | 4 | API 契约、历史证据与本轮证据的区别、未完成范围 |

第一轮输入、第二轮输入及逐文件 SHA-256 清单分别保存在
`build/double-review-before-20260906/`、
`build/double-review-round2-input-20260906/`。
第二轮开始后没有再修改生产代码或测试逻辑，只补充此报告和对应文档。

## 第一轮：确认并修复的问题

### R1-1 / P1：最终拼接阶段取消 COPY，仍会发布数据集

位置：`src/function/copy/lerobot_video_io.cpp:78`、
`src/function/copy/lerobot_video_writer.cpp:534`、
`src/function/copy/lerobot_copy.cpp:2343`。

编码任务的取消 token 没有覆盖最终 remux 的 AVIO；最终发布前也没有检查
调用方的中断状态。回归测试在最终 `shard-*.mp4` 打开时调用真实连接的
`Interrupt()`，不注入文件系统错误。修复前查询返回 `InterruptException`，
但最终 root 已存在，`CheckRollback` 的目录不存在断言失败。
用户看到取消，磁盘却已经发布数据集。

修复让 remux 的输入、输出及 faststart AVIO 借用同步调用方的 `ClientContext`
检查取消；编码任务仍保留其 batch token。`Finalize` 入口和发布 rename 前
检查中断。借用的取消源必须存活到 adapter 及其句柄销毁。

回归位于 `test/cpp/test_video_io.cpp:569` 和 `:821`，覆盖最终输出打开、
片段读取、faststart 重新打开、manifest 写入，以及无 FFmpeg 的 numeric COPY。
每次均检查异常类型、句柄归零、root/staging 不存在、同一数据库可查询并可重试。
Native CI 的视频 I/O LSan 作业加入 `[video_copy_cancel]`。

修复前证据：[失败日志](../build/double-review-r1-copy-before.log)。
修复后证据：[Release](../build/double-review-r1-copy.log)、
[ASAN/UBSAN](../build/double-review-r1-copy-asan.log)、
[LSan 子集](../build/double-review-r2-copy-lsan.log)、
[FFmpeg OFF](../build/double-review-r1-copy-off.log)。

### R1-2 / P2：非空远程数据集提交为空后，热缓存无法失效

位置：`src/storage/lerobot_metadata_cache.cpp:527`。

缓存复用先校验旧 episode 文件，再比较 `info.json` 指纹。服务端提交新的空
manifest 并删除 episode Parquet 后，旧文件列表的 `DISALLOW_EMPTY` 先抛错，
代码无法走到 manifest 失效和重新加载。原生 JSON 已读到 `total_frames=0`，
扩展仍报找不到 `meta/episodes/**/*.parquet`。

修复将顺序改为先比较 manifest，再校验仍属于同一版本的 episode 文件。
保持现有 GET 撤权检查和加载前后的验证，不允许借空数据集绕过 manifest
读取权限。HTTPFS 回归位于 `test/conformance/test_session_settings.py:370`：
同一连接预热，服务端更新为无 episode 文件的空数据集，再恢复非空数据集；
数据和视频两级缓存都必须更新，覆盖 external file cache 开/关。

修复前证据：[失败日志](../build/double-review-r1-empty-before.log)。
修复后证据：[FFmpeg ON](../build/double-review-r1-httpfs.log)、
[OFF](../build/double-review-r2-httpfs-off.log)、
[ASAN/UBSAN/LSan](../build/double-review-r2-httpfs-asan.log)。

## 第二轮：最终改动的交互与回归复核

第二轮重新从公开入口、异常退出和资源所有权检查完整改动，未发现新增可确认
问题。特别复查第一轮修复是否改变正常发布、取消错误优先级、空数据集恢复、
AVIO 销毁顺序，以及 FFmpeg OFF 的编译和清理行为。

| 重点 | 结论与证据 |
| --- | --- |
| 编码线程池 | 错误/取消保留运行中任务的 FileSystem 生命周期；排队任务可独立移除；并发预算和扩容测试通过 |
| producer 和嵌套查询 | 检查阻塞任务析构、取消监听注销与连接销毁顺序；原生 Executor 及隔离 C++ 回归通过 |
| 路径、缓存和会话 | 核查模板替换后校验、相对 root 解析、secret/endpoint 缓存身份及 I/O 后再次验证；完整 SQL 和真实 HTTPFS 对照通过 |
| 时间戳索引 | 核查重复/NULL/空 shard、被引用索引的内存计数、并发加载回退和指纹变化；19 个嵌套读取/缓存用例通过 |
| 数值与 PNG | 核查 chunk/view 生命周期、嵌套 selection、精确整数 extrema、编码器复用和输出独立性；写入 C++ 回归通过 |
| 视频拼接 | 7 次混合分辨率 COPY 的独立 FFmpeg 对照通过；另检查双深度相机，跨 16/64/66/128 像素宽度并发编码和拼接，48 帧通过 |
| 官方读回 | LeRobot 0.6.1 分片边界与双向读回检查通过；已文档化的分片大小策略差异仍存在，不将它误报为内容损坏 |
| 基础设施与记录 | 检查构建条件、CI 测试标签、脚本和结果统计口径；3 份历史 benchmark JSON 的中位数/派生值一致，未冒充本轮性能测量 |

## 本轮执行结果

| 检查 | 结果 | 日志（`build/`） |
| --- | --- | --- |
| 扩展 Release ON / OFF / ASAN+UBSAN 构建 | 全部成功 | `double-review-r1-{build,off-build,asan-build}.log` |
| 完整 SQL，FFmpeg ON | 18 用例 / 1,596 断言；1 个非 GPL codec 用例按条件跳过 | `double-review-r2-sql-on.log` |
| 完整 SQL，FFmpeg OFF | 12 用例 / 1,143 断言；6 个视觉和 1 个非 GPL codec 用例按条件跳过 | `double-review-r2-sql-off.log` |
| 写入 C++，Release / ASAN+UBSAN | 各 23 用例 / 6,979 断言 | `double-review-r1-copy{,-asan}.log` |
| 写入 C++，FFmpeg OFF | 13 用例 / 1,610 断言 | `double-review-r1-copy-off.log` |
| 写入 C++，ASAN+UBSAN+LSan 子集 | 21 用例 / 6,298 断言 | `double-review-r2-copy-lsan.log` |
| 嵌套读取/缓存/时间戳，Release / ASAN+UBSAN+LSan | 各 19 用例 / 1,089 断言 | `double-review-r2-nested{,-asan}.log` |
| 编码线程池，ASAN+UBSAN+LSan | 6 用例 / 1,390 断言 | `double-review-r2-codec-asan.log` |
| producer，ASAN+UBSAN+LSan | 6 用例 / 69 断言 | `double-review-r2-producer-asan.log` |
| decoder SQL，ASAN+UBSAN+LSan | 1 用例 / 179 断言，包括反复打开、LIMIT 和损坏输入 | `double-review-r2-decoder-lsan.log` |
| 真实 HTTPFS，ON / OFF / ASAN+UBSAN+LSan | 53 / 36 / 53 项通过 | `double-review-r1-httpfs.log`、`double-review-r2-httpfs-{off,asan}.log` |
| 混合分辨率视频独立对照 | 7 COPY / 52 episode-camera 编码 / 156 帧 | `double-review-r2-mixed-video.log` |
| 双深度相机补充实验 | 2 COPY / 4 视频 / 48 帧，像素与 GOP 检查通过 | `double-review-r2-two-depth.log` |
| LeRobot 0.6.1 分片/读回对照 | 9 数据集 / 42 帧由双方读回 | `double-review-r2-shards-oracle.log` |
| benchmark 子进程统计 | 6 个 Python 测试通过 | `double-review-r1-python.log` |
| 格式与静态解析 | 25 个 C++、11 个 Python；JSON/YAML、`git diff --check` 通过 | `double-review-r2-format.log` |

回归测试使用实际生产实现，C++ filesystem/encoder 只控制失败与取消发生的
边界。双深度相机是本轮本地补充实验，SQL 和 FFmpeg writer 日志保存在
`build/double-review-r2-two-depth/`，没有将它算作已接入 CI 的新用例。

复现本轮关键检查（在该工作树，使用已构建的 runner 和固定依赖）：

```bash
build/write-io-test-release/test/lerobot_video_io_test
build/cleanup-test-off/test/lerobot_video_io_test
build/nested-test-release/test/lerobot_nested_query_test
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  build/write-io-test-asan/test/lerobot_video_io_test \
  '[video_io],[video_copy_cancel],[copy_cleanup],[copy_bind],[numeric_stats],[image_writer],[image_copy]'
python3 test/conformance/test_session_settings.py \
  --duckdb build/mixed-video-cli \
  --extension build/audit/extension/lerobot/lerobot.duckdb_extension
python3 test/conformance/test_mixed_video.py \
  --duckdb build/mixed-video-cli \
  --extension build/audit/extension/lerobot/lerobot.duckdb_extension \
  --codec libaom-av1 --fps 30 --episode-lengths 1 2 3 6 \
  --workdir build/reproduce-double-review-mixed
```

最终源码、测试、二进制及日志的 SHA-256 记录在
`build/double-review-final-sha256.json`。两个审查输入快照保持不变。

## 仍未覆盖的范围

- 本轮在 Linux x86_64 运行，未执行 Windows、macOS、ARM 或托管 CI。
  编译配置接入不等于这些平台已经验证。
- sanitizer 插桩覆盖扩展及隔离 C++ 目标；复用的 DuckDB host、HTTPFS、系统
  FFmpeg/codec 库未重新插桩。完整视频写入测试使用 `detect_leaks=0`，上表
  明确标出的 LSan 子集和 decoder/HTTPFS 等检查使用 `detect_leaks=1`。
- 本机既有 TSan 运行时限制未改变，本轮没有宣称 TSan 通过。
- 远程缓存校验遵循宿主文件缓存、预取和 ETag 策略，不提供贯穿查询全程的
  ACL/配置事务快照，也无法检测指纹完全相同的内容替换。
- 路径检查是 lexical containment；符号链接依然遵循 DuckDB 文件系统规则。
  COPY 的发布点仍是目录 rename，没有新增事务式 append 或远程发布保证。
- 历史 benchmark 未重新测量；本轮没有以性能结果代替正确性验证。

## PR 提交前补充复核

PR 模板要求连续两轮本地代码审查无发现。上述第一轮修复后，第二轮为第一轮
无发现审查；提交 PR 前再次检查完整生产代码变更、构建接入和相关测试，未发现
新增代码问题。生产代码及测试逻辑与第二轮输入快照的 SHA-256 一致，补充这轮
后满足连续两轮本地代码审查无发现。GitHub 的两轮 `@codex review` 单独记录在
PR 对话中，不与本地审查混计。

提交准备另补全 `THIRD_PARTY_NOTICES.md` 中直接使用的 PyArrow、pandas 及
运行时生成 fixture 的来源说明；因此 PR 提交文件数为 60。原始审查记录及
快照仍描述当时的 58/59 文件范围。`build/` 中日志和二进制为本地证据，不随
PR 上传；CI 结果以 PR Checks 为准。

提交前再次运行固定 LeRobot 0.6.1 的双向读写、视觉 conformance、数值统计
oracle（27 组 / 1,440 次精确数组比较）及 Pillow 图像验证（16,744 张）均通过。
本地新增日志为 `build/pr-submit-{bidirectional,numeric-oracle,visual-oracle,image-oracle}.log`。
