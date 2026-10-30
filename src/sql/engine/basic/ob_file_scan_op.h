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

#pragma once

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{

// Physical plan data of one file table scan.
class ObFileScanSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObFileScanSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
      : ObOpSpec(alloc, type),
        file_path_(),
        format_(0),
        device_(0),
        inode_(0),
        file_size_(0),
        modified_time_ns_(0),
        column_types_(alloc),
        source_names_(alloc) {}

  // Canonical path of the file, taken while the schema was inferred.
  ObString file_path_;
  int32_t format_;
  uint64_t device_;
  uint64_t inode_;
  int64_t file_size_;
  int64_t modified_time_ns_;
  // ObFileColumnType of each output column, in output order.
  common::ObFixedArray<int32_t, common::ObIAllocator> column_types_;
  // Name of each column in the file (CSV header / JSON key).
  common::ObFixedArray<ObString, common::ObIAllocator> source_names_;

};

// Reads the rows of one file and writes them into the output expressions.
class ObFileScanOp : public ObOperator
{
public:
  ObFileScanOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
      : ObOperator(exec_ctx, spec, input),
        reader_(),
        schema_(),
        opened_(false) {}
  virtual ~ObFileScanOp() {}

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_close() override;
  virtual void destroy() override;

private:
  int build_schema();
  int fill_row(const std::vector<ObFileCell> &cells);

private:
  ObFileScanReader reader_;
  std::vector<ObFileColumnSchema> schema_;
  bool opened_;

  DISALLOW_COPY_AND_ASSIGN(ObFileScanOp);
};

}  // namespace sql
}  // namespace oceanbase
