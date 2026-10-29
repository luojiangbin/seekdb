# AI 函数增强实现说明（2026 第六届 OceanBase 数据库大赛 · seekdb C++ 赛题一）

本分支实现了题面「AI 函数增强」的全部六个子题，并附带本地端到端验证手段。

## 一、改动文件一览

| 文件 | 改动 |
|---|---|
| `src/sql/engine/expr/ob_expr_ai/ob_expr_ai_prompt.cpp` | 模板/参数支持 TEXT/LONGTEXT 与 JSON(Array/Object)；LOB 读取；嵌套 prompt |
| `src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp` | OLLAMA provider 注册；模板替换支持 JSON 对象序列化；Ollama header 补 Content-Type |
| `src/sql/engine/expr/ob_expr_ai/ob_expr_ai_complete.cpp` | 移除 prompt object 拒绝；响应非空校验 |
| `src/sql/engine/expr/ob_expr_ai/ob_expr_ai_embed.cpp` | 返回 JSON 数组与维度一致性校验；跨行批量求值（单请求多输入） |
| `src/sql/engine/expr/ob_expr_ai/ob_expr_ai_embed.h` | 批量求值接口声明 |
| `src/share/ai_service/ob_ai_service_struct.cpp/.h` | VALID_PROVIDERS 增加 OLLAMA；keyless provider 允许空 access key |
| `src/observer/ai_service/ob_ai_service_proxy.cpp` | endpoint 读取兼容 access_key 为 NULL（原报 OB_ERR_NULL_VALUE） |

## 二、分子题实现要点

### 子题 1：AI_PROMPT 类型与模板增强
- 类型层：模板接受 `ob_is_string_tc` 或 `ob_is_text_tc`；参数支持 varchar/TEXT/LONGTEXT/JSON
- 执行层：TEXT 模板走 `ObTextStringHelper::read_real_string_data`；JSON 参数用 `ObJsonExprHelper::get_json_doc` 取出后直接进 args_array
- 模板替换：`replace_all_str_args_in_template` 预先把每个参数序列化为文本（字符串原值 / JSON 对象 `get_raw_binary`），支持重复占位符与非法占位符报错

### 子题 2：AI_EMBED 单行语义
- 默认维度、显式 `dimension` 参数（塞进 provider config 的 `dimensions`）
- 返回结果必须是合法 JSON 数组；指定维度时校验向量长度一致

### 子题 3：AI_COMPLETE 请求与响应
- OpenAI/Endpoint 配置、`temperature`/`max_tokens` 透传（config 透传给 provider）
- 响应非空校验：空响应返回 `OB_INVALID_DATA`

### 子题 4：Ollama Provider
- `get_complete_provider`/`get_embed_provider` 注册 OLLAMA
- 空 access key 兼容：`is_keyless_provider` + endpoint 校验放行 + proxy 读取兼容 NULL
- 原生协议：Completion `POST /api/generate`（解析 `$.response`）、Embedding `POST /api/embed`（解析 `$.embeddings`）
- 请求头不带 Authorization，仅补 `Content-Type: application/json`

### 子题 5：AI_RERANK 结果正确性
- 字符串数组输入、对象数组 + `doc_key` 文本字段选择
- 分批（每批 20 篇）、返回数量/顺序/原始 ID 映射（基线已具备，本地全量验证）

### 子题 6：AI_EMBED 跨行批量
- 注册 `eval_batch_func_`，表扫描多行时收集同一批的 content，调用 `call_dense_embedding_vector_v2` —— **一次 HTTP 请求携带多条输入**
- NULL 行传播 NULL；结果按行索引回填（`get_str_res_mem(ctx, len, row)` 按行分配）
- 常量参数兼容：非 batch result 参数标量求值一次复用

## 三、本地验证方法

使用自建 Mock AI 服务（Ollama/Siliconflow 协议）验证全部网络路径：

```bash
# 启动服务
python3 mock_ollama.py   # 监听 0.0.0.0:11434

# 调用
select ai_complete('model', 'hello');                       -- mock-ollama-answer: hello
select ai_embed('model', 'x');                              -- [0.1,0.2,0.3,0.4]
select ai_embed('model', c) from t;                         -- 单请求多输入，逐行向量
select ai_rerank('model', 'q', json_array('a','b','c'));    -- 排序 + index 映射
select ai_prompt('v={0}', json_object('a',1));              -- JSON 参数
```

## 四、已知行为变更

`ai_prompt` 对 JSON 参数由「报错 1235」改为「正常接收」，这是题面「JSON (Array/Object) 参数」评分点的要求；旧 `ai_prompt.test` 的对应断言（`--error 1235`）因此不再适用。
