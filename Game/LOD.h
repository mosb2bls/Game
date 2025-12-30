#pragma once

#include "Core.h"
#include "Mesh.h"
#include <vector>
#include <map>
#include <cmath>
#include <algorithm>

// Simple edge collapse mesh simplification
class MeshSimplifier
{
public:
    // Simplify a mesh by target percentage (0.0 - 1.0)
    // ratio = 0.5 means reduce to 50% of original triangles
    static Mesh* simplifyMesh(Core* core,
        const std::vector<STATIC_VERTEX>& originalVertices,
        const std::vector<unsigned int>& originalIndices,
        float ratio)
    {
        // VALIDATION: Check for empty input
        if (originalVertices.empty() || originalIndices.empty())
        {
            std::cout << "[LOD] ERROR: Empty input mesh - cannot simplify!\n";
            return nullptr;
        }

        // VALIDATION: Check for minimum triangle count
        if (originalIndices.size() < 3)
        {
            std::cout << "[LOD] ERROR: Mesh has less than 1 triangle!\n";
            return nullptr;
        }

        if (ratio >= 1.0f)
        {
            // No simplification needed, return copy of original
            Mesh* mesh = new Mesh();
            std::vector<STATIC_VERTEX> verts = originalVertices;
            std::vector<unsigned int> indices = originalIndices;
            mesh->init(core, verts, indices);
            return mesh;
        }

        int targetTriangleCount = (int)(originalIndices.size() / 3 * ratio);
        targetTriangleCount = std::max(targetTriangleCount, 4); // Minimum 4 triangles

        std::cout << "[LOD] Simplifying: " << originalIndices.size() / 3
            << " tris -> " << targetTriangleCount << " tris (ratio: " << ratio << ")\n";

        // Simple decimation approach: remove vertices based on importance
        std::vector<STATIC_VERTEX> simplifiedVerts;
        std::vector<unsigned int> simplifiedIndices;

        // Calculate vertex importance (based on local curvature/position)
        std::vector<float> vertexImportance = calculateVertexImportance(originalVertices, originalIndices);

        // Cluster-based simplification
        clusterBasedSimplification(originalVertices, originalIndices, vertexImportance,
            targetTriangleCount, simplifiedVerts, simplifiedIndices);

        // VALIDATION: Check if simplification produced valid output
        if (simplifiedVerts.empty() || simplifiedIndices.empty() || simplifiedIndices.size() < 3)
        {
            std::cout << "[LOD] WARNING: Simplification produced empty/invalid mesh - using original!\n";
            // Fall back to original mesh
            Mesh* mesh = new Mesh();
            std::vector<STATIC_VERTEX> verts = originalVertices;
            std::vector<unsigned int> indices = originalIndices;
            mesh->init(core, verts, indices);
            return mesh;
        }

        // Create new mesh
        Mesh* simplifiedMesh = new Mesh();
        simplifiedMesh->init(core, simplifiedVerts, simplifiedIndices);

        return simplifiedMesh;
    }

private:
    // Calculate importance score for each vertex (higher = more important)
    static std::vector<float> calculateVertexImportance(
        const std::vector<STATIC_VERTEX>& vertices,
        const std::vector<unsigned int>& indices)
    {
        std::vector<float> importance(vertices.size(), 0.0f);

        if (vertices.empty() || indices.empty())
            return importance;

        // Build adjacency information
        std::vector<std::vector<int>> adjacentTriangles(vertices.size());

        for (size_t i = 0; i < indices.size(); i += 3)
        {
            // VALIDATION: Check index bounds
            if (indices[i] >= vertices.size() ||
                indices[i + 1] >= vertices.size() ||
                indices[i + 2] >= vertices.size())
            {
                continue; // Skip invalid triangles
            }

            int idx = (int)(i / 3);
            adjacentTriangles[indices[i]].push_back(idx);
            adjacentTriangles[indices[i + 1]].push_back(idx);
            adjacentTriangles[indices[i + 2]].push_back(idx);
        }

        // Calculate importance based on:
        // 1. Number of adjacent triangles (topology)
        // 2. Normal variation (curvature)
        // 3. Distance from center (keep silhouette)

        Vec3 center(0, 0, 0);
        for (const auto& v : vertices)
        {
            center.x += v.pos.x;
            center.y += v.pos.y;
            center.z += v.pos.z;
        }
        center.x /= vertices.size();
        center.y /= vertices.size();
        center.z /= vertices.size();

        for (size_t i = 0; i < vertices.size(); i++)
        {
            // Topology importance (more connections = more important)
            float topoScore = (float)adjacentTriangles[i].size() / 6.0f; // Normalize

            // Curvature importance (calculate normal variation)
            float curvatureScore = 0.0f;
            if (adjacentTriangles[i].size() > 1)
            {
                Vec3 avgNormal(0, 0, 0);
                for (int triIdx : adjacentTriangles[i])
                {
                    int baseIdx = triIdx * 3;
                    if (baseIdx < indices.size())
                    {
                        Vec3 n = vertices[indices[baseIdx]].normal;
                        avgNormal.x += n.x;
                        avgNormal.y += n.y;
                        avgNormal.z += n.z;
                    }
                }

                // VALIDATION: Avoid division by zero
                float len = sqrtf(avgNormal.x * avgNormal.x + avgNormal.y * avgNormal.y + avgNormal.z * avgNormal.z);
                if (len > 0.0001f)
                {
                    avgNormal.x /= len;
                    avgNormal.y /= len;
                    avgNormal.z /= len;
                }

                float variation = 0.0f;
                for (int triIdx : adjacentTriangles[i])
                {
                    int baseIdx = triIdx * 3;
                    if (baseIdx < indices.size())
                    {
                        Vec3 n = vertices[indices[baseIdx]].normal;
                        float nlen = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
                        if (nlen > 0.0001f)
                        {
                            n.x /= nlen;
                            n.y /= nlen;
                            n.z /= nlen;
                        }
                        float dot = avgNormal.x * n.x + avgNormal.y * n.y + avgNormal.z * n.z;
                        variation += (1.0f - dot);
                    }
                }
                curvatureScore = variation / adjacentTriangles[i].size();
            }

            // Silhouette importance (distance from center)
            float dx = vertices[i].pos.x - center.x;
            float dy = vertices[i].pos.y - center.y;
            float dz = vertices[i].pos.z - center.z;
            float distFromCenter = sqrtf(dx * dx + dy * dy + dz * dz);
            float silhouetteScore = distFromCenter;

            // Combine scores
            importance[i] = topoScore * 0.3f + curvatureScore * 0.4f + silhouetteScore * 0.3f;
        }

        return importance;
    }

    // Cluster-based simplification
    static void clusterBasedSimplification(
        const std::vector<STATIC_VERTEX>& originalVertices,
        const std::vector<unsigned int>& originalIndices,
        const std::vector<float>& importance,
        int targetTriangleCount,
        std::vector<STATIC_VERTEX>& outVertices,
        std::vector<unsigned int>& outIndices)
    {
        outVertices.clear();
        outIndices.clear();

        if (originalVertices.empty() || originalIndices.empty())
            return;

        // Simple grid-based clustering approach
        // Divide space into grid cells and keep most important vertex per cell

        // Find bounding box
        Vec3 minBounds(1e9f, 1e9f, 1e9f);
        Vec3 maxBounds(-1e9f, -1e9f, -1e9f);

        for (const auto& v : originalVertices)
        {
            minBounds.x = std::min(minBounds.x, v.pos.x);
            minBounds.y = std::min(minBounds.y, v.pos.y);
            minBounds.z = std::min(minBounds.z, v.pos.z);
            maxBounds.x = std::max(maxBounds.x, v.pos.x);
            maxBounds.y = std::max(maxBounds.y, v.pos.y);
            maxBounds.z = std::max(maxBounds.z, v.pos.z);
        }

        // VALIDATION: Check for degenerate bounding box
        float sizeX = maxBounds.x - minBounds.x;
        float sizeY = maxBounds.y - minBounds.y;
        float sizeZ = maxBounds.z - minBounds.z;

        // Add small epsilon to prevent division by zero
        const float epsilon = 0.0001f;
        if (sizeX < epsilon) sizeX = epsilon;
        if (sizeY < epsilon) sizeY = epsilon;
        if (sizeZ < epsilon) sizeZ = epsilon;

        // Determine grid resolution based on target triangle count
        int gridSize = (int)std::cbrt(originalVertices.size() / std::max(1.0f, targetTriangleCount / 3.0f)) + 1;
        gridSize = std::clamp(gridSize, 2, 100); // Limit grid size

        float cellSizeX = sizeX / gridSize;
        float cellSizeY = sizeY / gridSize;
        float cellSizeZ = sizeZ / gridSize;

        // Ensure cell sizes are not zero
        if (cellSizeX < epsilon) cellSizeX = epsilon;
        if (cellSizeY < epsilon) cellSizeY = epsilon;
        if (cellSizeZ < epsilon) cellSizeZ = epsilon;

        // Map each vertex to grid cell and keep only most important one
        std::map<int, int> cellToVertex; // cell hash -> vertex index

        for (size_t i = 0; i < originalVertices.size(); i++)
        {
            int cellX = (int)((originalVertices[i].pos.x - minBounds.x) / cellSizeX);
            int cellY = (int)((originalVertices[i].pos.y - minBounds.y) / cellSizeY);
            int cellZ = (int)((originalVertices[i].pos.z - minBounds.z) / cellSizeZ);

            cellX = std::clamp(cellX, 0, gridSize - 1);
            cellY = std::clamp(cellY, 0, gridSize - 1);
            cellZ = std::clamp(cellZ, 0, gridSize - 1);

            int cellHash = cellX + cellY * gridSize + cellZ * gridSize * gridSize;

            // Keep most important vertex in this cell
            if (cellToVertex.find(cellHash) == cellToVertex.end())
            {
                cellToVertex[cellHash] = (int)i;
            }
            else
            {
                int existingIdx = cellToVertex[cellHash];
                if (i < importance.size() && existingIdx < importance.size())
                {
                    if (importance[i] > importance[existingIdx])
                    {
                        cellToVertex[cellHash] = (int)i;
                    }
                }
            }
        }

        // Build vertex remap table
        std::vector<int> oldToNew(originalVertices.size(), -1);

        for (const auto& pair : cellToVertex)
        {
            int oldIdx = pair.second;
            int newIdx = (int)outVertices.size();
            oldToNew[oldIdx] = newIdx;
            outVertices.push_back(originalVertices[oldIdx]);
        }

        // Also map vertices that share the same cell to the kept vertex
        for (size_t i = 0; i < originalVertices.size(); i++)
        {
            if (oldToNew[i] == -1)
            {
                // Find which cell this vertex belongs to
                int cellX = (int)((originalVertices[i].pos.x - minBounds.x) / cellSizeX);
                int cellY = (int)((originalVertices[i].pos.y - minBounds.y) / cellSizeY);
                int cellZ = (int)((originalVertices[i].pos.z - minBounds.z) / cellSizeZ);

                cellX = std::clamp(cellX, 0, gridSize - 1);
                cellY = std::clamp(cellY, 0, gridSize - 1);
                cellZ = std::clamp(cellZ, 0, gridSize - 1);

                int cellHash = cellX + cellY * gridSize + cellZ * gridSize * gridSize;

                auto it = cellToVertex.find(cellHash);
                if (it != cellToVertex.end())
                {
                    oldToNew[i] = oldToNew[it->second];
                }
            }
        }

        // Rebuild indices
        for (size_t i = 0; i < originalIndices.size(); i += 3)
        {
            if (originalIndices[i] >= oldToNew.size() ||
                originalIndices[i + 1] >= oldToNew.size() ||
                originalIndices[i + 2] >= oldToNew.size())
            {
                continue; // Skip invalid indices
            }

            int v0 = oldToNew[originalIndices[i]];
            int v1 = oldToNew[originalIndices[i + 1]];
            int v2 = oldToNew[originalIndices[i + 2]];

            // Only add triangle if all vertices are kept and not degenerate
            if (v0 >= 0 && v1 >= 0 && v2 >= 0 && v0 != v1 && v1 != v2 && v0 != v2)
            {
                outIndices.push_back(v0);
                outIndices.push_back(v1);
                outIndices.push_back(v2);
            }
        }

        std::cout << "[LOD] Created simplified mesh: "
            << outVertices.size() << " verts, "
            << outIndices.size() / 3 << " tris\n";
    }
};

// LOD Generator - creates multiple LOD levels from a single mesh
class LODGenerator
{
public:
    // Generate 3 LOD levels from one mesh
    // Returns: [High, Medium, Low]
    static void generateLODLevels(Core* core,
        const std::vector<STATIC_VERTEX>& originalVertices,
        const std::vector<unsigned int>& originalIndices,
        Mesh** outHigh,
        Mesh** outMedium,
        Mesh** outLow)
    {
        // Initialize outputs to nullptr
        *outHigh = nullptr;
        *outMedium = nullptr;
        *outLow = nullptr;

        // VALIDATION: Check for empty input
        if (originalVertices.empty() || originalIndices.empty())
        {
            std::cout << "[LOD] ERROR: Cannot generate LOD from empty mesh!\n";
            return;
        }

        std::cout << "[LOD] Generating LOD levels from mesh with "
            << originalIndices.size() / 3 << " triangles, "
            << originalVertices.size() << " vertices\n";

        // High detail: 100% of original
        *outHigh = MeshSimplifier::simplifyMesh(core, originalVertices, originalIndices, 1.0f);

        if (*outHigh == nullptr)
        {
            std::cout << "[LOD] ERROR: Failed to create high-detail mesh!\n";
            return;
        }

        // Medium detail: 40% of original
        *outMedium = MeshSimplifier::simplifyMesh(core, originalVertices, originalIndices, 0.4f);

        // Low detail: 10% of original  
        *outLow = MeshSimplifier::simplifyMesh(core, originalVertices, originalIndices, 0.1f);

        // FALLBACK: If any LOD failed, use the high-detail mesh
        if (*outMedium == nullptr)
        {
            std::cout << "[LOD] WARNING: Medium LOD failed, using high-detail mesh\n";
            *outMedium = *outHigh;
        }
        if (*outLow == nullptr)
        {
            std::cout << "[LOD] WARNING: Low LOD failed, using high-detail mesh\n";
            *outLow = *outHigh;
        }
    }
};