add_executable(echo_server echo_server.cpp)
target_link_libraries(echo_server PRIVATE netforge)

add_executable(echo_client echo_client.cpp)
target_link_libraries(echo_client PRIVATE netforge)

add_executable(chat_server chat_server.cpp)
target_link_libraries(chat_server PRIVATE netforge)

add_executable(chat_client chat_client.cpp)
target_link_libraries(chat_client PRIVATE netforge)
