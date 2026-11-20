cmake_minimum_required(VERSION 3.14)
project(IntentsModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LogosModule.cmake")
    include(cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

logos_module(
    NAME intents_module
    SOURCES
        src/intents_module_impl.h
        src/intents_module_impl.cpp
        src/client_pool.h
        src/intents_service.h
        src/intents_service.cpp
        src/intent_broker.h
        src/intent_broker.cpp
        src/intent_registry.h
        src/intent_registry.cpp
        src/intent_rules.h
        src/intent_rules.cpp
)
