#pragma once

#include <switch.h>

#include <borealis.hpp>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

/* Lo que comparten la lógica de siempre (main.cpp) y las pantallas nuevas
   (vistas.cpp). */

// Topes del sistema (sys-ticon, FW 19+ y 20+): si uno se pasa, no se aplica ninguno.
constexpr size_t LIMITE_GRANDE         = 102400;
constexpr size_t LIMITE_CHICO          = 65536;
constexpr size_t LIMITE_CHICO_OBJETIVO = 60000;  // con margen

extern thread_local bool g_silencio;
#include <atomic>
extern std::atomic<bool> g_saliendo;
void avisar(const std::string& texto);
// Una línea con la hora en sdmc:/config/icongrabber/registro.log (para soporte).
void marca(const std::string& texto);

nlohmann::json loadConfig();
void saveConfig(nlohmann::json config);

std::string iconPathFor(const std::string& tid);
std::string smallIconPathFor(const std::string& tid);
bool hasCustomIcon(const std::string& tid);
bool sysmoduleInstalled();
std::string sysmoduleName();
std::string toLower(std::string s);

// fitMode -1: el de Ajustes.
bool overwriteIcon(const std::string& tid, const std::string& imagePath, int fitMode = -1);
bool restoreOriginalIcon(const std::string& tid, const std::string& gameName);
bool useOriginalIconAdapted(const std::string& tid, const std::string& gameName);
bool extractOriginalIcon(const std::string& tid, std::string& outPath);
bool controlDataAvailable(u64 appId);
void invalidateControlCache(const std::string& tid);
void askReboot(const std::string& mensaje);

nlohmann::json& installedGames(bool refresh = false);
std::set<u64> juegosEnUsb();

nlohmann::json requestGames(std::string gameName);
nlohmann::json requestIcons(const std::string& gameId, int styleId, int resolutionId);
std::string downloadImage(const nlohmann::json& icon, bool thumbnail);

std::string jstr(const nlohmann::json& j, const char* key, const std::string& def = "");
int jint(const nlohmann::json& j, const char* key, int def = 0);
bool jbool(const nlohmann::json& j, const char* key, bool def = false);

void frame_cropAdjust(const std::string& tid, const std::string& gameName, const std::string& imagePath);
void frame_localImages(const std::string& tid, const std::string& gameName);
void fillProfilesTab(brls::List* list);
int applyProfile(const std::string& nombre);
int saveProfile(const std::string& nombre);
int removeAllIcons();

// Rediseño
std::vector<unsigned char> previewRGBA(const std::string& imagePath, int fitMode, int side);
std::vector<unsigned char> readFileBytes(const std::string& path);
bool imageSize(const std::string& path, int& w, int& h);
std::vector<unsigned char> loadRGB(const std::string& path, int& w, int& h);
std::vector<unsigned char> renderRGBA(const unsigned char* rgb, int w, int h, int fitMode, int side);
bool writePngRGB(const std::string& path, int w, int h, const unsigned char* rgb);
std::vector<unsigned char> currentIconJpeg(const std::string& tid);
std::vector<unsigned char> originalIconJpeg(const std::string& tid);

enum class EstadoIcono { Original, Propio, NoEntra };
EstadoIcono iconState(const std::string& tid);

#include <functional>
#include <map>
std::map<std::string, std::string> iconSources();

bool namesSysmoduleInstalled();
std::string customName(const std::string& tid);
bool setCustomName(const std::string& tid, const std::string& nombre, const std::string& autor);
bool removeCustomName(const std::string& tid);

extern std::vector<std::string> allowedStyles;
