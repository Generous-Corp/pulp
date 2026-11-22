#pragma once

// Read a product test seam only in builds that explicitly enable them. Release
// binaries compile the expression to nullptr, so inherited environment values
// cannot alter shipped behaviour.
#include <cstdlib>

#if defined(PULP_ENABLE_TEST_SEAMS) && PULP_ENABLE_TEST_SEAMS
#define PULP_TEST_ENV(name) (::std::getenv(name))
#else
#define PULP_TEST_ENV(name) (nullptr)
#endif
