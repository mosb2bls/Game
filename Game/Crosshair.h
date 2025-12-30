#pragma once

#include "Core.h"
#include "Shaders.h"
#include "PSO.h"
#include "Mesh.h"
#include "Maths.h"
#include <vector>

// ============================================================================
// SIMPLE CROSSHAIR
// ============================================================================

class Crosshair
{
public:
    std::string shaderName = "Crosshair";
    std::string psoName = "CrosshairPSO";

    float size = 15.0f;
    float thickness = 2.0f;
    float gap = 5.0f;

    void init(Core* core, Shaders* shaders, PSOManager* psos, int screenWidth, int screenHeight)
    {
        this->screenWidth = screenWidth;
        this->screenHeight = screenHeight;

        // Load shaders
        shaders->load(core, shaderName, "Shaders/VSCrosshair.txt", "Shaders/PSCrosshair.txt");

        // Create mesh
        buildMesh(core);

        // Use standard blended PSO
        psos->createBlendedPSO(core, psoName,
            shaders->find(shaderName)->vs,
            shaders->find(shaderName)->ps,
            VertexLayoutCache::getStaticLayout());

        initialized = true;
        std::cout << "[Crosshair] Initialized\n";
    }

    void draw(Core* core, PSOManager* psos, Shaders* shaders)
    {
        if (!initialized) return;

        Matrix identity;
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "W", &identity);
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "VP", &identity);

        shaders->apply(core, shaderName);
        psos->bind(core, psoName);

        crosshairMesh.draw(core);
    }

private:
    Mesh crosshairMesh;
    int screenWidth = 1920;
    int screenHeight = 1080;
    bool initialized = false;

    void buildMesh(Core* core)
    {
        std::vector<STATIC_VERTEX> vertices;
        std::vector<unsigned int> indices;

        // Convert pixel sizes to NDC (-1 to 1)
        float halfThickX = (thickness / screenWidth);
        float halfThickY = (thickness / screenHeight);
        float gapX = (gap / screenWidth);
        float gapY = (gap / screenHeight);
        float sizeX = (size / screenWidth);
        float sizeY = (size / screenHeight);

        // Top line
        addQuad(vertices, indices, -halfThickX, gapY, halfThickX, sizeY);

        // Bottom line
        addQuad(vertices, indices, -halfThickX, -sizeY, halfThickX, -gapY);

        // Left line
        addQuad(vertices, indices, -sizeX, -halfThickY, -gapX, halfThickY);

        // Right line
        addQuad(vertices, indices, gapX, -halfThickY, sizeX, halfThickY);

        crosshairMesh.init(core, vertices, indices);
    }

    void addQuad(std::vector<STATIC_VERTEX>& vertices, std::vector<unsigned int>& indices,
        float x1, float y1, float x2, float y2)
    {
        unsigned int base = (unsigned int)vertices.size();

        STATIC_VERTEX v;
        v.normal = Vec3(0, 1, 0);
        v.tangent = Vec3(1, 0, 0);
        v.tu = 0; v.tv = 0;

        v.pos = Vec3(x1, y1, 0); vertices.push_back(v);
        v.pos = Vec3(x2, y1, 0); vertices.push_back(v);
        v.pos = Vec3(x1, y2, 0); vertices.push_back(v);
        v.pos = Vec3(x2, y2, 0); vertices.push_back(v);

        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 1);
        indices.push_back(base + 3);
        indices.push_back(base + 2);
    }
};