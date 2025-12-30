#pragma once

#include "Core.h"
#include "Maths.h"
#include "Shaders.h"
#include "PSO.h"
#include <d3d12.h>
#include <vector>
#include <iostream>
#include <cmath>

// Lake system: circular water mesh + Gerstner waves + planar reflection texture.
// Typical frame order:
// 1) beginReflectionPass(...) before main scene draw (renders scene into reflection RT)
// 2) render(...) after main scene draw (draws water surface sampling reflection RT)

struct LakeConfig
{
    // Basic placement and shape of the lake surface
    Vec3 center = Vec3(0, 0, 0);
    float radius = 50.0f;
    float waterLevel = 0.0f;

    // Mesh tessellation controls (higher = smoother circle, more triangles)
    int radialSegments = 64;
    int ringSegments = 32;

    // Overall wave animation tuning (individual waves are defined in constant buffer)
    float waveSpeed = 1.0f;
    float waveScale = 1.0f;

    // Water colour and transparency parameters (shallow/edge vs deep/center)
    Vec3 shallowColor = Vec3(0.1f, 0.4f, 0.5f);
    Vec3 deepColor = Vec3(0.0f, 0.1f, 0.2f);
    float transparency = 0.6f;
    float fresnelPower = 4.0f;
    float fresnelBias = 0.02f;

    // Reflection sampling strength and distortion (distortion is driven by wave normals)
    float reflectionStrength = 0.8f;
    float reflectionDistortion = 0.03f;

    // Sun lighting parameters for specular highlight on the water surface
    Vec3 sunDirection = Vec3(0.4f, 0.7f, -0.5f);
    Vec3 sunColor = Vec3(1.0f, 0.95f, 0.8f);
    float specularPower = 256.0f;
    float specularIntensity = 2.0f;
};

// Callback to render the reflected scene (sky/terrain/props), excluding the lake itself
typedef void (*SceneRenderCallback)(void* userData, const Matrix& view, const Matrix& proj);

class Lake
{
public:
    LakeConfig config;

    void init(Core* core, Shaders* shaders, PSOManager* psos, int screenWidth, int screenHeight)
    {
        // Store core and output resolution, then build all GPU resources required by the lake
        this->core = core;
        this->screenWidth = screenWidth;
        this->screenHeight = screenHeight;

        std::cout << "\n[Lake] Initializing...\n";
        std::cout << "  Center: (" << config.center.x << ", " << config.center.y << ", " << config.center.z << ")\n";
        std::cout << "  Radius: " << config.radius << "\n";
        std::cout << "  Water level: " << config.waterLevel << "\n";

        // Create heaps used by reflection RT (RTV/DSV) and reflection SRV (shader-visible)
        createDescriptorHeaps();

        // Allocate half-resolution reflection colour + depth surfaces and create RTV/DSV/SRV views
        createReflectionRenderTarget();

        // Generate a circular, triangulated mesh (center + rings) and upload VB/IB to GPU
        generateMesh();

        // Load water shaders used by the PSO (VSWater/PSWater)
        loadShaders(shaders);

        // Create a dedicated water PSO with alpha blending enabled (for transparency)
        createPSO(psos, shaders);

        // Create upload constant buffer that stores wave/lighting/reflection parameters
        createConstantBuffers();

        initialized = true;
        std::cout << "[Lake] Ready!\n\n";
    }

    void beginReflectionPass(const Matrix& view, const Matrix& proj, const Vec3& cameraPos,
        SceneRenderCallback renderScene, void* userData)
    {
        if (!initialized) return;

        auto cmdList = core->getCommandList();

        // Reflect camera around the water plane (Y = waterLevel) to render the mirrored scene
        float waterY = config.waterLevel;
        Vec3 reflectedCamPos = cameraPos;
        reflectedCamPos.y = 2.0f * waterY - cameraPos.y;

        // Build a reflected view matrix from the current view (mirror over water plane)
        Matrix reflectedView = createReflectedViewMatrix(view, waterY);

        // Store reflection matrices for sampling during the later water render
        this->reflectionView = reflectedView;
        this->reflectionProj = proj;

        // Switch reflection texture into RT state, render into it, then switch back to SRV state
        transitionResource(reflectionTexture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        cmdList->OMSetRenderTargets(1, &reflectionRTV, FALSE, &reflectionDSV);

        float clearColor[4] = { 0.5f, 0.7f, 0.9f, 1.0f };
        cmdList->ClearRenderTargetView(reflectionRTV, clearColor, 0, nullptr);
        cmdList->ClearDepthStencilView(reflectionDSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        D3D12_VIEWPORT vp = { 0, 0, (float)reflectionWidth, (float)reflectionHeight, 0, 1 };
        D3D12_RECT scissor = { 0, 0, (LONG)reflectionWidth, (LONG)reflectionHeight };
        cmdList->RSSetViewports(1, &vp);
        cmdList->RSSetScissorRects(1, &scissor);

        // User callback renders the scene using reflected view/projection into reflection RT
        if (renderScene)
        {
            renderScene(userData, reflectedView, proj);
        }

        transitionResource(reflectionTexture, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    void render(Core* core, PSOManager* psos, Shaders* shaders,
        const Matrix& viewProj, const Vec3& cameraPos, float totalTime)
    {
        if (!initialized || vertexCount == 0) return;

        auto cmdList = core->getCommandList();

        // Update all water parameters (matrices, colours, waves, reflection sampling settings)
        updateConstantBuffer(viewProj, cameraPos, totalTime);

        // Bind lake PSO and required descriptor heap (reflection SRV is stored here)
        psos->bind(core, "LakeWaterPSO");

        ID3D12DescriptorHeap* heaps[] = { srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);

        // Root parameter usage follows your engine root signature convention:
        // 0 = constant buffer view, 2 = texture SRV table
        cmdList->SetGraphicsRootConstantBufferView(0, waterConstantBuffer->GetGPUVirtualAddress());
        cmdList->SetGraphicsRootDescriptorTable(2, reflectionSRV);

        // Draw the uploaded circular lake mesh
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->IASetVertexBuffers(0, 1, &vertexBufferView);
        cmdList->IASetIndexBuffer(&indexBufferView);

        cmdList->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
    }

    bool isPointInLake(float x, float z) const
    {
        // Simple circle test in XZ plane for gameplay/logic checks
        float dx = x - config.center.x;
        float dz = z - config.center.z;
        return (dx * dx + dz * dz) <= (config.radius * config.radius);
    }

    float getWaterLevel() const { return config.waterLevel; }

    ~Lake()
    {
        // Release all owned GPU resources and heaps
        if (vertexBuffer) vertexBuffer->Release();
        if (indexBuffer) indexBuffer->Release();
        if (waterConstantBuffer) waterConstantBuffer->Release();
        if (reflectionTexture) reflectionTexture->Release();
        if (reflectionDepth) reflectionDepth->Release();
        if (rtvHeap) rtvHeap->Release();
        if (dsvHeap) dsvHeap->Release();
        if (srvHeap) srvHeap->Release();
    }

private:
    Core* core = nullptr;
    bool initialized = false;

    int screenWidth, screenHeight;
    int reflectionWidth, reflectionHeight;

    ID3D12Resource* vertexBuffer = nullptr;
    ID3D12Resource* indexBuffer = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferView = {};
    D3D12_INDEX_BUFFER_VIEW indexBufferView = {};
    UINT vertexCount = 0;
    UINT indexCount = 0;

    ID3D12Resource* waterConstantBuffer = nullptr;

    ID3D12Resource* reflectionTexture = nullptr;
    ID3D12Resource* reflectionDepth = nullptr;

    ID3D12DescriptorHeap* rtvHeap = nullptr;
    ID3D12DescriptorHeap* dsvHeap = nullptr;
    ID3D12DescriptorHeap* srvHeap = nullptr;

    D3D12_CPU_DESCRIPTOR_HANDLE reflectionRTV;
    D3D12_CPU_DESCRIPTOR_HANDLE reflectionDSV;
    D3D12_GPU_DESCRIPTOR_HANDLE reflectionSRV;

    Matrix reflectionView;
    Matrix reflectionProj;

    struct WaterVertex
    {
        float x, y, z;
        float u, v;
        float nx, ny, nz;
    };

    struct WaterCB
    {
        Matrix worldViewProj;
        Matrix world;
        Matrix reflectionMatrix;
        Vec4 cameraPos;
        Vec4 waterParams;
        Vec4 shallowColor;
        Vec4 deepColor;
        Vec4 sunDirection;
        Vec4 sunColor;
        Vec4 waveParams;
        Vec4 screenParams;

        Vec4 waveDirections[4];
        Vec4 waveParams2[4];
    };

    void createDescriptorHeaps()
    {
        // Create RTV heap for reflection colour target
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.NumDescriptors = 1;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        core->device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap));

        // Create DSV heap for reflection depth target
        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
        dsvDesc.NumDescriptors = 1;
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        core->device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsvHeap));

        // Create shader-visible SRV heap (reflection texture SRV is bound when drawing water)
        D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
        srvDesc.NumDescriptors = 2;
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        core->device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap));
    }

    void createReflectionRenderTarget()
    {
        // Allocate reflection at half resolution to reduce fill cost
        reflectionWidth = screenWidth / 2;
        reflectionHeight = screenHeight / 2;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        // Create colour RT that will be sampled in the water pixel shader
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = reflectionWidth;
        texDesc.Height = reflectionHeight;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        clearValue.Color[0] = 0.5f;
        clearValue.Color[1] = 0.7f;
        clearValue.Color[2] = 0.9f;
        clearValue.Color[3] = 1.0f;

        core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(&reflectionTexture));

        reflectionRTV = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        core->device->CreateRenderTargetView(reflectionTexture, nullptr, reflectionRTV);

        // Create SRV so the water shader can sample the reflection texture
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
        reflectionSRV = srvHeap->GetGPUDescriptorHandleForHeapStart();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        core->device->CreateShaderResourceView(reflectionTexture, &srvDesc, srvCpu);

        // Create a depth buffer for correct depth testing during reflection rendering
        D3D12_RESOURCE_DESC depthDesc = {};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = reflectionWidth;
        depthDesc.Height = reflectionHeight;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depthClear = {};
        depthClear.Format = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = 1.0f;

        core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, IID_PPV_ARGS(&reflectionDepth));

        reflectionDSV = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvViewDesc = {};
        dsvViewDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        core->device->CreateDepthStencilView(reflectionDepth, &dsvViewDesc, reflectionDSV);

        std::cout << "[Lake] Reflection RT created: " << reflectionWidth << "x" << reflectionHeight << "\n";
    }

    void generateMesh()
    {
        // Build a triangle fan from the center, then connect rings to form a disk
        std::vector<WaterVertex> vertices;
        std::vector<unsigned int> indices;

        int radialSegs = config.radialSegments;
        int ringSegs = config.ringSegments;
        float radius = config.radius;
        float centerX = config.center.x;
        float centerZ = config.center.z;
        float waterY = config.waterLevel;

        const float PI = 3.14159265359f;

        WaterVertex center;
        center.x = centerX;
        center.y = waterY;
        center.z = centerZ;
        center.u = 0.5f;
        center.v = 0.5f;
        center.nx = 0;
        center.ny = 1;
        center.nz = 0;
        vertices.push_back(center);

        for (int ring = 1; ring <= ringSegs; ring++)
        {
            float ringRadius = (float)ring / ringSegs * radius;
            float ringU = (float)ring / ringSegs * 0.5f;

            for (int seg = 0; seg < radialSegs; seg++)
            {
                float angle = (float)seg / radialSegs * 2.0f * PI;

                WaterVertex v;
                v.x = centerX + cosf(angle) * ringRadius;
                v.y = waterY;
                v.z = centerZ + sinf(angle) * ringRadius;
                v.u = 0.5f + cosf(angle) * ringU;
                v.v = 0.5f + sinf(angle) * ringU;
                v.nx = 0;
                v.ny = 1;
                v.nz = 0;

                vertices.push_back(v);
            }
        }

        for (int seg = 0; seg < radialSegs; seg++)
        {
            int next = (seg + 1) % radialSegs;
            indices.push_back(0);
            indices.push_back(1 + next);
            indices.push_back(1 + seg);
        }

        for (int ring = 1; ring < ringSegs; ring++)
        {
            int ringStart = 1 + (ring - 1) * radialSegs;
            int nextRingStart = 1 + ring * radialSegs;

            for (int seg = 0; seg < radialSegs; seg++)
            {
                int next = (seg + 1) % radialSegs;

                indices.push_back(ringStart + seg);
                indices.push_back(nextRingStart + next);
                indices.push_back(nextRingStart + seg);

                indices.push_back(ringStart + seg);
                indices.push_back(ringStart + next);
                indices.push_back(nextRingStart + next);
            }
        }

        vertexCount = (UINT)vertices.size();
        indexCount = (UINT)indices.size();

        std::cout << "[Lake] Generated mesh: " << vertexCount << " vertices, " << indexCount / 3 << " triangles\n";

        // Upload vertex/index data using your Core upload helper
        UINT vbSize = vertexCount * sizeof(WaterVertex);

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC vbDesc = {};
        vbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        vbDesc.Width = vbSize;
        vbDesc.Height = 1;
        vbDesc.DepthOrArraySize = 1;
        vbDesc.MipLevels = 1;
        vbDesc.SampleDesc.Count = 1;
        vbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &vbDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&vertexBuffer));

        core->uploadResource(vertexBuffer, vertices.data(), vbSize, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

        vertexBufferView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vertexBufferView.SizeInBytes = vbSize;
        vertexBufferView.StrideInBytes = sizeof(WaterVertex);

        UINT ibSize = indexCount * sizeof(unsigned int);

        D3D12_RESOURCE_DESC ibDesc = vbDesc;
        ibDesc.Width = ibSize;

        core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &ibDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&indexBuffer));

        core->uploadResource(indexBuffer, indices.data(), ibSize, D3D12_RESOURCE_STATE_INDEX_BUFFER);

        indexBufferView.BufferLocation = indexBuffer->GetGPUVirtualAddress();
        indexBufferView.SizeInBytes = ibSize;
        indexBufferView.Format = DXGI_FORMAT_R32_UINT;
    }

    void loadShaders(Shaders* shaders)
    {
        // Load water vertex/pixel shaders used by the lake PSO
        shaders->load(core, "Water", "Shaders/VSWater.txt", "Shaders/PSWater.txt");
    }

    void createPSO(PSOManager* psos, Shaders* shaders)
    {
        // Define input layout matching WaterVertex and enable alpha blending for transparent water
        std::vector<D3D12_INPUT_ELEMENT_DESC> layout = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        auto* shader = shaders->find("Water");
        if (!shader)
        {
            std::cout << "[Lake] ERROR: Water shader not found!\n";
            return;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = core->rootSignature;
        psoDesc.VS = { shader->vs->GetBufferPointer(), shader->vs->GetBufferSize() };
        psoDesc.PS = { shader->ps->GetBufferPointer(), shader->ps->GetBufferSize() };
        psoDesc.InputLayout = { layout.data(), (UINT)layout.size() };

        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;

        psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc.Count = 1;

        ID3D12PipelineState* pso = nullptr;
        HRESULT hr = core->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso));

        if (FAILED(hr))
        {
            std::cout << "[Lake] ERROR: Failed to create water PSO\n";
            return;
        }

        psos->add("LakeWaterPSO", pso);
        std::cout << "[Lake] PSO created\n";
    }

    void createConstantBuffers()
    {
        // Allocate an upload heap buffer large enough for the WaterCB struct
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 512;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&waterConstantBuffer));
    }

    void updateConstantBuffer(const Matrix& viewProj, const Vec3& cameraPos, float time)
    {
        // Pack all runtime parameters and animation state into the constant buffer for the shaders
        Matrix world;

        WaterCB cb;
        cb.worldViewProj = world * viewProj;
        cb.world = world;
        cb.reflectionMatrix = reflectionView * reflectionProj;
        cb.cameraPos = Vec4(cameraPos.x, cameraPos.y, cameraPos.z, time);
        cb.waterParams = Vec4(config.waterLevel, config.radius, config.transparency, config.fresnelPower);
        cb.shallowColor = Vec4(config.shallowColor.x, config.shallowColor.y, config.shallowColor.z, config.fresnelBias);
        cb.deepColor = Vec4(config.deepColor.x, config.deepColor.y, config.deepColor.z, config.reflectionStrength);
        cb.sunDirection = Vec4(config.sunDirection.x, config.sunDirection.y, config.sunDirection.z, config.specularPower);
        cb.sunColor = Vec4(config.sunColor.x, config.sunColor.y, config.sunColor.z, config.specularIntensity);
        cb.waveParams = Vec4(config.waveSpeed, config.waveScale, config.reflectionDistortion, 0);
        cb.screenParams = Vec4((float)reflectionWidth, (float)reflectionHeight,
            1.0f / reflectionWidth, 1.0f / reflectionHeight);

        // Define four Gerstner waves (direction + wavelength/amplitude/steepness/speed)
        cb.waveDirections[0] = Vec4(1.0f, 0.0f, 0, 0);
        cb.waveParams2[0] = Vec4(20.0f, 0.4f, 0.5f, 1.0f);

        cb.waveDirections[1] = Vec4(0.7f, 0.7f, 0, 0);
        cb.waveParams2[1] = Vec4(12.0f, 0.25f, 0.4f, 1.2f);

        cb.waveDirections[2] = Vec4(0.2f, 0.9f, 0, 0);
        cb.waveParams2[2] = Vec4(6.0f, 0.1f, 0.3f, 0.8f);

        cb.waveDirections[3] = Vec4(-0.4f, 0.8f, 0, 0);
        cb.waveParams2[3] = Vec4(3.0f, 0.05f, 0.2f, 1.5f);

        void* data;
        waterConstantBuffer->Map(0, nullptr, &data);
        memcpy(data, &cb, sizeof(cb));
        waterConstantBuffer->Unmap(0, nullptr);
    }

    Matrix createReflectedViewMatrix(const Matrix& view, float waterY)
    {
        // Build a simple reflection transform about plane Y=waterY and apply it to the view
        Matrix reflection;
        reflection.a[1][1] = -1.0f;
        reflection.a[1][3] = 2.0f * waterY;

        Matrix viewCopy = view;
        return reflection * viewCopy;
    }

    void transitionResource(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        // Standard D3D12 resource barrier for switching between render target and shader resource usage
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = res;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        core->getCommandList()->ResourceBarrier(1, &barrier);
    }
};
