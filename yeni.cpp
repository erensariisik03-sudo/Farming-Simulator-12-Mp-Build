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
#include <sys/mman.h>
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
static void RepairShopClick(void* gui, const char* where);   // tanim my_updateGUI oncesinde
static void GuestBlockDialogs(uintptr_t g);                   // client alisveris engeli (tanim RepairShopClick oncesinde)

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
            size_t plen = strlen(prefix);
            // Sonu '$' ile biten "onek" TAM ESLESME demektir (mangled adlarda '$' olmaz): ayni adli asiri yuklemeleri ayirmak icin
            // (ornek: ...enqueueDrawColumnMajor44EPKfjj ile ...EPKfjjj).
            const bool exactName = plen > 0 && prefix[plen - 1] == '$';
            if (exactName) --plen;
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
                    if (strncmp(&strtab[syms[k].st_name], prefix, plen) == 0 && (!exactName || strtab[syms[k].st_name + plen] == '\0')) {
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
static const uint8_t PACKET_MONEY_DELTA = 20;     // para degisimi (+/-): client->host, host->herkes
static const uint8_t PACKET_ACTIVE_VEHICLE = 22;  // "su an bu aracin icindeyim" (oyuncu basina, host iletir)
static const uint8_t PACKET_MONEY_SET = 21;
static const uint8_t PACKET_TIME_SYNC = 27;      // host -> client: oyun saati (Environment: Game+0xA098)       // host -> client: mutlak para (oyuncu odaya katilinca)
static const uint8_t PACKET_UDP_ACK = 16;      // host -> client (UDP): UDP yolu acildi
static const uint8_t PACKET_TRAILER_ATTACH = 23;  // romork takildi/cikarildi (olay, guvenilir)
static const uint8_t PACKET_FIELD_OP = 25;       // tarla islemi (hasat/pulluk/mibzer...): diger cihazlarda ayni tarla degissin
static const uint8_t PACKET_POOL_OP = 26;        // romork/alet satin alma (ekle) / satma (sil)
static const uint8_t PACKET_ENTITY_STATE = 24;    // romork doluluk / bicer tank, calisma, kesici durumu
static const uint8_t PACKET_BALE_OP = 28;         // balya eklendi / silindi (toplandi, satildi) / tasindi (guvenilir, host iletir)
static const uint8_t PACKET_BALE_LIST = 29;       // host -> client: tum balyalar (yakinsama icin, degisken uzunluk)
static const uint8_t PACKET_MAP_CLOCK = 30;       // host -> client: tarla buyume saati (Map zamanlayicisi)
static const uint8_t PACKET_FIELD_OWN = 31;       // tarla satin alindi (sahiplik bayti 0 -> 1), host iletir
static const uint8_t PACKET_DEMAND = 32;          // host -> client: fiyat listesi (buyuk talep, GreatDemandManager)
static const uint8_t PACKET_MISSION = 33;         // host -> client: gorev durumu (MissionManager)
static const uint8_t PACKET_MISSION_DONE = 34;    // client -> host: client gorevi tamamladi
static const uint8_t PACKET_RESUME = 36;          // client -> host: oyun arka plandan / ayarlardan dondu: son tarla durumu + arac konumlarini gonder
static const uint8_t PACKET_HOST_RESUME = 37;     // host -> client'lar: host oyuna dondu: son tarla durumu + arac konumlarini gonderin
static const uint8_t PACKET_REST_POSE = 38;       // duran (sahipsiz) araclarin son konumlari (degisken uzunluk, iletilmez)
static const uint8_t PACKET_FIELD_SNAP = 35;      // tek bir tarlanin tam durumu (ekili/bicilmis/surulmus/gubreli...), host iletir
static const uint8_t PACKET_TRAFFIC = 39;         // NPC trafik araclari (yolcu/kamyon): her cihaz kendi civarindakileri yayinlar, host iletir
struct __attribute__((packed)) TrafficHeader { uint8_t type; uint8_t ownerId; uint8_t count; };
struct __attribute__((packed)) TrafficEntry { uint16_t id; uint8_t type; uint8_t dir; uint16_t road; uint8_t color; uint8_t pad; float t, x, y, a, vx, vy; };
static const uint32_t kTrafficMax = 10;           // oyunun trafik havuzu en fazla 10 arac

// Baglanti sagligi: bu sureden uzun sessizlik = karsi taraf gitti (donmus / zorla kapatilmis).
static const long long HOST_SILENCE_TIMEOUT_MS = 10000;  // client: host'tan hicbir sey gelmedi
static const long long PEER_SILENCE_TIMEOUT_MS = 10000;  // host: client'tan hicbir sey gelmedi
static const long long HOST_ALIVE_WINDOW_MS = 3000;      // host oyun dongusu bu surede calistiysa "canli"
static const long long PING_INTERVAL_MS = 500;
static const long long AUTH_TABLE_INTERVAL_MS = 1000;
static const long long UDP_STALE_MS = 3000;              // UDP yolu bu kadar sessizse TCP'ye don

static std::atomic<long long> g_LastGameUpdateMs(0);     // oyun ana dongusunun son calistigi an
// Host duraklamasi: Game::updateStateBase (dunya mantigi) bu surede hic calismadiysa host oyunu
// durmus demektir (magaza/ayarlar, arka plan, bildirim paneli). Araci donup kalir; sahiplik birakilir.
static std::atomic<long long> g_LastStateBaseMs(0);      // updateStateBase'in son calistigi an
static const long long HOST_PAUSE_DETECT_MS = 1200;      // bu kadar suredir calismiyorsa host "duraklamis"
static const long long HOST_PAUSE_GRACE_MS = 180000;     // duraklama suresince ping/tablo bu kadar surdurulur (client atilmasin)
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
struct MoneyPacket {
    uint8_t type;
    uint8_t originId;
    double value;
};
struct TimeSyncPacket {
    uint8_t type;
    uint8_t originId;
    double time;        // Environment+0  (oyun zamani)
    double realTime;    // Environment+8  (HUD saati)
};
struct ActiveVehiclePacket {
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
    uint8_t steerQ;       // direksiyon (0 = veri yok)
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

// ========================================================================
// OYUNCU BASINA "SU AN HANGI ARACIN ICINDE" BILGISI
// Sahiplik (authority) arac DURUNCA birakiliyor; mahsul satisi ise genelde arac dururken olur.
// Bu yuzden "bu arac baska birinin mi?" sorusu, o oyuncunun aktif aracina bakilarak cevaplanir.
// ========================================================================
struct RemoteActive { int netId; long long ms; };
static RemoteActive g_RemoteActive[MAX_PLAYERS];
static std::mutex g_ActiveMutex;
static int g_LocalActiveNetId = -1;
static int g_LastSentActiveNetId = -2;
static long long g_LastActiveSendMs = 0;

static void StoreRemoteActive(uint8_t origin, uint16_t netId) {
    if (origin >= MAX_PLAYERS) return;
    std::lock_guard<std::mutex> lock(g_ActiveMutex);
    g_RemoteActive[origin].netId = (netId == 0xFFFF) ? -1 : (int)netId;
    g_RemoteActive[origin].ms = NowMs();
}

// Baska bir oyuncu (yakin zamanda) bu araci aktif araci olarak bildirdi mi?
static bool IsRemoteActiveVehicle(int netId) {
    if (netId < 0) return false;
    const uint8_t me = g_LocalPlayerId.load();
    const long long now = NowMs();
    std::lock_guard<std::mutex> lock(g_ActiveMutex);
    for (uint8_t p = 0; p < MAX_PLAYERS; ++p) {
        if (p == me) continue;
        if (g_RemoteActive[p].netId == netId && (now - g_RemoteActive[p].ms) < 6000) return true;
    }
    return false;
}

// Ayni aracin icinde, benden DUSUK numarali baska bir oyuncu da var mi? (ikisi de ayni aractaysa
// sadece en kucuk numarali oyuncunun cihazi para degisimini sayar.)
static bool HasLowerIdRemoteActive(int netId, uint8_t me) {
    if (netId < 0) return false;
    const long long now = NowMs();
    std::lock_guard<std::mutex> lock(g_ActiveMutex);
    for (uint8_t p = 0; p < me && p < MAX_PLAYERS; ++p)
        if (g_RemoteActive[p].netId == netId && (now - g_RemoteActive[p].ms) < 6000) return true;
    return false;
}

// Oyun thread'i, durum 6'da her karede.
static void ActiveVehicleFrame(uintptr_t game) {
    if (!g_IsConnected.load() || !g_VehNetArmed.load()) { g_LocalActiveNetId = -1; return; }
    const uint32_t n = VehicleCount(game);
    const uint32_t idx = *(volatile uint32_t*)(game + 0xA8);       // aktif arac indeksi
    int id = -1;
    if (idx < n) id = NetIdOfVehicle(RawVehicleSlot(game, idx));
    g_LocalActiveNetId = id;
    const long long now = NowMs();
    if (id != g_LastSentActiveNetId || (now - g_LastActiveSendMs) > 2000) {
        g_LastSentActiveNetId = id;
        g_LastActiveSendMs = now;
        ActiveVehiclePacket p;
        memset(&p, 0, sizeof(p));
        p.type = PACKET_ACTIVE_VEHICLE;
        p.originId = g_LocalPlayerId.load();
        p.netId = (id < 0) ? (uint16_t)0xFFFF : (uint16_t)id;
        QueueVehicleEvent(&p, sizeof(p));
    }
}

// ========================================================================
// ORTAK PARA (Game+0x6a0 = double)
// Oyun parayi alim/satim/yakit/maas/hasat satisi gibi pek cok yerde degistirir. Bu yuzden
// degisimler tek tek kancalanmaz: her karede "simdiki - son bilinen" farki hesaplanir (delta),
// birikip 300 ms'de bir gonderilir. Gelen deltalar yerel paraya eklenir. Host araya girip iletir.
// Sadece delta kullanildigi icin es zamanli degisiklikler kaybolmaz. Katilan oyuncuya host
// mutlak degeri (MONEY_SET) bir kez gonderir.
// kSharedMoney=false yaparsan her oyuncu kendi parasini kullanir.
// ========================================================================
static const bool kSharedMoney = true;
static std::mutex g_MoneyMutex;
static std::vector<double> g_MoneyIncoming;
static bool g_MoneySetPending = false;
static double g_MoneySetValue = 0.0;
static std::atomic<bool> g_MoneySendSet(false);
static bool g_MoneyInit = false;
static double g_MoneyLast = 0.0, g_MoneyAccum = 0.0;
static long long g_MoneyLastSendMs = 0;

static double ReadMoney(uintptr_t g) { double d; memcpy(&d, (const void*)(g + 0x6a0), sizeof(d)); return d; }
static void WriteMoney(uintptr_t g, double d) { memcpy((void*)(g + 0x6a0), &d, sizeof(d)); }
static bool MoneyOk(double d) { return d == d && d > -1e12 && d < 1e12; }

// Oyun thread'i, HER karede (tum oyun durumlarinda) cagrilir.
static void MoneyFrame(uintptr_t g) {
    if (!kSharedMoney || g == 0) return;
    const uint32_t st = *(volatile uint32_t*)(g + 0x64);
    const bool connected = g_IsConnected.load();
    if (!connected || (st != 6 && st != 7)) {
        // Oyun disi: son bilinen degeri birakma (menude/yuklemede para sicrar, delta sayilmasin).
        g_MoneyInit = false;
        g_MoneyAccum = 0.0;
        if (!connected) {
            std::lock_guard<std::mutex> lock(g_MoneyMutex);
            g_MoneyIncoming.clear();
            g_MoneySetPending = false;
        }
        return;
    }

    double cur = ReadMoney(g);
    if (!g_MoneyInit) { g_MoneyInit = true; g_MoneyLast = cur; g_MoneyAccum = 0.0; }

    // 1) yerel degisim (oyunun kendi alim/satim/gideri)
    g_MoneyAccum += cur - g_MoneyLast;
    g_MoneyLast = cur;

    // 2) diger oyunculardan gelenler
    double inc = 0.0, setVal = 0.0;
    bool haveSet = false;
    {
        std::lock_guard<std::mutex> lock(g_MoneyMutex);
        for (size_t i = 0; i < g_MoneyIncoming.size(); ++i) inc += g_MoneyIncoming[i];
        g_MoneyIncoming.clear();
        if (g_MoneySetPending) { haveSet = true; setVal = g_MoneySetValue; g_MoneySetPending = false; }
    }
    if (haveSet && MoneyOk(setVal)) {
        WriteMoney(g, setVal);
        cur = setVal; g_MoneyLast = setVal; g_MoneyAccum = 0.0;
        LOGI("[MONEY] host parasi alindi: %.2f", setVal);
    } else if (inc != 0.0 && MoneyOk(cur + inc)) {
        cur += inc;
        WriteMoney(g, cur);
        g_MoneyLast = cur;                      // bu degisim yerel sayilmasin
        if (fabs(inc) >= 100.0) LOGI("[MONEY] gelen degisim uygulandi: %+.2f", inc);
    }

    // 3) host: yeni katilana mutlak degeri gonder
    if (g_IsHost.load() && g_MoneySendSet.exchange(false)) {
        MoneyPacket p; memset(&p, 0, sizeof(p));
        p.type = PACKET_MONEY_SET; p.originId = g_LocalPlayerId.load(); p.value = cur;
        QueueVehicleEvent(&p, sizeof(p));
    }

    // 4) yerel degisimleri gonder
    const long long now = NowMs();
    if (fabs(g_MoneyAccum) > 1e-6 && (now - g_MoneyLastSendMs) >= 300) {
        MoneyPacket p; memset(&p, 0, sizeof(p));
        p.type = PACKET_MONEY_DELTA; p.originId = g_LocalPlayerId.load(); p.value = g_MoneyAccum;
        QueueVehicleEvent(&p, sizeof(p));
        if (fabs(g_MoneyAccum) >= 100.0) LOGI("[MONEY] yerel degisim gonderildi: %+.2f", g_MoneyAccum);
        g_MoneyAccum = 0.0;
        g_MoneyLastSendMs = now;
    }
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
// ---- WALKER (aractan inme) ileri bildirimleri: modul (walker_module.inc) LocalActiveVehiclePtr'dan sonra include edilir ----
static const uint16_t WK_NET_TYPE = 0x7001;   // MP'de yaya icin ayrilmis sahte arac tipi (VehicleSpawnPacket::vehicleType)
namespace WK {
static bool IsWalker(uintptr_t p);
static uint32_t AddVehicleForNet(void* game, int type, const float* pos, uint32_t angleBits);
static void OnVehicleRemoved(uintptr_t p);
}
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
    const uint32_t type = WK::IsWalker(p) ? (uint32_t)WK_NET_TYPE : *(uint32_t*)(p + 0x10);   // WALKER: yaya sahte tiple duyurulur (EntityManager::VEHICLES yerine)
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
    if (idx < VehicleCount(g)) WK::OnVehicleRemoved(RawVehicleSlot(g, idx));   // WALKER: yaya kaydini temizle
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
            WK::OnVehicleRemoved(p);                                                  // WALKER: havuz nesnesi yeniden kullanilmadan once yaya kaydini sil
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
        const uint32_t idx = WK::AddVehicleForNet((void*)game, (int)op.vehicleType, pos, op.angleBits);   // WALKER: NET_TYPE -> yaya, digerleri orig_Game_addVehicle
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

static const bool kSyncSteering = true;   // teker (direksiyon) acisi senkronu
// "moving" bayti: bit0 = hareket ediyor, bit1..7 = direksiyon (Vehicle+0x568) 7 bit: 1..127, 64 = duz, 0 = veri yok.
static uint8_t QuantizeSteer(float s) {
    if (!(s == s)) return 64;
    if (s > 1.6f) s = 1.6f; else if (s < -1.6f) s = -1.6f;
    int q = 64 + (int)lroundf(s / 1.6f * 63.0f);
    return (uint8_t)(q < 1 ? 1 : (q > 127 ? 127 : q));
}
static float DequantizeSteer(uint8_t q) { return ((float)q - 64.0f) / 63.0f * 1.6f; }

static void QueueVehiclePosition(uint16_t vehicleId, float x, float y, float angle, bool moving, float steer = 0.0f) {
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
    pkt.moving = (uint8_t)((moving ? 1 : 0) | (kSyncSteering ? (QuantizeSteer(steer) << 1) : 0));

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

// ========================================================================
// ROMORK KONUM SENKRONU
// Romorklar oyunda ayri bir dizide (game+0x1a0..., sayac game+0x19c; Trailer+0x180 = b2Body*,
// +0x184 = bagli oldugu arac, +0x10 = romork tipi). Araclarla AYNI anlik goruntu hattini kullaniriz:
// durum dizilerinde 256+sira numarasi, ag uzerinde 0x8000 | (tip << 8) | sira.
// Gonderen: romork bu cihazda surulen / bu cihazin sahip oldugu araca bagliysa (ve ayrildiktan
// sonra durana kadar) konumunu yayinlar. Alici: romorku sadece sira numarasi VE tipi uyusuyorsa
// ve yerelde surdugu araca bagli degilse yerlestirir (alim/satim sonrasi kayma yanlis romorku oynatmaz).
// Yeni kanca YOK; sadece okuma + b2Body::SetTransform (zaten kullaniliyor).
// ========================================================================
static void SetRemoteBodyVelocity(uintptr_t body, float vx, float vy, float w);
static const uint16_t TRAILER_ID_BASE = 256;
static const uint16_t TRAILER_WIRE_FLAG = 0x8000;
static const uint32_t TRAILER_MAX = 0x1e;
// Aletler (pulluk, mibzer vb.; game+0x290 sayac, game+0x294+i*4): ag kimligi 0xC000 | (tip << 8) | sira, durum dizisi 320+sira.
static const uint16_t TOOL_ID_BASE = 320;
static const uint16_t TOOL_WIRE_FLAG = 0x4000;
static const uint32_t TOOL_MAX = 0x1e;
static bool g_ToolMine[64];
static uint8_t g_RemoteToolType[64];
static bool g_TrailerMine[256];                // bu cihaz bu romorkun konumunu yayinliyor
static uint8_t g_RemoteTrailerType[256];       // uzaktan gelen son tip (dogrulama icin)

// ---- Serbest (araca bagli olmayan) alet/romork icin tek yonlu aktarim ----
// Balya sistemindeki mantigin aynisi: hareketi yalnizca TEK cihaz (arac otoritesi yakinda olan, yoksa host) yayinlar;
// diger cihazlar gelen konumu "sensor" modunda (carpisma tepkisi yok) uygular, uzaktan hareket bitince yerel fizik geri gelir.
// Boylece ust uste duran 10 pulluk her cihazda farkli yone kaymaz.
static const float kLooseNearVehicleR2 = 20.0f * 20.0f;      // otorite sahibi arac bu mesafedeyse nesneyi o iter
static const uint64_t kLooseRemoteFreshMs = 1500;             // uzaktan akis bu kadar tazeyse bu cihaz yayin yapmaz
static const uint64_t kLooseSensorIdleMs = 800;               // uzaktan hareket bitince fizik bu sure sonra geri gelir
static uintptr_t g_AttSensorBody[2][256];                     // [alet?1:0][sira]: sensor yapilan govde (0 = degil)
static uint32_t  g_AttSensorMask[2][256];                     // sensor yapmadan once SENSOR OLMAYAN fixture'lar (bit = fixture sirasi)
static uint64_t  g_AttSensorMs[2][256];

// Govdenin sensor OLMAYAN butun fixture'larini sensor yapar; hangilerini degistirdigini maske olarak dondurur.
static uint32_t B2BodyMakeSensor(uintptr_t body) {
    if (!body) return 0;
    const int fc = *(volatile int*)(body + 0x6C);
    if (fc <= 0 || fc > 32) return 0;
    uint32_t mask = 0;
    uintptr_t f = *(uintptr_t*)(body + 0x68);
    for (int n = 0; f != 0 && n < fc; ++n, f = *(uintptr_t*)(f + 0x14)) {
        if (*(volatile uint8_t*)(f + 0x32) == 0) { *(volatile uint8_t*)(f + 0x32) = 1; mask |= (1u << n); }
    }
    return mask;
}
// B2BodyMakeSensor'un degistirdigi fixture'lari eski haline dondurur ve govdeyi uyandirir.
static void B2BodyRestoreSensor(uintptr_t body, uint32_t mask) {
    if (!body) return;
    const int fc = *(volatile int*)(body + 0x6C);
    if (fc <= 0 || fc > 32) return;
    uintptr_t f = *(uintptr_t*)(body + 0x68);
    for (int n = 0; f != 0 && n < fc; ++n, f = *(uintptr_t*)(f + 0x14)) {
        if (mask & (1u << n)) *(volatile uint8_t*)(f + 0x32) = 0;
    }
    if (*(int*)body != 0) { *(uint16_t*)(body + 4) |= 2; *(float*)(body + 0x90) = 0.0f; }
}

// ---- Romork takma/cikarma olayi ve varlik durumu (doluluk, bicer) paketleri ----
struct __attribute__((packed)) TrailerAttachPacket {
    uint8_t type;
    uint8_t ownerId;        // host'ta peerId ile ezilir
    uint8_t attach;         // bit0: 1 = takildi, 0 = cikarildi | bit1: 1 = ALET (pulluk/mibzer), 0 = romork
    uint8_t trIdx;          // romork sirasi (game+0x1a0..)
    uint8_t trType;         // romork tipi (dogrulama)
    uint16_t vehNetId;      // bagli oldugu aracin netId'si
    float vx, vy, va;       // gonderenin arac konumu/acisi (takma aninda)
    float tx, ty, ta;       // gonderenin romork konumu/acisi
};
struct __attribute__((packed)) EntityStatePacket {
    uint8_t type;
    uint8_t ownerId;
    uint8_t kind;           // 1 = romork, 2 = arac, 3 = korna, 4 = alet doluluk
    uint16_t id;            // romork sirasi ya da arac netId
    uint8_t sub;            // romork: tip
    uint32_t wa;            // romork: seviye (float bitleri) | arac: Vehicle+0x440
    uint32_t wb;            // romork: dolgu tipi            | arac: Vehicle+0x448
    uint8_t flags;          // arac: 1 = bicer calisiyor, 2 = kesici/alet aktif, 4 = alet bagli, 8 = isci (AI) acik
};
static std::vector<TrailerAttachPacket> g_PendingTrailerAttach;   // g_VehicleEventMutex ile korunur
static std::vector<EntityStatePacket> g_PendingEntityStates;      // g_VehicleEventMutex ile korunur
static std::atomic<bool> g_EntityResendAll(false);                // host: yeni oyuncu icin tum durumlari tekrar yolla
static bool g_ApplyingRemoteTrailerOp = false;                    // sadece oyun thread'i

// ---- Tarla islemi (Map::updateFields cagrisi) ve romork/alet ekleme-silme paketleri ----
struct __attribute__((packed)) FieldOpPacket {
    uint8_t type;
    uint8_t ownerId;
    uint32_t wtype;         // FieldWork[0]: islem tipi (1 ekim, 3 hasat, 2/5 isleme...)
    uint32_t wfruit;        // FieldWork[1]: urun / alt tip
    uint32_t p[8];          // 4 kose noktasi (x,y) x4 - float bitleri
};
struct __attribute__((packed)) PoolOpPacket {
    uint8_t type;
    uint8_t ownerId;
    uint8_t kind;           // 1 = romork, 2 = alet
    uint8_t op;             // 1 = eklendi (satin alindi), 0 = silindi (satildi)
    uint16_t itemType;      // Trailer+0x10 / Tool+0x10
    uint8_t idx;            // dizideki sira
    float x, y, a;          // ekleme: konum (box2d x,y) ve aci
};
// ---- Balya (Bale) ve tarla buyume saati paketleri ----
struct __attribute__((packed)) BaleOpPacket {
    uint8_t type;
    uint8_t ownerId;        // host'ta peerId ile ezilir
    uint8_t op;             // 1 = eklendi, 2 = silindi, 3 = tasindi
    uint8_t sale;           // silme: 1 = satis noktasinda satildi (para geliri var)
    float ox, oy;           // tasima: eski konum | silme: silinen balyanin konumu
    float x, y, a;          // ekleme/tasima: yeni konum ve aci
};
struct __attribute__((packed)) BaleListHeader {      // arkasindan count * (x, y, a) float gelir
    uint8_t type;
    uint8_t count;
};
struct __attribute__((packed)) MapClockPacket {
    uint8_t type;
    uint8_t originId;
    uint32_t cycles;        // host'ta tamamlanan buyume turu sayaci
    int32_t idx;            // Map+0xc90 (suan islenen grup, -1 = bekliyor)
    uint32_t timerBits;     // Map+0xc88 (sonraki tura kalan sure, float bitleri)
    uint32_t ownMask;       // host'un sahip oldugu tarlalar (bit i = tarla i), client'a EKLENIR
};
struct __attribute__((packed)) FieldOwnPacket {
    uint8_t type;
    uint8_t ownerId;        // host'ta peerId ile ezilir
    uint8_t idx;            // tarla kaydi sirasi (Map + idx*100 + 0x2c = sahiplik bayti)
    uint8_t owned;
};
static const uint32_t kBaleMax = 50;                              // oyunun balya havuzu (Game+0x388, sayac Game+900)
static std::vector<BaleOpPacket> g_PendingBaleOps;                // g_VehicleEventMutex ile korunur
static std::vector<float> g_PendingBaleList;                      // host'un son tam listesi (x,y,a,...); g_VehicleEventMutex
static bool g_HasPendingBaleList = false;
static MapClockPacket g_PendingMapClock;                          // host'un son saati; g_VehicleEventMutex
static bool g_HasPendingMapClock = false;
static std::vector<FieldOwnPacket> g_PendingFieldOwn;             // g_VehicleEventMutex ile korunur
static std::vector<uint8_t> g_FieldOpOut;                         // g_VehicleEventMutex ile korunur
static std::vector<FieldOpPacket> g_PendingFieldOps;              // g_VehicleEventMutex ile korunur
static std::vector<PoolOpPacket> g_PendingPoolOps;                // g_VehicleEventMutex ile korunur
static bool g_ReplayingFieldOp = false;                           // sadece oyun thread'i: uzaktan tarla islemi uygulaniyor
static const bool kSyncFieldWork = true;                          // tarla senkronu (hasat, pulluk, mibzer...)

typedef void (*Vehicle_attachTrailer_t)(void* vehicle, void* trailer, void* world);
typedef void (*Entity_toggle_t)(void* self);
static Vehicle_attachTrailer_t g_VehicleAttachTrailerFn = nullptr;
static Vehicle_attachTrailer_t g_VehicleAttachToolFn = nullptr;      // Vehicle::attachTool(Tool*, b2World*) - ayni imza
static Entity_toggle_t g_CombineToggleFn = nullptr;               // Vehicle::toggleCombineIsTurnedOn
static Entity_toggle_t g_ToolToggleActiveFn = nullptr;            // Tool::toogleActive
typedef uint32_t (*Vehicle_prepareAi_t)(void* vehicle, void* game);   // Vehicle::prepareStartAi(Game*)
typedef void (*Vehicle_aiStart_t)(void* vehicle, void* game);         // Vehicle::aiStart(Game*)
static Vehicle_prepareAi_t g_VehiclePrepareAiFn = nullptr;
static Vehicle_aiStart_t g_VehicleAiStartFn = nullptr;
static Entity_toggle_t g_VehicleAiStopFn = nullptr;                    // Vehicle::aiStop()
static bool g_AiMirrored[VEHICLE_SLOT_LIMIT];                          // isci bu cihazda UZAKTAN baslatildi (konum yayinlama)
static std::atomic<long long> g_RemoteHornUntilMs(0);                  // uzaktan korna: bu ana kadar acik
static std::atomic<int> g_RemoteHornOrigin(-1);                         // kornaya basan uzak oyuncu
static bool g_HornForced = false;

struct TrailerTrack { uintptr_t tp; uintptr_t av; uint32_t levelWord; uint32_t typeWord; uint64_t lastMs; };
static TrailerTrack g_TrTrack[64];
// valid/flags = bu cihazin bildigi YEREL gercek (en son gonderilen ya da uygulandiktan sonra olculen);
// rvalid/rflags = karsidan en son bildirilen bayraklar (kenar tespiti icin). Ikisi karismaz -> yanki olmaz.
struct CombineTrack { bool valid; uint32_t wa, wb; uint8_t flags; uint64_t lastMs; bool rvalid; uint8_t rflags; };
static const uint8_t kStateFlagMask = 0x0B;   // 1 = bicer/kesici, 2 = alet aktif, 8 = isci
// Aynali isci: baska cihazda calisan isci bu cihazda da baslatilir. Boylece (1) bu cihazin HUD dugmeleri
// (isciye ver / isciden cik, biçer ve alet dugmeleri) dogru gorunur, (2) iscinin cihazi coker/kapanirsa
// tarla isi diger cihazda kesintisiz devam eder. Aynali simulasyon kendi kendine dusebilir; bu durumda
// en fazla 8 kez yeniden baslatilir (ve nedeni loga yazilir). Kullanici dugmesi olmadan aynali iscinin
// kararlari yayinlanmaz (yanki olmasin diye).
static const bool kMirrorAiOnRemote = true;
static const uint8_t kEdgeBit = 0x10;         // paketlerde: "kullanici bunu YENI yapti" (periyodik durum paketinden ayirir)
static uint64_t g_AiIntentMs = 0;             // bu cihazda kullanici isci (AI) dugmesine bastigi an
static int g_AiIntentVeh = -1;                // ... o anda aktif olan aracin netId'si
static bool g_AiPrevOn[VEHICLE_SLOT_LIMIT];   // onceki karede isci acik miydi (basis yonunu anlamak icin)
static uint64_t g_AiRestartMs[VEHICLE_SLOT_LIMIT];
static uint64_t g_AiOriginMs[VEHICLE_SLOT_LIMIT];     // iscyi calistiran cihazdan en son "isci acik" bilgisi alinan an
static uint64_t g_AiOrphanSince[VEHICLE_SLOT_LIMIT];  // aynali isci: sahipsiz kalma baslangici (devralma gecikmesi icin)
static uint64_t g_AiYieldSince[VEHICLE_SLOT_LIMIT];   // gercek isci: aracin sahibi baskasi oldu (geri cekilme icin)
static uint64_t g_AiStopHoldMs[VEHICLE_SLOT_LIMIT];   // bu cihazda kullanici iscyi durdurdu: bu ana kadar gelen ESKI "isci acik" periyodik paketleri yok sayilir
static uint8_t g_AiRestartTries[VEHICLE_SLOT_LIMIT];
static TrailerTrack g_ToolTrack[64];
static CombineTrack g_CbTrack[VEHICLE_SLOT_LIMIT];

static uint32_t TrailerCount(uintptr_t game) {
    const uint32_t n = *(uint32_t*)(game + 0x19C);
    return n > TRAILER_MAX ? TRAILER_MAX : n;
}

static uintptr_t RawTrailerSlot(uintptr_t game, uint32_t i) {
    return *(uintptr_t*)(game + ((uintptr_t)(i + 0x68) * 4u));
}

static uint32_t ToolCount(uintptr_t game) {
    const uint32_t n = *(uint32_t*)(game + 0x290);
    return n > TOOL_MAX ? TOOL_MAX : n;
}

static uintptr_t RawToolSlot(uintptr_t game, uint32_t i) {
    return *(uintptr_t*)(game + 0x294 + (uintptr_t)i * 4u);
}

static uintptr_t LocalActiveVehiclePtr(uintptr_t game) {
    const uint32_t ai = *(uint32_t*)(game + 0xA8);
    return ai < VehicleCount(game) ? RawVehicleSlot(game, ai) : 0;
}

// ---- WALKER (aractan inme) modulu. Gerekenler yukarida tanimli: HookChecked, SetRemoteBodyVelocity (ileri bildirim), orig_Game_addVehicle, ...
// ===== walker_module.inc (tek dosya olsun diye buraya gomuldu) =====
// ============================================================================
// walker_module.inc  (v2)  --  "ARACTAN IN" / "ARACA BIN"           FS12 Android / libapp.so
// yeni_a1003e.cpp icin UYARLANDI. Entegrasyon: walker_integration.patch (9 kucuk ekleme).
// ----------------------------------------------------------------------------
// FIKIR: Yaya = oyunun kendi Vehicle'i (temel arac tipinin desc'i gecici yamali). Boylece
//   kamera, aktif arac degistirme, netId, sahiplik, konum/hiz senkronu (MP kodunuz) HAZIR calisir.
//   Cizim (eklemli iskelet) ve hareket (hiz yazma) bu modulde.
//
// ETIKETLER:  [D] decompile'dan dogrulandi   [K] sizin kodunuzdan dogrulandi
//             [?] cikarim                    [!] bilinmiyor / cihazda test sart
//
// SIZIN KODUNUZDAN KULLANILANLAR (hepsi include noktasindan once tanimli olmali):
//   LOGI, NowMs, FindElfSymbolByPrefix, HookChecked, VehicleCount, RawVehicleSlot, SlotIndexOfVehicle,
//   LocalActiveVehiclePtr, SetRemoteBodyVelocity, orig_Game_addVehicle, my_Game_removeVehicle, g_VehicleDestroyFn,
//   ImGui.   Vehicle::update'i SIZIN hook'unuz (my_Vehicle_update) tutar; modul onu kancalamaz,
//   WK::AfterVehicleUpdate(...) ile cagrilir.
// ============================================================================
#include <math.h>
#include <string.h>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <algorithm>
#include <map>
#include <string>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <dlfcn.h>

namespace WK {

// ------------------------------- AYARLAR -------------------------------
// Dosyalar oyunun assets\objects klasorunde; oyunun kendi yukleyicisi "objects/..." goreli yolunu kullanir [D].
static const char* const PART_FILE[6] = {"objects/walker_torso.p3d", "objects/walker_head.p3d", "objects/walker_armL.p3d",
                                         "objects/walker_armR.p3d", "objects/walker_legL.p3d", "objects/walker_legR.p3d"};
static const char* const TEX_FILE     = "objects/walker_diffuse.p2d";
enum Part { P_TORSO, P_HEAD, P_ARM_L, P_ARM_R, P_LEG_L, P_LEG_R, P_COUNT };

static const int      BASE_TYPE      = 0;      // [!] klonlanacak EntityManager::VEHICLES tipi (en kucuk / en sorunsuz arac)
static const float    WALK_SPEED     = 2.2f;   // m/s
static const float    TURN_RATE      = 3.0f;   // rad/s
static const float    TURN_SIGN      = +1.0f;  // duzeltme: sol/sağ direksiyon işareti ters çevrildi
static const float    ENTER_RADIUS   = 7.0f;   // uzak inis konumundan da araca tekrar binebilmek icin (m)
static const float    EXIT_DIST      = 3.8f;   // inince aracin YANINA (sag/sol, bos olan taraf) koyma mesafesi (m); hitbox yamasi kapaliysa 5.5 yapin
static const float    EXIT_SIDE      = 1.0f;   // +1/-1: araci hangi yanindan inilir
static const float    HITBOX_HX = 0.30f, HITBOX_HY = 0.25f;  // insan kutusu (yari kenar, m) [?: girdi duzeni]
// --- v4: KARARLILIK ONLEMLERI (log bulgusu: yaya dogunca oyun kare basi ~13 sn ile calisti; Vehicle::update f1 = dt) ---
#ifndef WK_MAX_DT_S
#define WK_MAX_DT_S 0.25f      // Game::update dt kelepcesi (sn). 0 = kapali. Oyun dt'yi sinirlamiyor: tek takilma kalici uzun kareye doner.
#endif
#ifndef WK_AUTORECOVER
#define WK_AUTORECOVER 1       // yaya varken kare > WK_SLOW_FRAME_MS ise yayayi otomatik kaldir, bu oturumda kapat
#endif
#ifndef WK_SLOW_FRAME_MS
#define WK_SLOW_FRAME_MS 4000
#endif
#ifndef WK_PROFILER
#define WK_PROFILER 1          // oyun thread'i takilinca PC/LR ornekleyici ("[PROF]"), sadece ARM Android
#endif
// --- desc yamalari: yaya icin kucuk hitbox ve alt-parca iptali varsayilan acik ---
#ifndef WK_PATCH_HITBOX
#define WK_PATCH_HITBOX 1      // v7: yaya icin kucuk hitbox (tek kucuk kutu). Takilma tekrar ederse 0 yapin.
#endif
#ifndef WK_PATCH_PARTS
#define WK_PATCH_PARTS 0       // v6 GÜVENLI TEST: VehicleDesc alanlarini degistirmeden orijinal parca sayisini koru
#endif
#ifndef WK_KEEP_WHEELS
#define WK_KEEP_WHEELS 1       // v7: tekerlek yok (tekerlek govdeleri de cakisma kutusu yapiyordu; decompile: tekerlek sayisi sadece for-dongulerinde). Sorun olursa 1.
#endif
static const bool PATCH_HITBOX = WK_PATCH_HITBOX != 0, PATCH_PARTS = WK_PATCH_PARTS != 0, KEEP_WHEELS = WK_KEEP_WHEELS != 1;
static const bool     DEBUG_INPUT    = true;   // Vehicle::update'in 3 float'ini logcat'e basar
static const float    STRIDE         = 4.5f;   // yuruyus fazi (rad) = hiz(m/s) * STRIDE * dt
// Insan iskeleti (tools/make_walker_assets.py olculeriyle BIREBIR; model degisirse ikisini birlikte guncelleyin)
static const float RIG_NECK[3]  = {0.00f, 1.52f, 0.00f};
static const float RIG_SH_L[3]  = {+0.28f, 1.42f, 0.00f}, RIG_SH_R[3] = {-0.28f, 1.42f, 0.00f};
static const float RIG_HIP_L[3] = {+0.10f, 0.90f, 0.00f}, RIG_HIP_R[3] = {-0.10f, 0.90f, 0.00f};
static const float EYE_HEIGHT   = 1.66f;

// ------------------------------- OFFSETLER -------------------------------
static const uintptr_t G_ACTIVE = 0xA8, G_STATE = 0x64, G_ENTITYMGR = 0x708;           // [D] aktif arac idx / oyun durumu (6=oyun) / EntityManager
static const uintptr_t G_SYSDEV = 0x74, G_RENDERDEV = 0x78;                            // [D] Game::init'te EntityManager::init'e verilen aygitlar
static const uintptr_t EM_DESC0 = 8, DESC_STRIDE = 0x22C;                              // [D] desc = em + 8 + tip*0x22C
static const uintptr_t D_MESH_BODY = 0x1C, D_PART_COUNT = 0x44, D_WHEEL_COUNT = 0xC8;  // [D]
static const uintptr_t D_FIX_COUNT = 0xE4, D_FIX_ARRAY = 0xE8;                         // [D] sayi/dizi; girdi = {cx,cy,hx,hy,angle,density} [?]
static const uintptr_t V_BODY_MAT = 0x18, V_BODY_MESH = 0x58, V_PHYS_BODY = 0x528;     // [D][K]
static const uintptr_t V_CAM_WORLD = 0x4E0;                                            // [D] Game::updateStateBase bunu okur
static const uintptr_t B_X = 0x0C, B_Y = 0x10, B_ANGLE = 0x40, B_VX = 0x44, B_VY = 0x48; // [K] b2Body (sizin kodunuzdaki duzen)

// ------------------------------- MATEMATIK (4x4 kolon-major) -------------------------------
struct M4 { float m[16]; };
static M4 Id() { M4 r; memset(&r, 0, sizeof r); r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f; return r; }
static M4 Mul(const M4& a, const M4& b) {
    M4 r;
    for (int c = 0; c < 4; ++c) for (int rw = 0; rw < 4; ++rw) {
        float s = 0; for (int k = 0; k < 4; ++k) s += a.m[k * 4 + rw] * b.m[c * 4 + k];
        r.m[c * 4 + rw] = s;
    }
    return r;
}
static M4 Tr(float x, float y, float z) { M4 r = Id(); r.m[12] = x; r.m[13] = y; r.m[14] = z; return r; }
static M4 RX(float a) { M4 r = Id(); float c = cosf(a), s = sinf(a); r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c; return r; }
static M4 RY(float a) { M4 r = Id(); float c = cosf(a), s = sinf(a); r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c; return r; }
static M4 AroundPivot(const M4& body, const float p[3], const M4& rot) {
    return Mul(Mul(Mul(body, Tr(p[0], p[1], p[2])), rot), Tr(-p[0], -p[1], -p[2]));
}
static inline float BitsToFloat(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// ------------------------------- DURUM -------------------------------
struct Walker { uintptr_t p; float phase, amp; long long lastMs; };
static std::mutex g_Mu;
static std::vector<Walker> g_Walkers;          // TUM yayalar (yerel + uzak). Dizi KAYAR, bu yuzden pointer ile takip.
static uintptr_t g_LocalWalker = 0;            // bizim yayamiz
static uintptr_t g_LastVehicle = 0;            // indigimiz arac
static float g_HeadYaw = 0, g_HeadPitch = 0;   // yerel kafa (rad). Uzaklara SENKRONLANMAZ (bkz. README "MP genisletme")
static bool  g_FirstPerson = false;
static bool  g_ReqExit = false, g_ReqEnter = false;
static bool  g_AssetsReady = false; static long long g_AssetsRetryMs = 0;
static std::atomic<long long> g_LastFrameMs(0);   // OnGameFrame'in son calisma zamani (ornekleyici de okur)
static std::atomic<int>       g_GameTid(0);       // oyun thread'inin tid'i
static bool g_Disabled = false;                    // otomatik kurtarma sonrasi: bu oturumda yaya kapali
static int  g_PostSpawnFrames = 0;                 // dogustan sonra ayrintili kare logu sayaci
static float g_FpCam[16]; static volatile uint32_t g_FpStamp = 0;   // v7: son yazilan birinci sahis kamera matrisi (kesme duzeltmesi icin)
static void AddLook(float dYaw, float dPitch);                     // asagida tanimli
static uint32_t* g_Mesh[P_COUNT];              // oyunun IndexedTriangleMesh nesnesi: P[0]=vertexBuf, P[1]=indexBuf [D: enqueueRender]
static uint32_t  g_TexObj[4] = {0, 0, 0, 0};   // Texture nesnesi = 4 bayt (GL handle) [D: EntityManager'da Texture dizisi 4'er bayt]

// ------------------------------- CANLI LOG PANELI (libmultiplayermod.so + libapp.so) -------------------------------
// Ayni surecin logcat ciktisini (--pid) okuyup ekranda gosterir. Sol altta "LOG" dugmesi; filtre dugmesi: Hepsi / Mod / libapp.
// Not: logcat kullanilamazsa panelde hata satiri gorunur (cihaz/ROM kisiti) -> adb ile alinan log gecerlidir.
namespace LL {
static std::mutex g_Mu;
static std::vector<std::string> g_Lines;
static bool g_Started = false, g_Open = false;
static int g_Filter = 0;                      // 0 = hepsi, 1 = mod (MultiplayerMod/WALKER), 2 = libapp (NativeLog/FMOD/threaded_app)
static const size_t kMax = 200;
static void Push(const char* s) {
    std::lock_guard<std::mutex> lk(g_Mu);
    g_Lines.push_back(s);
    if (g_Lines.size() > kMax) g_Lines.erase(g_Lines.begin(), g_Lines.begin() + (g_Lines.size() - kMax));
}
static void Reader() {
    char cmd[128];
    snprintf(cmd, sizeof cmd, "logcat -v brief --pid=%d 2>&1", (int)getpid());
    for (int attempt = 0; attempt < 2; ++attempt) {
        FILE* f = popen(cmd, "r");
        if (!f) { Push("E/LL: logcat acilamadi (popen)"); return; }
        char buf[512]; int n = 0;
        while (fgets(buf, sizeof buf, f)) {
            size_t l = strlen(buf); while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = 0;
            if (!l) continue;
            ++n; Push(buf);
        }
        pclose(f);
        if (n == 0) Push("W/LL: logcat bos dondu, 3 sn sonra yeniden denenecek");
        else Push("W/LL: logcat akisi kapandi, yeniden baglaniliyor");
        sleep(3);
    }
}
static void Start() {
    if (g_Started) return; g_Started = true;
    std::thread(Reader).detach();
}
static bool Match(const std::string& s) {
    if (g_Filter == 0) return true;
    if (g_Filter == 1) return s.find("MultiplayerMod") != std::string::npos || s.find("WALKER") != std::string::npos;
    return s.find("NativeLog") != std::string::npos || s.find("FMOD") != std::string::npos || s.find("threaded_app") != std::string::npos
        || s.find("libapp") != std::string::npos;
}
static void Draw(const ImVec2& screen) {
    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.01f, screen.y * 0.99f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::Begin("##llbtn", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBackground);
    if (ImGui::Button(g_Open ? "LOG x" : "LOG", ImVec2(screen.x * 0.07f, screen.y * 0.06f))) g_Open = !g_Open;
    ImGui::End();
    if (!g_Open) return;
    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.01f, screen.y * 0.02f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(screen.x * 0.62f, screen.y * 0.78f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::Begin("##llwin", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    static const char* kNames[3] = { "Filtre: Hepsi", "Filtre: Mod (MultiplayerMod/WALKER)", "Filtre: libapp" };
    if (ImGui::Button(kNames[g_Filter])) g_Filter = (g_Filter + 1) % 3;
    ImGui::SameLine();
    if (ImGui::Button("Temizle")) { std::lock_guard<std::mutex> lk(g_Mu); g_Lines.clear(); }
    ImGui::Separator();
    ImGui::BeginChild("##llscroll", ImVec2(0, 0), false);
    ImGui::PushTextWrapPos(0.0f);
    {
        std::lock_guard<std::mutex> lk(g_Mu);
        for (size_t i = 0; i < g_Lines.size(); ++i) {
            const std::string& s = g_Lines[i];
            if (!Match(s)) continue;
            ImVec4 col(0.85f, 0.85f, 0.85f, 1.0f);
            if (s.find("WATCHDOG") != std::string::npos || (s.size() > 1 && s[0] == 'E' && s[1] == '/')) col = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
            else if (s.find("WALKER") != std::string::npos) col = ImVec4(0.4f, 0.9f, 1.0f, 1.0f);
            else if (s.size() > 1 && s[0] == 'W' && s[1] == '/') col = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
            else if (s.find("MultiplayerMod") != std::string::npos) col = ImVec4(0.6f, 1.0f, 0.6f, 1.0f);
            ImGui::TextColored(col, "%s", s.c_str());
        }
    }
    ImGui::PopTextWrapPos();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);   // sondaysa otomatik kay
    ImGui::EndChild();
    ImGui::End();
}
} // namespace LL

// ------------------------------- TANI: TAKILMA IZLEYICISI -------------------------------
// Oyun ya da cizim thread'i 2 sn'den uzun takilirsa logcat'e "son asama" yazar: donmanin NEREDE oldugunu gosterir.
//   logcat: adb logcat -d | Select-String "WALKER|MultiplayerMod"
static const char* volatile g_StageGame = "-";
static const char* volatile g_StageRender = "-";
static volatile uint32_t g_TickGame = 0, g_TickRender = 0;
static inline void StageG(const char* s) { g_StageGame = s; }
static inline void StageR(const char* s) { g_StageRender = s; }
static inline void TickRender() { g_TickRender = g_TickRender + 1; }
static void WatchdogThread() {
    uint32_t lastG = 0, lastR = 0; int stG = 0, stR = 0;
    for (;;) {
        usleep(1000000);
        const uint32_t g = g_TickGame, r = g_TickRender;
        if (g != 0 && g == lastG) ++stG; else stG = 0;
        if (r != 0 && r == lastR) ++stR; else stR = 0;
        lastG = g; lastR = r;
        if (stG >= 2 && (stG % 3) == 2) LOGI("[WALKER][WATCHDOG] OYUN thread'i %d sn takildi. son asama=%s", stG, g_StageGame);
        if (stR >= 2 && (stR % 3) == 2) LOGI("[WALKER][WATCHDOG] CIZIM (ImGui) thread'i %d sn takildi. son asama=%s", stR, g_StageRender);
    }
}

// ------------------------------- SEMBOL TIPLERI -------------------------------
// DUZELTME: IndexedTriangleMesh::load bir UYE fonksiyondur: ilk arguman 'this' = 8 baytlik {vertexBuf, indexBuf} nesnesi [D: libapp_so.c 5962, 23546].
typedef void*    (*MeshLoad_t)(void* self, const char* path, void* sys, void* dev);                       // IndexedTriangleMesh::load(this, const char*, SysDev*, RenderDev*) [D]
typedef uint32_t (*TexLoad_t)(void* tex, const char* path, void* sys, void* dev, uint32_t keep, uint32_t loc); // Texture::load [D]
// DUZELTME: iki asiri yukleme var; oyunun Vehicle kodu 3 argumanli olani kullanir: (dev, matris, vb, ib). 4 argumanli olan (dev, m, indexSayisi, vb, ib) [D: 44836, 44898].
typedef void     (*Draw44_t)(void* dev, const float* m, uint32_t vb, uint32_t ib);                        // GLESHandheldRenderDevice::enqueueDrawColumnMajor44(float const*, uint, uint) [D]
// DUZELTME: bindTextureLayer(this, doku, KATMAN): Ghidra 3. argumani (r2) dusurmus; govde glActiveTexture(r2 + 0x84C0) kullaniyor [D: 44167].
typedef void     (*BindTex_t)(void* dev, uint32_t handle, uint32_t layer);                                // bindTextureLayer [D]
// DUZELTME: mesh yuklendikten sonra oyun her vertex buffer icin bindShaderAttributes cagirir [D: 40088, 23549].
typedef void     (*BindAttrs_t)(void* dev, uint32_t vb, uint32_t prog, const void* bindings);             // GLESHandheldRenderDevice::bindShaderAttributes [D]
typedef void     (*ActiveChanged_t)(void* game);                                                          // Game::activeVehicleChanged [D]
typedef void     (*VehRender_t)(void* self, void* ra, void* dev, uint32_t p3);                            // Vehicle::enqueueRender [D]
typedef void     (*VehRender3_t)(void* self, void* ra, void* dev);                                        // Transparent / UVScroll / EnvMap [D]

static MeshLoad_t fnMeshLoad = nullptr; static TexLoad_t fnTexLoad = nullptr; static Draw44_t fnDraw = nullptr;
static BindTex_t fnBind = nullptr; static BindAttrs_t fnBindAttrs = nullptr; static ActiveChanged_t fnActiveChanged = nullptr;
static VehRender_t orig_VehRender = nullptr;
// Vehicle::enqueueRenderEmissive(RenderArgs&, dev, bool, bool, bool, Texture*): 7 arguman (3'u yigitta), geri donus r0:r1 [D: 18003]
typedef uint64_t (*VehEmis_t)(void* self, void* ra, void* dev, uint32_t a, uint32_t b, uint32_t c, void* tex);
static VehEmis_t orig_VehEmis = nullptr;
// VertexShaderAttributeBinding kumesi: 4 giris x 0xC bayt (isim*, tip, normalize bayti) + sayi @+0x30 [D: bindShaderAttributes govdesi]
struct AttrBind { const char* name; uint32_t type; uint32_t flag; };
struct AttrSet  { AttrBind b[4]; uint32_t count; };
static uint32_t g_MeshObj[P_COUNT][2];         // her parca icin IndexedTriangleMesh nesnesi (8 bayt)
static bool     g_MeshOk[P_COUNT];
static const uintptr_t G_SHADER = 0xA340;      // [D] Game::init -> EntityManagerDesc+0x10 = *(Game+0xA340): arac mesh'lerinin shader numarasi

// ------------------------------- YARDIMCILAR -------------------------------
static bool IsWalker(uintptr_t p) {
    if (!p) return false;
    std::lock_guard<std::mutex> lk(g_Mu);
    for (size_t i = 0; i < g_Walkers.size(); ++i) if (g_Walkers[i].p == p) return true;
    return false;
}
static void OnVehicleRemoved(uintptr_t p) {
    if (!p) return;
    std::lock_guard<std::mutex> lk(g_Mu);
    for (size_t i = g_Walkers.size(); i-- > 0;) if (g_Walkers[i].p == p) g_Walkers.erase(g_Walkers.begin() + i);
    if (g_LocalWalker == p) g_LocalWalker = 0;
    if (g_LastVehicle == p) g_LastVehicle = 0;
}
static bool BodyPose(uintptr_t veh, float* x, float* y, float* a) {
    const uintptr_t b = *(uintptr_t*)(veh + V_PHYS_BODY); if (!b) return false;
    *x = *(float*)(b + B_X); *y = *(float*)(b + B_Y); *a = *(float*)(b + B_ANGLE); return true;
}
static void SetActiveByPtr(uintptr_t game, uintptr_t veh) {
    const int idx = SlotIndexOfVehicle(game, veh); if (idx < 0) return;
    *(uint32_t*)(game + G_ACTIVE) = (uint32_t)idx;                    // [D] Game::buyItem de boyle yapiyor
    StageG("SetActive:activeVehicleChanged");
    if (fnActiveChanged) fnActiveChanged((void*)game);                // ses + kamera durumu [D]
    StageG("SetActive:bitti");
}

// ------------------------------- VARLIKLAR (GL thread'inde) -------------------------------
// Oyunun KENDI yukleyicileri kullanilir; dosyalar assets\objects'te. Aygit gostericileri Game+0x74/+0x78 [D].
static void EnsureAssets(uintptr_t game) {
    if (g_AssetsReady) return;
    const long long now = NowMs(); if (now < g_AssetsRetryMs) return; g_AssetsRetryMs = now + 5000;
    void* sys = *(void**)(game + G_SYSDEV); void* dev = *(void**)(game + G_RENDERDEV);
    if (!sys || !dev || !fnMeshLoad || !fnTexLoad || !fnDraw || !fnBind) {
        LOGI("[WALKER] varlik: sembol/aygit yok (sys=%p dev=%p mesh=%p tex=%p draw=%p bind=%p)", sys, dev, (void*)fnMeshLoad, (void*)fnTexLoad, (void*)fnDraw, (void*)fnBind);
        return;
    }
    const uint32_t prog = *(volatile uint32_t*)(game + G_SHADER);
    AttrSet as; memset(&as, 0, sizeof as);                                     // oyunun kendi arac mesh'leriyle AYNI vertex duzeni (pos, normal, uv)
    as.b[0].name = "Position";  as.b[0].type = 0xB; as.b[0].flag = 0;
    as.b[1].name = "Normal";    as.b[1].type = 0xC; as.b[1].flag = 1;
    as.b[2].name = "TexCoord0"; as.b[2].type = 7;   as.b[2].flag = 0;
    as.count = 3;
    for (int i = 0; i < P_COUNT; ++i) {
        if (g_MeshOk[i]) continue;                                             // yeniden denemede yuklenenleri tekrar yukleme
        g_MeshObj[i][0] = g_MeshObj[i][1] = 0xFFFFFFFFu;
        fnMeshLoad(g_MeshObj[i], PART_FILE[i], sys, dev);
        if (g_MeshObj[i][0] == 0xFFFFFFFFu || g_MeshObj[i][1] == 0xFFFFFFFFu) { LOGI("[WALKER] mesh yuklenemedi: %s (assets\\objects'te mi?)", PART_FILE[i]); return; }
        if (fnBindAttrs) fnBindAttrs(dev, g_MeshObj[i][0], prog, &as);
        g_Mesh[i] = g_MeshObj[i]; g_MeshOk[i] = true;
    }
    if (g_TexObj[0] == 0) fnTexLoad(g_TexObj, TEX_FILE, sys, dev, 0, 0xFFFFFFFFu);
    // [?] DOGRULAMA: oyunun kendi mesh gostericisiyle kiyasla; ikisi de kucuk GL handle sayilari olmali
    const uintptr_t desc = game + G_ENTITYMGR + EM_DESC0 + (uintptr_t)BASE_TYPE * DESC_STRIDE;
    const uint32_t* base = *(uint32_t**)(desc + D_MESH_BODY);
    LOGI("[WALKER] varliklar yuklendi: tex=%u shader=%u bindAttrs=%d ourMesh0=(%u,%u) gameBaseMesh=(%u,%u)", g_TexObj[0], prog, (int)(fnBindAttrs != nullptr),
         g_Mesh[0][0], g_Mesh[0][1], base ? base[0] : 0, base ? base[1] : 0);
    g_AssetsReady = (g_TexObj[0] != 0);
}

// ------------------------------- OLUSTURMA (desc'i gecici yamala) -------------------------------
// Vehicle::load desc gostericisini SAKLAMAZ, mesh/fixture/tekerlek bilgisini kopyalar [D] -> yedekle/yamala/ekle/geri yukle.
// Hem yerel inis hem uzaktan SPAWN (NET_TYPE) buradan gecer. orig_Game_addVehicle sizin kodunuzda HOOK'suz ham fonksiyondur [K].
static uint32_t SpawnWalker(uintptr_t game, const float* pos3, uint32_t angleBits, bool local) {
    uint8_t* desc = (uint8_t*)(game + G_ENTITYMGR + EM_DESC0 + (uintptr_t)BASE_TYPE * DESC_STRIDE);
    uint8_t backup[DESC_STRIDE]; memcpy(backup, desc, DESC_STRIDE);
    if (PATCH_HITBOX) {                                                        // hitbox: tek kucuk kutu
        *(uint32_t*)(desc + D_FIX_COUNT) = 1;
        float* fx = (float*)(desc + D_FIX_ARRAY); fx[0] = 0; fx[1] = 0; fx[2] = HITBOX_HX; fx[3] = HITBOX_HY; fx[4] = 0; fx[5] = 1.0f;
    }
    if (!KEEP_WHEELS) *(uint32_t*)(desc + D_WHEEL_COUNT) = 0;                  // tekerlek fizigi yok
    if (PATCH_PARTS)  *(uint32_t*)(desc + D_PART_COUNT) = 0;                   // alt parcalari biz cizeriz
    LOGI("[WALKER] yama: hitbox=%d parts=%d wheels0=%d", (int)PATCH_HITBOX, (int)PATCH_PARTS, (int)!KEEP_WHEELS);
    const long long tSpawn0 = NowMs();
    StageG("SpawnWalker:addVehicle");
    LOGI("[WALKER] SpawnWalker: addVehicle pos=(%.2f,%.2f,%.2f) baseType=%d", pos3[0], pos3[1], pos3[2], (int)BASE_TYPE);
    const uint32_t idx = orig_Game_addVehicle((void*)game, BASE_TYPE, pos3, angleBits, 0);
    StageG("SpawnWalker:addVehicle-bitti");
    LOGI("[WALKER] addVehicle SURESI = %lld ms", NowMs() - tSpawn0);           // ZAMAN: takilma burada mi, sonraki karelerde mi?
    memcpy(desc, backup, DESC_STRIDE);                                         // temel arac bozulmasin
    if (idx >= VehicleCount(game)) return idx;
    const uintptr_t veh = RawVehicleSlot(game, idx);
    { std::lock_guard<std::mutex> lk(g_Mu); Walker w; w.p = veh; w.phase = 0; w.amp = 0; w.lastMs = 0; g_Walkers.push_back(w); if (local) { g_LocalWalker = veh; g_PostSpawnFrames = 20; } }
    LOGI("[WALKER] dogdu idx=%u veh=%p local=%d", idx, (void*)veh, (int)local);
    return idx;
}
// MP: uzaktan SPAWN uygulayan yer (sizin kodunuzdaki orig_Game_addVehicle cagrisi yerine). type==NET_TYPE -> yaya.
static uint32_t AddVehicleForNet(void* game, int type, const float* pos, uint32_t angleBits) {
    if (type == (int)WK_NET_TYPE) return SpawnWalker((uintptr_t)game, pos, angleBits, false);
    return orig_Game_addVehicle(game, type, pos, angleBits, 0);
}

// ------------------------------- IN / BIN (OYUN thread'i) -------------------------------
static void DoExit(uintptr_t game) {
    StageG("DoExit:basla");
    const uintptr_t cur = LocalActiveVehiclePtr(game);
    LOGI("[WALKER] DoExit: cur=%p walker=%d localWalker=%p count=%u", (void*)cur, (int)(cur && IsWalker(cur)), (void*)g_LocalWalker, VehicleCount(game));
    if (g_Disabled || !cur || IsWalker(cur) || g_LocalWalker || VehicleCount(game) >= 30) return;
    float x, y, a; if (!BodyPose(cur, &x, &y, &a)) return;
    LOGI("[WALKER] DoExit: arac pozu x=%.2f y=%.2f a=%.3f", x, y, a);
    // v7: ARACIN YANINA in (on/arka degil). Yon, govde matrisinin ileri ekseninden (M[8],M[10]) bulunur -> aci sozlesmesinden bagimsiz.
    // Sag ve sol aday noktalardan, diger araclara/yayalara daha uzak (= bos) olan secilir; esitse EXIT_SIDE tercihi.
    float lx = 0.f, ly = 1.f;
    {
        const float* CM = (const float*)(cur + V_BODY_MAT);
        float fx = CM[8], fy = CM[10]; const float fl = sqrtf(fx * fx + fy * fy);
        if (fl > 1e-4f) { fx /= fl; fy /= fl; lx = -fy; ly = fx; } else { lx = -sinf(a); ly = cosf(a); }
    }
    const float cxA = x + lx * EXIT_DIST, cyA = y + ly * EXIT_DIST;              // aday A
    const float cxB = x - lx * EXIT_DIST, cyB = y - ly * EXIT_DIST;              // aday B
    float freeA = 1e9f, freeB = 1e9f;
    for (uint32_t i = 0; i < VehicleCount(game); ++i) {
        const uintptr_t v = RawVehicleSlot(game, i); if (!v || v == cur) continue;
        float ox, oy, oa; if (!BodyPose(v, &ox, &oy, &oa)) continue;
        const float dA = sqrtf((ox - cxA) * (ox - cxA) + (oy - cyA) * (oy - cyA));
        const float dB = sqrtf((ox - cxB) * (ox - cxB) + (oy - cyB) * (oy - cyB));
        if (dA < freeA) freeA = dA;
        if (dB < freeB) freeB = dB;
    }
    bool useA = (EXIT_SIDE >= 0.f);
    if (freeA > freeB + 1.0f) useA = true; else if (freeB > freeA + 1.0f) useA = false;
    const float px = useA ? cxA : cxB, py = useA ? cyA : cyB;
    LOGI("[WALKER] DoExit: yan secimi A=(%.1f,%.1f) bos=%.1f B=(%.1f,%.1f) bos=%.1f -> %s", cxA, cyA, freeA, cxB, cyB, freeB, useA ? "A" : "B");
    const float pos[3] = {px, 0.0f, py};                                       // Vector3(x, yukseklik, fizik-y) [D][K]
    uint32_t ab; memcpy(&ab, &a, 4);
    StageG("DoExit:SpawnWalker");
    const uint32_t idx = SpawnWalker(game, pos, ab, true);
    LOGI("[WALKER] DoExit: SpawnWalker idx=%u (count=%u)", idx, VehicleCount(game));
    if (idx >= VehicleCount(game)) return;
    g_LastVehicle = cur; g_HeadYaw = g_HeadPitch = 0;
    StageG("DoExit:SetActive");
    SetActiveByPtr(game, RawVehicleSlot(game, idx));                           // MP yayini: DetectNewLocalVehicles netId atar ve duyurur [K]
    StageG("DoExit:bitti");
    LOGI("[WALKER] DoExit: tamam");
}
static void DoEnter(uintptr_t game, bool ignoreRadius) {
    if (!g_LocalWalker || LocalActiveVehiclePtr(game) != g_LocalWalker) return;
    float wx, wy, wa; if (!BodyPose(g_LocalWalker, &wx, &wy, &wa)) return;
    uintptr_t best = 0; float bd = ignoreRadius ? 1e18f : ENTER_RADIUS * ENTER_RADIUS;
    for (uint32_t i = 0; i < VehicleCount(game); ++i) {
        const uintptr_t v = RawVehicleSlot(game, i); if (!v || IsWalker(v)) continue;
        float x, y, a; if (!BodyPose(v, &x, &y, &a)) continue;
        const float d = (x - wx) * (x - wx) + (y - wy) * (y - wy);
        if (d < bd) { bd = d; best = v; }
    }
    if (ignoreRadius && g_LastVehicle && SlotIndexOfVehicle(game, g_LastVehicle) >= 0) best = g_LastVehicle;
    if (!best) return;
    SetActiveByPtr(game, best);                                                // once hedefe gec
    const uintptr_t w = g_LocalWalker; const int wi = SlotIndexOfVehicle(game, w);
    if (wi >= 0) {
        my_Game_removeVehicle((void*)game, (uint32_t)wi);                      // sizin hook'unuz: REMOVE olayini yayinlar, sonra orijinali cagirir [K]
        if (g_VehicleDestroyFn) g_VehicleDestroyFn((void*)w);                  // uzaktan silme yolundaki gibi [K]
    }
    OnVehicleRemoved(w);
    SetActiveByPtr(game, best);                                                // dizi kaydi: indeksi yeniden hesapla
}

// ------------------------------- Vehicle::update ARDINDAN (sizin my_Vehicle_update'ten cagrilir) -------------------------------
// a,b,c = Vehicle::update'in 3 float argumani (bit deseni). Anlamlari decompile'da KAYIP [!]. Tahmin: a=dt, b=direksiyon, c=gaz.
// DEBUG_INPUT ile joystick oynatip logcat'ten teyit edin, sonra asagidaki 2 satiri duzeltin.
static void AfterVehicleUpdate(uintptr_t self, uint32_t ua, uint32_t ub, uint32_t uc, uint32_t ud) {
    if (self != g_LocalWalker || (ud & 0xFF) == 0) return;                     // sadece AKTIF yerel yaya
    const float f1 = BitsToFloat(ua), f2 = BitsToFloat(ub), f3 = BitsToFloat(uc);
    static long long s_log = 0; const long long now = NowMs();
    if (DEBUG_INPUT && (fabsf(f2) + fabsf(f3) > 0.05f) && now - s_log > 500) { s_log = now; LOGI("[WALKER] update girdi f1=%.3f f2=%.3f f3=%.3f", f1, f2, f3); }
    StageG("AfterVehicleUpdate");
    // TANI: girdi tahmini yanlis olabilir -> NaN/dev deger Box2D'yi bozmasin: sonlu degilse 0, aralik [-1,1]
    float steer = f2, throttle = f3;                                           // [!] teyit edilecek
    if (!(steer == steer) || !(throttle == throttle) || fabsf(steer) > 1e6f || fabsf(throttle) > 1e6f) { steer = 0; throttle = 0; }
    if (steer > 1.f) steer = 1.f; if (steer < -1.f) steer = -1.f;
    if (throttle > 1.f) throttle = 1.f; if (throttle < -1.f) throttle = -1.f;
    const uintptr_t body = *(uintptr_t*)(self + V_PHYS_BODY); if (!body) return;
    {   // TANI: saniyede bir yaya durumu
        static long long s_st = 0;
        if (now - s_st > 1000) {
            s_st = now;
            LOGI("[WALKER] durum x=%.2f y=%.2f vx=%.2f vy=%.2f ang=%.2f | f1=%.3f f2=%.3f f3=%.3f -> steer=%.2f thr=%.2f fp=%d",
                 *(float*)(body + B_X), *(float*)(body + B_Y), *(float*)(body + B_VX), *(float*)(body + B_VY), *(float*)(body + B_ANGLE),
                 f1, f2, f3, steer, throttle, (int)g_FirstPerson);
        }
    }
    // ileri yon = govde matrisinin model +z ekseni (fizik duzlemine (x,z) olarak izdusum) -> acisal sozlesmeden bagimsiz
    const float* M = (const float*)(self + V_BODY_MAT);
    float fx = M[8], fy = M[10]; const float fl = sqrtf(fx * fx + fy * fy);
    if (fl > 1e-4f) { fx /= fl; fy /= fl; } else { fx = 1.f; fy = 0.f; }
    // v7: geri giderken sag/sol ters calisiyordu -> geri (gaz < 0) iken donus yonu tersine cevrilir (arac gibi degil, "dogal" yon)
    const float revSign = (throttle < -0.05f) ? -1.0f : 1.0f;
    SetRemoteBodyVelocity(body, fx * throttle * WALK_SPEED, fy * throttle * WALK_SPEED, TURN_SIGN * revSign * steer * TURN_RATE);   // [K] uyandirma dahil
    if (g_FirstPerson) {                                                       // kamera dunya matrisi: oyun daha sonra bunun tersini view matrisi yapar
        M4 B; memcpy(B.m, M, 64);
        // Vehicle::update, araç model matrisini açı-PI ile kurar; kamera normalde -Z yönüne bakar.
        // Yarım tur düzeltmesi kamerayı aracın arkasından önüne çevirir.
        const float pi = 3.14159265358979323846f;
        M4 cam = Mul(Mul(Mul(B, Tr(0, EYE_HEIGHT, 0)), RY(pi + g_HeadYaw)), RX(g_HeadPitch));
        memcpy((void*)(self + V_CAM_WORLD), cam.m, 64);
        memcpy(g_FpCam, cam.m, 64); g_FpStamp = g_TickGame;                    // v7: RenderArgs::update kancasi (kesme duzeltmesi) bunu tanir
    }
}

// ------------------------------- v7: BIRINCI SAHIS GORUS KESME (CULL) DUZELTMESI -------------------------------
// Bulgu (decompile): Game::updateStateBase -> RenderArgs::update(view, camWorld) kamera ustunden yer duzlemindeki "gorunur taban" alanini
// hesaplar (RenderArgs+0xC4..: kamera frustumunun yer ile kesisimi; Map::cullScene arazi/nesne hucrelerini buna gore secer).
// Yatay bakan birinci sahis kamerasinda ust isin yere hic inmez -> taban alani cokup ILERISI HIC OLUSTURULMAZ.
// Cozum: sadece KESME icin ayni yonde, yukarida ve geride, asagi bakan sanal bir kamera verilir; cizim gorunumu (aMStack_98) degismez.
#ifndef WK_FP_CULLFIX
#define WK_FP_CULLFIX 1
#endif
#ifndef WK_FP_CULL_UP
#define WK_FP_CULL_UP 35.0f       // sanal kamera yuksekligi (m)
#endif
#ifndef WK_FP_CULL_BACK
#define WK_FP_CULL_BACK 22.0f     // sanal kamera geriye kaydirma (m)
#endif
#ifndef WK_FP_CULL_PITCH
#define WK_FP_CULL_PITCH 0.80f    // asagi bakis acisi (rad, ~46 derece)
#endif
typedef void (*RAUpdate_t)(void*, const float*, const float*);
static RAUpdate_t orig_RAUpdate = nullptr;
static bool AffineInverse(const float* m, float* o) {                          // kolon-major 3x4 afin matris tersi (alt satir 0,0,0,1)
    const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!(fabsf(det) > 1e-9f)) return false;
    const float id = 1.0f / det;
    const float r00 = (e * i - f * h) * id, r01 = (c * h - b * i) * id, r02 = (b * f - c * e) * id;
    const float r10 = (f * g - d * i) * id, r11 = (a * i - c * g) * id, r12 = (c * d - a * f) * id;
    const float r20 = (d * h - e * g) * id, r21 = (b * g - a * h) * id, r22 = (a * e - b * d) * id;
    const float tx = m[12], ty = m[13], tz = m[14];
    memset(o, 0, 64);
    o[0] = r00; o[4] = r01; o[8] = r02;  o[12] = -(r00 * tx + r01 * ty + r02 * tz);
    o[1] = r10; o[5] = r11; o[9] = r12;  o[13] = -(r10 * tx + r11 * ty + r12 * tz);
    o[2] = r20; o[6] = r21; o[10] = r22; o[14] = -(r20 * tx + r21 * ty + r22 * tz);
    o[15] = 1.0f;
    return true;
}
static void my_RAUpdate(void* self, const float* view, const float* cam) {
    if (WK_FP_CULLFIX && g_FirstPerson && g_LocalWalker && cam && (uint32_t)(g_TickGame - g_FpStamp) <= 3 && memcmp(cam, g_FpCam, 64) == 0) {
        M4 C; memcpy(C.m, cam, 64);
        const M4 base = Mul(C, Tr(0, WK_FP_CULL_UP, WK_FP_CULL_BACK));
        const M4 c1 = Mul(base, RX(-WK_FP_CULL_PITCH)), c2 = Mul(base, RX(+WK_FP_CULL_PITCH));
        const M4& cv = (c1.m[9] >= c2.m[9]) ? c1 : c2;                         // bakis ekseni (-Z) asagi gosteren aday: dunya y'si azalan = m[9] buyuk
        float inv[16];
        if (AffineInverse(cv.m, inv)) {
            static int s_n = 0; if (s_n < 3) { ++s_n; LOGI("[WALKER] kesme kamerasi aktif: konum=(%.1f,%.1f,%.1f) bakis.y=%.2f (gercek kamera y=%.1f)", cv.m[12], cv.m[13], cv.m[14], -cv.m[9], cam[13]); }
            orig_RAUpdate(self, inv, cv.m);
            return;
        }
    }
    orig_RAUpdate(self, view, cam);
}

// ------------------------------- v7: BIRINCI SAHIS BAKIS (DOKUNMA SURUKLEME) -------------------------------
// Ekranin ortasindaki bolgede (sol joystick, sag gaz surgusu ve alt dugmelerin disinda) parmak surukleyince kafa doner.
// Oyuna giden olay YUTULMAZ; sadece ek bir parmak/surukleme izlenir.
#ifndef WK_LOOK_YAW_SIGN
#define WK_LOOK_YAW_SIGN (-1.0f)     // yatay yon ters ise +1
#endif
#ifndef WK_LOOK_PITCH_SIGN
#define WK_LOOK_PITCH_SIGN (-1.0f)   // dikey yon ters ise +1
#endif
static void TouchLook(const AInputEvent* ev, int32_t rawAction, float W, float H) {
    static int s_id = -1; static float s_lx = 0, s_ly = 0;
    if (!g_FirstPerson || !g_LocalWalker || W < 100.f || H < 100.f) { s_id = -1; return; }
    const int32_t act = rawAction & AMOTION_EVENT_ACTION_MASK;
    const size_t n = AMotionEvent_getPointerCount(ev);
    if (act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        const size_t pi = (act == AMOTION_EVENT_ACTION_DOWN) ? 0 : (size_t)((rawAction & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        if (pi < n && s_id < 0) {
            const float x = AMotionEvent_getX(ev, pi), y = AMotionEvent_getY(ev, pi);
            if (x > W * 0.30f && x < W * 0.86f && y > H * 0.14f && y < H * 0.76f) { s_id = AMotionEvent_getPointerId(ev, pi); s_lx = x; s_ly = y; }
        }
    } else if (act == AMOTION_EVENT_ACTION_MOVE) {
        for (size_t i = 0; i < n; ++i) if (s_id >= 0 && AMotionEvent_getPointerId(ev, i) == s_id) {
            const float x = AMotionEvent_getX(ev, i), y = AMotionEvent_getY(ev, i);
            const float k = 3.2f / W;                                          // ekran genisligi ~ 3.2 rad
            AddLook(WK_LOOK_YAW_SIGN * (x - s_lx) * k, WK_LOOK_PITCH_SIGN * (y - s_ly) * k);
            s_lx = x; s_ly = y;
        }
    } else if (act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_CANCEL) {
        s_id = -1;
    } else if (act == AMOTION_EVENT_ACTION_POINTER_UP) {
        const size_t pi = (size_t)((rawAction & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        if (pi < n && AMotionEvent_getPointerId(ev, pi) == s_id) s_id = -1;
    }
}

// Sahte Vehicle olarak kullanılan yaya, HUD'da yakıtı olan gerçek araç gibi görünmesin.
// Game::updateStateBase, Vehicle+0x444 (kapasite) pozitifse Vehicle+0x440 değerini HUD'a aktarır.
static void StripWalkerVehicleHud(uintptr_t self) {
    if (!IsWalker(self)) return;
    *(float*)(self + 0x440) = 0.0f;  // mevcut yakıt
    *(float*)(self + 0x444) = 0.0f;  // yakıt kapasitesi: HUD yakıt oranını kapat
}

// ------------------------------- CIZIM -------------------------------
static void DrawWalker(uintptr_t self, void* dev) {
    if (!g_AssetsReady || !fnDraw || !fnBind) return;                          // hazir degilse hicbir sey cizme (temel arac gorunmesin)
    StageR("DrawWalker:basla");
    { static int s_n = 0; if (s_n < 3) { ++s_n; LOGI("[WALKER] DrawWalker #%d self=%p dev=%p", s_n, (void*)self, dev); } }
    const uintptr_t body = *(uintptr_t*)(self + V_PHYS_BODY);
    float speed = 0; if (body) { const float vx = *(float*)(body + B_VX), vy = *(float*)(body + B_VY); speed = sqrtf(vx * vx + vy * vy); }
    float ph = 0, amp = 0;
    {   // animasyon GERCEK hizdan (yerel + uzak ayni): uzak yaya icin ekstra paket gerekmez
        std::lock_guard<std::mutex> lk(g_Mu);
        for (size_t i = 0; i < g_Walkers.size(); ++i) if (g_Walkers[i].p == self) {
            Walker& w = g_Walkers[i]; const long long now = NowMs();
            float dt = w.lastMs ? (now - w.lastMs) * 0.001f : 0.016f; if (dt > 0.1f) dt = 0.1f; w.lastMs = now;
            const float target = fminf(speed / WALK_SPEED, 1.f);
            w.amp += (target - w.amp) * fminf(dt * 8.f, 1.f);
            w.phase += speed * STRIDE * dt;
            ph = w.phase; amp = w.amp; break;
        }
    }
    M4 B; memcpy(B.m, (void*)(self + V_BODY_MAT), 64);
    const float sw = sinf(ph) * amp;
    M4 mats[P_COUNT];
    mats[P_TORSO] = B;
    mats[P_LEG_L] = AroundPivot(B, RIG_HIP_L, RX(+sw * 0.5f)); mats[P_LEG_R] = AroundPivot(B, RIG_HIP_R, RX(-sw * 0.5f));
    mats[P_ARM_L] = AroundPivot(B, RIG_SH_L,  RX(-sw * 0.4f)); mats[P_ARM_R] = AroundPivot(B, RIG_SH_R, RX(+sw * 0.4f));
    const bool mine = (self == g_LocalWalker);
    mats[P_HEAD] = AroundPivot(B, RIG_NECK, mine ? Mul(RY(g_HeadYaw), RX(g_HeadPitch)) : Id());   // uzak yayada kafa duz bakar (senkron yok)
    // GUVENLIK: GL adlari kucuk pozitif tamsayidir; cop deger (0, 0xFFFFFFFF, ASCII) ile cizmek surucuyu dondurur.
    static bool s_disabled = false;
    if (s_disabled) return;
    bool sane = (g_TexObj[0] != 0 && g_TexObj[0] < 0x100000u);
    for (int i = 0; i < P_COUNT && sane; ++i) sane = g_Mesh[i] && g_Mesh[i][0] != 0 && g_Mesh[i][1] != 0 && g_Mesh[i][0] < 0x100000u && g_Mesh[i][1] < 0x100000u;
    if (!sane) { s_disabled = true; g_AssetsReady = false; LOGI("[WALKER] CIZIM KAPATILDI: GL handle'lari gecersiz (tex=%u)", g_TexObj[0]); return; }
    StageR("DrawWalker:bindTex");
    fnBind(dev, g_TexObj[0], 0);                                               // katman 0 = diffuse [D: bindTextureLayer 3. arguman]
    StageR("DrawWalker:parcalar");
    const bool hideHead = mine && g_FirstPerson;                                // v7: birinci sahista kafa kameranin onunu kapatmasin
    for (int i = 0; i < P_COUNT; ++i) if (g_Mesh[i] && !(hideHead && i == P_HEAD)) fnDraw(dev, mats[i].m, g_Mesh[i][0], g_Mesh[i][1]);
    StageR("DrawWalker:bitti");
}
static void my_VehRender(void* self, void* ra, void* dev, uint32_t p3) {
    if (IsWalker((uintptr_t)self)) { DrawWalker((uintptr_t)self, dev); return; }
    StageR("VehRender(normal)");
    orig_VehRender(self, ra, dev, p3);
}
// Emissive gecisi: temel aracin far/isik mesh'leri yayanin uzerinde cizilmesin. Donus 0 = "isik cizilmedi" [D: local_20 = 0].
static uint64_t my_VehEmis(void* self, void* ra, void* dev, uint32_t a, uint32_t b, uint32_t c, void* tex) {
    if (IsWalker((uintptr_t)self)) return 0;
    return orig_VehEmis(self, ra, dev, a, b, c, tex);
}
// Diger cizim gecisleri temel aracin mesh'lerini yayanin uzerine cizmesin diye bastirilir.
#define WK_SUPPRESS(NAME) \
    static VehRender3_t orig_##NAME = nullptr; \
    static void my_##NAME(void* self, void* ra, void* dev) { if (IsWalker((uintptr_t)self)) return; orig_##NAME(self, ra, dev); }
WK_SUPPRESS(VehTransp) WK_SUPPRESS(VehUV) WK_SUPPRESS(VehEnv)
// Emissive: imza decompile'dan (libapp_so.c 18003) okundu -> my_VehEmis (yukarida) bastirir.

// ------------------------------- GENEL API -------------------------------
static void* Sym(void* lib, const char* mangled, const char* prefix) {
    void* p = (lib && mangled && *mangled) ? dlsym(lib, mangled) : nullptr;
    if (!p && prefix) p = FindElfSymbolByPrefix("libapp.so", prefix);
    if (!p) LOGI("[WALKER] SEMBOL YOK: %s", prefix ? prefix : mangled);
    return p;
}
static void StartProfiler();                                               // asagida tanimli (ornekleyici)
static void Init(void* appLib) {
    fnMeshLoad = (MeshLoad_t)Sym(appLib, "_ZN19IndexedTriangleMesh4loadEPKcP27AndroidHandheldSystemDeviceP24GLESHandheldRenderDevice", "_ZN19IndexedTriangleMesh4loadEPKcP");
    fnTexLoad  = (TexLoad_t)Sym(appLib, "_ZN7Texture4loadEPKcP27AndroidHandheldSystemDeviceP24GLESHandheldRenderDevicebj", "_ZN7Texture4loadEPKcP");
    // DUZELTME: 3 argumanli asiri yukleme (EPKfjj); 4 argumanli olan (EPKfjjj) ile karismasin diye '$' = tam eslesme (FindElfSymbolByPrefix).
    fnDraw = (Draw44_t)Sym(appLib, "_ZN24GLESHandheldRenderDevice24enqueueDrawColumnMajor44EPKfjj", "_ZN24GLESHandheldRenderDevice24enqueueDrawColumnMajor44EPKfjj$");
    fnBind = (BindTex_t)Sym(appLib, "_ZN24GLESHandheldRenderDevice16bindTextureLayerEjj", "_ZN24GLESHandheldRenderDevice16bindTextureLayerE");
    fnBindAttrs = (BindAttrs_t)Sym(appLib, "", "_ZN24GLESHandheldRenderDevice20bindShaderAttributesE");
    fnActiveChanged = (ActiveChanged_t)Sym(appLib, "_ZN4Game20activeVehicleChangedEv", "_ZN4Game20activeVehicleChanged");
    // Vehicle::update'i KANCALAMIYORUZ: sizin my_Vehicle_update tutuyor -> WK::AfterVehicleUpdate oradan cagrilir.
    struct H { const char* n; const char* prefix; void* repl; void** orig; };
    const H hooks[] = {
        {"Vehicle::enqueueRender",            "_ZN7Vehicle13enqueueRenderE",            (void*)my_VehRender, (void**)&orig_VehRender},
        // DUZELTME: Itanium isim uzunluklari: enqueueRenderTransparent=24, enqueueRenderUVScroll=21 (eskiden 22 ve 19 yazilmisti -> sembol bulunamiyor, kanca takilmiyordu)
        {"Vehicle::enqueueRenderTransparent", "_ZN7Vehicle24enqueueRenderTransparentE", (void*)my_VehTransp, (void**)&orig_VehTransp},
        {"Vehicle::enqueueRenderUVScroll",    "_ZN7Vehicle21enqueueRenderUVScrollE",    (void*)my_VehUV,     (void**)&orig_VehUV},
        {"Vehicle::enqueueRenderEnvMap",      "_ZN7Vehicle19enqueueRenderEnvMapE",      (void*)my_VehEnv,    (void**)&orig_VehEnv},
        {"Vehicle::enqueueRenderEmissive",    "_ZN7Vehicle21enqueueRenderEmissiveE",    (void*)my_VehEmis,   (void**)&orig_VehEmis},
        // v7: birinci sahis gorus kesme duzeltmesi (RenderArgs::update tek yerden, Game::updateStateBase'ten cagriliyor)
        {"RenderArgs::update",                "_ZN10RenderArgs6updateE",                (void*)my_RAUpdate,  (void**)&orig_RAUpdate},
    };
    StartProfiler();
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); ++i) {
        void* fn = Sym(appLib, "", hooks[i].prefix);
        const bool ok = fn && HookChecked(hooks[i].n, fn, hooks[i].repl, hooks[i].orig, true);   // riskli prologue'da TAKILMAZ [K]
        LOGI("[WALKER] hook %s = %d", hooks[i].n, (int)ok);
    }
    { static bool s_wd = false; if (!s_wd) { s_wd = true; std::thread(WatchdogThread).detach(); LOGI("[WALKER] watchdog basladi"); } }
}
// Oyun thread'i: her karede (my_GameUpdate'ten)
// ------------------------------- KURTARMA + ORNEKLEYICI (v4) -------------------------------
// Yayayi, "aractan inme" kosulu aramadan kaldirir (yavas kare korumasi): once hedef araca gec, sonra yayayi sil.
static void HardRemoveLocalWalker(uintptr_t game) {
    const uintptr_t w = g_LocalWalker; if (!w) return;
    uintptr_t best = (g_LastVehicle && SlotIndexOfVehicle(game, g_LastVehicle) >= 0) ? g_LastVehicle : 0;
    for (uint32_t i = 0; !best && i < VehicleCount(game); ++i) { const uintptr_t v = RawVehicleSlot(game, i); if (v && !IsWalker(v)) best = v; }
    if (best) SetActiveByPtr(game, best);
    const int wi = SlotIndexOfVehicle(game, w);
    if (wi >= 0) { my_Game_removeVehicle((void*)game, (uint32_t)wi); if (g_VehicleDestroyFn) g_VehicleDestroyFn((void*)w); }
    OnVehicleRemoved(w);
    if (best) SetActiveByPtr(game, best);
    LOGI("[WALKER] yaya kaldirildi (hedef arac=%p)", (void*)best);
}

// Oyun thread'i takilinca (kare > 1.5 s, yaya varken) 30 kez PC/LR ornekler, en sik 6 konumu loglar:
//   "libapp.so+0xOFFSET <mangled isim>" -> hangi oyun fonksiyonunun CPU yedigi gorunur.
#if WK_PROFILER && ((defined(__ANDROID__) && defined(__arm__)) || defined(WK_FORCE_PROFILER))
static const int WK_SMP_MAX = 64;
static std::atomic<uintptr_t> g_SmpPc[WK_SMP_MAX], g_SmpLr[WK_SMP_MAX];
static std::atomic<int> g_SmpN(0);
static void SmpHandler(int, siginfo_t*, void* ucv) {                          // oyun thread'inde calisir: sadece atomik yazma
    ucontext_t* uc = (ucontext_t*)ucv; const int i = g_SmpN.fetch_add(1);
    if (i < WK_SMP_MAX) { g_SmpPc[i].store((uintptr_t)uc->uc_mcontext.arm_pc); g_SmpLr[i].store((uintptr_t)uc->uc_mcontext.arm_lr); }
}
static std::string SymOf(uintptr_t pc) {
    Dl_info di; char buf[240];
    if (pc && dladdr((void*)pc, &di)) {
        const char* fn = di.dli_fname ? strrchr(di.dli_fname, '/') : nullptr; fn = fn ? fn + 1 : (di.dli_fname ? di.dli_fname : "?");
        snprintf(buf, sizeof buf, "%s+0x%lx %s", fn, (unsigned long)(pc - (uintptr_t)di.dli_fbase), di.dli_sname ? di.dli_sname : "?");
    } else snprintf(buf, sizeof buf, "0x%lx ?", (unsigned long)pc);
    return std::string(buf);
}
static bool SmpCmp(const std::pair<int, std::string>& a, const std::pair<int, std::string>& b) { return a.first > b.first; }
static void LogTop(const char* tag, const std::map<std::string, int>& h) {
    std::vector<std::pair<int, std::string> > v;
    for (std::map<std::string, int>::const_iterator it = h.begin(); it != h.end(); ++it) v.push_back(std::make_pair(it->second, it->first));
    std::sort(v.begin(), v.end(), SmpCmp);
    for (size_t i = 0; i < v.size() && i < 6; ++i) LOGI("[PROF] %s %2dx %s", tag, v[i].first, v[i].second.c_str());
}
static void ProfilerThread() {
    int reports = 0;
    for (;;) {
        usleep(250000);
        const long long last = g_LastFrameMs.load(); const int tid = g_GameTid.load();
        if (!last || !tid || reports >= 6 || !g_LocalWalker) continue;
        if (NowMs() - last < 1500) continue;                                     // oyun akici: ornekleme yok
        g_SmpN.store(0);
        for (int k = 0; k < 30; ++k) { syscall(__NR_tgkill, getpid(), tid, SIGRTMIN + 4); usleep(100000); }
        const int n = std::min((int)g_SmpN.load(), WK_SMP_MAX);
        std::map<std::string, int> hp, hl;
        for (int i = 0; i < n; ++i) { ++hp[SymOf(g_SmpPc[i].load())]; ++hl[SymOf(g_SmpLr[i].load())]; }
        LOGI("[PROF] === rapor %d: %d ornek, oyun thread tid=%d ===", reports + 1, n, tid);
        LogTop("PC", hp); LogTop("LR", hl);
        ++reports;
    }
}
static void StartProfiler() {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = SmpHandler; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigemptyset(&sa.sa_mask);
    sigaction(SIGRTMIN + 4, &sa, nullptr);
    std::thread(ProfilerThread).detach();
    LOGI("[PROF] ornekleyici hazir");
}
#else
static void StartProfiler() {}
#endif

// v7: oyuncu oyunun kendi arac degistirme dugmesiyle (veya baska yolla) bir ARACA gectiyse yaya geride kalmasin: yayayi sil (REMOVE yayinlanir),
// aktif araci degistirme. Tekrar "Exit Vehicle" yeni bir yaya olusturur.
static void RemoveWalkerKeepActive(uintptr_t game, uintptr_t w) {
    const uintptr_t act = LocalActiveVehiclePtr(game);
    const int wi = SlotIndexOfVehicle(game, w);
    if (wi >= 0) {
        my_Game_removeVehicle((void*)game, (uint32_t)wi);                      // REMOVE olayini yayinlar, sonra orijinali cagirir
        if (g_VehicleDestroyFn) g_VehicleDestroyFn((void*)w);
    }
    OnVehicleRemoved(w);
    if (act && act != w) { const int ai = SlotIndexOfVehicle(game, act); if (ai >= 0) *(uint32_t*)(game + G_ACTIVE) = (uint32_t)ai; }   // dizi kaydi: indeksi yeniden hesapla
    LOGI("[WALKER] araca binildi -> yaya silindi (aktif arac=%p)", (void*)act);
}

static void OnGameFrame(uintptr_t game) {
    ++g_TickGame; StageG("OnGameFrame");
    const long long nowF = NowMs(); const long long prevF = g_LastFrameMs.exchange(nowF);
    g_GameTid.store((int)syscall(__NR_gettid));
    const long long gap = prevF ? nowF - prevF : 0;
    {   // silinen/satilan yayalari temizle (uzak REMOVE orig'i dogrudan cagirir; havuz nesnesi yeniden kullanilabilir)
        std::lock_guard<std::mutex> lk(g_Mu);
        for (size_t i = g_Walkers.size(); i-- > 0;) if (SlotIndexOfVehicle(game, g_Walkers[i].p) < 0) {
            if (g_LocalWalker == g_Walkers[i].p) g_LocalWalker = 0;
            g_Walkers.erase(g_Walkers.begin() + i);
        }
    }
    if (g_PostSpawnFrames > 0 && g_LocalWalker) {                              // dogustan sonraki kareler: sure/konum/hiz/NaN izi
        --g_PostSpawnFrames; float x = 0, y = 0, a = 0, vx = 0, vy = 0;
        if (BodyPose(g_LocalWalker, &x, &y, &a)) { const uintptr_t b = *(uintptr_t*)(g_LocalWalker + V_PHYS_BODY); vx = *(float*)(b + B_VX); vy = *(float*)(b + B_VY); }
        LOGI("[WALKER] kare+%d gap=%lld ms pos=(%.2f,%.2f) aci=%.3f vel=(%.2f,%.2f) sonlu=%d aktif=%u sayi=%u", 20 - g_PostSpawnFrames, gap, x, y, a, vx, vy,
             (int)(x == x && y == y && a == a && vx == vx && vy == vy), *(uint32_t*)(game + G_ACTIVE), VehicleCount(game));
    }
    if (WK_AUTORECOVER && g_LocalWalker && gap > WK_SLOW_FRAME_MS) {          // yaya varken bir kare cok uzun surdu
        LOGI("[WALKER] YAVAS KARE: %lld ms (esik %d) -> yaya kaldiriliyor, bu oturumda kapali", gap, (int)WK_SLOW_FRAME_MS);
        g_Disabled = true; HardRemoveLocalWalker(game);
    }
    {   // v7: yaya varken aktif arac yaya degilse (arac degistirme dugmesi vb.) yayayi kaldir
        static int s_notActive = 0;
        if (g_LocalWalker) {
            const uintptr_t act = LocalActiveVehiclePtr(game);
            if (act && act != g_LocalWalker) { if (++s_notActive >= 3) { s_notActive = 0; RemoveWalkerKeepActive(game, g_LocalWalker); } }
            else s_notActive = 0;
        } else s_notActive = 0;
    }
    if (g_ReqExit)  { g_ReqExit = false;  DoExit(game); }
    if (g_ReqEnter) { g_ReqEnter = false; DoEnter(game, false); }
}
// Kayit oncesi/oturum bitisinde cagirin: yayayi kaldirip aracina geri bindirir (yoksa Vehicle::serialize yayayi
// "temel arac" olarak kayda yazar). Su an HICBIR YERDEN CAGRILMIYOR [!]: kayit kancasina baglamak sizin kararinizdir.
static void ForceReturnToVehicle(uintptr_t game) { DoEnter(game, true); }
// Dokunma surukleme (ornegin ekranin sag yarisi): kafayi cevirir. Sizin dokunma kodunuzdan cagrilir.
static void AddLook(float dYaw, float dPitch) {
    g_HeadYaw += dYaw; g_HeadPitch += dPitch;
    if (g_HeadPitch > 1.4f)  g_HeadPitch = 1.4f;
    if (g_HeadPitch < -1.4f) g_HeadPitch = -1.4f;
}
// GL thread'i (ImGui kare fonksiyonundan). Etiketler Ingilizce: oyunun yazi tipi Turkce karakter icermeyebilir.
static void DrawImGui(uintptr_t game, const ImVec2& screen) {
    if (game == 0) return;
    if (*(volatile uint32_t*)(game + G_STATE) != 6) return;                    // sadece oyun icinde (magaza/ayarlar degil)
    EnsureAssets(game);                                                        // oyun hazir olunca (Game+0xA340 shader no, EntityManager kurulu) yukle
    const uintptr_t cur = LocalActiveVehiclePtr(game); if (!cur) return;
    const bool onFoot = (cur == g_LocalWalker);
    // Yaya kontrollerini üst-sağ bölgede tut: sağdaki gaz/fren sürgüsünü ve alt HUD düğmelerini kapatmasın.
    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.985f, screen.y * 0.13f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::Begin("##walker", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    const ImVec2 bs(screen.x * 0.15f, screen.y * 0.085f);
    if (g_Disabled) ImGui::TextUnformatted("Walker off (slow frame)");
    else if (!onFoot) { if (!IsWalker(cur) && ImGui::Button("Exit Vehicle", bs)) g_ReqExit = true; }
    else {
        if (ImGui::Button("Enter Vehicle", bs)) g_ReqEnter = true;
        if (ImGui::Button(g_FirstPerson ? "Third Person" : "First Person", bs)) g_FirstPerson = !g_FirstPerson;
    }
    ImGui::End();
    // TODO: dokunusun alttaki oyun HUD'una da gecmesini engelleyin (sohbet HUD dugmenizdeki "press armed" mantigi gibi) [!]
}

} // namespace WK

// Gerçek oyun içi takma işlemlerini de engelle: yaya teknik olarak Vehicle sınıfından türetilmiş bir nesne.
// Dönüş değerleri oyun kodunda kullanılmadığından attachTool koruması void imzasını korur.
static Vehicle_attachTrailer_t g_OrigAttachToolHook = nullptr;
static Vehicle_attachTrailer_t g_OrigAttachTrailerHook = nullptr;
static void my_VehicleAttachToolGuard(void* vehicle, void* tool, void* world) {
    if (WK::IsWalker((uintptr_t)vehicle)) {
        LOGI("[WALKER] attachTool engellendi: yaya araci alet kabul etmez");
        return;
    }
    if (g_OrigAttachToolHook) g_OrigAttachToolHook(vehicle, tool, world);
}
static void my_VehicleAttachTrailerGuard(void* vehicle, void* trailer, void* world) {
    if (WK::IsWalker((uintptr_t)vehicle)) {
        LOGI("[WALKER] attachTrailer engellendi: yaya araci romork kabul etmez");
        return;
    }
    if (g_OrigAttachTrailerHook) g_OrigAttachTrailerHook(vehicle, trailer, world);
}

// TANI + CANLI LOG yardimcilari WK icinde tanimli; dis kodun (my_GameUpdate, my_Vehicle_update, DrawImGui, ModMain) kullanabilmesi icin disari aciliyor
namespace LL = WK::LL;
using WK::StageG;
using WK::StageR;
using WK::TickRender;

// ===== walker_module.inc sonu =====

static void QueueTrailerPosition(uint32_t idx, uint32_t type, float x, float y, float angle, bool moving, bool tool = false) {
    if (!g_IsConnected.load() || idx >= (tool ? 64u : 256u) || type > 0x3F) return;
    const uint8_t ownerId = g_LocalPlayerId.load();
    if (ownerId == VEHICLE_OWNER_NONE || ownerId >= MAX_PLAYERS) return;
    VehiclePositionPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = PACKET_VEHICLE_POSITION;
    pkt.ownerId = ownerId;
    pkt.vehicleId = (uint16_t)(TRAILER_WIRE_FLAG | (tool ? TOOL_WIRE_FLAG : 0) | (type << 8) | idx);
    pkt.x = x; pkt.y = y; pkt.angle = angle;
    pkt.sequence = (uint32_t)GetMonotonicMilliseconds();
    pkt.moving = moving ? 1 : 0;
    const uint16_t slot = (uint16_t)((tool ? TOOL_ID_BASE : TRAILER_ID_BASE) + idx);
    std::lock_guard<std::mutex> lock(g_VehicleSendMutex);
    g_LatestOutgoingVehicles[slot] = pkt;
    g_HasLatestOutgoingVehicle[slot] = true;
}

static void QueueEntityState(uint8_t kind, uint16_t id, uint8_t sub, uint32_t wa, uint32_t wb, uint8_t flags) {
    if (!g_IsConnected.load()) return;
    EntityStatePacket p;
    memset(&p, 0, sizeof(p));
    p.type = PACKET_ENTITY_STATE;
    p.ownerId = g_LocalPlayerId.load();
    p.kind = kind; p.id = id; p.sub = sub; p.wa = wa; p.wb = wb; p.flags = flags;
    QueueVehicleEvent(&p, sizeof(p));
}

static std::atomic<long long> g_LastLocalFieldOpMs(0);    // bu cihazda en son yerel tarla islemi (hasat/ekim/isleme...)
static std::atomic<long long> g_LastRemoteFieldOpMs(0);   // en son uzaktan gelen tarla isleminin uygulandigi an
static void QueueFieldOp(uint32_t wtype, uint32_t wfruit, const uint32_t* pts) {
    if (!g_IsConnected.load()) return;
    g_LastLocalFieldOpMs.store(NowMs());
    FieldOpPacket k;
    memset(&k, 0, sizeof(k));
    k.type = PACKET_FIELD_OP;
    k.ownerId = g_LocalPlayerId.load();
    k.wtype = wtype; k.wfruit = wfruit;
    memcpy(k.p, pts, sizeof(k.p));
    std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
    if (g_FieldOpOut.size() > 6000) return;               // tampon doldu: bu islemi atla (arac olaylarini silme)
    const uint8_t* b = (const uint8_t*)&k;
    g_FieldOpOut.insert(g_FieldOpOut.end(), b, b + sizeof(k));
}

static void QueuePoolOp(uint8_t kind, uint8_t op, uint16_t itemType, uint8_t idx, float x, float y, float a) {
    if (!g_IsConnected.load()) return;
    PoolOpPacket k;
    memset(&k, 0, sizeof(k));
    k.type = PACKET_POOL_OP;
    k.ownerId = g_LocalPlayerId.load();
    k.kind = kind; k.op = op; k.itemType = itemType; k.idx = idx; k.x = x; k.y = y; k.a = a;
    QueueVehicleEvent(&k, sizeof(k));
}

static float WordToFloat(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }

static TrailerTrack* TrackForTrailer(uintptr_t tp, bool* fresh) {
    *fresh = false;
    TrailerTrack* freeSlot = nullptr;
    for (size_t k = 0; k < 64; ++k) {
        if (g_TrTrack[k].tp == tp) return &g_TrTrack[k];
        if (g_TrTrack[k].tp == 0 && !freeSlot) freeSlot = &g_TrTrack[k];
    }
    if (!freeSlot) return nullptr;
    memset(freeSlot, 0, sizeof(*freeSlot));
    freeSlot->tp = tp;
    *fresh = true;
    return freeSlot;
}

// Her ornekte (oyun thread'i): romorkun takildi/cikarildigini ve doluluk degisimini yakalar.
// Olay yalnizca BU cihazda olduysa gonderilir; uzaktan uygulanan degisiklik kayda islenir (yankilanmaz).
static void TrackTrailerAttachAndFill(uintptr_t game, uint64_t nowMs) {
    const uint32_t n = TrailerCount(game);
    const bool live = g_VehNetArmed.load() && g_IsConnected.load();
    const bool resend = g_EntityResendAll.load();
    // Artik dizide olmayan romork kayitlarini sil (satildi / yer degisti).
    for (size_t k = 0; k < 64; ++k) {
        if (g_TrTrack[k].tp == 0) continue;
        bool present = false;
        for (uint32_t i = 0; i < n; ++i) if (RawTrailerSlot(game, i) == g_TrTrack[k].tp) { present = true; break; }
        if (!present) memset(&g_TrTrack[k], 0, sizeof(g_TrTrack[k]));
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uintptr_t tp = RawTrailerSlot(game, i);
        if (tp == 0) continue;
        bool fresh = false;
        TrailerTrack* t = TrackForTrailer(tp, &fresh);
        if (!t) continue;
        uintptr_t av = *(uintptr_t*)(tp + 0x184);
        if (av != 0 && SlotIndexOfVehicle(game, av) < 0) av = 0;           // gecersiz adres: bagli sayma
        const uint32_t levelWord = *(uint32_t*)(tp + 0x388);
        const uint32_t typeWord = *(uint32_t*)(tp + 0x390);
        const uint32_t type = *(uint32_t*)(tp + 0x10);
        const uintptr_t body = *(uintptr_t*)(tp + 0x180);
        if (fresh) { t->av = av; t->levelWord = levelWord; t->typeWord = typeWord; t->lastMs = nowMs; continue; }

        // ---- takma / cikarma ----
        if (live && body != 0 && type <= 0x3F && (av != t->av || (resend && av != 0))) {
            TrailerAttachPacket pk;
            memset(&pk, 0, sizeof(pk));
            pk.type = PACKET_TRAILER_ATTACH;
            pk.ownerId = g_LocalPlayerId.load();
            pk.trIdx = (uint8_t)i; pk.trType = (uint8_t)type;
            pk.tx = *(float*)(body + 0x0C); pk.ty = *(float*)(body + 0x10); pk.ta = *(float*)(body + 0x40);
            if (av != t->av && t->av != 0) {                                // eski araca bagliydi: once cikar
                const int oldId = NetIdOfVehicle(t->av);
                if (oldId >= 0) {
                    pk.attach = 4; pk.vehNetId = (uint16_t)oldId;      // bit2: cikarma anindaki konumlar pakette (karsi cihaz romorku dogru yere koysun)
                    const uintptr_t ovb = *(uintptr_t*)(t->av + 0x528);
                    if (ovb != 0) { pk.vx = *(float*)(ovb + 0x0C); pk.vy = *(float*)(ovb + 0x10); pk.va = *(float*)(ovb + 0x40); }
                    else pk.attach = 0;
                    QueueVehicleEvent(&pk, sizeof(pk));
                    LOGI("[TRAILER] cikarildi: romork=%u arac=%d", (unsigned)i, oldId);
                }
            }
            if (av != 0) {
                const int newId = NetIdOfVehicle(av);
                const uintptr_t vb = *(uintptr_t*)(av + 0x528);
                if (newId >= 0 && vb != 0) {
                    pk.attach = 1; pk.vehNetId = (uint16_t)newId;
                    pk.vx = *(float*)(vb + 0x0C); pk.vy = *(float*)(vb + 0x10); pk.va = *(float*)(vb + 0x40);
                    QueueVehicleEvent(&pk, sizeof(pk));
                    LOGI("[TRAILER] takildi: romork=%u arac=%d", (unsigned)i, newId);
                }
            }
        }
        t->av = av;

        // ---- doluluk ----
        const float level = WordToFloat(levelWord);
        if (!(level == level) || level < -1.0f || level > 1.0e7f) continue;        // NaN / sacma deger
        if (live && type <= 0x3F && (nowMs - t->lastMs >= 250 || resend) &&
            (levelWord != t->levelWord || typeWord != t->typeWord || resend)) {
            if (resend || fabsf(level - WordToFloat(t->levelWord)) > 0.05f || typeWord != t->typeWord) {
                QueueEntityState(1, (uint16_t)i, (uint8_t)type, levelWord, typeWord, 0);
                t->levelWord = levelWord; t->typeWord = typeWord; t->lastMs = nowMs;
            }
        }
    }
}


static TrailerTrack* TrackForTool(uintptr_t tp, bool* fresh) {
    *fresh = false;
    TrailerTrack* freeSlot = nullptr;
    for (size_t k = 0; k < 64; ++k) {
        if (g_ToolTrack[k].tp == tp) return &g_ToolTrack[k];
        if (g_ToolTrack[k].tp == 0 && !freeSlot) freeSlot = &g_ToolTrack[k];
    }
    if (!freeSlot) return nullptr;
    memset(freeSlot, 0, sizeof(*freeSlot));
    freeSlot->tp = tp;
    *fresh = true;
    return freeSlot;
}

static bool ToolHasFill(uintptr_t tp) {
    const uint32_t cat = *(uint32_t*)(tp + 0x434);
    return cat == 1 || cat == 2 || cat == 5 || *(uint8_t*)(tp + 0x448) != 0;
}

// Aletin (pulluk, mibzer...) takildi/cikarildi olayi + doluluk (tohum vb., Tool+0x45c) takibi.
// Doluluk yalnizca alet bu cihazda surulen/sahip olunan araca bagliyken ya da hic bagli degilken yayinlanir.
static void TrackToolAttachAndFill(uintptr_t game, uint8_t localOwner, uint64_t nowMs, uint16_t activeVehicleId) {
    const uint32_t n = ToolCount(game);
    const bool live = g_VehNetArmed.load() && g_IsConnected.load();
    const bool resend = g_EntityResendAll.load();
    for (size_t k = 0; k < 64; ++k) {
        if (g_ToolTrack[k].tp == 0) continue;
        bool present = false;
        for (uint32_t i = 0; i < n; ++i) if (RawToolSlot(game, i) == g_ToolTrack[k].tp) { present = true; break; }
        if (!present) memset(&g_ToolTrack[k], 0, sizeof(g_ToolTrack[k]));
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uintptr_t tp = RawToolSlot(game, i);
        if (tp == 0) continue;
        bool fresh = false;
        TrailerTrack* t = TrackForTool(tp, &fresh);
        if (!t) continue;
        uintptr_t av = *(uintptr_t*)(tp + 0x23C);
        if (av != 0 && SlotIndexOfVehicle(game, av) < 0) av = 0;
        const uint32_t fillWord = *(uint32_t*)(tp + 0x45C);
        const uint32_t cat = *(uint32_t*)(tp + 0x434);
        const uint32_t typeWord = (cat == 1) ? *(uint32_t*)(tp + 0x438) : 0;
        const uint32_t type = *(uint32_t*)(tp + 0x10);
        const uintptr_t body = *(uintptr_t*)(tp + 0x228);
        if (fresh) { t->av = av; t->levelWord = fillWord; t->typeWord = typeWord; t->lastMs = nowMs; continue; }

        if (live && body != 0 && type <= 0x3F && (av != t->av || (resend && av != 0))) {
            TrailerAttachPacket pk;
            memset(&pk, 0, sizeof(pk));
            pk.type = PACKET_TRAILER_ATTACH;
            pk.ownerId = g_LocalPlayerId.load();
            pk.trIdx = (uint8_t)i; pk.trType = (uint8_t)type;
            pk.tx = *(float*)(body + 0x0C); pk.ty = *(float*)(body + 0x10); pk.ta = *(float*)(body + 0x40);
            if (av != t->av && t->av != 0) {
                const int oldId = NetIdOfVehicle(t->av);
                if (oldId >= 0) {
                    pk.attach = 6; pk.vehNetId = (uint16_t)oldId;      // 2 = alet, 4 = konumlar pakette
                    const uintptr_t ovb = *(uintptr_t*)(t->av + 0x528);
                    if (ovb != 0) { pk.vx = *(float*)(ovb + 0x0C); pk.vy = *(float*)(ovb + 0x10); pk.va = *(float*)(ovb + 0x40); }
                    else pk.attach = 2;
                    QueueVehicleEvent(&pk, sizeof(pk));
                    LOGI("[TOOL] cikarildi: alet=%u arac=%d", (unsigned)i, oldId);
                }
            }
            if (av != 0) {
                const int newId = NetIdOfVehicle(av);
                const uintptr_t vb = *(uintptr_t*)(av + 0x528);
                if (newId >= 0 && vb != 0) {
                    pk.attach = 3; pk.vehNetId = (uint16_t)newId;
                    pk.vx = *(float*)(vb + 0x0C); pk.vy = *(float*)(vb + 0x10); pk.va = *(float*)(vb + 0x40);
                    QueueVehicleEvent(&pk, sizeof(pk));
                    g_CbTrack[newId].valid = false;                 // alet aktif bitini yeniden yolla
                    LOGI("[TOOL] takildi: alet=%u arac=%d", (unsigned)i, newId);
                }
            }
        }
        t->av = av;

        if (!ToolHasFill(tp)) continue;
        const float fill = WordToFloat(fillWord);
        if (!(fill == fill) || fill < -1.0f || fill > 1.0e7f) continue;
        bool mine = (av == 0);
        if (!mine) {
            const int nid = NetIdOfVehicle(av);
            if (nid >= 0) {
                if ((uint16_t)nid == activeVehicleId) mine = true;
                else { std::lock_guard<std::mutex> lock(g_VehicleStateMutex); mine = (g_VehicleAuthority[nid].ownerId == localOwner); }
            }
        }
        if (live && mine && type <= 0x3F && (nowMs - t->lastMs >= 250 || resend) &&
            (fillWord != t->levelWord || typeWord != t->typeWord || resend)) {
            if (resend || fabsf(fill - WordToFloat(t->levelWord)) > 0.05f || typeWord != t->typeWord) {
                QueueEntityState(4, (uint16_t)i, (uint8_t)type, fillWord, typeWord, 0);
                t->levelWord = fillWord; t->typeWord = typeWord; t->lastMs = nowMs;
            }
        }
    }
}

// Uzaktan CIKARMA uygulandiktan sonra:
//  1) Oyunun kendi "yeniden takma bekleme" sayaci (Game+0xA420 = -10): oyun, kullanici cikardiktan sonra ~10 sn otomatik
//     takmayi kapatir (handleIngameGuiEvent). Uzaktan cikarmada bu sayac kurulmadigi icin, ayni araca bakan cihaz hemen
//     "yakindaki romork/alet" diye tekrar takip karsi cihaza geri yayiyordu (cikarilmayan romork/pulluk).
//  2) Gonderenin cikarma anindaki konumu (araca gore) bu cihazdaki araca gore uygulanir: alet, takma olayi hic uygulanmamis
//     bile olsa ilk konumunda kalmaz (pulluk isinlanmasi).
//  3) Bu nesnenin eski uzak akis kaydi / yayin sahipligi / sensor durumu sifirlanir (eski konuma geri cekme olmasin).
static void AfterRemoteDetach(uintptr_t game, const TrailerAttachPacket& pk, uintptr_t tbody, uintptr_t vbody, bool tool) {
    if (game != 0) *(float*)(game + 0xA420) = -10.0f;
    const uint32_t idx = pk.trIdx;
    const bool pv = (pk.attach & 4) != 0 && tbody != 0 && vbody != 0 && g_b2BodySetTransform &&
                    pk.tx == pk.tx && pk.ty == pk.ty && pk.ta == pk.ta && pk.vx == pk.vx && pk.vy == pk.vy && pk.va == pk.va &&
                    fabsf(pk.tx) < 1.0e5f && fabsf(pk.ty) < 1.0e5f && fabsf(pk.vx) < 1.0e5f && fabsf(pk.vy) < 1.0e5f;
    if (pv) {
        const float c0 = cosf(pk.va), s0 = sinf(pk.va);
        const float dx = pk.tx - pk.vx, dy = pk.ty - pk.vy;
        const float lx = c0 * dx + s0 * dy, ly = -s0 * dx + c0 * dy;
        const float va2 = *(float*)(vbody + 0x40);
        const float c1 = cosf(va2), s1 = sinf(va2);
        TestB2Vec2 pos;
        pos.x = *(float*)(vbody + 0x0C) + c1 * lx - s1 * ly;
        pos.y = *(float*)(vbody + 0x10) + s1 * lx + c1 * ly;
        g_b2BodySetTransform((void*)tbody, &pos, va2 + (pk.ta - pk.va));
        SetRemoteBodyVelocity(tbody, 0.0f, 0.0f, 0.0f);
    }
    if (idx < (tool ? 64u : 256u)) {
        const int ti = tool ? 1 : 0;
        if (tool) g_ToolMine[idx] = false; else g_TrailerMine[idx] = false;
        if (g_AttSensorBody[ti][idx] != 0) {
            if (g_AttSensorBody[ti][idx] == tbody) B2BodyRestoreSensor(tbody, g_AttSensorMask[ti][idx]);
            g_AttSensorBody[ti][idx] = 0; g_AttSensorMask[ti][idx] = 0;
        }
        const uint16_t sid = (uint16_t)((tool ? TOOL_ID_BASE : TRAILER_ID_BASE) + idx);
        std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
        memset(&g_RemoteVehicles[sid], 0, sizeof(g_RemoteVehicles[sid]));
        memset(&g_LocalSamples[sid], 0, sizeof(g_LocalSamples[sid]));
        memset(&g_LastSentLocalVehicles[sid], 0, sizeof(g_LastSentLocalVehicles[sid]));
        g_LocalStationarySince[sid] = 0;
        g_RemoteStationarySince[sid] = 0;
    }
}

// Uzaktan gelen alet takma/cikarma (Vehicle::attachTool / detachTool sadece CAGRILIR).
static void ApplyToolAttach(uintptr_t game, const TrailerAttachPacket& pk, void* world) {
    const uint32_t tn = ToolCount(game);
    if (pk.trIdx >= tn || world == nullptr) return;
    const uintptr_t tp = RawToolSlot(game, pk.trIdx);
    const uintptr_t veh = GetVehicleFromIndex(game, pk.vehNetId);
    if ((pk.attach & 1) != 0 && WK::IsWalker(veh)) {
        LOGI("[WALKER] uzaktan alet takma engellendi: yaya aracina alet baglanamaz");
        return;
    }
    if (tp == 0 || veh == 0 || *(uint32_t*)(tp + 0x10) != pk.trType) {
        LOGI("[TOOL] olay atlandi: alet=%u arac=%u (esleme yok)", (unsigned)pk.trIdx, (unsigned)pk.vehNetId);
        return;
    }
    const uintptr_t tbody = *(uintptr_t*)(tp + 0x228);
    const uintptr_t vbody = *(uintptr_t*)(veh + 0x528);
    if (tbody == 0 || vbody == 0 || !g_VehicleDetachToolFn) return;
    const uintptr_t cur = *(uintptr_t*)(tp + 0x23C);
    if ((pk.attach & 1) == 0) {
        if (cur == veh) g_VehicleDetachToolFn((void*)veh, world);
        AfterRemoteDetach(game, pk, tbody, vbody, true);
        LOGI("[TOOL] uzaktan cikarma uygulandi: alet=%u (bagliydi=%d)", (unsigned)pk.trIdx, (int)(cur == veh));
        return;
    }
    if (cur == veh) return;
    if (!g_VehicleAttachToolFn || *(uint32_t*)(veh + 0x730) != 0) {
        LOGI("[TOOL] takma atlandi: isci acik / fonksiyon yok");
        return;
    }
    if (cur != 0 && SlotIndexOfVehicle(game, cur) >= 0) g_VehicleDetachToolFn((void*)cur, world);
    if (*(uintptr_t*)(veh + 0x560) != 0 || *(uintptr_t*)(veh + 0x55C) != 0) {       // ortak eklem: once eskisini ayir
        if (*(uintptr_t*)(veh + 0x560) != 0) g_VehicleDetachToolFn((void*)veh, world);
        if (*(uintptr_t*)(veh + 0x55C) != 0 && g_VehicleDetachTrailerFn) g_VehicleDetachTrailerFn((void*)veh, world);
    }
    const float c0 = cosf(pk.va), s0 = sinf(pk.va);
    const float dx = pk.tx - pk.vx, dy = pk.ty - pk.vy;
    const float lx = c0 * dx + s0 * dy, ly = -s0 * dx + c0 * dy;
    const float va2 = *(float*)(vbody + 0x40);
    const float c1 = cosf(va2), s1 = sinf(va2);
    TestB2Vec2 pos;
    pos.x = *(float*)(vbody + 0x0C) + c1 * lx - s1 * ly;
    pos.y = *(float*)(vbody + 0x10) + s1 * lx + c1 * ly;
    g_b2BodySetTransform((void*)tbody, &pos, va2 + (pk.ta - pk.va));
    SetRemoteBodyVelocity(tbody, 0.0f, 0.0f, 0.0f);
    g_VehicleAttachToolFn((void*)veh, (void*)tp, world);
    if (pk.vehNetId < VEHICLE_SLOT_LIMIT) g_CbTrack[pk.vehNetId].rvalid = false;     // sonraki durum paketi tam uygulansin
    LOGI("[TOOL] uzaktan takma uygulandi: alet=%u arac=%u", (unsigned)pk.trIdx, (unsigned)pk.vehNetId);
}

static uint8_t LocalVehicleFlags(uintptr_t v) {
    uint8_t f = 0;
    if (*(uint8_t*)(v + 0x328) && *(uint8_t*)(v + 0x498)) f |= 1;            // bicer calisiyor / kesici inik
    const uintptr_t tool = *(uintptr_t*)(v + 0x560);
    if (tool != 0 && *(uintptr_t*)(tool + 0x23C) != 0) {
        f |= 4;                                                               // alet bagli
        if (*(uint8_t*)(tool + 0x230)) f |= 2;                                // alet aktif (indirilmis)
    }
    if (*(uint32_t*)(v + 0x730) != 0) f |= 8;                                 // isci (AI) acik
    return (uint8_t)(f & kStateFlagMask);
}

// Vehicle::toggleCombineIsTurnedOn ile birebir ayni (oyun koduyla): bayragi cevirir, zamanlayiciyi ters cevirir.
static void ToggleCombineInline(uintptr_t v) {
    *(uint8_t*)(v + 0x498) ^= 1;
    *(uint32_t*)(v + 0x14) |= 2;
    const float t = *(float*)(v + 0x43C);
    if (t >= 0.0f) *(float*)(v + 0x43C) = *(float*)(v + 0x32C) - t;
    else *(float*)(v + 0x43C) = 0.0f;
}

// Durum bayraklarinin "periyodik" yayincisi kim? Ayni aracin icinde birden fazla oyuncu varken
// sadece biri periyodik/tank bilgisini gonderir; KULLANICI DEGISIKLIKLERI (kenar) herkesten gider.
// Aracin bagli mibzerinin secili tohumu (Tool+0x438, tur Tool+0x434 == 1; 0..2). 0 = yok, aksi halde tohum + 1.
static uint8_t VehSeedSub(uintptr_t v) {
    const uintptr_t tool = *(uintptr_t*)(v + 0x560);
    if (tool == 0 || *(uint32_t*)(tool + 0x434) != 1) return 0;
    const uint32_t sd = *(uint32_t*)(tool + 0x438);
    return sd < 3 ? (uint8_t)(sd + 1) : (uint8_t)0;
}

static bool IsStatePublisherVeh(uint16_t id, uint8_t localOwner, uint16_t activeVehicleId, bool ai, uint8_t owner, bool remoteAi) {
    if (ai && !g_AiMirrored[id]) return true;
    if (remoteAi) return false;                                    // isciyi baska cihaz calistiriyor: onun durumu gecerli                       // isciyi bu cihaz calistiriyor: gercek kaynak
    if (owner != VEHICLE_OWNER_NONE) return owner == localOwner;
    if (id == activeVehicleId) return !HasLowerIdRemoteActive((int)id, localOwner);
    return false;
}

// Her arac (traktor + biciler): kesici/alet durumu, isci (AI), bicer tanki.
// KURALLAR (yanki / kavga olmasin diye):
//  - Kullanici degisikligi = oyunun giris cihazindaki dugme biti (isci: device+0x28 & 0x1000) ile ayni karede
//    gorulen degisiklik. Bu "kenar" olarak herkesten (aracin icindeki herkes) yayinlanir.
//  - Baska cihazin calistirdigi (aynali) iscinin KENDI simulasyon kararlari (durma, alet kaldirma) yayinlanmaz;
//    gercek durum iscinin calistigi cihazdan gelir. Aynali isci kendi kendine dursa yeniden baslatilir.
//  - Periyodik durum / bicer tanki sadece "yayinci" cihazdan gider.
static void CaptureAndQueueCombineStates(uintptr_t game, uint8_t localOwner, uint64_t nowMs, uint16_t activeVehicleId) {
    if (!g_VehNetArmed.load() || !g_IsConnected.load()) return;
    const bool resend = g_EntityResendAll.load();
    for (uint16_t id = 0; id < VEHICLE_SLOT_LIMIT; ++id) {
        if (g_KnownVehiclePointers[id] == 0) continue;
        const uintptr_t v = GetVehicleFromIndex(game, id);
        if (v == 0) continue;
        CombineTrack& c = g_CbTrack[id];
        bool ai = *(uint32_t*)(v + 0x730) != 0;
        const bool prevOn = g_AiPrevOn[id];
        g_AiPrevOn[id] = ai;
        // Ekrandaki HUD dugmesi cihaz biti (0x1000) DEGIL GUI olayi 4 uretir (Game::handleIngameGuiEvent -> aiHelperEvent);
        // bu yuzden niyet ayrica "aktif aracin isci durumu bu karede degisti" ile de anlasilir (oyun kendi dugme kodunu calistirdi).
        // Uzaktan uygulanan degisiklikler g_AiPrevOn'u kendisi gunceller, o yuzden kullanici sanilmaz.
        if (id == activeVehicleId && ai != prevOn) { g_AiIntentVeh = (int)id; g_AiIntentMs = nowMs; }
        const bool intent = (g_AiIntentVeh == (int)id) && (nowMs - g_AiIntentMs) < 1500;
        const bool remoteAi = c.rvalid && (c.rflags & 8) != 0;       // karsi cihaz "isci acik" diyor

        if (!ai && !g_AiMirrored[id]) g_AiRestartTries[id] = 0;

        // ---- Isci SAHIPSIZ KALDI / SAHIP DEGISTI ----
        // Iscyi calistiran cihaz oyunu durdurursa (ayarlar, arka plan, kopma) araci birakilir (PauseReleaseTick) ve o cihazdan
        // artik "isci acik" bilgisi gelmez. Aynali simulasyonu olan cihazlardan biri (oyuncu numarasina gore kademeli gecikme)
        // iscyi GERCEK isci olarak devralir ve konumu yayinlar; digerleri aracin yeni sahibini gorunce aynali kalir.
        // Ilk cihaz geri donerse ve arac baskasina gectiyse geri cekilir (aynali olur): iki cihaz ayni araci yayinlayip titretmesin.
        if (ai) {
            uint8_t ow = VEHICLE_OWNER_NONE;
            { std::lock_guard<std::mutex> lock(g_VehicleStateMutex); ow = g_VehicleAuthority[id].ownerId; }
            if (g_AiMirrored[id]) {
                if (ow == VEHICLE_OWNER_NONE && g_AiOriginMs[id] != 0 && (nowMs - g_AiOriginMs[id]) > 5000) {
                    if (g_AiOrphanSince[id] == 0) g_AiOrphanSince[id] = nowMs;
                    if (nowMs - g_AiOrphanSince[id] > 700ull * (uint64_t)(localOwner < MAX_PLAYERS ? localOwner : 0)) {
                        g_AiMirrored[id] = false;
                        g_AiOrphanSince[id] = 0;
                        LOGI("[VEHSTATE] sahipsiz isci devralindi netId=%u", (unsigned)id);
                    }
                } else {
                    g_AiOrphanSince[id] = 0;
                }
                g_AiYieldSince[id] = 0;
            } else {
                g_AiOrphanSince[id] = 0;
                if (id != activeVehicleId && ow != VEHICLE_OWNER_NONE && ow != localOwner) {
                    if (g_AiYieldSince[id] == 0) g_AiYieldSince[id] = nowMs;
                    else if (nowMs - g_AiYieldSince[id] > 2000) {
                        g_AiMirrored[id] = true;
                        g_AiOriginMs[id] = nowMs;
                        g_AiYieldSince[id] = 0;
                        LOGI("[VEHSTATE] isci baska cihaza devredildi netId=%u yeni sahip=%u", (unsigned)id, (unsigned)ow);
                    }
                } else {
                    g_AiYieldSince[id] = 0;
                }
            }
        } else {
            g_AiOrphanSince[id] = 0; g_AiYieldSince[id] = 0;
        }

        // ---- Aynali isci (baska cihazin calistirdigi isci) ----
        if (g_AiMirrored[id]) {
            // Herhangi bir cihaz (sahip olmasa bile) iscyi istedigi an durdurabilir: dugmeye basildiysa ve ortak durumda
            // isci aciksa niyet "durdur"dur. Dugme simulasyon kendi kendine durmusken basildiysa oyun iscyi YENIDEN baslatir;
            // onu da geri alip durdurma istegini gonderiyoruz.
            if (ai && intent && remoteAi && g_VehicleAiStopFn) {
                g_VehicleAiStopFn((void*)v);
                ai = *(uint32_t*)(v + 0x730) != 0;
                g_AiPrevOn[id] = ai;
                g_AiMirrored[id] = false;
                g_AiStopHoldMs[id] = nowMs + 3000;
            } else if (!ai) {
                if (intent) {                                         // kullanici bu cihazda durdurdu
                    g_AiMirrored[id] = false;
                    g_AiStopHoldMs[id] = nowMs + 3000;
                } else if (!remoteAi) {
                    g_AiMirrored[id] = false;                         // karsi taraf da durdurmus
                } else if (!intent && g_AiRestartTries[id] < 8 && (nowMs - g_AiRestartMs[id]) >= 1500 &&
                           g_VehiclePrepareAiFn && g_VehicleAiStartFn) {
                    g_AiRestartMs[id] = nowMs; g_AiRestartTries[id]++;     // simulasyon kendi kendine durdu: canli tut
                    const uint32_t ok = g_VehiclePrepareAiFn((void*)v, (void*)game) & 0xFF;
                    if (ok) { g_VehicleAiStartFn((void*)v, (void*)game); ai = *(uint32_t*)(v + 0x730) != 0; g_AiPrevOn[id] = ai; }
                    const uintptr_t tl = *(uintptr_t*)(v + 0x560);
                    LOGI("[VEHSTATE] aynali isci yeniden baslatildi netId=%u sonuc=%u deneme=%u alet=%p aletbayrak=%08x ai=%u",
                         (unsigned)id, (unsigned)ok, (unsigned)g_AiRestartTries[id], (void*)tl,
                         tl ? (unsigned)*(uint32_t*)tl : 0u, (unsigned)*(uint32_t*)(v + 0x730));
                }
            }
            if (g_AiMirrored[id] && !intent) continue;                // aynali isci simulasyonunun kararlari yayinlanmaz
        }

        // Karsi cihaz isciyi calistirirken kullanici bu cihazda dugmeye bastiysa niyet "durdur"dur (dugme tek bir
        // ortak durumu cevirir). Oyun yerelde isciyi baslatmis olabilir (geri al) ya da baslatamamis olabilir;
        // ikisinde de durdurma istegini isciyi calistiran cihaza gonder.
        if (intent && remoteAi && !g_AiMirrored[id] && g_AiIntentMs != 0) {
            if (ai && !prevOn && g_VehicleAiStopFn) {
                g_VehicleAiStopFn((void*)v);
                ai = *(uint32_t*)(v + 0x730) != 0;
                g_AiPrevOn[id] = ai;
            }
            if (!ai) {
                const bool cmb = *(uint8_t*)(v + 0x328) != 0;
                QueueEntityState(2, id, VehSeedSub(v), cmb ? *(uint32_t*)(v + 0x440) : 0, cmb ? *(uint32_t*)(v + 0x448) : 0,
                                 (uint8_t)((LocalVehicleFlags(v) & ~8) | kEdgeBit));
                c.rvalid = true; c.rflags = (uint8_t)(c.rflags & ~8);
                g_AiIntentMs = 0;                                   // niyet tuketildi
                g_AiStopHoldMs[id] = nowMs + 3000;
                LOGI("[VEHSTATE] isci durdurma istegi gonderildi netId=%u", (unsigned)id);
                continue;
            }
        }

        uint8_t owner = VEHICLE_OWNER_NONE;
        { std::lock_guard<std::mutex> lock(g_VehicleStateMutex); owner = g_VehicleAuthority[id].ownerId; }
        // Sahipsiz + kimsenin icinde olmadigi (bosaltma icin duran) bicerin GERCEK kopyasi host'tadir (IsGhostVehiclePtr);
        // tankini host yayinlar ki diger cihazlarda tank/romork ayni bosalsin.
        const bool hostIdleCombine = g_IsHost.load() && owner == VEHICLE_OWNER_NONE && *(uint8_t*)(v + 0x328) != 0 &&
                                     !(ai && g_AiMirrored[id]) && !remoteAi && !IsRemoteActiveVehicle((int)id);
        if (!(id == activeVehicleId || owner == localOwner || (ai && !g_AiMirrored[id]) || hostIdleCombine)) continue;

        const bool combine = *(uint8_t*)(v + 0x328) != 0;
        const uint8_t flags = LocalVehicleFlags(v);
        const uint32_t wa = combine ? *(uint32_t*)(v + 0x440) : 0;
        const uint32_t wb = combine ? *(uint32_t*)(v + 0x448) : 0;
        const bool publisher = hostIdleCombine || IsStatePublisherVeh(id, localOwner, activeVehicleId, ai, owner, remoteAi);
        const bool localEdge = c.valid && flags != c.flags;

        if (localEdge) {                                              // kullanici (ya da isci) bir sey degistirdi
            QueueEntityState(2, id, VehSeedSub(v), wa, wb, (uint8_t)(flags | kEdgeBit));
            LOGI("[VEHSTATE] gonderildi netId=%u bicer/kesici=%d aktif=%d isci=%d", (unsigned)id,
                 (int)(flags & 1), (int)((flags >> 1) & 1), (int)((flags >> 3) & 1));
            c.rvalid = true; c.rflags = flags;                        // ortak durum artik bu
            c.valid = true; c.wa = wa; c.wb = wb; c.flags = flags; c.lastMs = nowMs;
            continue;
        }
        if (!publisher) {
            if (!c.valid) { c.valid = true; c.wa = wa; c.wb = wb; c.flags = flags; c.lastMs = nowMs; }
            continue;
        }
        const bool tankChanged = c.valid && (c.wa != wa || c.wb != wb);
        const bool due = !c.valid || resend || (nowMs - c.lastMs) >= 3000 || (tankChanged && (nowMs - c.lastMs) >= 250);
        if (due) {
            QueueEntityState(2, id, VehSeedSub(v), wa, wb, flags);
            c.valid = true; c.wa = wa; c.wb = wb; c.flags = flags; c.lastMs = nowMs;
        }
    }
}

// ---- Korna: Game+0xa416 = korna tusu basili (her karenin sonunda yerel girdiden dolar), Game+0xa417 = calan ses.
// Yerel korna degisince "kind 3" paketi gider; uzaktan korna varsa bayrak her kare 1'e zorlanir ve oyun kendi sesini calar.
//
// SES KANALI SIZINTISI (decompile'dan): oyun kornayi birakinca sadece AudioSource::pause() cagirir
// (FMOD_Channel_SetPaused). Duraklatilmis kanal serbest kalmaz; her basis bir FMOD kanalini tutar,
// kanallar bitince motor/arac sesleri calinamaz ("korna basinca aracin sesi gelmiyor"). Duraklatmanin
// hemen ardindan AudioSource::stop() (FMOD_ChannelGroup_Stop) cagirip kanali birakiyoruz.
//
// MESAFE: korna tek bir global AudioSource'tur (Game+0xA468, konumsuz); uzaktaki oyuncunun kornasi icin ses
// seviyesini (AudioSource::setVolume) mesafeye gore ayarlariz: 12 m'ye kadar tam, 150 m'de sifir.
typedef void (*AudioStop_t)(void* self);
typedef void (*AudioSetVol_t)(void* self, float vol);
static AudioStop_t g_AudioStopFn = nullptr;           // AudioSource::stop()
static AudioSetVol_t g_AudioSetVolFn = nullptr;       // AudioSource::setVolume(float)
static const uintptr_t kHornSourceOff = 0xA468;       // Game+0xA468 = korna AudioSource
static const float kHornFullDist = 12.0f, kHornMaxDist = 150.0f;

// 0 = duyulmaz, 1 = tam ses. Bilgi yoksa 1.
static float RemoteHornGain(uintptr_t game) {
    const int origin = g_RemoteHornOrigin.load();
    if (origin < 0 || origin >= (int)MAX_PLAYERS) return 1.0f;
    int rid = -1;
    { std::lock_guard<std::mutex> lock(g_ActiveMutex); rid = g_RemoteActive[origin].netId; }
    if (rid < 0) return 1.0f;
    const uintptr_t rv = GetVehicleFromIndex(game, (uint16_t)rid);
    const uintptr_t lv = LocalActiveVehiclePtr(game);
    if (rv == 0 || lv == 0) return 1.0f;
    const uintptr_t rb = *(uintptr_t*)(rv + 0x528), lb = *(uintptr_t*)(lv + 0x528);
    if (rb == 0 || lb == 0) return 1.0f;
    const float dx = *(float*)(rb + 0x0C) - *(float*)(lb + 0x0C);
    const float dy = *(float*)(rb + 0x10) - *(float*)(lb + 0x10);
    const float d = sqrtf(dx * dx + dy * dy);
    if (d <= kHornFullDist) return 1.0f;
    if (d >= kHornMaxDist) return 0.0f;
    const float t = (kHornMaxDist - d) / (kHornMaxDist - kHornFullDist);
    return t * t;                                      // karesel azalma
}

static void HornFrame(uintptr_t game, uint64_t nowMs) {
    if (!g_IsConnected.load()) {
        if (g_HornForced) { *(uint8_t*)(game + 0xA416) = 0; g_HornForced = false; }
        return;
    }
    static bool s_sentOn = false;
    static uint64_t s_lastSend = 0;
    static bool s_prevPlaying = false;
    static float s_lastGain = -1.0f;
    void* hornSrc = (void*)(game + kHornSourceOff);

    // Kornanin sesi bu karede bitti mi? (oyun az once pause etti) -> kanali birak.
    const bool playing = *(uint8_t*)(game + 0xA417) != 0;
    if (s_prevPlaying && !playing && g_AudioStopFn) g_AudioStopFn(hornSrc);
    s_prevPlaying = playing;

    const bool localPressed = (*(uint8_t*)(game + 0xA416) != 0) && !g_HornForced;   // zorladigimiz degeri sayma
    if (!g_HornForced && (localPressed != s_sentOn || (localPressed && nowMs - s_lastSend > 1500))) {
        QueueEntityState(3, 0, 0, 0, 0, localPressed ? 1 : 0);
        s_sentOn = localPressed; s_lastSend = nowMs;
    }
    const bool remoteHorn = (long long)nowMs < g_RemoteHornUntilMs.load();
    const float gain = remoteHorn ? RemoteHornGain(game) : 1.0f;
    const bool remoteOn = remoteHorn && gain > 0.01f;                               // cok uzaksa hic calma
    if (remoteOn && !localPressed) {
        *(uint8_t*)(game + 0xA416) = 1;
        g_HornForced = true;
        if (g_AudioSetVolFn && playing && fabsf(gain - s_lastGain) > 0.02f) {       // mesafeye gore ses
            g_AudioSetVolFn(hornSrc, gain);
            s_lastGain = gain;
        }
    } else if (g_HornForced) {
        *(uint8_t*)(game + 0xA416) = 0;
        g_HornForced = false;
        s_lastGain = -1.0f;                                                         // bir sonraki calista yeniden ayarla
    }
}


// ========================================================================
// ROMORK / ALET SATIN ALMA - SATMA (karsilikli)
// Havuz dizileri (trailer: game+0x1a0.., alet: game+0x294..) sayac + "son ile yer degistir" ile silinir.
// Her karede dizinin goruntusu bir oncekiyle karsilastirilir: sonda yeni giris = satin alindi (ekle),
// kaybolan giris = satildi (sil). Karsi cihazda oyunun kendi Game::addTrailer/addTool,
// Game::removeTrailer/removeTool ve destroy fonksiyonlari CAGRILIR (kanca yok, para dokunulmaz).
// ========================================================================
typedef void (*Game_addItem_t)(void* game, int type, const float* pos, uint32_t angleBits, uint32_t extra);
typedef void (*Game_removeIdx_t)(void* game, uint32_t idx);
static Game_addItem_t g_GameAddTrailerFn = nullptr;
static Game_addItem_t g_GameAddToolFn = nullptr;
static Game_removeIdx_t g_GameRemoveTrailerFn = nullptr;
static Game_removeIdx_t g_GameRemoveToolFn = nullptr;
static Entity_toggle_t g_TrailerDestroyFn = nullptr;               // Trailer::destroy()
static Entity_toggle_t g_ToolDestroyFn = nullptr;                  // Tool::destroy()
static std::vector<uintptr_t> g_PoolSnap[2];                       // [0] romorklar, [1] aletler
static bool g_PoolSnapValid = false;

static void PoolTake(uintptr_t game, std::vector<uintptr_t>* snap) {
    snap[0].clear(); snap[1].clear();
    const uint32_t tn = TrailerCount(game), on = ToolCount(game);
    for (uint32_t i = 0; i < tn; ++i) snap[0].push_back(RawTrailerSlot(game, i));
    for (uint32_t i = 0; i < on; ++i) snap[1].push_back(RawToolSlot(game, i));
}

// Siralar degisti: sira numarasina bagli tum konum/yayin kayitlari gecersiz.
static void PoolOnChanged() {
    memset(g_TrailerMine, 0, sizeof(g_TrailerMine));
    memset(g_ToolMine, 0, sizeof(g_ToolMine));
    memset(g_AttSensorBody, 0, sizeof(g_AttSensorBody));
    memset(g_AttSensorMask, 0, sizeof(g_AttSensorMask));
    memset(g_AttSensorMs, 0, sizeof(g_AttSensorMs));
    std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
    for (uint16_t i = 0; i < 64; ++i) ClearVehicleStateSlot((uint16_t)(TRAILER_ID_BASE + i));
    for (uint16_t i = 0; i < 64; ++i) ClearVehicleStateSlot((uint16_t)(TOOL_ID_BASE + i));
}

static bool PtrIn(const std::vector<uintptr_t>& v, uintptr_t p) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == p) return true;
    return false;
}

// Her kare (oyun thread'i, durum 6 ve 7): yerel satin alma / satma tespiti.
// ========================================================================
// ZAMAN SENKRONU: Environment nesnesi Game+0xA098'de (+0 time, +8 realTime, ikisi double).
// Environment::update sadece Game::updateStateBase icinden cagrilir; yani oyun durumu 6 disinda
// (magaza, ayarlar) o cihazin saati DURUR, digerlerinin akar. Host'un saati referanstir:
// host her saniye saatini gonderir, client fark 1 sn'yi gecerse kendi saatini ona ceker.
// Boylece mağaza/ayarlardan donen cihaz geri kalanlarla ayni saate gelir.
// ========================================================================
static std::mutex g_TimeMutex;
static bool g_TimePending = false;
static double g_TimeHost = 0.0, g_TimeHostReal = 0.0;
static uint64_t g_TimeLastSendMs = 0;
static const uintptr_t kEnvOff = 0xA098;
static const double kTimeSyncThreshold = 1.0;      // saniye

static double ReadEnvD(uintptr_t game, uintptr_t off) { double d; memcpy(&d, (const void*)(game + kEnvOff + off), 8); return d; }
static void WriteEnvD(uintptr_t game, uintptr_t off, double d) { memcpy((void*)(game + kEnvOff + off), &d, 8); }

static void TimeSyncFrame(uintptr_t game, uint32_t st) {
    if (game == 0) return;
    static uint64_t s_lastAdv = 0;
    if (!g_IsConnected.load() || (st != 6 && st != 7)) { s_lastAdv = 0; return; }
    const uint64_t now = (uint64_t)NowMs();

    // Magaza/ayarlar (durum 7) acikken oyun saati durur; bagliyken saat akmaya devam etsin ki hicbir cihaz
    // digerlerinden geri kalmasin. Environment::update'in aynisi (decompile): realTime += dt;
    // time += dt * olcek (gece 20:00-06:00 arasi dt x4); olcek = Environment+0x10 (float).
    if (st == 7) {
        if (s_lastAdv != 0) {
            double dt = (double)(now - s_lastAdv) / 1000.0;
            if (dt > 0.1) dt = 0.1;
            if (dt > 0.0) {
                const double day = ReadEnvD(game, 0x18);
                float step = (float)dt;
                if (day > 72000.0 || day < 21600.0) step *= 4.0f;
                float scale = 0.0f;
                memcpy(&scale, (const void*)(game + kEnvOff + 0x10), sizeof(scale));
                if (scale > 0.0f && scale < 100000.0f) WriteEnvD(game, 0, ReadEnvD(game, 0) + (double)(step * scale));
                WriteEnvD(game, 8, ReadEnvD(game, 8) + dt);
            }
        }
        s_lastAdv = now;
    } else {
        s_lastAdv = 0;
    }
    if (g_IsHost.load()) {
        if (now - g_TimeLastSendMs >= 1000) {
            g_TimeLastSendMs = now;
            TimeSyncPacket tp; memset(&tp, 0, sizeof(tp));
            tp.type = PACKET_TIME_SYNC; tp.originId = g_LocalPlayerId.load();
            tp.time = ReadEnvD(game, 0); tp.realTime = ReadEnvD(game, 8);
            QueueVehicleEvent(&tp, sizeof(tp));
        }
        return;
    }
    bool have = false; double ht = 0.0, hr = 0.0;
    {
        std::lock_guard<std::mutex> lock(g_TimeMutex);
        if (g_TimePending) { have = true; ht = g_TimeHost; hr = g_TimeHostReal; g_TimePending = false; }
    }
    if (!have) return;
    const double lr = ReadEnvD(game, 8);
    if (fabs(lr - hr) > kTimeSyncThreshold) {
        WriteEnvD(game, 0, ht);
        WriteEnvD(game, 8, hr);
        LOGI("[TIME] saat host'a cekildi: fark=%.1f sn", hr - lr);
    }
}

static void PoolPollFrame(uintptr_t game) {
    if (game == 0) return;
    if (!g_VehNetArmed.load() || !g_IsConnected.load()) { g_PoolSnapValid = false; return; }
    std::vector<uintptr_t> cur[2];
    PoolTake(game, cur);
    if (!g_PoolSnapValid) { g_PoolSnap[0] = cur[0]; g_PoolSnap[1] = cur[1]; g_PoolSnapValid = true; return; }
    bool changed = false;
    for (int k = 0; k < 2; ++k) {
        const uint8_t kind = (uint8_t)(k + 1);
        const std::vector<uintptr_t>& prev = g_PoolSnap[k];
        const std::vector<uintptr_t>& now = cur[k];
        if (prev == now) continue;
        std::vector<uintptr_t> sim = prev;
        for (size_t i = 0; i < prev.size(); ++i) {            // kaybolanlar = satildi
            const uintptr_t pp = prev[i];
            if (PtrIn(now, pp)) continue;
            size_t idx = 0;
            for (; idx < sim.size(); ++idx) if (sim[idx] == pp) break;
            if (idx >= sim.size()) continue;
            const uint32_t type = *(uint32_t*)(pp + 0x10);
            QueuePoolOp(kind, 0, (uint16_t)type, (uint8_t)idx, 0, 0, 0);
            LOGI("[POOL] satildi: %s tip=%u sira=%u", k == 0 ? "romork" : "alet", (unsigned)type, (unsigned)idx);
            sim[idx] = sim.back();                             // oyunun "son ile yer degistir" mantigi
            sim.pop_back();
            changed = true;
        }
        for (size_t i = sim.size(); i < now.size(); ++i) {     // sonda yeni girisler = satin alindi
            const uintptr_t np = now[i];
            const uintptr_t body = *(uintptr_t*)(np + (k == 0 ? 0x180 : 0x228));
            const uint32_t type = *(uint32_t*)(np + 0x10);
            if (body == 0 || type > 0xFFFF) continue;
            QueuePoolOp(kind, 1, (uint16_t)type, (uint8_t)i, *(float*)(body + 0x0C), *(float*)(body + 0x10), *(float*)(body + 0x40));
            LOGI("[POOL] alindi: %s tip=%u sira=%u", k == 0 ? "romork" : "alet", (unsigned)type, (unsigned)i);
            changed = true;
        }
    }
    g_PoolSnap[0] = cur[0]; g_PoolSnap[1] = cur[1];
    if (changed) PoolOnChanged();
}

static void ApplyPendingPoolOps(uintptr_t game, uint8_t localOwner) {
    std::vector<PoolOpPacket> ops;
    {
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        if (g_PendingPoolOps.empty()) return;
        if (!g_VehNetArmed.load()) return;
        ops.swap(g_PendingPoolOps);
    }
    void* world = *(void**)(game + 0xA0);
    bool changed = false;
    for (const PoolOpPacket& op : ops) {
        if (op.ownerId == localOwner) continue;
        const bool isTool = (op.kind == 2);
        const char* nm = isTool ? "alet" : "romork";
        if (op.op == 1) {                                      // EKLE
            Game_addItem_t fn = isTool ? g_GameAddToolFn : g_GameAddTrailerFn;
            const uint32_t total = VehicleCount(game) + TrailerCount(game) + ToolCount(game);
            if (!fn || total >= 0x1e) { LOGI("[POOL] ekleme atlandi: %s (fonksiyon yok / sinir)", nm); continue; }
            const float pos[3] = { op.x, 0.0f, op.y };
            uint32_t ab; memcpy(&ab, &op.a, 4);
            fn((void*)game, (int)op.itemType, pos, ab, 0);
            LOGI("[POOL] uzaktan ekleme uygulandi: %s tip=%u", nm, (unsigned)op.itemType);
            changed = true;
        } else {                                               // SIL
            Game_removeIdx_t rm = isTool ? g_GameRemoveToolFn : g_GameRemoveTrailerFn;
            Entity_toggle_t des = isTool ? g_ToolDestroyFn : g_TrailerDestroyFn;
            if (!rm || !des) { LOGI("[POOL] silme atlandi: %s (fonksiyon yok)", nm); continue; }
            const uint32_t n = isTool ? ToolCount(game) : TrailerCount(game);
            int found = -1;
            if (op.idx < n) {
                const uintptr_t tp = isTool ? RawToolSlot(game, op.idx) : RawTrailerSlot(game, op.idx);
                if (tp != 0 && *(uint32_t*)(tp + 0x10) == op.itemType) found = op.idx;
            }
            if (found < 0) {                                   // sira kaymis: ayni tipteki ilk girisi bul (oyunun sat mantigi)
                for (uint32_t i = 0; i < n; ++i) {
                    const uintptr_t tp = isTool ? RawToolSlot(game, i) : RawTrailerSlot(game, i);
                    if (tp != 0 && *(uint32_t*)(tp + 0x10) == op.itemType) { found = (int)i; break; }
                }
            }
            if (found < 0) { LOGI("[POOL] silme atlandi: %s tip=%u bulunamadi", nm, (unsigned)op.itemType); continue; }
            const uintptr_t tp = isTool ? RawToolSlot(game, found) : RawTrailerSlot(game, found);
            const uintptr_t av = *(uintptr_t*)(tp + (isTool ? 0x23C : 0x184));
            if (av != 0 && world != nullptr && SlotIndexOfVehicle(game, av) >= 0) {
                if (isTool) { if (g_VehicleDetachToolFn) g_VehicleDetachToolFn((void*)av, world); }
                else { if (g_VehicleDetachTrailerFn) g_VehicleDetachTrailerFn((void*)av, world); }
            }
            rm((void*)game, (uint32_t)found);
            des((void*)tp);
            LOGI("[POOL] uzaktan silme uygulandi: %s tip=%u sira=%d", nm, (unsigned)op.itemType, found);
            changed = true;
        }
    }
    if (changed) {
        PoolTake(game, g_PoolSnap);                            // yerel degisiklik sanilip geri yayinlanmasin
        g_PoolSnapValid = true;
        PoolOnChanged();
    }
}

// ---- Tarla islemleri: Map::updateFields kancasi yerel islemi gonderir, bu fonksiyon uzaktakini uygular ----
typedef void (*Map_updateFields_t)(void* map, uint32_t f1, uint32_t f2, uint32_t f3, uint32_t s0, uint32_t s1,
                                   uint32_t s2, uint32_t s3, uint32_t s4, void* fieldWork);
static Map_updateFields_t orig_Map_updateFields = nullptr;
static void* g_MapUpdateFieldsRaw = nullptr;

static void ApplyPendingFieldOps(uintptr_t game) {
    std::vector<FieldOpPacket> ops;
    {
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        if (g_PendingFieldOps.empty()) return;
        ops.swap(g_PendingFieldOps);
    }
    void* map = (void*)(*(uintptr_t*)(game + 0x704));
    Map_updateFields_t fn = orig_Map_updateFields ? orig_Map_updateFields : (Map_updateFields_t)g_MapUpdateFieldsRaw;
    if (!map || !fn) return;
    static int s_logs = 0;
    g_ReplayingFieldOp = true;
    for (const FieldOpPacket& op : ops) {
        uint32_t fw[8] = { op.wtype, op.wfruit, 0, 0, 0, 0, 0, 0 };     // FieldWork: tip, urun, sonuc alanlari (sifir)
        fn(map, op.p[0], op.p[1], op.p[2], op.p[3], op.p[4], op.p[5], op.p[6], op.p[7], fw);
    }
    g_ReplayingFieldOp = false;
    g_LastRemoteFieldOpMs.store(NowMs());
    if (s_logs < 12) { ++s_logs; LOGI("[FIELD] uzaktan tarla islemi uygulandi: %u adet", (unsigned)ops.size()); }
}

// ========================================================================
// BALYALAR (Bale): ekleme / silme / konum senkronu
// Oyun balyalari Game+0x388 havuzunda tutar (sayac Game+900, en fazla 50). Bale+0x50 = b2Body*, govdede
// +0x0C/+0x10 = x/y, +0x40 = aci. Her karede yerel balya listesi bir onceki goruntuyle (isaretci kimligiyle)
// karsilastirilir: yeni isaretci = eklendi (baler birakti), kaybolan = silindi (alet topladi / satis noktasinda
// satildi), konumu degisen = tasindi (traktor itti). Olaylar karsi cihazda KONUMA gore eslenir (balyalar birbirinin
// aynisi oldugundan kimlik gerekmez) ve oyunun kendi Game::addBale / Game::removeBale(Bale*) + Bale::destroy
// fonksiyonlari CAGRILIR (yeni kanca YOK). Ek olarak host 5 sn'de bir tum listeyi yollar; client yerel degisiklik
// yokken eslesmeyen balyalari ekler/siler (olay kaybi / farkli gecmis icin guvenlik agi).
// SATIS: satis noktasinda (Map+0x778/0x780, yaricap 2.5) satilan balya parayi yerelde ekler; iki cihaz da ayni
// balyayi ayni anda satarsa gelir ikiye katlanirdi. Silme olayina "sale" bayragi konur; ikisi de satmissa
// oyuncu numarasi BUYUK olan cihaz kendi gelirini geri alir (para delta yoluyla herkese yansir).
// ========================================================================
static const bool kSyncBales = true;
typedef void (*Game_addBale_t)(void* game, const float* pos3, uint32_t angleBits, uint32_t extra);
typedef void (*Game_removeBaleP_t)(void* game, void* bale);
static Game_addBale_t g_GameAddBaleFn = nullptr;            // Game::addBale(Vector3 const&, float, unsigned)
static Game_removeBaleP_t g_GameRemoveBaleFn = nullptr;     // Game::removeBale(Bale*)
static Entity_toggle_t g_BaleDestroyFn = nullptr;           // Bale::destroy()

struct BaleTrack {
    uintptr_t p;
    float x, y, a;              // son gozlenen konum
    float sx, sy, sa;           // en son yayinlanan / uygulanan konum
    long long bornMs, sentMs, remoteMs;
    long long nearMs;           // bu cihazin otorite sahibi araci balyayi en son ne zaman itti
    uint16_t nearVehicleId;     // hareketi baslatan yerel arac; sahiplik degisirse eski cihaz yayin yapamaz
    float tx, ty, ta;           // uzaktan gelen hedef konum (yumusatma: her karede hedefe dogru yaklasir)
    float vx, vy;               // hedefin tahmini hizi (m/s): arac hayalet olarak ileri tahmin edildigi icin balya da ileri tahmin edilir
    long long tms;              // hedefin geldigi an
    bool hasTarget;
    bool sensor;                // uzaktan yonetilirken yerel fizik kapali (arac balyanin icinden gecip itip firlatmasin)
};
struct BaleRecent { float x, y; long long ms; bool sale; bool local; bool used; double gain; };
static std::vector<BaleTrack> g_BaleTrk;
static bool g_BaleTrkValid = false;
static long long g_BaleLocalChangeMs = 0;       // son yerel ekleme/silme
static long long g_BaleRemoteMs = 0;            // son uzaktan uygulanan olay
static long long g_BaleListSendMs = 0;
static double g_BaleFrameGain = 0.0;            // oyunun bu karede (updateStateBase) kazandigi para
static BaleRecent g_BaleRecent[16];
static int g_BaleRecentNext = 0;

static uint32_t BaleCountOf(uintptr_t game) {
    const uint32_t n = *(volatile uint32_t*)(game + 900);
    return n > kBaleMax ? kBaleMax : n;
}
static uintptr_t BalePtrAt(uintptr_t game, uint32_t i) { return *(uintptr_t*)(game + 0x388 + (uintptr_t)i * 4u); }

static bool ReadBalePose(uintptr_t p, float* x, float* y, float* a) {
    const uintptr_t body = p ? *(uintptr_t*)(p + 0x50) : 0;
    if (!body) return false;
    *x = *(float*)(body + 0x0C); *y = *(float*)(body + 0x10); *a = *(float*)(body + 0x40);
    return (*x == *x) && (*y == *y) && (*a == *a);
}
static BaleTrack* BaleTrackOf(uintptr_t p) {
    for (size_t i = 0; i < g_BaleTrk.size(); ++i) if (g_BaleTrk[i].p == p) return &g_BaleTrk[i];
    return nullptr;
}
static void BaleForget(uintptr_t p) {
    for (size_t i = 0; i < g_BaleTrk.size(); ++i)
        if (g_BaleTrk[i].p == p) { g_BaleTrk.erase(g_BaleTrk.begin() + i); return; }
}
// Bu cihazda uzaktan uygulanan / uzlastirilan balyayi "yerelde yayinlanmis" say (yanki olmasin) ve kisa sure uzaktan yonetilen isaretle.
static void BaleRemember(uintptr_t p, long long now) {
    float x, y, a;
    if (!ReadBalePose(p, &x, &y, &a)) return;
    BaleTrack* t = BaleTrackOf(p);
    if (!t) {
        BaleTrack nt; memset(&nt, 0, sizeof(nt));
        nt.p = p; nt.bornMs = now; nt.nearVehicleId = VEHICLE_ID_INVALID;
        g_BaleTrk.push_back(nt);
        t = &g_BaleTrk.back();
    }
    t->x = t->sx = x; t->y = t->sy = y; t->a = t->sa = a;
    t->sentMs = now; t->remoteMs = now;
}
static uintptr_t BaleNearest(uintptr_t game, float x, float y, float maxD2) {
    uintptr_t best = 0; float bd = maxD2;
    const uint32_t n = BaleCountOf(game);
    for (uint32_t i = 0; i < n; ++i) {
        const uintptr_t p = BalePtrAt(game, i);
        float bx, by, ba;
        if (!p || !ReadBalePose(p, &bx, &by, &ba)) continue;
        const float dx = bx - x, dy = by - y, d2 = dx * dx + dy * dy;
        if (d2 <= bd) { bd = d2; best = p; }
    }
    return best;
}
static bool BaleInTipSite(uintptr_t game, float x, float y) {
    const uintptr_t map = *(uintptr_t*)(game + 0x704);
    if (!map) return false;
    const float dx = *(float*)(map + 0x778) - x, dy = *(float*)(map + 0x780) - y;     // Map::getIsBaleTipSiteInRange
    return dx * dx + dy * dy < 6.25f;
}
static void BaleNoteRemoval(float x, float y, long long now, bool sale, bool local, double gain) {
    BaleRecent& r = g_BaleRecent[g_BaleRecentNext];
    g_BaleRecentNext = (g_BaleRecentNext + 1) & 15;
    r.x = x; r.y = y; r.ms = now; r.sale = sale; r.local = local; r.used = false; r.gain = gain;
}
// Bu cihazin kendi (henuz eslenmemis) silmesi: uzaktan gelen ayni silme olayi bunun kopyasidir.
static BaleRecent* BaleLocalRemovalNear(float x, float y, long long now) {
    BaleRecent* best = nullptr;
    for (int i = 0; i < 16; ++i) {
        BaleRecent& r = g_BaleRecent[i];
        if (r.ms == 0 || !r.local || r.used || now - r.ms > 1500) continue;
        const float dx = r.x - x, dy = r.y - y;
        if (dx * dx + dy * dy > 1.0f) continue;
        if (!best || r.ms < best->ms) best = &r;
    }
    return best;
}
// Tasima (op 3) olaylari kuyruk doluyken YOLLANMAZ (geri basinc): QueueVehicleEvent 4096 bayti asinca tum kuyrugu siler
// (ekleme/silme olaylari dahil). Tasima zaten bir sonraki karede en guncel konumla tekrar denenir. false = yollanmadi.
static bool QueueBaleOp(uint8_t op, float ox, float oy, float x, float y, float a, uint8_t sale) {
    if (!g_IsConnected.load()) return true;
    if (op == 3) {
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        if (g_VehicleEventOut.size() > 2400) return false;
    }
    BaleOpPacket p; memset(&p, 0, sizeof(p));
    p.type = PACKET_BALE_OP; p.ownerId = g_LocalPlayerId.load(); p.op = op; p.sale = sale;
    p.ox = ox; p.oy = oy; p.x = x; p.y = y; p.a = a;
    QueueVehicleEvent(&p, sizeof(p));
    return true;
}
static float BaleWrapAngle(float d) {
    while (d > 3.14159265f) d -= 6.28318531f;
    while (d < -3.14159265f) d += 6.28318531f;
    return d;
}
static void SetBalePose(uintptr_t p, float x, float y, float a) {
    const uintptr_t body = *(uintptr_t*)(p + 0x50);
    if (!body || !g_b2BodySetTransform) return;
    TestB2Vec2 pos; pos.x = x; pos.y = y;
    g_b2BodySetTransform((void*)body, &pos, a);
    SetRemoteBodyVelocity(body, 0.0f, 0.0f, 0.0f);
    if (*(int*)body != 0) {                                // dinamik govde: Bale::update gorunumu yenilesin
        *(uint16_t*)(body + 4) |= 2;
        *(float*)(body + 0x90) = 0.0f;
    }
}
static bool BaleAddLocal(uintptr_t game, float x, float y, float a, long long now) {
    if (!g_GameAddBaleFn) return false;
    if (BaleCountOf(game) >= kBaleMax) BaleForget(BalePtrAt(game, 0));     // oyun en eskiyi geri donusturur: silme olayi uretme
    const float pos3[3] = { x, 0.0f, y };                                  // Vector3: x, (kullanilmaz), y
    uint32_t ab; memcpy(&ab, &a, sizeof(ab));
    g_GameAddBaleFn((void*)game, pos3, ab, 0);
    const uint32_t n = BaleCountOf(game);
    if (n > 0) BaleRemember(BalePtrAt(game, n - 1), now);
    return true;
}
static bool BaleRemoveLocal(uintptr_t game, uintptr_t p) {
    if (!p || !g_GameRemoveBaleFn || !g_BaleDestroyFn) return false;
    g_GameRemoveBaleFn((void*)game, (void*)p);
    g_BaleDestroyFn((void*)p);
    BaleForget(p);
    return true;
}

// Balyanin butun parcalarini (fixture) "sensor" yapar: carpisma TEPKISI olmaz (Box2D 2.1: b2Contact::Update her adimda
// fixture+0x32 bayragini okur), ama konum/gorunum ayni kalir. Duzen (decompile'dan): b2Body+0x68 = fixture listesi,
// fixture+0x14 = sonraki, fixture+0x32 = isSensor.
static void BaleSetSensor(uintptr_t p, bool on) {
    const uintptr_t body = p ? *(uintptr_t*)(p + 0x50) : 0;
    if (!body) return;
    const int fc = *(volatile int*)(body + 0x6C);
    if (fc <= 0 || fc > 8) return;
    uintptr_t f = *(uintptr_t*)(body + 0x68);
    for (int n = 0; f != 0 && n < fc; ++n, f = *(uintptr_t*)(f + 0x14)) *(volatile uint8_t*)(f + 0x32) = on ? 1 : 0;
    if (*(int*)body != 0) { *(uint16_t*)(body + 4) |= 2; *(float*)(body + 0x90) = 0.0f; }
}

// Uzaktan gelen tasimayi tek seferde isinlamak yerine hedef olarak isaretle; BaleSmoothStep her karede hedefe yaklastirir.
static void BaleSetTarget(uintptr_t p, float x, float y, float a, long long now) {
    float cx, cy, ca;
    if (!ReadBalePose(p, &cx, &cy, &ca)) return;
    const float dx = x - cx, dy = y - cy;
    if (dx * dx + dy * dy > 16.0f) {                                       // 4 m'den uzak: yumusatma anlamsiz, direkt koy
        SetBalePose(p, x, y, a);
        BaleRemember(p, now);
        BaleTrack* t0 = BaleTrackOf(p);
        if (t0) t0->hasTarget = false;
        return;
    }
    BaleRemember(p, now);                                                  // iz yoksa olusturur, uzaktan yonetilen isaretler
    BaleTrack* t = BaleTrackOf(p);
    if (!t) return;
    // Hiz tahmini: ardisik hedeflerden (dusuk gecirgen). Kesintiden sonra sifirdan baslar.
    if (t->hasTarget && t->tms != 0 && now - t->tms >= 20 && now - t->tms <= 400) {
        const float dt = (float)(now - t->tms) * 0.001f;
        float nvx = (x - t->tx) / dt, nvy = (y - t->ty) / dt;
        const float sp2 = nvx * nvx + nvy * nvy;
        if (sp2 > 64.0f) { const float k = 8.0f / sqrtf(sp2); nvx *= k; nvy *= k; }   // en fazla 8 m/s
        t->vx = 0.5f * t->vx + 0.5f * nvx; t->vy = 0.5f * t->vy + 0.5f * nvy;
    } else {
        t->vx = t->vy = 0.0f;
    }
    t->tx = x; t->ty = y; t->ta = a; t->tms = now; t->hasTarget = true;
    if (!t->sensor) { BaleSetSensor(p, true); t->sensor = true; }
}
// Hedefi (op.ox, op.oy) olan, yumusatilan balyayi bul (yerel konum hedefin gerisinde kalabilir).
static uintptr_t BaleByTarget(float ox, float oy) {
    uintptr_t best = 0; float bd = 0.25f * 0.25f;
    for (size_t i = 0; i < g_BaleTrk.size(); ++i) {
        const BaleTrack& t = g_BaleTrk[i];
        if (!t.hasTarget) continue;
        const float dx = t.tx - ox, dy = t.ty - oy, d2 = dx * dx + dy * dy;
        if (d2 <= bd) { bd = d2; best = t.p; }
    }
    return best;
}
static void BaleSmoothStep(long long now) {
    static long long s_last = 0;
    long long dt = s_last ? now - s_last : 16;
    s_last = now;
    if (dt < 1) dt = 1;
    if (dt > 100) dt = 100;
    const float alpha = 1.0f - expf(-(float)dt / 25.0f);                   // ~25 ms: hareket hedefini daha yakindan takip et
    for (size_t i = 0; i < g_BaleTrk.size(); ++i) {
        BaleTrack& t = g_BaleTrk[i];
        if (t.sensor && !t.hasTarget && now - t.remoteMs > 700) {          // uzaktan hareket bitti: yerel fizik geri gelsin
            BaleSetSensor(t.p, false);
            t.sensor = false;
        }
        if (!t.hasTarget) continue;
        float x, y, a;
        if (!ReadBalePose(t.p, &x, &y, &a)) { t.hasTarget = false; t.sensor = false; continue; }
        float age = (float)(now - t.tms) * 0.001f;                         // hedef ne kadar once geldi: o kadar ileri tahmin (en fazla 120 ms)
        if (now - t.tms > 250) { t.vx = t.vy = 0.0f; age = 0.0f; }
        if (age > 0.12f) age = 0.12f;
        const float gx = t.tx + t.vx * age, gy = t.ty + t.vy * age;
        const float dx = gx - x, dy = gy - y, da = BaleWrapAngle(t.ta - a);
        float nx, ny, na;
        if (dx * dx + dy * dy < 0.0004f && fabsf(da) < 0.02f && t.vx == 0.0f && t.vy == 0.0f) {
            nx = t.tx; ny = t.ty; na = t.ta; t.hasTarget = false;
        } else {
            nx = x + dx * alpha; ny = y + dy * alpha; na = a + da * alpha;
            if (now - t.tms > 3000) t.hasTarget = false;                   // olay kesildi
        }
        SetBalePose(t.p, nx, ny, na);
        t.x = t.sx = nx; t.y = t.sy = ny; t.a = t.sa = na;
        t.sentMs = now; t.remoteMs = now;                                  // yerel olarak yayinlanmasin (yanki yok)
    }
}

// Karsi cihazdan gelen tek bir balya olayini uygular.
static void BaleApplyOp(uintptr_t game, const BaleOpPacket& op, uint8_t localOwner, long long now) {
    if (!(op.x == op.x) || !(op.y == op.y) || !(op.a == op.a) || !(op.ox == op.ox) || !(op.oy == op.oy)) return;
    if (fabsf(op.x) > 1.0e5f || fabsf(op.y) > 1.0e5f || fabsf(op.ox) > 1.0e5f || fabsf(op.oy) > 1.0e5f) return;
    static int s_moveLogs = 0;
    g_BaleRemoteMs = now;
    if (op.op == 1) {                                                      // eklendi
        const uintptr_t cand = BaleNearest(game, op.x, op.y, 0.8f * 0.8f);
        BaleTrack* ct = cand ? BaleTrackOf(cand) : nullptr;
        if (cand && ct && now - ct->bornMs < 3000) {                       // ayni balyayi bu cihaz da (aynali isciyle) az once uretti
            SetBalePose(cand, op.x, op.y, op.a);
            BaleRemember(cand, now);
            LOGI("[BALE] ekleme olayi yerel kopyayla birlestirildi (%.1f,%.1f)", op.x, op.y);
            return;
        }
        if (BaleAddLocal(game, op.x, op.y, op.a, now))
            LOGI("[BALE] uzaktan ekleme uygulandi (%.1f,%.1f) oyuncu=%u", op.x, op.y, (unsigned)op.ownerId);
        return;
    }
    if (op.op == 2) {                                                      // silindi
        BaleRecent* rr = BaleLocalRemovalNear(op.ox, op.oy, now);
        if (rr) {                                                          // bu cihaz ayni balyayi zaten kendisi sildi
            rr->used = true;
            if (op.sale && rr->sale && rr->gain > 0.0 && localOwner > op.ownerId) {
                WriteMoney(game, ReadMoney(game) - rr->gain);              // cift satis: numarasi buyuk olan geliri geri alir
                LOGI("[BALE] cift satis: yerel gelir geri alindi %.2f", rr->gain);
                rr->gain = 0.0;
            }
            return;
        }
        const uintptr_t cand = BaleNearest(game, op.ox, op.oy, 1.5f * 1.5f);
        if (!cand) { LOGI("[BALE] silme atlandi: (%.1f,%.1f) yakininda balya yok", op.ox, op.oy); return; }
        float bx, by, ba;
        ReadBalePose(cand, &bx, &by, &ba);
        if (BaleRemoveLocal(game, cand)) {
            BaleNoteRemoval(bx, by, now, op.sale != 0, false, 0.0);
            LOGI("[BALE] uzaktan silme uygulandi (%.1f,%.1f)%s oyuncu=%u", bx, by, op.sale ? " [satis]" : "", (unsigned)op.ownerId);
        }
        return;
    }
    if (op.op == 3) {                                                      // tasindi
        uintptr_t cand = BaleByTarget(op.ox, op.oy);                       // onceki hedefi bu olayin baslangici olan balya
        if (!cand) cand = BaleNearest(game, op.ox, op.oy, 1.5f * 1.5f);
        if (!cand) cand = BaleNearest(game, op.x, op.y, 0.6f * 0.6f);      // zaten orada
        if (!cand) return;
        // Bir balyaya yakin yerel arac bulunmasi, uzaktan gelen hareketi reddetmek icin yeterli degildir.
        // Aksi halde iki cihazdaki traktör/balya temasi ayni anda kontrol almaya calisip konumlari ayristirir.
        // Gelen hareket sensor + hedef takibiyle uygulanir; yerel fizik ancak uzaktan kontrol sona erince geri gelir.
        BaleSetTarget(cand, op.x, op.y, op.a, now);
        if (s_moveLogs < 20) { ++s_moveLogs; LOGI("[BALE] uzaktan tasima uygulandi (%.1f,%.1f)->(%.1f,%.1f)", op.ox, op.oy, op.x, op.y); }
    }
}

// Balya hareketinin "sahibi": balyayi gercekten iten, bu cihazda YEREL suruldugu (aktif / sahip / yerel isci) arac.
// Baska cihazin suruculugu bu cihazda "hayalet" arac olarak balyanin icinden gecer ya da fizigi bozup balyayi koseye firlatir;
// bu yuzden bir balyanin hareketi SADECE yerel gercek bir aracin yakininda olusmussa yayinlanir (yoksa host/sahip cihazin
// akisi gecerlidir ve gelen konum yerel fizigin ustune yazilir).
static const float kBaleNearVehicleR2 = 20.0f * 20.0f;
static const long long kBaleOwnHoldMs = 2500;                 // araci biraktiktan sonra balyanin kayip durmasi icin
static const long long kBaleSendIntervalMs = 20;              // 50 Hz: balya hareketi daha dusuk gecikmeyle gitsin
struct BaleLocalVehiclePoint {
    float x;
    float y;
    uint16_t vehicleId;
};

// Tek yonlu balya hareketi: yalnizca host'un araca verdigi otorite sahibi cihaz
// balyanin yerel hareketini yayin kaynagi kabul edilir. Aktif arac indisi tek basina
// sahiplik degildir; ayni araci iki cihaz aktif gosterse bile ikinci cihaz kaynak olamaz.
static void BaleLocalVehiclePoints(uintptr_t game, std::vector<BaleLocalVehiclePoint>* pts) {
    pts->clear();
    const uint8_t localOwner = g_LocalPlayerId.load();
    if (localOwner >= MAX_PLAYERS) return;

    const uint32_t vn = VehicleCount(game);
    for (uint32_t i = 0; i < vn; ++i) {
        const uintptr_t v = RawVehicleSlot(game, i);
        if (!v) continue;
        const int nid = NetIdOfVehicle(v);
        // Net kimligi veya host tarafindan atanmis sahipligi olmayan arac,
        // balya konumu icin otorite kaynagi olamaz.
        if (nid < 0 || nid >= (int)VEHICLE_SLOT_LIMIT || g_AiMirrored[nid]) continue;

        uint8_t owner = VEHICLE_OWNER_NONE;
        {
            std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
            owner = g_VehicleAuthority[nid].ownerId;
        }
        if (owner != localOwner) continue;

        const uintptr_t vb = *(uintptr_t*)(v + 0x528);
        if (!vb) continue;
        BaleLocalVehiclePoint point;
        point.x = *(float*)(vb + 0x0C);
        point.y = *(float*)(vb + 0x10);
        point.vehicleId = (uint16_t)nid;
        pts->push_back(point);
    }
}

// Bale source araci hala bu cihazin kontrolunde mi? Otorite baska oyuncuya gectiyse
// onceki cihaz, nearMs tutma suresi dolmamis olsa bile balya hareketi gondermeyi birakir.
static bool BaleSourceVehicleStillLocal(uintptr_t game, uint16_t vehicleId, uint8_t localOwner) {
    if (vehicleId >= VEHICLE_SLOT_LIMIT || GetVehicleFromIndex(game, vehicleId) == 0) return false;
    uint8_t owner = VEHICLE_OWNER_NONE;
    {
        std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
        owner = g_VehicleAuthority[vehicleId].ownerId;
    }
    // Sahiplik kisa sureligine bosaltilinca son balya hareketinin tamamlanmasina izin ver;
    // fakat baska bir oyuncu yetkiyi aldiysa eski kaynak artik yayin yapamaz.
    return owner == localOwner || owner == VEHICLE_OWNER_NONE;
}

// Yerel degisiklikleri yakala ve yayinla.
static void BaleCapture(uintptr_t game, long long now) {
    static std::vector<BaleLocalVehiclePoint> vpts;                        // sadece otorite sahibi araclar (yeniden kullanilir)
    bool vptsDone = false;                                                 // arac noktalari sadece bir balya oynadiginda hesaplanir
    int moveBudget = 16;                                              // kare basina en fazla 12 tasima olayi (kuyrugu bogmaz)
    struct Cur { uintptr_t p; float x, y, a; };
    Cur cur[kBaleMax];
    uint32_t cn = 0;
    const uint32_t n = BaleCountOf(game);
    for (uint32_t i = 0; i < n && cn < kBaleMax; ++i) {
        const uintptr_t p = BalePtrAt(game, i);
        float x, y, a;
        if (!p || !ReadBalePose(p, &x, &y, &a)) continue;
        cur[cn].p = p; cur[cn].x = x; cur[cn].y = y; cur[cn].a = a; ++cn;
    }
    if (!g_BaleTrkValid) {                                                 // ilk goruntu: hicbir sey yayinlama
        g_BaleTrk.clear();
        for (uint32_t i = 0; i < cn; ++i) {
            BaleTrack t; memset(&t, 0, sizeof(t));
            t.p = cur[i].p; t.x = t.sx = cur[i].x; t.y = t.sy = cur[i].y; t.a = t.sa = cur[i].a;
            t.bornMs = now - 60000; t.sentMs = now; t.remoteMs = 0; t.nearVehicleId = VEHICLE_ID_INVALID;
            g_BaleTrk.push_back(t);
        }
        g_BaleTrkValid = true;
        return;
    }
    // 1) kaybolanlar = silindi
    struct Gone { float x, y; bool tip; };
    std::vector<Gone> gone;
    for (size_t i = 0; i < g_BaleTrk.size(); ++i) {
        bool found = false;
        for (uint32_t j = 0; j < cn; ++j) if (cur[j].p == g_BaleTrk[i].p) { found = true; break; }
        if (!found) { Gone g; g.x = g_BaleTrk[i].x; g.y = g_BaleTrk[i].y; g.tip = BaleInTipSite(game, g.x, g.y); gone.push_back(g); }
    }
    int tipCount = 0;
    for (size_t i = 0; i < gone.size(); ++i) if (gone[i].tip) ++tipCount;
    const double gain = g_BaleFrameGain;
    for (size_t i = 0; i < gone.size(); ++i) {
        const bool sale = gone[i].tip && gain > 0.0 && tipCount > 0;
        const double part = sale ? gain / (double)tipCount : 0.0;
        QueueBaleOp(2, gone[i].x, gone[i].y, 0.0f, 0.0f, 0.0f, sale ? 1 : 0);
        BaleNoteRemoval(gone[i].x, gone[i].y, now, sale, true, part);
        g_BaleLocalChangeMs = now;
        LOGI("[BALE] yerel silme (%.1f,%.1f)%s", gone[i].x, gone[i].y, sale ? " [satis]" : "");
    }
    // 2) yeni + tasinanlar
    static std::vector<BaleTrack> nt;
    nt.clear();
    for (uint32_t i = 0; i < cn; ++i) {
        const Cur& c = cur[i];
        BaleTrack* old = BaleTrackOf(c.p);
        BaleTrack t;
        if (!old) {
            memset(&t, 0, sizeof(t));
            t.p = c.p; t.x = t.sx = c.x; t.y = t.sy = c.y; t.a = t.sa = c.a;
            t.bornMs = now; t.sentMs = now; t.remoteMs = 0; t.nearVehicleId = VEHICLE_ID_INVALID;
            QueueBaleOp(1, 0.0f, 0.0f, c.x, c.y, c.a, 0);
            g_BaleLocalChangeMs = now;
            LOGI("[BALE] yerel ekleme (%.1f,%.1f)", c.x, c.y);
            nt.push_back(t);
            continue;
        }
        t = *old;
        t.x = c.x; t.y = c.y; t.a = c.a;
        // Uzak cihaz balyayi yonetiyorsa, o cihazdan gelen fizik hareketini yerel surucuya
        // mal etme. Aksi halde her iki cihaz da ayni balyanin konum paketini uretmeye baslayabilir.
        const float dx = c.x - t.sx, dy = c.y - t.sy;
        const bool moved = (dx * dx + dy * dy > 0.00015f) || GetAngleDelta(c.a, t.sa) > 0.02f;    // ~1.2 cm / ~1.1 derece
        if (moved && !t.sensor && !t.hasTarget && !vptsDone) {
            BaleLocalVehiclePoints(game, &vpts);
            vptsDone = true;
        }
        if (moved && !t.sensor && !t.hasTarget) {
            for (size_t k = 0; k < vpts.size(); ++k) {
                const float vx = vpts[k].x - c.x, vy = vpts[k].y - c.y;
                if (vx * vx + vy * vy < kBaleNearVehicleR2) {
                    t.nearMs = now;
                    t.nearVehicleId = vpts[k].vehicleId;
                    break;
                }
            }
        }
        const bool mine = (now - t.nearMs) < kBaleOwnHoldMs &&
                          BaleSourceVehicleStillLocal(game, t.nearVehicleId, g_LocalPlayerId.load());
        if (t.sensor || t.hasTarget || !mine) {                              // uzak balya aktif kontrol altindaysa yayinlama; sensor birakilinca yerel itis devralabilir
            t.sx = c.x; t.sy = c.y; t.sa = c.a;
        } else if (moved && now - t.sentMs >= kBaleSendIntervalMs && moveBudget > 0) {   // 30 Hz; durunca son konum da gider
            if (QueueBaleOp(3, t.sx, t.sy, c.x, c.y, c.a, 0)) {            // kuyruk doluysa sx/sentMs degismez: sonraki karede tekrar
                t.sx = c.x; t.sy = c.y; t.sa = c.a; t.sentMs = now; --moveBudget;
            }
        }
        nt.push_back(t);
    }
    g_BaleTrk.swap(nt);
}

// Host: tum balya listesini yayinla. Client: gelen listeyle yerel balyalari uzlastir.
static void BaleHostBroadcast(uintptr_t game, long long now, bool force) {
    if (!force && now - g_BaleListSendMs < 5000) return;
    uint8_t buf[sizeof(BaleListHeader) + kBaleMax * 12];
    BaleListHeader h; h.type = PACKET_BALE_LIST; h.count = 0;
    size_t off = sizeof(h);
    const uint32_t n = BaleCountOf(game);
    for (uint32_t i = 0; i < n; ++i) {
        float x, y, a;
        if (!ReadBalePose(BalePtrAt(game, i), &x, &y, &a)) continue;
        memcpy(buf + off, &x, 4); memcpy(buf + off + 4, &y, 4); memcpy(buf + off + 8, &a, 4);
        off += 12; ++h.count;
    }
    memcpy(buf, &h, sizeof(h));
    // Konumlar 0.1 birime yuvarlanip karmalanir: liste degismediyse 10 sn'de bir kalp atisi yeter.
    static uint32_t s_baleHash = 0;
    uint32_t hh = 2166136261u;
    for (size_t k = sizeof(h); k < off; k += 4) {
        float f; memcpy(&f, buf + k, 4);
        const int32_t q = (int32_t)lroundf(f * 10.0f);
        hh ^= (uint32_t)q; hh *= 16777619u;
    }
    hh ^= h.count;
    if (!force && hh == s_baleHash && now - g_BaleListSendMs < 10000) return;
    s_baleHash = hh;
    g_BaleListSendMs = now;
    QueueVehicleEvent(buf, off);
}

static void BaleReconcile(uintptr_t game, const std::vector<float>& list, long long now) {
    if (now - g_BaleLocalChangeMs < 3000 || now - g_BaleRemoteMs < 800) return;      // yerel/uzak hareket var: simdi karisma
    const size_t hn = list.size() / 3;
    std::vector<uintptr_t> locals;
    const uint32_t ln = BaleCountOf(game);
    for (uint32_t i = 0; i < ln; ++i) { const uintptr_t p = BalePtrAt(game, i); if (p) locals.push_back(p); }
    std::vector<char> lmatched(locals.size(), 0);
    int adds = 0, removes = 0, fixes = 0;
    std::vector<size_t> unmatchedHost;
    for (size_t h = 0; h < hn; ++h) {
        const float hx = list[h * 3], hy = list[h * 3 + 1], ha = list[h * 3 + 2];
        int best = -1; float bd = 1.2f * 1.2f;
        for (size_t l = 0; l < locals.size(); ++l) {
            if (lmatched[l]) continue;
            float x, y, a;
            if (!ReadBalePose(locals[l], &x, &y, &a)) continue;
            const float dx = x - hx, dy = y - hy, d2 = dx * dx + dy * dy;
            if (d2 <= bd) { bd = d2; best = (int)l; }
        }
        if (best < 0) { unmatchedHost.push_back(h); continue; }
        lmatched[best] = 1;
        if (bd > 0.35f * 0.35f) {                                                  // eslesti ama yeri farkli
            BaleTrack* t = BaleTrackOf(locals[best]);
            if (t && !t->hasTarget && !(t->nearMs != 0 && now - t->nearMs < 2500) && now - t->sentMs > 1500 && now - t->remoteMs > 1500) { SetBalePose(locals[best], hx, hy, ha); BaleRemember(locals[best], now); ++fixes; }
        }
    }
    for (size_t k = 0; k < unmatchedHost.size() && adds < 6; ++k) {
        const size_t h = unmatchedHost[k];
        if (BaleAddLocal(game, list[h * 3], list[h * 3 + 1], list[h * 3 + 2], now)) ++adds;
    }
    for (size_t l = 0; l < locals.size() && removes < 6; ++l) {
        if (lmatched[l]) continue;
        BaleTrack* t = BaleTrackOf(locals[l]);
        if (t && now - t->bornMs < 3000) continue;                                 // yeni dogan: olayi henuz host'a ulasmamis olabilir
        if (BaleRemoveLocal(game, locals[l])) ++removes;
    }
    if (adds || removes || fixes) LOGI("[BALE] uzlastirma: ekle=%d sil=%d duzelt=%d (host=%u yerel=%u)", adds, removes, fixes, (unsigned)hn, (unsigned)locals.size());
}

// Oyun durumu 6'da, her karede (my_GameUpdateStateBase, orijinal cagrildiktan SONRA).
static void BaleFrame(uintptr_t game) {
    if (!kSyncBales || game == 0) return;
    if (!g_IsConnected.load() || !g_VehNetArmed.load() || !g_GameAddBaleFn || !g_GameRemoveBaleFn || !g_BaleDestroyFn) {
        if (g_BaleTrkValid) {                                              // baglanti kesildi: uzaktan yonetilen balyalarin fizigini geri ac
            const uint32_t bn = BaleCountOf(game);
            for (size_t i = 0; i < g_BaleTrk.size(); ++i) {
                if (!g_BaleTrk[i].sensor) continue;
                for (uint32_t k = 0; k < bn; ++k)
                    if (BalePtrAt(game, k) == g_BaleTrk[i].p) { BaleSetSensor(g_BaleTrk[i].p, false); break; }
                g_BaleTrk[i].sensor = false; g_BaleTrk[i].hasTarget = false;
            }
        }
        g_BaleTrkValid = false;
        return;
    }
    const long long now = NowMs();
    BaleCapture(game, now);                                                // once yerel degisiklikleri yakala (yanki/cift silme icin)
    static std::vector<BaleOpPacket> ops;                                  // yeniden kullanilir
    std::vector<float> list;
    bool haveList = false;
    ops.clear();
    {
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        ops.swap(g_PendingBaleOps);
        if (g_HasPendingBaleList) { list.swap(g_PendingBaleList); g_HasPendingBaleList = false; haveList = true; }
    }
    const uint8_t localOwner = g_LocalPlayerId.load();
    for (size_t i = 0; i < ops.size(); ++i) BaleApplyOp(game, ops[i], localOwner, now);
    BaleSmoothStep(now);                                                   // uzaktan gelen hareketleri hedefe dogru akici yaklastir
    if (g_IsHost.load()) BaleHostBroadcast(game, now, g_EntityResendAll.load());
    else if (haveList) BaleReconcile(game, list, now);
}

// ========================================================================
// TARLA SATIN ALMA (sahiplik)
// Satin alma Game::updateGameLogicState icinde (onay diyalogu, Game+0x68 == 9): secili tarla kaydi Game+0xa2f8,
// sahiplik bayti Map + idx*100 + 0x2c (0 -> 1) ve para (Game+0x6a0) dusulur. Para zaten ORTAK PARA ile senkron;
// burada sadece sahiplik baytini yayiniyoruz. Her karede 17 kayitlik bayt dizisi bir onceki goruntuyle
// karsilastirilir (kanca YOK); 0 -> 1 olan = satin alindi. Host ayrica 2 sn'de bir sahiplik maskesini yollar (guvenlik agi).
// ========================================================================
static const bool kSyncFieldOwn = true;
static const uint32_t kFieldRecMax = 17;                    // Map::serialize 17 kayit yazar (0x6a4 / 100)
static uint8_t g_OwnSnap[kFieldRecMax];
static bool g_OwnSnapValid = false;
static uintptr_t g_OwnMapPtr = 0;

static uint32_t FieldRecCount(uintptr_t map) {
    const int32_t n = *(volatile int32_t*)(map + 0x28);
    return (n < 0) ? 0u : ((uint32_t)n > kFieldRecMax ? kFieldRecMax : (uint32_t)n);
}
static uint32_t FieldOwnMask(uintptr_t map) {
    uint32_t m = 0;
    const uint32_t n = FieldRecCount(map);
    for (uint32_t i = 0; i < n; ++i) if (*(volatile uint8_t*)(map + (uintptr_t)i * 100 + 0x2c) != 0) m |= (1u << i);
    return m;
}
static void FieldOwnApplyMask(uintptr_t map, uint32_t mask) {
    const uint32_t n = FieldRecCount(map);
    for (uint32_t i = 0; i < n; ++i) {
        if (!(mask & (1u << i))) continue;
        volatile uint8_t* b = (volatile uint8_t*)(map + (uintptr_t)i * 100 + 0x2c);
        if (*b == 0) {
            *b = 1;
            if (g_OwnSnapValid) g_OwnSnap[i] = 1;
            LOGI("[FIELDOWN] tarla %u sahipligi host maskesinden alindi", (unsigned)i);
        }
    }
}
static void FieldOwnFrame(uintptr_t game, uint32_t st) {
    if (!kSyncFieldOwn || game == 0) return;
    if (!g_IsConnected.load() || (st != 6 && st != 7)) { g_OwnSnapValid = false; return; }
    const uintptr_t map = *(uintptr_t*)(game + 0x704);
    if (!map) { g_OwnSnapValid = false; return; }
    const uint32_t n = FieldRecCount(map);
    if (map != g_OwnMapPtr) { g_OwnMapPtr = map; g_OwnSnapValid = false; }   // harita yeniden yuklendi (kayit alindi)
    if (!g_OwnSnapValid) {
        for (uint32_t i = 0; i < kFieldRecMax; ++i)
            g_OwnSnap[i] = (i < n) ? *(volatile uint8_t*)(map + (uintptr_t)i * 100 + 0x2c) : 0;
        g_OwnSnapValid = true;
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        g_PendingFieldOwn.clear();
        return;
    }
    // uzaktan gelen satin almalari uygula (para zaten delta ile geliyor)
    std::vector<FieldOwnPacket> ops;
    { std::lock_guard<std::mutex> lock(g_VehicleEventMutex); ops.swap(g_PendingFieldOwn); }
    for (size_t k = 0; k < ops.size(); ++k) {
        const FieldOwnPacket& op = ops[k];
        if (op.idx >= n || !op.owned) continue;
        volatile uint8_t* b = (volatile uint8_t*)(map + (uintptr_t)op.idx * 100 + 0x2c);
        if (*b == 0) {
            *b = 1;
            LOGI("[FIELDOWN] uzaktan tarla satin alma uygulandi: tarla=%u oyuncu=%u", (unsigned)op.idx, (unsigned)op.ownerId);
        }
        g_OwnSnap[op.idx] = 1;                                              // yerel degisiklik sanilip geri yayinlanmasin
    }
    // yerel degisiklikler
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t cur = *(volatile uint8_t*)(map + (uintptr_t)i * 100 + 0x2c);
        if (cur == g_OwnSnap[i]) continue;
        g_OwnSnap[i] = cur;
        if (cur != 0) {
            FieldOwnPacket p; memset(&p, 0, sizeof(p));
            p.type = PACKET_FIELD_OWN; p.ownerId = g_LocalPlayerId.load(); p.idx = (uint8_t)i; p.owned = 1;
            QueueVehicleEvent(&p, sizeof(p));
            LOGI("[FIELDOWN] yerel tarla satin alma yayinlandi: tarla=%u", (unsigned)i);
        }
    }
}

// ========================================================================
// TARLA BUYUME SAATI (mahsul durumu)
// Tarla islemleri (ekim/hasat/isleme) FIELD_OP ile zaten senkron. Mahsulun BUYUMESI ise her cihazda kendi
// Map zamanlayicisiyla ilerler (Map::update: Map+0xc88 sonraki tura kalan sure, +0xc90 islenen grup, -1 = bekliyor).
// Magaza/ayarlar (durum 7) acikken Map::update hic cagrilmaz -> o cihaz buyume turlarini KACIRIR.
//  1) Bagliyken durum 7'de Map::update(dt) biz cagiririz (saat icin TimeSyncFrame'in yaptiginin aynisi).
//  2) Host 2 sn'de bir (tur sayaci, idx, zamanlayici) yollar; client ayni fazda ise zamanlayicisini host'a ceker,
//     host'tan en az 1 tur geride kalmissa (>= 8 sn kalici) Map::growFieldsNow ile eksik turlari yetistirir.
// ========================================================================
static const bool kSyncMapClock = true;
typedef void (*Map_update_t)(void* map, uint32_t dtBits);
typedef void (*Map_grow_t)(void* map);
static Map_update_t g_MapUpdateFn = nullptr;              // Map::update(float)
static Map_grow_t g_MapGrowNowFn = nullptr;               // Map::growFieldsNow()
static uint32_t g_McCycles = 0;
static float g_McPrevTimer = 0.0f;
static bool g_McPrevValid = false;
static bool g_McOffsetSet = false;
static int64_t g_McOffset = 0;
static long long g_McBehindSinceMs = 0;
static long long g_McSendMs = 0;
static uint64_t g_McLastAdv = 0;
static bool g_McInfoLogged = false;

static void MapClockFrame(uintptr_t game, uint32_t st) {
    if (!kSyncMapClock || game == 0) return;
    if (!g_IsConnected.load() || (st != 6 && st != 7)) {
        g_McPrevValid = false; g_McOffsetSet = false; g_McBehindSinceMs = 0; g_McLastAdv = 0;
        return;
    }
    const uintptr_t map = *(uintptr_t*)(game + 0x704);
    if (!map) return;
    const uint64_t now = (uint64_t)NowMs();
    if (!g_McInfoLogged) {
        g_McInfoLogged = true;
        LOGI("[MAPCLK] Map::update=%p growFieldsNow=%p tur suresi=%.1f grup=%d", (void*)g_MapUpdateFn, (void*)g_MapGrowNowFn,
             (double)*(float*)(map + 0xc8c), (int)*(int32_t*)(map + 0x28));
    }
    if (st == 7 && g_MapUpdateFn) {
        if (g_McLastAdv != 0) {
            double dt = (double)(now - g_McLastAdv) / 1000.0;
            if (dt > 0.1) dt = 0.1;
            if (dt > 0.0) { const float f = (float)dt; uint32_t b; memcpy(&b, &f, sizeof(b)); g_MapUpdateFn((void*)map, b); }
        }
        g_McLastAdv = now;
    } else {
        g_McLastAdv = 0;
    }

    const float tm = *(volatile float*)(map + 0xc88);
    if (tm == tm) {
        if (g_McPrevValid && tm > g_McPrevTimer + 1.0f) ++g_McCycles;      // zamanlayici yeniden kuruldu = yeni buyume turu basladi
        g_McPrevTimer = tm; g_McPrevValid = true;
    }
    if (!g_McPrevValid) return;

    if (g_IsHost.load()) {
        if ((long long)now - g_McSendMs >= 2000) {
            g_McSendMs = (long long)now;
            MapClockPacket p; memset(&p, 0, sizeof(p));
            p.type = PACKET_MAP_CLOCK; p.originId = g_LocalPlayerId.load();
            p.cycles = g_McCycles; p.idx = *(volatile int32_t*)(map + 0xc90);
            memcpy(&p.timerBits, &tm, sizeof(tm));
            p.ownMask = FieldOwnMask(map);
            QueueVehicleEvent(&p, sizeof(p));
        }
        return;
    }
    MapClockPacket hp; bool have = false;
    {
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        if (g_HasPendingMapClock) { hp = g_PendingMapClock; g_HasPendingMapClock = false; have = true; }
    }
    if (!have) return;
    if (kSyncFieldOwn) FieldOwnApplyMask(map, hp.ownMask);
    float ht; memcpy(&ht, &hp.timerBits, sizeof(ht));
    if (!(ht == ht) || ht < 0.0f || ht > 1.0e6f) return;
    const float lt = g_McPrevTimer;
    const int32_t lidx = *(volatile int32_t*)(map + 0xc90);
    if (!g_McOffsetSet) {
        if (fabsf(ht - lt) < 3.0f && lidx < 0 && hp.idx < 0) {              // ayni fazdayiz: tur sayaclarini hizala
            g_McOffset = (int64_t)hp.cycles - (int64_t)g_McCycles;
            g_McOffsetSet = true; g_McBehindSinceMs = 0;
            LOGI("[MAPCLK] hizalandi: host tur=%u yerel tur=%u zamanlayici host=%.1f yerel=%.1f",
                 (unsigned)hp.cycles, (unsigned)g_McCycles, (double)ht, (double)lt);
        }
        return;
    }
    const int64_t behind = ((int64_t)hp.cycles - g_McOffset) - (int64_t)g_McCycles;
    if (behind == 0) {
        g_McBehindSinceMs = 0;
        if (lidx < 0 && hp.idx < 0 && fabsf(ht - lt) > 1.0f) {              // faz kaymasi: zamanlayiciyi host'a cek
            *(volatile float*)(map + 0xc88) = ht;
            g_McPrevTimer = ht;
            LOGI("[MAPCLK] zamanlayici host'a cekildi: fark=%.1f sn", (double)(ht - lt));
        }
    } else if (behind >= 1 && behind <= 3) {
        if (g_McBehindSinceMs == 0) g_McBehindSinceMs = (long long)now;
        if ((long long)now - g_McBehindSinceMs >= 8000 && g_MapGrowNowFn) {
            for (int64_t k = 0; k < behind; ++k) g_MapGrowNowFn((void*)map);
            g_McCycles += (uint32_t)behind;
            g_McBehindSinceMs = 0;
            LOGI("[MAPCLK] client %d tur geride kaldi: buyume yetistirildi", (int)behind);
        }
    } else if (behind > 3 || behind < -3) {
        g_McOffset = (int64_t)hp.cycles - (int64_t)g_McCycles;               // cok fark var: toplu buyutme yapma, yeniden hizala
        g_McBehindSinceMs = 0;
        LOGI("[MAPCLK] tur farki %d: yeniden hizalandi (toplu buyutme yapilmadi)", (int)behind);
    } else {
        g_McBehindSinceMs = 0;                                              // client biraz onde: bekle
    }
}

// ========================================================================
// FIYAT LISTESI (BUYUK TALEP) + GOREVLER
//  - Fiyat listesi: GreatDemandManager (Game+0x6d8). Alani: +0 sonraki rastgele talebe kalan sure, +4 sayi (en cok 2),
//    +8/+0xC urun A, +0x20/+0x24 urun tipi B, +0x10/+0x14 fiyat carpani, +0x18/+0x1C kalan sure, +0x28 "yeni talep" bayragi.
//    Carpan ayrica Map+0x724 tablosuna yazilir: tablo[(A+1)*0x80 + (B+0x16)*4] (suresi dolunca 1.0 yapilir).
//    Host'un listesi 5 sn'de bir (degisince hemen) client'a gider; client kendi rastgele talebini URETMEZ (sayac sonsuz).
//  - Gorev: MissionManager (Game+0x9c6c). +0xd4 durum (0 bos, 1 teklif, 2 aktif), +0xd8 zamanlayici, +0xdc esya tipi,
//    +0xe0 bitis noktasi. Teklif/kabul host'ta; client'lara sadece "aktif gorev" (nokta, esya, kalan sure) gider.
//    Client gorevi tamamlarsa (para oyunun kendi koduyla ortak para olarak zaten akar) host'a MISSION_DONE gider,
//    host gorevi kapatir. Yeni kanca YOK: setMissionSpawnPoint ve cancelPending sadece CAGRILIR.
// ========================================================================
static const bool kSyncDemand = true;
static const bool kSyncMission = true;
struct __attribute__((packed)) DemandEntry { int32_t a; int32_t b; uint32_t multBits; uint32_t timerBits; };
struct __attribute__((packed)) DemandPacket { uint8_t type; uint8_t count; DemandEntry e[2]; };
struct __attribute__((packed)) MissionPacket { uint8_t type; uint8_t state; uint8_t item; uint8_t spawn; uint32_t timerBits; };
struct __attribute__((packed)) MissionDonePacket { uint8_t type; uint8_t ownerId; };
static DemandPacket g_PendingDemand;                  // g_VehicleEventMutex ile korunur
static bool g_HasPendingDemand = false;
static MissionPacket g_PendingMission;                // g_VehicleEventMutex ile korunur
static bool g_HasPendingMission = false;
static std::atomic<bool> g_MissionDoneFlag(false);    // host: bir client gorevi tamamladi
typedef void (*Mission_setSpawn_t)(void* mm, uint32_t idx);
typedef void (*Mission_cancel_t)(void* mm);
static Mission_setSpawn_t g_MissionSetSpawnFn = nullptr;   // MissionManager::setMissionSpawnPoint(unsigned int)
static Mission_cancel_t g_MissionCancelFn = nullptr;       // MissionManager::cancelPending()
static bool g_MsPrevValid = false;
static uint32_t g_MsPrevState = 0;
static float g_MsPrevTimer = 0.0f;
static long long g_MsIgnoreUntil = 0;
static long long g_DemandSendMs = 0;
static uint32_t g_DemandHash = 0;
static long long g_MissionSendMs = 0;
static int g_MsSentState = -1, g_MsSentItem = -1, g_MsSentSpawn = -1;

static bool DemandSlot(uintptr_t map, int32_t a, int32_t b, volatile float** out) {
    if (!map || a < 0 || b < 0 || b > 3) return false;
    const uintptr_t tbl = *(volatile uintptr_t*)(map + 0x724);
    const int32_t cnt = *(volatile int32_t*)(map + 0x728);
    if (!tbl || cnt < 4 || a > cnt - 4) return false;
    *out = (volatile float*)(tbl + (uintptr_t)(a + 1) * 0x80 + (uintptr_t)(b + 0x16) * 4);
    return true;
}

// Client: host'un fiyat listesini oyunun kendi yapisina yaz (eski talepleri 1.0'a dondur, yenileri uygula).
static void DemandApply(uintptr_t game, const DemandPacket& p) {
    const uintptr_t gd = game + 0x6d8;
    const uintptr_t map = *(uintptr_t*)(game + 0x704);
    if (!map) return;
    uint32_t oldN = *(volatile uint32_t*)(gd + 4);
    if (oldN > 2) oldN = 0;
    int32_t oa[2] = {-1, -1}, ob[2] = {-1, -1};
    for (uint32_t i = 0; i < oldN; ++i) {
        oa[i] = *(volatile int32_t*)(gd + 8 + 4 * i);
        ob[i] = *(volatile int32_t*)(gd + 0x20 + 4 * i);
        volatile float* s;
        if (DemandSlot(map, oa[i], ob[i], &s)) *s = 1.0f;
    }
    uint32_t n = 0;
    bool isNew = false;
    const uint32_t cnt = p.count > 2 ? 2 : p.count;
    for (uint32_t i = 0; i < cnt; ++i) {
        const int32_t a = p.e[i].a, b = p.e[i].b;
        volatile float* s;
        if (!DemandSlot(map, a, b, &s)) continue;
        float mult; memcpy(&mult, &p.e[i].multBits, sizeof(mult));
        float tmr; memcpy(&tmr, &p.e[i].timerBits, sizeof(tmr));
        if (!(mult == mult) || !(tmr == tmr) || mult < 0.0f || mult > 100.0f || tmr > 1.0e6f) continue;
        *(volatile int32_t*)(gd + 8 + 4 * n) = a;
        *(volatile int32_t*)(gd + 0x20 + 4 * n) = b;
        *(volatile uint32_t*)(gd + 0x10 + 4 * n) = p.e[i].multBits;
        *(volatile uint32_t*)(gd + 0x18 + 4 * n) = p.e[i].timerBits;
        *s = mult;
        bool known = false;
        for (uint32_t k = 0; k < oldN; ++k) if (oa[k] == a && ob[k] == b) known = true;
        if (!known) isNew = true;
        ++n;
    }
    *(volatile uint32_t*)(gd + 4) = n;
    if (isNew) *(volatile uint8_t*)(gd + 0x28) = 1;       // oyunun "yeni talep" bildirimi
    if (isNew || n != oldN) LOGI("[DEMAND] client: host fiyat listesi uygulandi (talep=%u yeni=%d)", (unsigned)n, (int)isNew);   // sadece degisimde
}

// Oyun thread'i, Game::updateStateBase orijinalinden ONCE (durum 6). Sadece client icin is yapar.
static void MiscSyncPre(uintptr_t game) {
    if (game == 0 || !g_IsConnected.load() || !g_IsClient.load()) { g_MsPrevValid = false; return; }
    const uintptr_t gd = game + 0x6d8;
    const uintptr_t mm = game + 0x9c6c;
    if (kSyncDemand) {
        *(volatile uint32_t*)gd = 0x7f7fffff;             // client kendi rastgele talebini uretmesin
        DemandPacket dp; bool have = false;
        {
            std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
            if (g_HasPendingDemand) { dp = g_PendingDemand; g_HasPendingDemand = false; have = true; }
        }
        if (have) DemandApply(game, dp);
    }
    if (kSyncMission && g_MissionSetSpawnFn) {
        MissionPacket mp; bool have = false;
        {
            std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
            if (g_HasPendingMission) { mp = g_PendingMission; g_HasPendingMission = false; have = true; }
        }
        const uint32_t cs = *(volatile uint32_t*)(mm + 0xd4);
        if (have) {
            if (mp.state == 2) {
                float ht; memcpy(&ht, &mp.timerBits, sizeof(ht));
                if (NowMs() >= g_MsIgnoreUntil && mp.spawn < 13 && mp.item < 5 && ht == ht && ht > 0.0f && ht < 1.0e5f) {
                    if (cs != 2 || *(volatile uint32_t*)(mm + 0xe0) != mp.spawn || *(volatile uint32_t*)(mm + 0xdc) != mp.item) {
                        *(volatile uint32_t*)(mm + 0xdc) = mp.item;
                        g_MissionSetSpawnFn((void*)mm, mp.spawn);
                        *(volatile uint32_t*)(mm + 0xd4) = 2;
                        LOGI("[MISSION] client: host gorevi basladi nokta=%u esya=%u sure=%.0f", (unsigned)mp.spawn, (unsigned)mp.item, (double)ht);
                    }
                    *(volatile float*)(mm + 0xd8) = ht;   // kalan sureyi host'a esitle
                }
            } else if (cs == 2) {
                *(volatile uint32_t*)(mm + 0xd4) = 0;
                LOGI("[MISSION] client: host gorevi bitti");
            }
        }
        // Client kendi gorev teklifi uretmesin: aktif degilken sayac sonsuz, teklif durumu (1) olmaz.
        const uint32_t s2 = *(volatile uint32_t*)(mm + 0xd4);
        if (s2 != 2) {
            if (s2 != 0) *(volatile uint32_t*)(mm + 0xd4) = 0;
            *(volatile float*)(mm + 0xd8) = 1.0e9f;
        }
        g_MsPrevState = *(volatile uint32_t*)(mm + 0xd4);
        g_MsPrevTimer = *(volatile float*)(mm + 0xd8);
        g_MsPrevValid = true;
    }
}

// Oyun thread'i, Game::updateStateBase orijinalinden SONRA (durum 6).
static void MiscSyncPost(uintptr_t game) {
    if (game == 0 || !g_IsConnected.load()) return;
    const uintptr_t gd = game + 0x6d8;
    const uintptr_t mm = game + 0x9c6c;
    const long long now = NowMs();
    if (g_IsHost.load()) {
        if (kSyncDemand) {
            uint32_t n = *(volatile uint32_t*)(gd + 4);
            if (n > 2) n = 0;
            DemandPacket p; memset(&p, 0, sizeof(p));
            p.type = PACKET_DEMAND; p.count = (uint8_t)n;
            int32_t key[5]; memset(key, 0, sizeof(key)); key[0] = (int32_t)n;
            for (uint32_t i = 0; i < n; ++i) {
                p.e[i].a = *(volatile int32_t*)(gd + 8 + 4 * i);
                p.e[i].b = *(volatile int32_t*)(gd + 0x20 + 4 * i);
                p.e[i].multBits = *(volatile uint32_t*)(gd + 0x10 + 4 * i);
                p.e[i].timerBits = *(volatile uint32_t*)(gd + 0x18 + 4 * i);
                key[1 + i * 2] = p.e[i].a; key[2 + i * 2] = p.e[i].b;
            }
            uint32_t h = 2166136261u;                         // FNV-1a
            for (size_t k = 0; k < sizeof(key); ++k) { h ^= ((const uint8_t*)key)[k]; h *= 16777619u; }
            if (h != g_DemandHash || now - g_DemandSendMs >= 10000) {   // degisimde hemen, aksi halde 10 sn'de bir kalp atisi
                g_DemandHash = h; g_DemandSendMs = now;
                QueueVehicleEvent(&p, sizeof(p));
            }
        }
        if (kSyncMission && g_MissionSetSpawnFn) {
            uint32_t st = *(volatile uint32_t*)(mm + 0xd4);
            if (g_MissionDoneFlag.exchange(false) && st == 2) {
                // Client gorevi tamamladi: gorevi kapat ve oyunun kendi rastgele bekleme suresini kur (cancelPending: durum 1 -> 0).
                if (g_MissionCancelFn) {
                    *(volatile uint32_t*)(mm + 0xd4) = 1;
                    g_MissionCancelFn((void*)mm);
                } else {
                    *(volatile uint32_t*)(mm + 0xd4) = 0;
                    *(volatile float*)(mm + 0xd8) = 420.0f;
                }
                st = *(volatile uint32_t*)(mm + 0xd4);
                LOGI("[MISSION] host: client gorevi tamamladi, gorev kapatildi");
            }
            const int s = (st == 2) ? 2 : 0;
            const int item = (int)*(volatile uint32_t*)(mm + 0xdc);
            const int spawn = (int)*(volatile uint32_t*)(mm + 0xe0);
            const bool changed = (s != g_MsSentState) || (s == 2 && (item != g_MsSentItem || spawn != g_MsSentSpawn));
            if (changed || now - g_MissionSendMs >= 3000) {
                g_MsSentState = s; g_MsSentItem = item; g_MsSentSpawn = spawn; g_MissionSendMs = now;
                MissionPacket p; memset(&p, 0, sizeof(p));
                p.type = PACKET_MISSION; p.state = (uint8_t)s;
                p.item = (uint8_t)(item < 0 || item > 255 ? 0 : item);
                p.spawn = (uint8_t)(spawn < 0 || spawn > 255 ? 0 : spawn);
                if (s == 2) p.timerBits = *(volatile uint32_t*)(mm + 0xd8);
                QueueVehicleEvent(&p, sizeof(p));
                if (changed) LOGI("[MISSION] host: durum yayinlandi state=%d nokta=%d esya=%d", s, spawn, item);
            }
        }
    } else if (kSyncMission && g_MissionSetSpawnFn && g_MsPrevValid) {
        const uint32_t cur = *(volatile uint32_t*)(mm + 0xd4);
        // Aktif gorev bu karede "sure dolmadan" bitti = bu client tamamladi (para oyunun kendi kodundan geldi).
        if (g_MsPrevState == 2 && cur == 0 && g_MsPrevTimer > 0.25f) {
            MissionDonePacket p; memset(&p, 0, sizeof(p));
            p.type = PACKET_MISSION_DONE; p.ownerId = g_LocalPlayerId.load();
            QueueVehicleEvent(&p, sizeof(p));
            g_MsIgnoreUntil = now + 2500;                  // host kapatana kadar eski "aktif" paketlerini yok say
            LOGI("[MISSION] client: gorevi tamamladim, host'a bildirildi");
        }
    }
}

// ========================================================================
// TARLA DURUMU GORUNTUSU — SADECE "DURAKLAMA DEVRI" ICIN
// Tarla islemi her cihazda yerel calisir (arac her cihazda hareket ettigi icin tarla orada da islenir) ve FIELD_OP ile
// desteklenir. Surekli tarla verisi YOLLANMAZ. Goruntu yalnizca su durumda gider: araci/iscisi tarlayi isleyen cihaz
// oyunu durdurursa (ayarlar, dukkan, arka plana alma, kisa kopma) o cihazin AG THREAD'i, son kendi islem yaptigi tarlalarin
// son halini yollar; diger cihazlar bunu uygular ve oyundaki aktif biri isi devralir. Herkes ayarlardaysa zaten herkeste
// oyun durur, sorun olmaz.
// Veri: her Field icin iki bit dizisi (Field+4: 384 bayt, Field+0x14: 48 bayt; BitVector = {veri*, bit, bayt, bayrak}).
// Tarlalar Map+0x18 GridCell dizisinden (sayac Map+0x24, hucre 100 bayt, hucre+0x48 = Field*) bulunur.
// Oyun thread'i saniyede bir SADECE YEREL olarak "bu cihazin son 30 sn'de isledigi tarlalari" isaretler (ag yok).
// Alici: kendisi son 3 sn'de yerel tarla isi yaptiysa goruntuyu atlar; aksi halde Field::deserialize(false) ile uygular.
// ========================================================================
static const bool kSyncFieldSnap = true;
static const uint32_t kFsB1 = 384, kFsB2 = 48;
struct __attribute__((packed)) FieldSnapPacket { uint8_t type; uint8_t ownerId; uint16_t idx; uint8_t d[kFsB1 + kFsB2]; };
static std::vector<FieldSnapPacket> g_PendingFieldSnaps;      // g_VehicleEventMutex ile korunur
typedef uint8_t* (*Field_deserialize_t)(void* field, uint8_t* buf, bool full);
static Field_deserialize_t g_FieldDeserFn = nullptr;          // Field::deserialize(unsigned char*, bool)
static const uint32_t kFsMaxCells = 65535;                    // Field kimligi = GridCell sirasi (uint16)
static const long long kFsDirtyWindowMs = 30000;              // bu kadar sure icinde islenen tarlalar duraklamada yollanir
static const long long kFsApplyQuietMs = 3000;                // alici: yerel tarla isinden bu kadar sonra uygular
static const size_t kFsFlushMax = 24;                         // bir duraklamada en fazla bu kadar tarla
static std::mutex g_FsMutex;                                  // g_FsKnown / g_FsDirtyMs / g_FsFields
static std::vector<uint32_t> g_FsKnown;                       // son gorulen ozet (yerel degisiklik tespiti icin)
static std::vector<long long> g_FsDirtyMs;                    // yerel isleme zamani; 0 = temiz
static std::vector<uint16_t> g_FsFields;                      // Field* bulunan hucrelerin sirasi
static std::atomic<uintptr_t> g_FsMap(0);
static std::atomic<bool> g_FsValid(false);
static long long g_FsScanMs = 0;
static bool g_FsBad = false;
static std::atomic<bool> g_FsResendAll(false);                // geri donen cihaz icin: TUM tarlalari tekrar yolla
static std::vector<uint16_t> g_FsResendQ;                     // sadece oyun thread'i
static uint32_t g_FsStatSent = 0, g_FsStatApplied = 0, g_FsStatRecv = 0, g_FsStatSkipped = 0;

static uintptr_t FsFieldAt(uintptr_t map, uint32_t i) {
    const uintptr_t cells = *(volatile uintptr_t*)(map + 0x18);
    const uint32_t cnt = *(volatile uint32_t*)(map + 0x24);
    if (!cells || i >= cnt || i >= kFsMaxCells) return 0;
    return *(volatile uintptr_t*)(cells + (uintptr_t)i * 100 + 0x48);
}
// Tarlanin iki bit dizisini d[432]'ye kopyalar; boyutlar beklenenle uyusmazsa false.
static bool FsRead(uintptr_t f, uint8_t* d) {
    const uint8_t* d1 = (const uint8_t*)*(volatile uintptr_t*)(f + 4);
    const uint32_t n1 = *(volatile uint32_t*)(f + 0xC);
    const uint8_t* d2 = (const uint8_t*)*(volatile uintptr_t*)(f + 0x14);
    const uint32_t n2 = *(volatile uint32_t*)(f + 0x1C);
    if (!d1 || !d2 || n1 != kFsB1 || n2 != kFsB2) return false;
    memcpy(d, d1, kFsB1);
    memcpy(d + kFsB1, d2, kFsB2);
    return true;
}
static uint32_t FsHash(const uint8_t* d) {
    uint32_t h = 2166136261u;
    for (uint32_t k = 0; k < kFsB1 + kFsB2; ++k) { h ^= d[k]; h *= 16777619u; }
    return h;
}
static bool QueueFieldSnap(uint16_t idx, const uint8_t* d) {
    if (!g_IsConnected.load()) return false;
    FieldSnapPacket p; memset(&p, 0, sizeof(p));
    p.type = PACKET_FIELD_SNAP; p.ownerId = g_LocalPlayerId.load(); p.idx = idx;
    memcpy(p.d, d, sizeof(p.d));
    std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
    if (g_FieldOpOut.size() > 5000) return false;          // tampon dolu: sonraki turda tekrar dene
    const uint8_t* b = (const uint8_t*)&p;
    g_FieldOpOut.insert(g_FieldOpOut.end(), b, b + sizeof(p));
    return true;
}

// Oyun thread'i, durum 6 (Game::updateStateBase sonrasi). Ag trafigi URETMEZ.
static void FieldSnapFrame(uintptr_t game) {
    if (!kSyncFieldSnap || game == 0 || g_FsBad) return;
    if (!g_IsConnected.load()) { g_FsValid.store(false); return; }
    const uintptr_t map = *(uintptr_t*)(game + 0x704);
    if (!map) { g_FsValid.store(false); return; }
    const long long now = NowMs();
    if (map != g_FsMap.load()) { g_FsValid.store(false); g_FsMap.store(map); }
    const uint32_t cnt = *(volatile uint32_t*)(map + 0x24);
    if (cnt == 0 || cnt > kFsMaxCells) return;
    const long long localOp = g_LastLocalFieldOpMs.load();

    // 1) Duraklayan cihazdan gelen tarla goruntulerini uygula (kendimiz calismiyorsak).
    std::vector<FieldSnapPacket> snaps;
    {
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        snaps.swap(g_PendingFieldSnaps);
    }
    if (!snaps.empty()) {
        g_FsStatRecv += (uint32_t)snaps.size();
        const bool quiet = localOp == 0 || (now - localOp) >= kFsApplyQuietMs;
        if (g_FsValid.load() && g_FieldDeserFn && quiet) {
            for (size_t k = 0; k < snaps.size(); ++k) {
                FieldSnapPacket& sp = snaps[k];
                const uintptr_t f = FsFieldAt(map, sp.idx);
                if (!f) continue;
                uint8_t cur[kFsB1 + kFsB2];
                if (!FsRead(f, cur) || FsHash(cur) == FsHash(sp.d)) continue;
                g_FieldDeserFn((void*)f, sp.d, false);
                uint8_t after[kFsB1 + kFsB2];
                if (FsRead(f, after)) {
                    std::lock_guard<std::mutex> lock(g_FsMutex);
                    if (sp.idx < g_FsKnown.size()) g_FsKnown[sp.idx] = FsHash(after);   // yerel degisiklik sanilmasin
                }
                ++g_FsStatApplied;
            }
        } else {
            g_FsStatSkipped += (uint32_t)snaps.size();
        }
        LOGI("[FIELDSNAP] duraklama devri alindi=%u uygulanan=%u atlanan=%u (yerel is sessiz=%d)", (unsigned)snaps.size(),
             (unsigned)g_FsStatApplied, (unsigned)g_FsStatSkipped, (int)quiet);
        g_FsStatApplied = g_FsStatSkipped = 0;
    }

    // 1b) Geri donen (arka plandan / ayarlardan) cihaz icin tum tarlalari kare basina birkac tane yolla.
    if (g_FsValid.load() && g_FsResendAll.exchange(false)) {
        std::lock_guard<std::mutex> lock(g_FsMutex);
        g_FsResendQ = g_FsFields;
        LOGI("[FIELDSNAP] geri donen cihaz icin %u tarla yollanacak", (unsigned)g_FsResendQ.size());
    }
    for (int n = 0; n < 6 && !g_FsResendQ.empty(); ++n) {
        const uint16_t i = g_FsResendQ.back();
        const uintptr_t f = FsFieldAt(map, i);
        uint8_t d[kFsB1 + kFsB2];
        if (!f || !FsRead(f, d)) { g_FsResendQ.pop_back(); continue; }
        if (!QueueFieldSnap(i, d)) break;                                  // tampon dolu: sonraki karede
        g_FsResendQ.pop_back();
    }

    // 2) Saniyede bir YEREL tarama: bu cihazin son zamanlarda isledigi tarlalari isaretle.
    if (now - g_FsScanMs < 1000) return;
    g_FsScanMs = now;
    std::lock_guard<std::mutex> lock(g_FsMutex);
    if (!g_FsValid.load()) {
        g_FsKnown.assign(cnt, 0);
        g_FsDirtyMs.assign(cnt, 0);
        g_FsFields.clear();
        for (uint32_t i = 0; i < cnt; ++i) {
            const uintptr_t f = FsFieldAt(map, i);
            if (!f) continue;
            uint8_t d[kFsB1 + kFsB2];
            if (!FsRead(f, d)) {
                if (g_FsFields.empty()) { LOGI("[FIELDSNAP] beklenmeyen Field bit dizisi boyutu: ozellik kapatildi"); g_FsBad = true; return; }
                continue;
            }
            g_FsKnown[i] = FsHash(d);
            g_FsFields.push_back((uint16_t)i);
        }
        g_FsValid.store(true);
        LOGI("[FIELDSNAP] hazir (sadece duraklama devri): %u tarla, hucre=%u deserialize=%p", (unsigned)g_FsFields.size(),
             (unsigned)cnt, (void*)g_FieldDeserFn);
        return;
    }
    const bool localRecent = localOp != 0 && (now - localOp) < 4000;
    for (size_t k = 0; k < g_FsFields.size(); ++k) {
        const uint32_t i = g_FsFields[k];
        if (i >= g_FsKnown.size()) continue;
        const uintptr_t f = FsFieldAt(map, i);
        if (!f) continue;
        uint8_t d[kFsB1 + kFsB2];
        if (!FsRead(f, d)) continue;
        const uint32_t h = FsHash(d);
        if (h == g_FsKnown[i]) continue;
        g_FsKnown[i] = h;
        if (localRecent) g_FsDirtyMs[i] = now;
    }
}

// AG THREAD'i (BuildOutgoing basinda, hem host hem client). Oyun dongusu (updateStateBase) durduysa — ayarlar, dukkan, arka plan,
// kisa kopma — bu cihazin son isledigi tarlalarin son halini bir kez yollar. Oyun durdugu icin tarla verisi bu sirada degismez.
static void FieldSnapPauseTick() {
    if (!kSyncFieldSnap || g_FsBad) return;
    static bool s_paused = false;
    static std::vector<uint16_t> s_flushQ;
    const long long now = NowMs();
    const long long sb = g_LastStateBaseMs.load();
    const bool paused = g_LocalInGame.load() && g_IsConnected.load() && sb > 0 && (now - sb) > HOST_PAUSE_DETECT_MS;
    if (paused && !s_paused) {
        s_paused = true;
        s_flushQ.clear();
        if (g_FsValid.load()) {
            std::vector<std::pair<long long, uint16_t> > dirty;
            {
                std::lock_guard<std::mutex> lock(g_FsMutex);
                for (size_t i = 0; i < g_FsDirtyMs.size(); ++i)
                    if (g_FsDirtyMs[i] != 0 && now - g_FsDirtyMs[i] < kFsDirtyWindowMs) dirty.push_back(std::make_pair(g_FsDirtyMs[i], (uint16_t)i));
            }
            std::sort(dirty.begin(), dirty.end());                              // en eski basta
            const size_t from = dirty.size() > kFsFlushMax ? dirty.size() - kFsFlushMax : 0;   // en yeni kFsFlushMax tarla
            for (size_t k = from; k < dirty.size(); ++k) s_flushQ.push_back(dirty[k].second);
        }
        LOGI("[FIELDSNAP] oyun durdu: %u tarlanin son hali yollanacak", (unsigned)s_flushQ.size());
    } else if (!paused && s_paused) {
        s_paused = false;
        s_flushQ.clear();
        std::lock_guard<std::mutex> lock(g_FsMutex);
        std::fill(g_FsDirtyMs.begin(), g_FsDirtyMs.end(), 0LL);                // yollandi / gecerliligi bitti
    }
    if (!s_paused || s_flushQ.empty()) return;
    const uintptr_t map = g_FsMap.load();
    if (!map) { s_flushQ.clear(); return; }
    for (int n = 0; n < 8 && !s_flushQ.empty(); ++n) {
        const uint16_t i = s_flushQ.back();
        const uintptr_t f = FsFieldAt(map, i);
        uint8_t d[kFsB1 + kFsB2];
        if (!f || !FsRead(f, d)) { s_flushQ.pop_back(); continue; }
        if (!QueueFieldSnap(i, d)) break;                                       // tampon dolu: sonraki turda
        s_flushQ.pop_back();
        ++g_FsStatSent;
        if (s_flushQ.empty()) { LOGI("[FIELDSNAP] duraklama devri yollandi (toplam=%u)", (unsigned)g_FsStatSent); g_FsStatSent = 0; }
    }
}

// AG THREAD'i (BuildOutgoing basinda, hem host hem client). Oyun dongusu durduysa (ayarlar, dukkan, arka plan) bu cihazin
// sahip oldugu TUM araclarin sahipligi birakilir; yoksa arac baska oyunculara kilitli kalir. Host dogrudan birakir,
// client tek yuvali release paketiyle sirayla (yuva bosalinca bir sonraki) birakir.
static void PauseReleaseTick() {
    static bool s_paused = false;
    static std::vector<uint16_t> s_q;
    const long long now = NowMs();
    const long long sb = g_LastStateBaseMs.load();
    const bool paused = g_LocalInGame.load() && g_IsConnected.load() && g_VehNetArmed.load() && sb > 0 && (now - sb) > HOST_PAUSE_DETECT_MS;
    const uint8_t me = g_LocalPlayerId.load();
    if (paused && !s_paused) {
        s_paused = true;
        s_q.clear();
        if (me < MAX_PLAYERS) {
            std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
            for (uint16_t id = 0; id < VEHICLE_SLOT_LIMIT; ++id)
                if (g_VehicleAuthority[id].ownerId == me) s_q.push_back(id);
        }
        LOGI("[PAUSE] oyun durdu: %u aracin sahipligi birakilacak", (unsigned)s_q.size());
        if (g_IsHost.load()) {
            for (size_t k = 0; k < s_q.size(); ++k) ReleaseVehicleAuthority(s_q[k], me, true);
            if (!s_q.empty()) g_HostForceTable.store(true);
            s_q.clear();
        }
    } else if (!paused && s_paused) {
        s_paused = false;
        s_q.clear();
    }
    if (!s_paused || s_q.empty() || g_IsHost.load()) return;
    bool slotFree;
    { std::lock_guard<std::mutex> lock(g_VehicleSendMutex); slotFree = !g_HasPendingReleasePacket; }
    if (!slotFree) return;
    const uint16_t id = s_q.back();
    s_q.pop_back();
    QueueVehicleRelease(id);
    std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
    g_ClaimPending[id] = true;
}

// ========================================================================
// GERI DONUS (RESUME) SENKRONU
// Oyun arka plana atilinca (ya da ayarlar uzun acik kalinca) bu cihazda her sey durur; bu sirada tarla islemleri / arac
// konumlari kacirilir. Oyun tekrar calismaya basladigi an (updateStateBase bosluktan sonra ilk kez calisir):
//  - client: host'tan "son tarla durumu + duran araclarin son konumlari" ister (PACKET_RESUME),
//  - host: client'lardan ayni seyi ister (PACKET_HOST_RESUME).
// Cevap veren cihaz TUM tarlalarini (FIELD_SNAP) ve sahipsiz araclarinin konumunu (REST_POSE) yollar.
// ========================================================================
struct __attribute__((packed)) RestPoseHeader { uint8_t type; uint8_t count; };
struct __attribute__((packed)) RestPoseEntry { uint16_t id; float x, y, a; };
static const uint32_t kRestPoseMax = 32;
static std::atomic<bool> g_RestResendAll(false);
static std::vector<RestPoseEntry> g_PendingRestPoses;          // g_VehicleEventMutex ile korunur

static void SendRestPoses(uintptr_t game) {
    uint8_t buf[sizeof(RestPoseHeader) + kRestPoseMax * sizeof(RestPoseEntry)];
    RestPoseHeader h; h.type = PACKET_REST_POSE; h.count = 0;
    size_t off = sizeof(h);
    const uint32_t vn = VehicleCount(game);
    for (uint32_t i = 0; i < vn && h.count < kRestPoseMax; ++i) {
        const uintptr_t v = RawVehicleSlot(game, i);
        if (!v) continue;
        const int nid = NetIdOfVehicle(v);
        if (nid < 0) continue;
        uint8_t owner;
        { std::lock_guard<std::mutex> lock(g_VehicleStateMutex); owner = g_VehicleAuthority[nid].ownerId; }
        if (owner != VEHICLE_OWNER_NONE) continue;                         // sahipli arac normal akisla gelir
        const uintptr_t body = *(uintptr_t*)(v + 0x528);
        if (!body) continue;
        RestPoseEntry e;
        e.id = (uint16_t)nid; e.x = *(float*)(body + 0x0C); e.y = *(float*)(body + 0x10); e.a = *(float*)(body + 0x40);
        if (!(e.x == e.x) || !(e.y == e.y) || !(e.a == e.a)) continue;
        memcpy(buf + off, &e, sizeof(e));
        off += sizeof(e); ++h.count;
    }
    if (h.count == 0) return;
    memcpy(buf, &h, sizeof(h));
    QueueVehicleEvent(buf, off);
    LOGI("[RESUME] %u sahipsiz aracin konumu yollandi", (unsigned)h.count);
}

static void ApplyRestPoses(uintptr_t game) {
    std::vector<RestPoseEntry> poses;
    {
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        poses.swap(g_PendingRestPoses);
    }
    if (poses.empty() || !g_b2BodySetTransform) return;
    int applied = 0;
    for (size_t k = 0; k < poses.size(); ++k) {
        const RestPoseEntry& e = poses[k];
        if (e.id >= VEHICLE_SLOT_LIMIT || !(e.x == e.x) || !(e.y == e.y) || !(e.a == e.a)) continue;
        if (fabsf(e.x) > 1.0e5f || fabsf(e.y) > 1.0e5f) continue;
        uint8_t owner;
        { std::lock_guard<std::mutex> lock(g_VehicleStateMutex); owner = g_VehicleAuthority[e.id].ownerId; }
        if (owner != VEHICLE_OWNER_NONE) continue;                         // sahibi var: konumunu sahibi belirler
        if ((int)e.id == *(volatile int*)&g_LocalActiveNetId) continue;    // bu cihazda surulen arac
        const uintptr_t v = GetVehicleFromIndex(game, e.id);
        if (!v || *(uint32_t*)(v + 0x730) != 0) continue;                  // isci calisan araca dokunma
        const uintptr_t body = *(uintptr_t*)(v + 0x528);
        if (!body) continue;
        const float dx = *(float*)(body + 0x0C) - e.x, dy = *(float*)(body + 0x10) - e.y;
        if (dx * dx + dy * dy < 0.09f && GetAngleDelta(*(float*)(body + 0x40), e.a) < 0.05f) continue;
        TestB2Vec2 pos; pos.x = e.x; pos.y = e.y;
        g_b2BodySetTransform((void*)body, &pos, e.a);
        SetRemoteBodyVelocity(body, 0.0f, 0.0f, 0.0f);
        ++applied;
    }
    if (applied) LOGI("[RESUME] %d aracin son konumu uygulandi", applied);
}

// ========================================================================
// NPC TRAFIK SENKRONU
// Trafik araclari (yollardaki yolcu/kamyon) Game+0x600 sayaci + Game+0x604 havuzunda (en fazla 10) durur ve YALNIZCA
// o cihazin aktif aracinin ~91 m civarinda dogar/yasar. Bu yuzden her cihazin trafigi farklidir.
// Cozum: her cihaz kendi gercek trafik araclarini (konum/hiz/yol/tip) 8 Hz yayinlar; diger cihazlar bunlari "ayna" arac olarak
// dogurur (Game::addTrafficVehicle), konumunu gelen akisa gore yumusakca surer ve carpismasiz (sensor) birakir.
// Aynalar TrafficVehicle::update kancasinda yonetilir: yakin/taze oldukca yasar, akis kesilince ya da uzaklasinca oyunun kendi
// dongusu tarafindan kaldirilir (update false doner).
// ========================================================================
static const bool kSyncTraffic = true;
typedef int (*Game_addTraffic_t)(void* game, uint32_t type, uint32_t road, uint32_t tBits, uint32_t dir,
                                 const float* pos3, uint32_t angBits, uint32_t zero);
typedef uint32_t (*TrafficUpdate_t)(void* self, uint32_t dtBits, uint32_t gamePtr);
static Game_addTraffic_t g_GameAddTrafficFn = nullptr;
static TrafficUpdate_t orig_TrafficUpdate = nullptr;
static bool g_TrafficHookOk = false;
struct TrafficPending { uint8_t owner; TrafficEntry e; };
static std::vector<TrafficPending> g_PendingTraffic;           // g_VehicleEventMutex ile korunur

static const uintptr_t kTrCountOff = 0x600;                    // Game+0x600: trafik arac sayisi
static const uintptr_t kTrArrOff = 0x604;                      // Game+0x604 + i*4: TrafficVehicle* havuzu
static const int kTrPoolSize = 5;                              // Game constructor'i SADECE 5 TrafficVehicle olusturur (0x604..0x617); sonrasi baska veri
static const int kTrMirrorMax = 5;                             // takipci cihazda yerel trafik kapatildigi icin havuzun tamami ayna olabilir
static std::atomic<long long> g_TrLastRxMs(0);                 // baska bir cihazdan son trafik paketi (bos "kalp atisi" dahil)
static bool g_TrFollower = false;                              // bu cihaz baska birinin trafigini izliyor: yerel trafik kapali
static const float kTrClusterR2 = 220.0f * 220.0f;             // iki oyuncunun aktif araci bu kadar yakinsa ayni "bolge" sayilir
static const float kTrSpawnR2 = 120.0f * 120.0f;               // ayna sadece yerel aktif aracin 120 m yakininda dogar
static const float kTrKeepR2 = 150.0f * 150.0f;                // 150 m'den uzaklasinca kaldirilir
struct TrafficMirror { bool used; uintptr_t ptr; uint8_t owner; uint16_t id; float x, y, a, vx, vy; long long rxMs; };
static TrafficMirror g_TrMir[kTrMirrorMax];
struct TrafficSendId { uintptr_t p; uint16_t id; float x, y; };
static TrafficSendId g_TrSendIds[12];
static uint16_t g_TrNextId = 1;
static long long g_TrLastSendMs = 0;
static int g_TrLogs = 0;

static TrafficMirror* TrafficMirrorOfPtr(uintptr_t p) {
    if (!p) return nullptr;
    for (int i = 0; i < kTrMirrorMax; ++i) if (g_TrMir[i].used && g_TrMir[i].ptr == p) return &g_TrMir[i];
    return nullptr;
}

static bool LocalActiveBodyPos(uintptr_t game, float* x, float* y) {
    const uint32_t ai = *(uint32_t*)(game + 0xA8);
    if (ai >= VehicleCount(game)) return false;
    const uintptr_t v = RawVehicleSlot(game, ai);
    if (!v) return false;
    const uintptr_t b = *(uintptr_t*)(v + 0x528);
    if (!b) return false;
    *x = *(float*)(b + 0x0C); *y = *(float*)(b + 0x10);
    return true;
}

static uint32_t my_TrafficVehicle_update(void* t, uint32_t dtBits, uint32_t gamePtr) {
    if (!orig_TrafficUpdate) return 1;
    TrafficMirror* m = t ? TrafficMirrorOfPtr((uintptr_t)t) : nullptr;
    if (!m) {
        if (g_TrFollower && g_TrafficHookOk && t) return 0;        // takipci: yerel trafik araci kaldirilir (yayinlayanin araclari gosterilir)
        return orig_TrafficUpdate(t, dtBits, gamePtr);
    }

    const long long now = NowMs();
    bool keep = g_IsConnected.load() && (now - m->rxMs) < 2000;
    float lx, ly;
    if (keep && gamePtr && LocalActiveBodyPos((uintptr_t)gamePtr, &lx, &ly)) {
        const float dx = m->x - lx, dy = m->y - ly;
        if (dx * dx + dy * dy > kTrKeepR2) keep = false;
    }
    if (!keep) {                                                   // oyunun kendi dongusu removeTrafficVehicle + destroy yapar
        m->used = false; m->ptr = 0;
        return 0;
    }
    const uintptr_t body = *(uintptr_t*)((uintptr_t)t + 0x6C);
    if (body && g_b2BodySetTransform) {
        float dt; memcpy(&dt, &dtBits, sizeof(dt));
        if (!(dt > 0.001f)) dt = 0.016f;
        if (dt > 0.1f) dt = 0.1f;
        float age = (float)(now - m->rxMs) * 0.001f;
        if (age > 0.4f) age = 0.4f;
        const float gx = m->x + m->vx * age, gy = m->y + m->vy * age;
        const float cx = *(float*)(body + 0x0C), cy = *(float*)(body + 0x10), ca = *(float*)(body + 0x40);
        const float ddx = gx - cx, ddy = gy - cy;
        float nx, ny, na;
        if (!(cx == cx) || !(cy == cy) || ddx * ddx + ddy * ddy > 25.0f) {
            nx = gx; ny = gy; na = m->a;
        } else {
            const float alpha = 1.0f - expf(-dt / 0.06f);
            nx = cx + ddx * alpha; ny = cy + ddy * alpha; na = ca + BaleWrapAngle(m->a - ca) * alpha;
        }
        TestB2Vec2 pos; pos.x = nx; pos.y = ny;
        g_b2BodySetTransform((void*)body, &pos, na);
        SetRemoteBodyVelocity(body, m->vx, m->vy, 0.0f);
    }
    orig_TrafficUpdate(t, dtBits, gamePtr);                        // gorunum matrisi/tekerlekler; kaldirma karari bizde
    return 1;
}

static void TrafficFrame(uintptr_t game) {
    if (!kSyncTraffic || game == 0) return;
    if (!g_IsConnected.load() || !g_VehNetArmed.load()) {
        g_TrFollower = false;
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        g_PendingTraffic.clear();
        return;
    }
    const long long now = NowMs();
    const int cnt0 = *(int*)(game + kTrCountOff);
    if (cnt0 < 0 || cnt0 > kTrPoolSize) return;
    g_TrFollower = g_TrafficHookOk && (now - g_TrLastRxMs.load()) < 3000;
    if (g_TrFollower) *(float*)(game + 0x62C) = 5.0f;              // oyunun kendi trafik dogurma sayaci: yerel arac dogmasin

    // ---- yayin: SADECE gerekliyse ----
    // Baska bir oyuncunun aktif araci bu cihazin aktif aracina yakin (ya da ayni arac) degilse kimsenin bu cihazin trafigini
    // gormesi gerekmez: hic yayin yok. Yakin oyuncular varsa bolgenin trafigini yalnizca EN DUSUK numarali oyuncunun cihazi
    // yayinlar (karsilikli / ikili yayin ve ayni aractaki iki kisinin cift yayini olmaz).
    bool publish = false;
    if (now - g_TrLastSendMs >= 120) {
        g_TrLastSendMs = now;
        float mx, my;
        if (LocalActiveBodyPos(game, &mx, &my)) {
            const uint8_t me = g_LocalPlayerId.load();
            bool inCluster = false, lowerInCluster = false;
            for (uint8_t pl = 0; pl < MAX_PLAYERS; ++pl) {
                if (pl == me) continue;
                int nid = -1;
                {
                    std::lock_guard<std::mutex> lock(g_ActiveMutex);
                    if ((now - g_RemoteActive[pl].ms) < 6000) nid = g_RemoteActive[pl].netId;
                }
                if (nid < 0) continue;
                const uintptr_t rv = GetVehicleFromIndex(game, (uint16_t)nid);
                const uintptr_t rb = rv ? *(uintptr_t*)(rv + 0x528) : 0;
                if (!rb) continue;
                const float dx = *(float*)(rb + 0x0C) - mx, dy = *(float*)(rb + 0x10) - my;
                if (!(dx == dx) || !(dy == dy) || dx * dx + dy * dy > kTrClusterR2) continue;
                inCluster = true;
                if (pl < me) lowerInCluster = true;
            }
            publish = inCluster && !lowerInCluster;
        }
    }
    if (publish) {
        uint8_t buf[sizeof(TrafficHeader) + kTrafficMax * sizeof(TrafficEntry)];
        TrafficHeader h; h.type = PACKET_TRAFFIC; h.ownerId = g_LocalPlayerId.load(); h.count = 0;
        size_t off = sizeof(h);
        bool seen[12] = { false };
        for (int i = 0; i < cnt0 && h.count < (uint8_t)kTrPoolSize; ++i) {
            const uintptr_t p = *(uintptr_t*)(game + kTrArrOff + (uintptr_t)i * 4u);
            if (!p || TrafficMirrorOfPtr(p)) continue;
            const uintptr_t body = *(uintptr_t*)(p + 0x6C);
            if (!body) continue;
            const float x = *(float*)(body + 0x0C), y = *(float*)(body + 0x10), a = *(float*)(body + 0x40);
            const float vx = *(float*)(body + 0x44), vy = *(float*)(body + 0x48);
            if (!(x == x) || !(y == y) || !(a == a) || !(vx == vx) || !(vy == vy)) continue;
            int s = -1;
            for (int k = 0; k < 12; ++k) if (g_TrSendIds[k].p == p) { s = k; break; }
            if (s >= 0) {
                const float dx = x - g_TrSendIds[s].x, dy = y - g_TrSendIds[s].y;
                if (dx * dx + dy * dy > 225.0f) { g_TrSendIds[s].id = g_TrNextId++; if (g_TrNextId == 0) g_TrNextId = 1; }   // havuz nesnesi baska araca yeniden verildi
            } else {
                for (int k = 0; k < 12; ++k) if (g_TrSendIds[k].p == 0) { s = k; break; }
                if (s < 0) continue;
                g_TrSendIds[s].p = p; g_TrSendIds[s].id = g_TrNextId++;
                if (g_TrNextId == 0) g_TrNextId = 1;
            }
            g_TrSendIds[s].x = x; g_TrSendIds[s].y = y; seen[s] = true;

            uint8_t type = 0;                                      // tip: mesh isaretcisi EntityManager tablosundakiyle eslesen
            const uintptr_t mesh = *(uintptr_t*)(p + 0x50);
            for (uint32_t k = 0; k < 2; ++k) {
                if (*(uintptr_t*)(game + 0x708 + (uintptr_t)k * 0xA8 + 0x1C64 + 8) == mesh) { type = (uint8_t)k; break; }
            }
            TrafficEntry e;
            memset(&e, 0, sizeof(e));
            e.id = g_TrSendIds[s].id; e.type = type; e.dir = *(uint8_t*)(p + 0x248) ? 1 : 0;
            e.color = (uint8_t)(*(uint32_t*)(p + 0x2F4) % 10u);    // renk indeksi (TrafficVehicle::COLORS), load'da rastgele secilir
            e.road = (uint16_t)(*(uint32_t*)(p + 0x244) & 0xFFFF);
            e.t = *(float*)(p + 0x240); e.x = x; e.y = y; e.a = a; e.vx = vx; e.vy = vy;
            memcpy(buf + off, &e, sizeof(e)); off += sizeof(e); ++h.count;
        }
        for (int k = 0; k < 12; ++k) if (!seen[k]) g_TrSendIds[k].p = 0;
        {   // arac olmasa da bos paket gider: takipci cihazlar yerel trafigi kapali tutsun
            memcpy(buf, &h, sizeof(h));
            std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
            if (g_VehicleEventOut.size() < 2048) g_VehicleEventOut.insert(g_VehicleEventOut.end(), buf, buf + off);   // kuyruk doluysa bu tur atlanir (diger olaylar silinmesin)
        }
    }

    // ---- alim: gelen trafik araclarini aynalara isle ----
    std::vector<TrafficPending> pend;
    {
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        pend.swap(g_PendingTraffic);
    }
    if (pend.empty()) return;
    float lx = 0.0f, ly = 0.0f;
    const bool haveLocal = LocalActiveBodyPos(game, &lx, &ly);
    const uintptr_t map = *(uintptr_t*)(game + 0x704);
    const uint32_t roadCount = map ? *(uint32_t*)(map + 0x720) : 0;
    for (size_t k = 0; k < pend.size(); ++k) {
        const TrafficPending& pe = pend[k];
        const TrafficEntry& e = pe.e;
        if (!(e.x == e.x) || !(e.y == e.y) || !(e.a == e.a) || !(e.vx == e.vx) || !(e.vy == e.vy) || !(e.t == e.t)) continue;
        if (fabsf(e.x) > 1.0e5f || fabsf(e.y) > 1.0e5f || fabsf(e.vx) > 60.0f || fabsf(e.vy) > 60.0f) continue;
        TrafficMirror* m = nullptr;
        for (int i = 0; i < kTrMirrorMax; ++i) if (g_TrMir[i].used && g_TrMir[i].owner == pe.owner && g_TrMir[i].id == e.id) { m = &g_TrMir[i]; break; }
        if (m) {
            m->x = e.x; m->y = e.y; m->a = e.a; m->vx = e.vx; m->vy = e.vy; m->rxMs = now;
            if (m->ptr) *(uint32_t*)(m->ptr + 0x2F4) = (uint32_t)(e.color % 10u);
            continue;
        }
        if (!g_TrafficHookOk || !g_GameAddTrafficFn || !haveLocal || e.road >= roadCount) continue;
        const float dx = e.x - lx, dy = e.y - ly;
        if (dx * dx + dy * dy > kTrSpawnR2) continue;              // bu cihazdaki oyuncu goremeyecegi kadar uzak
        const int cnt = *(int*)(game + kTrCountOff);
        if (cnt < 0 || cnt >= kTrPoolSize) continue;               // havuz (5 nesne) dolu: tasma = bellek bozulmasi
        TrafficMirror* slot = nullptr;
        for (int i = 0; i < kTrMirrorMax; ++i) if (!g_TrMir[i].used) { slot = &g_TrMir[i]; break; }
        if (!slot) continue;
        const float pos3[3] = { e.x, 0.0f, e.y };                  // Vector3: x, (kullanilmaz), y
        uint32_t tBits, aBits;
        const float tF = e.t, aF = e.a;
        memcpy(&tBits, &tF, sizeof(tBits)); memcpy(&aBits, &aF, sizeof(aBits));
        const int idx = g_GameAddTrafficFn((void*)game, (uint32_t)(e.type & 1), (uint32_t)e.road, tBits,
                                           e.dir ? 1u : 0u, pos3, aBits, 0);
        if (idx < 0 || idx >= kTrPoolSize) continue;
        const uintptr_t p = *(uintptr_t*)(game + kTrArrOff + (uintptr_t)idx * 4u);
        if (!p) continue;
        slot->used = true; slot->ptr = p; slot->owner = pe.owner; slot->id = e.id;
        slot->x = e.x; slot->y = e.y; slot->a = e.a; slot->vx = e.vx; slot->vy = e.vy; slot->rxMs = now;
        *(uint32_t*)(p + 0x2F4) = (uint32_t)(e.color % 10u);       // ayni renk
        const uintptr_t body = *(uintptr_t*)(p + 0x6C);
        if (body) B2BodyMakeSensor(body);                          // ayna arac carpismasiz: asil arac sahibinin cihazinda carpisir
        if (g_TrLogs < 20) { ++g_TrLogs; LOGI("[TRAFFIC] ayna dogdu oyuncu=%u id=%u tip=%u yol=%u (%.0f,%.0f)",
                                               (unsigned)pe.owner, (unsigned)e.id, (unsigned)e.type, (unsigned)e.road, e.x, e.y); }
    }
}

// ========================================================================
// DUNYA DURUMU: SILO STOGU, BIYOGAZ, SATIS NOKTASI FIYATLARI
// Oyun bu degerleri varlik (arac/romork/alet) update'leri icinde degistirir: romork silodan dolarken ya da bosaltirken
// Game+0x6a8.. (4 mahsul stogu) azalir/artar, Game+0x6b8 biyogaz dolulugu artar, satista TipSite fiyati (+0x10+4*mahsul) duser.
// Bu degerler cihazlar arasinda AKTARILMIYORDU (yalnizca para aktariliyordu).
// Kural (para korumasiyla ayni): GERCEK (hayalet olmayan) varlik update'indeki degisim bu cihazin "delta"sidir ve 150 ms'de
// bir yayinlanir; HAYALET varlik update'indeki degisim geri alinir (ayni olay her cihazda tekrar sayilmasin, ayni aractaki iki
// kisi de 2 kat saymasin). Gelen deltalar yerel degere eklenir. Ayrica host her 4 sn'de mutlak durumu (stoklar + tum
// satis fiyatlari) yollar; client son 2 sn'de yerel degisiklik yaptiysa onu atlar.
// ========================================================================
static const bool kSyncWorld = true;
static const uint8_t PACKET_WORLD_DELTA = 40;     // [type][ownerId][count] + count*{u16 idx, float delta}; host iletir
static const uint8_t PACKET_PRICE_TABLE = 41;     // host -> client: [type][sites] + 5 stok + sites*4 fiyat (mutlak)
static const int kWorldSites = 16;
static const int kWorldStocks = 5;                // idx 0..3: Game+0x6a8+4*i, idx 4: Game+0x6b8 (biyogaz)
// idx 5.. : satis noktasi fiyatlari (site*4+mahsul); idx kWorldExtraBase.. : biyogaz tesisi (TipSite) zamanlayici +0x6c ve doluluk +0x70 (site*2 + {0,1})
static const int kWorldExtraBase = kWorldStocks + kWorldSites * 4;
static const int kWorldN = kWorldExtraBase + kWorldSites * 2;
struct __attribute__((packed)) WorldDeltaHeader { uint8_t type; uint8_t ownerId; uint8_t count; };
struct __attribute__((packed)) WorldDeltaEntry { uint16_t idx; float d; };
struct __attribute__((packed)) PriceTableHeader { uint8_t type; uint8_t sites; };
// Tablo duzeni: [5 stok][sites*4 fiyat][sites*2 (zamanlayici, doluluk)]
static inline size_t PriceTableFloats(int sites) { return (size_t)kWorldStocks + (size_t)sites * 6; }
static const size_t kPriceTableMaxBytes = sizeof(PriceTableHeader) + (size_t)(kWorldStocks + kWorldSites * 6) * 4;
typedef void (*TipSite_updateBga_t)(void* site);
static TipSite_updateBga_t g_TipSiteUpdateBgaFn = nullptr;       // TipSite::updateBgaPlanes() (this = r0)
static float g_WorldAcc[kWorldN];                              // sadece oyun thread'i
static long long g_WorldLocalMs = 0;
static long long g_WorldFlushMs = 0, g_PriceTblMs = 0;
static uint32_t g_PriceTblHash = 0;
static std::vector<WorldDeltaEntry> g_PendingWorld;            // g_VehicleEventMutex ile korunur
static uint8_t g_PendingPriceBuf[kPriceTableMaxBytes];         // g_VehicleEventMutex ile korunur
static size_t g_PendingPriceLen = 0;
static int g_WorldLogs = 0;

static volatile float* WorldPtr(uintptr_t game, int idx) {
    if (!game || idx < 0) return nullptr;
    if (idx < 4) return (volatile float*)(game + 0x6A8 + (uintptr_t)idx * 4u);
    if (idx == 4) return (volatile float*)(game + 0x6B8);
    int s, f;
    bool extra = false;
    if (idx < kWorldExtraBase) { const int k = idx - kWorldStocks; s = k >> 2; f = k & 3; }
    else if (idx < kWorldN) { const int e = idx - kWorldExtraBase; s = e >> 1; f = e & 1; extra = true; }
    else return nullptr;
    if (s >= kWorldSites) return nullptr;
    const uintptr_t map = *(volatile uintptr_t*)(game + 0x704);
    if (!map) return nullptr;
    const uintptr_t tbl = *(volatile uintptr_t*)(map + 0x724);
    const int32_t cnt = *(volatile int32_t*)(map + 0x728);
    if (!tbl || cnt <= 0 || cnt > 64 || s >= cnt) return nullptr;
    if (extra) return (volatile float*)(tbl + (uintptr_t)s * 0x80u + (f == 0 ? 0x6Cu : 0x70u));
    return (volatile float*)(tbl + (uintptr_t)s * 0x80u + 0x10u + (uintptr_t)f * 4u);
}

// Bu idx bir satis noktasinin (TipSite) alani mi? Ise nesne adresini ve alan turunu verir: 0 fiyat, 1 zamanlayici (+0x6c), 2 doluluk (+0x70).
static int WorldSiteField(uintptr_t game, int idx, uintptr_t* siteOut) {
    if (idx < kWorldStocks || idx >= kWorldN) return -1;
    volatile float* p = WorldPtr(game, idx);
    if (!p) return -1;
    if (idx < kWorldExtraBase) { *siteOut = (uintptr_t)p - 0x10u - (uintptr_t)((idx - kWorldStocks) & 3) * 4u; return 0; }
    const int f = (idx - kWorldExtraBase) & 1;
    *siteOut = (uintptr_t)p - (f == 0 ? 0x6Cu : 0x70u);
    return f == 0 ? 1 : 2;
}

static int WorldSiteCount(uintptr_t game) {
    const uintptr_t map = *(volatile uintptr_t*)(game + 0x704);
    if (!map) return 0;
    const int32_t cnt = *(volatile int32_t*)(map + 0x728);
    if (cnt <= 0 || cnt > 64 || !*(volatile uintptr_t*)(map + 0x724)) return 0;
    return cnt > kWorldSites ? kWorldSites : cnt;
}

// Tum izlenen degerleri okur; dondurdugu sayi = gecerli idx sayisi (idx 0..n-1).
static int WorldRead(uintptr_t game, float* out) {
    const int n = kWorldN;                                         // olmayan alanlar 0 okunur (degismez)
    for (int i = 0; i < n; ++i) {
        volatile float* p = WorldPtr(game, i);
        out[i] = p ? *p : 0.0f;
    }
    return n;
}

// Oyun thread'i, durum 6, her karede.
static void WorldSyncFrame(uintptr_t game) {
    if (!kSyncWorld || game == 0) return;
    if (!g_IsConnected.load() || !g_VehNetArmed.load()) {
        memset(g_WorldAcc, 0, sizeof(g_WorldAcc));
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        g_PendingWorld.clear();
        g_PendingPriceLen = 0;
        return;
    }
    const long long now = NowMs();

    // 1) bu cihazin gercek varliklarindan biriken degisimleri yayinla
    if (now - g_WorldFlushMs >= 150) {
        g_WorldFlushMs = now;
        uint8_t buf[sizeof(WorldDeltaHeader) + 16 * sizeof(WorldDeltaEntry)];
        WorldDeltaHeader h; h.type = PACKET_WORLD_DELTA; h.ownerId = g_LocalPlayerId.load(); h.count = 0;
        size_t off = sizeof(h);
        for (int i = 0; i < kWorldN && h.count < 16; ++i) {
            const float d = g_WorldAcc[i];
            if (!(d == d) || fabsf(d) < 1.0e-4f) { if (!(d == d)) g_WorldAcc[i] = 0.0f; continue; }
            WorldDeltaEntry e; e.idx = (uint16_t)i; e.d = d;
            memcpy(buf + off, &e, sizeof(e)); off += sizeof(e); ++h.count;
            g_WorldAcc[i] = 0.0f;
        }
        if (h.count > 0) {
            memcpy(buf, &h, sizeof(h));
            QueueVehicleEvent(buf, off);
            g_WorldLocalMs = now;
        }
    }

    // 2) baska cihazlardan gelen degisimleri uygula
    std::vector<WorldDeltaEntry> pend;
    uint8_t tbl[kPriceTableMaxBytes];
    size_t tblLen = 0;
    {
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        pend.swap(g_PendingWorld);
        if (g_PendingPriceLen > 0) { memcpy(tbl, g_PendingPriceBuf, g_PendingPriceLen); tblLen = g_PendingPriceLen; g_PendingPriceLen = 0; }
    }
    for (size_t k = 0; k < pend.size(); ++k) {
        const int idx = pend[k].idx;
        const float d = pend[k].d;
        if (!(d == d) || fabsf(d) > 1.0e7f) continue;
        volatile float* p = WorldPtr(game, idx);
        if (!p) continue;
        float v = *p + d;
        if (!(v == v) || v < 0.0f) v = 0.0f;
        if (idx == 4 && v > 100000.0f) v = 100000.0f;
        uintptr_t site = 0;
        const int fk = WorldSiteField(game, idx, &site);
        if (fk == 0) {                                             // fiyat: satis noktasinin azami fiyatini (+0x30) gecmesin
            const float mx = *(volatile float*)((uintptr_t)p + 0x20);
            if (mx > 0.0f && v > mx) v = mx;
        } else if (fk == 2) {                                      // biyogaz doluluk: kapasiteyi (+0x74) gecmesin
            const float cap = *(volatile float*)((uintptr_t)p + 4);
            if (cap > 0.0f && v > cap) v = cap;
        }
        *p = v;
        if ((fk == 1 || fk == 2) && g_TipSiteUpdateBgaFn) g_TipSiteUpdateBgaFn((void*)site);   // gorunum (doluluk cubugu / isik)
    }
    if (tblLen >= sizeof(PriceTableHeader) && !g_IsHost.load() && now - g_WorldLocalMs >= 2000) {
        PriceTableHeader th; memcpy(&th, tbl, sizeof(th));
        const size_t need = sizeof(th) + PriceTableFloats(th.sites) * 4;
        if (th.sites <= kWorldSites && need <= tblLen) {
            const int nT = (int)PriceTableFloats(th.sites);
            const int priceEnd = kWorldStocks + th.sites * 4;
            for (int i = 0; i < nT; ++i) {
                float v; memcpy(&v, tbl + sizeof(th) + (size_t)i * 4, sizeof(v));
                if (!(v == v) || v < 0.0f || v > 1.0e8f) continue;
                const int idx = (i < priceEnd) ? i : (kWorldExtraBase + (i - priceEnd));
                volatile float* p = WorldPtr(game, idx);
                if (!p) continue;
                uintptr_t site = 0;
                const int fk = WorldSiteField(game, idx, &site);
                if (fk == 1) { if (v >= *p) continue; }            // zamanlayici: iki cihazdan KUCUK olan gecerli (durmus cihaz geri kalmaz, akan cihaz one gecmez)
                else if (fk == 2) {
                    const float cap = *(volatile float*)((uintptr_t)p + 4);
                    if (cap > 0.0f && v > cap) v = cap;
                }
                if (*p == v) continue;
                *p = v;
                if ((fk == 1 || fk == 2) && g_TipSiteUpdateBgaFn) g_TipSiteUpdateBgaFn((void*)site);
            }
            if (g_WorldLogs < 5) { ++g_WorldLogs; LOGI("[WORLD] client: host stok+fiyat+biyogaz tablosu uygulandi (satis noktasi=%u)", (unsigned)th.sites); }
        }
    }

    // 3) host: mutlak durumu (stoklar + fiyatlar + biyogaz zamanlayici/doluluk) periyodik yayinla
    // Degisim yoksa 12 sn'de bir kalp atisi (yeni baglanan / paket kacirmis cihaz toparlansin); degisim varsa en sik 4 sn'de bir.
    if (g_IsHost.load() && now - g_PriceTblMs >= 4000) {
        const int sites = WorldSiteCount(game);
        uint8_t buf[kPriceTableMaxBytes];
        PriceTableHeader th; th.type = PACKET_PRICE_TABLE; th.sites = (uint8_t)sites;
        memcpy(buf, &th, sizeof(th));
        float vals[kWorldN];
        WorldRead(game, vals);
        const int priceEnd = kWorldStocks + sites * 4;
        const int nT = (int)PriceTableFloats(sites);
        for (int i = 0; i < nT; ++i) {
            const int idx = (i < priceEnd) ? i : (kWorldExtraBase + (i - priceEnd));
            memcpy(buf + sizeof(th) + (size_t)i * 4, &vals[idx], 4);
        }
        const size_t tlen = sizeof(th) + (size_t)nT * 4;
        uint32_t hh = 2166136261u;                                 // FNV-1a
        for (size_t k = 0; k < tlen; ++k) { hh ^= buf[k]; hh *= 16777619u; }
        if (hh != g_PriceTblHash || now - g_PriceTblMs >= 12000) {
            g_PriceTblHash = hh; g_PriceTblMs = now;
            QueueVehicleEvent(buf, tlen);
        }
    }
}

// Oyun thread'i, durum 6, her karede.
static void ResumeSyncFrame(uintptr_t game) {
    if (g_RestResendAll.exchange(false)) SendRestPoses(game);
    ApplyRestPoses(game);
}

// my_GameUpdateStateBase basinda, bosluk suresiyle cagrilir (updateStateBase bu kadar ms calismadi).
static void OnGameResumed(long long gapMs) {
    if (!g_IsConnected.load() || !g_VehNetArmed.load()) return;
    LOGI("[RESUME] oyun geri dondu (bosluk=%lld ms) rol=%s", gapMs, g_IsHost.load() ? "host" : "client");
    const uint8_t t = g_IsHost.load() ? PACKET_HOST_RESUME : PACKET_RESUME;
    QueueVehicleEvent(&t, 1);
    if (!g_IsHost.load()) g_SyncRequestOut.store(true);
}

// Diger cihazdan gelen romork takma/cikarma ve durum paketlerini oyun thread'inde uygular.
static void ApplyPendingTrailerEvents(uintptr_t game, uint8_t localOwner, uint64_t nowMs) {
    std::vector<TrailerAttachPacket> attaches;
    std::vector<EntityStatePacket> states;
    {
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        if (g_PendingTrailerAttach.empty() && g_PendingEntityStates.empty()) return;
        if (!g_VehNetArmed.load()) return;               // netId tablosu hazir degil: bekle
        attaches.swap(g_PendingTrailerAttach);
        states.swap(g_PendingEntityStates);
    }
    void* world = *(void**)(game + 0xA0);
    const uint32_t tn = TrailerCount(game);

    for (const TrailerAttachPacket& pk : attaches) {
        if (pk.attach & 2) {                                  // alet (pulluk / mibzer)
            if (pk.ownerId == localOwner) continue;
            g_ApplyingRemoteTrailerOp = true;
            ApplyToolAttach(game, pk, world);
            g_ApplyingRemoteTrailerOp = false;
            continue;
        }
        if (pk.ownerId == localOwner || pk.trIdx >= tn || world == nullptr) continue;
        const uintptr_t tp = RawTrailerSlot(game, pk.trIdx);
        const uintptr_t veh = GetVehicleFromIndex(game, pk.vehNetId);
        if ((pk.attach & 1) != 0 && WK::IsWalker(veh)) {
            LOGI("[WALKER] uzaktan romork takma engellendi: yaya aracina romork baglanamaz");
            continue;
        }
        if (tp == 0 || veh == 0 || *(uint32_t*)(tp + 0x10) != pk.trType) {
            LOGI("[TRAILER] olay atlandi: romork=%u arac=%u (esleme yok)", (unsigned)pk.trIdx, (unsigned)pk.vehNetId);
            continue;
        }
        const uintptr_t tbody = *(uintptr_t*)(tp + 0x180);
        const uintptr_t vbody = *(uintptr_t*)(veh + 0x528);
        if (tbody == 0 || vbody == 0 || !g_VehicleDetachTrailerFn) continue;
        const uintptr_t cur = *(uintptr_t*)(tp + 0x184);
        g_ApplyingRemoteTrailerOp = true;
        if ((pk.attach & 1) == 0) {
            if (cur == veh) g_VehicleDetachTrailerFn((void*)veh, world);
            AfterRemoteDetach(game, pk, tbody, vbody, false);
            LOGI("[TRAILER] uzaktan cikarma uygulandi: romork=%u (bagliydi=%d)", (unsigned)pk.trIdx, (int)(cur == veh));
        } else if (cur != veh) {
            if (!g_VehicleAttachTrailerFn || *(uintptr_t*)(veh + 0x560) != 0 || *(uint32_t*)(veh + 0x730) != 0) {
                LOGI("[TRAILER] takma atlandi: alet bagli / isci acik / fonksiyon yok");
            } else {
                if (cur != 0 && SlotIndexOfVehicle(game, cur) >= 0) g_VehicleDetachTrailerFn((void*)cur, world);
                if (*(uintptr_t*)(veh + 0x55C) != 0) g_VehicleDetachTrailerFn((void*)veh, world);
                // Romorku, gonderendeki gorece konumuyla BU cihazdaki aracin arkasina yerlestir; sonra tak.
                const float c0 = cosf(pk.va), s0 = sinf(pk.va);
                const float dx = pk.tx - pk.vx, dy = pk.ty - pk.vy;
                const float lx = c0 * dx + s0 * dy, ly = -s0 * dx + c0 * dy;
                const float va2 = *(float*)(vbody + 0x40);
                const float c1 = cosf(va2), s1 = sinf(va2);
                TestB2Vec2 pos;
                pos.x = *(float*)(vbody + 0x0C) + c1 * lx - s1 * ly;
                pos.y = *(float*)(vbody + 0x10) + s1 * lx + c1 * ly;
                g_b2BodySetTransform((void*)tbody, &pos, va2 + (pk.ta - pk.va));
                SetRemoteBodyVelocity(tbody, 0.0f, 0.0f, 0.0f);
                g_VehicleAttachTrailerFn((void*)veh, (void*)tp, world);
                LOGI("[TRAILER] uzaktan takma uygulandi: romork=%u arac=%u", (unsigned)pk.trIdx, (unsigned)pk.vehNetId);
            }
        }
        g_ApplyingRemoteTrailerOp = false;
    }
    if (!attaches.empty()) {                                  // yankilanmasin: kaydi guncelle
        const uint32_t ton = ToolCount(game);
        for (uint32_t i = 0; i < ton; ++i) {
            const uintptr_t tp = RawToolSlot(game, i);
            bool fresh = false;
            TrailerTrack* t = tp ? TrackForTool(tp, &fresh) : nullptr;
            if (!t) continue;
            uintptr_t av = *(uintptr_t*)(tp + 0x23C);
            if (av != 0 && SlotIndexOfVehicle(game, av) < 0) av = 0;
            t->av = av;
        }
        for (uint32_t i = 0; i < tn; ++i) {
            const uintptr_t tp = RawTrailerSlot(game, i);
            bool fresh = false;
            TrailerTrack* t = tp ? TrackForTrailer(tp, &fresh) : nullptr;
            if (!t) continue;
            uintptr_t av = *(uintptr_t*)(tp + 0x184);
            if (av != 0 && SlotIndexOfVehicle(game, av) < 0) av = 0;
            t->av = av;
        }
    }

    for (const EntityStatePacket& st : states) {
        if (st.ownerId == localOwner) continue;
        if (st.kind == 1) {                                   // romork doluluk
            if (st.id >= tn) continue;
            const uintptr_t tp = RawTrailerSlot(game, st.id);
            if (tp == 0 || *(uint32_t*)(tp + 0x10) != st.sub) continue;
            float level = WordToFloat(st.wa);
            if (!(level == level) || level > 1.0e7f) continue;
            const float cap = *(float*)(tp + 0x38C);
            if (level < 0.0f) level = 0.0f;
            else if (level > cap) level = cap;
            memcpy((void*)(tp + 0x388), &level, 4);
            *(uint32_t*)(tp + 0x390) = st.wb;
            *(uint32_t*)(tp + 0x3A4) |= 4;                    // Trailer::setFillLevel ile ayni: gorunum yenilensin
            bool fresh = false;
            TrailerTrack* t = TrackForTrailer(tp, &fresh);
            if (t) { memcpy(&t->levelWord, &level, 4); t->typeWord = st.wb; t->lastMs = nowMs; }
        } else if (st.kind == 4) {                            // alet doluluk (tohum vb.)
            if (st.id >= ToolCount(game)) continue;
            const uintptr_t tp = RawToolSlot(game, st.id);
            if (tp == 0 || *(uint32_t*)(tp + 0x10) != st.sub || !ToolHasFill(tp)) continue;
            const float fill = WordToFloat(st.wa);
            if (!(fill == fill) || fill < 0.0f || fill > 1.0e7f) continue;
            memcpy((void*)(tp + 0x45C), &st.wa, 4);
            if (*(uint32_t*)(tp + 0x434) == 1 && st.wb < 64) *(uint32_t*)(tp + 0x438) = st.wb;
            bool fresh = false;
            TrailerTrack* t = TrackForTool(tp, &fresh);
            if (t) { t->levelWord = st.wa; t->typeWord = (*(uint32_t*)(tp + 0x434) == 1) ? *(uint32_t*)(tp + 0x438) : 0; t->lastMs = nowMs; }
        } else if (st.kind == 2) {                            // arac: kesici/alet, isci (AI), bicer tanki
            const uintptr_t v = GetVehicleFromIndex(game, st.id);
            if (v == 0) continue;
            uint8_t owner = VEHICLE_OWNER_NONE;
            { std::lock_guard<std::mutex> lock(g_VehicleStateMutex); owner = g_VehicleAuthority[st.id].ownerId; }
            const bool combine = *(uint8_t*)(v + 0x328) != 0;
            CombineTrack& c = g_CbTrack[st.id];
            // Bayraklar KENAR tetiklemeli: son bilinen durumdan (gonderdigimiz ya da aldigimiz) farkli olan bit uygulanir.
            // Boylece ayni arac iki cihazda secili olsa bile calisir, eski paket yerel degisikligi geri almaz.
            uint8_t rflagsNew = (uint8_t)(st.flags & kStateFlagMask);
            // Bu cihazda kullanici iscyi az once durdurdu: iscinin sahibinden gelen ESKI "isci acik" bilgisi (kenar olsa bile)
            // yok sayilir; yoksa durdurma geri alinir ("cikiyor gibi olup tekrar isciye geciyor").
            if ((rflagsNew & 8) && nowMs < g_AiStopHoldMs[st.id] && *(uint32_t*)(v + 0x730) == 0) rflagsNew = (uint8_t)(rflagsNew & ~8);
            else if (rflagsNew & 8) g_AiOriginMs[st.id] = nowMs;                    // isciyi calistiran cihaz hala canli
            const uint8_t base = c.rvalid ? c.rflags : LocalVehicleFlags(v);
            const uint8_t diff = (uint8_t)(base ^ rflagsNew);
            if (combine && owner != localOwner) {
                const float fa = WordToFloat(st.wa), fb = WordToFloat(st.wb);
                if (fa == fa && fb == fb && fa < 1.0e7f && fb < 1.0e7f) {
                    *(uint32_t*)(v + 0x440) = st.wa;
                    *(uint32_t*)(v + 0x448) = st.wb;
                }
            }
            if (st.sub != 0 && st.sub <= 3) {                 // tohum secimi (mibzer): kenar paketinde ya da isci baslarken
                const bool aiStartNow = ((st.flags & 8) != 0) && (*(uint32_t*)(v + 0x730) == 0);
                if ((st.flags & kEdgeBit) || aiStartNow) {
                    const uintptr_t tool = *(uintptr_t*)(v + 0x560);
                    if (tool != 0 && *(uint32_t*)(tool + 0x434) == 1 && *(uint32_t*)(tool + 0x438) != (uint32_t)(st.sub - 1)) {
                        *(uint32_t*)(tool + 0x438) = (uint32_t)(st.sub - 1);
                        bool fresh = false;
                        TrailerTrack* tt = TrackForTool(tool, &fresh);
                        if (tt) tt->typeWord = (uint32_t)(st.sub - 1);
                        LOGI("[SEED] tohum uzaktan secildi netId=%u tohum=%u", (unsigned)st.id, (unsigned)(st.sub - 1));
                    }
                }
            }
            if (diff & 8) {                                   // isci (AI): uzaktan baslat / durdur
                const bool wantAi = (rflagsNew & 8) != 0;
                const bool haveAi = *(uint32_t*)(v + 0x730) != 0;
                if (wantAi && !haveAi) {
                    if (kMirrorAiOnRemote && g_VehiclePrepareAiFn && g_VehicleAiStartFn) {
                        const uint32_t ok = g_VehiclePrepareAiFn((void*)v, (void*)game) & 0xFF;
                        if (ok) {
                            g_VehicleAiStartFn((void*)v, (void*)game);
                            g_AiPrevOn[st.id] = *(uint32_t*)(v + 0x730) != 0;     // uzaktan baslatma: kullanici basisi sanilmasin
                            g_AiMirrored[st.id] = true;
                            g_AiRestartTries[st.id] = 0;
                            // Araci bu cihaz suruyordu (sahibi bu cihaz): yetkiyi birak ki iscyi baslatan cihaz konumu yayinlasin.
                            if (owner == localOwner) {
                                if (g_IsHost.load()) ReleaseVehicleAuthority(st.id, localOwner, true);
                                else {
                                    QueueVehicleRelease(st.id);
                                    std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                                    g_ClaimPending[st.id] = true;
                                }
                            }
                        }
                        LOGI("[VEHSTATE] uzaktan isci baslatma netId=%u sonuc=%u", (unsigned)st.id, (unsigned)ok);
                    } else if (!kMirrorAiOnRemote) {
                        // Aynalama yok: sadece araci bu cihaz suruyorsa yetkiyi birak ki iscinin cihazi konumu yayinlasin.
                        if (owner == localOwner) {
                            if (g_IsHost.load()) ReleaseVehicleAuthority(st.id, localOwner, true);
                            else {
                                QueueVehicleRelease(st.id);
                                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                                g_ClaimPending[st.id] = true;
                            }
                        }
                        LOGI("[VEHSTATE] uzaktan isci acildi (aynalanmiyor) netId=%u", (unsigned)st.id);
                    }
                } else if (!wantAi && haveAi && g_VehicleAiStopFn &&
                           ((st.flags & kEdgeBit) || g_AiMirrored[st.id])) {   // kullanici durdurdu (kenar) ya da aynali isci iscinin sahibinden durdu
                    const bool wasMirrored = g_AiMirrored[st.id];
                    g_VehicleAiStopFn((void*)v);
                    g_AiPrevOn[st.id] = false;                                         // uzaktan durdurma: kullanici basisi sanilmasin
                    g_AiMirrored[st.id] = false;
                    // Iscyi bu cihaz calistiriyordu (aynali degil): yeni durumu KENAR olarak herkese duyur ki aynali simulasyonlar
                    // durdurma istegi bu cihaza ulasmadan once gelen eski "isci acik" paketlerinden dolayi geri baslamasin.
                    if (!wasMirrored && g_VehNetArmed.load()) {
                        const bool cmb = *(uint8_t*)(v + 0x328) != 0;
                        QueueEntityState(2, st.id, VehSeedSub(v), cmb ? *(uint32_t*)(v + 0x440) : 0, cmb ? *(uint32_t*)(v + 0x448) : 0,
                                         (uint8_t)((LocalVehicleFlags(v) & ~8) | kEdgeBit));
                    }
                    LOGI("[VEHSTATE] uzaktan isci durduruldu netId=%u", (unsigned)st.id);
                }
            }
            if ((rflagsNew & 8) == 0) { g_AiMirrored[st.id] = false; g_AiRestartTries[st.id] = 0; }   // isci kapali: aynalama biter
            if ((diff & 1) && combine) {                      // bicer calisiyor = kesici iner
                const bool wantOn = (st.flags & 1) != 0;
                if ((*(uint8_t*)(v + 0x498) != 0) != wantOn) {
                    ToggleCombineInline(v);
                    LOGI("[VEHSTATE] kesici/bicer uygulandi netId=%u acik=%d", (unsigned)st.id, (int)wantOn);
                }
            }
            if (diff & 2) {                                   // bagli aletin aktif (indirilmis) durumu
                const uintptr_t tool = *(uintptr_t*)(v + 0x560);
                if (tool != 0 && *(uintptr_t*)(tool + 0x23C) != 0 && g_ToolToggleActiveFn) {
                    const bool wantActive = (st.flags & 2) != 0;
                    if ((*(uint8_t*)(tool + 0x230) != 0) != wantActive) {
                        g_ToolToggleActiveFn((void*)tool);
                        LOGI("[VEHSTATE] alet durumu uygulandi netId=%u aktif=%d", (unsigned)st.id, (int)wantActive);
                    }
                }
            }
            // Yanki olmasin: yerel gercegi (uygulamadan SONRA olculen) kaydet; karsidan gelen bayraklar ayri tutulur.
            c.rvalid = true; c.rflags = rflagsNew;
            c.valid = true; c.flags = LocalVehicleFlags(v);
            const bool combineNow = *(uint8_t*)(v + 0x328) != 0;
            c.wa = combineNow ? *(uint32_t*)(v + 0x440) : 0;
            c.wb = combineNow ? *(uint32_t*)(v + 0x448) : 0;
            c.lastMs = nowMs;
        }
    }
}

// Bir araca bagli romork/aletin konumunu YAYINLAYAN cihaz tek olmali (hem gonderen hem alan ayni kurali kullanir):
//  1) Aracin sahibi (authority) varsa o cihaz.
//  2) Sahipsizse ve araci yerelde aktif kullaniyorsam: daha dusuk numarali baska oyuncu de ayni aractaysa o, degilse ben.
//  3) Sahipsiz, ben aktif degilim, baska oyuncu aktifse: o.
//  4) Kimse yoksa (park): host.
// Eskiden "av == aktifArac" her iki cihazda da dogruydu (ayni traktore binince) ve iki cihaz da kendi fizigini
// "gercek" sayip birbirinin konumunu uygulamiyordu: romork bir yanda sola, diger yanda saga yatiyordu.
static bool IsTowPublisher(int nid, bool localActive, uint8_t localOwner) {
    uint8_t ow = VEHICLE_OWNER_NONE;
    if (nid >= 0) { std::lock_guard<std::mutex> lock(g_VehicleStateMutex); ow = g_VehicleAuthority[nid].ownerId; }
    if (ow != VEHICLE_OWNER_NONE) return ow == localOwner;
    if (localActive) return !HasLowerIdRemoteActive(nid, localOwner);
    if (IsRemoteActiveVehicle(nid)) return false;
    return g_IsHost.load();
}

static void CaptureAndQueueLocalTrailerStates(uintptr_t game, uint8_t localOwner, uint64_t nowMs, bool tool = false) {
    const uint32_t n = tool ? ToolCount(game) : TrailerCount(game);
    const uint16_t idBase = tool ? TOOL_ID_BASE : TRAILER_ID_BASE;
    bool* mineArr = tool ? g_ToolMine : g_TrailerMine;
    const uintptr_t bodyOff = tool ? 0x228 : 0x180;
    const uintptr_t avOff = tool ? 0x23C : 0x184;
    const uintptr_t activeVeh = LocalActiveVehiclePtr(game);
    static std::vector<BaleLocalVehiclePoint> vpts;               // otorite sahibi yerel araclar (gerekince, kare basina bir kez)
    bool vptsDone = false;
    for (uint32_t i = 0; i < n; ++i) {
        const uintptr_t tp = tool ? RawToolSlot(game, i) : RawTrailerSlot(game, i);
        if (tp == 0) continue;
        const uintptr_t body = *(uintptr_t*)(tp + bodyOff);
        if (body == 0) continue;
        const uint32_t type = *(uint32_t*)(tp + 0x10);
        if (type > 0x3F) continue;

        const float x = *(float*)(body + 0x0C);
        const float y = *(float*)(body + 0x10);
        const float angle = *(float*)(body + 0x40);
        const uint16_t sid = (uint16_t)(idBase + i);

        bool moving = false;
        {
            std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
            VehicleSampleState& sample = g_LocalSamples[sid];
            if (sample.valid) {
                moving = fabsf(x - sample.x) > CLAIM_POSITION_EPSILON ||
                         fabsf(y - sample.y) > CLAIM_POSITION_EPSILON ||
                         GetAngleDelta(angle, sample.angle) > CLAIM_ANGLE_EPSILON;
            }
            sample.valid = true; sample.x = x; sample.y = y; sample.angle = angle;
        }

        const uintptr_t av = *(uintptr_t*)(tp + avOff);       // bagli oldugu arac (0 = bagli degil)
        if (av != 0) {
            const int nid = NetIdOfVehicle(av);
            const bool towedByMe = (nid >= 0) ? IsTowPublisher(nid, av == activeVeh, localOwner) : (av == activeVeh);
            {   // rol degisimi tani logu (tek yayinci olmali: ikisi de "yayin" ya da ikisi de "alici" gorunuyorsa hata burada)
                static uint8_t s_role[2][256];
                static std::atomic<int> s_roleLogs(0);
                const uint8_t role = towedByMe ? 1 : 2;
                if (i < 256 && s_role[tool ? 1 : 0][i] != role && s_roleLogs.load() < 80) {
                    s_role[tool ? 1 : 0][i] = role; s_roleLogs++;
                    uint8_t ow = VEHICLE_OWNER_NONE;
                    if (nid >= 0) { std::lock_guard<std::mutex> lock(g_VehicleStateMutex); ow = g_VehicleAuthority[nid].ownerId; }
                    LOGI("[TOW] %s=%u arac=%d rol=%s sahip=%u yerelAktif=%d uzakAktif=%d host=%d", tool ? "alet" : "romork",
                         (unsigned)i, nid, towedByMe ? "YAYIN" : "ALICI", (unsigned)ow, (int)(av == activeVeh),
                         (int)IsRemoteActiveVehicle(nid), (int)g_IsHost.load());
                }
            }
            if (towedByMe) mineArr[i] = true;
            else if (mineArr[i]) {                              // baska oyuncunun araci cekiyor: akisi o yayinlar
                mineArr[i] = false;
                g_LocalStationarySince[sid] = 0;
                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                g_LastSentLocalVehicles[sid].valid = false;
            }
        } else {
            // Serbest nesne (pulluk/romork araca bagli degil): tek yonlu yayin kurali (balya sistemiyle ayni).
            //  1) Yerel otorite sahibi arac yakindaysa ve nesne oynuyorsa: bu cihaz yayinlar (araci iten cihaz).
            //  2) Yakinda otorite araci yoksa, oynayan nesneyi host yayinlar (ust uste duran aletlerin kaymasi tek kaynaktan gelir).
            //  3) Baska cihazdan taze akis varsa ve yakinda yerel otorite araci yoksa bu cihaz yayini birakir.
            // Yavas kayma (kare basina mm) kare-kare karsilastirmada "hareket" sayilmaz: ~1.2 cm'lik referans konumdan sapma da hareket say.
            static float s_ref[2][256][3];
            static bool s_refOk[2][256];
            const int ti = tool ? 1 : 0;
            if (i < 256) {
                if (!s_refOk[ti][i] || fabsf(x - s_ref[ti][i][0]) > 0.012f || fabsf(y - s_ref[ti][i][1]) > 0.012f ||
                    GetAngleDelta(angle, s_ref[ti][i][2]) > 0.01f) {
                    if (s_refOk[ti][i]) moving = true;
                    s_ref[ti][i][0] = x; s_ref[ti][i][1] = y; s_ref[ti][i][2] = angle; s_refOk[ti][i] = true;
                }
            }
            // Bu nesneyi uzaktan gelen akis oynatiyorsa (sensor modu) gorulen hareket YEREL itme DEGILDIR (balyadaki
            // "moved && !sensor && !hasTarget" kurali). Aksi halde alici cihaz, kendi yakinindaki araci sebep gosterip nesneyi
            // sahiplenir; iki cihaz birbirinin akisini ezer, sensor surekli acilip kapanir ve fizik patlar (itince cokme).
            if (g_AttSensorBody[ti][i] != 0 || nowMs - g_AttSensorMs[ti][i] < 400) moving = false;
            const bool wasMine = mineArr[i];
            bool nearAuth = false;
            if (moving || wasMine) {
                if (!vptsDone) { BaleLocalVehiclePoints(game, &vpts); vptsDone = true; }
                for (size_t k = 0; k < vpts.size(); ++k) {
                    const float dx = vpts[k].x - x, dy = vpts[k].y - y;
                    if (dx * dx + dy * dy <= kLooseNearVehicleR2) { nearAuth = true; break; }
                }
            }
            bool remoteFresh = false;
            {
                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                const VehicleRemoteState& rs = g_RemoteVehicles[sid];
                remoteFresh = rs.valid && rs.ownerId != localOwner && rs.ownerId != VEHICLE_OWNER_NONE &&
                              nowMs >= rs.lastReceiveMs && (nowMs - rs.lastReceiveMs) < kLooseRemoteFreshMs;
            }
            if (!wasMine && moving) {
                if (nearAuth || (g_IsHost.load() && !remoteFresh)) mineArr[i] = true;
            } else if (wasMine && !nearAuth && remoteFresh) {
                mineArr[i] = false;                                 // baska cihaz iteliyor: akisi ona birak
                g_LocalStationarySince[sid] = 0;
                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                g_LastSentLocalVehicles[sid].valid = false;
                continue;
            }
        }
        if (!mineArr[i]) continue;

        if (moving || av != 0) g_LocalStationarySince[sid] = 0;
        else if (g_LocalStationarySince[sid] == 0) g_LocalStationarySince[sid] = nowMs;
        const bool settled = av == 0 && g_LocalStationarySince[sid] != 0 &&
                             (nowMs - g_LocalStationarySince[sid]) >= VEHICLE_STOP_RELEASE_MS;

        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
            VehicleSampleState& sent = g_LastSentLocalVehicles[sid];
            changed = !sent.valid || sent.moving != moving ||
                      fabsf(x - sent.x) > POSITION_EPSILON ||
                      fabsf(y - sent.y) > POSITION_EPSILON ||
                      GetAngleDelta(angle, sent.angle) > ANGLE_EPSILON ||
                      (nowMs - sent.timeMs) >= VEHICLE_KEEPALIVE_MS;
            if (changed) { sent.valid = true; sent.moving = moving; sent.x = x; sent.y = y; sent.angle = angle; sent.timeMs = nowMs; }
        }
        if (changed) QueueTrailerPosition(i, type, x, y, angle, moving, tool);
        if (settled) {                                          // son durum gonderildi: yayini birak
            mineArr[i] = false;
            std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
            g_LastSentLocalVehicles[sid].valid = false;
        }
    }
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
        // Isciye (AI) verilen arac aktif arac olmasa da hareket ediyorsa sahipligini iste/yayinla.
        const bool aiDriven = (*(uint32_t*)(vehicle + 0x730) != 0) && !g_AiMirrored[vehicleId];
        if ((vehicleId == activeVehicleId || aiDriven) && moving &&
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
        // Iscisi UZAKTAN baslatilmis (taklitci) arac konum yayinlamaz: konumu baslatan cihaz belirler.
        const bool publishActiveVehicle =
            ((vehicleId == activeVehicleId) && !g_AiMirrored[vehicleId]) || aiDriven;

        const bool publishOwnedVehicle =
            (ownerId == localOwner) && !g_AiMirrored[vehicleId];

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
            if (changed) QueueVehiclePosition(vehicleId, x, y, angle, moving, *(float*)(vehicle + 0x568));
        }
    }

    CaptureAndQueueLocalTrailerStates(game, localOwner, nowMs);
    CaptureAndQueueLocalTrailerStates(game, localOwner, nowMs, true);
    TrackTrailerAttachAndFill(game, nowMs);
    TrackToolAttachAndFill(game, localOwner, nowMs, activeVehicleId);
    CaptureAndQueueCombineStates(game, localOwner, nowMs, activeVehicleId);
    g_EntityResendAll.store(false);
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

// Romorklar ve aletler: sahiplik tablosu yok; sira + tip uyusuyorsa ve yerelde bir araca bagli degilse yerlestir.
static void ApplyRemoteAttachablePoses(uintptr_t game, uint8_t localOwner, uint64_t nowMs, float decay, bool tool) {
    const uint32_t tn = tool ? ToolCount(game) : TrailerCount(game);
    const uint16_t idBase = tool ? TOOL_ID_BASE : TRAILER_ID_BASE;
    const bool* mineArr = tool ? g_ToolMine : g_TrailerMine;
    const uintptr_t bodyOff = tool ? 0x228 : 0x180;
    const uintptr_t avOff = tool ? 0x23C : 0x184;
    const int ti = tool ? 1 : 0;
    for (uint32_t i = 0; i < tn && i < 256; ++i) {
        const uint16_t sid = (uint16_t)(idBase + i);
        // Uzaktan hareket bitti / nesne baglandi / bu cihaz yayinlamaya basladi: yerel fizigi geri ver.
        if (g_AttSensorBody[ti][i] != 0) {
            const uintptr_t tp0 = tool ? RawToolSlot(game, i) : RawTrailerSlot(game, i);
            const uintptr_t b0 = tp0 ? *(uintptr_t*)(tp0 + bodyOff) : 0;
            const bool same = (b0 == g_AttSensorBody[ti][i]);
            const bool attached0 = tp0 && *(uintptr_t*)(tp0 + avOff) != 0;
            if (!same || mineArr[i] || attached0 || nowMs - g_AttSensorMs[ti][i] > kLooseSensorIdleMs) {
                if (same) B2BodyRestoreSensor(b0, g_AttSensorMask[ti][i]);
                g_AttSensorBody[ti][i] = 0; g_AttSensorMask[ti][i] = 0;
            }
        }
        VehicleRemoteState state;
        {
            std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
            VehicleRemoteState& stored = g_RemoteVehicles[sid];
            if (!stored.valid || stored.ownerId == localOwner) continue;
            stored.errX *= decay;
            stored.errY *= decay;
            stored.errA *= decay;
            if (fabsf(stored.errX) + fabsf(stored.errY) + fabsf(stored.errA) < 0.003f) {
                stored.errX = stored.errY = stored.errA = 0.0f;
            }
            state = stored;
        }
        const bool offsetGone = (state.errX == 0.0f && state.errY == 0.0f && state.errA == 0.0f);
        if (!state.moving && offsetGone && state.appliedSequence == state.sequence) continue;
        if (mineArr[i]) continue;                                        // bu cihaz yayinliyor

        const uintptr_t tp = tool ? RawToolSlot(game, i) : RawTrailerSlot(game, i);
        const uint8_t wantType = tool ? g_RemoteToolType[i] : g_RemoteTrailerType[i];
        if (tp == 0 || *(uint32_t*)(tp + 0x10) != wantType) continue;
        const uintptr_t av = *(uintptr_t*)(tp + avOff);
        // Bagli nesne: (a) bu cihazin surdugu / sahip oldugu araca bagliysa yerel fizik gecerli (zaten yukarida mine);
        // (b) BASKA oyuncunun surdugu araca bagliysa nesnenin konumu/acisi surucunun cihazindan gelen akisla uygulanir.
        // Eskiden sadece eklem (joint) takip ediyordu: geri geri giderken romork/alet her cihazda fizigin kendi
        // dengesizligine gore farkli yone kivriliyordu (hostta sola, clientte saga). Artik gercek aci surucudeki gibidir.
        bool towedByRemote = false;
        if (av != 0) {
            const int nid = NetIdOfVehicle(av);
            if (nid < 0) continue;
            if (IsTowPublisher(nid, av == LocalActiveVehiclePtr(game), localOwner)) continue;   // yayinci bu cihaz: yerel fizik gecerli
            uint8_t ow = VEHICLE_OWNER_NONE;
            { std::lock_guard<std::mutex> lock(g_VehicleStateMutex); ow = g_VehicleAuthority[nid].ownerId; }
            if (ow == localOwner) continue;
            // Sahipli arac: akis sahibinden gelmeli. Sahipsiz (park): taze bir akis (surucu icerde ya da host) yeter;
            // aksi halde park halinde fizik romorku kendi yanina yatirir.
            if (ow != VEHICLE_OWNER_NONE) { if (ow != state.ownerId) continue; }
            else if (nowMs > state.lastReceiveMs && nowMs - state.lastReceiveMs > 3000) continue;
            towedByRemote = true;
        }
        const uintptr_t body = *(uintptr_t*)(tp + bodyOff);
        if (body == 0) continue;

        float poseX, poseY, poseAngle;
        PredictRemotePose(state, nowMs, &poseX, &poseY, &poseAngle);
        if (!(poseX == poseX) || !(poseY == poseY) || !(poseAngle == poseAngle) ||
            fabsf(poseX) > 1.0e5f || fabsf(poseY) > 1.0e5f) continue;          // bozuk konum: fizik govdesine yazma
        TestB2Vec2 position;
        position.x = poseX + state.errX;
        position.y = poseY + state.errY;
        if (towedByRemote) {                                             // araba bagli: sensor YOK, arac gibi hizla yerlestir
            {   // tani: yerel aci ile gelen aci farki (iki cihaz ayni aciya gelmeli); en cok 2 sn'de bir, 60 satir
                static uint64_t s_lastLogMs = 0;
                static std::atomic<int> s_applyLogs(0);
                if (s_applyLogs.load() < 60 && nowMs - s_lastLogMs > 2000) {
                    s_lastLogMs = nowMs; s_applyLogs++;
                    LOGI("[TOW] uygula %s=%u yerelAci=%.2f gelenAci=%.2f fark=%.2f hareket=%d yas=%llu",
                         tool ? "alet" : "romork", (unsigned)i, *(float*)(body + 0x40), poseAngle,
                         GetSignedAngleDelta(*(float*)(body + 0x40), poseAngle), (int)state.moving,
                         (unsigned long long)(nowMs - state.lastReceiveMs));
                }
            }
            g_b2BodySetTransform((void*)body, &position, poseAngle + state.errA);
            if (state.moving) {
                SetRemoteBodyVelocity(body, state.vx, state.vy, state.angularVelocity);
            } else {
                SetRemoteBodyVelocity(body, 0.0f, 0.0f, 0.0f);
                if (offsetGone) {
                    std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                    if (g_RemoteVehicles[sid].sequence == state.sequence) g_RemoteVehicles[sid].appliedSequence = state.sequence;
                }
            }
            continue;
        }
        // Uzaktan yonetilen serbest nesne: yerel carpisma tepkisi kapali (sensor) -> ust uste duran/itilen aletler
        // bu cihazda ayrica kaymaz, yalnizca gelen akisi izler.
        if (g_AttSensorBody[ti][i] != body) {
            if (g_AttSensorBody[ti][i] != 0) { g_AttSensorBody[ti][i] = 0; g_AttSensorMask[ti][i] = 0; }
            const uint32_t m = B2BodyMakeSensor(body);
            g_AttSensorBody[ti][i] = body;
            g_AttSensorMask[ti][i] = m;
        }
        g_AttSensorMs[ti][i] = nowMs;
        g_b2BodySetTransform((void*)body, &position, poseAngle + state.errA);
        // Sensor modunda govdeye hiz VERILMEZ (balyadaki SetBalePose gibi): hiz, sensor geri kapaninca nesneyi firlatirdi.
        // Govde sadece uyanik tutulur ki oyun gorunumu / fizigi yenilesin.
        SetRemoteBodyVelocity(body, 0.0f, 0.0f, 0.0f);
        if (*(int*)body != 0) { *(uint16_t*)(body + 4) |= 2; *(float*)(body + 0x90) = 0.0f; }

        if (!state.moving) {
            if (offsetGone) {
                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                if (g_RemoteVehicles[sid].sequence == state.sequence) g_RemoteVehicles[sid].appliedSequence = state.sequence;
            }
        }
    }
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

    ApplyPendingPoolOps(game, localOwner);
    ApplyPendingTrailerEvents(game, localOwner, nowMs);
    ApplyPendingFieldOps(game);

    ApplyRemoteAttachablePoses(game, localOwner, nowMs, decay, false);
    ApplyRemoteAttachablePoses(game, localOwner, nowMs, decay, true);
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
    bool moving,
    uint8_t steerQ = 0
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
    state.steerQ = steerQ;
}

// Ag uzerindeki vehicleId romork ise (0x8000 | tip<<8 | sira) durum dizisi numarasina cevirir.
static uint16_t DecodeWireVehicleId(uint16_t wire, bool* isTrailer) {
    *isTrailer = (wire & TRAILER_WIRE_FLAG) != 0;
    if (!*isTrailer) return wire;
    const uint16_t idx = wire & 0xFF;
    const uint8_t type = (uint8_t)((wire >> 8) & 0x3F);
    if (wire & TOOL_WIRE_FLAG) {                       // alet
        const uint16_t t = idx < 64 ? idx : 63;
        g_RemoteToolType[t] = type;
        return (uint16_t)(TOOL_ID_BASE + t);
    }
    g_RemoteTrailerType[idx] = type;
    return (uint16_t)(TRAILER_ID_BASE + idx);
}

static void HandleVehiclePositionPacket(
    const VehiclePositionPacket& pkt
) {
    bool isTrailer = false;
    const uint16_t vid = DecodeWireVehicleId(pkt.vehicleId, &isTrailer);
    if (!isTrailer) HostAdoptIfMoving(pkt.ownerId, vid, (pkt.moving & 1) != 0);
    StoreRemoteVehicleState(
        pkt.ownerId,
        vid,
        pkt.x,
        pkt.y,
        pkt.angle,
        pkt.sequence,
        (pkt.moving & 1) != 0,
        isTrailer ? 0 : (uint8_t)(pkt.moving >> 1)
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

        bool isTrailer = false;
        const uint16_t vid = DecodeWireVehicleId(entry.vehicleId, &isTrailer);
        if (!isTrailer) HostAdoptIfMoving(header.ownerId, vid, (entry.moving & 1) != 0);
        StoreRemoteVehicleState(
            header.ownerId,
            vid,
            entry.x,
            entry.y,
            entry.angle,
            entry.sequence,
            (entry.moving & 1) != 0,
            isTrailer ? 0 : (uint8_t)(entry.moving >> 1)
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
    memset(g_TrailerMine, 0, sizeof(g_TrailerMine));
    memset(g_TrTrack, 0, sizeof(g_TrTrack));
    memset(g_ToolTrack, 0, sizeof(g_ToolTrack));
    memset(g_ToolMine, 0, sizeof(g_ToolMine));
    memset(g_AttSensorBody, 0, sizeof(g_AttSensorBody));
    memset(g_AttSensorMask, 0, sizeof(g_AttSensorMask));
    memset(g_AttSensorMs, 0, sizeof(g_AttSensorMs));
    memset(g_CbTrack, 0, sizeof(g_CbTrack));
    memset(g_AiMirrored, 0, sizeof(g_AiMirrored));
    g_RemoteHornUntilMs.store(0);
    {
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        g_PendingTrailerAttach.clear();
        g_PendingEntityStates.clear();
        g_PendingFieldOps.clear();
        g_PendingPoolOps.clear();
        g_FieldOpOut.clear();
        g_PendingBaleOps.clear();
        g_PendingBaleList.clear();
        g_HasPendingBaleList = false;
        g_HasPendingMapClock = false;
        g_HasPendingDemand = false;
        g_HasPendingMission = false;
        g_PendingFieldSnaps.clear();
        g_PendingFieldOwn.clear();
        g_PendingRestPoses.clear();
    }
    g_FsValid.store(false); g_FsMap.store(0); g_FsScanMs = 0;
    g_MsPrevValid = false; g_MsIgnoreUntil = 0; g_MissionDoneFlag.store(false);
    g_DemandHash = 0; g_DemandSendMs = 0; g_MissionSendMs = 0; g_PriceTblHash = 0; g_PriceTblMs = 0;
    g_MsSentState = -1; g_MsSentItem = -1; g_MsSentSpawn = -1;
    g_OwnSnapValid = false; g_OwnMapPtr = 0;
    g_BaleTrk.clear();
    g_BaleTrkValid = false;
    g_BaleLocalChangeMs = 0; g_BaleRemoteMs = 0; g_BaleListSendMs = 0;
    memset(g_BaleRecent, 0, sizeof(g_BaleRecent));
    g_McPrevValid = false; g_McOffsetSet = false; g_McBehindSinceMs = 0; g_McLastAdv = 0; g_McCycles = 0;
    g_PoolSnapValid = false;
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
        else if (type == PACKET_MONEY_DELTA) { size = sizeof(MoneyPacket); relayIt = true; }
        else if (type == PACKET_MONEY_SET) size = sizeof(MoneyPacket);
        else if (type == PACKET_TIME_SYNC) size = sizeof(TimeSyncPacket);
        else if (type == PACKET_TRAILER_ATTACH) { size = sizeof(TrailerAttachPacket); relayIt = true; }
        else if (type == PACKET_ENTITY_STATE) { size = sizeof(EntityStatePacket); relayIt = true; }
        else if (type == PACKET_FIELD_OP) { size = sizeof(FieldOpPacket); relayIt = true; }
        else if (type == PACKET_POOL_OP) { size = sizeof(PoolOpPacket); relayIt = true; }
        else if (type == PACKET_BALE_OP) { size = sizeof(BaleOpPacket); relayIt = true; }
        else if (type == PACKET_MAP_CLOCK) size = sizeof(MapClockPacket);
        else if (type == PACKET_FIELD_SNAP) { size = sizeof(FieldSnapPacket); relayIt = true; }
        else if (type == PACKET_DEMAND) size = sizeof(DemandPacket);
        else if (type == PACKET_MISSION) size = sizeof(MissionPacket);
        else if (type == PACKET_MISSION_DONE) size = sizeof(MissionDonePacket);
        else if (type == PACKET_FIELD_OWN) { size = sizeof(FieldOwnPacket); relayIt = true; }
        else if (type == PACKET_BALE_LIST) {
            if (buffered < sizeof(BaleListHeader)) break;
            if (buf[1] > kBaleMax) return false;
            size = sizeof(BaleListHeader) + (size_t)buf[1] * 12;
        }
        else if (type == PACKET_ACTIVE_VEHICLE) { size = sizeof(ActiveVehiclePacket); relayIt = true; }
        else if (type == PACKET_VEHICLE_NETMAP) {
            if (buffered < 3) break;
            uint16_t cnt;
            memcpy(&cnt, buf + 1, 2);
            if (cnt > VEHICLE_SLOT_LIMIT) return false;
            size = 3 + (size_t)cnt * 2;
        }
        else if (type == PACKET_SYNC_REQUEST) size = 1;
        else if (type == PACKET_RESUME) size = 1;
        else if (type == PACKET_HOST_RESUME) size = 1;
        else if (type == PACKET_REST_POSE) {
            if (buffered < sizeof(RestPoseHeader)) break;
            if (buf[1] > kRestPoseMax) return false;
            size = sizeof(RestPoseHeader) + (size_t)buf[1] * sizeof(RestPoseEntry);
        }
        else if (type == PACKET_WORLD_DELTA) {
            if (buffered < sizeof(WorldDeltaHeader)) break;
            if (buf[2] > 16) return false;
            size = sizeof(WorldDeltaHeader) + (size_t)buf[2] * sizeof(WorldDeltaEntry);
            relayIt = true;
        }
        else if (type == PACKET_PRICE_TABLE) {
            if (buffered < sizeof(PriceTableHeader)) break;
            if (buf[1] > kWorldSites) return false;
            size = sizeof(PriceTableHeader) + PriceTableFloats(buf[1]) * 4;
        }
        else if (type == PACKET_TRAFFIC) {
            if (buffered < sizeof(TrafficHeader)) break;
            if (buf[2] > kTrafficMax) return false;
            size = sizeof(TrafficHeader) + (size_t)buf[2] * sizeof(TrafficEntry);
            relayIt = true;
        }
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
        } else if (type == PACKET_ACTIVE_VEHICLE) {
            ActiveVehiclePacket ap;
            memcpy(&ap, buf, sizeof(ap));
            if (ap.originId != g_LocalPlayerId.load()) StoreRemoteActive(ap.originId, ap.netId);
        } else if (type == PACKET_TIME_SYNC) {
            TimeSyncPacket tp;
            memcpy(&tp, buf, sizeof(tp));
            if (!g_IsHost.load() && !fromPeer && tp.time == tp.time && tp.realTime == tp.realTime) {
                std::lock_guard<std::mutex> lock(g_TimeMutex);
                g_TimePending = true; g_TimeHost = tp.time; g_TimeHostReal = tp.realTime;
            }
        } else if (type == PACKET_MONEY_DELTA) {
            MoneyPacket mp;
            memcpy(&mp, buf, sizeof(mp));
            if (kSharedMoney && mp.originId != g_LocalPlayerId.load() && MoneyOk(mp.value)) {
                std::lock_guard<std::mutex> lock(g_MoneyMutex);
                if (g_MoneyIncoming.size() < 1024) g_MoneyIncoming.push_back(mp.value);
            }
        } else if (type == PACKET_MONEY_SET) {
            MoneyPacket mp;
            memcpy(&mp, buf, sizeof(mp));
            if (kSharedMoney && !g_IsHost.load() && !fromPeer && MoneyOk(mp.value)) {
                std::lock_guard<std::mutex> lock(g_MoneyMutex);
                g_MoneySetPending = true;
                g_MoneySetValue = mp.value;
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
        } else if (type == PACKET_TRAILER_ATTACH) {
            TrailerAttachPacket tp;
            memcpy(&tp, buf, sizeof(tp));
            if (fromPeer) { tp.ownerId = (uint8_t)peerId; memcpy(buf, &tp, sizeof(tp)); }
            if (tp.ownerId != g_LocalPlayerId.load() && tp.vehNetId < VEHICLE_SLOT_LIMIT) {
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                if (g_PendingTrailerAttach.size() >= 64) g_PendingTrailerAttach.erase(g_PendingTrailerAttach.begin());
                g_PendingTrailerAttach.push_back(tp);
            }
        } else if (type == PACKET_ENTITY_STATE) {
            EntityStatePacket ep;
            memcpy(&ep, buf, sizeof(ep));
            if (fromPeer) { ep.ownerId = (uint8_t)peerId; memcpy(buf, &ep, sizeof(ep)); }
            if (ep.kind == 3 && ep.ownerId != g_LocalPlayerId.load()) {                 // korna
                g_RemoteHornUntilMs.store((ep.flags & 1) ? NowMs() + 2500 : 0);
                g_RemoteHornOrigin.store((int)ep.ownerId);
            } else if (ep.ownerId != g_LocalPlayerId.load() && (ep.kind == 1 || ep.kind == 2 || ep.kind == 4)) {
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                // Ayni varlik icin eski bekleyen durum varsa yenisiyle degistir.
                bool replaced = false;
                for (EntityStatePacket& q : g_PendingEntityStates)
                    if (q.kind == ep.kind && q.id == ep.id) {
                        const uint8_t eb = (uint8_t)((q.flags | ep.flags) & kEdgeBit);   // kenar bilgisi kaybolmasin
                        q = ep; q.flags |= eb; replaced = true; break;
                    }
                if (!replaced) {
                    if (g_PendingEntityStates.size() >= 128) g_PendingEntityStates.erase(g_PendingEntityStates.begin());
                    g_PendingEntityStates.push_back(ep);
                }
            }
        } else if (type == PACKET_FIELD_OP) {
            FieldOpPacket fp;
            memcpy(&fp, buf, sizeof(fp));
            if (fromPeer) { fp.ownerId = (uint8_t)peerId; memcpy(buf, &fp, sizeof(fp)); }
            if (kSyncFieldWork && fp.ownerId != g_LocalPlayerId.load() && fp.wtype < 16) {
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                if (g_PendingFieldOps.size() >= 1024) g_PendingFieldOps.erase(g_PendingFieldOps.begin());
                g_PendingFieldOps.push_back(fp);
            }
        } else if (type == PACKET_POOL_OP) {
            PoolOpPacket pp;
            memcpy(&pp, buf, sizeof(pp));
            if (fromPeer) { pp.ownerId = (uint8_t)peerId; memcpy(buf, &pp, sizeof(pp)); }
            if (pp.ownerId != g_LocalPlayerId.load() && (pp.kind == 1 || pp.kind == 2)) {
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                if (g_PendingPoolOps.size() >= 32) g_PendingPoolOps.erase(g_PendingPoolOps.begin());
                g_PendingPoolOps.push_back(pp);
            }
        } else if (type == PACKET_BALE_OP) {
            BaleOpPacket bp;
            memcpy(&bp, buf, sizeof(bp));
            if (fromPeer) { bp.ownerId = (uint8_t)peerId; memcpy(buf, &bp, sizeof(bp)); }
            if (kSyncBales && bp.ownerId != g_LocalPlayerId.load() && bp.op >= 1 && bp.op <= 3) {
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                bool merged = false;
                if (bp.op == 3) {                                          // ayni balyanin bekleyen tasimasi: zincirle (sadece son konum onemli)
                    for (size_t k = g_PendingBaleOps.size(); k-- > 0;) {
                        BaleOpPacket& q = g_PendingBaleOps[k];
                        if (q.op != 3 || q.ownerId != bp.ownerId) continue;
                        if (fabsf(q.x - bp.ox) < 0.01f && fabsf(q.y - bp.oy) < 0.01f) { q.x = bp.x; q.y = bp.y; q.a = bp.a; merged = true; break; }
                    }
                }
                if (!merged) {
                    if (g_PendingBaleOps.size() >= 128) {                  // tasimalar once atilir; ekleme/silme olaylari korunur
                        size_t drop = 0;
                        for (size_t k = 0; k < g_PendingBaleOps.size(); ++k) if (g_PendingBaleOps[k].op == 3) { drop = k; break; }
                        g_PendingBaleOps.erase(g_PendingBaleOps.begin() + drop);
                    }
                    g_PendingBaleOps.push_back(bp);
                }
            }
        } else if (type == PACKET_FIELD_OWN) {
            FieldOwnPacket fo;
            memcpy(&fo, buf, sizeof(fo));
            if (fromPeer) { fo.ownerId = (uint8_t)peerId; memcpy(buf, &fo, sizeof(fo)); }
            if (kSyncFieldOwn && fo.ownerId != g_LocalPlayerId.load() && fo.idx < kFieldRecMax) {
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                if (g_PendingFieldOwn.size() >= 32) g_PendingFieldOwn.erase(g_PendingFieldOwn.begin());
                g_PendingFieldOwn.push_back(fo);
            }
        } else if (type == PACKET_BALE_LIST) {
            if (kSyncBales && !g_IsHost.load() && !fromPeer) {
                const size_t cnt = buf[1];
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                g_PendingBaleList.assign(cnt * 3, 0.0f);
                if (cnt) memcpy(g_PendingBaleList.data(), buf + sizeof(BaleListHeader), cnt * 12);
                g_HasPendingBaleList = true;
            }
        } else if (type == PACKET_MAP_CLOCK) {
            if (kSyncMapClock && !g_IsHost.load() && !fromPeer) {
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                memcpy(&g_PendingMapClock, buf, sizeof(MapClockPacket));
                g_HasPendingMapClock = true;
            }
        } else if (type == PACKET_FIELD_SNAP) {
            FieldSnapPacket sp;
            memcpy(&sp, buf, sizeof(sp));
            if (fromPeer) { sp.ownerId = (uint8_t)peerId; memcpy(buf, &sp, sizeof(sp)); }
            if (kSyncFieldSnap && sp.ownerId != g_LocalPlayerId.load() && sp.idx < kFsMaxCells) {
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                bool replaced = false;
                for (size_t k = 0; k < g_PendingFieldSnaps.size(); ++k)
                    if (g_PendingFieldSnaps[k].idx == sp.idx) { g_PendingFieldSnaps[k] = sp; replaced = true; break; }
                if (!replaced) {
                    if (g_PendingFieldSnaps.size() >= 200) g_PendingFieldSnaps.erase(g_PendingFieldSnaps.begin());
                    g_PendingFieldSnaps.push_back(sp);
                }
            }
        } else if (type == PACKET_DEMAND) {
            if (kSyncDemand && !g_IsHost.load() && !fromPeer) {
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                memcpy(&g_PendingDemand, buf, sizeof(DemandPacket));
                g_HasPendingDemand = true;
            }
        } else if (type == PACKET_MISSION) {
            if (kSyncMission && !g_IsHost.load() && !fromPeer) {
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                memcpy(&g_PendingMission, buf, sizeof(MissionPacket));
                g_HasPendingMission = true;
            }
        } else if (type == PACKET_MISSION_DONE) {
            if (kSyncMission && g_IsHost.load() && fromPeer) g_MissionDoneFlag.store(true);
        } else if (type == PACKET_RESUME) {
            if (g_IsHost.load() && fromPeer) {                 // client geri dondu: tarlalar + duran araclar + normal tam senkron
                g_FsResendAll.store(true);
                g_RestResendAll.store(true);
                g_HostForceTable.store(true);
                g_ForceResendAll.store(true);
                g_EntityResendAll.store(true);
            }
        } else if (type == PACKET_HOST_RESUME) {
            if (!g_IsHost.load() && !fromPeer) {               // host geri dondu: bu cihaz kendi tarla/arac durumunu yollasin
                g_FsResendAll.store(true);
                g_RestResendAll.store(true);
                g_ForceResendAll.store(true);
                g_EntityResendAll.store(true);
            }
        } else if (type == PACKET_WORLD_DELTA) {
            if (kSyncWorld && buf[1] != g_LocalPlayerId.load() && buf[2] > 0 && buf[2] <= 16) {
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                for (uint8_t k = 0; k < buf[2] && g_PendingWorld.size() < 256; ++k) {
                    WorldDeltaEntry e;
                    memcpy(&e, buf + sizeof(WorldDeltaHeader) + (size_t)k * sizeof(WorldDeltaEntry), sizeof(e));
                    g_PendingWorld.push_back(e);
                }
            }
        } else if (type == PACKET_PRICE_TABLE) {
            if (kSyncWorld && !g_IsHost.load() && !fromPeer && size <= kPriceTableMaxBytes) {
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                memcpy(g_PendingPriceBuf, buf, size);
                g_PendingPriceLen = size;
            }
        } else if (type == PACKET_TRAFFIC) {
            if (buf[1] != g_LocalPlayerId.load()) g_TrLastRxMs.store(NowMs());
            if (buf[1] != g_LocalPlayerId.load() && buf[2] > 0 && buf[2] <= kTrafficMax) {
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                for (uint8_t k = 0; k < buf[2] && g_PendingTraffic.size() < 64; ++k) {
                    TrafficPending e;
                    e.owner = buf[1];
                    memcpy(&e.e, buf + sizeof(TrafficHeader) + (size_t)k * sizeof(TrafficEntry), sizeof(TrafficEntry));
                    g_PendingTraffic.push_back(e);
                }
            }
        } else if (type == PACKET_REST_POSE) {
            if (buf[1] > 0 && buf[1] <= kRestPoseMax) {
                std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
                for (uint8_t k = 0; k < buf[1] && g_PendingRestPoses.size() < 64; ++k) {
                    RestPoseEntry e;
                    memcpy(&e, buf + sizeof(RestPoseHeader) + (size_t)k * sizeof(RestPoseEntry), sizeof(e));
                    g_PendingRestPoses.push_back(e);
                }
            }
        } else if (type == PACKET_SYNC_REQUEST) {
            if (g_IsHost.load() && fromPeer) {
                g_HostForceTable.store(true);
                g_ForceResendAll.store(true);
                g_EntityResendAll.store(true);          // takili romorklar + doluluk + bicer durumlari yeniden
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
    FieldSnapPauseTick();                                   // oyun durduysa son tarla durumunu yolla (duraklama devri)
    PauseReleaseTick();                                     // oyun durduysa sahip olunan araclari birak
    {   // arac ekleme/silme olaylari (guvenilir kanal)
        std::lock_guard<std::mutex> lock(g_VehicleEventMutex);
        if (!g_VehicleEventOut.empty()) {
            out.insert(out.end(), g_VehicleEventOut.begin(), g_VehicleEventOut.end());
            g_VehicleEventOut.clear();
        }
        if (!g_FieldOpOut.empty()) {
            out.insert(out.end(), g_FieldOpOut.begin(), g_FieldOpOut.end());
            g_FieldOpOut.clear();
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
    bool hostPaused = false;                 // host oyunu su an duraklamis mi (asagida algilanir)
    int pausedReleasedNetId = -1;            // duraklarken sahipligi birakilan arac

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
                        g_MoneySendSet.store(true);       // yeni oyuncu host'un parasini alsin
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
            {   // client oyunu arka plana attiysa (ping dahil hicbir sey gelmiyor) araclarini birak: baskalari kullanabilsin
                static bool s_silentReleased[MAX_PLAYERS];
                if (p->id < MAX_PLAYERS) {
                    if (nowT - p->lastRecvMs > 3000) {
                        if (!s_silentReleased[p->id]) {
                            s_silentReleased[p->id] = true;
                            std::vector<uint16_t> owned;
                            {
                                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                                for (uint16_t v = 0; v < VEHICLE_SLOT_LIMIT; ++v)
                                    if (g_VehicleAuthority[v].ownerId == p->id) owned.push_back(v);
                            }
                            for (uint16_t v : owned) ReleaseVehicleAuthority(v, p->id, true);
                            if (!owned.empty()) g_HostForceTable.store(true);
                            LOGI("[PAUSE] oyuncu %u sessiz (arka plan?): %u aracin sahipligi birakildi", (unsigned)p->id, (unsigned)owned.size());
                        }
                    } else {
                        s_silentReleased[p->id] = false;
                    }
                }
            }
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
            // Host duraklamasi: dunya mantigi (updateStateBase) durduysa host'un araci hicbir sey yapmaz.
            // Aracin sahipligini host'un AG THREAD'i birakir (oyun thread'i durdugu icin kendisi birakamaz);
            // boylece client'lar araci surebilir. Host oyuna donunca (updateStateBase tekrar calisinca) normale doner.
            {
                const long long sb = g_LastStateBaseMs.load();
                const bool pausedNow = g_LocalInGame.load() && sb > 0 && (nowT - sb) > HOST_PAUSE_DETECT_MS;
                if (pausedNow && !hostPaused) {
                    hostPaused = true;
                    pausedReleasedNetId = -1;
                    const int nid = *(volatile int*)&g_LocalActiveNetId;
                    const uint8_t hostId = g_LocalPlayerId.load();
                    if (nid >= 0 && nid < (int)VEHICLE_SLOT_LIMIT && hostId < MAX_PLAYERS &&
                        ReleaseVehicleAuthority((uint16_t)nid, hostId, true)) {
                        pausedReleasedNetId = nid;
                        g_HostForceTable.store(true);          // tabloyu hemen yay
                    }
                    LOGI("[HOSTPAUSE] host oyunu durdu, aktif arac=%d sahiplik birakildi=%d",
                         nid, (int)(pausedReleasedNetId >= 0));
                } else if (!pausedNow && hostPaused) {
                    hostPaused = false;
                    LOGI("[HOSTPAUSE] host oyuna dondu (birakilan arac=%d)", pausedReleasedNetId);
                    pausedReleasedNetId = -1;
                    g_HostForceTable.store(true);
                }
            }

            // Oyun dongusu duruyorsa bile (host arka planda / bildirim paneli) kisa sure "canli" say:
            // ping ve sahiplik tablosu kesilmez, client'lar baglantidan atilmaz. Surec zorla kapatilirsa
            // ya da duraklama cok uzarsa (HOST_PAUSE_GRACE_MS) sinyaller yine kesilir.
            const long long sinceUpd = nowT - g_LastGameUpdateMs.load();
            const bool hostAlive = sinceUpd < HOST_ALIVE_WINDOW_MS || (hostPaused && sinceUpd < HOST_PAUSE_GRACE_MS);
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
                if (strcmp(buffer, "FS12_PING") == 0 && g_IsHost.load()) {
                    std::string reply = "FS12_PONG|" + std::string(g_Nickname) + "'s Room";
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
    
    std::string pingMsg = "FS12_PING";
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
            if (msg.find("FS12_PONG|") == 0) {
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

                if (g_ImGuiInitialized && ImGui::GetCurrentContext() != nullptr && WK::g_FirstPerson && WK::g_LocalWalker) {
                    const ImVec2 ds = ImGui::GetIO().DisplaySize;               // v7: birinci sahis kafa cevirme (surukleme)
                    WK::TouchLook(*outEvent, AMotionEvent_getAction(*outEvent), ds.x, ds.y);
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
                        // Release position is needed for click-on-release behavior;
                        // otherwise a finger may be dragged out and still activate a button.
                        g_TouchX.store(AMotionEvent_getX(*outEvent, 0));
                        g_TouchY.store(AMotionEvent_getY(*outEvent, 0));
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
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    // Keep the full hit area stable, but only activate when the finger is released
    // inside it (the same click-release behavior as a normal ImGui button).
    ImGui::SetCursorScreenPos(pos);
    const bool clicked = ImGui::InvisibleButton(id, size);
    const bool activated = ImGui::IsItemActivated();                 // parmak butona DEGDIGI an (birakmayi beklemez)
    const bool pressed = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (activated && playSound) PlayGameClickSound();                // ses basista calar (orijinal butonlar gibi), birakista degil
    const ImU32 tint = pressed ? IM_COL32(190, 190, 190, 255) : IM_COL32(255, 255, 255, 255);   // basili: boyut kuculmesi + hafif karartma

    // Press animation affects the artwork, never the hitbox or layout.
    const float visualScale = pressed ? 0.96f : 1.0f;
    const ImVec2 visualSize(size.x * visualScale, size.y * visualScale);
    const ImVec2 visualPos(pos.x + (size.x - visualSize.x) * 0.5f,
                           pos.y + (size.y - visualSize.y) * 0.5f);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    if (g_MultiplayerButtonTexture != 0) {
        if (!rightAligned) {
            draw->AddImage(ToImGuiTexture(g_MultiplayerButtonTexture),
                           visualPos,
                           ImVec2(visualPos.x + visualSize.x, visualPos.y + visualSize.y),
                           ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
        } else {
            draw->AddImage(ToImGuiTexture(g_MultiplayerButtonTexture),
                           visualPos,
                           ImVec2(visualPos.x + visualSize.x, visualPos.y + visualSize.y),
                           ImVec2(1.0f, 0.0f), ImVec2(0.0f, 1.0f), tint);
        }
        // No rectangular hover overlay: the original button texture stays clean.
    } else {
        draw->AddRectFilled(visualPos,
                            ImVec2(visualPos.x + visualSize.x, visualPos.y + visualSize.y),
                            IM_COL32(20, 24, 28, 205), 2.0f);
    }

    ImFont* font = g_GameUIFont ? g_GameUIFont : ImGui::GetFont();
    const float textSizePx = std::max(31.0f, 36.0f * textScale *
                                             std::max(0.90f, std::min(1.12f, ImGui::GetIO().DisplaySize.y / 1080.0f)));
    const ImVec2 measured = font->CalcTextSizeA(textSizePx, FLT_MAX, 0.0f, label);
    const ImVec2 textPos(
        visualPos.x + (visualSize.x - measured.x) * 0.5f,
        visualPos.y + (visualSize.y - measured.y) * 0.5f - 1.0f
    );
    draw->AddText(font, textSizePx, textPos, pressed ? IM_COL32(205, 205, 205, 255) : IM_COL32(245, 245, 245, 255), label);
    return clicked;
}

static bool DrawCenteredMirroredGameButton(const char* id, const char* label,
                                           float totalW, float h, float textScale = 1.34f) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();

    ImGui::SetCursorScreenPos(start);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(totalW, h));
    const bool activated = ImGui::IsItemActivated();
    const bool pressed = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (activated) PlayGameClickSound();                             // ses basista calar
    const ImU32 tint = pressed ? IM_COL32(190, 190, 190, 255) : IM_COL32(255, 255, 255, 255);

    const float visualScale = pressed ? 0.96f : 1.0f;
    const ImVec2 visualSize(totalW * visualScale, h * visualScale);
    const ImVec2 visualStart(start.x + (totalW - visualSize.x) * 0.5f,
                             start.y + (h - visualSize.y) * 0.5f);

    if (g_MultiplayerButtonTexture != 0) {
        // Keep the existing four-part mirrored texture; only its visual bounds shrink.
        const float overlap = std::min(42.0f, visualSize.x * 0.055f);
        const float refW = (visualSize.x + overlap) * 0.5f;
        const float capU0 = 0.86f;
        const float midU  = 0.50f;
        const float capPx = floorf((1.0f - capU0) * refW + 0.5f);

        const float x0 = floorf(visualStart.x + 0.5f);
        const float x1 = x0 + floorf(visualSize.x + 0.5f);
        const float xc = floorf((x0 + x1) * 0.5f + 0.5f);
        const float y0 = visualStart.y, y1 = visualStart.y + visualSize.y;
        ImTextureID tex = ToImGuiTexture(g_MultiplayerButtonTexture);

        // Right half (flat middle + right cap).
        draw->AddImage(tex, ImVec2(xc, y0), ImVec2(x1 - capPx, y1),
                       ImVec2(midU, 0.0f), ImVec2(capU0, 1.0f), tint);
        draw->AddImage(tex, ImVec2(x1 - capPx, y0), ImVec2(x1, y1),
                       ImVec2(capU0, 0.0f), ImVec2(1.0f, 1.0f), tint);
        // Mirrored left half.
        draw->AddImage(tex, ImVec2(x0 + capPx, y0), ImVec2(xc, y1),
                       ImVec2(capU0, 0.0f), ImVec2(midU, 1.0f), tint);
        draw->AddImage(tex, ImVec2(x0, y0), ImVec2(x0 + capPx, y1),
                       ImVec2(1.0f, 0.0f), ImVec2(capU0, 1.0f), tint);
    }

    ImFont* font = g_GameUIFont ? g_GameUIFont : ImGui::GetFont();
    const float textSizePx = std::max(31.0f, 36.0f * textScale *
                                             std::max(0.90f, std::min(1.12f, ImGui::GetIO().DisplaySize.y / 1080.0f)));
    const ImVec2 measured = font->CalcTextSizeA(textSizePx, FLT_MAX, 0.0f, label);
    draw->AddText(font, textSizePx,
                  ImVec2(visualStart.x + (visualSize.x - measured.x) * 0.5f,
                         visualStart.y + (visualSize.y - measured.y) * 0.5f - 1.0f),
                  IM_COL32(245, 245, 245, 255), label);
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
// ========================================================================
// OYUNCU ISIM ETIKETLERI: diger oyuncularin bulundugu araclarin ustunde isimleri gosterilir
// (kendi icinde bulundugun arac haric). Kamera gorunum matrisi RenderArgs::update'ten (her karede) alinir,
// izdusum matrisi Game+0xA2B0 (oyun ici) / +0xA270'tedir; ikisi de GL sutun-sirali. Oyuncu hangi aracin icinde
// diye ActiveVehicle paketlerinden (g_RemoteActive) bakilir.
// ========================================================================
typedef void (*RenderArgsUpdate_t)(void* thiz, const float* view, const float* world);
static RenderArgsUpdate_t orig_RenderArgs_update = nullptr;
static float g_CamView[16];
static std::atomic<long long> g_CamMs(0);

static void my_RenderArgs_update(void* t, const float* view, const float* world) {
    if (view) {
        memcpy(g_CamView, view, sizeof(g_CamView));
        g_CamMs.store(NowMs());
    }
    if (orig_RenderArgs_update) orig_RenderArgs_update(t, view, world);
}

static void DrawPlayerNameTags(const ImVec2& screen) {
    if (!g_IsConnected.load() || !g_VehNetArmed.load() || g_EngineInstance == 0) return;
    const uintptr_t game = g_EngineInstance;
    if (*(volatile int*)(game + 0x64) != 6) return;
    if (NowMs() - g_CamMs.load() > 600) return;                       // kamera verisi taze degil (kanca yok / duraklama)

    const float* P = nullptr;
    static const uintptr_t kProjOffs[2] = { 0xA2B0, 0xA270 };
    for (int k = 0; k < 2 && !P; ++k) {
        const float* c = (const float*)(game + kProjOffs[k]);
        if (fabsf(c[0]) > 0.05f && fabsf(c[5]) > 0.1f && fabsf(fabsf(c[11]) - 1.0f) < 0.01f && fabsf(c[15]) < 0.01f) P = c;
    }
    if (!P) {
        static bool s_logged = false;
        if (!s_logged) { s_logged = true; LOGI("[NAMETAG] gecerli izdusum matrisi bulunamadi (0xA2B0/0xA270)"); }
        return;
    }
    bool flipped = false;
    {
        const uintptr_t dev = *(uintptr_t*)(game + 0x78);
        if (dev != 0) flipped = *(uint8_t*)(dev + 0x10) != 0;         // cihaz izdusumu pi kadar cevirir
    }
    const float* V = g_CamView;

    int nids[MAX_PLAYERS];
    std::string names[MAX_PLAYERS];
    int cnt = 0;
    const uint8_t me = g_LocalPlayerId.load();
    const long long now = NowMs();
    int snapNid[MAX_PLAYERS];
    long long snapMs[MAX_PLAYERS];
    {
        std::lock_guard<std::mutex> lock(g_ActiveMutex);
        for (uint8_t p = 0; p < MAX_PLAYERS; ++p) { snapNid[p] = g_RemoteActive[p].netId; snapMs[p] = g_RemoteActive[p].ms; }
    }
    for (uint8_t p = 0; p < MAX_PLAYERS; ++p) {
        if (p == me) continue;
        if (snapNid[p] < 0 || (now - snapMs[p]) >= 6000) continue;      // kendi aracindaki diger oyuncularin adi da gosterilir
        std::string nm = GetPlayerName(p);
        if (nm.empty()) continue;
        int slot = -1;
        for (int k = 0; k < cnt; ++k) if (nids[k] == snapNid[p]) { slot = k; break; }
        if (slot < 0) { slot = cnt++; nids[slot] = snapNid[p]; names[slot].clear(); }
        if (!names[slot].empty()) names[slot] += ", ";
        names[slot] += nm;
    }
    if (cnt == 0) return;

    ImDrawList* d = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    static bool s_firstLog = false;
    for (int k = 0; k < cnt; ++k) {
        const uintptr_t v = GetVehicleFromIndex(game, (uint16_t)nids[k]);
        if (v == 0) continue;
        const float wx = *(float*)(v + 0x18 + 0x30);
        const float wy = *(float*)(v + 0x18 + 0x34) + 4.2f;           // arac govdesinin ustu
        const float wz = *(float*)(v + 0x18 + 0x38);
        if (!(wx == wx) || !(wy == wy) || !(wz == wz)) continue;
        const float vx = V[0] * wx + V[4] * wy + V[8] * wz + V[12];
        const float vy = V[1] * wx + V[5] * wy + V[9] * wz + V[13];
        const float vz = V[2] * wx + V[6] * wy + V[10] * wz + V[14];
        const float cx = P[0] * vx + P[4] * vy + P[8] * vz + P[12];
        const float cy = P[1] * vx + P[5] * vy + P[9] * vz + P[13];
        const float cw = P[3] * vx + P[7] * vy + P[11] * vz + P[15];
        if (cw < 1.0f || cw > 300.0f) continue;                       // arkada ya da cok uzak
        float nx = cx / cw, ny = cy / cw;
        if (flipped) { nx = -nx; ny = -ny; }
        if (nx < -1.15f || nx > 1.15f || ny < -1.15f || ny > 1.15f) continue;
        const float sx = (nx * 0.5f + 0.5f) * screen.x;
        const float sy = (1.0f - (ny * 0.5f + 0.5f)) * screen.y;
        if (!s_firstLog) {
            s_firstLog = true;
            LOGI("[NAMETAG] ilk etiket: arac=%d mesafe=%.1f ekran=(%.0f,%.0f) P0=%.3f P5=%.3f P11=%.1f flip=%d", nids[k], cw, sx, sy,
                 P[0], P[5], P[11], (int)flipped);
        }
        float sc = 55.0f / cw;
        if (sc < 0.60f) sc = 0.60f;
        if (sc > 1.15f) sc = 1.15f;
        const float fs = screen.y * 0.030f * sc;
        const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, names[k].c_str());
        const float padX = fs * 0.55f, padY = fs * 0.28f;
        const ImVec2 a(sx - ts.x * 0.5f - padX, sy - ts.y - padY * 2.0f - fs * 0.35f);
        const ImVec2 b(sx + ts.x * 0.5f + padX, sy - fs * 0.35f);
        d->AddRectFilled(a, b, IM_COL32(0, 0, 0, 150), fs * 0.40f);
        d->AddTriangleFilled(ImVec2(sx - fs * 0.30f, b.y - 1.0f), ImVec2(sx + fs * 0.30f, b.y - 1.0f),
                             ImVec2(sx, b.y + fs * 0.32f), IM_COL32(0, 0, 0, 150));
        d->AddText(font, fs, ImVec2(a.x + padX, a.y + padY), IM_COL32(255, 255, 255, 240), names[k].c_str());
    }
}

static void DrawTypingBar(const ImVec2& screen) {
    static bool s_barFocused = false;
    static bool s_btnHeld = false;       // parmak TAMAM'in uzerinde: bu sirada yazi kutusu odagi geri istenmez (tiklamayi iptal ederdi)

    const int mode = g_KeyboardFieldMode.load();
    if (!g_AndroidKeyboardOpen.load() || (mode != 1 && mode != 2) || g_GameUIFont == nullptr) {
        s_barFocused = false;
        s_btnHeld = false;
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
    if (!s_barFocused && !s_btnHeld) ImGui::SetKeyboardFocusHere();
    const bool enterPressed = ImGui::InputTextWithHint("##TypingBarInput", hint, buf, bufSize,
                                                       ImGuiInputTextFlags_EnterReturnsTrue);
    s_barFocused = ImGui::IsItemActive();

    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y + 2.0f), ImVec2(mx.x, mx.y + 2.0f),
                                        IM_COL32(0, 128, 128, 255), 2.0f);

    // TAMAM also activates on release, not at the beginning of a touch.
    const ImVec2 btnLocal(screen.x - btnW - 16.0f, (barH - fh * 1.3f) * 0.5f);
    ImGui::SetCursorPos(btnLocal);
    const ImVec2 btnSize(btnW, fh * 1.3f);
    const ImVec2 btnPos = ImGui::GetCursorScreenPos();
    const bool btnClicked = ImGui::InvisibleButton("##TypingBarDone", btnSize);
    const bool btnActivated = ImGui::IsItemActivated();
    const bool btnPressed = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    s_btnHeld = btnPressed;
    const float btnScale = btnPressed ? 0.96f : 1.0f;
    const ImVec2 btnVisualSize(btnSize.x * btnScale, btnSize.y * btnScale);
    const ImVec2 btnVisualPos(btnPos.x + (btnSize.x - btnVisualSize.x) * 0.5f,
                              btnPos.y + (btnSize.y - btnVisualSize.y) * 0.5f);
    ImDrawList* btnDraw = ImGui::GetWindowDrawList();
    btnDraw->AddRectFilled(btnVisualPos,
                           ImVec2(btnVisualPos.x + btnVisualSize.x, btnVisualPos.y + btnVisualSize.y),
                           btnPressed ? IM_COL32(186, 186, 186, 255) : IM_COL32(245, 245, 245, 255), 4.0f);
    btnDraw->AddRect(btnVisualPos,
                     ImVec2(btnVisualPos.x + btnVisualSize.x, btnVisualPos.y + btnVisualSize.y),
                     IM_COL32(115, 125, 130, 255), 4.0f, 0, 1.0f);
    const char* doneLabel = "TAMAM";
    const ImVec2 doneTextSize = ImGui::CalcTextSize(doneLabel);
    btnDraw->AddText(ImVec2(btnVisualPos.x + (btnVisualSize.x - doneTextSize.x) * 0.5f,
                            btnVisualPos.y + (btnVisualSize.y - doneTextSize.y) * 0.5f),
                     IM_COL32(24, 30, 32, 255), doneLabel);

    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(8);

    if (btnActivated) PlayGameClickSound();                          // ses basista calar
    if (enterPressed || btnClicked) {
        if (mode == 2) SubmitChatInput();
        s_barFocused = false;
        s_btnHeld = false;
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
    static bool s_HudChatPressArmed = false;
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
        const bool insideNow = dx * dx + dy * dy <= r * r * 1.44f;
        const bool pressed = g_TouchDown.load() && insideNow;
        const float visualScale = pressed ? 0.95f : 1.0f;
        const float vr = r * visualScale;
        ImDrawList* d = ImGui::GetForegroundDrawList();
        const ImVec2 c(cx, cy);
        d->AddCircleFilled(c, vr, pressed ? IM_COL32(8, 8, 8, 245) : IM_COL32(18, 18, 18, 235), 48);
        if (pressed) d->AddCircleFilled(c, vr, IM_COL32(0, 0, 0, 70), 48);   // basili: hafif karartma
        d->AddCircle(c, vr - vr * 0.04f, IM_COL32(120, 120, 120, 255), 48, vr * 0.07f);
        // Chat bubble scales with the pressed button, while its hitbox remains unchanged.
        const ImU32 white = IM_COL32(245, 245, 245, 255), dark = IM_COL32(18, 18, 18, 255);
        d->AddRectFilled(ImVec2(cx - vr * 0.50f, cy - vr * 0.38f), ImVec2(cx + vr * 0.50f, cy + vr * 0.20f),
                         white, vr * 0.20f);
        d->AddTriangleFilled(ImVec2(cx - vr * 0.28f, cy + vr * 0.15f), ImVec2(cx - vr * 0.34f, cy + vr * 0.46f),
                             ImVec2(cx - vr * 0.02f, cy + vr * 0.15f), white);
        for (int i = -1; i <= 1; ++i)
            d->AddCircleFilled(ImVec2(cx + i * vr * 0.24f, cy - vr * 0.09f), vr * 0.07f, dark, 12);

        // A raw DOWN only arms the button. The menu opens after UP, provided release
        // is still inside the hitbox; this prevents instant activation on touch-down.
        if (g_PendingTouchDown.load()) {
            const float px = g_PendingTouchX.load() - cx, py = g_PendingTouchY.load() - cy;
            g_PendingTouchDown.store(false);
            s_HudChatPressArmed = (px * px + py * py <= r * r * 1.44f);
            if (s_HudChatPressArmed) PlayGameClickSound();           // ses basista calar
        }
        if (s_HudChatPressArmed && !g_TouchDown.load()) {
            s_HudChatPressArmed = false;
            const float rx = g_TouchX.load() - cx, ry = g_TouchY.load() - cy;
            if (rx * rx + ry * ry <= r * r * 1.44f) {
                g_MpFromLan = false;
                g_MpView = MPV_ROOM;
                g_IsMultiplayerMenuActive = true;
            }
        }
    } else {
        // Do not leave a stale press armed when the HUD button is not visible.
        s_HudChatPressArmed = false;
    }

    // Diger oyuncularin araclari uzerinde isim etiketleri (kendi aracin haric)
    if (g_OnlineMode && !g_IsMultiplayerMenuActive) DrawPlayerNameTags(screen);
    TickRender();                                                              // TANI: cizim thread'i canli mi (watchdog)
    if (!g_IsMultiplayerMenuActive) WK::DrawImGui(g_EngineInstance, screen);   // WALKER: Exit/Enter Vehicle dugmeleri (+ varlik yukleme: GL thread)
    LL::Draw(screen);                                                          // CANLI LOG paneli (libmultiplayermod + libapp)

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
                        const bool roomClicked = DrawGameStyleButton(
                            roomId,
                            roomLabel.c_str(),
                            ImVec2(roomW, roomH),
                            1.06f,
                            false
                        );

                        if (roomClicked) {
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
// ========================================================================
// ORTAK PARA: "HAYALET" VARLIKLARIN PARA DEGISIMINI GERI AL
// Oyun, parayi varliklarin kendi update fonksiyonlarinda degistirir (urun satisi: Trailer::update
// icinden TipSite::tip; yakit/is maliyeti: Vehicle::update, Tool::update). Baska bir oyuncunun
// surdugu arac her cihazda "hayalet" olarak calisir, yani ayni satis HER cihazda gerceklesir ve
// para cihaz sayisi kadar katlanirdi.
// Cozum: sahibi BASKA oyuncu olan bir aracin (veya ona bagli romork/aletin) update'i para
// degistirirse, o degisim hemen geri alinir. Sadece araci gercekten suren cihazin degisimi
// kalir ve MoneyFrame ile diger cihazlara delta olarak gider. Boylece her olay 1 kez sayilir.
// Kanca takilamazsa (prologue riskli) bu ozellik sessizce kapanir, loga yazilir.
// ========================================================================
static const bool kGuardGhostMoney = true;
typedef void* (*EntityUpdate_t)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static EntityUpdate_t orig_Vehicle_update = nullptr;
static EntityUpdate_t orig_Trailer_update = nullptr;
static std::atomic<int> g_MoneyGuardLogs(0);

static bool IsGhostVehiclePtr(uintptr_t veh) {
    if (veh == 0) return false;
    const int id = NetIdOfVehicle(veh);
    if (id < 0) return false;
    const uint8_t me = g_LocalPlayerId.load();
    uint8_t owner = VEHICLE_OWNER_NONE;
    GetVehicleOwner((uint16_t)id, &owner);
    // Isci (AI) calisiyorsa: gercek simulasyon yalnizca iscyi baslatan cihazda; diger cihazlardaki aynali kopya hayalettir.
    if (id < (int)VEHICLE_SLOT_LIMIT && *(uint32_t*)(veh + 0x730) != 0) return g_AiMirrored[id];
    if (id == g_LocalActiveNetId) {
        // Yerel oyuncu bu aracin icinde. Baska bir oyuncu da AYNI aractaysa sadece biri saymali:
        //  - sahip (suren) baskasiysa o sayar, sahip bensem ben sayarim,
        //  - sahip yoksa (arac duruyor) en kucuk oyuncu numarasi sayar.
        if (!IsRemoteActiveVehicle(id)) return false;
        if (owner != VEHICLE_OWNER_NONE) return owner != me;
        return HasLowerIdRemoteActive(id, me);
    }
    if (owner != VEHICLE_OWNER_NONE && owner != me) return true;   // baskasi suruyor
    if (IsRemoteActiveVehicle(id)) return true;                    // baskasi icinde (arac duruyor olabilir)
    // Sahipsiz ve kimsenin icinde olmayan arac (ornegin bosaltma icin duran bicer): HER cihaz kendini gercek sanirsa tank/romork
    // simulasyonu iki cihazda ayri calisir, romork mutlak degeri birbirini ezer ve mahsul KATLANIR. Tek gercek kopya host'ta.
    if (owner == me) return false;
    return !g_IsHost.load();
}

static void* GuardedEntityUpdate(EntityUpdate_t orig, uintptr_t ownerVeh, const char* name, void* thiz,
                                 uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    if (!orig) return nullptr;
    const uintptr_t g = g_EngineInstance;
    if (g == 0 || !kSharedMoney || !g_IsConnected.load() || !g_VehNetArmed.load())
        return orig(thiz, a, b, c, d, e);
    const bool ghost = IsGhostVehiclePtr(ownerVeh);

    // Dunya durumu (silo stogu, biyogaz, satis fiyatlari): en dis update'te olc. Hayalette geri al, gercekte delta olarak biriktir.
    static int s_worldDepth = 0;
    const bool measureWorld = kSyncWorld && s_worldDepth == 0;
    float wBefore[kWorldN];
    int wN = 0;
    if (measureWorld) wN = WorldRead(g, wBefore);
    ++s_worldDepth;

    const double before = ghost ? ReadMoney(g) : 0.0;
    void* r = orig(thiz, a, b, c, d, e);
    --s_worldDepth;
    if (ghost) {
        const double after = ReadMoney(g);
        if (after != before) {
            WriteMoney(g, before);
            if (g_MoneyGuardLogs.fetch_add(1) < 30)
                LOGI("[MONEY] hayalet %s para degisimi geri alindi: %+.2f", name, after - before);
        }
    }
    if (measureWorld && wN > 0) {
        float wAfter[kWorldN];
        const int n2 = WorldRead(g, wAfter);
        if (n2 == wN) {
            for (int i = 0; i < wN; ++i) {
                if (wAfter[i] == wBefore[i]) continue;
                const float dv = wAfter[i] - wBefore[i];
                if (!(dv == dv) || fabsf(dv) > 1.0e7f) continue;
                if (ghost) {
                    volatile float* p = WorldPtr(g, i);
                    if (p) *p = wBefore[i];
                } else {
                    g_WorldAcc[i] += dv;
                }
            }
        }
    }
    return r;
}

static void* my_Vehicle_update(void* t, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    // Uzaktan surulen aracin direksiyon durumunu (Vehicle+0x568) oyunun kendi tekerlek kodu kullansin diye yaz.
    if (kSyncSteering && t && g_IsConnected.load() && g_VehNetArmed.load()) {
        const int nid = NetIdOfVehicle((uintptr_t)t);
        if (nid >= 0) {
            uint8_t owner = VEHICLE_OWNER_NONE;
            uint8_t q = 0;
            bool fresh = false;
            {
                std::lock_guard<std::mutex> lock(g_VehicleStateMutex);
                owner = g_VehicleAuthority[nid].ownerId;
                const VehicleRemoteState& rs = g_RemoteVehicles[nid];
                q = rs.steerQ;
                fresh = rs.valid && (GetMonotonicMilliseconds() - rs.lastReceiveMs) < 400;
            }
            if (fresh && q != 0 && owner != VEHICLE_OWNER_NONE && owner != g_LocalPlayerId.load())
                *(float*)((uintptr_t)t + 0x568) = DequantizeSteer(q);
        }
    }
    StageG("orig_Vehicle_update");
    void* wkRet = GuardedEntityUpdate(orig_Vehicle_update, (uintptr_t)t, "Vehicle", t, a, b, c, d, e);
    WK::StripWalkerVehicleHud((uintptr_t)t);               // WALKER: sahte aracın yakıt HUD değerlerini temizle
    WK::AfterVehicleUpdate((uintptr_t)t, a, b, c, d);     // WALKER: yerel yayanin hareketi + birinci sahis kamera (update'ten SONRA)
    return wkRet;
}
static void* my_Trailer_update(void* t, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    const uintptr_t veh = t ? *(uintptr_t*)((uintptr_t)t + 0x184) : 0;     // Trailer+0x184 = bagli oldugu arac
    return GuardedEntityUpdate(orig_Trailer_update, veh, "Trailer", t, a, b, c, d, e);
}

// ---- BICER BORU BOSALTMA: hayalet bicer icerik URETMESIN ----
// Bicerin tanki sahibi cihazda gercek, diger cihazlarda "hayalet"tir (tank mutlak deger olarak her ~250 ms'de
// sahibinden gelir). Hayalet bicerin borusu da yerel romorku doldurup yerel tanki bosaltiyordu; sonraki paket tanki
// tekrar DOLU hale getirdigi icin ayni mahsul tekrar tekrar romorka aktiyordu (offline %66 dolan tarla multiplayerda
// %100'u asiyordu). Artik hayalet bicerin boru bosaltmasinin tank/romork etkisi geri alinir (boru animasyonu kalir);
// icerik sadece borunun GERCEK sahibi cihazda romorka akar, romork doluluğu zaten mutlak olarak herkese yayilir.
typedef void (*Vehicle_updatePipeState_t)(void* thiz, uint32_t dtBits, void* game, void* outTrailer);
static Vehicle_updatePipeState_t orig_Vehicle_updatePipeState = nullptr;
static std::atomic<int> g_PipeGuardLogs(0);
static std::atomic<int> g_FillLogs(0);

static void my_Vehicle_updatePipeState(void* t, uint32_t dtBits, void* gm, void* outTr) {
    if (!orig_Vehicle_updatePipeState) return;
    const uintptr_t g = g_EngineInstance;
    if (t == nullptr || g == 0 || !g_IsConnected.load() || !g_VehNetArmed.load() || !IsGhostVehiclePtr((uintptr_t)t)) {
        if (t != nullptr && g != 0 && g_IsConnected.load() && g_FillLogs.load() < 40) {
            // TANI: GERCEK borunun tank/romork degisimi (bosaltma korunumu: tank -X, romork +X olmali). Yalnizca baglantiliyken, sinirli log.
            const uintptr_t v0 = (uintptr_t)t;
            const float tankB = WordToFloat(*(uint32_t*)(v0 + 0x440));
            float sumB = 0.0f;
            const uint32_t tn0 = TrailerCount(g);
            for (uint32_t i = 0; i < tn0; ++i) { const uintptr_t tp = RawTrailerSlot(g, i); if (tp) sumB += WordToFloat(*(uint32_t*)(tp + 0x388)); }
            orig_Vehicle_updatePipeState(t, dtBits, gm, outTr);
            const float tankA = WordToFloat(*(uint32_t*)(v0 + 0x440));
            float sumA = 0.0f;
            for (uint32_t i = 0; i < tn0; ++i) { const uintptr_t tp = RawTrailerSlot(g, i); if (tp) sumA += WordToFloat(*(uint32_t*)(tp + 0x388)); }
            static float s_accTank = 0.0f, s_accTr = 0.0f; static long long s_lastLog = 0;
            if (tankA == tankA && tankB == tankB && sumA == sumA && sumB == sumB) { s_accTank += tankB - tankA; s_accTr += sumA - sumB; }
            const long long nowL = NowMs();
            if ((s_accTank > 1.0f || s_accTr > 1.0f) && nowL - s_lastLog >= 1000) {
                s_lastLog = nowL;
                LOGI("[FILL] gercek boru: tank dustu=%.1f romork artti=%.1f (rol=%s)", s_accTank, s_accTr, g_IsHost.load() ? "host" : "client");
                s_accTank = s_accTr = 0.0f;
                g_FillLogs.fetch_add(1);
            }
            return;
        }
        orig_Vehicle_updatePipeState(t, dtBits, gm, outTr);
        return;
    }
    const uintptr_t v = (uintptr_t)t;
    const uint32_t tankBefore = *(uint32_t*)(v + 0x440);
    struct PipeSnap { uintptr_t tp; uint32_t lvl, typ; };
    PipeSnap snaps[TRAILER_MAX];
    uint32_t ns = 0;
    const uint32_t tn = TrailerCount(g);
    for (uint32_t i = 0; i < tn && ns < TRAILER_MAX; ++i) {
        const uintptr_t tp = RawTrailerSlot(g, i);
        if (tp == 0) continue;
        snaps[ns].tp = tp;
        snaps[ns].lvl = *(uint32_t*)(tp + 0x388);
        snaps[ns].typ = *(uint32_t*)(tp + 0x390);
        ++ns;
    }
    orig_Vehicle_updatePipeState(t, dtBits, gm, outTr);
    bool undone = false;
    if (*(uint32_t*)(v + 0x440) != tankBefore) { *(uint32_t*)(v + 0x440) = tankBefore; undone = true; }
    for (uint32_t i = 0; i < ns; ++i) {
        const uintptr_t tp = snaps[i].tp;
        if (*(uint32_t*)(tp + 0x388) != snaps[i].lvl || *(uint32_t*)(tp + 0x390) != snaps[i].typ) {
            *(uint32_t*)(tp + 0x388) = snaps[i].lvl;
            *(uint32_t*)(tp + 0x390) = snaps[i].typ;
            *(uint32_t*)(tp + 0x3A4) |= 4;                    // Trailer::setFillLevel ile ayni: gorunum yenilensin
            undone = true;
        }
    }
    if (undone && g_PipeGuardLogs.fetch_add(1) < 6)
        LOGI("[PIPE] hayalet bicer bosaltmasi geri alindi (cift dolum engellendi)");
}

// ---- Tarla islemi kancasi: Map::updateFields(8 kose float, FieldWork&) ----
// Hasat / pulluk / mibzer gibi tum tarla degisiklikleri bu fonksiyondan gecer. Bu cihazin surdugu (ya da
// isciye verdigi) aracin islemi diger cihazlara gonderilir; onlar ayni cagriyi kendi tarlalarinda yapar.
// FieldWork bir arac (+0x3d4) ya da aletin (+0x434) icinde oldugundan kaynak adresten bulunur.
struct FieldSrcCache { uintptr_t fw; uint32_t p[8]; uint32_t wtype, wfruit; long long ms; };
static FieldSrcCache g_FieldSrc[16];
static int g_FieldSrcNext = 0;

static int FieldSourceKind(uintptr_t game, uintptr_t fw, uintptr_t* vehOut) {
    const uint32_t vn = VehicleCount(game);
    for (uint32_t i = 0; i < vn; ++i) {
        const uintptr_t v = RawVehicleSlot(game, i);
        if (v != 0 && fw == v + 0x3D4) { *vehOut = v; return 1; }
    }
    const uint32_t tn = ToolCount(game);
    for (uint32_t i = 0; i < tn; ++i) {
        const uintptr_t t = RawToolSlot(game, i);
        if (t != 0 && fw == t + 0x434) { *vehOut = *(uintptr_t*)(t + 0x23C); return 2; }
    }
    return 0;
}

static void my_Map_updateFields(void* map, uint32_t f1, uint32_t f2, uint32_t f3, uint32_t s0, uint32_t s1,
                                uint32_t s2, uint32_t s3, uint32_t s4, void* fw) {
    const bool capture = kSyncFieldWork && fw != nullptr && !g_ReplayingFieldOp && g_IsConnected.load() &&
                         g_VehNetArmed.load() && g_EngineInstance != 0;
    uint32_t wt = 0, wf = 0;
    if (capture) { wt = ((uint32_t*)fw)[0]; wf = ((uint32_t*)fw)[1]; }
    // NOT: hayalet bicerin yerel hasadi ATLANMAZ (shopfix-28'de atlanmisti, clientte tarladaki mahsul belli bir yerden
    // sonra gitmiyordu): hayalet de kendi kopyasinda tarlayi biçer, gelen tarla islemleri ek guvence olur. Tank sahibinden
    // mutlak gelir ve hayalet boru bosaltmasi geri alindigi icin ek mahsul romorka gecmez.
    // Tarla isi HER cihazda yerel olarak calisir: uzaktan surulen (hayalet) arac ve aynalanan isci de bu cihazda
    // tarlayi isler (herkes ayni isi kendi kopyasinda yapar). Sadece YAYIN sofor/isciyi baslatan cihazdan gider;
    // hayalet ve aynali aracin islemi yollanmaz (yanki olmasin).
    if (orig_Map_updateFields) orig_Map_updateFields(map, f1, f2, f3, s0, s1, s2, s3, s4, fw);
    if (!capture || wt >= 16) return;
    uintptr_t veh = 0;
    if (FieldSourceKind(g_EngineInstance, (uintptr_t)fw, &veh) == 0 || veh == 0) return;
    const int nid = NetIdOfVehicle(veh);
    if (nid < 0 || g_AiMirrored[nid] || IsGhostVehiclePtr(veh)) return;
    const uint32_t pts[8] = { f1, f2, f3, s0, s1, s2, s3, s4 };
    const long long now = NowMs();
    FieldSrcCache* c = nullptr;
    for (int i = 0; i < 16; ++i) if (g_FieldSrc[i].fw == (uintptr_t)fw) { c = &g_FieldSrc[i]; break; }
    if (!c) { c = &g_FieldSrc[g_FieldSrcNext]; g_FieldSrcNext = (g_FieldSrcNext + 1) & 15; c->fw = (uintptr_t)fw; c->ms = 0; }
    if (c->ms != 0 && now - c->ms < 1500 && c->wtype == wt && c->wfruit == wf && memcmp(c->p, pts, sizeof(pts)) == 0) return;   // ayni dikdortgen: tekrar yollama
    memcpy(c->p, pts, sizeof(pts)); c->wtype = wt; c->wfruit = wf; c->ms = now;
    QueueFieldOp(wt, wf, pts);
}

void my_GameUpdate(void* thiz, float param_1) {
    // v4: Game::update'in param_1'i SANIYE cinsinden kare suresidir (this+0x58 ona toplanip 120/300 s ile karsilastiriliyor).
    // Oyun bunu sinirlamiyor: tek bir uzun takilma, sonraki karelerin dt'sini de uzatip KALICI ~13 sn'lik karelere donusuyordu.
    if (WK_MAX_DT_S > 0.0f && param_1 > WK_MAX_DT_S) {
        static long long s_dl = 0; const long long nd = NowMs();
        if (nd - s_dl > 2000) { s_dl = nd; LOGI("[WALKER] dt kelepcelendi: %.3f s -> %.3f s", (double)param_1, (double)WK_MAX_DT_S); }
        param_1 = WK_MAX_DT_S;
    }
    if (g_HUDInstance != 0) RepairShopClick((void*)g_HUDInstance, "gameUpdate");   // tiklama bu karede islenmeden once
    g_LastGameUpdateMs.store(NowMs());   // host canlilik sinyali icin (oyun dongusu donarsa ping kesilir)
    g_EngineInstance = (uintptr_t)thiz;
    GuestBlockDialogs((uintptr_t)thiz);   // client: satis / tarla satin alma onay pencerelerini kapat
    g_CurrentMenu = MENU_INGAME;
    MoneyFrame((uintptr_t)thiz);         // ortak para (her kare, tum oyun durumlarinda)
    WK::OnGameFrame((uintptr_t)thiz);    // WALKER: in/bin istekleri (oyun fonksiyonlari sadece bu thread'den cagrilir)
    {
        // Game::updateStateBase SADECE durum 6'da calisir; magaza/ayarlar (durum 7) acikken hic cagrilmaz.
        // Alim/satim tespiti ve arac tablosu bu yuzden burada (her karede) calistirilir; boylece magaza
        // KAPANMADAN diger cihazlara bildirilir.
        const uint32_t st0 = *(volatile uint32_t*)((uintptr_t)thiz + 0x64);
        if (st0 == 6 || st0 == 7) PoolPollFrame((uintptr_t)thiz);
        if (st0 == 7) RefreshVehicleTopology((uintptr_t)thiz);
        TimeSyncFrame((uintptr_t)thiz, st0);
        MapClockFrame((uintptr_t)thiz, st0);
        FieldOwnFrame((uintptr_t)thiz, st0);
    }

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
    StageG("orig_GameUpdate");
    if (orig_GameUpdate) orig_GameUpdate(thiz, param_1);
    StageG("GameUpdate-bitti");
}

void my_GameUpdateStateBase(void* thiz, float param_1, uint32_t param_2, uint32_t param_3, uint32_t param_4) {
    g_EngineInstance = (uintptr_t)thiz;
    {
        const long long nowSb = NowMs();
        const long long prevSb = g_LastStateBaseMs.load();
        if (prevSb > 0 && nowSb - prevSb > 2500 && *(volatile uint32_t*)(thiz ? (uintptr_t)thiz + 0x64 : 0) == 6) OnGameResumed(nowSb - prevSb);
    }
    g_LastStateBaseMs.store(NowMs());   // dunya mantigi calisiyor (host duraklama algisi icin)

    // Kullanici isci (AI) dugmesine bastiysa giris cihazinda (device+0x28) bit 0x1000 yanar; oyunun kendi kodu
    // bunu bu fonksiyonun icinde temizler, bu yuzden orijinalden ONCE okunur.
    if (*(volatile uint32_t*)(g_EngineInstance + 0x64) == 6) {
        const uintptr_t dev = *(volatile uintptr_t*)(g_EngineInstance + 0x7c);
        if (dev != 0 && (*(volatile uint32_t*)(dev + 0x28) & 0x1000)) {
            const uint32_t ai = *(volatile uint32_t*)(g_EngineInstance + 0xA8);
            g_AiIntentVeh = (ai < VehicleCount(g_EngineInstance)) ? NetIdOfVehicle(RawVehicleSlot(g_EngineInstance, ai)) : -1;
            g_AiIntentMs = NowMs();
        }
    }

    // Balya satisi geliri: oyunun kendi guncellemesi (orijinal cagri) sirasinda kazanilan para (BaleCapture satis tespiti).
    const bool inGame6 = *(volatile uint32_t*)(g_EngineInstance + 0x64) == 6;
    const double moneyBefore = inGame6 ? ReadMoney(g_EngineInstance) : 0.0;
    if (inGame6) MiscSyncPre(g_EngineInstance);        // client: host'un fiyat listesi + gorevi (oyun kendi rastgelesini uretmesin)

    if (orig_GameUpdateStateBase) {
        StageG("orig_GameUpdateStateBase");
        orig_GameUpdateStateBase(thiz, param_1, param_2, param_3, param_4);
        StageG("StateBase-bitti");
    }
    g_BaleFrameGain = inGame6 ? (ReadMoney(g_EngineInstance) - moneyBefore) : 0.0;

    // The game itself decides the real frame/vsync cadence. The C output shows
    // Game::update() ends with waitVSync(), so this hook must not try to force
    // the engine to 120 FPS. We sample at up to 120 Hz when callbacks allow it,
    // and the network is independently capped at 30 snapshots/sec.
    if (*(volatile uint32_t*)(g_EngineInstance + 0x64) == 6) {
        FieldSnapFrame(g_EngineInstance);                  // duraklama devri / geri donus icin tarla goruntusu (surekli ag trafigi yok)
        ResumeSyncFrame(g_EngineInstance);                 // geri donus: duran araclarin son konumlari
        MiscSyncPost(g_EngineInstance);                    // host: fiyat listesi + gorev yayini | client: gorev tamamlama bildirimi
        ApplyRemoteVehicleStates(g_EngineInstance);
        TrafficFrame(g_EngineInstance);                    // NPC trafik: kendi araclarini yayinla, gelenleri ayna olarak surdur
        WorldSyncFrame(g_EngineInstance);                  // silo stogu / biyogaz / satis fiyatlari: delta + host'un periyodik tablosu
        BaleFrame(g_EngineInstance);                    // CaptureAndQueueLocalVehicleState'ten ONCE (g_EntityResendAll orada silinir)
        CaptureAndQueueLocalVehicleState(g_EngineInstance);
        ActiveVehicleFrame(g_EngineInstance);
        HornFrame(g_EngineInstance, GetMonotonicMilliseconds());
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

// Oyunun kendi koduna gore (GenericGUIManager::updateGuiButtons, case 0x20/0x21/0x22) magaza
// alim/satim olaylarinda (0x19..0x1d) urun numarasi GUI yoneticisinin +0x583c alanindan
// Game+0x9c24'e yazilir. Logda Game+0x9c24'un bu olaylarda hep 0x42800000 (float 64.0)
// oldugu goruldu (+0x583c ise saglam) ve buyItem bu numarayla cokuyordu.
// Cozum: numaray oyunun kendi kaynagindan (+0x583c) geri yaz. Gecerli aralik 0..0x11.
// ---- Client alisveris engeli ----
// Oyun host'un; client arac/makine/alet/romork/tarla alip satamaz (host'un oyununu bozmasin).
// Magaza olaylari (Game+0x9c20): 0x19 = satin al, 0x1a = sat (onay penceresi acar), 0x1c = satin al (IAP yolu).
// Yeni kanca YOK: olay kodu oyunun kendi isleyicisi okumadan once 0 (olay yok) yapilir.
static bool IsGuestInSession() {
    return g_ClientPlaying.load() || (g_IsClient.load() && g_IsConnected.load());
}
static long long g_LastShopBlockToastMs = 0;
static void GuestShopBlockToast() {
    const long long now = NowMs();
    if (now - g_LastShopBlockToastMs < 2000) return;
    g_LastShopBlockToastMs = now;
    ShowNativeToast("Only the host can buy or sell vehicles, tools and fields.");
}
// Onay pencereleri: 9 = tarla satin alma onayi, 0x10 = arac/alet satis onayi. Oyunun kendi "iptal"
// koduyla ayni sey yapilir: Game+0x68 = 0 ve GUIInterfaceDesc::resetDialog (Game+0x8910) alanlari sifirlanir.
static void GuestBlockDialogs(uintptr_t g) {
    if (g == 0 || !IsGuestInSession()) return;
    volatile uint32_t* dlg = (volatile uint32_t*)(g + 0x68);
    const uint32_t d = *dlg;
    // Oyun, son kayittan 900 sn sonra "oyunu kaydet?" penceresini kendiliginden acar (Game+0xa3f8 sayaci > 900.0).
    // Client kaydedemez: sayaci surekli sifirla, pencere hic acilmasin.
    if (d == 0) *(volatile float*)(g + 0xa3f8) = 0.0f;
    if (d != 9 && d != 0x10) return;
    *dlg = 0;
    uint32_t* desc = (uint32_t*)(g + 0x8910);        // GUIInterfaceDesc::resetDialog() ile birebir ayni
    desc[2] = 0;            // +0x08
    desc[4] = 0xffffffffu;  // +0x10
    desc[7] = 0;            // +0x1c
    desc[5] = 0xffffffffu;  // +0x14
    desc[8] = 0;            // +0x20
    desc[6] = 0xffffffffu;  // +0x18
    desc[9] = 0;            // +0x24
    desc[10] = 0;           // +0x28
    LOGI("[SHOPBLOCK] client: onay penceresi (0x%x) kapatildi", (unsigned)d);
    GuestShopBlockToast();
}

static void RepairShopClick(void* gui, const char* where) {
    if (g_EngineInstance == 0 || gui == nullptr) return;
    const uintptr_t g = g_EngineInstance;
    if (*(volatile uint32_t*)(g + 0x64) != 7) return;        // sadece oyun ici menu
    {
        const uint32_t ev0 = *(volatile uint32_t*)(g + 0x9c20);
        if ((ev0 == 0x19 || ev0 == 0x1a || ev0 == 0x1c) && IsGuestInSession()) {
            *(volatile uint32_t*)(g + 0x9c20) = 0;           // oyun bu olayi hic gormez
            LOGI("[SHOPBLOCK] %s: client magaza olayi 0x%x engellendi", where, (unsigned)ev0);
            GuestShopBlockToast();
            return;
        }
    }
    volatile uint32_t* selp = (volatile uint32_t*)((uintptr_t)gui + 0x583c);
    if (*selp > 0x11) {
        LOGI("[SHOP] %s: GUI secili urun bozuk: 0x%08x -> 0", where, (unsigned)*selp);
        *selp = 0;
    }
    const uint32_t ev = *(volatile uint32_t*)(g + 0x9c20);
    if (ev < 0x19 || ev > 0x1d) return;
    const uint32_t sel = *selp;
    volatile uint32_t* idxp = (volatile uint32_t*)(g + 0x9c24);
    if (*idxp != sel) {
        LOGI("[SHOP] %s: olay=0x%x urun numarasi 0x%08x -> %u (GUI secimi)", where, (unsigned)ev, (unsigned)*idxp, (unsigned)sel);
        *idxp = sel;
    }
}
static void GuardShopSelection(void* gui) { RepairShopClick(gui, "updateGUI"); }

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
    LOGI(">>> MULTIPLAYER MOD STARTING <<< build=shopfix-39+walker-v7");

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
        g_VehicleAttachTrailerFn = (Vehicle_attachTrailer_t)dlsym(appLib, "_ZN7Vehicle13attachTrailerEP7TrailerP7b2World");
        if (!g_VehicleAttachTrailerFn) g_VehicleAttachTrailerFn = (Vehicle_attachTrailer_t)FindElfSymbolByPrefix("libapp.so", "_ZN7Vehicle13attachTrailer");
        g_VehicleAttachToolFn = (Vehicle_attachTrailer_t)dlsym(appLib, "_ZN7Vehicle10attachToolEP4ToolP7b2World");
        if (!g_VehicleAttachToolFn) g_VehicleAttachToolFn = (Vehicle_attachTrailer_t)FindElfSymbolByPrefix("libapp.so", "_ZN7Vehicle10attachTool");
        LOGI("[TOOL] symbols: attachTool=%p detachTool=%p", (void*)g_VehicleAttachToolFn, (void*)g_VehicleDetachToolFn);
        g_CombineToggleFn = (Entity_toggle_t)dlsym(appLib, "_ZN7Vehicle23toggleCombineIsTurnedOnEv");
        if (!g_CombineToggleFn) g_CombineToggleFn = (Entity_toggle_t)FindElfSymbolByPrefix("libapp.so", "_ZN7Vehicle23toggleCombineIsTurnedOn");
        g_AudioStopFn = (AudioStop_t)dlsym(appLib, "_ZN11AudioSource4stopEv");
        g_AudioSetVolFn = (AudioSetVol_t)dlsym(appLib, "_ZN11AudioSource9setVolumeEf");
        LOGI("[HORN] AudioSource::stop=%p setVolume=%p", (void*)g_AudioStopFn, (void*)g_AudioSetVolFn);
        g_ToolToggleActiveFn = (Entity_toggle_t)dlsym(appLib, "_ZN4Tool12toogleActiveEv");
        if (!g_ToolToggleActiveFn) g_ToolToggleActiveFn = (Entity_toggle_t)FindElfSymbolByPrefix("libapp.so", "_ZN4Tool12toogleActive");
        g_VehiclePrepareAiFn = (Vehicle_prepareAi_t)dlsym(appLib, "_ZN7Vehicle14prepareStartAiEP4Game");
        if (!g_VehiclePrepareAiFn) g_VehiclePrepareAiFn = (Vehicle_prepareAi_t)FindElfSymbolByPrefix("libapp.so", "_ZN7Vehicle14prepareStartAi");
        g_VehicleAiStartFn = (Vehicle_aiStart_t)dlsym(appLib, "_ZN7Vehicle7aiStartEP4Game");
        if (!g_VehicleAiStartFn) g_VehicleAiStartFn = (Vehicle_aiStart_t)FindElfSymbolByPrefix("libapp.so", "_ZN7Vehicle7aiStart");
        g_VehicleAiStopFn = (Entity_toggle_t)dlsym(appLib, "_ZN7Vehicle6aiStopEv");
        if (!g_VehicleAiStopFn) g_VehicleAiStopFn = (Entity_toggle_t)FindElfSymbolByPrefix("libapp.so", "_ZN7Vehicle6aiStop");
        LOGI("[VEHSTATE] symbols: prepareStartAi=%p aiStart=%p aiStop=%p",
             (void*)g_VehiclePrepareAiFn, (void*)g_VehicleAiStartFn, (void*)g_VehicleAiStopFn);
        LOGI("[TRAILER] symbols: attachTrailer=%p combineToggle=%p toolToggleActive=%p",
             (void*)g_VehicleAttachTrailerFn, (void*)g_CombineToggleFn, (void*)g_ToolToggleActiveFn);
        const bool hAttachTool = g_VehicleAttachToolFn && HookChecked(
            "Vehicle::attachTool (walker guard)", (void*)g_VehicleAttachToolFn,
            (void*)my_VehicleAttachToolGuard, (void**)&g_OrigAttachToolHook, true);
        const bool hAttachTrailer = g_VehicleAttachTrailerFn && HookChecked(
            "Vehicle::attachTrailer (walker guard)", (void*)g_VehicleAttachTrailerFn,
            (void*)my_VehicleAttachTrailerGuard, (void**)&g_OrigAttachTrailerHook, true);
        LOGI("[WALKER] attachment guards: tool=%d trailer=%d", (int)hAttachTool, (int)hAttachTrailer);
        LOGI("vehicle hooks: addVehicle=%p removeVehicle=%p Vehicle::destroy=%p detachTool=%p detachTrailer=%p",
             addVeh, remVeh, (void*)g_VehicleDestroyFn, (void*)g_VehicleDetachToolFn, (void*)g_VehicleDetachTrailerFn);
        // skipIfRisky=false: bu ikisi zaten calisiyor; sadece prologue riskini loga yaz.
        orig_Game_addVehicle = (Game_addVehicle_t)addVeh;     // kanca YOK: sadece cagrilacak ham fonksiyon
        HookChecked("Game::removeVehicle", remVeh, (void*)my_Game_removeVehicle, (void**)&orig_Game_removeVehicle, false);
        g_VehicleHooksOk = addVeh && remVeh && g_VehicleDestroyFn;
        WK::Init(appLib);                                      // WALKER: sembolleri bul + cizim kancalari (Vehicle::update'e DOKUNMAZ)
        LL::Start();                                           // CANLI LOG: logcat okuyucu thread

        // Ortak para: hayalet varliklarin gelir/giderini geri alan kancalar (riskli prologue'da TAKILMAZ).
        if (kSharedMoney && kGuardGhostMoney) {
            void* vu = dlsym(appLib, "_ZN7Vehicle6updateEfffbP4Game");
            if (!vu) vu = FindElfSymbolByPrefix("libapp.so", "_ZN7Vehicle6update");
            void* tru = dlsym(appLib, "_ZN7Trailer6updateEfP4Game");
            if (!tru) tru = FindElfSymbolByPrefix("libapp.so", "_ZN7Trailer6update");
            // Tool::update bu oyun surumunde guvenle kancalanamiyor (prologue pc-gorece yukleme): hic denenmez.
            LOGI("[MONEY] guard symbols: Vehicle::update=%p Trailer::update=%p", vu, tru);
            const bool h1 = HookChecked("Vehicle::update", vu, (void*)my_Vehicle_update, (void**)&orig_Vehicle_update, true);
            const bool h2 = HookChecked("Trailer::update", tru, (void*)my_Trailer_update, (void**)&orig_Trailer_update, true);
            LOGI("[MONEY] guard hooks installed: Vehicle=%d Trailer=%d", (int)h1, (int)h2);
            void* ups = dlsym(appLib, "_ZN7Vehicle15updatePipeStateEfP4GameRP7Trailer");
            if (!ups) ups = FindElfSymbolByPrefix("libapp.so", "_ZN7Vehicle15updatePipeState");
            const bool h4 = HookChecked("Vehicle::updatePipeState", ups, (void*)my_Vehicle_updatePipeState,
                                        (void**)&orig_Vehicle_updatePipeState, true);
            LOGI("[PIPE] updatePipeState=%p hook=%d", ups, (int)h4);
        }
        {   // Oyuncu isim etiketleri: kameranin gorunum matrisi RenderArgs::update'ten alinir
            void* rau = dlsym(appLib, "_ZN10RenderArgs6updateERK9Matrix4x4S2_");
            if (!rau) rau = FindElfSymbolByPrefix("libapp.so", "_ZN10RenderArgs6update");
            const bool hr = HookChecked("RenderArgs::update", rau, (void*)my_RenderArgs_update,
                                        (void**)&orig_RenderArgs_update, true);
            LOGI("[NAMETAG] RenderArgs::update=%p hook=%d", rau, (int)hr);
        }
        {   // romork/alet alim-satim fonksiyonlari (sadece cagrilir) + tarla islemi kancasi
            g_GameAddTrailerFn = (Game_addItem_t)dlsym(appLib, "_ZN4Game10addTrailerEN13EntityManager8TRAILERSERK7Vector3fj");
            if (!g_GameAddTrailerFn) g_GameAddTrailerFn = (Game_addItem_t)FindElfSymbolByPrefix("libapp.so", "_ZN4Game10addTrailerE");
            g_GameAddToolFn = (Game_addItem_t)dlsym(appLib, "_ZN4Game7addToolEN13EntityManager5TOOLSERK7Vector3fj");
            if (!g_GameAddToolFn) g_GameAddToolFn = (Game_addItem_t)FindElfSymbolByPrefix("libapp.so", "_ZN4Game7addToolE");
            g_GameRemoveTrailerFn = (Game_removeIdx_t)dlsym(appLib, "_ZN4Game13removeTrailerEj");
            g_GameRemoveToolFn = (Game_removeIdx_t)dlsym(appLib, "_ZN4Game10removeToolEj");
            g_TrailerDestroyFn = (Entity_toggle_t)dlsym(appLib, "_ZN7Trailer7destroyEv");
            g_ToolDestroyFn = (Entity_toggle_t)dlsym(appLib, "_ZN4Tool7destroyEv");
            LOGI("[POOL] symbols: addTrailer=%p addTool=%p removeTrailer=%p removeTool=%p trailerDestroy=%p toolDestroy=%p",
                 (void*)g_GameAddTrailerFn, (void*)g_GameAddToolFn, (void*)g_GameRemoveTrailerFn,
                 (void*)g_GameRemoveToolFn, (void*)g_TrailerDestroyFn, (void*)g_ToolDestroyFn);
            void* uf = dlsym(appLib, "_ZN3Map12updateFieldsEffffffffRN5Field9FieldWorkE");
            if (!uf) uf = FindElfSymbolByPrefix("libapp.so", "_ZN3Map12updateFields");
            g_MapUpdateFieldsRaw = uf;
            const bool hf = kSyncFieldWork && HookChecked("Map::updateFields", uf, (void*)my_Map_updateFields,
                                                          (void**)&orig_Map_updateFields, true);
            LOGI("[FIELD] Map::updateFields=%p hook=%d", uf, (int)hf);
        }
        {   // YENI: balya senkronu (sadece CAGRILAN fonksiyonlar, kanca YOK) + tarla buyume saati
            g_GameAddBaleFn = (Game_addBale_t)dlsym(appLib, "_ZN4Game7addBaleERK7Vector3fj");
            if (!g_GameAddBaleFn) g_GameAddBaleFn = (Game_addBale_t)FindElfSymbolByPrefix("libapp.so", "_ZN4Game7addBale");
            g_GameRemoveBaleFn = (Game_removeBaleP_t)dlsym(appLib, "_ZN4Game10removeBaleEP4Bale");
            g_BaleDestroyFn = (Entity_toggle_t)dlsym(appLib, "_ZN4Bale7destroyEv");
            if (!g_BaleDestroyFn) g_BaleDestroyFn = (Entity_toggle_t)FindElfSymbolByPrefix("libapp.so", "_ZN4Bale7destroy");
            LOGI("[BALE] symbols: addBale=%p removeBale=%p destroy=%p", (void*)g_GameAddBaleFn, (void*)g_GameRemoveBaleFn, (void*)g_BaleDestroyFn);
            g_MapUpdateFn = (Map_update_t)dlsym(appLib, "_ZN3Map6updateEf");
            g_MapGrowNowFn = (Map_grow_t)dlsym(appLib, "_ZN3Map13growFieldsNowEv");
            LOGI("[MAPCLK] symbols: Map::update=%p growFieldsNow=%p", (void*)g_MapUpdateFn, (void*)g_MapGrowNowFn);
            g_TipSiteUpdateBgaFn = (TipSite_updateBga_t)dlsym(appLib, "_ZN7TipSite15updateBgaPlanesEv");
            if (!g_TipSiteUpdateBgaFn) g_TipSiteUpdateBgaFn = (TipSite_updateBga_t)FindElfSymbolByPrefix("libapp.so", "_ZN7TipSite15updateBgaPlanes");
            LOGI("[WORLD] symbols: TipSite::updateBgaPlanes=%p", (void*)g_TipSiteUpdateBgaFn);
            g_MissionSetSpawnFn = (Mission_setSpawn_t)dlsym(appLib, "_ZN14MissionManager20setMissionSpawnPointEj");
            if (!g_MissionSetSpawnFn) g_MissionSetSpawnFn = (Mission_setSpawn_t)FindElfSymbolByPrefix("libapp.so", "_ZN14MissionManager20setMissionSpawnPoint");
            g_MissionCancelFn = (Mission_cancel_t)dlsym(appLib, "_ZN14MissionManager13cancelPendingEv");
            if (!g_MissionCancelFn) g_MissionCancelFn = (Mission_cancel_t)FindElfSymbolByPrefix("libapp.so", "_ZN14MissionManager13cancelPending");
            LOGI("[MISSION] symbols: setMissionSpawnPoint=%p cancelPending=%p", (void*)g_MissionSetSpawnFn, (void*)g_MissionCancelFn);
            g_FieldDeserFn = (Field_deserialize_t)dlsym(appLib, "_ZN5Field11deserializeEPhb");
            if (!g_FieldDeserFn) g_FieldDeserFn = (Field_deserialize_t)FindElfSymbolByPrefix("libapp.so", "_ZN5Field11deserialize");
            LOGI("[FIELDSNAP] symbols: Field::deserialize=%p", (void*)g_FieldDeserFn);
        }
        if (kSyncTraffic) {   // NPC trafik: addTrafficVehicle (sadece cagrilir) + TrafficVehicle::update kancasi
            g_GameAddTrafficFn = (Game_addTraffic_t)dlsym(appLib, "_ZN4Game17addTrafficVehicleEN13EntityManager16TRAFFIC_VEHICLESEjfbRK7Vector3fj");
            if (!g_GameAddTrafficFn) g_GameAddTrafficFn = (Game_addTraffic_t)FindElfSymbolByPrefix("libapp.so", "_ZN4Game17addTrafficVehicleE");
            void* tuf = dlsym(appLib, "_ZN14TrafficVehicle6updateEfP4Game");
            if (!tuf) tuf = FindElfSymbolByPrefix("libapp.so", "_ZN14TrafficVehicle6update");
            const bool ht = tuf && HookChecked("TrafficVehicle::update", tuf, (void*)my_TrafficVehicle_update,
                                               (void**)&orig_TrafficUpdate, true);
            g_TrafficHookOk = ht && g_GameAddTrafficFn != nullptr;
            LOGI("[TRAFFIC] symbols: addTrafficVehicle=%p update=%p hook=%d ok=%d", (void*)g_GameAddTrafficFn, tuf, (int)ht, (int)g_TrafficHookOk);
        }
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
