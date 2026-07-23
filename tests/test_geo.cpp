// tests/test_geo.cpp — Unit tests for geographic utility functions
#include "utils/geo.h"
#include <iostream>
#include <cmath>
#include <string>

static int tests_passed = 0;
static int tests_failed = 0;

void check(bool cond, const std::string& name, const std::string& detail = "") {
    if (cond) {
        std::cout << "  [PASS] " << name << std::endl;
        tests_passed++;
    } else {
        std::cout << "  [FAIL] " << name;
        if (!detail.empty()) std::cout << " — " << detail;
        std::cout << std::endl;
        tests_failed++;
    }
}

int main() {
    std::cout << "=== Geo Functions Unit Tests ===" << std::endl;

    // --- Test 1: geoDistance ---
    std::cout << "Test 1: geoDistance" << std::endl;
    {
        check(std::abs(geoDistance(0, 0, 0, 0)) < 0.0001, "Same point = 0");
        check(std::abs(geoDistance(0, 0, 1, 0) - 1.0) < 0.001, "1 degree lat ≈ 1");
        // At equator, 1 degree lon ≈ 1 degree
        check(std::abs(geoDistance(0, 0, 0, 1) - 1.0) < 0.001, "1 degree lon at equator ≈ 1");
        // At 60° latitude, longitude is compressed by cos(60°) = 0.5
        double d = geoDistance(60, 0, 60, 1);
        check(std::abs(d - 0.5) < 0.01, "1 degree lon at 60°N ≈ 0.5", std::to_string(d));
        // Symmetric
        check(std::abs(geoDistance(10, 20, 30, 40) - geoDistance(30, 40, 10, 20)) < 0.0001,
              "Distance is symmetric");
    }

    // --- Test 2: Region::contains ---
    std::cout << "Test 2: Region::contains" << std::endl;
    {
        Region r{-10, 10, -20, 20};  // lat: [-10,10], lon: [-20,20]

        check(r.contains(0, 0), "Center point inside");
        check(r.contains(10, 20), "Corner point inside (boundary)");
        check(r.contains(-10, -20), "Opposite corner inside (boundary)");
        check(!r.contains(11, 0), "Point above max_lat outside");
        check(!r.contains(0, 21), "Point right of max_lon outside");
        check(!r.contains(-11, 0), "Point below min_lat outside");
    }

    // --- Test 3: Region::overlaps ---
    std::cout << "Test 3: Region::overlaps" << std::endl;
    {
        Region a{0, 10, 0, 10};
        Region b{5, 15, 5, 15};   // overlaps with a
        Region c{20, 30, 20, 30}; // no overlap with a
        Region d{10, 20, 0, 10};  // touches a at boundary

        check(a.overlaps(b), "Overlapping regions");
        check(!a.overlaps(c), "Non-overlapping regions");
        check(a.overlaps(d), "Boundary-touching regions overlap");
    }

    // --- Test 4: Region::merge ---
    std::cout << "Test 4: Region::merge" << std::endl;
    {
        Region a{0, 10, 0, 10};
        Region b{5, 15, -5, 5};
        Region m = Region::merge(a, b);

        check(m.min_lat == 0, "Merged min_lat == 0");
        check(m.max_lat == 15, "Merged max_lat == 15");
        check(m.min_lon == -5, "Merged min_lon == -5");
        check(m.max_lon == 10, "Merged max_lon == 10");

        // Merged region contains both originals
        check(m.contains(0, 0) && m.contains(15, 5), "Merged contains both originals");
    }

    // --- Test 5: Region::quadrantCenters ---
    std::cout << "Test 5: Region::quadrantCenters" << std::endl;
    {
        Region r{0, 10, 0, 10};  // mid = (5, 5)
        auto qc = r.quadrantCenters();

        // SW: (2.5, 2.5), SE: (2.5, 7.5), NW: (7.5, 2.5), NE: (7.5, 7.5)
        check(std::abs(qc[0].first - 2.5) < 0.001 && std::abs(qc[0].second - 2.5) < 0.001,
              "SW quadrant center");
        check(std::abs(qc[1].first - 2.5) < 0.001 && std::abs(qc[1].second - 7.5) < 0.001,
              "SE quadrant center");
        check(std::abs(qc[2].first - 7.5) < 0.001 && std::abs(qc[2].second - 2.5) < 0.001,
              "NW quadrant center");
        check(std::abs(qc[3].first - 7.5) < 0.001 && std::abs(qc[3].second - 7.5) < 0.001,
              "NE quadrant center");
    }

    // --- Test 6: Region::quadrantOf ---
    std::cout << "Test 6: Region::quadrantOf" << std::endl;
    {
        Region r{0, 10, 0, 10};  // mid = (5, 5)

        check(r.quadrantOf(2, 2) == 0, "SW -> quadrant 0");
        check(r.quadrantOf(2, 8) == 1, "SE -> quadrant 1");
        check(r.quadrantOf(8, 2) == 2, "NW -> quadrant 2");
        check(r.quadrantOf(8, 8) == 3, "NE -> quadrant 3");
        check(r.quadrantOf(5, 5) == 3, "Center -> quadrant 3 (boundary goes NE)");
    }

    // --- Test 7: Negative coordinates ---
    std::cout << "Test 7: Negative coordinates (London region)" << std::endl;
    {
        // London area: lat [51.3, 51.7], lon [-0.5, 0.1]
        Region london{51.3, 51.7, -0.5, 0.1};

        check(london.contains(51.5, -0.1), "Central London inside");
        check(!london.contains(48.8, 2.3), "Paris outside London region");
        check(london.overlaps(Region{51.0, 52.0, -1.0, 0.5}), "Overlaps with larger SE England");
    }

    // --- Summary ---
    std::cout << "\n=== " << tests_passed << " passed, " << tests_failed
              << " failed ===" << std::endl;
    return tests_failed > 0 ? 1 : 0;
}