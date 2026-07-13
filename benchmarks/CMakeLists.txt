# Micro-benchmarks (disabled by default; enable with -DMAIL_BUILD_BENCHMARKS=ON).
#
# Deferred until a performance-relevant hot path exists. The primary candidate
# is the CRLF line reader / protocol tokenizer in src/common, which sits on
# every byte of every connection. When that lands, add one benchmark per hot
# path here:
#
#   add_executable(line_reader_bench line_reader_bench.cpp)
#   target_link_libraries(line_reader_bench PRIVATE mailcommon)
#
# A framework such as google/benchmark can be pulled in via FetchContent at
# that point. No benchmark targets are defined yet on purpose -- there is no
# measurable code path to benchmark.
