#pragma once
// In-Engine Planetary Destruction & Multi-Scale Simulation Engine
// Core Types, Spherical Coordinate Projections, Scale Tiers, and Physical Constants.
//
// Governed by RULES.md: Authoritative on the simulation side, zero view-state feedback.

#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <array>
#include <algorithm>

namespace planetary {

// Canonical engine voxel resolution: 1 mm = 0.001 m (matches VOXEL_SIZE)
inline constexpr double kCanonicalVoxelSize = 0.001;
// Subunit particle edge: 0.125 mm (matches kSubEdge)
inline constexpr double kSubEdge = 0.001 / 8.0;

// Standard Earth-like celestial constants (SI units)
inline constexpr double kStandardEarthRadiusM = 6371000.0;       // 6,371 km
inline constexpr double kStandardEarthMassKg  = 5.972e24;        // 5.972 x 10^24 kg
inline constexpr double kGravitationalConst   = 6.67430e-11;     // m^3 kg^-1 s^-2
inline constexpr double kStandardEarthDensity = 5515.0;          // kg / m^3
inline constexpr uint8_t kMaxZoom             = 32;

// 5-Tier Multi-Scale Hierarchy
enum class ScaleTier : uint8_t {
    Astronomical = 0,   // 10^7 - 10^9 m: Celestial bodies, orbital mechanics, N-body gravity
    PlateZones   = 1,   // 10^5 - 10^7 m: Tectonic plates, crust thickness, fault lines
    WaterBasins  = 2,   // 10^3 - 10^5 m: Ocean basins, fluid depth, mega-tsunamis
    Structural   = 3,   // 10^1 - 10^3 m: Building zones, load matrices (structural_matrix.hpp)
    MicroGrid    = 4    // 10^-3 - 10^1 m: Unit-cubic 1mm voxels, discrete ballistics
};

inline const char* tierToString(ScaleTier tier) {
    switch (tier) {
        case ScaleTier::Astronomical: return "Astronomical";
        case ScaleTier::PlateZones:   return "PlateZones";
        case ScaleTier::WaterBasins:  return "WaterBasins";
        case ScaleTier::Structural:   return "Structural";
        case ScaleTier::MicroGrid:    return "MicroGrid";
        default:                      return "Unknown";
    }
}

// 6 Faces of the Cubed-Sphere Quadtree Projection
enum class CubeFace : uint8_t {
    PosX = 0, // +X: East / Right
    NegX = 1, // -X: West / Left
    PosY = 2, // +Y: North Pole / Top
    NegY = 3, // -Y: South Pole / Bottom
    PosZ = 4, // +Z: Prime Meridian / Front
    NegZ = 5  // -Z: Anti-Meridian / Back
};

inline const char* faceToString(CubeFace face) {
    switch (face) {
        case CubeFace::PosX: return "+X (East)";
        case CubeFace::NegX: return "-X (West)";
        case CubeFace::PosY: return "+Y (North Pole)";
        case CubeFace::NegY: return "-Y (South Pole)";
        case CubeFace::PosZ: return "+Z (Prime Meridian)";
        case CubeFace::NegZ: return "-Z (Anti-Meridian)";
        default:             return "UnknownFace";
    }
}

// Concentric Volumetric Shells of the Planetary Body
enum class PlanetaryShell : uint8_t {
    Atmosphere = 0, // Exosphere to sea level (optical scattering, re-entry)
    Crust      = 1, // Brittle lithosphere (0 - 40 km depth)
    Mantle     = 2, // Silicate ductile mantle (40 - 2900 km depth)
    Core       = 3  // High-density iron-nickel core (2900 - 6371 km depth)
};

// Macro Crustal Material Classes
enum class CrustMaterial : uint8_t {
    Vacuum              = 0,
    GraniteContinental  = 1,
    BasaltOceanic       = 2,
    OrogenyMountain     = 3,
    IceShield           = 4,
    SedimentaryBasin    = 5,
    MoltenMagma         = 6,
    SilicateMantle      = 7,
    MetallicCore        = 8,
    WaterLiquid         = 9
};

// 3D Cartesian Vector in Planetary-Centered Inertial Frame (meters)
struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    Vec3d() = default;
    constexpr Vec3d(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    Vec3d operator+(const Vec3d& o) const { return Vec3d(x + o.x, y + o.y, z + o.z); }
    Vec3d operator-(const Vec3d& o) const { return Vec3d(x - o.x, y - o.y, z - o.z); }
    Vec3d operator*(double s) const { return Vec3d(x * s, y * s, z * s); }
    Vec3d operator/(double s) const { return Vec3d(x / s, y / s, z / s); }

    double lengthSq() const { return x * x + y * y + z * z; }
    double length() const { return std::sqrt(lengthSq()); }

    Vec3d normalized() const {
        double len = length();
        return (len > 1e-12) ? (*this / len) : Vec3d(0, 0, 0);
    }

    double dot(const Vec3d& o) const { return x * o.x + y * o.y + z * o.z; }

    Vec3d cross(const Vec3d& o) const {
        return Vec3d(
            y * o.z - z * o.y,
            z * o.x - x * o.z,
            x * o.y - y * o.x
        );
    }
};

// Spherical Geographic Coordinate (degrees & altitude in meters)
struct GeoCoord {
    double latDeg = 0.0; // [-90, +90]
    double lonDeg = 0.0; // [-180, +180]
    double altM   = 0.0; // Elevation relative to mean sea level

    GeoCoord() = default;
    constexpr GeoCoord(double lat, double lon, double alt = 0.0)
        : latDeg(lat), lonDeg(lon), altM(alt) {}
};

// Coordinate Conversions
inline Vec3d geoToCartesian(const GeoCoord& g, double planetRadius = kStandardEarthRadiusM) {
    constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
    const double latRad = g.latDeg * kDegToRad;
    const double lonRad = g.lonDeg * kDegToRad;
    const double r = planetRadius + g.altM;
    const double cosLat = std::cos(latRad);

    return Vec3d(
        r * cosLat * std::sin(lonRad),
        r * std::sin(latRad),
        r * cosLat * std::cos(lonRad)
    );
}

inline GeoCoord cartesianToGeo(const Vec3d& v, double planetRadius = kStandardEarthRadiusM) {
    constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;
    const double r = v.length();
    if (r < 1e-6) return GeoCoord(0, 0, -planetRadius);

    const double latRad = std::asin(std::clamp(v.y / r, -1.0, 1.0));
    const double lonRad = std::atan2(v.x, v.z);

    return GeoCoord(latRad * kRadToDeg, lonRad * kRadToDeg, r - planetRadius);
}

// Cubed-Sphere Projection: Projects a 3D cartesian point onto 6 faces of a cube
// where u, v in [-1.0, +1.0] parameterize the respective face plane.
inline void cartesianToCubedSphere(const Vec3d& v, CubeFace& outFace, double& outU, double& outV) {
    const double absX = std::abs(v.x);
    const double absY = std::abs(v.y);
    const double absZ = std::abs(v.z);

    if (absX >= absY && absX >= absZ) {
        if (v.x > 0.0) {
            outFace = CubeFace::PosX;
            outU = -v.z / absX;
            outV =  v.y / absX;
        } else {
            outFace = CubeFace::NegX;
            outU =  v.z / absX;
            outV =  v.y / absX;
        }
    } else if (absY >= absX && absY >= absZ) {
        if (v.y > 0.0) {
            outFace = CubeFace::PosY;
            outU =  v.x / absY;
            outV = -v.z / absY;
        } else {
            outFace = CubeFace::NegY;
            outU =  v.x / absY;
            outV =  v.z / absY;
        }
    } else {
        if (v.z > 0.0) {
            outFace = CubeFace::PosZ;
            outU =  v.x / absZ;
            outV =  v.y / absZ;
        } else {
            outFace = CubeFace::NegZ;
            outU = -v.x / absZ;
            outV =  v.y / absZ;
        }
    }
}

// Inverse Cubed-Sphere: Given face, u, v in [-1, +1], projects radially to sphere
inline Vec3d cubedSphereToCartesian(CubeFace face, double u, double v, double radius) {
    Vec3d p;
    switch (face) {
        case CubeFace::PosX: p = Vec3d( 1.0,    v,   -u); break;
        case CubeFace::NegX: p = Vec3d(-1.0,    v,    u); break;
        case CubeFace::PosY: p = Vec3d(   u,  1.0,   -v); break;
        case CubeFace::NegY: p = Vec3d(   u, -1.0,    v); break;
        case CubeFace::PosZ: p = Vec3d(   u,    v,  1.0); break;
        case CubeFace::NegZ: p = Vec3d(  -u,    v, -1.0); break;
    }
    return p.normalized() * radius;
}

// Catastrophic Disruption Energy Threshold (Q_D*)
// Calculates specific impact energy required to shatter and disperse a planet (J/kg)
// Formula: Q_D* = q_s * (R / 1cm)^-a + q_g * rho * (R / 1cm)^b
inline double calculateCatastrophicDisruptionThreshold(double radiusMeters, double densityKgM3 = kStandardEarthDensity) {
    const double radiusCm = radiusMeters * 100.0;
    // Standard Benz & Asphaug (1999) basalt scaling at hypervelocity v=30 km/s:
    // Strength regime: qs = 9.0e7 erg/g = 9.0e3 J/kg, a = 0.36
    // Gravity regime:  qg = 0.5 erg*cm^3/(g^2) = 5.0e-8 J*m^3/kg^2, b = 1.36
    constexpr double qs = 9.0e3;
    constexpr double a  = 0.36;
    constexpr double qg = 5.0e-8;
    constexpr double b  = 1.36;

    const double qStrength = qs * std::pow(radiusCm, -a);
    const double qGravity  = qg * densityKgM3 * std::pow(radiusCm, b);
    return qStrength + qGravity; // J / kg
}

// Determines if an impactor's kinetic energy exceeds the planetary disruption threshold
inline bool willDisruptPlanet(double impactEnergyJoules, double planetMassKg = kStandardEarthMassKg,
                             double radiusMeters = kStandardEarthRadiusM, double densityKgM3 = kStandardEarthDensity) {
    const double specificEnergy = impactEnergyJoules / planetMassKg;
    const double qThreshold = calculateCatastrophicDisruptionThreshold(radiusMeters, densityKgM3);
    return specificEnergy >= qThreshold;
}

} // namespace planetary
