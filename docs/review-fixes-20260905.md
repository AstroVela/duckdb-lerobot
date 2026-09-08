# Review 修复记录（2026-09-05 起）

修复分支：`fix/review-correctness-20260905`。基线为拉取后的
`v1.5-variegata @ fab70ad4effa1e5e5dda43e58a99cc17d70bb5d4`，
DuckDB 子模块为 v1.5.5 / `d8cdaa33`。

## 本批变更

| Review 项 | 处理 |
| --- | --- |
| §2.1 AVIO 泄漏 | `Close()` 先释放当前 `avio_context->buffer`，再释放 context；正常关闭及构造失败共用该路径。 |
| §2.2 路径逃逸 | 数据、视频模板在展开后逐段校验；元数据和 COPY 共用 feature name 校验。拒绝父目录、点目录、空段、反斜杠、盘符/流语法、通配符、URI 转义/分隔符和控制字符。合法的嵌套模板、Unicode、内部空格及单引号继续可用。 |
| §2.3、§5.15 concat 路径 | 列表生成下沉到 writer，只接受同一 staging 目录内的生成片段；显式使用本地 `file:` URL、单引号转义和 `file` 协议白名单；Windows 路径转换为 `/` 分隔符。检查列表是否完整写入。 |
| §2.5 线程预算 | 当前基线已将运行期错误改成 `InvalidInputException`。补充 PREPARE → 降低 threads → EXECUTE 的回归，验证错误可恢复且 staging 被清理。 |
| §5.2 重复检查 | 删除已被 feature name 去重覆盖的 `video_keys_seen` 检查。 |
| §5.10 单线程测试 | visual COPY 用例显式设置 `threads = 2`，使其声明的编码预算不依赖 runner 的默认线程数。 |
| 评估中新发现的 CMake 问题 | 将扩展的 FFmpeg 汇总变量与 `pkg_check_modules` 缓存输出分开，防止第二次配置丢失非系统头文件目录。 |
| §6 sanitizer 覆盖 | CI 增加开启 `detect_leaks=1` 的 decoder 专项；覆盖 LRU、重复打开、提前 LIMIT 和打开失败。 |

AVIO 的释放顺序与 [FFmpeg 官方自定义 AVIO 示例](https://ffmpeg.org/doxygen/7.1/avio_read_callback_8c-example.html)
一致。concat 保留现有 demuxer 的时间戳处理，并按
[FFmpeg concat 文档](https://ffmpeg.org/ffmpeg-formats.html#concat-1)生成列表。
绝对 `file:` URL 需要 `safe=0`；列表来自扩展内部，协议白名单只允许 `file`。
不能直接使用相对片段名：FFmpeg 会把列表所在本地目录中的 `#` 和 `?`
当作 URL 分隔符，导致片段路径解析错误。本次已验证这两个字符以及单引号目录。

## 验证结果

| 检查 | 结果 |
| --- | --- |
| FFmpeg 开启的扩展编译及 SQLLogicTest | 14 个用例、1321 条断言通过；跳过要求非 GPL FFmpeg 的 1 个用例。 |
| FFmpeg 关闭的扩展编译及 SQLLogicTest | 9 个用例、965 条断言通过；按条件跳过 6 个视觉/便携 FFmpeg 用例。 |
| visual COPY，`--single-threaded` | 95 条断言通过，包含引号/空格/`#` 目录拼接和线程预算回归。 |
| decoder SQLLogicTest + LSan | 179 条断言通过，进程正常退出，无泄漏报告。 |
| 修复前后相同的 10 次解码 | 修复前 LSan 报告 10 块缓冲、655360 字节泄漏；修复后无泄漏报告。 |
| 路径回归的反向验证 | 相同源码基线的旧扩展在 `data_path='..'` 时实际读出受控 sibling 文件中的 `outside dataset root`；新扩展在交给 Parquet reader 前拒绝它。 |
| 连续两次 CMake 配置 | 两次均保留非系统 FFmpeg include 目录；修复前第二次丢失。 |
| CI YAML、diff | YAML 解析及 LSan 环境检查通过，`git diff --check` 通过。 |

本机只重新编译扩展，复用固定 v1.5.5 的 DuckDB host 和 SQLLogicTest runner。
runner 在独立输出路径重新链接并导出符号，通过测试配置的 `on_init` 加载本次扩展，
避免误测 runner 中静态链接的旧 LeRobot。LSan 在 Release 扩展上拦截堆分配；
这不等同于本机重编译并运行完整 ASAN/UBSAN DuckDB。完整 sanitizer 构建由 CI 验证。
本机详细输出保存在忽略的 `build/fix-*.log` 和 `build/fix-*-results.json`。

## 2026-09-06：相对路径与会话缓存隔离

修复 §3.2 中的文件定位问题。`file_search_path` 既不会被新建连接继承，
也不会被指纹读取时的 `OpenFile` 自动应用，因此仅向嵌套连接复制设置不足以解决问题。

- 新的 root resolver 在调用方上下文中定位 `meta/info.json`，随后将明确的本地路径
  传给 JSON/Parquet 查询及视频 AVIO。`~` 使用调用方的 `home_directory` 展开。
- 按 DuckDB 的非 glob 查找规则优先选择当前目录；搜索路径中存在多个候选数据集时，
  明确报错并要求显式路径，防止不同目录的元数据与 shard 混合。
- 两级缓存的相对 root 键包含工作目录和展开后的搜索路径；不同连接及设置切换不会
  串用缓存。缓存验证还比较实际 root，避免查找位置改变时沿用旧数据集的指纹。
- `lerobot_cache_info` 仍只查看已有缓存。在缺失/歧义路径以及关闭外部文件访问时，
  均不执行存储查找。`refresh` 只失效当前查找身份对应的两个缓存。

新增 `test/sql/lerobot_search_path.test`，覆盖全部读取入口、跨连接隔离、设置切换、
多搜索目录、歧义拒绝、tilde 展开和被动缓存诊断；visual COPY 用例复用真实视频，
验证 `video_frames`、`video_windows`、`video_targets` 的相对 root 解码。

| 检查 | 结果 |
| --- | --- |
| 同一回归对比修复前扩展 | `lerobot_info('dataset')` 成功后，旧 `lerobot_episodes('dataset')` 因找不到 info.json 失败；修复后通过。 |
| FFmpeg 开启的完整 SQLLogicTest | 15 个用例、1416 条断言通过；跳过要求非 GPL FFmpeg 的 1 个用例。 |
| FFmpeg 关闭的完整 SQLLogicTest | 10 个用例、1055 条断言通过；按条件跳过 6 个用例。 |
| visual COPY，`--single-threaded` | 100 条断言通过。 |
| 缓存预热后新增当前目录同名数据集 | 同一连接、相同搜索设置下，聚合结果从搜索目录的 3 正确切换为当前目录的 300。 |
| 格式与 diff | 修改行已运行 clang-format，`git diff --check` 通过。 |

验证沿用上述固定 DuckDB runner，日志位于 `build/search-path-*.log`，
当前目录优先级验证保存在 `build/search-path-priority-results.json`。
本次只解决文件定位，其他会话设置及取消/事务传播仍是独立问题。

## 2026-09-06：编码队列取消与收尾

修复 §4.10。COPY 的等待线程观察到中断或本批编码错误后，直接从共享队列
移除本批尚未被领取的任务，并分别在对应锁内更新剩余任务数和完成数。
这样即使所有编码线程都被另一条 COPY 占用，取消也不再依赖那批编码结束。

已经被 worker 领取的任务仍走原来的收尾路径，等待线程不会提前返回；
因此编码器借用的 `FileSystem` 在使用期间仍然有效。worker 先归还线程预算，
再发布任务完成。队列清理不同时持有结果锁和共享队列锁，且每个任务只由
队列清理或领取它的 worker 计数一次。首个编码错误保留，查询中断仍优先报告。

新增 `test/cpp/test_codec_executor.cpp`，直接编译生产 executor，链接受控的
测试 encoder；不需要 FFmpeg，也不向生产接口添加测试开关。条件变量控制
编码入口和退出，覆盖：

- 单线程池被另一批占满时，取消排队批次仍可返回，另一批保持运行。
- 取消必须等待自己的运行中 encoder，排队的其他 encoder 不再启动。
- 注入编码失败后，等待自己的运行中 encoder，保留原始错误；其他批次及
  后续批次继续执行。
- 编码乱序结束时结果仍按输入顺序返回，不均匀线程预算仍正确分配。
- 32 轮中断及 worker 收尾交错，每轮 32 个任务，验证完成计数及预算复用。

原生 CMake 增加 `test_lerobot_codec_executor` 目标；三个 native CI 配置均执行
该套件，sanitizer 构建同时启用泄漏检测。README 补充取消语义和运行命令。

| 检查 | 结果 |
| --- | --- |
| 修复前 executor + 同一回归 | 排队批次等待 5 秒仍无法取消；释放另一批 encoder 后才能收尾，该项稳定失败。其余 4 个用例通过。 |
| 修复后 executor 回归 | 5 个用例、1172 条断言通过。 |
| executor 和测试代码启用 ASAN + UBSAN + LSan | 5 个用例、1172 条断言通过，无 sanitizer 报告。DuckDB host 沿用未重新插桩的固定版本。 |
| FFmpeg 开启的完整 SQLLogicTest | 15 个用例、1416 条断言通过；按条件跳过 1 个用例。 |
| FFmpeg 关闭的完整 SQLLogicTest | 10 个用例、1055 条断言通过；按条件跳过 6 个用例。 |
| 原生 CMake / CI | 已确认测试目标注册、用原生 C++11 Release 编译参数编译两个测试源文件、校验 CI YAML；`git diff --check` 通过。 |
| TSan | 已编译，但本机运行期不能启动；不依赖 DuckDB 的最小原子变量/线程程序也报告 `ThreadSanitizer: unexpected memory mapping`。本次不计为通过。 |

日志位于忽略的 `build/codec-executor-*.log`、`build/codec-test-*.log`、
`build/codec-cancel-*.log` 和 `build/codec-ci-*.log`。
本次保留 10 ms 的中断检查周期；已经进入 FFmpeg 的调用仍需要自身返回才能
响应取消。这些测试验证 executor 的并发和生命周期，不证明所有真实 codec
调用都有固定的取消延迟。线程池动态扩容和 episode 流水线属于其他后续项。

## 2026-09-06：读侧 producer 取消收尾

修复 §3.7。`Executor::CancelTasks()` 在持有 executor 锁时销毁被挂起的任务；
旧 producer 没有析构收尾，因此两个已进入 `TASK_BLOCKED` 的任务被销毁后，
保留的 producer state 仍报告 `active_producers = 2`。本次用真实的 DuckDB
任务挂起/取消接口复现了这一点，1 线程和 4 线程配置均失败。

- producer 正常、异常和析构路径共用一次性完成标记，避免重复扣减活动计数。
- 析构按放弃任务处理：设置停止标记、归还计数、清理尚未领取的缓冲，不发布
  尾批，也不重新调度其他任务。正常完成仍由最后一个 producer 发布尾批。
- 消费者已经领取的缓冲仍由消费者归还，取消不清除它对应的 busy shard 记录。
- 提升 blocked producer 的弱引用前先预留容器容量，避免分配失败时在 state
  锁内释放最后一个任务引用，再由任务析构重入同一把锁。

新增 `test/cpp/test_video_producer.cpp`。它包含实际 producer 实现，在测试中
保留 state 以检查 DuckDB 销毁任务后的状态，不增加生产 SQL 测试开关。
6 组用例覆盖真实 `TASK_BLOCKED` 取消、部分缓冲丢弃、正常尾批只发布一次、
消费者缓冲归还、错误保留、连接及 metadata/state 引用释放；每组检查后续查询。
SQL pipeline 用例还在 1/4 线程、synchronous/default 策略下各重复三次
`LIMIT 1` → 完整读取，检查提前结束后的数据库可用性。

| 检查 | 结果 |
| --- | --- |
| 修复前 producer + 首批 5 组回归 | 阻塞取消和部分缓冲放弃两组失败：活动计数分别停在 2 和 1。其余 3 组通过。 |
| 修复后的 C++ producer 套件 | 6 组、69 条断言通过。 |
| producer、辅助代码及测试启用 ASAN + UBSAN + LSan | 6 组、69 条断言通过，无 sanitizer 报告；DuckDB host 沿用固定的未插桩版本。 |
| FFmpeg 开启的完整 SQLLogicTest | 15 个用例、1446 条断言通过；按条件跳过 1 个用例。 |
| FFmpeg 关闭的完整 SQLLogicTest | 10 个用例、1085 条断言通过；按条件跳过 6 个用例。 |
| pipeline SQLLogicTest，`--single-threaded` | 146 条断言通过；用例自身会显式切换 1/4 线程。 |
| 真实 decoder + LSan | 179 条断言通过，包含重复打开及提前 LIMIT，无泄漏报告。 |
| 原生 CMake、CI、格式 | 新测试的 6 个源文件用原生 C++11 Release 参数编译通过；CI YAML、测试格式和 `git diff --check` 通过。 |

原生 CMake 增加 `test_lerobot_video_producer` 目标；三个 native CI 配置均运行，
sanitizer 配置启用泄漏检测。日志保存在忽略的 `build/producer-*.log`。
TSan 仍受前述本机运行时问题限制。本次解决任务计数及资源收尾，不宣称解决
所有嵌套查询的中断延迟；§3.2 的其他查询仍需单独处理。

## 2026-09-06：嵌套读取的取消传播

修复 §3.2 的四处读侧取消缺口：`info.json` 解析、episode 数据路由、
episode 视频路由，以及 `video_targets` 的 Parquet 时间戳查询。
这些连接仍使用 DuckDB 原生 JSON/Parquet 查询，但现在通过
`LerobotNestedQuery` 接收调用方的中断。

DuckDB 1.5.5 的 `ClientContext::Interrupt()` 只设置原子标记，没有可供
扩展订阅的取消回调；仅在 `Query`/`Fetch` 前后检查无法中断其内部的阻塞读取。
本次增加每个数据库共享的一个监测线程，活动期间每 10 ms 检查外层中断，
没有登记的读取时休眠。该线程只检查和转发原子中断标记，不执行查询或 I/O。
观测到取消后持续转发，避免内层查询启动时清除中断标记造成信号丢失。

生命周期由同一对象管理，销毁顺序为查询结果、取消登记、内层连接。
注销登记与监测线程通过同一把锁同步，保证销毁连接前不再访问借用的上下文；
监测线程不获取上下文的强引用，避免最后一个引用在该线程释放时触发数据库
析构并等待自身。准备或取数中发生取消时，错误统一保留为 `INTERRUPT`，
不再被元数据错误包装成 `BinderException` 或时间戳错误包装成 `InvalidInputException`。
未取消的查询继续保留原有错误语义。

新增 `test/cpp/test_nested_query.cpp`：

- 通过受控文件系统暂停真实 JSON/Parquet 的 Read，直接检查原生 reader 的
  内层 `ClientContext` 收到中断；关闭文件内容缓存，确保视频路由的二次读取
  不被缓存绕过。覆盖四处入口及 1/4 线程配置。
- 中断传到 reader 后仍保持读取阻塞，确认调用没有提前返回；释放读取后检查
  `InterruptException`、连接与文件句柄释放、失败元数据未入缓存、后续读取成功。
- 受控表扫描在 `SendQuery` 返回之后才启用阻塞，验证流式 `Fetch` 阶段仍能
  传播取消；模拟清除首个中断，验证后续信号重发。
- 覆盖进入前已取消、查询错误、提前丢弃未读完的结果、重复注册/注销，以及
  同一数据库中两条嵌套连接的取消隔离。

| 检查 | 结果 |
| --- | --- |
| 修复前四处读取 + 同一受控 I/O 回归 | 1/4 线程的 8 个组合均在等待 5 秒后失败，reader 未收到中断；释放 gate 后能够收尾。 |
| 修复后的 C++ 嵌套查询套件 | 4 组、445 条断言通过。 |
| 嵌套查询 helper、读侧辅助代码及测试启用 ASAN + UBSAN + LSan | 4 组、445 条断言通过，无 sanitizer 报告；DuckDB host 沿用固定的未插桩版本。 |
| 既有 producer C++ 回归 | 6 组、69 条断言通过。 |
| FFmpeg 开启的完整 SQLLogicTest | 15 个用例、1446 条断言通过；按条件跳过 1 个用例。 |
| FFmpeg 关闭的完整 SQLLogicTest | 10 个用例、1085 条断言通过；按条件跳过 6 个用例。 |

原生 CMake 增加 `test_lerobot_nested_query` 目标；三个 native CI 配置均运行，
sanitizer 配置启用泄漏检测。新测试的 7 个源文件按原生 C++11 Release 参数
编译通过，CI YAML、新 C++ 文件格式及 `git diff --check` 通过。
TSan 仍受前述本机运行时问题限制。对照源码、构建和运行日志保存在忽略的
`build/before-nested-query-fix/` 和 `build/nested-*.log`。

取消仍需底层文件系统响应内层中断，不能强制打断任意操作系统阻塞调用，也
不承诺固定 I/O 取消延迟。本次没有改变内部连接的事务或其他会话设置继承；
COPY 的内存 `FEATURES` JSON 解析连接及已有独立取消路径的视频 producer
不在本批替换范围内。

## 2026-09-06：编码线程池扩容不足

修复 §4.12。旧 `DesiredWorkerCountLocked()` 将在途批次最小的宿主线程
预算同时用于任务准入和线程池规模。若 A 在 `threads = 2` 时进入 executor，
随后将设置提高到 4 并提交四相机批次 B，B 入队时仍只准备两个线程。
A 结束后，任务准入已允许四个编码器，但 B 没有新的入队动作触发扩容，
于是仍只能使用两个 worker。

现在入队时以在途批次最大的宿主预算计算线程池目标规模，并以各批次剩余
worker 需求之和约束；累加在达到上界前判断，避免加法溢出。
实际编码任务的准入仍使用最小宿主预算，以及原有的每批 worker/codec
预算。提前准备的线程在预算不足时等待，所以 A 完成后 B 可以立即达到其
允许的并发度，无需第三次 `Execute`。线程创建仍在原来的入队异常清理范围
内，没有向 worker 完成路径增加可能抛异常的创建操作。

扩展 `test/cpp/test_codec_executor.cpp` 的受控 encoder，记录同时运行的任务
及编码线程预算峰值。新增回归组合正常完成、取消、编码失败三种收尾，
以及 4/6 两种总预算，覆盖四个 worker 均匀和不均匀分配编码线程的情况。
先等待 B 的首个可准入任务启动，确认 B 已完成入队和建池，再释放 A；
四个 B encoder 全部保持阻塞，必须同时启动才能通过，串行执行无法混过。
同时检查原有较低准入上限、取消/原始错误、排队任务清理、结果顺序与后续查询。

| 检查 | 结果 |
| --- | --- |
| 修复前 executor + 新增回归 | 6 个组合全部失败：释放较低预算批次后，仍有编码器等待 5 秒无法启动。 |
| 修复后的完整 executor 套件 | 6 组、1390 条断言通过；新增组合验证四个编码器同时运行，且预算峰值未超限。 |
| executor 和测试代码启用 ASAN + UBSAN + LSan | 6 组、1390 条断言通过，无 sanitizer 报告；DuckDB host 沿用固定的未插桩版本。 |
| FFmpeg 开启的完整 SQLLogicTest | 15 个用例、1446 条断言通过；按条件跳过 1 个用例。 |
| FFmpeg 关闭的完整 SQLLogicTest | 10 个用例、1085 条断言通过；按条件跳过 6 个用例。 |
| 原生 CMake / CI / 格式 | 两个源文件按原生 C++11 Release 参数编译通过；三个 native CI 配置继续执行整套 executor 测试，YAML 与格式检查通过。 |

本次只修复因批次交错导致的 worker 不足，没有增加中途改写已捕获预算或
线程池缩容机制，也没有实现跨 episode 的编码/摄取流水线。TSan 仍受前述
本机运行时问题限制。修复前快照和本批日志位于忽略的
`build/before-codec-growth-fix/`、`build/codec-growth-*.log`。

## 2026-09-06：混合分辨率编码对照与窄图深度损坏

补齐 §5.17 和 §6 中真实 codec 的混合分辨率并发覆盖。新增
`test/sql/lerobot_copy_mixed_video.test`，以三个 worker 同时调度两种尺寸的
RGB 相机及空间变化的深度图，覆盖三个 episode、片段拼接和同一数据库的
重复 COPY。逐帧检查尺寸、时间戳、路由、像素和唯一性；测试显式设置
`threads = 3`，不依赖 runner 默认线程数。

新增 `test/conformance/test_mixed_video.py`，只依赖 Python 标准库及系统
FFmpeg/ffprobe，不需要 LeRobot/Torch。三个混合相机 COPY 在同一进程内
交替改变尺寸，随后再写四组宽度为 16/32/64/66 的深度渐变图。每次运行共
7 次 COPY、39 次 episode/camera 编码、156 帧。独立 FFmpeg reader 对照
输入检查所有像素、每个 PTS、codec/pixel format、帧数和 GOP；扩展 reader
再与独立 reader 及原始深度值对照。两种 RGB 后端均纳入 native CI 配置：
Release 使用 libaom，ASAN/UBSAN 使用 SVT。

这组对照发现了一个原 review 未指出的正确性错误：64×96 深度图的第 6 帧
右下角 8×8 区域，12 位码值应为 256，实际为 4095。独立运行系统 FFmpeg
6.1.1 / x265 3.5 编码同一组四帧仍可复现，PyAV 15.1.0 所带的 FFmpeg 7 /
x265 4.1 也复现，因此不依赖扩展的量化实现、线程池或 concat。
扩大到渐变/噪声后，提高 `rd` 仍有错误，不能作为可靠修复。

[x265 的参考图边缘填充实现](https://github.com/videolan/x265/blob/419182243fb2e2dfbe91dfc45a51778cf704f849/source/encoder/framefilter.cpp#L387-L438)
在首列与末列重合时只增加一侧 padding 的拷贝宽度，又将起点移至左边界；
这会遗漏参考图右上/右下的填充。该代码推导与单列 CTU、运动预测访问图像
边界外参考像素时出现的实测错误吻合。减小 CTU 仅对部分尺寸有效，也不适合依赖它修复
最窄图像或改变同一进程多个 encoder 的全局 CTU 配置。

本次在扩展中规避该路径：宽度不超过 64 像素的深度视频使用 GOP 1，逐帧
独立编码；64 是 x265 最大 CTU 宽度，超过它的图像必有多列 CTU。更宽深度
图继续使用 GOP 2。`LerobotEncodedVideoInfo` 返回实际 GOP，`info.json`
使用该值，避免 writer 与元数据分别硬编码。窄图可能失去部分帧间压缩收益，
README 已明确说明；量化参数、12 位码值和物理深度契约不变。

| 检查 | 结果 |
| --- | --- |
| 修复前扩展 + 新 SQL 回归 | 检出 1 帧深度像素错误，实际失败于像素对照而非元数据断言。 |
| 修复后新增 SQL 回归，`--single-threaded` | 92 条断言通过；用例自身显式设置三个 worker 的宿主预算。 |
| 独立 FFmpeg 对照，Release / SVT 和 libaom | 两组各 156 帧全部通过。RGB 最大误差分别为 6/5，最大单帧平均误差约 1.863/1.693；阈值保持最大 16、平均 3。深度码值及整数毫米值精确匹配。 |
| 全扩展源码启用 ASAN + UBSAN，SVT 对照 | 156 帧全部通过，无 sanitizer 报告；已核对真实编译命令含两种 sanitizer 和 `DUCKDB_FORCE_ASSERT`。DuckDB host 与第三方 codec 沿用未插桩版本，编码检查关闭 LSan。 |
| 窄图独立参数对照 | 12 种尺寸 × 块状/渐变/噪声三种图案，共 36 组、432 帧，在上述 GOP 策略下码值全部精确匹配。 |
| x265 4.1 四帧对照 | GOP 2 有 128 字节不同，GOP 1 全部精确匹配。 |
| FFmpeg 开启的完整 SQLLogicTest | 16 个用例、1538 条断言通过；按条件跳过 1 个用例。 |
| FFmpeg 关闭的完整 SQLLogicTest | 10 个用例、1085 条断言通过；按条件跳过 7 个用例。 |
| 格式与配置 | C++ clang-format、Python Black/Ruff、SQL 文件格式、CI YAML/矩阵及 `git diff --check` 通过。 |

本机复用固定 DuckDB shared library，在单独的 CLI driver 中关闭旧扩展的
自动加载，再显式加载本次 loadable extension，避免误测静态链接的旧实现。
CI 使用完整的新构建；本次只更新 CI 配置，没有远程运行结果。原有 decoder
专项仍启用 LSan；本批不宣称完成 TSan 或 Windows/macOS/ARM 验证。

日志和可复查媒体位于忽略的 `build/mixed-video-*.log`、
`build/mixed-video-{svt,aom}-fixed/`、`build/mixed-video-svt-asan/`；
修复前快照位于 `build/before-narrow-depth-fix/`，独立 codec 复现和参数矩阵
位于 `build/mixed-depth-isolation/`、`build/depth-*-isolation/`。

## 2026-09-06：写侧文件系统、收尾错误与失败回滚

完成 §2.3 的 DuckDB AVIO writer adapter，并补齐 §6 的写入失败注入。
此前 `EncodeVideo` 和 concat 输出通过 `avio_open` 绕过 DuckDB 文件系统，
成功返回前没有检查 `avio_closep` 的结果；`info.json` 写入也未检查返回字节数。
新的回归验证旧实现完全绕过 MP4 文件打开故障，并在 `info.json` 实际短写后
仍把 COPY 当作成功返回。

`LerobotVideoIO` 现在拥有主 AVIO 和 MP4 faststart 重新打开的读取句柄。
读、写、seek、sync、close 全部经过 DuckDB 的 `FileSystem`，FFmpeg 只接收
固定的内部文件标签。C 回调不抛 C++ 异常，而是保存首次异常；调用方在
header、packet、trailer 以及显式关闭之后重新抛出原始错误。收尾按
flush → 检查 AVIO 错误 → sync → close 执行，短写直接报错；后续清理错误
不会替换首次错误。AVIO 释放当前 buffer，构造失败或 FFmpeg 已释放 format
时，adapter 仍负责剩余句柄。文件系统返回无效大小时使用 `IOException`，
避开 `FileHandle::GetFileSize` 内部无符号转换可能触发的 `InternalException`。

没有沿用 concat 文本列表：FFmpeg 6.1 的
[concat demuxer](https://ffmpeg.org/doxygen/6.1/concatdec_8c_source.html)
在打开子片段时不继承 `io_open`。现在逐个打开 staging 中的单视频片段，
直接 remux，输入和输出均走 adapter。输出保留 faststart，其二次读句柄
通过 [AVFormatContext 的 I/O 回调](https://ffmpeg.org/doxygen/6.1/structAVFormatContext.html)
访问同一个受控文件。片段时间偏移在视频 stream time base 中累加，避免
每个 episode 先截成整数微秒引入漂移。初始修复中使用的 ffconcat 列表与
路径转义代码已经移除。COPY 的本地 root 限制保留。

验证中另发现并修复两个原有正确性问题：

- DuckDB v1.5.5 的 `StringUtil::Trim/RTrim` 会丢弃有符号 UTF-8 尾字节。
  COPY 到 `dataset ' # 机器人` 实际会发布到 `dataset ' # `。COPY 现在只
  移除结尾路径分隔符，读侧 root normalization 只裁剪 ASCII 空白。
  SQL 回归用原生 JSON reader 验证真实发布位置，再检查扩展的 info、scan
  和 episodes；故障注入也始终使用这个特殊字符 root。
- 30 fps 下把 1、2、3、6 帧 depth episodes 拼到同一个 shard，旧实现报
  `non monotonically increasing dts ... 1024 >= 512`。短片段的 DTS 修补
  与较长片段的 x265 默认重排延迟不一致。depth 的封闭 GOP 固定为 1 或 2，
  现在显式设置 `max_b_frames=0`，移除仅针对短片段的 DTS 改写，并在
  `video.extra_options` 中记录 `bf=0`。FFmpeg 的
  [libx265 wrapper](https://ffmpeg.org/doxygen/6.1/libx265_8c_source.html)
  会把这一字段传给 x265 的 `bframes` 参数。已有窄图 GOP 保护保持生效。

`test/cpp/test_video_io.cpp` 注册生产 COPY 函数，通过受控本地文件系统注入
23 组错误：编码输出打开/写入/短写/seek/sync/关闭、faststart 读取与关闭、
remux 输入读取、shard 输出、JSON 短写/关闭、Parquet 写入/关闭，以及 shard
移动和最终发布 rename。每组都验证原始错误、无已发布 root、无残留 staging、
文件句柄归零、连接仍可查询，以及同一数据库和 worker pool 中重试成功。
独立 adapter 用例覆盖延迟到 close 的缓冲写入错误、二次关闭错误、取消、
越界 seek、EOF、无效大小、非预期二次路径和遗弃 format 的资源释放。

| 检查 | 结果 |
| --- | --- |
| 新 C++ I/O 测试，Release | 5 个用例、506 条断言通过，其中 COPY 覆盖 23 组故障及重试。 |
| 生产 COPY 测试，ASAN + UBSAN | 437 条断言通过，无 sanitizer 报告。 |
| adapter 专项，ASAN + UBSAN + LSan | 4 个用例、69 条断言通过，`detect_leaks=1`，无泄漏报告。 |
| 旧 writer/COPY + 新故障回归 | MP4 打开故障完全未触发；metadata 短写已触发但 COPY 仍成功。两项分别失败，日志保留。 |
| 旧扩展 + 30 fps、不等长 episode 对照 | 在 depth shard 拼接时复现 DTS 倒退错误。 |
| 修复后独立 FFmpeg 对照 | libaom 和 SVT 各跑 10 fps/等长及 30 fps/不等长，共 624 帧；额外 libaom 7 fps 及 ASAN/UBSAN SVT 30 fps 各 156 帧，总计 936 帧全部通过。 |
| FFmpeg ON 完整 SQLLogicTest | 16 个用例、1544 条断言通过；跳过 1 个要求非 GPL FFmpeg 的用例。 |
| FFmpeg OFF 完整 SQLLogicTest | 10 个用例、1091 条断言通过；按环境条件跳过 7 个用例。 |
| 原生 CMake 配置 | FFmpeg 依赖正确传给测试目录，8 个测试/生产源文件使用 DuckDB 原生 C++11 配置编译通过。 |
| 静态检查 | C++/SQL 格式、Python Black/Ruff、CI YAML 解析及 `git diff --check` 通过。 |

独立对照检查每帧时间戳、episode 路由及全部像素，depth code 和毫米值要求
精确一致；RGB 沿用既有有损容差。每个对照进程仍连续执行 7 个 COPY，
等长时共 39 次 episode/camera 编码，不等长时共 52 次。CI 的 FFmpeg 矩阵
增加 I/O 故障测试、不等长对照及独立 adapter LSan 步骤。

本机复用固定 v1.5.5 的 DuckDB host/runner，重新编译扩展和 C++ 测试；
ASAN/UBSAN 覆盖本次生产源文件，DuckDB host 和系统 codec 库未重新插桩。
真实编码用例关闭 LSan，adapter 和既有 decoder 专项保留 LSan。
CI 配置已更新，未在远端执行；本批也未运行 LeRobot/Torch Python conformance
或 Windows/macOS/ARM 构建。新的 sync 会增加片段收尾的磁盘同步开销，
本批未做吞吐量 benchmark。测试中的 I/O 故障均为一次性；底层文件系统持续
拒绝 staging 删除时仍只能尽力清理，本批不宣称解决 §5.7 的清理诊断问题。

详细日志位于忽略的 `build/write-io-*.log`；外部媒体和逐帧结果位于
`build/write-io-final-*/`，旧实现快照位于 `build/before-write-io-fix/`。

## 2026-09-06：清理失败隔离、构造回滚与残留目录诊断

修复 §5.7。旧析构把所有 spool 关闭、Parquet state 释放及目录删除放在同一个
`try` 中，首个关闭错误会跳过后续显式清理，异常被整体吞掉。另一个缺口是
staging 创建后构造函数抛异常：完整对象的析构不会执行，目录会遗留。

- `LerobotStagingDirectory` 在打开文件的成员之前构造，并在所有这些成员
  销毁后才清理目录。它先完成自身构造，再创建 staging，覆盖文件系统
  创建目录后报告失败，以及后续 COPY state 构造失败的展开路径。
- 每个 spool 独立尝试关闭。关闭时先移走句柄所有权，异常展开仍会销毁
  FileHandle；一个 spool 报错不会跳过其余 spool 或 staging 的收尾。
  原生 Parquet states 通过正常成员析构释放，避免在回滚时重新 finalize。
- 只有本次拥有的 UUID staging 目录会被清理。最终 rename 成功后解除所有权；
  已存在的 root、碰撞的 staging 目录及此前 COPY 的残留目录均保持不动。
- 清理错误使用 DuckDB WARNING 日志记录操作、资源路径、staging 完整路径
  和原因。持有当前查询 logger 的共享引用；日志分配、存储错误或
  `warnings_as_errors` 抛错都被限制在诊断路径内，不中断后续清理，不改写
  原始 COPY 异常类型或消息。

扩展现有 C++ 文件系统故障注入，共新增 13 个组合：第二个 episode 的两个
spool 同时报错、同时删除 staging 失败、标准及非标准清理异常、日志警告
被升级为异常、构造过程中目录已创建但返回失败、staging 探测失败，以及
目录所有权保护。模拟文件系统还会拒绝在有未关闭句柄时删除 staging，
因此测试同时约束 Parquet 与 spool 的释放顺序。旧目录不能删除时，同一
数据库可以在新 staging 中重试并发布，旧目录和其诊断仍保留。

| 检查 | 结果 |
| --- | --- |
| 新清理回归对比旧生产代码 | 12 个组合失败：关闭异常后未尝试删除、构造展开未删除、删除/探测失败没有诊断。目录所有权用例通过。 |
| 完整 I/O + 清理 C++ suite，Release | 9 个用例、870 条断言通过；新增部分为 4 个用例、364 条断言。 |
| FFmpeg OFF 的清理专项 | 3 个用例、120 条断言通过，包含不删除旧残留目录的重试。 |
| ASAN + UBSAN，真实视频 I/O/组合清理 | 2 个用例、681 条断言通过，无 sanitizer 报告。 |
| ASAN + UBSAN + LSan，adapter/非视觉清理 | 7 个用例、189 条断言通过，`detect_leaks=1`，无泄漏报告。 |
| FFmpeg ON 完整 SQLLogicTest | 16 个用例、1544 条断言通过；跳过 1 个非 GPL FFmpeg 用例。 |
| FFmpeg OFF 完整 SQLLogicTest | 10 个用例、1091 条断言通过；按环境条件跳过 7 个用例。 |
| 原生 CMake / C++11 | FFmpeg OFF 配置包含 cleanup target，8 个测试/生产源文件编译通过；FFmpeg ON 的独立测试同样按 C++11 编译。 |
| 格式与 CI | C++ 格式、CI YAML 和 `git diff --check` 通过；三个 native matrix 项均运行非视觉清理专项，保留 LSan。 |

本次只改变 COPY 资源生命周期与诊断，没有修改 codec、量化或统计公式。
本机仍复用固定 v1.5.5 的 DuckDB host/runner；ASAN/UBSAN 插桩扩展测试中的
生产源码，宿主和系统 codec 库未重新插桩。未在远端执行 CI，也未运行
Windows/macOS/ARM 构建。

诊断遵循 DuckDB 的日志配置，README 提供 `SET enable_logging=true`、
`SET logging_level='WARNING'` 与 `duckdb_logs` 查询示例。未启用日志、日志
不可用或警告被配置为抛错时，不能保证保存诊断；原始错误和其余清理仍受
保护。持续拒绝删除的目录会保留在 staging 路径，扩展不扫描或自动删除
其他 COPY 的旧目录。

修复前快照位于忽略的 `build/before-cleanup-fix/`，验证输出位于
`build/cleanup-*.log`，包括旧代码的 12 个失败组合。

## 2026-09-06：关系目标的有界时间戳索引

处理 §3.3 中 `lerobot_video_targets` 的多批次重复查找。新增
`benchmark/video_target_timestamps.py`，直接执行真实关系入口，以原生 join
核对计数和时间戳和。它覆盖稀疏、密集、重复请求，多 shard、多个输入 chunk、
64 MB 内存限制和四线程；可选媒体路径另测解码，并在计时外联合散列请求身份、
目标时间戳和像素。原有 `timestamp_lookup.py` 的临时表实验继续保留。

实现位于 `storage/lerobot_timestamp_lookup.cpp`。每个目标算子的 worker 共享
查询内索引，保留现有关系输入、route/delta 校验及解码调度。累计至少 4,096 个
逻辑 target、且同一 shard 被至少三个批次访问后才尝试建索引；此前不增加
open/stat/HEAD。原生 Parquet footer count 决定分配大小，随后只读取三个字段。
已按键排序的数据直接使用；无序数据原地排序，并定期检查取消。

索引行缓冲区经 DuckDB buffer allocator 分配，包含构建中和仍被借用的缓冲区，
总额不超过 `min(32 MiB, memory_limit / 8)`，限制在算子初始化时确定。
本平台每行实际为 32 字节，包含两个键、时间戳、NULL 标记及对齐，不能按
“每帧 8 字节”估算。最多跟踪 64 个 shard 状态；路径/状态对象和原生查询工作
内存不包含在该行缓冲区上限中。按使用顺序驱逐索引，驱逐后抑制重复全量构建，
过大、内存不足或正在被其他 worker 加载的 shard 继续走原生过滤查询。
持锁范围内不执行 I/O 或嵌套查询，也不等待另一个 loader。

每次复用检查数据 shard 自身的 size、mtime、version tag，构建前后也检查；
加载途中版本变化的索引不会发布。索引随查询释放，预编译查询下一次执行会
创建新状态。它不进入数据库级 ObjectCache，也不增加 `lerobot_cache_info`
的公开列。检测仍受文件系统指纹能力约束，三个属性全部不变的修改无法识别。

查找合并所有选中文件的匹配计数，保留跨 shard 错放行的重复检测；仅对请求到
的帧检查缺失、重复、NULL、非有限或负时间戳。收尾测试实际复现了一个容易
遗漏的边界：未请求行包含不可转换的字符串时，全量索引读取会比原查询更早
报错。现在构建中的转换、普通读取、输入和内存错误回退到原生过滤查询，由
该查询判断请求的行是否可用；不吞取消、内部或致命错误。该回归修复前的失败
记录保存在 `build/timestamp-unrequested-before.log`。

### 性能证据

最终扩展的 SHA-256、DuckDB 提交、参数及 28 组前后对照结果保存在
[`benchmark/results/timestamp-lookup-20260906.json`](../benchmark/results/timestamp-lookup-20260906.json)。
前后所有计数/时间戳结果一致，六组动态 H.264 视频对照的请求与像素散列也一致。
媒体用 FFmpeg `testsrc2=size=32x24:rate=32` 生成 4,096 帧，libx264/ultrafast、
yuv420p、单编码线程；每组查询 8,192 个 target。

百万行、单线程、32,768 个 target、每次至多 2,048 行输入的本地元数据投影：

| shard 数 | 请求 | 修复前中位数 | 修复后中位数 | 比值 |
| --- | --- | --- | --- | --- |
| 1 | 密集 | 636.13 ms | 180.30 ms | 3.53× |
| 1 | 稀疏 | 1745.64 ms | 307.99 ms | 5.67× |
| 1 | 重复 | 261.18 ms | 115.83 ms | 2.25× |
| 4 | 密集 | 503.92 ms | 103.47 ms | 4.87× |
| 4 | 稀疏 | 1770.38 ms | 338.91 ms | 5.22× |
| 4 | 重复 | 257.52 ms | 119.02 ms | 2.16× |

以上每组另有一次首次执行及五次计时重复。四线程的四 shard 稀疏请求从
1,047.34 ms 降到 217.48 ms。64 MB 限制下索引不适合百万行单 shard，回退
查询仍可完成；两组中位数比原来慢约 2%–5%。小请求没有索引收益，不宣称它们
均有提升。动态视频的六组端到端比值约 0.95–1.32×，说明时间戳提速不能直接
当作解码整体提速；低内存、四线程、视频组各使用两次计时重复。

这些是 Linux x86_64 的本地合成数据，OS cache 未清空，native join 先运行；
没有远程 I/O 或跨平台性能结论。JSON profiler 的 buffer 高水位是连接累计值，
不是索引独占内存或 RSS。DuckDB 1.5.5 的 table-in/out 算子不会调用
`dynamic_to_string`，基准将不可用的动态指标保留为空，不伪报为零。
C++ 受控文件系统直接验证查询次数、数据读取和内存：建成索引后的 20 个批次
不再读 Parquet 数据列，也不启动新的嵌套查询。

### 验证

| 检查 | 结果 |
| --- | --- |
| 真实 C++ 嵌套读取/时间戳模块 | 15 个用例、856 条断言通过；其中本轮新增 11 个用例。 |
| ASAN + UBSAN + LSan | 同一 15 个用例、856 条断言通过，`detect_leaks=1`。 |
| Producer 生命周期 | 6 个用例、69 条断言通过；对应 CMake 目标链接新源文件。 |
| FFmpeg ON 完整 SQL | 17 个用例、1581 条断言通过；跳过 1 个非 GPL 环境用例。 |
| FFmpeg OFF 完整 SQL | 11 个用例、1128 条断言通过；按环境跳过 7 个用例。 |
| 新增 SQL 回归 | 37 条断言，覆盖跨 chunk 的 padding/稳定 ID、独立文件更新、空输入、坏值及重复/缺失帧。 |
| 静态检查 | C++/SQL 格式、Black/Ruff、CI YAML 解析及 `git diff --check` 通过。 |

受控测试还覆盖无序数据、空 shard、NULL join key、大于 32 位的键、内存驱逐、
构建中仍被借用的内存、同 shard 与不同 shard 的并发、构建期间文件变化、
取消后重试及失败清理。SQL runner 使用绝对临时目录，以匹配现有路径断言。
构建仍复用固定 v1.5.5 的本机 DuckDB host；sanitizer 对扩展/测试源码插桩，
host 未重新插桩。CI 已将新源文件和用例接入现有矩阵，未远程运行。

## 2026-09-06：COPY FEATURES 绑定期间的取消传播

补齐 §3.2 中 COPY 内存 `FEATURES` JSON 解析的取消缺口。
`ParseUserFeatures` 改用已有 `LerobotNestedQuery`，在内部查询执行、结果检查
和逐批提取时接收外层中断。JSON SQL、feature 校验规则和普通错误的
`Failed to bind LeRobot FEATURES JSON` 提示保持原样；中断以 `INTERRUPT`
返回，不被包装为 FEATURES 的 binder 错误。

新增 `test/cpp/test_copy_bind.cpp`，与真实 COPY 实现链接。测试通过 DuckDB
的连接/查询回调暂停内部 JSON 查询，不替换 JSON 函数，也不添加生产测试
开关。回调只观察内部连接的中断标记，转发仍由生产 helper 完成。

- 直接 COPY 和 `Connection::Prepare`，分别覆盖 1/4 线程，以及内部查询
  开始、执行任务、查询收尾三个阶段。调用方必须等待内部查询完成收尾。
- JSON 已产生解析错误、但外层同时取消时，最终仍保留中断类型。
- 取消后内部连接被销毁、输出目录为空，其他连接仍可查询；原连接可重新
  prepare/execute COPY，并由原生 Parquet reader 核对写出的数值。
- 畸形 JSON、不可转换的 shape、NULL 和非正维度保持 binder 错误及原有提示；
  同一连接连续失败后仍可查询并成功重试。

| 检查 | 结果 |
| --- | --- |
| 修复前的同一回归 | 首个组合（单线程、直接 COPY、内部查询开始）等待 5 秒仍未收到中断，断言失败；测试释放暂停点后正常收尾。 |
| Release / FFmpeg ON，绑定与既有清理专项 | 6 个用例、604 条断言通过。 |
| ASAN + UBSAN + LSan，同一专项 | 6 个用例、604 条断言通过，`detect_leaks=1`。 |
| Release / FFmpeg OFF，新绑定专项 | `test_lerobot_copy_bind` 目标执行成功，3 个用例、484 条断言通过。 |
| FFmpeg ON 完整 SQL | 17 个用例、1581 条断言通过；跳过 1 个非 GPL 环境用例。 |
| FFmpeg OFF 完整 SQL | 11 个用例、1128 条断言通过；按环境跳过 7 个用例。 |
| 格式与配置 | C++ 格式、CI YAML 解析及 `git diff --check` 通过。 |

两个 loadable extension 和独立 C++11 测试均重新编译。CI 的三个 native
配置在清理专项中一并执行 `[copy_bind]`，保留泄漏检测；未远程运行 CI。
本机仍复用固定 DuckDB v1.5.5 host，sanitizer 插桩扩展和测试源码，host
未重新插桩。本次不改变嵌套连接的事务/其他会话设置继承；取消延迟仍取决于
DuckDB 执行代码检查中断，不能强制终止任意解析步骤。

旧实现快照位于忽略的 `build/before-copy-bind-fix/`，构建和验证日志位于
`build/copy-bind-*.log`。本次没有修改 codec、量化或统计计算。

## 2026-09-06：COPY 数值统计读取优化

处理 §4.1 的逐行 `GetValue`、递归数组 `Value` 构造与逐元素
`DefaultCastAs(DOUBLE)` 开销。新增 `LerobotNumericStatsVector`，每次扫描
chunk 后建立数值缓冲区视图，按 selection 和 validity 读取标量/定长数组。
输入的数值存储类型已由 COPY bind 限定；转换仍先产生 DOUBLE，再交给原有
统计代码。保留整数极值的首遍读取继续构造原整数类型的 `Value`，完整保留
int64/uint64 位数，其他读取不再逐元素装箱。

NumPy pairwise 的求和顺序、float32 中间舍入、直方图边界及分位数插值、
跨 episode 合并和每 64 维分批的算法保持原样。视图仅持有各层的形状、
selection、validity 和数据指针，不建立整块 DOUBLE 缓存。数值集合仍由
DuckDB buffer manager 管理并按原规则溢写，现有统计缓冲区上限不变。

### 可复现基准

新增 `benchmark/lerobot_copy_numeric.py`。每次重复启动新进程，先建立同一
输入表，再用 DuckDB profiler 测量 COPY；计时外逐行核对所有输出帧，并对
全部 episode/dataset 统计生成摘要。Linux 用 GNU time 的进程峰值 RSS，
覆盖校验和退出阶段；同时记录连接累计 buffer 高水位和临时磁盘占用。
基准不包含视频编码，未清空 OS cache；本机不允许 perf 调用栈采样，以下
报告的是 COPY 整体耗时，不声称获得了统计函数的采样占比。

本地 Linux x86_64、固定 DuckDB v1.5.5、单线程、256 MB 配置、两集各
10,000 帧、每组重复三次的中位数：

| dtype / 维度 | 优化前 | 优化后 | 比值 |
| --- | --- | --- | --- |
| float32 / 14 | 0.719 s | 0.102 s | 7.04× |
| float32 / 64 | 2.619 s | 0.304 s | 8.61× |
| float32 / 65 | 4.973 s | 0.315 s | 15.77× |
| float32 / 256 | 37.742 s | 1.143 s | 33.01× |
| float64 / 256 | 12.636 s | 1.221 s | 10.35× |
| int64 / 256 | 35.747 s | 1.478 s | 24.18× |

完整 12 组及一个 96 MB 溢写组保存在
[`benchmark/results/numeric-copy-20260906.json`](../benchmark/results/numeric-copy-20260906.json)，
包含 CLI/扩展 SHA-256、DuckDB 提交、CPU、参数、每次测量和统计摘要。
13 组的新旧帧校验和统计摘要全部一致。256 维 float32 的进程峰值 RSS
约为 178.2 → 177.8 MiB；float64 为 258.4 → 261.2 MiB，内存没有量级增长。
这不是 RSS 必须低于 `memory_limit` 的承诺，profiler 的 buffer 数也是
连接累计高水位，不能当作读取器独占内存。

96 MB 配置使用同一 256 维 float32 输入、重复两次；新旧实现均实际溢写，
COPY 为 36.400 → 1.125 s。64 MB 下两者都在同一 11.7 MiB 分配处失败，
保留该失败证据，未借调整内存配置宣称解决原有的分配下限。

### 兼容性与验证

原有双向 conformance 主要核对格式和数据帧，本轮补充独立的
`test/conformance/test_numeric_stats.py`，直接调用 LeRobot 0.6.1 的
`compute_episode_stats`（NumPy 2.2.6）。27 组单 episode fixture 覆盖
1/14/64/65/256 维、1/7/8/129/2050 帧与全部 11 种数值/bool dtype，
精确比较 episode Parquet 和 dataset JSON 的全部十个统计字段。
二进制有限小数输入使除法前的求和可精确表示；这里的无 tolerance 对照
只说明这些 fixture 一致。原有难例 SQL 继续钉住 pairwise 舍入和超过
DOUBLE 精确范围的整数极值；多 episode 的全部统计另由新旧摘要核对。

| 检查 | 结果 |
| --- | --- |
| 新数值 C++ 专项，FFmpeg OFF | 4 个用例、982 条断言通过；包含真实 4097 行、129 维 COPY。 |
| Release / FFmpeg ON，数值 + 绑定 + 清理 | 10 个用例、1586 条断言通过。 |
| ASAN + UBSAN + LSan，同一专项 | 10 个用例、1586 条断言通过，`detect_leaks=1`。 |
| 固定 LeRobot 数值统计 oracle | 优化前后均通过 27 组、1440 项精确数组比较。 |
| 原有 LeRobot 双向格式/数据帧对照 | 优化前后均通过。 |
| FFmpeg ON 完整 SQL | 18 个用例、1596 条断言通过；跳过 1 个非 GPL 环境用例。 |
| FFmpeg OFF 完整 SQL | 12 个用例、1143 条断言通过；按环境跳过 7 个用例。 |
| 新数值 SQL 回归 | 15 条断言，含跨 chunk/维度批次、嵌套数组、bool、NULL 和 NaN。 |

C++ 专项还覆盖所有层级的字典选择、常量向量、视图重建、各层 NULL、
signed zero、整数类型及完整 64 位极值。两个扩展和 C++11 测试均已重编译；
C++/SQL 格式、Black/Ruff、CI YAML 与 `git diff --check` 通过。
三个 native CI 配置运行数值专项，FFmpeg release job 增加固定统计 oracle。
未远程运行 CI，未验证其他平台；本机 sanitizer 插桩扩展/测试源码，
仍复用未重新插桩的固定 DuckDB host。

旧实现与二进制位于忽略的 `build/before-stats-fix/`，日志和原始基准报告
位于 `build/stats-*.log/json`。本次只优化数值读取，没有改动 codec 或统计公式。

## 2026-09-06：COPY RGB image 的 PNG 编码器复用

处理 §4.2。此前每路 RGB `image` 的每一帧都创建、打开并销毁 PNG
编码器、AVFrame 和 AVPacket。现在 `LerobotCopyGlobalData` 按特征持有
`LerobotImageWriter`，第一帧到达时创建，跨 chunk/episode 复用，随 COPY
状态销毁。不同特征和不同 COPY 各自持有资源；空数据集不打开编码器。
图像尺寸和原始 dtype 仍由原有 schema/帧检查限定。

RGB 编码固定单线程，保证每次提交立即得到一个完整 PNG；重复使用帧前
调用 `av_frame_make_writable`，复制输出字节后立即 `av_packet_unref`。
构造过程使用 RAII，部分初始化失败和异常退出也释放已取得的资源。
深度 image 仍走原来的 TIFF 实现。未更改压缩参数、统计公式或 SQL 参数，
也未引入图像编码任务队列。

协议核对依据：FFmpeg 6.1.1 的
[`encode_png`](https://github.com/FFmpeg/FFmpeg/blob/n6.1.1/libavcodec/pngenc.c#L562)
每次输出完整 PNG，像素压缩后重置 zlib 状态；不是 APNG 的帧间依赖路径。
帧引用与 packet 所有权按官方
[send/receive API](https://ffmpeg.org/doxygen/6.1/group__lavc__encdec.html)
处理。实际输出还逐张用独立 Pillow 解码器核验。

### 基准与资源

新增 `benchmark/lerobot_copy_image.py`，与新的 Pillow conformance 共用
输入生成和逐像素校验。输入先生成 Parquet 并由 DuckDB materialize，
然后单独用 profiler 测 COPY。每次运行新进程，Linux GNU time 的峰值
RSS 包含输入加载与退出，不包含 Python 进程。记录连接累计 buffer/spill
高水位；没有清空 OS cache，也没有把 `memory_limit` 当作 RSS 上限。

本地 Linux x86_64、DuckDB v1.5.5、FFmpeg 6.1.1、`threads=1`、512 MB
配置、两个 episode、每组各三次的中位数（尺寸为宽×高）：

| 场景 | 每集帧数 | 优化前 | 优化后 | 比值 |
| --- | ---: | ---: | ---: | ---: |
| 16×16 | 2048 | 0.392 s | 0.340 s | 1.152× |
| 160×120 | 256 | 1.801 s | 1.765 s | 1.020× |
| 640×480 | 64 | 1.733 s | 1.635 s | 1.060× |
| 640×480，随机噪声 | 64 | 5.405 s | 5.539 s | 0.976× |
| 64×48 + 160×120，两路 | 256 | 2.111 s | 2.057 s | 1.026× |

噪声组首轮疑似慢约 2.5%，追加四对新旧版本交替顺序测量，得到
5.659 → 5.324 s（1.063×）。保留两轮全部样本，不删除不利结果；
大图仍受压缩开销和运行波动影响，不能把小图约 13% 的耗时下降推广到
所有图像数据集。基准只测 COPY 总耗时，未做编码函数调用栈采样。

五组新旧 PNG 字节摘要及全部 episode/dataset 统计摘要均相同，追加
交替组也相同；每张图片都与独立输入逐像素一致。首轮进程峰值 RSS 的
中位数增加约 0.4–1.7 MiB。资源驻留按实际使用的图像特征数和各自尺寸
增长，持续到 COPY 状态释放；不是按数据集帧数积累，也不是 DuckDB
buffer manager 管理的硬限额内存。

完整样本、机器信息、参数、扩展/源码 SHA-256 保存在
[`benchmark/results/image-copy-20260906.json`](../benchmark/results/image-copy-20260906.json)。
脚本支持 `--baseline-extension` 自动交替顺序并检查新旧摘要；复现命令
见 `benchmark/README.md` 的 Image COPY 段落。旧源码/二进制保存在忽略的
`build/before-png-fix/`，本轮日志及原始报告位于 `build/png-*.log/json`。

### 验证与 CI

| 检查 | 结果 |
| --- | --- |
| PNG/COPY C++ 专项，Release | 4 个用例、4564 条断言通过。 |
| PNG + 数值统计 + COPY bind/cleanup + AVIO，ASAN/UBSAN/LSan | 18 个用例、6219 条断言通过，`detect_leaks=1`。 |
| FFmpeg OFF 的图像拒绝路径及原有数值/bind/cleanup | 11 个用例、1587 条断言通过。 |
| Pillow 独立逐像素 conformance | 新旧版本各 16,744 张 PNG 全部通过，输出摘要一致。 |
| 原有固定 LeRobot 视觉 conformance | PNG/TIFF、RGB/depth 视频、自定义量化、单位和 padding windows 全部通过。 |
| FFmpeg ON 完整 SQL | 18 个用例、1596 条断言通过；跳过 1 个非 GPL 环境用例。 |
| FFmpeg OFF 完整 SQL | 12 个用例、1143 条断言通过；按环境跳过 7 个用例。 |

C++ 专项保留前一帧输出并交错写入不同尺寸的编码器，核验返回字节独立。
坏帧后可继续编码；还覆盖编码器已使用后的异常栈展开、raw spool/Parquet/
publish 错误，以及第一集已编码完成时从另一线程真正调用 Connection
Interrupt。文件系统仅用作可控暂停点，不代替生产路径抛中断异常。
`threads=1/4` 下各重复取消三次，验证目录/句柄清理、其他连接可查询，
最后在同一数据库中重试成功。

Pillow conformance 覆盖单像素、非对齐行、随机噪声、双路不同尺寸、
跨多个 chunk 与 episode；同一连接连续三次 COPY 改变尺寸，分别设置
`threads=1/4`。每张 PNG 使用新的解码器，要求独立完整的单帧 RGB 图像。
新旧压缩字节一致只是本机相同 FFmpeg 构建下的观测，不新增跨版本位级
兼容承诺。原有 LeRobot 视觉对照继续覆盖 TIFF。

Native CI 增加独立 PNG LSan 步骤，FFmpeg release job 接入 Pillow
conformance；没有远程运行 CI。本地 sanitizer 插桩扩展/测试源码，复用
固定 DuckDB host 与系统 FFmpeg，未重新给这两个依赖插桩。C++11 两种
构建、格式检查、Black/Ruff、CI YAML 和 `git diff --check` 均通过。

## 2026-09-06：优先清理 bug

按用户要求暂停新的性能优化，先复现并关闭剩余正确性问题。本轮修复
三个读写问题和两个元数据/工具问题；不把 review 中每项设计意见都当成
已经成立的 bug，也不因已有测试通过就宣称全部潜在缺陷已清零。

| 项目 | 修复前的证据 | 最终处理 |
| --- | --- | --- |
| §3.2 会话配置丢失 | 本地 S3 服务上，同一连接的原生 JSON/Parquet 读取成功，`lerobot_scan` 的嵌套 JSON GET 却使用全局配置并收到 403。 | 嵌套读和视频 producer 新建连接后、执行查询前，继承调用方显式设置的扩展选项。全局设置/secret 继续共用；不复制事务或整个 ClientConfig。 |
| 新发现：跨 endpoint 误用路由缓存 | 两个 endpoint 返回完全相同的 `info.json` 字节、size、mtime、ETag，但 episode 路由不同。切换后原生读取新文件得到 action 合计 22，扩展仍得到旧 shard 的 1；视频也仍指向 `video-0.mp4`。 | 数据和视频缓存 key 同时包含 S3 的 endpoint、region、URL style、TLS 路由配置，按 DuckDB secret manager 和调用方设置解析。不把 access key、secret key、session token 放进 key。 |
| §5.5 timestamp 舍入 | 合法 `FPS=16777217`、第二帧：旧 COPY 的 float32 bits 是 864026624，官方 Python 除法后 Arrow 转 float32 是 864026623，相差 1 ULP。 | 先用 double 做整数商，再转为 float32；不再预先把分子/分母舍入成 float。 |
| §5.4 tasks 伪造 provenance | COPY 写出的 pandas 元数据声明了未调用的 PyArrow/Pandas 版本；新外部断言在旧扩展上失败。 | 保留索引、列和 attributes 描述，删除 creator/pandas_version；真实 Pandas 和 LeRobot 仍正确恢复 task 索引。 |
| §5.16 旧视频基准漏计尾部 I/O | 子进程写入并 fsync 8 MiB 后立即退出，10 次旧轮询中 9 次没有记录完整 wchar，部分 write_bytes 也只记到 4/6 MiB。 | 改为 `wait4` 回收指定子进程并记录完整生命周期的资源计数；结果 schema 升为 2，明确切换为 filesystem block 指标。 |

原 review 将 S3/HF 设置全部视为全局，范围判断不准确：固定 DuckDB 的
`DBConfig::AddExtensionOption` 默认 `SetScope::SESSION`，HTTPFS 的 S3
选项按此方式注册。继承实现直接复制已验证的 Value，不拼接 SET SQL，
也不从 worker 重放可能修改全局状态的设置回调。NULL、引号、反斜杠和
换行按值保留。DuckDB RESET 扩展选项会将注册默认值写成会话覆盖值，
测试保留这个原生语义。其它连接不继承调用者的覆盖值。

`test_session_settings.py` 使用真实 HTTPFS 和 loopback 服务。除了原生
读取对照、两类会话覆盖、视频 producer 和 target 查询，还验证：预热
缓存后撤掉会话凭证会失败；SESSION/GLOBAL 修改 endpoint、替换 secret
以及切回旧 endpoint 都选择正确的数据/视频路由。两个服务的 commit
marker 完全相同，新端仍保留旧文件，使错误表现为读错数据而非简单 404。
服务只使用公开的 dummy key，按 key ID 控制访问，不记录授权头，也不
声称验证 AWS 签名实现。缓存身份的 secret 解析不读取 dataset 文件；
`lerobot_cache_info` 的文档据此明确其被动查看范围。

### 视频 shard 边界的结论

§5.11 的物理切分差异确实存在，但本次对照没有发现帧或路由错误。
固定 LeRobot 0.6.1 的真实 writer 只替换 encoder 输出，继续执行其原有
metadata/Parquet 写入、文件 stat、轮转和 concat，使用与扩展相同的编码
片段，避免把编码器差异混入判断。

本机三个片段为 884、884、882 字节；官方合并前两个后为 958 字节。
在 2245 字节阈值下，官方三个 episode 均在 shard 0，扩展将第三个放入
shard 1。两边都能正确读取每帧。另测首次合并阈值上下各 1 字节和等于
阈值、单集大于阈值，以及 chunk 边界，共 9 个数据集、42 帧通过两个
reader。保留一次拼接的现有实现，README 明确这是目标大小和切分策略，
不承诺与官方 writer 的物理 file_index 相同。

### 基准计量和验证记录

`wait4` 返回该子进程的完整资源使用量，见
[Python 文档](https://docs.python.org/3/library/os.html#os.wait4)。新字段
`fs_input_blocks` / `fs_output_blocks` 对应内核文件系统块计数，不是旧
`rchar` / `wchar` 的逻辑字节；Linux 的单位为 512 字节，page cache 会
影响结果，见 [getrusage](https://man7.org/linux/man-pages/man2/getrusage.2.html)。
缺失计数或零分母的 ratio 保留 null。Linux/macOS 的每子进程峰值 RSS
统一为字节，其他平台不猜单位。无 wait4 时保留耗时，资源字段为 null。
`--extension` 可显式指定每个测量/验证进程加载的本地扩展。

六个 Python 回归通过：末次 8 MiB 写入完整计量、连续大小进程的 RSS
互不污染、非零退出诊断、等待中断后的回收、缺失 wait4 和缺失/零计数。
实际 2/4 相机、2/4 episode 的完整基准 smoke 也通过，包含全部 decode
及 route 校验；没有把这个小样本作为新的性能结论。

| 检查 | 结果 |
| --- | --- |
| FFmpeg ON / OFF 完整 SQL | 分别 1596 / 1143 条断言，18 / 12 个用例通过；按环境跳过 1 / 7 个用例。 |
| 嵌套读/时间戳缓存 C++，Release 和 ASAN/UBSAN/LSan | 18 个用例、917 条断言通过，涵盖一/四线程的会话配置和取消/生命周期。 |
| producer 生命周期，ASAN/UBSAN/LSan | 6 个用例、69 条断言通过。 |
| timestamp/数值/COPY bind/cleanup，ASAN/UBSAN/LSan 和 FFmpeg OFF | 各 11 个用例、1595 条断言通过。 |
| 真实 HTTPFS loopback，FFmpeg ON / OFF / ASAN+UBSAN+LSan | 分别 14 / 10 / 14 项检查通过；sanitizer 进程均开启 `detect_leaks=1`，包括真实视频解码和 secret 替换。 |
| 固定 LeRobot/Pandas/Arrow 对照 | task 索引正确，FPS 3、30、16777217、16777219 的 timestamp float32 位模式一致。 |
| 相同视频片段的官方 shard 对照 | 9 个数据集、42 帧，两个 reader 全部通过。 |
| Python 基准回归与完整 smoke | 6 个回归和全部 smoke 验证通过，测量/验证显式加载本轮扩展。 |

复现和回归日志保存在 `build/bug-*.log`，本轮开始前的源码/扩展快照为
`build/before-bug-closeout/`；最终源码和三种扩展的 SHA-256 记录在
`build/bug-final-sha256.json`。C++ 格式、Black/Ruff、CI YAML 和
`git diff --check` 均通过。Native CI 接入 loopback S3、shard 外部对照
和 Python 基准测试，原有 bidirectional job 覆盖新增 task/timestamp 断言。
本地复用固定 DuckDB host、HTTPFS 和 FFmpeg；sanitizer 只插桩扩展/测试
源码，不把这些结果当作第三方依赖全部经过插桩或 hosted CI 已通过。

## 2026-09-06：S3 访问范围和加载中途切换配置

继续以可复现 bug 为优先。本轮新增确认并修复三项缓存正确性问题；前两项
补齐上一轮只按 manifest 的 endpoint 配置区分缓存的不足。

| 问题 | 修复前证据 | 修复 |
| --- | --- | --- |
| 重置凭证后仍复用有权限时的路由 | 真实 HTTPFS loopback：`info.json`、数据和视频公开，episode Parquet 受限。重置会话 access key 后，原生 episode 读取报 403，预热过的 `lerobot_scan` 仍返回 action 合计 1。 | 缓存身份纳入 S3 访问配置及其 LOCAL/GLOBAL 来源，凭证变化触发缓存失效。公开 manifest 的 stat 不再代替客户端访问身份检查。 |
| 漏掉 episode 目录和文件范围的 secret | 两个 endpoint 的 `info.json` 完全相同。只替换 `meta/episodes/` 范围内的 secret，扩展继续返回旧 shard 的 1 和 `video-0.mp4`，而当前元数据应路由到合计 22 和 `video-1.mp4`。 | 收集所有与 dataset 路径相交的 S3/R2/GCS/AWS secret，覆盖任意深度的 episode 子路径，按内容生成确定的摘要；不再只解析 `meta/info.json` 的 winning secret。 |
| 加载中途切换 endpoint 会污染旧缓存 | 受控文件系统在首次元数据 I/O 时让另一连接执行 `SET GLOBAL s3_endpoint`。切回原 endpoint 后，数据、视频分别仍返回 `data-1.parquet`、`video-1.mp4`，应为 shard 0。 | 数据和视频缓存均在 I/O 前后复核访问身份，失败后用新身份重试。预热缓存也复核；显式 refresh 在每次数据加载尝试中失效当前两级缓存。连续变化在两次尝试后报 `IOException`。 |

缓存摘要使用固定 DuckDB 的 SHA-256 实现。参与摘要的字段使用长度前缀和
类型编码，secret 条目排序后再合并；缓存 key 只保留摘要，不保存凭证明文。
与 dataset 完全无关的 secret 不参与其身份；真实 HTTPFS 测试确认新增相邻
dataset 的 secret 后，`lerobot_cache_info` 仍报告原缓存命中，扫描结果正确。
旧身份的条目仍由现有 ObjectCache 按内存预算驱逐。

测试中的单文件 endpoint 切换另有一个需要保留的原生行为：目录列表来自 A，
单个 Parquet 来自 B，二者 ETag 不同。原生 HTTPFS 拒绝读取；修复后的扩展
也拒绝读取，预热缓存不会掩盖这一错误。没有关闭 ETag 检查来使测试通过。

`test_session_settings.py` 新增：公开 manifest/listing/payload、私有 episode
Parquet；重置 SESSION 凭证；删除目录和单文件 secret；目录级 endpoint
替换；单文件 ETag 冲突；无关 secret 不影响缓存。服务只识别公开的 fixture
key ID，不验证 AWS 签名，也不记录 Authorization 头。
`test_nested_query.cpp` 用固定 I/O 时机覆盖数据/视频 × 冷/热缓存 × 普通/
refresh 共八组，并逐组检查持续切换时有限重试、设置稳定后的恢复。

最终源码的验证结果：

| 验证 | 结果 |
| --- | --- |
| FFmpeg ON / OFF / ASAN+UBSAN 扩展构建 | 三套均成功 |
| 完整 SQL，FFmpeg ON | 18 个用例、1,596 条断言通过；仅未启用的非 GPL codec 用例跳过 |
| 完整 SQL，FFmpeg OFF | 12 个用例、1,143 条断言通过；6 个视觉和 1 个非 GPL codec 用例按条件跳过 |
| 真实 HTTPFS loopback，ON / OFF / ASAN+UBSAN+LSan | 分别 23 / 16 / 23 项检查通过；sanitizer 使用 `detect_leaks=1` |
| 嵌套读取、时间戳和缓存 C++，Release / ASAN+UBSAN+LSan | 各 19 个用例、993 条断言通过 |
| 视频 producer C++，ASAN+UBSAN+LSan | 6 个用例、69 条断言通过 |

修复前快照和扩展位于 `build/before-cache-lifecycle-20260906/`。
`build/cache-access-before.log`、`build/cache-scope-before.log` 和
`build/cache-race-before.log` 保存三个失败证据。最终运行日志为
`build/cache-final-{sql,httpfs}-*.log`、`build/cache-race-final.log`、
`build/cache-race-asan.log`、`build/cache-producer-asan.log`；
`build/cache-lifecycle-final-sha256.json` 记录源码、二进制和日志哈希。
Black、Ruff、clang-format、YAML 解析和 `git diff --check` 通过。新增检查沿用
现有 Native CI 入口；本机没有执行托管 CI。sanitizer 插桩扩展及 C++ 测试，
复用的 DuckDB host、HTTPFS 和系统媒体库未在本轮重新插桩。

一致性检查仍以 `info.json` 提交标记及 I/O 前后观察到的客户端访问身份为依据；
这不提供跨连接任意配置变更的事务快照。服务器单独修改 ACL 而客户端身份
不变的情况，不在本轮凭证/secret 变更测试的覆盖范围内。

## 2026-09-06：服务端撤权与远程 episode 元数据重新校验

本轮继续验证上一节明确留下的服务端 ACL 边界。客户端凭证、endpoint、
`info.json` 和所有对象字节保持不变，只由 loopback 服务在两次查询之间撤销
episode 元数据读取权限。修复前，预热后的原生 Parquet 读取报 403，
`lerobot_scan` 仍返回旧路由的 action 合计 1；失败证据位于
`build/server-acl-before.log`。因此仅重新检查客户端身份不足以覆盖服务端撤权。

同时确认：远程 episode Parquet 被替换、但 `info.json` 指纹不变时，原生读取
已经看到新的 `data/file_index = 1`，扩展仍返回 action 合计 1 和
`video-0.mp4`，应为 22 和 `video-1.mp4`。证据位于
`build/metadata-change-before.log`。这一修复扩展了原来的 manifest 提交标记
契约，不要求手工修改远程元数据时必须同时更新 `info.json`。

数据缓存现在保存远程 episode 文件的排序路径列表及 size/mtime/version-tag
指纹，视频缓存保存同一快照。复用前和加载完成后重新校验：通过 DuckDB
文件系统列举目录、保留列举携带的版本信息打开文件，并对每个非空文件请求
读取 1 字节。只检查 HEAD 无法发现 GET 被单独拒绝，所以需要实际读取。
零行 Parquet 文件也参与校验；路径或指纹变化会让数据和视频路由一起重建。
快照的内存计入缓存估算，文件句柄由 RAII 释放，读取使用调用方取消状态。
连续变化沿用有界重试，不发布未通过检查的路由。

新增回归覆盖：对象 HEAD/GET 同时撤权、仅 GET 撤权、目录列举撤权；
默认文件缓存开启和显式关闭；撤权后的被动 `lerobot_cache_info`；元数据替换、
可见文件集合变化，以及空/非空 Parquet 间移动 episode。每类都有原生读取
对照。C++ 测试在实际探测读取处阻塞并中断，验证异常类型、句柄归零和同一
连接恢复；原有数据/视频 × 冷/热 × refresh/普通的配置切换回归继续通过。

| 验证 | 结果 |
| --- | --- |
| FFmpeg ON / OFF / ASAN+UBSAN 扩展构建 | 三套均成功 |
| 完整 SQL，FFmpeg ON | 18 个用例、1,596 条断言通过；1 个非 GPL codec 用例按条件跳过 |
| 完整 SQL，FFmpeg OFF | 12 个用例、1,143 条断言通过；6 个视觉和 1 个非 GPL codec 用例按条件跳过 |
| 真实 HTTPFS loopback，ON / OFF / ASAN+UBSAN+LSan | 分别 41 / 28 / 41 项检查通过 |
| 嵌套读取、时间戳和缓存 C++，Release / ASAN+UBSAN+LSan | 各 19 个用例、1,041 条断言通过 |
| 视频 producer C++，ASAN+UBSAN+LSan | 6 个用例、69 条断言通过 |

修复前源码及扩展快照位于 `build/before-cache-revalidation-20260906/`。
最终日志为 `build/metadata-validation-sql-{on,off}.log`、
`build/metadata-validation-httpfs{,-off,-asan}-final.log`、
`build/metadata-validation-cpp{,-asan}.log` 和
`build/metadata-validation-producer-asan.log`；
`build/metadata-validation-final-sha256.json` 记录源码、二进制及日志哈希。
格式检查及 `git diff --check` 通过。新增检查沿用现有 Native CI 入口，未在
本轮执行托管 CI。sanitizer 开启 LSan，插桩范围为扩展及 C++ 测试，复用的
DuckDB host、HTTPFS 和系统媒体库未重新插桩。

代价是每次远程校验增加一次目录列举和逐元数据文件探测，实际请求量与下载量
仍受 DuckDB 文件系统缓存、预取、整文件下载及重试策略影响。本轮覆盖默认
文件缓存开/关，没有禁用 ETag 检查。此校验仅用于 DuckDB 识别为 remote 的
root；本地仍使用 `info.json` 提交标记。它不提供跨查询全程的 ACL 或配置
事务快照，也不对元数据指纹完全不变的内容修改作保证。

## 2026-09-06：补齐 manifest GET 撤权检查

继续检查上一轮新增的远程校验，确认同类问题仍遗漏 `meta/info.json`：
`ReadInfoFingerprint` 只打开和读取文件属性，没有实际读取内容。当服务端
允许 HEAD、拒绝 GET 时，预热后的原生 JSON 读取报 403，扩展的数据缓存
仍返回 action 合计 1。零 episode 数据集同样复现：只保留 `info.json`、
没有任何 episode/data/video 文件，撤权后仍能从缓存返回零行。
客户端凭证、endpoint、manifest 字节和指纹始终不变。

现在远程 manifest 的指纹校验也通过 DuckDB 文件系统请求读取 1 字节。
空数据集执行同一校验，不依赖 episode 文件存在。中断检查覆盖读取前后，
文件系统包装的取消错误转换回 `InterruptException`，句柄由 RAII 释放。
本地 root 继续只读取文件属性，远程读取仍服从宿主的缓存和下载策略。

真实 HTTPFS 测试复用同一连接预热，再由服务端独立撤权，以原生 JSON 为
对照；覆盖普通/空数据集、数据/视频、文件缓存开/关。被动缓存诊断保持可用。
C++ 在 manifest 和 episode 探测读取处分别中断，验证句柄归零及连接恢复。

已完成验证：三种扩展构建成功；FFmpeg ON / OFF 完整 SQL 分别通过
1,596 / 1,143 条断言；HTTPFS ON / OFF / ASAN+UBSAN+LSan 分别通过
51 / 34 / 51 项检查；Release 和 ASAN+UBSAN+LSan C++ 均通过 19 个用例、
1,089 条断言，sanitizer 视频 producer 另通过 6 个用例、69 条断言。
Black、Ruff、clang-format、CI YAML 解析和 `git diff --check` 通过。

修复前快照位于 `build/before-manifest-access-20260906/`，失败证据分别为
`build/manifest-access-before.log` 和 `build/manifest-access-empty-before.log`。
本轮日志使用 `build/manifest-access-*.log`，最终文件哈希归档于
`build/manifest-access-final-sha256.json`。新增检查沿用现有 Native CI 入口；
本轮没有执行托管 CI，也没有重新插桩 DuckDB host、HTTPFS 或系统媒体库。

## 2026-09-06：连续两轮复核当前全部改动

按用户要求，对相对 `fab70ad` 的全部 58 个已修改/未跟踪文件连续进行两轮
review。第一轮发现并实测复现两项 bug，修复后冻结输入再进行第二轮；第二轮
没有发现新增可确认问题。详细范围、证据、验证命令和限制见
[两轮审查记录](review-double-20260906.md)。

- 远程非空数据集提交为空并删除 episode 文件后，旧缓存先列举旧文件而报错。
  改为先验证 `info.json` 提交标记，再决定是否校验旧 generation 的 episode
  文件。HTTPFS 回归验证同一连接的非空 → 空 → 非空以及文件缓存开/关。
- COPY 在最终 remux 已开始后取消，旧实现仍可能发布最终 root，再向用户
  返回 `InterruptException`。remux AVIO 现在检查调用方取消状态，最终发布
  前另加中断检查；实测覆盖输出打开、片段读取、faststart 和 manifest 写入，
  以及无 FFmpeg 的 numeric COPY。失败后句柄和 staging 清理、同库重试均通过。

FFmpeg ON/OFF 完整 SQL 分别通过 1,596/1,143 条断言；HTTPFS
ON/OFF/ASAN+UBSAN+LSan 分别通过 53/36/53 项；写入 C++ Release 和
ASAN+UBSAN 各通过 23 个用例、6,979 条断言，OFF 通过 13 个用例、1,610 条
断言。开启 LSan 的写入子集通过 21 个用例、6,298 条断言；嵌套读取、codec
executor、producer 和 decoder 的 sanitizer 检查也通过，详见审查记录。

两个输入快照分别为 `build/double-review-before-20260906/` 和
`build/double-review-round2-input-20260906/`。本轮证据使用
`build/double-review-*.log`，最终哈希记录为
`build/double-review-final-sha256.json`。用户原始审查文档未修改。

## 范围与后续

- 路径保护是 lexical containment；符号链接等行为仍遵循 DuckDB 文件系统规则。
- 本机未执行 Windows、macOS 或 ARM 构建。Windows concat 的原有反斜杠拒绝已移除，不能据此宣称完整平台支持已验证。
- 写侧 MP4、faststart 和 concat 已接入 DuckDB 文件系统；跨平台运行仍需单独验证。
- 新基线已包含显式 HF URI、深度参数化及越界策略、稳定 target ID、API 收敛和外部 conformance 基础设施，未重复实现旧 review 中已解决的项。
- 已复现的会话配置丢失、S3 凭证/secret 范围缓存隔离、加载中途切换 endpoint 的缓存污染、服务端撤权后的旧路由复用及远程 episode 元数据变化未失效已修；其它会话状态和事务仍未继承，不能仅凭独立 Connection 推断已有事务正确性 bug。
- Windows/macOS/ARM、更多 FFmpeg 版本和本机无法运行的 TSan 属于待验证项目，可能暴露新 bug；不归为已完成或纯性能优化。
- 跨查询时间戳缓存、windows 的 VALUES SQL、线程池缩容、depth 常量计算和跨 episode 流水线等属于后续优化；新增可复现 bug 优先于这些项目。
