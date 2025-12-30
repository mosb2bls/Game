#pragma once

#include "Core.h"
#include "Shaders.h"
#include "PSO.h"
#include "Mesh.h"
#include "GEMLoader.h"
#include "Maths.h"
#include <vector>

// A simple tree actor composed of three draw parts: a canopy mesh, a cylinder trunk, and a flat ground shadow.
// The canopy is loaded from a GEM model and rendered with its own texture.
// The trunk is procedurally generated as a low-poly cylinder and rendered with a separate trunk texture.
// The shadow is a simple blended disc that is drawn first so it sits under the tree.
class Tree
{
public:
    std::string shaderName = "Tree";
    std::string psoName = "TreePSO";
    std::string shadowShaderName = "TreeShadow";
    std::string shadowPsoName = "TreeShadowPSO";
    std::string trunkShaderName = "TreeTrunk";
    std::string trunkPsoName = "TreeTrunkPSO";

    void init(Core* core, Shaders* shaders, PSOManager* psos,
        const std::string& modelPath,
        const std::string& texturePath,
        const std::string& trunkTexturePath,
        const Vec3& position,
        float scale = 1.0f,
        float rotationY = 0.0f)
    {
        this->position = position;
        this->scale = scale;
        this->rotationY = rotationY;

        std::cout << "[Tree] Initializing at (" << position.x << ", " << position.y << ", " << position.z << ")\n";

        // Load the canopy/leaves geometry from a GEM model, then convert to STATIC_VERTEX for your Mesh class.
        // Only the first mesh is used here (meshes[0]) which is common for single-mesh tree assets.
        GEMLoader::GEMModelLoader loader;
        std::vector<GEMLoader::GEMMesh> meshes;
        loader.load(modelPath, meshes);

        if (!meshes.empty())
        {
            std::vector<STATIC_VERTEX> vertices;
            std::vector<unsigned int> indices;

            for (auto& gv : meshes[0].verticesStatic)
            {
                STATIC_VERTEX v;
                v.pos = Vec3(gv.position.x, gv.position.y, gv.position.z);
                v.normal = Vec3(gv.normal.x, gv.normal.y, gv.normal.z);
                v.tangent = Vec3(gv.tangent.x, gv.tangent.y, gv.tangent.z);
                v.tu = gv.u;
                v.tv = gv.v;
                vertices.push_back(v);
            }

            indices = meshes[0].indices;
            treeMesh.init(core, vertices, indices);
            std::cout << "[Tree] Loaded model: " << vertices.size() << " vertices\n";
        }

        // Load the canopy texture and a separate trunk texture so bark and leaves can have different materials.
        treeTexture = core->loadTexture(texturePath);
        trunkTexture = core->loadTexture(trunkTexturePath);

        // Generate simple procedural meshes for trunk and shadow so the tree can render even with minimal assets.
        createTrunkMesh(core);
        createShadowMesh(core);

        // Load three shader variants: main tree, blended shadow, and trunk shading.
        // They can share the same vertex shader if your VS just transforms STATIC_VERTEX and passes UV/normal.
        shaders->load(core, shaderName, "Shaders/VSTree.txt", "Shaders/PSTree.txt");
        shaders->load(core, shadowShaderName, "Shaders/VSTree.txt", "Shaders/PSTreeShadow.txt");
        shaders->load(core, trunkShaderName, "Shaders/VSTree.txt", "Shaders/PSTreeTrunk.txt");

        // Create PSOs: normal opaque for canopy and trunk, and alpha blended PSO for the shadow disc.
        psos->createPSO(core, psoName,
            shaders->find(shaderName)->vs,
            shaders->find(shaderName)->ps,
            VertexLayoutCache::getStaticLayout());

        psos->createBlendedPSO(core, shadowPsoName,
            shaders->find(shadowShaderName)->vs,
            shaders->find(shadowShaderName)->ps,
            VertexLayoutCache::getStaticLayout());

        psos->createPSO(core, trunkPsoName,
            shaders->find(trunkShaderName)->vs,
            shaders->find(trunkShaderName)->ps,
            VertexLayoutCache::getStaticLayout());

        initialized = true;
        std::cout << "[Tree] Ready!\n";
    }

    void draw(Core* core, PSOManager* psos, Shaders* shaders, const Matrix& vp)
    {
        if (!initialized) return;

        // Draw shadow first so it blends onto the ground before the solid trunk/canopy are rendered.
        drawShadow(core, psos, shaders, vp);

        // Draw trunk next so the canopy can visually sit on top without sorting issues.
        drawTrunk(core, psos, shaders, vp);

        // Draw canopy using the loaded mesh, with a standard S*R*T transform in world space.
        Matrix S = Matrix::scaling(Vec3(scale, scale, scale));
        Matrix R = Matrix::rotateY(rotationY);
        Matrix T = Matrix::translation(position);
        Matrix world = S * R * T;

        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "VP", (void*)&vp);
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "W", (void*)&world);

        shaders->apply(core, shaderName);
        psos->bind(core, psoName);

        core->getCommandList()->SetGraphicsRootDescriptorTable(2, treeTexture.srvHandle);
        treeMesh.draw(core);
    }

    Vec3 position = Vec3(0, 0, 0);
    float scale = 1.0f;
    float rotationY = 0.0f;
    float shadowRadius = 3.0f;
    float shadowOpacity = 0.5f;

    float trunkRadius = 0.3f;
    float trunkHeight = 4.0f;
    float trunkOffsetY = 0.0f;

private:
    Mesh treeMesh;
    Mesh trunkMesh;
    Mesh shadowMesh;
    Texture treeTexture;
    Texture trunkTexture;
    bool initialized = false;

    void createTrunkMesh(Core* core)
    {
        std::vector<STATIC_VERTEX> vertices;
        std::vector<unsigned int> indices;

        // Build a unit cylinder (radius=1, height=1) aligned on +Y, then scale in drawTrunk().
        // This keeps the mesh reusable for different trunkRadius/trunkHeight values via the world matrix.
        int segments = 16;
        float radius = 1.0f;
        float height = 1.0f;

        for (int i = 0; i <= segments; i++)
        {
            float angle = (float)i / (float)segments * 2.0f * 3.14159265f;
            float x = cosf(angle) * radius;
            float z = sinf(angle) * radius;

            float u = (float)i / (float)segments;

            STATIC_VERTEX vBottom;
            vBottom.pos = Vec3(x, 0, z);
            vBottom.normal = Vec3(x, 0, z);
            vBottom.tangent = Vec3(-sinf(angle), 0, cosf(angle));
            vBottom.tu = u;
            vBottom.tv = 1.0f;
            vertices.push_back(vBottom);

            STATIC_VERTEX vTop;
            vTop.pos = Vec3(x, height, z);
            vTop.normal = Vec3(x, 0, z);
            vTop.tangent = Vec3(-sinf(angle), 0, cosf(angle));
            vTop.tu = u;
            vTop.tv = 0.0f;
            vertices.push_back(vTop);
        }

        // Build side quads as two triangles per segment, using the paired bottom/top vertices per slice.
        for (int i = 0; i < segments; i++)
        {
            int bottom1 = i * 2;
            int top1 = i * 2 + 1;
            int bottom2 = (i + 1) * 2;
            int top2 = (i + 1) * 2 + 1;

            indices.push_back(bottom1);
            indices.push_back(top1);
            indices.push_back(bottom2);

            indices.push_back(top1);
            indices.push_back(top2);
            indices.push_back(bottom2);
        }

        trunkMesh.init(core, vertices, indices);
        std::cout << "[Tree] Created trunk mesh: " << vertices.size() << " vertices\n";
    }

    void createShadowMesh(Core* core)
    {
        std::vector<STATIC_VERTEX> vertices;
        std::vector<unsigned int> indices;

        // Build a unit disc (radius=1) on the XZ plane; it is scaled in drawShadow().
        // The shadow pixel shader typically outputs a dark color with alpha for soft blending.
        int segments = 32;
        float radius = 1.0f;

        STATIC_VERTEX center;
        center.pos = Vec3(0, 0, 0);
        center.normal = Vec3(0, 1, 0);
        center.tangent = Vec3(1, 0, 0);
        center.tu = 0.5f;
        center.tv = 0.5f;
        vertices.push_back(center);

        for (int i = 0; i <= segments; i++)
        {
            float angle = (float)i / (float)segments * 2.0f * 3.14159265f;
            float x = cosf(angle) * radius;
            float z = sinf(angle) * radius;

            STATIC_VERTEX v;
            v.pos = Vec3(x, 0, z);
            v.normal = Vec3(0, 1, 0);
            v.tangent = Vec3(1, 0, 0);
            v.tu = cosf(angle) * 0.5f + 0.5f;
            v.tv = sinf(angle) * 0.5f + 0.5f;
            vertices.push_back(v);
        }

        for (int i = 1; i <= segments; i++)
        {
            indices.push_back(0);
            indices.push_back(i);
            indices.push_back(i + 1);
        }

        shadowMesh.init(core, vertices, indices);
    }

    void drawTrunk(Core* core, PSOManager* psos, Shaders* shaders, const Matrix& vp)
    {
        // Place trunk at the tree base, then apply S*R*T so thickness/height are controllable per Tree instance.
        Vec3 trunkPos = Vec3(position.x, position.y + trunkOffsetY, position.z);

        Matrix S = Matrix::scaling(Vec3(trunkRadius * scale, trunkHeight * scale, trunkRadius * scale));
        Matrix R = Matrix::rotateY(rotationY);
        Matrix T = Matrix::translation(trunkPos);
        Matrix world = S * R * T;

        shaders->updateConstantVS(trunkShaderName, "staticMeshBuffer", "VP", (void*)&vp);
        shaders->updateConstantVS(trunkShaderName, "staticMeshBuffer", "W", (void*)&world);

        shaders->apply(core, trunkShaderName);
        psos->bind(core, trunkPsoName);

        core->getCommandList()->SetGraphicsRootDescriptorTable(2, trunkTexture.srvHandle);
        trunkMesh.draw(core);
    }

    void drawShadow(Core* core, PSOManager* psos, Shaders* shaders, const Matrix& vp)
    {
        // Offset slightly above ground to avoid z-fighting with terrain, then scale to desired radius.
        Vec3 shadowPos = Vec3(position.x, position.y + 0.05f, position.z);

        Matrix S = Matrix::scaling(Vec3(shadowRadius * scale, 1.0f, shadowRadius * scale));
        Matrix T = Matrix::translation(shadowPos);
        Matrix world = S * T;

        shaders->updateConstantVS(shadowShaderName, "staticMeshBuffer", "VP", (void*)&vp);
        shaders->updateConstantVS(shadowShaderName, "staticMeshBuffer", "W", (void*)&world);

        shaders->apply(core, shadowShaderName);
        psos->bind(core, shadowPsoName);

        shadowMesh.draw(core);
    }
};
