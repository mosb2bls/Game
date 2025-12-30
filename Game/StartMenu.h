#pragma once

#include "Core.h"
#include "Shaders.h"
#include "PSO.h"
#include "Mesh.h"
#include "Maths.h"
#include <vector>

// ============================================================================
// START MENU / TITLE SCREEN
// ============================================================================

class StartMenu
{
    Texture menuTexture;

public:
    std::string shaderName = "StartMenu";
    std::string psoName = "StartMenuPSO";

    bool isActive = true;

    // Path to your 1920x1080 menu image
    std::string menuImagePath = "Assets/StartMenu/StartMenu.png";

    void init(Core* core, Shaders* shaders, PSOManager* psos, int screenWidth, int screenHeight)
    {
        this->screenWidth = screenWidth;
        this->screenHeight = screenHeight;

        // Load shaders
        shaders->load(core, shaderName, "Shaders/VSStartMenu.txt", "Shaders/PSStartMenu.txt");

        // Load the menu background texture (same pattern as SkyDome)
        menuTexture = core->loadTexture(menuImagePath);

        // Create fullscreen quad mesh
        createFullscreenQuad(core);

        // Use standard blended PSO
        psos->createBlendedPSO(core, psoName,
            shaders->find(shaderName)->vs,
            shaders->find(shaderName)->ps,
            VertexLayoutCache::getStaticLayout());

        initialized = true;
        std::cout << "[StartMenu] Initialized with texture: " << menuImagePath << "\n";
    }

    bool update(bool enterPressed, bool spacePressed, bool escapePressed)
    {
        if (!isActive) return false;

        if (enterPressed || spacePressed)
        {
            isActive = false;
            return true;
        }

        pulseTime += 0.05f;
        return false;
    }

    void draw(Core* core, PSOManager* psos, Shaders* shaders)
    {
        if (!isActive || !initialized) return;

        Matrix identity;
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "W", &identity);
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "VP", &identity);

        shaders->apply(core, shaderName);
        psos->bind(core, psoName);

        // Bind the menu texture (same pattern as SkyDome)
        core->getCommandList()->SetGraphicsRootDescriptorTable(2, menuTexture.srvHandle);

        // Draw fullscreen quad
        fullscreenQuad.draw(core);
    }

private:
    int screenWidth = 1920;
    int screenHeight = 1080;
    bool initialized = false;
    float pulseTime = 0.0f;

    Mesh fullscreenQuad;

    void createFullscreenQuad(Core* core)
    {
        std::vector<STATIC_VERTEX> vertices;
        std::vector<unsigned int> indices;

        STATIC_VERTEX v;
        v.normal = Vec3(1, 1, 1);  // White (no tint)
        v.tangent = Vec3(1, 0, 0); // Full alpha

        // Bottom-left
        v.pos = Vec3(-1.0f, -1.0f, 0);
        v.tu = 0.0f;
        v.tv = 1.0f;
        vertices.push_back(v);

        // Bottom-right
        v.pos = Vec3(1.0f, -1.0f, 0);
        v.tu = 1.0f;
        v.tv = 1.0f;
        vertices.push_back(v);

        // Top-left
        v.pos = Vec3(-1.0f, 1.0f, 0);
        v.tu = 0.0f;
        v.tv = 0.0f;
        vertices.push_back(v);

        // Top-right
        v.pos = Vec3(1.0f, 1.0f, 0);
        v.tu = 1.0f;
        v.tv = 0.0f;
        vertices.push_back(v);

        indices.push_back(0);
        indices.push_back(1);
        indices.push_back(2);
        indices.push_back(1);
        indices.push_back(3);
        indices.push_back(2);

        fullscreenQuad.init(core, vertices, indices);
    }
};
