// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/json/reader.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "arrow/io/interfaces.h"
#include "arrow/json/options.h"
#include "arrow/json/test_common.h"
#include "arrow/table.h"
#include "arrow/testing/gtest_util.h"

namespace arrow {
namespace json {

using util::string_view;

using internal::checked_cast;

class ReaderTest : public ::testing::TestWithParam<bool> {
 public:
  void SetUpReader() {
    read_options_.use_threads = GetParam();
    ASSERT_OK_AND_ASSIGN(reader_, TableReader::Make(default_memory_pool(), input_,
                                                    read_options_, parse_options_));
  }

  void SetUpReader(util::string_view input) {
    ASSERT_OK(MakeStream(input, &input_));
    SetUpReader();
  }

  std::shared_ptr<ChunkedArray> ChunkedFromJSON(const std::shared_ptr<Field>& field,
                                                const std::vector<std::string>& data) {
    ArrayVector chunks(data.size());
    for (size_t i = 0; i < chunks.size(); ++i) {
      chunks[i] = ArrayFromJSON(field->type(), data[i]);
    }
    return std::make_shared<ChunkedArray>(std::move(chunks));
  }

  ParseOptions parse_options_ = ParseOptions::Defaults();
  ReadOptions read_options_ = ReadOptions::Defaults();
  std::shared_ptr<io::InputStream> input_;
  std::shared_ptr<TableReader> reader_;
  std::shared_ptr<Table> table_;
};

INSTANTIATE_TEST_SUITE_P(ReaderTest, ReaderTest, ::testing::Values(false, true));

TEST_P(ReaderTest, Empty) {
  SetUpReader("{}\n{}\n");
  ASSERT_OK_AND_ASSIGN(table_, reader_->Read());

  auto expected_table = Table::Make(schema({}), ArrayVector(), 2);
  AssertTablesEqual(*expected_table, *table_);
}

TEST_P(ReaderTest, EmptyNoNewlineAtEnd) {
  SetUpReader("{}\n{}");
  ASSERT_OK_AND_ASSIGN(table_, reader_->Read());

  auto expected_table = Table::Make(schema({}), ArrayVector(), 2);
  AssertTablesEqual(*expected_table, *table_);
}

TEST_P(ReaderTest, EmptyManyNewlines) {
  SetUpReader("{}\n\r\n{}\n\r\n");
  ASSERT_OK_AND_ASSIGN(table_, reader_->Read());

  auto expected_table = Table::Make(schema({}), ArrayVector(), 2);
  AssertTablesEqual(*expected_table, *table_);
}

TEST_P(ReaderTest, Basics) {
  parse_options_.unexpected_field_behavior = UnexpectedFieldBehavior::InferType;
  auto src = scalars_only_src();
  SetUpReader(src);
  ASSERT_OK_AND_ASSIGN(table_, reader_->Read());

  auto schema = ::arrow::schema(
      {field("hello", float64()), field("world", boolean()), field("yo", utf8())});

  auto expected_table = Table::Make(
      schema, {
                  ArrayFromJSON(schema->field(0)->type(), "[3.5, 3.25, 3.125, 0.0]"),
                  ArrayFromJSON(schema->field(1)->type(), "[false, null, null, true]"),
                  ArrayFromJSON(schema->field(2)->type(),
                                "[\"thing\", null, \"\xe5\xbf\x8d\", null]"),
              });
  AssertTablesEqual(*expected_table, *table_);
}

TEST_P(ReaderTest, Nested) {
  parse_options_.unexpected_field_behavior = UnexpectedFieldBehavior::InferType;
  auto src = nested_src();
  SetUpReader(src);
  ASSERT_OK_AND_ASSIGN(table_, reader_->Read());

  auto schema = ::arrow::schema({field("hello", float64()), field("world", boolean()),
                                 field("yo", utf8()), field("arr", list(int64())),
                                 field("nuf", struct_({field("ps", int64())}))});

  auto a0 = ArrayFromJSON(schema->field(0)->type(), "[3.5, 3.25, 3.125, 0.0]");
  auto a1 = ArrayFromJSON(schema->field(1)->type(), "[false, null, null, true]");
  auto a2 = ArrayFromJSON(schema->field(2)->type(),
                          "[\"thing\", null, \"\xe5\xbf\x8d\", null]");
  auto a3 = ArrayFromJSON(schema->field(3)->type(), "[[1, 2, 3], [2], [], null]");
  auto a4 = ArrayFromJSON(schema->field(4)->type(),
                          R"([{"ps":null}, null, {"ps":78}, {"ps":90}])");
  auto expected_table = Table::Make(schema, {a0, a1, a2, a3, a4});
  AssertTablesEqual(*expected_table, *table_);
}

TEST_P(ReaderTest, PartialSchema) {
  parse_options_.unexpected_field_behavior = UnexpectedFieldBehavior::InferType;
  parse_options_.explicit_schema =
      schema({field("nuf", struct_({field("absent", date32())})),
              field("arr", list(float32()))});
  auto src = nested_src();
  SetUpReader(src);
  ASSERT_OK_AND_ASSIGN(table_, reader_->Read());

  auto schema = ::arrow::schema(
      {field("nuf", struct_({field("absent", date32()), field("ps", int64())})),
       field("arr", list(float32())), field("hello", float64()),
       field("world", boolean()), field("yo", utf8())});

  auto expected_table = Table::Make(
      schema,
      {
          // NB: explicitly declared fields will appear first
          ArrayFromJSON(
              schema->field(0)->type(),
              R"([{"absent":null,"ps":null}, null, {"absent":null,"ps":78}, {"absent":null,"ps":90}])"),
          ArrayFromJSON(schema->field(1)->type(), R"([[1, 2, 3], [2], [], null])"),
          // ...followed by undeclared fields
          ArrayFromJSON(schema->field(2)->type(), "[3.5, 3.25, 3.125, 0.0]"),
          ArrayFromJSON(schema->field(3)->type(), "[false, null, null, true]"),
          ArrayFromJSON(schema->field(4)->type(),
                        "[\"thing\", null, \"\xe5\xbf\x8d\", null]"),
      });
  AssertTablesEqual(*expected_table, *table_);
}

TEST_P(ReaderTest, TypeInference) {
  parse_options_.unexpected_field_behavior = UnexpectedFieldBehavior::InferType;
  SetUpReader(R"(
    {"ts":null, "f": null}
    {"ts":"1970-01-01", "f": 3}
    {"ts":"2018-11-13 17:11:10", "f":3.125}
    )");
  ASSERT_OK_AND_ASSIGN(table_, reader_->Read());

  auto schema =
      ::arrow::schema({field("ts", timestamp(TimeUnit::SECOND)), field("f", float64())});
  auto expected_table = Table::Make(
      schema, {ArrayFromJSON(schema->field(0)->type(),
                             R"([null, "1970-01-01", "2018-11-13 17:11:10"])"),
               ArrayFromJSON(schema->field(1)->type(), R"([null, 3, 3.125])")});
  AssertTablesEqual(*expected_table, *table_);
}

TEST_P(ReaderTest, MultipleChunks) {
  parse_options_.unexpected_field_behavior = UnexpectedFieldBehavior::InferType;

  auto src = scalars_only_src();
  read_options_.block_size = static_cast<int>(src.length() / 3);

  SetUpReader(src);
  ASSERT_OK_AND_ASSIGN(table_, reader_->Read());

  auto schema = ::arrow::schema(
      {field("hello", float64()), field("world", boolean()), field("yo", utf8())});

  // there is an empty chunk because the last block of the file is "  "
  auto expected_table = Table::Make(
      schema,
      {
          ChunkedFromJSON(schema->field(0), {"[3.5]", "[3.25]", "[3.125, 0.0]", "[]"}),
          ChunkedFromJSON(schema->field(1), {"[false]", "[null]", "[null, true]", "[]"}),
          ChunkedFromJSON(schema->field(2),
                          {"[\"thing\"]", "[null]", "[\"\xe5\xbf\x8d\", null]", "[]"}),
      });
  AssertTablesEqual(*expected_table, *table_);
}

template <typename T>
std::string RowsOfOneColumn(string_view name, std::initializer_list<T> values,
                            decltype(std::to_string(*values.begin()))* = nullptr) {
  std::stringstream ss;
  for (auto value : values) {
    ss << R"({")" << name << R"(":)" << std::to_string(value) << "}\n";
  }
  return ss.str();
}

TEST(ReaderTest, MultipleChunksParallel) {
  int64_t count = 1 << 10;

  ParseOptions parse_options;
  parse_options.unexpected_field_behavior = UnexpectedFieldBehavior::InferType;
  ReadOptions read_options;
  read_options.block_size =
      static_cast<int>(count / 2);  // there will be about two dozen blocks

  std::string json;
  for (int i = 0; i < count; ++i) {
    json += "{\"a\":" + std::to_string(i) + "}\n";
  }
  std::shared_ptr<io::InputStream> input;
  std::shared_ptr<TableReader> reader;

  read_options.use_threads = true;
  ASSERT_OK(MakeStream(json, &input));
  ASSERT_OK_AND_ASSIGN(reader, TableReader::Make(default_memory_pool(), input,
                                                 read_options, parse_options));
  ASSERT_OK_AND_ASSIGN(auto threaded, reader->Read());

  read_options.use_threads = false;
  ASSERT_OK(MakeStream(json, &input));
  ASSERT_OK_AND_ASSIGN(reader, TableReader::Make(default_memory_pool(), input,
                                                 read_options, parse_options));
  ASSERT_OK_AND_ASSIGN(auto serial, reader->Read());

  ASSERT_EQ(serial->column(0)->type()->id(), Type::INT64);
  int expected = 0;
  for (auto chunk : serial->column(0)->chunks()) {
    for (int64_t i = 0; i < chunk->length(); ++i) {
      ASSERT_EQ(checked_cast<const Int64Array*>(chunk.get())->GetView(i), expected)
          << " at index " << i;
      ++expected;
    }
  }

  AssertTablesEqual(*serial, *threaded);
}

TEST(ReaderTest, ListArrayWithFewValues) {
  // ARROW-7647
  ParseOptions parse_options;
  parse_options.unexpected_field_behavior = UnexpectedFieldBehavior::InferType;
  ReadOptions read_options;

  auto expected_batch = RecordBatchFromJSON(
      schema({field("a", list(int64())),
              field("b", struct_({field("c", boolean()),
                                  field("d", timestamp(TimeUnit::SECOND))}))}),
      R"([
        {"a": [1], "b": {"c": true, "d": "1991-02-03"}},
        {"a": [], "b": {"c": false, "d": "2019-04-01"}}
      ])");
  ASSERT_OK_AND_ASSIGN(auto expected_table, Table::FromRecordBatches({expected_batch}));

  std::string json = R"({"a": [1], "b": {"c": true, "d": "1991-02-03"}}
{"a": [], "b": {"c": false, "d": "2019-04-01"}}
)";
  std::shared_ptr<io::InputStream> input;
  ASSERT_OK(MakeStream(json, &input));

  read_options.use_threads = false;
  ASSERT_OK_AND_ASSIGN(auto reader, TableReader::Make(default_memory_pool(), input,
                                                      read_options, parse_options));

  ASSERT_OK_AND_ASSIGN(auto actual_table, reader->Read());
  AssertTablesEqual(*actual_table, *expected_table);
}

TEST(ReaderTest, FixedSizeList) {
  constexpr int32_t NUM_VALS = 3;
  auto fsl_type = fixed_size_list(int64(), NUM_VALS);
  auto s = schema({field("a", fsl_type)});
  ParseOptions parse_options;
  parse_options.unexpected_field_behavior = UnexpectedFieldBehavior::InferType;
  parse_options.explicit_schema = s;
  ReadOptions read_options;
  read_options.block_size = 15;
  read_options.use_threads = false;

  auto value_builder1 = std::make_shared<Int64Builder>();
  auto list_builder1 = std::make_shared<FixedSizeListBuilder>(
      default_memory_pool(), std::make_shared<Int64Builder>(), NUM_VALS);
  auto value_builder2 = std::make_shared<Int64Builder>();
  auto list_builder2 = std::make_shared<FixedSizeListBuilder>(default_memory_pool(),
                                                              value_builder2, NUM_VALS);

  ASSERT_OK(list_builder1->Append());
  ASSERT_OK(value_builder1->AppendValues({1, 2, 3}));
  ASSERT_OK(list_builder2->Append());
  ASSERT_OK(value_builder2->AppendValues({4, 5, 6}));

  std::shared_ptr<Array> array1;
  ASSERT_OK(list_builder1->Finish(&array1));
  std::shared_ptr<Array> array2;
  ASSERT_OK(list_builder2->Finish(&array2));

  auto eb1 = RecordBatch::Make(s, 1, {array1});
  auto eb2 = RecordBatch::Make(s, 1, {array2});

  auto e_chunked_table = arrow::Table::FromRecordBatches({eb1, eb2}).ValueOrDie();
  auto e_combined_table = e_chunked_table->CombineChunks().ValueOrDie();
  auto e_table_reader = arrow::TableBatchReader(*e_combined_table);
  auto expected_batch = e_table_reader.Next().ValueOrDie();

  std::shared_ptr<io::InputStream> input1;
  std::shared_ptr<io::InputStream> input2;

  std::string json1 = R"({"a": [1,2,3,4,5,6,7,8,9,10,11,12]})";
  std::string json2 = R"({"a": [13,14,15,16,17,18,19,20,21,22,23,24]})";

  auto buf1 = arrow::Buffer::Wrap(json1.data(), json1.length());
  auto buf2 = arrow::Buffer::Wrap(json2.data(), json2.length());

  auto ab1 = ParseOne(parse_options, buf1).ValueOrDie();
  auto ab2 = ParseOne(parse_options, buf2).ValueOrDie();

  auto a_chunked_table = arrow::Table::FromRecordBatches({ab1, ab2}).ValueOrDie();
  auto a_combined_table = a_chunked_table->CombineChunks().ValueOrDie();
  auto a_table_reader = arrow::TableBatchReader(*a_combined_table);
  auto actual_batch = a_table_reader.Next().ValueOrDie();

  std::cout << "Expected:" << std::endl << expected_batch->ToString();
  std::cout << "Actual:" << std::endl << actual_batch->ToString();

  // AssertTablesEqual(*expected_table, *actual_table, false);
}

TEST(ReaderTest, List) {
  auto list_type = list(int64());
  auto s = schema({field("a", list_type)});
  ParseOptions parse_options;
  parse_options.unexpected_field_behavior = UnexpectedFieldBehavior::InferType;
  parse_options.explicit_schema = s;
  ReadOptions read_options;

  auto values_builder = std::make_shared<Int64Builder>();
  auto list_builder =
      std::make_shared<ListBuilder>(default_memory_pool(), values_builder);

  ASSERT_OK(list_builder->Append());
  ASSERT_OK(values_builder->AppendValues({1, 2, 3}));
  ASSERT_OK(list_builder->Append());
  ASSERT_OK(values_builder->AppendValues({4, 5, 6, 7}));

  std::shared_ptr<Array> array;
  ASSERT_OK(list_builder->Finish(&array));
  auto batch = RecordBatch::Make(s, 2, {array});
  auto expected_table = Table::FromRecordBatches({batch}).ValueOrDie();

  std::string json = R"({"a": [1, 2, 3]}
{"a": [4, 5, 6, 7]}
)";
  std::shared_ptr<io::InputStream> input;
  ASSERT_OK(MakeStream(json, &input));

  read_options.use_threads = false;
  ASSERT_OK_AND_ASSIGN(auto reader, TableReader::Make(default_memory_pool(), input,
                                                      read_options, parse_options));

  ASSERT_OK_AND_ASSIGN(auto actual_table, reader->Read());
  AssertTablesEqual(*expected_table, *actual_table, false);
}

}  // namespace json
}  // namespace arrow
