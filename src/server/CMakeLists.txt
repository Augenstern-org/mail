find_package(Threads REQUIRED)

add_executable(server main.cpp)
target_link_libraries(server PRIVATE mailcommon Threads::Threads)
