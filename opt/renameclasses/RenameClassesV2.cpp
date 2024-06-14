/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RenameClassesV2.h"

#include <algorithm>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/regex.hpp>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ConfigFiles.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "KeepReason.h"
#include "PassManager.h"
#include "ProguardConfiguration.h"
#include "ProguardMap.h"
#include "ReachableClasses.h"
#include "RedexResources.h"
#include "Show.h"
#include "TypeStringRewriter.h"
#include "Walkers.h"
#include "Warning.h"

#include "Trace.h"
#include <locator.h>
using facebook::Locator;

#define MAX_DESCRIPTOR_LENGTH (1024)

static const char* METRIC_DIGITS = "num_digits";
static const char* METRIC_CLASSES_IN_SCOPE = "num_classes_in_scope";
static const char* METRIC_RENAMED_CLASSES = "**num_renamed**";
static const char* METRIC_FORCE_RENAMED_CLASSES = "num_force_renamed";
static const char* METRIC_REWRITTEN_CONST_STRINGS =
    "num_rewritten_const_strings";
static const char* METRIC_MISSING_HIERARCHY_TYPES =
    "num_missing_hierarchy_types";
static const char* METRIC_MISSING_HIERARCHY_CLASSES =
    "num_missing_hierarchy_classes";

static RenameClassesPassV2 s_pass;

namespace {

const char* dont_rename_reason_to_metric(DontRenameReasonCode reason) {
  switch (reason) {
  case DontRenameReasonCode::Annotated:
    return "num_dont_rename_annotated";
  case DontRenameReasonCode::Annotations:
    return "num_dont_rename_annotations";
  case DontRenameReasonCode::Specific:
    return "num_dont_rename_specific";
  case DontRenameReasonCode::Packages:
    return "num_dont_rename_packages";
  case DontRenameReasonCode::Hierarchy:
    return "num_dont_rename_hierarchy";
  case DontRenameReasonCode::Resources:
    return "num_dont_rename_resources";
  case DontRenameReasonCode::ClassNameLiterals:
    return "num_dont_rename_class_name_literals";
  case DontRenameReasonCode::Canaries:
    return "num_dont_rename_canaries";
  case DontRenameReasonCode::NativeBindings:
    return "num_dont_rename_native_bindings";
  case DontRenameReasonCode::ClassForTypesWithReflection:
    return "num_dont_rename_class_for_types_with_reflection";
  case DontRenameReasonCode::ProguardCantRename:
    return "num_dont_rename_pg_cant_rename";
  case DontRenameReasonCode::SerdeRelationships:
    return "num_dont_rename_serde_relationships";
  default:
    not_reached_log("Unexpected DontRenameReasonCode: %d", (int)reason);
  }
}

bool dont_rename_reason_to_metric_per_rule(DontRenameReasonCode reason) {
  switch (reason) {
  case DontRenameReasonCode::Annotated:
  case DontRenameReasonCode::Packages:
  case DontRenameReasonCode::Hierarchy:
    // Set to true to add more detailed metrics for renamer if needed
    return false;
  case DontRenameReasonCode::ProguardCantRename:
    return keep_reason::Reason::record_keep_reasons();
  default:
    return false;
  }
}

} // namespace

// Returns idx of the vector of packages if the given class name matches, or -1
// if not found.
ssize_t find_matching_package(
    const std::string_view classname,
    const std::vector<std::string>& allowed_packages) {
  for (size_t i = 0; i < allowed_packages.size(); i++) {
    if (classname.rfind("L" + allowed_packages[i]) == 0) {
      return i;
    }
  }
  return -1;
}

bool referenced_by_layouts(const DexClass* clazz) {
  return clazz->rstate.is_referenced_by_resource_xml();
}

// Returns true if this class is a layout, and allowed for renaming via config.
bool is_allowed_layout_class(
    const DexClass* clazz,
    const std::vector<std::string>& allow_layout_rename_packages) {
  always_assert(referenced_by_layouts(clazz));
  auto idx = find_matching_package(clazz->get_name()->str(),
                                   allow_layout_rename_packages);
  return idx != -1;
}

std::unordered_set<std::string>
RenameClassesPassV2::build_dont_rename_class_name_literals(Scope& scope) {
  using namespace boost::algorithm;
  std::unordered_set<const DexString*> all_strings;
  for (auto clazz : scope) {
    clazz->gather_strings(all_strings);
  }
  std::unordered_set<std::string> result;
  boost::regex external_name_regex{
      "((org)|(com)|(android(x|\\.support)))\\."
      "([a-zA-Z][a-zA-Z\\d_$]*\\.)*"
      "[a-zA-Z][a-zA-Z\\d_$]*"};
  for (auto dex_str : all_strings) {
    const std::string_view s = dex_str->str();
    if (!ends_with(s, ".java") &&
        boost::regex_match(dex_str->c_str(), external_name_regex)) {
      std::string internal_name = java_names::external_to_internal(s);
      auto cls = type_class(DexType::get_type(internal_name));
      if (cls != nullptr && !cls->is_external()) {
        result.insert(std::move(internal_name));
        TRACE(RENAME, 4, "Found %s in string pool before renaming",
              str_copy(s).c_str());
      }
    }
  }
  return result;
}

std::unordered_set<const DexString*>
RenameClassesPassV2::build_dont_rename_for_types_with_reflection(
    Scope& scope, const ProguardMap& pg_map) {
  std::unordered_set<const DexString*>
      dont_rename_class_for_types_with_reflection;
  std::unordered_set<DexType*> refl_map;
  for (auto const& refl_type_str : m_dont_rename_types_with_reflection) {
    auto deobf_cls_string = pg_map.translate_class(refl_type_str);
    TRACE(RENAME,
          4,
          "%s got translated to %s",
          refl_type_str.c_str(),
          deobf_cls_string.c_str());
    if (deobf_cls_string.empty()) {
      deobf_cls_string = refl_type_str;
    }
    DexType* type_with_refl = DexType::get_type(deobf_cls_string);
    if (type_with_refl != nullptr) {
      TRACE(RENAME, 4, "got DexType %s", SHOW(type_with_refl));
      refl_map.insert(type_with_refl);
    }
  }

  walk::opcodes(
      scope,
      [](DexMethod*) { return true; },
      [&](DexMethod* m, IRInstruction* insn) {
        if (insn->has_method()) {
          auto callee = insn->get_method();
          if (callee == nullptr || !callee->is_concrete()) return;
          auto callee_method_cls = callee->get_class();
          if (refl_map.count(callee_method_cls) == 0) return;
          const auto* classname = m->get_class()->get_name();
          TRACE(RENAME, 4,
                "Found %s with known reflection usage. marking reachable",
                classname->c_str());
          dont_rename_class_for_types_with_reflection.insert(classname);
        }
      });
  return dont_rename_class_for_types_with_reflection;
}

std::unordered_set<const DexString*>
RenameClassesPassV2::build_dont_rename_canaries(Scope& scope) {
  std::unordered_set<const DexString*> dont_rename_canaries;
  // Gather canaries
  for (auto clazz : scope) {
    if (strstr(clazz->get_name()->c_str(), "/Canary")) {
      dont_rename_canaries.insert(clazz->get_name());
    }
  }
  return dont_rename_canaries;
}

std::unordered_set<const DexType*>
RenameClassesPassV2::build_force_rename_hierarchies(
    PassManager& mgr, Scope& scope, const ClassHierarchy& class_hierarchy) {
  std::unordered_set<const DexType*> force_rename_hierarchies;
  std::vector<DexClass*> base_classes;
  for (const auto& base : m_force_rename_hierarchies) {
    // skip comments
    if (base.c_str()[0] == '#') continue;
    auto base_type = DexType::get_type(base);
    if (base_type != nullptr) {
      DexClass* base_class = type_class(base_type);
      if (!base_class) {
        TRACE(RENAME, 2, "Can't find class for force_rename_hierachy rule %s",
              base.c_str());
        mgr.incr_metric(METRIC_MISSING_HIERARCHY_CLASSES, 1);
      } else {
        base_classes.emplace_back(base_class);
      }
    } else {
      TRACE(RENAME, 2, "Can't find type for force_rename_hierachy rule %s",
            base.c_str());
      mgr.incr_metric(METRIC_MISSING_HIERARCHY_TYPES, 1);
    }
  }
  for (const auto& base_class : base_classes) {
    force_rename_hierarchies.insert(base_class->get_type());
    TypeSet children_and_implementors;
    get_all_children_or_implementors(class_hierarchy, scope, base_class,
                                     children_and_implementors);
    for (const auto& cls : children_and_implementors) {
      force_rename_hierarchies.insert(cls);
    }
  }
  return force_rename_hierarchies;
}

std::unordered_map<const DexType*, const DexString*>
RenameClassesPassV2::build_dont_rename_hierarchies(
    PassManager& mgr, Scope& scope, const ClassHierarchy& class_hierarchy) {
  std::unordered_map<const DexType*, const DexString*> dont_rename_hierarchies;
  std::vector<DexClass*> base_classes;
  for (const auto& base : m_dont_rename_hierarchies) {
    // skip comments
    if (base.c_str()[0] == '#') continue;
    auto base_type = DexType::get_type(base);
    if (base_type != nullptr) {
      DexClass* base_class = type_class(base_type);
      if (!base_class) {
        TRACE(RENAME, 2, "Can't find class for dont_rename_hierachy rule %s",
              base.c_str());
        mgr.incr_metric(METRIC_MISSING_HIERARCHY_CLASSES, 1);
      } else {
        base_classes.emplace_back(base_class);
      }
    } else {
      TRACE(RENAME, 2, "Can't find type for dont_rename_hierachy rule %s",
            base.c_str());
      mgr.incr_metric(METRIC_MISSING_HIERARCHY_TYPES, 1);
    }
  }
  for (const auto& base_class : base_classes) {
    const auto* base_name = base_class->get_name();
    dont_rename_hierarchies[base_class->get_type()] = base_name;
    TypeSet children_and_implementors;
    get_all_children_or_implementors(class_hierarchy, scope, base_class,
                                     children_and_implementors);
    for (const auto& cls : children_and_implementors) {
      dont_rename_hierarchies[cls] = base_name;
    }
  }
  return dont_rename_hierarchies;
}

std::unordered_set<const DexType*>
RenameClassesPassV2::build_dont_rename_serde_relationships(Scope& scope) {
  std::unordered_set<const DexType*> dont_rename_serde_relationships;
  for (const auto& cls : scope) {
    klass::Serdes cls_serdes = klass::get_serdes(cls);
    std::string name = cls->get_name()->str_copy();
    name.pop_back();

    // Look for a class that matches one of the two deserializer patterns
    DexType* deser = cls_serdes.get_deser();
    DexType* flatbuf_deser = cls_serdes.get_flatbuf_deser();
    bool has_deser_finder = false;

    if (deser || flatbuf_deser) {
      for (const auto& method : cls->get_dmethods()) {
        if (!strcmp("$$getDeserializerClass", method->get_name()->c_str())) {
          has_deser_finder = true;
          break;
        }
      }
    }

    // Look for a class that matches one of the two serializer patterns
    DexType* ser = cls_serdes.get_ser();
    DexType* flatbuf_ser = cls_serdes.get_flatbuf_ser();
    bool has_ser_finder = false;

    if (ser || flatbuf_ser) {
      for (const auto& method : cls->get_dmethods()) {
        if (!strcmp("$$getSerializerClass", method->get_name()->c_str())) {
          has_ser_finder = true;
          break;
        }
      }
    }

    bool dont_rename = ((deser || flatbuf_deser) && !has_deser_finder) ||
                       ((ser || flatbuf_ser) && !has_ser_finder);

    if (dont_rename) {
      dont_rename_serde_relationships.insert(cls->get_type());
      if (deser) dont_rename_serde_relationships.insert(deser);
      if (flatbuf_deser) dont_rename_serde_relationships.insert(flatbuf_deser);
      if (ser) dont_rename_serde_relationships.insert(ser);
      if (flatbuf_ser) dont_rename_serde_relationships.insert(flatbuf_ser);
    }
  }

  return dont_rename_serde_relationships;
}

std::unordered_set<const DexType*>
RenameClassesPassV2::build_dont_rename_native_bindings(Scope& scope) {
  std::unordered_set<const DexType*> dont_rename_native_bindings;
  // find all classes with native methods, and all types mentioned
  // in protos of native methods
  for (auto clazz : scope) {
    for (auto meth : clazz->get_dmethods()) {
      if (is_native(meth)) {
        dont_rename_native_bindings.insert(clazz->get_type());
        auto proto = meth->get_proto();
        auto rtype = proto->get_rtype();
        dont_rename_native_bindings.insert(rtype);
        for (auto ptype : *proto->get_args()) {
          dont_rename_native_bindings.insert(
              type::get_element_type_if_array(ptype));
        }
      }
    }
    for (auto meth : clazz->get_vmethods()) {
      if (is_native(meth)) {
        dont_rename_native_bindings.insert(clazz->get_type());
        auto proto = meth->get_proto();
        auto rtype = proto->get_rtype();
        dont_rename_native_bindings.insert(rtype);
        for (auto ptype : *proto->get_args()) {
          dont_rename_native_bindings.insert(
              type::get_element_type_if_array(ptype));
        }
      }
    }
  }
  return dont_rename_native_bindings;
}

std::unordered_set<const DexType*>
RenameClassesPassV2::build_dont_rename_annotated() {
  std::unordered_set<const DexType*> dont_rename_annotated;
  for (const auto& annotation : m_dont_rename_annotated) {
    DexType* anno = DexType::get_type(annotation);
    if (anno) {
      dont_rename_annotated.insert(anno);
    }
  }
  return dont_rename_annotated;
}

static void sanity_check(const Scope& scope,
                         const rewriter::TypeStringMap& name_mapping) {
  std::vector<std::string> external_names_vec;
  // Class.forName() expects strings of the form "foo.bar.Baz". We should be
  // very suspicious if we see these strings in the string pool that
  // correspond to the old name of a class that we have renamed...
  for (const auto& it : name_mapping.get_class_map()) {
    external_names_vec.push_back(
        java_names::internal_to_external(it.first->str()));
  }
  std::unordered_set<std::string_view> external_names;
  for (auto& s : external_names_vec) {
    external_names.insert(s);
  }
  std::unordered_set<const DexString*> all_strings;
  for (auto clazz : scope) {
    clazz->gather_strings(all_strings);
  }
  int sketchy_strings = 0;
  for (auto s : all_strings) {
    if (external_names.find(s->str()) != external_names.end() ||
        name_mapping.get_new_type_name(s)) {
      TRACE(RENAME, 2, "Found %s in string pool after renaming", s->c_str());
      sketchy_strings++;
    }
  }
  if (sketchy_strings > 0) {
    fprintf(stderr,
            "WARNING: Found a number of sketchy class-like strings after class "
            "renaming. Re-run with TRACE=RENAME:2 for more details.\n");
  }
}

std::string get_keep_rule(const DexClass* clazz) {
  if (keep_reason::Reason::record_keep_reasons()) {
    const auto& keep_reasons = clazz->rstate.keep_reasons();
    for (const auto* reason : keep_reasons) {
      if (reason->type == keep_reason::KEEP_RULE &&
          !reason->keep_rule->allowobfuscation) {
        return show(*reason);
      }
    }
  }
  return "";
}

void RenameClassesPassV2::eval_classes(Scope& scope,
                                       const ClassHierarchy& class_hierarchy,
                                       ConfigFiles& conf,
                                       bool rename_annotations,
                                       PassManager& mgr) {
  std::unordered_set<const DexType*> force_rename_hierarchies;
  std::unordered_set<const DexType*> dont_rename_serde_relationships;
  std::unordered_set<std::string> dont_rename_class_name_literals;
  std::unordered_set<const DexString*>
      dont_rename_class_for_types_with_reflection;
  std::unordered_set<const DexString*> dont_rename_canaries;
  std::unordered_map<const DexType*, const DexString*> dont_rename_hierarchies;
  std::unordered_set<const DexType*> dont_rename_native_bindings;
  std::unordered_set<const DexType*> dont_rename_annotated;

  std::vector<std::function<void()>> fns{
      [&] {
        force_rename_hierarchies =
            build_force_rename_hierarchies(mgr, scope, class_hierarchy);
      },
      [&] {
        dont_rename_serde_relationships =
            build_dont_rename_serde_relationships(scope);
      },
      [&] {
        dont_rename_class_name_literals =
            build_dont_rename_class_name_literals(scope);
      },
      [&] {
        dont_rename_class_for_types_with_reflection =
            build_dont_rename_for_types_with_reflection(
                scope, conf.get_proguard_map());
      },
      [&] { dont_rename_canaries = build_dont_rename_canaries(scope); },
      [&] {
        dont_rename_hierarchies =
            build_dont_rename_hierarchies(mgr, scope, class_hierarchy);
      },
      [&] {
        dont_rename_native_bindings = build_dont_rename_native_bindings(scope);
      },
      [&] { dont_rename_annotated = build_dont_rename_annotated(); }};

  workqueue_run<std::function<void()>>(
      [](const std::function<void()>& fn) { fn(); }, fns);

  std::string norule;

  auto on_class_renamable = [&](DexClass* clazz) {
    if (referenced_by_layouts(clazz)) {
      m_renamable_layout_classes.emplace(clazz->get_name());
    }
  };

  for (auto clazz : scope) {
    // Short circuit force renames
    if (force_rename_hierarchies.count(clazz->get_type())) {
      clazz->rstate.set_force_rename();
      on_class_renamable(clazz);
      continue;
    }

    // Don't rename annotations
    if (!rename_annotations && is_annotation(clazz)) {
      clazz->rstate.set_dont_rename();
      m_dont_rename_reasons[clazz] = {DontRenameReasonCode::Annotations,
                                      norule};
      continue;
    }

    // Don't rename types annotated with anything in dont_rename_annotated
    bool annotated = false;
    for (const auto& anno : dont_rename_annotated) {
      if (has_anno(clazz, anno)) {
        clazz->rstate.set_dont_rename();
        m_dont_rename_reasons[clazz] = {DontRenameReasonCode::Annotated,
                                        anno->str_copy()};
        annotated = true;
        break;
      }
    }
    if (annotated) continue;

    // Don't rename anything mentioned in resources. Two variants of checks here
    // to cover both configuration options (either we're relying on aapt to
    // compute resource reachability, or we're doing it ourselves).
    if (referenced_by_layouts(clazz) &&
        !is_allowed_layout_class(clazz, m_allow_layout_rename_packages)) {
      clazz->rstate.set_dont_rename();
      m_dont_rename_reasons[clazz] = {DontRenameReasonCode::Resources, norule};
      continue;
    }

    std::string strname = clazz->get_name()->str_copy();

    // Don't rename anythings in the direct name blocklist (hierarchy ignored)
    if (m_dont_rename_specific.count(strname)) {
      clazz->rstate.set_dont_rename();
      m_dont_rename_reasons[clazz] = {DontRenameReasonCode::Specific,
                                      std::move(strname)};
      continue;
    }

    // Don't rename anything if it falls in an excluded package
    bool package_blocklisted = false;
    for (const auto& pkg : m_dont_rename_packages) {
      if (strname.rfind("L" + pkg) == 0) {
        TRACE(RENAME, 2, "%s excluded by pkg rule %s", strname.c_str(),
              pkg.c_str());
        clazz->rstate.set_dont_rename();
        m_dont_rename_reasons[clazz] = {DontRenameReasonCode::Packages, pkg};
        package_blocklisted = true;
        break;
      }
    }
    if (package_blocklisted) continue;

    if (dont_rename_class_name_literals.count(strname)) {
      clazz->rstate.set_dont_rename();
      m_dont_rename_reasons[clazz] = {DontRenameReasonCode::ClassNameLiterals,
                                      norule};
      continue;
    }

    if (dont_rename_class_for_types_with_reflection.count(clazz->get_name())) {
      clazz->rstate.set_dont_rename();
      m_dont_rename_reasons[clazz] = {
          DontRenameReasonCode::ClassForTypesWithReflection, norule};
      continue;
    }

    if (dont_rename_canaries.count(clazz->get_name())) {
      clazz->rstate.set_dont_rename();
      m_dont_rename_reasons[clazz] = {DontRenameReasonCode::Canaries, norule};
      continue;
    }

    if (dont_rename_native_bindings.count(clazz->get_type())) {
      clazz->rstate.set_dont_rename();
      m_dont_rename_reasons[clazz] = {DontRenameReasonCode::NativeBindings,
                                      norule};
      continue;
    }

    if (dont_rename_hierarchies.count(clazz->get_type())) {
      const auto* rule = dont_rename_hierarchies[clazz->get_type()];
      clazz->rstate.set_dont_rename();
      m_dont_rename_reasons[clazz] = {DontRenameReasonCode::Hierarchy,
                                      rule->str_copy()};
      continue;
    }

    if (dont_rename_serde_relationships.count(clazz->get_type())) {
      clazz->rstate.set_dont_rename();
      m_dont_rename_reasons[clazz] = {DontRenameReasonCode::SerdeRelationships,
                                      norule};
      continue;
    }

    if (!can_rename_if_also_renaming_xml(clazz)) {
      const auto& keep_reasons = clazz->rstate.keep_reasons();
      auto rule = !keep_reasons.empty() ? show(*keep_reasons.begin()) : "";
      clazz->rstate.set_dont_rename();
      m_dont_rename_reasons[clazz] = {DontRenameReasonCode::ProguardCantRename,
                                      get_keep_rule(clazz)};
      continue;
    }

    // All above checks have passed, callback for a type which appears to be
    // renamable.
    on_class_renamable(clazz);
  }
}

/**
 * We re-evaluate a number of config rules again at pass running time.
 * The reason is that the types specified in those rules can be created in
 * previous Redex passes and did not exist when the initial evaluation happened.
 */
void RenameClassesPassV2::eval_classes_post(
    Scope& scope, const ClassHierarchy& class_hierarchy, PassManager& mgr) {
  Timer t("eval_classes_post");
  auto dont_rename_hierarchies =
      build_dont_rename_hierarchies(mgr, scope, class_hierarchy);
  for (auto clazz : scope) {
    if (m_dont_rename_reasons.find(clazz) != m_dont_rename_reasons.end()) {
      continue;
    }

    std::string strname = clazz->get_name()->str_copy();

    // Don't rename anythings in the direct name blocklist (hierarchy ignored)
    if (m_dont_rename_specific.count(strname)) {
      m_dont_rename_reasons[clazz] = {DontRenameReasonCode::Specific,
                                      std::move(strname)};
      continue;
    }

    // Don't rename anything if it falls in an excluded package
    bool package_blocklisted = false;
    for (const auto& pkg : m_dont_rename_packages) {
      if (strname.rfind("L" + pkg) == 0) {
        TRACE(RENAME, 2, "%s excluded by pkg rule %s",
              str_copy(strname).c_str(), pkg.c_str());
        m_dont_rename_reasons[clazz] = {DontRenameReasonCode::Packages, pkg};
        package_blocklisted = true;
        break;
      }
    }
    if (package_blocklisted) continue;

    if (dont_rename_hierarchies.count(clazz->get_type())) {
      const auto* rule = dont_rename_hierarchies[clazz->get_type()];
      m_dont_rename_reasons[clazz] = {DontRenameReasonCode::Hierarchy,
                                      rule->str_copy()};
      continue;
    }

    // Don't rename anything if something changed and the class cannot be
    // renamed anymore.
    if (!can_rename_if_also_renaming_xml(clazz)) {
      m_dont_rename_reasons[clazz] = {DontRenameReasonCode::ProguardCantRename,
                                      get_keep_rule(clazz)};
    }
  }
}

void RenameClassesPassV2::eval_pass(DexStoresVector& stores,
                                    ConfigFiles& conf,
                                    PassManager& mgr) {
  const auto& json = conf.get_json_config();
  json.get("apk_dir", "", m_apk_dir);
  TRACE(RENAME, 3, "APK Dir: %s", m_apk_dir.c_str());
  auto scope = build_class_scope(stores);
  ClassHierarchy class_hierarchy = build_type_hierarchy(scope);
  eval_classes(scope, class_hierarchy, conf, m_rename_annotations, mgr);
}

std::unordered_set<DexClass*> RenameClassesPassV2::get_renamable_classes(
    Scope& scope) {
  std::unordered_set<DexClass*> renamable_classes;
  for (auto clazz : scope) {
    if (clazz->rstate.is_force_rename() ||
        !m_dont_rename_reasons.count(clazz)) {
      renamable_classes.insert(clazz);
    }
  }
  return renamable_classes;
}

void RenameClassesPassV2::rename_classes(
    Scope& scope,
    size_t digits,
    const std::unordered_set<DexClass*>& renamable_classes,
    PassManager& mgr) {
  rewriter::TypeStringMap name_mapping;
  uint32_t sequence = 0;
  for (auto clazz : scope) {
    auto dtype = clazz->get_type();
    auto oldname = dtype->get_name();

    if (clazz->rstate.is_force_rename()) {
      mgr.incr_metric(METRIC_FORCE_RENAMED_CLASSES, 1);
      TRACE(RENAME, 2, "Forced renamed: '%s'", oldname->c_str());
    } else if (!clazz->rstate.is_renamable_initialized_and_renamable() ||
               clazz->rstate.is_generated()) {
      // Either cls is not renamble, or it is a Redex newly generated class.
      if (m_dont_rename_reasons.find(clazz) != m_dont_rename_reasons.end()) {
        auto reason = m_dont_rename_reasons[clazz];
        std::string metric = dont_rename_reason_to_metric(reason.code);
        mgr.incr_metric(metric, 1);
        if (dont_rename_reason_to_metric_per_rule(reason.code)) {
          std::string str = metric + "::" + std::string(reason.rule);
          mgr.incr_metric(str, 1);
          TRACE(RENAME, 2, "'%s' NOT RENAMED due to %s'", oldname->c_str(),
                str.c_str());
        } else {
          TRACE(RENAME, 2, "'%s' NOT RENAMED due to %s'", oldname->c_str(),
                metric.c_str());
        }
        sequence++;
        always_assert(!renamable_classes.count(clazz));
        continue;
      }
    }

    always_assert(renamable_classes.count(clazz));
    mgr.incr_metric(METRIC_RENAMED_CLASSES, 1);

    std::array<char, Locator::encoded_global_class_index_max> array;
    char* descriptor = array.data();
    always_assert(sequence != Locator::invalid_global_class_index);
    Locator::encodeGlobalClassIndex(sequence, digits, descriptor);
    always_assert_log(facebook::Locator::decodeGlobalClassIndex(descriptor) ==
                          sequence,
                      "global class index didn't roundtrip; %s generated from "
                      "%u parsed to %u",
                      descriptor, sequence,
                      facebook::Locator::decodeGlobalClassIndex(descriptor));

    sequence++;

    std::string prefixed_descriptor = prepend_package_prefix(descriptor);

    TRACE(RENAME, 2, "'%s' ->  %s (%u)'", oldname->c_str(),
          prefixed_descriptor.c_str(), sequence);

    auto dstring = DexString::make_string(prefixed_descriptor);

    always_assert_log(!DexType::get_type(dstring),
                      "Type name collision detected. %s already exists.",
                      prefixed_descriptor.c_str());

    name_mapping.add_type_name(clazz->get_name(), dstring);
    dtype->set_name(dstring);
    m_base_strings_size += oldname->size();
    m_ren_strings_size += dstring->size();

    while (1) {
      std::string arrayop("[");
      arrayop += oldname->str();
      oldname = DexString::get_string(arrayop);
      if (oldname == nullptr) {
        break;
      }
      auto arraytype = DexType::get_type(oldname);
      if (arraytype == nullptr) {
        break;
      }
      std::string newarraytype("[");
      newarraytype += dstring->str();
      dstring = DexString::make_string(newarraytype);
      arraytype->set_name(dstring);
    }
  }

  /* Now rewrite all const-string strings for force renamed classes. */
  rewriter::TypeStringMap force_rename_map;
  for (const auto& pair : name_mapping.get_class_map()) {
    auto type = DexType::get_type(pair.first);
    if (!type) {
      continue;
    }
    auto clazz = type_class(type);
    if (clazz && clazz->rstate.is_force_rename()) {
      force_rename_map.add_type_name(pair.first, pair.second);
    }
  }
  auto updated_instructions =
      rewriter::rewrite_string_literal_instructions(scope, force_rename_map);
  mgr.incr_metric(METRIC_REWRITTEN_CONST_STRINGS, updated_instructions);

  /* Now we need to re-write the Signature annotations.  They use
   * Strings rather than Type's, so they have to be explicitly
   * handled.
   */
  rewrite_dalvik_annotation_signature(scope, name_mapping);

  rename_classes_in_layouts(name_mapping, mgr);

  sanity_check(scope, name_mapping);
}

void RenameClassesPassV2::rename_classes_in_layouts(
    const rewriter::TypeStringMap& name_mapping, PassManager& mgr) {
  // Sync up ResStringPool entries in XML layouts. Class names should appear
  // in their "external" name, i.e. java.lang.String instead of
  // Ljava/lang/String;
  std::map<std::string, std::string> rename_map_for_layouts;
  for (auto&& [old_name, new_name] : name_mapping.get_class_map()) {
    // Application should be configuring specific packages/class names to
    // prevent collisions/accidental rewrites of unrelated xml
    // elements/attributes/values; filter the given map to only be known View
    // classes.
    if (m_renamable_layout_classes.count(old_name) > 0) {
      rename_map_for_layouts.emplace(
          java_names::internal_to_external(old_name->str()),
          java_names::internal_to_external(new_name->str()));
    }
  }
  auto resources = create_resource_reader(m_apk_dir);
  resources->rename_classes_in_layouts(rename_map_for_layouts);
}

std::string RenameClassesPassV2::prepend_package_prefix(
    const char* descriptor) {
  always_assert_log(*descriptor == 'L',
                    "Class descriptor \"%s\" did not start with L!\n",
                    descriptor);
  descriptor++; // drop letter 'L'

  std::stringstream ss;
  ss << "L" << m_package_prefix << descriptor;
  return ss.str();
}

std::unordered_set<DexClass*> RenameClassesPassV2::get_renamable_classes(
    Scope& scope, ConfigFiles& conf, PassManager& mgr) {
  if (!m_package_prefix.empty()) {
    always_assert_log(
        !(conf.get_json_config().get("emit_locator_strings", false)),
        "Rename classes package_prefix doesn't work together with "
        "emit_locator_strings.\n");
  }

  ClassHierarchy class_hierarchy = build_type_hierarchy(scope);
  eval_classes_post(scope, class_hierarchy, mgr);

  always_assert_log(scope.size() <
                        std::pow(Locator::global_class_index_digits_base,
                                 Locator::global_class_index_digits_max),
                    "scope size %zu too large", scope.size());
  int total_classes = scope.size();
  mgr.incr_metric(METRIC_CLASSES_IN_SCOPE, total_classes);

  return get_renamable_classes(scope);
}

void RenameClassesPassV2::run_pass(DexStoresVector& stores,
                                   ConfigFiles& conf,
                                   PassManager& mgr) {
  auto scope = build_class_scope(stores);

  // encode the whole sequence as base 62: [0 - 9], [A - Z], [a - z]
  auto digits =
      (size_t)std::ceil(std::log(scope.size()) /
                        std::log(Locator::global_class_index_digits_base));
  TRACE(RENAME, 1,
        "Total classes in scope for renaming: %zu chosen number of digits: %zu",
        scope.size(), digits);

  rename_classes(scope, digits, get_renamable_classes(scope, conf, mgr), mgr);

  mgr.incr_metric(METRIC_DIGITS, digits);

  TRACE(RENAME, 1, "String savings, at least %d-%d = %d bytes ",
        m_base_strings_size, m_ren_strings_size,
        m_base_strings_size - m_ren_strings_size);
}
