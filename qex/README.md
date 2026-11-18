# qex — 带运行时代码生成（JIT）的查询执行器

一个自包含的 C++17 项目，演示如何给解释执行的日志分析引擎加上**运行时
C 代码生成 + 本机编译 + 动态库加载**的 JIT 执行路径，并通过一个内嵌网页
实时观察查询提交、生成代码、编译缓存、向量化和性能对比。

## 它解决的问题

解释执行器对每个表达式节点都走一次虚函数派发，一条复杂过滤表达式在每一
行上都付出同样的间接跳转、装箱和分支成本，CPU 全耗在调度上。qex 在运行
期把表达式直接翻译成紧凑的、可向量化的 C 代码，调本机编译器编成动态库后
`dlopen` 执行，消除解释开销。

## 八个核心机制（对应需求）

| # | 机制 | 实现位置 |
|---|------|----------|
| 1 | 过滤/投影表达式运行时生成 C，本机编译器编成动态库加载执行 | `src/codegen.cpp` `src/jit.cpp` |
| 2 | 按表达式**结构**缓存，同结构不同常量复用同一份动态库，常量运行期绑定 | `src/expr.cpp`（结构哈希）`src/codegen.cpp`（`a->consts[]`） |
| 3 | 编译超时/失败自动退回解释执行，结果逐行比对保证一致 | `src/jit.cpp`（超时+错误处理）`src/engine.cpp`（回退+校验） |
| 4 | 字符串比较/LIKE 按模式生成专用路径：exact/prefix/contains/glob 分流 | `src/expr.cpp`（模式分类）`src/codegen.cpp`（字典位图） |
| 5 | 公共子表达式生成前合并（每行只算一次）；多个 AND 条件按选择率重排短路 | `src/optimizer.cpp` |
| 6 | 空值处理放进位图批量计算，不在每行里判断 | `src/codegen.cpp`（`nt*` 位图、批量 null 字数组） |
| 7 | 动态库按结构哈希落盘缓存，重启直接复用；编译器版本变化自动失效 | `src/jit.cpp`（指纹目录） |
| 8 | 网页展示生成代码、编译耗时、缓存命中与向量化情况 | `src/webserver.cpp` `web` 命令 |

## 构建

```sh
make            # 产出 ./qex，需要 clang/gcc（C++17）和可用的 cc
```

运行 JIT 时生成的 C 源文件 `#include "qex_runtime.h"`，通过环境变量
`QEX_RUNTIME_H` 指定该头文件路径（也会自动尝试 `src/qex_runtime.h`）：

```sh
export QEX_RUNTIME_H=$PWD/src/qex_runtime.h
```

## 使用流程

### 1. 生成模拟日志（列式存储 + 字典编码）

```sh
./qex gen --rows 100000000 -o var/logs.qex   # 一亿行
./qex info var/logs.qex                       # 查看列、基数、空值
```

表结构（11 列）：`ts, service, level, region, host, msg`（字典编码字符串），
`status, latency_us, resp_bytes, user_id`（i64），`cpu_pct`（f64），
测量列带约 1.5% 空值。

### 2. 跑基准（解释 vs JIT，逐行校验）

```sh
./qex run --data var/logs.qex --queries queries/bench.sql \
          --cache var/jitcache
```

常用开关：

- `--timeout 0.001`：把编译超时压到 1ms，演示**超时回退**
- `--force-fail`：传非法编译参数，演示**编译失败回退**
- `--interp`：强制全程解释执行
- `--no-verify`：跳过逐行比对（压测纯吞吐）

### 3. 打开网页仪表盘

```sh
./qex web --data var/logs.qex --cache var/jitcache --port 8080
# 浏览器访问 http://127.0.0.1:8080
```

五个标签页：**① 查询提交 ② 生成代码 ③ 编译缓存 ④ 向量化情况 ⑤ 性能对比**。

## 缓存目录布局

```
var/jitcache/
├── fingerprint.txt                 # 编译器 + 目标三元组 + 版本哈希
└── <fingerprint-hash>/             # 编译器版本变了就换目录 -> 自动失效
    ├── qex_runtime.h
    ├── <结构哈希>.c                 # 生成的翻译单元
    ├── <结构哈希>.dylib|.so         # 编译产物（跨进程复用）
    └── <结构哈希>.meta              # 编译耗时、向量化循环数
```

结构哈希只编码 AST **形状**和 LIKE 模式分类，不含字面量值，因此
`latency_us > 3000` 与 `latency_us > 9000` 复用同一个动态库，常量通过
`qex_filter_args.consts[]` 在运行期注入。

## 关键设计说明

- **字符串谓词字典位图**：LIKE/等值匹配在计划期对小字典（几十到几百项）
  一次性算出匹配位图，执行期每行只需一次位图探测，可向量化；前缀、包含、
  通用 glob 走不同匹配函数。
- **位图化空值（3VL）**：每个中间值带一个 null 位图字数组，AND/OR 的
  SQL 三值逻辑用位运算批量求值，避免逐行 null 分支。
- **AND 选择率重排**：基于列基数、min/max 和空值率估算各条件选择率，
  最严苛（命中率最低）的条件排在最前，配合短路减少求值。
- **向量化**：生成代码以 1024 行一批、`restrict` 指针 + 紧凑数组表达，
  编译时加 `-Rpass=loop-vectorize` 收集向量化备注并展示在网页上。

## 性能特征（实测，100M 行 / Apple Silicon）

- **过滤是 JIT 的主战场**：低选择率谓词上，JIT 过滤比解释执行快
  约 **6–17 倍**（纯虚函数派发被完全消除、循环被向量化）。
- 高选择率（绝大多数行被选中）时，投影和结果物化受内存带宽限制，
  两种执行方式趋同——这是列式扫描的物理上限，而非代码生成失效。
- 单次编译约 60–170ms；结构缓存命中后该成本归零，落盘缓存让进程重启
  也不重复编译。网页和 `run` 输出分别报告编译耗时占比与缓存省下的时间。

## 源码地图

```
src/
├── qex_runtime.h   生成代码与引擎共享的 C ABI（列、常量、位图、匹配函数）
├── common.*        列式 Table/Column、位图、哈希、文件工具
├── expr.*          SQL 子集词法/语法分析、AST、结构哈希、类型推导
├── optimizer.*     公共子表达式合并、AND 选择率重排、统计信息
├── interpreter.*   基线：逐行虚函数派发的解释执行器（3VL）
├── codegen.*       AST -> C 翻译单元（位图 null、向量化循环、字符串路径）
├── jit.*           本机编译、超时、dlopen、内存/落盘两级缓存、执行
├── datagen.*       模拟日志生成、二进制列格式存取
├── engine.*        编排：解析->优化->解释/JIT->回退->逐行校验->统计
├── webserver.*     零依赖 HTTP 服务 + 单页仪表盘
└── main.cpp        gen / info / run / web 四个子命令
```
