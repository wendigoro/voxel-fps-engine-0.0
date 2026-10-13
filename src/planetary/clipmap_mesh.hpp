#pragma once
// In-Engine Planetary Destruction & Multi-Scale Simulation Engine
// Multi-Scale Cubed-Sphere Quadtree Clipmap Mesh Generation & Horizon Culling.
//
// Governed by RULES.md: View only reads what simulation sends; zero feedback to sim.

#include "planetary_types.hpp"

#include <cstdint>
#include <cmath>
#include <vector>
#include <array>
#include <algorithm>

namespace planetary {

// Vertex format for GPU-friendly Cubed-Sphere LOD Clipmap rendering
struct ClipmapMeshVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float nx = 0.0f;
    float ny = 1.0f;
    float nz = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    float elevationM = 0.0f;
    uint32_t materialId = 1; // CrustMaterial
};

// Complete Mesh Data for a single Cubed-Sphere Quadtree Tile
struct ClipmapTileMesh {
    ZTileKey key;
    std::vector<ClipmapMeshVertex> vertices;
    std::vector<uint32_t> indices;
    Vec3d centerSpherePos;
    double boundingRadiusM = 0.0;
    bool isCulledByHorizon = false;
};

// Evaluates spherical horizon culling for a tile given camera world position
inline bool isTileCulledByHorizon(const Vec3d& tileCenterM, double tileRadiusM,
                                  const Vec3d& cameraPosM, double planetRadiusM) {
    const double camDist = std::sqrt(cameraPosM.lengthSq());
    if (camDist <= planetRadiusM) {
        // Camera is on or inside planet surface; horizon culling not applicable
        return false;
    }

    // Angular distance to the geometric planetary horizon: cos(alpha) = R / ||C||
    const double cosHorizon = planetRadiusM / camDist;
    const double alphaHorizonRad = std::acos(std::clamp(cosHorizon, -1.0, 1.0));

    // Angle between camera vector and tile center vector
    const Vec3d normCam = cameraPosM.normalized();
    const Vec3d normTile = tileCenterM.normalized();
    const double dot = std::clamp(normCam.dot(normTile), -1.0, 1.0);
    const double angleTileRad = std::acos(dot);

    // Angular radius subtended by the tile
    const double angleTileRadiusRad = std::asin(std::clamp(tileRadiusM / planetRadiusM, 0.0, 1.0));

    // If tile is entirely behind the horizon limb, cull it
    if (angleTileRad > (alphaHorizonRad + angleTileRadiusRad)) {
        return true;
    }
    return false;
}

// Generates an NxN regular clipmap patch for a given Z-Tile on a cube face
inline ClipmapTileMesh generateClipmapTileMesh(const ZTileKey& key, int gridResolution = 16,
                                              double planetRadiusM = kStandardEarthRadiusM,
                                              double skirtDepthM = 50000.0) {
    ClipmapTileMesh mesh;
    mesh.key = key;

    const int N = std::clamp(gridResolution, 4, 64);
    const uint32_t totalTiles = (key.zoom > 0) ? (1U << key.zoom) : 1U;
    const double tileStep = 2.0 / static_cast<double>(totalTiles); // Total cube face spans [-1, 1] (width 2)

    // Cube face local origin in [-1, 1]
    const double u0 = -1.0 + key.x * tileStep;
    const double v0 = -1.0 + key.y * tileStep;
    const double du = tileStep / static_cast<double>(N - 1);
    const double dv = tileStep / static_cast<double>(N - 1);

    mesh.vertices.reserve(N * N + (4 * N)); // Interior grid + skirts
    mesh.indices.reserve((N - 1) * (N - 1) * 6 + (4 * (N - 1) * 6));

    // 1. Generate regular grid interior vertices
    for (int j = 0; j < N; ++j) {
        for (int i = 0; i < N; ++i) {
            const double u = u0 + i * du;
            const double v = v0 + j * dv;

            // Map face (u, v) to 3D cube point
            Vec3d cubePt;
            switch (static_cast<CubeFace>(key.face)) {
                case CubeFace::PosX: cubePt = Vec3d(1.0, -v, -u); break;
                case CubeFace::NegX: cubePt = Vec3d(-1.0, -v, u); break;
                case CubeFace::PosY: cubePt = Vec3d(u, 1.0, v);   break;
                case CubeFace::NegY: cubePt = Vec3d(u, -1.0, -v); break;
                case CubeFace::PosZ: cubePt = Vec3d(u, -v, 1.0);  break;
                case CubeFace::NegZ: cubePt = Vec3d(-u, -v, -1.0); break;
            }

            // Project normalized cube vector onto sphere
            const Vec3d sphereNorm = cubePt.normalized();
            const Vec3d spherePt = sphereNorm * planetRadiusM;

            ClipmapMeshVertex vert;
            vert.x = static_cast<float>(spherePt.x);
            vert.y = static_cast<float>(spherePt.y);
            vert.z = static_cast<float>(spherePt.z);
            vert.nx = static_cast<float>(sphereNorm.x);
            vert.ny = static_cast<float>(sphereNorm.y);
            vert.nz = static_cast<float>(sphereNorm.z);
            vert.u = static_cast<float>(i) / static_cast<float>(N - 1);
            vert.v = static_cast<float>(j) / static_cast<float>(N - 1);
            vert.elevationM = 0.0f;
            vert.materialId = (key.face == 2 || key.face == 3) ? 4 : 1; // IceShield at poles, Granite elsewhere

            mesh.vertices.push_back(vert);
        }
    }

    // Compute center and bounding radius
    const int midIdx = (N / 2) * N + (N / 2);
    mesh.centerSpherePos = Vec3d(mesh.vertices[midIdx].x, mesh.vertices[midIdx].y, mesh.vertices[midIdx].z);
    const double cornerDist = (Vec3d(mesh.vertices[0].x, mesh.vertices[0].y, mesh.vertices[0].z) - mesh.centerSpherePos).length();
    mesh.boundingRadiusM = cornerDist;

    // 2. Build interior index buffer (two triangles per cell)
    for (int j = 0; j < N - 1; ++j) {
        for (int i = 0; i < N - 1; ++i) {
            const uint32_t i0 = j * N + i;
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = (j + 1) * N + i;
            const uint32_t i3 = i2 + 1;

            // Triangle 1
            mesh.indices.push_back(i0);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i1);

            // Triangle 2
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i3);
        }
    }

    // 3. Extrude boundary skirts downward into planetary interior to prevent LOD cracking
    const uint32_t skirtBaseIdx = static_cast<uint32_t>(mesh.vertices.size());
    // Skirt along bottom row (j = 0)
    for (int i = 0; i < N; ++i) {
        ClipmapMeshVertex sv = mesh.vertices[i];
        sv.x -= static_cast<float>(sv.nx * skirtDepthM);
        sv.y -= static_cast<float>(sv.ny * skirtDepthM);
        sv.z -= static_cast<float>(sv.nz * skirtDepthM);
        mesh.vertices.push_back(sv);
    }
    for (int i = 0; i < N - 1; ++i) {
        const uint32_t topA = i;
        const uint32_t topB = i + 1;
        const uint32_t botA = skirtBaseIdx + i;
        const uint32_t botB = skirtBaseIdx + i + 1;

        mesh.indices.push_back(topA);
        mesh.indices.push_back(botA);
        mesh.indices.push_back(topB);

        mesh.indices.push_back(topB);
        mesh.indices.push_back(botA);
        mesh.indices.push_back(botB);
    }

    return mesh;
}

} // namespace planetary
