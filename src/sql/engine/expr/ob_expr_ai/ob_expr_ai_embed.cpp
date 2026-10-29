/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "oblib/common/json_type/ob_json_base.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
    res.set_null();
  } else if (arg_content->is_null()) {
    // A NULL input row yields a NULL embedding instead of failing the statement so
    // that a column with missing values can still be embedded as a whole.
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content = arg_content->get_string();
    if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (result.empty()) {
        ret = OB_INVALID_DATA;
        LOG_USER_ERROR(OB_INVALID_DATA, "ai_embed, embedding result is empty");
        res.set_null();
      } else {
        // 返回结果必须是合法 JSON 数组；指定 dimension 时向量维度必须一致
        ObIJsonBase *j_base = nullptr;
        if (OB_FAIL(ObJsonBaseFactory::get_json_base(&temp_allocator, result,
                                                     ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, j_base))) {
          LOG_USER_ERROR(OB_INVALID_DATA, "ai_embed, embedding result is not valid json");
        } else if (OB_ISNULL(j_base) || j_base->json_type() != ObJsonNodeType::J_ARRAY) {
          ret = OB_INVALID_DATA;
          LOG_USER_ERROR(OB_INVALID_DATA, "ai_embed, embedding result is not a json array");
        } else if (dim > 0 && j_base->element_count() != static_cast<uint64_t>(dim)) {
          ret = OB_INVALID_DATA;
          LOG_USER_ERROR(OB_INVALID_DATA, "ai_embed, embedding dimension does not match the dimension parameter");
        } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr,
                                       ObEvalCtx &ctx,
                                       const ObBitVector &skip,
                                       const int64_t batch_size)
{
  INIT_SUCC(ret);
  ObDatum *result = expr.locate_batch_datums(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  // Constant arguments (literals/params) have no per-row datum vector, evaluate them
  // once and reuse the value for every row.
  ObDatum *const_model_datum = nullptr;
  ObDatum *const_content_datum = nullptr;
  ObDatum *const_dim_datum = nullptr;
  bool model_is_batch = expr.args_[MODEL_IDX]->is_batch_result();
  bool content_is_batch = expr.args_[CONTENT_IDX]->is_batch_result();
  bool dim_is_batch = expr.arg_cnt_ == 3 && expr.args_[DIM_IDX]->is_batch_result();
  if (OB_ISNULL(result)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!model_is_batch && OB_FAIL(expr.args_[MODEL_IDX]->eval(ctx, const_model_datum))) {
  } else if (!content_is_batch && OB_FAIL(expr.args_[CONTENT_IDX]->eval(ctx, const_content_datum))) {
  } else if (expr.arg_cnt_ == 3 && !dim_is_batch && OB_FAIL(expr.args_[DIM_IDX]->eval(ctx, const_dim_datum))) {
  } else if (model_is_batch && OB_FAIL(expr.args_[MODEL_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (content_is_batch && OB_FAIL(expr.args_[CONTENT_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else if (dim_is_batch && OB_FAIL(expr.args_[DIM_IDX]->eval_batch(ctx, skip, batch_size))) {
  } else {
    ObDatum *model_datums = model_is_batch ? expr.args_[MODEL_IDX]->locate_batch_datums(ctx) : nullptr;
    ObDatum *content_datums = content_is_batch ? expr.args_[CONTENT_IDX]->locate_batch_datums(ctx) : nullptr;
    ObDatum *dim_datums = dim_is_batch ? expr.args_[DIM_IDX]->locate_batch_datums(ctx) : nullptr;
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObArray<ObString> contents;
    ObArray<int64_t> row_indexes;
    ObString model_id;
    int64_t dim = 0;
    bool has_batch_row = false;
    for (int64_t j = 0; OB_SUCC(ret) && j < batch_size; j++) {
      if (skip.at(j) || eval_flags.at(j)) {
        continue;
      }
      ObDatum *model_datum = model_is_batch ? &model_datums[j] : const_model_datum;
      ObDatum *content_datum = content_is_batch ? &content_datums[j] : const_content_datum;
      ObDatum *dim_datum = dim_is_batch ? &dim_datums[j] : const_dim_datum;
      if (OB_ISNULL(model_datum) || OB_ISNULL(content_datum)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (model_datum->is_null() || content_datum->is_null()) {
        // NULL 输入传播为 NULL 结果
        result[j].set_null();
        eval_flags.set(j);
      } else {
        if (!has_batch_row) {
          model_id = model_datum->get_string();
          if (OB_NOT_NULL(dim_datum) && !dim_datum->is_null()) {
            dim = dim_datum->get_int();
          }
          has_batch_row = true;
        }
        if (OB_FAIL(contents.push_back(content_datum->get_string()))) {
        } else if (OB_FAIL(row_indexes.push_back(j))) {
        }
      }
    }

    if (OB_FAIL(ret) || contents.empty()) {
      // 没有可批量处理的行
    } else if (model_id.empty()) {
      for (int64_t k = 0; OB_SUCC(ret) && k < row_indexes.count(); k++) {
        result[row_indexes.at(k)].set_null();
        eval_flags.set(row_indexes.at(k));
      }
    } else {
      ObAIFuncExprInfo *info = nullptr;
      share::ObAiModelEndpointInfo resolved_endpoint;
      const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
      query::ObIAiEndpointResolver *endpoint_resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
      ObJsonObject *config = nullptr;
      if (dim <= 0) {
        // 不指定维度
      } else {
        ObJsonInt *dim_json = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
        } else if (OB_FAIL(config->add("dimensions", dim_json))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
      } else if (OB_ISNULL(endpoint_resolver)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                     model_id, temp_allocator, resolved_endpoint))) {
      } else {
        ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
        ObArray<ObString> results;
        if (OB_FAIL(model.call_dense_embedding_vector_v2(contents, config, results))) {
        } else if (results.count() != contents.count()) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          for (int64_t k = 0; OB_SUCC(ret) && k < row_indexes.count(); k++) {
            int64_t row = row_indexes.at(k);
            ObString &row_result = results.at(k);
            char *buf = expr.get_str_res_mem(ctx, row_result.length(), row);
            if (OB_ISNULL(buf)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              MEMCPY(buf, row_result.ptr(), row_result.length());
              result[row].set_string(buf, row_result.length());
              eval_flags.set(row);
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   ObObj const_data;
  //   bool got_data = false;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
