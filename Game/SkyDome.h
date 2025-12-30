#pragma once

#include <vector>
#include <string>
#include <cmath>

#include "Core.h"
#include "Shaders.h"
#include "PSO.h"
#include "Mesh.h"
#include "Maths.h"

// Sky dome that renders a large textured sphere centered on the camera.
// The dome moves with the camera so it never appears to be “reached” by the player,
// and it uses a sky shader that can blend zenith/horizon colors plus an optional sky texture.
class SkyDome
{
    Texture skyTexture;

public:
    std::string shaderName = "Sky";
    std::string psoName = "SkyPSO";

    void init(Core* core, PSOManager* psos, Shaders* shaders,
        float radius = 5000.0f, int slices = 64, int stacks = 32)
    {
        this->radius = radius;

        // Build a unit sphere (radius = 1) using latitude/longitude stacks and slices,
        // then we scale it in the world matrix when drawing.
        std::vector<STATIC_VERTEX> vertices;
        std::vector<unsigned int>  indices;
        buildSphere(vertices, indices, slices, stacks);
        mesh.init(core, vertices, indices);

        // Load the sky texture; the shader can sample this (if enabled) for sky detail.
        skyTexture = core->loadTexture("Assets/Sky/sky.png");

        // Load sky shaders and create a standard PSO using the same static vertex layout as other meshes.
        shaders->load(core, shaderName, "Shaders/VSSky.txt", "Shaders/PSSky.txt");
        psos->createPSO(core, psoName,
            shaders->find(shaderName)->vs,
            shaders->find(shaderName)->ps,
            VertexLayoutCache::getStaticLayout());
    }

    void draw(Core* core, PSOManager* psos, Shaders* shaders,
        const Matrix& vp, const Vec3& cameraPos)
    {
        // Place the sphere at the camera position and scale it to a very large radius,
        // so it always surrounds the player and stays visually “infinitely far away”.
        Matrix W = Matrix::scaling(Vec3(radius, radius, radius))
            * Matrix::translation(cameraPos);

        // Provide W and VP to the vertex shader so the sky dome renders correctly in clip space.
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "W", (void*)&W);
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "VP", (void*)&vp);

        // Provide simple gradient controls (top and horizon colors) to the pixel shader.
        // If your sky pixel shader blends these, it helps reduce banding and supports stylized skies.
        Vec4 zenith(0.2f, 0.4f, 0.8f, 1.0f);
        Vec4 horizon(0.8f, 0.7f, 0.5f, 1.0f);
        shaders->updateConstantPS(shaderName, "skyPSBuffer", "zenithColor", &zenith);
        shaders->updateConstantPS(shaderName, "skyPSBuffer", "horizonColor", &horizon);

        // Bind shader + PSO, then bind the sky texture SRV and draw the sphere mesh.
        shaders->apply(core, shaderName);
        psos->bind(core, psoName);

        core->getCommandList()->SetGraphicsRootDescriptorTable(2, skyTexture.srvHandle);
        mesh.draw(core);
    }

private:
    Mesh  mesh;
    float radius = 5000.0f;

    // Build a STATIC_VERTEX using only the fields your engine already supports.
    // Tangent is derived from the normal using a simple shading frame for consistency.
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

    // Generate a unit sphere mesh using a standard latitude/longitude parameterization.
    // Slices control horizontal resolution (around Y), stacks control vertical resolution (top to bottom).
    void buildSphere(std::vector<STATIC_VERTEX>& outV,
        std::vector<unsigned int>& outI,
        int slices, int stacks)
    {
        outV.clear();
        outI.clear();

        const float PI = 3.141592654f;

        // Create vertices for each stack ring; we duplicate the seam vertex (slice = slices)
        // so the U coordinate can wrap cleanly from 1 back to 0.
        for (int stack = 0; stack <= stacks; ++stack)
        {
            float phi = (float)stack / (float)stacks * PI;
            float y = cosf(phi);
            float r = sinf(phi);

            for (int slice = 0; slice <= slices; ++slice)
            {
                float theta = (float)slice / (float)slices * (2.0f * PI);
                float x = r * cosf(theta);
                float z = r * sinf(theta);

                Vec3 pos(x, y, z);
                Vec3 normal = pos.normalize();

                // Standard spherical UVs; sky texture typically wraps horizontally (U).
                float u = (float)slice / (float)slices;
                float v = (float)stack / (float)stacks;

                outV.push_back(makeVertex(pos, normal, u, v));
            }
        }

        // Build indices as two triangles per quad between adjacent stack rings.
        int ring = slices + 1;

        for (int stack = 0; stack < stacks; ++stack)
        {
            for (int slice = 0; slice < slices; ++slice)
            {
                unsigned int i0 = (unsigned int)(stack * ring + slice);
                unsigned int i1 = (unsigned int)(i0 + 1);
                unsigned int i2 = (unsigned int)((stack + 1) * ring + slice);
                unsigned int i3 = (unsigned int)(i2 + 1);

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
