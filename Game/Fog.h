#pragma once

#include "Core.h"
#include "Maths.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

struct FogConfig
{
    float density = 0.015f;              // Base fog density
    float heightFalloff = 0.08f;         // Exponential height attenuation
    float groundLevel = 0.0f;            // Fog start height reference
    float maxHeight = 80.0f;             // Hard cap for fog contribution

    Vec3 fogColor = Vec3(0.6f, 0.7f, 0.85f);      // Fog albedo/tint
    Vec3 sunColor = Vec3(1.0f, 0.95f, 0.8f);      // Sun scattering tint
    Vec3 ambientColor = Vec3(0.4f, 0.5f, 0.6f);   // Ambient contribution

    Vec3 sunDirection = Vec3(0.4f, 0.6f, -0.5f);  // Light direction (world)
    float scattering = 0.5f;                      // Scattering intensity
    float mieG = 0.75f;                           // Mie phase asymmetry

    int raymarchSteps = 24;            // Fog raymarch step count
    float maxDistance = 150.0f;        // Max ray distance

    float windSpeed = 0.3f;            // Noise advection speed
    Vec2 windDirection = Vec2(1.0f, 0.3f); // Noise advection direction

    float blurStrength = 1.0f;         // Blur intensity scalar
    float blurRadius = 8.0f;           // Blur radius in pixels
    float blurBlend = 0.8f;            // Composite blend factor (0..1)
};

class VolumetricFog
{
public:
    FogConfig config;                  // Runtime tunables
    bool enabled = true;               // Master toggle

    // Allocate RTs/heaps, compile shaders, create PSOs, build fullscreen geometry
    void init(Core* core, int screenWidth, int screenHeight)
    {
        this->core = core;
        this->screenWidth = screenWidth;
        this->screenHeight = screenHeight;

        fogWidth = screenWidth / 2;
        fogHeight = screenHeight / 2;

        std::cout << "\n[VolumetricFog] Initializing with Gaussian Blur...\n";
        std::cout << "  Full res: " << screenWidth << "x" << screenHeight << "\n";
        std::cout << "  Fog res: " << fogWidth << "x" << fogHeight << " (half)\n";

        createDescriptorHeaps();
        createRenderTargets();
        createRootSignature();
        loadShaders();
        createPSOs();
        createConstantBuffers();
        createFullscreenQuad();

        initialized = true;
        std::cout << "[VolumetricFog] Ready with Gaussian blur!\n\n";
    }

    // Redirect scene rendering into offscreen scene RT/DSV (or backbuffer if disabled)
    void beginSceneCapture()
    {
        if (!initialized) return;

        auto cmdList = core->getCommandList();

        if (!enabled)
        {
            UINT rtvSize = core->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV = core->backbufferHeap->GetCPUDescriptorHandleForHeapStart();
            backBufferRTV.ptr += core->frameIndex() * rtvSize;

            cmdList->OMSetRenderTargets(1, &backBufferRTV, FALSE, &core->dsvHandle);

            D3D12_VIEWPORT vp = { 0, 0, (float)screenWidth, (float)screenHeight, 0, 1 };
            D3D12_RECT scissor = { 0, 0, (LONG)screenWidth, (LONG)screenHeight };
            cmdList->RSSetViewports(1, &vp);
            cmdList->RSSetScissorRects(1, &scissor);
            return;
        }

        transitionResource(sceneColorBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        cmdList->OMSetRenderTargets(1, &sceneRTV, FALSE, &sceneDSV);

        float clearColor[4] = { 0.5f, 0.7f, 0.9f, 1.0f };
        cmdList->ClearRenderTargetView(sceneRTV, clearColor, 0, nullptr);
        cmdList->ClearDepthStencilView(sceneDSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        D3D12_VIEWPORT vp = { 0, 0, (float)screenWidth, (float)screenHeight, 0, 1 };
        D3D12_RECT scissor = { 0, 0, (LONG)screenWidth, (LONG)screenHeight };
        cmdList->RSSetViewports(1, &vp);
        cmdList->RSSetScissorRects(1, &scissor);
    }

    // Execute fog + blur + composite passes and output to backbuffer
    void endSceneAndApplyFog(const Matrix& view, const Matrix& projection,
        const Vec3& cameraPos, float totalTime)
    {
        if (!initialized || !enabled) return;

        auto cmdList = core->getCommandList();

        transitionResource(sceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transitionResource(sceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        renderFogPass(view, projection, cameraPos, totalTime);
        renderBlurHorizontalPass();
        renderBlurVerticalPass();
        renderCompositePass();

        transitionResource(sceneDepthBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    // Release all owned GPU resources/heaps/PSOs/shader blobs
    ~VolumetricFog()
    {
        if (sceneColorBuffer) sceneColorBuffer->Release();
        if (sceneDepthBuffer) sceneDepthBuffer->Release();
        if (fogBuffer) fogBuffer->Release();
        if (blurTempBuffer) blurTempBuffer->Release();
        if (blurredBuffer) blurredBuffer->Release();
        if (fogConstantBuffer) fogConstantBuffer->Release();
        if (blurConstantBuffer) blurConstantBuffer->Release();
        if (compositeConstantBuffer) compositeConstantBuffer->Release();
        if (quadVertexBuffer) quadVertexBuffer->Release();
        if (fogRootSignature) fogRootSignature->Release();
        if (fogPSO) fogPSO->Release();
        if (blurHorizontalPSO) blurHorizontalPSO->Release();
        if (blurVerticalPSO) blurVerticalPSO->Release();
        if (compositePSO) compositePSO->Release();
        if (rtvHeap) rtvHeap->Release();
        if (dsvHeap) dsvHeap->Release();
        if (srvHeap) srvHeap->Release();
    }

private:
    Core* core = nullptr;               // D3D12 context/utility wrapper
    bool initialized = false;           // Init guard

    int screenWidth, screenHeight;      // Full-resolution dimensions
    int fogWidth, fogHeight;            // Half-resolution fog dimensions

    ID3D12Resource* sceneColorBuffer = nullptr;   // Full-res scene color
    ID3D12Resource* sceneDepthBuffer = nullptr;   // Full-res scene depth
    ID3D12Resource* fogBuffer = nullptr;          // Half-res fog output
    ID3D12Resource* blurTempBuffer = nullptr;     // Full-res blur intermediate
    ID3D12Resource* blurredBuffer = nullptr;      // Full-res blurred scene

    ID3D12Resource* fogConstantBuffer = nullptr;       // Fog CB (raymarch)
    ID3D12Resource* blurConstantBuffer = nullptr;      // Blur CB (H/V)
    ID3D12Resource* compositeConstantBuffer = nullptr; // Composite CB
    ID3D12Resource* quadVertexBuffer = nullptr;        // Fullscreen triangle VB

    ID3D12RootSignature* fogRootSignature = nullptr; // Shared root signature (all passes)
    ID3D12PipelineState* fogPSO = nullptr;            // Raymarch fog PSO
    ID3D12PipelineState* blurHorizontalPSO = nullptr; // Horizontal blur PSO
    ID3D12PipelineState* blurVerticalPSO = nullptr;   // Vertical blur PSO
    ID3D12PipelineState* compositePSO = nullptr;      // Final composite PSO

    ID3DBlob* vsFullscreenBlob = nullptr;        // Fullscreen VS
    ID3DBlob* psFogBlob = nullptr;               // Fog PS
    ID3DBlob* psBlurHorizontalBlob = nullptr;    // Blur H PS
    ID3DBlob* psBlurVerticalBlob = nullptr;      // Blur V PS
    ID3DBlob* psCompositeBlob = nullptr;         // Composite PS

    ID3D12DescriptorHeap* rtvHeap = nullptr;     // RTV heap for offscreen RTs
    ID3D12DescriptorHeap* dsvHeap = nullptr;     // DSV heap for scene depth
    ID3D12DescriptorHeap* srvHeap = nullptr;     // Shader-visible SRV heap

    D3D12_CPU_DESCRIPTOR_HANDLE sceneRTV;        // Scene color RTV
    D3D12_CPU_DESCRIPTOR_HANDLE fogRTV;          // Fog RTV
    D3D12_CPU_DESCRIPTOR_HANDLE blurTempRTV;     // Blur temp RTV
    D3D12_CPU_DESCRIPTOR_HANDLE blurredRTV;      // Blurred RTV
    D3D12_CPU_DESCRIPTOR_HANDLE sceneDSV;        // Scene DSV

    D3D12_GPU_DESCRIPTOR_HANDLE sceneColorSRV;   // Scene color SRV
    D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSRV;   // Scene depth SRV
    D3D12_GPU_DESCRIPTOR_HANDLE fogSRV;          // Fog SRV
    D3D12_GPU_DESCRIPTOR_HANDLE blurTempSRV;     // Blur temp SRV
    D3D12_GPU_DESCRIPTOR_HANDLE blurredSRV;      // Blurred SRV

    D3D12_VERTEX_BUFFER_VIEW quadVBView;         // Fullscreen VB view

    struct FogCB
    {
        Matrix invViewProj;          // Inverse view-projection for ray reconstruction
        Vec4 cameraPos_time;         // xyz=camera pos, w=time
        Vec4 fogColor_density;       // rgb=fog color, a=density
        Vec4 sunDir_scattering;      // xyz=sun dir, w=scattering
        Vec4 params1;                // x=heightFalloff,y=groundLevel,z=maxHeight,w=mieG
        Vec4 params2;                // x=maxDistance,y=windSpeed,z=windDir.x,w=windDir.y
        Vec4 sunColor_pad;           // rgb=sun color
        Vec4 ambientColor_pad;       // rgb=ambient color
        Vec4 screenSize;             // xy=screen, zw=fog buffer
        int numSteps;                // raymarch steps
        float pad[3];                // 16-byte alignment
    };

    struct BlurCB
    {
        Vec4 screenSize;             // xy=screen, zw=inv screen
        Vec4 blurParams;             // x=strength, y=radius
    };

    struct CompositeCB
    {
        Vec4 screenSize;             // xy=screen, zw=fog buffer
        Vec4 compositeParams;         // x=blurBlend
    };

    void createDescriptorHeaps()
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.NumDescriptors = 4;   // scene, fog, blurTemp, blurred
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        core->device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap));

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
        dsvDesc.NumDescriptors = 1;   // scene depth
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        core->device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsvHeap));

        D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
        srvDesc.NumDescriptors = 8;   // scene color/depth + fog + blur intermediates
        srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        core->device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap));
    }

    void createRenderTargets()
    {
        UINT rtvSize = core->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        UINT srvSize = core->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        // Create RT + RTV + SRV in the local heaps (indices are caller-controlled)
        auto createRT = [&](ID3D12Resource** resource, DXGI_FORMAT format, int w, int h,
            D3D12_CPU_DESCRIPTOR_HANDLE* rtv, D3D12_GPU_DESCRIPTOR_HANDLE* srv,
            int rtvIndex, int srvIndex)
            {
                D3D12_RESOURCE_DESC desc = {};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                desc.Width = w;
                desc.Height = h;
                desc.DepthOrArraySize = 1;
                desc.MipLevels = 1;
                desc.Format = format;
                desc.SampleDesc.Count = 1;
                desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

                D3D12_CLEAR_VALUE clearValue = {};
                clearValue.Format = format;

                core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue, IID_PPV_ARGS(resource));

                *rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
                rtv->ptr += rtvIndex * rtvSize;
                core->device->CreateRenderTargetView(*resource, nullptr, *rtv);

                D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
                srvCpu.ptr += srvIndex * srvSize;
                *srv = srvHeap->GetGPUDescriptorHandleForHeapStart();
                srv->ptr += srvIndex * srvSize;

                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                srvDesc.Format = format;
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.Texture2D.MipLevels = 1;
                core->device->CreateShaderResourceView(*resource, &srvDesc, srvCpu);
            };

        createRT(&sceneColorBuffer, DXGI_FORMAT_R11G11B10_FLOAT, screenWidth, screenHeight,
            &sceneRTV, &sceneColorSRV, 0, 0);

        createRT(&fogBuffer, DXGI_FORMAT_R16G16B16A16_FLOAT, fogWidth, fogHeight,
            &fogRTV, &fogSRV, 1, 2);

        createRT(&blurTempBuffer, DXGI_FORMAT_R11G11B10_FLOAT, screenWidth, screenHeight,
            &blurTempRTV, &blurTempSRV, 2, 3);

        createRT(&blurredBuffer, DXGI_FORMAT_R11G11B10_FLOAT, screenWidth, screenHeight,
            &blurredRTV, &blurredSRV, 3, 4);

        // Create depth texture as typeless, then view as DSV (D32) and SRV (R32_FLOAT)
        D3D12_RESOURCE_DESC depthDesc = {};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = screenWidth;
        depthDesc.Height = screenHeight;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depthClear = {};
        depthClear.Format = DXGI_FORMAT_D32_FLOAT;
        depthClear.DepthStencil.Depth = 1.0f;

        core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, IID_PPV_ARGS(&sceneDepthBuffer));

        sceneDSV = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        core->device->CreateDepthStencilView(sceneDepthBuffer, &dsvDesc, sceneDSV);

        D3D12_CPU_DESCRIPTOR_HANDLE depthSrvCpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
        depthSrvCpu.ptr += 1 * srvSize;
        sceneDepthSRV = srvHeap->GetGPUDescriptorHandleForHeapStart();
        sceneDepthSRV.ptr += 1 * srvSize;

        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;
        core->device->CreateShaderResourceView(sceneDepthBuffer, &depthSrvDesc, depthSrvCpu);

        std::cout << "[VolumetricFog] Render targets created\n";
    }

    void createRootSignature()
    {
        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 5;   // scene color/depth + fog + blur buffers
        srvRange.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[2] = {};

        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; // b0
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; // t0..t4
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srvRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister = 0; // s0
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &sampler;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* signature = nullptr;
        ID3DBlob* error = nullptr;
        D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);

        if (error)
        {
            std::cout << "[VolumetricFog] Root signature error: " << (char*)error->GetBufferPointer() << "\n";
            error->Release();
        }

        core->device->CreateRootSignature(0, signature->GetBufferPointer(),
            signature->GetBufferSize(), IID_PPV_ARGS(&fogRootSignature));

        if (signature) signature->Release();
        std::cout << "[VolumetricFog] Root signature created\n";
    }

    // Load shader source text from disk
    std::string loadShaderFile(const std::string& filename)
    {
        std::ifstream file(filename);
        if (!file.is_open())
        {
            std::cout << "[VolumetricFog] ERROR: Cannot open shader file: " << filename << "\n";
            return "";
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // Compile HLSL source into a shader blob
    ID3DBlob* compileShader(const std::string& source, const std::string& entryPoint,
        const std::string& target, const std::string& name)
    {
        ID3DBlob* blob = nullptr;
        ID3DBlob* error = nullptr;

        HRESULT hr = D3DCompile(source.c_str(), source.length(), name.c_str(),
            nullptr, nullptr, entryPoint.c_str(), target.c_str(),
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error);

        if (FAILED(hr) || error)
        {
            std::cout << "[VolumetricFog] Shader compile error (" << name << "): "
                << (error ? (char*)error->GetBufferPointer() : "Unknown") << "\n";
            if (error) error->Release();
            return nullptr;
        }

        std::cout << "[VolumetricFog] Compiled: " << name << "\n";
        return blob;
    }

    void loadShaders()
    {
        std::cout << "[VolumetricFog] Loading shaders...\n";

        std::string vsSource = loadShaderFile("Shaders/VSFullscreen.txt");
        vsFullscreenBlob = compileShader(vsSource, "main", "vs_5_0", "VSFullscreen");

        std::string psFogSource = loadShaderFile("Shaders/PSFogRaymarch.txt");
        psFogBlob = compileShader(psFogSource, "main", "ps_5_0", "PSFogRaymarch");

        std::string psBlurHSource = loadShaderFile("Shaders/PSBlurHorizontal.txt");
        psBlurHorizontalBlob = compileShader(psBlurHSource, "main", "ps_5_0", "PSBlurHorizontal");

        std::string psBlurVSource = loadShaderFile("Shaders/PSBlurVertical.txt");
        psBlurVerticalBlob = compileShader(psBlurVSource, "main", "ps_5_0", "PSBlurVertical");

        std::string psCompSource = loadShaderFile("Shaders/PSFogComposite.txt");
        psCompositeBlob = compileShader(psCompSource, "main", "ps_5_0", "PSFogComposite");
    }

    void createPSOs()
    {
        // PSOs depend on all shader blobs being valid
        if (!vsFullscreenBlob || !psFogBlob || !psBlurHorizontalBlob ||
            !psBlurVerticalBlob || !psCompositeBlob)
        {
            std::cout << "[VolumetricFog] ERROR: Shader compilation failed, cannot create PSOs\n";
            return;
        }

        D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = fogRootSignature;
        psoDesc.VS = { vsFullscreenBlob->GetBufferPointer(), vsFullscreenBlob->GetBufferSize() };
        psoDesc.InputLayout = { layout, 2 };
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.SampleDesc.Count = 1;

        psoDesc.PS = { psFogBlob->GetBufferPointer(), psFogBlob->GetBufferSize() };
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        core->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&fogPSO));

        psoDesc.PS = { psBlurHorizontalBlob->GetBufferPointer(), psBlurHorizontalBlob->GetBufferSize() };
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R11G11B10_FLOAT;
        core->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&blurHorizontalPSO));

        psoDesc.PS = { psBlurVerticalBlob->GetBufferPointer(), psBlurVerticalBlob->GetBufferSize() };
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R11G11B10_FLOAT;
        core->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&blurVerticalPSO));

        psoDesc.PS = { psCompositeBlob->GetBufferPointer(), psCompositeBlob->GetBufferSize() };
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        core->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&compositePSO));

        std::cout << "[VolumetricFog] PSOs created\n";
    }

    void createConstantBuffers()
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = 256; // assumes CB payload fits within 256 bytes
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&fogConstantBuffer));

        core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&blurConstantBuffer));

        core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&compositeConstantBuffer));
    }

    void createFullscreenQuad()
    {
        // Fullscreen triangle (covers the screen without an index buffer)
        struct Vertex { float x, y, u, v; };
        Vertex verts[3] = {
            { -1,  3, 0, -1 },
            { -1, -1, 0,  1 },
            {  3, -1, 2,  1 }
        };

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = sizeof(verts);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        core->device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&quadVertexBuffer));

        void* data;
        quadVertexBuffer->Map(0, nullptr, &data);
        memcpy(data, verts, sizeof(verts));
        quadVertexBuffer->Unmap(0, nullptr);

        quadVBView.BufferLocation = quadVertexBuffer->GetGPUVirtualAddress();
        quadVBView.SizeInBytes = sizeof(verts);
        quadVBView.StrideInBytes = sizeof(Vertex);
    }

    // Insert a transition barrier for the given resource
    void transitionResource(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = res;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        core->getCommandList()->ResourceBarrier(1, &barrier);
    }

    // Draw the fullscreen triangle with current PSO/root bindings
    void drawFullscreenQuad(ID3D12GraphicsCommandList* cmdList)
    {
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->IASetVertexBuffers(0, 1, &quadVBView);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    void renderFogPass(const Matrix& view, const Matrix& proj, const Vec3& camPos, float time)
    {
        auto cmdList = core->getCommandList();

        Matrix viewCopy = view;
        Matrix projCopy = proj;
        Matrix vp = viewCopy * projCopy;
        Matrix invVP = vp.invert();

        // Update fog raymarch constant buffer
        FogCB cb;
        cb.invViewProj = invVP;
        cb.cameraPos_time = Vec4(camPos.x, camPos.y, camPos.z, time);
        cb.fogColor_density = Vec4(config.fogColor.x, config.fogColor.y, config.fogColor.z, config.density);
        cb.sunDir_scattering = Vec4(config.sunDirection.x, config.sunDirection.y, config.sunDirection.z, config.scattering);
        cb.params1 = Vec4(config.heightFalloff, config.groundLevel, config.maxHeight, config.mieG);
        cb.params2 = Vec4(config.maxDistance, config.windSpeed, config.windDirection.x, config.windDirection.y);
        cb.sunColor_pad = Vec4(config.sunColor.x, config.sunColor.y, config.sunColor.z, 0);
        cb.ambientColor_pad = Vec4(config.ambientColor.x, config.ambientColor.y, config.ambientColor.z, 0);
        cb.screenSize = Vec4((float)screenWidth, (float)screenHeight, (float)fogWidth, (float)fogHeight);
        cb.numSteps = config.raymarchSteps;

        void* data;
        fogConstantBuffer->Map(0, nullptr, &data);
        memcpy(data, &cb, sizeof(cb));
        fogConstantBuffer->Unmap(0, nullptr);

        transitionResource(fogBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        cmdList->OMSetRenderTargets(1, &fogRTV, FALSE, nullptr);
        float clear[4] = { 0, 0, 0, 0 };
        cmdList->ClearRenderTargetView(fogRTV, clear, 0, nullptr);

        D3D12_VIEWPORT vp2 = { 0, 0, (float)fogWidth, (float)fogHeight, 0, 1 };
        D3D12_RECT scissor = { 0, 0, (LONG)fogWidth, (LONG)fogHeight };
        cmdList->RSSetViewports(1, &vp2);
        cmdList->RSSetScissorRects(1, &scissor);

        cmdList->SetPipelineState(fogPSO);
        cmdList->SetGraphicsRootSignature(fogRootSignature);
        cmdList->SetGraphicsRootConstantBufferView(0, fogConstantBuffer->GetGPUVirtualAddress());

        ID3D12DescriptorHeap* heaps[] = { srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootDescriptorTable(1, sceneColorSRV);

        drawFullscreenQuad(cmdList);

        transitionResource(fogBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    void renderBlurHorizontalPass()
    {
        auto cmdList = core->getCommandList();

        // Horizontal blur CB update (sceneColor as input)
        BlurCB cb;
        cb.screenSize = Vec4((float)screenWidth, (float)screenHeight,
            1.0f / screenWidth, 1.0f / screenHeight);
        cb.blurParams = Vec4(config.blurStrength, config.blurRadius, 0, 0);

        void* data;
        blurConstantBuffer->Map(0, nullptr, &data);
        memcpy(data, &cb, sizeof(cb));
        blurConstantBuffer->Unmap(0, nullptr);

        transitionResource(blurTempBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        cmdList->OMSetRenderTargets(1, &blurTempRTV, FALSE, nullptr);

        D3D12_VIEWPORT vp = { 0, 0, (float)screenWidth, (float)screenHeight, 0, 1 };
        D3D12_RECT scissor = { 0, 0, (LONG)screenWidth, (LONG)screenHeight };
        cmdList->RSSetViewports(1, &vp);
        cmdList->RSSetScissorRects(1, &scissor);

        cmdList->SetPipelineState(blurHorizontalPSO);
        cmdList->SetGraphicsRootSignature(fogRootSignature);
        cmdList->SetGraphicsRootConstantBufferView(0, blurConstantBuffer->GetGPUVirtualAddress());

        ID3D12DescriptorHeap* heaps[] = { srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootDescriptorTable(1, sceneColorSRV);

        drawFullscreenQuad(cmdList);

        transitionResource(blurTempBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    void renderBlurVerticalPass()
    {
        auto cmdList = core->getCommandList();

        // Vertical blur reads blurTemp and writes blurredBuffer
        transitionResource(blurredBuffer, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        cmdList->OMSetRenderTargets(1, &blurredRTV, FALSE, nullptr);

        D3D12_VIEWPORT vp = { 0, 0, (float)screenWidth, (float)screenHeight, 0, 1 };
        D3D12_RECT scissor = { 0, 0, (LONG)screenWidth, (LONG)screenHeight };
        cmdList->RSSetViewports(1, &vp);
        cmdList->RSSetScissorRects(1, &scissor);

        cmdList->SetPipelineState(blurVerticalPSO);
        cmdList->SetGraphicsRootSignature(fogRootSignature);
        cmdList->SetGraphicsRootConstantBufferView(0, blurConstantBuffer->GetGPUVirtualAddress());

        ID3D12DescriptorHeap* heaps[] = { srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootDescriptorTable(1, blurTempSRV);

        drawFullscreenQuad(cmdList);

        transitionResource(blurredBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    void renderCompositePass()
    {
        auto cmdList = core->getCommandList();

        // Composite CB update (final blend control)
        CompositeCB cb;
        cb.screenSize = Vec4((float)screenWidth, (float)screenHeight, (float)fogWidth, (float)fogHeight);
        cb.compositeParams = Vec4(config.blurBlend, 0, 0, 0);

        void* data;
        compositeConstantBuffer->Map(0, nullptr, &data);
        memcpy(data, &cb, sizeof(cb));
        compositeConstantBuffer->Unmap(0, nullptr);

        // Bind swapchain backbuffer as output
        UINT rtvSize = core->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferRTV = core->backbufferHeap->GetCPUDescriptorHandleForHeapStart();
        backBufferRTV.ptr += core->frameIndex() * rtvSize;

        cmdList->OMSetRenderTargets(1, &backBufferRTV, FALSE, nullptr);

        D3D12_VIEWPORT vp = { 0, 0, (float)screenWidth, (float)screenHeight, 0, 1 };
        D3D12_RECT scissor = { 0, 0, (LONG)screenWidth, (LONG)screenHeight };
        cmdList->RSSetViewports(1, &vp);
        cmdList->RSSetScissorRects(1, &scissor);

        cmdList->SetPipelineState(compositePSO);
        cmdList->SetGraphicsRootSignature(fogRootSignature);
        cmdList->SetGraphicsRootConstantBufferView(0, compositeConstantBuffer->GetGPUVirtualAddress());

        ID3D12DescriptorHeap* heaps[] = { srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootDescriptorTable(1, sceneColorSRV);

        drawFullscreenQuad(cmdList);
    }
};
