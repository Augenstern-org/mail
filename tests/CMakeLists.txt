# CTest 测试注册。
#
# 单元测试链接 mailcommon 并直接演练各模块。随着解析器/编解码器在 src/common 落地，
# 在此为每个模块新增一个测试可执行（解析器与编解码器是对字节缓冲的纯函数，是价值最
# 高的测试目标）。

find_package(Threads REQUIRED)

# LineReader 单元测试：纯逻辑、无套接字，经由内存 ByteSource 伪实现驱动。
add_executable(line_reader_test line_reader_test.cpp)
target_link_libraries(line_reader_test PRIVATE mailcommon)
add_test(NAME line_reader COMMAND line_reader_test)

# SMTP 命令解析与响应序列化单元测试：纯函数，无套接字、无状态机。
add_executable(smtp_command_test smtp_command_test.cpp)
target_link_libraries(smtp_command_test PRIVATE mailcommon)
add_test(NAME smtp_command COMMAND smtp_command_test)

# SMTP 会话状态机整场单测：纯逻辑，经内存 ByteSource/ByteSink 伪实现驱动完整事务序列。
add_executable(smtp_session_test smtp_session_test.cpp)
target_link_libraries(smtp_session_test PRIVATE mailcommon)
add_test(NAME smtp_session COMMAND smtp_session_test)

# net 集成测试：单进程内经回环运行真实的 Listener/Connection。
add_executable(net_integration_test net_integration_test.cpp)
target_link_libraries(net_integration_test PRIVATE mailcommon Threads::Threads)
add_test(NAME net_integration COMMAND net_integration_test)

# 已移除 server_smoke：服务器现在是长期运行的守护进程，运行并检查 stdout 的冒烟测试
# 不再适用。客户端仍是简单可执行，故保留其冒烟测试。
add_test(NAME client_smoke COMMAND client)
set_tests_properties(client_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "Hello, Client!")
