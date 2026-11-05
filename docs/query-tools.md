# 筛选、统计与 agent 工具

本轮加入六个工具：`get_dataset_schema`、`profile_fields`、`query_vertices`、`query_edges`、`aggregate_vertices`、`aggregate_edges`。它们与原有八个工具一起由 `backend/tools.py` 注册。数据扫描、字段类型检查、筛选、排序、分组和聚合在 C++ 中执行；Python 管参数结构校验、agent 调度和结果呈现。

## 数据认识

先调用 `get_dataset_schema({"entity":"vertices"})` 或 `edges`，取得逻辑字段名、物理列名、标量类型、允许空值与隐式字段信息。`nullable` 表示格式允许空值，不代表数据中实际有空值。`query_field` 是用于查询的无歧义字段名。

再按需调用 `profile_fields`，例如：

```json
{"entity":"vertices","fields":["country","name"],"example_limit":3}
```

返回完整扫描的空值／非空数量、空值比例，以及加载顺序中最先遇到的少量不同非空值。示例不是随机样本，也不是频率最高的取值。空表的空值比例为 null。只认识描述文件中映射的字段；不猜测未映射物理列。

## 查询与筛选

以下示例中的字段必须先在当前数据集中确认：

```json
{
  "filter":[{"field":"country","op":"eq","value":"AU"}],
  "fields":["id","name","country"],
  "order_by":[{"field":"name","direction":"asc"}],
  "limit":10
}
```

作为 `query_vertices` 参数可取得符合条件的节点、全部匹配数量和结果截断标记。查询返回原始属性；原始 name 为空时仍为空。名称检索、排名、邻居里的显示名称继续使用 ID 兜底。

- 筛选条件列表表示 AND；支持 eq/ne/in/gt/gte/lt/lte/is_null/is_not_null。
- `in` 的 value 是非空数组；空值判断不带 value。其他比较均排除空值，包括 ne。
- 字符串精确、区分大小写；没有 substring、正则或 OR。
- int64 可用整数或规范十进制字符串比较，大整数请用字符串；不接受浮点数冒充整数。bool 只支持 eq/ne/in 与空值判断。
- 默认按数值外部 ID 排序；指定排序时空值总在最后，ID 用于稳定打破并列。
- query 结果每次最多 50 行，但 total_matches 统计全部匹配记录。可选 fields 最多 16 项，省略时取字段顺序中的前 16 项。

边工具的 `filter` 可以使用 `weight`、`edge.kind` 等边字段，以及 `source.country`、`target.country` 等端点属性。`source_filter` 和 `target_filter` 则使用普通节点字段名。边属性本身如叫 `source.country`，用 `edge.source.country` 消除歧义；schema 的 query_field 已处理这种情况。

**无向边每条只计一次，source 是外部 ID 较小的端点，target 是较大的端点。** 这不是方向，也不是按类别字母顺序排列。无向图的端点属性分组是这种规范次序下的分组，不是对称类别矩阵。若要求无序类别对总量，不能只选择 source 类别 A、target 类别 B 而漏掉相反次序；当前没有 OR 或专用无序类别对聚合工具，应说明此限制，或分别查询两种互斥次序后基于真实结果报告。

## 分组聚合

例如 `aggregate_vertices`：

```json
{
  "group_by":["country"],
  "aggregations":[{"op":"count","as":"vertex_count"}],
  "order_by":[{"field":"vertex_count","direction":"desc"}],
  "limit":10
}
```

例如 `aggregate_edges`：

```json
{
  "source_filter":[{"field":"country","op":"eq","value":"AU"}],
  "aggregations":[
    {"op":"count","as":"edge_count"},
    {"op":"sum","field":"weight","as":"total_weight"}
  ]
}
```

支持 count、sum、avg、min、max。count 不指定 field 时计记录数；指定 field 时只计非空值。sum/avg 只接受数值字段。分组键为 null 时单独成组；字段聚合跳过 null。没有匹配记录且没有 group_by 时仍返回一行：count/sum 为 0，min/max/avg 为 null；有 group_by 时返回空行集。

所有匹配记录都会参与聚合，limit 仅限制最终返回多少组。最多 3 个分组字段、8 个聚合、2 个排序字段；别名不得重复或与分组字段冲突。超过 100000 组会报错，不会悄悄抽样。整数求和采用有符号 int64，并检测中间溢出；avg 为近似 float64。对端点属性求和按每条匹配边计入一次，不是按去重节点计入，没有 distinct count。

## 现有工具扩展

- `rank_vertices_weighted/unweighted` 新增 `filter`。只筛选候选节点；排名指标仍在完整图上计算。`total_vertices` 是全图数量，`matched_vertices` 是筛选后数量。
- `get_vertex_neighbors_weighted/unweighted` 新增 `neighbor_filter`，筛选邻居节点属性。`total_neighbors` 是筛选前数量，`matched_neighbors` 是筛选后数量；加权结果另有 matched_weight。
- 邻居 share 仍以完整方向 total_weight 为分母，筛选和截断后的占比之和可能小于 1。零总权重时 share 为 null。
- 不构造诱导子图、不改变图和属性，不保留跨任务中间结果。

## 注册表和英文参考

英文文档位于 `backend/skills/tool_reference.md`，由注册表的 schema、用途、示例和返回说明生成。agent 每次请求会读取该文档和 graph_analysis.md。手动修改自动生成的 Markdown 会在下次构建时被覆盖。

```powershell
.\.venv\Scripts\python.exe -m backend.tools --write-reference
```

`setup.ps1` 自动执行上述命令。参数名、嵌套对象、数组、枚举、边界及跨字段要求由 Python 递归校验；实际字段存在性和字段／操作数类型由 C++ 再校验。新增计算逻辑仍需实现对应的 C++ 处理器并测试，不是只写一段文档就能执行。

## 验证

71 项 Python 测试及 C++ incidence 测试通过，包含新增查询、空值、类型、ID 精度、分组、溢出、过滤排名／邻居、无向端点、注册表／文档同步，以及模拟模型调用真实 C++ 工具的连续分析。

真实数据验证：OpenAIRE 198393 个节点按 country 分为 190 组，耗时约 0.14 秒；全量 330373 条边计数和权重求和约 0.19 秒。另一个有向示例的 788007 个节点按 hq_country 分为 218 组，约 0.62 秒；2210046 条边全量计数与求和约 1.12 秒。两图总权重分别为 1331715、2628805，与原有结果一致。时间不含加载、模型或浏览器，取自本机本轮单次查询，不是跨机器性能承诺。临时记录在 `.runtime/query-validation.json`。测试没有调用真实模型 API。

当前运行路径为 `.runtime/build/Release/khyper_worker.exe`，所需 DLL 与其放在一起。停止旧服务后重新运行 start.ps1 并重新加载数据即可使用新功能。
