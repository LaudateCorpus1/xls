// Copyright 2021 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/contrib/xlscc/cc_parser.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/file/temp_file.h"
#include "xls/common/status/matchers.h"
#include "xls/contrib/xlscc/metadata_output.pb.h"
#include "xls/contrib/xlscc/unit_test.h"
#include "xls/ir/ir_test_base.h"

namespace {

class CCParserTest : public XlsccTestBase {
 public:
};

TEST_F(CCParserTest, Basic) {
  xlscc::CCParser parser;

  const std::string cpp_src = R"(
    #pragma hls_top
    int foo(int a, int b) {
      const int foo = a + b;
      return foo;
    }
  )";

  XLS_ASSERT_OK(ScanTempFileWithContent(cpp_src, {}, &parser));
  XLS_ASSERT_OK_AND_ASSIGN(const auto* top_ptr, parser.GetTopFunction());
  EXPECT_NE(top_ptr, nullptr);
}

TEST_F(CCParserTest, TopNotFound) {
  xlscc::CCParser parser;

  const std::string cpp_src = R"(
    int foo(int a, int b) {
      const int foo = a + b;
      return foo;
    }
  )";

  XLS_ASSERT_OK(ScanTempFileWithContent(cpp_src, {}, &parser));
  EXPECT_THAT(parser.GetTopFunction().status(),
              xls::status_testing::StatusIs(absl::StatusCode::kNotFound));
}

TEST_F(CCParserTest, SourceMeta) {
  xlscc::CCParser parser;

  const std::string cpp_src = R"(
    #pragma hls_top
    int foo(int a, int b) {
      const int foo = a + b;
      return foo;
    }
  )";

  XLS_ASSERT_OK(ScanTempFileWithContent(cpp_src, {}, &parser));
  XLS_ASSERT_OK_AND_ASSIGN(const auto* top_ptr, parser.GetTopFunction());
  ASSERT_NE(top_ptr, nullptr);

  xls::SourceLocation loc = parser.GetLoc(*top_ptr);

  xlscc_metadata::MetadataOutput output;
  parser.AddSourceInfoToMetadata(output);
  ASSERT_EQ(output.sources_size(), 1);
  EXPECT_EQ(static_cast<int32_t>(loc.fileno()),
            static_cast<int32_t>(output.sources(0).number()));
}

TEST_F(CCParserTest, Pragma) {
  xlscc::CCParser parser;

  const std::string cpp_src = R"(
    #pragma hls_top
    int foo(int a, int b) {
      const int foo = a + b;
      return foo;
    }
  )";

  XLS_ASSERT_OK(ScanTempFileWithContent(cpp_src, {}, &parser));
  XLS_ASSERT_OK_AND_ASSIGN(const auto* top_ptr, parser.GetTopFunction());
  ASSERT_NE(top_ptr, nullptr);

  clang::PresumedLoc loc = parser.GetPresumedLoc(*top_ptr);

  XLS_ASSERT_OK_AND_ASSIGN(xlscc::Pragma pragma, parser.FindPragmaForLoc(loc));

  ASSERT_EQ(pragma.type(), xlscc::Pragma_Top);
}

TEST_F(CCParserTest, PragmaSavedLine) {
  xlscc::CCParser parser;

  const std::string cpp_src = R"(
    #pragma hls_top
    int foo(int a, int b) {
      int foo = a;
      #pragma hls_pipeline_init_interval 3
      for(int i=0;i<2;++i) {
        foo += b;
      }
      return foo;
    }
  )";

  XLS_ASSERT_OK(ScanTempFileWithContent(cpp_src, {}, &parser));
  XLS_ASSERT_OK_AND_ASSIGN(const auto* top_ptr, parser.GetTopFunction());
  ASSERT_NE(top_ptr, nullptr);

  clang::PresumedLoc func_loc = parser.GetPresumedLoc(*top_ptr);
  clang::PresumedLoc loop_loc(func_loc.getFilename(), func_loc.getFileID(),
                              func_loc.getLine() + 3, func_loc.getColumn(),
                              func_loc.getIncludeLoc());

  XLS_ASSERT_OK_AND_ASSIGN(xlscc::Pragma pragma,
                           parser.FindPragmaForLoc(loop_loc));

  ASSERT_EQ(pragma.type(), xlscc::Pragma_InitInterval);
  ASSERT_EQ(pragma.int_argument(), 3);
}

TEST_F(CCParserTest, UnknownPragma) {
  xlscc::CCParser parser;

  const std::string cpp_src = R"(
    #pragma hls_top
    int foo(int a, int b) {
      int foo = a;
      #pragma foo
      for(int i=0;i<2;++i) {
        foo += b;
      }
      return foo;
    }
  )";

  XLS_ASSERT_OK(ScanTempFileWithContent(cpp_src, {}, &parser));
}

TEST_F(CCParserTest, InvalidPragmaArg) {
  xlscc::CCParser parser;

  const std::string cpp_src = R"(
    #pragma hls_top
    int foo(int a, int b) {
      int foo = a;
      #pragma hls_pipeline_init_interval hey
      for(int i=0;i<2;++i) {
        foo += b;
      }
      return foo;
    }
  )";

  ASSERT_THAT(
      ScanTempFileWithContent(cpp_src, {}, &parser),
      xls::status_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(CCParserTest, InvalidPragmaArg2) {
  xlscc::CCParser parser;

  const std::string cpp_src = R"(
    #pragma hls_top
    int foo(int a, int b) {
      int foo = a;
      #pragma hls_pipeline_init_interval -22
      for(int i=0;i<2;++i) {
        foo += b;
      }
      return foo;
    }
  )";

  ASSERT_THAT(
      ScanTempFileWithContent(cpp_src, {}, &parser),
      xls::status_testing::StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST_F(CCParserTest, CommentedPragma) {
  xlscc::CCParser parser;

  const std::string cpp_src = R"(
    #pragma hls_top
    int foo(int a, int b) {
      int foo = a;
      //#pragma hls_pipeline_init_interval -22
      for(int i=0;i<2;++i) {
        foo += b;
      }
      return foo;
    }
  )";

  XLS_ASSERT_OK(ScanTempFileWithContent(cpp_src, {}, &parser));
  XLS_ASSERT_OK_AND_ASSIGN(const auto* top_ptr, parser.GetTopFunction());
  ASSERT_NE(top_ptr, nullptr);

  clang::PresumedLoc loc = parser.GetPresumedLoc(*top_ptr);

  XLS_ASSERT_OK_AND_ASSIGN(xlscc::Pragma pragma, parser.FindPragmaForLoc(loc));

  ASSERT_EQ(pragma.type(), xlscc::Pragma_Top);
}

}  // namespace
