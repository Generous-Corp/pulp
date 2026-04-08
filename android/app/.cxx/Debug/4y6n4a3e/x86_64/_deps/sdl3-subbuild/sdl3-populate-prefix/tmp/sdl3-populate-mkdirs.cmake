# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/x86_64/_deps/sdl3-src")
  file(MAKE_DIRECTORY "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/x86_64/_deps/sdl3-src")
endif()
file(MAKE_DIRECTORY
  "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/x86_64/_deps/sdl3-build"
  "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/x86_64/_deps/sdl3-subbuild/sdl3-populate-prefix"
  "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/x86_64/_deps/sdl3-subbuild/sdl3-populate-prefix/tmp"
  "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/x86_64/_deps/sdl3-subbuild/sdl3-populate-prefix/src/sdl3-populate-stamp"
  "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/x86_64/_deps/sdl3-subbuild/sdl3-populate-prefix/src"
  "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/x86_64/_deps/sdl3-subbuild/sdl3-populate-prefix/src/sdl3-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/x86_64/_deps/sdl3-subbuild/sdl3-populate-prefix/src/sdl3-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/danielraffel/Code/pulp-android/android/app/.cxx/Debug/4y6n4a3e/x86_64/_deps/sdl3-subbuild/sdl3-populate-prefix/src/sdl3-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
