cmake_minimum_required(VERSION 3.16)
project(mp3_player CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(AVAHI REQUIRED avahi-client)

add_executable(mp3_player mp3_player.cpp)

target_include_directories(mp3_player PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${ALSA_INCLUDE_DIRS} ${AVAHI_INCLUDE_DIRS})
target_link_libraries(mp3_player PRIVATE ${ALSA_LIBRARIES} ${AVAHI_LIBRARIES})
target_compile_options(mp3_player PRIVATE -Wall -Wextra -O2)

install(TARGETS mp3_player DESTINATION bin)
install(FILES config.ini DESTINATION share/mp3-player)
