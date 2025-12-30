#pragma once

#include "Core.h"
#include "Shaders.h"
#include "PSO.h"
#include "Mesh.h"
#include "GEMLoader.h"
#include "HeightmapTerrain.h"
#include "Maths.h"
#include <vector>
#include <random>
#include <string>

struct GrassTypeConfig
{
    std::string modelPath;
    std::string texturePath;
    float weight;
    std::string name;
};

struct GrassGroupConfig
{
    std::string groupName;
    float groupWeight;
    std::vector<GrassTypeConfig> types;
};

struct GrassType
{
    Mesh* mesh = nullptr;
    Texture texture;
    std::string name;
    int groupIndex = 0;
    int typeIndex = 0;
};

struct GrassInstanceGPU
{
    Vec3 position;
    float rotationY;
    float scale;
    float windPhase;
};

struct GrassInstance
{
    Vec3 position;
    float rotationY;
    float scale;
    float windPhase;
    int groupIndex;
    int typeIndex;
};

struct GrassGroup
{
    std::string name;
    std::vector<GrassType> types;
    std::vector<std::vector<GrassInstance>> instancesByType;
    std::vector<std::vector<GrassInstance>> visibleInstancesByType;
    std::vector<ID3D12Resource*> instanceBuffers;
    std::vector<D3D12_VERTEX_BUFFER_VIEW> instanceBufferViews;
};

class HybridGrassField
{
public:
    std::string shaderName = "GrassInstanced";
    std::string psoName = "GrassInstancedPSO";

    float terrainSizeX = 300.0f;
    float terrainSizeZ = 300.0f;
    float viewDistance = 50.0f;
    void init(Core* core, PSOManager* psos, Shaders* shaders,
        HeightmapTerrain* terrain,
        const std::vector<GrassGroupConfig>& groupConfigs,
        float density = 3.0f,
        float minDistance = 0.5f,
        float viewDistance = 50.0f,
        float chunkSize = 16.0f)
    {
        // Cache terrain + rendering parameters
        this->terrain = terrain;
        this->viewDistance = viewDistance;
        this->density = density;
        this->chunkSize = chunkSize;

        // Load meshes/textures for all configured groups/types
        loadGrassGroups(core, groupConfigs);

        if (groups.empty())
        {
            std::cout << "[HybridGrassField] Error: No grass groups loaded!\n";
            return;
        }

        // Normalize group/type weights for stable weighted selection
        normalizeWeights(groupConfigs);

        // Generate random instances across the terrain, stored per chunk
        generateWeightedGrassChunks(minDistance);

        // Build per-type lists, create GPU instance buffers, then setup shaders/PSO
        finishInit(core, psos, shaders);
    }

    void initWithInstances(Core* core, PSOManager* psos, Shaders* shaders,
        HeightmapTerrain* terrain,
        const std::vector<GrassGroupConfig>& groupConfigs,
        const std::vector<GrassInstance>& preGeneratedInstances,
        float viewDistance = 100.0f,
        float chunkSize = 16.0f)
    {
        // Cache terrain + rendering parameters (instances are provided externally)
        this->terrain = terrain;
        this->viewDistance = viewDistance;
        this->chunkSize = chunkSize;

        std::cout << "[HybridGrassField] Initializing with " << preGeneratedInstances.size()
            << " pre-generated instances\n";

        // Load meshes/textures for all configured groups/types
        loadGrassGroups(core, groupConfigs);

        if (groups.empty())
        {
            std::cout << "[HybridGrassField] Error: No grass groups loaded!\n";
            return;
        }

        // Normalize weights (still useful for distribution logic and validation)
        normalizeWeights(groupConfigs);

        // Copy instances and clamp indices to current group/type ranges
        allInstances = preGeneratedInstances;

        for (auto& inst : allInstances)
        {
            if (inst.groupIndex < 0 || inst.groupIndex >= (int)groups.size())
            {
                inst.groupIndex = inst.groupIndex % (int)groups.size();
                if (inst.groupIndex < 0) inst.groupIndex = 0;
            }

            int maxType = (int)groups[inst.groupIndex].types.size();
            if (maxType > 0 && (inst.typeIndex < 0 || inst.typeIndex >= maxType))
            {
                inst.typeIndex = inst.typeIndex % maxType;
                if (inst.typeIndex < 0) inst.typeIndex = 0;
            }
        }

        // Build chunk lists so culling can work the same way as random generation
        organizeIntoChunks();

        // Build per-type lists, create GPU instance buffers, then setup shaders/PSO
        finishInit(core, psos, shaders);
    }

    void update(float deltaTime)
    {
        // Advance wind animation time
        windTime += deltaTime;
    }

    void draw(Core* core, PSOManager* psos, Shaders* shaders,
        const Matrix& vp, const Vec3& cameraPos)
    {
        if (groups.empty()) return;

        // Culling step: mark visible chunks, rebuild visible instance lists, update buffers
        performChunkCulling(cameraPos);

        // Update shared shader constants (VP, wind, camera, lighting, colours)
        Matrix world;
        shaders->updateConstantVS(shaderName, "grassBuffer", "VP", (void*)&vp);
        shaders->updateConstantVS(shaderName, "grassBuffer", "W", (void*)&world);

        Vec4 windData(windDirection.x, windDirection.y, windStrength, windTime);
        shaders->updateConstantVS(shaderName, "grassBuffer", "windParams", &windData);

        Vec4 cameraData(cameraPos.x, cameraPos.y, cameraPos.z, viewDistance);
        shaders->updateConstantVS(shaderName, "grassBuffer", "cameraPos", &cameraData);

        Vec4 lightDir(0.5f, 1.0f, -0.5f, 0.3f);
        shaders->updateConstantPS(shaderName, "grassPSBuffer", "lightDir_ambient", &lightDir);

        shaders->updateConstantPS(shaderName, "grassPSBuffer", "grassColorTop", &colorTop);
        shaders->updateConstantPS(shaderName, "grassPSBuffer", "grassColorBottom", &colorBottom);

        // Bind shaders + PSO once, then draw each group/type with instancing
        shaders->apply(core, shaderName);
        psos->bind(core, psoName);

        for (auto& group : groups)
        {
            drawGroup(core, group);
        }
    }

    Vec2 windDirection = Vec2(1.0f, 0.5f);
    float windStrength = 1.5f;
    float windSpeed = 1.0f;

    Vec4 colorTop = Vec4(0.6f, 0.9f, 0.5f, 1.0f);
    Vec4 colorBottom = Vec4(0.3f, 0.5f, 0.2f, 1.0f);

    size_t getInstanceCount() const { return allInstances.size(); }

    ~HybridGrassField()
    {
        // Release meshes and per-type instance buffers
        for (auto& group : groups)
        {
            for (auto& type : group.types)
            {
                if (type.mesh) delete type.mesh;
            }
            for (auto& buffer : group.instanceBuffers)
            {
                if (buffer) buffer->Release();
            }
        }
    }

private:
    HeightmapTerrain* terrain = nullptr;
    std::vector<GrassGroup> groups;

    struct GrassChunk
    {
        Vec3 centerPos;
        std::vector<GrassInstance> instances;
        bool isVisible = false;
    };

    std::vector<GrassChunk> chunks;
    std::vector<GrassInstance> allInstances;

    std::vector<float> normalizedGroupWeights;
    std::vector<std::vector<float>> normalizedTypeWeights;

    float density = 3.0f;
    
    float chunkSize = 16.0f;
    float windTime = 0.0f;

    void finishInit(Core* core, PSOManager* psos, Shaders* shaders)
    {
        // Organize instances into per-group/per-type containers
        separateInstancesByGroupAndType();

        // Allocate one GPU instance buffer per type (updated during culling)
        createInstanceBuffers(core);

        // Load shared grass shaders and build instanced PSO
        shaders->load(core, shaderName, "Shaders/VSGrass.txt", "Shaders/PSGrass.txt");

        psos->createPSO(core, psoName,
            shaders->find(shaderName)->vs,
            shaders->find(shaderName)->ps,
            VertexLayoutCache::getGrassInstancedLayout());

        // Print a quick breakdown of distribution and draw call count
        printStatistics();
    }

    void organizeIntoChunks()
    {
        // Build an empty chunk grid covering the terrain bounds
        chunks.clear();

        float halfX = terrainSizeX * 0.5f;
        float halfZ = terrainSizeZ * 0.5f;

        int numChunksX = (int)std::ceil(terrainSizeX / chunkSize);
        int numChunksZ = (int)std::ceil(terrainSizeZ / chunkSize);

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

        // Assign each instance to a chunk based on its x/z position
        for (const auto& inst : allInstances)
        {
            int cx = (int)((inst.position.x + halfX) / chunkSize);
            int cz = (int)((inst.position.z + halfZ) / chunkSize);

            cx = std::clamp(cx, 0, numChunksX - 1);
            cz = std::clamp(cz, 0, numChunksZ - 1);

            int chunkIdx = cz * numChunksX + cx;
            chunks[chunkIdx].instances.push_back(inst);
        }

        std::cout << "[HybridGrassField] Organized " << allInstances.size()
            << " instances into " << chunks.size() << " chunks\n";
    }

    void loadGrassGroups(Core* core, const std::vector<GrassGroupConfig>& configs)
    {
        // Load each group and its types (mesh + texture)
        for (size_t g = 0; g < configs.size(); g++)
        {
            const auto& groupConfig = configs[g];

            GrassGroup group;
            group.name = groupConfig.groupName;

            for (size_t t = 0; t < groupConfig.types.size(); t++)
            {
                const auto& typeConfig = groupConfig.types[t];

                GrassType type;
                type.name = typeConfig.name;
                type.groupIndex = (int)g;
                type.typeIndex = (int)t;

                type.mesh = loadGrassModel(core, typeConfig.modelPath);
                if (!type.mesh)
                {
                    std::cout << "[HybridGrassField] Failed to load: "
                        << typeConfig.modelPath << "\n";
                    continue;
                }

                type.texture = core->loadTexture(typeConfig.texturePath);

                group.types.push_back(type);
            }

            if (!group.types.empty())
            {
                groups.push_back(group);
            }
        }
    }

    Mesh* loadGrassModel(Core* core, const std::string& path)
    {
        // Load GEM mesh and upload to Mesh class
        GEMLoader::GEMModelLoader loader;
        std::vector<GEMLoader::GEMMesh> gemmeshes;
        loader.load(path, gemmeshes);

        if (gemmeshes.empty()) return nullptr;

        Mesh* mesh = new Mesh();
        std::vector<STATIC_VERTEX> vertices;
        for (auto& v : gemmeshes[0].verticesStatic)
        {
            STATIC_VERTEX vert;
            memcpy(&vert, &v, sizeof(STATIC_VERTEX));
            vertices.push_back(vert);
        }
        mesh->init(core, vertices, gemmeshes[0].indices);
        return mesh;
    }

    void normalizeWeights(const std::vector<GrassGroupConfig>& configs)
    {
        // Normalize group weights so they sum to 1
        float totalGroupWeight = 0.0f;
        for (const auto& config : configs)
        {
            totalGroupWeight += config.groupWeight;
        }

        normalizedGroupWeights.clear();
        for (const auto& config : configs)
        {
            float normalized = (totalGroupWeight > 0.0f) ? config.groupWeight / totalGroupWeight : 0.0f;
            normalizedGroupWeights.push_back(normalized);
        }

        // Normalize per-type weights within each group
        normalizedTypeWeights.clear();
        for (const auto& config : configs)
        {
            float totalTypeWeight = 0.0f;
            for (const auto& type : config.types)
            {
                totalTypeWeight += type.weight;
            }

            std::vector<float> typeWeights;
            for (const auto& type : config.types)
            {
                float normalized = (totalTypeWeight > 0.0f) ? type.weight / totalTypeWeight : 0.0f;
                typeWeights.push_back(normalized);
            }
            normalizedTypeWeights.push_back(typeWeights);
        }
    }

    void generateWeightedGrassChunks(float minSpacing)
    {
        // Random generators for transform variation and weighted selection
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<float> randScale(0.8f, 1.2f);
        std::uniform_real_distribution<float> randRot(0.0f, 6.28318f);
        std::uniform_real_distribution<float> randPhase(0.0f, 6.28318f);
        std::uniform_real_distribution<float> rand01(0.0f, 1.0f);
        std::uniform_real_distribution<float> randOffset(-minSpacing * 0.3f, minSpacing * 0.3f);

        float halfX = terrainSizeX * 0.5f;
        float halfZ = terrainSizeZ * 0.5f;

        int numChunksX = (int)(terrainSizeX / chunkSize);
        int numChunksZ = (int)(terrainSizeZ / chunkSize);

        // Convert density into approximate grid spacing
        float spacing = 1.0f / std::sqrt(density);

        for (int cz = 0; cz < numChunksZ; cz++)
        {
            for (int cx = 0; cx < numChunksX; cx++)
            {
                GrassChunk chunk;

                float chunkMinX = cx * chunkSize - halfX;
                float chunkMinZ = cz * chunkSize - halfZ;

                chunk.centerPos = Vec3(
                    chunkMinX + chunkSize * 0.5f,
                    0.0f,
                    chunkMinZ + chunkSize * 0.5f
                );

                int gridCount = (int)(chunkSize / spacing);

                // Place one instance per grid cell with a small random offset
                for (int z = 0; z < gridCount; z++)
                {
                    for (int x = 0; x < gridCount; x++)
                    {
                        float worldX = chunkMinX + (x * spacing) + randOffset(gen);
                        float worldZ = chunkMinZ + (z * spacing) + randOffset(gen);
                        float worldY = terrain->sampleHeightWorld(worldX, worldZ);

                        // Weighted choice of group/type determines which mesh+texture to use
                        int selectedGroup = selectWeightedGroup(rand01(gen));
                        int selectedType = selectWeightedType(selectedGroup, rand01(gen));

                        GrassInstance inst;
                        inst.position = Vec3(worldX, worldY, worldZ);
                        inst.rotationY = randRot(gen);
                        inst.scale = randScale(gen);
                        inst.windPhase = randPhase(gen);
                        inst.groupIndex = selectedGroup;
                        inst.typeIndex = selectedType;

                        chunk.instances.push_back(inst);
                        allInstances.push_back(inst);
                    }
                }

                chunks.push_back(chunk);
            }
        }
    }

    int selectWeightedGroup(float randomValue)
    {
        // Roulette-wheel selection over normalized group weights
        float cumulative = 0.0f;
        for (size_t i = 0; i < normalizedGroupWeights.size(); i++)
        {
            cumulative += normalizedGroupWeights[i];
            if (randomValue <= cumulative)
            {
                return (int)i;
            }
        }
        return (int)normalizedGroupWeights.size() - 1;
    }

    int selectWeightedType(int groupIndex, float randomValue)
    {
        // Roulette-wheel selection over normalized type weights for a given group
        if (groupIndex < 0 || groupIndex >= (int)normalizedTypeWeights.size())
            return 0;

        const auto& weights = normalizedTypeWeights[groupIndex];
        float cumulative = 0.0f;
        for (size_t i = 0; i < weights.size(); i++)
        {
            cumulative += weights[i];
            if (randomValue <= cumulative)
            {
                return (int)i;
            }
        }
        return (int)weights.size() - 1;
    }

    void separateInstancesByGroupAndType()
    {
        // Allocate per-type arrays so we can batch by mesh/texture
        for (auto& group : groups)
        {
            group.instancesByType.resize(group.types.size());
            group.visibleInstancesByType.resize(group.types.size());
        }

        // Distribute all instances into their group/type buckets
        for (const auto& inst : allInstances)
        {
            if (inst.groupIndex >= 0 && inst.groupIndex < (int)groups.size())
            {
                auto& group = groups[inst.groupIndex];
                if (inst.typeIndex >= 0 && inst.typeIndex < (int)group.instancesByType.size())
                {
                    group.instancesByType[inst.typeIndex].push_back(inst);
                }
            }
        }
    }

    void createInstanceBuffers(Core* core)
    {
        // Create one upload buffer per type; later we map and overwrite visible instances
        for (auto& group : groups)
        {
            group.instanceBuffers.resize(group.types.size(), nullptr);
            group.instanceBufferViews.resize(group.types.size());

            size_t totalGroupInstances = 0;
            for (size_t t = 0; t < group.types.size(); t++)
            {
                totalGroupInstances += group.instancesByType[t].size();
            }

            for (size_t t = 0; t < group.types.size(); t++)
            {
                size_t maxInstances = totalGroupInstances;
                if (maxInstances == 0) maxInstances = 1000;

                UINT bufferSize = (UINT)(maxInstances * sizeof(GrassInstanceGPU));

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
                    &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&group.instanceBuffers[t])
                );

                if (FAILED(hr) || group.instanceBuffers[t] == nullptr)
                {
                    std::cout << "[HybridGrassField] ERROR: Failed to create buffer for group "
                        << group.name << " type " << t << "\n";
                    continue;
                }

                group.instanceBufferViews[t].BufferLocation =
                    group.instanceBuffers[t]->GetGPUVirtualAddress();
                group.instanceBufferViews[t].StrideInBytes = sizeof(GrassInstanceGPU);
                group.instanceBufferViews[t].SizeInBytes = bufferSize;
            }
        }
    }

    void performChunkCulling(const Vec3& cameraPos)
    {
        // Clear visible lists so we can rebuild them for this frame
        for (auto& group : groups)
        {
            for (auto& list : group.visibleInstancesByType)
            {
                list.clear();
            }
        }

        // Coarse distance test against chunk centers
        float maxDist = viewDistance + chunkSize * 0.5f;
        float maxDistSq = maxDist * maxDist;

        for (auto& chunk : chunks)
        {
            float dx = chunk.centerPos.x - cameraPos.x;
            float dz = chunk.centerPos.z - cameraPos.z;
            float distSq = dx * dx + dz * dz;

            chunk.isVisible = (distSq <= maxDistSq);

            if (chunk.isVisible)
            {
                // Append chunk instances into per-group/per-type visible buckets
                for (const auto& inst : chunk.instances)
                {
                    if (inst.groupIndex >= 0 && inst.groupIndex < (int)groups.size())
                    {
                        auto& group = groups[inst.groupIndex];
                        if (inst.typeIndex >= 0 && inst.typeIndex < (int)group.visibleInstancesByType.size())
                        {
                            group.visibleInstancesByType[inst.typeIndex].push_back(inst);
                        }
                    }
                }
            }
        }

        // For each type, convert to GPU layout and upload visible instances
        for (auto& group : groups)
        {
            for (size_t t = 0; t < group.types.size(); t++)
            {
                if (group.visibleInstancesByType[t].empty()) continue;
                if (group.instanceBuffers[t] == nullptr) continue;

                std::vector<GrassInstanceGPU> gpuInstances;
                gpuInstances.reserve(group.visibleInstancesByType[t].size());

                for (const auto& inst : group.visibleInstancesByType[t])
                {
                    GrassInstanceGPU gpuInst;
                    gpuInst.position = inst.position;
                    gpuInst.rotationY = inst.rotationY;
                    gpuInst.scale = inst.scale;
                    gpuInst.windPhase = inst.windPhase;
                    gpuInstances.push_back(gpuInst);
                }

                void* mappedData = nullptr;
                D3D12_RANGE readRange = { 0, 0 };
                HRESULT hr = group.instanceBuffers[t]->Map(0, &readRange, &mappedData);

                if (SUCCEEDED(hr) && mappedData)
                {
                    size_t copySize = gpuInstances.size() * sizeof(GrassInstanceGPU);
                    memcpy(mappedData, gpuInstances.data(), copySize);
                    group.instanceBuffers[t]->Unmap(0, nullptr);
                }
            }
        }
    }

    void drawGroup(Core* core, GrassGroup& group)
    {
        // Draw each type separately (bind texture + mesh VB + instance VB, then instanced draw)
        for (size_t t = 0; t < group.types.size(); t++)
        {
            int visibleCount = (int)group.visibleInstancesByType[t].size();
            if (visibleCount == 0) continue;

            if (group.instanceBuffers[t] == nullptr) continue;

            auto& type = group.types[t];
            if (type.mesh == nullptr) continue;

            core->getCommandList()->SetGraphicsRootDescriptorTable(2, type.texture.srvHandle);

            D3D12_VERTEX_BUFFER_VIEW views[2];
            views[0] = type.mesh->getVertexBufferView();
            views[1] = group.instanceBufferViews[t];
            core->getCommandList()->IASetVertexBuffers(0, 2, views);

            D3D12_INDEX_BUFFER_VIEW ibView = type.mesh->getIndexBufferView();
            core->getCommandList()->IASetIndexBuffer(&ibView);

            core->getCommandList()->DrawIndexedInstanced(
                type.mesh->getIndexCount(),
                visibleCount,
                0, 0, 0
            );
        }
    }

    void printStatistics()
    {
        // Debug overview of distribution across groups/types and expected draw calls
        std::cout << "\n[HybridGrassField] Statistics:\n";
        std::cout << "================================\n";
        std::cout << "Total Instances: " << allInstances.size() << "\n";
        std::cout << "Chunks: " << chunks.size() << "\n\n";

        int totalDrawCalls = 0;
        for (size_t g = 0; g < groups.size(); g++)
        {
            auto& group = groups[g];
            int groupTotal = 0;

            std::cout << "Group: " << group.name << "\n";
            for (size_t t = 0; t < group.types.size(); t++)
            {
                int count = (int)group.instancesByType[t].size();
                groupTotal += count;
                totalDrawCalls++;

                float percentage = allInstances.empty() ? 0.0f :
                    (float)count / (float)allInstances.size() * 100.0f;

                std::cout << "  - " << group.types[t].name << ": "
                    << count << " (" << percentage << "%)\n";
            }

            float groupPercentage = allInstances.empty() ? 0.0f :
                (float)groupTotal / (float)allInstances.size() * 100.0f;

            std::cout << "  Total: " << groupTotal << " (" << groupPercentage << "%)\n\n";
        }

        std::cout << "Total Draw Calls: " << totalDrawCalls << "\n";
        std::cout << "================================\n\n";
    }
};
