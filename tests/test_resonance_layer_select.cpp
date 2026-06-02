#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "resonance/resonance_layer_select.h"
using namespace ithaca;

static NoteSlots makeNote(std::initializer_list<float> rms) {
    NoteSlots ns; ns.recorded = true;
    for (float v : rms) { VelocitySlot s; s.rms_db = v; ns.slots.push_back(s); }
    return ns;
}

TEST_CASE("nearestSlotByRms vybere nejblizsi rms_db") {
    NoteSlots ns = makeNote({-40.f, -25.f, -10.f});
    CHECK(nearestSlotByRms(ns, -25.f) == 1);
    CHECK(nearestSlotByRms(ns, -38.f) == 0);
    CHECK(nearestSlotByRms(ns, -5.f)  == 2);
    CHECK(nearestSlotByRms(ns, -1.f)  == 2);
    CHECK(nearestSlotByRms(ns, -100.f)== 0);
}
TEST_CASE("shoda vzdalenosti → nizsi index") {
    NoteSlots ns = makeNote({-30.f, -20.f});
    CHECK(nearestSlotByRms(ns, -25.f) == 0);
}
TEST_CASE("prazdne / jednoslotove") {
    NoteSlots empty; empty.recorded = true;
    CHECK(nearestSlotByRms(empty, -25.f) == -1);
    NoteSlots one = makeNote({-12.f});
    CHECK(nearestSlotByRms(one, -25.f) == 0);
    CHECK(nearestSlotByRms(one, 0.f)   == 0);
}
