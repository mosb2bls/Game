#pragma once

#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <algorithm>

#include "Core.h"
#include "Shaders.h"
#include "PSO.h"
#include "Mesh.h"
#include "Maths.h"
#include "stb_image.h"

// ============================================================================
// HeightmapTerrain
// ----------------------------------------------------------------------------
// - Loads height data from RAW / PNG heightmaps
// - Converts height samples into world-space terrain heights
// - Builds a grid mesh with computed normals and tangents
// - Renders terrain with simple lighting and height-based colour blending
// - Provides CPU-side height sampling for camera, grass, collision, etc.
// ============================================================================
class HeightmapTerrain
{
    Texture terrainTexture; // Diffuse terrain texture
public:
    // Supported heightmap input formats
    enum class Format
    {
        RAW8,
        RAW16_LE,
        PNG8,
        PNG16
    };

    std::string shaderName = "Terrain";     // Terrain shader identifier
    std::string psoName = "TerrainPSO";     // Terrain PSO identifier

    // ------------------------------------------------------------------------
    // init
    // ------------------------------------------------------------------------
    // 1) Load heightmap into CPU memory
    // 2) Generate grid mesh from height samples
    // 3) Load shaders and create PSO
    // ------------------------------------------------------------------------
    bool init(Core* core, PSOManager* psos, Shaders* shaders,
        const std::string& heightmapFile,
        int hmWidth, int hmHeight,
        float worldSizeX, float worldSizeZ,
        float heightScale,
        float heightOffset = 0.0f,
        Format fmt = Format::RAW8)
    {
        this->hmW = hmWidth;
        this->hmH = hmHeight;
        this->worldX = worldSizeX;
        this->worldZ = worldSizeZ;
        this->heightScale = heightScale;
        this->heightOffset = heightOffset;

        // Allocate CPU-side height array
        heights.resize((size_t)hmW * (size_t)hmH, 0.0f);

        // Load heightmap based on selected format
        bool ok = false;
        if (fmt == Format::RAW8)     ok = loadRAW8(heightmapFile);
        if (fmt == Format::RAW16_LE) ok = loadRAW16LE(heightmapFile);
        if (fmt == Format::PNG8)     ok = loadPNG8(heightmapFile);
        if (fmt == Format::PNG16)    ok = loadPNG16(heightmapFile);

        if (!ok)
        {
            std::cout << "[HeightmapTerrain] failed to load heightmap: "
                << heightmapFile << std::endl;
            return false;
        }

        // Build terrain mesh from height samples
        std::vector<STATIC_VERTEX> vertices;
        std::vector<unsigned int> indices;
        buildTerrainMesh(vertices, indices);

        // Upload mesh to GPU
        mesh.init(core, vertices, indices);

        // Load terrain texture
        terrainTexture = core->loadTexture("Assets/Heightmap/grass2.png");

        // Load shaders and create PSO
        shaders->load(core, shaderName,
            "Shaders/VSTerrain.txt",
            "Shaders/PSTerrain.txt");

        psos->createPSO(core, psoName,
            shaders->find(shaderName)->vs,
            shaders->find(shaderName)->ps,
            VertexLayoutCache::getStaticLayout());

        return true;
    }

    // ------------------------------------------------------------------------
    // draw
    // ------------------------------------------------------------------------
    // - Updates VS constants (W, VP)
    // - Sends lighting and height-blending parameters to PS
    // - Binds texture and draws terrain mesh
    // ------------------------------------------------------------------------
    void draw(Core* core, PSOManager* psos, Shaders* shaders,
        const Matrix& vp, const Matrix& world)
    {
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "VP", (void*)&vp);
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "W", (void*)&world);

        // Directional light + ambient term
        Vec4 lightDirAmbient(0.3f, 0.9f, 0.2f, 0.25f);
        Vec4 lowCol(0.10f, 0.35f, 0.10f, 1.0f);   // Low altitude colour
        Vec4 highCol(0.45f, 0.45f, 0.45f, 1.0f);  // High altitude colour
        Vec4 heightParams(minHeightWorld, maxHeightWorld, 0.0f, 0.0f);

        shaders->updateConstantPS(shaderName, "terrainPSBuffer",
            "lightDir_ambient", &lightDirAmbient);
        shaders->updateConstantPS(shaderName, "terrainPSBuffer",
            "baseColorLow", &lowCol);
        shaders->updateConstantPS(shaderName, "terrainPSBuffer",
            "baseColorHigh", &highCol);
        shaders->updateConstantPS(shaderName, "terrainPSBuffer",
            "heightParams", &heightParams);

        shaders->apply(core, shaderName);
        psos->bind(core, psoName);

        // Bind terrain texture SRV
        core->getCommandList()->SetGraphicsRootDescriptorTable(
            2, terrainTexture.srvHandle);

        mesh.draw(core);
    }

    // ------------------------------------------------------------------------
    // sampleHeightWorld
    // ------------------------------------------------------------------------
    // Bilinearly samples terrain height at arbitrary world-space coordinates.
    // Used for camera positioning, grass placement, collision, fog, etc.
    // ------------------------------------------------------------------------
    float sampleHeightWorld(float worldPosX, float worldPosZ) const
    {
        float halfX = worldX * 0.5f;
        float halfZ = worldZ * 0.5f;

        float fx = (worldPosX + halfX) / worldX;
        float fz = (worldPosZ + halfZ) / worldZ;

        fx = std::clamp(fx, 0.0f, 1.0f);
        fz = std::clamp(fz, 0.0f, 1.0f);

        float x = fx * (hmW - 1);
        float z = fz * (hmH - 1);

        int x0 = (int)floorf(x);
        int z0 = (int)floorf(z);
        int x1 = std::min(x0 + 1, hmW - 1);
        int z1 = std::min(z0 + 1, hmH - 1);

        float tx = x - (float)x0;
        float tz = z - (float)z0;

        float h00 = heightAt(x0, z0);
        float h10 = heightAt(x1, z0);
        float h01 = heightAt(x0, z1);
        float h11 = heightAt(x1, z1);

        float h0 = h00 * (1.0f - tx) + h10 * tx;
        float h1 = h01 * (1.0f - tx) + h11 * tx;

        return (h0 * (1.0f - tz) + h1 * tz);
    }

private:
    Mesh mesh; // Terrain grid mesh

    int hmW = 0;
    int hmH = 0;

    float worldX = 1.0f;
    float worldZ = 1.0f;

    float heightScale = 10.0f;
    float heightOffset = 0.0f;

    std::vector<float> heights; // World-space height values

    float minHeightWorld = 0.0f;
    float maxHeightWorld = 1.0f;

    // Safe height lookup with clamping
    float heightAt(int x, int z) const
    {
        x = std::clamp(x, 0, hmW - 1);
        z = std::clamp(z, 0, hmH - 1);
        return heights[(size_t)z * (size_t)hmW + (size_t)x];
    }

    // ------------------------------------------------------------------------
    // Heightmap loading (PNG 8-bit)
    // ------------------------------------------------------------------------
    bool loadPNG8(const std::string& file)
    {
        int w = 0, h = 0, comp = 0;
        unsigned char* data = stbi_load(file.c_str(), &w, &h, &comp, 0);
        if (!data) return false;

        hmW = w;
        hmH = h;
        heights.assign((size_t)hmW * (size_t)hmH, 0.0f);

        minHeightWorld = 1e9f;
        maxHeightWorld = -1e9f;

        for (int y = 0; y < hmH; ++y)
        {
            for (int x = 0; x < hmW; ++x)
            {
                int idx = (y * hmW + x) * comp;
                float gray01 = 0.0f;

                if (comp == 1)
                {
                    gray01 = (float)data[y * hmW + x] / 255.0f;
                }
                else
                {
                    float r = (float)data[idx + 0] / 255.0f;
                    float g = (float)data[idx + 1] / 255.0f;
                    float b = (float)data[idx + 2] / 255.0f;
                    gray01 = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                }

                float hWorld = heightOffset + gray01 * heightScale;
                heights[(size_t)y * (size_t)hmW + (size_t)x] = hWorld;

                minHeightWorld = std::min(minHeightWorld, hWorld);
                maxHeightWorld = std::max(maxHeightWorld, hWorld);
            }
        }

        stbi_image_free(data);
        return true;
    }

    // ------------------------------------------------------------------------
    // Heightmap loading (PNG 16-bit)
    // ------------------------------------------------------------------------
    bool loadPNG16(const std::string& file)
    {
        int w = 0, h = 0, comp = 0;
        unsigned short* data = stbi_load_16(file.c_str(), &w, &h, &comp, 0);
        if (!data) return false;

        hmW = w;
        hmH = h;
        heights.assign((size_t)hmW * (size_t)hmH, 0.0f);

        minHeightWorld = 1e9f;
        maxHeightWorld = -1e9f;

        for (int y = 0; y < hmH; ++y)
        {
            for (int x = 0; x < hmW; ++x)
            {
                int idx = (y * hmW + x) * comp;
                float gray01 = 0.0f;

                if (comp == 1)
                {
                    gray01 = (float)data[y * hmW + x] / 65535.0f;
                }
                else
                {
                    float r = (float)data[idx + 0] / 65535.0f;
                    float g = (float)data[idx + 1] / 65535.0f;
                    float b = (float)data[idx + 2] / 65535.0f;
                    gray01 = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                }

                float hWorld = heightOffset + gray01 * heightScale;
                heights[(size_t)y * (size_t)hmW + (size_t)x] = hWorld;

                minHeightWorld = std::min(minHeightWorld, hWorld);
                maxHeightWorld = std::max(maxHeightWorld, hWorld);
            }
        }

        stbi_image_free(data);
        return true;
    }

    // ------------------------------------------------------------------------
    // Heightmap loading (RAW 8-bit)
    // ------------------------------------------------------------------------
    bool loadRAW8(const std::string& file)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in.is_open()) return false;

        const size_t expected = (size_t)hmW * (size_t)hmH;
        std::vector<unsigned char> bytes(expected);

        in.read((char*)bytes.data(), (std::streamsize)expected);
        if ((size_t)in.gcount() != expected) return false;

        minHeightWorld = 1e9f;
        maxHeightWorld = -1e9f;

        for (size_t i = 0; i < expected; ++i)
        {
            float n = (float)bytes[i] / 255.0f;
            float h = heightOffset + n * heightScale;
            heights[i] = h;

            minHeightWorld = std::min(minHeightWorld, h);
            maxHeightWorld = std::max(maxHeightWorld, h);
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Heightmap loading (RAW 16-bit little endian)
    // ------------------------------------------------------------------------
    bool loadRAW16LE(const std::string& file)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in.is_open()) return false;

        const size_t expectedSamples = (size_t)hmW * (size_t)hmH;
        const size_t expectedBytes = expectedSamples * 2;

        std::vector<unsigned char> bytes(expectedBytes);
        in.read((char*)bytes.data(), (std::streamsize)expectedBytes);
        if ((size_t)in.gcount() != expectedBytes) return false;

        minHeightWorld = 1e9f;
        maxHeightWorld = -1e9f;

        for (size_t i = 0; i < expectedSamples; ++i)
        {
            unsigned int lo = bytes[i * 2 + 0];
            unsigned int hi = bytes[i * 2 + 1];
            unsigned int v = (hi << 8) | lo;

            float n = (float)v / 65535.0f;
            float h = heightOffset + n * heightScale;
            heights[i] = h;

            minHeightWorld = std::min(minHeightWorld, h);
            maxHeightWorld = std::max(maxHeightWorld, h);
        }
        return true;
    }

    // Build STATIC_VERTEX with normal and tangent
    STATIC_VERTEX makeVertex(const Vec3& p, const Vec3& n, float u, float v)
    {
        STATIC_VERTEX vert;
        vert.pos = p;
        vert.normal = n;

        Frame frame;
        frame.fromVector(n);
        vert.tangent = frame.u;

        vert.tu = u;
        vert.tv = v;
        return vert;
    }

    // Approximate normal using central differences
    Vec3 computeNormal(int x, int z, float dx, float dz) const
    {
        float hL = heightAt(x - 1, z);
        float hR = heightAt(x + 1, z);
        float hD = heightAt(x, z - 1);
        float hU = heightAt(x, z + 1);

        Vec3 dX(2.0f * dx, hR - hL, 0.0f);
        Vec3 dZ(0.0f, hU - hD, 2.0f * dz);

        return Cross(dZ, dX).normalize();
    }

    // Generate grid mesh from heightmap
    void buildTerrainMesh(std::vector<STATIC_VERTEX>& outV,
        std::vector<unsigned int>& outI)
    {
        outV.clear();
        outI.clear();

        float dx = worldX / (float)(hmW - 1);
        float dz = worldZ / (float)(hmH - 1);

        float halfX = worldX * 0.5f;
        float halfZ = worldZ * 0.5f;

        outV.reserve((size_t)hmW * (size_t)hmH);

        for (int z = 0; z < hmH; ++z)
        {
            for (int x = 0; x < hmW; ++x)
            {
                float px = (float)x * dx - halfX;
                float pz = (float)z * dz - halfZ;
                float py = heightAt(x, z);

                Vec3 pos(px, py, pz);
                Vec3 n = computeNormal(x, z, dx, dz);

                float u = (float)x / (float)(hmW - 1);
                float v = (float)z / (float)(hmH - 1);

                outV.push_back(makeVertex(pos, n, u, v));
            }
        }

        outI.reserve((size_t)(hmW - 1) * (size_t)(hmH - 1) * 6);

        for (int z = 0; z < hmH - 1; ++z)
        {
            for (int x = 0; x < hmW - 1; ++x)
            {
                unsigned int i0 = (unsigned int)(z * hmW + x);
                unsigned int i1 = i0 + 1;
                unsigned int i2 = (unsigned int)((z + 1) * hmW + x);
                unsigned int i3 = i2 + 1;

                outI.push_back(i0);
                outI.push_back(i1);
                outI.push_back(i2);

                outI.push_back(i1);
                outI.push_back(i3);
                outI.push_back(i2);
            }
        }
    }
};
