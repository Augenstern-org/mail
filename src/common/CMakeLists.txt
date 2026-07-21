# mailcommon：承载所有非可执行层（net，以及后续的 auth/storage/message/protocol）
# 的共享静态库。公开头文件位于 include/mail/...；源文件位于 src/...
#
# 随着模块落地，把新增的 .cpp 文件加入下面的 add_library 列表。

add_library(mailcommon STATIC
    src/net/connection.cpp
    src/net/listener.cpp
    src/net/line_reader.cpp
    src/smtp/command.cpp
    src/smtp/reply.cpp
    src/smtp/session.cpp
    src/store/maildir_name.cpp
    src/store/maildir.cpp
    src/base64.cpp
    src/auth/password.cpp
    src/auth/user_store.cpp
    src/auth/sasl.cpp
)

target_include_directories(mailcommon PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# libsodium 经 apt libsodium-dev + pkg-config 取得（vcpkg.json 保持为空）。
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
# 仅 src/auth/password.cpp include <sodium.h>；公开头文件绝不暴露 libsodium 类型，
# 故 PRIVATE 即可 —— 静态库的链接需求仍会通过 $<LINK_ONLY:> 传给消费者。
target_link_libraries(mailcommon PRIVATE PkgConfig::SODIUM)

# UserStore 用 std::counting_semaphore，在 Linux 上需要 pthread。PUBLIC：现有的
# 六个测试目标都不自带 Threads，必须由 mailcommon 传播，否则链接失败。
find_package(Threads REQUIRED)
target_link_libraries(mailcommon PUBLIC Threads::Threads)

target_compile_features(mailcommon PUBLIC cxx_std_20)
