/*
    IconGrabber — versión OQB
    Fork de Slluxx/IconGrabber (GPLv3). El recorrido original estaba partido en
    tres pantallas: en una se elegía el tamaño, en otra se buscaba el juego, y
    para aplicar el icono había que ir a una tercera y volver a buscar el juego.
    Acá todo pasa en un solo camino: se elige el juego instalado, se ven sus
    iconos y se aplica ahí mismo.
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
#include <nlohmann/json.hpp>
#include <string_view>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image/stb_image_resize.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image/stb_image_write.h"

namespace i18n = brls::i18n;
using namespace i18n::literals;

std::string configPath = "sdmc:/config/icongrabber/config.json";

// El sysmodule que aplica los iconos. sys-icon es el que sigue mantenido;
// sys-tweak es el viejo y se acepta por compatibilidad.
const std::string USER_ICONS_DIR = "sdmc:/iconos/";
const std::string PROFILES_DIR   = "sdmc:/iconos/perfiles/";

const std::string SYSICON_PATH  = "sdmc:/atmosphere/contents/00FF69636F6EFF00/exefs.nsp";
const std::string SYSTWEAK_PATH = "sdmc:/atmosphere/contents/00FF747765616BFF/exefs.nsp";

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

// ---------------------------------------------------------------- config ---

nlohmann::json loadConfig()
{
    nlohmann::json config;
    if (!std::filesystem::exists(configPath))
    {
        std::filesystem::create_directories("sdmc:/config/icongrabber/");
        config["api_token"]     = "";
        config["style_id"]      = 0;
        config["resolution_id"] = 0;
        std::ofstream o(configPath);
        o << config.dump(4);
        o.close();
        return config;
    }

    try
    {
        std::ifstream i(configPath);
        i >> config;
        i.close();
    }
    catch (const std::exception& e)
    {
        brls::Logger::error("config ilegible, se usa uno nuevo");
        config["api_token"]     = "";
        config["style_id"]      = 0;
        config["resolution_id"] = 0;
    }

    if (!config.contains("api_token")) config["api_token"] = "";
    if (!config.contains("style_id")) config["style_id"] = 0;
    if (!config.contains("resolution_id")) config["resolution_id"] = 0;
    return config;
}

void saveConfig(nlohmann::json config)
{
    std::filesystem::create_directories("sdmc:/config/icongrabber/");
    std::ofstream o(configPath);
    o << config.dump(4);
    o.close();
}

// ----------------------------------------------------------------- util ----

std::string formatApplicationId(u64 ApplicationId)
{
    std::stringstream strm;
    strm << std::uppercase << std::setfill('0') << std::setw(16) << std::hex << ApplicationId;
    return strm.str();
}

// sys-icon lee dos archivos por título: el grande (256) que se ve en la fila
// principal y el chico (174) que usa la pantalla de todas las aplicaciones.
// Escribir sólo el primero dejaba el icono viejo en la lista completa.
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
    return std::filesystem::exists(iconPathFor(tid)) || std::filesystem::exists(smallIconPathFor(tid));
}

bool sysmoduleInstalled()
{
    return std::filesystem::exists(SYSICON_PATH) || std::filesystem::exists(SYSTWEAK_PATH);
}

std::string sysmoduleName()
{
    if (std::filesystem::exists(SYSICON_PATH)) return "sys-icon";
    if (std::filesystem::exists(SYSTWEAK_PATH)) return "sys-tweak";
    return "";
}

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Escribe el icono donde lo lee el sysmodule: 256x256 jpg en la carpeta del
// título. El original pasaba los canales de la imagen de origen a stbi_write_jpg
// aunque el buffer redimensionado siempre tiene 3, así que una imagen con
// transparencia salía corrupta.
// El menú HOME siempre muestra un cuadrado de 256x256, así que una imagen
// vertical hay que encajarla de alguna forma. Tres maneras, elegibles en
// Ajustes, porque cuál conviene depende de la imagen:
//   0 recortar: toma el cuadrado del centro. No deforma, pero corta arriba y abajo.
//   1 ajustar:  entra completa, con franjas del color del borde a los lados.
//   2 estirar:  la aplasta hasta el cuadrado (lo que hacía antes).
//   3 tema vertical: la prepara para temas que estiran el icono a 2:3.
void invalidateControlCache(const std::string& tid);
bool controlDataAvailable(u64 appId);

bool renderIcon(unsigned char* img, int width, int height, int fitMode, int SIDE, const std::string& outPath)
{
    unsigned char* out = (unsigned char*)malloc((size_t)SIDE * SIDE * 3);
    if (out == NULL)
        return false;

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

    // El icono viaja dentro de los datos del juego, en un hueco de 0x20000
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
        // los dos el icono vale para cualquiera de los dos sysmodules, y el
        // usuario no tiene que saber cual tiene instalado.
        const size_t LIMITE = (SIDE >= 256) ? 102400 : 60000;
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

            brls::Logger::info("icono de " + std::to_string(size) + " bytes, se reintenta con menos calidad");
        }
    }

    free(out);
    return ok;
}

bool overwriteIcon(const std::string& tid, const std::string& imagePath)
{
    int width, height, channels;
    unsigned char* img = stbi_load(imagePath.c_str(), &width, &height, &channels, 3);
    if (img == NULL || width <= 0 || height <= 0)
    {
        brls::Application::notify("No se pudo leer la imagen");
        return false;
    }

    int fitMode = loadConfig().value("fit_mode", 3);

    std::error_code ec;
    std::filesystem::create_directories("sdmc:/atmosphere/contents/" + tid, ec);

    // Se borran antes de escribir: si una de las dos escrituras fallara, es
    // preferible quedarse sin icono propio que con los dos tamaños mezclados,
    // uno nuevo y otro de la vez anterior.
    std::remove(iconPathFor(tid).c_str());
    std::remove(smallIconPathFor(tid).c_str());

    // Y se invalida la cache ya, con los archivos fuera. Sin este paso el menu
    // pasa de un icono propio a otro sin ver nunca el estado intermedio, y se
    // queda mostrando el anterior: habia que borrar el icono, volver al
    // original, reiniciar, y recien entonces elegir otro. Invalidando aqui el
    // sistema refresca primero al original y despues al nuevo, que es lo mismo
    // que hacia ese rodeo manual pero sin reiniciar.
    invalidateControlCache(tid);

    // Los dos archivos se muestran distinto, así que se preparan distinto:
    //  - el grande lo estira el tema vertical, así que va aplastado a propósito;
    //  - el chico lo dibuja el sistema en marco cuadrado (la lista completa y la
    //    ventanita al cambiar de app), así que va recortado, nunca aplastado.
    // Con un solo tratamiento para ambos, uno de los dos salía siempre mal.
    bool big = renderIcon(img, width, height, fitMode, 256, iconPathFor(tid));

    // El icono chico manda en la lista completa y en la ventanita al cambiar de
    // app. Cómo prepararlo depende del tema que uses ahí:
    //   0 no tocarlo, se queda el del juego;
    //   1 recortado, para temas que lo muestran cuadrado;
    //   2 igual que el grande, para temas que también lo muestran a lo alto.
    int smallMode = loadConfig().value("small_mode", 0);
    if (smallMode > 0)
    {
        int mode = (smallMode == 2) ? fitMode : 0;
        if (!renderIcon(img, width, height, mode, 174, smallIconPathFor(tid)))
            brls::Application::notify("El icono chico no se pudo escribir");
    }

    stbi_image_free(img);

    // Si el juego no está instalado, el menú sigue sirviendo su icono desde la
    // caché y no podemos refrescarla sin borrarle el nombre para siempre. El
    // archivo queda escrito y servirá cuando se reinstale, pero conviene
    // decirlo en vez de dejar al usuario probando una y otra vez.
    if (!controlDataAvailable(strtoull(tid.c_str(), NULL, 16)))
    {
        brls::Application::notify("Este juego no está instalado.\nEl icono queda guardado, pero el menú\nseguirá mostrando el actual hasta reinstalarlo.");
        return big;
    }

    invalidateControlCache(tid);
    return big;
}

// ------------------------------------------------------------ juegos SD ----

static nlohmann::json g_games   = nlohmann::json::array();
static bool g_gamesLoaded       = false;

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
        // icono preparado para cuando se vuelvan a instalar.
        bool archivado = false;
        if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, tid, controlData, sizeof(NsApplicationControlData), &controlSize)))
        {
            if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_CacheOnly, tid, controlData, sizeof(NsApplicationControlData), &controlSize)))
                continue;
            archivado = true;
        }
        if (R_FAILED(nacpGetLanguageEntry(&controlData->nacp, &langEntry)))
            continue;
        if (langEntry == NULL || !langEntry->name[0])
            continue;

        games.push_back({ { "tid", formatApplicationId(tid) }, { "name", std::string(langEntry->name) }, { "archivado", archivado } });
    }

    free(controlData);
    delete[] records;

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

    std::sort(games.begin(), games.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return toLower(a["name"].get<std::string>()) < toLower(b["name"].get<std::string>());
    });
    return games;
}

nlohmann::json& installedGames(bool refresh = false)
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
}

nlohmann::json httpGetJson(const std::string& url)
{
    nlohmann::json config = loadConfig();
    std::string authString = "Authorization: Bearer " + config["api_token"].get<std::string>();

    nlohmann::json fail;
    fail["success"] = false;

    curl_global_init(CURL_GLOBAL_ALL);
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        curl_global_cleanup();
        return fail;
    }

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
    curl_global_cleanup();

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
    if (std::filesystem::exists(outpath))
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
std::string jstr(const nlohmann::json& j, const char* key, const std::string& def = "")
{
    if (!j.contains(key) || j[key].is_null()) return def;
    if (j[key].is_string()) return j[key].get<std::string>();
    return j[key].dump();
}

int jint(const nlohmann::json& j, const char* key, int def = 0)
{
    if (!j.contains(key) || !j[key].is_number()) return def;
    return j[key].get<int>();
}

bool jbool(const nlohmann::json& j, const char* key, bool def = false)
{
    if (!j.contains(key) || !j[key].is_boolean()) return def;
    return j[key].get<bool>();
}

// Tamaños que se pueden elegir dentro de la pantalla de iconos. El primero no
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

// El menú HOME guarda en caché los datos de cada juego, icono incluido. Si no
// se invalida, puede seguir mostrando el anterior aunque el archivo de la SD ya
// esté cambiado: es el comando 404 de ns:am, el mismo que usa OQB-updater al
// crear accesos directos.
// OJO: invalidar la caché de un juego que NO está instalado le borra el nombre
// y el icono para siempre, porque su contenido ya no está en la consola y sólo
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

void invalidateControlCache(const std::string& tid)
{
    u64 appId = strtoull(tid.c_str(), NULL, 16);

    if (!controlDataAvailable(appId))
    {
        brls::Logger::info("archivado o sin contenido: no se toca su caché");
        return;
    }

    Service srv;
    if (R_FAILED(nsGetApplicationManagerInterface(&srv)))
        return;
    serviceDispatchIn(&srv, 404, appId);
    serviceClose(&srv);
}

// Saca el icono que trae el propio juego y lo deja en un archivo. Sirve para
// re-prepararlo: con un tema vertical, el icono original (cuadrado) sale
// estirado, así que hay que pasarlo por el mismo tratamiento que los demás.
bool extractOriginalIcon(const std::string& tid, std::string& outPath)
{
    u64 appId = strtoull(tid.c_str(), NULL, 16);

    // Si el juego no está instalado no hay icono original que sacar, y quitarle
    // los que tiene lo dejaría sin ninguno.
    if (!controlDataAvailable(appId))
        return false;

    // Con los iconos propios puestos, sys-icon devolvería esos mismos y
    // acabaríamos reaplicando lo que ya había. Se quitan antes de preguntar.
    std::remove(iconPathFor(tid).c_str());
    std::remove(smallIconPathFor(tid).c_str());
    invalidateControlCache(tid);

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

// Deja el icono del juego tal cual venía, pero adaptado al modo elegido. Con un
// tema vertical eso es lo que hace que un icono cuadrado deje de verse estirado.
bool useOriginalIconAdapted(const std::string& tid, const std::string& gameName)
{
    std::string original;
    if (!extractOriginalIcon(tid, original))
    {
        brls::Application::notify("No se pudo leer el icono original de " + gameName);
        return false;
    }
    if (!overwriteIcon(tid, original))
    {
        brls::Application::notify("No se pudo preparar el icono original");
        return false;
    }
    brls::Application::notify("Icono original adaptado: " + gameName + "\nSe ve al reiniciar");
    return true;
}

// El cambio no se ve hasta reiniciar, así que conviene ofrecerlo en el momento
// y no dejar que el usuario tenga que acordarse de ir a Ajustes.
void askReboot(const std::string& mensaje)
{
    brls::Dialog* dialog = new brls::Dialog(mensaje + "\n\nLos iconos se ven después de reiniciar.\n¿Reiniciar ahora?");
    dialog->addButton("Reiniciar", [](brls::View* v) {
        if (R_SUCCEEDED(spsmInitialize()))
        {
            spsmShutdown(true);
            spsmExit();
        }
        else
        {
            brls::Application::notify("No se pudo reiniciar");
        }
    });
    dialog->addButton("Después", [dialog](brls::View* v) { dialog->close(); });
    dialog->setCancelable(true);
    dialog->open();
}

// Devolver el icono original es borrar el archivo que escribimos: el sistema
// vuelve a usar el que trae el juego.
bool restoreOriginalIcon(const std::string& tid, const std::string& gameName)
{
    if (!hasCustomIcon(tid))
    {
        brls::Application::notify(gameName + " ya tiene su icono original");
        return false;
    }
    std::remove(iconPathFor(tid).c_str());
    std::remove(smallIconPathFor(tid).c_str());
    invalidateControlCache(tid);
    brls::Application::notify("Icono original restaurado: " + gameName + "\nSe ve después de reiniciar");
    return true;
}

bool isImageFile(const std::filesystem::path& p)
{
    std::string ext = toLower(p.extension().string());
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
}

// Ajuste del recorte con vista previa. Cada cambio vuelve a generar la imagen
// final y la muestra, así se elige mirando el resultado y no a ciegas.
void frame_cropAdjust(const std::string& tid, const std::string& gameName, const std::string& imagePath)
{
    static int previewToggle = 0;

    brls::ThumbnailFrame* frame = new brls::ThumbnailFrame();
    frame->setTitle("Ajustar el recorte");
    frame->setIcon(BOREALIS_ASSET("icon/borealis.jpg"));
    frame->getSidebar()->setTitle(gameName);
    frame->getSidebar()->getButton()->setLabel("Aplicar así");

    brls::List* list = new brls::List();

    brls::ListItem* posItem = new brls::ListItem("Posición del recorte", "", "Arriba conserva la cabeza; abajo, el título");

    // Genera la previa con el recorte actual. Se alterna el nombre del archivo
    // porque si se reescribe el mismo, la imagen que ya está en pantalla no se
    // refresca.
    auto refresh = [frame, posItem, imagePath]() {
        int cropY = loadConfig().value("crop_y", 50);
        posItem->setValue(std::to_string(cropY) + "%");

        int w, h, c;
        unsigned char* img = stbi_load(imagePath.c_str(), &w, &h, &c, 3);
        if (img == NULL)
            return;

        std::error_code ec;
        std::filesystem::create_directories("sdmc:/gameIcons/", ec);
        previewToggle       = 1 - previewToggle;
        std::string preview = std::string("sdmc:/gameIcons/preview") + std::to_string(previewToggle) + ".jpg";

        if (renderIcon(img, w, h, 0, 256, preview))
            frame->getSidebar()->setThumbnail(preview);
        stbi_image_free(img);
    };

    auto move = [refresh](int delta) {
        nlohmann::json c = loadConfig();
        int cropY        = std::max(0, std::min(100, c.value("crop_y", 50) + delta));
        c["crop_y"]      = cropY;
        saveConfig(c);
        refresh();
    };

    list->addView(posItem);

    brls::ListItem* up = new brls::ListItem("Subir el recorte", "", "Toma una parte más alta de la imagen");
    up->getClickEvent()->subscribe([move](brls::View* view) { move(-10); });
    list->addView(up);

    brls::ListItem* center = new brls::ListItem("Centrar");
    center->getClickEvent()->subscribe([refresh](brls::View* view) {
        nlohmann::json c = loadConfig();
        c["crop_y"]      = 50;
        saveConfig(c);
        refresh();
    });
    list->addView(center);

    brls::ListItem* down = new brls::ListItem("Bajar el recorte", "", "Toma una parte más baja de la imagen");
    down->getClickEvent()->subscribe([move](brls::View* view) { move(10); });
    list->addView(down);

    brls::ListItem* apply = new brls::ListItem("Aplicar este recorte");
    apply->getClickEvent()->subscribe([tid, gameName, imagePath](brls::View* view) {
        if (overwriteIcon(tid, imagePath))
            askReboot("Icono aplicado a " + gameName);
        else
            brls::Application::notify("No se pudo aplicar");
    });
    list->addView(apply);

    frame->getSidebar()->getButton()->getClickEvent()->subscribe([tid, gameName, imagePath](brls::View* view) {
        if (overwriteIcon(tid, imagePath))
            askReboot("Icono aplicado a " + gameName);
        else
            brls::Application::notify("No se pudo aplicar");
    });

    refresh();
    frame->setContentView(list);
    brls::Application::pushView(frame);
}

// Iconos propios: cualquier imagen que el usuario deje en /iconos/ de la SD, más
// lo que ya se haya descargado antes. Sirve cuando SteamGridDB no tiene nada.
void frame_localImages(const std::string& tid, const std::string& gameName)
{
    std::error_code ec;
    std::filesystem::create_directories(USER_ICONS_DIR, ec);

    brls::ThumbnailFrame* frame = new brls::ThumbnailFrame();
    frame->setTitle("Iconos guardados en la SD");
    frame->setIcon(BOREALIS_ASSET("icon/borealis.jpg"));
    frame->getSidebar()->setThumbnail(BOREALIS_ASSET("icon/borealis.jpg"));
    frame->getSidebar()->setTitle(gameName);
    frame->getSidebar()->setSubtitle("Elegí una imagen");
    frame->getSidebar()->getButton()->setLabel("Aplicar este icono");

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
                    askReboot("Icono aplicado a " + gameName);
                else
                    brls::Application::notify("No se pudo leer esa imagen");
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
                        brls::Application::notify("Icono aplicado a " + gameName + "\nPulsá B para volver");
                    else
                        brls::Application::notify("No se pudo aplicar esa imagen");
                });
            });
            list->addView(litem);
            added++;
        }
    }

    if (added == 0)
        list->addView(new brls::ListItem("No hay imágenes todavía", "Copiá archivos .jpg o .png a la carpeta /iconos/ de la SD"));

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
    frame->getSidebar()->setSubtitle("Elegí un icono");
    frame->getSidebar()->getButton()->setLabel("Aplicar este icono");

    nlohmann::json icons = requestIcons(gameId, styleId, resolutionId);

    brls::List* list = new brls::List();

    // Un ListItem normal, no un SelectListItem: el evento de ese último se
    // dispara mientras su propio desplegable se está cerrando, y abrir otra
    // pantalla desde ahí congelaba la app.
    brls::ListItem* sizeItem = new brls::ListItem("Tamaño", "", "Elegí con qué medida buscar");
    sizeItem->setValue(sizeChoices[sizeChoice]);
    sizeItem->getClickEvent()->subscribe([tid, gameName, gameId, sizeChoice](brls::View* view) {
        brls::AppletFrame* picker = new brls::AppletFrame(true, true);
        picker->setTitle("Tamaño del icono");
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
        brls::ListItem* restore = new brls::ListItem("Restaurar el icono original", "", "Quita el icono que le pusiste a este juego");
        restore->getClickEvent()->subscribe([tid, gameName, restore](brls::View* view) {
            if (restoreOriginalIcon(tid, gameName))
                restore->setValue("hecho");
        });
        list->addView(restore);
    }

    // Salida manual por si el menu se queda mostrando un icono viejo. Normalmente
    // no hace falta, porque aplicar ya invalida la cache; esta aqui para no tener
    // que recurrir al rodeo de quitar el icono, reiniciar y volver a ponerlo.
    brls::ListItem* refresh = new brls::ListItem("Refrescar la caché del menú", "", "Si el menú sigue mostrando el icono anterior");
    refresh->getClickEvent()->subscribe([tid, gameName, refresh](brls::View* view) {
        if (!controlDataAvailable(strtoull(tid.c_str(), NULL, 16)))
        {
            brls::Application::notify("Este juego no está instalado.\nRefrescar su caché le borraría el nombre\ny el icono, así que no se toca.");
            return;
        }
        invalidateControlCache(tid);
        refresh->setValue("hecho");
        brls::Application::notify("Caché refrescada para " + gameName);
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

            std::string label = jstr(it, "style", "icono");
            label.append(" · ");
            label.append(std::to_string(jint(it, "width")));
            label.append("x");
            label.append(std::to_string(jint(it, "height")));

            brls::ListItem* litem = new brls::ListItem(label);

            // Aplicar directamente con A. El botón lateral sólo funciona si el
            // icono llegó a tener el foco, y eso dejaba la impresión de que la
            // app se colgaba: al entrar, el foco está en "Tamaño", no en un icono.
            litem->getClickEvent()->subscribe([=](brls::View* view) {
                brls::Application::notify("Descargando…");
                std::string full = downloadImage(it, false);
                if (full.empty())
                {
                    brls::Application::notify("No se pudo descargar esa imagen");
                    return;
                }
                if (overwriteIcon(tid, full))
                    brls::Application::notify("Icono aplicado a " + gameName + "\nSe ve al reiniciar");
                else
                    brls::Application::notify("La imagen descargada no se pudo leer");
            });

            litem->registerAction("Ajustar el recorte", brls::Key::Y, [=] {
                brls::Application::notify("Descargando…");
                std::string full = downloadImage(it, false);
                if (full.empty())
                {
                    brls::Application::notify("No se pudo descargar esa imagen");
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
                    brls::Application::notify("Descargando…");
                    std::string full = downloadImage(it, false);
                    if (full.empty())
                    {
                        brls::Application::notify("No se pudo descargar");
                        return;
                    }
                    if (overwriteIcon(tid, full))
                        askReboot("Icono aplicado a " + gameName);
                    else
                        brls::Application::notify("La imagen descargada no se pudo leer");
                });
            });
            list->addView(litem);
            added++;
        }
    }

    if (added == 0)
    {
        std::string detalle = icons.contains("error") ? jstr(icons, "error") : "Probá con otro tamaño acá arriba";
        list->addView(new brls::ListItem("Sin iconos con este filtro", detalle));
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

    brls::ListItem* adapt = new brls::ListItem("Usar el icono original del juego", "", "Lo adapta al tema, para que no salga estirado");
    adapt->getClickEvent()->subscribe([tid, gameName, adapt](brls::View* view) {
        if (useOriginalIconAdapted(tid, gameName))
            adapt->setValue("hecho");
    });
    list->addView(adapt);

    if (hasCustomIcon(tid))
    {
        brls::ListItem* restore = new brls::ListItem("Quitar el icono personalizado", "", "Vuelve al icono del juego, sin adaptar");
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
        std::string detalle = found.contains("error") ? jstr(found, "error") : "Pulsá X y escribí otro nombre";
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
        brls::ListItem* litem = new brls::ListItem(name, "", archivado ? "archivado: el icono queda listo para cuando lo reinstales" : "");
        if (hasCustomIcon(tid))
            litem->setValue("icono propio");
        else if (archivado)
            litem->setValue("archivado", true);

        litem->getClickEvent()->subscribe([tid, name](brls::View* view) {
            frame_matches(tid, name, name);
        });

        litem->registerAction("Icono original adaptado al tema", brls::Key::PLUS, [tid, name, litem] {
            if (useOriginalIconAdapted(tid, name))
                litem->setValue("icono propio");
            return true;
        });

        litem->registerAction("Restaurar icono original", brls::Key::X, [tid, name, litem] {
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
            filter.empty() ? "" : "Pulsá Y para buscar otra cosa");
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
// Un perfil es una copia de todos los iconos puestos, guardada en
// /iconos/perfiles/<nombre>/<tid>/. Sirve para tener un juego de iconos por
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

// Recorre las carpetas de contents y se queda con las que tienen iconos
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
        // iconos que mostrar y no habia forma de saber por que.
        int archivosCopiados = 0;
        for (const std::string& archivo : { std::string("icon.jpg"), std::string("icon174.jpg") })
        {
            std::string origen = "sdmc:/atmosphere/contents/" + tid + "/" + archivo;
            std::error_code ecCopia;
            if (!std::filesystem::exists(origen, ecCopia))
                continue;

            std::filesystem::copy_file(origen, dest + tid + "/" + archivo, std::filesystem::copy_options::overwrite_existing, ecCopia);
            if (ecCopia)
                continue;

            // Que copy_file no se queje no basta: se comprueba que el destino
            // exista y no este vacio.
            auto tam = std::filesystem::file_size(dest + tid + "/" + archivo, ecCopia);
            if (!ecCopia && tam > 0)
                archivosCopiados++;
        }

        if (archivosCopiados > 0)
            copiados++;
        else
            std::filesystem::remove_all(dest + tid, ec);  // sin iconos, no dejar la carpeta
    }
    return copiados;
}

int applyProfile(const std::string& nombre)
{
    std::error_code ec;
    std::string origen = PROFILES_DIR + nombre + "/";
    int puestos = 0;

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
                std::filesystem::copy_file(f, dest + archivo, std::filesystem::copy_options::overwrite_existing, ec);
                alguno = true;
            }
        }
        if (alguno)
        {
            invalidateControlCache(tid);
            puestos++;
        }
    }
    return puestos;
}

// Deja la consola con los iconos de siempre. Sólo borra los dos archivos que
// escribimos nosotros; lo demás de cada carpeta se queda intacto.
int removeAllIcons()
{
    int quitados = 0;
    for (const std::string& tid : titlesWithIcons())
    {
        std::remove(iconPathFor(tid).c_str());
        std::remove(smallIconPathFor(tid).c_str());
        invalidateControlCache(tid);
        quitados++;
    }
    return quitados;
}

// Abre un perfil y muestra sus iconos uno por uno, con vista previa. Desde acá
// se puede aplicar el perfil entero o sólo el de un juego.
void frame_profile(const std::string& nombre)
{
    brls::ThumbnailFrame* frame = new brls::ThumbnailFrame();
    frame->setTitle(nombre);
    frame->setIcon(BOREALIS_ASSET("icon/borealis.jpg"));
    frame->getSidebar()->setThumbnail(BOREALIS_ASSET("icon/borealis.jpg"));
    frame->getSidebar()->setTitle(nombre);
    frame->getSidebar()->setSubtitle("Elegí un juego");
    frame->getSidebar()->getButton()->setLabel("Aplicar sólo este");

    brls::List* list = new brls::List();

    brls::ListItem* todos = new brls::ListItem("Aplicar todo el perfil");
    todos->getClickEvent()->subscribe([nombre](brls::View* view) {
        brls::Dialog* dialog = new brls::Dialog("Aplicar el perfil entero.\n\nLa pantalla se queda quieta un momento.\n¿Seguimos?");
        dialog->addButton("Aplicar", [dialog, nombre](brls::View* v) {
            dialog->close();
            int n = applyProfile(nombre);
            askReboot("Perfil aplicado a " + std::to_string(n) + " juegos");
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
                std::filesystem::copy_file(big, dest + "icon.jpg", std::filesystem::copy_options::overwrite_existing, ec2);
            if (std::filesystem::exists(small, ec2))
                std::filesystem::copy_file(small, dest + "icon174.jpg", std::filesystem::copy_options::overwrite_existing, ec2);
            invalidateControlCache(tid);
            brls::Application::notify("Aplicado a " + etiqueta + "\nSe ve al reiniciar");
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
            brls::Application::notify("Quitado del perfil");
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
    brls::ListItem* guardar = new brls::ListItem("Guardar los iconos actuales", "", "Crea un perfil con todo lo que tenés puesto ahora");
    guardar->getClickEvent()->subscribe([list](brls::View* view) {
        brls::Swkbd::openForText([list](std::string nombre) {
            if (nombre.empty())
                return;
            int n = saveProfile(nombre);
            brls::Application::notify("Perfil \"" + nombre + "\": " + std::to_string(n) + " juegos guardados");
            rebuildProfilesTab(list);
        }, "Nombre del perfil", "", 32, "perfil 1");
    });
    list->addView(guardar);

    brls::ListItem* quitar = new brls::ListItem("Dejar todos los iconos originales", "", "Quita los tuyos; útil al volver a un tema cuadrado");
    quitar->getClickEvent()->subscribe([list](brls::View* view) {
        brls::Dialog* dialog = new brls::Dialog("Se van a quitar tus iconos de todos los juegos.\n\nLos perfiles guardados no se tocan, así que podés volver cuando quieras.\n¿Seguimos?");
        dialog->addButton("Quitar", [dialog, list](brls::View* v) {
            dialog->close();
            int n = removeAllIcons();
            rebuildProfilesTab(list);
            askReboot("Quitados los iconos de " + std::to_string(n) + " juegos");
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

    for (const std::string& nombre : perfiles)
    {
        std::error_code ec;
        int juegos = 0;
        for (const auto& e : std::filesystem::directory_iterator(PROFILES_DIR + nombre, ec))
            if (!ec && e.is_directory(ec)) juegos++;

        brls::ListItem* item = new brls::ListItem(nombre, "", std::to_string(juegos) + " juegos · A para verlo, X para borrarlo");
        item->getClickEvent()->subscribe([nombre](brls::View* view) {
            frame_profile(nombre);
        });
        item->registerAction("Borrar el perfil", brls::Key::X, [nombre, item] {
            std::error_code ec;
            std::filesystem::remove_all(PROFILES_DIR + nombre, ec);
            item->setValue("borrado");
            brls::Application::notify("Perfil borrado: " + nombre);
            return true;
        });
        list->addView(item);
    }
}

// --------------------------------------------------------------- main ----

int main(int argc, char* argv[])
{
    nsInitialize();
    socketInitializeDefault();

    brls::Logger::setLogLevel(brls::LogLevel::INFO);
    i18n::loadTranslations();
    if (!brls::Application::init("main/name"_i18n))
    {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }

    nlohmann::json config = loadConfig();

    std::error_code ec;
    std::filesystem::create_directories(USER_ICONS_DIR, ec);

    brls::TabFrame* rootFrame = new brls::TabFrame();
    rootFrame->setTitle("IconGrabber OQB");
    rootFrame->setIcon(BOREALIS_ASSET("icon/borealis.jpg"));

    // --- pestaña de juegos: el único recorrido que hace falta ---
    brls::List* gamesTab = new brls::List();
    static std::string currentFilter = "";
    fillGameList(gamesTab, currentFilter);

    rootFrame->registerAction("Buscar juego", brls::Key::Y, [gamesTab] {
        brls::Swkbd::openForText([gamesTab](std::string text) {
            currentFilter = text;
            rebuildGameList(gamesTab, currentFilter);
        }, "Buscar entre los juegos instalados", "", 64, currentFilter);
        return true;
    });

    rootFrame->registerAction("Recargar lista", brls::Key::MINUS, [gamesTab] {
        installedGames(true);
        rebuildGameList(gamesTab, currentFilter);
        brls::Application::notify("Lista actualizada");
        return true;
    });

    // --- pestaña de ajustes ---
    brls::List* settingsTab = new brls::List();

    std::string sysStatus = sysmoduleInstalled()
        ? sysmoduleName() + " instalado"
        : "Falta el sysmodule: sin él los iconos no cambian";
    brls::ListItem* sysItem = new brls::ListItem("Sysmodule", sysStatus);
    settingsTab->addView(sysItem);

    brls::InputListItem* settingApiToken = new brls::InputListItem("Token de la API", config["api_token"], "Pegá tu token de steamgriddb.com", "Se saca gratis en steamgriddb.com", 64);
    brls::SelectListItem* settingStyles  = new brls::SelectListItem("Estilo de icono", allowedStyles, config["style_id"]);

    settingApiToken->getClickEvent()->subscribe([settingApiToken](brls::View* view) {
        nlohmann::json c = loadConfig();
        c["api_token"]   = settingApiToken->getValue();
        saveConfig(c);
    });
    settingStyles->getValueSelectedEvent()->subscribe([](size_t selection) {
        nlohmann::json c = loadConfig();
        c["style_id"]    = selection;
        saveConfig(c);
    });

    // sys-icon aplica los iconos al arrancar, así que hace falta reiniciar para
    // verlos. Tenerlo acá evita salir de la app y pelearse con el menú de power.
    std::vector<std::string> fitNames = { "recortar", "ajustar", "estirar", "tema vertical 2:3" };
    brls::ListItem* fitItem = new brls::ListItem("Imágenes no cuadradas", "", "Cómo encajarlas en el cuadro del menú HOME");
    fitItem->setValue(fitNames[loadConfig().value("fit_mode", 3)]);
    fitItem->getClickEvent()->subscribe([fitItem, fitNames](brls::View* view) {
        nlohmann::json c = loadConfig();
        int mode         = (c.value("fit_mode", 3) + 1) % 4;
        c["fit_mode"]    = mode;
        saveConfig(c);
        fitItem->setValue(fitNames[mode]);
        const char* detalle[4] = { "Toma el centro, sin deformar", "Entra completa, con franjas", "La aplasta al cuadro", "Vertical en el tema y recortado donde va cuadrado" };
        brls::Application::notify(detalle[mode]);
    });

    std::vector<std::string> smallNames = { "se deja el original", "recortado", "igual que el grande" };
    // Con un tema vertical, los juegos que nunca tocaste siguen mostrando su
    // icono cuadrado y el tema los estira. Esto recorre esos y les deja su
    // propio icono, pero ya preparado, sin buscar imágenes en ningún lado.
    brls::ListItem* adaptAll = new brls::ListItem("Adaptar todos los juegos al tema", "", "Usa el icono propio de cada uno, preparado para el tema");
    adaptAll->getClickEvent()->subscribe([gamesTab](brls::View* view) {
        nlohmann::json& games = installedGames();
        int pendientes        = 0;
        for (auto it : games)
            if (!hasCustomIcon(it["tid"].get<std::string>()))
                pendientes++;

        if (pendientes == 0)
        {
            brls::Application::notify("Todos los juegos ya tienen icono propio");
            return;
        }

        brls::Dialog* dialog = new brls::Dialog(
            "Se van a adaptar " + std::to_string(pendientes) + " juegos.\n\nLa pantalla se queda quieta mientras trabaja; puede tardar un rato.\n¿Seguimos?");
        dialog->addButton("Adaptar", [dialog, gamesTab](brls::View* v) {
            dialog->close();
            nlohmann::json& games = installedGames();
            int hechos = 0, fallados = 0;
            for (auto it : games)
            {
                std::string tid  = it["tid"].get<std::string>();
                std::string name = it["name"].get<std::string>();
                if (hasCustomIcon(tid) || it.value("archivado", false))
                    continue;

                std::string original;
                if (extractOriginalIcon(tid, original) && overwriteIcon(tid, original))
                    hechos++;
                else
                    fallados++;
            }
            rebuildGameList(gamesTab, currentFilter);
            std::string resumen = "Adaptados " + std::to_string(hechos) + " juegos";
            if (fallados)
                resumen += ", " + std::to_string(fallados) + " no se pudieron";
            askReboot(resumen);
        });
        dialog->addButton("Cancelar", [dialog](brls::View* v) { dialog->close(); });
        dialog->setCancelable(true);
        dialog->open();
    });

    brls::ListItem* smallItem = new brls::ListItem("Icono chico", "", "El de la lista completa y la ventanita al cambiar de app");
    smallItem->setValue(smallNames[loadConfig().value("small_mode", 0)]);
    smallItem->getClickEvent()->subscribe([smallItem, smallNames](brls::View* view) {
        nlohmann::json c  = loadConfig();
        int mode          = (c.value("small_mode", 0) + 1) % 3;
        c["small_mode"]   = mode;
        saveConfig(c);
        smallItem->setValue(smallNames[mode]);
        const char* detalle[3] = { "No se toca el icono del juego",
                                   "Para temas que lo muestran cuadrado",
                                   "Para temas que también lo muestran a lo alto" };
        brls::Application::notify(detalle[mode]);
    });

    brls::ListItem* sortItem = new brls::ListItem("Orden de la lista", "", "Alfabético o por lo último que jugaste");
    sortItem->setValue(loadConfig().value("sort_recent", false) ? "recientes" : "alfabético");
    sortItem->getClickEvent()->subscribe([sortItem, gamesTab](brls::View* view) {
        nlohmann::json c = loadConfig();
        bool recent      = !c.value("sort_recent", false);
        c["sort_recent"] = recent;
        saveConfig(c);
        sortItem->setValue(recent ? "recientes" : "alfabético");
        rebuildGameList(gamesTab, currentFilter);
        brls::Application::notify(recent ? "Ordenado por recientes" : "Ordenado por nombre");
    });

    brls::ListItem* rebootItem = new brls::ListItem("Reiniciar la consola", "Los iconos nuevos se ven después de reiniciar");
    rebootItem->getClickEvent()->subscribe([](brls::View* view) {
        brls::Dialog* dialog = new brls::Dialog("¿Reiniciar ahora?\nCerrá cualquier juego antes de seguir.");
        dialog->addButton("Reiniciar", [dialog](brls::View* v) {
            if (R_SUCCEEDED(spsmInitialize()))
            {
                spsmShutdown(true);
                spsmExit();
            }
            else
            {
                brls::Application::notify("No se pudo reiniciar");
                dialog->close();
            }
        });
        dialog->addButton("Cancelar", [dialog](brls::View* v) { dialog->close(); });
        dialog->setCancelable(true);
        dialog->open();
    });

    brls::ListItem* deleteIconCache = new brls::ListItem("Borrar caché de imágenes", "No toca los iconos ya aplicados");
    deleteIconCache->getClickEvent()->subscribe([](brls::View* view) {
        std::error_code ec;
        std::filesystem::remove_all("sdmc:/gameIcons/full/", ec);
        std::filesystem::remove_all("sdmc:/gameIcons/thumbnails/", ec);
        std::filesystem::create_directories("sdmc:/gameIcons/full/", ec);
        std::filesystem::create_directories("sdmc:/gameIcons/thumbnails/", ec);
        brls::Application::notify("Caché borrada");
    });

    settingsTab->addView(settingApiToken);
    settingsTab->addView(settingStyles);
    settingsTab->addView(fitItem);
    settingsTab->addView(smallItem);
    settingsTab->addView(adaptAll);
    settingsTab->addView(sortItem);
    settingsTab->addView(rebootItem);
    settingsTab->addView(deleteIconCache);

    brls::List* profilesTab = new brls::List();
    fillProfilesTab(profilesTab);

    rootFrame->addTab("Juegos", gamesTab);
    rootFrame->addTab("Perfiles", profilesTab);
    rootFrame->addSeparator();
    rootFrame->addTab("Ajustes", settingsTab);

    brls::Application::pushView(rootFrame);

    while (brls::Application::mainLoop())
        ;

    socketExit();
    nsExit();
    return EXIT_SUCCESS;
}
