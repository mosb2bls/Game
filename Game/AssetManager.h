#pragma once

#include "Core.h"
#include "Mesh.h"
#include "GEMLoader.h"
#include "HybridGrassField.h"  // Include for GrassGroupConfig and GrassTypeConfig
#include <map>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>

struct ModelAsset
{
    std::string name;          // Lookup key used in code/config
    std::string path;          // Disk path to .gem
    Mesh* mesh = nullptr;      // Loaded mesh (owned by AssetManager)
};

struct TextureAsset
{
    std::string name;          // Lookup key used in code/config
    std::string path;          // Disk path to image
    Texture texture;           // Loaded GPU texture + SRV handle
};

struct GrassGroupAsset
{
    std::string groupName;                 // Group identifier
    float groupWeight;                     // Group selection weight
    std::vector<std::string> typeNames;    // Per-type name
    std::vector<std::string> modelPaths;   // Per-type model path
    std::vector<std::string> texturePaths; // Per-type texture path
    std::vector<float> typeWeights;        // Per-type selection weight
};

struct RockSetAsset
{
    std::vector<std::string> names;        // Rock instance names
    std::vector<std::string> modelPaths;   // Per-rock model path
    std::vector<std::string> texturePaths; // Per-rock texture path
};

class AssetManager
{
public:
    // Parse config, then load all referenced models/textures into GPU-ready assets
    bool loadFromConfig(Core* core, const std::string& configFilePath)
    {
        std::cout << "[AssetManager] Loading assets from: " << configFilePath << "\n";

        std::ifstream file(configFilePath);
        if (!file.is_open())
        {
            std::cout << "[AssetManager] ERROR: Could not open config file!\n";
            return false;
        }

        std::string line;
        std::string currentSection = "";

        while (std::getline(file, line))
        {
            // Strip inline // comments before parsing
            size_t commentPos = line.find("//");
            if (commentPos != std::string::npos)
                line = line.substr(0, commentPos);

            line = trim(line);
            if (line.empty()) continue;

            // Section header: [MODELS] / [TEXTURES] / [GRASS_GROUPS] / [ROCKS]
            if (line[0] == '[' && line[line.length() - 1] == ']')
            {
                currentSection = line.substr(1, line.length() - 2);
                continue;
            }

            // Dispatch section-specific parsing
            if (currentSection == "MODELS")
                parseModel(line);
            else if (currentSection == "TEXTURES")
                parseTexture(line);
            else if (currentSection == "GRASS_GROUPS")
                parseGrassGroup(file, line);
            else if (currentSection == "ROCKS")
                parseRockSet(file, line);
        }

        file.close();

        // Load all model configs into Mesh objects
        std::cout << "[AssetManager] Loading " << modelConfigs.size() << " models...\n";
        for (auto& config : modelConfigs)
        {
            config.mesh = loadMesh(core, config.path);
            if (config.mesh)
                loadedModels[config.name] = config.mesh;
        }

        // Load all texture configs into GPU textures + SRVs
        std::cout << "[AssetManager] Loading " << textureConfigs.size() << " textures...\n";
        for (auto& config : textureConfigs)
        {
            config.texture = core->loadTexture(config.path);
            loadedTextures[config.name] = config.texture;
        }

        std::cout << "[AssetManager] Assets loaded successfully!\n";
        std::cout << "  Models: " << loadedModels.size() << "\n";
        std::cout << "  Textures: " << loadedTextures.size() << "\n";
        std::cout << "  Grass Groups: " << grassGroups.size() << "\n";
        std::cout << "  Rock Sets: " << rockSets.size() << "\n";

        return true;
    }

    // Lookup loaded mesh by config name
    Mesh* getModel(const std::string& name)
    {
        auto it = loadedModels.find(name);
        return (it != loadedModels.end()) ? it->second : nullptr;
    }

    // Lookup loaded texture by config name
    Texture getTexture(const std::string& name)
    {
        auto it = loadedTextures.find(name);
        return (it != loadedTextures.end()) ? it->second : Texture();
    }

    // Raw parsed grass groups (from config)
    std::vector<GrassGroupAsset>& getGrassGroups() { return grassGroups; }

    // Raw parsed rock sets (from config)
    std::vector<RockSetAsset>& getRockSets() { return rockSets; }

    // Convert parsed grass groups into HybridGrassField runtime config objects
    std::vector<GrassGroupConfig> getGrassGroupConfigs()
    {
        std::vector<GrassGroupConfig> configs;

        for (auto& group : grassGroups)
        {
            GrassGroupConfig config;
            config.groupName = group.groupName;
            config.groupWeight = group.groupWeight;

            for (size_t i = 0; i < group.typeNames.size(); i++)
            {
                GrassTypeConfig typeConfig;
                typeConfig.name = group.typeNames[i];
                typeConfig.modelPath = group.modelPaths[i];
                typeConfig.texturePath = group.texturePaths[i];
                typeConfig.weight = group.typeWeights[i];

                config.types.push_back(typeConfig);
            }

            configs.push_back(config);
        }

        return configs;
    }

    ~AssetManager()
    {
        // Free meshes allocated in loadMesh()
        for (auto& pair : loadedModels)
        {
            if (pair.second)
                delete pair.second;
        }
    }

private:
    std::vector<ModelAsset> modelConfigs;       // Model entries parsed from config
    std::vector<TextureAsset> textureConfigs;   // Texture entries parsed from config
    std::vector<GrassGroupAsset> grassGroups;   // Grass group blocks parsed from config
    std::vector<RockSetAsset> rockSets;         // Rock set blocks parsed from config

    std::map<std::string, Mesh*> loadedModels;  // Loaded meshes by name
    std::map<std::string, Texture> loadedTextures; // Loaded textures by name

    // Trim whitespace from both ends
    std::string trim(const std::string& str)
    {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, last - first + 1);
    }

    // Split string by delimiter and trim each token
    std::vector<std::string> split(const std::string& str, char delimiter)
    {
        std::vector<std::string> tokens;
        std::stringstream ss(str);
        std::string token;
        while (std::getline(ss, token, delimiter))
        {
            tokens.push_back(trim(token));
        }
        return tokens;
    }

    // Parse: ModelName = path/to/model.gem
    void parseModel(const std::string& line)
    {
        auto parts = split(line, '=');
        if (parts.size() == 2)
        {
            ModelAsset asset;
            asset.name = parts[0];
            asset.path = parts[1];
            modelConfigs.push_back(asset);
        }
    }

    // Parse: TextureName = path/to/texture.png
    void parseTexture(const std::string& line)
    {
        auto parts = split(line, '=');
        if (parts.size() == 2)
        {
            TextureAsset asset;
            asset.name = parts[0];
            asset.path = parts[1];
            textureConfigs.push_back(asset);
        }
    }

    // Parse a grass group block until END
    void parseGrassGroup(std::ifstream& file, const std::string& firstLine)
    {
        auto parts = split(firstLine, ',');
        if (parts.size() < 2) return;

        GrassGroupAsset group;
        group.groupName = parts[0];
        group.groupWeight = std::stof(parts[1]);

        std::string line;
        while (std::getline(file, line))
        {
            line = trim(line);
            if (line.empty()) continue;
            if (line == "END") break;

            auto typeParts = split(line, ',');
            if (typeParts.size() >= 4)
            {
                group.typeNames.push_back(typeParts[0]);
                group.modelPaths.push_back(typeParts[1]);
                group.texturePaths.push_back(typeParts[2]);
                group.typeWeights.push_back(std::stof(typeParts[3]));
            }
        }

        grassGroups.push_back(group);
    }

    // Parse rock entries until END
    void parseRockSet(std::ifstream& file, const std::string& firstLine)
    {
        RockSetAsset rockSet;

        std::string line = firstLine;
        do
        {
            line = trim(line);
            if (line.empty()) continue;
            if (line == "END") break;

            auto parts = split(line, ',');
            if (parts.size() >= 3)
            {
                rockSet.names.push_back(parts[0]);
                rockSet.modelPaths.push_back(parts[1]);
                rockSet.texturePaths.push_back(parts[2]);
            }
        } while (std::getline(file, line));

        if (!rockSet.names.empty())
        {
            rockSets.push_back(rockSet);
        }
    }

    // Load first GEM mesh into Mesh (STATIC_VERTEX + indices)
    Mesh* loadMesh(Core* core, const std::string& path)
    {
        GEMLoader::GEMModelLoader loader;
        std::vector<GEMLoader::GEMMesh> gemmeshes;
        loader.load(path, gemmeshes);

        // Fail fast if file loads but produces no meshes
        if (gemmeshes.empty())
        {
            std::cout << "[AssetManager] WARNING: Failed to load " << path << "\n";
            return nullptr;
        }

        Mesh* mesh = new Mesh();

        // Copy GEM static vertices into engine STATIC_VERTEX format
        std::vector<STATIC_VERTEX> vertices;
        for (auto& v : gemmeshes[0].verticesStatic)
        {
            STATIC_VERTEX vert;
            memcpy(&vert, &v, sizeof(STATIC_VERTEX));
            vertices.push_back(vert);
        }

        // Init GPU buffers for the mesh
        mesh->init(core, vertices, gemmeshes[0].indices);
        return mesh;
    }
};
