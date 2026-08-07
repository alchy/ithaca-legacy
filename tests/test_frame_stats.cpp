// tests/test_frame_stats.cpp — histogram, percentily a formatovani radku.
//
// Duvod pro tenhle test: percentily se ctou z log-rozlozeneho histogramu, coz
// je presne ten druh kodu, ktery vypada trivialne a mlci, kdyz se splete o
// jednu prihradku. A na cislech z nej se budou stavet rozhodnuti o optimalizaci
// GUI, takze musi byt duveryhodna.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "frame_stats.h"

#include <cstring>
#include <string>

using ithaca::gui::FrameStats;

TEST_CASE("prazdna statistika nic nehlasi") {
    FrameStats s(1.0);
    char buf[192];
    CHECK_FALSE(s.maybeReport(0.0, buf, sizeof(buf)));   // prvni volani = start
    CHECK_FALSE(s.maybeReport(5.0, buf, sizeof(buf)));   // zadne snimky
    CHECK(s.frames() == 0);
}

TEST_CASE("percentil sedi na konstantnim vzorku") {
    FrameStats s;
    for (int i = 0; i < 1000; ++i) s.add(2.0f, 10.f, 100, 300, 1);
    // Prihradka je +19 %, vraci se jeji horni hrana → hodnota smi byt vyse,
    // nikdy ne niz nez vzorek.
    const float p50 = s.cpuPercentile(0.50f);
    CHECK(p50 >= 2.0f);
    CHECK(p50 <= 2.0f * 1.20f);
    CHECK(s.cpuMax() == doctest::Approx(2.0f));
}

TEST_CASE("odlehla hodnota neposune p50, ale je videt v max") {
    FrameStats s;
    for (int i = 0; i < 999; ++i) s.add(2.0f, 1.f, 100, 300, 1);
    s.add(200.0f, 1.f, 100, 300, 1);                     // jedna spicka planovace

    const float p50 = s.cpuPercentile(0.50f);
    CHECK(p50 <= 2.0f * 1.20f);            // ← tohle prumer neumi
    CHECK(s.cpuMax() == doctest::Approx(200.0f));
    // Prumer by byl 2.2 ms, tedy o 10 % vedle. Proto se nepocita.
}

TEST_CASE("p95 najde horni pasmo") {
    FrameStats s;
    for (int i = 0; i < 950; ++i) s.add(1.0f, 1.f, 0, 0, 0);
    for (int i = 0; i < 50;  ++i) s.add(8.0f, 1.f, 0, 0, 0);
    CHECK(s.cpuPercentile(0.50f) <= 1.0f * 1.20f);
    CHECK(s.cpuPercentile(0.95f) >= 8.0f);
    CHECK(s.cpuPercentile(0.95f) <= 8.0f * 1.20f);
}

TEST_CASE("late pocita snimky pres rozpocet") {
    FrameStats s;
    s.setBudgetMs(5.f);
    for (int i = 0; i < 10; ++i) s.add(1.0f, 1.f, 0, 0, 0);
    for (int i = 0; i < 3;  ++i) s.add(9.0f, 1.f, 0, 0, 0);
    CHECK(s.late() == 3);
}

TEST_CASE("okrajove hodnoty nespadnou mimo histogram") {
    FrameStats s;
    s.add(0.f, 0.f, 0, 0, 0);              // nula
    s.add(-1.f, -1.f, 0, 0, 0);            // zaporna (nemelo by nastat)
    s.add(1e9f, 1e9f, 0, 0, 0);            // absurdne velka
    CHECK(s.frames() == 3);
    CHECK(s.cpuPercentile(0.99f) > 0.f);   // nespadlo
}

TEST_CASE("geometrie se hlasi jako posledni a maximum") {
    FrameStats s(1.0);
    s.add(1.f, 1.f, 5000, 15000, 4);
    s.add(1.f, 1.f, 9000, 27000, 6);
    s.add(1.f, 1.f, 7000, 21000, 5);
    CHECK(s.vtxLast() == 7000);

    char buf[192];
    CHECK_FALSE(s.maybeReport(0.0, buf, sizeof(buf)));
    REQUIRE(s.maybeReport(2.0, buf, sizeof(buf)));
    const std::string line(buf);
    CHECK(line.find("vtx=7000") != std::string::npos);
    CHECK(line.find("max=9000") != std::string::npos);
    CHECK(line.find("idx=21000") != std::string::npos);
    CHECK(line.find("3f") != std::string::npos);          // pocet snimku
}

TEST_CASE("report vynuluje citace a nastavi novy start") {
    FrameStats s(1.0);
    char buf[192];
    s.maybeReport(0.0, buf, sizeof(buf));
    for (int i = 0; i < 100; ++i) s.add(3.f, 1.f, 10, 30, 1);
    REQUIRE(s.maybeReport(2.0, buf, sizeof(buf)));
    CHECK(s.frames() == 0);
    CHECK(s.cpuMax() == doctest::Approx(0.f));
    CHECK_FALSE(s.maybeReport(2.5, buf, sizeof(buf)));    // perioda jeste neubehla
}
