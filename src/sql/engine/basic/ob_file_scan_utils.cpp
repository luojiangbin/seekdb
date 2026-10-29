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

#include "sql/engine/basic/ob_file_scan_utils.h"

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

#include "share/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"

namespace oceanbase
{
namespace sql
{

namespace
{

const char *const CSV_SUFFIX = ".csv";
const char *const JSONL_SUFFIX = ".jsonl";
const char *const NDJSON_SUFFIX = ".ndjson";
const char *const PARQUET_SUFFIX = ".parquet";

bool ends_with(const std::string &value, const char *suffix)
{
  const size_t suffix_len = strlen(suffix);
  return value.size() >= suffix_len
      && 0 == value.compare(value.size() - suffix_len, suffix_len, suffix);
}

int read_whole_file(const std::string &path, std::string &content)
{
  int ret = OB_SUCCESS;
  FILE *file = fopen(path.c_str(), "rb");
  if (nullptr == file) {
    ret = OB_IO_ERROR;
  } else {
    content.clear();
    char buffer[65536];
    size_t read_bytes = 0;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
      content.append(buffer, read_bytes);
    }
    if (ferror(file)) {
      ret = OB_IO_ERROR;
    }
    fclose(file);
  }
  return ret;
}

bool is_blank(const std::string &value)
{
  for (char ch : value) {
    if (' ' != ch && '\t' != ch && '\r' != ch && '\n' != ch) {
      return false;
    }
  }
  return true;
}

std::string trim(const std::string &value)
{
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && (' ' == value[begin] || '\t' == value[begin])) {
    ++begin;
  }
  while (end > begin && (' ' == value[end - 1] || '\t' == value[end - 1])) {
    --end;
  }
  return value.substr(begin, end - begin);
}

bool parse_int64(const std::string &text, int64_t &value)
{
  if (text.empty()) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const long long parsed = strtoll(text.c_str(), &end, 10);
  if (0 != errno || nullptr == end || '\0' != *end) {
    return false;
  }
  value = static_cast<int64_t>(parsed);
  return true;
}

bool parse_double(const std::string &text, double &value)
{
  if (text.empty()) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const double parsed = strtod(text.c_str(), &end);
  if (0 != errno || nullptr == end || '\0' != *end || std::isinf(parsed) || std::isnan(parsed)) {
    return false;
  }
  value = parsed;
  return true;
}

bool parse_bool(const std::string &text, bool &value)
{
  if (0 == strcasecmp(text.c_str(), "true") || 0 == strcasecmp(text.c_str(), "t")) {
    value = true;
    return true;
  }
  if (0 == strcasecmp(text.c_str(), "false") || 0 == strcasecmp(text.c_str(), "f")) {
    value = false;
    return true;
  }
  return false;
}

bool is_digits(const std::string &text, const size_t begin, const size_t count)
{
  if (begin + count > text.size()) {
    return false;
  }
  for (size_t i = begin; i < begin + count; ++i) {
    if (text[i] < '0' || text[i] > '9') {
      return false;
    }
  }
  return true;
}

// YYYY-MM-DD
bool parse_date(const std::string &text)
{
  if (text.size() != 10 || '-' != text[4] || '-' != text[7]) {
    return false;
  }
  if (!is_digits(text, 0, 4) || !is_digits(text, 5, 2) || !is_digits(text, 8, 2)) {
    return false;
  }
  const int month = (text[5] - '0') * 10 + (text[6] - '0');
  const int day = (text[8] - '0') * 10 + (text[9] - '0');
  return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

// YYYY-MM-DD HH:MM:SS with an optional fractional part.
bool parse_datetime(const std::string &text)
{
  if (text.size() < 19 || (' ' != text[10] && 'T' != text[10])) {
    return false;
  }
  if (!parse_date(text.substr(0, 10))) {
    return false;
  }
  if (':' != text[13] || ':' != text[16]) {
    return false;
  }
  if (!is_digits(text, 11, 2) || !is_digits(text, 14, 2) || !is_digits(text, 17, 2)) {
    return false;
  }
  const int hour = (text[11] - '0') * 10 + (text[12] - '0');
  const int minute = (text[14] - '0') * 10 + (text[15] - '0');
  const int second = (text[17] - '0') * 10 + (text[18] - '0');
  if (hour > 23 || minute > 59 || second > 59) {
    return false;
  }
  if (text.size() > 19) {
    if ('.' != text[19]) {
      return false;
    }
    if (text.size() == 20 || !is_digits(text, 20, text.size() - 20)) {
      return false;
    }
  }
  return true;
}

// Deterministic type of one CSV field.
ObFileColumnType infer_field_type(const std::string &raw, bool &is_null)
{
  const std::string text = trim(raw);
  is_null = text.empty();
  if (is_null) {
    return ObFileColumnType::INVALID;
  }
  int64_t int_value = 0;
  if (parse_int64(text, int_value)) {
    return ObFileColumnType::BIGINT;
  }
  double double_value = 0.0;
  if (parse_double(text, double_value)) {
    return ObFileColumnType::DOUBLE;
  }
  bool bool_value = false;
  if (parse_bool(text, bool_value)) {
    return ObFileColumnType::BOOLEAN;
  }
  if (parse_datetime(text)) {
    return ObFileColumnType::DATETIME;
  }
  if (parse_date(text)) {
    return ObFileColumnType::DATE;
  }
  return ObFileColumnType::VARCHAR;
}

// Widens two inferred types into the type that can hold both.
ObFileColumnType merge_type(const ObFileColumnType left, const ObFileColumnType right)
{
  if (ObFileColumnType::INVALID == left) {
    return right;
  }
  if (ObFileColumnType::INVALID == right) {
    return left;
  }
  if (left == right) {
    return left;
  }
  const bool left_numeric = ObFileColumnType::BIGINT == left || ObFileColumnType::DOUBLE == left;
  const bool right_numeric = ObFileColumnType::BIGINT == right || ObFileColumnType::DOUBLE == right;
  if (left_numeric && right_numeric) {
    return ObFileColumnType::DOUBLE;
  }
  // Anything that is not consistently one scalar class is kept as text.
  return ObFileColumnType::VARCHAR;
}

// Splits one CSV record. Handles quotes, doubled quotes and embedded newlines.
// Returns the offset after the record (including the line terminator).
size_t parse_csv_record(const std::string &content, const size_t begin,
                        std::vector<std::string> &fields, bool &complete)
{
  fields.clear();
  complete = false;
  std::string field;
  bool in_quotes = false;
  size_t pos = begin;
  while (pos < content.size()) {
    const char ch = content[pos];
    if (in_quotes) {
      if ('"' == ch) {
        if (pos + 1 < content.size() && '"' == content[pos + 1]) {
          field.push_back('"');
          pos += 2;
        } else {
          in_quotes = false;
          ++pos;
        }
      } else {
        field.push_back(ch);
        ++pos;
      }
    } else if ('"' == ch) {
      in_quotes = true;
      ++pos;
    } else if (',' == ch) {
      fields.push_back(field);
      field.clear();
      ++pos;
    } else if ('\n' == ch || '\r' == ch) {
      if ('\r' == ch && pos + 1 < content.size() && '\n' == content[pos + 1]) {
        ++pos;
      }
      ++pos;
      fields.push_back(field);
      complete = true;
      return pos;
    } else {
      field.push_back(ch);
      ++pos;
    }
  }
  fields.push_back(field);
  complete = !in_quotes;
  return pos;
}

std::string build_column_name(const std::string &source_name, const int64_t duplicate_index)
{
  if (duplicate_index <= 1) {
    return source_name;
  }
  return source_name + "__" + std::to_string(duplicate_index);
}

int stat_path(const std::string &path, uint64_t &device, uint64_t &inode,
              int64_t &file_size, int64_t &modified_time_ns)
{
  int ret = OB_SUCCESS;
  struct stat file_stat;
  if (0 != stat(path.c_str(), &file_stat)) {
    ret = OB_IO_ERROR;
  } else {
    device = static_cast<uint64_t>(file_stat.st_dev);
    inode = static_cast<uint64_t>(file_stat.st_ino);
    file_size = static_cast<int64_t>(file_stat.st_size);
#if defined(__APPLE__)
    modified_time_ns = static_cast<int64_t>(file_stat.st_mtimespec.tv_sec) * 1000000000L
        + static_cast<int64_t>(file_stat.st_mtimespec.tv_nsec);
#else
    modified_time_ns = static_cast<int64_t>(file_stat.st_mtim.tv_sec) * 1000000000L
        + static_cast<int64_t>(file_stat.st_mtim.tv_nsec);
#endif
  }
  return ret;
}

int make_canonical_path(const std::string &path, std::string &canonical_path)
{
  int ret = OB_SUCCESS;
  char resolved[PATH_MAX > 0 ? PATH_MAX : 4096] = {'\0'};
  if (nullptr == realpath(path.c_str(), resolved)) {
    ret = OB_IO_ERROR;
  } else {
    canonical_path.assign(resolved);
  }
  return ret;
}

// Builds a reader row for one CSV record according to the inferred schema.
int build_csv_row(const std::vector<std::string> &fields,
                  const std::vector<ObFileColumnSchema> &columns,
                  std::vector<ObFileCell> &cells)
{
  int ret = OB_SUCCESS;
  cells.clear();
  cells.resize(columns.size());
  for (size_t i = 0; i < columns.size() && OB_SUCC(ret); ++i) {
    ObFileCell &cell = cells[i];
    if (i >= fields.size() || is_blank(fields[i])) {
      cell.is_null_ = true;
    } else {
      cell.is_null_ = false;
      const std::string text = trim(fields[i]);
      switch (columns[i].type_) {
        case ObFileColumnType::BIGINT: {
          int64_t value = 0;
          if (!parse_int64(text, value)) {
            ret = OB_INVALID_DATA;
          } else {
            cell.int_value_ = value;
          }
          break;
        }
        case ObFileColumnType::DOUBLE: {
          double value = 0.0;
          if (!parse_double(text, value)) {
            ret = OB_INVALID_DATA;
          } else {
            cell.double_value_ = value;
          }
          break;
        }
        case ObFileColumnType::BOOLEAN: {
          bool value = false;
          if (!parse_bool(text, value)) {
            ret = OB_INVALID_DATA;
          } else {
            cell.bool_value_ = value;
          }
          break;
        }
        case ObFileColumnType::DATE:
        case ObFileColumnType::DATETIME:
        case ObFileColumnType::VARCHAR:
        default:
          cell.string_value_ = text;
          break;
      }
    }
  }
  return ret;
}

// Minimal flat-object JSON reader used for newline delimited JSON. Only the
// shapes required by the file SQL contract are accepted.
struct JsonlValue
{
  enum Kind { NUL, BOOL, INT, DOUBLE, STRING } kind_ = NUL;
  bool bool_value_ = false;
  int64_t int_value_ = 0;
  double double_value_ = 0.0;
  std::string string_value_;
};

void skip_space(const std::string &text, size_t &pos)
{
  while (pos < text.size() && (' ' == text[pos] || '\t' == text[pos])) {
    ++pos;
  }
}

bool parse_json_string(const std::string &text, size_t &pos, std::string &value)
{
  if (pos >= text.size() || '"' != text[pos]) {
    return false;
  }
  ++pos;
  value.clear();
  while (pos < text.size()) {
    const char ch = text[pos];
    if ('"' == ch) {
      ++pos;
      return true;
    }
    if ('\\' == ch) {
      ++pos;
      if (pos >= text.size()) {
        return false;
      }
      const char escaped = text[pos];
      switch (escaped) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'u': {
          if (pos + 4 >= text.size()) {
            return false;
          }
          unsigned code = 0;
          for (int i = 1; i <= 4; ++i) {
            const char digit = text[pos + i];
            code <<= 4;
            if (digit >= '0' && digit <= '9') {
              code |= static_cast<unsigned>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
              code |= static_cast<unsigned>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
              code |= static_cast<unsigned>(digit - 'A' + 10);
            } else {
              return false;
            }
          }
          pos += 4;
          // Encode the code point as UTF-8 (BMP only, which is enough here).
          if (code < 0x80) {
            value.push_back(static_cast<char>(code));
          } else if (code < 0x800) {
            value.push_back(static_cast<char>(0xC0 | (code >> 6)));
            value.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          } else {
            value.push_back(static_cast<char>(0xE0 | (code >> 12)));
            value.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            value.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          }
          break;
        }
        default:
          return false;
      }
      ++pos;
    } else {
      value.push_back(ch);
      ++pos;
    }
  }
  return false;
}

// Parses {"key":value,...} with scalar values only. Nested objects and arrays
// are rejected with OB_NOT_SUPPORTED.
int parse_flat_json_object(const std::string &line, std::vector<std::pair<std::string, JsonlValue>> &values)
{
  int ret = OB_SUCCESS;
  values.clear();
  size_t pos = 0;
  skip_space(line, pos);
  if (pos >= line.size() || '{' != line[pos]) {
    return OB_INVALID_DATA;
  }
  ++pos;
  skip_space(line, pos);
  if (pos < line.size() && '}' == line[pos]) {
    return OB_SUCCESS;
  }
  while (pos < line.size() && OB_SUCC(ret)) {
    skip_space(line, pos);
    std::string key;
    if (!parse_json_string(line, pos, key)) {
      ret = OB_INVALID_DATA;
    } else {
      skip_space(line, pos);
      if (pos >= line.size() || ':' != line[pos]) {
        ret = OB_INVALID_DATA;
      } else {
        ++pos;
        skip_space(line, pos);
        JsonlValue value;
        if (pos >= line.size()) {
          ret = OB_INVALID_DATA;
        } else if ('{' == line[pos] || '[' == line[pos]) {
          ret = OB_NOT_SUPPORTED;
        } else if ('"' == line[pos]) {
          value.kind_ = JsonlValue::STRING;
          if (!parse_json_string(line, pos, value.string_value_)) {
            ret = OB_INVALID_DATA;
          }
        } else if (0 == line.compare(pos, 4, "true")) {
          value.kind_ = JsonlValue::BOOL;
          value.bool_value_ = true;
          pos += 4;
        } else if (0 == line.compare(pos, 5, "false")) {
          value.kind_ = JsonlValue::BOOL;
          value.bool_value_ = false;
          pos += 5;
        } else if (0 == line.compare(pos, 4, "null")) {
          value.kind_ = JsonlValue::NUL;
          pos += 4;
        } else {
          size_t end = pos;
          while (end < line.size() && ',' != line[end] && '}' != line[end]
                 && ' ' != line[end] && '\t' != line[end]) {
            ++end;
          }
          const std::string number = line.substr(pos, end - pos);
          int64_t int_value = 0;
          double double_value = 0.0;
          if (parse_int64(number, int_value)) {
            value.kind_ = JsonlValue::INT;
            value.int_value_ = int_value;
          } else if (parse_double(number, double_value)) {
            value.kind_ = JsonlValue::DOUBLE;
            value.double_value_ = double_value;
          } else {
            ret = OB_INVALID_DATA;
          }
          pos = end;
        }
        if (OB_SUCC(ret)) {
          values.emplace_back(key, value);
          skip_space(line, pos);
          if (pos < line.size() && ',' == line[pos]) {
            ++pos;
          } else if (pos < line.size() && '}' == line[pos]) {
            ++pos;
            // Trailing content is not expected.
            skip_space(line, pos);
            if (pos != line.size()) {
              ret = OB_INVALID_DATA;
            }
            break;
          } else {
            ret = OB_INVALID_DATA;
          }
        }
      }
    }
  }
  return ret;
}

// Type of one JSONL scalar as it appears in the union schema.
ObFileColumnType infer_jsonl_type(const JsonlValue &value, bool &is_null)
{
  switch (value.kind_) {
    case JsonlValue::NUL:
      is_null = true;
      return ObFileColumnType::INVALID;
    case JsonlValue::BOOL:
      return ObFileColumnType::BOOLEAN;
    case JsonlValue::INT:
      return ObFileColumnType::BIGINT;
    case JsonlValue::DOUBLE:
      return ObFileColumnType::DOUBLE;
    case JsonlValue::STRING:
    default: {
      // Strings keep their narrowing: date, datetime, number or plain text.
      bool field_null = false;
      return infer_field_type(value.string_value_, field_null);
    }
  }
}

// Fills one cell from a JSONL scalar using the type of the inferred column, so a
// column that was widened to DOUBLE also exposes integers through double_value_.
void fill_jsonl_cell(ObFileCell &cell, const ObFileColumnType column_type, const JsonlValue &value)
{
  cell = ObFileCell();
  switch (value.kind_) {
    case JsonlValue::NUL:
      cell.is_null_ = true;
      break;
    case JsonlValue::BOOL:
      cell.is_null_ = false;
      cell.bool_value_ = value.bool_value_;
      if (ObFileColumnType::VARCHAR == column_type) {
        cell.string_value_ = value.bool_value_ ? "true" : "false";
      }
      break;
    case JsonlValue::INT:
      cell.is_null_ = false;
      cell.int_value_ = value.int_value_;
      cell.double_value_ = static_cast<double>(value.int_value_);
      if (ObFileColumnType::VARCHAR == column_type) {
        cell.string_value_ = std::to_string(value.int_value_);
      }
      break;
    case JsonlValue::DOUBLE:
      cell.is_null_ = false;
      cell.double_value_ = value.double_value_;
      cell.int_value_ = static_cast<int64_t>(value.double_value_);
      if (ObFileColumnType::VARCHAR == column_type) {
        cell.string_value_ = std::to_string(value.double_value_);
      }
      break;
    case JsonlValue::STRING:
    default:
      cell.is_null_ = false;
      cell.string_value_ = value.string_value_;
      if (ObFileColumnType::BIGINT == column_type) {
        int64_t int_value = 0;
        if (parse_int64(value.string_value_, int_value)) {
          cell.int_value_ = int_value;
        }
      } else if (ObFileColumnType::DOUBLE == column_type) {
        double double_value = 0.0;
        if (parse_double(value.string_value_, double_value)) {
          cell.double_value_ = double_value;
        }
      } else if (ObFileColumnType::BOOLEAN == column_type) {
        bool bool_value = false;
        if (parse_bool(value.string_value_, bool_value)) {
          cell.bool_value_ = bool_value;
        }
      }
      break;
  }
}

}  // namespace

ObFileFormat ObFileScanUtils::detect_format(const std::string &path)
{
  ObFileFormat format = ObFileFormat::INVALID;
  if (ends_with(path, CSV_SUFFIX)) {
    format = ObFileFormat::CSV;
  } else if (ends_with(path, JSONL_SUFFIX) || ends_with(path, NDJSON_SUFFIX)) {
    format = ObFileFormat::JSONL;
  } else if (ends_with(path, PARQUET_SUFFIX)) {
    format = ObFileFormat::PARQUET;
  }
  return format;
}

const char *ObFileScanUtils::format_name(const ObFileFormat format)
{
  const char *name = "INVALID";
  switch (format) {
    case ObFileFormat::AUTO: name = "AUTO"; break;
    case ObFileFormat::CSV: name = "CSV"; break;
    case ObFileFormat::JSONL: name = "JSONL"; break;
    case ObFileFormat::PARQUET: name = "PARQUET"; break;
    default: break;
  }
  return name;
}

const char *ObFileScanUtils::type_name(const ObFileColumnType type)
{
  const char *name = "NULL";
  switch (type) {
    case ObFileColumnType::BIGINT: name = "BIGINT"; break;
    case ObFileColumnType::DOUBLE: name = "DOUBLE"; break;
    case ObFileColumnType::BOOLEAN: name = "BOOLEAN"; break;
    case ObFileColumnType::DATE: name = "DATE"; break;
    case ObFileColumnType::DATETIME: name = "DATETIME"; break;
    case ObFileColumnType::VARCHAR: name = "VARCHAR"; break;
    default: break;
  }
  return name;
}

int ObFileScanUtils::get_directory_fingerprint(const std::string &path,
                                               std::string &canonical_path,
                                               uint64_t &device,
                                               uint64_t &inode,
                                               int64_t &modified_time_ns)
{
  int ret = OB_SUCCESS;
  int64_t file_size = 0;
  if (OB_FAIL(make_canonical_path(path, canonical_path))) {
  } else if (OB_FAIL(stat_path(canonical_path, device, inode, file_size, modified_time_ns))) {
  }
  return ret;
}

int ObFileScanUtils::infer_schema(const std::string &path,
                                  const ObFileFormat requested,
                                  std::vector<ObFileColumnSchema> &columns,
                                  int64_t &rows,
                                  std::string &canonical_path,
                                  ObFileFormat &format,
                                  uint64_t &device,
                                  uint64_t &inode,
                                  int64_t &file_size,
                                  int64_t &modified_time_ns)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(make_canonical_path(path, canonical_path))) {
  } else if (OB_FAIL(stat_path(canonical_path, device, inode, file_size, modified_time_ns))) {
  } else if (OB_FAIL(infer_schema(canonical_path, requested, columns, rows, canonical_path,
                                  format, file_size, modified_time_ns))) {
  }
  return ret;
}

int ObFileScanUtils::infer_schema(const std::string &path,
                                  const ObFileFormat requested,
                                  std::vector<ObFileColumnSchema> &columns,
                                  int64_t &rows,
                                  std::string &canonical_path,
                                  ObFileFormat &format,
                                  int64_t &file_size,
                                  int64_t &modified_time_ns)
{
  int ret = OB_SUCCESS;
  std::string content;
  uint64_t device = 0;
  uint64_t inode = 0;
  columns.clear();
  rows = 0;
  if (path.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObFileFormat::PARQUET == requested) {
    // The Parquet reader is not part of this build yet.
    ret = OB_NOT_SUPPORTED;
  } else {
    format = requested;
    if (ObFileFormat::AUTO == format) {
      format = detect_format(path);
    }
    if (ObFileFormat::CSV != format && ObFileFormat::JSONL != format) {
      ret = OB_NOT_SUPPORTED;
    } else if (OB_FAIL(make_canonical_path(path, canonical_path))) {
    } else if (OB_FAIL(stat_path(canonical_path, device, inode, file_size, modified_time_ns))) {
    } else if (OB_FAIL(read_whole_file(canonical_path, content))) {
    } else if (ObFileFormat::CSV == format) {
      // ---- CSV: header + deterministic per column type inference ----
      std::vector<std::string> header_fields;
      std::vector<std::string> record_fields;
      std::vector<std::vector<std::string>> data_records;
      bool complete = false;
      size_t pos = 0;
      // Skip a UTF-8 BOM when present.
      if (content.size() >= 3 && '\xEF' == content[0] && '\xBB' == content[1]
          && '\xBF' == content[2]) {
        pos = 3;
      }
      pos = parse_csv_record(content, pos, header_fields, complete);
      if (!complete || header_fields.empty()) {
        ret = OB_INVALID_DATA;
      }
      while (OB_SUCC(ret) && pos < content.size()) {
        const size_t next = parse_csv_record(content, pos, record_fields, complete);
        if (!complete) {
          ret = OB_INVALID_DATA;
        } else if (record_fields.size() == 1 && is_blank(record_fields[0])) {
          // Skip blank lines.
        } else {
          data_records.push_back(record_fields);
        }
        pos = next;
      }
      if (OB_SUCC(ret)) {
        rows = static_cast<int64_t>(data_records.size());
        std::map<std::string, int64_t> name_counter;
        for (size_t i = 0; i < header_fields.size() && OB_SUCC(ret); ++i) {
          ObFileColumnSchema schema;
          schema.source_name_ = trim(header_fields[i]);
          if (schema.source_name_.empty()) {
            schema.source_name_ = "column" + std::to_string(i);
          }
          const int64_t seen = ++name_counter[schema.source_name_];
          schema.column_name_ = build_column_name(schema.source_name_, seen);
          schema.nullable_ = false;
          ObFileColumnType merged = ObFileColumnType::INVALID;
          for (size_t r = 0; r < data_records.size(); ++r) {
            std::string raw;
            if (i < data_records[r].size()) {
              raw = data_records[r][i];
            }
            bool field_null = false;
            const ObFileColumnType field_type = infer_field_type(raw, field_null);
            if (field_null) {
              schema.nullable_ = true;
            } else {
              merged = merge_type(merged, field_type);
            }
          }
          // A column that never had a value stays VARCHAR so it can be read.
          schema.type_ = ObFileColumnType::INVALID == merged
              ? ObFileColumnType::VARCHAR : merged;
          schema.source_type_name_ = std::string("CSV/") + type_name(schema.type_);
          columns.push_back(schema);
        }
      }
    } else {
      // ---- JSONL: union of the flat keys of every line ----
      size_t pos = 0;
      int64_t line_count = 0;
      std::map<std::string, size_t> column_index;
      while (OB_SUCC(ret) && pos <= content.size()) {
        size_t line_end = content.find('\n', pos);
        if (std::string::npos == line_end) {
          line_end = content.size();
        }
        std::string line = content.substr(pos, line_end - pos);
        pos = line_end + 1;
        if (!line.empty() && '\r' == line[line.size() - 1]) {
          line.pop_back();
        }
        if (is_blank(line)) {
          continue;
        }
        std::vector<std::pair<std::string, JsonlValue>> values;
        if (OB_FAIL(parse_flat_json_object(line, values))) {
        } else {
          ++line_count;
          for (const auto &entry : values) {
            auto iter = column_index.find(entry.first);
            bool field_null = false;
            const ObFileColumnType field_type = infer_jsonl_type(entry.second, field_null);
            if (column_index.end() == iter) {
              ObFileColumnSchema schema;
              schema.source_name_ = entry.first;
              schema.column_name_ = entry.first;
              schema.type_ = ObFileColumnType::INVALID == field_type
                  ? ObFileColumnType::VARCHAR : field_type;
              schema.nullable_ = field_null;
              schema.source_type_name_ = std::string("JSONL/")
                  + (ObFileColumnType::INVALID == field_type ? "NULL" : type_name(field_type));
              column_index[entry.first] = columns.size();
              columns.push_back(schema);
            } else {
              ObFileColumnSchema &schema = columns[iter->second];
              if (field_null) {
                schema.nullable_ = true;
              } else {
                schema.type_ = merge_type(schema.type_, field_type);
                schema.source_type_name_ = std::string("JSONL/") + type_name(schema.type_);
              }
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        rows = line_count;
        // Columns that do not appear in every line are nullable.
        for (auto &schema : columns) {
          schema.nullable_ = schema.nullable_ || rows > 1;
        }
      }
    }
  }
  return ret;
}

int ObFileScanReader::check_unchanged() const
{
  int ret = OB_SUCCESS;
  uint64_t device = 0;
  uint64_t inode = 0;
  int64_t file_size = 0;
  int64_t modified_time_ns = 0;
  if (!opened_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(stat_path(path_, device, inode, file_size, modified_time_ns))) {
  } else if (file_size != file_size_
             || modified_time_ns != modified_time_ns_
             || (0 != inode_ && inode != inode_)) {
    ret = OB_SCHEMA_EAGAIN;
  }
  return ret;
}

void ObFileScanReader::reset()
{
  opened_ = false;
  format_ = ObFileFormat::INVALID;
  path_.clear();
  columns_.clear();
  rows_.clear();
  next_row_ = 0;
  device_ = 0;
  inode_ = 0;
  file_size_ = 0;
  modified_time_ns_ = 0;
}

int ObFileScanReader::open(const std::string &path,
                           const ObFileFormat format,
                           const std::vector<ObFileColumnSchema> &columns,
                           const uint64_t device,
                           const uint64_t inode,
                           const int64_t file_size,
                           const int64_t modified_time_ns)
{
  int ret = OB_SUCCESS;
  reset();
  uint64_t current_device = 0;
  uint64_t current_inode = 0;
  int64_t current_size = 0;
  int64_t current_mtime = 0;
  if (path.empty() || columns.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(stat_path(path, current_device, current_inode, current_size, current_mtime))) {
  } else if (current_size != file_size || current_mtime != modified_time_ns
             || (0 != inode && current_inode != inode)) {
    ret = OB_SCHEMA_EAGAIN;
  } else {
    std::string content;
    if (OB_FAIL(read_whole_file(path, content))) {
    } else {
      std::vector<std::string> fields;
      bool complete = false;
      size_t pos = 0;
      if (content.size() >= 3 && '\xEF' == content[0] && '\xBB' == content[1]
          && '\xBF' == content[2]) {
        pos = 3;
      }
      // Drop the header record.
      pos = parse_csv_record(content, pos, fields, complete);
      if (!complete) {
        ret = OB_INVALID_DATA;
      }
      while (OB_SUCC(ret) && pos < content.size()) {
        const size_t next = parse_csv_record(content, pos, fields, complete);
        if (!complete) {
          ret = OB_INVALID_DATA;
        } else if (fields.size() == 1 && is_blank(fields[0])) {
          // Skip blank lines.
        } else {
          std::vector<ObFileCell> row;
          if (OB_FAIL(build_csv_row(fields, columns, row))) {
          } else {
            rows_.push_back(std::move(row));
          }
        }
        pos = next;
      }
    }
  }
  if (OB_SUCC(ret)) {
    opened_ = true;
    format_ = format;
    path_ = path;
    columns_ = columns;
    device_ = current_device;
    inode_ = current_inode;
    file_size_ = current_size;
    modified_time_ns_ = current_mtime;
    next_row_ = 0;
  }
  return ret;
}

int ObFileScanReader::open(const std::string &path,
                           const ObFileFormat format,
                           const std::vector<ObFileColumnSchema> &columns,
                           const int64_t file_size,
                           const int64_t modified_time_ns)
{
  int ret = OB_SUCCESS;
  reset();
  uint64_t device = 0;
  uint64_t inode = 0;
  int64_t current_size = 0;
  int64_t current_mtime = 0;
  if (path.empty() || columns.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObFileFormat::JSONL != format) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(stat_path(path, device, inode, current_size, current_mtime))) {
  } else if (current_size != file_size || current_mtime != modified_time_ns) {
    ret = OB_SCHEMA_EAGAIN;
  } else {
    std::string content;
    if (OB_FAIL(read_whole_file(path, content))) {
    } else {
      std::map<std::string, size_t> column_index;
      for (size_t i = 0; i < columns.size(); ++i) {
        column_index[columns[i].source_name_] = i;
      }
      size_t pos = 0;
      while (OB_SUCC(ret) && pos <= content.size()) {
        size_t line_end = content.find('\n', pos);
        if (std::string::npos == line_end) {
          line_end = content.size();
        }
        std::string line = content.substr(pos, line_end - pos);
        pos = line_end + 1;
        if (!line.empty() && '\r' == line[line.size() - 1]) {
          line.pop_back();
        }
        if (is_blank(line)) {
          continue;
        }
        std::vector<std::pair<std::string, JsonlValue>> values;
        if (OB_FAIL(parse_flat_json_object(line, values))) {
        } else {
          std::vector<ObFileCell> row(columns.size());
          for (const auto &entry : values) {
            auto iter = column_index.find(entry.first);
            if (column_index.end() == iter) {
              continue;
            }
            const size_t index = iter->second;
            fill_jsonl_cell(row[index], columns[index].type_, entry.second);
          }
          rows_.push_back(std::move(row));
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    opened_ = true;
    format_ = format;
    path_ = path;
    columns_ = columns;
    // Store the identity to keep the mutation check working; JSONL callers only
    // pass the size and mtime.
    device_ = 0;
    inode_ = 0;
    file_size_ = current_size;
    modified_time_ns_ = current_mtime;
    next_row_ = 0;
  }
  return ret;
}

int ObFileScanReader::get_next_row(std::vector<ObFileCell> &cells)
{
  int ret = OB_SUCCESS;
  if (!opened_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(check_unchanged())) {
  } else if (next_row_ >= static_cast<int64_t>(rows_.size())) {
    ret = OB_ITER_END;
  } else {
    cells = rows_[next_row_++];
  }
  return ret;
}

}  // namespace sql
}  // namespace oceanbase
