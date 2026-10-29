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

#include <cstdint>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{

// Local file formats that can be read as tables.
enum class ObFileFormat
{
  INVALID = 0,
  AUTO,
  CSV,
  JSONL,
  PARQUET,
};

// Column types produced by the deterministic file schema inference.
enum class ObFileColumnType
{
  INVALID = 0,
  BIGINT,
  DOUBLE,
  BOOLEAN,
  DATE,
  DATETIME,
  VARCHAR,
};

struct ObFileColumnSchema
{
  // Name used in the resolved table (duplicates get a __N suffix).
  std::string column_name_;
  // Name as it appears in the file header / JSON object.
  std::string source_name_;
  ObFileColumnType type_ = ObFileColumnType::INVALID;
  // Human readable origin of the type, e.g. "CSV/BIGINT".
  std::string source_type_name_;
  bool nullable_ = false;
};

// One value of one row. Only the member matching the column type is meaningful.
struct ObFileCell
{
  bool is_null_ = true;
  int64_t int_value_ = 0;
  double double_value_ = 0.0;
  bool bool_value_ = false;
  std::string string_value_;
};

// Streams the rows of one already inferred file. The reader refuses to run when
// the file changed since the schema was inferred (size, mtime or identity).
class ObFileScanReader
{
public:
  ObFileScanReader() = default;
  ~ObFileScanReader() = default;

  int open(const std::string &path,
           const ObFileFormat format,
           const std::vector<ObFileColumnSchema> &columns,
           const uint64_t device,
           const uint64_t inode,
           const int64_t file_size,
           const int64_t modified_time_ns);

  int open(const std::string &path,
           const ObFileFormat format,
           const std::vector<ObFileColumnSchema> &columns,
           const int64_t file_size,
           const int64_t modified_time_ns);

  // OB_SUCCESS with the next row in cells, OB_ITER_END at EOF, OB_SCHEMA_EAGAIN
  // when the file changed while it was being scanned.
  int get_next_row(std::vector<ObFileCell> &cells);

  void reset();

private:
  int check_unchanged() const;

private:
  bool opened_ = false;
  ObFileFormat format_ = ObFileFormat::INVALID;
  std::string path_;
  std::vector<ObFileColumnSchema> columns_;
  // Rows are decoded up front; the accessor above still yields them one by one.
  std::vector<std::vector<ObFileCell>> rows_;
  int64_t next_row_ = 0;
  uint64_t device_ = 0;
  uint64_t inode_ = 0;
  int64_t file_size_ = 0;
  int64_t modified_time_ns_ = 0;
};

// Schema inference and file discovery helpers shared by the file scan operator.
class ObFileScanUtils
{
public:
  // Infers the columns of path. requested may be AUTO to detect by extension.
  static int infer_schema(const std::string &path,
                          const ObFileFormat requested,
                          std::vector<ObFileColumnSchema> &columns,
                          int64_t &rows,
                          std::string &canonical_path,
                          ObFileFormat &format,
                          uint64_t &device,
                          uint64_t &inode,
                          int64_t &file_size,
                          int64_t &modified_time_ns);

  static int infer_schema(const std::string &path,
                          const ObFileFormat requested,
                          std::vector<ObFileColumnSchema> &columns,
                          int64_t &rows,
                          std::string &canonical_path,
                          ObFileFormat &format,
                          int64_t &file_size,
                          int64_t &modified_time_ns);

  static int get_directory_fingerprint(const std::string &path,
                                       std::string &canonical_path,
                                       uint64_t &device,
                                       uint64_t &inode,
                                       int64_t &modified_time_ns);

  // Detects the format from the file extension, INVALID when unknown.
  static ObFileFormat detect_format(const std::string &path);
  static const char *format_name(const ObFileFormat format);
  static const char *type_name(const ObFileColumnType type);
};

}  // namespace sql
}  // namespace oceanbase
