# A 模块联调

主路径为 Afanasy 的 afserver → 四个 afrender → C++ af-image-worker。每张图是一个原生 Job，只有一个 image_tile Block，每块是一条 Task。任务在原管理台可查询。Linux 验收脚本启动真实服务与执行端，再通过 Afanasy 原生客户端提交作业。

## 在 Linux 重现

依赖 C++ 编译器、CMake、Python 3.9+ 与开发头文件、libzip-dev、libpng-dev、zlib1g-dev、python3-opencv。进程终止接口使用 Linux pidfd。以下命令从仓库根目录执行。

```bash
export AF_GUI=NO AF_POSTGRESQL=NO AF_OSTYPE=linux
build=$(mktemp -d)
cmake -S afanasy/src/project.cmake -B "$build/a/b/c" -DBUILD_TESTING=OFF
cmake --build "$build/a/b/c" --target afserver afrender af-image-worker af-image-fs -j4
export PATH="$(dirname "$(find "$build" -type f -name afserver -executable -print -quit)"):$PATH"
python3 -u course/integration.py
```

若生成器输出目录与构建树不同，请将三个可执行文件所在目录加入 PATH。构建和验收的自动化入口为 [.github/workflows/image-lab.yml](../.github/workflows/image-lab.yml)。脚本使用临时端口和独立的存储目录，结束时关闭本次启动的进程。日志、输入、正式像素和事件保存在 course-runtime 下。演示延时以毫秒为单位，默认 0；测试打开它以便稳定选中正在运行的 PID。

## 接口约定

- **提交与 C**：调用 submit(pixels, width, height, operation, priority, side)，返回原生作业 ID 和私有目录。pixels 为从左上逐行排列、无行填充的 RGB8，operation 可选 gray、invert，priority 可选 normal、high。job key 为服务端提交适配器生成的 32 位十六进制串。块号从 0 按行优先递增，descriptor() 返回输出矩形。Worker 首版支持 RGB8 灰度和反色算子，灰度结果复制到三个通道。C 后续接上图片编解码即可。
- **派发与恢复**：同级按就绪序号调度，高优先级先领取，正在运行的块可以继续。重派使用原优先级和新的入队顺序。每次原生任务启动时，服务端把新的 starts_count 注入 IMAGE_ATTEMPT，并通过任务上报的辅助编号校验本次尝试；旧尝试的状态不能完成当前任务。Linux 执行端负责回收计算子进程。示例提交允许失败后三次重试。
- **B 的存储接口**：[tile_store.h](../afanasy/src/image_lab/tile_store.h) 的 stage 接收本次尝试的完整 RGB8 缓冲，commit 由 afserver 在收到对应成功上报后调用。成功返回包含首次确认及已提交重放；错误抛出异常并保留任务错误记录。过期上报在调用 commit 之前过滤。所有像素和作业数据通过自建 inode 镜像 disk.img 保存。afserver 先持久化每个正式图块。最后一个图块提交后生成无损 PNG，再发布作业完成状态。执行事件日志仍在宿主侧供管理台读取。
- **D 的进程与事件接口**：[control.py](control.py) 提供 read_events(job_key, cursor)，返回下一字节游标和完整事件列表。时间是 Unix 毫秒，块号和尝试号关联同一次执行，服务端的 dispatch_sequence 表示实际派发次序。started 记录真实 PID，exit-N 或 signal-N 表示退出原因，killed/failed、requeued、committed、commit_failed 表示后续状态。调度序号仅在同一次服务器运行期间有效。画布只读取已提交结果。
- **D 的终止接口**：已完成管理员身份和作业权限校验的后端调用 terminate_attempt(job_key, tile, attempt, pid)。该接口固定进程句柄并检查真实可执行文件、作业和尝试环境，防止 PID 复用或旧页面操作误杀。普通用户的事件与结果查询同样须由业务后端校验作业归属。服务端、执行端需在同机、同一可信系统账号下运行，共用私有 IMAGE_LAB_ROOT。公开用户登录及前端按钮由 D 对接。

## 通过条件

单块灰度像素与串行值一致。普通作业的四块同时占用四个不同 PID，随后提交的高优先级两块按入队顺序获得接下来的两个派发位置。终端终止某一次计算后，受影响的块使用新 PID 重派，其他块保留，最终结果逐字节正确。管理接口拒绝错误尝试号，终止正确尝试后同样可完成。存储测试覆盖边缘块、部分写入、确认丢失重放以及迟到重复结果。

## B 模块 · disk.img

所有进程共用 IMAGE_LAB_ROOT/disk.img。超级块记录版本和容量，两个交替的元数据区各含世代号、CRC、inode 表与空闲块位图。目录由父 inode 编号与子项名称解析。数据块 4096 字节，其中 4 字节指向下一块。写事务持独占文件锁，先分配空闲块，写入并刷盘，再发布新的元数据检查点。空间不足时读者仍看到先前完整的文件和位图。共享读锁允许多个 Worker 同时取数。事件日志及 Afanasy 自身的节点状态仍采用各自的存储。

首次提交会创建 64 MiB 镜像。也可以先运行以下命令，以 4096 字节为单位指定新镜像容量。已有镜像按原容量打开。服务端、执行端、提交端应共享同一私有目录及镜像。本版本使用 Linux 小端格式。

```bash
export IMAGE_LAB_ROOT="$PWD/course-runtime"
mkdir -p "$IMAGE_LAB_ROOT"
af-image-fs init 16384
af-image-fs list /
af-image-fs list "/$JOB_KEY"
af-image-fs read "/$JOB_KEY/result.png" > recovered.png
```

镜像内每个作业保存 manifest（尺寸、块边长、操作和优先级）、source.rgb、上传的 original.png 或 original.jpg、tile-N.rgb 及 result.png。stage 的尝试文件与正式块分名，commit 在同一事务中重命名并确认；最后的 PNG 若因空间不足而写入失败，最后一块回退到暂存状态。进程若在最后一块提交后、拼图前崩溃，重启后直接从正式块拼图。旧宿主目录中已完成的作业可用 `af-image-fs import-completed "$JOB_KEY"` 导入，工具校验所有块后一次提交，保留原目录。单独持有正常关闭后的镜像也可以列出和导出已完成作业。

## C 模块：真实图片往返

单机集群启动后，在已配置 Afanasy Python 客户端与 IMAGE_LAB_ROOT 的提交环境安装 python3-opencv，并运行：

```bash
/usr/bin/python3 course/image_job.py photo.jpg result.png --operation gray
/usr/bin/python3 course/image_job.py photo.png inverted.png --operation invert
```

输入按文件内容识别 PNG 或 JPEG，解码为原始存储方向的三通道 RGB8（舍弃透明度）。原文件和解码像素保存在镜像内的作业目录。afserver 提交最后一块后按坐标拼图，把完整 PNG 写入镜像再发布作业完成。下载命令直接导出该 PNG。集成测试使用 130×150 的非对称彩图、64×64 图块和独立的整图参考，重新打开实际导出的 PNG 逐像素比较。JPEG 的参考输入取自有损编码后的解码图。

## D 模块：管理台

Linux 构建需包含 afserver、afrender、af-image-fs 和 af-image-worker，并把它们放在 PATH 中。安装 python3-flask、python3-opencv；用于投影演示的 Linux 浏览器可安装 fonts-noto-cjk 显示中文。创建两个普通用户和一个管理员，密码通过终端交互输入。没有内置密码。

```bash
export IMAGE_LAB_ROOT="$PWD/course-runtime/demo"
mkdir -p -m 700 "$IMAGE_LAB_ROOT"
/usr/bin/python3 course/web.py account alice --role user
/usr/bin/python3 course/web.py account bob --role user
/usr/bin/python3 course/web.py account manager --role admin
IMAGE_LAB_DEMO_DELAY_MS=5000 /usr/bin/python3 course/console_cluster.py
```

浏览器打开 http://127.0.0.1:8080。在普通窗口上传 PNG/JPEG，选择操作与优先级。在独立浏览器会话登录管理员，查看全部图像作业和按优先级、真实入队序号排列的就绪队列。打开作业，在进程时间线点“终止 PID”。选中对应图块后，执行历史显示派发、真实 PID、退出信号、重新入队、新尝试及正式提交。已提交块显示从 disk.img 读取的真实像素，虚线框表示发生过重试。下载返回镜像中的最终 PNG。性能实验使用默认延时 0。

管理页使用 Afanasy Browser 的原生 TaskItem、状态渲染和样式，图片作业继续使用原有 afserver/afrender 协议。角色和密码哈希、过期及可撤销的会话保存在私有 web.sqlite 中；上传时由服务端把登录身份写入镜像中的作业描述。普通用户只能经服务端读取自己的作业、增量事件、图块和结果，管理员可查看全局并终止核验过的计算尝试。旧作业及 CLI 提交的无所有者作业仅对管理员可见。课程模式下原生控制端口仅绑定本机 IPv4 并拒绝浏览器 HTTP，浏览器统一使用认证入口；同机能运行原生客户端或读取运行目录的 OS 账号属于受信任运行环境。退出登录或重置密码会撤销对应会话。

CI 在真实四执行端集群中启用 IMAGE_LAB_BROWSER_TEST=1，使用两个隔离 Chromium context 检查两个普通账号的作业隔离，再在其中一个会话改用管理员登录。测试点击终止按钮，核对页面上的旧、新 PID 与尝试号，并检查下载的 PNG 和已验证的参考输出完全一致。截图随 image-lab-evidence 测试产物保存。

## LRU 块缓存实验

IMAGE_LAB_CACHE_BLOCKS 指定每个打开的 Disk 句柄可缓存多少个 4096 字节的数据块，默认 64，设为 0 即关闭。启动整个集群之前设置，提交端及其子进程应使用相同设置。容量按需分配；可设范围为 0–262144。链指针、完整文件与矩形行读取共用缓存。命中把块移到最近使用位置，满时淘汰最久未使用的块。写入直接更新镜像，在写入前作废对应缓存项。缓存寿命与持有文件锁的事务一致，其他进程写入后重新打开镜像会冷启动；每次独立打开的元数据读取单独计数。

固定序列为 AABACABACABDDDCA。四个一块长的小文件分别对应四个不同的镜像数据块。每种容量在同一镜像上新开一个句柄，从空缓存按同一顺序读取 16 次。命中率为命中次数除以数据块访问次数。data_preads 在 pread 调用处计数，metadata_preads 另记挂载检查点等元数据读取。实验用 strace 按 disk.img 路径核对系统调用总数；操作系统页缓存也可能满足这些调用，计数反映应用到内核的镜像读取。

在构建好的 Linux 环境运行以下命令。脚本创建独立的临时镜像，检查返回数据的 CRC，并输出可保存的 JSON。完整 CI 还会依次以 0、1、2、4 块容量重启服务与执行端，在每种配置下重新处理 130×150 的 PNG/JPEG 并逐像素核验灰度和反色，同时核对最终 PNG 的 SHA-256。

```bash
python3 course/cache_benchmark.py --output course-runtime/lru-results.json
IMAGE_LAB_CACHE_BLOCKS=64 IMAGE_LAB_CACHE_TEST=1 /usr/bin/python3 course/integration.py
```

## 整盘备份与恢复

在已构建的 Linux 环境设置 IMAGE_LAB_ROOT 指向运行目录，再执行：

```bash
af-image-fs backup ./online-backup.img
mkdir ./restored
af-image-fs restore ./online-backup.img ./restored
IMAGE_LAB_ROOT="$PWD/restored" af-image-fs list /
IMAGE_LAB_ROOT="$PWD/restored" af-image-fs list "/$JOB_KEY"
IMAGE_LAB_ROOT="$PWD/restored" af-image-fs read "/$JOB_KEY/result.png" > restored.png
sha256sum restored.png
```

备份持有镜像共享锁直到复制发布。已开始的写事务先结束，计算进程和读事务可继续，新的镜像写入等待备份结束。备份和恢复都检查超级块与元数据检查点，将完整副本刷盘后发布到目标路径。目标已存在时操作失败，恢复默认写入调用者指定目录的 disk.img。镜像包含备份时点的作业目录、图块和已提交的最终 PNG。运行中的作业保留当时已提交的图块，调度状态、宿主侧事件日志和 Web 账号由各自的存储管理。

集成测试在一个六块作业的最后两块开始计算时备份。恢复副本应包含该作业的前四块及其描述，活跃镜像随后完成全部六块。四个先前完成的作业逐文件比对恢复副本，并校验最终 PNG 的 SHA-256。

## 服务器崩溃续跑

重启 afserver 时沿用同一运行目录和磁盘镜像。服务端读取已有 Afanasy 作业和镜像中的正式图块，将正式块恢复为已完成，将缺块重新入队。尝试编号在派发前先写入镜像，旧 Worker 的回报不能代替重启后的尝试。新尝试开始时回收同块的旧暂存文件。

图块发布与最终 PNG 发布分别使用镜像检查点。重启若发现图块已经齐全而 PNG 缺失，会直接拼接，不再派发任何图块。最终 PNG 可读后才发布作业完成。正常运行时若拼图因镜像满而失败，最后一块回退至暂存状态。续跑依赖已有 Afanasy 作业记录及其磁盘镜像。

运行 Linux 集成测试并加上 IMAGE_LAB_CRASH_TEST=1，可在计算中、完整数据暂存后且确认前、最后一块提交后且拼图前对 afserver 发送 SIGKILL。验收记录三次恢复的实际重算块、Afanasy 的完成计数和整图逐像素比较。测试保留原执行端，让重连和迟到回报经过真实任务通道。

## 调度策略对比

启动集群前设置 IMAGE_LAB_SCHEDULER=priority 或 IMAGE_LAB_SCHEDULER=fifo，默认 priority。两种模式都按就绪块派发且不抢占。priority 先比较作业优先级，同级按图块就绪序号；fifo 直接按跨图像作业的就绪序号。重派块重新取得就绪序号。课程管理台的管理员就绪队列使用相同排序。

以 IMAGE_LAB_SCHEDULER_TEST=1 /usr/bin/python3 -u course/integration.py 启动独立实验，使用新建的测试镜像。实验使用同样两张确定性 PNG，普通作业 194×194、高优先级作业 126×126，块尺寸 64×64，4 个执行端，均执行反色，演示延时设为 0。先提交普通作业，观察其前四块派发后再提交高优先级作业。每策略三轮，并交替先后次序。验收检查实际到达和派发顺序。

时间由服务端事件计算，以首块入队为到达，分别测至高优先级首块派发和普通作业最后一块提交完成。JSON 记录每轮的实际到达间隔、两项时间、派发次序、均值和中位数，同时保留输出 PNG 与哈希。每份输出解码后与对应串行结果和其他轮输出逐像素比较。实验报告保存在本轮 course-runtime 集成测试目录中的 scheduler-results.json。
