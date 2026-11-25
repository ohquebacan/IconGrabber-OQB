/*
    IconGrabber — versión OQB
    Fork de Slluxx/IconGrabber (GPLv3). El recorrido original estaba partido en
    tres pantallas: en una se elegía el tamaño, en otra se buscaba el juego, y
    para aplicar el ícono había que ir a una tercera y volver a buscar el juego.
    Aquí todo pasa en un solo camino: se elige el juego instalado, se ven sus
    íconos y se aplica ahí mismo.
*/

#include <curl/curl.h>
#include <nanovg/stb_image.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include <algorithm>
#include <borealis.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <iomanip>
#include <set>
#include <sstream>
#include <string_view>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image/stb_image_resize.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image/stb_image_write.h"

namespace i18n = brls::i18n;
using namespace i18n::literals;

std::string configPath = "sdmc:/config/icongrabber/config.json";

// El sysmodule que aplica los íconos. sys-icon es el que sigue mantenido;
// sys-tweak es el viejo y se acepta por compatibilidad.
const std::string USER_ICONS_DIR = "sdmc:/iconos/";
const std::string PROFILES_DIR   = "sdmc:/iconos/perfiles/";

const std::string SYSICON_PATH  = "sdmc:/atmosphere/contents/00FF69636F6EFF00/exefs.nsp";

std::vector<std::string> allowedStyles = {
    "all styles",
    "alternate",
    "blurred",
    "white_logo",
    "material",
    "no_logo"
};

std::vector<std::string> allowedImageResolutions = {
    "512x512",
    "1024x1024",
    "600x900",
    "660x930",
    "342x482",
    "460x215",
    "920x430"
};

#include "core.hpp"
#include "dialogo_centrado.hpp"
#include "vistas.hpp"

// Las notificaciones de borealis no se pueden pedir desde otro hilo: el trabajo
// en segundo plano (íconos para todos, aplicar un perfil) pone esto en true y
// sus avisos van solo al registro.
thread_local bool g_silencio = false;

void avisar(const std::string& texto)
{
    if (g_silencio)
        brls::Logger::info(texto);
    else
        brls::Application::notify(texto);
}

void marca(const std::string& texto)
{
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/icongrabber/", ec);
    if (FILE* f = fopen("sdmc:/config/icongrabber/registro.log", "a"))
    {
        fprintf(f, "%llu %s\n", (unsigned long long)armGetSystemTick() / 19200, texto.c_str());
        fclose(f);
    }
}

// ---------------------------------------------------------------- config ---

/* La configuración se lee desde varios hilos (el servidor del QR la escribe
   mientras la pantalla la lee): un candado, sin excepciones de filesystem y
   escritura a un temporal que después reemplaza al archivo. Leerla a medio
   escribir cerraba la app (24-nov-2026). */
static std::mutex g_mConfig;

static nlohmann::json configPorDefecto()
{
    nlohmann::json config;
    config["api_token"]     = "";
    config["style_id"]      = 0;
    config["resolution_id"] = 0;
    return config;
}

static void escribirConfig(const nlohmann::json& config)
{
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/icongrabber/", ec);
    const std::string temp = configPath + ".tmp";
    {
        std::ofstream o(temp, std::ios::trunc);
        if (!o) return;
        o << config.dump(4);
        if (!o.good()) return;
    }
    std::remove(configPath.c_str());
    std::rename(temp.c_str(), configPath.c_str());
}

nlohmann::json loadConfig()
{
    std::lock_guard<std::mutex> l(g_mConfig);
    std::error_code ec;
    if (!std::filesystem::exists(configPath, ec))
    {
        nlohmann::json config = configPorDefecto();
        escribirConfig(config);
        return config;
    }

    nlohmann::json config;
    try
    {
        std::ifstream i(configPath);
        i >> config;
    }
    catch (const std::exception& e)
    {
        brls::Logger::error("config ilegible, se usa uno nuevo");
        config = configPorDefecto();
    }
    if (!config.is_object()) config = configPorDefecto();
    if (!config.contains("api_token") || !config["api_token"].is_string()) config["api_token"] = "";
    if (!config.contains("style_id") || !config["style_id"].is_number()) config["style_id"] = 0;
    if (!config.contains("resolution_id") || !config["resolution_id"].is_number()) config["resolution_id"] = 0;
    return config;
}

void saveConfig(nlohmann::json config)
{
    std::lock_guard<std::mutex> l(g_mConfig);
    escribirConfig(config);
}

// ----------------------------------------------------------------- util ----

// exists() sin excepciones: con la SD ocupada lanza «I/O error» y cerraba la app.
static bool existe(const std::string& ruta)
{
    std::error_code ec;
    return std::filesystem::exists(ruta, ec);
}

std::string formatApplicationId(u64 ApplicationId)
{
    std::stringstream strm;
    strm << std::uppercase << std::setfill('0') << std::setw(16) << std::hex << ApplicationId;
    return strm.str();
}

// sys-icon lee dos archivos por título: el grande (256) que se ve en la fila
// principal y el chico (174) que usa la pantalla de todas las aplicaciones.
// Escribir sólo el primero dejaba el ícono viejo en la lista completa.
std::string iconPathFor(const std::string& tid)
{
    return "sdmc:/atmosphere/contents/" + tid + "/icon.jpg";
}

std::string smallIconPathFor(const std::string& tid)
{
    return "sdmc:/atmosphere/contents/" + tid + "/icon174.jpg";
}

bool hasCustomIcon(const std::string& tid)
{
    return existe(iconPathFor(tid)) || existe(smallIconPathFor(tid));
}

bool sysmoduleInstalled()
{
    return existe(SYSICON_PATH);
}

std::string sysmoduleName()
{
    if (existe(SYSICON_PATH)) return "sys-icon";
    return "";
}

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Escribe el ícono donde lo lee el sysmodule: 256x256 jpg en la carpeta del
// título. El original pasaba los canales de la imagen de origen a stbi_write_jpg
// aunque el buffer redimensionado siempre tiene 3, así que una imagen con
// transparencia salía corrupta.
// El menú HOME siempre muestra un cuadrado de 256x256, así que una imagen
// vertical hay que encajarla de alguna forma. Tres maneras, elegibles en
// Ajustes, porque cuál conviene depende de la imagen:
//   0 recortar: toma el cuadrado del centro. No deforma, pero corta arriba y abajo.
//   1 ajustar:  entra completa, con franjas del color del borde a los lados.
//   2 estirar:  la aplasta hasta el cuadrado (lo que hacía antes).
//   3 tema vertical: la prepara para temas que estiran el ícono a 2:3.
bool controlDataAvailable(u64 appId);

// El cuadro final (SIDE x SIDE, RGB) según el modo, sin escribir nada. Lo usan
// renderIcon y la vista previa. Devuelve NULL si no se pudo; liberar con free.
unsigned char* renderPixels(unsigned char* img, int width, int height, int fitMode, int SIDE)
{
    unsigned char* out = (unsigned char*)malloc((size_t)SIDE * SIDE * 3);
    if (out == NULL)
        return NULL;

    bool ok = false;

    if (fitMode == 0 && width != height)
    {
        // Recorte: se copia el cuadrado más grande que quepa. Qué parte se toma
        // lo decide cropY (0 = arriba, 50 = centro, 100 = abajo), que en una
        // portada vertical es la diferencia entre agarrar la cara o el título.
        int cropY = loadConfig().value("crop_y", 50);
        cropY     = std::max(0, std::min(100, cropY));

        int side = std::min(width, height);
        int offX = (width - side) / 2;
        int offY = (int)((double)(height - side) * cropY / 100.0);
        unsigned char* square = (unsigned char*)malloc((size_t)side * side * 3);
        if (square)
        {
            for (int y = 0; y < side; y++)
                memcpy(square + (size_t)y * side * 3, img + ((size_t)(y + offY) * width + offX) * 3, (size_t)side * 3);
            ok = stbir_resize_uint8(square, side, side, 0, out, SIDE, SIDE, SIDE * 3, 3) != 0;
            free(square);
        }
    }
    else if (fitMode == 1 && width != height)
    {
        // Ajuste con franjas: la imagen entra entera y el resto se rellena con
        // el color promedio del borde, que disimula bastante mejor que el negro.
        double scale = std::min((double)SIDE / width, (double)SIDE / height);
        int newW     = std::max(1, (int)(width * scale));
        int newH     = std::max(1, (int)(height * scale));

        unsigned long r = 0, g = 0, b = 0, n = 0;
        for (int x = 0; x < width; x++)
        {
            const unsigned char* top = img + ((size_t)0 * width + x) * 3;
            const unsigned char* bot = img + ((size_t)(height - 1) * width + x) * 3;
            r += top[0] + bot[0];
            g += top[1] + bot[1];
            b += top[2] + bot[2];
            n += 2;
        }
        unsigned char fill[3] = { (unsigned char)(r / n), (unsigned char)(g / n), (unsigned char)(b / n) };
        for (int i = 0; i < SIDE * SIDE; i++)
            memcpy(out + (size_t)i * 3, fill, 3);

        unsigned char* scaled = (unsigned char*)malloc((size_t)newW * newH * 3);
        if (scaled)
        {
            if (stbir_resize_uint8(img, width, height, 0, scaled, newW, newH, newW * 3, 3))
            {
                int offX = (SIDE - newW) / 2;
                int offY = (SIDE - newH) / 2;
                for (int y = 0; y < newH; y++)
                    memcpy(out + ((size_t)(y + offY) * SIDE + offX) * 3, scaled + (size_t)y * newW * 3, (size_t)newW * 3);
                ok = true;
            }
            free(scaled);
        }
    }
    else if (fitMode == 3)
    {
        // Tema vertical (2:3). El sistema sólo guarda un cuadrado, y estos temas
        // lo estiran a lo alto al dibujarlo. Así que se arma primero un lienzo
        // 2:3 con la imagen bien proporcionada dentro y después se aplasta al
        // cuadrado: el tema deshace ese aplastado y se ve correcta. Una imagen
        // que ya es 600x900 entra exacta; una cuadrada queda con franjas en vez
        // de deformada.
        const int CW = 512, CH = 768;
        // Si la imagen ya viene en 2:3 (las de 600x900, por ejemplo) se aplasta
        // directo: pasar por el lienzo sería redimensionar dos veces y perder
        // nitidez sin ganar nada.
        double ratio = (double)width / height;
        if (ratio > 0.64 && ratio < 0.70)
        {
            ok = stbir_resize_uint8(img, width, height, 0, out, SIDE, SIDE, SIDE * 3, 3) != 0;
        }
        else
        {
        unsigned char* canvas = (unsigned char*)malloc((size_t)CW * CH * 3);
        if (canvas)
        {
            unsigned long r = 0, g = 0, b = 0, n = 0;
            for (int x = 0; x < width; x++)
            {
                const unsigned char* top = img + ((size_t)0 * width + x) * 3;
                const unsigned char* bot = img + ((size_t)(height - 1) * width + x) * 3;
                r += top[0] + bot[0];
                g += top[1] + bot[1];
                b += top[2] + bot[2];
                n += 2;
            }
            unsigned char fill[3] = { (unsigned char)(r / n), (unsigned char)(g / n), (unsigned char)(b / n) };
            for (int i = 0; i < CW * CH; i++)
                memcpy(canvas + (size_t)i * 3, fill, 3);

            double scale = std::min((double)CW / width, (double)CH / height);
            int newW     = std::max(1, (int)(width * scale));
            int newH     = std::max(1, (int)(height * scale));

            unsigned char* scaled = (unsigned char*)malloc((size_t)newW * newH * 3);
            if (scaled)
            {
                if (stbir_resize_uint8(img, width, height, 0, scaled, newW, newH, newW * 3, 3))
                {
                    int offX = (CW - newW) / 2;
                    int offY = (CH - newH) / 2;
                    for (int y = 0; y < newH; y++)
                        memcpy(canvas + ((size_t)(y + offY) * CW + offX) * 3, scaled + (size_t)y * newW * 3, (size_t)newW * 3);
                    ok = stbir_resize_uint8(canvas, CW, CH, 0, out, SIDE, SIDE, SIDE * 3, 3) != 0;
                }
                free(scaled);
            }
            free(canvas);
        }
        }
    }
    else
    {
        ok = stbir_resize_uint8(img, width, height, 0, out, SIDE, SIDE, SIDE * 3, 3) != 0;
    }

    if (!ok)
    {
        free(out);
        return NULL;
    }
    return out;
}

bool renderIcon(unsigned char* img, int width, int height, int fitMode, int SIDE, const std::string& outPath)
{
    unsigned char* out = renderPixels(img, width, height, fitMode, SIDE);
    if (out == NULL)
        return false;
    bool ok = true;

    // El ícono viaja dentro de los datos del juego, en un hueco de 0x20000
    // bytes (131.072). Si el JPEG pasa de ahí, sys-icon no lo puede usar y el
    // menú se queda con el original: era el caso de las carátulas con mucho
    // detalle guardadas a calidad 100. Se baja la calidad hasta que entre.
    if (ok)
    {
        // El grande entra en 0x20000 (131.072). El chico tiene su propio tope, más
        // bajo: medido en consola, los que pasaban de ~65 KB no se aplicaban, y
        // encima arrastraban al grande, que se quedaba sin cambiar tampoco.
        // 102400 y 65536 son los limites que documenta sys-ticon para FW 19.0.0+
        // y 20.0.0+. sys-icon aguanta mas en el grande, pero capando al menor de
        // los dos el ícono vale para cualquiera de los dos sysmodules, y el
        // usuario no tiene que saber cual tiene instalado.
        const size_t LIMITE = (SIDE >= 256) ? LIMITE_GRANDE : LIMITE_CHICO_OBJETIVO;
        const int calidades[] = { 100, 92, 85, 75, 65, 55, 45 };
        ok = false;

        for (int q : calidades)
        {
            if (!stbi_write_jpg(outPath.c_str(), SIDE, SIDE, 3, out, q))
                break;

            std::error_code ec;
            auto size = std::filesystem::file_size(outPath, ec);
            if (ec)
                break;

            ok = true;
            if (size <= LIMITE)
                break;

            brls::Logger::info("ícono de " + std::to_string(size) + " bytes, se reintenta con menos calidad");
        }
    }

    free(out);
    return ok;
}

/* De qué imagen salió cada ícono, para poder rehacerlos al cambiar de tema
   (Tema): sdmc:/config/icongrabber/fuentes.json, tid -> ruta. */
static std::mutex g_mFuentes;
static const char FUENTES_PATH[] = "sdmc:/config/icongrabber/fuentes.json";

std::map<std::string, std::string> iconSources()
{
    std::lock_guard<std::mutex> l(g_mFuentes);
    std::map<std::string, std::string> out;
    try
    {
        std::ifstream f(FUENTES_PATH);
        if (!f) return out;
        nlohmann::json j;
        f >> j;
        for (auto& [k, v] : j.items())
            if (v.is_string()) out[k] = v.get<std::string>();
    }
    catch (const std::exception&)
    {
    }
    return out;
}

static void setIconSource(const std::string& tid, const std::string& ruta)
{
    auto todas = iconSources();
    std::lock_guard<std::mutex> l(g_mFuentes);
    if (ruta.empty()) todas.erase(tid);
    else todas[tid] = ruta;
    nlohmann::json j = nlohmann::json::object();
    for (auto& [k, v] : todas) j[k] = v;
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/config/icongrabber/", ec);
    const std::string temp = std::string(FUENTES_PATH) + ".tmp";
    {
        std::ofstream o(temp, std::ios::trunc);
        o << j.dump(1);
    }
    std::remove(FUENTES_PATH);
    std::rename(temp.c_str(), FUENTES_PATH);
}

static bool overwriteIconImpl(const std::string& tid, const std::string& imagePath, int fitMode);

bool overwriteIcon(const std::string& tid, const std::string& imagePath, int fitMode)
{
    const bool ok = overwriteIconImpl(tid, imagePath, fitMode);
    if (ok) setIconSource(tid, imagePath);
    return ok;
}

static bool overwriteIconImpl(const std::string& tid, const std::string& imagePath, int fitMode)
{
    int width, height, channels;
    unsigned char* img = stbi_load(imagePath.c_str(), &width, &height, &channels, 3);
    if (img == NULL || width <= 0 || height <= 0)
    {
        avisar("No se pudo leer la imagen");
        return false;
    }

    if (fitMode < 0 || fitMode > 3) fitMode = loadConfig().value("fit_mode", 0);

    std::error_code ec;
    std::filesystem::create_directories("sdmc:/atmosphere/contents/" + tid, ec);

    // Se borran antes de escribir: si una de las dos escrituras fallara, es
    // preferible quedarse sin ícono propio que con los dos tamaños mezclados,
    // uno nuevo y otro de la vez anterior.
    std::remove(iconPathFor(tid).c_str());
    std::remove(smallIconPathFor(tid).c_str());

    // Y se invalida la cache ya, con los archivos fuera. Sin este paso el menu
    // pasa de un ícono propio a otro sin ver nunca el estado intermedio, y se
    // queda mostrando el anterior: habia que borrar el ícono, volver al
    // original, reiniciar, y recien entonces elegir otro. Invalidando aqui el
    // sistema refresca primero al original y despues al nuevo, que es lo mismo
    // que hacia ese rodeo manual pero sin reiniciar.
    // Sin recargar el HOME todavía: con sys-icon, si lo consulta ahora que no hay
    // archivos, se queda con que el juego no tiene ícono propio hasta reiniciar.
    invalidateControlCache(tid, false);

    // Los dos archivos se muestran distinto, así que se preparan distinto:
    //  - el grande lo estira el tema vertical, así que va aplastado a propósito;
    //  - el chico lo dibuja el sistema en marco cuadrado (la lista completa y la
    //    ventanita al cambiar de app), así que va recortado, nunca aplastado.
    // Con un solo tratamiento para ambos, uno de los dos salía siempre mal.
    bool big = renderIcon(img, width, height, fitMode, 256, iconPathFor(tid));

    // El ícono chico manda en la lista completa y en la ventanita al cambiar de
    // app. Cómo prepararlo depende del tema que uses ahí:
    //   0 no tocarlo, se queda el del juego;
    //   1 recortado, para temas que lo muestran cuadrado;
    //   2 igual que el grande, para temas que también lo muestran a lo alto.
    int smallMode = loadConfig().value("small_mode", 1);
    if (smallMode > 0)
    {
        int mode = (smallMode == 2) ? fitMode : 0;
        if (!renderIcon(img, width, height, mode, 174, smallIconPathFor(tid)))
            avisar("El ícono chico no se pudo escribir");
    }

    stbi_image_free(img);

    /* Lo que pasó con Smash y Mario Tennis: si un archivo pasa del tope, el
       sysmodule no aplica NINGUNO de los dos y el juego se queda con el suyo
       sin decir nada. Se baja la calidad hasta 45; si aun así no entra, se
       quita el que sobra y se avisa, en vez de dejar algo que no funciona. */
    {
        std::error_code ec;
        auto grande = std::filesystem::file_size(iconPathFor(tid), ec);
        if (!ec && grande > LIMITE_GRANDE)
        {
            std::remove(iconPathFor(tid).c_str());
            std::remove(smallIconPathFor(tid).c_str());
            avisar("Esa imagen no entra en el HOME ni con menos calidad.\nPrueba con otra.");
            return false;
        }
        auto chico = std::filesystem::file_size(smallIconPathFor(tid), ec);
        if (!ec && chico > LIMITE_CHICO)
        {
            std::remove(smallIconPathFor(tid).c_str());
            avisar("El ícono chico no entraba: se dejó solo el grande.");
        }
    }

    // Si el juego no está instalado, el menú sigue sirviendo su ícono desde la
    // caché y no podemos refrescarla sin borrarle el nombre para siempre. El
    // archivo queda escrito y servirá cuando se reinstale, pero conviene
    // decirlo en vez de dejar al usuario probando una y otra vez.
    if (!controlDataAvailable(strtoull(tid.c_str(), NULL, 16)))
    {
        avisar("Este juego no está instalado.\nEl ícono queda guardado, pero el menú\nseguirá mostrando el actual hasta reinstalarlo.");
        return big;
    }

    invalidateControlCache(tid);
    return big;
}

// ------------------------------------------------------------ juegos SD ----

static nlohmann::json g_games   = nlohmann::json::array();
static bool g_gamesLoaded       = false;

// Los juegos que Nexus Bacán instaló en el disco USB: el mapa de títulos del
// USB y las marcas que deja en la SD (lo mismo que mira OQB Updater).
std::set<u64> juegosEnUsb()
{
    std::set<u64> juegos;
    std::ifstream f("sdmc:/atmosphere/usb_content/title_map.bin", std::ios::binary);
    if (f)
    {
        std::stringstream st;
        st << f.rdbuf();
        const std::string b = st.str();
        auto u32At = [&b](size_t p) { u32 v; memcpy(&v, b.data() + p, 4); return v; };
        auto u64At = [&b](size_t p) { u64 v; memcpy(&v, b.data() + p, 8); return v; };
        if (b.size() >= 16 && u32At(0) == 0x54425355 && u32At(4) == 1)
        {
            const u32 n = u32At(8);
            if (b.size() == 16 + (size_t)n * 16)
                for (u32 i = 0; i < n; ++i)
                {
                    const u64 id = u64At(16 + (size_t)i * 16);
                    if (id) juegos.insert(id & ~0x1FFFULL);
                }
        }
    }
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("sdmc:/atmosphere/usb_badges", ec))
    {
        const std::string n = e.path().filename().string();
        if (n.size() != 22 || n.compare(16, 6, "-0.ubg") != 0) continue;
        char* fin = nullptr;
        const u64 id = strtoull(n.substr(0, 16).c_str(), &fin, 16);
        if (id && fin && *fin == 0) juegos.insert(id & ~0x1FFFULL);
    }
    return juegos;
}

// El original pedía 64000 registros de golpe y cortaba el recorrido entero con
// break si un título fallaba, así que un solo juego roto dejaba la lista a
// medias. Ahora se salta ese título y sigue.
nlohmann::json getInstalledGames()
{
    const int MAX_RECORDS = 4096;
    NsApplicationRecord* records = new NsApplicationRecord[MAX_RECORDS]();
    NsApplicationControlData* controlData = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));

    nlohmann::json games = nlohmann::json::array();
    if (controlData == NULL)
    {
        delete[] records;
        return games;
    }

    marca("lista: usb");
    const std::set<u64> enUsb = juegosEnUsb();
    marca("lista: registros");

    int recordCount = 0;
    if (R_FAILED(nsListApplicationRecord(records, MAX_RECORDS, 0, &recordCount)))
    {
        free(controlData);
        delete[] records;
        return games;
    }

    for (s32 i = 0; i < recordCount; i++)
    {
        u64 tid            = records[i].application_id;
        size_t controlSize = 0;
        NacpLanguageEntry* langEntry = NULL;

        // Un juego archivado ya no tiene su contenido en la consola, así que
        // pedir sus datos al almacenamiento falla; pero el menú sigue mostrando
        // su nombre porque quedan en la caché. Por eso se prueban las dos vías:
        // así los archivados también salen en la lista y se les puede dejar el
        // ícono preparado para cuando se vuelvan a instalar.
        /* Primero la caché del HOME: no lee el juego de la tarjeta ni del USB.
           Leer cada juego del almacenamiento tardaba 22 s con 91 juegos.
           Archivado = sin su contenido en la consola. */
        bool instalado = false;
        bool archivado = R_SUCCEEDED(nsIsAnyApplicationEntityInstalled(tid, &instalado)) && !instalado;
        if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_CacheOnly, tid, controlData, sizeof(NsApplicationControlData), &controlSize)) &&
            R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, tid, controlData, sizeof(NsApplicationControlData), &controlSize)))
            continue;
        if (R_FAILED(nacpGetLanguageEntry(&controlData->nacp, &langEntry)))
            continue;
        if (langEntry == NULL || !langEntry->name[0])
            continue;

        games.push_back({ { "tid", formatApplicationId(tid) },
                          { "name", std::string(langEntry->name) },
                          { "author", std::string(langEntry->author) },
                          { "archivado", archivado },
                          { "usb", enUsb.count(tid & ~0x1FFFULL) > 0 } });
    }

    free(controlData);
    delete[] records;

    marca("lista: " + std::to_string(games.size()) + " juegos; pdm");
    // Cuánto hace que se jugó cada uno, para poder ordenar por recientes.
    // pdm devuelve los minutos transcurridos desde la última vez: menos = más
    // reciente. flag en 0 significa que no hay dato, y esos van al final.
    if (R_SUCCEEDED(pdmqryInitialize()))
    {
        const s32 CHUNK = 32;
        std::vector<u64> ids;
        for (auto& g : games)
            ids.push_back(strtoull(g["tid"].get<std::string>().c_str(), NULL, 16));

        for (size_t start = 0; start < ids.size(); start += CHUNK)
        {
            s32 count = (s32)std::min((size_t)CHUNK, ids.size() - start);
            PdmLastPlayTime times[CHUNK] = {};
            s32 total = 0;
            if (R_FAILED(pdmqryQueryLastPlayTime(false, times, ids.data() + start, count, &total)))
                continue;
            for (s32 i = 0; i < total && (start + i) < games.size(); i++)
            {
                if (times[i].flag)
                    games[start + i]["last_played"] = (u64)times[i].last_played_minutes;
            }
        }
        pdmqryExit();
    }

    marca("lista: orden");
    std::sort(games.begin(), games.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return toLower(a["name"].get<std::string>()) < toLower(b["name"].get<std::string>());
    });
    return games;
}

nlohmann::json& installedGames(bool refresh)
{
    if (refresh || !g_gamesLoaded)
    {
        g_games       = getInstalledGames();
        g_gamesLoaded = true;
    }
    return g_games;
}

// ------------------------------------------------------------- red -------

size_t write_to_string(void* ptr, size_t size, size_t nmemb, void* userdata)
{
    size_t realsize   = size * nmemb;
    std::string* out  = static_cast<std::string*>(userdata);
    out->append(static_cast<const char*>(ptr), realsize);
    return realsize;
}

size_t write_to_file(void* ptr, size_t size, size_t nmemb, FILE* stream)
{
    return fwrite(ptr, size, nmemb, stream);
}

std::string base_name(std::string const& path)
{
    return path.substr(path.find_last_of("/\\") + 1);
}

void applyCommonCurlOptions(CURL* curl)
{
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "IconGrabber-OQB/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // Al cerrar la app, cortar lo que esté bajando: si no, el hilo seguiría
    // corriendo con el código ya descargado de memoria.
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +[](void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int { return g_saliendo ? 1 : 0; });
}

nlohmann::json httpGetJson(const std::string& url)
{
    nlohmann::json config = loadConfig();
    std::string authString = "Authorization: Bearer " + config["api_token"].get<std::string>();

    nlohmann::json fail;
    fail["success"] = false;

    // curl_global_init va una sola vez en main: no se puede llamar desde
    // varios hilos a la vez, y ahora las búsquedas corren en segundo plano.
    CURL* curl = curl_easy_init();
    if (!curl)
        return fail;

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, authString.c_str());

    std::string response;
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    applyCommonCurlOptions(curl);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        brls::Logger::error(std::string("curl: ") + curl_easy_strerror(res));
        fail["error"] = curl_easy_strerror(res);
        return fail;
    }

    // Una respuesta rara de la API no debe tumbar la app.
    try
    {
        return nlohmann::json::parse(response);
    }
    catch (const std::exception& e)
    {
        brls::Logger::error("respuesta de la API ilegible");
        fail["error"] = "respuesta ilegible";
        return fail;
    }
}

nlohmann::json requestGames(std::string gameName)
{
    CURL* esc = curl_easy_init();
    char* encoded = esc ? curl_easy_escape(esc, gameName.c_str(), gameName.length()) : NULL;
    std::string url = "https://www.steamgriddb.com/api/v2/search/autocomplete/";
    url.append(encoded ? encoded : gameName.c_str());
    if (encoded) curl_free(encoded);
    if (esc) curl_easy_cleanup(esc);
    return httpGetJson(url);
}

nlohmann::json requestIcons(const std::string& gameId, int styleId, int resolutionId)
{
    std::string url = "https://www.steamgriddb.com/api/v2/grids/game/" + gameId;
    url.append("?styles=");
    if (styleId == 0)
        url.append("alternate,blurred,white_logo,material,no_logo");
    else
        url.append(allowedStyles[styleId]);

    // resolutionId == -1 significa "todos los tamaños": sin filtro, que es lo
    // que evita la pantalla vacía cuando el tamaño elegido no existe.
    if (resolutionId >= 0)
    {
        url.append("&dimensions=");
        url.append(allowedImageResolutions[resolutionId]);
    }
    url.append("&mimes=image/png,image/jpeg");
    return httpGetJson(url);
}

// Descarga la miniatura (para previsualizar) o la imagen completa (para aplicar).
std::string downloadImage(const nlohmann::json& icon, bool thumbnail)
{
    std::string url;
    if (thumbnail)
        url = icon.value("thumb", "");
    else
        url = icon.value("url", icon.value("thumb", ""));

    if (url.empty())
        return "";

    std::string outpath = thumbnail ? "sdmc:/gameIcons/thumbnails/" : "sdmc:/gameIcons/full/";
    std::error_code ec;
    std::filesystem::create_directories(outpath, ec);

    outpath.append(icon["id"].dump());
    outpath.append("_");
    outpath.append(base_name(url));

    // Una descarga cortada deja un archivo de 0 bytes en la caché y después
    // "aplicar" fallaba siempre con esa imagen. Si está vacío, se rehace.
    if (existe(outpath))
    {
        std::error_code sizeEc;
        if (std::filesystem::file_size(outpath, sizeEc) > 0 && !sizeEc)
            return outpath;
        std::remove(outpath.c_str());
    }

    CURL* curl = curl_easy_init();
    if (!curl)
        return "";

    FILE* fp = fopen(outpath.c_str(), "wb");
    if (!fp)
    {
        curl_easy_cleanup(curl);
        return "";
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    applyCommonCurlOptions(curl);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (res != CURLE_OK)
    {
        std::remove(outpath.c_str());
        return "";
    }
    return outpath;
}

// ------------------------------------------------------------ pantallas ---

// Lecturas defensivas: la API devuelve null en campos que uno espera con valor,
// y json::value() lanza excepción cuando el tipo no coincide. Eso tumbaba la app.
std::string jstr(const nlohmann::json& j, const char* key, const std::string& def)
{
    if (!j.contains(key) || j[key].is_null()) return def;
    if (j[key].is_string()) return j[key].get<std::string>();
    return j[key].dump();
}

int jint(const nlohmann::json& j, const char* key, int def)
{
    if (!j.contains(key) || !j[key].is_number()) return def;
    return j[key].get<int>();
}

bool jbool(const nlohmann::json& j, const char* key, bool def)
{
    if (!j.contains(key) || !j[key].is_boolean()) return def;
    return j[key].get<bool>();
}

// Tamaños que se pueden elegir dentro de la pantalla de íconos. El primero no
// filtra nada, para que nunca quede vacía por culpa del filtro.
std::vector<std::string> sizeChoices = {
    "Todos los tamaños",
    "512x512",
    "1024x1024",
    "600x900",
    "660x930",
    "342x482",
    "460x215",
    "920x430"
};

void frame_icons(const std::string& tid, const std::string& gameName, const std::string& gameId, size_t sizeChoice);
void frame_localImages(const std::string& tid, const std::string& gameName);
void frame_cropAdjust(const std::string& tid, const std::string& gameName, const std::string& imagePath);
void frame_matches(const std::string& tid, const std::string& gameName, const std::string& searchTerm);

// El menú HOME guarda en caché los datos de cada juego, ícono incluido. Si no
// se invalida, puede seguir mostrando el anterior aunque el archivo de la SD ya
// esté cambiado: es el comando 404 de ns:am, el mismo que usa OQB-updater al
// crear accesos directos.
// OJO: invalidar la caché de un juego que NO está instalado le borra el nombre
// y el ícono para siempre, porque su contenido ya no está en la consola y sólo
// podría recuperarlos de los servidores de Nintendo. Por eso antes se comprueba
// que sus datos se puedan leer del almacenamiento; si no, no se toca nada.
bool controlDataAvailable(u64 appId)
{
    NsApplicationControlData* probe = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
    if (probe == NULL)
        return false;

    size_t probeSize = 0;
    bool ok = R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_StorageOnly, appId, probe, sizeof(NsApplicationControlData), &probeSize)) && probeSize > sizeof(probe->nacp);
    free(probe);
    return ok;
}

/* En 23.5 el comando 404 solo ya no basta: borra la caché, pero el HOME no
   vuelve a pedir el ícono hasta reiniciar. Lo que sí lo hace recargar el juego
   en el momento es que ns publique otra vez su registro, como cuando se instala
   algo. Se vuelve a publicar idéntico (los mismos contenidos y el mismo último
   evento), así que no cambia nada más que el aviso al HOME. Es lo mismo que hace
   «Actualizar HOME» en Nexus Bacán (RefreshHome en home_badges.cpp, probado en
   la Lite el 25-nov-2026). Comandos 16 y 17 de IApplicationManagerInterface. */
namespace
{
struct RegistroContenido
{
    NcmContentMetaKey key;
    u8 storage_id;
    u8 padding[0x7];
};
static_assert(sizeof(RegistroContenido) == 0x18, "ContentStorageRecord mide 0x18");

bool leerRegistro(Service* srv, u64 appId, u8& evento, std::vector<RegistroContenido>& registros)
{
    bool encontrado = false;
    std::vector<NsApplicationRecord> pagina(64);
    s32 desde = 0, leidos = 0;
    do
    {
        if (R_FAILED(nsListApplicationRecord(pagina.data(), (s32)pagina.size(), desde, &leidos))) return false;
        for (s32 i = 0; i < leidos; ++i)
            if (pagina[i].application_id == appId)
            {
                evento     = pagina[i].last_event;
                encontrado = true;
                break;
            }
        desde += leidos;
    } while (!encontrado && leidos == (s32)pagina.size() && desde < 16384);
    if (!encontrado) return false;

    s32 cuantos = 0;
    if (R_FAILED(nsCountApplicationContentMeta(appId, &cuantos)) || cuantos <= 0 || cuantos > 4096) return false;
    registros.resize(cuantos);
    const struct
    {
        u64 offset;
        u64 tid;
    } in = { 0, appId };
    s32 vinieron = 0;
    if (R_FAILED(serviceDispatchInOut(srv, 17, in, vinieron,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
            .buffers      = { { registros.data(), registros.size() * sizeof(RegistroContenido) } })))
        return false;
    // Si no vino exactamente lo que ns dijo que había, mejor no publicar nada:
    // un registro con contenidos de menos dejaría el juego a medias.
    return vinieron == cuantos;
}

bool republicarRegistro(Service* srv, u64 appId)
{
    // Con IconGrabber abierto en lugar de un juego (title override), ese juego
    // está corriendo: a su registro no se lo toca, se queda con el 404 solo.
    u64 propio = 0;
    if (R_SUCCEEDED(svcGetInfo(&propio, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) && propio == appId) return false;

    u8 evento = 0;
    std::vector<RegistroContenido> registros;
    if (!leerRegistro(srv, appId, evento, registros)) return false;
    const struct
    {
        u8 last_modified_event;
        u8 padding[0x7];
        u64 tid;
    } in = { evento, { 0 }, appId };
    return R_SUCCEEDED(serviceDispatchIn(srv, 16, in,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers      = { { registros.data(), registros.size() * sizeof(RegistroContenido) } }));
}

/* sys-icon (pack sin Nexus) guarda por juego si tiene icon.jpg. Un juego que no
   tenía ícono propio cuando el HOME lo consultó sigue sin mostrar el nuevo hasta
   reiniciar, aunque se lo recargue. Como aproximación del arranque se toma la
   lista de juegos con ícono propio al abrir la app; un juego que el HOME vuelve
   a cargar sin ícono propio (porque se lo quitamos) sale de la lista. */
std::mutex g_conIconoMutex;
std::set<std::string> g_conIconoAlCargar;
bool g_conIconoListo = false;
}  // namespace

std::vector<std::string> titlesWithIcons();
std::atomic<int> g_sinVerHastaReiniciar{ 0 };

void recordarIconosDelArranque()
{
    std::lock_guard<std::mutex> l(g_conIconoMutex);
    if (g_conIconoListo) return;
    for (const std::string& tid : titlesWithIcons()) g_conIconoAlCargar.insert(tid);
    g_conIconoListo = true;
}

bool seVeSinReiniciar(const std::string& tid)
{
    if (nexusInstalado() || !sysIconActivo()) return true;
    if (!hasCustomIcon(tid)) return true;
    std::lock_guard<std::mutex> l(g_conIconoMutex);
    return g_conIconoAlCargar.count(tid) > 0;
}

std::string avisoHome(const std::string& tid)
{
    if (!nexusInstalado() && !sysIconActivo())
        return "Probablemente no se vea todavía: no encuentro sys-icon activo ni Nexus Bacán. sys-icon se instala desde OQB Updater › Apps › Sysmodules.";
    if (!seVeSinReiniciar(tid))
        return "Probablemente se vea recién al reiniciar: es su primer ícono propio desde que encendiste la consola.";
    if (nexusInstalado() && juegosEnUsb().count(strtoull(tid.c_str(), NULL, 16)))
        return "Ya se ve en el HOME. La insignia USB vuelve con «Actualizar HOME» en Nexus Bacán, con el USB conectado.";
    return "Ya se ve en el HOME.";
}

void avisarCambio(const std::string& tid, const std::string& mensaje)
{
    if (seVeSinReiniciar(tid))
        avisar(mensaje + "\n" + avisoHome(tid));
    else
        askReboot(mensaje + "\n\n" + avisoHome(tid));
}

void invalidateControlCache(const std::string& tid, bool refrescarHome)
{
    u64 appId = strtoull(tid.c_str(), NULL, 16);

    if (!controlDataAvailable(appId))
    {
        brls::Logger::info("archivado o sin contenido: no se toca su caché");
        return;
    }

    g_genIconos++;
    Service srv;
    if (R_FAILED(nsGetApplicationManagerInterface(&srv)))
        return;
    serviceDispatchIn(&srv, 404, appId);
    if (refrescarHome)
    {
        const bool ok = republicarRegistro(&srv, appId);
        // Otra vez, por si el HOME pidió los datos antes de que llegara el
        // primero: así recarga seguro lo que hay ahora en la SD.
        serviceDispatchIn(&srv, 404, appId);
        marca(std::string("HOME ") + tid + (ok ? " recargado" : " sin registro, solo caché"));

        if (!seVeSinReiniciar(tid))
            g_sinVerHastaReiniciar++;
        else if (ok && !hasCustomIcon(tid) && sysIconActivo() && !nexusInstalado())
        {
            std::lock_guard<std::mutex> l(g_conIconoMutex);
            g_conIconoAlCargar.erase(tid);
        }
    }
    serviceClose(&srv);
}

// Saca el ícono que trae el propio juego y lo deja en un archivo. Sirve para
// re-prepararlo: con un tema vertical, el ícono original (cuadrado) sale
// estirado, así que hay que pasarlo por el mismo tratamiento que los demás.
bool extractOriginalIcon(const std::string& tid, std::string& outPath)
{
    u64 appId = strtoull(tid.c_str(), NULL, 16);

    // Si el juego no está instalado no hay ícono original que sacar, y quitarle
    // los que tiene lo dejaría sin ninguno.
    if (!controlDataAvailable(appId))
        return false;

    // Con los íconos propios puestos, sys-icon devolvería esos mismos y
    // acabaríamos reaplicando lo que ya había. Se quitan antes de preguntar.
    std::remove(iconPathFor(tid).c_str());
    std::remove(smallIconPathFor(tid).c_str());
    invalidateControlCache(tid, false);

    NsApplicationControlData* controlData = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
    if (controlData == NULL)
        return false;

    size_t controlSize = 0;
    if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_StorageOnly, appId, controlData, sizeof(NsApplicationControlData), &controlSize)) || controlSize <= sizeof(controlData->nacp))
    {
        free(controlData);
        return false;
    }

    size_t iconSize = controlSize - sizeof(controlData->nacp);

    std::error_code ec;
    std::filesystem::create_directories("sdmc:/gameIcons/originales/", ec);
    outPath = "sdmc:/gameIcons/originales/" + tid + ".jpg";

    FILE* f = fopen(outPath.c_str(), "wb");
    if (f == NULL)
    {
        free(controlData);
        return false;
    }
    fwrite(controlData->icon, 1, iconSize, f);
    fclose(f);
    free(controlData);
    return true;
}

// Deja el ícono del juego tal cual venía, pero adaptado al modo elegido. Con un
// tema vertical eso es lo que hace que un ícono cuadrado deje de verse estirado.
bool useOriginalIconAdapted(const std::string& tid, const std::string& gameName)
{
    std::string original;
    if (!extractOriginalIcon(tid, original))
    {
        avisar("No se pudo leer el ícono original de " + gameName);
        return false;
    }
    if (!overwriteIcon(tid, original))
    {
        avisar("No se pudo preparar el ícono original");
        return false;
    }
    avisarCambio(tid, "Ícono original adaptado: " + gameName);
    return true;
}

// Cuando un cambio no se ve hasta reiniciar, conviene ofrecerlo en el momento y
// no dejar que el usuario tenga que acordarse de ir a Ajustes.
void askReboot(const std::string& mensaje)
{
    brls::Dialog* dialog = new DialogoCentrado(mensaje + "\n\n¿Reiniciar ahora?");
    dialog->addButton("Reiniciar", [](brls::View* v) {
        if (R_SUCCEEDED(spsmInitialize()))
        {
            spsmShutdown(true);
            spsmExit();
        }
        else
        {
            avisar("No se pudo reiniciar");
        }
    });
    dialog->addButton("Después", [dialog](brls::View* v) { dialog->close(); });
    dialog->setCancelable(true);
    dialog->open();
}

// Devolver el ícono original es borrar el archivo que escribimos: el sistema
// vuelve a usar el que trae el juego.
bool restoreOriginalIcon(const std::string& tid, const std::string& gameName)
{
    if (!hasCustomIcon(tid))
    {
        avisar(gameName + " ya tiene su ícono original");
        return false;
    }
    std::remove(iconPathFor(tid).c_str());
    std::remove(smallIconPathFor(tid).c_str());
    setIconSource(tid, "");
    invalidateControlCache(tid);
    avisarCambio(tid, "Ícono original restaurado: " + gameName);
    return true;
}

bool isImageFile(const std::filesystem::path& p)
{
    std::string ext = toLower(p.extension().string());
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}

// El ajuste del recorte está en vistas.cpp (VistaRecorte).

// Íconos propios: cualquier imagen que el usuario deje en /iconos/ de la SD, más
// lo que ya se haya descargado antes. Sirve cuando SteamGridDB no tiene nada.
void frame_localImages(const std::string& tid, const std::string& gameName)
{
    std::error_code ec;
    std::filesystem::create_directories(USER_ICONS_DIR, ec);

    brls::ThumbnailFrame* frame = new brls::ThumbnailFrame();
    frame->setTitle("Íconos guardados en la SD");
    frame->setIcon(BOREALIS_ASSET("icon/borealis.jpg"));
    frame->getSidebar()->setThumbnail(BOREALIS_ASSET("icon/borealis.jpg"));
    frame->getSidebar()->setTitle(gameName);
    frame->getSidebar()->setSubtitle("Elige una imagen");
    frame->getSidebar()->getButton()->setLabel("Aplicar este ícono");

    brls::List* list = new brls::List();
    int added        = 0;

    for (const std::string& dir : { USER_ICONS_DIR, std::string("sdmc:/gameIcons/full/") })
    {
        if (!std::filesystem::exists(dir, ec))
            continue;

        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (ec) break;
            if (!entry.is_regular_file(ec) || !isImageFile(entry.path()))
                continue;

            std::string path     = entry.path().string();
            std::string filename = entry.path().filename().string();
            std::string origen   = dir == USER_ICONS_DIR ? "tuya" : "descargada";

            brls::ListItem* litem = new brls::ListItem(filename, "", origen);
            litem->getClickEvent()->subscribe([=](brls::View* view) {
                if (overwriteIcon(tid, path))
                    avisarCambio(tid, "Ícono aplicado a " + gameName);
                else
                    avisar("No se pudo leer esa imagen");
            });
            litem->registerAction("Ajustar el recorte", brls::Key::Y, [tid, gameName, path] {
                frame_cropAdjust(tid, gameName, path);
                return true;
            });
            litem->getFocusEvent()->subscribe([=](brls::View* view) {
                frame->getSidebar()->getButton()->getClickEvent()->unsubscribeAll();
                frame->getSidebar()->setThumbnail(path);
                frame->getSidebar()->setSubtitle(filename);
                frame->getSidebar()->getButton()->getClickEvent()->subscribe([=](brls::View* view) {
                    if (overwriteIcon(tid, path))
                        avisar("Ícono aplicado a " + gameName + "\nPulsa B para volver");
                    else
                        avisar("No se pudo aplicar esa imagen");
                });
            });
            list->addView(litem);
            added++;
        }
    }

    if (added == 0)
        list->addView(new brls::ListItem("No hay imágenes todavía", "Copia archivos .jpg o .png a la carpeta /iconos/ de la SD"));

    frame->setContentView(list);
    brls::Application::pushView(frame);
}

void frame_icons(const std::string& tid, const std::string& gameName, const std::string& gameId, size_t sizeChoice)
{
    nlohmann::json config = loadConfig();
    int styleId           = config["style_id"].get<int>();
    int resolutionId      = sizeChoice == 0 ? -1 : (int)(sizeChoice - 1);

    brls::ThumbnailFrame* frame = new brls::ThumbnailFrame();
    frame->setTitle(gameName);
    frame->setIcon(BOREALIS_ASSET("icon/borealis.jpg"));
    frame->getSidebar()->setThumbnail(BOREALIS_ASSET("icon/borealis.jpg"));
    frame->getSidebar()->setTitle(gameName);
    frame->getSidebar()->setSubtitle("Elige un ícono");
    frame->getSidebar()->getButton()->setLabel("Aplicar este ícono");

    nlohmann::json icons = requestIcons(gameId, styleId, resolutionId);

    brls::List* list = new brls::List();

    // Un ListItem normal, no un SelectListItem: el evento de ese último se
    // dispara mientras su propio desplegable se está cerrando, y abrir otra
    // pantalla desde ahí congelaba la app.
    brls::ListItem* sizeItem = new brls::ListItem("Tamaño", "", "Elige con qué medida buscar");
    sizeItem->setValue(sizeChoices[sizeChoice]);
    sizeItem->getClickEvent()->subscribe([tid, gameName, gameId, sizeChoice](brls::View* view) {
        brls::AppletFrame* picker = new brls::AppletFrame(true, true);
        picker->setTitle("Tamaño del ícono");
        picker->setIcon(BOREALIS_ASSET("icon/borealis.jpg"));
        brls::List* opts = new brls::List();
        for (size_t i = 0; i < sizeChoices.size(); i++)
        {
            brls::ListItem* opt = new brls::ListItem(sizeChoices[i]);
            if (i == sizeChoice)
                opt->setValue("actual");
            opt->getClickEvent()->subscribe([tid, gameName, gameId, i](brls::View* v) {
                frame_icons(tid, gameName, gameId, i);
            });
            opts->addView(opt);
        }
        picker->setContentView(opts);
        brls::Application::pushView(picker);
    });
    list->addView(sizeItem);

    if (hasCustomIcon(tid))
    {
        brls::ListItem* restore = new brls::ListItem("Restaurar el ícono original", "", "Quita el ícono que le pusiste a este juego");
        restore->getClickEvent()->subscribe([tid, gameName, restore](brls::View* view) {
            if (restoreOriginalIcon(tid, gameName))
                restore->setValue("hecho");
        });
        list->addView(restore);
    }

    // Salida manual por si el menu se queda mostrando un ícono viejo. Normalmente
    // no hace falta, porque aplicar ya invalida la cache; está aquí para no tener
    // que recurrir al rodeo de quitar el ícono, reiniciar y volver a ponerlo.
    brls::ListItem* refresh = new brls::ListItem("Refrescar la caché del menú", "", "Si el menú sigue mostrando el ícono anterior");
    refresh->getClickEvent()->subscribe([tid, gameName, refresh](brls::View* view) {
        if (!controlDataAvailable(strtoull(tid.c_str(), NULL, 16)))
        {
            avisar("Este juego no está instalado.\nRefrescar su caché le borraría el nombre\ny el ícono, así que no se toca.");
            return;
        }
        invalidateControlCache(tid);
        refresh->setValue("hecho");
        avisar("Caché refrescada para " + gameName);
    });
    list->addView(refresh);

    brls::ListItem* localItem = new brls::ListItem("Usar una imagen de la SD", "", "Para cuando SteamGridDB no tiene nada");
    localItem->getClickEvent()->subscribe([tid, gameName](brls::View* view) {
        frame_localImages(tid, gameName);
    });
    list->addView(localItem);

    int added = 0;
    if (jbool(icons, "success") && icons.contains("data") && icons["data"].is_array())
    {
        for (auto it : icons["data"])
        {
            if (jbool(it, "lock"))
                continue;

            std::string label = jstr(it, "style", "ícono");
            label.append(" · ");
            label.append(std::to_string(jint(it, "width")));
            label.append("x");
            label.append(std::to_string(jint(it, "height")));

            brls::ListItem* litem = new brls::ListItem(label);

            // Aplicar directamente con A. El botón lateral sólo funciona si el
            // ícono llegó a tener el foco, y eso dejaba la impresión de que la
            // app se colgaba: al entrar, el foco está en "Tamaño", no en un ícono.
            litem->getClickEvent()->subscribe([=](brls::View* view) {
                avisar("Descargando…");
                std::string full = downloadImage(it, false);
                if (full.empty())
                {
                    avisar("No se pudo descargar esa imagen");
                    return;
                }
                if (overwriteIcon(tid, full))
                    avisarCambio(tid, "Ícono aplicado a " + gameName);
                else
                    avisar("La imagen descargada no se pudo leer");
            });

            litem->registerAction("Ajustar el recorte", brls::Key::Y, [=] {
                avisar("Descargando…");
                std::string full = downloadImage(it, false);
                if (full.empty())
                {
                    avisar("No se pudo descargar esa imagen");
                    return true;
                }
                frame_cropAdjust(tid, gameName, full);
                return true;
            });

            litem->getFocusEvent()->subscribe([=](brls::View* view) {
                frame->getSidebar()->getButton()->getClickEvent()->unsubscribeAll();
                std::string thumb = downloadImage(it, true);
                if (!thumb.empty())
                {
                    frame->getSidebar()->setThumbnail(thumb);
                    frame->getSidebar()->setSubtitle(label);
                }
                else
                {
                    frame->getSidebar()->setThumbnail(BOREALIS_ASSET("icon/borealis.jpg"));
                    frame->getSidebar()->setSubtitle("Sin vista previa");
                }
                frame->getSidebar()->getButton()->getClickEvent()->subscribe([=](brls::View* view) {
                    avisar("Descargando…");
                    std::string full = downloadImage(it, false);
                    if (full.empty())
                    {
                        avisar("No se pudo descargar");
                        return;
                    }
                    if (overwriteIcon(tid, full))
                        avisarCambio(tid, "Ícono aplicado a " + gameName);
                    else
                        avisar("La imagen descargada no se pudo leer");
                });
            });
            list->addView(litem);
            added++;
        }
    }

    if (added == 0)
    {
        std::string detalle = icons.contains("error") ? jstr(icons, "error") : "Prueba con otro tamaño aquí arriba";
        list->addView(new brls::ListItem("Sin íconos con este filtro", detalle));
    }

    frame->setContentView(list);
    brls::Application::pushView(frame);
}

// Paso intermedio: SteamGridDB devuelve varios juegos parecidos y conviene
// elegir cuál es. Con FC 27, por ejemplo, salen varios FIFA y el usuario sabe
// mejor que nosotros cuál sirve.
void frame_matches(const std::string& tid, const std::string& gameName, const std::string& searchTerm)
{
    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle(gameName);
    frame->setIcon(BOREALIS_ASSET("icon/borealis.jpg"));

    nlohmann::json found = requestGames(searchTerm);
    brls::List* list     = new brls::List();

    brls::ListItem* adapt = new brls::ListItem("Usar el ícono original del juego", "", "Lo adapta al tema, para que no salga estirado");
    adapt->getClickEvent()->subscribe([tid, gameName, adapt](brls::View* view) {
        if (useOriginalIconAdapted(tid, gameName))
            adapt->setValue("hecho");
    });
    list->addView(adapt);

    if (hasCustomIcon(tid))
    {
        brls::ListItem* restore = new brls::ListItem("Quitar el ícono personalizado", "", "Vuelve al ícono del juego, sin adaptar");
        restore->getClickEvent()->subscribe([tid, gameName, restore](brls::View* view) {
            if (restoreOriginalIcon(tid, gameName))
                restore->setValue("hecho");
        });
        list->addView(restore);
    }

    int added = 0;
    if (jbool(found, "success") && found.contains("data") && found["data"].is_array())
    {
        for (auto it : found["data"])
        {
            std::string name = jstr(it, "name");
            std::string id   = jstr(it, "id");
            if (name.empty() || id.empty())
                continue;

            brls::ListItem* litem = new brls::ListItem(name);
            litem->getClickEvent()->subscribe([tid, gameName, id](brls::View* view) {
                frame_icons(tid, gameName, id, 0);
            });
            list->addView(litem);
            added++;
        }
    }

    if (added == 0)
    {
        std::string detalle = found.contains("error") ? jstr(found, "error") : "Pulsa X y escribe otro nombre";
        list->addView(new brls::ListItem("Sin coincidencias para: " + searchTerm, detalle));
    }

    frame->setContentView(list);

    // Abre el teclado y, al aceptar, apila otra pantalla de coincidencias. No se
    // cierra la actual desde su propio manejador: eso era lo que crasheaba.
    frame->registerAction("Usar imagen de la SD", brls::Key::Y, [tid, gameName] {
        frame_localImages(tid, gameName);
        return true;
    });

    frame->registerAction("Buscar otro nombre", brls::Key::X, [tid, gameName, searchTerm] {
        brls::Swkbd::openForText([tid, gameName](std::string text) {
            if (!text.empty())
                frame_matches(tid, gameName, text);
        }, "Nombre para buscar en SteamGridDB", "", 64, searchTerm);
        return true;
    });

    brls::Application::pushView(frame);
}

void fillGameList(brls::List* list, const std::string& filter);

// Al vaciar la lista, borealis borra las vistas pero no limpia a quién tenía el
// foco, y eso deja un puntero colgando que revienta en la siguiente pulsación.
// Por eso se suelta el foco ANTES de tocar nada y se devuelve después.
void rebuildGameList(brls::List* list, const std::string& filter)
{
    brls::Application::giveFocus(nullptr);
    list->clear(true);
    fillGameList(list, filter);
    brls::Application::giveFocus(list);
}

void fillGameList(brls::List* list, const std::string& filter)
{
    nlohmann::json& games = installedGames();
    std::string needle    = toLower(filter);
    int shown             = 0;

    // Orden elegido en Ajustes: por nombre o por lo último que jugaste.
    nlohmann::json ordered = games;
    if (loadConfig().value("sort_recent", false))
    {
        std::stable_sort(ordered.begin(), ordered.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            u64 ta = a.contains("last_played") ? a["last_played"].get<u64>() : UINT64_MAX;
            u64 tb = b.contains("last_played") ? b["last_played"].get<u64>() : UINT64_MAX;
            return ta < tb;
        });
    }

    for (auto it : ordered)
    {
        std::string name = it["name"].get<std::string>();
        std::string tid  = it["tid"].get<std::string>();

        if (!needle.empty() && toLower(name).find(needle) == std::string::npos)
            continue;

        bool archivado        = it.value("archivado", false);
        brls::ListItem* litem = new brls::ListItem(name, "", archivado ? "archivado: el ícono queda listo para cuando lo reinstales" : "");
        if (hasCustomIcon(tid))
            litem->setValue("ícono propio");
        else if (archivado)
            litem->setValue("archivado", true);

        litem->getClickEvent()->subscribe([tid, name](brls::View* view) {
            frame_matches(tid, name, name);
        });

        litem->registerAction("Ícono original adaptado al tema", brls::Key::PLUS, [tid, name, litem] {
            if (useOriginalIconAdapted(tid, name))
                litem->setValue("ícono propio");
            return true;
        });

        litem->registerAction("Restaurar ícono original", brls::Key::X, [tid, name, litem] {
            if (restoreOriginalIcon(tid, name))
                litem->setValue("");
            return true;
        });

        list->addView(litem);
        shown++;
    }

    if (shown == 0)
    {
        brls::ListItem* empty = new brls::ListItem(
            filter.empty() ? "No se encontraron juegos instalados" : "Ningún juego coincide con: " + filter,
            filter.empty() ? "" : "Pulsa Y para buscar otra cosa");
        list->addView(empty);
    }
}

// La vista previa por ruta falla con los archivos de los perfiles (se ve una
// línea en vez de la imagen), así que se carga el archivo a memoria y se pasa
// el buffer: borealis lo copia por dentro, de modo que liberarlo es seguro.
bool setSidebarImage(brls::ThumbnailSidebar* sidebar, const std::string& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return false;

    auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0)
        return false;

    FILE* f = fopen(path.c_str(), "rb");
    if (f == NULL)
        return false;

    unsigned char* buffer = new unsigned char[size];
    size_t leidos = fread(buffer, 1, size, f);
    fclose(f);

    bool ok = leidos == size;
    if (ok)
        sidebar->setThumbnail(buffer, size);

    delete[] buffer;
    return ok;
}

// ------------------------------------------------------------- perfiles ---
// Un perfil es una copia de todos los íconos puestos, guardada en
// /iconos/perfiles/<nombre>/<tid>/. Sirve para tener un juego de íconos por
// tema: verticales para uno, cuadrados para otro, y cambiar de golpe.

std::vector<std::string> listProfiles()
{
    std::vector<std::string> out;
    std::error_code ec;
    std::filesystem::create_directories(PROFILES_DIR, ec);
    for (const auto& entry : std::filesystem::directory_iterator(PROFILES_DIR, ec))
    {
        if (ec) break;
        if (entry.is_directory(ec))
            out.push_back(entry.path().filename().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Recorre las carpetas de contents y se queda con las que tienen íconos
// nuestros. Nunca toca nada más de esas carpetas: ahí también viven los mods.
std::vector<std::string> titlesWithIcons()
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("sdmc:/atmosphere/contents/", ec))
    {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;
        std::string tid = entry.path().filename().string();
        if (hasCustomIcon(tid))
            out.push_back(tid);
    }
    return out;
}

// std::filesystem::copy_file no copia nada en esta consola con rutas sdmc:, y
// como devuelve el fallo por error_code en vez de lanzar, los perfiles se
// guardaban vacios sin que nada se quejara. Se copia a mano, que es lo que usa
// el resto de la app y funciona.
bool copyFileRaw(const std::string& origen, const std::string& destino)
{
    FILE* in = fopen(origen.c_str(), "rb");
    if (in == NULL)
        return false;

    FILE* out = fopen(destino.c_str(), "wb");
    if (out == NULL)
    {
        fclose(in);
        return false;
    }

    char buffer[32 * 1024];
    size_t leidos;
    bool ok = true;
    while ((leidos = fread(buffer, 1, sizeof(buffer), in)) > 0)
    {
        if (fwrite(buffer, 1, leidos, out) != leidos)
        {
            ok = false;
            break;
        }
    }
    if (ferror(in))
        ok = false;

    fclose(in);
    fclose(out);

    if (!ok)
        std::remove(destino.c_str());

    return ok;
}

int saveProfile(const std::string& nombre)
{
    std::error_code ec;
    std::string dest = PROFILES_DIR + nombre + "/";
    std::filesystem::create_directories(dest, ec);

    int copiados = 0;
    for (const std::string& tid : titlesWithIcons())
    {
        std::filesystem::create_directories(dest + tid, ec);

        // Antes se contaba el titulo pasara lo que pasara y los errores de copia
        // se tragaban, asi que un perfil podia quedarse con carpetas vacias y aun
        // asi decir que habia guardado N juegos. Luego, al abrirlo, no habia
        // íconos que mostrar y no habia forma de saber por que.
        int archivosCopiados = 0;
        for (const std::string& archivo : { std::string("icon.jpg"), std::string("icon174.jpg") })
        {
            std::string origen = "sdmc:/atmosphere/contents/" + tid + "/" + archivo;
            std::error_code ecCopia;
            if (!std::filesystem::exists(origen, ecCopia))
                continue;

            if (!copyFileRaw(origen, dest + tid + "/" + archivo))
                continue;

            // Que la copia diga que fue bien no basta: se comprueba que el
            // destino exista y no este vacio.
            auto tam = std::filesystem::file_size(dest + tid + "/" + archivo, ecCopia);
            if (!ecCopia && tam > 0)
                archivosCopiados++;
        }

        if (archivosCopiados > 0)
            copiados++;
        else
            std::filesystem::remove_all(dest + tid, ec);  // sin íconos, no dejar la carpeta
    }

    // El tema con el que se armaron: al aplicar el perfil se vuelve a él.
    const nlohmann::json c = loadConfig();
    nlohmann::json info;
    info["fit_mode"]   = c.value("fit_mode", 0);
    info["small_mode"] = c.value("small_mode", 1);
    std::ofstream o(dest + "perfil.json", std::ios::trunc);
    o << info.dump(1);
    return copiados;
}

// «Tema vertical», «Cuadrado» o vacío (perfiles de antes de la 3.0).
std::string profileTheme(const std::string& nombre)
{
    try
    {
        std::ifstream f(PROFILES_DIR + nombre + "/perfil.json");
        if (!f) return "";
        nlohmann::json j;
        f >> j;
        const int fit = j.value("fit_mode", 0), chico = j.value("small_mode", 1);
        if (fit == 3 && chico == 0) return "Tema con HOME vertical";
        if (fit == 3 && chico == 2) return "Tema vertical en todo";
        if (fit == 0 && chico == 1) return "Tema normal (cuadrado)";
        return "Combinación propia";
    }
    catch (const std::exception&)
    {
        return "";
    }
}

int applyProfile(const std::string& nombre)
{
    std::error_code ec;
    std::string origen = PROFILES_DIR + nombre + "/";
    int puestos = 0;

    // Un perfil es un juego completo de íconos para su tema: primero se quitan
    // los que hay (pueden ser del otro tema) y después se ponen los suyos. El
    // HOME se recarga al final, con cada juego ya en su estado nuevo.
    std::set<std::string> quitados;
    for (const std::string& tid : titlesWithIcons()) quitados.insert(tid);
    removeAllIcons(false);

    // Su tema, para que lo que pongas después salga igual.
    try
    {
        std::ifstream f(origen + "perfil.json");
        if (f)
        {
            nlohmann::json j;
            f >> j;
            nlohmann::json c   = loadConfig();
            c["fit_mode"]      = j.value("fit_mode", c.value("fit_mode", 0));
            c["small_mode"]    = j.value("small_mode", c.value("small_mode", 1));
            saveConfig(c);
        }
    }
    catch (const std::exception&)
    {
    }

    for (const auto& entry : std::filesystem::directory_iterator(origen, ec))
    {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;

        std::string tid  = entry.path().filename().string();
        std::string dest = "sdmc:/atmosphere/contents/" + tid + "/";
        std::filesystem::create_directories(dest, ec);

        bool alguno = false;
        for (const std::string& archivo : { std::string("icon.jpg"), std::string("icon174.jpg") })
        {
            std::string f = entry.path().string() + "/" + archivo;
            if (std::filesystem::exists(f, ec))
            {
                copyFileRaw(f, dest + archivo);
                alguno = true;
            }
        }
        if (alguno)
        {
            invalidateControlCache(tid);
            quitados.erase(tid);
            puestos++;
        }
    }
    for (const std::string& tid : quitados) invalidateControlCache(tid);
    return puestos;
}

// Deja la consola con los íconos de siempre. Sólo borra los dos archivos que
// escribimos nosotros; lo demás de cada carpeta se queda intacto.
int removeAllIcons(bool refrescarHome)
{
    int quitados = 0;
    for (const std::string& tid : titlesWithIcons())
    {
        std::remove(iconPathFor(tid).c_str());
        std::remove(smallIconPathFor(tid).c_str());
        invalidateControlCache(tid, refrescarHome);
        quitados++;
    }
    return quitados;
}

// Abre un perfil y muestra sus íconos uno por uno, con vista previa. Desde aquí
// se puede aplicar el perfil entero o sólo el de un juego.
void frame_profile(const std::string& nombre)
{
    brls::ThumbnailFrame* frame = new brls::ThumbnailFrame();
    frame->setTitle(nombre);
    frame->setIcon(BOREALIS_ASSET("icon/borealis.jpg"));
    frame->getSidebar()->setThumbnail(BOREALIS_ASSET("icon/borealis.jpg"));
    frame->getSidebar()->setTitle(nombre);
    frame->getSidebar()->setSubtitle("Elige un juego");
    frame->getSidebar()->getButton()->setLabel("Aplicar sólo este");

    brls::List* list = new brls::List();

    brls::ListItem* todos = new brls::ListItem("Aplicar todo el perfil");
    todos->getClickEvent()->subscribe([nombre](brls::View* view) {
        const std::string tema = profileTheme(nombre);
        brls::Dialog* dialog = new DialogoCentrado("¿Aplicar el perfil «" + nombre + "»?\n\n" + (tema.empty() ? "" : tema + ".\n") + "Se quitan tus íconos actuales y se ponen los del perfil, con su tema.");
        dialog->addButton("Aplicar", [dialog, nombre](brls::View* v) {
            dialog->close([nombre] {
                enSegundoPlano("Aplicar perfil", nombre, [nombre](std::function<void(int, int)>) {
                    return "Perfil aplicado a " + std::to_string(applyProfile(nombre)) + " juegos, con su tema.";
                }, true);
            });
        });
        dialog->addButton("Cancelar", [dialog](brls::View* v) { dialog->close(); });
        dialog->setCancelable(true);
        dialog->open();
    });
    list->addView(todos);

    // Nombre del juego a partir del tid, si sigue instalado.
    nlohmann::json& games = installedGames();

    std::error_code ec;
    int total = 0;
    for (const auto& entry : std::filesystem::directory_iterator(PROFILES_DIR + nombre, ec))
    {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;

        std::string tid   = entry.path().filename().string();
        std::string big   = entry.path().string() + "/icon.jpg";
        std::string small = entry.path().string() + "/icon174.jpg";
        if (!std::filesystem::exists(big, ec) && !std::filesystem::exists(small, ec))
            continue;

        std::string etiqueta = tid;
        for (auto g : games)
            if (g["tid"].get<std::string>() == tid)
            {
                etiqueta = g["name"].get<std::string>();
                break;
            }

        std::string detalle = std::filesystem::exists(small, ec) ? "grande y chico" : "sólo el grande";
        brls::ListItem* item = new brls::ListItem(etiqueta, "", detalle);

        auto aplicarUno = [tid, big, small, etiqueta]() {
            std::error_code ec2;
            std::string dest = "sdmc:/atmosphere/contents/" + tid + "/";
            std::filesystem::create_directories(dest, ec2);
            if (std::filesystem::exists(big, ec2))
                copyFileRaw(big, dest + "icon.jpg");
            if (std::filesystem::exists(small, ec2))
                copyFileRaw(small, dest + "icon174.jpg");
            invalidateControlCache(tid);
            avisarCambio(tid, "Aplicado a " + etiqueta);
        };

        item->getClickEvent()->subscribe([aplicarUno](brls::View* view) { aplicarUno(); });

        item->getFocusEvent()->subscribe([=](brls::View* view) {
            frame->getSidebar()->getButton()->getClickEvent()->unsubscribeAll();
            if (!setSidebarImage(frame->getSidebar(), big))
                if (!setSidebarImage(frame->getSidebar(), small))
                    frame->getSidebar()->setThumbnail(BOREALIS_ASSET("icon/borealis.jpg"));
            frame->getSidebar()->setSubtitle(etiqueta);
            frame->getSidebar()->getButton()->getClickEvent()->subscribe([aplicarUno](brls::View* v) { aplicarUno(); });
        });

        item->registerAction("Quitar del perfil", brls::Key::X, [tid, nombre, item] {
            std::error_code ec2;
            std::filesystem::remove_all(PROFILES_DIR + nombre + "/" + tid, ec2);
            item->setValue("quitado");
            avisar("Quitado del perfil");
            return true;
        });

        list->addView(item);
        total++;
    }

    if (total == 0)
        list->addView(new brls::ListItem("Este perfil está vacío"));

    frame->setContentView(list);
    brls::Application::pushView(frame);
}

void fillProfilesTab(brls::List* list);
void addProfileRows(brls::List* list);

// Los dos primeros elementos (guardar y dejar originales) son fijos y NO se
// tocan: son los que disparan este refresco, y borrar la vista que está
// ejecutando su propio manejador revienta la app. Sólo se rehacen las filas de
// perfiles, que van de la tercera en adelante.
const int PROFILE_FIXED_ITEMS = 2;

void rebuildProfilesTab(brls::List* list)
{
    brls::Application::giveFocus(nullptr);
    while ((int)list->getViewsCount() > PROFILE_FIXED_ITEMS)
        list->removeView(PROFILE_FIXED_ITEMS, true);
    addProfileRows(list);
    brls::Application::giveFocus(list);
}

void addProfileRows(brls::List* list);

void fillProfilesTab(brls::List* list)
{
    brls::ListItem* guardar = new brls::ListItem("Guardar los íconos actuales", "", "Crea un perfil con todo lo que tienes puesto ahora");
    guardar->getClickEvent()->subscribe([list](brls::View* view) {
        // Un nombre sugerido según el tema; se puede cambiar.
        const nlohmann::json c = loadConfig();
        const std::string base = c.value("fit_mode", 0) != 3 ? "Cuadrado" : (c.value("small_mode", 1) == 2 ? "Vertical todo" : "Vertical");
        int n = 1;
        std::error_code ec;
        while (std::filesystem::exists(PROFILES_DIR + base + " " + std::to_string(n), ec)) n++;
        brls::Swkbd::openForText([list](std::string nombre) {
            nombre.erase(std::remove_if(nombre.begin(), nombre.end(), [](char ch) { return ch == '/' || ch == '\\' || ch == ':'; }), nombre.end());
            if (nombre.empty())
                return;
            int n = saveProfile(nombre);
            avisar("Perfil \"" + nombre + "\": " + std::to_string(n) + " juegos guardados, con su tema");
            rebuildProfilesTab(list);
        }, "Nombre del perfil", "Guarda tus íconos y el tema", 32, base + " " + std::to_string(n));
    });
    list->addView(guardar);

    brls::ListItem* quitar = new brls::ListItem("Dejar todos los íconos originales", "", "Quita los tuyos de todos los juegos (los perfiles no se tocan)");
    quitar->getClickEvent()->subscribe([list](brls::View* view) {
        brls::Dialog* dialog = new DialogoCentrado("Se van a quitar tus íconos de todos los juegos.\n\nLos perfiles guardados no se tocan, así que puedes volver cuando quieras.\n¿Seguimos?");
        dialog->addButton("Quitar", [dialog](brls::View* v) {
            dialog->close([] {
                enSegundoPlano("Íconos originales", "Se quitan los tuyos de todos los juegos.", [](std::function<void(int, int)>) {
                    return "Quitados los íconos de " + std::to_string(removeAllIcons()) + " juegos.";
                }, true);
            });
        });
        dialog->addButton("Cancelar", [dialog](brls::View* v) { dialog->close(); });
        dialog->setCancelable(true);
        dialog->open();
    });
    list->addView(quitar);

    addProfileRows(list);
}

void addProfileRows(brls::List* list)
{
    std::vector<std::string> perfiles = listProfiles();
    if (perfiles.empty())
    {
        list->addView(new brls::ListItem("Todavía no hay perfiles", "", "Se guardan en /iconos/perfiles/ de la SD"));
        return;
    }

    // Agrupados por tema: primero los cuadrados, después los de HOME vertical.
    std::stable_sort(perfiles.begin(), perfiles.end(), [](const std::string& a, const std::string& b) {
        auto orden = [](const std::string& t) { return t == "Tema normal (cuadrado)" ? 0 : t == "Tema con HOME vertical" ? 1 : t == "Tema vertical en todo" ? 2 : 3; };
        return orden(profileTheme(a)) < orden(profileTheme(b));
    });
    std::string grupo = "-";
    for (const std::string& nombre : perfiles)
    {
        const std::string temaGrupo = profileTheme(nombre);
        if (temaGrupo != grupo)
        {
            grupo = temaGrupo;
            list->addView(new brls::Header(grupo.empty() ? "Sin tema guardado (de antes)" : grupo));
        }
        std::error_code ec;
        int juegos = 0;
        for (const auto& e : std::filesystem::directory_iterator(PROFILES_DIR + nombre, ec))
            if (!ec && e.is_directory(ec)) juegos++;

        const std::string tema = profileTheme(nombre);
        auto actual = std::make_shared<std::string>(nombre);  // cambia si se renombra
        brls::ListItem* item = new brls::ListItem(nombre, "", std::to_string(juegos) + (juegos == 1 ? " juego" : " juegos") + " · A verlo · Y renombrar · X borrar");
        item->getClickEvent()->subscribe([actual](brls::View* view) {
            frame_profile(*actual);
        });
        item->registerAction("Renombrar", brls::Key::Y, [actual, item] {
            brls::Swkbd::openForText([actual, item](std::string nuevo) {
                nuevo.erase(std::remove_if(nuevo.begin(), nuevo.end(), [](char ch) { return ch == '/' || ch == '\\' || ch == ':'; }), nuevo.end());
                if (nuevo.empty() || nuevo == *actual) return;
                std::error_code ec;
                if (std::filesystem::exists(PROFILES_DIR + nuevo, ec))
                {
                    avisar("Ya hay un perfil con ese nombre");
                    return;
                }
                if (std::rename((PROFILES_DIR + *actual).c_str(), (PROFILES_DIR + nuevo).c_str()) != 0)
                {
                    avisar("No se pudo renombrar");
                    return;
                }
                *actual = nuevo;
                item->setLabel(nuevo);
                avisar("Perfil renombrado");
            }, "Nuevo nombre del perfil", "", 32, *actual);
            return true;
        });
        item->registerAction("Borrar el perfil", brls::Key::X, [actual, item] {
            std::error_code ec;
            std::filesystem::remove_all(PROFILES_DIR + *actual, ec);
            item->setValue("borrado");
            avisar("Perfil borrado: " + *actual);
            return true;
        });
        list->addView(item);
    }
}


// ------------------------------------------------------ rediseño (3.0) ----

std::vector<unsigned char> previewRGBA(const std::string& imagePath, int fitMode, int side)
{
    std::vector<unsigned char> rgba;
    int w, h, c;
    unsigned char* img = stbi_load(imagePath.c_str(), &w, &h, &c, 3);
    if (img == NULL)
        return rgba;
    unsigned char* rgb = renderPixels(img, w, h, fitMode, side);
    stbi_image_free(img);
    if (rgb == NULL)
        return rgba;
    rgba.resize((size_t)side * side * 4);
    for (size_t i = 0; i < (size_t)side * side; i++)
    {
        rgba[i * 4 + 0] = rgb[i * 3 + 0];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }
    free(rgb);
    return rgba;
}

bool imageSize(const std::string& path, int& w, int& h)
{
    int c = 0;
    return stbi_info(path.c_str(), &w, &h, &c) != 0;
}

std::vector<unsigned char> readFileBytes(const std::string& path)
{
    std::vector<unsigned char> out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return out;
    unsigned char buf[16 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.insert(out.end(), buf, buf + n);
    fclose(f);
    return out;
}

// Lo que el HOME muestra hoy: el nuestro si hay, si no el del juego (de la
// caché, que no lee la tarjeta ni el USB y es rápido).
std::vector<unsigned char> currentIconJpeg(const std::string& tid)
{
    if (existe(iconPathFor(tid)))
    {
        auto propio = readFileBytes(iconPathFor(tid));
        if (!propio.empty())
            return propio;
    }
    return originalIconJpeg(tid);
}

// El ícono que trae el juego (el que se ve en Todas las apps con un tema vertical).
std::vector<unsigned char> originalIconJpeg(const std::string& tid)
{
    std::vector<unsigned char> out;
    NsApplicationControlData* cd = (NsApplicationControlData*)malloc(sizeof(NsApplicationControlData));
    if (!cd)
        return out;
    size_t tam = 0;
    u64 id     = strtoull(tid.c_str(), NULL, 16);
    if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_CacheOnly, id, cd, sizeof(NsApplicationControlData), &tam)) && tam > sizeof(cd->nacp))
        out.assign(cd->icon, cd->icon + (tam - sizeof(cd->nacp)));
    free(cd);
    return out;
}

EstadoIcono iconState(const std::string& tid)
{
    if (!hasCustomIcon(tid))
        return EstadoIcono::Original;
    std::error_code ec;
    auto grande = std::filesystem::file_size(iconPathFor(tid), ec);
    if (!ec && grande > LIMITE_GRANDE)
        return EstadoIcono::NoEntra;
    auto chico = std::filesystem::file_size(smallIconPathFor(tid), ec);
    if (!ec && chico > LIMITE_CHICO)
        return EstadoIcono::NoEntra;
    return EstadoIcono::Propio;
}

// ------------------------------------------------- nombres (sys-ticon) ----
// sys-ticon lee atmosphere/contents/<tid>/config.ini, sección [override_nacp]
// al principio del archivo: name y author van siempre juntos.

/* Nexus Bacán (juegos desde el USB): con sys-icon a la vez, la consola se queda
   en el logo al encender (probado en la Lite el 24-nov-2026). */
bool nexusInstalado()
{
    /* Nexus registra el servicio «ams:usb» mientras corre. Los archivos que deja en
       la SD (su registro, su kip) se quedan aunque se pase a un pack sin Nexus, y
       daban falsos avisos. Se pregunta al sistema, sin conectarse. */
    // Comando 65100 de sm (AtmosphereHasService); desde FW 12 sm habla TIPC.
    const SmServiceName nombre = smEncodeName("ams:usb");
    u8 hay                     = 0;
    const Result rc            = hosversionAtLeast(12, 0, 0) ? tipcDispatchInOut(smGetServiceSessionTipc(), 65100, nombre, hay)
                                                             : serviceDispatchInOut(smGetServiceSession(), 65100, nombre, hay);
    return R_SUCCEEDED(rc) && hay != 0;
}

bool sysIconActivo()
{
    return existe("sdmc:/atmosphere/contents/00FF69636F6EFF00/flags/boot2.flag");
}

bool apagarSysIcon()
{
    return std::remove("sdmc:/atmosphere/contents/00FF69636F6EFF00/flags/boot2.flag") == 0;
}

// Los nombres los aplica sys-icon (config.ini de cada juego).
bool namesSysmoduleInstalled()
{
    return existe(SYSICON_PATH);
}


static std::string nameConfigPath(const std::string& tid)
{
    return "sdmc:/atmosphere/contents/" + tid + "/config.ini";
}

// El archivo partido en la sección nuestra y el resto, que se conserva tal cual.
static void splitConfig(const std::string& tid, std::map<std::string, std::string>& nuestra, std::string& resto)
{
    std::ifstream f(nameConfigPath(tid));
    std::string linea;
    bool dentro = false;
    while (std::getline(f, linea))
    {
        std::string l = linea;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty() && l[0] == '[')
            dentro = l == "[override_nacp]";
        if (dentro)
        {
            size_t eq = l.find('=');
            if (eq != std::string::npos)
                nuestra[l.substr(0, eq)] = l.substr(eq + 1);
        }
        else
            resto += linea + "\n";
    }
}

std::string customName(const std::string& tid)
{
    std::map<std::string, std::string> nuestra;
    std::string resto;
    splitConfig(tid, nuestra, resto);
    return nuestra.count("name") ? nuestra["name"] : "";
}

bool setCustomName(const std::string& tid, const std::string& nombre, const std::string& autor)
{
    if (nombre.empty() || nombre.size() > 511 || autor.size() > 255)
        return false;
    std::map<std::string, std::string> nuestra;
    std::string resto;
    splitConfig(tid, nuestra, resto);
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/atmosphere/contents/" + tid, ec);
    std::ofstream o(nameConfigPath(tid), std::ios::trunc | std::ios::binary);
    if (!o)
        return false;
    // Nuestra sección primero: así lo pide sys-ticon. UTF-8 sin BOM.
    o << "[override_nacp]\nname=" << nombre << "\nauthor=" << (autor.empty() ? " " : autor) << "\n";
    if (nuestra.count("display_version"))
        o << "display_version=" << nuestra["display_version"] << "\n";
    o << resto;
    return o.good();
}

bool removeCustomName(const std::string& tid)
{
    std::map<std::string, std::string> nuestra;
    std::string resto;
    splitConfig(tid, nuestra, resto);
    if (!nuestra.count("name"))
        return false;
    bool vacio = resto.find_first_not_of(" \r\n\t") == std::string::npos;
    if (vacio && !nuestra.count("display_version"))
        return std::remove(nameConfigPath(tid).c_str()) == 0;
    std::ofstream o(nameConfigPath(tid), std::ios::trunc | std::ios::binary);
    if (nuestra.count("display_version"))
        o << "[override_nacp]\ndisplay_version=" << nuestra["display_version"] << "\n";
    o << resto;
    return o.good();
}

std::vector<unsigned char> loadRGB(const std::string& path, int& w, int& h)
{
    std::vector<unsigned char> out;
    int c = 0;
    unsigned char* img = stbi_load(path.c_str(), &w, &h, &c, 3);
    if (img == NULL)
        return out;
    out.assign(img, img + (size_t)w * h * 3);
    stbi_image_free(img);
    return out;
}

std::vector<unsigned char> renderRGBA(const unsigned char* rgb, int w, int h, int fitMode, int side)
{
    std::vector<unsigned char> rgba;
    unsigned char* px = renderPixels(const_cast<unsigned char*>(rgb), w, h, fitMode, side);
    if (px == NULL)
        return rgba;
    rgba.resize((size_t)side * side * 4);
    for (size_t i = 0; i < (size_t)side * side; i++)
    {
        rgba[i * 4 + 0] = px[i * 3 + 0];
        rgba[i * 4 + 1] = px[i * 3 + 1];
        rgba[i * 4 + 2] = px[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }
    free(px);
    return rgba;
}

bool writePngRGB(const std::string& path, int w, int h, const unsigned char* rgb)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    return stbi_write_png(path.c_str(), w, h, 3, rgb, w * 3) != 0;
}

