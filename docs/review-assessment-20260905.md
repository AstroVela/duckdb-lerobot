# 对 2026-09-04 review 的复核

评估日期：2026-09-05。原 review 的目标为 `81db162`；本次拉取后的分支为
`v1.5-variegata @ fab70ad4effa1e5e5dda43e58a99cc17d70bb5d4`，DuckDB 仍为
`d8cdaa33` / v1.5.5。评估分支：`review/v1.5-variegata-20260905`。

这份 review 值得采纳，但不能直接作为当前版本的待办或发布阻塞清单。
AVIO 泄漏、路径检查、嵌套连接、编码取消延迟等有明确代码依据；若干结论的
影响范围、严重度或数量不准确；四个后续 PR 已经解决了一部分问题。
本文只评估和保留复现证据，没有修改产品代码。

## 版本差异

| 原提交之后的提交 | 与 review 有关的变化 |
| --- | --- |
| `943726e` / #22 | FFmpeg 与 sanitizer CI、许可证、依赖声明、数值双向 conformance |
| `ca5260c` / #24 | 移除隐式 HF repo-id 改写；`lerobot_scan` 替代旧 scan/layout API；显式命名参数、原生 bind replacement |
| `3172ef4` / #25 | 稳定 `target_id`；深度量化选项和显式 clipping 契约；线程预算改为可恢复错误；视觉 conformance、LIMIT 回归 |
| `fab70ad` / #23 | extension-ci-tools、vcpkg、发布工作流、便携 AV1 编码、按实际 codec 判断 SVT 锁 |

`docs/review-followups.md` 是已有的设计决策记录，本次以代码和复现为准，
不把它声明的历史验证当作本轮重新执行的结果。

## §2：所谓五个 P0

| 条目 | 对 `81db162` 的判断 | 当前状态与优先级 |
| --- | --- | --- |
| 2.1 AVIO buffer 泄漏 | 成立。自定义 AVIO 的 buffer 需要单独释放。初始大小为 64 KiB，FFmpeg 可替换或调整 buffer，故不能保证每次最终泄漏恰好 64 KiB。 | **本次已实测，仍在，建议 P1 / 发布前修复。** `src/function/video/lerobot_video_frames.cpp:940-962` 仍只释放 context；10 次解码查询泄漏 655,360 字节，LSan 栈指向 LerobotShardDecoder::Open。 |
| 2.2 路径校验缺口 | 成立。本次证据比原 review 更强：**Linux 上 `..` 也可导致越界读取**，因为原生 Parquet reader 会展开目录；反斜杠在 Linux 上是普通字符，Windows 上则另有穿越风险。 | **本次已实测，仍在，P1 安全修复。** `src/storage/lerobot_metadata_cache.cpp:130-141,320-321`。`data_path='..'` 配合 `union_by_name=true` 成功返回了 dataset root 之外、由本次复现创建的 Parquet 标记。不能进一步泛化成 `hf://` 会切换到本机文件系统。 |
| 2.3 Windows concat | 常规 Windows 本地路径会被拒绝，成立。触发条件是**同一个 shard 有多个 episode fragment**；不必跨越文件大小阈值。Linux 上合法的单引号路径也被拒绝。 | **仍在；Windows 发布阻塞 / P1。** `lerobot_copy.cpp:1822-1869`。全面引入写侧 AVIO 是一种方案，并非修复 Windows 路径的必要前置。 |
| 2.4 深度常量及 clipping | 常量和 clipping 属实；默认饱和也是 LeRobot 兼容性策略，不能把“默认必须报错”当作唯一正确契约。 | **参数化已完成。** `lerobot_copy_options.cpp:101-110`；`DEPTH_CLIP` 默认 true，false 时严格拒绝越界，README 有明确说明。是否改默认值是 API 决策。 |
| 2.5 可达 InternalException | **本次在重新编译的 `81db162` 上实测成立**：PREPARE → SET threads → EXECUTE 后，下一条 SELECT 报 database has been invalidated。影响是一整个 DatabaseInstance 失效，既非只读，也不是持久数据库文件损坏。 | **本次对照实测已修复致命后果。** `lerobot_codec_executor.cpp:211-214` 改为 `InvalidInputException`，COPY 失败后同连接 SELECT 42 成功；没有实现自动 clamp。保留可恢复错误是合理方案，clamp 不是“零风险”修改。 |

FFmpeg 的[官方自定义 AVIO 示例](https://ffmpeg.org/doxygen/7.1/avio_read_callback_8c-example.html)
在释放 AVIO context 前释放其当前 buffer；[实现](https://ffmpeg.org/doxygen/7.1/aviobuf_8c_source.html)
也支持上述所有权判断。当前 CI 的原因可直接确定：
`.github/workflows/native-ci.yml:118` 明确设了 `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`。
该作业确实设置视觉测试开关，所以不能再把“视觉用例没跑”和“LSan 关闭”混作未知原因。
另一个时间上的不一致是：`81db162` 本身还没有 `.github/`，不能把后续 CI 说成该提交的既有设施。

原文的 InternalException 全量统计也少算了两个文件：`81db162` 实际有 **61** 个
`throw InternalException`，其中 video writer 2 个、temporal targets 3 个未计入
原文的 56 个。补漏的站点看起来也在保护内部不变式，但原统计不能支持
“已经完整枚举全部站点”的表述。

路径修复应明确本地路径、远程 URI、模板片段、video key 的不同契约。拒绝 `..`
与反斜杠穿越可以是独立小修复；词法 containment 不会自动阻止符号链接，
也不等价于对所有远程 filesystem 的授权检查。concat 列表即使改成内存 AVIO，
demuxer 仍可能自行打开列表中的子文件；若要完全接管 I/O，还需接管子文件打开，
或直接 remux 受控输入。不能只换列表存储方式便宣称“去路径化”。

## §3：设计与 API

| 条目 | 判断与当前处置 |
| --- | --- |
| 3.1 相对 root 被改写 | 历史行为成立，但原来只识别合法字符构成的两段 repo-id，并非所有相对路径。`ca5260c` 已删除改写；现在要求显式 `hf://`。 |
| 3.2 六处 Connection | 构造点的计数和 LOCAL 设置不继承基本准确，目前仍是三个 metadata、一个 FEATURES parser、两个 video 构造点。取消传播与重入值得维护，但这些查询主要读取外部文件/常量 JSON，不能据此推断已发生事务一致性损坏。`lerobot_info` 的原生 bind replacement 已让它直接使用调用方设置，episodes/tasks 的元数据解析仍没有。 |
| 3.3 每批 timestamp SQL | 成立，优先作为测量驱动的 P2 优化。批量是 `min(当前输入 chunk 剩余行数,max_pending_targets)`，默认 DuckDB vector 为 2048，4096 是上限而非稳定批大小；百万行通常约 489 批或更多，不能直接按 250 次算。BuildTargetBuffers 当前无条件调用 ReadTargetTimestamps，投影不需要像素也不能消除这一查询。 |
| 3.3 的缓存方案 | 可研究，不能直接承诺“一帧 8 字节即可解决全部问题”。8 字节只是 timestamp payload 下限，不包括稀疏键、重复/缺失校验、索引和构建峰值。还需定义并发加载、取消、内存计账、shard 身份与失效。`bind_replace` 解决 SQL 文本构造不必然解决执行期数据访问成本。已有 `benchmark/timestamp_lookup.py` 可作为实验起点。 |
| 3.4 两种 ordinal 都不稳定 | **部分错误。** temporal/video targets 的 `target_ordinal` 确为原子分配；video windows 的 `request_ordinal` 是输入列表下标，`81db162` 就如此，稳定。当前 README 已明确前者的无序契约，并新增调用者提供的 `target_id`。无序关系在没有显式输入顺序时也不存在可凭空生成的稳定“输入行号”。 |
| 3.5 继承全部原生参数 | 历史属实，当前改成显式参数定义。其附带说法“info/episodes/tasks 接受 refresh 但无效果”是**错误**：旧版没有在这些函数上注册该命名参数，binder 先拒绝，`ParseOption` 分支不可达。当前 refresh 已注册并会清除两个 route cache。 |
| 3.6 COPY 输入顺序 | 契约成立，但 README 原来已有 contiguous/ordered 说明及 ORDER BY 示例，不能说完全未记录。现在仍可改善错误提示；建议按 episode_index 和原始帧顺序列排序，输出 frame_index 是扩展生成的。无序来源只能运行时发现，bind 时不能一般性证明顺序。 |
| 3.7 producer 析构不 Finish | 取消时计数可能不归零的推导成立，但原文也没有指出实际用户故障，建议 P2 / 生命周期治理。当前已有 LIMIT 1 后继续查询的回归。**不要直接在析构中调用现有 Finish**：ProducerFinished 会 flush buffer，可能分配内存，不能自然满足 noexcept 析构要求；需要单独设计不抛异常的取消收尾。 |

## §4：性能与资源逐项

| # | 复核 |
| --- | --- |
| 1 | Value 装箱、递归与多轮统计读取仍在，合理优化候选；“最容易优化的热点”需 profile，修改前补统计参照。 |
| 2 | RGB image 每帧重新开 PNG encoder 且同步编码仍在。复用 encoder 是候选，不据源码猜具体加速倍数。 |
| 3 | episode 边界/生成列逐行 GetValue/SetValue 仍在，可独立向量化。 |
| 4 | log bounds 在像素函数里计算的源代码结构仍在，但实际每帧 60 万次多余 log 调用需要检查优化后的代码或 profile；不能忽略常量折叠/循环优化。参数化后更值得预计算。 |
| 5 | episodes/tasks 每次 bind 解析 info 仍在，与 3.2 同一原因，避免重复立项。 |
| 6 | 原始帧 spool 不受 temp_directory / max_temp_directory_size 控制，磁盘预算缺口成立；它按 episode 编码后删除，不是整个数据集的全部原始帧永久累计。区分 disk 使用和此前已解决的 bounded memory。 |
| 7 | pool 不缩容、10 ms 轮询成立；这是资源/延迟权衡，不等于线程泄漏。 |
| 8 | 输出过滤在 Produce 后执行成立；只有需要像素输出才发生像素解码，不能泛称任意 WHERE 都解码全部帧。显式 frame_indices 在解码前缩减目标，适合文档说明和后续谓词优化。 |
| 9 | 每次有效性检查做 open/stat 成立；实际 HF HTTP 请求次数取决于宿主 filesystem 的缓存、HEAD/GET 策略，不能从扩展源码保证“每次至少一个 HEAD”。 |
| 10 | 取消的排队 job 等 worker 取走才计为完成，延迟缺口成立。只有其它工作占满 worker 等条件下才被别的批次拖延；等待界限是有 worker 回到循环，不必是对方整个 episode 完成。当前同步等待保住 FileSystem 指针生命周期，没有证实 UAF。 |
| 11 | 默认 min(4,threads) 确实限制大主机单相机速度，但原 README 已明确说明默认值，不能说没有提示。修改默认前比较吞吐和多个并发 COPY 的资源占用。 |
| 12 | 只在 Execute 入口扩容成立。退化需低 host_thread_budget 的存量 batch 与较高 host budget 的新 batch 重叠，通常涉及中途修改 threads；不能由“某批 VIDEO_WORKERS=1”单独推出池被卡成 1。 |
| 13 | 每 episode 等编码结束，缺少摄取/编码重叠，成立。单相机没有跨相机 job 并行，仍可能使用 codec 内部多线程；不能扩展成“单相机完全没有任何并行”。episode 流水线要同时设计磁盘、内存、取消与发布顺序。 |

## §5：代码卫生逐项

| # | 当前结论 |
| --- | --- |
| 1 | `pixels` 死代码已删除。 |
| 2 | video key 重复检查仍被 feature_names_seen 的更强检查覆盖，可删。 |
| 3 | codec/pixel format 已使用编码结果并校验跨 episode 一致性，主要问题已解决。 |
| 4 | pandas metadata 的 pyarrow/pandas 固定版本仍在，属于 provenance 清理。 |
| 5 | FLOAT 数据列与 DOUBLE 统计的两条 timestamp 路径仍在，位级兼容边界成立；需要明确可复现数值和平台，不能由公式差异推出常规数据已经不兼容。 |
| 6 | deprecated max_open_shards alias 已移除。 |
| 7 | 清理失败被吞仍在。析构不能随意抛异常，建议 best-effort 清理后记录日志/诊断。 |
| 8 | episodes 的递归 Parquet glob 仍在；无关 Parquet 可导致 schema 错误，属于布局约束和诊断体验问题。 |
| 9 | FFmpeg=ON 缺依赖现在明确 FATAL_ERROR，原问题已解决；显式 OFF 的 metadata-only 构建仍是受支持能力组合。 |
| 10 | 多相机视觉用例仍没有先 SET threads，且显式要求两个 worker/codec thread。**仍需修复测试的资源前置条件。** |
| 11 | rotation 按 fragment 大小之和判断仍在。可以讨论 shard 兼容性，但文件布局不相同不自动意味着 LeRobot 格式不兼容；需阈值用例及原生实现参照后确定严重度。 |
| 12 | encoder_threads 仍为 optional_idx，唯一调用方设置值，属于可简化的内部接口。 |
| 13 | 选项解析已拆文件，但三段范围检查重复仍在；低优先级。 |
| 14 | codec open/free 的锁内外重复分支仍在；合并可提高可读性，不是正确性修复。 |
| 15 | concat 列表仍由 COPY 生成；与 2.3 一起处理，避免重复项目。 |
| 16 | /proc 采样无法保证终态统计，成立；但“Finalize 全部 I/O 丢失、wchar_ratio 必变 None”不能从实现推出。循环在 poll 前采样并保留历史最大值；尾部可能漏采、长 Finalize 也可能被采到，只有基准 wchar 缺失/为零才会让 ratio 为 None。重复 helper 是独立清理项。 |
| 17 | SVT 锁已按实际 codec 名 `is_svt` 判断，主要分派问题修复；混合分辨率并发和具体旧版 SVT 的证据仍不足。 |

## §6：测试建议的取舍

“没有外部 oracle / 没有 LICENSE、CI、vcpkg、ci-tools、clang-tidy”在原提交上
基本准确，在当前提交上已经过时。仓库现在有固定 LeRobot 0.6.1、NumPy 2.2.6、
PyAV 15.1.0、Pillow 12.3.0 等依赖的数值与视觉双向 conformance，且视觉测试
直接比较官方 depth quantizer，超出了仅自产自读。

仍然缺少完整统计/采样/resize 的跨平台参照、真实混合分辨率并发、显式用户
中断与失败注入、远程固定 revision smoke、TSan，以及开启 LSan 的解码检查。
默认视觉测试仍需要环境开关；已配置在 CI 执行不等于默认本地测试会执行，
也不等于本轮已经验证所有 hosted job。外部动态生成 fixture 加版本锁也是
有效 oracle，不必先把全部媒体二进制和哈希入库才能推进修复。

## 建议下一轮任务

1. 单独修 AVIO buffer 生命周期，并加真正启用 LSan 的 decoder 正常/失败关闭验证。
2. 补模板展开后和 video key 的跨平台片段校验；补 Windows、本地单引号路径 concat 用例。
3. 修视觉 SQL 测试的 threads 前置条件；处理编码队列的可取消摘除，并验证并发 COPY 下的取消延迟。
4. 补调用方 file_search_path 与取消传播；按证据选择 metadata/bind 结构改造。
5. 在统计/resize oracle 和 timestamp benchmark 上补证据，再决定缓存、向量化及 episode 流水线。

不建议把默认 clip、线程数默认值、ObjectCache 索引、写侧 AVIO、宏替换和所有
P3 清理合成一个“修 P0”大改。严重度应由可达影响和支持矩阵决定，不能用
P0 同时表示内存泄漏、平台不支持和 API 偏好。

## 本轮额外发现：重复 configure 丢失 FFmpeg include 路径

**P2，已实测，位于当前 `src/CMakeLists.txt:34-35,58-67`。** 在非系统目录提供
libswscale 开发包、通过 pkg-config 查找时，第一次 configure 的编译命令有正确的
include 目录；同样环境、同样参数第二次 configure 后，该 include 目录消失。
继续编译会报 `libswscale/swscale.h: No such file or directory`。

原因是项目先把 `LEROBOT_FFMPEG_INCLUDE_DIRS` 清空，却把同一前缀交给
`pkg_check_modules`；后者复用缓存时没有重新计算已被普通变量覆盖的值。
vcpkg 分支已有缓存处理，pkg-config 分支仍有这处问题。本轮独立运行两次
相同 configure，记录为 `build/config-probe-results.json`：第一次 true、第二次 false。
评估构建通过清除 `__pkg_config_checked_LEROBOT_FFMPEG` 重新查找继续完成。
建议修复时把项目汇总变量与 pkg-config 输出前缀分开；本次没有修改 CMake 产品代码。

## 本次执行证据

以下运行日志、复现程序和生成数据保留在本 worktree 的 `build/` 下；这些是
评估辅助材料，不进入产品代码或永久测试套件。

| 验证 | 本轮结果 |
| --- | --- |
| 当前源码 FFmpeg 编译 | 新建 `build/audit`，Release，FFmpeg 开启，动态扩展、hidden visibility；所有扩展源文件编译通过。复用已有 v1.5.5 宿主库，没有声称从零完成整个 DuckDB 构建。 |
| 加载身份 | `build/audit_runner` 禁止宿主预加载旧 LeRobot，显式加载当前扩展；查询返回 `extension_version=fab70ad`。 |
| 现有 SQL suite | 复用 PR #23 portable 构建及 unittest runner。该 worktree 的 `b03ef9c` 与 `fab70ad` Git tree 一致；在本次 worktree 的测试文件上运行，启用 `LEROBOT_NON_GPL_FFMPEG_TESTS=1`，9 个用例 / 865 条断言通过，5 个 `LEROBOT_FFMPEG_TESTS` 文件跳过。日志：`build/sql-suite.log`。这不是全视觉套件通过的声明。 |
| 最新代码定向 SQL | `build/reproduce_review.py` 与 `build/review-results.json`：路径、会话设置、预算错误恢复、单线程 bind、concat、RGB 编解码和 video key 均有独立 SQL 与结果。 |
| 原 review 提交的对照 | 通过 `git archive 81db162 CMakeLists.txt extension_config.cmake src` 导出精确旧源码并重新编译 FFmpeg 扩展到 `build/baseline`。`81db162_budget` 复现数据库失效；`81db162_refresh` 证实命名参数在 binder 被拒绝；`81db162_repo_rewrite` 复现 HF 改写。未使用无版本身份的旧 artifact 作为该提交的运行证据。 |
| LSan | `build/audit_runner_asan` 以 `detect_leaks=1:fast_unwind_on_malloc=0` 运行 `build/lsan.sql`。10 次 RGB 帧解码均返回 12,288 bytes，退出时报告 **655,360 bytes / 10 allocations**；栈经 FFmpeg realloc/probe/open 回到本次编译的 `LerobotShardDecoder::Open`。日志：`build/lsan.log`。这是对实际解码路径的分配泄漏检查，不是全项目 ASAN/UBSAN instrumentation 认证。 |
| 输入顺序与 ordinal | 根据 `81db162` 和当前源码核对；未声称并发重排已通过压力测试重现。 |
| 平台与外部验证边界 | 本轮未执行 Windows/macOS/ARM、TSan、网络 HF 或完整 Python conformance；相关判断按代码与现有测试定义标识。 |

关键复现均使用自行生成的临时数据，未读取其它用户数据。例如，`parent`
是 dataset root，`outside.parquet` 是其兄弟文件，内容仅为本轮写入的固定标记：

```sql
-- parent/meta/info.json 中设置 "data_path": ".."
SELECT DISTINCT audit_marker
FROM lerobot_scan('/.../review-fixtures-.../parent', union_by_name := true)
WHERE audit_marker IS NOT NULL;
-- outside dataset root
```

当前线程预算恢复验证：在 `threads=4` 时 PREPARE 一个 `ENCODER_THREADS 4`
的视觉 COPY，改为 `threads=2` 后 EXECUTE，得到可恢复的 Invalid Input Error；
随后同一连接 `SELECT 42 AS healthy` 正常返回。单线程验证则在 `threads=1`
时使用测试中的 `VIDEO_WORKERS 2, ENCODER_THREADS 2`，得到预期 Binder Error。

`file_search_path` 验证中，相同连接的原生 `read_json_auto` 与新版
`lerobot_info` 都成功，`lerobot_episodes` 的内部 info 查询失败。这把 3.2
从泛泛的设置推导缩小为一个仍可复现的用户行为差异。

本轮还用真正只有两个路径分量的本地 `build/two-part-...` 成功读取了两行
数据，确认当前相对路径行为已修复，而不只是测试旧版本也不会改写的三段路径。
