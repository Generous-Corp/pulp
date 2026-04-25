cmake_minimum_required(VERSION 3.18)
project(DemoFetchPin LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(pulp
  GIT_REPOSITORY https://github.com/danielraffel/pulp.git
  GIT_TAG v0.30.0)
FetchContent_MakeAvailable(pulp)
