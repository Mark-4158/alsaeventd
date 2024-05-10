cmake_minimum_required(VERSION 3.5)

project(alsaeventd LANGUAGES C)

add_executable(alsaeventd main.c)
target_link_libraries(alsaeventd asound canberra)

install(TARGETS alsaeventd
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
