# Copyright (c) 2026 by Bert Laverman. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Two ways to build this:
#
#  * For the Zero (`cppr-deploy Zero2WBusService`, which gives a toolchain file): the service `i2cbus`, and nothing else in
#    build/. cppr-deploy copies the first executable it finds there to the Zero, so there must be only one.
#  * On a PC: the core, the command line tool and the tests, which need nothing but the standard library. They end up in
#    build/tools/. Run the tests with `build/tools/core-test`.
#
# I2CBUS_ZERO decides. It is on when there is a toolchain file, and can be set by hand.

cmake_minimum_required(VERSION 3.13)

set(APP_NAME i2cbus)

if(CMAKE_TOOLCHAIN_FILE)
  set(I2CBUS_ZERO_DEFAULT ON)
else()
  set(I2CBUS_ZERO_DEFAULT OFF)
endif()
option(I2CBUS_ZERO "Build the service for the Zero 2 W" ${I2CBUS_ZERO_DEFAULT})

if(I2CBUS_ZERO)
  # Set the CppRaspberry switches, like Zero2WTestI2C does.
  set(TARGET_PICO off)
  set(TARGET_ZERO2W on)
  set(HAVE_I2C on)
  set(HAVE_SPI off)
  set(HAVE_PWM off)
  set(HAVE_MAX7219 on)

  # The CppRaspberry library location is taken from the CPP_RASPBERRY_PATH environment variable
  # set(CPP_RASPBERRY_PATH $ENV{CPP_RASPBERRY_PATH})
  # If this is not set, you can set it here
  set(CPP_RASPBERRY_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../CppRaspberry")

  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
endif()

project(${APP_NAME} C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

add_library(i2cbus-core STATIC
    src/core/ini-file.cpp
    src/core/config.cpp
    src/core/state-store.cpp
    src/core/commands.cpp
    src/core/bus-core.cpp)
target_include_directories(i2cbus-core PUBLIC src/core)
target_compile_options(i2cbus-core PRIVATE -Wall -Wextra -Werror)

if(I2CBUS_ZERO)
  # GCC notes that the way std::format_args is passed changed in GCC 9.1, for every std::format call. Nobody mixes old and
  # new objects here.
  target_compile_options(i2cbus-core PRIVATE -Wno-psabi)

  include(${CPP_RASPBERRY_PATH}/CppRaspberry.cmake)

  add_executable(${APP_NAME} src/service/main.cpp)
  add_cpp_raspberry_app(${APP_NAME})
  target_link_libraries(${APP_NAME} i2cbus-core pigpiod_if2)
  target_compile_options(${APP_NAME} PRIVATE -Wall -Wextra -Werror -Wno-psabi)
  target_link_options(${APP_NAME} PRIVATE -static -pthread)
else()
  set(TOOLS_DIR ${CMAKE_BINARY_DIR}/tools)

  add_executable(i2cbus-cli src/cli/i2cbus-cli.cpp)
  target_link_libraries(i2cbus-cli i2cbus-core)
  target_compile_options(i2cbus-cli PRIVATE -Wall -Wextra -Werror)
  set_target_properties(i2cbus-cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TOOLS_DIR})

  enable_testing()
  add_executable(core-test test/core-test.cpp)
  target_link_libraries(core-test i2cbus-core)
  target_compile_options(core-test PRIVATE -Wall -Wextra -Werror)
  set_target_properties(core-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TOOLS_DIR})
  add_test(NAME core-test COMMAND core-test)
endif()
