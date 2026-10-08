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
#include <netinet/tcp.h>
#include <poll.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <strings.h>
#include <elf.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <cerrno>
#include <math.h>
#include <algorithm>
#include <utility>

static bool SendAllBytes(int socketFd, const void* data, size_t size);

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

// Menu akisi:  Baslik -> [Mod secimi: Offline / LAN / Server]
//                              LAN -> [LAN secimi: Create Room / Join Room]
//                                        Create Room -> Multiplayer Room (host + chat)
//                                        Join Room   -> Multiplayer Server Browser
bool g_ShowModeSelect = false;   // Offline / LAN / Server menusu
bool g_ShowLanChoice  = false;   // Create Room / Join Room menusu
enum MpView { MPV_ROOM = 0, MPV_BROWSER = 1 };
MpView g_MpView = MPV_BROWSER;   // g_IsMultiplayerMenuActive iken hangi ekran cizilir
bool g_ReturnToMp = false;       // Select a file'da Back -> Multiplayer menusune don (host Play / client Join)
bool g_MpFromLan = false;        // Room/Browser LAN menusunden mi acildi? (Back davranisi icin)
static long long g_LastSaveMenuRenderMs = 0;   // my_renderMenu'nun en son calistigi an
static long long g_LastServerToastMs = 0;      // Server butonu toast spam korumasi
static std::atomic<long long> g_LastImGuiFrameMs(0); // DrawImGui'nin en son calistigi an

static long long NowMs() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Oyunun kendi BACK tusunu tetikler. Game+0x7c = AndroidHandheldInputDevice*,
// cihaz+0xcc = backKeyPressed bayragi (updateStateSavegame bunu okuyup basliga doner).
static void GameGoBackToTitle();
// Bir ust menuye don (Room/Browser -> LAN secimi -> Mod secimi).
static void MpGoBack();

int g_ButtonOrigWidth = 0;
int g_ButtonOrigHeight = 0;
int g_GameMenuBackgroundWidth = 0;
int g_GameMenuBackgroundHeight = 0;

std::atomic<float> g_TouchX(0.0f);
std::atomic<float> g_TouchY(0.0f);
std::atomic<bool> g_TouchDown(false);

static bool g_IsEatingTouch = false;
static std::atomic<bool> g_AndroidKeyboardOpen(false);
static std::atomic<bool> g_PendingTouchDown(false);
static std::atomic<float> g_PendingTouchX(0.0f);
static std::atomic<float> g_PendingTouchY(0.0f);

// LAN secildiyse true, Offline secildiyse false: oyun ici sohbet butonu sadece LAN'da gorunur.
static bool g_OnlineMode = false;
// Oyun ici yuvarlak sohbet butonu (ekran pikseli). Yaricap 0 = buton cizilmiyor.
static std::atomic<float> g_HudBtnX(0.0f), g_HudBtnY(0.0f), g_HudBtnR(0.0f);
static bool HudButtonHit(float x, float y) {
    const float r = g_HudBtnR.load();
    if (r <= 0.0f || (NowMs() - g_LastImGuiFrameMs.load()) > 300) return false;
    const float dx = x - g_HudBtnX.load(), dy = y - g_HudBtnY.load();
    return dx * dx + dy * dy <= (r * 1.2f) * (r * 1.2f);
}
static ImFont* g_GameUIFont = nullptr;
static bool g_GameUIFontInitialized = false;

// USER NICKNAME
char g_Nickname[32] = "Player"; 

// ========================================================================
// KALICI OYUNCU PROFILI (JSON)
// Dosya uygulamanin OZEL dahili deposunda tutulur: /data/data/<paket>/files/mp_profile.json
// (Android/data gibi kullanicinin dosya yoneticisiyle erisebildigi bir yer degil; root'suz
// erisilemez). Icerik: {"defaultName":"Player123456","nickname":"Player123456"}
// ========================================================================
static char g_DefaultName[32] = "";      // bu cihaza ozgu, kalici varsayilan isim
static char g_SavedNickname[32] = "";    // diske en son yazilan isim
static std::string g_ProfilePath;

static bool IsDefaultStyleName(const char* n) {   // "Player" + 6 rakam
    if (strncmp(n, "Player", 6) != 0 || strlen(n) != 12) return false;
    for (int i = 6; i < 12; ++i) if (n[i] < '0' || n[i] > '9') return false;
    return true;
}

static void MakeRandomDefaultName(char* out, size_t size) {
    uint32_t r = 0;
    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { if (read(fd, &r, sizeof(r)) != (ssize_t)sizeof(r)) r = 0; close(fd); }
    if (r == 0) {   // yedek: zaman + pid karisimi
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        r = (uint32_t)ts.tv_nsec * 2654435761u ^ (uint32_t)ts.tv_sec ^ ((uint32_t)getpid() << 16);
    }
    snprintf(out, size, "Player%06u", 100000u + (r % 900000u));
}

static void BuildProfilePath() {
    if (!g_ProfilePath.empty()) return;
    char pkg[128] = {0};
    FILE* f = fopen("/proc/self/cmdline", "r");
    if (f) { if (!fgets(pkg, sizeof(pkg) - 1, f)) pkg[0] = 0; fclose(f); }
    if (pkg[0] == 0 || strchr(pkg, '/')) return;
    const std::string dir = std::string("/data/data/") + pkg + "/files";
    mkdir(dir.c_str(), 0700);   // zaten varsa hata verir, sorun degil
    g_ProfilePath = dir + "/mp_profile.json";
}

static void JsonEscapeInto(std::string& dst, const char* s) {
    for (; *s; ++s) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { dst += '\\'; dst += (char)c; }
        else if (c < 0x20) dst += ' ';
        else dst += (char)c;
    }
}

static bool JsonGetString(const std::string& js, const char* key, char* out, size_t size) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = js.find(k);
    if (p == std::string::npos) return false;
    p = js.find(':', p + k.size());
    if (p == std::string::npos) return false;
    p = js.find('"', p);
    if (p == std::string::npos) return false;
    size_t o = 0;
    for (++p; p < js.size() && js[p] != '"'; ++p) {
        char c = js[p];
        if (c == '\\' && p + 1 < js.size()) c = js[++p];
        if (o + 1 < size) out[o++] = c;
    }
    out[o] = 0;
    return true;
}

static void SaveProfile() {
    BuildProfilePath();
    if (g_ProfilePath.empty()) return;
    std::string js = "{\"defaultName\":\"";
    JsonEscapeInto(js, g_DefaultName);
    js += "\",\"nickname\":\"";
    JsonEscapeInto(js, g_Nickname);
    js += "\"}\n";
    const std::string tmp = g_ProfilePath + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool ok = fwrite(js.data(), 1, js.size(), f) == js.size();
    fclose(f);
    if (ok && rename(tmp.c_str(), g_ProfilePath.c_str()) == 0) {
        strncpy(g_SavedNickname, g_Nickname, sizeof(g_SavedNickname) - 1);
    } else {
        remove(tmp.c_str());
    }
}

// Acilista bir kez: profili oku; yoksa/bozuksa rastgele "Player######" uret ve kaydet.
static void LoadOrCreateProfile() {
    static bool done = false;
    if (done) return;
    done = true;
    BuildProfilePath();
    std::string js;
    if (!g_ProfilePath.empty()) {
        FILE* f = fopen(g_ProfilePath.c_str(), "rb");
        if (f) {
            char tmp[512]; size_t n;
            while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0 && js.size() < 4096) js.append(tmp, n);
            fclose(f);
        }
    }
    char nick[32] = {0};
    JsonGetString(js, "defaultName", g_DefaultName, sizeof(g_DefaultName));
    JsonGetString(js, "nickname", nick, sizeof(nick));
    if (!IsDefaultStyleName(g_DefaultName)) MakeRandomDefaultName(g_DefaultName, sizeof(g_DefaultName));
    if (nick[0] == 0) strncpy(nick, g_DefaultName, sizeof(nick) - 1);
    strncpy(g_Nickname, nick, sizeof(g_Nickname) - 1);
    g_Nickname[sizeof(g_Nickname) - 1] = 0;
    SaveProfile();   // ilk calistirmada dosyayi olustur
    LOGI("[PROFILE] nickname=%s path=%s", g_Nickname, g_ProfilePath.c_str());
}

// Her karede: isim kutusu duzenlenmiyorsa bos ismi varsayilana cevir, degistiyse diske yaz.
static void PersistNicknameIfChanged(bool editingNick) {
    if (editingNick) return;
    // bastaki/sondaki bosluklari at
    size_t len = strlen(g_Nickname), st = 0;
    while (st < len && g_Nickname[st] == ' ') ++st;
    while (len > st && g_Nickname[len - 1] == ' ') --len;
    if (st > 0 || len < strlen(g_Nickname)) {
        memmove(g_Nickname, g_Nickname + st, len - st);
        g_Nickname[len - st] = 0;
    }
    if (g_Nickname[0] == 0) strncpy(g_Nickname, g_DefaultName, sizeof(g_Nickname) - 1);
    if (strcmp(g_Nickname, g_SavedNickname) != 0) SaveProfile();
}

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
std::atomic<bool> g_HostInGame(false);   // client tarafi: host oyuna girdi mi?
std::atomic<bool> g_LocalInGame(false);  // bu cihaz oyunda mi (Game durumu 6/7)

// ---- Host kaydini client'a aktarma (hicbir dosyaya yazilmaz, sadece bellekte tutulur)
static std::mutex g_SaveBlobMutex;
static std::atomic<bool> g_SaveRequestOut(false);        // client: host'tan kayit iste
static std::atomic<bool> g_SaveCaptureRequested(false);  // host: oyun thread'i kaydi alsin
static std::atomic<int> g_SaveRequestPeer(-1);           // host: kaydi isteyen oyuncunun id'si
static std::atomic<bool> g_HostBlobReady(false);         // host: gonderilecek kayit hazir
static std::vector<uint8_t> g_HostBlob;
static uint32_t g_HostBlobVersion = 0;
static std::vector<uint8_t> g_RecvSave;                  // client: parcalar burada birlesir
static size_t g_RecvSaveGot = 0;
static uint32_t g_RecvSaveVersion = 0;
static std::atomic<int> g_SaveProgress(-1);              // client: -1 yok, 0..99 iniyor, 100 tamam
static std::atomic<long long> g_SaveRequestMs(0);
static std::atomic<bool> g_ClientSaveReady(false);       // client: kayit indi
static std::vector<uint8_t> g_ClientSave;                // client: oyuna yuklenecek host kaydi
static uint32_t g_ClientSaveVersion = 0;
static std::atomic<bool> g_UseHostSave(false);           // loadSavegame host kaydini kullansin
static std::atomic<bool> g_CaptureNext(false);           // host: saveFile cagrisini yakala
static std::vector<uint8_t> g_CaptureBuf;
static void* g_SaveStartTaskFn = nullptr;                // SaveGames::startTask
static bool g_SaveHooksOk = false;                       // saveFile + loadSavegame baglandi
static int g_RestoreSlot = -1;                           // gecici "kayit var" bayragi geri alinacak slot

// SOCKETS
int g_TcpServerFd = -1; 
int g_TcpSocket = -1;   
std::string g_ConnectedStatus = "Not Connected";

// CHAT SYSTEM
std::vector<std::string> g_ChatMessages;
std::mutex g_ChatMutex;

std::vector<std::string> g_OutgoingChats;
std::mutex g_OutgoingChatMutex;

// Active chat text is kept globally for the render thread.
// Network threads only raise the pending-clear flag to avoid a data race.
char g_ChatInputBuffer[200] = "";
std::atomic<bool> g_ClearChatInputPending(false);

static std::atomic<int> g_KeyboardFieldMode(0); // 0=none, 1=nickname, 2=chat
// Typing bar: bottom edge as a fraction of the screen height. Android gives no reliable
// keyboard height, so this is a fixed, conservative spot. Lower it if the keyboard still
// covers the bar, raise it if the bar floats too high.
static const float kTypingBarBottom = 0.38f;

// ========================================================================
// JNI_OnLoad
// ========================================================================
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_GlobalJavaVM = vm;
    LoadOrCreateProfile();
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

// dlsym'in bulamadigi semboller icin yedek: libapp.so'yu diskten okuyup .symtab/.dynsym icinde
// verilen on ekle baslayan ilk sembolu arar (parametre yazimi tutmasa bile bulur).
static std::string GetLibraryPath(const char* lib) {
    std::string path;
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return path;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, lib)) continue;
        const char* sl = strchr(line, '/');
        if (!sl) continue;
        path = sl;
        while (!path.empty() && (path.back() == '\n' || path.back() == ' ')) path.pop_back();
        break;
    }
    fclose(fp);
    return path;
}

static void* FindElfSymbolByPrefix(const char* lib, const char* prefix) {
    const uintptr_t base = GetLibraryBase(lib);
    const std::string path = GetLibraryPath(lib);
    if (!base || path.empty()) return nullptr;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    void* result = nullptr;
    Elf32_Ehdr eh;
    if (fread(&eh, sizeof(eh), 1, f) == 1 && memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 &&
        eh.e_shentsize == sizeof(Elf32_Shdr) && eh.e_shnum > 0 && eh.e_shnum < 256) {
        std::vector<Elf32_Shdr> sh(eh.e_shnum);
        fseek(f, (long)eh.e_shoff, SEEK_SET);
        if (fread(sh.data(), sizeof(Elf32_Shdr), sh.size(), f) == sh.size()) {
            const size_t plen = strlen(prefix);
            for (size_t i = 0; i < sh.size() && !result; ++i) {
                if (sh[i].sh_type != SHT_SYMTAB && sh[i].sh_type != SHT_DYNSYM) continue;
                if (sh[i].sh_link >= sh.size()) continue;
                const Elf32_Shdr& str = sh[sh[i].sh_link];
                std::vector<char> strtab(str.sh_size + 1, 0);
                fseek(f, (long)str.sh_offset, SEEK_SET);
                if (fread(strtab.data(), 1, str.sh_size, f) != str.sh_size) continue;
                std::vector<Elf32_Sym> syms(sh[i].sh_size / sizeof(Elf32_Sym));
                fseek(f, (long)sh[i].sh_offset, SEEK_SET);
                if (fread(syms.data(), sizeof(Elf32_Sym), syms.size(), f) != syms.size()) continue;
                for (size_t k = 0; k < syms.size(); ++k) {
                    if (syms[k].st_value == 0 || syms[k].st_name >= str.sh_size) continue;
                    if (strncmp(&strtab[syms[k].st_name], prefix, plen) == 0) {
                        result = (void*)(base + syms[k].st_value);   // Thumb biti st_value'da zaten var
                        break;
                    }
                }
            }
        }
    }
    fclose(f);
    return result;
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
    g_ClearChatInputPending.store(true);
}

// Client icin oyunu kayit/zorluk ekranlarini atlayarak baslatir. Asil is my_updateGUI icindeki
// AutoStartInject()'te yapilir; burada sadece menuler kapatilir ve zaman penceresi acilir.
static std::atomic<long long> g_AutoStartUntilMs(0);
static std::atomic<long long> g_KickUntilMs(0);   // >0 iken client oyundan basliga atilir
static std::atomic<bool> g_ClientPlaying(false);  // client Join Game ile host'un oyununa girdi
void CloseAndroidKeyboard();
void AutoStartGameForClient() {
    CloseAndroidKeyboard();
    g_IsMultiplayerMenuActive = false;
    g_ShowLanChoice = false;
    g_ShowModeSelect = false;
    g_PendingTouchDown.store(false);
    g_ReturnToMp = true;
    g_AutoStartUntilMs.store(NowMs() + 20000);
    g_ClientPlaying.store(true);
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

// Client baska birinin oyununda oldugu icin kaydedemez: SaveGames::startTask(3 = slota kaydet) engellenir.
typedef void (*SaveStartTask_t)(void* thiz, int cmd, unsigned int slot, int sync);
static SaveStartTask_t orig_SaveStartTask = nullptr;
static void my_SaveStartTask(void* thiz, int cmd, unsigned int slot, int sync) {
    // 3 = slota kaydet, 5 = gecici kayit (host dunyasi client'ta diske yazilmasin)
    if ((cmd == 3 || cmd == 5) && (g_ClientPlaying.load() || (g_IsClient.load() && g_IsConnected.load()))) {
        if (cmd == 3) ShowNativeToast("Only the host can save the game.");
        return;
    }
    if (orig_SaveStartTask) orig_SaveStartTask(thiz, cmd, slot, sync);
}

// Host: oyunun kendi gecici kayit akisi (task 5) calisirken saveFile'a giden veriyi yakala.
typedef int (*SaveFile_t)(void* dev, const char* name, unsigned char* buf, unsigned int size);
static SaveFile_t orig_SaveFile = nullptr;
static int my_SaveFile(void* dev, const char* name, unsigned char* buf, unsigned int size) {
    if (g_CaptureNext.load() && buf && size > 4096 && g_CaptureBuf.empty())
        g_CaptureBuf.assign(buf, buf + size);
    return orig_SaveFile ? orig_SaveFile(dev, name, buf, size) : 0;
}

// Client: Join Game ile baslayan yukleme sirasinda slot dosyasi yerine host kaydini yukle.
typedef int (*LoadSavegame_t)(void* thiz, const char* name, unsigned int crc, unsigned int ver);
static LoadSavegame_t orig_LoadSavegame = nullptr;
static int my_LoadSavegame(void* thiz, const char* name, unsigned int crc, unsigned int ver) {
    if (g_UseHostSave.load()) {
        std::vector<uint8_t> copy;
        uint32_t version = 0;
        {
            std::lock_guard<std::mutex> lock(g_SaveBlobMutex);
            copy = g_ClientSave;
            version = g_ClientSaveVersion;
        }
        void* cb = *(void**)((uintptr_t)thiz + 0x60);          // AppSaveGameCallbackInterface
        if (cb && !copy.empty()) {
            typedef void (*Deserialize_t)(void*, const void*, unsigned int, int, unsigned int);
            Deserialize_t fn = (Deserialize_t)(*(void***)cb)[3];
            fn(cb, copy.data(), (unsigned int)copy.size(), 0, version);
            return 1;
        }
    }
    return orig_LoadSavegame ? orig_LoadSavegame(thiz, name, crc, ver) : 0;
}

// Host: oyun thread'inde client'in istedigi kaydi al (game+0xa374 = SaveGames).
static void HostCaptureSaveIfRequested() {
    if (!g_SaveCaptureRequested.exchange(false)) return;
    if (g_EngineInstance == 0 || !g_SaveStartTaskFn || !g_SaveHooksOk) return;
    const int st = *(volatile int*)(g_EngineInstance + 0x64);
    if (st != 6 && st != 7) return;
    const uintptr_t sg = g_EngineInstance + 0xa374;
    g_CaptureBuf.clear();
    g_CaptureNext.store(true);
    ((SaveStartTask_t)g_SaveStartTaskFn)((void*)sg, 5, *(volatile unsigned int*)(g_EngineInstance + 0xa3f0), 1);
    g_CaptureNext.store(false);
    if (g_CaptureBuf.empty()) return;
    std::lock_guard<std::mutex> lock(g_SaveBlobMutex);
    g_HostBlob.swap(g_CaptureBuf);
    g_HostBlobVersion = *(volatile unsigned int*)(sg + 0x58);
    g_HostBlobReady.store(true);
}

typedef void (*GameUpdateStateBase_t)(void* thiz, float param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4);
GameUpdateStateBase_t orig_GameUpdateStateBase = nullptr;

// ========================================================================
// VEHICLE / MULTIPLAYER SYNCHRONIZATION
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
static const uint8_t PACKET_PLAYER_INFO = 8;
static const uint8_t PACKET_HOST_STATE = 9;   // host oyunda mi?
static const uint8_t PACKET_SAVE_REQUEST = 10; // client -> host: kaydi gonder
static const uint8_t PACKET_SAVE_CHUNK = 11;   // host -> client: kayit parcasi
static const uint8_t PACKET_AUTH_TABLE = 12;   // host -> client: TUM arac sahiplikleri (periyodik + istek uzerine)
static const uint8_t PACKET_SYNC_REQUEST = 13; // client -> host: "bana tam senkron gonder"
static const uint8_t PACKET_PING = 14;         // canlilik sinyali (TCP ve UDP)
static const uint8_t PACKET_UDP_HELLO = 15;    // client -> host (UDP): [type][playerId]
static const uint8_t PACKET_VEHICLE_SPAWN = 17;   // arac eklendi (client->host: istek netId=0xFFFF; host->herkes: netId atanmis)
static const uint8_t PACKET_VEHICLE_REMOVE = 18;  // arac satildi/silindi (netId)
static const uint8_t PACKET_VEHICLE_NETMAP = 19;  // host -> client: oyundaki arac sirasi -> netId esleme tablosu
static const uint8_t PACKET_UDP_ACK = 16;      // host -> client (UDP): UDP yolu acildi

// Baglanti sagligi: bu sureden uzun sessizlik = karsi taraf gitti (donmus / zorla kapatilmis).
static const long long HOST_SILENCE_TIMEOUT_MS = 10000;  // client: host'tan hicbir sey gelmedi
static const long long PEER_SILENCE_TIMEOUT_MS = 10000;  // host: client'tan hicbir sey gelmedi
static const long long HOST_ALIVE_WINDOW_MS = 3000;      // host oyun dongusu bu surede calistiysa "canli"
static const long long PING_INTERVAL_MS = 500;
static const long long AUTH_TABLE_INTERVAL_MS = 1000;
static const long long UDP_STALE_MS = 3000;              // UDP yolu bu kadar sessizse TCP'ye don

static std::atomic<long long> g_LastGameUpdateMs(0);     // oyun ana dongusunun son calistigi an
static std::atomic<bool> g_SyncRequestOut(false);        // client: host'tan tam senkron iste
static std::atomic<bool> g_HostForceTable(false);        // host: sahiplik tablosunu hemen yayinla
static std::atomic<bool> g_ForceResendAll(false);        // yerel araclari yeniden gonder

static const uint8_t MAX_PLAYERS = 4;
static const uint16_t VEHICLE_ID_INVALID = 0xFFFF;
static const uint16_t VEHICLE_SLOT_LIMIT = 512;
static const uint8_t VEHICLE_OWNER_NONE = 0xFF;

// Local vehicle sampling target: 120 Hz.
// The actual callback frequency still follows the game's own update/vsync loop.
// Network vehicle snapshots go out 30 times/sec, but only for vehicles that changed.
static const uint32_t VEHICLE_LOCAL_SAMPLE_INTERVAL_US = 8333;
static const uint32_t VEHICLE_NETWORK_INTERVAL_MS = 33;
static const uint32_t VEHICLE_KEEPALIVE_MS = 1000;  // unchanged vehicles are re-sent this often (late joiners)
static const uint32_t VEHICLE_STOP_RELEASE_MS = 1000;
static const uint8_t VEHICLE_SNAPSHOT_MAX_COUNT = 64;
static const float POSITION_EPSILON = 0.02f;
static const float ANGLE_EPSILON = 0.005f;

// Remote vehicle smoothing.
static const float REMOTE_MAX_EXTRAPOLATION_SEC = 0.15f; // never predict further than this past a snapshot
static const float REMOTE_ERROR_DECAY_SEC = 0.10f;       // time constant for hiding corrections
static const float REMOTE_TELEPORT_DISTANCE = 15.0f;     // bigger jumps are snapped, not smoothed
static const float REMOTE_MAX_SPEED = 60.0f;             // faster than this = bogus data
static const float CLAIM_POSITION_EPSILON = 0.05f;
static const float CLAIM_ANGLE_EPSILON = 0.02f;

#pragma pack(push, 1)

struct SessionWelcomePacket {
    uint8_t type;
    uint8_t ownerId;
    uint8_t maxPlayers;
    uint8_t reserved;
};

struct PlayerInfoPacket {
    uint8_t type;
    uint8_t playerId;
    char playerName[32];
};

struct HostStatePacket {
    uint8_t type;
    uint8_t inGame;
};

struct SaveRequestPacket {
    uint8_t type;
};

static const uint16_t SAVE_CHUNK_DATA = 1000;
struct SaveChunkPacket {
    uint8_t type;
    uint32_t total;
    uint32_t offset;
    uint32_t version;
    uint16_t len;
    uint8_t data[SAVE_CHUNK_DATA];
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
// This keeps vehicle traffic at ~30 network messages/sec regardless of
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

struct VehicleSpawnPacket {
    uint8_t type;
    uint8_t originId;       // aracı satin alan oyuncu
    uint16_t netId;         // 0xFFFF: henuz atanmadi (client istegi)
    uint16_t vehicleType;   // EntityManager::VEHICLES
    float x, y, z;          // Vector3
    uint32_t angleBits;     // float acinin bit deseni
};

struct VehicleRemovePacket {
    uint8_t type;
    uint8_t originId;
    uint16_t netId;
};

#pragma pack(pop)

static_assert(sizeof(SessionWelcomePacket) == 4, "SessionWelcomePacket size mismatch");
static_assert(sizeof(PlayerInfoPacket) == 34, "PlayerInfoPacket size mismatch");
static_assert(sizeof(NetworkPacket) == 256, "NetworkPacket size mismatch");
static_assert(sizeof(VehiclePositionPacket) == 21, "VehiclePositionPacket size mismatch");
static_assert(sizeof(VehicleSnapshotHeader) == 8, "VehicleSnapshotHeader size mismatch");
static_assert(sizeof(VehicleSnapshotEntry) == 19, "VehicleSnapshotEntry size mismatch");
static_assert(sizeof(VehicleClaimPacket) == 8, "VehicleClaimPacket size mismatch");
static_assert(sizeof(VehicleAuthorityPacket) == 8, "VehicleAuthorityPacket size mismatch");
static_assert(sizeof(VehicleReleasePacket) == 8, "VehicleReleasePacket size mismatch");
static_assert(sizeof(VehicleSpawnPacket) == 22, "VehicleSpawnPacket size mismatch");
static_assert(sizeof(VehicleRemovePacket) == 4, "VehicleRemovePacket size mismatch");

static std::atomic<uint8_t> g_LocalPlayerId(0xFF);
static char g_PlayerNames[4][32] = {{0}};
static std::mutex g_PlayerNamesMutex;

static void ResetPlayerNames() {
    std::lock_guard<std::mutex> lock(g_PlayerNamesMutex);
    memset(g_PlayerNames, 0, sizeof(g_PlayerNames));
}

static void SetPlayerName(uint8_t playerId, const char* name) {
    if (playerId >= 4) return;
    std::lock_guard<std::mutex> lock(g_PlayerNamesMutex);
    memset(g_PlayerNames[playerId], 0, sizeof(g_PlayerNames[playerId]));
    if (name) strncpy(g_PlayerNames[playerId], name, sizeof(g_PlayerNames[playerId]) - 1);
}

static std::string GetPlayerName(uint8_t playerId) {
    if (playerId >= 4) return std::string();
    std::lock_guard<std::mutex> lock(g_PlayerNamesMutex);
    return std::string(g_PlayerNames[playerId]);
}

// Host: baska bir oyuncuyla ayni isim gelirse benzersiz hale getirir. "Player######" ise yeni
// rakamlar uretilir, ozel isimse sonuna 2..99 eklenir. true donerse isim degisti.
static bool NameTakenByOther(uint8_t playerId, const char* name) {
    std::lock_guard<std::mutex> lock(g_PlayerNamesMutex);
    for (int i = 0; i < 4; ++i) {
        if (i == playerId || g_PlayerNames[i][0] == 0) continue;
        if (strcasecmp(g_PlayerNames[i], name) == 0) return true;
    }
    return false;
}

static bool MakeNameUnique(uint8_t playerId, char* name, size_t size) {
    if (name[0] == 0 || !NameTakenByOther(playerId, name)) return false;
    if (IsDefaultStyleName(name)) {
        for (int t = 0; t < 20; ++t) {
            MakeRandomDefaultName(name, size);
            if (!NameTakenByOther(playerId, name)) return true;
        }
        return true;
    }
    char base[32];
    strncpy(base, name, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    for (int n = 2; n < 100; ++n) {
        char suffix[8];
        snprintf(suffix, sizeof(suffix), "%d", n);
        const size_t room = size - 1 - strlen(suffix);
        snprintf(name, size, "%.*s%s", (int)room, base, suffix);
        if (!NameTakenByOther(playerId, name)) return true;
    }
    return true;
}

// Host, bir client'in ismini degistirdiyse duzeltilmis paket sender'a da geri gonderilir.
static std::atomic<int> g_NameFixPeer(-1);
static PlayerInfoPacket g_NameFixPkt;

static void SendLocalPlayerInfo() {
    if (!g_IsConnected.load() || g_TcpSocket < 0) return;
    const uint8_t playerId = g_LocalPlayerId.load();
    if (playerId >= MAX_PLAYERS) return;

    PlayerInfoPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_PLAYER_INFO;
    pkt.playerId = playerId;
    strncpy(pkt.playerName, g_Nickname, sizeof(pkt.playerName) - 1);
    SendAllBytes(g_TcpSocket, &pkt, sizeof(pkt));
}

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
    float errX;           // visual offset left over from the last correction;
    float errY;           // it fades to zero (see ApplyRemoteVehicleStates)
    float errA;
    uint32_t sequence;    // sender clock in ms of the newest snapshot
    uint32_t appliedSequence;
    uint64_t lastReceiveMs;
    bool moving;
};

struct VehicleSampleState {
    bool valid;
    bool moving;
    float x;
    float y;
    float angle;
    uint64_t timeMs;
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
// The network thread packs all changed samples into one snapshot every 33 ms.
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

// ========================================================================
// KALICI ARAC KIMLIGI (netId)
// Oyunun arac dizisi (game+0xA8.. / sayac +0xA4) arac satilinca KAYAR; dizideki sira kalici
// kimlik degildir. Aglarda artik "vehicleId" = netId: host'un verdigi, arac yasadigi surece
// degismeyen numara. g_KnownVehiclePointers[netId] = o aracin Vehicle* adresi (0 = bos).
// Arac eklenince/silinince olay paketi (SPAWN/REMOVE) tum cihazlarda ayni islemi yaptirir.
// ========================================================================
struct PendingVehicleOp {
    uint8_t kind;           // 0: SPAWN istegi (host), 1: SPAWN (netId atanmis), 2: REMOVE
    uint8_t originId;
    uint16_t netId;
    uint16_t vehicleType;
    float x, y, z;
    uint32_t angleBits;
    uint32_t seq;
};
static std::atomic<bool> g_VehNetArmed(false);          // netId tablosu kuruldu, olaylar aktif
static std::atomic<bool> g_NetMapDirty(false);          // host: netmap paketi yeniden uretilmeli
static bool g_ApplyingRemoteVehicleOp = false;          // sadece oyun thread'i: uzaktan uygulanan islem
static std::vector<uintptr_t> g_UnboundLocalVehicles;   // bu cihazda alinip host'tan netId bekleyenler (FIFO)
static std::mutex g_VehicleEventMutex;                  // asagidaki 5 degiskeni korur
static std::vector<uint8_t> g_VehicleEventOut;          // BuildOutgoing'in gonderecegi arac olaylari
static std::vector<PendingVehicleOp> g_PendingVehicleOps;
static std::vector<uint16_t> g_PendingNetMap;
static bool g_HasPendingNetMap = false;
static std::vector<uint8_t> g_HostNetMapBytes;          // host: son uretilen netmap paketi
static uint32_t g_OpSeq = 0, g_NetMapSeq = 0;
static bool g_VehicleHooksOk = false;

static uintptr_t RawVehicleSlot(uintptr_t game, uint32_t idx) {
    return *(uintptr_t*)(game + ((uintptr_t)(idx + 0x2A) * 4u) + 4u);
}

static uint32_t VehicleCount(uintptr_t game) {
    const uint32_t n = *(uint32_t*)(game + 0xA4);
    return n > VEHICLE_SLOT_LIMIT ? VEHICLE_SLOT_LIMIT : n;
}

static int SlotIndexOfVehicle(uintptr_t game, uintptr_t p) {
    const uint32_t n = VehicleCount(game);
    for (uint32_t i = 0; i < n; ++i) if (RawVehicleSlot(game, i) == p) return (int)i;
    return -1;
}

static int NetIdOfVehicle(uintptr_t p) {
    if (p == 0) return -1;
    for (uint16_t i = 0; i < VEHICLE_SLOT_LIMIT; ++i) if (g_KnownVehiclePointers[i] == p) return (int)i;
    return -1;
}

static int AllocNetId() {
    for (uint16_t i = 0; i < VEHICLE_SLOT_LIMIT; ++i) if (g_KnownVehiclePointers[i] == 0) return (int)i;
    return -1;
}

// netId -> canli Vehicle*. Tablodaki adres oyunun GUNCEL arac dizisinde yoksa (satildi/yok edildi)
// 0 doner; boylece yok edilmis bir araca asla dokunulmaz.
static uintptr_t GetVehicleFromIndex(uintptr_t game, uint16_t netId) {
    if (game == 0 || netId >= VEHICLE_SLOT_LIMIT) return 0;
    const uintptr_t p = g_KnownVehiclePointers[netId];
    if (p == 0) return 0;
    return SlotIndexOfVehicle(game, p) >= 0 ? p : 0;
}

static uintptr_t GetActiveVehicleFromGame(uintptr_t game, uint16_t* outVehicleId) {
    if (game == 0) return 0;
    const uint32_t idx = *(uint32_t*)(game + 0xA8);
    if (idx >= VehicleCount(game)) return 0;
    const uintptr_t p = RawVehicleSlot(game, idx);
    const int id = NetIdOfVehicle(p);
    if (id < 0) return 0;               // henuz netId almamis (yeni alinan arac)
    if (outVehicleId) *outVehicleId = (uint16_t)id;
    return p;
}

static void QueueVehicleEvent(const void* pkt, size_t size) {
    if (!g_IsConnected.load()) return;
    std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
    if (g_VehicleEventOut.size() > 4096) g_VehicleEventOut.clear();
    const uint8_t* b = (const uint8_t*)pkt;
    g_VehicleEventOut.insert(g_VehicleEventOut.end(), b, b + size);
}

static void PushVehicleOp(PendingVehicleOp op) {
    LOGI("[VEHNET] olay alindi: kind=%d netId=%u origin=%u (armed=%d)", (int)op.kind, (unsigned)op.netId,
         (unsigned)op.originId, (int)g_VehNetArmed.load());
    std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
    if (g_PendingVehicleOps.size() >= 64) g_PendingVehicleOps.erase(g_PendingVehicleOps.begin());
    op.seq = ++g_OpSeq;
    g_PendingVehicleOps.push_back(op);
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

        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Non-blocking socket with a full send buffer: wait a moment instead of dropping the link.
            struct pollfd pfd;
            pfd.fd = socketFd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, 50) > 0) continue;
        }

        return false;
    }

    return true;
}

// Small vehicle packets must leave immediately: without TCP_NODELAY the OS holds them back
// (Nagle) and releases them in bunches, which shows up as stutter on the other device.
static void ConfigureLowLatencySocket(int socketFd) {
    if (socketFd < 0) return;
    int one = 1;
    setsockopt(socketFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // Yarim acik baglantiyi (karsi taraf sessizce kayboldu) birkac saniyede yakala.
    setsockopt(socketFd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int idle = 4, intvl = 2, cnt = 3;
    setsockopt(socketFd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(socketFd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(socketFd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
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

static void BuildHostNetMap(uintptr_t game) {
    const uint32_t n = VehicleCount(game);
    std::vector<uint8_t> b;
    b.push_back(PACKET_VEHICLE_NETMAP);
    const uint16_t c = (uint16_t)n;
    b.insert(b.end(), (const uint8_t*)&c, (const uint8_t*)&c + 2);
    for (uint32_t i = 0; i < n; ++i) {
        const int id = NetIdOfVehicle(RawVehicleSlot(game, i));
        const uint16_t v = id < 0 ? (uint16_t)0xFFFF : (uint16_t)id;
        b.insert(b.end(), (const uint8_t*)&v, (const uint8_t*)&v + 2);
    }
    std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
    g_HostNetMapBytes.swap(b);
    g_NetMapDirty.store(false);
}

// Game::addVehicle KANCALANMIYOR (orig_Game_addVehicle = oyunun kendi fonksiyonu, sadece cagrilir).
// Satin alinan araci, oyunun arac dizisindeki degisiklikten (netId'si olmayan yeni adres) buluruz.
// Boylece Substrate'in fonksiyon basini tasima hatasi riski tamamen ortadan kalkar.
static uint32_t g_PolledVehicleCount = 0;
static bool g_VehRemovedSincePoll = false;                 // satis oldu: havuzdaki nesne yeniden kullanilmis olabilir
static std::vector<uintptr_t> g_IgnoredVehiclePtrs;        // kurulumda esleme tablosuna giremeyen mevcut araclar

static bool VecHas(const std::vector<uintptr_t>& v, uintptr_t p) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == p) return true;
    return false;
}

static void AnnounceLocalVehicle(uintptr_t game, uintptr_t p) {
    const uintptr_t body = *(uintptr_t*)(p + 0x528);        // Vehicle::body
    if (body == 0) return;
    const uint32_t type = *(uint32_t*)(p + 0x10);           // EntityManager::VEHICLES (satis kodunda da boyle okunuyor)
    const float x = *(float*)(body + 0x0C);
    const float y = *(float*)(body + 0x10);
    const float a = *(float*)(body + 0x40);

    VehicleSpawnPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_VEHICLE_SPAWN;
    pkt.originId = g_LocalPlayerId.load();
    pkt.vehicleType = (uint16_t)type;
    pkt.x = x; pkt.y = 0.0f; pkt.z = y;                     // Vector3(x, yukseklik=0, box2d y) - oyunun kendi duzeni
    memcpy(&pkt.angleBits, &a, 4);
    if (g_IsHost.load()) {
        const int id = AllocNetId();
        if (id < 0) return;
        { std::lock_guard<std::mutex> lock(g_VehicleStateMutex); g_KnownVehiclePointers[id] = p; }
        pkt.netId = (uint16_t)id;
        g_NetMapDirty.store(true);
        g_HostForceTable.store(true);
    } else {
        g_UnboundLocalVehicles.push_back(p);                // host netId verince baglanir
        pkt.netId = 0xFFFF;
    }
    QueueVehicleEvent(&pkt, sizeof(pkt));
    LOGI("[VEHNET] yerel arac eklendi: type=%u netId=%u pos=(%.1f,%.1f)", (unsigned)type, (unsigned)pkt.netId, x, y);
}

static void DetectNewLocalVehicles(uintptr_t game) {
    const uint32_t n = VehicleCount(game);
    const bool grew = n > g_PolledVehicleCount;
    const bool reuse = g_VehRemovedSincePoll;
    g_PolledVehicleCount = n;
    g_VehRemovedSincePoll = false;
    // Dizide artik olmayan "yoksay" kayitlarini temizle.
    for (size_t i = g_IgnoredVehiclePtrs.size(); i-- > 0;)
        if (SlotIndexOfVehicle(game, g_IgnoredVehiclePtrs[i]) < 0) g_IgnoredVehiclePtrs.erase(g_IgnoredVehiclePtrs.begin() + i);
    if (!grew && !reuse) return;
    for (uint32_t i = 0; i < n; ++i) {
        const uintptr_t p = RawVehicleSlot(game, i);
        if (p == 0 || NetIdOfVehicle(p) >= 0) continue;
        if (VecHas(g_UnboundLocalVehicles, p) || VecHas(g_IgnoredVehiclePtrs, p)) continue;
        AnnounceLocalVehicle(game, p);
    }
}

static void DisarmVehicleNet() {
    {
        std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
        for (uint16_t i = 0; i < VEHICLE_SLOT_LIMIT; ++i) ClearVehicleStateSlot(i);
    }
    g_VehNetArmed.store(false);
    g_UnboundLocalVehicles.clear();
    g_IgnoredVehiclePtrs.clear();
    std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
    g_HostNetMapBytes.clear();
}

// Her karede (oyun thread'i): netId tablosunu kurar / yok olan araclari temizler.
static void RefreshVehicleTopology(uintptr_t game) {
    if (game == 0) return;
    const uint32_t state = *(uint32_t*)(game + 0x64);
    const uint32_t n = VehicleCount(game);
    // Durum 6 = oyun, durum 7 = oyun ici menu (magaza/ayarlar). Alim/satim durum 7'de olur;
    // ag orada kapanirsa satis/alim hic bildirilmez (host logunda "armed=0" olarak goruldu).
    if ((state != 6 && state != 7) || n == 0 || !g_IsConnected.load()) {
        if (g_VehNetArmed.load()) DisarmVehicleNet();
        return;
    }

    if (!g_VehNetArmed.load()) {
        std::vector<uint16_t> map;
        uint32_t mapSeq = 0;
        if (g_IsHost.load()) {
            for (uint32_t i = 0; i < n; ++i) map.push_back((uint16_t)i);   // host: sira = netId
        } else {
            // Client: host'un gonderdigi esleme tablosunu bekle (sayi uyusmali).
            bool have = false;
            {
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                if (g_HasPendingNetMap) {
                    if (g_PendingNetMap.size() == n) { map = g_PendingNetMap; mapSeq = g_NetMapSeq; have = true; }
                    g_HasPendingNetMap = false;
                }
            }
            static long long s_waitStart = 0, s_lastReq = 0;
            const long long now = NowMs();
            if (s_waitStart == 0) s_waitStart = now;
            if (!have) {
                if (now - s_lastReq > 1500) { s_lastReq = now; g_SyncRequestOut.store(true); }
                if (now - s_waitStart < 6000) return;                  // biraz daha bekle
                for (uint32_t i = 0; i < n; ++i) map.push_back((uint16_t)i);   // son care: sira = netId
                LOGI("[VEHNET] netmap gelmedi, sira=netId varsayildi");
            }
            s_waitStart = 0;
        }
        {
            std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
            for (uint16_t i = 0; i < VEHICLE_SLOT_LIMIT; ++i) ClearVehicleStateSlot(i);
            for (uint32_t i = 0; i < n && i < map.size(); ++i)
                if (map[i] < VEHICLE_SLOT_LIMIT) g_KnownVehiclePointers[map[i]] = RawVehicleSlot(game, i);
        }
        g_UnboundLocalVehicles.clear();
        {   // netmap'ten ESKI olan bekleyen olaylari at (zaten kayitta/tabloda var)
            std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
            std::vector<PendingVehicleOp> keep;
            for (const PendingVehicleOp& op : g_PendingVehicleOps) if (op.seq > mapSeq) keep.push_back(op);
            g_PendingVehicleOps.swap(keep);
        }
        g_IgnoredVehiclePtrs.clear();
        for (uint32_t i = 0; i < n; ++i) {
            const uintptr_t pp = RawVehicleSlot(game, i);
            if (NetIdOfVehicle(pp) < 0) g_IgnoredVehiclePtrs.push_back(pp);   // esleme tablosuna girmeyen mevcut arac
        }
        g_PolledVehicleCount = n;
        g_VehRemovedSincePoll = false;
        g_VehNetArmed.store(true);
        g_NetMapDirty.store(true);
        LOGI("[VEHNET] armed, vehicles=%u hooksOk=%d", n, (int)g_VehicleHooksOk);
        static bool s_hookWarned = false;
        if (!g_VehicleHooksOk && !s_hookWarned) {
            s_hookWarned = true;
            ShowNativeToast("Vehicle buy/sell sync is unavailable (game hooks not found).");
        }
        if (g_IsClient.load()) g_SyncRequestOut.store(true);
        if (g_IsHost.load()) g_HostForceTable.store(true);
    } else {
        // Yok olan araclari temizle (satildi / kayit degisti).
        bool changed = false;
        for (uint16_t id = 0; id < VEHICLE_SLOT_LIMIT; ++id) {
            const uintptr_t p = g_KnownVehiclePointers[id];
            if (p != 0 && SlotIndexOfVehicle(game, p) < 0) {
                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                ClearVehicleStateSlot(id);
                changed = true;
            }
        }
        for (size_t i = g_UnboundLocalVehicles.size(); i-- > 0;)
            if (SlotIndexOfVehicle(game, g_UnboundLocalVehicles[i]) < 0)
                g_UnboundLocalVehicles.erase(g_UnboundLocalVehicles.begin() + i);
        if (changed) {
            g_NetMapDirty.store(true);
            if (g_IsHost.load()) g_HostForceTable.store(true);
        }
        DetectNewLocalVehicles(game);        // magazadan yeni arac alindiysa diger cihazlara bildir
    }
    if (g_IsHost.load() && g_NetMapDirty.load()) BuildHostNetMap(game);
}

// ---- Kanca guvenligi: Substrate (Thumb) fonksiyonun ilk 8-10 baytini kendi alanina TASIR. ----
// Bu baytlarda PC'ye bagli bir talimat (ldr rX,[pc,#..], adr, add rX,pc, dal) varsa tasinirken
// bozulur ve kanca takili fonksiyon rastgele bellege erisip oyunu cokertir.
// (Game::buyItem tam olarak boyleydi: "ldr r1,[pc,#0x244]" ilk 8 baytin icindeydi.)
static bool ThumbPrologueHasPcRelative(const void* fn, char* why, size_t whySize) {
    const uintptr_t a = (uintptr_t)fn;
    if (!(a & 1u)) return false;                       // ARM modu: bu kontrol Thumb icin
    const uint16_t* p = (const uint16_t*)(a & ~(uintptr_t)1);
    size_t off = 0;
    while (off < 10) {
        const uint16_t hw = p[off / 2];
        const uint16_t top = hw & 0xF800;
        const bool is32 = (top == 0xE800 || top == 0xF000 || top == 0xF800);
        const char* bad = nullptr;
        if (!is32) {
            if ((hw & 0xF800) == 0x4800) bad = "ldr rX,[pc,#imm]";
            else if ((hw & 0xF800) == 0xA000) bad = "adr";
            else if ((hw & 0xFF78) == 0x4478) bad = "add rX,pc";
            else if ((hw & 0xF000) == 0xD000 && (hw & 0x0F00) < 0x0E00) bad = "conditional branch";
            else if ((hw & 0xF800) == 0xE000) bad = "b";
            else if ((hw & 0xF500) == 0xB100) bad = "cbz/cbnz";
            off += 2;
        } else {
            const uint16_t hw2 = p[off / 2 + 1];
            if ((hw & 0xFE00) == 0xF800 && (hw & 0xF) == 0xF) bad = "ldr.w/ldrb/ldrh literal";
            else if ((hw & 0xFF00) == 0xED00 && (hw & 0xF) == 0xF) bad = "vldr literal";
            else if ((hw & 0xFE00) == 0xE800 && (hw & 0xF) == 0xF) bad = "ldrd literal";
            else if ((hw & 0xFBFF) == 0xF20F || (hw & 0xFBFF) == 0xF2AF) bad = "adr.w";
            else if ((hw & 0xF800) == 0xF000 && (hw2 & 0x8000)) bad = "b.w/bl/blx";
            off += 4;
        }
        if (bad) { snprintf(why, whySize, "%s at +%u", bad, (unsigned)(off - ((is32) ? 4u : 2u))); return true; }
    }
    return false;
}

// skipIfRisky=true ise riskli fonksiyona kanca TAKILMAZ (oyun cokmesin diye) ve false doner.
static bool HookChecked(const char* name, void* fn, void* replacement, void** original, bool skipIfRisky) {
    if (!fn) return false;
    char why[64] = {0};
    if (ThumbPrologueHasPcRelative(fn, why, sizeof(why))) {
        LOGI("[HOOK] WARNING %s: %s in the first 10 bytes (Substrate may relocate it wrongly)", name, why);
        if (skipIfRisky) { LOGI("[HOOK] %s SKIPPED", name); return false; }
    }
    MSHookFunction(fn, replacement, original);
    return true;
}

// ---- Oyundaki arac ekleme/silme kancalari (Game::addVehicle / Game::removeVehicle) ----
typedef uint32_t (*Game_addVehicle_t)(void* game, int type, const float* pos, uint32_t angleBits, uint32_t extra);
typedef void (*Game_removeVehicle_t)(void* game, uint32_t idx);
typedef void (*Vehicle_destroy_t)(void* vehicle);
static Game_addVehicle_t orig_Game_addVehicle = nullptr;
static Game_removeVehicle_t orig_Game_removeVehicle = nullptr;
static Vehicle_destroy_t g_VehicleDestroyFn = nullptr;

static void my_Game_removeVehicle(void* game, uint32_t idx) {
    const uintptr_t g = (uintptr_t)game;
    int netId = -1;
    if (g_VehNetArmed.load() && g_IsConnected.load() && idx < VehicleCount(g)) {
        const uintptr_t p = RawVehicleSlot(g, idx);
        netId = NetIdOfVehicle(p);
        for (size_t i = g_UnboundLocalVehicles.size(); i-- > 0;)
            if (g_UnboundLocalVehicles[i] == p) g_UnboundLocalVehicles.erase(g_UnboundLocalVehicles.begin() + i);
        for (size_t i = g_IgnoredVehiclePtrs.size(); i-- > 0;)
            if (g_IgnoredVehiclePtrs[i] == p) g_IgnoredVehiclePtrs.erase(g_IgnoredVehiclePtrs.begin() + i);
        g_VehRemovedSincePoll = true;
        if (netId >= 0) {
            // Arac yok edilmeden ONCE tum kayitlari temizle: artik hicbir yerde bu arac kalmaz.
            std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
            ClearVehicleStateSlot((uint16_t)netId);
        }
        g_NetMapDirty.store(true);
        if (g_IsHost.load()) g_HostForceTable.store(true);
    }
    if (netId < 0 && !g_ApplyingRemoteVehicleOp)
        LOGI("[VEHNET] remove idx=%u: netId yok (armed=%d connected=%d) -> diger cihaza bildirilmedi",
             idx, (int)g_VehNetArmed.load(), (int)g_IsConnected.load());
    if (netId >= 0 && !g_ApplyingRemoteVehicleOp) {
        VehicleRemovePacket pkt;
        pkt.type = PACKET_VEHICLE_REMOVE;
        pkt.originId = g_LocalPlayerId.load();
        pkt.netId = (uint16_t)netId;
        QueueVehicleEvent(&pkt, sizeof(pkt));
        LOGI("[VEHNET] remove idx=%u netId=%d", idx, netId);
    }
    orig_Game_removeVehicle(game, idx);
}

// NOT: Game::buyItem / Game::sellItem artik KANCALANMIYOR. buyItem'in ilk 8 baytinda
// "ldr r1,[pc,#0x244]" var; Substrate bunu tasirken bozuyor ve host satin alirken oyun cokuyordu.
// Gerek de yok: alim/satim zaten addVehicle/removeVehicle kancalarindan iki yone de yansiyor.

// Vehicle::detachTool / detachTrailer (Vehicle* this, b2World* dunya): sadece CAGRILIR, kancalanmaz.
typedef int (*Vehicle_detach_t)(void* vehicle, void* world);
static Vehicle_detach_t g_VehicleDetachToolFn = nullptr;
static Vehicle_detach_t g_VehicleDetachTrailerFn = nullptr;

// Diger cihazlardan gelen arac ekle/sil olaylarini oyun thread'inde uygular.
static void ApplyPendingVehicleOps(uintptr_t game) {
    if (game == 0 || !g_IsConnected.load()) return;
    RefreshVehicleTopology(game);
    if (!g_VehNetArmed.load()) {                     // tablo hazir degil: olaylar bekler
        static long long s_lastWaitLog = 0;
        const long long t = NowMs();
        if (t - s_lastWaitLog > 2000) { s_lastWaitLog = t; LOGI("[VEHNET] olaylar bekliyor: tablo henuz kurulmadi"); }
        return;
    }
    std::vector<PendingVehicleOp> ops;
    {
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        if (g_PendingVehicleOps.empty()) return;
        ops.swap(g_PendingVehicleOps);
    }
    for (const PendingVehicleOp& op : ops) {
        if (op.netId != 0xFFFF && op.netId >= VEHICLE_SLOT_LIMIT) continue;

        if (op.kind == 2) {                          // REMOVE
            if (!orig_Game_removeVehicle || !g_VehicleDestroyFn) { LOGI("[VEHNET] remove: kanca yok"); continue; }
            const uintptr_t p = (op.netId < VEHICLE_SLOT_LIMIT) ? g_KnownVehiclePointers[op.netId] : 0;
            const int idx = p ? SlotIndexOfVehicle(game, p) : -1;
            if (idx < 0) {
                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                ClearVehicleStateSlot(op.netId);
                continue;
            }
            // Bagli alet/romork varsa once ayir: Game::sellItem de siraiyla detachTool, detachTrailer,
            // removeVehicle, Vehicle::destroy cagirir. (Eskiden burada satis ATLANIYORDU: ornegin
            // bicerdoverin tablasi bagliyken client'ta arac hic silinmiyordu.)
            if (*(uintptr_t*)(p + 0x55C) != 0 || *(uintptr_t*)(p + 0x560) != 0) {
                void* world = *(void**)(game + 0xA0);
                if (g_VehicleDetachToolFn && world) g_VehicleDetachToolFn((void*)p, world);
                if (g_VehicleDetachTrailerFn && world) g_VehicleDetachTrailerFn((void*)p, world);
            }
            if (*(uintptr_t*)(p + 0x55C) != 0 || *(uintptr_t*)(p + 0x560) != 0) {
                LOGI("[VEHNET] remove netId=%u atlandi (romork/alet ayrilamadi)", (unsigned)op.netId);
                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                ClearVehicleStateSlot(op.netId);
                continue;
            }
            LOGI("[VEHNET] remove netId=%u idx=%d uygulaniyor", (unsigned)op.netId, idx);
            {
                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                ClearVehicleStateSlot(op.netId);
            }
            g_ApplyingRemoteVehicleOp = true;
            orig_Game_removeVehicle((void*)game, (uint32_t)idx);
            g_VehicleDestroyFn((void*)p);
            g_ApplyingRemoteVehicleOp = false;
            g_NetMapDirty.store(true);
            if (g_IsHost.load()) g_HostForceTable.store(true);
            continue;
        }

        // SPAWN (kind 0: host, client istegini uyguluyor / kind 1: host netId atamis)
        if (!orig_Game_addVehicle) { LOGI("[VEHNET] spawn: addVehicle kancasi yok"); continue; }
        if (op.kind == 1 && op.originId == g_LocalPlayerId.load()) {
            // Bu cihazda zaten alinmisti: sirada bekleyen yerel araci host'un verdigi netId'ye bagla.
            while (!g_UnboundLocalVehicles.empty()) {
                const uintptr_t p = g_UnboundLocalVehicles.front();
                g_UnboundLocalVehicles.erase(g_UnboundLocalVehicles.begin());
                if (SlotIndexOfVehicle(game, p) < 0) continue;
                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                g_KnownVehiclePointers[op.netId] = p;
                break;
            }
            g_NetMapDirty.store(true);
            continue;
        }
        if (VehicleCount(game) >= 30) { LOGI("[VEHNET] spawn atlandi: arac siniri"); continue; }
        int netId = op.netId;
        if (op.kind == 0) {
            if (!g_IsHost.load()) continue;
            netId = AllocNetId();
            if (netId < 0) continue;
        }
        const float pos[3] = { op.x, op.y, op.z };
        g_ApplyingRemoteVehicleOp = true;
        const uint32_t idx = orig_Game_addVehicle((void*)game, (int)op.vehicleType, pos, op.angleBits, 0);
        g_ApplyingRemoteVehicleOp = false;
        if (idx >= VehicleCount(game)) continue;
        {
            std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
            g_KnownVehiclePointers[netId] = RawVehicleSlot(game, idx);
        }
        g_NetMapDirty.store(true);
        if (op.kind == 0) {                          // host: herkese (istegi yapan dahil) netId'yi bildir
            VehicleSpawnPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.type = PACKET_VEHICLE_SPAWN;
            pkt.originId = op.originId;
            pkt.netId = (uint16_t)netId;
            pkt.vehicleType = op.vehicleType;
            pkt.x = op.x; pkt.y = op.y; pkt.z = op.z;
            pkt.angleBits = op.angleBits;
            QueueVehicleEvent(&pkt, sizeof(pkt));
            g_HostForceTable.store(true);
        }
        LOGI("[VEHNET] spawn uygulandi type=%u netId=%d idx=%u", (unsigned)op.vehicleType, netId, idx);
    }
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
    pkt.sequence = (uint32_t)GetMonotonicMilliseconds(); // sample time = stream clock for the receiver
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

    // Yeni katilan / senkron isteyen oyuncu icin: aktif ve sahip olunan araclari hemen yeniden gonder.
    if (g_ForceResendAll.exchange(false)) {
        std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
        for (uint16_t i = 0; i < VEHICLE_SLOT_LIMIT; ++i) g_LastSentLocalVehicles[i].valid = false;
    }

    uint16_t activeVehicleId = VEHICLE_ID_INVALID;
    GetActiveVehicleFromGame(game, &activeVehicleId);

    const uint8_t localOwner = g_LocalPlayerId.load();
    if (localOwner == VEHICLE_OWNER_NONE || localOwner >= MAX_PLAYERS) return;

    const uint64_t nowMs = GetMonotonicMilliseconds();

    for (uint16_t vehicleId = 0; vehicleId < VEHICLE_SLOT_LIMIT; ++vehicleId) {
        if (g_KnownVehiclePointers[vehicleId] == 0) continue;
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
            // Only changed vehicles go on the wire, plus a slow keep-alive so a player
            // who joins later still learns where the parked vehicles are.
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                VehicleSampleState& sent = g_LastSentLocalVehicles[vehicleId];
                changed = !sent.valid ||
                          sent.moving != moving ||
                          fabsf(x - sent.x) > POSITION_EPSILON ||
                          fabsf(y - sent.y) > POSITION_EPSILON ||
                          GetAngleDelta(angle, sent.angle) > ANGLE_EPSILON ||
                          (nowMs - sent.timeMs) >= VEHICLE_KEEPALIVE_MS;
                if (changed) {
                    sent.valid = true;
                    sent.moving = moving;
                    sent.x = x;
                    sent.y = y;
                    sent.angle = angle;
                    sent.timeMs = nowMs;
                }
            }
            if (changed) QueueVehiclePosition(vehicleId, x, y, angle, moving);
        }
    }
}

// Where a remote vehicle should be right now: its newest snapshot pushed forward by the
// measured speed (dead reckoning), capped so a lost packet cannot launch it.
static void PredictRemotePose(const VehicleRemoteState& s, uint64_t nowMs,
                              float* x, float* y, float* angle) {
    float age = 0.0f;
    if (s.moving && nowMs > s.lastReceiveMs) {
        age = (float)(nowMs - s.lastReceiveMs) * 0.001f;
        if (age > REMOTE_MAX_EXTRAPOLATION_SEC) age = REMOTE_MAX_EXTRAPOLATION_SEC;
    }
    *x = s.x + s.vx * age;
    *y = s.y + s.vy * age;
    *angle = s.angle + s.angularVelocity * age;
}

// Box2D 2.1 b2Body layout, checked against b2Body::SetLinearVelocity / SetTransform in the
// decompile: +0x00 type, +0x04 flags (bit 1 = awake), +0x44/+0x48 linear velocity,
// +0x4C angular velocity, +0x90 sleep time. Giving the remote body its real speed keeps
// joints, trailers and contacts calm; a body that teleports with zero speed makes them fight.
static void SetRemoteBodyVelocity(uintptr_t body, float vx, float vy, float w) {
    if (*(int*)body == 0) return; // static body
    if (vx * vx + vy * vy > 0.0f && (*(uint16_t*)(body + 4) & 2) == 0) {
        *(uint16_t*)(body + 4) |= 2;
        *(float*)(body + 0x90) = 0.0f;
    }
    *(float*)(body + 0x44) = vx;
    *(float*)(body + 0x48) = vy;
    *(float*)(body + 0x4C) = w;
}

static void ApplyRemoteVehicleStates(uintptr_t game) {
    if (game == 0 || !g_b2BodySetTransform) return;
    if (!g_IsConnected.load()) return;

    ApplyPendingVehicleOps(game);          // (icinde RefreshVehicleTopology da calisir)

    const uint8_t localOwner = g_LocalPlayerId.load();
    if (localOwner == VEHICLE_OWNER_NONE || localOwner >= MAX_PLAYERS) return;

    const uint64_t nowMs = GetMonotonicMilliseconds();

    // Frame-rate independent fade for the visual correction offsets.
    static uint64_t s_lastApplyMs = 0;
    float frameSec = (s_lastApplyMs != 0 && nowMs > s_lastApplyMs)
        ? (float)(nowMs - s_lastApplyMs) * 0.001f : 0.016f;
    if (frameSec > 0.1f) frameSec = 0.1f;
    s_lastApplyMs = nowMs;
    const float decay = expf(-frameSec / REMOTE_ERROR_DECAY_SEC);

    for (uint16_t vehicleId = 0; vehicleId < VEHICLE_SLOT_LIMIT; ++vehicleId) {
        VehicleRemoteState state;

        {
            std::lock_guard<std::mutex> lock(g_VehicleStateMutex);

            // A snapshot may arrive before the host's authority packet.
            // Keep buffering it, but NEVER apply it until this vehicle is
            // explicitly owned by that remote player. This is the critical
            // separation between network reception and local physics control.
            const uint8_t authorityOwner = g_VehicleAuthority[vehicleId].ownerId;
            if (authorityOwner == VEHICLE_OWNER_NONE || authorityOwner == localOwner) continue;

            VehicleRemoteState& stored = g_RemoteVehicles[vehicleId];
            if (!stored.valid) continue;

            // A remote snapshot is valid only when its sender is the current
            // authoritative owner for this exact vehicle slot.
            if (stored.ownerId != authorityOwner || stored.vehicleId != vehicleId) continue;

            stored.errX *= decay;
            stored.errY *= decay;
            stored.errA *= decay;
            if (fabsf(stored.errX) + fabsf(stored.errY) + fabsf(stored.errA) < 0.003f) {
                stored.errX = stored.errY = stored.errA = 0.0f;
            }
            state = stored;
        }

        const bool offsetGone = (state.errX == 0.0f && state.errY == 0.0f && state.errA == 0.0f);

        // Parked and already placed: leave the physics body alone.
        if (!state.moving && offsetGone && state.appliedSequence == state.sequence) continue;

        uintptr_t vehicle = GetVehicleFromIndex(game, vehicleId);
        if (vehicle == 0) continue;

        uintptr_t body = *(uintptr_t*)(vehicle + 0x528);
        if (body == 0) continue;

        float poseX, poseY, poseAngle;
        PredictRemotePose(state, nowMs, &poseX, &poseY, &poseAngle);

        TestB2Vec2 position;
        position.x = poseX + state.errX;
        position.y = poseY + state.errY;

        // This call is intentionally restricted to remotely-authoritative
        // vehicles. The local driver's physics is left completely intact.
        g_b2BodySetTransform((void*)body, &position, poseAngle + state.errA);

        if (state.moving) {
            SetRemoteBodyVelocity(body, state.vx, state.vy, state.angularVelocity);
        } else {
            SetRemoteBodyVelocity(body, 0.0f, 0.0f, 0.0f);
            if (offsetGone) {
                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                if (g_RemoteVehicles[vehicleId].sequence == state.sequence) {
                    g_RemoteVehicles[vehicleId].appliedSequence = state.sequence;
                }
            }
        }
    }
}

static void HandleSaveChunk(const SaveChunkPacket& p) {
    if (g_IsHost.load()) return;
    if (p.total == 0 || p.total > 4u * 1024u * 1024u || p.len > SAVE_CHUNK_DATA ||
        p.offset + p.len > p.total) return;
    if (p.offset == 0) {
        g_RecvSave.assign(p.total, 0);
        g_RecvSaveGot = 0;
        g_RecvSaveVersion = p.version;
    }
    if (g_RecvSave.size() != p.total) return;
    memcpy(g_RecvSave.data() + p.offset, p.data, p.len);
    g_RecvSaveGot += p.len;
    if (g_RecvSaveGot < p.total) {
        g_SaveProgress.store((int)(g_RecvSaveGot * 99 / p.total));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_SaveBlobMutex);
        g_ClientSave.swap(g_RecvSave);
        g_ClientSaveVersion = g_RecvSaveVersion;
    }
    g_SaveProgress.store(100);
    g_ClientSaveReady.store(true);
}

static void HandleSessionWelcome(const SessionWelcomePacket& pkt) {
    if (pkt.ownerId >= MAX_PLAYERS) return;
    g_LocalPlayerId.store(pkt.ownerId);
    SetPlayerName(pkt.ownerId, g_Nickname);

    char playerMsg[96];
    snprintf(playerMsg, sizeof(playerMsg), "Joined multiplayer as %s",
             g_Nickname);
    ShowNativeToast(playerMsg);

    // Tell the host our nickname as soon as the welcome packet is received.
    SendLocalPlayerInfo();
}

static void HandlePlayerInfoPacket(const PlayerInfoPacket& pkt) {
    if (pkt.playerId >= MAX_PLAYERS) return;
    char safeName[sizeof(pkt.playerName)];
    memcpy(safeName, pkt.playerName, sizeof(safeName));
    safeName[sizeof(safeName) - 1] = '\0';
    SetPlayerName(pkt.playerId, safeName);
    // Host ismimizi cakistigi icin degistirdiyse onu benimse (varsayilan tarzda ise kalici yap).
    if (!g_IsHost.load() && pkt.playerId == g_LocalPlayerId.load() && safeName[0] != 0 &&
        strcmp(safeName, g_Nickname) != 0) {
        strncpy(g_Nickname, safeName, sizeof(g_Nickname) - 1);
        g_Nickname[sizeof(g_Nickname) - 1] = 0;
        if (IsDefaultStyleName(g_Nickname)) strncpy(g_DefaultName, g_Nickname, sizeof(g_DefaultName) - 1);
    }
    LOGI("[PLAYERS] player=%u name=%s", (unsigned)pkt.playerId, safeName);
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

    // NOT: Eskiden burada "auth.generation > pkt.generation" ise paket yok sayiliyordu. Her cihaz
    // topoloji degisince kendi generation sayacini 0'a ceviriyordu; host'un yeni (kucuk) numarasi
    // client'ta reddedilip sahiplik hic guncellenmiyordu. Paketler tek TCP akisinda sirali
    // geldigi icin bu kontrole gerek yok: gelen her paket en guncel durumdur.

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

// Host'un yayinladigi TAM sahiplik tablosu. Tabloda olmayan arac = sahipsiz. Boylece tek tek
// paketler kaybolsa / yerel tablo silinse bile en gec AUTH_TABLE_INTERVAL_MS icinde duzelir.
static void HandleAuthorityTable(const uint8_t* entries, uint16_t count) {
    if (g_IsHost.load()) return;

    uint8_t owners[VEHICLE_SLOT_LIMIT];
    memset(owners, 0xFF, sizeof(owners));
    for (uint16_t i = 0; i < count; ++i) {
        uint16_t id;
        memcpy(&id, entries + (size_t)i * 3, 2);
        const uint8_t owner = entries[(size_t)i * 3 + 2];
        if (id < VEHICLE_SLOT_LIMIT && owner < MAX_PLAYERS) owners[id] = owner;
    }

    std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
    for (uint16_t v = 0; v < VEHICLE_SLOT_LIMIT; ++v) {
        VehicleAuthorityState& auth = g_VehicleAuthority[v];
        if (auth.ownerId == owners[v]) continue;
        auth.ownerId = owners[v];
        auth.generation++;
        g_ClaimPending[v] = false;
        memset(&g_RemoteVehicles[v], 0, sizeof(g_RemoteVehicles[v]));
        memset(&g_LocalSamples[v], 0, sizeof(g_LocalSamples[v]));
        memset(&g_LastSentLocalVehicles[v], 0, sizeof(g_LastSentLocalVehicles[v]));
        g_LocalStationarySince[v] = 0;
        g_RemoteStationarySince[v] = 0;
    }
}

// Host: sahipsiz bir arac icin hareket eden bir oyuncudan anlik goruntu geldiyse, o oyuncu
// aslinda aracin surucusudur (claim paketi kayboldu ya da host tablosu silindi). Beklemeden
// sahipligi ver; boylece "host o araca ilk dokunana kadar hareket gorunmez" sorunu olmaz.
static void HostAdoptIfMoving(uint8_t ownerId, uint16_t vehicleId, bool moving) {
    if (!moving || !g_IsHost.load()) return;
    if (ownerId >= MAX_PLAYERS || ownerId == g_LocalPlayerId.load()) return;
    if (vehicleId >= VEHICLE_SLOT_LIMIT) return;
    uint8_t current = VEHICLE_OWNER_NONE;
    {
        std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
        current = g_VehicleAuthority[vehicleId].ownerId;
    }
    if (current == VEHICLE_OWNER_NONE) AssignVehicleAuthority(vehicleId, ownerId, true);
}

static void StoreRemoteVehicleState(
    uint8_t ownerId,
    uint16_t vehicleId,
    float x,
    float y,
    float angle,
    uint32_t timeMs,   // sender clock in ms (QueueVehiclePosition)
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
    float vx = 0.0f, vy = 0.0f, angularVelocity = 0.0f;
    float errX = 0.0f, errY = 0.0f, errA = 0.0f;

    if (state.valid && state.ownerId == ownerId) {
        // Signed difference survives the 32-bit wrap-around of the sender clock.
        const int32_t dtMs = (int32_t)(timeMs - state.sequence);
        if (dtMs <= 0) return; // old or duplicate snapshot

        // What the player sees right now (before this snapshot changes anything).
        float oldX, oldY, oldAngle;
        PredictRemotePose(state, nowMs, &oldX, &oldY, &oldAngle);

        // Speed comes from the SENDER's clock, so network delay and bunching
        // cannot distort it.
        if (moving && dtMs >= 5 && dtMs <= 500) {
            const float dt = (float)dtMs * 0.001f;
            vx = (x - state.x) / dt;
            vy = (y - state.y) / dt;
            angularVelocity = GetSignedAngleDelta(state.angle, angle) / dt;
        }

        const bool teleport =
            hypotf(oldX - x, oldY - y) > REMOTE_TELEPORT_DISTANCE ||
            (vx * vx + vy * vy) > REMOTE_MAX_SPEED * REMOTE_MAX_SPEED;

        if (teleport) {
            vx = vy = angularVelocity = 0.0f;
        } else {
            // Do not jump to the new truth: keep the picture continuous and let the
            // difference fade out over the next few frames.
            errX = oldX + state.errX - x;
            errY = oldY + state.errY - y;
            errA = GetSignedAngleDelta(angle, oldAngle) + state.errA;
        }
    }

    state.valid = true;
    state.ownerId = ownerId;
    state.vehicleId = vehicleId;
    state.x = x;
    state.y = y;
    state.angle = angle;
    state.vx = vx;
    state.vy = vy;
    state.angularVelocity = angularVelocity;
    state.errX = errX;
    state.errY = errY;
    state.errA = errA;
    state.sequence = timeMs;
    state.appliedSequence = 0;
    state.lastReceiveMs = nowMs;
    state.moving = moving;
}

static void HandleVehiclePositionPacket(
    const VehiclePositionPacket& pkt
) {
    HostAdoptIfMoving(pkt.ownerId, pkt.vehicleId, pkt.moving != 0);
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

        HostAdoptIfMoving(header.ownerId, entry.vehicleId, entry.moving != 0);
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
    g_LocalClaimSequence.store(0);
    g_LastKnownVehicleCount = 0;
    g_VehNetArmed.store(false);
    g_NetMapDirty.store(false);
    {
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        g_VehicleEventOut.clear();
        g_PendingVehicleOps.clear();
        g_PendingNetMap.clear();
        g_HasPendingNetMap = false;
        g_HostNetMapBytes.clear();
    }
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

// ------------------------------------------------------------------------
// Soft keyboard. The game view has no text editor, so the keyboard is toggled
// through InputMethodManager and Android delivers the letters as key events
// (handled in my_AInputQueue_getEvent). The text itself is shown by the typing bar.
// ------------------------------------------------------------------------
static jobject GetInputMethodManager(JNIEnv* env) {
    jclass atc = env->FindClass("android/app/ActivityThread");
    if (!atc) return nullptr;
    jmethodID cur = env->GetStaticMethodID(atc, "currentActivityThread", "()Landroid/app/ActivityThread;");
    jmethodID getApp = env->GetMethodID(atc, "getApplication", "()Landroid/app/Application;");
    if (!cur || !getApp) return nullptr;
    jobject at = env->CallStaticObjectMethod(atc, cur);
    jobject ctx = at ? env->CallObjectMethod(at, getApp) : nullptr;
    if (!ctx) return nullptr;
    jmethodID getSvc = env->GetMethodID(env->GetObjectClass(ctx), "getSystemService",
                                        "(Ljava/lang/String;)Ljava/lang/Object;");
    if (!getSvc) return nullptr;
    return env->CallObjectMethod(ctx, getSvc, env->NewStringUTF("input_method"));
}

static void ToggleSoftKeyboard(int showFlags, int hideFlags) {
    if (g_GlobalJavaVM == nullptr) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_GlobalJavaVM->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (g_GlobalJavaVM->AttachCurrentThread(&env, nullptr) != 0) return;
        attached = true;
    }
    if (env) {
        env->PushLocalFrame(16);
        jobject imm = GetInputMethodManager(env);
        jmethodID toggle = imm ? env->GetMethodID(env->GetObjectClass(imm), "toggleSoftInput", "(II)V") : nullptr;
        if (toggle) env->CallVoidMethod(imm, toggle, showFlags, hideFlags);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->PopLocalFrame(nullptr);
    }
    if (attached) g_GlobalJavaVM->DetachCurrentThread();
}

void OpenAndroidKeyboardForField(int fieldMode, const char* /*initialText*/) {
    g_KeyboardFieldMode.store(fieldMode);
    if (g_AndroidKeyboardOpen.load()) return;
    ToggleSoftKeyboard(2, 0); // SHOW_FORCED
    g_AndroidKeyboardOpen.store(true);
}

void CloseAndroidKeyboard() {
    if (g_AndroidKeyboardOpen.load()) ToggleSoftKeyboard(0, 0);
    g_AndroidKeyboardOpen.store(false);
    g_KeyboardFieldMode.store(0);
}

// Sends the chat box text (when connected and not empty) and clears it.
static void SubmitChatInput() {
    if (!g_IsConnected.load() || strlen(g_ChatInputBuffer) == 0) return;
    const std::string msgStr(g_ChatInputBuffer);
    {
        std::lock_guard<std::mutex> lock(g_ChatMutex);
        g_ChatMessages.push_back(std::string(g_Nickname) + ": " + msgStr);
    }
    {
        std::lock_guard<std::mutex> lock(g_OutgoingChatMutex);
        g_OutgoingChats.push_back(msgStr);
    }
    memset(g_ChatInputBuffer, 0, sizeof(g_ChatInputBuffer));
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
// PAKET ISLEME (client ve host ortak)
// ========================================================================
static void AppendBytes(std::vector<uint8_t>& dst, const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;
    dst.insert(dst.end(), p, p + size);
}

// Tamponda biriken tam paketleri isler. peerId: client'ta -1, host'ta gonderen oyuncunun id'si.
// Host'ta oyuncu kimligi (peerId) paketlere zorla yazilir (taklit edilemez) ve diger oyunculara
// iletilecek paketler `relay`e eklenir. Protokol hatasinda false doner.
static bool ProcessPackets(uint8_t* buf, size_t& buffered, int peerId, std::vector<uint8_t>* relay,
                           std::vector<uint8_t>* relayFast = nullptr) {
    while (buffered > 0) {
        const uint8_t type = buf[0];
        size_t size = 0;
        bool relayIt = false;

        if (type == PACKET_SESSION_WELCOME) size = sizeof(SessionWelcomePacket);
        else if (type == PACKET_PLAYER_INFO) { size = sizeof(PlayerInfoPacket); relayIt = true; }
        else if (type == PACKET_HOST_STATE) size = sizeof(HostStatePacket);
        else if (type == PACKET_SAVE_REQUEST) size = sizeof(SaveRequestPacket);
        else if (type == PACKET_SAVE_CHUNK) size = sizeof(SaveChunkPacket);
        else if (type == PACKET_VEHICLE_SPAWN) size = sizeof(VehicleSpawnPacket);
        else if (type == PACKET_VEHICLE_REMOVE) { size = sizeof(VehicleRemovePacket); relayIt = true; }
        else if (type == PACKET_VEHICLE_NETMAP) {
            if (buffered < 3) break;
            uint16_t cnt;
            memcpy(&cnt, buf + 1, 2);
            if (cnt > VEHICLE_SLOT_LIMIT) return false;
            size = 3 + (size_t)cnt * 2;
        }
        else if (type == PACKET_SYNC_REQUEST) size = 1;
        else if (type == PACKET_PING) size = 1;
        else if (type == PACKET_AUTH_TABLE) {
            if (buffered < 3) break;
            uint16_t cnt;
            memcpy(&cnt, buf + 1, 2);
            if (cnt > VEHICLE_SLOT_LIMIT) return false;
            size = 3 + (size_t)cnt * 3;
        }
        else if (type == PACKET_CHAT) { size = sizeof(NetworkPacket); relayIt = true; }
        else if (type == PACKET_VEHICLE_POSITION) { size = sizeof(VehiclePositionPacket); relayIt = true; }
        else if (type == PACKET_VEHICLE_CLAIM) size = sizeof(VehicleClaimPacket);
        else if (type == PACKET_VEHICLE_AUTHORITY) size = sizeof(VehicleAuthorityPacket);
        else if (type == PACKET_VEHICLE_RELEASE) size = sizeof(VehicleReleasePacket);
        else if (type == PACKET_VEHICLE_SNAPSHOT) {
            if (buffered < sizeof(VehicleSnapshotHeader)) break;
            VehicleSnapshotHeader header;
            memcpy(&header, buf, sizeof(header));
            if (header.count > VEHICLE_SNAPSHOT_MAX_COUNT) return false;
            size = sizeof(VehicleSnapshotHeader) + (size_t)header.count * sizeof(VehicleSnapshotEntry);
            relayIt = true;
        } else {
            return false;
        }

        if (buffered < size) break;

        const bool fromPeer = (peerId >= 0);
        if (type == PACKET_PLAYER_INFO) {
            PlayerInfoPacket pkt;
            memcpy(&pkt, buf, sizeof(pkt));
            if (fromPeer) {
                pkt.playerId = (uint8_t)peerId;
                pkt.playerName[sizeof(pkt.playerName) - 1] = '\0';
                if (g_IsHost.load() && MakeNameUnique(pkt.playerId, pkt.playerName, sizeof(pkt.playerName))) {
                    g_NameFixPkt = pkt;               // sender'a duzeltilmis ismi geri bildir
                    g_NameFixPeer.store(peerId);
                }
                memcpy(buf, &pkt, sizeof(pkt));
            }
            HandlePlayerInfoPacket(pkt);
        } else if (type == PACKET_SESSION_WELCOME) {
            SessionWelcomePacket pkt;
            memcpy(&pkt, buf, sizeof(pkt));
            HandleSessionWelcome(pkt);
        } else if (type == PACKET_HOST_STATE) {
            HostStatePacket pkt;
            memcpy(&pkt, buf, sizeof(pkt));
            g_HostInGame.store(pkt.inGame != 0);
        } else if (type == PACKET_SAVE_REQUEST) {
            if (g_IsHost.load() && fromPeer) {
                g_SaveRequestPeer.store(peerId);
                g_SaveCaptureRequested.store(true);
            }
        } else if (type == PACKET_SAVE_CHUNK) {
            SaveChunkPacket pkt;
            memcpy(&pkt, buf, sizeof(pkt));
            HandleSaveChunk(pkt);
        } else if (type == PACKET_CHAT) {
            NetworkPacket pkt;
            memcpy(&pkt, buf, sizeof(pkt));
            pkt.senderName[sizeof(pkt.senderName) - 1] = '\0';
            pkt.chatData[sizeof(pkt.chatData) - 1] = '\0';
            const std::string formatted = std::string(pkt.senderName) + ": " + pkt.chatData;
            {
                std::lock_guard<std::mutex> lock(g_ChatMutex);
                g_ChatMessages.push_back(formatted);
            }
            ShowNativeToast(formatted);
        } else if (type == PACKET_VEHICLE_POSITION) {
            VehiclePositionPacket pkt;
            memcpy(&pkt, buf, sizeof(pkt));
            if (fromPeer) { pkt.ownerId = (uint8_t)peerId; memcpy(buf, &pkt, sizeof(pkt)); }
            HandleVehiclePositionPacket(pkt);
        } else if (type == PACKET_VEHICLE_SNAPSHOT) {
            VehicleSnapshotHeader header;
            memcpy(&header, buf, sizeof(header));
            if (fromPeer) { header.ownerId = (uint8_t)peerId; memcpy(buf, &header, sizeof(header)); }
            HandleVehicleSnapshotPacket(header, buf + sizeof(header));
        } else if (type == PACKET_VEHICLE_CLAIM) {
            VehicleClaimPacket pkt;
            memcpy(&pkt, buf, sizeof(pkt));
            if (fromPeer) pkt.ownerId = (uint8_t)peerId;
            HandleVehicleClaimPacket(pkt);
        } else if (type == PACKET_VEHICLE_AUTHORITY) {
            VehicleAuthorityPacket pkt;
            memcpy(&pkt, buf, sizeof(pkt));
            HandleVehicleAuthorityPacket(pkt);
        } else if (type == PACKET_VEHICLE_RELEASE) {
            VehicleReleasePacket pkt;
            memcpy(&pkt, buf, sizeof(pkt));
            if (fromPeer) pkt.ownerId = (uint8_t)peerId;
            HandleVehicleReleasePacket(pkt);
        } else if (type == PACKET_AUTH_TABLE) {
            if (!fromPeer && !g_IsHost.load()) {
                uint16_t cnt;
                memcpy(&cnt, buf + 1, 2);
                HandleAuthorityTable(buf + 3, cnt);
            }
        } else if (type == PACKET_VEHICLE_SPAWN) {
            VehicleSpawnPacket pkt;
            memcpy(&pkt, buf, sizeof(pkt));
            PendingVehicleOp op;
            memset(&op, 0, sizeof(op));
            op.originId = pkt.originId; op.netId = pkt.netId; op.vehicleType = pkt.vehicleType;
            op.x = pkt.x; op.y = pkt.y; op.z = pkt.z; op.angleBits = pkt.angleBits;
            if (fromPeer && g_IsHost.load()) {            // client'in alim istegi
                op.kind = 0; op.originId = (uint8_t)peerId; op.netId = 0xFFFF;
                PushVehicleOp(op);
            } else if (!fromPeer && !g_IsHost.load()) {   // host'tan: netId atanmis spawn
                op.kind = 1;
                PushVehicleOp(op);
            }
        } else if (type == PACKET_VEHICLE_REMOVE) {
            VehicleRemovePacket pkt;
            memcpy(&pkt, buf, sizeof(pkt));
            if (fromPeer) { pkt.originId = (uint8_t)peerId; memcpy(buf, &pkt, sizeof(pkt)); }
            PendingVehicleOp op;
            memset(&op, 0, sizeof(op));
            op.kind = 2; op.originId = pkt.originId; op.netId = pkt.netId;
            PushVehicleOp(op);
        } else if (type == PACKET_VEHICLE_NETMAP) {
            if (!fromPeer && !g_IsHost.load()) {
                uint16_t cnt;
                memcpy(&cnt, buf + 1, 2);
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                g_PendingNetMap.assign(cnt, 0xFFFF);
                if (cnt) memcpy(g_PendingNetMap.data(), buf + 3, (size_t)cnt * 2);
                g_HasPendingNetMap = true;
                g_NetMapSeq = ++g_OpSeq;
            }
        } else if (type == PACKET_SYNC_REQUEST) {
            if (g_IsHost.load() && fromPeer) {
                g_HostForceTable.store(true);
                g_ForceResendAll.store(true);
            }
        }
        // PACKET_PING: sadece "canlayim" demek, ozel islem yok (alim zamani disarida kaydedilir).

        if (relay && relayIt) {
            const bool fast = (type == PACKET_VEHICLE_SNAPSHOT || type == PACKET_VEHICLE_POSITION);
            if (relayFast && fast) AppendBytes(*relayFast, buf, size);
            else AppendBytes(*relay, buf, size);
        }

        buffered -= size;
        if (buffered > 0) memmove(buf, buf + size, buffered);
    }
    return true;
}

// Bu cihazin gonderecegi her seyi (sohbet, sahiplik, arac anlik goruntusu) tek tamponda toplar.
static void BuildOutgoing(std::vector<uint8_t>& out, std::vector<uint8_t>* fast = nullptr) {
    {   // arac ekleme/silme olaylari (guvenilir kanal)
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        if (!g_VehicleEventOut.empty()) {
            out.insert(out.end(), g_VehicleEventOut.begin(), g_VehicleEventOut.end());
            g_VehicleEventOut.clear();
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_OutgoingChatMutex);
        for (const std::string& msg : g_OutgoingChats) {
            NetworkPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.type = PACKET_CHAT;
            strncpy(pkt.senderName, g_Nickname, sizeof(pkt.senderName) - 1);
            strncpy(pkt.chatData, msg.c_str(), sizeof(pkt.chatData) - 1);
            AppendBytes(out, &pkt, sizeof(pkt));
        }
        g_OutgoingChats.clear();
    }

    {
        std::lock_guard<std::mutex> lock(g_VehicleSendMutex);
        if (g_HasPendingClaimPacket) {
            AppendBytes(out, &g_PendingClaimPacket, sizeof(g_PendingClaimPacket));
            g_HasPendingClaimPacket = false;
        }
        if (g_HasPendingReleasePacket) {
            AppendBytes(out, &g_PendingReleasePacket, sizeof(g_PendingReleasePacket));
            g_HasPendingReleasePacket = false;
        }
        for (const VehicleAuthorityPacket& pkt : g_PendingAuthorityPackets) AppendBytes(out, &pkt, sizeof(pkt));
        g_PendingAuthorityPackets.clear();
    }

    // Arac trafigi saniyede en fazla 30 anlik goruntu; degisen tum araclar tek mesajda.
    const uint64_t nowMs = GetMonotonicMilliseconds();
    if (g_LastVehicleNetworkSendMs != 0 && (nowMs - g_LastVehicleNetworkSendMs) < VEHICLE_NETWORK_INTERVAL_MS) return;

    std::vector<VehicleSnapshotEntry> entries;
    entries.reserve(VEHICLE_SNAPSHOT_MAX_COUNT);
    {
        std::lock_guard<std::mutex> lock(g_VehicleSendMutex);
        for (uint16_t id = 0; id < VEHICLE_SLOT_LIMIT && entries.size() < VEHICLE_SNAPSHOT_MAX_COUNT; ++id) {
            if (!g_HasLatestOutgoingVehicle[id]) continue;
            const VehiclePositionPacket& src = g_LatestOutgoingVehicles[id];
            VehicleSnapshotEntry entry;
            memset(&entry, 0, sizeof(entry));
            entry.vehicleId = src.vehicleId;
            entry.x = src.x;
            entry.y = src.y;
            entry.angle = src.angle;
            entry.sequence = src.sequence;
            entry.moving = src.moving;
            entries.push_back(entry);
            g_HasLatestOutgoingVehicle[id] = false; // bir kez gonderilir
        }
    }
    if (!entries.empty()) {
        VehicleSnapshotHeader header;
        memset(&header, 0, sizeof(header));
        header.type = PACKET_VEHICLE_SNAPSHOT;
        header.ownerId = g_LocalPlayerId.load();
        header.count = (uint8_t)entries.size();
        header.sequence = (uint32_t)nowMs;
        // fast verilmisse arac goruntuleri ayri tamponda toplanir (UDP ile gonderilebilsin diye).
        std::vector<uint8_t>& dst = fast ? *fast : out;
        AppendBytes(dst, &header, sizeof(header));
        AppendBytes(dst, entries.data(), entries.size() * sizeof(VehicleSnapshotEntry));
    }
    g_LastVehicleNetworkSendMs = nowMs;
}

// ========================================================================
// ILETISIM YARDIMCILARI
//   - UDP: arac goruntuleri (en yeni durum kazanir; kayip paket bir sonrakiyle telafi olur,
//     TCP'deki "bas bloklama" yok). Sohbet / sahiplik / kayit hala guvenilir TCP'den gider.
//   - Host: oyuncu basina engellemeyen gonderim kuyrugu (yavas bir telefon herkesi dondurmaz).
// ========================================================================
static void SetLanQos(int fd) {
#ifdef IP_TOS
    int tos = 0xB8;   // EF: Wi-Fi'da (WMM) yuksek oncelikli kuyruk
    setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
#endif
}

static int OpenUdpSocket(bool bindToSyncPort) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    if (bindToSyncPort) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = INADDR_ANY;
        a.sin_port = htons(TCP_SYNC_PORT);
        if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) { close(fd); return -1; }
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    SetLanQos(fd);
    return fd;
}

// Arac paketlerinden olusan tamponu tek tek paketlere boler (her biri ayri datagram olur).
static void SplitFastPackets(const std::vector<uint8_t>& v, std::vector<std::pair<size_t, size_t> >& outPk) {
    size_t off = 0;
    while (off < v.size()) {
        const uint8_t type = v[off];
        size_t size = 0;
        if (type == PACKET_VEHICLE_POSITION) {
            size = sizeof(VehiclePositionPacket);
        } else if (type == PACKET_VEHICLE_SNAPSHOT) {
            if (off + sizeof(VehicleSnapshotHeader) > v.size()) break;
            VehicleSnapshotHeader h;
            memcpy(&h, v.data() + off, sizeof(h));
            size = sizeof(h) + (size_t)h.count * sizeof(VehicleSnapshotEntry);
        } else {
            break;
        }
        if (off + size > v.size()) break;
        outPk.push_back(std::make_pair(off, size));
        off += size;
    }
}

static void SendDatagrams(int fd, const struct sockaddr_in& to, const std::vector<uint8_t>& v) {
    std::vector<std::pair<size_t, size_t> > pk;
    SplitFastPackets(v, pk);
    for (size_t i = 0; i < pk.size(); ++i) {
        sendto(fd, v.data() + pk[i].first, pk[i].second, MSG_DONTWAIT,
               (const struct sockaddr*)&to, sizeof(to));
    }
}

static void AppendHostNetMap(std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
    if (!g_HostNetMapBytes.empty()) out.insert(out.end(), g_HostNetMapBytes.begin(), g_HostNetMapBytes.end());
}

// Host'un bildigi TUM arac sahiplikleri tek pakette: [type][count u16]{id u16, owner u8}*
static void AppendAuthorityTable(std::vector<uint8_t>& out) {
    std::vector<uint8_t> entries;
    uint16_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
        for (uint16_t v = 0; v < VEHICLE_SLOT_LIMIT; ++v) {
            const uint8_t owner = g_VehicleAuthority[v].ownerId;
            if (owner == VEHICLE_OWNER_NONE) continue;
            AppendBytes(entries, &v, 2);
            entries.push_back(owner);
            ++count;
        }
    }
    out.push_back(PACKET_AUTH_TABLE);
    AppendBytes(out, &count, 2);
    if (!entries.empty()) AppendBytes(out, entries.data(), entries.size());
}

// ========================================================================
// CLIENT AG DONGUSU (TCP: host'a guvenilir kanal, UDP: arac goruntuleri)
// ========================================================================
void NetworkLoop() {
    fcntl(g_TcpSocket, F_SETFL, O_NONBLOCK);

    // UDP kanali: host'un TCP adresiyle ayni IP, ayni port numarasi.
    int udpFd = -1;
    struct sockaddr_in hostUdp;
    memset(&hostUdp, 0, sizeof(hostUdp));
    {
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        memset(&peer, 0, sizeof(peer));
        if (getpeername(g_TcpSocket, (struct sockaddr*)&peer, &pl) == 0 && peer.sin_family == AF_INET) {
            udpFd = OpenUdpSocket(false);
            hostUdp = peer;
            hostUdp.sin_port = htons(TCP_SYNC_PORT);
        }
    }
    bool udpReady = false;                       // host UDP'yi onayladi mi?
    bool udpWarned = false;                      // "UDP desteklenmiyor" uyarisi baglanti basina bir kez
    const long long connStartMs = NowMs();
    const char* kUdpWarn = "UDP is not supported on this device. The game may not run properly.";
    if (udpFd < 0) { udpWarned = true; ShowNativeToast(kUdpWarn); }
    long long lastUdpRxMs = 0, lastUdpPingMs = 0, lastHelloMs = 0, lastPingMs = 0;
    long long lastRecvMs = NowMs();              // host'tan en son ne zaman bir sey geldi
    g_SyncRequestOut.store(true);                // baglanir baglanmaz tam senkron iste

    uint8_t buf[4096];
    size_t buffered = 0;
    std::vector<uint8_t> out, fast;

    while (g_IsConnected.load() && g_TcpSocket >= 0) {
        const long long nowT = NowMs();

        // --- TCP al ---
        if (buffered < sizeof(buf)) {
            const ssize_t n = recv(g_TcpSocket, buf + buffered, sizeof(buf) - buffered, 0);
            if (n > 0) {
                buffered += (size_t)n;
                lastRecvMs = nowT;
            } else if (n == 0) {
                ShowNativeToast("Connection Lost (Other player left)!");
                g_IsConnected.store(false);
                break;
            } else if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
                ShowNativeToast("Error: Network Connection Lost!");
                g_IsConnected.store(false);
                break;
            }
        }

        // --- UDP al (sadece host'tan; sadece arac goruntuleri / ping / onay) ---
        if (udpFd >= 0) {
            for (int k = 0; k < 64; ++k) {
                uint8_t dg[1500];
                struct sockaddr_in from;
                socklen_t fl = sizeof(from);
                memset(&from, 0, sizeof(from));
                const ssize_t n = recvfrom(udpFd, dg, sizeof(dg), MSG_DONTWAIT, (struct sockaddr*)&from, &fl);
                if (n <= 0) break;
                if (from.sin_addr.s_addr != hostUdp.sin_addr.s_addr) continue;
                lastRecvMs = nowT;
                lastUdpRxMs = nowT;
                if (dg[0] == PACKET_UDP_ACK) {
                    if (!udpReady) {
                        LOGI("[UDP] fast channel active");
                    }
                    udpReady = true;
                    continue;
                }
                if (dg[0] != PACKET_VEHICLE_SNAPSHOT && dg[0] != PACKET_VEHICLE_POSITION) continue;
                size_t len = (size_t)n;
                ProcessPackets(dg, len, -1, nullptr);
            }
            if (udpReady && (nowT - lastUdpRxMs) > UDP_STALE_MS) udpReady = false;  // yol bozuldu: TCP'ye don
        }

        if (!ProcessPackets(buf, buffered, -1, nullptr)) {
            g_IsConnected.store(false);
            break;
        }

        // --- Host sessiz kaldi: zorla kapatildi / dondu / ag koptu ---
        if (nowT - lastRecvMs > HOST_SILENCE_TIMEOUT_MS) {
            ShowNativeToast("Connection Lost (host is not responding)!");
            g_IsConnected.store(false);
            break;
        }

        // UDP yolu ilk 6 sn icinde hic acilmadiysa (cihaz/ag UDP'yi desteklemiyor) bir kez uyar.
        if (!udpWarned && !udpReady && (nowT - connStartMs) > 6000) { udpWarned = true; ShowNativeToast(kUdpWarn); }

        // --- UDP el sikisma: onay gelene kadar 250 ms'de bir ---
        const uint8_t myId = g_LocalPlayerId.load();
        if (udpFd >= 0 && !udpReady && myId < MAX_PLAYERS && (nowT - lastHelloMs) >= 250) {
            lastHelloMs = nowT;
            const uint8_t hello[2] = { PACKET_UDP_HELLO, myId };
            sendto(udpFd, hello, sizeof(hello), MSG_DONTWAIT, (const struct sockaddr*)&hostUdp, sizeof(hostUdp));
        }
        if (udpFd >= 0 && udpReady && (nowT - lastUdpPingMs) >= PING_INTERVAL_MS) {
            lastUdpPingMs = nowT;
            const uint8_t ping = PACKET_PING;
            sendto(udpFd, &ping, 1, MSG_DONTWAIT, (const struct sockaddr*)&hostUdp, sizeof(hostUdp));
        }

        // --- Gonder ---
        out.clear();
        fast.clear();
        if (g_SaveRequestOut.exchange(false)) {
            SaveRequestPacket rq;
            rq.type = PACKET_SAVE_REQUEST;
            AppendBytes(out, &rq, sizeof(rq));
        }
        if (g_SyncRequestOut.exchange(false)) out.push_back(PACKET_SYNC_REQUEST);
        if ((nowT - lastPingMs) >= 1000) { lastPingMs = nowT; out.push_back(PACKET_PING); }
        BuildOutgoing(out, &fast);
        if (!fast.empty()) {
            if (udpFd >= 0 && udpReady) SendDatagrams(udpFd, hostUdp, fast);
            else AppendBytes(out, fast.data(), fast.size());     // UDP yoksa eskisi gibi TCP
        }
        if (!out.empty() && !SendAllBytes(g_TcpSocket, out.data(), out.size())) {
            g_IsConnected.store(false);
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (udpFd >= 0) close(udpFd);
}

// ========================================================================
// HOST AG DONGUSU (en fazla MAX_PLAYERS-1 client; yildiz topoloji, host iletir)
// ========================================================================
struct HostPeer {
    int fd;
    uint8_t id;
    bool dead;
    size_t buffered;
    uint8_t buf[4096];

    std::vector<uint8_t> sendQ;      // gonderilmeyi bekleyen guvenilir (TCP) veri
    size_t sendOff;
    long long lastRecvMs;            // bu oyuncudan en son ne zaman bir sey geldi
    long long lastProgressMs;        // kuyruktan en son ne zaman veri cikti
    long long lastUdpMs;             // UDP yolundan en son ne zaman bir sey geldi
    uint32_t ip;                     // TCP kaynak adresi (UDP kimlik dogrulamasi icin)
    bool udpReady;
    struct sockaddr_in udpAddr;
};

static std::atomic<int> g_HostGen(0);   // yeni oda acilinca eski host dongusu kendiliginden biter

static const size_t PEER_SENDQ_MAX = 8u * 1024u * 1024u;   // kayit aktarimi dahil, bunun uzeri = takilmis

// Veriyi oyuncunun kuyruguna ekler; ASLA beklemez.
static void PeerQueue(HostPeer* p, const void* data, size_t size) {
    if (p->dead || size == 0) return;
    if (p->sendOff >= p->sendQ.size()) {      // kuyruk bos: ilerleme saatini bastan baslat
        p->sendQ.clear();
        p->sendOff = 0;
        p->lastProgressMs = NowMs();
    }
    if (p->sendQ.size() - p->sendOff + size > PEER_SENDQ_MAX) { p->dead = true; return; }
    AppendBytes(p->sendQ, data, size);
}

// Kuyruktan soketin kabul ettigi kadarini gonderir; dolu ise birakir, sonraki turda devam eder.
static void PeerFlush(HostPeer* p) {
    if (p->dead) return;
    while (p->sendOff < p->sendQ.size()) {
        const ssize_t n = send(p->fd, p->sendQ.data() + p->sendOff, p->sendQ.size() - p->sendOff,
                               MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            p->sendOff += (size_t)n;
            p->lastProgressMs = NowMs();
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        p->dead = true;
        return;
    }
    if (p->sendOff >= p->sendQ.size()) {
        p->sendQ.clear();
        p->sendOff = 0;
    } else {
        if (NowMs() - p->lastProgressMs > 5000) { p->dead = true; return; }   // 5 sn hic ilerlemedi
        if (p->sendOff > 65536) {
            p->sendQ.erase(p->sendQ.begin(), p->sendQ.begin() + (ptrdiff_t)p->sendOff);
            p->sendOff = 0;
        }
    }
}

// Arac goruntusu: UDP hazirsa datagram (kayip olursa sorun degil), degilse TCP kuyrugu.
static void PeerSendFast(HostPeer* p, int udpFd, const uint8_t* pkt, size_t size) {
    if (p->dead) return;
    if (p->udpReady && udpFd >= 0) {
        sendto(udpFd, pkt, size, MSG_DONTWAIT, (const struct sockaddr*)&p->udpAddr, sizeof(p->udpAddr));
        return;
    }
    PeerQueue(p, pkt, size);
}

static void HostNetworkLoop(int serverFd, int gen) {
    std::vector<HostPeer*> peers;
    std::vector<uint8_t> out, fast, relay, relayFast;
    int lastSentInGame = -1;
    long long lastPingMs = 0, lastTableMs = 0;

    const int udpFd = OpenUdpSocket(true);   // -1 ise her sey TCP'den gider (eskisi gibi)
    if (udpFd < 0) ShowNativeToast("UDP is not supported on this device. The game may not run properly.");

    auto broadcast = [&](const void* data, size_t size, const HostPeer* except) {
        for (HostPeer* p : peers) {
            if (p == except || p->dead) continue;
            PeerQueue(p, data, size);
        }
    };

    auto broadcastFast = [&](const std::vector<uint8_t>& v, const HostPeer* except) {
        if (v.empty()) return;
        std::vector<std::pair<size_t, size_t> > pk;
        SplitFastPackets(v, pk);
        for (HostPeer* p : peers) {
            if (p == except || p->dead) continue;
            for (size_t k = 0; k < pk.size(); ++k) PeerSendFast(p, udpFd, v.data() + pk[k].first, pk[k].second);
        }
    };

    auto sendPlayerInfo = [&](uint8_t id, const std::string& name, HostPeer* only) {
        PlayerInfoPacket info;
        memset(&info, 0, sizeof(info));
        info.type = PACKET_PLAYER_INFO;
        info.playerId = id;
        strncpy(info.playerName, name.c_str(), sizeof(info.playerName) - 1);
        if (only) PeerQueue(only, &info, sizeof(info));
        else broadcast(&info, sizeof(info), nullptr);
    };

    while (g_IsHost.load() && g_HostGen.load() == gen) {
        const long long nowT = NowMs();

        // 1) Yeni oyuncu kabul et (oda doluysa hemen kapat).
        struct pollfd pfd;
        pfd.fd = serverFd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            struct sockaddr_in ca;
            socklen_t cl = sizeof(ca);
            memset(&ca, 0, sizeof(ca));
            const int fd = accept(serverFd, (struct sockaddr*)&ca, &cl);
            if (fd >= 0) {
                uint8_t freeId = 0;
                for (uint8_t id = 1; id < MAX_PLAYERS && freeId == 0; ++id) {
                    bool used = false;
                    for (HostPeer* p : peers) if (p->id == id) used = true;
                    if (!used) freeId = id;
                }
                if (freeId == 0) {
                    close(fd);
                } else {
                    ConfigureLowLatencySocket(fd);
                    fcntl(fd, F_SETFL, O_NONBLOCK);
                    HostPeer* peer = new HostPeer();
                    peer->fd = fd;
                    peer->id = freeId;
                    peer->dead = false;
                    peer->buffered = 0;
                    peer->sendOff = 0;
                    peer->lastRecvMs = nowT;
                    peer->lastProgressMs = nowT;
                    peer->lastUdpMs = nowT;
                    peer->ip = ca.sin_addr.s_addr;
                    peer->udpReady = false;
                    memset(&peer->udpAddr, 0, sizeof(peer->udpAddr));

                    SessionWelcomePacket welcome;
                    memset(&welcome, 0, sizeof(welcome));
                    welcome.type = PACKET_SESSION_WELCOME;
                    welcome.ownerId = freeId;
                    welcome.maxPlayers = MAX_PLAYERS;
                    HostStatePacket hs;
                    hs.type = PACKET_HOST_STATE;
                    hs.inGame = g_LocalInGame.load() ? 1 : 0;
                    // Kuyruk henuz bos oldugu icin bu iki dogrudan gonderim akisi bozmaz.
                    if (!SendAllBytes(fd, &welcome, sizeof(welcome)) || !SendAllBytes(fd, &hs, sizeof(hs))) {
                        close(fd);
                        delete peer;
                    } else {
                        // Mevcut oyuncu listesini yeni gelene gonder.
                        for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
                            const std::string name = GetPlayerName(id);
                            if (!name.empty() && id != freeId) sendPlayerInfo(id, name, peer);
                        }
                        peers.push_back(peer);
                        g_IsConnected = true;
                        ShowNativeToast("A player joined the room!");
                        QueueAllCurrentAuthorities();
                        g_HostForceTable.store(true);     // yeni gelen hemen tam sahiplik tablosunu alsin
                        g_ForceResendAll.store(true);     // host'un araclari hemen yeniden gonderilsin
                    }
                }
            }
        }

        // 2) Oyunculardan (TCP) gelenleri al, isle, digerlerine ilet.
        for (size_t i = 0; i < peers.size(); ++i) {
            HostPeer* p = peers[i];
            if (p->dead) continue;
            if (p->buffered < sizeof(p->buf)) {
                const ssize_t n = recv(p->fd, p->buf + p->buffered, sizeof(p->buf) - p->buffered, 0);
                if (n > 0) { p->buffered += (size_t)n; p->lastRecvMs = nowT; }
                else if (n == 0) p->dead = true;
                else if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) p->dead = true;
            }
            if (p->dead) continue;
            if (nowT - p->lastRecvMs > PEER_SILENCE_TIMEOUT_MS) { p->dead = true; continue; }
            relay.clear();
            relayFast.clear();
            if (!ProcessPackets(p->buf, p->buffered, p->id, &relay, &relayFast)) { p->dead = true; continue; }
            if (!relay.empty()) broadcast(relay.data(), relay.size(), p);
            broadcastFast(relayFast, p);
            if (g_NameFixPeer.load() == p->id) {      // isim cakisti: duzeltilmisini sender'a da gonder
                PeerQueue(p, &g_NameFixPkt, sizeof(g_NameFixPkt));
                g_NameFixPeer.store(-1);
            }
        }

        // 2b) UDP: el sikisma + arac goruntuleri.
        if (udpFd >= 0) {
            for (int k = 0; k < 64; ++k) {
                uint8_t dg[1500];
                struct sockaddr_in from;
                socklen_t fl = sizeof(from);
                memset(&from, 0, sizeof(from));
                const ssize_t n = recvfrom(udpFd, dg, sizeof(dg), MSG_DONTWAIT, (struct sockaddr*)&from, &fl);
                if (n <= 0) break;

                if (dg[0] == PACKET_UDP_HELLO) {
                    if (n < 2) continue;
                    for (HostPeer* p : peers) {
                        // Kimlik: oyuncu numarasi + TCP baglantisiyla ayni IP adresi.
                        if (p->dead || p->id != dg[1] || p->ip != from.sin_addr.s_addr) continue;
                        p->udpAddr = from;
                        p->udpReady = true;
                        p->lastUdpMs = nowT;
                        const uint8_t ack = PACKET_UDP_ACK;
                        sendto(udpFd, &ack, 1, MSG_DONTWAIT, (const struct sockaddr*)&from, sizeof(from));
                    }
                    continue;
                }

                HostPeer* src = nullptr;
                for (HostPeer* p : peers) {
                    if (!p->dead && p->udpReady && p->udpAddr.sin_addr.s_addr == from.sin_addr.s_addr &&
                        p->udpAddr.sin_port == from.sin_port) { src = p; break; }
                }
                if (!src) continue;
                src->lastUdpMs = nowT;
                src->lastRecvMs = nowT;
                if (dg[0] != PACKET_VEHICLE_SNAPSHOT && dg[0] != PACKET_VEHICLE_POSITION) continue;  // PING vb.

                size_t len = (size_t)n;
                relay.clear();
                relayFast.clear();
                ProcessPackets(dg, len, src->id, &relay, &relayFast);
                broadcastFast(relayFast, src);
            }
            // UDP yolu bozulduysa bu oyuncuya TCP'den gonder (client yeniden el sikisir).
            for (HostPeer* p : peers) {
                if (p->udpReady && (nowT - p->lastUdpMs) > UDP_STALE_MS) p->udpReady = false;
            }
        }

        // 3) Host'un kendi giden verisi: hepsine yayinla.
        if (!peers.empty()) {
            const int cur = g_LocalInGame.load() ? 1 : 0;
            if (cur != lastSentInGame) {
                HostStatePacket hs;
                hs.type = PACKET_HOST_STATE;
                hs.inGame = (uint8_t)cur;
                broadcast(&hs, sizeof(hs), nullptr);
                lastSentInGame = cur;
            }

            // Host oyun dongusu calisiyorsa "canli": ping + sahiplik tablosu. Oyun zorla kapatilip
            // surec bir sure yasasa bile bu sinyaller kesilir, client'lar 10 sn icinde atilir.
            const bool hostAlive = (nowT - g_LastGameUpdateMs.load()) < HOST_ALIVE_WINDOW_MS;
            out.clear();
            fast.clear();
            if (hostAlive && (nowT - lastPingMs) >= PING_INTERVAL_MS) {
                lastPingMs = nowT;
                out.push_back(PACKET_PING);
                if (udpFd >= 0) {
                    const uint8_t ping = PACKET_PING;
                    for (HostPeer* p : peers) {
                        if (!p->dead && p->udpReady)
                            sendto(udpFd, &ping, 1, MSG_DONTWAIT, (const struct sockaddr*)&p->udpAddr, sizeof(p->udpAddr));
                    }
                }
            }
            if (g_HostForceTable.exchange(false) || (hostAlive && (nowT - lastTableMs) >= AUTH_TABLE_INTERVAL_MS)) {
                lastTableMs = nowT;
                AppendHostNetMap(out);
                AppendAuthorityTable(out);
            }
            BuildOutgoing(out, &fast);
            if (!out.empty()) broadcast(out.data(), out.size(), nullptr);
            broadcastFast(fast, nullptr);

            // Host kaydi: sadece isteyen oyuncuya parca parca gonder (kuyruga; host dongusu beklemez).
            if (g_HostBlobReady.load()) {
                std::vector<uint8_t> blob;
                uint32_t version = 0;
                {
                    std::lock_guard<std::mutex> lock(g_SaveBlobMutex);
                    blob.swap(g_HostBlob);
                    version = g_HostBlobVersion;
                    g_HostBlobReady.store(false);
                }
                const int target = g_SaveRequestPeer.load();
                for (HostPeer* p : peers) {
                    if (p->dead || p->id != target) continue;
                    for (size_t off = 0; off < blob.size() && !p->dead; off += SAVE_CHUNK_DATA) {
                        SaveChunkPacket c;
                        memset(&c, 0, sizeof(c));
                        c.type = PACKET_SAVE_CHUNK;
                        c.total = (uint32_t)blob.size();
                        c.offset = (uint32_t)off;
                        c.version = version;
                        c.len = (uint16_t)std::min<size_t>(SAVE_CHUNK_DATA, blob.size() - off);
                        memcpy(c.data, blob.data() + off, c.len);
                        PeerQueue(p, &c, sizeof(c));
                    }
                }
            }
        }

        // 3b) Kuyruklari bosalt: her oyuncuya soketin kabul ettigi kadar gonder (asla beklemez).
        for (HostPeer* p : peers) PeerFlush(p);

        // 4) Ayrilan oyunculari temizle: araclari birakilir, isim silinir, herkese duyurulur.
        for (size_t i = 0; i < peers.size();) {
            HostPeer* p = peers[i];
            if (!p->dead) { ++i; continue; }
            const uint8_t id = p->id;
            shutdown(p->fd, SHUT_RDWR);
            close(p->fd);
            peers.erase(peers.begin() + i);
            delete p;

            std::vector<uint16_t> owned;
            {
                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                for (uint16_t v = 0; v < VEHICLE_SLOT_LIMIT; ++v)
                    if (g_VehicleAuthority[v].ownerId == id) owned.push_back(v);
            }
            for (uint16_t v : owned) ReleaseVehicleAuthority(v, id, true);
            const std::string leftName = GetPlayerName(id);
            SetPlayerName(id, "");
            sendPlayerInfo(id, "", nullptr);
            ShowNativeToast((leftName.empty() ? std::string("A player") : leftName) + " left the room.");
        }

        if (peers.empty()) {
            if (g_IsConnected.load()) {
                g_IsConnected = false;
                ClearChat();
                ResetVehicleSyncState();
                lastSentInGame = -1;
            }
            g_ConnectedStatus = "Host Started. Waiting for players...";
        } else {
            char st[48];
            snprintf(st, sizeof(st), "%d player(s) connected", (int)peers.size());
            g_ConnectedStatus = st;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    for (HostPeer* p : peers) { shutdown(p->fd, SHUT_RDWR); close(p->fd); delete p; }
    if (udpFd >= 0) close(udpFd);
}

void StartPONGResponderThread() {
    // Ag kopsa / soket hata verse bile islemciyi bos donguyle yememesi icin: hata olursa
    // bekle ve soketi yeniden ac. recvfrom zaman asimli, bos beklemez.
    while (true) {
        int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) { std::this_thread::sleep_for(std::chrono::seconds(2)); continue; }
        int opt = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in server_addr, client_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(DISCOVERY_PORT);
        if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            close(sockfd);
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        char buffer[256];
        while (true) {
            socklen_t client_len = sizeof(client_addr);
            int n = recvfrom(sockfd, buffer, sizeof(buffer)-1, 0, (struct sockaddr*)&client_addr, &client_len);
            if (n > 0) {
                buffer[n] = '\0';
                if (strcmp(buffer, "FS14_PING") == 0 && g_IsHost.load()) {
                    std::string reply = "FS14_PONG|" + std::string(g_Nickname) + "'s Room";
                    sendto(sockfd, reply.c_str(), reply.length(), 0, (struct sockaddr*)&client_addr, client_len);
                }
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;   // soket bozuldu -> yeniden olustur
            }
        }
        close(sockfd);
        std::this_thread::sleep_for(std::chrono::seconds(1));
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
    const int gen = ++g_HostGen;
    ResetVehicleSyncState();
    ResetPlayerNames();
    g_LocalPlayerId.store(0);
    SetPlayerName(0, g_Nickname);

    if (!IsNetworkAvailable()) {
        ShowNativeToast("Error: Check your network connection!");
        g_ConnectedStatus = "Connection Error (No Network)";
        g_IsHost = false;
        return;
    }

    const int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(TCP_SYNC_PORT);
    if (serverFd < 0 || bind(serverFd, (struct sockaddr*)&address, sizeof(address)) < 0 ||
        listen(serverFd, MAX_PLAYERS) < 0) {
        ShowNativeToast("Error: Could not open the room!");
        g_ConnectedStatus = "Connection Error";
        if (serverFd >= 0) close(serverFd);
        g_IsHost = false;
        return;
    }
    g_TcpServerFd = serverFd;

    g_ConnectedStatus = "Host Started. Waiting for players...";
    ShowNativeToast("Room Created. Waiting for players...");

    HostNetworkLoop(serverFd, gen);

    close(serverFd);
    if (g_HostGen.load() != gen) return;     // arada yeni oda acildi: ortak durumu bozma
    g_TcpServerFd = -1;
    g_IsConnected = false;
    g_IsHost = false;
    ClearChat();
    ResetPlayerNames();
    ResetVehicleSyncState();
    if (g_ConnectedStatus != "Room Closed.") g_ConnectedStatus = "Connection Lost.";
}

void TCPClientThread(std::string hostIP) {
    g_HostInGame.store(false);
    ResetVehicleSyncState();
    ResetPlayerNames();
    g_LocalPlayerId.store(0xFF);

    g_TcpSocket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(TCP_SYNC_PORT);
    
    if (inet_pton(AF_INET, hostIP.c_str(), &serv_addr.sin_addr) <= 0) {
        ShowNativeToast("Error: Invalid room address!");
        g_IsClient = false;
        ClearChat();
        return;
    }

    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(g_TcpSocket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    g_ConnectedStatus = "Connecting to room...";
    ShowNativeToast("Connecting to room...");

    if (connect(g_TcpSocket, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) >= 0) {
        ConfigureLowLatencySocket(g_TcpSocket);
        g_IsConnected = true;
        g_ConnectedStatus = "Connected to Host!";
        ShowNativeToast("Successfully Joined Room!"); 
        NetworkLoop(); 
    } else {
        ShowNativeToast("Error: Could not connect to room!");
        g_ConnectedStatus = "Connection Failed";
        ClearChat();
    }
    
    if (g_TcpSocket >= 0) { shutdown(g_TcpSocket, SHUT_RDWR); close(g_TcpSocket); g_TcpSocket = -1; }
    g_IsConnected = false;
    g_IsClient = false;
    ClearChat();
    ResetPlayerNames();
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
                    if (action == AMOTION_EVENT_ACTION_DOWN) {
                        const float tx = AMotionEvent_getX(*outEvent, 0);
                        const float ty = AMotionEvent_getY(*outEvent, 0);
                        g_TouchX.store(tx);
                        g_TouchY.store(ty);
                        g_PendingTouchX.store(tx);
                        g_PendingTouchY.store(ty);
                        g_PendingTouchDown.store(true);
                        g_TouchDown.store(true);
                    } else if (action == AMOTION_EVENT_ACTION_MOVE) {
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
                    // DrawImGui artik cagrilmiyorsa (ornegin baslik ekranina donuldu) ImGui'nin
                    // WantCaptureMouse degeri eskidir -> dokunusu yutmasin.
                    const bool uiAlive = (NowMs() - g_LastImGuiFrameMs.load()) < 300;
                    const bool wantMouse = io.WantCaptureMouse && uiAlive;
                    if (action == AMOTION_EVENT_ACTION_DOWN) {
                        g_IsEatingTouch = wantMouse || HudButtonHit(AMotionEvent_getX(*outEvent, 0),
                                                                    AMotionEvent_getY(*outEvent, 0));
                    }
                    if (g_IsEatingTouch || wantMouse) {
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

                // Preserve the newer BACK behavior, but use the exact working
                // keyboard state from the known-good keyboard implementation.
                if (keyCode == AKEYCODE_BACK && action == AKEY_EVENT_ACTION_DOWN &&
                    g_ImGuiInitialized && ImGui::GetCurrentContext() != nullptr) {
                    if (g_IsMultiplayerMenuActive) {
                        if (g_AndroidKeyboardOpen.load()) {
                            CloseAndroidKeyboard();
                        } else {
                            MpGoBack();
                        }
                        shouldEatEvent = true;
                    } else if (g_ShowLanChoice) {
                        MpGoBack();
                        shouldEatEvent = true;
                    }
                    // g_ShowModeSelect iken BACK'i yutmuyoruz: oyun kendi BACK islemini yapip
                    // baslik ekranina doner (menu bayragi acik kalir, gorunmez olur).
                }

                if (g_AndroidKeyboardOpen.load() && action == AKEY_EVENT_ACTION_DOWN &&
                    (keyCode == AKEYCODE_ENTER || keyCode == AKEYCODE_NUMPAD_ENTER) &&
                    g_KeyboardFieldMode.load() == 2) {
                    // Native IME DONE/ENTER: send chat immediately.
                    SubmitChatInput();
                    CloseAndroidKeyboard();
                    shouldEatEvent = true;
                }

                if (g_ImGuiInitialized && ImGui::GetCurrentContext() != nullptr) {
                    ImGuiIO& io = ImGui::GetIO();
                    bool isDown = (action == AKEY_EVENT_ACTION_DOWN);

                    // Soft-keyboard letters arrive as key events: forward them to ImGui.
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

                        if (c != 0) io.AddInputCharacter(c);
                    }

                    if (io.WantCaptureKeyboard) shouldEatEvent = true;
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

// Oyunun kendi buton tiklama sesi (buton.p1d): AudioSource::play(vol, pitch, loop).
// Ses kaynagi Game+0x8910 yapisinin ilk alani; sembol bulunamazsa sessiz kalir.
typedef void (*AudioSourcePlay_t)(void* self, float vol, float pitch, bool loop);
static void PlayGameClickSound() {
    static AudioSourcePlay_t play = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        void* lib = dlopen("libapp.so", RTLD_NOW | RTLD_NOLOAD);
        if (lib) { play = (AudioSourcePlay_t)dlsym(lib, "_ZN11AudioSource4playEffb"); dlclose(lib); }
        LOGI("click sound symbol: %p", (void*)play);
    }
    if (!play || g_EngineInstance == 0) return;
    void* src = *(void**)(g_EngineInstance + 0x8910);
    if ((uintptr_t)src < 0x10000) return;
    play(src, 1.0f, 1.0f, false);
}

static bool DrawGameStyleButton(const char* id, const char* label, ImVec2 size,
                                float textScale = 1.0f, bool rightAligned = false, bool playSound = true) {
    ImVec2 pos = ImGui::GetCursorScreenPos();

    // Görselin tamamını tıklanabilir yap: texture maskesi dokunma alanını daraltmasın.
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = hovered && ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    if (clicked && playSound) PlayGameClickSound();

    ImGui::SetCursorScreenPos(pos);

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

    ImGui::SetCursorScreenPos(start);
    ImGui::InvisibleButton(id, ImVec2(totalW, h));
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    if (clicked) PlayGameClickSound();

    ImGui::SetCursorScreenPos(start);

    if (g_MultiplayerButtonTexture != 0) {
        // Buton 4 parcadan olusur: [sol uc][sol orta][sag orta][sag uc].
        // Iki yarinin bulustugu orta noktada dokunun KENAR sutunu degil, ic kismi (midU)
        // kullanilir; boylece birlesim yerinde bosluk/cizgi olmaz ve ust uste binme yoktur.
        // Uclar (egimli kisim) eski olcekte cizilir, aradaki duz kisim gerilir.
        const float overlap = std::min(42.0f, totalW * 0.055f);
        const float refW = (totalW + overlap) * 0.5f;   // eski tasarimda dokunun tamaminin genisligi
        const float capU0 = 0.86f;                      // egimli ucun basladigi u
        const float midU  = 0.50f;                      // iki yarinin bulustugu (ic) u
        const float capPx = floorf((1.0f - capU0) * refW + 0.5f);

        const float x0 = floorf(start.x + 0.5f);
        const float x1 = x0 + floorf(totalW + 0.5f);
        const float xc = floorf((x0 + x1) * 0.5f + 0.5f);
        const float y0 = start.y, y1 = start.y + h;
        ImTextureID tex = ToImGuiTexture(g_MultiplayerButtonTexture);

        // Sag yari (duz)
        draw->AddImage(tex, ImVec2(xc, y0), ImVec2(x1 - capPx, y1),
                       ImVec2(midU, 0.0f), ImVec2(capU0, 1.0f));
        draw->AddImage(tex, ImVec2(x1 - capPx, y0), ImVec2(x1, y1),
                       ImVec2(capU0, 0.0f), ImVec2(1.0f, 1.0f));
        // Sol yari (aynali)
        draw->AddImage(tex, ImVec2(x0 + capPx, y0), ImVec2(xc, y1),
                       ImVec2(capU0, 0.0f), ImVec2(midU, 1.0f));
        draw->AddImage(tex, ImVec2(x0, y0), ImVec2(x0 + capPx, y1),
                       ImVec2(1.0f, 0.0f), ImVec2(capU0, 1.0f));
    }

    ImFont* font = g_GameUIFont ? g_GameUIFont : ImGui::GetFont();
    const float textSizePx = std::max(31.0f, 36.0f * textScale *
                                             std::max(0.90f, std::min(1.12f, ImGui::GetIO().DisplaySize.y / 1080.0f)));
    ImVec2 measured = font->CalcTextSizeA(textSizePx, FLT_MAX, 0.0f, label);
    draw->AddText(font, textSizePx,
                  ImVec2(start.x + (totalW - measured.x) * 0.5f,
                         start.y + (h - measured.y) * 0.5f - 1.0f),
                  IM_COL32(245,245,245,255), label);
    return clicked;
}

static void DrawFadingHeaderLine(float startX, float y, float endX, ImDrawList* draw = nullptr) {
    if (!draw) draw = ImGui::GetWindowDrawList();
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

static bool ConsumePendingTouchForRect(const ImVec2& minPos, const ImVec2& maxPos) {
    if (!g_PendingTouchDown.load()) return false;
    const float x = g_PendingTouchX.load();
    const float y = g_PendingTouchY.load();
    if (x >= minPos.x && x <= maxPos.x && y >= minPos.y && y <= maxPos.y) {
        g_PendingTouchDown.store(false);
        return true;
    }
    return false;
}

// Eklenen tum menulerde ayni, arka plani tamamen kaplayan sabit karartma.
// 0 = karartma yok, 255 = tamamen siyah. Ust bant / gradyan yok.
static const int kMenuDimAlpha = 70;

static void DrawHeaderDarkening(const ImVec2& screen) {
    if (kMenuDimAlpha <= 0) return;
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0.0f, 0.0f), screen, IM_COL32(0, 0, 0, kMenuDimAlpha));
}

// ========================================================================
// TYPING BAR (shows the text being typed, just above the soft keyboard)
// ========================================================================
static void DrawTypingBar(const ImVec2& screen) {
    static bool s_barFocused = false;

    const int mode = g_KeyboardFieldMode.load();
    if (!g_AndroidKeyboardOpen.load() || (mode != 1 && mode != 2) || g_GameUIFont == nullptr) {
        s_barFocused = false;
        return;
    }

    char* buf = (mode == 1) ? g_Nickname : g_ChatInputBuffer;
    const size_t bufSize = (mode == 1) ? sizeof(g_Nickname) : sizeof(g_ChatInputBuffer);
    const char* hint = (mode == 1) ? "Enter your name..." : "Type a message...";
    const float barH = 88.0f;
    const float btnW = 190.0f;

    ImGui::SetNextWindowPos(ImVec2(0.0f, std::max(0.0f, screen.y * kTypingBarBottom - barH)), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(screen.x, barH), ImGuiCond_Always);
    ImGui::SetNextWindowFocus(); // keep the bar on top of the menu window

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.09f, 0.09f, 0.09f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.0f, 0.0f, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.0f, 0.0f, 0.12f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::Begin("##TypingBar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(g_GameUIFont);

    const float fh = ImGui::GetFrameHeight();

    // Text field (kept focused so the key events always land here).
    ImGui::SetCursorPos(ImVec2(24.0f, (barH - fh) * 0.5f));
    ImGui::SetNextItemWidth(std::max(120.0f, screen.x - btnW - 56.0f));
    if (!s_barFocused) ImGui::SetKeyboardFocusHere();
    const bool enterPressed = ImGui::InputTextWithHint("##TypingBarInput", hint, buf, bufSize,
                                                       ImGuiInputTextFlags_EnterReturnsTrue);
    s_barFocused = ImGui::IsItemActive();

    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y + 2.0f), ImVec2(mx.x, mx.y + 2.0f),
                                        IM_COL32(0, 128, 128, 255), 2.0f);

    // TAMAM button (clicked via ImGui or the raw touch, like the other buttons).
    const ImVec2 winPos = ImGui::GetWindowPos();
    const ImVec2 btnLocal(screen.x - btnW - 16.0f, (barH - fh * 1.3f) * 0.5f);
    ImGui::SetCursorPos(btnLocal);
    const bool btnClicked = ImGui::Button("TAMAM", ImVec2(btnW, fh * 1.3f));
    const bool btnTouch = ConsumePendingTouchForRect(
        ImVec2(winPos.x + btnLocal.x, winPos.y + btnLocal.y),
        ImVec2(winPos.x + btnLocal.x + btnW, winPos.y + btnLocal.y + fh * 1.3f));

    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(8);

    if (btnClicked || btnTouch) PlayGameClickSound();
    if (enterPressed || btnClicked || btnTouch) {
        if (mode == 2) SubmitChatInput();
        s_barFocused = false;
        CloseAndroidKeyboard();
    }
}

// ========================================================================
// MODE SELECT MENU  (Offline / LAN / Server)
// ========================================================================
// Kullanici adi alani. editable=false ise salt-okunur (dokunma / klavye yok).
// Oda tablosu: oda adi, oyuncu sayisi ve oyuncu satirlari (Host / Client).
static void DrawRoomTable(float width) {
    std::string names[MAX_PLAYERS];
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        names[i] = GetPlayerName((uint8_t)i);
        if (!names[i].empty()) ++count;
    }
    ImFont* font = g_GameUIFont ? g_GameUIFont : ImGui::GetFont();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float headH = 52.0f, rowH = 42.0f, pad = 14.0f, fs = 26.0f;
    const float h = headH + rowH * MAX_PLAYERS;
    DrawGamePanel(p, ImVec2(p.x + width, p.y + h), 112);
    d->PushClipRect(p, ImVec2(p.x + width, p.y + h), true);

    const std::string room = (names[0].empty() ? std::string(g_Nickname) : names[0]) + "'s Room";
    const float headY = p.y + (headH - fs) * 0.5f;
    d->AddText(font, fs, ImVec2(p.x + pad, headY), IM_COL32(130, 225, 245, 255), room.c_str());
    char cnt[24];
    snprintf(cnt, sizeof(cnt), "Players %d/%d", count, (int)MAX_PLAYERS);
    const float cw = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, cnt).x;
    d->AddText(font, fs, ImVec2(p.x + width - pad - cw, headY), IM_COL32(245, 245, 245, 255), cnt);
    d->AddLine(ImVec2(p.x, p.y + headH), ImVec2(p.x + width, p.y + headH), IM_COL32(255, 255, 255, 40), 1.0f);

    for (int i = 0; i < MAX_PLAYERS; ++i) {
        const float rowTop = p.y + headH + rowH * i;
        const float y = rowTop + (rowH - fs) * 0.5f;
        const bool empty = names[i].empty();
        char idx[8];
        snprintf(idx, sizeof(idx), "%d", i + 1);
        d->AddText(font, fs, ImVec2(p.x + pad, y), IM_COL32(150, 160, 168, 255), idx);
        d->AddText(font, fs, ImVec2(p.x + pad + 40.0f, y),
                   empty ? IM_COL32(130, 138, 145, 255) : IM_COL32(245, 245, 245, 255),
                   empty ? "Waiting for player..." : names[i].c_str());
        if (!empty) {   // rol, oyuncu baglanmadan gosterilmez
            const char* role = (i == 0) ? "Host" : "Client";
            const float rw = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, role).x;
            d->AddText(font, fs, ImVec2(p.x + width - pad - rw, y), IM_COL32(150, 160, 168, 255), role);
        }
        if (i + 1 < MAX_PLAYERS)
            d->AddLine(ImVec2(p.x + pad, rowTop + rowH), ImVec2(p.x + width - pad, rowTop + rowH),
                       IM_COL32(255, 255, 255, 24), 1.0f);
    }
    d->PopClipRect();
    ImGui::Dummy(ImVec2(width, h));
}

static void DrawNicknameField(float infoW, bool editable) {
    ImGui::Text("Username");
    const float nickInputW = std::max(180.0f, infoW * 0.56f);
    ImGui::SetNextItemWidth(nickInputW);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.015f, 0.022f, 0.028f, 0.82f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.72f, 0.82f, 0.88f, 0.34f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

    if (!editable) {
        if (g_AndroidKeyboardOpen.load() && g_KeyboardFieldMode.load() == 1) CloseAndroidKeyboard();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.66f, 0.70f, 1.0f));
        ImGui::InputText("##NicknameInput", g_Nickname, IM_ARRAYSIZE(g_Nickname),
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();
    } else {
        // İsim kutusunda ImGui tıklamasına ek olarak ham Android dokunuşu da kullanılır.
        const ImVec2 nickInputMin = ImGui::GetCursorScreenPos();
        const ImVec2 nickInputMax(nickInputMin.x + nickInputW, nickInputMin.y + ImGui::GetFrameHeight());
        const bool nickTouch = ConsumePendingTouchForRect(nickInputMin, nickInputMax);
        if (nickTouch) ImGui::SetKeyboardFocusHere();
        bool nickEnterPressed = ImGui::InputText("##NicknameInput", g_Nickname,
                                                 IM_ARRAYSIZE(g_Nickname),
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemClicked() || nickTouch) {
            OpenAndroidKeyboardForField(1, g_Nickname);
        }
        if (nickEnterPressed && g_AndroidKeyboardOpen.load()) CloseAndroidKeyboard();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

// Isim sadece LAN menusunden gelindiyse ve oda/baglanti yokken degistirilebilir.
// Oyun ici ayarlardaki Multiplayer sekmesinde asla degistirilemez.
static bool CanEditNickname() {
    return g_MpFromLan && !g_IsHost.load() && !g_IsConnected.load();
}

// Tam ekran secim menusu: baslik + ortada alt alta butonlar + sol altta Back.
// Tiklanan butonun indexini dondurur (-1: yok). Back tiklanirsa backClicked = true.
static int DrawChoiceMenu(const char* windowId, const char* title,
                          const char* const* labels, int count,
                          bool& backClicked, const ImVec2& screen) {
    backClicked = false;
    int clicked = -1;

    // Arka plan: LAN menusuyle ayni gorsel
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
    } else {
        bg->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(8, 14, 20, 255));
    }

    // Tam ekran pencere: tum dokunuslari yakalar (arkadaki oyun menusu tiklanmaz)
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(screen, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin(windowId, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushFont(g_GameUIFont);

    // Baslik (diger menulerle ayni yerlesim)
    const float titleSize = std::max(44.0f, std::min(54.0f, screen.y * 0.052f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float titleWidth = g_GameUIFont->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, title).x;
    const float titleX = std::max(24.0f, screen.x - 18.0f - titleWidth);
    const float titleY = 9.0f;
    draw->AddText(g_GameUIFont, titleSize, ImVec2(titleX, titleY),
                  IM_COL32(255, 255, 255, 255), title);
    DrawFadingHeaderLine(std::max(screen.x * 0.36f, titleX - screen.x * 0.40f),
                         titleY + titleSize + 9.0f, screen.x - 8.0f);

    // Butonlar: ortada, alt alta
    const float btnW = std::min(680.0f, std::max(440.0f, screen.x * 0.36f));
    const float btnH = btnW / 4.30f;
    const float gap  = btnH * 0.22f;
    const float totalH = btnH * count + gap * (count - 1);
    const float headerBottom = titleY + titleSize + 28.0f;
    float y = headerBottom + (screen.y - headerBottom - totalH) * 0.5f;
    if (y < headerBottom + 8.0f) y = headerBottom + 8.0f;
    const float x = (screen.x - btnW) * 0.5f;

    for (int i = 0; i < count; ++i) {
        char id[48];
        snprintf(id, sizeof(id), "##%s_btn%d", windowId, i);
        ImGui::SetCursorPos(ImVec2(x, y + (btnH + gap) * i));
        if (DrawCenteredMirroredGameButton(id, labels[i], btnW, btnH, 1.80f)) clicked = i;
    }

    // Sol alt: Back
    const float backW = std::min(420.0f, std::max(300.0f, screen.x * 0.22f));
    const float backH = backW / 3.35f;
    const float bottomMargin = std::max(18.0f, screen.y * 0.03f);
    char backId[48];
    snprintf(backId, sizeof(backId), "##%s_back", windowId);
    ImGui::SetCursorPos(ImVec2(-10.0f, screen.y - backH - bottomMargin));
    // Mod menusunden Back oyunun kendi geri akisini tetikler ve oyun kendi sesini calar:
    // cift ses olmasin diye burada mod sesi kapali. (LAN menusu oyuna donmez, sesi kalir.)
    const bool backToTitle = (strcmp(windowId, "##ModeSelectRoot") == 0);
    if (DrawGameStyleButton(backId, "Back", ImVec2(backW, backH), 1.34f, false, !backToTitle)) {
        backClicked = true;
    }

    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar();
    return clicked;
}

static void GameGoBackToTitle() {
    if (g_EngineInstance == 0) return;
    uintptr_t dev = *(uintptr_t*)(g_EngineInstance + 0x7c);
    if (dev == 0) return;
    *(volatile uint8_t*)(dev + 0xcc) = 1;   // oyun bir sonraki karede basliga doner
}

static void MpGoBack() {
    if (g_IsMultiplayerMenuActive) {
        g_IsMultiplayerMenuActive = false;
        if (g_MpFromLan) g_ShowLanChoice = true;   // Room/Browser -> LAN secimi
    } else if (g_ShowLanChoice) {
        g_ShowLanChoice = false;
        g_ShowModeSelect = true;                   // LAN secimi -> Mod secimi
    } else if (g_ShowModeSelect) {
        GameGoBackToTitle();                       // Mod secimi -> baslik ekrani
    }
    g_PendingTouchDown.store(false);
}

// Client odadan ayrilir (Leave Room butonu ve client oyundan cikinca).
static void ClientLeaveRoom() {
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

// Host odayi kapatir (Close Room butonu ve host oyundan cikinca). Client'in baglantisi kopar.
static void HostCloseRoom() {
    ClearChat();
    g_IsHost = false;          // host dongusu soketleri kendisi kapatir, oyuncularin baglantisi kopar
    g_IsConnected = false;
    g_ConnectedStatus = "Room Closed.";
}

// LAN odasi hazir (host) ya da odaya baglanildi (client): menuyu kapat, oyunun kendi
// "Select a file" ekrani gorunsun. Baglanti acik kalir.
static void MpProceedToGame() {
    if (g_AndroidKeyboardOpen.load()) CloseAndroidKeyboard();
    g_IsMultiplayerMenuActive = false;
    g_ShowLanChoice = false;
    g_ShowModeSelect = false;
    g_ReturnToMp = true;
    g_PendingTouchDown.store(false);
}

void DrawImGui() {
    LoadOrCreateProfile();
    PersistNicknameIfChanged(g_AndroidKeyboardOpen.load() && g_KeyboardFieldMode.load() == 1);
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

    g_LastImGuiFrameMs.store(NowMs());

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

    // Join Room butonu KALDIRILDI. Ayarlar menusundeki giris butonu artik "Multiplayer".
    // Oyunun kendi "Select a file" cizgisi geri geldi (artik hicbir butonla cakismiyor).

    // 1) Mod secimi: Offline / LAN / Server
    if (g_ShowModeSelect && g_CurrentMenu == MENU_SAVELOAD && !g_IsMultiplayerMenuActive) {
        static const char* kModes[] = { "Offline", "LAN", "Server" };
        bool backClicked = false;
        const int c = DrawChoiceMenu("##ModeSelectRoot", "Select Mode", kModes, 3, backClicked, screen);
        if (backClicked) {
            MpGoBack();                               // -> baslik ekrani
        } else if (c == 0) {                          // Offline: orijinal "Select a file"
            g_OnlineMode = false;
            g_ReturnToMp = false;
            g_ShowModeSelect = false;
            g_ShowLanChoice = false;
        } else if (c == 1) {                          // LAN: Create Room / Join Room secimi
            g_OnlineMode = true;
            g_ShowModeSelect = false;
            g_ShowLanChoice = true;
            g_PendingTouchDown.store(false);
        } else if (c == 2) {                          // Server: yakinda
            const long long now = NowMs();
            if (now - g_LastServerToastMs > 2500) {
                g_LastServerToastMs = now;
                ShowNativeToast("Coming soon!");
            }
        }
    }
    // 2) LAN secimi: Create Room / Join Room
    else if (g_ShowLanChoice && g_CurrentMenu == MENU_SAVELOAD && !g_IsMultiplayerMenuActive) {
        static const char* kLan[] = { "Create Room", "Join Room" };
        bool backClicked = false;
        const int c = DrawChoiceMenu("##LanChoiceRoot", "LAN", kLan, 2, backClicked, screen);
        if (backClicked) {
            MpGoBack();                               // -> mod secimi
        } else if (c == 0) {                          // Create Room -> Multiplayer Room (host + chat)
            g_ShowLanChoice = false;
            g_MpFromLan = true;
            g_MpView = MPV_ROOM;
            g_IsMultiplayerMenuActive = true;
            g_PendingTouchDown.store(false);
        } else if (c == 1) {                          // Join Room -> Server Browser (tarama + katil)
            g_ShowLanChoice = false;
            g_MpFromLan = true;
            g_MpView = MPV_BROWSER;
            g_IsMultiplayerMenuActive = true;
            g_PendingTouchDown.store(false);
        }
    }

    // 3) Oyun ici: shop butonunun altinda yuvarlak sohbet butonu (sadece LAN modunda)
    g_HudBtnR.store(0.0f);
    if (g_OnlineMode && !g_IsMultiplayerMenuActive && g_EngineInstance != 0 &&
        *(volatile int*)(g_EngineInstance + 0x64) == 6) {
        const float r  = screen.y * 0.045f;               // shop butonuyla ayni boyut
        const float cx = screen.y * 0.0745f;
        const float cy = screen.y * 0.169f;
        g_HudBtnX.store(cx); g_HudBtnY.store(cy); g_HudBtnR.store(r);

        const float dx = g_TouchX.load() - cx, dy = g_TouchY.load() - cy;
        const bool pressed = g_TouchDown.load() && dx * dx + dy * dy <= r * r * 1.44f;
        ImDrawList* d = ImGui::GetForegroundDrawList();
        const ImVec2 c(cx, cy);
        d->AddCircleFilled(c, r, pressed ? IM_COL32(60, 60, 60, 240) : IM_COL32(18, 18, 18, 235), 48);
        d->AddCircle(c, r - r * 0.04f, IM_COL32(120, 120, 120, 255), 48, r * 0.07f);
        // Sohbet balonu
        const ImU32 white = IM_COL32(245, 245, 245, 255), dark = IM_COL32(18, 18, 18, 255);
        d->AddRectFilled(ImVec2(cx - r * 0.50f, cy - r * 0.38f), ImVec2(cx + r * 0.50f, cy + r * 0.20f),
                         white, r * 0.20f);
        d->AddTriangleFilled(ImVec2(cx - r * 0.28f, cy + r * 0.15f), ImVec2(cx - r * 0.34f, cy + r * 0.46f),
                             ImVec2(cx - r * 0.02f, cy + r * 0.15f), white);
        for (int i = -1; i <= 1; ++i)
            d->AddCircleFilled(ImVec2(cx + i * r * 0.24f, cy - r * 0.09f), r * 0.07f, dark, 12);

        if (g_PendingTouchDown.load()) {
            const float px = g_PendingTouchX.load() - cx, py = g_PendingTouchY.load() - cy;
            g_PendingTouchDown.store(false);
            if (px * px + py * py <= r * r * 1.44f) {
                PlayGameClickSound();
                g_MpFromLan = false;
                g_MpView = MPV_ROOM;
                g_IsMultiplayerMenuActive = true;
            }
        }
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

        const char* title = (g_MpView == MPV_BROWSER)
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

        const float commonButtonW = std::min(500.0f, std::max(360.0f, screen.x * 0.27f));
        const float commonButtonH = commonButtonW / 3.35f;

        const float headerBottom = titleY + titleSize + 28.0f;
        const float actionY = std::max(headerBottom - 9.0f, 58.0f);
        const float backX = screen.x - commonButtonW - edgeBleed;
        const float buttonY = actionY - 10.0f;
        ImGui::SetCursorPos(ImVec2(backX, buttonY));
        if (DrawGameStyleButton("##BackButton", "Back", ImVec2(commonButtonW, commonButtonH), 1.34f, true)) {
            if (g_AndroidKeyboardOpen.load()) CloseAndroidKeyboard();
            MpGoBack();      // LAN'dan geldiyse LAN secimine, ayarlardan geldiyse oyuna doner
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
            if (g_ClearChatInputPending.exchange(false)) {
                memset(g_ChatInputBuffer, 0, sizeof(g_ChatInputBuffer));
            }

            const float inner = 14.0f;
            ImGui::SetCursorPos(ImVec2(x + inner, y + 12.0f));
            DrawGameSectionTitle("Chat", width - inner * 2.0f);

            const float inputH = std::max(64.0f, std::min(84.0f, commonButtonH * 0.34f));
            const float historyH = std::max(100.0f, height - inputH - 66.0f);
            const float historyW = width - inner * 2.0f;

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
            ImGui::SetCursorPos(ImVec2(x + inner, y + 62.0f));
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

            // Tek gerçek sohbet kutusu: yazdığımız metin doğrudan burada görünür.
            // Ayrı "Type a text"/preview bloğu yoktur.
            const float inputY = y + height - inputH - 10.0f;
            ImGui::SetCursorPos(ImVec2(x + inner, inputY));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.015f, 0.022f, 0.028f, 0.92f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.025f, 0.040f, 0.050f, 0.96f));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.035f, 0.060f, 0.070f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));

            const float chatInputW = std::max(160.0f, width - inner * 2.0f);
            ImGui::SetNextItemWidth(chatInputW);
            const ImVec2 chatInputMin = ImGui::GetCursorScreenPos();
            const ImVec2 chatInputMax(
                chatInputMin.x + chatInputW,
                chatInputMin.y + ImGui::GetFrameHeight()
            );
            const bool chatTouch = ConsumePendingTouchForRect(chatInputMin, chatInputMax);
            if (chatTouch) ImGui::SetKeyboardFocusHere();

            const bool enterPressed = ImGui::InputTextWithHint(
                "##ChatInput", "Type a message...",
                g_ChatInputBuffer,
                IM_ARRAYSIZE(g_ChatInputBuffer),
                ImGuiInputTextFlags_EnterReturnsTrue
            );

            if (ImGui::IsItemClicked() || ImGui::IsItemActivated() || chatTouch) {
                OpenAndroidKeyboardForField(2, g_ChatInputBuffer);
            }

            ImGui::PopStyleColor(4);

            // Fallback for an ENTER that still reaches ImGui instead of the native editor.
            if (enterPressed) {
                SubmitChatInput();
                CloseAndroidKeyboard();
            }
        };

        if (g_MpView == MPV_ROOM) {
            ImGui::SetCursorPos(ImVec2(edgeBleed, buttonY));
            const float innerButtonW = commonButtonW;
            const float innerButtonH = commonButtonH;
            if (g_IsClient && g_IsConnected) {
                if (DrawGameStyleButton("##LeaveRoom", "Leave Room",
                                        ImVec2(innerButtonW, innerButtonH), 1.30f, false)) {
                    ClientLeaveRoom();
                }
            } else if (!g_IsHost) {
                if (DrawGameStyleButton("##HostRoom", "Host Room",
                                        ImVec2(innerButtonW, innerButtonH), 1.30f, false)) {
                    ClearChat();
                    g_IsHost = true;
                    SetPlayerName(0, g_Nickname);
                    std::thread(TCPHostThread).detach();
                }
            } else {
                if (DrawGameStyleButton("##CloseRoom", "Close Room",
                                        ImVec2(innerButtonW, innerButtonH), 1.30f, false)) {
                    HostCloseRoom();
                }
            }

            // Host: oda acikken oyuna gec (kayit secme ekrani acilir).
            if (g_MpFromLan && g_IsHost.load()) {
                const float playW = std::min(innerButtonH * 4.30f, screen.x - innerButtonW * 2.0f - 24.0f);
                ImGui::SetCursorPos(ImVec2((screen.x - playW) * 0.5f, buttonY));
                if (DrawCenteredMirroredGameButton("##PlayGame", "Play", playW, innerButtonH, 1.80f)) {
                    MpProceedToGame();
                }
            }

            const float infoX = 18.0f;
            const float infoW = std::max(260.0f, controlW - 36.0f);
            ImGui::SetCursorPos(ImVec2(infoX, bodyTop + 12.0f));
            DrawGameSectionTitle("Room Info", infoW);
            DrawGameStatusText("Status:", g_ConnectedStatus, infoW);
            // Oda kurulunca / baglaninca isim kutusu tamamen gizlenir.
            if (!g_IsHost.load() && !g_IsConnected.load()) DrawNicknameField(infoW, CanEditNickname());
            ImGui::Spacing();
            DrawRoomTable(infoW);

            RenderChatUI(chatX, bodyTop, chatW, bodyBottom - bodyTop);
        } else if (g_MpView == MPV_BROWSER) {
            // Üst ana buton bağlantı durumuna göre değişir:
            // - bağlı değilken Scan Networks
            // - bağlıyken Disconnect
            ImGui::SetCursorPos(ImVec2(edgeBleed, buttonY));

            if (!g_IsConnected.load()) {
                if (DrawGameStyleButton("##ScanNetworks",
                                        g_IsSearching.load() ? "Searching..." : "Scan Networks",
                                        ImVec2(commonButtonW, commonButtonH), 1.30f, false)) {
                    if (!g_IsSearching.load()) {
                        g_IsSearching.store(true);
                        std::thread(StartLANDiscoveryThread).detach();
                    }
                }
            } else {
                if (DrawGameStyleButton("##DisconnectTop", "Disconnect",
                                        ImVec2(commonButtonW, commonButtonH), 1.30f, false)) {
                    ClearChat();
                    CloseAndroidKeyboard();
                    ResetPlayerNames();
                    g_IsHost.store(false);
                    g_IsClient.store(false);
                    g_IsConnected.store(false);
                    if (g_TcpSocket >= 0) {
                        shutdown(g_TcpSocket, SHUT_RDWR);
                        close(g_TcpSocket);
                        g_TcpSocket = -1;
                    }
                    g_ConnectedStatus = "Disconnected.";
                }
            }

            const float infoX = 18.0f;
            const float infoW = std::max(260.0f, controlW - 36.0f);
            ImGui::SetCursorPos(ImVec2(infoX, bodyTop + 12.0f));
            DrawGameSectionTitle("Player", infoW);
            DrawGameStatusText("Network:", g_ConnectedStatus, infoW);
            if (!g_IsConnected.load()) DrawNicknameField(infoW, CanEditNickname());


            ImGui::SetCursorPos(ImVec2(infoX, bodyTop + 200.0f));
            DrawGameSectionTitle(g_IsConnected.load() ? "Connected Room" : "Discovered Rooms", infoW);
            const float listH = std::max(100.0f, bodyBottom - ImGui::GetCursorScreenPos().y - 12.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
            ImGui::BeginChild("##RoomsList", ImVec2(infoW, listH), false,
                              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
            if (!g_IsConnected) {
                std::lock_guard<std::mutex> lock(g_PeerMutex);
                if (g_DiscoveredPeers.empty()) {
                    ImGui::TextDisabled("No active rooms found yet.");
                } else {
                    const float roomW = std::min(commonButtonW * 0.82f, infoW);
                    const float roomH = std::max(72.0f, commonButtonH * 0.72f);

                    for (size_t i = 0; i < g_DiscoveredPeers.size(); ++i) {
                        std::string roomLabel = g_DiscoveredPeers[i].name;
                        if (roomLabel.empty()) roomLabel = "Room";

                        char roomId[32];
                        snprintf(roomId, sizeof(roomId), "##Room%u",
                                 static_cast<unsigned int>(i));

                        // DrawGameStyleButton imleci eski konumunda bıraktığından,
                        // child sınırını büyütmek için SetCursorScreenPos kullanmıyoruz.
                        // Dummy() her oda satırını temiz şekilde aşağı taşır ve ImGui
                        // parent sınırlarını da doğru hesaplar.
                        const ImVec2 roomButtonMin = ImGui::GetCursorScreenPos();
                        const ImVec2 roomButtonMax(
                            roomButtonMin.x + roomW,
                            roomButtonMin.y + roomH
                        );

                        const bool roomTouch =
                            ConsumePendingTouchForRect(roomButtonMin, roomButtonMax);

                        const bool roomClicked = DrawGameStyleButton(
                            roomId,
                            roomLabel.c_str(),
                            ImVec2(roomW, roomH),
                            1.06f,
                            false
                        );

                        if (roomTouch && !roomClicked) PlayGameClickSound();
                        if (roomClicked || roomTouch) {
                            if (!g_IsHost.load() && !g_IsClient.load()) {
                                ClearChat();
                                g_IsClient.store(true);
                                const std::string targetIP = g_DiscoveredPeers[i].ip;
                                std::thread(TCPClientThread, targetIP).detach();
                            }
                        }

                        // IP adresi tamamen gizlendi.
                        ImGui::Dummy(ImVec2(roomW, roomH + 18.0f));
                    }
                }
            } else {
                ImGui::Spacing();

                // Bağlıyken sadece Join Game içeride kalır.
                // Disconnect artık yukarıdaki ana butondadır.
                // Host oyuna girmeden Join Game calismaz.
                const bool hostReady = g_HostInGame.load();
                const int dl = g_SaveProgress.load();
                const bool downloading = (dl >= 0 && dl < 100);
                char joinLabel[40];
                if (!hostReady) snprintf(joinLabel, sizeof(joinLabel), "Waiting for host...");
                else if (downloading) snprintf(joinLabel, sizeof(joinLabel), "Downloading... %d%%", dl);
                else snprintf(joinLabel, sizeof(joinLabel), "Join Game");
                const bool joinDisabled = !hostReady || downloading;
                if (joinDisabled) ImGui::BeginDisabled();
                if (DrawGameStyleButton(
                        "##JoinGame",
                        joinLabel,
                        ImVec2(std::min(commonButtonW, infoW), commonButtonH),
                        1.20f,
                        false) && !joinDisabled) {
                    if (g_SaveHooksOk) {
                        // Host'un o anki kaydi istenir; inince oyun otomatik acilir.
                        g_ClientSaveReady.store(false);
                        g_SaveProgress.store(0);
                        g_SaveRequestMs.store(NowMs());
                        g_SaveRequestOut.store(true);
                    } else {
                        AutoStartGameForClient();
                    }
                }
                if (joinDisabled) ImGui::EndDisabled();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();

            RenderChatUI(chatX, bodyTop, chatW, bodyBottom - bodyTop);
        }

        ImGui::PopFont();
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // Typing bar above the keyboard (shows the text being typed).
    DrawTypingBar(screen);

    // If this tap was not on either input field, discard it after this frame.
    g_PendingTouchDown.store(false);

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
// ========================================================================
void my_GameUpdate(void* thiz, float param_1) {
    g_LastGameUpdateMs.store(NowMs());   // host canlilik sinyali icin (oyun dongusu donarsa ping kesilir)
    g_EngineInstance = (uintptr_t)thiz; 
    g_CurrentMenu = MENU_INGAME; 

    // Oturum guvenligi: host oyundan cikarsa oda kapanir, client baglantisi kopunca oyundan atilir.
    {
        static bool s_prevInGame = false;
        const int st = *(volatile int*)((uintptr_t)thiz + 0x64);
        const bool inGame = (st == 6 || st == 7);
        g_LocalInGame.store(inGame);
        if (s_prevInGame && !inGame && g_IsHost.load()) HostCloseRoom();
        if (s_prevInGame && !inGame && g_IsClient.load() && g_IsConnected.load()) ClientLeaveRoom();
        HostCaptureSaveIfRequested();

        // Client: kayit indiyse oyunu baslat; baglanti kopar / host cevap vermezse indirmeyi iptal et.
        if (g_ClientSaveReady.exchange(false)) {
            g_UseHostSave.store(true);
            AutoStartGameForClient();
        }
        const int dl = g_SaveProgress.load();
        if (dl >= 0 && dl < 100 &&
            (!g_IsConnected.load() || NowMs() - g_SaveRequestMs.load() > 15000)) {
            g_SaveProgress.store(-1);
            ShowNativeToast("Could not download the host's game.");
        }
        if (dl == 100 && !g_ClientPlaying.load()) g_SaveProgress.store(-1);

        // Gecici "kayit var" bayragini geri al (kalici hale gelmesin).
        if (g_RestoreSlot >= 0 && (st == 6 || g_AutoStartUntilMs.load() == 0)) {
            *(volatile uint8_t*)((uintptr_t)thiz + 0xa374 + g_RestoreSlot * 0x18 + 8) = 0;
            g_RestoreSlot = -1;
        }
        if (g_UseHostSave.load() && (st == 6 || g_AutoStartUntilMs.load() == 0)) g_UseHostSave.store(false);
        if (g_ClientPlaying.load()) {
            if (st == 1) {
                g_ClientPlaying.store(false);                       // basliga donuldu
            } else if (inGame && !g_IsConnected.load() && g_KickUntilMs.load() == 0) {
                ShowNativeToast("The host left the game. Returning to the menu.");
                g_KickUntilMs.store(NowMs() + 15000);
            }
        }
        s_prevInGame = inGame;
    }
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
    // and the network is independently capped at 30 snapshots/sec.
    if (*(volatile uint32_t*)(g_EngineInstance + 0x64) == 6) {
        ApplyRemoteVehicleStates(g_EngineInstance);
        CaptureAndQueueLocalVehicleState(g_EngineInstance);
    } else {
        // Oyun ici menu (magaza): konum esitleme yok, ama arac tablosu acik kalir ve
        // magazadan alinan/satilan araclar hemen diger cihazlara bildirilir.
        RefreshVehicleTopology(g_EngineInstance);
    }

    // Oyun ici ImGui cizimi: ekran renderQueues'ta (presentGLESFramebuffer) ekrana verilir,
    // yani cizim ondan ONCE yapilmali. Bu fonksiyon sadece oyun durumu 6'da cagrilir.
    if (g_OnlineMode || g_IsMultiplayerMenuActive) {
        g_CurrentMenu = MENU_INGAME;
        DrawImGui();
    }
}

// Kayit secme ekraninda oyunun kendi "slot tiklandi" kodunu (Game+0x9c20) tetikler:
// 10+slot = kaydi yukle, 0x10 = (bos slotta) yeni oyun / kolay zorluk. Oyun sonra kendi
// yukleme akisini calistirip durum 6'ya (oyun) gecer.
static void* g_SwitchToStateStart = nullptr;   // Game::switchToStateStart() (dlsym)

// Client oyundan basliga atilir: oyunun kendi "ayarlar -> Exit (kaydetmeden)" akisi calistirilir.
static void KickToTitleInject() {
    const long long until = g_KickUntilMs.load();
    if (until == 0 || g_EngineInstance == 0) return;
    const uintptr_t g = g_EngineInstance;
    const int state = *(volatile int*)(g + 0x64);
    if (state == 1 || NowMs() > until) { g_KickUntilMs.store(0); return; }
    volatile int* clicked = (volatile int*)(g + 0x9c20);
    if (state == 6) {
        if (g_SwitchToStateStart) ((void (*)(void*))g_SwitchToStateStart)((void*)g);
        else g_KickUntilMs.store(0);
    } else if (state == 7) {
        const int sub = *(volatile int*)(g + 0x68);
        if (sub == 0) *clicked = 0x15;          // Exit
        else if (sub == 7) *clicked = 6;        // "kaydetmeden cik?" -> Evet
    }
}

// "Select a file" ekraninda Back (dokunma ya da donanim tusu) basliga degil, bir onceki
// mod penceresine (Mode select ya da Multiplayer menusu) goturur.
static void SaveMenuBackIntercept() {
    if (g_EngineInstance == 0) return;
    const uintptr_t g = g_EngineInstance;
    if (*(volatile int*)(g + 0x64) != 3 || *(volatile int*)(g + 0x68) != 0) return;
    if (g_ShowModeSelect || g_ShowLanChoice || g_IsMultiplayerMenuActive) return;
    if (g_AutoStartUntilMs.load() != 0 || g_KickUntilMs.load() != 0) return;
    volatile int* clicked = (volatile int*)(g + 0x9c20);
    const uintptr_t dev = *(volatile uintptr_t*)(g + 0x7c);
    volatile uint8_t* backFlag = dev ? (volatile uint8_t*)(dev + 0xcc) : nullptr;
    if (!(*clicked == 9 || (backFlag && *backFlag))) return;
    *clicked = 0;
    if (backFlag) *backFlag = 0;
    if (g_ReturnToMp && g_MpFromLan) g_IsMultiplayerMenuActive = true;
    else g_ShowModeSelect = true;
    g_PendingTouchDown.store(false);
}

static void AutoStartInject() {
    KickToTitleInject();
    SaveMenuBackIntercept();
    const long long until = g_AutoStartUntilMs.load();
    if (until == 0 || g_EngineInstance == 0) return;
    const uintptr_t g = g_EngineInstance;
    const int state = *(volatile int*)(g + 0x64);
    if (state == 6 || NowMs() > until) { g_AutoStartUntilMs.store(0); return; }
    if (*(volatile int*)(g + 0x68) != 0) return;          // yukleme / diyalog suruyor
    volatile int* clicked = (volatile int*)(g + 0x9c20);
    if (state == 3 && g_UseHostSave.load()) {              // host kaydi: slot dosyasi okunmaz
        int slot = *(volatile int*)(g + 0xa3f0);
        if ((unsigned)slot > 2) slot = 0;
        volatile uint8_t* exists = (volatile uint8_t*)(g + 0xa374 + slot * 0x18 + 8);
        if (!*exists) { *exists = 1; g_RestoreSlot = slot; }
        *clicked = 10 + slot;
    } else if (state == 3) {                               // kayit secme
        int slot = *(volatile int*)(g + 0xa3f0);           // son kullanilan slot
        if ((unsigned)slot > 2 || !*(volatile uint8_t*)(g + 0x8a0c + slot)) {
            slot = 0;
            for (int i = 0; i < 3; ++i) if (*(volatile uint8_t*)(g + 0x8a0c + i)) { slot = i; break; }
        }
        *clicked = 10 + slot;
    } else if (state == 5) {                               // zorluk secimi (bos slot)
        *clicked = 0x10;
    }
}

// Game::buyItem bazen urun numarasi olarak bir float bit deseni (0x42800000 = 64.0f) aliyor ve
// this+(numara+0x23c8)*4 adresine erisirken cokuyor (adb: buyItem+18, fault addr 0xf95c8f34).
// Numara GUI yoneticisinin +0x583c alanindan ve Game+0x9c24'ten geliyor. Gecerli aralik 0..0x11.
// Burada bozuk deger gorulurse duzeltilir / tiklama iptal edilir ve LOG'a yazilir (nedeni ararken).
static void GuardShopSelection(void* gui) {
    if (g_EngineInstance == 0 || gui == nullptr) return;
    const uintptr_t g = g_EngineInstance;
    if (*(volatile uint32_t*)(g + 0x64) != 7) return;        // sadece oyun ici menu
    volatile uint32_t* sel = (volatile uint32_t*)((uintptr_t)gui + 0x583c);
    if (*sel > 0x11) {
        LOGI("[SHOP] GUI secili urun bozuk: 0x%08x -> 0", (unsigned)*sel);
        *sel = 0;
    }
    const uint32_t ev = *(volatile uint32_t*)(g + 0x9c20);
    if (ev >= 0x19 && ev <= 0x1d) {
        const uint32_t idx = *(volatile uint32_t*)(g + 0x9c24);
        if (idx > 0x11) {
            LOGI("[SHOP] bozuk urun numarasi 0x%08x (olay=0x%x): tiklama iptal edildi", (unsigned)idx, (unsigned)ev);
            *(volatile uint32_t*)(g + 0x9c20) = 0;
        }
    }
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

    void* ret = orig_updateGUI ? orig_updateGUI(thiz, p1, p2, p3, p4) : nullptr;
    GuardShopSelection(thiz);
    AutoStartInject();
    return ret;
}

// The game draws every title underline from one global overlay description
// (GenericGUIManager::m_overlayTitleUnderlineRight, float[6], [1] = height scale).
// Setting the height to 0 while the "Select a file." menu renders hides the game's own line.
// If the symbol cannot be found this stays null and the menu simply keeps the original line.
static float* GetNativeTitleUnderline() {
    static float* desc = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        void* lib = dlopen("libapp.so", RTLD_NOW | RTLD_NOLOAD);
        if (lib) {
            desc = (float*)dlsym(lib, "_ZN17GenericGUIManager28m_overlayTitleUnderlineRightE");
            dlclose(lib);
        }
    }
    return desc;
}

void* my_renderMenu(void* thiz, void* p1, void* p2, void* p3) {
    g_MenuInstance = (uintptr_t)thiz;

    // Baslik ekranindan "Select a file" ekranina YENI girildi mi?
    // Bu fonksiyon menu acikken her karede cagrilir; aradaki bosluk 700 ms'den
    // buyukse menu daha once gorunmuyordu demektir -> mod secim menusunu ac.
    const long long now = NowMs();
    if (now - g_LastSaveMenuRenderMs > 700 && !g_IsMultiplayerMenuActive && !g_ShowLanChoice) {
        g_ShowModeSelect = true;
        g_ReturnToMp = false;
    }
    g_LastSaveMenuRenderMs = now;

    // Oyunun kendi cizgisi sadece bizim tam ekran menulerimizin altinda gizlenir.
    float* underline = GetNativeTitleUnderline();
    const bool hideUnderline = g_ShowModeSelect || g_ShowLanChoice || g_IsMultiplayerMenuActive;
    const float savedHeight = underline ? underline[1] : 0.0f;
    if (underline && hideUnderline) underline[1] = 0.0f;

    void* ret = nullptr;
    if (orig_renderMenu) ret = orig_renderMenu(thiz, p1, p2, p3);

    if (underline && hideUnderline) underline[1] = savedHeight;
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

    void* appLib = dlopen("libapp.so", RTLD_NOW | RTLD_NOLOAD);
    if (appLib) {
        void* saveTask = dlsym(appLib, "_ZN9SaveGames9startTaskENS_17WorkerTaskCommandEjb");
        g_SwitchToStateStart = dlsym(appLib, "_ZN4Game18switchToStateStartEv");
        LOGI("startTask symbol: %p, switchToStateStart symbol: %p", saveTask, g_SwitchToStateStart);
        void* saveFile = dlsym(appLib, "_ZN27AndroidHandheldSystemDevice8saveFileEPKcPhj");
        void* loadSave = dlsym(appLib, "_ZN9SaveGames12loadSavegameEPKcjj");
        LOGI("saveFile symbol: %p, loadSavegame symbol: %p", saveFile, loadSave);
        void* addVeh = dlsym(appLib, "_ZN4Game10addVehicleEN13EntityManager8VEHICLESERK7Vector3fj");
        if (!addVeh) addVeh = FindElfSymbolByPrefix("libapp.so", "_ZN4Game10addVehicleE");
        void* remVeh = dlsym(appLib, "_ZN4Game13removeVehicleEj");
        g_VehicleDestroyFn = (Vehicle_destroy_t)dlsym(appLib, "_ZN7Vehicle7destroyEv");
        if (!g_VehicleDestroyFn) g_VehicleDestroyFn = (Vehicle_destroy_t)FindElfSymbolByPrefix("libapp.so", "_ZN7Vehicle7destroyEv");
        g_VehicleDetachToolFn = (Vehicle_detach_t)dlsym(appLib, "_ZN7Vehicle10detachToolEP7b2World");
        if (!g_VehicleDetachToolFn) g_VehicleDetachToolFn = (Vehicle_detach_t)FindElfSymbolByPrefix("libapp.so", "_ZN7Vehicle10detachTool");
        g_VehicleDetachTrailerFn = (Vehicle_detach_t)dlsym(appLib, "_ZN7Vehicle13detachTrailerEP7b2World");
        if (!g_VehicleDetachTrailerFn) g_VehicleDetachTrailerFn = (Vehicle_detach_t)FindElfSymbolByPrefix("libapp.so", "_ZN7Vehicle13detachTrailer");
        LOGI("vehicle hooks: addVehicle=%p removeVehicle=%p Vehicle::destroy=%p detachTool=%p detachTrailer=%p",
             addVeh, remVeh, (void*)g_VehicleDestroyFn, (void*)g_VehicleDetachToolFn, (void*)g_VehicleDetachTrailerFn);
        // skipIfRisky=false: bu ikisi zaten calisiyor; sadece prologue riskini loga yaz.
        orig_Game_addVehicle = (Game_addVehicle_t)addVeh;     // kanca YOK: sadece cagrilacak ham fonksiyon
        HookChecked("Game::removeVehicle", remVeh, (void*)my_Game_removeVehicle, (void**)&orig_Game_removeVehicle, false);
        g_VehicleHooksOk = addVeh && remVeh && g_VehicleDestroyFn;
        if (saveTask) {
            g_SaveStartTaskFn = saveTask;
            MSHookFunction(saveTask, (void*)my_SaveStartTask, (void**)&orig_SaveStartTask);
        }
        if (saveTask && saveFile && loadSave) {
            MSHookFunction(saveFile, (void*)my_SaveFile, (void**)&orig_SaveFile);
            MSHookFunction(loadSave, (void*)my_LoadSavegame, (void**)&orig_LoadSavegame);
            g_SaveHooksOk = true;
        }
        dlclose(appLib);
    }

    void* inputQueueGetEventAddr = dlsym(RTLD_DEFAULT, "AInputQueue_getEvent");
    if (inputQueueGetEventAddr != nullptr) {
        MSHookFunction(inputQueueGetEventAddr, (void*)my_AInputQueue_getEvent, (void**)&orig_AInputQueue_getEvent);
    }
}
