/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "GlobalConfig.h"

namespace resources {
// TODO: move away from this function. It does not take into consideration R
// classes that were created via Buck codegen or otherwise specified in the
// Redex config.
bool is_r_class(const DexClass* cls);

// Helper class for reading autogenerated R class fields and clinit.
class RClassReader {
 public:
  explicit RClassReader(const ResourceConfig& global_resources_config)
      : m_global_resources_config(global_resources_config) {}

  explicit RClassReader(const GlobalConfig& global_config)
      : m_global_resources_config(
            *global_config.get_config_by_name<ResourceConfig>("resources")) {}

  bool is_r_class(const DexClass* cls) const;

  void extract_resource_ids_from_static_arrays(
      const std::unordered_set<DexField*>& array_fields,
      std::unordered_set<uint32_t>* out_values) const;

 private:
  const ResourceConfig m_global_resources_config;
};
} // namespace resources