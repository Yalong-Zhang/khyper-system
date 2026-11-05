# KHyper System

本地通用图分析工作空间：选择外部图数据集，向单个 agent 提问，调用 C++ 图统计工具并生成带数据依据的报告。

![通用数据集选择](docs/dataset-preview.png)

## 启动和选择数据集

在项目目录打开 PowerShell，运行 `powershell -NoProfile -ExecutionPolicy Bypass -File .\start.ps1`。默认页面为 http://127.0.0.1:8765 。保持终端运行，按 Ctrl+C 停止。

1. 在页面点击“浏览路径…”选择 JSON 数据集描述文件，或直接填写绝对路径。
2. 点击“加载数据集”，等待校验和加载完成。
3. 输入问题，例如“按度数列出前三个节点”或“分析权重最高节点的邻居”。

页面提供 `examples/datasets` 下三个描述示例。示例仅引用原有外部数据，不附带复制的数据文件：有向示例、OpenAIRE 无向合著图、DBLP 无向合著图。DBLP 的完整图较大，需足够内存；目前完整实测有向示例和 OpenAIRE，小图测试覆盖 TSV。

**选择的是描述文件，不是上传大文件。** 描述文件映射节点表、边表的路径与字段，格式详见 [DATASET_FORMAT.md](docs/DATASET_FORMAT.md)。两张表可在任何可读本地目录，路径相对于描述文件解析。Python 只发送描述文件路径，C++ 直接读取和校验原文件，并建立内存中的 KHyperGraph；不转换格式、不生成二进制数据缓存。

首次准备依赖或修改 C++ 后，停止服务再运行 `setup.ps1`；已有依赖时可用 `setup.ps1 -SkipInstall`。需要 Python 3.12+、CMake 3.20+ 和 Visual Studio 2022 的“使用 C++ 的桌面开发”组件（MSVC x64 与 Windows SDK）。原生读取器复用项目 PyArrow 附带的 Arrow/Parquet C++ 库；其 Windows ABI 要求使用 MSVC。构建脚本会部署所需 DLL，无须手动配置 PATH。`start.ps1 -Port 8770 -NoBrowser` 可指定测试端口。

## 模型配置

只使用一份 `config.yaml`。填写：

```yaml
llm:
  api_key: "你的密钥"
  url: "https://服务商接口地址/v1"
  model_name: "准确的模型名称"
```

支持 Chat Completions 原生工具调用，根地址或完整 `/chat/completions` 地址均可。修改配置后重启。默认请求不带服务商特定的 thinking 参数。若服务明确返回工具与推理模式冲突并要求 none，只针对该错误尝试一次工具兼容请求。需要同时推理和工具的其他协议仍需适配。

真实密钥只由后端读取，`config.yaml` 已被 Git 忽略。可选环境变量会覆盖 YAML；系统不读取旧 api.txt。

## 当前能力

- 有向图：入度、出度、总度、不同邻居并集，入／出强度、总强度、净入强度。
- 无向图：度数、强度和邻居，agent 只获得适用的指标与方向。
- 加权与不加权查询提供独立工具；名称或外部 ID 检索、排名、节点统计、邻居查询均为通用接口。
- 外部节点 ID 使用十进制字符串，支持 OpenAIRE 的大整数 ID，避免浏览器精度损失。
- 数据源必须是已准备好的简单图：自环、重复边、缺失端点和非法字段会被拒绝，不自动清洗。
- 读取进度、校验、就绪、失败状态可见；分析或加载中禁止切换。切换成功开始加载时清空旧报告，失败不会激活半张图。
- 每次分析独立，报告不落盘；刷新页面保留服务进程中的当前图，但不会恢复报告。重启后需重新选择。
- 字段认识：逻辑字段／物理列映射、标量类型、允许空值情况，以及完整扫描得到的空值数量和少量取值示例。
- 通用查询：节点／边属性筛选、端点属性筛选、字段投影和排序；支持 AND 与 eq/ne/in/gt/gte/lt/lte/is_null/is_not_null。
- 分组统计：节点／边分组、count/sum/avg/min/max，先计算全部匹配记录再限制输出行数。
- 排名支持候选节点属性筛选；邻居支持邻居属性筛选。指标仍基于完整图，邻居 share 分母仍为完整方向权重。
- 暂无中心性、社区发现、roll-up、OR、诱导子图计算或跨任务追问。

## 文件组织

```text
backend/                 后端服务、agent、数据加载与工具
  native/
    src/                 C++ 图加载和统计实现
    vendor/              KHyperGraph 核心与 JSON 依赖
  skills/                agent 分析规则与自动生成的英文工具参考
  maintenance/           数据检查及加载测速工具
  serve.py               启动服务与打开浏览器
  pick_dataset.py        本机文件选择窗口
frontend/                数据集选择、进度、报告和依据表
examples/datasets/       引用外部数据的描述文件示例
tests/                   Python 与 C++ 测试
docs/                    架构、数据格式及验证说明
.runtime/                自动生成的缓存、构建产物及临时记录
.venv/                   项目 Python 运行环境
```

## 验证

以下命令不消耗模型 API：

```powershell
.\.venv\Scripts\python.exe -m pytest -q
.\.venv\Scripts\python.exe backend\maintenance\check_graph.py examples\datasets\openaire-coauthor.json
```

测试验证外部文件内容不变、有向和无向指标、两种格式、大整数 ID、孤立节点、错误图拒绝、agent 与 HTTP 调度。`/docs` 为 API 文档。实现细节见 [dataset-selection.md](docs/dataset-selection.md)。

加载性能的优化说明、实测结果及复测命令见 [loading-performance.md](docs/loading-performance.md)。

“浏览路径”对话框出现在后端运行的电脑上，适合当前本机使用；远程服务请填后端可访问的路径。文件选择取消后不改变当前数据集。

## 更新后页面显示旧样式

停止旧服务，重新运行 start.ps1，再刷新页面。HTML 自动为 CSS/JS 加上内容版本号，静态资源同时使用 no-store，避免新页面混用旧脚本和样式。Windows 浏览路径使用系统原生对话框；取消会恢复按钮，超过三分钟未选择会提示超时。

## 临时文件与缓存

Python 字节码缓存与 pytest 缓存统一写入 `.runtime/python/` 和 `.runtime/pytest/`，不再在源码目录生成 `__pycache__` 或在根目录生成 `.pytest_cache`。`setup.ps1` 会将内置缓存规则安装到项目虚拟环境，`start.ps1` 会在缺失时自动调用安装，因此直接调用 `.venv/Scripts/python.exe` 也使用此规则。`.runtime/build/` 存放 C++ 构建产物，`config.yaml` 中的 worker 路径也指向这里。停止服务后可清理 `.runtime/`；删除构建产物后，下次启动会自动重新编译。`.venv/` 为 Python 运行环境。

## 查询工具与英文参考

工具注册表位于 `backend/tools.py`，统一维护模型工具 schema、递归参数校验、用途、示例和页面步骤说明。英文参考 [tool_reference.md](backend/skills/tool_reference.md) 由注册表生成，agent 在每次分析时会读取它。修改工具后运行 `.\.venv\Scripts\python.exe -m backend.tools --write-reference`；`setup.ps1` 也会自动生成。C++ 查询实现位于 `backend/native/src/query_engine.cpp`。

使用与统计口径详见 [query-tools.md](docs/query-tools.md)。例如可以询问“有哪些字段，哪些字段有空值？”、“按某个类别统计节点数量”、“在指定类别的节点中按完整图强度排名”、“统计满足两端属性条件的边数及权重总和”。字段名和属性取值均以当前数据集为准。
