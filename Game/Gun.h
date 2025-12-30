#pragma once

#include "Core.h"
#include "Shaders.h"
#include "PSO.h"
#include "Mesh.h"
#include "GEMLoader.h"
#include "Animation.h"
#include <vector>
#include <string>

class Gun
{
public:
    std::vector<Mesh*> meshes;         // Model sub-meshes (owned)
    Animation animation;               // Skeleton + animation clips
    Texture gunTexture;                // Albedo texture SRV

    std::string shaderName = "AnimatedTextured";   // Shader key
    std::string psoName = "AnimatedTexturedPSO";   // PSO key

    void load(Core* core, std::string modelFilename, std::string textureFilename,
        PSOManager* psos, Shaders* shaders)
    {
        GEMLoader::GEMModelLoader loader;
        std::vector<GEMLoader::GEMMesh> gemmeshes;
        GEMLoader::GEMAnimation gemanimation;
        loader.load(modelFilename, gemmeshes, gemanimation); // Geometry + skeleton + clips

        // Build GPU meshes from animated vertex streams
        for (int i = 0; i < gemmeshes.size(); i++)
        {
            Mesh* mesh = new Mesh();
            std::vector<ANIMATED_VERTEX> vertices;
            for (int j = 0; j < gemmeshes[i].verticesAnimated.size(); j++)
            {
                ANIMATED_VERTEX v;
                memcpy(&v, &gemmeshes[i].verticesAnimated[j], sizeof(ANIMATED_VERTEX));
                vertices.push_back(v);
            }
            mesh->init(core, vertices, gemmeshes[i].indices);
            meshes.push_back(mesh);
        }

        gunTexture = core->loadTexture(textureFilename); // GPU texture + SRV handle

        shaders->load(core, shaderName, "Shaders/VSAnim.txt", "Shaders/PSAnim.txt"); // Skinning VS + textured PS

        psos->createPSO(core, psoName,
            shaders->find(shaderName)->vs,
            shaders->find(shaderName)->ps,
            VertexLayoutCache::getAnimatedLayout()); // ANIMATED_VERTEX input layout

        memcpy(&animation.skeleton.globalInverse, &gemanimation.globalInverse, 16 * sizeof(float)); // Importer root inverse

        // Copy bone hierarchy + inverse bind offsets
        for (int i = 0; i < gemanimation.bones.size(); i++)
        {
            Bone bone;
            bone.name = gemanimation.bones[i].name;
            memcpy(&bone.offset, &gemanimation.bones[i].offset, 16 * sizeof(float));
            bone.parentIndex = gemanimation.bones[i].parentIndex;
            animation.skeleton.bones.push_back(bone);
        }

        // Copy animation clips (per-frame per-bone TRS)
        for (int i = 0; i < gemanimation.animations.size(); i++)
        {
            std::string name = gemanimation.animations[i].name;
            AnimationSequence aseq;
            aseq.ticksPerSecond = gemanimation.animations[i].ticksPerSecond;

            for (int j = 0; j < gemanimation.animations[i].frames.size(); j++)
            {
                AnimationFrame frame;
                for (int index = 0; index < gemanimation.animations[i].frames[j].positions.size(); index++)
                {
                    Vec3 p;
                    Quaternion q;
                    Vec3 s;
                    memcpy(&p, &gemanimation.animations[i].frames[j].positions[index], sizeof(Vec3));
                    frame.positions.push_back(p);
                    memcpy(&q, &gemanimation.animations[i].frames[j].rotations[index], sizeof(Quaternion));
                    frame.rotations.push_back(q);
                    memcpy(&s, &gemanimation.animations[i].frames[j].scales[index], sizeof(Vec3));
                    frame.scales.push_back(s);
                }
                aseq.frames.push_back(frame);
            }
            animation.animations.insert({ name, aseq }); // Register clip by name
        }
    }

    void updateWorld(Shaders* shaders, Matrix& w)
    {
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "W", &w); // Update model transform
    }

    void draw(Core* core, PSOManager* psos, Shaders* shaders,
        AnimationInstance* instance, Matrix& vp, Matrix& w)
    {
        psos->bind(core, psoName); // Bind pipeline state for skinned textured draw

        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "W", &w);
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "VP", &vp);
        shaders->updateConstantVS(shaderName, "staticMeshBuffer", "bones", instance->matrices); // Bone palette

        shaders->apply(core, shaderName); // Bind root signature + constant buffers

        core->getCommandList()->SetGraphicsRootDescriptorTable(2, gunTexture.srvHandle); // Bind texture SRV table

        for (int i = 0; i < meshes.size(); i++)
        {
            meshes[i]->draw(core); // Draw each sub-mesh
        }
    }

    ~Gun()
    {
        for (auto* mesh : meshes)
        {
            delete mesh;
        }
        meshes.clear();
    }
};
