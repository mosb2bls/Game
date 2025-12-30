#pragma once

#include <d3d12.h>
#include <vector>
#include "Core.h"
#include "Maths.h"

#define SQ(x) ((x)*(x))

// Vertex structure for static meshes
struct STATIC_VERTEX
{
    Vec3 pos;
    Vec3 normal;
    Vec3 tangent;
    float tu;
    float tv;
};

// Vertex structure for animated meshes
struct ANIMATED_VERTEX
{
    Vec3 pos;
    Vec3 normal;
    Vec3 tangent;
    float tu;
    float tv;
    unsigned int boneIDs[4];
    float boneWeights[4];
};

// Vertex layout cache for creating PSOs
class VertexLayoutCache
{
public:
    static D3D12_INPUT_LAYOUT_DESC getStaticLayout()
    {
        static D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        D3D12_INPUT_LAYOUT_DESC desc;
        desc.pInputElementDescs = layout;
        desc.NumElements = _countof(layout);
        return desc;
    }

    static D3D12_INPUT_LAYOUT_DESC getAnimatedLayout()
    {
        static D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT",      0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BONEIDS",      0, DXGI_FORMAT_R32G32B32A32_UINT,  0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BONEWEIGHTS",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 60, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        D3D12_INPUT_LAYOUT_DESC desc;
        desc.pInputElementDescs = layout;
        desc.NumElements = _countof(layout);
        return desc;
    }

    static D3D12_INPUT_LAYOUT_DESC getGrassInstancedLayout()
    {
        static D3D12_INPUT_ELEMENT_DESC layout[] = {
            // Slot 0: Per-vertex data (from mesh)
            { "POSITION",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD",  0, DXGI_FORMAT_R32G32_FLOAT,    0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

            // Slot 1: Per-instance data (from instance buffer)
            { "INSTANCEPOS",       0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0,  D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "INSTANCEROT",       0, DXGI_FORMAT_R32_FLOAT,       1, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "INSTANCESCALE",     0, DXGI_FORMAT_R32_FLOAT,       1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "INSTANCEWINDPHASE", 0, DXGI_FORMAT_R32_FLOAT,       1, 20, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        };

        D3D12_INPUT_LAYOUT_DESC desc;
        desc.pInputElementDescs = layout;
        desc.NumElements = _countof(layout);
        return desc;
    }

    static D3D12_INPUT_LAYOUT_DESC getRockInstancedLayout()
    {
        static D3D12_INPUT_ELEMENT_DESC layout[] = {
            // Slot 0: Per-vertex data (from mesh)
            { "POSITION",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD",  0, DXGI_FORMAT_R32G32_FLOAT,    0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

            // Slot 1: Per-instance data (rocks: position, rotation, scale only)
            { "INSTANCEPOS",   0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0,  D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "INSTANCEROT",   0, DXGI_FORMAT_R32_FLOAT,       1, 12, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "INSTANCESCALE", 0, DXGI_FORMAT_R32_FLOAT,       1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        };

        D3D12_INPUT_LAYOUT_DESC desc;
        desc.pInputElementDescs = layout;
        desc.NumElements = _countof(layout);
        return desc;
    }
};

// Main Mesh class
class Mesh
{
public:
    Mesh() : vertexBuffer(nullptr), indexBuffer(nullptr),
        vertexCount(0), indexCount(0),
        vertexStride(0), vertexBufferSize(0), indexBufferSize(0) {
    }

    // Initialize with static vertices
    void init(Core* core, std::vector<STATIC_VERTEX>& vertices, std::vector<unsigned int>& indices)
    {
        vertexCount = (unsigned int)vertices.size();
        indexCount = (unsigned int)indices.size();
        vertexStride = sizeof(STATIC_VERTEX);
        vertexBufferSize = vertexCount * vertexStride;
        indexBufferSize = indexCount * sizeof(unsigned int);

        createVertexBuffer(core, vertices.data(), vertexBufferSize);
        createIndexBuffer(core, indices.data(), indexBufferSize);
    }

    // Initialize with animated vertices
    void init(Core* core, std::vector<ANIMATED_VERTEX>& vertices, std::vector<unsigned int>& indices)
    {
        vertexCount = (unsigned int)vertices.size();
        indexCount = (unsigned int)indices.size();
        vertexStride = sizeof(ANIMATED_VERTEX);
        vertexBufferSize = vertexCount * vertexStride;
        indexBufferSize = indexCount * sizeof(unsigned int);

        createVertexBuffer(core, vertices.data(), vertexBufferSize);
        createIndexBuffer(core, indices.data(), indexBufferSize);
    }

    // Draw the mesh
    void draw(Core* core)
    {
        D3D12_VERTEX_BUFFER_VIEW vbView = getVertexBufferView();
        D3D12_INDEX_BUFFER_VIEW ibView = getIndexBufferView();

        core->getCommandList()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        core->getCommandList()->IASetVertexBuffers(0, 1, &vbView);
        core->getCommandList()->IASetIndexBuffer(&ibView);
        core->getCommandList()->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
    }

    // Getters for instanced rendering
    D3D12_VERTEX_BUFFER_VIEW getVertexBufferView() const
    {
        D3D12_VERTEX_BUFFER_VIEW view;
        view.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        view.StrideInBytes = vertexStride;
        view.SizeInBytes = vertexBufferSize;
        return view;
    }

    D3D12_INDEX_BUFFER_VIEW getIndexBufferView() const
    {
        D3D12_INDEX_BUFFER_VIEW view;
        view.BufferLocation = indexBuffer->GetGPUVirtualAddress();
        view.Format = DXGI_FORMAT_R32_UINT;
        view.SizeInBytes = indexBufferSize;
        return view;
    }

    unsigned int getIndexCount() const { return indexCount; }
    unsigned int getVertexCount() const { return vertexCount; }

    ~Mesh()
    {
        if (vertexBuffer) vertexBuffer->Release();
        if (indexBuffer) indexBuffer->Release();
    }

private:
    ID3D12Resource* vertexBuffer;
    ID3D12Resource* indexBuffer;

    unsigned int vertexCount;
    unsigned int indexCount;
    unsigned int vertexStride;
    unsigned int vertexBufferSize;
    unsigned int indexBufferSize;

    void createVertexBuffer(Core* core, void* data, unsigned int size)
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = size;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        core->device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&vertexBuffer)
        );

        core->uploadResource(vertexBuffer, data, size, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    }

    void createIndexBuffer(Core* core, void* data, unsigned int size)
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = size;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        core->device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&indexBuffer)
        );

        core->uploadResource(indexBuffer, data, size, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }
};