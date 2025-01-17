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

#include "xls/dslx/deduce_ctx.h"

#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "absl/types/variant.h"
#include "xls/common/string_to_int.h"

namespace xls::dslx {

std::string FnStackEntry::ToReprString() const {
  return absl::StrFormat("FnStackEntry{\"%s\", %s}", name_,
                         symbolic_bindings_.ToString());
}

DeduceCtx::DeduceCtx(TypeInfo* type_info, Module* module,
                     DeduceFn deduce_function,
                     TypecheckFunctionFn typecheck_function,
                     TypecheckModuleFn typecheck_module,
                     TypecheckInvocationFn typecheck_invocation,
                     ImportData* import_data)
    : type_info_(type_info),
      module_(module),
      deduce_function_(std::move(XLS_DIE_IF_NULL(deduce_function))),
      typecheck_function_(std::move(typecheck_function)),
      typecheck_module_(std::move(typecheck_module)),
      typecheck_invocation_(std::move(typecheck_invocation)),
      import_data_(import_data) {}

// Helper that converts the symbolic bindings to a parametric expression
// environment (for parametric evaluation).
ParametricExpression::Env ToParametricEnv(
    const SymbolicBindings& symbolic_bindings) {
  ParametricExpression::Env env;
  for (const SymbolicBinding& binding : symbolic_bindings.bindings()) {
    env[binding.identifier] = binding.value;
  }
  return env;
}

absl::flat_hash_map<std::string, InterpValue> MakeConstexprEnv(
    const Expr* node, const SymbolicBindings& symbolic_bindings,
    TypeInfo* type_info, absl::flat_hash_set<const NameDef*> bypass_env) {
  XLS_CHECK_EQ(node->owner(), type_info->module())
      << "expr `" << node->ToString()
      << "` from module: " << node->owner()->name()
      << " vs type info module: " << type_info->module()->name();
  XLS_VLOG(5) << "Creating constexpr environment for node: "
              << node->ToString();
  absl::flat_hash_map<std::string, InterpValue> env;
  absl::flat_hash_map<std::string, InterpValue> values;

  for (auto [id, value] : symbolic_bindings.ToMap()) {
    env.insert({id, value});
  }

  // Collect all the freevars that are constexpr.
  //
  // TODO(https://github.com/google/xls/issues/333): 2020-03-11 We'll want the
  // expression to also be able to constexpr evaluate local non-integral values,
  // like constant tuple definitions and such. We'll need to extend the
  // constexpr ability to full InterpValues to accomplish this.
  //
  // E.g. fn main(x: u32) -> ... { const B = u32:20; x[:B] }

  // Collect all the freevars that are constexpr.
  FreeVariables freevars = node->GetFreeVariables();
  XLS_VLOG(5) << "freevars for " << node->ToString() << ": "
              << freevars.GetFreeVariableCount();
  freevars = freevars.DropBuiltinDefs();
  for (const auto& [name, name_refs] : freevars.values()) {
    const NameRef* target_ref = nullptr;
    for (const NameRef* name_ref : name_refs) {
      if (!bypass_env.contains(absl::get<NameDef*>(name_ref->name_def()))) {
        target_ref = name_ref;
        break;
      }
    }

    if (target_ref == nullptr) {
      continue;
    }

    absl::optional<InterpValue> const_expr =
        type_info->GetConstExpr(target_ref);
    if (const_expr.has_value()) {
      env.insert({name, const_expr.value()});
    }
  }

  for (const ConstRef* const_ref : freevars.GetConstRefs()) {
    ConstantDef* constant_def = const_ref->GetConstantDef();
    XLS_VLOG(5) << "analyzing constant reference: " << const_ref->ToString()
                << " def: " << constant_def->ToString();
    absl::optional<InterpValue> value =
        type_info->GetConstExpr(constant_def->value());
    if (!value.has_value()) {
      // Could be a tuple or similar, not part of the (currently integral-only)
      // constexpr environment.
      XLS_VLOG(5) << "Could not find constexpr value for constant def: `"
                  << constant_def->ToString() << "` @ " << constant_def->value()
                  << " in " << type_info;
      continue;
    }

    XLS_VLOG(5) << "freevar env record: " << const_ref->identifier() << " => "
                << value->ToString();
    env.insert({const_ref->identifier(), *value});
  }

  return env;
}

}  // namespace xls::dslx
