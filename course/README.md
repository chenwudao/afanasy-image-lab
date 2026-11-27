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
- **B 的存储接口**：[tile_store.h](../afanasy/src/image_lab/tile_store.h) 的 stage 接收本次尝试的完整 RGB8 缓冲，commit 由 afserver 在收到对应成功上报后调用。成功返回包含首次确认及已提交重放；错误抛出异常并保留任务错误记录。过期上报在调用 commit 之前过滤。所有像素和作业数据通过自建 inode 镜像 disk.img 保存。最后一个图块提交时 afserver 同时拼图、生成无损 PNG 并提交同一元数据检查点。执行事件日志仍在宿主侧供管理台读取。
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

镜像内每个作业保存 manifest（尺寸、块边长、操作和优先级）、source.rgb、上传的 original.png 或 original.jpg、tile-N.rgb 及 result.png。stage 的尝试文件与正式块分名，commit 在同一事务中重命名并确认；最后的 PNG 若写入失败，最后一块也不会成为正式结果。旧宿主目录中已完成的作业可用 `af-image-fs import-completed "$JOB_KEY"` 导入，工具校验所有块后一次提交，保留原目录。单独持有正常关闭后的镜像也可以列出和导出已完成作业。

## C 模块：真实图片往返

单机集群启动后，在已配置 Afanasy Python 客户端与 IMAGE_LAB_ROOT 的提交环境安装 python3-opencv，并运行：

```bash
/usr/bin/python3 course/image_job.py photo.jpg result.png --operation gray
/usr/bin/python3 course/image_job.py photo.png inverted.png --operation invert
```

输入按文件内容识别 PNG 或 JPEG，解码为原始存储方向的三通道 RGB8（舍弃透明度）。原文件和解码像素保存在镜像内的作业目录。afserver 收到最后一块时按坐标拼图，与最终 PNG 一起提交；下载命令直接导出该 PNG。集成测试使用 130×150 的非对称彩图、64×64 图块和独立的整图参考，重新打开实际导出的 PNG 逐像素比较。JPEG 的参考输入取自有损编码后的解码图。
