cmake_minimum_required(VERSION 3.18)
project(DemoBranchPin LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(pulp
  GIT_REPOSITORY https://github.com/danielraffel/pulp.git
  GIT_TAG main)
FetchContent_MakeAvailable(pulp)
