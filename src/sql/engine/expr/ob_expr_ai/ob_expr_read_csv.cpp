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
#include "ob_expr_read_csv.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "sql/engine/ob_exec_context.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/session/ob_sql_session_info.h"
#include "common/json_type/ob_json_base.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace sql
{

// Per-query state: whole file content, parsed lines and current row cursor.
struct ObExprReadCSVCtx : public ObExprOperatorCtx
{
  ObExprReadCSVCtx() : content_(), rows_(), curr_row_(0), loaded_(false) {}
  virtual ~ObExprReadCSVCtx() = default;
  ObString content_;
  ObArray<ObString> rows_;
  int64_t curr_row_;
  bool loaded_;
  TO_STRING_KV(K_(loaded), K_(curr_row), KP(content_.ptr()), K(content_.length()));
};

ObExprReadCSV::ObExprReadCSV(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc,
                         T_FUN_SYS_READ_CSV,
                         N_READ_CSV,
                         MORE_THAN_ZERO,
                         NOT_VALID_FOR_GENERATED_COL,
                         NOT_ROW_DIMENSION)
{
}

ObExprReadCSV::~ObExprReadCSV()
{
}

int ObExprReadCSV::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 1 || param_num > 2)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString func_name(get_name());
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name.length(), func_name.ptr());
  } else if (!ob_is_string_type(types_stack[PATH_IDX].get_type())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "read_csv, path must be a string");
  } else {
    types_stack[PATH_IDX].set_calc_type(ObVarcharType);
    types_stack[PATH_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 2 && !ob_is_string_type(types_stack[OPTION_IDX].get_type())
        && !ob_is_json(types_stack[OPTION_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "read_csv, options must be a string or json");
    }
    if (OB_SUCC(ret)) {
      // Each row is returned as a JSON array text, e.g. ["1","alice","90"]
      type.set_varchar();
      type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      type.set_collation_level(CS_LEVEL_IMPLICIT);
      type.set_length(4096);
    }
  }
  return ret;
}

// Read whole file into alloc; NUL terminated.
static int read_whole_file(ObIAllocator &alloc, const ObString &path, ObString &content)
{
  int ret = OB_SUCCESS;
  char *path_buf = static_cast<char *>(alloc.alloc(path.length() + 1));
  int fd = -1;
  struct stat file_stat;
  char *buf = nullptr;
  int64_t total = 0;
  if (OB_ISNULL(path_buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(path_buf, path.ptr(), path.length());
    path_buf[path.length()] = '\0';
    if ((fd = ::open(path_buf, O_RDONLY)) < 0) {
      ret = OB_IO_ERROR;
      LOG_WARN("read_csv, fail to open file", K(ret), K(path));
      LOG_USER_ERROR(OB_IO_ERROR, "read_csv, fail to open file");
    } else if (0 != ::fstat(fd, &file_stat)) {
      ret = OB_IO_ERROR;
      LOG_WARN("read_csv, fail to stat file", K(ret), K(path));
    } else if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(file_stat.st_size + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      while (total < file_stat.st_size) {
        ssize_t n = ::read(fd, buf + total, file_stat.st_size - total);
        if (n <= 0) {
          break;
        }
        total += n;
      }
      buf[total] = '\0';
      content.assign_ptr(buf, static_cast<int32_t>(total));
    }
    if (fd >= 0) {
      ::close(fd);
    }
  }
  return ret;
}

// Split content into lines (supports \r\n).
static int split_lines(ObIAllocator &alloc, const ObString &content, ObArray<ObString> &rows)
{
  UNUSED(alloc);
  int ret = OB_SUCCESS;
  const char *ptr = content.ptr();
  int64_t len = content.length();
  int64_t line_start = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i <= len; ++i) {
    if (i == len || ptr[i] == '\n') {
      int64_t line_end = i;
      if (line_end > line_start && ptr[line_end - 1] == '\r') {
        --line_end;
      }
      if (line_end > line_start) {
        ObString line;
        line.assign_ptr(ptr + line_start, static_cast<int32_t>(line_end - line_start));
        if (OB_FAIL(rows.push_back(line))) {
        }
      }
      line_start = i + 1;
    }
  }
  return ret;
}

// Split one CSV line into fields (supports double-quoted fields and "" escape).
static int parse_csv_fields(ObIAllocator &alloc, const ObString &line, ObJsonArray *&array)
{
  int ret = OB_SUCCESS;
  ObJsonArray *fields = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(alloc, fields))) {
  } else {
    const char *ptr = line.ptr();
    int64_t len = line.length();
    int64_t pos = 0;
    while (OB_SUCC(ret) && pos <= len) {
      ObStringBuffer field(&alloc);
      if (pos < len && ptr[pos] == '"') {
        ++pos;
        while (pos < len) {
          if (ptr[pos] == '"') {
            if (pos + 1 < len && ptr[pos + 1] == '"') {
              if (OB_FAIL(field.append(ptr + pos, 1))) {
              }
              pos += 2;
            } else {
              ++pos;
              break;
            }
          } else {
            if (OB_FAIL(field.append(ptr + pos, 1))) {
            }
            ++pos;
          }
        }
        while (pos < len && ptr[pos] != ',') {
          ++pos;
        }
      } else {
        int64_t start = pos;
        while (pos < len && ptr[pos] != ',') {
          ++pos;
        }
        if (OB_FAIL(field.append(ptr + start, static_cast<int32_t>(pos - start)))) {
        }
      }
      if (OB_SUCC(ret)) {
        ObJsonString *json_str = nullptr;
        ObString value = field.string();
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(alloc, value, json_str))) {
        } else if (OB_FAIL(fields->append(json_str))) {
        }
      }
      if (OB_SUCC(ret)) {
        ++pos;
      }
    }
    if (OB_SUCC(ret)) {
      array = fields;
    }
  }
  return ret;
}

int ObExprReadCSV::eval_read_csv(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  int ret = OB_SUCCESS;
  ObDatum *path_datum = nullptr;
  ObExecContext &exec_ctx = ctx.exec_ctx_;
  ObExprReadCSVCtx *csv_ctx = nullptr;
  uint64_t op_id = expr.expr_ctx_id_;
  if (OB_FAIL(expr.eval_param_value(ctx, path_datum))) {
  } else if (path_datum->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "read_csv, path is null");
    res.set_null();
  } else if (OB_ISNULL(csv_ctx = static_cast<ObExprReadCSVCtx *>(exec_ctx.get_expr_op_ctx(op_id)))) {
    if (OB_FAIL(exec_ctx.create_expr_op_ctx(op_id, csv_ctx))) {
    } else if (OB_ISNULL(csv_ctx)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (!csv_ctx->loaded_) {
    ObString path = path_datum->get_string();
    ObString secure_file_priv;
    ObSQLSessionInfo *session = ctx.exec_ctx_.get_my_session();
    if (OB_ISNULL(session)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(session->get_secure_file_priv(secure_file_priv))) {
    } else if (OB_FAIL(ObResolverUtils::check_secure_path(secure_file_priv, path))) {
      LOG_WARN("read_csv, path is not allowed by secure_file_priv", K(ret), K(path), K(secure_file_priv));
    } else if (OB_FAIL(read_whole_file(exec_ctx.get_allocator(), path, csv_ctx->content_))) {
    } else if (OB_FAIL(split_lines(exec_ctx.get_allocator(), csv_ctx->content_, csv_ctx->rows_))) {
    } else {
      csv_ctx->loaded_ = true;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (csv_ctx->curr_row_ >= csv_ctx->rows_.count()) {
    ret = OB_ITER_END;
  } else {
    ObArenaAllocator temp_alloc;
    ObJsonArray *fields = nullptr;
    ObString row_line = csv_ctx->rows_.at(csv_ctx->curr_row_);
    if (OB_FAIL(parse_csv_fields(temp_alloc, row_line, fields))) {
    } else {
      ObStringBuffer buf(&exec_ctx.get_allocator());
      if (OB_FAIL(fields->print(buf, 0))) {
      } else {
        ObString row_json = buf.string();
        if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, row_json))) {
        } else {
          ++csv_ctx->curr_row_;
        }
      }
    }
  }
  return ret;
}

int ObExprReadCSV::cg_expr(ObExprCGCtx &expr_cg_ctx,
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const
{
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  int ret = OB_SUCCESS;
  rt_expr.eval_func_ = ObExprReadCSV::eval_read_csv;
  return ret;
}

} // namespace sql
} // namespace oceanbase
