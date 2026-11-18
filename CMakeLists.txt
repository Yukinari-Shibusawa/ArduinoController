cmake_minimum_required(VERSION 3.16)

project(ArduinoController LANGUAGES CXX)

add_library(arduino_controller
    src/GpioController.cpp
    src/SerialConnection.cpp
)
add_library(ArduinoController::arduino_controller ALIAS arduino_controller)

target_include_directories(arduino_controller PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(arduino_controller PUBLIC cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(arduino_controller PUBLIC Threads::Threads)

include(CTest)
if(BUILD_TESTING)
    add_executable(arduino_controller_tests tests/SerialConnectionTests.cpp)
    target_link_libraries(arduino_controller_tests PRIVATE
        ArduinoController::arduino_controller
    )
    add_test(NAME arduino_controller_tests COMMAND arduino_controller_tests)
endif()

include(GNUInstallDirs)
install(TARGETS arduino_controller
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
