# benchmarks/

微基准测试目录，默认不构建。

启用方式：

```bash
cmake -B build -DMAIL_BUILD_BENCHMARKS=ON
cmake --build build
```

## 现状

目前**没有**任何基准测试目标。项目尚处于骨架阶段，不存在有意义的性能热点，此时编写基准测试只会产生无参考价值的噪声数据。

## 规划

当以下热点代码落地后，在此目录逐个补充基准：

1. **CRLF 行读取器 / 协议分词器**（`src/common`）——位于每个连接每一字节的必经路径，是首要基准对象。
2. **MIME 解析器**——大附件解析的吞吐量。
3. **Maildir 写入**——投递路径的 IO 开销。

届时可通过 CMake `FetchContent` 引入 [google/benchmark](https://github.com/google/benchmark) 作为框架。
