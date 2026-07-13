# Test registration for CTest.
#
# Current state: only smoke tests that run the existing executables and check
# their output. As real modules land in src/common (protocol parsers, MIME,
# Maildir, ...), add one unit-test executable per module here:
#
#   add_executable(smtp_parser_test smtp_parser_test.cpp)
#   target_link_libraries(smtp_parser_test PRIVATE mailcommon)
#   add_test(NAME smtp_parser COMMAND smtp_parser_test)
#
# Parsers and codecs are the highest-value unit-test targets, since they are
# pure functions over byte buffers and easy to cover exhaustively.

add_test(NAME server_smoke COMMAND server)
set_tests_properties(server_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "Hello, Server!")

add_test(NAME client_smoke COMMAND client)
set_tests_properties(client_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "Hello, Client!")
