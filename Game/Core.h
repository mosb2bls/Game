#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>

#include <string>          

#include <iostream>
#include "stb_image.h"
#pragma comment(lib, "d3d12")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "d3dcompiler.lib")


class GPUFence
{
public:
	ID3D12Fence* fence;
	HANDLE eventHandle;
	long long value;

	// Create fence + event for CPU/GPU synchronization
	void create(ID3D12Device5* device)
	{
		value = 0;
		device->CreateFence(value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
		eventHandle = CreateEvent(NULL, FALSE, FALSE, NULL);
	}

	// Signal GPU fence (advance value)
	void signal(ID3D12CommandQueue* queue)
	{
		queue->Signal(fence, ++value);
	}

	// Wait on CPU until GPU reaches current fence value
	void wait()
	{
		if (fence->GetCompletedValue() < value)
		{
			fence->SetEventOnCompletion(value, eventHandle);
			WaitForSingleObject(eventHandle, INFINITE);
		}
	}

	// Release OS + D3D resources
	~GPUFence()
	{
		CloseHandle(eventHandle);
		fence->Release();
	}
};

class Barrier
{
public:
	// Insert a resource state transition barrier
	static void add(ID3D12Resource* res, D3D12_RESOURCE_STATES first, D3D12_RESOURCE_STATES second, ID3D12GraphicsCommandList4* commandList)
	{
		D3D12_RESOURCE_BARRIER rb;
		memset(&rb, 0, sizeof(D3D12_RESOURCE_BARRIER));
		rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		rb.Transition.pResource = res;
		rb.Transition.StateBefore = first;
		rb.Transition.StateAfter = second;
		rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		commandList->ResourceBarrier(1, &rb);
	}
};

struct Texture {
	ID3D12Resource* resource;                 // GPU texture resource
	D3D12_GPU_DESCRIPTOR_HANDLE srvHandle;    // SRV handle in shader-visible heap
};

class Core
{
public:
	ID3D12Device5* device;
	ID3D12CommandQueue* graphicsQueue;
	ID3D12CommandQueue* copyQueue;
	ID3D12CommandQueue* computeQueue;
	IDXGISwapChain3* swapchain;

	ID3D12DescriptorHeap* backbufferHeap;
	ID3D12Resource** backbuffers;

	D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle;
	ID3D12DescriptorHeap* dsvHeap;
	ID3D12Resource* dsv;

	D3D12_VIEWPORT viewport;
	D3D12_RECT scissorRect;

	ID3D12DescriptorHeap* srvHeap;
	unsigned int srvDescriptorSize;
	unsigned int srvHeapIndex = 0;

	ID3D12CommandAllocator* graphicsCommandAllocator[2];
	ID3D12GraphicsCommandList4* graphicsCommandList[2];
	ID3D12RootSignature* rootSignature;
	unsigned int srvTableIndex;
	GPUFence graphicsQueueFence[2];
	int width;
	int height;
	HWND windowHandle;
	ID3D12DescriptorHeap* rtvHeap;
	int frameInd;

	// Initialise device, queues, swapchain, heaps, command lists, fences, root signature
	void init(HWND hwnd, int _width, int _height)
	{
		IDXGIFactory4* factory = nullptr;
		HRESULT hrFactory = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
		if (FAILED(hrFactory) || factory == nullptr)
			return;

		int i = 0;
		IDXGIAdapter1* adapterf;
		std::vector<IDXGIAdapter1*> adapters;
		while (factory->EnumAdapters1(i, &adapterf) != DXGI_ERROR_NOT_FOUND)
		{
			adapters.push_back(adapterf);
			i++;
		}
		DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		long long maxVideoMemory = 0;
		int useAdapterIndex = 0;

		// Pick adapter with the most dedicated VRAM
		for (int i = 0; i < adapters.size(); i++)
		{
			DXGI_ADAPTER_DESC desc;
			adapters[i]->GetDesc(&desc);
			if (desc.DedicatedVideoMemory > maxVideoMemory)
			{
				maxVideoMemory = desc.DedicatedVideoMemory;
				useAdapterIndex = i;
			}
		}
		IDXGIAdapter* adapter = adapters[useAdapterIndex];

		// Create Device
		D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device));

		for (auto& adapter : adapters)
		{
			adapter->Release();
		}

		// Create Queues
		D3D12_COMMAND_QUEUE_DESC graphicsQueueDesc;
		memset(&graphicsQueueDesc, 0, sizeof(D3D12_COMMAND_QUEUE_DESC));
		graphicsQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		device->CreateCommandQueue(&graphicsQueueDesc, IID_PPV_ARGS(&graphicsQueue));
		D3D12_COMMAND_QUEUE_DESC copyQueueDesc;
		memset(&copyQueueDesc, 0, sizeof(D3D12_COMMAND_QUEUE_DESC));
		copyQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
		device->CreateCommandQueue(&copyQueueDesc, IID_PPV_ARGS(&copyQueue));
		D3D12_COMMAND_QUEUE_DESC computeQueueDesc;
		memset(&computeQueueDesc, 0, sizeof(D3D12_COMMAND_QUEUE_DESC));
		computeQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
		device->CreateCommandQueue(&computeQueueDesc, IID_PPV_ARGS(&computeQueue));

		// Create Swapchain
		DXGI_SWAP_CHAIN_DESC1 scDesc;
		memset(&scDesc, 0, sizeof(DXGI_SWAP_CHAIN_DESC1));
		scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		scDesc.Width = _width;
		scDesc.Height = _height;
		scDesc.SampleDesc.Count = 1; // MSAA here
		scDesc.SampleDesc.Quality = 0;
		scDesc.BufferCount = 2;
		scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		IDXGISwapChain1* swapChain1;
		factory->CreateSwapChainForHwnd(graphicsQueue, hwnd, &scDesc, nullptr, nullptr, &swapChain1);
		swapChain1->QueryInterface(&swapchain); // This is needed to map to the correct interface
		swapChain1->Release();

		factory->Release();

		D3D12_DESCRIPTOR_HEAP_DESC renderTargetViewHeapDesc;
		D3D12_CPU_DESCRIPTOR_HANDLE renderTargetViewHandle;
		memset(&renderTargetViewHeapDesc, 0, sizeof(D3D12_DESCRIPTOR_HEAP_DESC));
		renderTargetViewHeapDesc.NumDescriptors = scDesc.BufferCount;
		renderTargetViewHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		renderTargetViewHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		device->CreateDescriptorHeap(&renderTargetViewHeapDesc, IID_PPV_ARGS(&backbufferHeap));
		renderTargetViewHandle = backbufferHeap->GetCPUDescriptorHandleForHeapStart();
		backbuffers = new ID3D12Resource * [scDesc.BufferCount];
		backbuffers[0] = NULL;
		backbuffers[1] = NULL;

		D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
		memset(&dsvHeapDesc, 0, sizeof(D3D12_DESCRIPTOR_HEAP_DESC));
		dsvHeapDesc.NumDescriptors = 1;
		dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap));
		dsvHandle = dsvHeap->GetCPUDescriptorHandleForHeapStart();
		dsv = NULL;

		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
		srvHeapDesc.NumDescriptors = 128; // Allow up to 128 textures
		srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; // Important!
		device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap));

		srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		srvHeapIndex = 0;

		width = _width;
		height = _height;
		updateScreenResources(_width, _height);

		device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&graphicsCommandAllocator[0]));
		device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&graphicsCommandList[0]));
		device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&graphicsCommandAllocator[1]));
		device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&graphicsCommandList[1]));

		graphicsQueueFence[0].create(device);
		graphicsQueueFence[1].create(device);

		createRootSignature();

		windowHandle = hwnd;
	}


	// Load image, upload to GPU, create SRV, return resource + GPU handle
	Texture loadTexture(std::string filename)
	{
		// 1. Load Pixels from file
		int w, h, channels;
		unsigned char* data = stbi_load(filename.c_str(), &w, &h, &channels, 4);
		if (!data) return {};

		// 2. Create the GPU Resource
		D3D12_RESOURCE_DESC textureDesc = {};
		textureDesc.MipLevels = 1;
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.Width = w;
		textureDesc.Height = h;
		textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
		textureDesc.DepthOrArraySize = 1;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.SampleDesc.Quality = 0;
		textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

		ID3D12Resource* textureResource;
		D3D12_HEAP_PROPERTIES heapProps = {};
		heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

		device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &textureDesc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&textureResource));

		// 3. Calculate Footprint
		UINT64 uploadBufferSize;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
		D3D12_RESOURCE_DESC desc = textureResource->GetDesc();
		device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadBufferSize);

		// 4. Pad rows to match footprint.RowPitch for CopyTextureRegion
		std::vector<unsigned char> paddedData(uploadBufferSize);

		// Copy row-by-row into padded layout
		for (int y = 0; y < h; y++)
		{
			unsigned char* src = data + (y * w * 4);
			unsigned char* dst = paddedData.data() + (y * footprint.Footprint.RowPitch);
			memcpy(dst, src, w * 4);
		}

		// 5. Upload padded data and transition to shader resource state
		uploadResource(textureResource, paddedData.data(), uploadBufferSize,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &footprint);

		stbi_image_free(data);

		// 6. Allocate SRV slot in descriptor heap
		D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
		cpuHandle.ptr += srvHeapIndex * srvDescriptorSize;

		D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = srvHeap->GetGPUDescriptorHandleForHeapStart();
		gpuHandle.ptr += srvHeapIndex * srvDescriptorSize;

		srvHeapIndex++;

		// 7. Create SRV descriptor
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = textureDesc.Format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(textureResource, &srvDesc, cpuHandle);

		return { textureResource, gpuHandle };
	}

	// Recreate backbuffers/RTVs, viewport/scissor, depth buffer/DSV, and reset SRV heap
	void updateScreenResources(int _width, int _height)
	{
		for (unsigned int i = 0; i < 2; i++)
		{
			if (backbuffers[i] != NULL)
			{
				backbuffers[i]->Release();
			}
		}
		if (_width != width || _height != height)
		{
			swapchain->ResizeBuffers(0, _width, _height, DXGI_FORMAT_UNKNOWN, 0);
		}
		DXGI_SWAP_CHAIN_DESC desc;
		swapchain->GetDesc(&desc);
		width = desc.BufferDesc.Width;
		height = desc.BufferDesc.Height;

		D3D12_CPU_DESCRIPTOR_HANDLE renderTargetViewHandle;
		renderTargetViewHandle = backbufferHeap->GetCPUDescriptorHandleForHeapStart();
		unsigned int renderTargetViewDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		for (unsigned int i = 0; i < 2; i++)
		{
			swapchain->GetBuffer(i, IID_PPV_ARGS(&backbuffers[i]));
			device->CreateRenderTargetView(backbuffers[i], nullptr, renderTargetViewHandle);
			renderTargetViewHandle.ptr += renderTargetViewDescriptorSize;
		}

		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;
		viewport.Width = (float)width;
		viewport.Height = (float)height;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;

		scissorRect.left = 0;
		scissorRect.top = 0;
		scissorRect.right = width;
		scissorRect.bottom = height;

		// Recreate depth buffer for current screen size
		if (dsv != NULL)
		{
			dsv->Release();
		}
		D3D12_DEPTH_STENCIL_VIEW_DESC depthStencilDesc = {};
		depthStencilDesc.Format = DXGI_FORMAT_D32_FLOAT;
		depthStencilDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		depthStencilDesc.Flags = D3D12_DSV_FLAG_NONE;
		D3D12_CLEAR_VALUE depthClearValue = {};
		depthClearValue.Format = DXGI_FORMAT_D32_FLOAT;
		depthClearValue.DepthStencil.Depth = 1.0f;
		depthClearValue.DepthStencil.Stencil = 0;
		D3D12_HEAP_PROPERTIES heapprops;
		memset(&heapprops, 0, sizeof(D3D12_HEAP_PROPERTIES));
		heapprops.Type = D3D12_HEAP_TYPE_DEFAULT;
		heapprops.CreationNodeMask = 1;
		heapprops.VisibleNodeMask = 1;
		D3D12_RESOURCE_DESC dsvDesc;
		memset(&dsvDesc, 0, sizeof(D3D12_RESOURCE_DESC));
		dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
		dsvDesc.Width = width;
		dsvDesc.Height = height;
		dsvDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		dsvDesc.DepthOrArraySize = 1;
		dsvDesc.MipLevels = 1;
		dsvDesc.SampleDesc.Count = 1;
		dsvDesc.SampleDesc.Quality = 0;
		dsvDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		dsvDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		device->CreateCommittedResource(&heapprops, D3D12_HEAP_FLAG_NONE, &dsvDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClearValue, __uuidof(ID3D12Resource), (void**)&dsv);
		device->CreateDepthStencilView(dsv, &depthStencilDesc, dsvHeap->GetCPUDescriptorHandleForHeapStart());

		// Recreate SRV heap and reset allocation index
		D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
		srvHeapDesc.NumDescriptors = 128; // Allow up to 128 textures
		srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; // Important!
		device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap));

		srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		srvHeapIndex = 0;

	}

	// Build root signature for VS/PS CBVs and a texture SRV table + sampler
	void createRootSignature()
	{
		std::vector<D3D12_ROOT_PARAMETER> parameters;

		// Parameter 0: Vertex Shader Constant Buffer (b0)
		D3D12_ROOT_PARAMETER rootParameterCBVS;
		rootParameterCBVS.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParameterCBVS.Descriptor.ShaderRegister = 0;
		rootParameterCBVS.Descriptor.RegisterSpace = 0;
		rootParameterCBVS.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		parameters.push_back(rootParameterCBVS);

		// Parameter 1: Pixel Shader Constant Buffer (b0)
		D3D12_ROOT_PARAMETER rootParameterCBPS;
		rootParameterCBPS.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParameterCBPS.Descriptor.ShaderRegister = 0;
		rootParameterCBPS.Descriptor.RegisterSpace = 0;
		rootParameterCBPS.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		parameters.push_back(rootParameterCBPS);

		// Parameter 2: Texture SRV descriptor table (t0)
		D3D12_DESCRIPTOR_RANGE descriptorRange = {};
		descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		descriptorRange.NumDescriptors = 1; // single bound texture
		descriptorRange.BaseShaderRegister = 0; // t0
		descriptorRange.RegisterSpace = 0;
		descriptorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER rootParameterSRV = {};
		rootParameterSRV.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParameterSRV.DescriptorTable.NumDescriptorRanges = 1;
		rootParameterSRV.DescriptorTable.pDescriptorRanges = &descriptorRange;
		rootParameterSRV.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		parameters.push_back(rootParameterSRV);

		// Static sampler for texture sampling (s0)
		D3D12_STATIC_SAMPLER_DESC sampler = {};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR; // Linear filtering (smooth)
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP; // Repeat texture
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler.MipLODBias = 0;
		sampler.MaxAnisotropy = 0;
		sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		sampler.MinLOD = 0.0f;
		sampler.MaxLOD = D3D12_FLOAT32_MAX;
		sampler.ShaderRegister = 0; // s0
		sampler.RegisterSpace = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_ROOT_SIGNATURE_DESC desc = {};
		desc.NumParameters = parameters.size();
		desc.pParameters = parameters.data();
		desc.NumStaticSamplers = 1;
		desc.pStaticSamplers = &sampler;
		desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

		ID3DBlob* serialized;
		ID3DBlob* error;
		D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
		if (error) OutputDebugStringA((char*)error->GetBufferPointer());
		device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
		serialized->Release();
	}

	// Reset per-frame allocator + command list
	void resetCommandList()
	{
		unsigned int frameIndex = swapchain->GetCurrentBackBufferIndex();
		graphicsCommandAllocator[frameIndex]->Reset();
		graphicsCommandList[frameIndex]->Reset(graphicsCommandAllocator[frameIndex], NULL);
	}

	// Close and execute current command list on graphics queue
	void runCommandList()
	{
		getCommandList()->Close();
		ID3D12CommandList* lists[] = { getCommandList() };
		graphicsQueue->ExecuteCommandLists(1, lists);
	}

	// Upload CPU data through an UPLOAD buffer, then transition to target state
	void uploadResource(ID3D12Resource* dstResource, const void* data, unsigned int size, D3D12_RESOURCE_STATES targetState, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* texFootprint = NULL)
	{
		unsigned int frameIndex = swapchain->GetCurrentBackBufferIndex();
		ID3D12Resource* uploadBuffer;
		D3D12_HEAP_PROPERTIES heapProps = {};
		heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC bufferDesc = {};
		bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufferDesc.Width = size;
		bufferDesc.Height = 1;
		bufferDesc.DepthOrArraySize = 1;
		bufferDesc.MipLevels = 1;
		bufferDesc.SampleDesc.Count = 1;
		bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&uploadBuffer));

		void* mappeddata = nullptr;
		uploadBuffer->Map(0, nullptr, &mappeddata);
		memcpy(mappeddata, data, size);
		uploadBuffer->Unmap(0, nullptr);

		resetCommandList();

		// Texture path uses CopyTextureRegion with placed footprint
		if (texFootprint != NULL)
		{
			D3D12_TEXTURE_COPY_LOCATION src = {};
			src.pResource = uploadBuffer;
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint = *texFootprint;
			D3D12_TEXTURE_COPY_LOCATION dst = {};
			dst.pResource = dstResource;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst.SubresourceIndex = 0;
			getCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		}
		else
		{
			getCommandList()->CopyBufferRegion(dstResource, 0, uploadBuffer, 0, size);
		}

		// Transition destination resource to requested state
		Barrier::add(dstResource, D3D12_RESOURCE_STATE_COPY_DEST, targetState, getCommandList());

		getCommandList()->Close();
		ID3D12CommandList* lists[] = { getCommandList() };
		graphicsQueue->ExecuteCommandLists(1, lists);

		flushGraphicsQueue();

		uploadBuffer->Release();
	}

	// Get command list for current backbuffer index
	ID3D12GraphicsCommandList4* getCommandList()
	{
		unsigned int frameIndex = swapchain->GetCurrentBackBufferIndex();
		return graphicsCommandList[frameIndex];
	}

	// Begin frame: wait fence, transition backbuffer, bind RTV/DSV, clear
	void beginFrame()
	{
		unsigned int frameIndex = swapchain->GetCurrentBackBufferIndex();
		graphicsQueueFence[frameIndex].wait();
		D3D12_CPU_DESCRIPTOR_HANDLE renderTargetViewHandle = backbufferHeap->GetCPUDescriptorHandleForHeapStart();
		unsigned int renderTargetViewDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		renderTargetViewHandle.ptr += frameIndex * renderTargetViewDescriptorSize;
		resetCommandList();
		Barrier::add(backbuffers[frameIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET, getCommandList());
		getCommandList()->OMSetRenderTargets(1, &renderTargetViewHandle, FALSE, &dsvHandle);
		float color[4];
		color[0] = 0;
		color[1] = 0;
		color[2] = 1.0;
		color[3] = 1.0;
		getCommandList()->ClearRenderTargetView(renderTargetViewHandle, color, 0, NULL);
		getCommandList()->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
	}

	// Set pipeline-wide frame state before issuing draw calls
	void beginRenderPass()
	{
		unsigned int frameIndex = swapchain->GetCurrentBackBufferIndex();
		getCommandList()->RSSetViewports(1, &viewport);
		getCommandList()->RSSetScissorRects(1, &scissorRect);
		getCommandList()->SetGraphicsRootSignature(rootSignature);
		ID3D12DescriptorHeap* heaps[] = { srvHeap };
		getCommandList()->SetDescriptorHeaps(1, heaps);

		getCommandList()->SetGraphicsRootSignature(rootSignature);
	}

	// Current swapchain buffer index
	int frameIndex()
	{
		return swapchain->GetCurrentBackBufferIndex();
	}

	// End frame: transition backbuffer to present, submit, signal, present
	void finishFrame()
	{
		unsigned int frameIndex = swapchain->GetCurrentBackBufferIndex();
		Barrier::add(backbuffers[frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT, getCommandList());
		runCommandList();
		graphicsQueueFence[frameIndex].signal(graphicsQueue);
		swapchain->Present(1, 0);
	}

	// Force GPU to complete all outstanding graphics work
	void flushGraphicsQueue()
	{
		graphicsQueueFence[0].signal(graphicsQueue);
		graphicsQueueFence[0].wait();
	}

	//fog

	// CPU RTV handle for current backbuffer (requires valid rtvHeap)
	D3D12_CPU_DESCRIPTOR_HANDLE getBackBufferRTVHandle()
	{
		UINT rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
		rtvHandle.ptr += frameIndex() * rtvDescriptorSize;
		return rtvHandle;
	}

	// CPU DSV handle (heap start)
	D3D12_CPU_DESCRIPTOR_HANDLE getDSVHandle()
	{
		return dsvHeap->GetCPUDescriptorHandleForHeapStart();
	}

	// Bind backbuffer RTV + DSV to OM
	void setBackBufferRenderTarget()
	{
		UINT rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = backbufferHeap->GetCPUDescriptorHandleForHeapStart();
		rtvHandle.ptr += frameIndex() * rtvDescriptorSize;
		getCommandList()->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
	}

	// Bind backbuffer RTV only (no depth)
	void setBackBufferRenderTargetNoDepth()
	{
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = getBackBufferRTVHandle();
		getCommandList()->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
	}

	// Get screen width
	int getWidth() const { return width; }

	// Get screen height  
	int getHeight() const { return height; }

	// Bind SRV heap on current command list
	void setDefaultDescriptorHeaps()
	{
		ID3D12DescriptorHeap* heaps[] = { srvHeap };
		getCommandList()->SetDescriptorHeaps(1, heaps);
	}


	~Core()
	{
		for (int i = 0; i < 2; i++)
		{
			graphicsQueueFence[i].signal(graphicsQueue);
			graphicsQueueFence[i].wait();
		}
		rootSignature->Release();
		graphicsCommandList[0]->Release();
		graphicsCommandAllocator[0]->Release();
		graphicsCommandList[1]->Release();
		graphicsCommandAllocator[1]->Release();
		backbuffers[0]->Release();
		backbuffers[1]->Release();
		delete[] backbuffers;
		backbufferHeap->Release();
		dsv->Release();
		dsvHeap->Release();
		swapchain->Release();
		srvHeap->Release();
		computeQueue->Release();
		copyQueue->Release();
		graphicsQueue->Release();
		device->Release();
	}
};

template<typename T>
T use()
{
	// Per-type static instance helper
	static T t;
	return t;
}
