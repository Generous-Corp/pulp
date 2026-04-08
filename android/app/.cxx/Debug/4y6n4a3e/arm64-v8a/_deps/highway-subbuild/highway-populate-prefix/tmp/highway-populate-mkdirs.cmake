# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/arm64-v8a/_deps/highway-src")
  file(MAKE_DIRECTORY "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/arm64-v8a/_deps/highway-src")
endif()
file(MAKE_DIRECTORY
  "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/arm64-v8a/_deps/highway-build"
  "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/arm64-v8a/_deps/highway-subbuild/highway-populate-prefix"
  "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/arm64-v8a/_deps/highway-subbuild/highway-populate-prefix/tmp"
  "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/arm64-v8a/_deps/highway-subbuild/highway-populate-prefix/src/highway-populate-stamp"
  "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/arm64-v8a/_deps/highway-subbuild/highway-populate-prefix/src"
  "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/arm64-v8a/_deps/highway-subbuild/highway-populate-prefix/src/highway-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/arm64-v8a/_deps/highway-subbuild/highway-populate-prefix/src/highway-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/arm64-v8a/_deps/highway-subbuild/highway-populate-prefix/src/highway-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
