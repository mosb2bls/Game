#pragma once

#include <d3d12.h>
#include <d3dcompiler.h>
#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <vector>

#include "Core.h"

#pragma comment(lib, "dxguid.lib")

struct ConstantBufferVariable
{
	unsigned int offset;
	unsigned int size;
};

class ConstantBuffer
{
public:
	std::string name;
	std::map<std::string, ConstantBufferVariable> constantBufferData;
	ID3D12Resource* constantBuffer;
	unsigned char *buffer;
	unsigned int cbSizeInBytes;
	unsigned int numInstances;
	unsigned int offsetIndex;
	void init(Core* core, unsigned int sizeInBytes, unsigned int maxDrawCalls = 1024)
	{
		cbSizeInBytes = (sizeInBytes + 255) & ~255;
		unsigned int cbSizeInBytesAligned = cbSizeInBytes * maxDrawCalls;
		numInstances = maxDrawCalls;
		offsetIndex = 0;
		HRESULT hr;
		D3D12_HEAP_PROPERTIES heapprops;
		memset(&heapprops, 0, sizeof(D3D12_HEAP_PROPERTIES));
		heapprops.Type = D3D12_HEAP_TYPE_UPLOAD;
		heapprops.CreationNodeMask = 1;
		heapprops.VisibleNodeMask = 1;
		D3D12_RESOURCE_DESC cbDesc;
		memset(&cbDesc, 0, sizeof(D3D12_RESOURCE_DESC));
		cbDesc.Width = cbSizeInBytesAligned;
		cbDesc.Height = 1;
		cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		cbDesc.DepthOrArraySize = 1;
		cbDesc.MipLevels = 1;
		cbDesc.SampleDesc.Count = 1;
		cbDesc.SampleDesc.Quality = 0;
		cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		hr = core->device->CreateCommittedResource(&heapprops, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, __uuidof(ID3D12Resource), (void**)&constantBuffer);
		D3D12_RANGE readRange = {0, 0};
		hr = constantBuffer->Map(0, &readRange, (void**)&buffer);
	}
	void update(std::string name, void* data) // Data is immediatly visible
	{
		ConstantBufferVariable cbVariable = constantBufferData[name];
		unsigned int offset = offsetIndex * cbSizeInBytes;
		memcpy(&buffer[offset + cbVariable.offset], data, cbVariable.size);
	}
	D3D12_GPU_VIRTUAL_ADDRESS getGPUAddress() const
	{
		return (constantBuffer->GetGPUVirtualAddress() + (offsetIndex * cbSizeInBytes));
	}
	void next()
	{
		offsetIndex++;
		if (offsetIndex >= numInstances)
		{
			offsetIndex = 0;
		}
	}
	void free()
	{
		constantBuffer->Unmap(0, NULL);
		constantBuffer->Release();
	}
};

class Shader
{
public:
	ID3DBlob* ps;
	ID3DBlob* vs;
	std::vector<ConstantBuffer> psConstantBuffers;
	std::vector<ConstantBuffer> vsConstantBuffers;
	std::map<std::string, int> textureBindPoints;
	int hasLayout;
	void initConstantBuffers(Core *core, ID3DBlob* shader, std::vector<ConstantBuffer> &buffers)
	{
		ID3D12ShaderReflection *reflection;
		D3DReflect(shader->GetBufferPointer(), shader->GetBufferSize(), IID_PPV_ARGS(&reflection));
		D3D12_SHADER_DESC desc;
		reflection->GetDesc(&desc);
		for (int i = 0; i < desc.ConstantBuffers; i++)
		{
			ConstantBuffer buffer;
			ID3D12ShaderReflectionConstantBuffer* constantBuffer = reflection->GetConstantBufferByIndex(i);
			D3D12_SHADER_BUFFER_DESC cbDesc;
			constantBuffer->GetDesc(&cbDesc);
			buffer.name = cbDesc.Name;
			unsigned int totalSize = 0;
			for (int j = 0; j < cbDesc.Variables; j++)
			{
				ID3D12ShaderReflectionVariable* var = constantBuffer->GetVariableByIndex(j);
				D3D12_SHADER_VARIABLE_DESC vDesc;
				var->GetDesc(&vDesc);
				ConstantBufferVariable bufferVariable;
				bufferVariable.offset = vDesc.StartOffset;
				bufferVariable.size = vDesc.Size;
				buffer.constantBufferData.insert({vDesc.Name, bufferVariable});
				totalSize += bufferVariable.size;
			}
			buffer.init(core, totalSize);
			buffers.push_back(buffer);
		}
		for (int i = 0; i < desc.BoundResources; i++)
		{
			D3D12_SHADER_INPUT_BIND_DESC bindDesc;
			reflection->GetResourceBindingDesc(i, &bindDesc);
			if (bindDesc.Type == D3D_SIT_TEXTURE)
			{
				textureBindPoints.insert({bindDesc.Name, bindDesc.BindPoint});
			}
		}
		reflection->Release();
	}
	void loadPS(Core *core, std::string hlsl)
	{
		ID3DBlob* status;
		HRESULT hr = D3DCompile(hlsl.c_str(), strlen(hlsl.c_str()), NULL, NULL, NULL, "PS", "ps_5_0", 0, 0, &ps, &status);
		if (FAILED(hr))
		{
			printf("%s\n", (char*)status->GetBufferPointer());
			exit(0);
		}
		initConstantBuffers(core, ps, psConstantBuffers);
	}
	void loadVS(Core* core, std::string hlsl)
	{
		ID3DBlob* status;
		HRESULT hr = D3DCompile(hlsl.c_str(), strlen(hlsl.c_str()), NULL, NULL, NULL, "VS", "vs_5_0", 0, 0, &vs, &status);
		if (FAILED(hr))
		{
			printf("%s\n", (char*)status->GetBufferPointer());
			exit(0);
		}
		initConstantBuffers(core, vs, vsConstantBuffers);
	}
	void updateConstant(std::string constantBufferName, std::string variableName, void* data, std::vector<ConstantBuffer>& buffers)
	{
		for (int i = 0; i < buffers.size(); i++)
		{
			if (buffers[i].name == constantBufferName)
			{
				buffers[i].update(variableName, data);
				return;
			}
		}
	}
	void updateConstantVS(std::string constantBufferName, std::string variableName, void* data)
	{
		updateConstant(constantBufferName, variableName, data, vsConstantBuffers);
	}
	void updateConstantPS(std::string constantBufferName, std::string variableName, void* data)
	{
		updateConstant(constantBufferName, variableName, data, psConstantBuffers);
	}
	void apply(Core* core)
	{
		for (int i = 0; i < vsConstantBuffers.size(); i++)
		{
			core->getCommandList()->SetGraphicsRootConstantBufferView(0, vsConstantBuffers[i].getGPUAddress());
			vsConstantBuffers[i].next();
		}
		for (int i = 0; i < psConstantBuffers.size(); i++)
		{
			core->getCommandList()->SetGraphicsRootConstantBufferView(1, psConstantBuffers[i].getGPUAddress());
			psConstantBuffers[i].next();
		}
	}
	void free()
	{
		ps->Release();
		vs->Release();
		for (auto cb : psConstantBuffers)
		{
			cb.free();
		}
		for (auto cb : vsConstantBuffers)
		{
			cb.free();
		}
	}
};

class Shaders
{
public:
	std::map<std::string, Shader> shaders;
	std::string readFile(std::string filename)
	{
		std::ifstream file(filename);
		std::stringstream buffer;
		buffer << file.rdbuf();
		return buffer.str();
	}
	void load(Core* core, std::string shadername, std::string vsfilename, std::string psfilename)
	{
		std::map<std::string, Shader>::iterator it = shaders.find(shadername);
		if (it != shaders.end())
		{
			return;
		}
		Shader shader;
		shader.loadPS(core, readFile(psfilename));
		shader.loadVS(core, readFile(vsfilename));
		shaders.insert({shadername, shader});
	}
	void updateConstantVS(std::string name, std::string constantBufferName, std::string variableName, void* data)
	{
		shaders[name].updateConstantVS(constantBufferName, variableName, data);
	}
	void updateConstantPS(std::string name, std::string constantBufferName, std::string variableName, void* data)
	{
		shaders[name].updateConstantPS(constantBufferName, variableName, data);
	}
	Shader* find(std::string name)
	{
		return &shaders[name];
	}
	void apply(Core* core, std::string name)
	{
		shaders[name].apply(core);
	}
	~Shaders()
	{
		for (auto it = shaders.begin(); it != shaders.end(); )
		{
			it->second.free();
			shaders.erase(it++);
		}
	}
};