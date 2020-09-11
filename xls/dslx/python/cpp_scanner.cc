// Copyright 2020 Google LLC
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

#include "xls/dslx/cpp_scanner.h"

#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "pybind11/functional.h"
#include "pybind11/pybind11.h"
#include "pybind11/stl.h"
#include "xls/common/status/statusor_pybind_caster.h"
#include "xls/dslx/cpp_ast_builtin_types.inc"
#include "xls/dslx/cpp_pos.h"

namespace py = pybind11;

namespace xls::dslx {

class ScanError : public std::exception {
 public:
  ScanError(Pos pos, std::string message)
      : pos_(std::move(pos)), message_(std::move(message)) {}

  const char* what() const noexcept override { return message_.c_str(); }

  const Pos& pos() const { return pos_; }

 private:
  Pos pos_;
  std::string message_;
};

void TryThrowScanError(const absl::Status& status) {
  absl::string_view s = status.message();
  if (absl::ConsumePrefix(&s, "ScanError: ")) {
    std::vector<absl::string_view> pieces =
        absl::StrSplit(s, absl::MaxSplits(" ", 1));
    if (pieces.size() < 2) {
      return;
    }
    xabsl::StatusOr<Pos> pos = Pos::FromString(pieces[0]);
    throw ScanError(std::move(pos.value()), std::string(pieces[1]));
  }
}

const absl::Status& GetStatus(const absl::Status& status) { return status; }
template <typename T>
const absl::Status& GetStatus(const xabsl::StatusOr<T>& status_or) {
  return status_or.status();
}

template <typename ReturnT, typename... Args>
std::function<ReturnT(Scanner*, Args...)> ScanErrorWrap(
    ReturnT (Scanner::*f)(Args...)) {
  return [f](Scanner* s, Args... args) {
    auto statusor = ((*s).*f)(std::forward<Args>(args)...);
    TryThrowScanError(GetStatus(statusor));
    return statusor;
  };
}

std::unordered_map<Keyword, std::tuple<bool, int64>>
GetTypeKeywordsToSignednessAndBits() {
  std::unordered_map<Keyword, std::tuple<bool, int64>> result;
#define ADD(__enum, __pyname, __str, __signedness, __bits) \
  result.insert({Keyword::__enum, {__signedness, __bits}});
  XLS_DSLX_BUILTIN_TYPE_EACH(ADD)
#undef ADD
  return result;
}

std::unordered_set<std::string> GetTypeKeywordStrings() {
  std::unordered_set<std::string> result;
#define ADD(__enum, __pyname, __str, __signedness, __bits) result.insert(__str);
  XLS_DSLX_BUILTIN_TYPE_EACH(ADD)
#undef ADD
  return result;
}

PYBIND11_MODULE(cpp_scanner, m) {
  py::enum_<Keyword>(m, "Keyword")
#define VALUE(__enum, __pyattr, ...) .value(#__pyattr, Keyword::__enum)
      XLS_DSLX_KEYWORDS(VALUE)
#undef VALUE
          .export_values()
          .def_property_readonly("value", [](Keyword keyword) {
            return KeywordToString(keyword);
          });

  py::enum_<TokenKind>(m, "TokenKind")
#define VALUE(__enum, __pyattr, ...) .value(#__pyattr, TokenKind::__enum)
      XLS_DSLX_TOKEN_KINDS(VALUE)
#undef VALUE
          .export_values()
          .def_property_readonly(
              "value", [](TokenKind kind) { return TokenKindToString(kind); });

  m.attr("TYPE_KEYWORDS") = std::unordered_set<Keyword>(
      GetTypeKeywords().begin(), GetTypeKeywords().end());
  m.attr("TYPE_KEYWORDS_TO_SIGNEDNESS_AND_BITS") =
      GetTypeKeywordsToSignednessAndBits();
  m.attr("TYPE_KEYWORD_STRINGS") = GetTypeKeywordStrings();

  m.def("KeywordFromString",
        [](absl::string_view s) { return KeywordFromString(s); });
  m.def("TokenKindFromString",
        [](absl::string_view s) { return TokenKindFromString(s); });

  py::register_exception<ScanError>(m, "ScanError");

  py::class_<Token>(m, "Token")
      .def(py::init([](TokenKind kind, Span span,
                       absl::optional<std::string> value) {
             return Token(kind, span, value);
           }),
           py::arg("kind"), py::arg("span"), py::arg("value"))
      .def(py::init(
               [](Span span, Keyword keyword) { return Token(span, keyword); }),
           py::arg("span"), py::arg("value"))
      .def_property_readonly("kind", &Token::kind)
      .def_property_readonly("span", &Token::span)
      .def_property_readonly("value", &Token::GetPayload)
      .def("to_error_str", &Token::ToErrorString)
      .def("is_keyword", &Token::IsKeyword)
      .def("is_keyword_in", &Token::IsKeywordIn)
      .def("is_type_keyword", &Token::IsTypeKeyword)
      .def("is_identifier", &Token::IsIdentifier)
      .def("is_number", &Token::IsNumber)
      .def("__str__", &Token::ToString)
      .def("__repr__", &Token::ToRepr);

  py::class_<Scanner>(m, "Scanner")
      .def(py::init([](std::string filename, std::string text,
                       bool include_whitespace_and_comments) {
             return Scanner(std::move(filename), std::move(text),
                            include_whitespace_and_comments);
           }),
           py::arg("filename"), py::arg("text"),
           py::arg("include_whitespace_and_comments") = false)
      .def("at_eof", &Scanner::AtEof)
      .def("peek", ScanErrorWrap(&Scanner::Peek))
      .def("pop", ScanErrorWrap(&Scanner::Pop))
      // TODO(leary): 2020-09-08 Rename to try_drop and try_drop_keyword
      .def("try_pop", ScanErrorWrap(&Scanner::TryDrop))
      .def("try_pop_keyword", ScanErrorWrap(&Scanner::TryDropKeyword))
      .def("pop_or_error", ScanErrorWrap(&Scanner::PopOrError))
      .def("drop_or_error", ScanErrorWrap(&Scanner::DropOrError))
      .def("pop_all", ScanErrorWrap(&Scanner::PopAll))
      .def_property_readonly("pos", &Scanner::GetPos);
}

}  // namespace xls::dslx
