/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * The IntraDexClassMergingPass runs after InterDexPass, it should never
 * introduce more type/method/field references than dex limit.
 * TODO: It now requires inliner to inline the merge virtual methods to
 * eliminate the extra method refs and it creates type tag fields.
 */

#pragma once

#include "InterDex.h"
#include "InterDexReshuffleImpl.h"
#include "Model.h"
#include "Pass.h"

namespace class_merging {

class IntraDexClassMergingPass : public Pass {
 public:
  IntraDexClassMergingPass() : Pass("IntraDexClassMergingPass") {}

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::interactions;
    using namespace redex_properties::names;
    return {
        {DexLimitsObeyed, Preserves},
        {NoResolvablePureRefs, Preserves},
        {InitialRenameClass, Preserves},
    };
  }

  void bind_config() override;

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

 private:
  ModelSpec m_merging_spec;
  bool m_enable_reshuffle;
  bool m_enable_mergeability_aware_reshuffle;
  ReshuffleConfig m_reshuffle_config;
  size_t m_global_min_count;
};

} // namespace class_merging
