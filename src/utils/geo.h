#ifndef GEO_H
#define GEO_H

#include <cmath>
#include <array>
#include <algorithm>
#include <string>

// ============================================================
// Geographic utility functions for DNS++ spatial routing
// ============================================================

// Equirectangular approximation distance (good enough for routing,
// not navigation). Returns distance in "degree-equivalent" units.
// Only one cos() call — suitable for hot path.
inline double geoDistance(double lat1, double lon1,
                          double lat2, double lon2) {
    double dlat = lat1 - lat2;
    double dlon = (lon1 - lon2) * std::cos((lat1 + lat2) / 2.0 * M_PI / 180.0);
    return std::sqrt(dlat * dlat + dlon * dlon);
}

// 2D Minimum Bounding Hyperrectangle (MBH)
// For Phase 1-2 we use 2D (lat/lon). The paper supports N-D.
struct Region {
    double min_lat = 0.0, max_lat = 0.0;
    double min_lon = 0.0, max_lon = 0.0;

    bool contains(double lat, double lon) const {
        return lat >= min_lat && lat <= max_lat &&
               lon >= min_lon && lon <= max_lon;
    }

    bool overlaps(const Region& o) const {
        return !(max_lat < o.min_lat || min_lat > o.max_lat ||
                 max_lon < o.min_lon || min_lon > o.max_lon);
    }

    static Region merge(const Region& a, const Region& b) {
        return {
            std::min(a.min_lat, b.min_lat), std::max(a.max_lat, b.max_lat),
            std::min(a.min_lon, b.min_lon), std::max(a.max_lon, b.max_lon)
        };
    }

    // 2D: four quadrant centers (for Algorithm 1, Publication Processing)
    std::array<std::pair<double, double>, 4> quadrantCenters() const {
        double mid_lat = (min_lat + max_lat) / 2.0;
        double mid_lon = (min_lon + max_lon) / 2.0;
        return {{
            {(min_lat + mid_lat) / 2.0, (min_lon + mid_lon) / 2.0},  // SW
            {(min_lat + mid_lat) / 2.0, (mid_lon + max_lon) / 2.0},  // SE
            {(mid_lat + max_lat) / 2.0, (min_lon + mid_lon) / 2.0},  // NW
            {(mid_lat + max_lat) / 2.0, (mid_lon + max_lon) / 2.0}   // NE
        }};
    }

    // Which quadrant does a point fall into? (0=SW, 1=SE, 2=NW, 3=NE)
    int quadrantOf(double lat, double lon) const {
        double mid_lat = (min_lat + max_lat) / 2.0;
        double mid_lon = (min_lon + max_lon) / 2.0;
        int q = 0;
        if (lat >= mid_lat) q += 2;  // north
        if (lon >= mid_lon) q += 1;  // east
        return q;
    }

    std::string toString() const {
        return "[" + std::to_string(min_lat) + "," + std::to_string(max_lat)
             + "]x[" + std::to_string(min_lon) + "," + std::to_string(max_lon)
             + "]";
    }
};

#endif // GEO_H