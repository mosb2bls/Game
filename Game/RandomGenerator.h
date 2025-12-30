#pragma once

#include "Core.h"
#include "Mesh.h"
#include "Maths.h"
#include "HeightmapTerrain.h"
#include <vector>
#include <random>
#include <cmath>
#include <algorithm>
#include <unordered_set>
#include <functional>

// Vegetation generator using grid-based spawn points with jitter to guarantee coverage.
// Uses noise to bias rock vs grass, optional clustering for natural patches,
// and a spatial hash grid to enforce minimum spacing / avoid overlaps.

enum class VegetationType
{
    Grass,
    Rock
};

struct VegetationItem
{
    Vec3 position;
    float rotationY;
    float scale;
    int typeIndex;
    VegetationType type;
    float radius;
};

struct ClusterConfig
{
    float probability = 0.3f;
    int minItems = 3;
    int maxItems = 8;
    float radius = 5.0f;
    float falloff = 2.0f;
};

struct VegetationConfig
{
    // Global density controls: minPointSpacing is a hard lower bound, density adds a spacing target.
    float density = 0.5f;
    float minPointSpacing = 2.0f;

    // Rock/grass mixing: rockProbability is the baseline, noise adds biome-like variation.
    float rockProbability = 0.15f;
    float noiseInfluence = 0.4f;
    float noiseScale = 0.02f;

    // Grass randomization and collision radius.
    float grassMinScale = 0.8f;
    float grassMaxScale = 1.2f;
    float grassRadius = 0.3f;
    ClusterConfig grassCluster;

    // Rock randomization and collision radius.
    float rockMinScale = 0.5f;
    float rockMaxScale = 2.0f;
    float rockRadius = 1.0f;
    ClusterConfig rockCluster;

    // Terrain filters for placement (height + slope in degrees).
    float minSlope = 0.0f;
    float maxSlope = 45.0f;
    float minHeight = -1000.0f;
    float maxHeight = 1000.0f;

    VegetationConfig()
    {
        // Default grass cluster: frequent and fairly dense.
        grassCluster.probability = 0.6f;
        grassCluster.minItems = 5;
        grassCluster.maxItems = 15;
        grassCluster.radius = 3.0f;
        grassCluster.falloff = 1.5f;

        // Default rock cluster: less frequent, smaller counts, wider spread.
        rockCluster.probability = 0.4f;
        rockCluster.minItems = 2;
        rockCluster.maxItems = 5;
        rockCluster.radius = 4.0f;
        rockCluster.falloff = 2.0f;
    }
};

// Spatial hash grid used to accelerate overlap checks by only testing nearby cells.
class SpatialHashGrid
{
public:
    // Build a 2D grid covering [-worldSize/2, +worldSize/2] with given cell size.
    void init(float worldSizeX, float worldSizeZ, float cellSize)
    {
        this->cellSize = cellSize;
        this->offsetX = worldSizeX * 0.5f;
        this->offsetZ = worldSizeZ * 0.5f;
        this->gridWidth = (int)std::ceil(worldSizeX / cellSize) + 1;
        this->gridHeight = (int)std::ceil(worldSizeZ / cellSize) + 1;

        cells.clear();
        cells.resize(gridWidth * gridHeight);
    }

    // Clear all stored items (used when regenerating).
    void clear()
    {
        for (auto& cell : cells)
            cell.clear();
    }

    // Insert an item into the cell corresponding to its XZ position.
    void insert(const VegetationItem& item)
    {
        int cellIdx = getCellIndex(item.position.x, item.position.z);
        if (cellIdx >= 0 && cellIdx < (int)cells.size())
            cells[cellIdx].push_back(item);
    }

    // Check overlap against items in the 3x3 neighborhood of the candidate position.
    bool checkOverlap(float x, float z, float radius) const
    {
        int centerCellX = (int)((x + offsetX) / cellSize);
        int centerCellZ = (int)((z + offsetZ) / cellSize);

        for (int dz = -1; dz <= 1; dz++)
        {
            for (int dx = -1; dx <= 1; dx++)
            {
                int cellX = centerCellX + dx;
                int cellZ = centerCellZ + dz;

                if (cellX < 0 || cellX >= gridWidth || cellZ < 0 || cellZ >= gridHeight)
                    continue;

                int cellIdx = cellZ * gridWidth + cellX;

                for (const auto& item : cells[cellIdx])
                {
                    float distX = item.position.x - x;
                    float distZ = item.position.z - z;
                    float distSq = distX * distX + distZ * distZ;
                    float minDist = item.radius + radius;

                    if (distSq < minDist * minDist)
                        return true;
                }
            }
        }

        return false;
    }

private:
    // Convert world XZ to a linear cell index; returns -1 if out of bounds.
    int getCellIndex(float x, float z) const
    {
        int cellX = (int)((x + offsetX) / cellSize);
        int cellZ = (int)((z + offsetZ) / cellSize);

        if (cellX < 0 || cellX >= gridWidth || cellZ < 0 || cellZ >= gridHeight)
            return -1;

        return cellZ * gridWidth + cellX;
    }

    float cellSize = 5.0f;
    float offsetX = 0.0f;
    float offsetZ = 0.0f;
    int gridWidth = 0;
    int gridHeight = 0;
    std::vector<std::vector<VegetationItem>> cells;
};

// Simple Perlin-style noise plus FBM for biome-like variation.
class NoiseGenerator
{
public:
    // Seeded permutation table so the noise pattern is reproducible per seed.
    NoiseGenerator(unsigned int seed = 12345)
    {
        std::mt19937 rng(seed);
        for (int i = 0; i < 256; i++)
            perm[i] = i;

        std::shuffle(perm, perm + 256, rng);

        for (int i = 0; i < 256; i++)
            perm[256 + i] = perm[i];
    }

    // 2D noise in roughly [-1, 1] using gradient hashing and smooth interpolation.
    float noise2D(float x, float y) const
    {
        int X = (int)std::floor(x) & 255;
        int Y = (int)std::floor(y) & 255;

        x -= std::floor(x);
        y -= std::floor(y);

        float u = fade(x);
        float v = fade(y);

        int A = perm[X] + Y;
        int AA = perm[A];
        int AB = perm[A + 1];
        int B = perm[X + 1] + Y;
        int BA = perm[B];
        int BB = perm[B + 1];

        return lerp(v,
            lerp(u, grad(perm[AA], x, y), grad(perm[BA], x - 1, y)),
            lerp(u, grad(perm[AB], x, y - 1), grad(perm[BB], x - 1, y - 1))
        );
    }

    // FBM layers multiple octaves to create larger coherent regions (biomes).
    float fbm(float x, float y, int octaves = 4, float persistence = 0.5f) const
    {
        float total = 0.0f;
        float amplitude = 1.0f;
        float frequency = 1.0f;
        float maxValue = 0.0f;

        for (int i = 0; i < octaves; i++)
        {
            total += noise2D(x * frequency, y * frequency) * amplitude;
            maxValue += amplitude;
            amplitude *= persistence;
            frequency *= 2.0f;
        }

        return total / maxValue;
    }

private:
    float fade(float t) const { return t * t * t * (t * (t * 6 - 15) + 10); }
    float lerp(float t, float a, float b) const { return a + t * (b - a); }

    // Small set of gradient directions chosen by low bits of hash.
    float grad(int hash, float x, float y) const
    {
        int h = hash & 3;
        float u = h < 2 ? x : y;
        float v = h < 2 ? y : x;
        return ((h & 1) ? -u : u) + ((h & 2) ? -2.0f * v : 2.0f * v);
    }

    int perm[512];
};

class VegetationGenerator
{
public:
    // Main entry: generates grass + rocks over a rectangular terrain area centered at origin.
    // Uses a deterministic seed if provided (seed=0 picks a random seed via random_device).
    void generate(
        HeightmapTerrain* terrain,
        const VegetationConfig& config,
        float terrainSizeX,
        float terrainSizeZ,
        unsigned int seed = 0)
    {
        std::cout << "\n[VegetationGenerator] Starting generation...\n";
        std::cout << "  Terrain size: " << terrainSizeX << " x " << terrainSizeZ << "\n";

        if (seed == 0)
        {
            std::random_device rd;
            seed = rd();
        }

        rng.seed(seed);
        noise = NoiseGenerator(seed);

        this->terrain = terrain;
        this->config = config;
        this->terrainSizeX = terrainSizeX;
        this->terrainSizeZ = terrainSizeZ;

        // Reset output lists and rebuild the spatial grid for this terrain size.
        grassItems.clear();
        rockItems.clear();

        float cellSize = std::max(config.rockRadius, config.grassRadius) * 4.0f;
        spatialGrid.init(terrainSizeX, terrainSizeZ, cellSize);

        // Generate candidate points on a regular grid, then jitter and optionally skip to avoid visible patterns.
        std::cout << "[VegetationGenerator] Generating spawn points (grid-based)...\n";
        std::vector<Vec2> spawnPoints = generateSpawnPointsGrid();
        std::cout << "  Generated " << spawnPoints.size() << " spawn points\n";

        // For each candidate point, choose rock/grass by noise-biased probability and spawn either a single item or a cluster.
        std::cout << "[VegetationGenerator] Placing vegetation...\n";
        int clusterCount = 0;

        for (const auto& point : spawnPoints)
        {
            VegetationType type = determineType(point.x, point.y);
            bool isCluster = shouldGenerateCluster(type);

            if (isCluster)
            {
                clusterCount++;
                generateCluster(point.x, point.y, type);
            }
            else
            {
                tryPlaceItem(point.x, point.y, type);
            }
        }

        std::cout << "[VegetationGenerator] Generation complete!\n";
        std::cout << "  Grass items: " << grassItems.size() << "\n";
        std::cout << "  Rock items: " << rockItems.size() << "\n";
        std::cout << "  Clusters generated: " << clusterCount << "\n";
    }

    // Access the generated vegetation lists (use these to feed your instancing systems).
    const std::vector<VegetationItem>& getGrassItems() const { return grassItems; }
    const std::vector<VegetationItem>& getRockItems() const { return rockItems; }

    // Helper: convert internal rock items into your runtime RockInstance list.
    std::vector<RockInstance> getRockInstances() const
    {
        std::vector<RockInstance> instances;
        instances.reserve(rockItems.size());

        for (const auto& item : rockItems)
        {
            RockInstance inst;
            inst.position = item.position;
            inst.rotationY = item.rotationY;
            inst.scale = item.scale;
            inst.typeIndex = item.typeIndex;
            inst.distanceToCamera = 0.0f;
            inst.lodLevel = 2;
            instances.push_back(inst);
        }

        return instances;
    }

private:
    // Build spawn points covering the full map using grid spacing derived from density/minPointSpacing.
    // Points are jittered within each cell and optionally skipped to create a more organic distribution.
    std::vector<Vec2> generateSpawnPointsGrid()
    {
        std::vector<Vec2> points;

        float halfX = terrainSizeX * 0.5f;
        float halfZ = terrainSizeZ * 0.5f;

        float spacing = config.minPointSpacing;
        if (config.density > 0)
        {
            spacing = std::max(spacing, 1.0f / std::sqrt(config.density));
        }

        int gridCountX = (int)std::ceil(terrainSizeX / spacing);
        int gridCountZ = (int)std::ceil(terrainSizeZ / spacing);

        std::cout << "  Grid: " << gridCountX << " x " << gridCountZ
            << " (spacing: " << spacing << "m)\n";

        float jitterAmount = spacing * 0.4f;
        std::uniform_real_distribution<float> jitterX(-jitterAmount, jitterAmount);
        std::uniform_real_distribution<float> jitterZ(-jitterAmount, jitterAmount);
        std::uniform_real_distribution<float> skipChance(0.0f, 1.0f);

        for (int gz = 0; gz < gridCountZ; gz++)
        {
            for (int gx = 0; gx < gridCountX; gx++)
            {
                float baseX = -halfX + (gx + 0.5f) * spacing;
                float baseZ = -halfZ + (gz + 0.5f) * spacing;

                float x = baseX + jitterX(rng);
                float z = baseZ + jitterZ(rng);

                x = std::clamp(x, -halfX + 1.0f, halfX - 1.0f);
                z = std::clamp(z, -halfZ + 1.0f, halfZ - 1.0f);

                if (skipChance(rng) < 0.1f)
                    continue;

                if (!isValidTerrainLocation(x, z))
                    continue;

                points.push_back(Vec2(x, z));
            }
        }

        std::shuffle(points.begin(), points.end(), rng);
        return points;
    }

    // Validate placement against height and slope constraints by sampling the heightmap.
    bool isValidTerrainLocation(float x, float z)
    {
        if (!terrain) return true;

        float height = terrain->sampleHeightWorld(x, z);

        if (height < config.minHeight || height > config.maxHeight)
            return false;

        float delta = 0.5f;
        float h1 = terrain->sampleHeightWorld(x + delta, z);
        float h2 = terrain->sampleHeightWorld(x - delta, z);
        float h3 = terrain->sampleHeightWorld(x, z + delta);
        float h4 = terrain->sampleHeightWorld(x, z - delta);

        float slopeX = (h1 - h2) / (2.0f * delta);
        float slopeZ = (h3 - h4) / (2.0f * delta);
        float slopeAngle = std::atan(std::sqrt(slopeX * slopeX + slopeZ * slopeZ)) * 180.0f / 3.14159265f;

        if (slopeAngle < config.minSlope || slopeAngle > config.maxSlope)
            return false;

        return true;
    }

    // Decide grass vs rock using a baseline probability plus FBM noise for spatial variation.
    VegetationType determineType(float x, float z)
    {
        float rockChance = config.rockProbability;

        float noiseValue = noise.fbm(x * config.noiseScale, z * config.noiseScale, 4, 0.5f);
        noiseValue = (noiseValue + 1.0f) * 0.5f;

        rockChance += (noiseValue - 0.5f) * config.noiseInfluence * 2.0f;
        rockChance = std::clamp(rockChance, 0.05f, 0.95f);

        std::uniform_real_distribution<float> rand01(0.0f, 1.0f);
        return (rand01(rng) < rockChance) ? VegetationType::Rock : VegetationType::Grass;
    }

    // Decide whether to spawn a cluster at this point based on the per-type cluster probability.
    bool shouldGenerateCluster(VegetationType type)
    {
        const ClusterConfig& clusterConfig = (type == VegetationType::Rock)
            ? config.rockCluster
            : config.grassCluster;

        std::uniform_real_distribution<float> rand01(0.0f, 1.0f);
        return rand01(rng) < clusterConfig.probability;
    }

    // Spawn a cluster: place the center item first, then scatter remaining items with falloff from the center.
    void generateCluster(float centerX, float centerZ, VegetationType type)
    {
        const ClusterConfig& clusterConfig = (type == VegetationType::Rock)
            ? config.rockCluster
            : config.grassCluster;

        std::uniform_int_distribution<int> randCount(clusterConfig.minItems, clusterConfig.maxItems);
        int itemCount = randCount(rng);

        std::uniform_real_distribution<float> randAngle(0.0f, 2.0f * 3.14159265f);
        std::uniform_real_distribution<float> rand01(0.0f, 1.0f);

        tryPlaceItem(centerX, centerZ, type);

        for (int i = 1; i < itemCount; i++)
        {
            float t = rand01(rng);
            float distance = clusterConfig.radius * (1.0f - std::pow(1.0f - t, clusterConfig.falloff));

            float jitter = rand01(rng) * 0.3f + 0.85f;
            distance *= jitter;

            float angle = randAngle(rng);
            float x = centerX + std::cos(angle) * distance;
            float z = centerZ + std::sin(angle) * distance;

            float halfX = terrainSizeX * 0.5f;
            float halfZ = terrainSizeZ * 0.5f;
            if (x < -halfX || x > halfX || z < -halfZ || z > halfZ)
                continue;

            tryPlaceItem(x, z, type);
        }
    }

    // Attempt to place a single item by sampling height, randomizing scale/rotation/typeIndex, and enforcing overlap rules.
    bool tryPlaceItem(float x, float z, VegetationType type)
    {
        if (!terrain) return false;

        if (!isValidTerrainLocation(x, z))
            return false;

        float y = terrain->sampleHeightWorld(x, z);

        float radius, scale;
        int typeIndex;

        std::uniform_real_distribution<float> randRotation(0.0f, 2.0f * 3.14159265f);

        if (type == VegetationType::Rock)
        {
            std::uniform_real_distribution<float> randScale(config.rockMinScale, config.rockMaxScale);
            scale = randScale(rng);
            radius = config.rockRadius * scale;

            std::uniform_int_distribution<int> randType(0, 2);
            typeIndex = randType(rng);
        }
        else
        {
            std::uniform_real_distribution<float> randScale(config.grassMinScale, config.grassMaxScale);
            scale = randScale(rng);
            radius = config.grassRadius * scale;

            std::uniform_int_distribution<int> randType(0, 8);
            typeIndex = randType(rng);
        }

        if (spatialGrid.checkOverlap(x, z, radius))
            return false;

        VegetationItem item;
        item.position = Vec3(x, y, z);
        item.rotationY = randRotation(rng);
        item.scale = scale;
        item.typeIndex = typeIndex;
        item.type = type;
        item.radius = radius;

        if (type == VegetationType::Rock)
            rockItems.push_back(item);
        else
            grassItems.push_back(item);

        spatialGrid.insert(item);
        return true;
    }

    HeightmapTerrain* terrain = nullptr;
    VegetationConfig config;
    float terrainSizeX = 0.0f;
    float terrainSizeZ = 0.0f;

    std::mt19937 rng;
    NoiseGenerator noise;
    SpatialHashGrid spatialGrid;

    std::vector<VegetationItem> grassItems;
    std::vector<VegetationItem> rockItems;
};

namespace VegetationPresets
{
    // Lush meadow: high grass density with only occasional rocks and weaker biome noise.
    inline VegetationConfig Meadow()
    {
        VegetationConfig config;
        config.density = 2.0f;
        config.minPointSpacing = 1.0f;
        config.rockProbability = 0.05f;
        config.noiseInfluence = 0.1f;

        config.grassCluster.probability = 0.7f;
        config.grassCluster.minItems = 8;
        config.grassCluster.maxItems = 20;
        config.grassCluster.radius = 4.0f;

        config.rockCluster.probability = 0.2f;
        config.rockCluster.minItems = 1;
        config.rockCluster.maxItems = 3;

        return config;
    }

    // Rocky preset: fewer overall items, more rocks, and larger rock clusters.
    inline VegetationConfig Rocky()
    {
        VegetationConfig config;
        config.density = 0.5f;
        config.minPointSpacing = 2.5f;
        config.rockProbability = 0.6f;
        config.noiseInfluence = 0.3f;

        config.grassCluster.probability = 0.3f;
        config.grassCluster.minItems = 3;
        config.grassCluster.maxItems = 8;

        config.rockCluster.probability = 0.5f;
        config.rockCluster.minItems = 3;
        config.rockCluster.maxItems = 8;
        config.rockCluster.radius = 6.0f;

        config.rockMinScale = 0.8f;
        config.rockMaxScale = 3.0f;

        return config;
    }

    // Forest floor: balanced mix with stronger noise influence to create varied patches.
    inline VegetationConfig Forest()
    {
        VegetationConfig config;
        config.density = 1.0f;
        config.minPointSpacing = 1.5f;
        config.rockProbability = 0.15f;
        config.noiseInfluence = 0.5f;
        config.noiseScale = 0.03f;

        config.grassCluster.probability = 0.5f;
        config.grassCluster.minItems = 5;
        config.grassCluster.maxItems = 12;

        config.rockCluster.probability = 0.4f;
        config.rockCluster.minItems = 2;
        config.rockCluster.maxItems = 5;

        return config;
    }

    // Desert: sparse items, mostly rocks, weak noise variation, smaller grass.
    inline VegetationConfig Desert()
    {
        VegetationConfig config;
        config.density = 0.2f;
        config.minPointSpacing = 4.0f;
        config.rockProbability = 0.7f;
        config.noiseInfluence = 0.2f;

        config.grassCluster.probability = 0.2f;
        config.grassCluster.minItems = 2;
        config.grassCluster.maxItems = 5;
        config.grassCluster.radius = 2.0f;

        config.rockCluster.probability = 0.3f;
        config.rockCluster.minItems = 1;
        config.rockCluster.maxItems = 4;

        config.grassMinScale = 0.5f;
        config.grassMaxScale = 0.8f;

        return config;
    }

    // Dense preset: near full coverage with frequent grass clusters and occasional rocks.
    inline VegetationConfig Dense()
    {
        VegetationConfig config;
        config.density = 3.0f;
        config.minPointSpacing = 0.8f;
        config.rockProbability = 0.1f;
        config.noiseInfluence = 0.3f;

        config.grassCluster.probability = 0.8f;
        config.grassCluster.minItems = 10;
        config.grassCluster.maxItems = 25;
        config.grassCluster.radius = 5.0f;

        config.rockCluster.probability = 0.3f;
        config.rockCluster.minItems = 2;
        config.rockCluster.maxItems = 4;

        return config;
    }
}
