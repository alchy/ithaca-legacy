// tests/test_log.cpp
// Unit testy RT-safe loggeru. doctest single-header runner.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "util/log.h"
#include "util/version.h"

#include <string>

using namespace ithaca;

TEST_CASE("severity_from_string parsuje zname hodnoty") {
    CHECK(log::severity_from_string("debug")   == log::Severity::Debug);
    CHECK(log::severity_from_string("info")    == log::Severity::Info);
    CHECK(log::severity_from_string("warn")    == log::Severity::Warning);
    CHECK(log::severity_from_string("warning") == log::Severity::Warning);
    CHECK(log::severity_from_string("error")   == log::Severity::Error);
    CHECK(log::severity_from_string("fatal")   == log::Severity::Fatal);
}

TEST_CASE("severity_from_string vraci default pro neznamou hodnotu") {
    CHECK(log::severity_from_string("blabla", log::Severity::Warning)
          == log::Severity::Warning);
}

TEST_CASE("severity_to_string je nenulovy retezec") {
    CHECK(std::string(log::severity_to_string(log::Severity::Info)) == "INFO");
    CHECK(std::string(log::severity_to_string(log::Severity::Error)) == "ERROR");
}

TEST_CASE("setMinSeverity / getMinSeverity round-trip") {
    log::Logger lg;
    lg.setMinSeverity(log::Severity::Warning);
    CHECK(lg.getMinSeverity() == log::Severity::Warning);
}

TEST_CASE("logRT zapise do ring bufferu a flushRTBuffer ho vyprazdni") {
    log::Logger lg;
    lg.setMinSeverity(log::Severity::Debug);
    lg.setOutputMode(/*console=*/false, /*file=*/false);  // tichy pro test
    lg.logRT("test", log::Severity::Info, "ahoj %d", 42);
    int flushed = lg.flushRTBuffer();
    CHECK(flushed == 1);
    // Druhy flush uz nic nema
    CHECK(lg.flushRTBuffer() == 0);
}

TEST_CASE("version string neni prazdny") {
    CHECK(std::string(ITHACA_VERSION).size() > 0);
}
