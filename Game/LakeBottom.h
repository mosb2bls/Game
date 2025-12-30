#pragma once

#include "Core.h"
#include "Shaders.h"
#include "PSO.h"
#include "Mesh.h"
#include "Maths.h"
#include <vector>
#include <cmath>

// Lake bottom mesh: generates a smooth "bowl" under the lake by using a sphere segment.
// The mesh is built in world space so the world matrix can remain identity during draw.

class LakeBottom
{
public:
    std::string shaderName = "LakeBottom";
    std::string psoName = "LakeBottomPSO";

    void init(Core* core, Shaders* shaders, PSOManager* psos,
        const std::string& texturePath,
        const Vec3& center, float radius, float waterY, float bowlDepth = 8.0f)
    {
        // Store lake parameters so the bowl can be generated to match your lake surface
        this->lakeCenter = center;
        this->lakeRadius = radius;
        this->waterLevel = waterY;
        this->depth = bowlDepth;

        std::cout << "[LakeBottom] Initializing...\n";
        std::cout << "  Center: (" << center.x << ", " << center.y << ", " << center.z << ")\n";
        std::cout << "  Radius: " << radius << ", Depth: " << bowlDepth << "\n";

        // Load the bottom texture (sand/rocks/mud) and keep its SRV for rendering
        bottomTexture = core->loadTexture(texturePath);

        // Build a curved bowl mesh (STATIC_VERTEX + indices), then upload via your Mesh wrapper
        std::vector<STATIC_VERTEX> vertices;
        std::vector<unsigned int> indices;
        generateBowlMesh(vertices, indices);

        mesh.init(core, vertices, indices);

        // Load shaders dedicated to the lake bottom surface shading
        shaders->load(core, shaderName, "Shaders/VSLakeBottom.txt", "Shaders/PSLakeBottom.txt");

        // Create PSO using the standard static mesh input layout (pos/normal/tangent/uv)
        psos->createPSO(core, psoName,
            shaders->find(shaderName)->vs,
            shaders->find(shaderName)->ps,
            VertexLayoutCache::getStaticLayout());

        initialized = true;
        std::cout << "[LakeBottom] Ready! Vertices: " << vertices.size()
            << ", Triangles: " << indices.size() / 3 << "\n";
    }

    void draw(Core* core, PSOManager* psos, Shaders* shaders, const Matrix& vp)
    {
        if (!initialized) return;

        // Use identity world matrix because the vertices were generated directly in world space
        Matrix world;
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "VP", (void*)&vp);
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "W", (void*)&world);

        // Apply shaders / bind PSO, then bind the texture SRV and draw the mesh
        shaders->apply(core, shaderName);
        psos->bind(core, psoName);

        core->getCommandList()->SetGraphicsRootDescriptorTable(2, bottomTexture.srvHandle);

        mesh.draw(core);
    }

private:
    Mesh mesh;
    Texture bottomTexture;
    bool initialized = false;

    Vec3 lakeCenter = Vec3(0, 0, 0);
    float lakeRadius = 25.0f;
    float depth = 8.0f;
    float waterLevel = 0.0f;
    int segments = 64;

    void generateBowlMesh(std::vector<STATIC_VERTEX>& vertices, std::vector<unsigned int>& indices)
    {
        vertices.clear();
        indices.clear();

        // Compute an implicit sphere that forms the bowl:
        // The bowl rim is at the water surface with radius = lakeRadius, and the bowl drops by "depth".
        // sphereRadius = (r^2 + d^2) / (2d) gives a sphere whose cap matches that geometry.
        float sphereRadius = (lakeRadius * lakeRadius + depth * depth) / (2.0f * depth);

        // Place the sphere center so that the lowest point of the cap is at waterLevel - depth
        Vec3 sphereCenter = Vec3(lakeCenter.x, waterLevel - depth + sphereRadius, lakeCenter.z);

        // Use a grid of rings/slices (like a UV sphere) but only for the lower cap portion
        int rings = segments / 2;
        int slices = segments;

        // Find the polar angle where the sphere reaches the rim circle at the water surface
        // This limits the mesh so it ends exactly at the lake radius.
        float cosMaxTheta = (sphereRadius - depth) / sphereRadius;
        float maxTheta = acosf(cosMaxTheta);

        // Generate vertices from bottom (theta = PI) up towards the rim (theta = maxTheta)
        for (int ring = 0; ring <= rings; ring++)
        {
            float t = (float)ring / (float)rings;
            float theta = 3.14159265f - t * (3.14159265f - maxTheta);

            float sinTheta = sinf(theta);
            float cosTheta = cosf(theta);

            for (int slice = 0; slice <= slices; slice++)
            {
                float phi = (float)slice / (float)slices * 2.0f * 3.14159265f;
                float sinPhi = sinf(phi);
                float cosPhi = cosf(phi);

                // Compute vertex position on the sphere surface
                Vec3 pos;
                pos.x = sphereCenter.x + sphereRadius * sinTheta * cosPhi;
                pos.y = sphereCenter.y + sphereRadius * cosTheta;
                pos.z = sphereCenter.z + sphereRadius * sinTheta * sinPhi;

                // Normals point inward because the camera is above looking into the bowl
                Vec3 normal;
                normal.x = -sinTheta * cosPhi;
                normal.y = -cosTheta;
                normal.z = -sinTheta * sinPhi;

                // Simple planar UVs projected in XZ to make texturing stable and intuitive
                float u = (pos.x - lakeCenter.x) / (lakeRadius * 2.0f) + 0.5f;
                float v = (pos.z - lakeCenter.z) / (lakeRadius * 2.0f) + 0.5f;

                // Fill STATIC_VERTEX according to your existing Mesh / shader expectations
                STATIC_VERTEX vert;
                vert.pos = pos;
                vert.normal = normal;
                vert.tangent = Vec3(1, 0, 0);
                vert.tu = u;
                vert.tv = v;

                vertices.push_back(vert);
            }
        }

        // Build triangle indices (two triangles per quad on the ring/slice grid)
        for (int ring = 0; ring < rings; ring++)
        {
            for (int slice = 0; slice < slices; slice++)
            {
                int current = ring * (slices + 1) + slice;
                int next = current + slices + 1;

                indices.push_back(current);
                indices.push_back(current + 1);
                indices.push_back(next);

                indices.push_back(current + 1);
                indices.push_back(next + 1);
                indices.push_back(next);
            }
        }
    }
};
