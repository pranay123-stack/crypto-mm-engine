cmake_minimum_required(VERSION 3.24)

project(crypto_mm_engine
        VERSION 0.1.0
        DESCRIPTION "Institutional crypto market-making execution platform"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MMX_WERROR      "Treat warnings as errors"                     ON)
option(MMX_NATIVE_ARCH "Compile for the host microarchitecture"       OFF)
option(MMX_LTO         "Link-time optimization for Release"           OFF)
option(MMX_ASAN        "Address + UB sanitizers"                      OFF)
option(MMX_TSAN        "Thread sanitizer"                             OFF)
option(MMX_BUILD_TESTS "Build the test suites"                        ON)
option(MMX_BUILD_BENCH "Build the benchmarks"                         ON)

if(MMX_ASAN AND MMX_TSAN)
  message(FATAL_ERROR "MMX_ASAN and MMX_TSAN are mutually exclusive")
endif()

include(GNUInstallDirs)
include(CheckCXXCompilerFlag)

# ---------------------------------------------------------------------------
# Shared compile flags
# ---------------------------------------------------------------------------
add_library(mm_flags INTERFACE)

target_compile_options(mm_flags INTERFACE
  -Wall -Wextra -Wpedantic
  -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Woverloaded-virtual
  -Wcast-align -Wcast-qual -Wdouble-promotion
  -Wformat=2 -Wimplicit-fallthrough -Wold-style-cast
  -Wunused -Wnull-dereference -Wextra-semi
  $<$<BOOL:${MMX_WERROR}>:-Werror>
)

target_compile_definitions(mm_flags INTERFACE
  $<$<CONFIG:Debug>:MMX_DEBUG=1>
  $<$<NOT:$<CONFIG:Debug>>:NDEBUG=1>
)

if(MMX_NATIVE_ARCH)
  check_cxx_compiler_flag(-march=native MMX_HAS_MARCH_NATIVE)
  if(MMX_HAS_MARCH_NATIVE)
    target_compile_options(mm_flags INTERFACE
      $<$<NOT:$<CONFIG:Debug>>:-march=native -mtune=native>)
  endif()
endif()

if(MMX_ASAN)
  target_compile_options(mm_flags INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(mm_flags INTERFACE -fsanitize=address,undefined)
endif()

if(MMX_TSAN)
  # GCC's thread sanitizer cannot model std::atomic_thread_fence, which the
  # seqlock depends on; it rejects the translation unit outright. Fail here
  # rather than let someone believe they ran a clean sanitized build.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
      "MMX_TSAN requires Clang: GCC's -fsanitize=thread does not support "
      "atomic_thread_fence. Re-run with -DCMAKE_CXX_COMPILER=clang++")
  endif()
  target_compile_options(mm_flags INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(mm_flags INTERFACE -fsanitize=thread)
endif()

if(MMX_LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# ---------------------------------------------------------------------------
# Dependencies
#
# Vendored under third_party/ so the build is reproducible and offline-capable.
# System packages are used where the distribution ships a suitable version.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)

# Boost: header-only Asio/Beast are all we need at compile time.
set(Boost_USE_STATIC_LIBS OFF)
find_package(Boost 1.74 REQUIRED)

find_path(MMX_NLOHMANN_INCLUDE_DIR nlohmann/json.hpp REQUIRED)

set(MMX_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# fmt, spdlog, yaml-cpp and GoogleTest are pinned submodules. An un-initialised
# submodule otherwise surfaces as a baffling "add_subdirectory given source
# directory which has no CMakeLists.txt" several lines later.
foreach(_dep fmt spdlog yaml-cpp googletest)
  if(NOT EXISTS "${MMX_THIRD_PARTY}/${_dep}/CMakeLists.txt")
    message(FATAL_ERROR
      "third_party/${_dep} is empty - the vendored dependencies are git submodules.\n"
      "Run:  git submodule update --init --recursive")
  endif()
endforeach()

set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${MMX_THIRD_PARTY}/fmt" EXCLUDE_FROM_ALL)

set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${MMX_THIRD_PARTY}/spdlog" EXCLUDE_FROM_ALL)

set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
add_subdirectory("${MMX_THIRD_PARTY}/yaml-cpp" EXCLUDE_FROM_ALL)

# Third-party headers must not be compiled against our warning set.
foreach(dep fmt spdlog yaml-cpp)
  if(TARGET ${dep})
    set_target_properties(${dep} PROPERTIES SYSTEM ON)
  endif()
endforeach()

# Boost and nlohmann come in as SYSTEM include dirs for the same reason.
add_library(mm_thirdparty_headers INTERFACE)
target_include_directories(mm_thirdparty_headers SYSTEM INTERFACE
  ${Boost_INCLUDE_DIRS}
  ${MMX_NLOHMANN_INCLUDE_DIR}
)

# ---------------------------------------------------------------------------
# Helper: declare a module library with the house defaults
# ---------------------------------------------------------------------------
function(mmx_add_module name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;PRIVATE_DEPS" ${ARGN})
  add_library(${name} STATIC ${ARG_SOURCES})
  target_include_directories(${name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(${name} PUBLIC mm_flags ${ARG_DEPS})
  if(ARG_PRIVATE_DEPS)
    target_link_libraries(${name} PRIVATE ${ARG_PRIVATE_DEPS})
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Module graph  (see docs/architecture.md §2 — dependencies point downward)
# ---------------------------------------------------------------------------

# L0 — common: types, time, ids, status, config, logging, metrics, lock-free
file(GLOB_RECURSE MM_COMMON_SRC CONFIGURE_DEPENDS "src/common/*.cpp")
mmx_add_module(mm_common
  SOURCES ${MM_COMMON_SRC}
  DEPS    Threads::Threads spdlog::spdlog fmt::fmt yaml-cpp::yaml-cpp
)

# L0a - exchange interfaces and normalized types.
#
# Links mm_common and NOTHING ELSE. That single line is what keeps venue
# details out of the core: an adapter cannot become a dependency of the
# interfaces it implements.
file(GLOB_RECURSE MM_EXCHANGE_SRC CONFIGURE_DEPENDS "src/exchange/common/*.cpp")
mmx_add_module(mm_exchange
  SOURCES ${MM_EXCHANGE_SRC}
  DEPS    mm_common
)

# L0b - local order book and venue-agnostic snapshot/delta synchronization.
file(GLOB_RECURSE MM_ORDERBOOK_SRC CONFIGURE_DEPENDS "src/orderbook/*.cpp")
mmx_add_module(mm_orderbook
  SOURCES ${MM_ORDERBOOK_SRC}
  DEPS    mm_exchange
)

# L1 - strategy runtime. Links mm_orderbook and mm_common and NOTHING ELSE.
# That single line is what makes "a strategy cannot reach the exchange"
# structural: there is no adapter, no Asio, no OpenSSL and no JSON in its
# dependency closure to reach through.
file(GLOB_RECURSE MM_STRATEGY_SRC CONFIGURE_DEPENDS "src/strategy/*.cpp")
mmx_add_module(mm_strategy
  SOURCES ${MM_STRATEGY_SRC}
  DEPS    mm_orderbook
)

# L2 - quote manager. Links mm_strategy (for QuoteIntent) and mm_exchange (for
# validation and capabilities). No adapter, no risk, no OMS.
file(GLOB_RECURSE MM_QUOTE_SRC CONFIGURE_DEPENDS "src/quote/*.cpp")
mmx_add_module(mm_quote
  SOURCES ${MM_QUOTE_SRC}
  DEPS    mm_strategy
)

# L3 - risk engine. Links mm_quote (for OrderAction) and through it mm_exchange
# for venue rules. No adapter, no OMS, no strategy implementation.
file(GLOB_RECURSE MM_RISK_SRC CONFIGURE_DEPENDS "src/risk/*.cpp")
mmx_add_module(mm_risk
  SOURCES ${MM_RISK_SRC}
  DEPS    mm_quote
)

# L4 - order management. Links mm_risk (for the approval token) and through it
# mm_quote and mm_exchange. No adapter, no strategy implementation.
file(GLOB_RECURSE MM_OMS_SRC CONFIGURE_DEPENDS "src/oms/*.cpp")
mmx_add_module(mm_oms
  SOURCES ${MM_OMS_SRC}
  DEPS    mm_risk
)

# Strategy plug-ins.
#
# An OBJECT library, not a static one: a self-registering translation unit that
# nothing references is dead-stripped out of a static archive, and the strategy
# then silently vanishes from the registry at link time. Object libraries are
# linked whole. tests/strategy_swap asserts the registry is actually populated.
file(GLOB_RECURSE MM_STRATEGIES_SRC CONFIGURE_DEPENDS "strategies/*/*.cpp")
if(MM_STRATEGIES_SRC)
  add_library(mm_strategies OBJECT ${MM_STRATEGIES_SRC})
  target_include_directories(mm_strategies PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(mm_strategies PUBLIC mm_flags mm_strategy)
endif()

# Binance adapter. Links mm_orderbook (for synchronization) plus the header-only
# Boost.Asio/Beast and nlohmann/json, which appear here and nowhere above.
file(GLOB_RECURSE MM_BINANCE_SRC CONFIGURE_DEPENDS "src/exchange/binance/*.cpp")
if(MM_BINANCE_SRC)
  mmx_add_module(mm_exchange_binance
    SOURCES ${MM_BINANCE_SRC}
    DEPS    mm_orderbook mm_thirdparty_headers
    PRIVATE_DEPS OpenSSL::SSL OpenSSL::Crypto
  )
endif()

# GCC 13 reports -Wnull-dereference inside libstdc++'s <shared_ptr_base.h> when
# it inlines through Beast's composed asynchronous operations. Evidence that it
# is a false positive rather than a defect:
#   * Clang 18 with the identical warning set compiles the file clean.
#   * GCC reports it at -O2 and not at all at -O0, which is the signature of an
#     inlining-analysis artifact rather than a real null path.
#   * ASan+UBSan runs of this code report nothing.
# Scoped to this one translation unit so the project keeps the full warning set
# everywhere else. Revisit when the GCC baseline moves.
# The two translation units that instantiate Asio/Beast composed operations.
# Everything else in the project keeps the full warning set.
if(MM_BINANCE_SRC AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/exchange/binance/BinanceTransport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/exchange/binance/BinanceMarketData.cpp"
    PROPERTIES COMPILE_OPTIONS "-Wno-null-dereference")
endif()

# Mock adapter - a reference implementation of both interfaces, used by tests
# and benchmarks. It is a peer of the future Binance adapter, not a special
# case, which is what makes it evidence that the abstraction is implementable.
file(GLOB_RECURSE MM_EXCHANGE_MOCK_SRC CONFIGURE_DEPENDS "src/exchange/mock/*.cpp")
if(MM_EXCHANGE_MOCK_SRC)
  mmx_add_module(mm_exchange_mock
    SOURCES ${MM_EXCHANGE_MOCK_SRC}
    DEPS    mm_exchange
  )
endif()

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
if(TARGET mm_exchange_binance)
  # Live public-market-data probe. Not run by ctest: it needs the internet, and
  # a test suite that fails when the network is down teaches people to ignore it.
  add_executable(mm_md_probe apps/mm_md_probe.cpp)
  target_link_libraries(mm_md_probe PRIVATE mm_exchange_binance)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/apps/mm_md_probe.cpp"
      PROPERTIES COMPILE_OPTIONS "-Wno-null-dereference")
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MMX_BUILD_TESTS)
  enable_testing()

  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
  add_subdirectory("${MMX_THIRD_PARTY}/googletest" EXCLUDE_FROM_ALL)
  foreach(dep gtest gtest_main gmock gmock_main)
    if(TARGET ${dep})
      set_target_properties(${dep} PROPERTIES SYSTEM ON)
    endif()
  endforeach()

  add_library(mm_test_support INTERFACE)
  target_include_directories(mm_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(mm_test_support INTERFACE gtest gmock gtest_main mm_flags)

  # Test binaries are grouped by suite so a failing failure-injection test is
  # distinguishable from a failing unit test in CI output.
  function(mmx_add_test_suite suite)
    file(GLOB_RECURSE _srcs CONFIGURE_DEPENDS "tests/${suite}/*.cpp")
    if(NOT _srcs)
      return()
    endif()
    add_executable(mm_${suite}_tests ${_srcs})
    target_link_libraries(mm_${suite}_tests PRIVATE mm_test_support ${ARGN})
    # Lets tests load the configs that actually ship, rather than a copy that
    # can drift away from them.
    target_compile_definitions(mm_${suite}_tests
      PRIVATE MMX_CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/config")
    add_test(NAME ${suite} COMMAND mm_${suite}_tests)
    set_tests_properties(${suite} PROPERTIES TIMEOUT 300)
  endfunction()

  mmx_add_test_suite(unit mm_common mm_exchange mm_orderbook mm_strategy mm_quote mm_risk mm_oms mm_strategies mm_exchange_mock mm_exchange_binance)
  mmx_add_test_suite(architecture mm_common mm_exchange mm_orderbook mm_strategy mm_quote mm_risk mm_oms mm_strategies mm_exchange_mock mm_exchange_binance)
  mmx_add_test_suite(failure mm_common mm_exchange mm_orderbook mm_strategy mm_quote mm_risk mm_oms mm_strategies mm_exchange_mock mm_exchange_binance)
  mmx_add_test_suite(strategy_swap mm_common mm_exchange mm_orderbook mm_strategy mm_quote mm_risk mm_oms mm_strategies mm_exchange_mock mm_exchange_binance)
  mmx_add_test_suite(integration mm_common mm_exchange mm_orderbook mm_strategy mm_quote mm_risk mm_oms mm_strategies mm_exchange_mock mm_exchange_binance)

  # Acceptance tests B and F are source-level properties, not runtime ones:
  # no core file may name a venue or include an adapter header. Enforced here
  # so the boundary cannot erode between reviews.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME exchange_boundary
             COMMAND ${Python3_EXECUTABLE}
                     "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_exchange_boundary.py")
    set_tests_properties(exchange_boundary PROPERTIES TIMEOUT 60)
  else()
    message(WARNING "Python3 not found - the exchange boundary check will not run")
  endif()
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(MMX_BUILD_BENCH)
  find_package(benchmark QUIET)
  if(benchmark_FOUND)
    file(GLOB_RECURSE MM_BENCH_SRC CONFIGURE_DEPENDS "benchmarks/*.cpp")
    if(MM_BENCH_SRC)
      add_executable(mm_bench ${MM_BENCH_SRC})
      target_link_libraries(mm_bench PRIVATE mm_common mm_exchange mm_orderbook mm_strategy mm_quote mm_risk mm_oms mm_strategies mm_exchange_mock mm_exchange_binance benchmark::benchmark)
    endif()
  else()
    message(STATUS "Google Benchmark not found - benchmarks disabled")
  endif()
endif()

# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "crypto_mm_engine ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests      : ${MMX_BUILD_TESTS}")
message(STATUS "  benchmarks : ${MMX_BUILD_BENCH}")
message(STATUS "  asan/tsan  : ${MMX_ASAN}/${MMX_TSAN}")
message(STATUS "")
