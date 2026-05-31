// tests/test_log_subscriber.cpp - LogRingBuffer + Logger Subscriber API.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "util/log.h"
#include "log_subscriber.h"  // app/gui/log_subscriber.h (via include path)

#include <string>

using namespace ithaca;

TEST_CASE("LogRingBuffer push + snapshot pod kapacitou") {
    gui::LogRingBuffer rb;

    log::LogEntry e1{1, "t", log::Severity::Info, "msg1"};
    log::LogEntry e2{2, "t", log::Severity::Info, "msg2"};
    log::LogEntry e3{3, "t", log::Severity::Info, "msg3"};

    rb.push(e1);
    rb.push(e2);
    rb.push(e3);

    log::LogEntry out[10];
    int n = rb.snapshot(out, 10);
    CHECK(n == 3);
    CHECK(out[0].timestamp_us == 1);
    CHECK(out[1].timestamp_us == 2);
    CHECK(out[2].timestamp_us == 3);
    CHECK(out[2].message == "msg3");
}

TEST_CASE("LogRingBuffer wrap-around po kapacite") {
    gui::LogRingBuffer rb;
    const int extra = 10;
    for (int i = 0; i < gui::LogRingBuffer::kCapacity + extra; ++i) {
        rb.push({(long long)i, "t", log::Severity::Info, "x"});
    }
    CHECK(rb.size() == gui::LogRingBuffer::kCapacity);
    log::LogEntry out[gui::LogRingBuffer::kCapacity];
    int n = rb.snapshot(out, gui::LogRingBuffer::kCapacity);
    CHECK(n == gui::LogRingBuffer::kCapacity);
    // Nejstarsi zustal: index `extra` (po wrap-around).
    CHECK(out[0].timestamp_us == extra);
    CHECK(out[n - 1].timestamp_us == gui::LogRingBuffer::kCapacity + extra - 1);
}

TEST_CASE("LogRingBuffer snapshot s max_n mensim nez size vraci jen poslednich N") {
    gui::LogRingBuffer rb;
    for (int i = 0; i < 50; ++i) {
        rb.push({(long long)i, "t", log::Severity::Info, "x"});
    }
    log::LogEntry out[5];
    int n = rb.snapshot(out, 5);
    CHECK(n == 5);
    CHECK(out[0].timestamp_us == 45);
    CHECK(out[4].timestamp_us == 49);
}

TEST_CASE("LogRingBuffer snapshot s max_n=0 nic nedela") {
    gui::LogRingBuffer rb;
    rb.push({1, "t", log::Severity::Info, "x"});
    log::LogEntry out[1];
    CHECK(rb.snapshot(out, 0) == 0);
}

TEST_CASE("LogRingBuffer clear nuluje velikost") {
    gui::LogRingBuffer rb;
    rb.push({1, "t", log::Severity::Info, "x"});
    rb.push({2, "t", log::Severity::Info, "y"});
    CHECK(rb.size() == 2);
    rb.clear();
    CHECK(rb.size() == 0);
    log::LogEntry out[2];
    CHECK(rb.snapshot(out, 2) == 0);
}

TEST_CASE("Logger Subscriber API doruci kazdy log") {
    auto& lg = log::Logger::default_();
    // Tichy logger pro test — neceme zaspamovat stdout.
    lg.setOutputMode(/*console=*/false, /*file=*/false);
    lg.setMinSeverity(log::Severity::Debug);

    int count = 0;
    std::string last_msg;
    log::Severity last_sev = log::Severity::Info;
    std::string last_topic;

    lg.clearSubscribers();
    lg.addSubscriber([&](const log::LogEntry& e) {
        ++count;
        last_msg = e.message;
        last_sev = e.sev;
        last_topic = e.topic;
    });

    lg.log("test", log::Severity::Info, "hello");
    lg.log("test", log::Severity::Warning, "world %d", 42);

    CHECK(count == 2);
    CHECK(last_msg == "world 42");
    CHECK(last_sev == log::Severity::Warning);
    CHECK(last_topic == "test");

    lg.clearSubscribers();
    // Po clearSubscribers se uz nedoruci.
    lg.log("test", log::Severity::Info, "ignored");
    CHECK(count == 2);

    // Restore default output mode (jiny test by jinak zdedil ticho).
    lg.setOutputMode(/*console=*/true, /*file=*/false);
}

TEST_CASE("Logger Subscriber + LogRingBuffer end-to-end") {
    auto& lg = log::Logger::default_();
    lg.setOutputMode(/*console=*/false, /*file=*/false);
    lg.setMinSeverity(log::Severity::Debug);

    gui::LogRingBuffer rb;
    lg.clearSubscribers();
    lg.addSubscriber([&rb](const log::LogEntry& e) { rb.push(e); });

    lg.log("comp", log::Severity::Info, "first");
    lg.log("comp", log::Severity::Error, "second");

    log::LogEntry out[10];
    int n = rb.snapshot(out, 10);
    CHECK(n == 2);
    CHECK(out[0].message == "first");
    CHECK(out[1].message == "second");
    CHECK(out[1].sev == log::Severity::Error);

    lg.clearSubscribers();
    lg.setOutputMode(/*console=*/true, /*file=*/false);
}
