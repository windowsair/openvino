// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "behavior/caching/caching_tests.hpp"

using namespace LayerTestsDefinitions;

namespace {
    static const std::vector<ov::element::Type> precisionsTemplate = {
            ov::element::f32,
    };

    static const std::vector<std::size_t> batchSizesTemplate = {
            1, 2
    };

    INSTANTIATE_TEST_SUITE_P(smoke_Behavior_CachingSupportCase_Template, LoadNetworkCacheTestBase,
                            ::testing::Combine(
                                    ::testing::ValuesIn(LoadNetworkCacheTestBase::getStandardFunctions()),
                                    ::testing::ValuesIn(precisionsTemplate),
                                    ::testing::ValuesIn(batchSizesTemplate),
                                    ::testing::Values(CommonTestUtils::DEVICE_TEMPLATE)),
                            LoadNetworkCacheTestBase::getTestCaseName);
} // namespace
