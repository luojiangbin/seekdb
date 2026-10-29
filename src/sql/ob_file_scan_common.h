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

#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "share/ob_define.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{

// Which file table function produced a file table item.
enum class ObFileScanKind
{
  INVALID = 0,
  READ_CSV,
  READ_JSON,
  READ_PARQUET,
  GLOB,
};

// One column of a resolved file table.
struct ObFileTableColumnDef
{
  // Name used by the statement (duplicates get a __N suffix).
  common::ObString column_name_;
  // Name in the file header or JSON object.
  common::ObString source_name_;
  // Origin of the type, for example "CSV/BIGINT" (used by DESCRIBE).
  common::ObString source_type_name_;
  ObFileColumnType type_;
  bool nullable_;
  int64_t max_length_;

  ObFileTableColumnDef()
    : column_name_(), source_name_(), source_type_name_(),
      type_(ObFileColumnType::INVALID), nullable_(false), max_length_(0)
  {}

  TO_STRING_KV(K_(column_name), K_(source_name), K_(source_type_name),
               "type", ObFileScanUtils::type_name(type_), K_(nullable), K_(max_length));
};

// Everything the resolver learns about one file and the scan operator needs to
// reopen it safely: the canonical path, the identity fingerprint taken while the
// schema was inferred, and the inferred columns.
struct ObFileTableDef
{
  ObFileScanKind kind_;
  ObFileFormat format_;
  common::ObString canonical_path_;
  // Set for glob(); the matched directory is canonical_path_.
  common::ObString glob_pattern_;
  // secure_file_priv snapshot taken during resolution.
  common::ObString secure_file_priv_;
  uint64_t device_;
  uint64_t inode_;
  int64_t file_size_;
  int64_t modified_time_ns_;
  // Number of data rows seen while inferring (glob(): number of matched files).
  int64_t rows_;
  common::ObSEArray<ObFileTableColumnDef, 8> columns_;
  // Inferred columns in file order, including ones not projected by the query.
  common::ObSEArray<ObFileTableColumnDef, 8> source_columns_;

  ObFileTableDef()
    : kind_(ObFileScanKind::INVALID), format_(ObFileFormat::INVALID),
      canonical_path_(), glob_pattern_(), secure_file_priv_(),
      device_(0), inode_(0), file_size_(0), modified_time_ns_(0), rows_(0),
      columns_(), source_columns_()
  {}

  void reset()
  {
    kind_ = ObFileScanKind::INVALID;
    format_ = ObFileFormat::INVALID;
    canonical_path_.reset();
    glob_pattern_.reset();
    secure_file_priv_.reset();
    device_ = 0;
    inode_ = 0;
    file_size_ = 0;
    modified_time_ns_ = 0;
    rows_ = 0;
    columns_.reset();
    source_columns_.reset();
  }

  bool is_valid() const
  {
    return ObFileScanKind::INVALID != kind_ && !canonical_path_.empty();
  }

  TO_STRING_KV("kind", static_cast<int64_t>(kind_),
               "format", ObFileScanUtils::format_name(format_),
               K_(canonical_path), K_(glob_pattern), K_(secure_file_priv),
               K_(device), K_(inode), K_(file_size), K_(modified_time_ns),
               K_(rows), K_(columns), K_(source_columns));
};

}  // namespace sql
}  // namespace oceanbase
