#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <android/keycodes.h> 
#include <dlfcn.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <atomic> 
#include <thread>
#include <vector>
#include <string>
#include <mutex>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <cerrno>
#include <math.h>
#include <algorithm>
#include <math.h>

#include "Substrate.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"

// Button Texture Data
#include "buton_texture.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define LOG_TAG "MultiplayerMod"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define DISCOVERY_PORT 8888
#define TCP_SYNC_PORT 8889

// ========================================================================
// GLOBAL VARIABLES
// ========================================================================
JavaVM* g_GlobalJavaVM = nullptr; 

GLuint g_MultiplayerButtonTexture = 0;
GLuint g_GameMenuBackgroundTexture = 0;
bool g_ImGuiInitialized = false;
bool g_TextureLoaded = false;
bool g_GameMenuBackgroundLoaded = false;
bool g_IsMultiplayerMenuActive = false;

int g_ButtonOrigWidth = 0;
int g_ButtonOrigHeight = 0;
int g_GameMenuBackgroundWidth = 0;
int g_GameMenuBackgroundHeight = 0;

std::atomic<float> g_TouchX(0.0f);
std::atomic<float> g_TouchY(0.0f);
std::atomic<bool> g_TouchDown(false);

static bool g_IsEatingTouch = false;
static std::atomic<bool> g_AndroidKeyboardOpen(false);

static ImFont* g_GameUIFont = nullptr;
static bool g_GameUIFontInitialized = false;

// USER NICKNAME
char g_Nickname[32] = "Player"; 

enum ActiveMenuType {
    MENU_NONE = 0,
    MENU_SETTINGS = 1,
    MENU_SAVELOAD = 2,
    MENU_INGAME = 3
};
ActiveMenuType g_CurrentMenu = MENU_NONE;

struct PeerDevice { std::string ip; std::string name; };
std::vector<PeerDevice> g_DiscoveredPeers;
std::mutex g_PeerMutex;
std::atomic<bool> g_IsSearching(false);

// POINTER MANAGEMENT
uintptr_t g_EngineInstance = 0; 
uintptr_t g_MenuInstance = 0;
uintptr_t g_StartMenuInstance = 0;
uintptr_t g_HUDInstance = 0;

std::atomic<bool> g_IsHost(false);
std::atomic<bool> g_IsClient(false);
std::atomic<bool> g_IsConnected(false);

// SOCKETS
int g_TcpServerFd = -1; 
int g_TcpSocket = -1;   
std::string g_ConnectedStatus = "Not Connected";

// CHAT SYSTEM
std::vector<std::string> g_ChatMessages;
std::mutex g_ChatMutex;

std::vector<std::string> g_OutgoingChats;
std::mutex g_OutgoingChatMutex;

// ========================================================================
// JNI_OnLoad
// ========================================================================
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_GlobalJavaVM = vm;
    return JNI_VERSION_1_6;
}

// ========================================================================
// NATIVE ANDROID TOAST NOTIFICATION (JNI)
// ========================================================================
void ShowNativeToast(std::string message) {
    std::thread([message]() {
        if (g_GlobalJavaVM == nullptr) return;
        JNIEnv* env = nullptr;
        if (g_GlobalJavaVM->AttachCurrentThread(&env, nullptr) != 0) return;

        jclass looperClass = env->FindClass("android/os/Looper");
        jmethodID myLooper = env->GetStaticMethodID(looperClass, "myLooper", "()Landroid/os/Looper;");
        jobject looper = env->CallStaticObjectMethod(looperClass, myLooper);
        if (looper == nullptr) {
            jmethodID prepare = env->GetStaticMethodID(looperClass, "prepare", "()V");
            env->CallStaticVoidMethod(looperClass, prepare);
        }

        jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
        jmethodID currentActivityThread = env->GetStaticMethodID(activityThreadClass, "currentActivityThread", "()Landroid/app/ActivityThread;");
        jobject activityThread = env->CallStaticObjectMethod(activityThreadClass, currentActivityThread);
        
        if (activityThread != nullptr) {
            jmethodID getApplication = env->GetMethodID(activityThreadClass, "getApplication", "()Landroid/app/Application;");
            jobject context = env->CallObjectMethod(activityThread, getApplication);

            if (context != nullptr) {
                jclass toastClass = env->FindClass("android/widget/Toast");
                jmethodID makeText = env->GetStaticMethodID(toastClass, "makeText", "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;");
                jstring jmsg = env->NewStringUTF(message.c_str());
                jobject toast = env->CallStaticObjectMethod(toastClass, makeText, context, jmsg, 0); 
                
                if (toast != nullptr) {
                    jmethodID show = env->GetMethodID(toastClass, "show", "()V");
                    env->CallVoidMethod(toast, show);
                }
                env->DeleteLocalRef(jmsg);
            }
        }
        
        jmethodID loop = env->GetStaticMethodID(looperClass, "loop", "()V");
        env->CallStaticVoidMethod(looperClass, loop);

        g_GlobalJavaVM->DetachCurrentThread();
    }).detach();
}

// ========================================================================
// HELPER FUNCTIONS
// ========================================================================
uintptr_t GetLibraryBase(const char* libName) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;
    char line[512];
    uintptr_t baseAddress = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, libName)) {
            sscanf(line, "%x-%*x", &baseAddress);
            break;
        }
    }
    fclose(fp);
    return baseAddress;
}

void ClearChat() {
    {
        std::lock_guard<std::mutex> lock(g_ChatMutex);
        g_ChatMessages.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_OutgoingChatMutex);
        g_OutgoingChats.clear();
    }
}

void AutoStartGameForClient() {
    g_IsMultiplayerMenuActive = false;
    ShowNativeToast("Switching to game. Please start the game manually.");
}

// ========================================================================
// ORIGINAL FUNCTION POINTERS
// ========================================================================
typedef void* (*renderMenu_t)(void* thiz, void* p1, void* p2, void* p3);
renderMenu_t orig_renderMenu = nullptr;

typedef void* (*renderStartMenuMain_t)(void* thiz, void* p1, void* p2, void* p3);
renderStartMenuMain_t orig_renderStartMenuMain = nullptr;

typedef void* (*updateGUI_t)(void* thiz, void* p1, void* p2, void* p3, void* p4);
updateGUI_t orig_updateGUI = nullptr;

typedef int32_t (*AInputQueue_getEvent_t)(void* queue, AInputEvent** outEvent);
AInputQueue_getEvent_t orig_AInputQueue_getEvent = nullptr;

typedef void (*GameUpdate_t)(void* thiz, float param_1);
GameUpdate_t orig_GameUpdate = nullptr;

typedef void (*GameUpdateStateBase_t)(void* thiz, float param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4);
GameUpdateStateBase_t orig_GameUpdateStateBase = nullptr;

// ========================================================================
// VEHICLE / MULTIPLAYER SYNCHRONIZATION
// Core synchronized implementation retained from the known-working yeni.cpp.
// ========================================================================
struct TestB2Vec2 {
    float x;
    float y;
};

typedef void (*VehicleGetPosition_t)(void* vehicle, float* p1, float* p2);
typedef float (*VehicleGetOrientation_t)(void* vehicle);
typedef void (*b2BodySetTransform_t)(void* body, const TestB2Vec2* position, float angle);

VehicleGetPosition_t g_VehicleGetPosition = nullptr;
VehicleGetOrientation_t g_VehicleGetOrientation = nullptr;
b2BodySetTransform_t g_b2BodySetTransform = nullptr;

static const uint8_t PACKET_SESSION_WELCOME = 1;
static const uint8_t PACKET_CHAT = 2;
static const uint8_t PACKET_VEHICLE_POSITION = 3;
static const uint8_t PACKET_VEHICLE_CLAIM = 4;
static const uint8_t PACKET_VEHICLE_AUTHORITY = 5;
static const uint8_t PACKET_VEHICLE_RELEASE = 6;
static const uint8_t PACKET_VEHICLE_SNAPSHOT = 7;

static const uint8_t MAX_PLAYERS = 4;
static const uint16_t VEHICLE_ID_INVALID = 0xFFFF;
static const uint16_t VEHICLE_SLOT_LIMIT = 512;
static const uint8_t VEHICLE_OWNER_NONE = 0xFF;

// Local vehicle sampling target: 120 Hz.
// The actual callback frequency still follows the game's own update/vsync loop.
// Network vehicle snapshots are deliberately limited to 10 snapshots/sec.
static const uint32_t VEHICLE_LOCAL_SAMPLE_INTERVAL_US = 8333;
static const uint32_t VEHICLE_NETWORK_INTERVAL_MS = 100;
static const uint32_t VEHICLE_STOP_RELEASE_MS = 1000;
static const uint8_t VEHICLE_SNAPSHOT_MAX_COUNT = 64;
static const float POSITION_EPSILON = 0.02f;
static const float ANGLE_EPSILON = 0.005f;
static const float CLAIM_POSITION_EPSILON = 0.05f;
static const float CLAIM_ANGLE_EPSILON = 0.02f;

#pragma pack(push, 1)

struct SessionWelcomePacket {
    uint8_t type;
    uint8_t ownerId;
    uint8_t maxPlayers;
    uint8_t reserved;
};

struct NetworkPacket {
    uint8_t type;
    char senderName[32];
    char chatData[223];
};

struct VehiclePositionPacket {
    uint8_t type;
    uint8_t ownerId;
    uint16_t vehicleId;
    float x;
    float y;
    float angle;
    uint32_t sequence;
    uint8_t moving;
};

// One TCP message can carry the newest state of many vehicles.
// This keeps vehicle traffic at ~10 network messages/sec regardless of
// how many local vehicles are being sampled.
struct VehicleSnapshotHeader {
    uint8_t type;
    uint8_t ownerId;
    uint8_t count;
    uint8_t reserved;
    uint32_t sequence;
};

struct VehicleSnapshotEntry {
    uint16_t vehicleId;
    float x;
    float y;
    float angle;
    uint32_t sequence;
    uint8_t moving;
};

// Client -> host. The host decides which player gets authority first.
struct VehicleClaimPacket {
    uint8_t type;
    uint8_t ownerId;
    uint16_t vehicleId;
    uint32_t claimSequence;
};

// Host -> client. This is the authoritative owner of one vehicle.
// ownerId == VEHICLE_OWNER_NONE means that the vehicle was released.
struct VehicleAuthorityPacket {
    uint8_t type;
    uint8_t ownerId;
    uint16_t vehicleId;
    uint32_t generation;
};

// Client -> host. The owner explicitly releases a vehicle after stopping.
struct VehicleReleasePacket {
    uint8_t type;
    uint8_t ownerId;
    uint16_t vehicleId;
    uint32_t releaseSequence;
};

#pragma pack(pop)

static_assert(sizeof(SessionWelcomePacket) == 4, "SessionWelcomePacket size mismatch");
static_assert(sizeof(NetworkPacket) == 256, "NetworkPacket size mismatch");
static_assert(sizeof(VehiclePositionPacket) == 21, "VehiclePositionPacket size mismatch");
static_assert(sizeof(VehicleSnapshotHeader) == 8, "VehicleSnapshotHeader size mismatch");
static_assert(sizeof(VehicleSnapshotEntry) == 19, "VehicleSnapshotEntry size mismatch");
static_assert(sizeof(VehicleClaimPacket) == 8, "VehicleClaimPacket size mismatch");
static_assert(sizeof(VehicleAuthorityPacket) == 8, "VehicleAuthorityPacket size mismatch");
static_assert(sizeof(VehicleReleasePacket) == 8, "VehicleReleasePacket size mismatch");

static std::atomic<uint8_t> g_LocalPlayerId(0xFF);
static std::atomic<uint32_t> g_LocalVehicleSequence(0);
static std::atomic<uint32_t> g_LocalClaimSequence(0);

struct VehicleAuthorityState {
    uint8_t ownerId;
    uint32_t generation;
};

struct VehicleRemoteState {
    bool valid;
    uint8_t ownerId;
    uint16_t vehicleId;
    float x;
    float y;
    float angle;
    float vx;
    float vy;
    float angularVelocity;
    uint32_t sequence;
    uint32_t appliedSequence;
    uint64_t lastReceiveMs;
    bool moving;
};

struct VehicleSampleState {
    bool valid;
    float x;
    float y;
    float angle;
};

static VehicleAuthorityState g_VehicleAuthority[VEHICLE_SLOT_LIMIT];
static VehicleRemoteState g_RemoteVehicles[VEHICLE_SLOT_LIMIT];
static VehicleSampleState g_LocalSamples[VEHICLE_SLOT_LIMIT];
static VehicleSampleState g_LastSentLocalVehicles[VEHICLE_SLOT_LIMIT];
static uintptr_t g_KnownVehiclePointers[VEHICLE_SLOT_LIMIT];
static bool g_ClaimPending[VEHICLE_SLOT_LIMIT];
static uint64_t g_LocalStationarySince[VEHICLE_SLOT_LIMIT];
static uint64_t g_RemoteStationarySince[VEHICLE_SLOT_LIMIT];

static uint32_t g_LastKnownVehicleCount = 0;
static std::mutex g_VehicleStateMutex;

// Network transport keeps only the newest sample for each vehicle.
// The network thread packs all valid samples into one snapshot every 100 ms.
static VehiclePositionPacket g_LatestOutgoingVehicles[VEHICLE_SLOT_LIMIT];
static bool g_HasLatestOutgoingVehicle[VEHICLE_SLOT_LIMIT];
static uint64_t g_LastVehicleNetworkSendMs = 0;

static VehicleClaimPacket g_PendingClaimPacket;
static bool g_HasPendingClaimPacket = false;
static VehicleReleasePacket g_PendingReleasePacket;
static bool g_HasPendingReleasePacket = false;
static std::vector<VehicleAuthorityPacket> g_PendingAuthorityPackets;
static std::mutex g_VehicleSendMutex;

static std::chrono::steady_clock::time_point g_LastVehicleSync = std::chrono::steady_clock::now();

static uintptr_t GetVehicleFromIndex(uintptr_t game, uint16_t vehicleId) {
    if (game == 0) return 0;

    const uint32_t vehicleCount = *(uint32_t*)(game + 0xA4);
    if (vehicleId >= vehicleCount || vehicleId >= VEHICLE_SLOT_LIMIT) return 0;

    const uintptr_t vehicleSlotAddress = game + ((uintptr_t)(vehicleId + 0x2A) * 4u) + 4u;
    return *(uintptr_t*)vehicleSlotAddress;
}

static uintptr_t GetActiveVehicleFromGame(uintptr_t game, uint16_t* outVehicleId) {
    if (game == 0) return 0;

    const uint32_t vehicleIndex = *(uint32_t*)(game + 0xA8);
    if (vehicleIndex >= VEHICLE_SLOT_LIMIT) return 0;

    if (outVehicleId) *outVehicleId = (uint16_t)vehicleIndex;
    return GetVehicleFromIndex(game, (uint16_t)vehicleIndex);
}

// Send a complete packet over TCP, handling partial sends.
static bool SendAllBytes(int socketFd, const void* data, size_t size) {
    if (socketFd < 0 || data == nullptr || size == 0) return false;

    const uint8_t* bytes = (const uint8_t*)data;
    size_t totalSent = 0;

    while (totalSent < size) {
        ssize_t sent = send(socketFd, bytes + totalSent, size - totalSent, MSG_NOSIGNAL);

        if (sent > 0) {
            totalSent += (size_t)sent;
            continue;
        }

        if (sent < 0 && (errno == EINTR)) {
            continue;
        }

        return false;
    }

    return true;
}

static uint64_t GetMonotonicMilliseconds() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static float GetAngleDelta(float a, float b) {
    float delta = fabsf(a - b);
    const float twoPi = 6.28318530718f;
    if (delta > 3.14159265359f) delta = twoPi - delta;
    return fabsf(delta);
}

static float GetSignedAngleDelta(float from, float to) {
    float delta = to - from;
    const float pi = 3.14159265359f;
    const float twoPi = 6.28318530718f;

    while (delta > pi) delta -= twoPi;
    while (delta < -pi) delta += twoPi;

    return delta;
}

static void ClearVehicleStateSlot(uint16_t vehicleId) {
    if (vehicleId >= VEHICLE_SLOT_LIMIT) return;

    g_VehicleAuthority[vehicleId].ownerId = VEHICLE_OWNER_NONE;
    g_VehicleAuthority[vehicleId].generation = 0;
    memset(&g_RemoteVehicles[vehicleId], 0, sizeof(g_RemoteVehicles[vehicleId]));
    memset(&g_LocalSamples[vehicleId], 0, sizeof(g_LocalSamples[vehicleId]));
    memset(&g_LastSentLocalVehicles[vehicleId], 0, sizeof(g_LastSentLocalVehicles[vehicleId]));
    g_KnownVehiclePointers[vehicleId] = 0;
    g_ClaimPending[vehicleId] = false;
    g_LocalStationarySince[vehicleId] = 0;
    g_RemoteStationarySince[vehicleId] = 0;
}

static void RefreshVehicleTopology(uintptr_t game) {
    if (game == 0) return;

    const uint32_t vehicleCount = *(uint32_t*)(game + 0xA4);
    const uint32_t safeVehicleCount = (vehicleCount > VEHICLE_SLOT_LIMIT)
        ? VEHICLE_SLOT_LIMIT : vehicleCount;

    bool topologyChanged = (safeVehicleCount != g_LastKnownVehicleCount);

    if (!topologyChanged) {
        for (uint16_t i = 0; i < safeVehicleCount; ++i) {
            const uintptr_t currentVehicle = GetVehicleFromIndex(game, i);
            if (currentVehicle != g_KnownVehiclePointers[i]) {
                topologyChanged = true;
                break;
            }
        }
    }

    if (!topologyChanged) return;

    std::lock_guard<std::mutex> lock(g_VehicleStateMutex);

    // Vehicle::removeVehicle() compacts the pointer array, so a numeric index
    // is not a persistent identity. Reset all ownership if the pointer topology
    // changes instead of assigning an old owner to a different vehicle.
    for (uint16_t i = 0; i < VEHICLE_SLOT_LIMIT; ++i) {
        ClearVehicleStateSlot(i);
    }

    for (uint16_t i = 0; i < safeVehicleCount; ++i) {
        g_KnownVehiclePointers[i] = GetVehicleFromIndex(game, i);
    }

    g_LastKnownVehicleCount = safeVehicleCount;
}

static bool GetVehicleOwner(uint16_t vehicleId, uint8_t* outOwner) {
    if (vehicleId >= VEHICLE_SLOT_LIMIT) return false;

    std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
    const uint8_t owner = g_VehicleAuthority[vehicleId].ownerId;
    if (outOwner) *outOwner = owner;
    return true;
}

static void QueueVehiclePosition(uint16_t vehicleId, float x, float y, float angle, bool moving) {
    if (!g_IsConnected.load()) return;

    const uint8_t ownerId = g_LocalPlayerId.load();
    if (ownerId == VEHICLE_OWNER_NONE || ownerId >= MAX_PLAYERS) return;
    if (vehicleId >= VEHICLE_SLOT_LIMIT) return;

    VehiclePositionPacket pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.type = PACKET_VEHICLE_POSITION;
    pkt.ownerId = ownerId;
    pkt.vehicleId = vehicleId;
    pkt.x = x;
    pkt.y = y;
    pkt.angle = angle;
    pkt.sequence = g_LocalVehicleSequence.fetch_add(1) + 1;
    pkt.moving = moving ? 1 : 0;

    {
        std::lock_guard<std::mutex> lock(g_VehicleSendMutex);
        g_LatestOutgoingVehicles[vehicleId] = pkt;
        g_HasLatestOutgoingVehicle[vehicleId] = true;
    }
}

static void QueueVehicleClaim(uint16_t vehicleId) {
    if (!g_IsConnected.load() || g_IsHost.load()) return;
    if (vehicleId >= VEHICLE_SLOT_LIMIT) return;

    const uint8_t ownerId = g_LocalPlayerId.load();
    if (ownerId == VEHICLE_OWNER_NONE || ownerId >= MAX_PLAYERS) return;

    VehicleClaimPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_VEHICLE_CLAIM;
    pkt.ownerId = ownerId;
    pkt.vehicleId = vehicleId;
    pkt.claimSequence = g_LocalClaimSequence.fetch_add(1) + 1;

    std::lock_guard<std::mutex> lock(g_VehicleSendMutex);
    g_PendingClaimPacket = pkt;
    g_HasPendingClaimPacket = true;
}

static void QueueVehicleRelease(uint16_t vehicleId) {
    if (!g_IsConnected.load() || g_IsHost.load()) return;
    if (vehicleId >= VEHICLE_SLOT_LIMIT) return;

    const uint8_t ownerId = g_LocalPlayerId.load();
    if (ownerId == VEHICLE_OWNER_NONE || ownerId >= MAX_PLAYERS) return;

    VehicleReleasePacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_VEHICLE_RELEASE;
    pkt.ownerId = ownerId;
    pkt.vehicleId = vehicleId;
    pkt.releaseSequence = g_LocalClaimSequence.fetch_add(1) + 1;

    std::lock_guard<std::mutex> lock(g_VehicleSendMutex);
    g_PendingReleasePacket = pkt;
    g_HasPendingReleasePacket = true;
}

static void QueueVehicleAuthority(uint16_t vehicleId, uint8_t ownerId, uint32_t generation) {
    if (!g_IsConnected.load() || !g_IsHost.load()) return;
    if (vehicleId >= VEHICLE_SLOT_LIMIT) return;

    VehicleAuthorityPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_VEHICLE_AUTHORITY;
    pkt.ownerId = ownerId;
    pkt.vehicleId = vehicleId;
    pkt.generation = generation;

    std::lock_guard<std::mutex> lock(g_VehicleSendMutex);
    if (g_PendingAuthorityPackets.size() >= 16) {
        g_PendingAuthorityPackets.erase(g_PendingAuthorityPackets.begin());
    }
    g_PendingAuthorityPackets.push_back(pkt);
}

static bool AssignVehicleAuthority(uint16_t vehicleId, uint8_t requestedOwner, bool notifyClient) {
    if (vehicleId >= VEHICLE_SLOT_LIMIT) return false;
    if (requestedOwner >= MAX_PLAYERS && requestedOwner != VEHICLE_OWNER_NONE) return false;

    uint8_t finalOwner = VEHICLE_OWNER_NONE;
    uint32_t generation = 0;
    bool changed = false;

    {
        std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
        VehicleAuthorityState& auth = g_VehicleAuthority[vehicleId];

        if (auth.ownerId == VEHICLE_OWNER_NONE) {
            auth.ownerId = requestedOwner;
            auth.generation++;
            changed = true;
        } else if (auth.ownerId != requestedOwner) {
            return false;
        }

        finalOwner = auth.ownerId;
        generation = auth.generation;

        if (changed) {
            memset(&g_RemoteVehicles[vehicleId], 0, sizeof(g_RemoteVehicles[vehicleId]));
            g_ClaimPending[vehicleId] = false;
            g_LocalStationarySince[vehicleId] = 0;
            g_RemoteStationarySince[vehicleId] = 0;
            memset(&g_LastSentLocalVehicles[vehicleId], 0, sizeof(g_LastSentLocalVehicles[vehicleId]));
        }
    }

    if (notifyClient && g_IsHost.load() && g_IsConnected.load()) {
        QueueVehicleAuthority(vehicleId, finalOwner, generation);
    }

    return changed || finalOwner == requestedOwner;
}

static bool ReleaseVehicleAuthority(uint16_t vehicleId, uint8_t requestedOwner, bool notifyClient) {
    if (vehicleId >= VEHICLE_SLOT_LIMIT) return false;
    if (requestedOwner >= MAX_PLAYERS) return false;

    uint32_t generation = 0;

    {
        std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
        VehicleAuthorityState& auth = g_VehicleAuthority[vehicleId];

        if (auth.ownerId != requestedOwner) return false;

        auth.ownerId = VEHICLE_OWNER_NONE;
        auth.generation++;
        generation = auth.generation;

        memset(&g_RemoteVehicles[vehicleId], 0, sizeof(g_RemoteVehicles[vehicleId]));
        memset(&g_LocalSamples[vehicleId], 0, sizeof(g_LocalSamples[vehicleId]));
        memset(&g_LastSentLocalVehicles[vehicleId], 0, sizeof(g_LastSentLocalVehicles[vehicleId]));
        g_ClaimPending[vehicleId] = false;
        g_LocalStationarySince[vehicleId] = 0;
        g_RemoteStationarySince[vehicleId] = 0;
    }

    if (notifyClient && g_IsHost.load() && g_IsConnected.load()) {
        QueueVehicleAuthority(vehicleId, VEHICLE_OWNER_NONE, generation);
    }

    LOGI("[AUTHORITY] released vehicle=%u owner=%u generation=%u",
         (unsigned)vehicleId, (unsigned)requestedOwner, (unsigned)generation);
    return true;
}

static bool TryClaimLocalVehicle(uint16_t vehicleId) {
    if (vehicleId >= VEHICLE_SLOT_LIMIT) return false;
    if (!g_IsConnected.load()) return false;

    const uint8_t localOwner = g_LocalPlayerId.load();
    if (localOwner == VEHICLE_OWNER_NONE || localOwner >= MAX_PLAYERS) return false;

    uint8_t currentOwner = VEHICLE_OWNER_NONE;
    {
        std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
        currentOwner = g_VehicleAuthority[vehicleId].ownerId;

        if (currentOwner == localOwner) return true;
        if (currentOwner != VEHICLE_OWNER_NONE) return false;

        if (g_ClaimPending[vehicleId]) return false;

        g_ClaimPending[vehicleId] = true;
    }

    if (g_IsHost.load()) {
        // Host participates in the same first-claim arbitration as the client.
        if (!AssignVehicleAuthority(vehicleId, localOwner, true)) {
            std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
            g_ClaimPending[vehicleId] = false;
            return false;
        }
        return true;
    }

    QueueVehicleClaim(vehicleId);
    LOGI("[AUTHORITY] claim requested vehicle=%u owner=%u",
         (unsigned)vehicleId, (unsigned)localOwner);
    return false;
}

static void CaptureAndQueueLocalVehicleState(uintptr_t game) {
    if (game == 0 || !g_IsConnected.load()) return;
    if (!g_VehicleGetPosition || !g_VehicleGetOrientation) return;

    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const uint64_t elapsedUs =
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
            now - g_LastVehicleSync).count();

    if (elapsedUs < VEHICLE_LOCAL_SAMPLE_INTERVAL_US) return;
    g_LastVehicleSync = now;

    RefreshVehicleTopology(game);

    const uint32_t rawVehicleCount = *(uint32_t*)(game + 0xA4);
    const uint32_t vehicleCount =
        (rawVehicleCount > VEHICLE_SLOT_LIMIT)
            ? VEHICLE_SLOT_LIMIT
            : rawVehicleCount;

    uint16_t activeVehicleId = VEHICLE_ID_INVALID;
    GetActiveVehicleFromGame(game, &activeVehicleId);

    const uint8_t localOwner = g_LocalPlayerId.load();
    if (localOwner == VEHICLE_OWNER_NONE || localOwner >= MAX_PLAYERS) return;

    const uint64_t nowMs = GetMonotonicMilliseconds();

    for (uint16_t vehicleId = 0; vehicleId < vehicleCount; ++vehicleId) {
        uintptr_t vehicle = GetVehicleFromIndex(game, vehicleId);
        if (vehicle == 0) continue;

        float x = 0.0f;
        float y = 0.0f;
        float angle = 0.0f;

        g_VehicleGetPosition((void*)vehicle, &x, &y);
        angle = g_VehicleGetOrientation((void*)vehicle);

        bool moved = false;
        bool rotated = false;
        uint8_t ownerId = VEHICLE_OWNER_NONE;

        {
            std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
            VehicleSampleState& sample = g_LocalSamples[vehicleId];

            if (sample.valid) {
                moved =
                    fabsf(x - sample.x) > CLAIM_POSITION_EPSILON ||
                    fabsf(y - sample.y) > CLAIM_POSITION_EPSILON;

                rotated =
                    GetAngleDelta(angle, sample.angle) >
                    CLAIM_ANGLE_EPSILON;
            }

            sample.valid = true;
            sample.x = x;
            sample.y = y;
            sample.angle = angle;

            ownerId = g_VehicleAuthority[vehicleId].ownerId;
        }

        const bool moving = moved || rotated;

        // Ask the host for authority when appropriate, but do not make
        // network publication depend on the authority handshake. This is
        // what allows Player A to drive the harvester while Player B drives
        // the tractor on a different vehicle slot.
        if (vehicleId == activeVehicleId && moving &&
            ownerId == VEHICLE_OWNER_NONE) {
            TryClaimLocalVehicle(vehicleId);
        }

        if (moving) {
            g_LocalStationarySince[vehicleId] = 0;
        } else if (g_LocalStationarySince[vehicleId] == 0) {
            g_LocalStationarySince[vehicleId] = nowMs;
        }

        const bool stoppedLongEnough =
            g_LocalStationarySince[vehicleId] != 0 &&
            (nowMs - g_LocalStationarySince[vehicleId]) >= VEHICLE_STOP_RELEASE_MS;

        if (stoppedLongEnough && ownerId == localOwner) {
            if (g_IsHost.load()) {
                ReleaseVehicleAuthority(vehicleId, localOwner, true);
            } else {
                QueueVehicleRelease(vehicleId);

                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                g_ClaimPending[vehicleId] = true;
            }
        }

        // Publish:
        //   - the currently driven vehicle even if it is still contested;
        //   - vehicles for which this device already has authority.
        //
        // Therefore different vehicles can travel simultaneously, while the
        // same vehicle can still exhibit the expected tug-of-war/tremble.
        const bool publishActiveVehicle =
            (vehicleId == activeVehicleId);

        const bool publishOwnedVehicle =
            (ownerId == localOwner);

        if (publishActiveVehicle || publishOwnedVehicle) {
            QueueVehiclePosition(
                vehicleId,
                x,
                y,
                angle,
                moving
            );
        }
    }
}

static void ApplyRemoteVehicleStates(uintptr_t game) {
    if (game == 0 || !g_b2BodySetTransform) return;
    if (!g_IsConnected.load()) return;

    RefreshVehicleTopology(game);

    const uint32_t rawVehicleCount = *(uint32_t*)(game + 0xA4);
    const uint32_t vehicleCount =
        (rawVehicleCount > VEHICLE_SLOT_LIMIT)
            ? VEHICLE_SLOT_LIMIT
            : rawVehicleCount;

    const uint8_t localOwner = g_LocalPlayerId.load();
    if (localOwner == VEHICLE_OWNER_NONE || localOwner >= MAX_PLAYERS) return;

    const uint64_t nowMs = GetMonotonicMilliseconds();

    for (uint16_t vehicleId = 0; vehicleId < vehicleCount; ++vehicleId) {
        VehicleRemoteState state;
        uint8_t authorityOwner = VEHICLE_OWNER_NONE;

        {
            std::lock_guard<std::mutex> lock(g_VehicleStateMutex);

            // A snapshot may arrive before the host's authority packet.
            // Keep buffering it, but NEVER apply it until this vehicle is
            // explicitly owned by that remote player. This is the critical
            // separation between network reception and local physics control.
            authorityOwner = g_VehicleAuthority[vehicleId].ownerId;

            if (authorityOwner == VEHICLE_OWNER_NONE) {
                continue;
            }

            if (authorityOwner == localOwner) {
                continue;
            }

            if (!g_RemoteVehicles[vehicleId].valid) {
                continue;
            }

            state = g_RemoteVehicles[vehicleId];
        }

        // A remote snapshot is valid only when its sender is the current
        // authoritative owner for this exact vehicle slot.
        if (state.ownerId != authorityOwner) continue;
        if (state.vehicleId != vehicleId) continue;

        uintptr_t vehicle = GetVehicleFromIndex(game, vehicleId);
        if (vehicle == 0) continue;

        uintptr_t body = *(uintptr_t*)(vehicle + 0x528);
        if (body == 0) continue;

        float predictedX = state.x;
        float predictedY = state.y;
        float predictedAngle = state.angle;

        if (state.moving && state.lastReceiveMs != 0) {
            float elapsedSec =
                (float)(nowMs - state.lastReceiveMs) / 1000.0f;

            // Never extrapolate too far past the newest snapshot.
            if (elapsedSec > 0.12f) {
                elapsedSec = 0.12f;
            }

            predictedX += state.vx * elapsedSec;
            predictedY += state.vy * elapsedSec;
            predictedAngle += state.angularVelocity * elapsedSec;
        }

        TestB2Vec2 position;
        position.x = predictedX;
        position.y = predictedY;

        // This call is intentionally restricted to remotely-authoritative
        // vehicles. The local driver's physics is left completely intact.
        g_b2BodySetTransform(
            (void*)body,
            &position,
            predictedAngle
        );
    }
}

static void HandleSessionWelcome(const SessionWelcomePacket& pkt) {
    if (pkt.ownerId >= MAX_PLAYERS) return;
    g_LocalPlayerId.store(pkt.ownerId);

    char playerMsg[64];
    snprintf(playerMsg, sizeof(playerMsg), "Joined multiplayer as Player %u",
             (unsigned)(pkt.ownerId + 1));
    ShowNativeToast(playerMsg);
}

static void HandleVehicleClaimPacket(const VehicleClaimPacket& pkt) {
    if (!g_IsHost.load()) return;
    if (pkt.ownerId >= MAX_PLAYERS) return;
    if (pkt.ownerId == g_LocalPlayerId.load()) return;
    if (pkt.vehicleId >= VEHICLE_SLOT_LIMIT) return;

    bool granted = AssignVehicleAuthority(pkt.vehicleId, pkt.ownerId, true);

    if (granted) {
        LOGI("[AUTHORITY] granted vehicle=%u owner=%u claimSeq=%u",
             (unsigned)pkt.vehicleId, (unsigned)pkt.ownerId,
             (unsigned)pkt.claimSequence);
    }

    if (!granted) {
        uint8_t currentOwner = VEHICLE_OWNER_NONE;
        uint32_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
            currentOwner = g_VehicleAuthority[pkt.vehicleId].ownerId;
            generation = g_VehicleAuthority[pkt.vehicleId].generation;
        }

        if (currentOwner != VEHICLE_OWNER_NONE) {
            QueueVehicleAuthority(pkt.vehicleId, currentOwner, generation);
        }
    }
}

static void HandleVehicleAuthorityPacket(const VehicleAuthorityPacket& pkt) {
    if (pkt.vehicleId >= VEHICLE_SLOT_LIMIT) return;
    if (pkt.ownerId >= MAX_PLAYERS && pkt.ownerId != VEHICLE_OWNER_NONE) return;

    if (g_IsHost.load()) return;

    std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
    VehicleAuthorityState& auth = g_VehicleAuthority[pkt.vehicleId];

    if (auth.generation > pkt.generation) return;

    const bool ownerChanged = (auth.ownerId != pkt.ownerId);
    auth.ownerId = pkt.ownerId;
    auth.generation = pkt.generation;
    g_ClaimPending[pkt.vehicleId] = false;

    if (ownerChanged) {
        memset(&g_RemoteVehicles[pkt.vehicleId], 0, sizeof(g_RemoteVehicles[pkt.vehicleId]));
        memset(&g_LocalSamples[pkt.vehicleId], 0, sizeof(g_LocalSamples[pkt.vehicleId]));
        memset(&g_LastSentLocalVehicles[pkt.vehicleId], 0, sizeof(g_LastSentLocalVehicles[pkt.vehicleId]));
        g_LocalStationarySince[pkt.vehicleId] = 0;
        g_RemoteStationarySince[pkt.vehicleId] = 0;
    } else if (pkt.ownerId == g_LocalPlayerId.load()) {
        memset(&g_RemoteVehicles[pkt.vehicleId], 0, sizeof(g_RemoteVehicles[pkt.vehicleId]));
    }

    LOGI("[AUTHORITY] vehicle=%u owner=%u generation=%u",
         (unsigned)pkt.vehicleId,
         (unsigned)pkt.ownerId,
         (unsigned)pkt.generation);
}

static void StoreRemoteVehicleState(
    uint8_t ownerId,
    uint16_t vehicleId,
    float x,
    float y,
    float angle,
    uint32_t sequence,
    bool moving
) {
    if (ownerId >= MAX_PLAYERS) return;
    if (vehicleId >= VEHICLE_SLOT_LIMIT) return;

    const uint8_t localOwner = g_LocalPlayerId.load();
    if (ownerId == localOwner) return;

    const uint64_t nowMs = GetMonotonicMilliseconds();

    std::lock_guard<std::mutex> lock(g_VehicleStateMutex);

    VehicleRemoteState& state = g_RemoteVehicles[vehicleId];

    // Receiving is deliberately independent from authority. The state is
    // buffered here; ApplyRemoteVehicleStates() is the only place allowed to
    // touch the physics body, and it checks authority before doing so.
    if (state.valid &&
        state.ownerId == ownerId &&
        sequence <= state.sequence) {
        return;
    }

    if (state.valid &&
        state.ownerId == ownerId &&
        state.lastReceiveMs != 0) {

        uint64_t deltaMs = nowMs - state.lastReceiveMs;

        if (deltaMs >= 10 && deltaMs <= 500) {
            float dt = (float)deltaMs / 1000.0f;

            state.vx = (x - state.x) / dt;
            state.vy = (y - state.y) / dt;

            const float angleDelta =
                GetSignedAngleDelta(state.angle, angle);

            state.angularVelocity =
                angleDelta / dt;
        }
    } else {
        state.vx = 0.0f;
        state.vy = 0.0f;
        state.angularVelocity = 0.0f;
    }

    state.valid = true;
    state.ownerId = ownerId;
    state.vehicleId = vehicleId;
    state.x = x;
    state.y = y;
    state.angle = angle;
    state.sequence = sequence;
    state.appliedSequence = 0;
    state.lastReceiveMs = nowMs;
    state.moving = moving;
}

static void HandleVehiclePositionPacket(
    const VehiclePositionPacket& pkt
) {
    StoreRemoteVehicleState(
        pkt.ownerId,
        pkt.vehicleId,
        pkt.x,
        pkt.y,
        pkt.angle,
        pkt.sequence,
        pkt.moving != 0
    );
}

static void HandleVehicleSnapshotPacket(
    const VehicleSnapshotHeader& header,
    const uint8_t* payload
) {
    if (header.ownerId >= MAX_PLAYERS) return;
    if (header.count > VEHICLE_SNAPSHOT_MAX_COUNT) return;
    if (payload == nullptr && header.count != 0) return;

    for (uint8_t i = 0; i < header.count; ++i) {
        VehicleSnapshotEntry entry;
        memcpy(
            &entry,
            payload + ((size_t)i * sizeof(VehicleSnapshotEntry)),
            sizeof(entry)
        );

        StoreRemoteVehicleState(
            header.ownerId,
            entry.vehicleId,
            entry.x,
            entry.y,
            entry.angle,
            entry.sequence,
            entry.moving != 0
        );
    }
}

static void HandleVehicleReleasePacket(const VehicleReleasePacket& pkt) {
    if (!g_IsHost.load()) return;
    if (pkt.ownerId >= MAX_PLAYERS) return;
    if (pkt.vehicleId >= VEHICLE_SLOT_LIMIT) return;

    ReleaseVehicleAuthority(pkt.vehicleId, pkt.ownerId, true);
}

static void ResetVehicleSyncState() {
    g_LocalPlayerId.store(0xFF);
    g_LocalVehicleSequence.store(0);
    g_LocalClaimSequence.store(0);
    g_LastKnownVehicleCount = 0;
    g_LastVehicleSync = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
        for (uint16_t i = 0; i < VEHICLE_SLOT_LIMIT; ++i) {
            ClearVehicleStateSlot(i);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_VehicleSendMutex);

        memset(
            g_LatestOutgoingVehicles,
            0,
            sizeof(g_LatestOutgoingVehicles)
        );

        memset(
            g_HasLatestOutgoingVehicle,
            0,
            sizeof(g_HasLatestOutgoingVehicle)
        );

        g_LastVehicleNetworkSendMs = 0;
        g_PendingAuthorityPackets.clear();
        memset(&g_PendingClaimPacket, 0, sizeof(g_PendingClaimPacket));
        g_HasPendingClaimPacket = false;
        memset(&g_PendingReleasePacket, 0, sizeof(g_PendingReleasePacket));
        g_HasPendingReleasePacket = false;
    }
}

// ========================================================================
// NETWORK FUNCTIONS
// ========================================================================
bool IsNetworkAvailable() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    struct ifconf ifc;
    char buf[4096];
    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;

    if (ioctl(sock, SIOCGIFCONF, &ifc) < 0) {
        close(sock);
        return false;
    }

    bool hasNetwork = false;
    struct ifreq* it = ifc.ifc_req;
    const struct ifreq* const end = it + (ifc.ifc_len / sizeof(struct ifreq));

    for (; it != end; ++it) {
        if (it->ifr_addr.sa_family == AF_INET) {
            struct sockaddr_in* addr = (struct sockaddr_in*)&it->ifr_addr;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
            if (strcmp(ip, "127.0.0.1") != 0 && strcmp(ip, "0.0.0.0") != 0) {
                hasNetwork = true;
                break;
            }
        }
    }
    close(sock);
    return hasNetwork;
}

bool IsLocalIP(const std::string& ip) {
    if (ip == "127.0.0.1") return true;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    struct ifconf ifc;
    char buf[4096];
    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;

    if (ioctl(sock, SIOCGIFCONF, &ifc) < 0) {
        close(sock);
        return false;
    }

    bool isLocal = false;
    struct ifreq* it = ifc.ifc_req;
    const struct ifreq* const end = it + (ifc.ifc_len / sizeof(struct ifreq));

    for (; it != end; ++it) {
        if (it->ifr_addr.sa_family == AF_INET) {
            struct sockaddr_in* addr = (struct sockaddr_in*)&it->ifr_addr;
            char localIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr->sin_addr, localIp, sizeof(localIp));
            if (ip == localIp) {
                isLocal = true;
                break;
            }
        }
    }
    close(sock);
    return isLocal;
}

void OpenAndroidKeyboard() {
    if (g_GlobalJavaVM == nullptr) return;
    g_AndroidKeyboardOpen.store(true);

    JNIEnv* env = nullptr;
    bool attached = false;
    jint res = g_GlobalJavaVM->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        if (g_GlobalJavaVM->AttachCurrentThread(&env, nullptr) == 0) {
            attached = true;
        }
    }

    if (env != nullptr) {
        jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
        if (activityThreadClass != nullptr) {
            jmethodID currentActivityThreadMethod = env->GetStaticMethodID(activityThreadClass, "currentActivityThread", "()Landroid/app/ActivityThread;");
            if (currentActivityThreadMethod != nullptr) {
                jobject activityThread = env->CallStaticObjectMethod(activityThreadClass, currentActivityThreadMethod);
                if (activityThread != nullptr) {
                    jmethodID getApplicationMethod = env->GetMethodID(activityThreadClass, "getApplication", "()Landroid/app/Application;");
                    jobject context = env->CallObjectMethod(activityThread, getApplicationMethod);

                    if (context != nullptr) {
                        jclass contextClass = env->GetObjectClass(context);
                        jmethodID getSystemServiceMethod = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
                        
                        jstring immStr = env->NewStringUTF("input_method");
                        jobject imm = env->CallObjectMethod(context, getSystemServiceMethod, immStr);
                        env->DeleteLocalRef(immStr);

                        if (imm != nullptr) {
                            jclass immClass = env->GetObjectClass(imm);
                            jmethodID toggleSoftInputMethod = env->GetMethodID(immClass, "toggleSoftInput", "(II)V");
                            if (toggleSoftInputMethod != nullptr) {
                                env->CallVoidMethod(imm, toggleSoftInputMethod, 2, 0); 
                            }
                        }
                    }
                }
            }
        }
    }

    if (attached) {
        g_GlobalJavaVM->DetachCurrentThread();
    }
}

void CloseAndroidKeyboard() {
    // Do not blindly call toggleSoftInput(): when the keyboard is already hidden,
    // that API can OPEN it. The old UI used exactly that toggle and made BACK appear
    // to open the keyboard. We now close only when our UI actually opened it.
    if (!g_AndroidKeyboardOpen.exchange(false)) return;
    if (g_GlobalJavaVM == nullptr) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    jint res = g_GlobalJavaVM->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        if (g_GlobalJavaVM->AttachCurrentThread(&env, nullptr) == 0) {
            attached = true;
        }
    }

    if (env != nullptr) {
        jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
        if (activityThreadClass != nullptr) {
            jmethodID currentActivityThreadMethod = env->GetStaticMethodID(activityThreadClass, "currentActivityThread", "()Landroid/app/ActivityThread;");
            if (currentActivityThreadMethod != nullptr) {
                jobject activityThread = env->CallStaticObjectMethod(activityThreadClass, currentActivityThreadMethod);
                if (activityThread != nullptr) {
                    jmethodID getApplicationMethod = env->GetMethodID(activityThreadClass, "getApplication", "()Landroid/app/Application;");
                    jobject context = env->CallObjectMethod(activityThread, getApplicationMethod);

                    if (context != nullptr) {
                        jclass contextClass = env->GetObjectClass(context);
                        jmethodID getSystemServiceMethod = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
                        
                        jstring immStr = env->NewStringUTF("input_method");
                        jobject imm = env->CallObjectMethod(context, getSystemServiceMethod, immStr);
                        env->DeleteLocalRef(immStr);

                        if (imm != nullptr) {
                            jclass immClass = env->GetObjectClass(imm);
                            jmethodID toggleSoftInputMethod = env->GetMethodID(immClass, "toggleSoftInput", "(II)V");
                            if (toggleSoftInputMethod != nullptr) {
                                env->CallVoidMethod(imm, toggleSoftInputMethod, 0, 0); 
                            }
                        }
                    }
                }
            }
        }
    }

    if (attached) {
        g_GlobalJavaVM->DetachCurrentThread();
    }
}

GLuint LoadTextureFromPNGArrayEx(const unsigned char* png_data, int data_len, int* outWidth, int* outHeight) {
    if (!png_data || data_len <= 0) return 0;

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(png_data, data_len, &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return 0;
    }

    GLuint textureID = 0;
    glGenTextures(1, &textureID);
    if (textureID == 0) {
        stbi_image_free(pixels);
        return 0;
    }

    glBindTexture(GL_TEXTURE_2D, textureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);

    if (outWidth) *outWidth = width;
    if (outHeight) *outHeight = height;
    return textureID;
}

GLuint LoadTextureFromPNGArray(const unsigned char* png_data, int data_len) {
    return LoadTextureFromPNGArrayEx(png_data, data_len, &g_ButtonOrigWidth, &g_ButtonOrigHeight);
}

// Send the current authority table to a newly connected client.
static void QueueAllCurrentAuthorities() {
    if (!g_IsHost.load() || !g_IsConnected.load()) return;

    std::vector<VehicleAuthorityPacket> packets;
    {
        std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
        for (uint16_t vehicleId = 0; vehicleId < VEHICLE_SLOT_LIMIT; ++vehicleId) {
            const VehicleAuthorityState& auth = g_VehicleAuthority[vehicleId];
            if (auth.ownerId == VEHICLE_OWNER_NONE) continue;

            VehicleAuthorityPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.type = PACKET_VEHICLE_AUTHORITY;
            pkt.ownerId = auth.ownerId;
            pkt.vehicleId = vehicleId;
            pkt.generation = auth.generation;
            packets.push_back(pkt);
        }
    }

    if (!packets.empty()) {
        std::lock_guard<std::mutex> lock(g_VehicleSendMutex);
        g_PendingAuthorityPackets.insert(g_PendingAuthorityPackets.end(), packets.begin(), packets.end());
    }
}

// ========================================================================
// NETWORK THREADS
// ========================================================================
void NetworkLoop() {
    fcntl(g_TcpSocket, F_SETFL, O_NONBLOCK);

    uint8_t recvBuffer[4096];
    size_t bufferedBytes = 0;

    while (g_IsConnected.load()) {
        if (g_TcpSocket < 0) break;

        if (bufferedBytes < sizeof(recvBuffer)) {
            ssize_t bytesRead = recv(
                g_TcpSocket,
                recvBuffer + bufferedBytes,
                sizeof(recvBuffer) - bufferedBytes,
                0
            );

            if (bytesRead > 0) {
                bufferedBytes += (size_t)bytesRead;
            } else if (bytesRead == 0) {
                ShowNativeToast("Connection Lost (Other player left)!");
                g_IsConnected.store(false);
                break;
            } else if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
                ShowNativeToast("Error: Network Connection Lost!");
                g_IsConnected.store(false);
                break;
            }
        }

        while (bufferedBytes > 0) {
            const uint8_t packetType = recvBuffer[0];
            size_t packetSize = 0;

            if (packetType == PACKET_SESSION_WELCOME) {
                packetSize = sizeof(SessionWelcomePacket);
            } else if (packetType == PACKET_CHAT) {
                packetSize = sizeof(NetworkPacket);
            } else if (packetType == PACKET_VEHICLE_POSITION) {
                packetSize = sizeof(VehiclePositionPacket);
            } else if (packetType == PACKET_VEHICLE_CLAIM) {
                packetSize = sizeof(VehicleClaimPacket);
            } else if (packetType == PACKET_VEHICLE_AUTHORITY) {
                packetSize = sizeof(VehicleAuthorityPacket);
            } else if (packetType == PACKET_VEHICLE_RELEASE) {
                packetSize = sizeof(VehicleReleasePacket);
            } else if (packetType == PACKET_VEHICLE_SNAPSHOT) {
                if (bufferedBytes < sizeof(VehicleSnapshotHeader)) {
                    break;
                }

                VehicleSnapshotHeader header;
                memcpy(
                    &header,
                    recvBuffer,
                    sizeof(header)
                );

                if (header.count > VEHICLE_SNAPSHOT_MAX_COUNT) {
                    g_IsConnected.store(false);
                    break;
                }

                packetSize =
                    sizeof(VehicleSnapshotHeader) +
                    ((size_t)header.count * sizeof(VehicleSnapshotEntry));
            } else {
                g_IsConnected.store(false);
                break;
            }

            if (bufferedBytes < packetSize) break;

            if (packetType == PACKET_SESSION_WELCOME) {
                SessionWelcomePacket pkt;
                memcpy(&pkt, recvBuffer, sizeof(pkt));
                HandleSessionWelcome(pkt);
            } else if (packetType == PACKET_CHAT) {
                NetworkPacket pkt;
                memcpy(&pkt, recvBuffer, sizeof(pkt));
                pkt.senderName[sizeof(pkt.senderName) - 1] = '\0';
                pkt.chatData[sizeof(pkt.chatData) - 1] = '\0';

                std::string senderStr(pkt.senderName);
                std::string msgStr(pkt.chatData);
                std::string formattedMsg = senderStr + ": " + msgStr;

                {
                    std::lock_guard<std::mutex> lock(g_ChatMutex);
                    g_ChatMessages.push_back(formattedMsg);
                }

                ShowNativeToast(formattedMsg);
            } else if (packetType == PACKET_VEHICLE_POSITION) {
                VehiclePositionPacket pkt;
                memcpy(&pkt, recvBuffer, sizeof(pkt));
                HandleVehiclePositionPacket(pkt);
            } else if (packetType == PACKET_VEHICLE_SNAPSHOT) {
                VehicleSnapshotHeader header;
                memcpy(
                    &header,
                    recvBuffer,
                    sizeof(header)
                );

                HandleVehicleSnapshotPacket(
                    header,
                    recvBuffer + sizeof(header)
                );
            } else if (packetType == PACKET_VEHICLE_CLAIM) {
                VehicleClaimPacket pkt;
                memcpy(&pkt, recvBuffer, sizeof(pkt));
                HandleVehicleClaimPacket(pkt);
            } else if (packetType == PACKET_VEHICLE_AUTHORITY) {
                VehicleAuthorityPacket pkt;
                memcpy(&pkt, recvBuffer, sizeof(pkt));
                HandleVehicleAuthorityPacket(pkt);
            } else if (packetType == PACKET_VEHICLE_RELEASE) {
                VehicleReleasePacket pkt;
                memcpy(&pkt, recvBuffer, sizeof(pkt));
                HandleVehicleReleasePacket(pkt);
            }

            bufferedBytes -= packetSize;
            if (bufferedBytes > 0) {
                memmove(recvBuffer, recvBuffer + packetSize, bufferedBytes);
            }
        }

        std::string outMsg;
        bool hasOutMsg = false;
        {
            std::lock_guard<std::mutex> lock(g_OutgoingChatMutex);
            if (!g_OutgoingChats.empty()) {
                outMsg = g_OutgoingChats.front();
                g_OutgoingChats.erase(g_OutgoingChats.begin());
                hasOutMsg = true;
            }
        }

        if (hasOutMsg && g_IsConnected.load()) {
            NetworkPacket outPkt;
            memset(&outPkt, 0, sizeof(outPkt));
            outPkt.type = PACKET_CHAT;
            strncpy(outPkt.senderName, g_Nickname, sizeof(outPkt.senderName) - 1);
            strncpy(outPkt.chatData, outMsg.c_str(), sizeof(outPkt.chatData) - 1);
            if (!SendAllBytes(g_TcpSocket, &outPkt, sizeof(outPkt))) {
                g_IsConnected.store(false);
                break;
            }
        }

        if (g_IsConnected.load()) {
            VehicleClaimPacket claimPkt;
            bool hasClaim = false;
            {
                std::lock_guard<std::mutex> lock(g_VehicleSendMutex);
                if (g_HasPendingClaimPacket) {
                    claimPkt = g_PendingClaimPacket;
                    g_HasPendingClaimPacket = false;
                    hasClaim = true;
                }
            }

            if (hasClaim) {
                if (!SendAllBytes(g_TcpSocket, &claimPkt, sizeof(claimPkt))) {
                    g_IsConnected.store(false);
                    break;
                }
            }
        }

        if (g_IsConnected.load()) {
            VehicleReleasePacket releasePkt;
            bool hasRelease = false;
            {
                std::lock_guard<std::mutex> lock(g_VehicleSendMutex);
                if (g_HasPendingReleasePacket) {
                    releasePkt = g_PendingReleasePacket;
                    g_HasPendingReleasePacket = false;
                    hasRelease = true;
                }
            }

            if (hasRelease) {
                if (!SendAllBytes(g_TcpSocket, &releasePkt, sizeof(releasePkt))) {
                    g_IsConnected.store(false);
                    break;
                }
            }
        }

        if (g_IsConnected.load()) {
            std::vector<VehicleAuthorityPacket> authorityPackets;
            {
                std::lock_guard<std::mutex> lock(g_VehicleSendMutex);
                authorityPackets.swap(g_PendingAuthorityPackets);
            }

            for (size_t i = 0; i < authorityPackets.size() && g_IsConnected.load(); ++i) {
                if (!SendAllBytes(g_TcpSocket, &authorityPackets[i], sizeof(authorityPackets[i]))) {
                    g_IsConnected.store(false);
                    break;
                }
            }
        }

        // Vehicle network traffic is intentionally capped at 10 snapshots/sec.
        // All currently-known local vehicle states are packed into one TCP message.
        if (g_IsConnected.load()) {
            const uint64_t nowMs = GetMonotonicMilliseconds();

            if (g_LastVehicleNetworkSendMs == 0 ||
                (nowMs - g_LastVehicleNetworkSendMs) >= VEHICLE_NETWORK_INTERVAL_MS) {

                std::vector<VehicleSnapshotEntry> entries;
                entries.reserve(VEHICLE_SNAPSHOT_MAX_COUNT);

                {
                    std::lock_guard<std::mutex> lock(g_VehicleSendMutex);

                    for (uint16_t vehicleId = 0;
                         vehicleId < VEHICLE_SLOT_LIMIT &&
                         entries.size() < VEHICLE_SNAPSHOT_MAX_COUNT;
                         ++vehicleId) {

                        if (!g_HasLatestOutgoingVehicle[vehicleId]) {
                            continue;
                        }

                        const VehiclePositionPacket& srcPkt =
                            g_LatestOutgoingVehicles[vehicleId];

                        VehicleSnapshotEntry entry;
                        memset(&entry, 0, sizeof(entry));

                        entry.vehicleId = srcPkt.vehicleId;
                        entry.x = srcPkt.x;
                        entry.y = srcPkt.y;
                        entry.angle = srcPkt.angle;
                        entry.sequence = srcPkt.sequence;
                        entry.moving = srcPkt.moving;

                        entries.push_back(entry);
                    }
                }

                if (!entries.empty()) {
                    VehicleSnapshotHeader header;
                    memset(&header, 0, sizeof(header));

                    header.type = PACKET_VEHICLE_SNAPSHOT;
                    header.ownerId = g_LocalPlayerId.load();
                    header.count = (uint8_t)entries.size();
                    header.sequence =
                        g_LocalVehicleSequence.load();

                    std::vector<uint8_t> snapshotBuffer;
                    snapshotBuffer.resize(
                        sizeof(header) +
                        (entries.size() * sizeof(VehicleSnapshotEntry))
                    );

                    memcpy(
                        snapshotBuffer.data(),
                        &header,
                        sizeof(header)
                    );

                    memcpy(
                        snapshotBuffer.data() + sizeof(header),
                        entries.data(),
                        entries.size() * sizeof(VehicleSnapshotEntry)
                    );

                    if (!SendAllBytes(
                            g_TcpSocket,
                            snapshotBuffer.data(),
                            snapshotBuffer.size()
                        )) {

                        g_IsConnected.store(false);
                        break;
                    }
                }

                g_LastVehicleNetworkSendMs = nowMs;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void StartPONGResponderThread() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in server_addr, client_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(DISCOVERY_PORT);
    bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    
    char buffer[256];
    socklen_t client_len = sizeof(client_addr);
    while (true) {
        int n = recvfrom(sockfd, buffer, sizeof(buffer)-1, 0, (struct sockaddr*)&client_addr, &client_len);
        if (n > 0) {
            buffer[n] = '\0';
            if (strcmp(buffer, "FS14_PING") == 0) {
                if (g_IsHost.load()) {
                    std::string roomName = std::string(g_Nickname) + "'s Room";
                    std::string reply = "FS14_PONG|" + roomName;
                    sendto(sockfd, reply.c_str(), reply.length(), 0, (struct sockaddr*)&client_addr, client_len);
                }
            }
        }
    }
}

void StartLANDiscoveryThread() {
    if (!IsNetworkAvailable()) {
        ShowNativeToast("Error: Check your network connection!");
        g_IsSearching.store(false);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_PeerMutex);
        g_DiscoveredPeers.clear();
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    int broadcastEnable = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));
    
    struct sockaddr_in broadcast_addr;
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
    broadcast_addr.sin_port = htons(DISCOVERY_PORT);
    
    std::string pingMsg = "FS14_PING";
    sendto(sockfd, pingMsg.c_str(), pingMsg.length(), 0, (struct sockaddr*)&broadcast_addr, sizeof(broadcast_addr));
    
    struct timeval tv;
    tv.tv_sec = 2; tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    char buffer[256];
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);
    auto startTime = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startTime).count() < 3) {
        int n = recvfrom(sockfd, buffer, sizeof(buffer)-1, 0, (struct sockaddr*)&sender_addr, &sender_len);
        if (n > 0) {
            buffer[n] = '\0';
            std::string msg(buffer);
            if (msg.find("FS14_PONG|") == 0) {
                std::string roomName = msg.substr(10); 
                std::string devIP = inet_ntoa(sender_addr.sin_addr);
                
                if (IsLocalIP(devIP)) continue;

                std::lock_guard<std::mutex> lock(g_PeerMutex);
                bool exists = false;
                for (auto& peer : g_DiscoveredPeers) { if (peer.ip == devIP) { exists = true; break; } }
                if (!exists) g_DiscoveredPeers.push_back({devIP, roomName});
            }
        }
    }
    close(sockfd);
    g_IsSearching.store(false);
}

void TCPHostThread() {
    ResetVehicleSyncState();
    g_LocalPlayerId.store(0);

    if (!IsNetworkAvailable()) {
        ShowNativeToast("Error: Check your network connection!");
        g_ConnectedStatus = "Connection Error (No Network)";
        g_IsHost = false;
        return;
    }

    g_TcpServerFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(g_TcpServerFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(TCP_SYNC_PORT);
    bind(g_TcpServerFd, (struct sockaddr*)&address, sizeof(address));
    listen(g_TcpServerFd, 3);
    
    g_ConnectedStatus = "Host Started. Waiting for client...";
    ShowNativeToast("Room Created. Waiting for client...");
    int addrlen = sizeof(address);
    
    g_TcpSocket = accept(g_TcpServerFd, (struct sockaddr*)&address, (socklen_t*)&addrlen);
    
    if (g_TcpSocket >= 0 && g_IsHost) {
        SessionWelcomePacket welcome;
        memset(&welcome, 0, sizeof(welcome));
        welcome.type = PACKET_SESSION_WELCOME;
        welcome.ownerId = 1;
        welcome.maxPlayers = MAX_PLAYERS;

        if (!SendAllBytes(g_TcpSocket, &welcome, sizeof(welcome))) {
            ShowNativeToast("Error: Failed to initialize multiplayer session.");
            g_ConnectedStatus = "Connection Error";
        } else {
            g_IsConnected = true;
            g_ConnectedStatus = "Client Connected!";
            ShowNativeToast("Client Joined the Room!");
            QueueAllCurrentAuthorities();
            NetworkLoop();
        }
    }
    
    if (g_TcpSocket >= 0) { shutdown(g_TcpSocket, SHUT_RDWR); close(g_TcpSocket); g_TcpSocket = -1; }
    if (g_TcpServerFd >= 0) { shutdown(g_TcpServerFd, SHUT_RDWR); close(g_TcpServerFd); g_TcpServerFd = -1; }
    
    g_IsConnected = false;
    g_IsHost = false;
    ResetVehicleSyncState();
    if (g_ConnectedStatus != "Room Closed.") g_ConnectedStatus = "Connection Lost.";
}

void TCPClientThread(std::string hostIP) {
    ResetVehicleSyncState();
    g_LocalPlayerId.store(0xFF);

    g_TcpSocket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(TCP_SYNC_PORT);
    
    if (inet_pton(AF_INET, hostIP.c_str(), &serv_addr.sin_addr) <= 0) {
        ShowNativeToast("Error: Invalid IP!");
        g_IsClient = false;
        return;
    }

    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(g_TcpSocket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    g_ConnectedStatus = "Connecting to room...";
    ShowNativeToast("Connecting to room: " + hostIP);

    if (connect(g_TcpSocket, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) >= 0) {
        g_IsConnected = true;
        g_ConnectedStatus = "Connected to Host!";
        ShowNativeToast("Successfully Joined Room!"); 
        NetworkLoop(); 
    } else {
        ShowNativeToast("Error: Could not connect to room!");
        g_ConnectedStatus = "Connection Failed";
    }
    
    if (g_TcpSocket >= 0) { shutdown(g_TcpSocket, SHUT_RDWR); close(g_TcpSocket); g_TcpSocket = -1; }
    g_IsConnected = false;
    g_IsClient = false;
    ResetVehicleSyncState();
}

// ========================================================================
// HOOKS AND RENDERING
// ========================================================================
int32_t my_AInputQueue_getEvent(void* queue, AInputEvent** outEvent) {
    int32_t result;
    while (true) {
        result = orig_AInputQueue_getEvent(queue, outEvent);
        if (result >= 0 && outEvent != nullptr && *outEvent != nullptr) {
            int32_t eventType = AInputEvent_getType(*outEvent);

            if (eventType == AINPUT_EVENT_TYPE_MOTION) {
                int32_t action = AMotionEvent_getAction(*outEvent) & AMOTION_EVENT_ACTION_MASK;
                
                if (action == AMOTION_EVENT_ACTION_CANCEL) {
                    g_TouchDown.store(false);
                    g_IsEatingTouch = false;
                    void* finishEventAddr = dlsym(RTLD_DEFAULT, "AInputQueue_finishEvent");
                    if (finishEventAddr) {
                        typedef void (*finish_t)(void*, AInputEvent*, int);
                        ((finish_t)finishEventAddr)(queue, *outEvent, 1);
                    }
                    continue; 
                }
                
                size_t pointerCount = AMotionEvent_getPointerCount(*outEvent); 
                if (pointerCount > 0) {
                    if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_MOVE) {
                        g_TouchX.store(AMotionEvent_getX(*outEvent, 0));
                        g_TouchY.store(AMotionEvent_getY(*outEvent, 0));
                        g_TouchDown.store(true);
                    } else if (action == AMOTION_EVENT_ACTION_UP) {
                        g_TouchDown.store(false);
                    }
                }

                bool shouldEatEvent = false;
                if (g_ImGuiInitialized && ImGui::GetCurrentContext() != nullptr) {
                    ImGuiIO& io = ImGui::GetIO();
                    if (action == AMOTION_EVENT_ACTION_DOWN) {
                        g_IsEatingTouch = io.WantCaptureMouse;
                    }
                    if (g_IsEatingTouch || io.WantCaptureMouse) {
                        shouldEatEvent = true;
                    }
                    if (action == AMOTION_EVENT_ACTION_UP) {
                        if (g_IsEatingTouch) shouldEatEvent = true;
                        g_IsEatingTouch = false; 
                    }
                }

                if (shouldEatEvent) {
                    void* finishEventAddr = dlsym(RTLD_DEFAULT, "AInputQueue_finishEvent");
                    if (finishEventAddr) {
                        typedef void (*finish_t)(void*, AInputEvent*, int);
                        ((finish_t)finishEventAddr)(queue, *outEvent, 1);
                    }
                    continue; 
                }
            }
            else if (eventType == AINPUT_EVENT_TYPE_KEY) {
                int32_t action = AKeyEvent_getAction(*outEvent);
                int32_t keyCode = AKeyEvent_getKeyCode(*outEvent);
                int32_t metaState = AKeyEvent_getMetaState(*outEvent);

                bool shouldEatEvent = false;

                // Android BACK: first close the soft keyboard, otherwise close our MP menu.
                if (keyCode == AKEYCODE_BACK && action == AKEY_EVENT_ACTION_DOWN &&
                    g_ImGuiInitialized && ImGui::GetCurrentContext() != nullptr) {
                    if (g_IsMultiplayerMenuActive) {
                        if (g_AndroidKeyboardOpen.load()) {
                            CloseAndroidKeyboard();
                        } else {
                            g_IsMultiplayerMenuActive = false;
                        }
                        shouldEatEvent = true;
                    }
                }

                if (g_ImGuiInitialized && ImGui::GetCurrentContext() != nullptr) {
                    ImGuiIO& io = ImGui::GetIO();
                    bool isDown = (action == AKEY_EVENT_ACTION_DOWN);

                    if (keyCode == AKEYCODE_DEL) {
                        io.AddKeyEvent(ImGuiKey_Backspace, isDown);
                    } else if (keyCode == AKEYCODE_ENTER || keyCode == AKEYCODE_NUMPAD_ENTER) {
                        io.AddKeyEvent(ImGuiKey_Enter, isDown);
                    } else if (keyCode == AKEYCODE_DPAD_LEFT) {
                        io.AddKeyEvent(ImGuiKey_LeftArrow, isDown);
                    } else if (keyCode == AKEYCODE_DPAD_RIGHT) {
                        io.AddKeyEvent(ImGuiKey_RightArrow, isDown);
                    }

                    if (isDown) {
                        bool isShift = (metaState & AMETA_SHIFT_ON) != 0;
                        char c = 0;

                        if (keyCode >= AKEYCODE_A && keyCode <= AKEYCODE_Z) {
                            c = (isShift ? 'A' : 'a') + (keyCode - AKEYCODE_A);
                        } else if (keyCode >= AKEYCODE_0 && keyCode <= AKEYCODE_9) {
                            c = '0' + (keyCode - AKEYCODE_0);
                        } else if (keyCode == AKEYCODE_SPACE) { c = ' '; }
                          else if (keyCode == AKEYCODE_PERIOD) { c = '.'; }
                          else if (keyCode == AKEYCODE_COMMA) { c = ','; }
                          else if (keyCode == AKEYCODE_MINUS) { c = (isShift ? '_' : '-'); }
                          else if (keyCode == AKEYCODE_EQUALS) { c = (isShift ? '+' : '='); }
                          else if (keyCode == AKEYCODE_SLASH) { c = (isShift ? '?' : '/'); }
                        
                        if (c != 0) {
                            io.AddInputCharacter(c);
                        }
                    }

                    if (io.WantCaptureKeyboard) {
                        shouldEatEvent = true;
                    }
                }

                if (shouldEatEvent) {
                    void* finishEventAddr = dlsym(RTLD_DEFAULT, "AInputQueue_finishEvent");
                    if (finishEventAddr) {
                        typedef void (*finish_t)(void*, AInputEvent*, int);
                        ((finish_t)finishEventAddr)(queue, *outEvent, 1);
                    }
                    continue; 
                }
            }
        }
        break; 
    }
    return result; 
}

// ========================================================================
// GAME-STYLE MENU UI HELPERS
// ========================================================================
static ImTextureID ToImGuiTexture(GLuint texture) {
    return (ImTextureID)(intptr_t)texture;
}

static void InitGameUIFont() {
    if (g_GameUIFontInitialized) return;
    g_GameUIFontInitialized = true;

    ImGuiIO& io = ImGui::GetIO();
    static const ImWchar gameRanges[] = {
        0x0020, 0x024F, // Latin + Latin Extended
        0x00C0, 0x00FF, // Latin-1 supplement (kept explicit for older ImGui builds)
        0
    };

    const char* paths[] = {
        "/system/fonts/RobotoCondensed-Regular.ttf",
        "/system/fonts/Roboto-Regular.ttf",
        "/system/fonts/DroidSans.ttf",
        "/system/fonts/NotoSans-Regular.ttf"
    };

    const float fontPx = 31.0f;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (access(paths[i], R_OK) == 0) {
            g_GameUIFont = io.Fonts->AddFontFromFileTTF(paths[i], fontPx, nullptr, gameRanges);
            if (g_GameUIFont != nullptr) break;
        }
    }

    if (g_GameUIFont == nullptr) {
        g_GameUIFont = io.Fonts->AddFontDefault();
    }
}

static bool IsInsideGameButtonTexture(ImVec2 pos, ImVec2 size, bool rightAligned) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (mouse.x < pos.x || mouse.x > pos.x + size.x ||
        mouse.y < pos.y || mouse.y > pos.y + size.y) {
        return false;
    }

    float u = (mouse.x - pos.x) / std::max(1.0f, size.x);
    float v = (mouse.y - pos.y) / std::max(1.0f, size.y);
    if (rightAligned) u = 1.0f - u;

    if (v < 0.245f || v > 0.765f) return false;
    const float t = (v - 0.245f) / (0.765f - 0.245f);
    const float left = 0.006f;
    const float right = 0.990f - 0.080f * t;
    return u >= left && u <= right;
}

static bool DrawGameStyleButton(const char* id, const char* label, ImVec2 size,
                                float textScale = 1.0f, bool rightAligned = false) {
    ImVec2 pos = ImGui::GetCursorScreenPos();

    const float paintTop = 0.245f;
    const float paintBottom = 0.765f;
    const float paintH = size.y * (paintBottom - paintTop);
    const ImVec2 hitPos(pos.x, pos.y + size.y * paintTop);
    ImGui::SetCursorScreenPos(hitPos);
    ImGui::InvisibleButton(id, ImVec2(size.x, std::max(1.0f, paintH)));
    const bool hovered = ImGui::IsItemHovered() && IsInsideGameButtonTexture(pos, size, rightAligned);
    const bool active = hovered && ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) &&
                         IsInsideGameButtonTexture(pos, size, rightAligned);

    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(size);

    ImDrawList* draw = ImGui::GetWindowDrawList();

    if (g_MultiplayerButtonTexture != 0) {
        if (!rightAligned) {
            draw->AddImage(ToImGuiTexture(g_MultiplayerButtonTexture),
                           pos, ImVec2(pos.x + size.x, pos.y + size.y),
                           ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
        } else {
            draw->AddImage(ToImGuiTexture(g_MultiplayerButtonTexture),
                           pos, ImVec2(pos.x + size.x, pos.y + size.y),
                           ImVec2(1.0f, 0.0f), ImVec2(0.0f, 1.0f));
        }

        if (hovered) {
            draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                                IM_COL32(255, 255, 255, active ? 26 : 12));
        }
    } else {
        draw->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                            IM_COL32(20, 24, 28, 205), 2.0f);
    }

    ImFont* font = g_GameUIFont ? g_GameUIFont : ImGui::GetFont();
    const float textSizePx = std::max(31.0f, 36.0f * textScale *
                                             std::max(0.90f, std::min(1.12f, ImGui::GetIO().DisplaySize.y / 1080.0f)));
    ImVec2 measured = font->CalcTextSizeA(textSizePx, FLT_MAX, 0.0f, label);
    ImVec2 textPos(
        pos.x + (size.x - measured.x) * 0.5f,
        pos.y + (size.y - measured.y) * 0.5f - 1.0f
    );
    draw->AddText(font, textSizePx, textPos,
                  IM_COL32(245, 245, 245, 255), label);
    return clicked;
}

static bool DrawCenteredMirroredGameButton(const char* id, const char* label,
                                           float totalW, float h, float textScale = 1.34f) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();

    const float overlap = std::min(42.0f, totalW * 0.055f);
    const float halfW = (totalW + overlap) * 0.5f;
    const float actualTotal = halfW * 2.0f - overlap;

    const float paintTop = 0.245f;
    const float paintBottom = 0.765f;
    const float hitH = std::max(1.0f, h * (paintBottom - paintTop));
    const ImVec2 hitStart(start.x, start.y + h * paintTop);
    ImGui::SetCursorScreenPos(hitStart);
    ImGui::InvisibleButton(id, ImVec2(actualTotal, hitH));
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    if (clicked) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float relX = mouse.x - start.x;
        const float relY = mouse.y - start.y;
        bool inVisible = false;
        for (int side = 0; side < 2; ++side) {
            const float localX = side == 0 ? relX : (actualTotal - relX);
            if (localX < 0.0f || localX > halfW) continue;
            const float u = localX / std::max(1.0f, halfW);
            const float v = relY / std::max(1.0f, h);
            if (v >= paintTop && v <= paintBottom) {
                const float t = (v - paintTop) / (paintBottom - paintTop);
                const float right = 0.990f - 0.080f * t;
                if (u >= 0.006f && u <= right) {
                    inVisible = true;
                    break;
                }
            }
        }
        clicked = inVisible;
    }

    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(actualTotal, h));

    if (g_MultiplayerButtonTexture != 0) {
        // Sol taraf: Aynalı (Çıkıntısı sola/dışa bakar)
        draw->AddImage(ToImGuiTexture(g_MultiplayerButtonTexture),
                       start, ImVec2(start.x + halfW, start.y + h),
                       ImVec2(1.0f, 0.0f), ImVec2(0.0f, 1.0f));
        // Sağ taraf: Düz (Çıkıntısı sağa/dışa bakar)
        draw->AddImage(ToImGuiTexture(g_MultiplayerButtonTexture),
                       ImVec2(start.x + halfW - overlap, start.y),
                       ImVec2(start.x + actualTotal, start.y + h),
                       ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
    }

    ImFont* font = g_GameUIFont ? g_GameUIFont : ImGui::GetFont();
    const float textSizePx = std::max(31.0f, 36.0f * textScale *
                                             std::max(0.90f, std::min(1.12f, ImGui::GetIO().DisplaySize.y / 1080.0f)));
    ImVec2 measured = font->CalcTextSizeA(textSizePx, FLT_MAX, 0.0f, label);
    draw->AddText(font, textSizePx,
                  ImVec2(start.x + (actualTotal - measured.x) * 0.5f,
                         start.y + (h - measured.y) * 0.5f - 1.0f),
                  IM_COL32(245,245,245,255), label);
    return clicked;
}

static void DrawFadingHeaderLine(float startX, float y, float endX) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float thick = std::max(5.5f, ImGui::GetIO().DisplaySize.y * 0.0050f);
    
    // Soldan sağa yumuşak geçişli (fade-out) çizgi
    draw->AddRectFilledMultiColor(
        ImVec2(startX, y - thick * 0.5f), ImVec2(endX, y + thick * 0.5f),
        IM_COL32(255, 255, 255, 222), IM_COL32(255, 255, 255, 0),
        IM_COL32(255, 255, 255, 222), IM_COL32(255, 255, 255, 0));
}

static void DrawGameSectionTitle(const char* title, float width) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    draw->AddText(g_GameUIFont, 30.0f, p, IM_COL32(255, 255, 255, 255), title);
    const float lineY = p.y + 36.0f;
    DrawFadingHeaderLine(p.x, lineY, p.x + width);
    ImGui::Dummy(ImVec2(width, 46.0f));
}

static void DrawGameStatusText(const char* label, const std::string& value, float width) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    draw->AddText(g_GameUIFont, 25.0f, p, IM_COL32(225, 225, 225, 255), label);
    ImVec2 labelSize = ImGui::CalcTextSize(label);
    draw->AddText(g_GameUIFont, 25.0f,
                  ImVec2(p.x + labelSize.x + 10.0f, p.y),
                  IM_COL32(130, 225, 245, 255), value.c_str());
    ImGui::Dummy(ImVec2(width, 34.0f));
}

static void DrawGamePanel(ImVec2 minPos, ImVec2 maxPos, int alpha = 88) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(minPos, maxPos, IM_COL32(3, 7, 10, alpha), 3.0f);
    draw->AddRect(minPos, maxPos, IM_COL32(255, 255, 255, 28), 3.0f, 0, 1.0f);
}

static void DrawHeaderDarkening(const ImVec2& screen) {
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float bandH = std::min(210.0f, screen.y * 0.28f);

    draw->AddRectFilled(ImVec2(0.0f, 0.0f), screen, IM_COL32(0, 0, 0, 34));

    draw->AddRectFilledMultiColor(
        ImVec2(0.0f, 0.0f), ImVec2(screen.x, bandH),
        IM_COL32(0,0,0,104), IM_COL32(0,0,0,104),
        IM_COL32(0,0,0,20), IM_COL32(0,0,0,20));
}

void DrawImGui() {
    if (!g_ImGuiInitialized) {
        ImGui::CreateContext();
        InitGameUIFont();
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 0.0f;
        style.ChildRounding = 2.0f;
        style.FrameRounding = 2.0f;
        style.ScrollbarRounding = 2.0f;
        style.WindowBorderSize = 0.0f;
        style.FrameBorderSize = 0.0f;
        style.ItemSpacing = ImVec2(7.0f, 5.0f);
        style.ItemInnerSpacing = ImVec2(6.0f, 5.0f);
        style.FramePadding = ImVec2(10.0f, 8.0f);
        ImGui_ImplOpenGL3_Init("#version 100");
        g_ImGuiInitialized = true;
    }

    if (!g_ImGuiInitialized) return;
    if (!g_GameUIFontInitialized) InitGameUIFont();

    if (!g_TextureLoaded) {
        g_MultiplayerButtonTexture = LoadTextureFromPNGArray(buton_png_data, buton_png_len);
        g_TextureLoaded = (g_MultiplayerButtonTexture != 0);
    }
    if (!g_GameMenuBackgroundLoaded) {
        g_GameMenuBackgroundTexture = LoadTextureFromPNGArrayEx(
            game_menu_bg_png_data, game_menu_bg_png_len,
            &g_GameMenuBackgroundWidth, &g_GameMenuBackgroundHeight);
        g_GameMenuBackgroundLoaded = (g_GameMenuBackgroundTexture != 0);
    }

    ImGuiIO& io = ImGui::GetIO();
    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    const ImVec2 screen((float)viewport[2], (float)viewport[3]);
    io.DisplaySize = screen;
    io.DeltaTime = 1.0f / 60.0f;
    io.FontGlobalScale = 1.0f;

    io.AddMousePosEvent(g_TouchX.load(), g_TouchY.load());
    io.AddMouseButtonEvent(0, g_TouchDown.load());

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    if (g_CurrentMenu != MENU_INGAME && !g_IsMultiplayerMenuActive && g_MultiplayerButtonTexture != 0) {
        const bool isSettings = (g_CurrentMenu == MENU_SETTINGS);
        const char* entryLabel = isSettings ? "Create Room" : "Join Room";
        float entryW = isSettings
                         ? std::min(680.0f, std::max(440.0f, screen.x * 0.36f))
                         : std::min(620.0f, std::max(420.0f, screen.x * 0.32f));
        float entryH = isSettings ? entryW / 5.80f : entryW / 2.72f;
        const float entryX = isSettings
                             ? (screen.x - entryW) * 0.5f
                             : -10.0f;
        const float entryY = isSettings
                             ? std::max(8.0f, screen.y - entryH - screen.y * 0.075f)
                             : std::max(3.0f, screen.y * 0.005f);

        ImGui::SetNextWindowPos(ImVec2(entryX, entryY), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(entryW, entryH), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##MPEntryButton", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
        const bool entryClicked = isSettings
            ? DrawCenteredMirroredGameButton("##RoomEntryCreate", entryLabel, entryW, entryH, 1.36f)
            : DrawGameStyleButton("##RoomEntryJoin", entryLabel, ImVec2(entryW, entryH), 1.34f, false);
        if (entryClicked) {
            g_CurrentMenu = isSettings ? MENU_SETTINGS : MENU_SAVELOAD;
            g_IsMultiplayerMenuActive = true;
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    if (g_IsMultiplayerMenuActive) {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        if (g_GameMenuBackgroundTexture != 0 &&
            g_GameMenuBackgroundWidth > 0 && g_GameMenuBackgroundHeight > 0) {
            float scale = screen.x / (float)g_GameMenuBackgroundWidth;
            const float drawW = g_GameMenuBackgroundWidth * scale;
            const float drawH = g_GameMenuBackgroundHeight * scale;
            float drawY = screen.y - drawH;
            if (drawY > 0.0f) drawY = 0.0f;
            bg->AddImage(ToImGuiTexture(g_GameMenuBackgroundTexture),
                         ImVec2(0.0f, drawY), ImVec2(drawW, drawY + drawH),
                         ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f));
            DrawHeaderDarkening(screen);
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(screen, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##GameStyleMultiplayerRoot", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);

        ImGui::PushFont(g_GameUIFont);

        const float edgeBleed = -10.0f;

        const char* title = (g_CurrentMenu == MENU_SAVELOAD)
                                ? "Multiplayer Server Browser"
                                : "Multiplayer Room";
        const float titleSize = std::max(44.0f, std::min(54.0f, screen.y * 0.052f));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float titleWidth = g_GameUIFont->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, title).x;
        const float titleRight = screen.x - 18.0f;
        const float titleX = std::max(24.0f, titleRight - titleWidth);
        const float titleY = 9.0f;
        draw->AddText(g_GameUIFont, titleSize, ImVec2(titleX, titleY),
                      IM_COL32(255,255,255,255), title);
        const float lineRight = screen.x - 8.0f;
        const float lineLeft = std::max(screen.x * 0.36f, titleX - screen.x * 0.40f);
        DrawFadingHeaderLine(lineLeft, titleY + titleSize + 9.0f, lineRight);

        const float commonButtonW = std::min(560.0f, std::max(440.0f, screen.x * 0.30f));
        const float commonButtonH = commonButtonW / 3.05f;

        const float headerBottom = titleY + titleSize + 28.0f;
        const float actionY = std::max(headerBottom - 9.0f, 58.0f);
        const float backX = screen.x - commonButtonW - edgeBleed;
        const float buttonY = actionY - 10.0f;
        ImGui::SetCursorPos(ImVec2(backX, buttonY));
        if (DrawGameStyleButton("##BackButton", "Back", ImVec2(commonButtonW, commonButtonH), 1.34f, true)) {
            if (g_AndroidKeyboardOpen.load()) CloseAndroidKeyboard();
            g_IsMultiplayerMenuActive = false;
        }

        const float bottomMargin = std::max(18.0f, screen.y * 0.03f);
        const float bodyTop = actionY + commonButtonH + 10.0f;
        const float bodyBottom = screen.y - bottomMargin;

        const float chatX = screen.x * 0.50f;
        const float chatW = screen.x - chatX;
        const float controlW = chatX;
        DrawGamePanel(ImVec2(0.0f, bodyTop), ImVec2(controlW, bodyBottom), 112);
        DrawGamePanel(ImVec2(chatX, bodyTop), ImVec2(screen.x, bodyBottom), 112);

        auto RenderChatUI = [&](float x, float y, float width, float height) {
            const float inner = 14.0f;
            ImGui::SetCursorPos(ImVec2(x + inner, y + 12.0f));
            DrawGameSectionTitle("Chat", width - inner * 2.0f);

            // Büyütülen Send butonu boyutları
            const float inputH = std::max(60.0f, std::min(84.0f, commonButtonH * 0.34f));
            const float sendW = std::min(145.0f, width * 0.26f);
            const float sendH = inputH + 4.0f;
            const float historyH = std::max(90.0f, height - inputH - 76.0f);
            const float historyW = width - inner * 2.0f;

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
            ImGui::BeginChild("##ChatHistory", ImVec2(historyW, historyH), false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
            {
                std::lock_guard<std::mutex> lock(g_ChatMutex);
                for (const auto& msg : g_ChatMessages) {
                    ImGui::TextWrapped("%s", msg.c_str());
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
                    ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            const float inputY = y + height - inputH - 10.0f;
            ImGui::SetCursorPos(ImVec2(x + inner, inputY));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.015f, 0.022f, 0.028f, 0.82f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.025f, 0.040f, 0.050f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.035f, 0.060f, 0.070f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            
            // Input genişliği Send butonuna göre ayarlandı
            ImGui::SetNextItemWidth(std::max(160.0f, width - inner * 2.0f - sendW - 12.0f));
            static char inputBuffer[200] = "";
            const bool chatConnected = g_IsConnected.load();
            if (!chatConnected) ImGui::BeginDisabled(true);
            bool enterPressed = ImGui::InputText("##ChatInput", inputBuffer,
                                                 IM_ARRAYSIZE(inputBuffer),
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
            if (chatConnected && ImGui::IsItemClicked()) OpenAndroidKeyboard();
            const bool chatInputActive = chatConnected && ImGui::IsItemActive();
            if (!chatConnected) ImGui::EndDisabled();
            ImGui::PopStyleColor(4);

            // Send butonu tam sağa dayandı
            const float sendX = x + width - sendW;
            ImGui::SetCursorPos(ImVec2(sendX, inputY - 2.0f));
            if (!chatConnected) ImGui::BeginDisabled(true);
            const bool sendClicked = DrawGameStyleButton("##SendButton", "Send", ImVec2(sendW, sendH), 1.15f, true);
            if (!chatConnected) ImGui::EndDisabled();
            if (chatConnected && (sendClicked || enterPressed)) {
                if (strlen(inputBuffer) > 0) {
                    const std::string msgStr(inputBuffer);
                    {
                        std::lock_guard<std::mutex> lock(g_ChatMutex);
                        g_ChatMessages.push_back("You: " + msgStr);
                    }
                    {
                        std::lock_guard<std::mutex> lock(g_OutgoingChatMutex);
                        g_OutgoingChats.push_back(msgStr);
                    }
                    memset(inputBuffer, 0, sizeof(inputBuffer));
                }
                if (chatInputActive || g_AndroidKeyboardOpen.load()) CloseAndroidKeyboard();
            }
        };

        if (g_CurrentMenu == MENU_SETTINGS) {
            ImGui::SetCursorPos(ImVec2(edgeBleed, buttonY));
            const float innerButtonW = commonButtonW;
            const float innerButtonH = commonButtonH;
            if (g_IsClient && g_IsConnected) {
                if (DrawGameStyleButton("##LeaveRoom", "Leave Room",
                                        ImVec2(innerButtonW, innerButtonH), 1.30f, false)) {
                    ClearChat();
                    g_IsClient = false;
                    g_IsConnected = false;
                    if (g_TcpSocket >= 0) {
                        shutdown(g_TcpSocket, SHUT_RDWR);
                        close(g_TcpSocket);
                        g_TcpSocket = -1;
                    }
                    g_ConnectedStatus = "Left the room.";
                }
            } else if (!g_IsHost) {
                if (DrawGameStyleButton("##HostRoom", "Host Room",
                                        ImVec2(innerButtonW, innerButtonH), 1.30f, false)) {
                    ClearChat();
                    g_IsHost = true;
                    std::thread(TCPHostThread).detach();
                }
            } else {
                if (DrawGameStyleButton("##CloseRoom", "Close Room",
                                        ImVec2(innerButtonW, innerButtonH), 1.30f, false)) {
                    ClearChat();
                    g_IsHost = false;
                    g_IsConnected = false;
                    if (g_TcpServerFd >= 0) {
                        shutdown(g_TcpServerFd, SHUT_RDWR);
                        close(g_TcpServerFd);
                        g_TcpServerFd = -1;
                    }
                    if (g_TcpSocket >= 0) {
                        shutdown(g_TcpSocket, SHUT_RDWR);
                        close(g_TcpSocket);
                        g_TcpSocket = -1;
                    }
                    g_ConnectedStatus = "Room Closed.";
                }
            }

            const float infoX = 18.0f;
            const float infoW = std::max(260.0f, controlW - 36.0f);
            ImGui::SetCursorPos(ImVec2(infoX, bodyTop + 12.0f));
            DrawGameSectionTitle("Room Info", infoW);
            DrawGameStatusText("Status:", g_ConnectedStatus, infoW);
            ImGui::Text("Room");
            ImGui::TextDisabled("%s's Room", g_Nickname);
            const uint8_t hostPlayerId = g_LocalPlayerId.load();
            if (hostPlayerId < MAX_PLAYERS) ImGui::Text("Player %u", (unsigned)(hostPlayerId + 1));
            else ImGui::TextDisabled("Player -");
            ImGui::TextDisabled("Up to %u players", (unsigned)MAX_PLAYERS);

            RenderChatUI(chatX, bodyTop, chatW, bodyBottom - bodyTop);
        } else if (g_CurrentMenu == MENU_SAVELOAD) {
            ImGui::SetCursorPos(ImVec2(edgeBleed, buttonY));
            if (DrawGameStyleButton("##ScanNetworks",
                                    g_IsSearching.load() ? "Searching..." : "Scan Networks",
                                    ImVec2(commonButtonW, commonButtonH), 1.30f, false)) {
                if (!g_IsSearching.load()) {
                    g_IsSearching.store(true);
                    std::thread(StartLANDiscoveryThread).detach();
                }
            }

            const float infoX = 18.0f;
            const float infoW = std::max(260.0f, controlW - 36.0f);
            ImGui::SetCursorPos(ImVec2(infoX, bodyTop + 12.0f));
            DrawGameSectionTitle("Player", infoW);
            DrawGameStatusText("Network:", g_ConnectedStatus, infoW);
            ImGui::Text("Username");
            ImGui::SetNextItemWidth(std::max(180.0f, infoW * 0.56f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.015f, 0.022f, 0.028f, 0.82f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.72f, 0.82f, 0.88f, 0.34f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            
            // İsim Kutusu: Dokunulduğunda/Odaklandığında Android Klavesi tetiklenir
            bool nickEnterPressed = ImGui::InputText("##NicknameInput", g_Nickname,
                                                     IM_ARRAYSIZE(g_Nickname),
                                                     ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::IsItemClicked() || ImGui::IsItemActivated()) {
                OpenAndroidKeyboard();
            }
            if (nickEnterPressed && g_AndroidKeyboardOpen.load()) CloseAndroidKeyboard();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);

            // Discovered Rooms başlığı aşağıya taşındı (bodyTop + 200.0f)
            ImGui::SetCursorPos(ImVec2(infoX, bodyTop + 200.0f));
            DrawGameSectionTitle("Discovered Rooms", infoW);
            const float listH = std::max(100.0f, bodyBottom - ImGui::GetCursorScreenPos().y - 12.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
            ImGui::BeginChild("##RoomsList", ImVec2(infoW, listH), false,
                              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
            if (!g_IsConnected) {
                std::lock_guard<std::mutex> lock(g_PeerMutex);
                if (g_DiscoveredPeers.empty()) {
                    ImGui::TextDisabled("No active rooms found yet.");
                } else {
                    const float roomW = std::min(commonButtonW, infoW);
                    const float roomH = commonButtonH;
                    for (size_t i = 0; i < g_DiscoveredPeers.size(); ++i) {
                        std::string roomLabel = g_DiscoveredPeers[i].name;
                        if (roomLabel.empty()) roomLabel = "Room";
                        char roomId[32];
                        snprintf(roomId, sizeof(roomId), "##Room%u", static_cast<unsigned int>(i));
                        if (DrawGameStyleButton(roomId, roomLabel.c_str(),
                                                ImVec2(roomW, roomH), 1.06f, false)) {
                            if (!g_IsHost.load() && !g_IsClient.load()) {
                                ClearChat();
                                g_IsClient.store(true);
                                std::string targetIP = g_DiscoveredPeers[i].ip;
                                std::thread(TCPClientThread, targetIP).detach();
                            }
                        }
                        ImGui::TextDisabled("%s", g_DiscoveredPeers[i].ip.c_str());
                        ImGui::Spacing();
                    }
                }
            } else {
                ImGui::Text("Connected To Room");
                ImGui::Spacing();
                if (DrawGameStyleButton("##JoinGame", "Join Game",
                                        ImVec2(std::min(commonButtonW, infoW), commonButtonH), 1.20f, false)) {
                    AutoStartGameForClient();
                }
                ImGui::Spacing();
                if (DrawGameStyleButton("##Disconnect", "Disconnect",
                                        ImVec2(std::min(commonButtonW, infoW), commonButtonH), 1.20f, false)) {
                    ClearChat();
                    g_IsHost = false;
                    g_IsClient = false;
                    g_IsConnected = false;
                    if (g_TcpSocket >= 0) {
                        shutdown(g_TcpSocket, SHUT_RDWR);
                        close(g_TcpSocket);
                        g_TcpSocket = -1;
                    }
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();

            RenderChatUI(chatX, bodyTop, chatW, bodyBottom - bodyTop);
        }

        ImGui::PopFont();
        ImGui::End();
        ImGui::PopStyleVar();
    }

    ImGui::Render();

    GLboolean depthTestEnabled = glIsEnabled(GL_DEPTH_TEST);
    GLboolean cullFaceEnabled = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (depthTestEnabled) glEnable(GL_DEPTH_TEST);
    if (cullFaceEnabled) glEnable(GL_CULL_FACE);
}

// ========================================================================
// RENDER HOOKS
// UI/render layer merged from arayüz.cpp; game/network hooks retained.
// ========================================================================
void my_GameUpdate(void* thiz, float param_1) {
    g_EngineInstance = (uintptr_t)thiz; 
    g_CurrentMenu = MENU_INGAME; 
    if (orig_GameUpdate) orig_GameUpdate(thiz, param_1);
}

void my_GameUpdateStateBase(void* thiz, float param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4) {
    g_EngineInstance = (uintptr_t)thiz;

    if (orig_GameUpdateStateBase) {
        orig_GameUpdateStateBase(thiz, param_1, param_2, param_3, param_4);
    }

    // The game itself decides the real frame/vsync cadence. The C output shows
    // Game::update() ends with waitVSync(), so this hook must not try to force
    // the engine to 120 FPS. We sample at up to 120 Hz when callbacks allow it,
    // and the network is independently capped at 10 snapshots/sec.
    ApplyRemoteVehicleStates(g_EngineInstance);
    CaptureAndQueueLocalVehicleState(g_EngineInstance);
}

void* my_updateGUI(void* thiz, void* p1, void* p2, void* p3, void* p4) {
    g_HUDInstance = (uintptr_t)thiz;

    if (!g_TextureLoaded) {
        g_MultiplayerButtonTexture = LoadTextureFromPNGArray(buton_png_data, buton_png_len);
        g_TextureLoaded = (g_MultiplayerButtonTexture != 0);
    }

    if (!g_GameMenuBackgroundLoaded) {
        g_GameMenuBackgroundTexture = LoadTextureFromPNGArrayEx(
            game_menu_bg_png_data, game_menu_bg_png_len,
            &g_GameMenuBackgroundWidth, &g_GameMenuBackgroundHeight
        );
        g_GameMenuBackgroundLoaded = (g_GameMenuBackgroundTexture != 0);
    }
    return orig_updateGUI ? orig_updateGUI(thiz, p1, p2, p3, p4) : nullptr;
}

void* my_renderMenu(void* thiz, void* p1, void* p2, void* p3) {
    g_MenuInstance = (uintptr_t)thiz;

    void* ret = nullptr;
    if (orig_renderMenu) ret = orig_renderMenu(thiz, p1, p2, p3);
    g_CurrentMenu = MENU_SAVELOAD;
    DrawImGui();
    return ret; 
}

void* my_renderStartMenuMain(void* thiz, void* p1, void* p2, void* p3) {
    g_StartMenuInstance = (uintptr_t)thiz;

    void* ret = nullptr;
    if (orig_renderStartMenuMain) ret = orig_renderStartMenuMain(thiz, p1, p2, p3);
    g_CurrentMenu = MENU_SETTINGS;
    DrawImGui();
    return ret; 
}

// ========================================================================
// MAIN MOD INITIALIZER
// ========================================================================
__attribute__((constructor))
void ModMain() {
    LOGI(">>> MULTIPLAYER MOD STARTING <<<");

    ResetVehicleSyncState();
    std::thread(StartPONGResponderThread).detach();

    uintptr_t libBase = GetLibraryBase("libapp.so");
    if (libBase == 0) return;
    
    uintptr_t renderMenuAddr = libBase + 0x00033974 + 1; 
    uintptr_t updateGUIAddr  = libBase + 0x0002f6a0 + 1; 
    uintptr_t gameUpdateAddr = libBase + 0x00047ee8 + 1;
    uintptr_t updateStateBaseAddr = libBase + 0x00046748 + 1;
    uintptr_t inGameMenuAddr = libBase + 0x00032090 + 1;

    g_VehicleGetPosition = (VehicleGetPosition_t)(libBase + 0x000397da + 1);
    g_VehicleGetOrientation = (VehicleGetOrientation_t)(libBase + 0x000397ea + 1);
    g_b2BodySetTransform = (b2BodySetTransform_t)(libBase + 0x00060a0c + 1);

    MSHookFunction((void*)renderMenuAddr, (void*)my_renderMenu, (void**)&orig_renderMenu);
    MSHookFunction((void*)updateGUIAddr, (void*)my_updateGUI, (void**)&orig_updateGUI);
    MSHookFunction((void*)gameUpdateAddr, (void*)my_GameUpdate, (void**)&orig_GameUpdate);
    MSHookFunction((void*)updateStateBaseAddr, (void*)my_GameUpdateStateBase, (void**)&orig_GameUpdateStateBase);
    MSHookFunction((void*)inGameMenuAddr, (void*)my_renderStartMenuMain, (void**)&orig_renderStartMenuMain);

    void* inputQueueGetEventAddr = dlsym(RTLD_DEFAULT, "AInputQueue_getEvent");
    if (inputQueueGetEventAddr != nullptr) {
        MSHookFunction(inputQueueGetEventAddr, (void*)my_AInputQueue_getEvent, (void**)&orig_AInputQueue_getEvent);
    }
}
