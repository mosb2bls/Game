#include "Core.h"
#include "Window.h"
#include "Timer.h"
#include "Maths.h"
#include "Shaders.h"
#include "Mesh.h"
#include "PSO.h"
#include "SkyDome.h"
#include "HeightmapTerrain.h"
#include "Gun.h"
#include "HybridGrassField.h"
#include "Rocks.h"
#include "AssetManager.h"
#include "Animation.h"
#include "modelState.h"
#include "Fog.h"
#include "LakeBottom.h"
#include "Tree.h"
#include "Lake.h"
#include "Crosshair.h"
#include "StartMenu.h"
#include "RandomGenerator.h"

#include <algorithm>
#include <Windows.h>

#define _CRT_SECURE_NO_WARNINGS

#define WIDTH  1920
#define HEIGHT 1080

static float deg2rad(float d) { return d * 3.1415926535f / 180.0f; }
static float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }

// ============================================================================
// GLOBAL POINTERS
// ============================================================================
Lake* g_lake = nullptr;

// ============================================================================
// HELPER: Convert VegetationItems to GrassInstances
// ============================================================================
std::vector<GrassInstance> convertToGrassInstances(
    const std::vector<VegetationItem>& items,
    int numGroups,
    int numTypesPerGroup)
{
    std::vector<GrassInstance> instances;
    instances.reserve(items.size());

    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> randPhase(0.0f, 6.28318f);

    int skippedCount = 0;

    for (const auto& item : items)
    {
        // Skip grass that's inside the lake
        if (g_lake != nullptr)
        {
            float margin = 2.0f;
            float dx = item.position.x - g_lake->config.center.x;
            float dz = item.position.z - g_lake->config.center.z;
            float distSq = dx * dx + dz * dz;
            float radiusWithMargin = g_lake->config.radius + margin;

            if (distSq < radiusWithMargin * radiusWithMargin)
            {
                skippedCount++;
                continue;
            }
        }

        GrassInstance inst;
        inst.position = item.position;
        inst.rotationY = item.rotationY;
        inst.scale = item.scale;
        inst.windPhase = randPhase(rng);

        if (numGroups > 0 && numTypesPerGroup > 0)
        {
            inst.groupIndex = item.typeIndex % numGroups;
            inst.typeIndex = (item.typeIndex / numGroups) % numTypesPerGroup;
        }
        else
        {
            inst.groupIndex = 0;
            inst.typeIndex = 0;
        }

        instances.push_back(inst);
    }

    if (skippedCount > 0)
    {
        std::cout << "[Grass] Skipped " << skippedCount << " grass instances inside lake area\n";
    }

    return instances;
}

// ============================================================================
// HELPER: Convert VegetationItems to RockInstances
// ============================================================================
std::vector<RockInstance> convertToRockInstances(const std::vector<VegetationItem>& items)
{
    std::vector<RockInstance> instances;
    instances.reserve(items.size());

    const float spawnExclusionRadius = 5.0f;
    int skippedCount = 0;

    for (const auto& item : items)
    {
        // Skip rocks near spawn point (0, 0)
        float distSq = item.position.x * item.position.x + item.position.z * item.position.z;
        if (distSq < spawnExclusionRadius * spawnExclusionRadius)
        {
            skippedCount++;
            continue;
        }

        RockInstance inst;
        inst.position = item.position;
        inst.rotationY = item.rotationY;
        inst.scale = item.scale;
        inst.typeIndex = item.typeIndex;
        inst.distanceToCamera = 0.0f;
        inst.lodLevel = 2;
        instances.push_back(inst);
    }

    if (skippedCount > 0)
    {
        std::cout << "[Rocks] Skipped " << skippedCount << " rocks near spawn point\n";
    }

    return instances;
}

// ============================================================================
// REFLECTION CALLBACK DATA
// ============================================================================
struct SceneRenderData
{
    Core* core;
    PSOManager* psos;
    Shaders* shaders;
    SkyDome* sky;
    HeightmapTerrain* terrain;
    Rocks* rocks;
    HybridGrassField* grass;
    Vec3 cameraPos;
    bool hasRocks;
    bool hasGrass;
};

void RenderSceneForReflection(void* userData, const Matrix& view, const Matrix& proj)
{
    SceneRenderData* data = (SceneRenderData*)userData;

    Matrix viewCopy = view;
    Matrix projCopy = proj;
    Matrix vp = viewCopy * projCopy;
    Matrix terrainW;

    data->core->setDefaultDescriptorHeaps();
    data->core->getCommandList()->SetGraphicsRootSignature(data->core->rootSignature);

    data->sky->draw(data->core, data->psos, data->shaders, vp, data->cameraPos);
    data->terrain->draw(data->core, data->psos, data->shaders, vp, terrainW);

    if (data->hasRocks)
        data->rocks->draw(data->core, data->psos, data->shaders, vp, data->cameraPos);
}

// ============================================================================
// COLLISION: Check collision with rocks
// ============================================================================
bool checkRockCollision(const Vec3& position, const std::vector<RockInstance>& rockInstances, float playerRadius)
{
    for (const auto& rock : rockInstances)
    {
        float rockRadius = rock.scale * 1.5f;

        float dx = position.x - rock.position.x;
        float dz = position.z - rock.position.z;
        float distSq = dx * dx + dz * dz;

        float minDist = playerRadius + rockRadius;

        if (distSq < minDist * minDist)
        {
            return true;
        }
    }
    return false;
}

// ============================================================================
// COLLISION: Check collision with lake (water)
// ============================================================================
bool checkLakeCollision(const Vec3& position, const Lake& lake, float playerRadius)
{
    float dx = position.x - lake.config.center.x;
    float dz = position.z - lake.config.center.z;
    float distSq = dx * dx + dz * dz;

    float waterEdge = lake.config.radius - playerRadius - 0.5f;

    if (distSq < waterEdge * waterEdge)
    {
        return true;
    }
    return false;
}


// ============================================================================
// MAIN
// ============================================================================
int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, int nCmdShow)
{
    // ========================================================================
    // CONSOLE OUTPUT
    // ========================================================================
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);

    // ========================================================================
    // WINDOW & CORE
    // ========================================================================
    Window window;
    window.create(WIDTH, HEIGHT, "The Game");

    Core core;
    core.init(window.hwnd, WIDTH, HEIGHT);

    Shaders shaders;
    PSOManager psos;

    // ========================================================================
    // DECLARE OBJECTS
    // ========================================================================
    Lake lake;
    g_lake = &lake;

    LakeBottom lakeBottom;
    StartMenu startMenu;
    Crosshair crosshair;
    Tree tree;

    // ========================================================================
    // FOG SYSTEM
    // ========================================================================
    VolumetricFog fog;
    fog.init(&core, WIDTH, HEIGHT);

    fog.config.density = 0.02f;
    fog.config.heightFalloff = 0.06f;
    fog.config.groundLevel = -5.0f;
    fog.config.maxHeight = 60.0f;

    fog.config.fogColor = Vec3(0.65f, 0.75f, 0.88f);
    fog.config.sunColor = Vec3(1.0f, 0.95f, 0.85f);
    fog.config.ambientColor = Vec3(0.4f, 0.5f, 0.6f);

    fog.config.sunDirection = Vec3(0.4f, 0.7f, -0.5f);
    fog.config.scattering = 0.6f;
    fog.config.mieG = 0.75f;

    fog.config.raymarchSteps = 24;
    fog.config.maxDistance = 150.0f;

    fog.config.windSpeed = 0.4f;
    fog.config.windDirection = Vec2(1.0f, 0.2f);

    fog.enabled = true;

    std::cout << "[Game] Fog system initialized\n";

    // ========================================================================
    // LOAD ASSETS
    // ========================================================================
    AssetManager assets;
    if (!assets.loadFromConfig(&core, "assets.cfg"))
    {
        std::string msg = "Failed to load assets.cfg";
        OutputDebugStringA(msg.c_str());
        MessageBoxA(nullptr, msg.c_str(), "Asset Load Error", MB_OK);
    }

    // ========================================================================
    // SKY
    // ========================================================================
    SkyDome sky;
    sky.init(&core, &psos, &shaders, 5000.0f);

    // ========================================================================
    // TERRAIN
    // ========================================================================
    HeightmapTerrain terrain;

    float terrainSizeX = 300.0f;
    float terrainSizeZ = 300.0f;

    bool terrainOK = terrain.init(
        &core, &psos, &shaders,
        "Assets/Heightmap/map2.png",
        512, 512,
        terrainSizeX, terrainSizeZ,
        40.0f, 0.0f,
        HeightmapTerrain::Format::PNG16
    );

    if (!terrainOK)
    {
        std::string msg = "Map not open";
        OutputDebugStringA(msg.c_str());
        MessageBoxA(nullptr, msg.c_str(), "Terrain Load Error", MB_OK);
        return 0;
    }

    // ========================================================================
    // LAKE
    // ========================================================================
    lake.config.center = Vec3(30.0f, 0.0f, 40.0f);
    lake.config.radius = 25.0f;

    float lakeCenterHeight = terrain.sampleHeightWorld(lake.config.center.x, lake.config.center.z);
    lake.config.waterLevel = lakeCenterHeight + 0.1f;

    std::cout << "[Lake] Water level: " << lake.config.waterLevel << "\n";

    lake.config.shallowColor = Vec3(0.0f, 0.2f, 0.5f);
    lake.config.deepColor = Vec3(0.0f, 0.05f, 0.15f);
    lake.config.transparency = 0.85f;
    lake.config.reflectionStrength = 0.6f;
    lake.config.fresnelBias = 0.02f;

    lake.config.waveSpeed = 0.6f;
    lake.config.waveScale = 0.3f;

    lake.config.reflectionStrength = 0.8f;
    lake.config.reflectionDistortion = 0.02f;

    lake.config.sunDirection = Vec3(0.4f, 0.7f, -0.5f);
    lake.config.sunColor = Vec3(1.0f, 0.95f, 0.8f);
    lake.config.specularPower = 256.0f;
    lake.config.specularIntensity = 1.5f;

    lake.config.radialSegments = 64;
    lake.config.ringSegments = 32;

    lake.init(&core, &shaders, &psos, WIDTH, HEIGHT);

    std::cout << "[Game] Lake initialized at (" << lake.config.center.x << ", "
        << lake.config.waterLevel << ", " << lake.config.center.z
        << ") with radius " << lake.config.radius << "\n";

    // ========================================================================
    // LAKE BOTTOM
    // ========================================================================
    lakeBottom.init(&core, &shaders, &psos,
        "Assets/Lake/ground.jpg",
        lake.config.center,
        lake.config.radius,
        lake.config.waterLevel,
        8.0f);

    // ========================================================================
    // TREE
    // ========================================================================
    float treeX = lake.config.center.x + lake.config.radius + 5.0f;
    float treeZ = lake.config.center.z;
    float treeY = terrain.sampleHeightWorld(treeX, treeZ);

    tree.init(&core, &shaders, &psos,
        "Assets/Tree/Ash_Tree_Full_01b.gem",
        "Assets/Tree/Ash_Tree_Full_01b.jpg",
        "Assets/Tree/Bark012_4K-JPG_Color.jpg",
        Vec3(treeX, treeY, treeZ),
        2.0f,
        0.0f);

    tree.trunkRadius = 0.3f;
    tree.trunkHeight = 4.0f;
    tree.trunkOffsetY = 0.0f;

    tree.shadowRadius = 4.0f;
    tree.shadowOpacity = 0.4f;

    // ========================================================================
    // VEGETATION GENERATION
    // ========================================================================
    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "   VEGETATION GENERATION SYSTEM\n";
    std::cout << "========================================\n\n";

    VegetationGenerator vegGen;
    VegetationConfig vegConfig;

    vegConfig.density = 1.0f;
    vegConfig.minPointSpacing = 1.5f;

    vegConfig.rockProbability = 0.12f;
    vegConfig.noiseInfluence = 0.5f;
    vegConfig.noiseScale = 0.018f;

    vegConfig.grassMinScale = 0.7f;
    vegConfig.grassMaxScale = 1.4f;
    vegConfig.grassRadius = 0.2f;

    vegConfig.grassCluster.probability = 0.7f;
    vegConfig.grassCluster.minItems = 6;
    vegConfig.grassCluster.maxItems = 18;
    vegConfig.grassCluster.radius = 4.0f;
    vegConfig.grassCluster.falloff = 1.2f;

    vegConfig.rockMinScale = 0.4f;
    vegConfig.rockMaxScale = 2.8f;
    vegConfig.rockRadius = 1.2f;

    vegConfig.rockCluster.probability = 0.45f;
    vegConfig.rockCluster.minItems = 2;
    vegConfig.rockCluster.maxItems = 7;
    vegConfig.rockCluster.radius = 6.0f;
    vegConfig.rockCluster.falloff = 2.5f;

    vegConfig.maxSlope = 40.0f;

    unsigned int seed = 42;

    std::cout << "[VegetationGenerator] Configuration:\n";
    std::cout << "  Total points: " << vegConfig.density << "\n";
    std::cout << "  Rock probability: " << (vegConfig.rockProbability * 100) << "%\n";
    std::cout << "  Grass cluster prob: " << (vegConfig.grassCluster.probability * 100) << "%\n";
    std::cout << "  Rock cluster prob: " << (vegConfig.rockCluster.probability * 100) << "%\n";
    std::cout << "  Seed: " << seed << "\n\n";

    vegGen.generate(&terrain, vegConfig, terrainSizeX, terrainSizeZ, seed);

    const auto& generatedRocks = vegGen.getRockItems();
    const auto& generatedGrass = vegGen.getGrassItems();

    std::cout << "\n[Game] Vegetation generation complete!\n";
    std::cout << "  Rocks generated: " << generatedRocks.size() << "\n";
    std::cout << "  Grass generated: " << generatedGrass.size() << "\n\n";

    // ========================================================================
    // ROCKS
    // ========================================================================
    Rocks rocks;
    bool hasRocks = false;
    std::vector<RockInstance> rockInstances;

    auto& rockSets = assets.getRockSets();
    if (!rockSets.empty() && !generatedRocks.empty())
    {
        auto& rockSet = rockSets[0];

        rockInstances = convertToRockInstances(generatedRocks);

        rocks.terrainSizeX = terrainSizeX;
        rocks.terrainSizeZ = terrainSizeZ;
        rocks.initWithInstances(&core, &psos, &shaders, &terrain,
            rockSet.modelPaths,
            rockSet.texturePaths,
            rockInstances,
            100.0f,
            32.0f
        );

        rocks.rockColor = Vec4(0.75f, 0.72f, 0.68f, 1.0f);
        rocks.lodDistanceHigh = 25.0f;
        rocks.lodDistanceMedium = 60.0f;

        hasRocks = true;
        std::cout << "[Game] Rocks initialized: " << rockInstances.size() << " instances\n";
    }
    else
    {
        std::cout << "[Game] No rocks to initialize\n";
    }

    // ========================================================================
    // GRASS
    // ========================================================================
    HybridGrassField grassField;
    bool hasGrass = false;

    auto grassConfigs = assets.getGrassGroupConfigs();
    if (!grassConfigs.empty() && !generatedGrass.empty())
    {
        int numGroups = (int)grassConfigs.size();
        int avgTypesPerGroup = 0;
        for (const auto& group : grassConfigs)
        {
            avgTypesPerGroup += (int)group.types.size();
        }
        avgTypesPerGroup = numGroups > 0 ? avgTypesPerGroup / numGroups : 1;

        std::vector<GrassInstance> grassInstances = convertToGrassInstances(
            generatedGrass, numGroups, avgTypesPerGroup);

        grassField.terrainSizeX = terrainSizeX;
        grassField.terrainSizeZ = terrainSizeZ;
        grassField.initWithInstances(&core, &psos, &shaders, &terrain,
            grassConfigs,
            grassInstances,
            50.0f,
            16.0f
        );

        grassField.colorTop = Vec4(100.0f/225.0f, 125.0f / 225.0f, 31.0f / 225.0f, 1.0f);
        grassField.colorBottom = Vec4(100.0f / 225.0f, 125.0f / 225.0f, 31.0f / 225.0f, 1.0f);

        grassField.windDirection = Vec2(1.0f, 0.3f);
        grassField.windStrength = 0.0f;

        hasGrass = true;
        std::cout << "[Game] Grass initialized: " << grassInstances.size() << " instances\n";
    }
    else
    {
        std::cout << "[Game] No grass to initialize\n";
    }

    std::cout << "\n========================================\n";
    std::cout << "   VEGETATION SETUP COMPLETE\n";
    std::cout << "========================================\n\n";

    // ========================================================================
    // GUN
    // ========================================================================
    Gun gunModel;
    gunModel.load(&core,
        "Assets/Models/AutomaticCarbine.gem",
        "Assets/Models/Textures/gun.png",
        &psos, &shaders);

    AnimationInstance gunAnim;
    gunAnim.init(&gunModel.animation, 0);

    // ========================================================================
    // CAMERA & PLAYER STATE
    // ========================================================================
    Vec3 camPos(0.0f, 1.7f, 0.0f);
    const float eyeHeight = 1.7f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    const float moveSpeed = 8.0f;
    const float sprintMultiplier = 1.5f;
    const float mouseSens = 0.0025f;
    const float pitchLimit = 1.45f;
    const float playerRadius = 0.5f;

    while (ShowCursor(FALSE) >= 0);
    window.useMouseClip = true;

    modelState modelState;
    modelState.idleClip = "04 idle";
    modelState.walkClip = "07 walk";
    modelState.fireClip = "08 fire";
    modelState.reloadClip = "17 reload";
    modelState.shotsPerSecond = 12.0f;
    modelState.fireAnimRate = 3.0f;

    auto getCenterScreen = [&]() {
        RECT rc{};
        GetClientRect(window.hwnd, &rc);
        POINT c{ (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 };
        ClientToScreen(window.hwnd, &c);
        return c;
        };

    POINT center = getCenterScreen();
    SetCursorPos(center.x, center.y);

    // Viewmodel placement
    float gunX = 0.08f;
    float gunY = 0.0f;
    float gunZ = 0.0f;
    Vec3 gunScale(0.01f, 0.01f, 0.01f);
    const float PI = 3.141592654f;
    float modelRotX = 0.0f;
    float modelRotY = +PI * 1.01f;
    float modelRotZ = 0.0f;

    // ========================================================================
    // REFLECTION CALLBACK DATA
    // ========================================================================
    SceneRenderData sceneData;
    sceneData.core = &core;
    sceneData.psos = &psos;
    sceneData.shaders = &shaders;
    sceneData.sky = &sky;
    sceneData.terrain = &terrain;
    sceneData.rocks = &rocks;
    sceneData.grass = &grassField;
    sceneData.hasRocks = hasRocks;
    sceneData.hasGrass = hasGrass;

    // ========================================================================
    // UI: START MENU & CROSSHAIR
    // ========================================================================
    startMenu.init(&core, &shaders, &psos, WIDTH, HEIGHT);
    crosshair.init(&core, &shaders, &psos, WIDTH, HEIGHT);

    crosshair.size = 15.0f;
    crosshair.thickness = 2.0f;
    crosshair.gap = 5.0f;

    // Show cursor for menu
    ShowCursor(TRUE);

    // ========================================================================
    // TIMING & GAME STATE
    // ========================================================================
    Timer timer;
    float totalTime = 0.0f;
    bool gameStarted = false;

    std::cout << "========================================\n";
    std::cout << "   GAME RUNNING - Press ESC to exit\n";
    std::cout << "========================================\n\n";

    // ========================================================================
    // MAIN GAME LOOP
    // ========================================================================
    while (1)
    {
        core.beginFrame();

        float dt = timer.dt();
        dt = std::min(dt, 0.05f);

        window.checkInput();
        if (window.keys[VK_ESCAPE]) break;

        // ====================================================================
        // START MENU
        // ====================================================================
        if (!gameStarted)
        {
            if (window.keys[VK_ESCAPE])
            {
                break;
            }

            bool startKeyPressed = window.keys[VK_RETURN] || window.keys[VK_SPACE];
            if (startKeyPressed)
            {
                gameStarted = true;
                while (ShowCursor(FALSE) >= 0);
                window.useMouseClip = true;
                center = getCenterScreen();
                SetCursorPos(center.x, center.y);

                std::cout << "[Game] Starting gameplay!\n";
                core.finishFrame();
                continue;
            }

            core.beginRenderPass();
            startMenu.draw(&core, &psos, &shaders);
            core.finishFrame();

            continue;
        }

        // ====================================================================
        // FOG TOGGLE
        // ====================================================================
        static bool togglePressed = false;

        if (window.keys['T'] && !togglePressed) {
            fog.enabled = !fog.enabled;
        }
        togglePressed = window.keys['T'];

        if (window.keys['G'] && !togglePressed) {
            fog.config.density = std::min(fog.config.density + 0.005f, 0.1f);
        }

        if (window.keys['H'] && !togglePressed) {
            fog.config.density = std::max(fog.config.density - 0.005f, 0.001f);
        }

        // ====================================================================
        // MOUSE LOOK
        // ====================================================================
        center = getCenterScreen();

        POINT cur{};
        GetCursorPos(&cur);
        float dx = float(cur.x - center.x);
        float dy = float(cur.y - center.y);
        SetCursorPos(center.x, center.y);

        yaw += dx * mouseSens;
        pitch -= dy * mouseSens;
        pitch = clampf(pitch, -pitchLimit, +pitchLimit);

        Vec3 forward(
            sinf(yaw) * cosf(pitch),
            sinf(pitch),
            cosf(yaw) * cosf(pitch)
        );
        forward = forward.normalize();

        Vec3 forwardFlat(forward.x, 0.0f, forward.z);
        if (forwardFlat.length() > 0.0001f)
            forwardFlat = forwardFlat.normalize();

        Vec3 worldUp(0, 1, 0);
        Vec3 rightFlat = Cross(worldUp, forwardFlat).normalize();

        // ====================================================================
        // UPDATE SYSTEMS
        // ====================================================================
        float groundY = terrain.sampleHeightWorld(camPos.x, camPos.z);

        if (hasGrass)
            grassField.update(dt);

        if (hasRocks)
            rocks.update(camPos);

        // ====================================================================
        // PLAYER MOVEMENT WITH COLLISION
        // ====================================================================
        Vec3 newPos = camPos;

        // Sprint: hold Shift to move 1.5x faster
        bool sprinting = (window.keys[VK_SHIFT] != 0);
        float currentSpeed = sprinting ? (moveSpeed * sprintMultiplier) : moveSpeed;

        if (window.keys['W']) newPos = newPos + forwardFlat * (currentSpeed * dt);
        if (window.keys['S']) newPos = newPos - forwardFlat * (currentSpeed * dt);
        if (window.keys['A']) newPos = newPos - rightFlat * (currentSpeed * dt);
        if (window.keys['D']) newPos = newPos + rightFlat * (currentSpeed * dt);

        bool canMove = true;

        if (checkLakeCollision(newPos, lake, playerRadius))
        {
            canMove = false;
        }

        if (canMove && hasRocks && checkRockCollision(newPos, rockInstances, playerRadius))
        {
            canMove = false;
        }

        float halfX = terrainSizeX * 0.5f;
        float halfZ = terrainSizeZ * 0.5f;
        if (newPos.x < -halfX + 1.0f) newPos.x = -halfX + 1.0f;
        if (newPos.x > halfX - 1.0f) newPos.x = halfX - 1.0f;
        if (newPos.z < -halfZ + 1.0f) newPos.z = -halfZ + 1.0f;
        if (newPos.z > halfZ - 1.0f) newPos.z = halfZ - 1.0f;

        if (canMove)
        {
            camPos = newPos;
        }
        else
        {
            // Sliding collision
            Vec3 newPosX = camPos;
            Vec3 newPosZ = camPos;

            if (window.keys['W'] || window.keys['S'])
            {
                Vec3 moveDir = window.keys['W'] ? forwardFlat : (forwardFlat * -1.0f);
                newPosX.x = camPos.x + moveDir.x * currentSpeed * dt;
                newPosZ.z = camPos.z + moveDir.z * currentSpeed * dt;
            }
            if (window.keys['A'] || window.keys['D'])
            {
                Vec3 moveDir = window.keys['D'] ? rightFlat : (rightFlat * -1.0f);
                newPosX.x = camPos.x + moveDir.x * currentSpeed * dt;
                newPosZ.z = camPos.z + moveDir.z * currentSpeed * dt;
            }

            bool canMoveX = true;
            if (checkLakeCollision(newPosX, lake, playerRadius)) canMoveX = false;
            if (canMoveX && hasRocks && checkRockCollision(newPosX, rockInstances, playerRadius)) canMoveX = false;
            if (newPosX.x < 1.0f || newPosX.x > terrainSizeX - 1.0f) canMoveX = false;

            bool canMoveZ = true;
            if (checkLakeCollision(newPosZ, lake, playerRadius)) canMoveZ = false;
            if (canMoveZ && hasRocks && checkRockCollision(newPosZ, rockInstances, playerRadius)) canMoveZ = false;
            if (newPosZ.z < 1.0f || newPosZ.z > terrainSizeZ - 1.0f) canMoveZ = false;

            if (canMoveX) camPos.x = newPosX.x;
            if (canMoveZ) camPos.z = newPosZ.z;
        }

        camPos.y = groundY + eyeHeight + 5.0f;

        // ====================================================================
        // UPDATE GUN STATE (before matrices so we can apply zoom offset)
        // ====================================================================
        modelState.update(window, gunAnim, dt);
        modelState.getGunOffset(gunX, gunY, gunZ, modelRotY);

        // ====================================================================
        // MATRICES
        // ====================================================================
        // Apply camera zoom offset: move camera forward when ADS
        float zoomOffset = modelState.getCameraZoomOffset();
        Vec3 renderCamPos = camPos + forward * zoomOffset;

        float aspect = (float)WIDTH / (float)HEIGHT;
        Matrix pWorld = Matrix::perspective(0.01f, 10000.0f, aspect, 60.0f);
        Matrix vWorld = Matrix::lookAt(renderCamPos, renderCamPos + forward, worldUp);
        Matrix vpWorld = vWorld * pWorld;

        core.beginRenderPass();

        // ====================================================================
        // LAKE REFLECTION PASS
        // ====================================================================
        sceneData.cameraPos = renderCamPos;
        sceneData.hasRocks = hasRocks;
        sceneData.hasGrass = hasGrass;

        lake.beginReflectionPass(vWorld, pWorld, renderCamPos, RenderSceneForReflection, &sceneData);

        core.setBackBufferRenderTarget();
        core.setDefaultDescriptorHeaps();
        core.getCommandList()->SetGraphicsRootSignature(core.rootSignature);

        D3D12_VIEWPORT vp = { 0, 0, (float)WIDTH, (float)HEIGHT, 0, 1 };
        D3D12_RECT scissor = { 0, 0, (LONG)WIDTH, (LONG)HEIGHT };
        core.getCommandList()->RSSetViewports(1, &vp);
        core.getCommandList()->RSSetScissorRects(1, &scissor);

        // ====================================================================
        // FOG CAPTURE BEGIN
        // ====================================================================
        if (fog.enabled)
        {
            fog.beginSceneCapture();
        }

        // ====================================================================
        // DRAW WORLD
        // ====================================================================
        sky.draw(&core, &psos, &shaders, vpWorld, renderCamPos);

        Matrix terrainW;
        terrain.draw(&core, &psos, &shaders, vpWorld, terrainW);

        if (hasRocks)
            rocks.draw(&core, &psos, &shaders, vpWorld, renderCamPos);

        if (hasGrass)
            grassField.draw(&core, &psos, &shaders, vpWorld, renderCamPos);

        lakeBottom.draw(&core, &psos, &shaders, vpWorld);

        tree.draw(&core, &psos, &shaders, vpWorld);

        lake.render(&core, &psos, &shaders, vpWorld, renderCamPos, totalTime);

        // ====================================================================
        // FOG APPLY
        // ====================================================================
        totalTime += dt;
        if (fog.enabled)
        {
            fog.endSceneAndApplyFog(vWorld, pWorld, renderCamPos, totalTime);

            core.setBackBufferRenderTarget();
            core.setDefaultDescriptorHeaps();
            core.getCommandList()->SetGraphicsRootSignature(core.rootSignature);
        }

        // ====================================================================
        // DRAW GUN
        // ====================================================================
        Matrix pGun = Matrix::perspective(0.001f, 1000.0f, aspect, 60.0f);
        Matrix vpGun = pGun;
        Matrix S = Matrix::scaling(gunScale);
        Matrix R = Matrix::rotateZ(modelRotZ) * Matrix::rotateY(modelRotY) * Matrix::rotateX(modelRotX);
        Matrix T = Matrix::translation(Vec3(gunX, gunY, gunZ));
        Matrix Wgun = S * R * T;
        gunModel.draw(&core, &psos, &shaders, &gunAnim, vpGun, Wgun);

        // ====================================================================
        // DRAW CROSSHAIR
        // ====================================================================
        crosshair.draw(&core, &psos, &shaders);

        core.finishFrame();
    }

    // ========================================================================
    // CLEANUP
    // ========================================================================
    g_lake = nullptr;
    core.flushGraphicsQueue();

    std::cout << "\n========================================\n";
    std::cout << "   GAME ENDED\n";
    std::cout << "========================================\n";

    return 0;
}