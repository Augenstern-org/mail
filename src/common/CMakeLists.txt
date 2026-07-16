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
)

target_include_directories(mailcommon PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(mailcommon PUBLIC cxx_std_20)
