#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ps4::core {

enum class DisplayMode : int {
    Windowed = 0,
    Borderless = 1,
    ExclusiveFullscreen = 2,
};

enum class AspectMode : int {
    Preserve16x9 = 0,
    IntegerScale = 1,
};

enum class QualityPreset : int {
    Low = 0,
    Medium = 1,
    High = 2,
    Ultra = 3,
    Custom = 4,
};

enum class TextureFilterMode : int {
    PixelPoint = 0,
    SharpBilinear = 1,
    Bilinear = 2,
    Anisotropic = 3,
};

enum class VSyncMode : int {
    Off = 0,
    On = 1,
    Adaptive = 2,
};

struct PlayerKeyBinds {
    int up = 0;
    int down = 0;
    int left = 0;
    int right = 0;
    int jump = 0;
    int attack = 0;
    int back = 0;
    int start = 0;

    // Optional secondary bindings (e.g. Arrow keys on Player 1)
    int altUp = 0;
    int altDown = 0;
    int altLeft = 0;
    int altRight = 0;
    int altJump = 0;
    int altAttack = 0;
};

struct Settings {
    // Paths
    std::string assetPath;

    // Display
    DisplayMode displayMode = DisplayMode::Windowed;
    uint32_t displayWidth = 1920;
    uint32_t displayHeight = 1080;
    int monitorIndex = 0;
    AspectMode aspectMode = AspectMode::Preserve16x9;

    // Graphics
    QualityPreset qualityPreset = QualityPreset::High;
    float renderScale = 1.0f;  // 0.5 .. 2.0 (internal RT multiplier over 1920x1080)
    TextureFilterMode textureFilter = TextureFilterMode::PixelPoint;
    int anisotropy = 1;        // 1, 2, 4, 8, 16
    bool postScanlines = false;
    VSyncMode vsync = VSyncMode::On;
    int swapBuffers = 2;       // 2 or 3
    int presentationFpsLimit = 60;  // 0 = Unlimited, 30, 60, 120, 144, or custom (simulation stays 60 Hz)

    // Audio
    float masterVolume = 1.0f;
    float musicVolume = 0.85f;
    float sfxVolume = 1.0f;
    bool mute = false;
    int audioDeviceId = -1;    // -1 = Windows default (WAVE_MAPPER)

    // Input
    PlayerKeyBinds p1Keys;
    PlayerKeyBinds p2Keys;
    int p1Controller = 0;      // -1 = disabled, 0..3 = XInput index
    int p2Controller = 1;
    bool vibration = true;

    // Online
    std::string playerName = "Fencer";
    std::string region = "us-east";
    std::string lobbyServerUrl = "https://lobby.japer.me";
    int inputDelayFrames = 2;  // 1 .. 12; minimum when automatic calibration is enabled.
    bool autoInputDelay = true; // Negotiate latency/jitter allowance before frame zero.

    // General / Diagnostics
    bool launchDirectly = false;
    bool showFps = false;
    bool showFrameGraph = false;
    bool showNetStats = true;
};

struct DisplayModeInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refreshHz = 60;
};

struct MonitorInfo {
    int index = 0;
    std::string name;
    int left = 0;
    int top = 0;
    uint32_t width = 1920;
    uint32_t height = 1080;
    bool primary = false;
    std::vector<DisplayModeInfo> modes;
};

struct AudioDeviceInfo {
    int id = -1;
    std::string name;
};

struct KeyConflict {
    std::string actionA;
    std::string actionB;
    int vk = 0;
};

// Unicode filesystem helpers
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& w);
std::FILE* FopenUtf8(const std::string& path, const char* mode);
bool FileExistsUtf8(const std::string& path);
bool DirExistsUtf8(const std::string& path);
bool MakeDirsUtf8(const std::string& dirPath);
std::string ExeDir();

// Writable user data directory (portable if writable & portable, else %LOCALAPPDATA%\Nidhogg)
void SetUserDirOverride(const std::string& dir);
const std::string& UserDir();
std::string ConfigFilePath();
std::string SavesDirPath();
std::string LogsDirPath();

// Settings lifecycle
Settings MakeDefaultSettings();
void ApplyQualityPreset(Settings& s, QualityPreset preset);
Settings& CurrentSettings();
bool LoadSettings(Settings& out, const std::string& path = {});
bool SaveSettings(const Settings& s, const std::string& path = {});
void ResetSettingsToDefaults();

// Hardware enumeration & input validation
std::vector<MonitorInfo> EnumerateMonitors();
std::vector<AudioDeviceInfo> EnumerateAudioDevices();
std::vector<KeyConflict> DetectKeyConflicts(const Settings& s);
const char* VirtualKeyName(int vk);

// Asset discovery & validation (accepts CUSA00920 root or folder with eboot.bin/nidhogg.elf + Data/)
struct AssetValidation {
    bool valid = false;
    std::string rootDir;
    std::string executablePath;  // eboot.bin or nidhogg.elf
    std::string dataDir;
    std::vector<std::string> missingFiles;
    std::string summary;
};

AssetValidation ValidateAssetsAt(const std::string& candidateDir);
AssetValidation AutoDiscoverAssets(const Settings& s, const char* cliArg = nullptr);
bool PromptForAssetDirectoryGui(void* parentHwnd, std::string& outSelectedDir);

}  // namespace ps4::core
