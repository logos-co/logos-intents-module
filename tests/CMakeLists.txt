cmake_minimum_required(VERSION 3.14)
project(IntentsModuleTests LANGUAGES CXX)

include(LogosTest)

logos_test(
    NAME intents_module_tests
    MODULE_SOURCES
        ../src/intent_rules.cpp
        ../src/intent_registry.cpp
        ../src/intent_broker.cpp
        ../src/intents_service.cpp
    TEST_SOURCES
        main.cpp
        test_rules.cpp
        test_registry.cpp
        test_broker.cpp
        test_service.cpp
        test_hardening.cpp
)
