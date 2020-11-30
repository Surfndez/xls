// Copyright 2020 The XLS Authors
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

#include "absl/base/casts.h"
#include "absl/status/statusor.h"
#include "pybind11/functional.h"
#include "pybind11/pybind11.h"
#include "pybind11/stl.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/status/statusor_pybind_caster.h"
#include "xls/dslx/concrete_type.h"
#include "xls/dslx/deduce.h"
#include "xls/dslx/import_routines.h"
#include "xls/dslx/interp_bindings.h"
#include "xls/dslx/python/callback_converters.h"
#include "xls/dslx/python/cpp_ast.h"
#include "xls/dslx/python/errors.h"

namespace py = pybind11;

namespace xls::dslx {
namespace {

// Error raised when an error occurs during deductive type inference.
//
// Attributes:
//   span: The span at which the type deduction error occurred.
//   type: The (AST) type that failed to deduce. May be null.
class TypeInferenceError : public std::exception {
 public:
  // Args:
  //  suffix: Message suffix to use when displaying the error.
  TypeInferenceError(Span span, std::unique_ptr<ConcreteType> type,
                     std::string suffix)
      : span_(std::move(span)), type_(std::move(type)) {
    std::string type_str;
    if (type != nullptr) {
      type_str = " for " + type->ToString();
    }
    message_ = absl::StrFormat("Could not infer type%s @ %s", type_str,
                               span.ToString());
    if (!suffix.empty()) {
      message_ += ": " + suffix;
    }
  }

  const char* what() const noexcept override { return message_.c_str(); }

  const Span& span() const { return span_; }
  const ConcreteType* type() const { return type_.get(); }
  const std::string& message() const { return message_; }

 private:
  Span span_;
  std::unique_ptr<ConcreteType> type_;
  std::string message_;
};

void TryThrowTypeInferenceError(const absl::Status& status) {
  absl::string_view s = status.message();
  if (absl::ConsumePrefix(&s, "TypeInferenceError: ")) {
    std::vector<absl::string_view> pieces =
        absl::StrSplit(s, absl::MaxSplits(" ", 2));
    if (pieces.size() < 3) {
      return;
    }
    absl::StatusOr<Span> span = Span::FromString(pieces[0]);
    absl::StatusOr<std::unique_ptr<ConcreteType>> type =
        ConcreteTypeFromString(pieces[1]);
    throw TypeInferenceError(std::move(span.value()), std::move(type).value(),
                             std::string(pieces[2]));
  }
}

}  // namespace

PYBIND11_MODULE(cpp_deduce, m) {
  ImportStatusModule();

  py::class_<FnStackEntry>(m, "FnStackEntry")
      .def_property_readonly(
          "name", [](const FnStackEntry& entry) { return entry.name; })
      .def_property_readonly(
          "symbolic_bindings",
          [](const FnStackEntry& entry) { return entry.symbolic_bindings; });

  static py::exception<TypeInferenceError> type_inference_exc(
      m, "TypeInferenceError");

  py::register_exception_translator([](std::exception_ptr p) {
    try {
      if (p) std::rethrow_exception(p);
    } catch (const TypeInferenceError& e) {
      type_inference_exc.attr("span") = e.span();
      if (e.type() == nullptr) {
        type_inference_exc.attr("type_") = nullptr;
      } else {
        type_inference_exc.attr("type_") = e.type()->CloneToUnique();
      }
      type_inference_exc.attr("message") = e.message();
      type_inference_exc(e.what());
    }
  });

  py::class_<DeduceCtx, std::shared_ptr<DeduceCtx>>(m, "DeduceCtx")
      .def(py::init([](const std::shared_ptr<TypeInfo>& type_info,
                       ModuleHolder module,
                       PyTypecheckFunctionFn typecheck_function,
                       PyTypecheckFn typecheck_module,
                       absl::optional<ImportCache*> import_cache) {
        ImportCache* pimport_cache = import_cache ? *import_cache : nullptr;
        return std::make_shared<DeduceCtx>(
            type_info, module.deref().shared_from_this(),
            ToCppTypecheckFunction(typecheck_function),
            ToCppTypecheck(typecheck_module), pimport_cache);
      }))
      .def("add_fn_stack_entry",
           [](DeduceCtx& self, std::string name,
              const SymbolicBindings& sym_bindings) {
             self.AddFnStackEntry(FnStackEntry{name, sym_bindings});
           })
      .def("pop_fn_stack_entry",
           [](DeduceCtx& self) { return self.PopFnStackEntry(); })
      .def("peek_fn_stack",
           [](DeduceCtx& self) {
             absl::optional<FnStackEntry> result;
             if (!self.fn_stack().empty()) {
               result = self.fn_stack().back();
             }
             return result;
           })
      .def("add_derived_type_info",
           [](DeduceCtx& self) { self.AddDerivedTypeInfo(); })
      .def("pop_derived_type_info",
           [](DeduceCtx& self) { return self.PopDerivedTypeInfo(); })
      .def_property_readonly(
          "import_cache",
          [](const DeduceCtx& ctx) -> absl::optional<ImportCache*> {
            ImportCache* cache = ctx.import_cache();
            if (cache == nullptr) {
              return absl::nullopt;
            }
            return cache;
          })
      .def_property_readonly(
          "type_info", [](const DeduceCtx& ctx) { return ctx.type_info(); })
      .def_property_readonly("module",
                             [](const DeduceCtx& ctx) {
                               return ModuleHolder(ctx.module().get(),
                                                   ctx.module());
                             })
      .def_property_readonly("typecheck_module",
                             [](const DeduceCtx& ctx) {
                               return ToPyTypecheck(ctx.typecheck_module());
                             })
      .def("typecheck_function",
           [](const DeduceCtx& self, FunctionHolder function, DeduceCtx* ctx) {
             return self.typecheck_function()(&function.deref(), ctx);
           })
      .def("make_ctx", [](const DeduceCtx& self,
                          const std::shared_ptr<TypeInfo>& new_type_info,
                          ModuleHolder new_module) {
        return self.MakeCtx(new_type_info, new_module.module());
      });

  m.def("check_bitwidth", [](NumberHolder number, const ConcreteType& type) {
    auto status = CheckBitwidth(number.deref(), type);
    TryThrowTypeInferenceError(status);
    return status;
  });
}

}  // namespace xls::dslx