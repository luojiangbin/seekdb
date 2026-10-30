/*
 * Copyright (c) 2026 OceanBase.
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
#include "sql/engine/basic/ob_file_scan_op.h"
#include "sql/engine/ob_exec_context.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{

OB_SERIALIZE_MEMBER((ObFileScanSpec, ObOpSpec), file_path_, format_, device_, inode_,
                    file_size_, modified_time_ns_, column_types_, source_names_);

int ObFileScanOp::build_schema()
{
  int ret = OB_SUCCESS;
  const ObFileScanSpec &spec = static_cast<const ObFileScanSpec &>(spec_);
  schema_.clear();
  if (spec.column_types_.count() != spec.output_.count()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file scan column count mismatch", K(ret),
             K(spec.column_types_.count()), K(spec.output_.count()));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < spec.column_types_.count(); ++i) {
      ObFileColumnSchema column;
      column.type_ = static_cast<ObFileColumnType>(spec.column_types_.at(i));
      column.nullable_ = true;
      const ObString &name = spec.source_names_.at(i);
      column.column_name_ = std::string(name.ptr(), name.length());
      column.source_name_ = column.column_name_;
      schema_.push_back(column);
    }
  }
  return ret;
}

int ObFileScanOp::inner_open()
{
  int ret = OB_SUCCESS;
  const ObFileScanSpec &spec = static_cast<const ObFileScanSpec &>(spec_);
  opened_ = false;
  if (OB_FAIL(build_schema())) {
  } else {
    const std::string path(spec.file_path_.ptr(), spec.file_path_.length());
    const int iret = reader_.open(path, static_cast<ObFileFormat>(spec.format_), schema_,
                                  spec.device_, spec.inode_, spec.file_size_,
                                  spec.modified_time_ns_);
    if (OB_SUCCESS != iret) {
      ret = iret;
      LOG_WARN("failed to open the file", K(ret), K(spec.file_path_));
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file changed since the schema was inferred");
    } else {
      opened_ = true;
    }
  }
  return ret;
}

int ObFileScanOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
    LOG_WARN("failed to rescan the operator", K(ret));
  } else if (OB_FAIL(inner_open())) {
    LOG_WARN("failed to reopen the file", K(ret));
  }
  return ret;
}

int ObFileScanOp::fill_row(const std::vector<ObFileCell> &cells)
{
  int ret = OB_SUCCESS;
  const ObFileScanSpec &spec = static_cast<const ObFileScanSpec &>(spec_);
  if (cells.size() != static_cast<size_t>(spec.output_.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected file row width", K(ret), K(cells.size()), K(spec.output_.count()));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < spec.output_.count(); ++i) {
      ObExpr *expr = spec.output_.at(i);
      ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
      const ObFileCell &cell = cells.at(i);
      if (cell.is_null_) {
        datum.set_null();
      } else {
        switch (expr->datum_meta_.type_) {
          case ObIntType:
          case ObTinyIntType:
            datum.set_int(cell.int_value_);
            break;
          case ObDoubleType:
            datum.set_double(cell.double_value_);
            break;
          case ObDateType:
          case ObDateTimeType:
          case ObVarcharType:
          default:
            datum.set_string(cell.string_value_.c_str(),
                             static_cast<int32_t>(cell.string_value_.size()));
            break;
        }
      }
      expr->set_evaluated_projected(eval_ctx_);
    }
  }
  return ret;
}

int ObFileScanOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  cur_cells_.clear();
  const int iret = reader_.get_next_row(cur_cells_);
  if (OB_ITER_END == iret) {
    ret = OB_ITER_END;
  } else if (OB_SUCCESS != iret) {
    ret = iret;
    LOG_WARN("failed to read the next file row", K(ret));
  } else if (OB_FAIL(fill_row(cur_cells_))) {
    LOG_WARN("failed to fill the output row", K(ret));
  }
  return ret;
}

int ObFileScanOp::inner_close()
{
  int ret = OB_SUCCESS;
  reader_.reset();
  opened_ = false;
  if (OB_FAIL(ObOperator::inner_close())) {
    LOG_WARN("failed to close the operator", K(ret));
  }
  return ret;
}

void ObFileScanOp::destroy()
{
  reader_.reset();
  ObOperator::destroy();
}

}  // namespace sql
}  // namespace oceanbase
