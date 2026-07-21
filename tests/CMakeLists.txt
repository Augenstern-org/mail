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

# Maildir unique-name 纯函数单元测试：无 I/O、无系统调用。
add_executable(maildir_name_test maildir_name_test.cpp)
target_link_libraries(maildir_name_test PRIVATE mailcommon)
add_test(NAME maildir_name COMMAND maildir_name_test)

# MaildirStore 落盘集成测试：在临时目录内演练 open/deliverMessage 全序列。
add_executable(maildir_store_test maildir_store_test.cpp)
target_link_libraries(maildir_store_test PRIVATE mailcommon)
add_test(NAME maildir_store COMMAND maildir_store_test)

# MaildirSink 适配器测试：header-only，位于应用层 src/server（storage 层不得
# include MessageSink，适配器反向依赖 store，见 message_sink.hpp 头注）。
add_executable(maildir_sink_test maildir_sink_test.cpp)
target_link_libraries(maildir_sink_test PRIVATE mailcommon)
target_include_directories(maildir_sink_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/server)
add_test(NAME maildir_sink COMMAND maildir_sink_test)

# base64 编解码单元测试：纯函数，覆盖 RFC 4648 §10 向量与解码器的全部严格性边界。
add_executable(base64_test base64_test.cpp)
target_link_libraries(base64_test PRIVATE mailcommon)
add_test(NAME base64 COMMAND base64_test)

# 口令哈希单元测试：经 libsodium 演练 hash/verify 往返与畸形摘要的拒绝。
add_executable(auth_password_test auth_password_test.cpp)
target_link_libraries(auth_password_test PRIVATE mailcommon)
add_test(NAME auth_password COMMAND auth_password_test)

# UserStore 单元测试：账户表的加载、查找与并发访问。
add_executable(auth_user_store_test auth_user_store_test.cpp)
target_link_libraries(auth_user_store_test PRIVATE mailcommon)
add_test(NAME auth_user_store COMMAND auth_user_store_test)

# SASL 机制单元测试：PLAIN/LOGIN 的载荷解析，纯函数、无套接字。
add_executable(auth_sasl_test auth_sasl_test.cpp)
target_link_libraries(auth_sasl_test PRIVATE mailcommon)
add_test(NAME auth_sasl COMMAND auth_sasl_test)

# UserStoreVerifier 适配器测试：与 MaildirSink 同理，位于应用层 src/server，
# 故需额外把该目录加入 include 路径。
add_executable(user_store_verifier_test user_store_verifier_test.cpp)
target_link_libraries(user_store_verifier_test PRIVATE mailcommon)
target_include_directories(user_store_verifier_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/server)
add_test(NAME user_store_verifier COMMAND user_store_verifier_test)
