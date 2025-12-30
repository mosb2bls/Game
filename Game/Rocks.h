#pragma once

#include "Core.h"
#include "Shaders.h"
#include "PSO.h"
#include "Mesh.h"
#include "GEMLoader.h"
#include "HeightmapTerrain.h"
#include "Maths.h"
#include "LOD.h"  
#include <vector>
#include <random>
#include <string>
#include <algorithm>
#include <set>

#pragma comment(lib, "d3dcompiler.lib")

// ============================================================================
// GPU Instance Data - MUST match shader input layout exactly!
// ============================================================================
struct RockInstanceGPU
{
    Vec3 position;      // 12 bytes - INSTANCEPOS
    float rotationY;    // 4 bytes  - INSTANCEROT
    float scale;        // 4 bytes  - INSTANCESCALE
    // Total: 20 bytes
};

// ============================================================================
// CPU Instance Data - Contains extra fields for culling/LOD
// ============================================================================
struct RockInstance
{
    Vec3 position;
    float rotationY;
    float scale;
    int typeIndex;
    float distanceToCamera;
    int lodLevel;
};

// ============================================================================
// Rock Type - Contains mesh LODs and texture
// ============================================================================
struct RockType
{
    std::string name;
    Mesh* meshHigh = nullptr;
    Mesh* meshMedium = nullptr;
    Mesh* meshLow = nullptr;
    Texture texture;
    int typeIndex = 0;
};

// ============================================================================
// Spatial Chunk - For efficient culling
// ============================================================================
struct RockChunk
{
    Vec3 centerPos;
    std::vector<RockInstance> instances;
    bool isVisible = false;
};

// ============================================================================
// ROCKS CLASS
// ============================================================================
class Rocks
{
public:
    std::string shaderName = "RockInstanced";
    std::string psoName = "RockInstancedPSO";

    // Terrain size (configurable)
    float terrainSizeX = 300.0f;
    float terrainSizeZ = 300.0f;

    // ========================================================================
    // INIT METHOD 1: Original - Random generation (density-based)
    // ========================================================================
    void init(Core* core, PSOManager* psos, Shaders* shaders,
        HeightmapTerrain* terrain,
        const std::vector<std::string>& rockModelPaths,
        const std::vector<std::string>& rockTexturePaths,
        float density = 0.5f,
        float minDistance = 3.0f,
        float viewDistance = 100.0f,
        float chunkSize = 32.0f)
    {
        this->terrain = terrain;
        this->viewDistance = viewDistance;
        this->density = density;
        this->chunkSize = chunkSize;

        // 1. Load rock types and AUTO-GENERATE LOD levels
        loadRockTypesWithAutoLOD(core, rockModelPaths, rockTexturePaths);

        if (rockTypes.empty())
        {
            std::cout << "[Rocks] Error: No rock types loaded!\n";
            return;
        }

        // 2. Generate instances randomly
        generateRockChunks(minDistance);

        // 3. Finish initialization
        finishInit(core, psos, shaders);
    }

    // ========================================================================
    // INIT METHOD 2: NEW - Use pre-generated instances from VegetationGenerator
    // ========================================================================
    void initWithInstances(Core* core, PSOManager* psos, Shaders* shaders,
        HeightmapTerrain* terrain,
        const std::vector<std::string>& rockModelPaths,
        const std::vector<std::string>& rockTexturePaths,
        const std::vector<RockInstance>& preGeneratedInstances,
        float viewDistance = 100.0f,
        float chunkSize = 32.0f)
    {
        this->terrain = terrain;
        this->viewDistance = viewDistance;
        this->chunkSize = chunkSize;

        std::cout << "[Rocks] Initializing with " << preGeneratedInstances.size()
            << " pre-generated instances\n";

        // 1. Load rock types and AUTO-GENERATE LOD levels
        loadRockTypesWithAutoLOD(core, rockModelPaths, rockTexturePaths);

        if (rockTypes.empty())
        {
            std::cout << "[Rocks] Error: No rock types loaded!\n";
            return;
        }

        // 2. USE PRE-GENERATED INSTANCES
        allInstances = preGeneratedInstances;

        // Clamp typeIndex to valid range
        int numTypes = (int)rockTypes.size();
        for (auto& inst : allInstances)
        {
            if (inst.typeIndex < 0 || inst.typeIndex >= numTypes)
            {
                inst.typeIndex = inst.typeIndex % numTypes;
                if (inst.typeIndex < 0) inst.typeIndex = 0;
            }
        }

        // 3. Organize into chunks
        organizeIntoChunks();

        // 4. Finish initialization
        finishInit(core, psos, shaders);
    }

    // ========================================================================
    // UPDATE - Call each frame
    // ========================================================================
    void update(const Vec3& cameraPos)
    {
        updateLODLevels(cameraPos);
    }

    // ========================================================================
    // DRAW - Call each frame after update
    // ========================================================================
    void draw(Core* core, PSOManager* psos, Shaders* shaders,
        const Matrix& vp, const Vec3& cameraPos)
    {
        if (rockTypes.empty()) return;

        performChunkCulling(cameraPos);

        Matrix world;
        shaders->updateConstantVS(shaderName, "rockBuffer", "VP", (void*)&vp);
        shaders->updateConstantVS(shaderName, "rockBuffer", "W", (void*)&world);

        Vec4 cameraData(cameraPos.x, cameraPos.y, cameraPos.z, viewDistance);
        shaders->updateConstantVS(shaderName, "rockBuffer", "cameraPos", &cameraData);

        Vec4 lightDir(0.5f, 1.0f, -0.5f, 0.2f);
        shaders->updateConstantPS(shaderName, "rockPSBuffer", "lightDir_ambient", &lightDir);
        shaders->updateConstantPS(shaderName, "rockPSBuffer", "rockColor", &rockColor);

        shaders->apply(core, shaderName);
        psos->bind(core, psoName);

        for (auto& type : rockTypes)
        {
            drawRockType(core, type);
        }
    }

    // ========================================================================
    // PUBLIC PROPERTIES
    // ========================================================================
    Vec4 rockColor = Vec4(0.7f, 0.7f, 0.7f, 1.0f);
    float lodDistanceHigh = 20.0f;
    float lodDistanceMedium = 50.0f;

    // Get instance count
    size_t getInstanceCount() const { return allInstances.size(); }

    // ========================================================================
    // DESTRUCTOR
    // ========================================================================
    ~Rocks()
    {
        std::set<Mesh*> deletedMeshes;
        for (auto& type : rockTypes)
        {
            if (type.meshHigh && deletedMeshes.find(type.meshHigh) == deletedMeshes.end())
            {
                delete type.meshHigh;
                deletedMeshes.insert(type.meshHigh);
            }
            if (type.meshMedium && deletedMeshes.find(type.meshMedium) == deletedMeshes.end())
            {
                delete type.meshMedium;
                deletedMeshes.insert(type.meshMedium);
            }
            if (type.meshLow && deletedMeshes.find(type.meshLow) == deletedMeshes.end())
            {
                delete type.meshLow;
                deletedMeshes.insert(type.meshLow);
            }
        }
        for (auto& buffers : instanceBuffersByType)
        {
            for (auto& buffer : buffers)
            {
                if (buffer) buffer->Release();
            }
        }
    }

private:
    HeightmapTerrain* terrain = nullptr;
    std::vector<RockType> rockTypes;
    std::vector<RockChunk> chunks;
    std::vector<RockInstance> allInstances;

    std::vector<std::vector<std::vector<RockInstance>>> instancesByTypeLOD;
    std::vector<std::vector<std::vector<RockInstance>>> visibleInstancesByTypeLOD;
    std::vector<std::vector<ID3D12Resource*>> instanceBuffersByType;
    std::vector<std::vector<D3D12_VERTEX_BUFFER_VIEW>> instanceBufferViewsByType;

    float density = 0.5f;
    float viewDistance = 100.0f;
    float chunkSize = 32.0f;

    // ========================================================================
    // COMMON INITIALIZATION (shared between both init methods)
    // ========================================================================
    void finishInit(Core* core, PSOManager* psos, Shaders* shaders)
    {
        // Separate by type and LOD
        separateInstancesByTypeAndLOD();

        // Create buffers
        createInstanceBuffers(core);

        // Load shaders
        shaders->load(core, shaderName, "Shaders/VSRock.txt", "Shaders/PSRock.txt");

        // Create PSO
        psos->createPSO(core, psoName,
            shaders->find(shaderName)->vs,
            shaders->find(shaderName)->ps,
            VertexLayoutCache::getRockInstancedLayout());

        printStatistics();
    }

    // ========================================================================
    // ORGANIZE INSTANCES INTO SPATIAL CHUNKS
    // ========================================================================
    void organizeIntoChunks()
    {
        chunks.clear();

        float halfX = terrainSizeX * 0.5f;
        float halfZ = terrainSizeZ * 0.5f;

        int numChunksX = (int)std::ceil(terrainSizeX / chunkSize);
        int numChunksZ = (int)std::ceil(terrainSizeZ / chunkSize);

        // Create empty chunks
        chunks.resize(numChunksX * numChunksZ);

        for (int cz = 0; cz < numChunksZ; cz++)
        {
            for (int cx = 0; cx < numChunksX; cx++)
            {
                int idx = cz * numChunksX + cx;
                float chunkMinX = cx * chunkSize - halfX;
                float chunkMinZ = cz * chunkSize - halfZ;
                chunks[idx].centerPos = Vec3(
                    chunkMinX + chunkSize * 0.5f,
                    0.0f,
                    chunkMinZ + chunkSize * 0.5f
                );
                chunks[idx].isVisible = false;
            }
        }

        // Assign instances to chunks based on position
        for (const auto& inst : allInstances)
        {
            int cx = (int)((inst.position.x + halfX) / chunkSize);
            int cz = (int)((inst.position.z + halfZ) / chunkSize);

            cx = std::clamp(cx, 0, numChunksX - 1);
            cz = std::clamp(cz, 0, numChunksZ - 1);

            int chunkIdx = cz * numChunksX + cx;
            chunks[chunkIdx].instances.push_back(inst);
        }

        std::cout << "[Rocks] Organized " << allInstances.size()
            << " instances into " << chunks.size() << " chunks\n";
    }

    // ========================================================================
    // LOAD ROCK MODELS AND GENERATE LOD
    // ========================================================================
    void loadRockTypesWithAutoLOD(Core* core,
        const std::vector<std::string>& modelPaths,
        const std::vector<std::string>& texturePaths)
    {
        size_t numTypes = std::min(modelPaths.size(), texturePaths.size());

        for (size_t i = 0; i < numTypes; i++)
        {
            std::cout << "\n[Rocks] Loading rock type " << i << ": " << modelPaths[i] << "\n";

            // Load original mesh data
            GEMLoader::GEMModelLoader loader;
            std::vector<GEMLoader::GEMMesh> gemmeshes;
            loader.load(modelPaths[i], gemmeshes);

            if (gemmeshes.empty())
            {
                std::cout << "[Rocks] ERROR: Failed to load model: " << modelPaths[i] << "\n";
                continue;
            }

            // Validation
            if (gemmeshes[0].verticesStatic.empty() || gemmeshes[0].indices.empty())
            {
                std::cout << "[Rocks] ERROR: Model has no geometry: " << modelPaths[i] << "\n";
                continue;
            }

            std::cout << "[Rocks] Loaded mesh with "
                << gemmeshes[0].verticesStatic.size() << " vertices, "
                << gemmeshes[0].indices.size() / 3 << " triangles\n";

            // Convert to STATIC_VERTEX format
            std::vector<STATIC_VERTEX> vertices;
            for (auto& v : gemmeshes[0].verticesStatic)
            {
                STATIC_VERTEX vert;
                memcpy(&vert, &v, sizeof(STATIC_VERTEX));
                vertices.push_back(vert);
            }

            RockType type;
            type.name = "Rock_" + std::to_string(i);
            type.typeIndex = (int)rockTypes.size();

            // AUTO-GENERATE 3 LOD LEVELS
            LODGenerator::generateLODLevels(core, vertices, gemmeshes[0].indices,
                &type.meshHigh,
                &type.meshMedium,
                &type.meshLow);

            // Validation
            if (type.meshHigh == nullptr)
            {
                std::cout << "[Rocks] ERROR: Failed to generate LOD for rock " << i << " - skipping!\n";
                continue;
            }

            // Load texture
            type.texture = core->loadTexture(texturePaths[i]);

            rockTypes.push_back(type);

            std::cout << "[Rocks] Successfully created type " << rockTypes.size() - 1 << " with auto-LOD\n";
        }

        std::cout << "[Rocks] Total rock types loaded: " << rockTypes.size() << "\n";
    }

    // ========================================================================
    // RANDOM GENERATION (Original method)
    // ========================================================================
    void generateRockChunks(float minSpacing)
    {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<float> randScale(0.8f, 1.5f);
        std::uniform_real_distribution<float> randRot(0.0f, 6.28318f);
        std::uniform_int_distribution<int> randType(0, (int)rockTypes.size() - 1);

        float halfX = terrainSizeX * 0.5f;
        float halfZ = terrainSizeZ * 0.5f;

        int numChunksX = (int)(terrainSizeX / chunkSize);
        int numChunksZ = (int)(terrainSizeZ / chunkSize);

        float chunkArea = chunkSize * chunkSize;
        float rocksPerChunk = (density / 100.0f) * chunkArea;

        for (int cz = 0; cz < numChunksZ; cz++)
        {
            for (int cx = 0; cx < numChunksX; cx++)
            {
                RockChunk chunk;
                float chunkMinX = cx * chunkSize - halfX;
                float chunkMinZ = cz * chunkSize - halfZ;
                chunk.centerPos = Vec3(chunkMinX + chunkSize * 0.5f, 0.0f, chunkMinZ + chunkSize * 0.5f);

                std::vector<Vec2> rockPositions = poissonDiskSampling(
                    chunkMinX, chunkMinZ, chunkSize, chunkSize,
                    minSpacing, (int)rocksPerChunk, gen);

                for (const auto& pos2D : rockPositions)
                {
                    float worldY = terrain->sampleHeightWorld(pos2D.x, pos2D.y);
                    RockInstance inst;
                    inst.position = Vec3(pos2D.x, worldY, pos2D.y);
                    inst.rotationY = randRot(gen);
                    inst.scale = randScale(gen);
                    inst.typeIndex = randType(gen);
                    inst.distanceToCamera = 0.0f;
                    inst.lodLevel = 2;
                    chunk.instances.push_back(inst);
                    allInstances.push_back(inst);
                }
                chunks.push_back(chunk);
            }
        }
    }

    std::vector<Vec2> poissonDiskSampling(float minX, float minZ, float width, float height,
        float radius, int numSamples, std::mt19937& gen)
    {
        std::vector<Vec2> samples;
        std::uniform_real_distribution<float> randX(minX, minX + width);
        std::uniform_real_distribution<float> randZ(minZ, minZ + height);
        const int maxAttempts = 30;

        for (int i = 0; i < numSamples; i++)
        {
            for (int attempt = 0; attempt < maxAttempts; attempt++)
            {
                float x = randX(gen);
                float z = randZ(gen);
                bool tooClose = false;
                for (const auto& existing : samples)
                {
                    float dx = x - existing.x;
                    float dz = z - existing.y;
                    if (dx * dx + dz * dz < radius * radius)
                    {
                        tooClose = true;
                        break;
                    }
                }
                if (!tooClose)
                {
                    samples.push_back(Vec2(x, z));
                    break;
                }
            }
        }
        return samples;
    }

    // ========================================================================
    // LOD UPDATE
    // ========================================================================
    void updateLODLevels(const Vec3& cameraPos)
    {
        for (auto& chunk : chunks)
        {
            for (auto& inst : chunk.instances)
            {
                float dx = inst.position.x - cameraPos.x;
                float dz = inst.position.z - cameraPos.z;
                inst.distanceToCamera = sqrtf(dx * dx + dz * dz);

                if (inst.distanceToCamera < lodDistanceHigh)
                    inst.lodLevel = 0;
                else if (inst.distanceToCamera < lodDistanceMedium)
                    inst.lodLevel = 1;
                else
                    inst.lodLevel = 2;
            }
        }
    }

    // ========================================================================
    // SEPARATE INSTANCES BY TYPE AND LOD
    // ========================================================================
    void separateInstancesByTypeAndLOD()
    {
        instancesByTypeLOD.resize(rockTypes.size());
        visibleInstancesByTypeLOD.resize(rockTypes.size());

        for (size_t i = 0; i < rockTypes.size(); i++)
        {
            instancesByTypeLOD[i].resize(3);
            visibleInstancesByTypeLOD[i].resize(3);
        }

        for (const auto& inst : allInstances)
        {
            if (inst.typeIndex >= 0 && inst.typeIndex < (int)rockTypes.size())
                instancesByTypeLOD[inst.typeIndex][inst.lodLevel].push_back(inst);
        }
    }

    // ========================================================================
    // CREATE GPU BUFFERS
    // ========================================================================
    void createInstanceBuffers(Core* core)
    {
        instanceBuffersByType.resize(rockTypes.size());
        instanceBufferViewsByType.resize(rockTypes.size());

        for (size_t t = 0; t < rockTypes.size(); t++)
        {
            instanceBuffersByType[t].resize(3, nullptr);
            instanceBufferViewsByType[t].resize(3);

            // Count TOTAL instances for this type (across all LODs)
            size_t totalInstances = 0;
            for (int lod = 0; lod < 3; lod++)
            {
                totalInstances += instancesByTypeLOD[t][lod].size();
            }

            if (totalInstances == 0) continue;

            // Create buffer for EACH LOD with enough space for ALL instances
            // (because instances can move between LOD levels at runtime)
            for (int lod = 0; lod < 3; lod++)
            {
                // USE RockInstanceGPU size (20 bytes), not RockInstance (32 bytes)!
                UINT bufferSize = (UINT)(totalInstances * sizeof(RockInstanceGPU));

                D3D12_HEAP_PROPERTIES heapProps = {};
                heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

                D3D12_RESOURCE_DESC bufferDesc = {};
                bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bufferDesc.Width = bufferSize;
                bufferDesc.Height = 1;
                bufferDesc.DepthOrArraySize = 1;
                bufferDesc.MipLevels = 1;
                bufferDesc.SampleDesc.Count = 1;
                bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

                HRESULT hr = core->device->CreateCommittedResource(
                    &heapProps,
                    D3D12_HEAP_FLAG_NONE,
                    &bufferDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr,
                    IID_PPV_ARGS(&instanceBuffersByType[t][lod]));

                if (FAILED(hr) || instanceBuffersByType[t][lod] == nullptr)
                {
                    std::cout << "[Rocks] ERROR: Failed to create instance buffer for type "
                        << t << " LOD " << lod << "\n";
                    continue;
                }

                instanceBufferViewsByType[t][lod].BufferLocation =
                    instanceBuffersByType[t][lod]->GetGPUVirtualAddress();
                instanceBufferViewsByType[t][lod].StrideInBytes = sizeof(RockInstanceGPU);
                instanceBufferViewsByType[t][lod].SizeInBytes = bufferSize;
            }
        }
    }

    // ========================================================================
    // CHUNK CULLING AND BUFFER UPDATE
    // ========================================================================
    void performChunkCulling(const Vec3& cameraPos)
    {
        // Clear visible lists
        for (auto& typeLODs : visibleInstancesByTypeLOD)
        {
            for (auto& lodList : typeLODs)
                lodList.clear();
        }

        // Determine which chunks are visible
        float maxDist = viewDistance + chunkSize * 0.5f;
        float maxDistSq = maxDist * maxDist;

        for (auto& chunk : chunks)
        {
            float dx = chunk.centerPos.x - cameraPos.x;
            float dz = chunk.centerPos.z - cameraPos.z;
            chunk.isVisible = (dx * dx + dz * dz <= maxDistSq);

            if (chunk.isVisible)
            {
                for (const auto& inst : chunk.instances)
                {
                    if (inst.typeIndex >= 0 && inst.typeIndex < (int)rockTypes.size() &&
                        inst.lodLevel >= 0 && inst.lodLevel < 3)
                    {
                        visibleInstancesByTypeLOD[inst.typeIndex][inst.lodLevel].push_back(inst);
                    }
                }
            }
        }

        // Upload visible instances to GPU (converting to GPU format)
        for (size_t t = 0; t < rockTypes.size(); t++)
        {
            for (int lod = 0; lod < 3; lod++)
            {
                if (visibleInstancesByTypeLOD[t][lod].empty()) continue;
                if (instanceBuffersByType[t][lod] == nullptr) continue;

                // Convert to GPU format (strip CPU-only fields)
                std::vector<RockInstanceGPU> gpuInstances;
                gpuInstances.reserve(visibleInstancesByTypeLOD[t][lod].size());

                for (const auto& inst : visibleInstancesByTypeLOD[t][lod])
                {
                    RockInstanceGPU gpuInst;
                    gpuInst.position = inst.position;
                    gpuInst.rotationY = inst.rotationY;
                    gpuInst.scale = inst.scale;
                    gpuInstances.push_back(gpuInst);
                }

                // Upload to GPU
                void* mappedData = nullptr;
                D3D12_RANGE readRange = { 0, 0 };
                HRESULT hr = instanceBuffersByType[t][lod]->Map(0, &readRange, &mappedData);

                if (SUCCEEDED(hr) && mappedData)
                {
                    size_t copySize = gpuInstances.size() * sizeof(RockInstanceGPU);
                    memcpy(mappedData, gpuInstances.data(), copySize);
                    instanceBuffersByType[t][lod]->Unmap(0, nullptr);
                }
            }
        }
    }

    // ========================================================================
    // DRAW A SINGLE ROCK TYPE
    // ========================================================================
    int drawRockType(Core* core, RockType& type)
    {
        int totalDrawn = 0;

        for (int lod = 0; lod < 3; lod++)
        {
            int visibleCount = (int)visibleInstancesByTypeLOD[type.typeIndex][lod].size();
            if (visibleCount == 0) continue;

            // Null checks
            if (instanceBuffersByType[type.typeIndex][lod] == nullptr) continue;

            Mesh* mesh = (lod == 0) ? type.meshHigh : (lod == 1) ? type.meshMedium : type.meshLow;
            if (mesh == nullptr) continue;

            // Set texture
            core->getCommandList()->SetGraphicsRootDescriptorTable(2, type.texture.srvHandle);

            // Set vertex buffers (mesh + instance data)
            D3D12_VERTEX_BUFFER_VIEW views[2];
            views[0] = mesh->getVertexBufferView();
            views[1] = instanceBufferViewsByType[type.typeIndex][lod];
            core->getCommandList()->IASetVertexBuffers(0, 2, views);

            // Set index buffer
            D3D12_INDEX_BUFFER_VIEW ibView = mesh->getIndexBufferView();
            core->getCommandList()->IASetIndexBuffer(&ibView);

            // Draw!
            core->getCommandList()->DrawIndexedInstanced(mesh->getIndexCount(), visibleCount, 0, 0, 0);

            totalDrawn += visibleCount;
        }

        return totalDrawn;
    }

    // ========================================================================
    // STATISTICS
    // ========================================================================
    void printStatistics()
    {
        std::cout << "\n[Rocks] Statistics:\n";
        std::cout << "================================\n";
        std::cout << "Total Rocks: " << allInstances.size() << "\n";
        std::cout << "Chunks: " << chunks.size() << "\n\n";

        for (size_t t = 0; t < rockTypes.size(); t++)
        {
            int totalForType = 0;
            std::cout << "Type " << t << " (" << rockTypes[t].name << "):\n";

            for (int lod = 0; lod < 3; lod++)
            {
                int count = (int)instancesByTypeLOD[t][lod].size();
                totalForType += count;
                std::string lodName = (lod == 0) ? "High" : (lod == 1) ? "Medium" : "Low";
                std::cout << "  LOD " << lodName << ": " << count << "\n";
            }

            std::cout << "  Total: " << totalForType << "\n";
        }

        std::cout << "================================\n\n";
    }
};