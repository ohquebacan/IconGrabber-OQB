#include "vistas.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <switch.h>
extern "C" {
#include <switch/services/bsd.h>
}
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <thread>

#include "core.hpp"
#include "oqb_theme.hpp"
#include "qrcodegen.h"

std::atomic<int> g_genIconos{ 0 };
std::atomic<bool> g_saliendo{ false };

/* std::thread::detach() aborta la app con la toolchain de IconGrabber
   (terminate sin excepción, probado en consola el 24-nov-2026). Así que los
   hilos quedan en esta lista y se juntan cuando terminan. Al salir, los que
   siguen trabajando se dejan como están: la lista no se destruye nunca. */
namespace
{
struct Hilo
{
    std::thread t;
    std::shared_ptr<std::atomic<bool>> fin;
};
std::mutex g_mHilos;
std::vector<Hilo>* g_hilos = new std::vector<Hilo>();
}  // namespace

void lanzar(std::function<void()> f)
{
    auto fin = std::make_shared<std::atomic<bool>>(false);
    std::thread t([f, fin] {
        try
        {
            f();
        }
        catch (const std::exception& e)
        {
            marca(std::string("hilo: ") + e.what());
        }
        *fin = true;
    });
    std::lock_guard<std::mutex> l(g_mHilos);
    g_hilos->push_back({ std::move(t), fin });
}

void esperarHilos()
{
    std::vector<Hilo> todos;
    {
        std::lock_guard<std::mutex> l(g_mHilos);
        todos.swap(*g_hilos);
    }
    for (auto& h : todos)
        if (h.t.joinable()) h.t.join();
}

// Lo que no se puede hacer dentro del manejador de un botón (cerrar la
// pantalla que lo contiene): se hace al empezar el cuadro siguiente.
namespace
{
std::mutex g_mLuego;
std::vector<std::function<void()>> g_luego;
}  // namespace

void luego(std::function<void()> f)
{
    std::lock_guard<std::mutex> l(g_mLuego);
    g_luego.push_back(std::move(f));
}

void cosecharHilos()
{
    {
        std::vector<std::function<void()>> pendientes;
        {
            std::lock_guard<std::mutex> l(g_mLuego);
            pendientes.swap(g_luego);
        }
        for (auto& f : pendientes) f();
    }
    std::lock_guard<std::mutex> l(g_mHilos);
    for (auto it = g_hilos->begin(); it != g_hilos->end();)
    {
        if (*it->fin)
        {
            it->t.join();
            it = g_hilos->erase(it);
        }
        else
            ++it;
    }
}

namespace
{

// ------------------------------------------------------------ dibujo ------

// View::a es protegido: lo mismo para los ayudantes de dibujo.
NVGcolor al(brls::View* v, NVGcolor c)
{
    c.a *= v->getAlpha();
    return c;
}

int fuenteTitulo(NVGcontext* vg)
{
    static int f = -2;
    if (f == -2)
    {
        f = nvgFindFont(vg, "upheaval");
        if (f < 0) f = nvgCreateFont(vg, "upheaval", "romfs:/fonts/upheavtt.ttf");
    }
    return f;
}

std::string cortar(NVGcontext* vg, const std::string& texto, float ancho)
{
    if (ancho <= 0) return "";
    if (nvgTextBounds(vg, 0, 0, texto.c_str(), nullptr, nullptr) <= ancho) return texto;
    std::string s = texto;
    while (!s.empty())
    {
        do s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80);
        const std::string prueba = s + "…";
        if (nvgTextBounds(vg, 0, 0, prueba.c_str(), nullptr, nullptr) <= ancho) return prueba;
    }
    return "…";
}

std::string iniciales(const std::string& n)
{
    std::string out;
    for (size_t i = 0; i < n.size() && out.size() < 2; ++i)
        if (std::isalnum(static_cast<unsigned char>(n[i])) && (i == 0 || n[i - 1] == ' ' || n[i - 1] == ':' || n[i - 1] == '-'))
            out += (char)std::toupper(static_cast<unsigned char>(n[i]));
    return out.empty() ? "?" : out;
}

void titulo(brls::View* v, NVGcontext* vg, float x, float y, const std::string& t, brls::FrameContext* ctx)
{
    const int f = fuenteTitulo(vg);
    nvgFontFaceId(vg, f >= 0 ? f : ctx->fontStash->regular);
    nvgFontSize(vg, 40.0f);
    nvgTextLetterSpacing(vg, 1.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg, al(v, ctx->theme->textColor));
    nvgBeginPath(vg);
    std::string mayus = t;
    std::transform(mayus.begin(), mayus.end(), mayus.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    nvgText(vg, x, y, mayus.c_str(), nullptr);
    nvgTextLetterSpacing(vg, 0.0f);
    nvgFontFaceId(vg, ctx->fontStash->regular);
}

void texto(brls::View* v, NVGcontext* vg, float x, float y, const std::string& t, float tam, NVGcolor color, int align = NVG_ALIGN_LEFT | NVG_ALIGN_TOP)
{
    nvgFontSize(vg, tam);
    nvgTextAlign(vg, align);
    nvgFillColor(vg, al(v, color));
    nvgBeginPath(vg);
    nvgText(vg, x, y, t.c_str(), nullptr);
}

// Chips en una o varias filas; devuelve dónde sigue lo de abajo.
float chips(brls::View* v, NVGcontext* vg, float x, float y, float ancho, const std::vector<std::string>& lista, int sel, brls::FrameContext* ctx)
{
    float cx = x;
    nvgFontSize(vg, 15.0f);
    for (int i = 0; i < (int)lista.size(); ++i)
    {
        const float w = nvgTextBounds(vg, 0, 0, lista[i].c_str(), nullptr, nullptr) + 26.0f;
        if (cx + w > x + ancho && cx > x)
        {
            cx = x;
            y += 40.0f;
        }
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx, y, w, 32.0f, 16.0f);
        if (i == sel)
        {
            nvgFillColor(vg, al(v, ctx->theme->textColor));
            nvgFill(vg);
        }
        else
        {
            nvgStrokeColor(vg, al(v, ctx->theme->sidebarSeparatorColor));
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);
        }
        texto(v, vg, cx + w / 2, y + 17.0f, lista[i], 15.0f, i == sel ? ctx->theme->backgroundColorRGB : ctx->theme->descriptionColor, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        cx += w + 8.0f;
    }
    return y + 46.0f;
}

/* Con el tema de HOME vertical todo se muestra en 2:3, como lo verá el
   HOME. Se lee de la configuración como mucho una vez por segundo. */
bool esTemaVertical()
{
    static bool valor = false;
    static auto ultima = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    const auto ahora   = std::chrono::steady_clock::now();
    if (ahora - ultima > std::chrono::seconds(1))
    {
        valor  = loadConfig().value("fit_mode", 0) == 3;
        ultima = ahora;
    }
    return valor;
}

/* Un cuadro con imagen (o iniciales) y, si se pide, el marco de selección.
   contener: la imagen entra entera con su proporción (los resultados de
   SteamGridDB); si no, llena el cuadro (el ícono tal como lo estira el HOME). */
void loseta(brls::View* v, NVGcontext* vg, float x, float y, float w, float h, int img, const std::string& nombre, bool sel, bool foco, brls::FrameContext* ctx, float alfa = 1.0f, bool contener = false)
{
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 14.0f);
    int iw = 0, ih = 0;
    if (img > 0 && contener) nvgImageSize(vg, img, &iw, &ih);
    if (img > 0 && contener && iw > 0 && ih > 0)
    {
        nvgFillColor(vg, al(v, oqb::velo(14)));
        nvgFill(vg);
        const float esc = std::min(w / iw, h / ih);
        const float dw = iw * esc, dh = ih * esc, dx = x + (w - dw) / 2, dy = y + (h - dh) / 2;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, dx, dy, dw, dh, 12.0f);
        nvgFillPaint(vg, nvgImagePattern(vg, dx, dy, dw, dh, 0, img, v->getAlpha() * alfa));
    }
    else if (img > 0)
        nvgFillPaint(vg, nvgImagePattern(vg, x, y, w, h, 0, img, v->getAlpha() * alfa));
    else
        nvgFillColor(vg, al(v, oqb::velo(24)));
    nvgFill(vg);
    if (img <= 0 && !nombre.empty())
        texto(v, vg, x + w / 2, y + h / 2, iniciales(nombre), std::min(w, h) * 0.26f, ctx->theme->descriptionColor, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    if (sel && foco)
    {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x - 4, y - 4, w + 8, h + 8, 17.0f);
        nvgStrokeColor(vg, al(v, ctx->theme->highlightColor1));
        nvgStrokeWidth(vg, 3.0f);
        nvgStroke(vg);
    }
}

// Una pastilla de estado sobre una loseta: 0 gris, 1 verde, 2 dorado, 3 rojo.
void pastilla(brls::View* v, NVGcontext* vg, float derecha, float y, const std::string& t, int tono)
{
    nvgFontSize(vg, 12.0f);
    const float w = nvgTextBounds(vg, 0, 0, t.c_str(), nullptr, nullptr) + 16.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, derecha - w, y, w, 22.0f, 11.0f);
    const NVGcolor fondos[4] = { nvgRGBA(20, 20, 20, 190), nvgRGBA(18, 58, 36, 230), nvgRGBA(58, 50, 32, 235), nvgRGBA(70, 24, 24, 235) };
    nvgFillColor(vg, al(v, fondos[tono]));
    nvgFill(vg);
    const NVGcolor letras[4] = { nvgRGB(220, 222, 218), nvgRGB(157, 234, 154), nvgRGB(232, 184, 74), nvgRGB(240, 149, 149) };
    texto(v, vg, derecha - w / 2, y + 12.0f, t, 12.0f, letras[tono], NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
}

void boton(brls::View* v, NVGcontext* vg, float x, float y, float w, float h, const std::string& t, bool principal, brls::FrameContext* ctx)
{
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 12.0f);
    nvgFillColor(vg, al(v, principal ? oqb::esmeralda() : oqb::velo(18)));
    nvgFill(vg);
    texto(v, vg, x + w / 2, y + h / 2 + 1, t, 18.0f, principal ? (oqb::oscuro() ? nvgRGB(6, 20, 12) : nvgRGB(255, 255, 255)) : ctx->theme->textColor, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
}

void barra(brls::View* v, NVGcontext* vg, float x, float y, float w, float avance)
{
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, 8.0f, 4.0f);
    nvgFillColor(vg, al(v, oqb::velo(22)));
    nvgFill(vg);
    avance = std::max(0.0f, std::min(1.0f, avance));
    if (avance > 0)
    {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, w * avance, 8.0f, 4.0f);
        nvgFillColor(vg, al(v, oqb::esmeralda()));
        nvgFill(vg);
    }
}

int imagenDeBytes(NVGcontext* vg, std::vector<unsigned char>& datos)
{
    if (datos.empty()) return 0;
    int img = nvgCreateImageMem(vg, 0, datos.data(), (int)datos.size());
    return img > 0 ? img : 0;
}

std::string kb(std::uintmax_t bytes)
{
    return std::to_string((bytes + 512) / 1024) + " KB";
}

const std::vector<std::string> ESTILOS = { "Todos", "Alternativos", "Borrosos", "Logo blanco", "Material", "Sin logo" };

/* Cuadradas (o casi) y verticales por separado: con un tema vertical se
   quieren las de 2:3 y con uno cuadrado, las de 1:1; mezcladas cuesta verlas. */
const std::vector<std::string> FORMAS = { "Cuadrados", "Verticales", "Todos" };

// 0 cuadrada o casi, 1 vertical, 2 horizontal, -1 no se sabe.
int formaDe(int w, int h)
{
    if (w <= 0 || h <= 0) return -1;
    const double r = (double)h / w;
    if (r >= 0.8 && r <= 1.25) return 0;
    return r > 1.25 ? 1 : 2;
}

bool entraEnForma(int forma, int w, int h)
{
    return forma == 2 || formaDe(w, h) == forma;
}

// Lo primero que se muestra sigue el ajuste: tema vertical, verticales.
int formaInicial()
{
    return loadConfig().value("fit_mode", 0) == 3 ? 1 : 0;
}

// Mover la selección en una grilla con la cruceta.
bool mover(int& elegido, int total, int columnas, int dx, int dy)
{
    if (total <= 0) return dx < 0 ? false : true;
    const int col = elegido % columnas;
    if (dx < 0 && col == 0) return false;  // al menú lateral
    int n = elegido + dx + dy * columnas;
    if (dx > 0 && (col == columnas - 1 || n >= total)) return true;
    if (n < 0 || n >= total)
    {
        if (dy > 0 && elegido / columnas < (total - 1) / columnas) n = total - 1;
        else return true;
    }
    elegido = n;
    return true;
}

void ajustarScroll(int elegido, int columnas, int filasVisibles, int& primeraFila)
{
    const int fila = elegido / columnas;
    if (fila < primeraFila) primeraFila = fila;
    if (fila >= primeraFila + filasVisibles) primeraFila = fila - filasVisibles + 1;
}

}  // namespace

// ------------------------------------------------------------- marcos -----

MarcoPrincipal::MarcoPrincipal()
    : TabFrame()
{
    this->setTitle("");
    // Corto: a la derecha van los botones.
    std::string pie = "v" APP_VERSION;
    if (sysmoduleInstalled())
        pie += " · " + std::string(sysmoduleName() == "sys-tweak" ? "sys-ticon" : sysmoduleName());
    else
        pie += " · sin sys-icon";
    this->setFooterText(pie);
}

void MarcoPrincipal::layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash)
{
    const unsigned cabecera                 = style->AppletFrame.headerHeightRegular;
    style->AppletFrame.headerHeightRegular = 0;
    brls::AppletFrame::layout(vg, style, stash);
    style->AppletFrame.headerHeightRegular = cabecera;
}

void MarcoPrincipal::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx)
{
    {
        const unsigned cabecera                 = style->AppletFrame.headerHeightRegular;
        style->AppletFrame.headerHeightRegular = 0;
        brls::TabFrame::draw(vg, x, y, width, height, style, ctx);
        style->AppletFrame.headerHeightRegular = cabecera;
    }
    // El nombre arriba del panel, como OQB Updater: «ICON» con la Trifuerza y «GRABBER».
    static int bebas = -2;
    if (bebas == -2) bebas = nvgCreateFont(vg, "bebas", "romfs:/fonts/BebasNeue-Regular.ttf");
    const float lx  = x + style->Sidebar.marginLeft + 12.0f;
    const float tam = 56.0f;
    nvgFontFaceId(vg, bebas >= 0 ? bebas : ctx->fontStash->regular);
    nvgFontSize(vg, tam);
    nvgTextLetterSpacing(vg, 1.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, this->a(ctx->theme->textColor));
    const float cy1 = y + 38.0f, cy2 = y + 86.0f;
    nvgBeginPath(vg);
    const float fin = nvgText(vg, lx, cy1, "ICON", nullptr);
    nvgBeginPath(vg);
    nvgText(vg, lx, cy2, "GRABBER", nullptr);
    nvgTextLetterSpacing(vg, 0.0f);
    const float th = tam * 0.70f, esc = th / 31.0f;
    const float tx = fin + 12.0f, ty = cy1 + tam * 0.25f - th;
    const float tris[3][6] = { { 18, 0, 27, 15.5f, 9, 15.5f }, { 27, 15.5f, 36, 31, 18, 31 }, { 9, 15.5f, 18, 31, 0, 31 } };
    nvgFillColor(vg, this->a(oqb::acentoFuerte()));
    for (const auto& t : tris)
    {
        nvgBeginPath(vg);
        nvgMoveTo(vg, tx + t[0] * esc, ty + t[1] * esc);
        nvgLineTo(vg, tx + t[2] * esc, ty + t[3] * esc);
        nvgLineTo(vg, tx + t[4] * esc, ty + t[5] * esc);
        nvgClosePath(vg);
        nvgFill(vg);
    }
    nvgFontFaceId(vg, ctx->fontStash->regular);
}

MarcoCompleto::MarcoCompleto(brls::View* contenido)
    : AppletFrame(false, false)
{
    this->setTitle("");
    this->setContentView(contenido);
}

void MarcoCompleto::layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash)
{
    const unsigned cabecera                 = style->AppletFrame.headerHeightRegular;
    style->AppletFrame.headerHeightRegular = 0;
    brls::AppletFrame::layout(vg, style, stash);
    style->AppletFrame.headerHeightRegular = cabecera;
}

void MarcoCompleto::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx)
{
    const unsigned cabecera                 = style->AppletFrame.headerHeightRegular;
    style->AppletFrame.headerHeightRegular = 0;
    brls::AppletFrame::draw(vg, x, y, width, height, style, ctx);
    style->AppletFrame.headerHeightRegular = cabecera;
}

// ------------------------------------------------------------- juegos -----

namespace
{
const char* FILTROS[] = { "Todos", "Con ícono propio", "Originales", "En el USB", "Archivados" };
}

VistaJuegos::VistaJuegos()
{
    marca("juegos: recargar");
    this->recargar();
    marca("juegos: acciones");

    this->registerAction("", brls::Key::DUP, [this] { return mover(this->elegido, (int)this->visibles().size(), this->columnas, 0, -1); }, true);
    this->registerAction("", brls::Key::DDOWN, [this] { return mover(this->elegido, (int)this->visibles().size(), this->columnas, 0, 1); }, true);
    this->registerAction("", brls::Key::DLEFT, [this] { return mover(this->elegido, (int)this->visibles().size(), this->columnas, -1, 0); }, true);
    this->registerAction("", brls::Key::DRIGHT, [this] { return mover(this->elegido, (int)this->visibles().size(), this->columnas, 1, 0); }, true);
    this->registerAction("", brls::Key::L, [this] {
        this->filtro = (this->filtro + 4) % 5;
        this->elegido = this->primeraFila = 0;
        return true;
    }, true);
    this->registerAction("Filtro", brls::Key::R, [this] {
        this->filtro = (this->filtro + 1) % 5;
        this->elegido = this->primeraFila = 0;
        return true;
    });
    this->registerAction("Buscar", brls::Key::X, [this] {
        brls::Swkbd::openForText([this](std::string t) {
            this->busqueda = t;
            this->elegido = this->primeraFila = 0;
        }, "Buscar entre tus juegos", "", 64, this->busqueda);
        return true;
    });
    this->registerAction("Íconos para todos", brls::Key::Y, [] {
        brls::Application::pushView(new MarcoCompleto(new VistaLote()));
        return true;
    });
    this->registerAction("", brls::Key::MINUS, [this] {
        installedGames(true);
        this->recargar();
        avisar("Lista actualizada");
        return true;
    }, true);
    this->registerAction("Cambiar", brls::Key::A, [this] {
        this->abrir();
        return true;
    });
}

VistaJuegos::~VistaJuegos()
{
    this->liberar();
}

void VistaJuegos::liberar()
{
    NVGcontext* vg = brls::Application::getNVGContext();
    for (auto& j : this->juegos)
    {
        if (j.img > 0 && vg) nvgDeleteImage(vg, j.img);
        j.img = -1;
    }
}

void VistaJuegos::recargar()
{
    this->liberar();
    this->juegos.clear();
    nlohmann::json ordenados = installedGames();
    if (loadConfig().value("sort_recent", false))
        std::stable_sort(ordenados.begin(), ordenados.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            u64 ta = a.contains("last_played") ? a["last_played"].get<u64>() : UINT64_MAX;
            u64 tb = b.contains("last_played") ? b["last_played"].get<u64>() : UINT64_MAX;
            return ta < tb;
        });
    marca("recargar: estados");
    for (auto& g : ordenados)
    {
        Juego j;
        j.tid       = g["tid"].get<std::string>();
        j.nombre    = g["name"].get<std::string>();
        j.archivado = g.value("archivado", false);
        j.usb       = g.value("usb", false);
        const EstadoIcono e = iconState(j.tid);
        j.estado    = e == EstadoIcono::Original ? 0 : (e == EstadoIcono::Propio ? 1 : 2);
        this->juegos.push_back(j);
    }
    this->gen = g_genIconos.load();
}

std::vector<int> VistaJuegos::visibles() const
{
    std::vector<int> out;
    const std::string aguja = toLower(this->busqueda);
    for (int i = 0; i < (int)this->juegos.size(); ++i)
    {
        const Juego& j = this->juegos[i];
        if (!aguja.empty() && toLower(j.nombre).find(aguja) == std::string::npos) continue;
        if (this->filtro == 1 && j.estado == 0) continue;
        if (this->filtro == 2 && j.estado != 0) continue;
        if (this->filtro == 3 && !j.usb) continue;
        if (this->filtro == 4 && !j.archivado) continue;
        out.push_back(i);
    }
    return out;
}

void VistaJuegos::abrir()
{
    const auto v = this->visibles();
    if (this->elegido < 0 || this->elegido >= (int)v.size()) return;
    const Juego& j = this->juegos[v[this->elegido]];
    brls::Application::pushView(new MarcoCompleto(new VistaIconos(j.tid, j.nombre)));
}

void VistaJuegos::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx)
{
    if (g_genIconos.load() != this->gen) this->recargar();

    const float lx = x + 36.0f, lw = width - 72.0f;
    float ly       = y + 30.0f;
    titulo(this, vg, lx, ly, "Juegos", ctx);

    // Buscador a la derecha del título.
    {
        nvgFontSize(vg, 40.0f);
        const float bx = lx + 190.0f, bw = lx + lw - bx;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx, ly - 2, bw, 44.0f, 12.0f);
        nvgStrokeColor(vg, this->a(ctx->theme->sidebarSeparatorColor));
        nvgStrokeWidth(vg, 1.0f);
        nvgStroke(vg);
        const std::string t = this->busqueda.empty() ? " Buscar entre " + std::to_string(this->juegos.size()) + " juegos" : " Buscando: " + this->busqueda;
        texto(this, vg, bx + 16.0f, ly + 20.0f, t, 16.0f, ctx->theme->descriptionColor, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    }
    ly += 60.0f;

    int cuenta[5] = { (int)this->juegos.size(), 0, 0, 0, 0 };
    for (auto& j : this->juegos)
    {
        cuenta[1] += j.estado != 0;
        cuenta[2] += j.estado == 0;
        cuenta[3] += j.usb;
        cuenta[4] += j.archivado;
    }
    std::vector<std::string> nombres;
    for (int i = 0; i < 5; ++i)
        nombres.push_back(std::string(FILTROS[i]) + " · " + std::to_string(cuenta[i]));
    ly = chips(this, vg, lx, ly, lw, nombres, this->filtro, ctx);

    const auto v = this->visibles();
    if (this->elegido >= (int)v.size()) this->elegido = std::max(0, (int)v.size() - 1);
    // La grilla de Juegos siempre cuadrada: muestra cada juego como es. Lo
    // vertical va al entrar a un juego, donde se eligen los íconos.
    this->columnas = 6;
    const float gap = 16.0f;
    const float tam = (lw - gap * (this->columnas - 1)) / this->columnas;
    const float alto = tam;
    const float fila = alto + 34.0f;
    const int filasVisibles = std::max(1, (int)((y + height - 6.0f - ly) / fila));
    ajustarScroll(this->elegido, this->columnas, filasVisibles, this->primeraFila);

    if (v.empty())
    {
        texto(this, vg, lx, ly + 10.0f, this->juegos.empty() ? "No se encontraron juegos instalados." : "Ningún juego con este filtro.", 18.0f, ctx->theme->descriptionColor);
        return;
    }

    nvgSave(vg);
    nvgScissor(vg, x, ly - 6, width, y + height - ly + 6);
    int cargadas = 0;
    for (int k = this->primeraFila * this->columnas; k < (int)v.size() && k < (this->primeraFila + filasVisibles + 1) * this->columnas; ++k)
    {
        Juego& j     = this->juegos[v[k]];
        const int fi = k / this->columnas - this->primeraFila, co = k % this->columnas;
        const float tx = lx + co * (tam + gap), ty = ly + fi * fila;
        if (j.img < 0 && cargadas < 3)
        {
            auto datos = currentIconJpeg(j.tid);
            j.img      = imagenDeBytes(vg, datos);
            ++cargadas;
        }
        loseta(this, vg, tx, ty, tam, alto, j.img, j.nombre, k == this->elegido, this->isFocused(), ctx, j.archivado ? 0.55f : 1.0f);
        if (j.estado == 1) pastilla(this, vg, tx + tam - 6, ty + 6, "Propio", 1);
        else if (j.estado == 2) pastilla(this, vg, tx + tam - 6, ty + 6, "No se aplica", 2);
        else if (j.archivado) pastilla(this, vg, tx + tam - 6, ty + 6, "Archivado", 0);
        else if (j.usb) pastilla(this, vg, tx + tam - 6, ty + 6, "USB", 0);
        nvgFontSize(vg, 14.0f);
        texto(this, vg, tx + tam / 2, ty + alto + 8.0f, cortar(vg, j.nombre, tam), 14.0f, k == this->elegido ? ctx->theme->textColor : ctx->theme->descriptionColor, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    }
    nvgRestore(vg);
}

// ------------------------------------------------------- un juego ---------

VistaIconos::VistaIconos(const std::string& tid, const std::string& nombre)
    : tid(tid)
    , nombre(nombre)
    , termino(nombre)
{
    this->chip  = std::max(0, std::min((int)ESTILOS.size() - 1, loadConfig().value("style_id", 0)));
    this->forma        = formaInicial();
    this->temaVertical = loadConfig().value("fit_mode", 0) == 3;
    this->buscar();

    auto total = [this] {
        std::lock_guard<std::mutex> l(this->busqueda->m);
        return (int)this->visibles().size();
    };
    this->registerAction("", brls::Key::DUP, [this, total] { mover(this->elegido, total(), this->columnas, 0, -1); return true; }, true);
    this->registerAction("", brls::Key::DDOWN, [this, total] { mover(this->elegido, total(), this->columnas, 0, 1); return true; }, true);
    this->registerAction("", brls::Key::DLEFT, [this, total] { mover(this->elegido, total(), this->columnas, -1, 0); return true; }, true);
    this->registerAction("", brls::Key::DRIGHT, [this, total] { mover(this->elegido, total(), this->columnas, 1, 0); return true; }, true);
    this->registerAction("", brls::Key::L, [this] {
        this->forma   = (this->forma + 2) % 3;
        this->elegido = this->primeraFila = 0;
        return true;
    }, true);
    this->registerAction("Forma", brls::Key::R, [this] {
        this->forma   = (this->forma + 1) % 3;
        this->elegido = this->primeraFila = 0;
        return true;
    });
    this->registerAction("Buscar otro nombre", brls::Key::X, [this] {
        brls::Swkbd::openForText([this](std::string t) {
            if (t.empty()) return;
            this->termino = t;
            this->juegoId.clear();
            this->buscar();
        }, "Nombre para buscar en SteamGridDB", "", 64, this->termino);
        return true;
    });
    this->registerAction("Más opciones", brls::Key::MINUS, [this] {
        this->opciones();
        return true;
    });
    this->registerAction("Ajustar recorte", brls::Key::Y, [this] {
        this->aplicar(true);
        return true;
    });
    this->registerAction("Poner este ícono", brls::Key::A, [this] {
        this->aplicar(false);
        return true;
    });
}

std::vector<int> VistaIconos::visibles()
{
    std::vector<int> v;
    const auto& res = this->busqueda->res;
    for (int i = 0; i < (int)res.size(); ++i)
        if (entraEnForma(this->forma, res[i].w, res[i].h) || (res[i].w <= 0 && this->forma != 2 && !res[i].local.empty()))
            v.push_back(i);
    return v;
}

VistaIconos::~VistaIconos()
{
    if (this->busqueda) this->busqueda->cancelar = true;
    NVGcontext* vg = brls::Application::getNVGContext();
    if (this->busqueda)
    {
        std::lock_guard<std::mutex> l(this->busqueda->m);
        for (auto& r : this->busqueda->res)
            if (r.img > 0 && vg) nvgDeleteImage(vg, r.img);
    }
    this->liberarPrevias(vg);
    if (this->actual > 0 && vg) nvgDeleteImage(vg, this->actual);
    if (this->original > 0 && vg) nvgDeleteImage(vg, this->original);
}

void VistaIconos::liberarPrevias(NVGcontext* vg)
{
    if (vg && this->previaGrande > 0) nvgDeleteImage(vg, this->previaGrande);
    if (vg && this->previaChica > 0) nvgDeleteImage(vg, this->previaChica);
    this->previaGrande = this->previaChica = -1;
    this->previaDe     = -1;
}

void VistaIconos::buscar()
{
    NVGcontext* vg = brls::Application::getNVGContext();
    if (this->busqueda)
    {
        this->busqueda->cancelar = true;
        std::lock_guard<std::mutex> l(this->busqueda->m);
        for (auto& r : this->busqueda->res)
            if (r.img > 0 && vg) nvgDeleteImage(vg, r.img);
        this->busqueda->res.clear();
    }
    this->liberarPrevias(vg);
    this->elegido = this->primeraFila = 0;

    auto b        = std::make_shared<Busqueda>();
    this->busqueda = b;

    // «De la SD»: lo que dejaste en /iconos/ y lo ya descargado antes.
    if (this->chip == (int)ESTILOS.size())
    {
        std::error_code ec;
        for (const std::string& dir : { std::string("sdmc:/iconos/"), std::string("sdmc:/gameIcons/full/") })
            for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            {
                if (ec) break;
                const std::string ext = toLower(e.path().extension().string());
                if (!e.is_regular_file(ec) || (ext != ".jpg" && ext != ".jpeg" && ext != ".png")) continue;
                Resultado r;
                r.local = r.thumb = e.path().string();
                r.listo = true;
                imageSize(r.local, r.w, r.h);
                b->res.push_back(r);
            }
        b->juegoNombre = "Imágenes de tu SD";
        if (b->res.empty()) b->error = "No hay imágenes. Copia archivos .jpg o .png a la carpeta /iconos/ de la SD.";
        b->fase = b->res.empty() ? 2 : 1;
        return;
    }

    const std::string termino = this->termino, fijo = this->juegoId;
    const int estilo = this->chip;
    lanzar([b, termino, fijo, estilo] {
        g_silencio = true;
        std::string id = fijo, nombreJuego;
        if (id.empty())
        {
            nlohmann::json found = requestGames(termino);
            if (jbool(found, "success") && found.contains("data") && found["data"].is_array())
                for (auto& it : found["data"])
                {
                    std::string n = jstr(it, "name"), i = jstr(it, "id");
                    if (!n.empty() && !i.empty())
                    {
                        std::lock_guard<std::mutex> l(b->m);
                        b->coincidencias.push_back({ n, i });
                    }
                }
            std::lock_guard<std::mutex> l(b->m);
            if (b->coincidencias.empty())
            {
                b->error = found.contains("error") ? "No hubo respuesta de SteamGridDB (" + jstr(found, "error") + "). Probablemente falta el token o no hay internet: revísalo en Ajustes."
                                                   : "SteamGridDB no tiene «" + termino + "». Pulsa X y prueba con otro nombre.";
                b->fase  = 2;
                return;
            }
            id          = b->coincidencias[0].second;
            nombreJuego = b->coincidencias[0].first;
        }
        if (b->cancelar) return;
        nlohmann::json icons = requestIcons(id, estilo, -1);
        {
            std::lock_guard<std::mutex> l(b->m);
            b->juegoId = id;
            if (!nombreJuego.empty()) b->juegoNombre = nombreJuego;
            if (jbool(icons, "success") && icons.contains("data") && icons["data"].is_array())
                for (auto& it : icons["data"])
                {
                    if (jbool(it, "lock")) continue;
                    Resultado r;
                    r.icono = it;
                    r.w     = jint(it, "width");
                    r.h     = jint(it, "height");
                    b->res.push_back(r);
                }
            if (b->res.empty())
                b->error = icons.contains("error") ? "No hubo respuesta de SteamGridDB (" + jstr(icons, "error") + ")." : "No hay íconos con este estilo. Prueba otro estilo en Más opciones (\uE0F0).";
            b->fase = b->res.empty() ? 2 : 1;
        }
        for (size_t i = 0;; ++i)
        {
            nlohmann::json icono;
            {
                std::lock_guard<std::mutex> l(b->m);
                if (b->cancelar || g_saliendo || i >= b->res.size()) break;
                icono = b->res[i].icono;
            }
            std::string thumb = downloadImage(icono, true);
            std::lock_guard<std::mutex> l(b->m);
            if (b->cancelar || g_saliendo || i >= b->res.size()) break;
            b->res[i].thumb = thumb;
            b->res[i].fallo = thumb.empty();
            b->res[i].listo = !thumb.empty();
        }
    });
}

void VistaIconos::aplicar(bool recorte)
{
    if (this->aplicando && this->aplicando->fase == 1) return;
    Resultado r;
    {
        std::lock_guard<std::mutex> l(this->busqueda->m);
        const auto v = this->visibles();
        if (this->elegido < 0 || this->elegido >= (int)v.size()) return;
        r = this->busqueda->res[v[this->elegido]];
    }
    auto ap        = std::make_shared<Aplicando>();
    ap->recorte    = recorte;
    ap->fase       = 1;
    this->aplicando = ap;
    const std::string tid = this->tid;
    lanzar([ap, r, tid] {
        g_silencio       = true;
        std::string full = r.local.empty() ? downloadImage(r.icono, false) : r.local;
        if (full.empty())
        {
            ap->mensaje = "No se pudo descargar esa imagen. Probablemente se cortó la conexión; prueba otra vez.";
            ap->fase    = 2;
            return;
        }
        ap->imagen = full;
        if (ap->recorte)
        {
            ap->ok   = true;
            ap->fase = 2;
            return;
        }
        ap->ok = overwriteIcon(tid, full);
        if (ap->ok)
        {
            std::error_code ec;
            auto g      = std::filesystem::file_size(iconPathFor(tid), ec);
            std::string chico;
            auto c = std::filesystem::file_size(smallIconPathFor(tid), ec);
            if (!ec) chico = ", chico " + kb(c);
            ap->mensaje = "Listo: grande " + kb(g) + chico + ". Entra en el HOME.";
            if (!controlDataAvailable(strtoull(tid.c_str(), NULL, 16)))
                ap->mensaje += "\nEl juego no está instalado: se verá al reinstalarlo.";
            else
                ap->mensaje += "\nSi el HOME sigue mostrando el anterior, reinicia.";
        }
        else
            ap->mensaje = "No se pudo poner: probablemente la imagen no entra en el HOME ni con menos calidad. Prueba otra.";
        g_genIconos++;
        ap->fase = 2;
    });
}

void VistaIconos::opciones()
{
    const std::string tid = this->tid, nombre = this->nombre;
    std::vector<std::pair<std::string, std::string>> coincidencias;
    {
        std::lock_guard<std::mutex> l(this->busqueda->m);
        coincidencias = this->busqueda->coincidencias;
    }
    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle(nombre);
    brls::List* list = new brls::List();

    brls::ListItem* quitar = new brls::ListItem("Quitar mi ícono", "", "Vuelve al ícono del juego, sin adaptar");
    quitar->getClickEvent()->subscribe([tid, nombre, quitar](brls::View*) {
        if (restoreOriginalIcon(tid, nombre))
        {
            quitar->setValue("hecho");
            g_genIconos++;
        }
    });
    list->addView(quitar);

    brls::ListItem* adaptar = new brls::ListItem("Usar el ícono original adaptado al tema", "", "Para que no salga estirado con un tema vertical");
    adaptar->getClickEvent()->subscribe([tid, nombre, adaptar](brls::View*) {
        if (useOriginalIconAdapted(tid, nombre))
        {
            adaptar->setValue("hecho");
            g_genIconos++;
        }
    });
    list->addView(adaptar);

    list->addView(new brls::Header("Estilo de los íconos"));
    for (int i = 0; i <= (int)ESTILOS.size(); ++i)
    {
        brls::ListItem* item = new brls::ListItem(i < (int)ESTILOS.size() ? ESTILOS[i] : std::string("Imágenes de tu SD"));
        if (i == this->chip) item->setValue("actual");
        item->getClickEvent()->subscribe([this, i](brls::View*) {
            luego([this, i] {
                brls::Application::popView();
                this->chip = i;
                this->buscar();
            });
        });
        list->addView(item);
    }

    if (coincidencias.size() > 1)
    {
        list->addView(new brls::Header("Otro juego de SteamGridDB"));
        for (auto& c : coincidencias)
        {
            brls::ListItem* item = new brls::ListItem(c.first);
            const std::string id = c.second, n = c.first;
            item->getClickEvent()->subscribe([this, id, n](brls::View*) {
                luego([this, id, n] {
                    brls::Application::popView();
                    this->juegoId = id;
                    this->termino = n;
                    this->buscar();
                });
            });
            list->addView(item);
        }
    }
    frame->setContentView(list);
    brls::Application::pushView(frame);
}

void VistaIconos::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx)
{
    // Terminó de poner el ícono: aviso y, si se pidió, el ajuste del recorte.
    if (this->aplicando && this->aplicando->fase == 2)
    {
        auto ap = this->aplicando;
        this->aplicando.reset();
        if (ap->recorte && ap->ok)
            brls::Application::pushView(new MarcoCompleto(new VistaRecorte(this->tid, this->nombre, ap->imagen)));
        else
            avisar(ap->mensaje);
        if (this->actual > 0) nvgDeleteImage(vg, this->actual);
        this->actual = -1;
    }

    const float panel = 360.0f;
    const float lx = x + 36.0f, lw = width - panel - 60.0f;
    float ly       = y + 30.0f;
    titulo(this, vg, lx, ly, this->nombre.size() > 22 ? this->nombre.substr(0, 22) : this->nombre, ctx);
    ly += 52.0f;

    std::lock_guard<std::mutex> l(this->busqueda->m);
    Busqueda& b = *this->busqueda;
    std::string sub;
    if (b.fase == 0) sub = "Buscando en SteamGridDB…";
    else if (this->chip == (int)ESTILOS.size()) sub = std::to_string(b.res.size()) + " imágenes en tu SD · \uE0F0 cambia el estilo";
    else sub = std::to_string(b.res.size()) + " íconos de «" + (b.juegoNombre.empty() ? this->termino : b.juegoNombre) + "» · estilo " + ESTILOS[this->chip] + " (\uE0F0 lo cambia)";
    nvgFontSize(vg, 16.0f);
    texto(this, vg, lx, ly, cortar(vg, sub, lw), 16.0f, ctx->theme->descriptionColor);
    ly += 34.0f;

    const std::vector<int> vis = this->visibles();
    {
        int n[3] = { 0, 0, (int)b.res.size() };
        for (auto& r : b.res)
        {
            const int f = formaDe(r.w, r.h);
            if (f == 0 || f == 1) n[f]++;
        }
        std::vector<std::string> nombresChips;
        for (int i = 0; i < 3; ++i) nombresChips.push_back(FORMAS[i] + " · " + std::to_string(n[i]));
        ly = chips(this, vg, lx, ly, lw, nombresChips, this->forma, ctx);
    }
    if (b.fase == 1 && vis.empty() && !b.res.empty())
    {
        b.error = std::string("No hay íconos ") + (this->forma == 0 ? "cuadrados" : "verticales") + " con este estilo. Cambia de forma con L/R o prueba otro estilo (\uE0F0).";
    }

    if ((b.fase == 2 && b.res.empty()) || (b.fase == 1 && vis.empty() && !b.res.empty()))
    {
        nvgFontSize(vg, 17.0f);
        nvgTextLineHeight(vg, 1.35f);
        nvgFillColor(vg, this->a(ctx->theme->descriptionColor));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgBeginPath(vg);
        nvgTextBox(vg, lx, ly + 10.0f, lw, b.error.c_str(), nullptr);
    }

    const bool vertical = esTemaVertical();
    this->columnas = vertical ? 6 : 5;
    const float gap = 14.0f;
    const float tam = (lw - gap * (this->columnas - 1)) / this->columnas;
    const float alto = vertical ? tam * 1.5f : tam;
    const int filasVisibles = std::max(1, (int)((y + height - 6.0f - ly) / (alto + gap)));
    if (this->elegido >= (int)vis.size()) this->elegido = std::max(0, (int)vis.size() - 1);
    ajustarScroll(this->elegido, this->columnas, filasVisibles, this->primeraFila);
    nvgSave(vg);
    nvgScissor(vg, x, ly - 6, lw + 60.0f, y + height - ly + 6);
    int cargadas = 0;
    for (int k = this->primeraFila * this->columnas; k < (int)vis.size() && k < (this->primeraFila + filasVisibles + 1) * this->columnas; ++k)
    {
        Resultado& r = b.res[vis[k]];
        if (r.img < 0 && r.listo && cargadas < 2)
        {
            r.img = nvgCreateImage(vg, r.thumb.c_str(), 0);
            if (r.img <= 0) r.img = 0;
            ++cargadas;
        }
        const int fi = k / this->columnas - this->primeraFila, co = k % this->columnas;
        loseta(this, vg, lx + co * (tam + gap), ly + fi * (alto + gap), tam, alto, r.img, "", k == this->elegido, this->isFocused(), ctx, 1.0f, true);
    }
    nvgRestore(vg);

    // Panel: así se va a ver.
    const float fx = x + width - panel, px = fx + 28.0f, pw = panel - 56.0f;
    nvgBeginPath(vg);
    nvgRect(vg, fx, y, panel, height);
    nvgFillColor(vg, this->a(oqb::velo(oqb::oscuro() ? 8 : 6)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRect(vg, fx, y, 1, height);
    nvgFillColor(vg, this->a(ctx->theme->sidebarSeparatorColor));
    nvgFill(vg);
    float py = y + 30.0f;

    const int real = this->elegido < (int)vis.size() ? vis[this->elegido] : -1;
    const bool hay = real >= 0 && b.res[real].listo;
    if (hay && this->previaDe != real)
    {
        this->liberarPrevias(vg);
        const nlohmann::json c = loadConfig();
        const int fit = c.value("fit_mode", 0), chico = c.value("small_mode", 1);
        this->temaVertical = fit == 3;
        auto g = previewRGBA(b.res[real].thumb, fit, 256);
        if (!g.empty()) this->previaGrande = nvgCreateImageRGBA(vg, 256, 256, 0, g.data());
        if (chico > 0)
        {
            auto s = previewRGBA(b.res[real].thumb, chico == 2 ? fit : 0, 174);
            if (!s.empty()) this->previaChica = nvgCreateImageRGBA(vg, 174, 174, 0, s.data());
        }
        this->previaDe = real;
    }
    if (this->actual < 0)
    {
        auto datos   = currentIconJpeg(this->tid);
        this->actual = imagenDeBytes(vg, datos);
    }

    if (hay)
    {
        /* Solo lo de tu tema (pestaña Tema). Normal: el HOME cuadrado y el
           chico recortado de Todas las apps. HOME vertical: el HOME como lo
           estira el tema y, en Todas las apps, el ícono original del juego. */
        texto(this, vg, px, py, this->temaVertical ? "Así se va a ver (tema con HOME vertical)" : "Así se va a ver (tema normal)", 15.0f, ctx->theme->descriptionColor);
        py += 30.0f;
        const int grande = this->previaGrande > 0 ? this->previaGrande : 0;
        if (this->temaVertical)
        {
            if (this->original < 0)
            {
                auto datos     = originalIconJpeg(this->tid);
                this->original = imagenDeBytes(vg, datos);
            }
            loseta(this, vg, px + 10, py, 107, 160, grande, "", false, false, ctx);
            loseta(this, vg, px + 170, py + 96, 64, 64, this->original, this->nombre, false, false, ctx);
            texto(this, vg, px + 63, py + 170, "HOME", 13.0f, ctx->theme->descriptionColor, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            texto(this, vg, px + 202, py + 170, "Todas las apps", 13.0f, ctx->theme->descriptionColor, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        }
        else
        {
            loseta(this, vg, px, py + 16, 144, 144, grande, "", false, false, ctx);
            loseta(this, vg, px + 170, py + 96, 64, 64, this->previaChica > 0 ? this->previaChica : grande, "", false, false, ctx);
            texto(this, vg, px + 72, py + 170, "HOME", 13.0f, ctx->theme->descriptionColor, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
            texto(this, vg, px + 202, py + 170, "Todas las apps", 13.0f, ctx->theme->descriptionColor, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        }
        py += 200.0f;

        const Resultado& r = b.res[real];
        auto dato = [&](const std::string& etiqueta, const std::string& valor) {
            if (valor.empty()) return;
            texto(this, vg, px, py, etiqueta, 15.0f, ctx->theme->descriptionColor);
            nvgFontSize(vg, 15.0f);
            texto(this, vg, px + 96, py, cortar(vg, valor, pw - 96), 15.0f, ctx->theme->textColor);
            py += 26.0f;
        };
        if (r.local.empty())
        {
            std::string autor;
            if (r.icono.contains("author") && r.icono["author"].is_object()) autor = jstr(r.icono["author"], "name");
            dato("Autor", autor);
            dato("Medida", std::to_string(jint(r.icono, "width")) + "x" + std::to_string(jint(r.icono, "height")));
            dato("Estilo", jstr(r.icono, "style"));
        }
        else
            dato("Archivo", std::filesystem::path(r.local).filename().string());
    }
    else
    {
        texto(this, vg, px, py, "Tu ícono de ahora", 15.0f, ctx->theme->descriptionColor);
        py += 30.0f;
        loseta(this, vg, px, py, 128, 128, this->actual, this->nombre, false, false, ctx);
        py += 150.0f;
    }

    // Estado del ícono de este juego.
    {
        const EstadoIcono e = iconState(this->tid);
        std::string t = e == EstadoIcono::Original ? "Tiene su ícono original" : (e == EstadoIcono::Propio ? "Tiene un ícono tuyo" : "Su ícono no entra en el HOME");
        texto(this, vg, px, py + 6, t, 15.0f, e == EstadoIcono::NoEntra ? oqb::doradoTexto() : (e == EstadoIcono::Propio ? oqb::verdeTexto() : ctx->theme->descriptionColor));
    }

    const float bh = 46.0f, by = y + height - 24.0f - bh;
    if (this->aplicando && this->aplicando->fase == 1)
    {
        texto(this, vg, px + pw / 2, by - 30, "Poniendo el ícono…", 16.0f, ctx->theme->textColor, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        const float t = std::fmod(std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count(), 1.2f) / 1.2f;
        barra(this, vg, px, by + 18, pw, t);
    }
    else if (hay)
    {
        boton(this, vg, px, by - bh - 10, pw, bh, " Ajustar recorte", false, ctx);
        boton(this, vg, px, by, pw, bh, " Poner este ícono", true, ctx);
    }
    else
        boton(this, vg, px, by, pw, bh, " Más opciones", false, ctx);
}

// ------------------------------------------------------- recorte ---------

namespace
{
const std::vector<std::string> MODOS = { "Recortar", "Con franjas", "Estirar", "Tema vertical" };
}

void frame_cropAdjust(const std::string& tid, const std::string& gameName, const std::string& imagePath)
{
    brls::Application::pushView(new MarcoCompleto(new VistaRecorte(tid, gameName, imagePath)));
}

VistaRecorte::VistaRecorte(const std::string& tid, const std::string& nombre, const std::string& ruta)
    : tid(tid)
    , nombre(nombre)
    , ruta(ruta)
{
    this->rgb  = loadRGB(ruta, this->W, this->H);
    this->modo = std::max(0, std::min(3, loadConfig().value("fit_mode", 0)));

    this->registerAction("", brls::Key::DUP, [this] { this->mover(0, -0.05f); return true; }, true);
    this->registerAction("", brls::Key::DDOWN, [this] { this->mover(0, 0.05f); return true; }, true);
    this->registerAction("", brls::Key::DLEFT, [this] { this->mover(-0.05f, 0); return true; }, true);
    this->registerAction("", brls::Key::DRIGHT, [this] { this->mover(0.05f, 0); return true; }, true);
    this->registerAction("", brls::Key::L, [this] {
        this->modo  = (this->modo + 3) % 4;
        this->sucio = true;
        return true;
    }, true);
    this->registerAction("Modo", brls::Key::R, [this] {
        this->modo  = (this->modo + 1) % 4;
        this->sucio = true;
        return true;
    });
    this->registerAction("Acercar", brls::Key::X, [this] {
        this->zoom  = std::max(0.35f, this->zoom - 0.1f);
        this->sucio = true;
        return true;
    });
    this->registerAction("Alejar", brls::Key::Y, [this] {
        this->zoom  = std::min(1.0f, this->zoom + 0.1f);
        this->sucio = true;
        return true;
    });
    this->registerAction("Centrar", brls::Key::MINUS, [this] {
        this->zoom = 1.0f;
        this->px = this->py = 0.5f;
        this->sucio = true;
        return true;
    });
    this->registerAction("Poner así", brls::Key::A, [this] {
        this->aplicar();
        return true;
    });
}

VistaRecorte::~VistaRecorte()
{
    NVGcontext* vg = brls::Application::getNVGContext();
    for (int t : { this->img, this->previaGrande, this->previaChica })
        if (t > 0 && vg) nvgDeleteImage(vg, t);
}

// El área de la imagen que se usa, en píxeles. Recortar: un cuadrado. Tema
// vertical: un 2:3 (el tema lo estira a lo alto). Con franjas y Estirar usan
// la imagen entera.
void VistaRecorte::region(int& rx, int& ry, int& rw, int& rh) const
{
    rw = this->W;
    rh = this->H;
    if (this->modo == 0)
        rw = rh = (int)(std::min(this->W, this->H) * this->zoom);
    else if (this->modo == 3)
    {
        const float a = 2.0f / 3.0f;
        if ((float)this->W / this->H > a)
        {
            rh = this->H;
            rw = (int)(this->H * a);
        }
        else
        {
            rw = this->W;
            rh = (int)(this->W / a);
        }
        rw = (int)(rw * this->zoom);
        rh = (int)(rh * this->zoom);
    }
    rw = std::max(1, std::min(rw, this->W));
    rh = std::max(1, std::min(rh, this->H));
    rx = (int)((this->W - rw) * this->px);
    ry = (int)((this->H - rh) * this->py);
}

std::vector<unsigned char> VistaRecorte::recorte(int& w, int& h) const
{
    int rx, ry;
    this->region(rx, ry, w, h);
    std::vector<unsigned char> out((size_t)w * h * 3);
    for (int yy = 0; yy < h; ++yy)
        memcpy(out.data() + (size_t)yy * w * 3, this->rgb.data() + ((size_t)(ry + yy) * this->W + rx) * 3, (size_t)w * 3);
    return out;
}

void VistaRecorte::mover(float dx, float dy)
{
    if (this->modo == 1 || this->modo == 2) return;  // usan la imagen entera
    this->px    = std::max(0.0f, std::min(1.0f, this->px + dx));
    this->py    = std::max(0.0f, std::min(1.0f, this->py + dy));
    this->sucio = true;
}

void VistaRecorte::aplicar()
{
    if (this->rgb.empty() || (this->aplicando && this->aplicando->fase == 1)) return;
    int w, h;
    auto trozo = std::make_shared<std::vector<unsigned char>>(this->recorte(w, h));
    auto ap    = std::make_shared<Aplicando>();
    ap->fase   = 1;
    this->aplicando = ap;
    const std::string tid = this->tid;
    // Lo recortado ya tiene la forma final: cuadrado o 2:3.
    const int modoFinal = this->modo == 0 ? 0 : this->modo;
    lanzar([ap, trozo, w, h, tid, modoFinal] {
        g_silencio = true;
        // Se guarda (no es temporal): es la imagen de la que sale el ícono, para rehacerlo si cambias de tema.
        const std::string temp = "sdmc:/iconos/recortes/" + tid + ".png";
        if (!writePngRGB(temp, w, h, trozo->data()))
        {
            ap->mensaje = "No se pudo preparar el recorte. Probablemente la SD está llena.";
            ap->fase    = 2;
            return;
        }
        ap->ok = overwriteIcon(tid, temp, modoFinal);
        if (ap->ok)
        {
            std::error_code ec;
            auto g = std::filesystem::file_size(iconPathFor(tid), ec);
            ap->mensaje = "Listo: grande " + kb(g) + ". Entra en el HOME.\nSi el HOME sigue mostrando el anterior, reinicia.";
        }
        else
            ap->mensaje = "No se pudo poner: probablemente no entra en el HOME ni con menos calidad. Prueba acercando o con otra imagen.";
        g_genIconos++;
        ap->fase = 2;
    });
}

void VistaRecorte::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx)
{
    if (this->aplicando && this->aplicando->fase == 2)
    {
        auto ap = this->aplicando;
        this->aplicando.reset();
        avisar(ap->mensaje);
        if (ap->ok)
        {
            brls::Application::popView();
            return;
        }
    }

    const float panel = 360.0f;
    const float lx = x + 36.0f, lw = width - panel - 60.0f;
    float ly       = y + 30.0f;
    titulo(this, vg, lx, ly, "Ajustar el ícono", ctx);
    ly += 52.0f;
    nvgFontSize(vg, 16.0f);
    texto(this, vg, lx, ly, cortar(vg, this->nombre + " · L/R cambia el modo", lw), 16.0f, ctx->theme->descriptionColor);
    ly += 34.0f;
    ly = chips(this, vg, lx, ly, lw, MODOS, this->modo, ctx);

    if (this->rgb.empty())
    {
        texto(this, vg, lx, ly + 10, "No se pudo leer la imagen. Probablemente se descargó a medias: vuelve atrás y prueba otra vez.", 17.0f, ctx->theme->descriptionColor);
        return;
    }

    // La imagen entera, con el área que se usa marcada y el resto oscurecido.
    if (this->img < 0)
    {
        this->img = nvgCreateImage(vg, this->ruta.c_str(), 0);
        if (this->img <= 0) this->img = 0;
    }
    const float cajaW = lw, cajaH = y + height - 60.0f - ly;
    const float esc   = std::min(cajaW / this->W, cajaH / this->H);
    const float iw = this->W * esc, ih = this->H * esc;
    const float ix = lx + (cajaW - iw) / 2, iy = ly;
    if (this->img > 0)
    {
        nvgBeginPath(vg);
        nvgRect(vg, ix, iy, iw, ih);
        nvgFillPaint(vg, nvgImagePattern(vg, ix, iy, iw, ih, 0, this->img, this->getAlpha()));
        nvgFill(vg);
    }
    int rx, ry, rw, rh;
    this->region(rx, ry, rw, rh);
    const float ax = ix + rx * esc, ay = iy + ry * esc, aw = rw * esc, ah = rh * esc;
    nvgFillColor(vg, this->a(nvgRGBA(0, 0, 0, 160)));
    nvgBeginPath(vg);
    nvgRect(vg, ix, iy, iw, ay - iy);
    nvgRect(vg, ix, ay + ah, iw, iy + ih - (ay + ah));
    nvgRect(vg, ix, ay, ax - ix, ah);
    nvgRect(vg, ax + aw, ay, ix + iw - (ax + aw), ah);
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRect(vg, ax, ay, aw, ah);
    nvgStrokeColor(vg, this->a(oqb::esmeralda()));
    nvgStrokeWidth(vg, 3.0f);
    nvgStroke(vg);

    const bool mueve = this->modo == 0 || this->modo == 3;
    texto(this, vg, lx, y + height - 46.0f,
          mueve ? "Cruceta mueve el área · \uE0E2 acerca · \uE0E3 aleja · \uE0F0 centra" : "Este modo usa la imagen entera.", 15.0f, ctx->theme->descriptionColor);

    // Panel: cómo queda.
    const float fx = x + width - panel, px0 = fx + 28.0f, pw = panel - 56.0f;
    nvgBeginPath(vg);
    nvgRect(vg, fx, y, panel, height);
    nvgFillColor(vg, this->a(oqb::velo(oqb::oscuro() ? 8 : 6)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRect(vg, fx, y, 1, height);
    nvgFillColor(vg, this->a(ctx->theme->sidebarSeparatorColor));
    nvgFill(vg);

    if (this->sucio)
    {
        this->sucio = false;
        for (int* t : { &this->previaGrande, &this->previaChica })
        {
            if (*t > 0) nvgDeleteImage(vg, *t);
            *t = -1;
        }
        int w, h;
        auto trozo = this->recorte(w, h);
        auto g     = renderRGBA(trozo.data(), w, h, this->modo, 256);
        if (!g.empty()) this->previaGrande = nvgCreateImageRGBA(vg, 256, 256, 0, g.data());
        const int chico = loadConfig().value("small_mode", 1);
        if (chico > 0)
        {
            auto c = renderRGBA(trozo.data(), w, h, chico == 2 ? this->modo : 0, 174);
            if (!c.empty()) this->previaChica = nvgCreateImageRGBA(vg, 174, 174, 0, c.data());
        }
    }

    float py0 = y + 30.0f;
    texto(this, vg, px0, py0, "Así se va a ver", 15.0f, ctx->theme->descriptionColor);
    py0 += 46.0f;
    const int grande = this->previaGrande > 0 ? this->previaGrande : 0;
    loseta(this, vg, px0, py0, 150, 150, grande, "", false, false, ctx);
    texto(this, vg, px0 + 75, py0 + 160, "HOME", 13.0f, ctx->theme->descriptionColor, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    loseta(this, vg, px0 + 170, py0 - 20, 100, 150 * 1.0f + 0, grande, "", false, false, ctx);
    texto(this, vg, px0 + 220, py0 + 160, "Tema vertical", 13.0f, ctx->theme->descriptionColor, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    py0 += 200.0f;
    if (this->previaChica > 0)
    {
        loseta(this, vg, px0, py0, 80, 80, this->previaChica, "", false, false, ctx);
        texto(this, vg, px0 + 96, py0 + 30, "Chico (lista completa)", 14.0f, ctx->theme->descriptionColor);
    }
    else
        texto(this, vg, px0, py0, "El ícono chico se deja el del juego (Ajustes).", 14.0f, ctx->theme->descriptionColor);

    const float bh = 46.0f, by = y + height - 24.0f - bh;
    if (this->aplicando && this->aplicando->fase == 1)
    {
        texto(this, vg, px0 + pw / 2, by - 30, "Poniendo el ícono…", 16.0f, ctx->theme->textColor, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        const float t = std::fmod(std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count(), 1.2f) / 1.2f;
        barra(this, vg, px0, by + 18, pw, t);
    }
    else
        boton(this, vg, px0, by, pw, bh, "\uE0E0 Poner así", true, ctx);
}

// --------------------------------------------------------------- tema -----

/* Cómo se ven los íconos según el tema de la consola. Dos opciones:
   - normal (cuadrado): tu ícono cuadrado en el HOME y en Todas las apps;
   - HOME vertical: vertical solo en el HOME; Todas las apps se queda con el
     cuadrado del juego, que es como lo dibuja el sistema con esos temas.
   Detrás son los dos ajustes de siempre (fit_mode y small_mode). */
VistaTema::VistaTema()
{
    this->registerAction("", brls::Key::DUP, [this] { this->elegido = 0; return true; }, true);
    this->registerAction("", brls::Key::DDOWN, [this] { this->elegido = 1; return true; }, true);
    this->registerAction("Usar este", brls::Key::A, [this] {
        this->elegir(this->elegido);
        return true;
    });
    this->elegido = this->actual();
}

int VistaTema::actual() const
{
    const nlohmann::json c = loadConfig();
    return c.value("fit_mode", 0) == 3 && c.value("small_mode", 1) == 0 ? 1 : (c.value("fit_mode", 0) == 0 && c.value("small_mode", 1) == 1 ? 0 : -1);
}

/* Cambiar de tema deja los íconos limpios: los que tienes están hechos para
   el otro tema y se verían mal. Antes se ofrece guardarlos en un perfil, así
   volver es aplicar ese perfil (que trae también su tema). */
void VistaTema::elegir(int cual)
{
    if (cual == this->actual()) return;
    auto cambiarTema = [cual] {
        nlohmann::json c = loadConfig();
        c["fit_mode"]    = cual == 1 ? 3 : 0;  // tema vertical / recortar
        c["small_mode"]  = cual == 1 ? 0 : 1;  // el chico: el del juego / recortado
        saveConfig(c);
    };
    int propios = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("sdmc:/atmosphere/contents/", ec))
        if (!ec && hasCustomIcon(e.path().filename().string())) propios++;
    if (propios == 0)
    {
        cambiarTema();
        avisar(cual == 1 ? "Listo: busca íconos verticales; saldrán verticales en el HOME." : "Listo: busca íconos cuadrados; saldrán cuadrados en todas partes.");
        return;
    }

    // Nombre libre para el perfil del tema que se deja: «Cuadrado 1», «Vertical 2»...
    const std::string base = cual == 1 ? "Cuadrado" : "Vertical";
    int n = 1;
    while (std::filesystem::exists("sdmc:/iconos/perfiles/" + base + " " + std::to_string(n), ec)) n++;
    const std::string perfil = base + " " + std::to_string(n);

    const std::string texto = std::to_string(propios) + (propios == 1 ? " ícono tuyo está hecho" : " íconos tuyos están hechos") +
                              " para el otro tema.\n\nAl cambiar se quitan, para que empieces de cero con " +
                              (cual == 1 ? "íconos verticales." : "íconos cuadrados.") + "\n\n¿Los guardo antes en el perfil «" + perfil + "»?";
    auto trabajo = [cambiarTema, perfil](bool guardar) {
        return [cambiarTema, perfil, guardar](std::function<void(int, int)>) {
            auto iconos = [](int n) { return std::to_string(n) + (n == 1 ? " ícono" : " íconos"); };
            std::string r;
            if (guardar) r = "Guardé " + iconos(saveProfile(perfil)) + " en el perfil «" + perfil + "».\n";
            const int quitados = removeAllIcons();
            cambiarTema();
            return r + "Quité " + iconos(quitados) + ". Ya puedes buscar íconos para el tema nuevo.";
        };
    };
    brls::Dialog* d = new brls::Dialog(texto);
    d->addButton("Guardar y cambiar", [d, trabajo](brls::View*) {
        d->close([trabajo] { enSegundoPlano("Cambiar de tema", "Se guardan tus íconos en un perfil y se quitan.", trabajo(true), true); });
    });
    d->addButton("Cambiar sin guardar", [d, trabajo](brls::View*) {
        d->close([trabajo] { enSegundoPlano("Cambiar de tema", "Se quitan tus íconos para empezar de cero.", trabajo(false), true); });
    });
    d->addButton("Cancelar", [d](brls::View*) { d->close(); });
    d->setCancelable(true);
    d->open();
}

void VistaTema::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx)
{
    const float lx = x + 36.0f, lw = width - 72.0f;
    float ly       = y + 30.0f;
    titulo(this, vg, lx, ly, "Tema", ctx);
    ly += 52.0f;
    texto(this, vg, lx, ly, "Elige cómo es el tema de tu consola: así tus íconos salen bien en el HOME y en Todas las apps.", 16.0f, ctx->theme->descriptionColor);
    ly += 40.0f;

    const int enUso = this->actual();
    const char* titulos[2] = { "Tema normal (cuadrado)", "Tema con HOME vertical" };
    const char* detalles[2] = {
        "Lo normal. Tus íconos salen cuadrados, tal como los eliges, en el HOME y en Todas las apps.",
        "Solo si tu tema muestra el HOME en vertical. Tus íconos se preparan en 2:3 para el HOME y Todas las apps se queda con los cuadrados originales, como los dibuja el sistema.",
    };
    const float ch = (y + height - 70.0f - ly - 18.0f) / 2;
    for (int i = 0; i < 2; ++i)
    {
        const float cy = ly + i * (ch + 18.0f);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, lx, cy, lw, ch, 18.0f);
        nvgFillColor(vg, this->a(oqb::velo(i == enUso ? 14 : 8)));
        nvgFill(vg);
        if (i == this->elegido && this->isFocused())
        {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, lx - 3, cy - 3, lw + 6, ch + 6, 20.0f);
            nvgStrokeColor(vg, this->a(ctx->theme->highlightColor1));
            nvgStrokeWidth(vg, 3.0f);
            nvgStroke(vg);
        }
        // Dibujo: tres del HOME y una fila de Todas las apps.
        const float dx = lx + 26.0f, dy = cy + 26.0f;
        const float hw = 54.0f, hh = i == 1 ? 81.0f : 54.0f;
        for (int k = 0; k < 3; ++k)
        {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, dx + k * (hw + 10), dy + (81.0f - hh), hw, hh, 8.0f);
            nvgFillColor(vg, this->a(oqb::esmeralda()));
            nvgFill(vg);
        }
        texto(this, vg, dx + (3 * hw + 20) / 2, dy + 90.0f, "HOME", 13.0f, ctx->theme->descriptionColor, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        const float ax = dx + 3 * (hw + 10) + 30.0f;
        for (int k = 0; k < 4; ++k)
        {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, ax + k * 40.0f, dy + 45.0f, 34.0f, 34.0f, 6.0f);
            nvgFillColor(vg, this->a(i == 1 ? oqb::velo(40) : oqb::esmeralda()));
            nvgFill(vg);
        }
        texto(this, vg, ax + 77.0f, dy + 90.0f, i == 1 ? "Todas las apps: originales" : "Todas las apps: tuyos", 13.0f, ctx->theme->descriptionColor, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);

        const float tx = ax + 200.0f, tw = lx + lw - 24.0f - tx;
        texto(this, vg, tx, cy + 28.0f, titulos[i], 22.0f, ctx->theme->textColor);
        if (i == enUso)
        {
            nvgFontSize(vg, 22.0f);
            const float fin = tx + nvgTextBounds(vg, 0, 0, titulos[i], nullptr, nullptr) + 14.0f;
            nvgFontSize(vg, 13.0f);
            const float pw = nvgTextBounds(vg, 0, 0, "En uso", nullptr, nullptr) + 18.0f;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, fin, cy + 30.0f, pw, 24.0f, 12.0f);
            nvgFillColor(vg, this->a(nvgRGBA(52, 213, 131, 46)));
            nvgFill(vg);
            texto(this, vg, fin + pw / 2, cy + 42.0f, "En uso", 13.0f, oqb::verdeTexto(), NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        }
        nvgFontSize(vg, 16.0f);
        nvgTextLineHeight(vg, 1.35f);
        nvgFillColor(vg, this->a(ctx->theme->descriptionColor));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgBeginPath(vg);
        nvgTextBox(vg, tx, cy + 66.0f, tw, detalles[i], nullptr);
    }
    if (enUso < 0)
        texto(this, vg, lx, y + height - 44.0f, "Ahora tienes una combinación propia (Ajustes › Avanzado). Elegir una de estas la reemplaza.", 15.0f, oqb::doradoTexto());
    else
        texto(this, vg, lx, y + height - 44.0f, "Al cambiar de tema se quitan tus íconos para empezar de cero (antes te ofrece guardarlos en un perfil).", 15.0f, ctx->theme->descriptionColor);
}

// --------------------------------------------------- íconos para todos ----

VistaLote::VistaLote()
{
    this->estilo = std::max(0, std::min((int)ESTILOS.size() - 1, loadConfig().value("style_id", 0)));
    this->forma  = formaInicial();
    this->buscar();
    auto total = [this] {
        std::lock_guard<std::mutex> l(this->st->m);
        return (int)this->st->props.size();
    };
    this->registerAction("", brls::Key::DUP, [this, total] { mover(this->elegido, total(), this->columnas, 0, -1); return true; }, true);
    this->registerAction("", brls::Key::DDOWN, [this, total] { mover(this->elegido, total(), this->columnas, 0, 1); return true; }, true);
    this->registerAction("", brls::Key::DLEFT, [this, total] { mover(this->elegido, total(), this->columnas, -1, 0); return true; }, true);
    this->registerAction("", brls::Key::DRIGHT, [this, total] { mover(this->elegido, total(), this->columnas, 1, 0); return true; }, true);
    this->registerAction("", brls::Key::L, [this] {
        if (this->st->fase >= 2) return true;
        this->forma = (this->forma + 2) % 3;
        this->buscar();
        return true;
    }, true);
    this->registerAction("Forma", brls::Key::R, [this] {
        if (this->st->fase >= 2) return true;
        this->forma = (this->forma + 1) % 3;
        this->buscar();
        return true;
    });
    this->registerAction("Quitar o dejar este", brls::Key::X, [this] {
        std::lock_guard<std::mutex> l(this->st->m);
        if (this->st->fase < 2 && this->elegido < (int)this->st->props.size())
            this->st->props[this->elegido].incluido = !this->st->props[this->elegido].incluido;
        return true;
    });
    this->registerAction("Aplicar", brls::Key::A, [this] {
        if (this->st->fase == 3)
            brls::Application::popView();
        else if (this->st->fase < 2)
            this->aplicar();
        return true;
    });
}

VistaLote::~VistaLote()
{
    this->st->cancelar = true;
    NVGcontext* vg = brls::Application::getNVGContext();
    std::lock_guard<std::mutex> l(this->st->m);
    for (auto& p : this->st->props)
        if (p.img > 0 && vg) nvgDeleteImage(vg, p.img);
}

void VistaLote::buscar()
{
    if (this->st)
    {
        this->st->cancelar = true;
        NVGcontext* vg     = brls::Application::getNVGContext();
        std::lock_guard<std::mutex> l(this->st->m);
        for (auto& p : this->st->props)
            if (p.img > 0 && vg) nvgDeleteImage(vg, p.img);
    }
    auto s   = std::make_shared<Estado>();
    this->st = s;
    this->elegido = this->primeraFila = 0;
    for (auto& g : installedGames())
    {
        const std::string tid = g["tid"].get<std::string>();
        if (hasCustomIcon(tid) || g.value("archivado", false)) continue;
        Propuesta p;
        p.tid    = tid;
        p.nombre = g["name"].get<std::string>();
        s->props.push_back(p);
    }
    s->total     = (int)s->props.size();
    const int es = this->estilo, fo = this->forma;
    lanzar([s, es, fo] {
        g_silencio = true;
        for (size_t i = 0;; ++i)
        {
            std::string nombre;
            {
                std::lock_guard<std::mutex> l(s->m);
                if (s->cancelar || g_saliendo || i >= s->props.size()) break;
                nombre = s->props[i].nombre;
            }
            nlohmann::json icono;
            std::string thumb;
            nlohmann::json found = requestGames(nombre);
            if (jbool(found, "success") && found.contains("data") && found["data"].is_array() && !found["data"].empty())
            {
                const std::string id = jstr(found["data"][0], "id");
                nlohmann::json icons = requestIcons(id, es, -1);
                if (jbool(icons, "success") && icons.contains("data") && icons["data"].is_array())
                    for (auto& it : icons["data"])
                        if (!jbool(it, "lock") && entraEnForma(fo, jint(it, "width"), jint(it, "height")))
                        {
                            icono = it;
                            break;
                        }
                if (!icono.is_null()) thumb = downloadImage(icono, true);
            }
            std::lock_guard<std::mutex> l(s->m);
            if (s->cancelar) break;
            s->props[i].icono  = icono;
            s->props[i].thumb  = thumb;
            s->props[i].estado = thumb.empty() ? 2 : 1;
            if (thumb.empty()) s->props[i].incluido = false;
            s->hechos++;
        }
        if (!s->cancelar) s->fase = 1;
    });
}

void VistaLote::aplicar()
{
    auto s = this->st;
    s->fase    = 2;
    s->hechos  = 0;
    int cuantos = 0;
    {
        std::lock_guard<std::mutex> l(s->m);
        for (auto& p : s->props) cuantos += p.incluido && p.estado == 1;
    }
    s->total = cuantos;
    lanzar([s] {
        g_silencio = true;
        for (size_t i = 0;; ++i)
        {
            Propuesta p;
            {
                std::lock_guard<std::mutex> l(s->m);
                if (s->cancelar || g_saliendo || i >= s->props.size()) break;
                p = s->props[i];
            }
            if (!p.incluido || p.estado != 1) continue;
            const std::string full = downloadImage(p.icono, false);
            if (!full.empty() && overwriteIcon(p.tid, full)) s->puestos++;
            else s->fallados++;
            s->hechos++;
        }
        g_genIconos++;
        s->fase = 3;
    });
}

void VistaLote::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx)
{
    auto s         = this->st;
    const float lx = x + 36.0f, lw = width - 72.0f;
    float ly       = y + 30.0f;
    titulo(this, vg, lx, ly, "Íconos para todos", ctx);
    ly += 52.0f;
    texto(this, vg, lx, ly, "El más votado de SteamGridDB para cada juego que tiene su ícono original. X quita o deja uno; los tuyos no se tocan.", 16.0f, ctx->theme->descriptionColor);
    ly += 34.0f;
    ly = chips(this, vg, lx, ly, lw, FORMAS, this->forma, ctx);

    std::lock_guard<std::mutex> l(s->m);
    const bool vertical = esTemaVertical();
    const float gap = 12.0f;
    const float tam = (lw - gap * (this->columnas - 1)) / this->columnas;
    const float alto = vertical ? tam * 1.5f : tam;
    const float fila = alto + 30.0f;
    const float abajo = 70.0f;
    const int filasVisibles = std::max(1, (int)((y + height - abajo - ly) / fila));
    if (this->elegido >= (int)s->props.size()) this->elegido = std::max(0, (int)s->props.size() - 1);
    ajustarScroll(this->elegido, this->columnas, filasVisibles, this->primeraFila);

    if (s->props.empty())
        texto(this, vg, lx, ly + 10, "Todos tus juegos ya tienen un ícono propio.", 18.0f, ctx->theme->descriptionColor);

    nvgSave(vg);
    nvgScissor(vg, x, ly - 6, width, filasVisibles * fila + 6);
    int cargadas = 0;
    for (int k = this->primeraFila * this->columnas; k < (int)s->props.size() && k < (this->primeraFila + filasVisibles) * this->columnas; ++k)
    {
        Propuesta& p = s->props[k];
        if (p.img < 0 && p.estado == 1 && cargadas < 2)
        {
            p.img = nvgCreateImage(vg, p.thumb.c_str(), 0);
            if (p.img <= 0) p.img = 0;
            ++cargadas;
        }
        const int fi = k / this->columnas - this->primeraFila, co = k % this->columnas;
        const float tx = lx + co * (tam + gap), ty = ly + fi * fila;
        loseta(this, vg, tx, ty, tam, alto, p.img > 0 ? p.img : 0, p.nombre, k == this->elegido, this->isFocused(), ctx, p.incluido ? 1.0f : 0.3f, true);
        if (p.estado == 2) pastilla(this, vg, tx + tam - 4, ty + 4, "Sin ícono", 0);
        else if (!p.incluido) pastilla(this, vg, tx + tam - 4, ty + 4, "Se deja", 0);
        nvgFontSize(vg, 12.0f);
        texto(this, vg, tx + tam / 2, ty + alto + 6, cortar(vg, p.nombre, tam), 12.0f, ctx->theme->descriptionColor, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    }
    nvgRestore(vg);

    const float by = y + height - abajo + 14.0f;
    int incluidos  = 0;
    for (auto& p : s->props) incluidos += p.incluido && p.estado == 1;
    std::string estado;
    float avance = 0;
    if (s->fase == 0)
    {
        estado = "Buscando " + std::to_string(s->hechos) + " de " + std::to_string(s->total) + "…";
        avance = s->total ? (float)s->hechos / s->total : 0;
    }
    else if (s->fase == 1)
    {
        estado = std::to_string(incluidos) + " íconos listos para poner ·  Aplicar a " + std::to_string(incluidos);
        avance = 1;
    }
    else if (s->fase == 2)
    {
        estado = "Poniendo " + std::to_string(s->hechos) + " de " + std::to_string(s->total) + "… Puedes esperar aquí.";
        avance = s->total ? (float)s->hechos / s->total : 0;
    }
    else
    {
        estado = "Listo: " + std::to_string(s->puestos) + " íconos puestos" + (s->fallados ? ", " + std::to_string(s->fallados) + " no se pudieron" : "") + ".  Volver";
        avance = 1;
        if (!this->avisado)
        {
            this->avisado = true;
            askReboot("Íconos puestos en " + std::to_string(s->puestos) + " juegos");
        }
    }
    barra(this, vg, lx, by, lw, avance);
    texto(this, vg, lx, by + 18, estado, 16.0f, ctx->theme->textColor);
}

// ------------------------------------------------------- token por QR -----

namespace
{
constexpr int PUERTO = 8123;

std::string urlDecode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size())
        {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        }
        else out += s[i];
    }
    return out;
}

const char* PAGINA_INICIO = R"(<!doctype html><html lang="es"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>IconGrabber OQB</title>
<style>body{margin:0;background:#0b0f0a;color:#eef2e3;font-family:system-ui,sans-serif;padding:24px}h1{font-size:22px;margin:0 0 8px}p{color:#c9d1c2;line-height:1.5}
input{width:100%;box-sizing:border-box;padding:14px;border-radius:12px;border:1px solid #2a3327;background:#121711;color:#eef2e3;font-size:16px;margin:12px 0}
button{width:100%;padding:14px;border:0;border-radius:12px;background:#34d583;color:#06140c;font-size:17px;font-weight:600}a{color:#9dea9a}</style></head><body>
<h1>Token de SteamGridDB</h1><p>1. Entra a <a href="https://www.steamgriddb.com/profile/preferences/api">steamgriddb.com › Preferencias › API</a> (cuenta gratis) y copia tu token.<br>2. Pégalo aquí y toca Guardar. Llega directo a tu Switch.</p>
<form method="post" action="/token"><input name="token" placeholder="Tu token" autocomplete="off" autocapitalize="off"><button>Guardar en la Switch</button></form></body></html>)";

std::string paginaResultado(const std::string& t, bool ok)
{
    return std::string("<!doctype html><html lang=\"es\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><style>body{margin:0;background:#0b0f0a;color:#eef2e3;font-family:system-ui,sans-serif;padding:24px}h1{font-size:22px;color:")
           + (ok ? "#9dea9a" : "#e8b84a") + "}</style></head><body><h1>" + t + "</h1><p>Ya puedes volver a la Switch.</p></body></html>";
}

void responder(int c, const std::string& html)
{
    std::string r = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " + std::to_string(html.size()) + "\r\n\r\n" + html;
    size_t enviado = 0;
    while (enviado < r.size())
    {
        ssize_t n = bsdSend(c, r.data() + enviado, r.size() - enviado, 0);
        if (n <= 0) break;
        enviado += (size_t)n;
    }
}

void atender(std::shared_ptr<VistaToken::Servidor> srv, int c)
{
    std::string req;
    char buf[2048];
    const auto inicio = std::chrono::steady_clock::now();
    size_t largo = std::string::npos, cabecera = std::string::npos;
    while (std::chrono::steady_clock::now() - inicio < std::chrono::seconds(5) && req.size() < 16384)
    {
        ssize_t n = bsdRecv(c, buf, sizeof(buf), 0);
        if (n > 0) req.append(buf, n);
        else if (n == 0) break;
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        cabecera = req.find("\r\n\r\n");
        if (cabecera == std::string::npos) continue;
        if (req.rfind("POST", 0) != 0) break;
        if (largo == std::string::npos)
        {
            std::string bajo = toLower(req.substr(0, cabecera));
            size_t p = bajo.find("content-length:");
            largo = p == std::string::npos ? 0 : strtoul(bajo.c_str() + p + 15, nullptr, 10);
        }
        if (req.size() >= cabecera + 4 + largo) break;
    }
    if (req.rfind("POST /token", 0) == 0 && cabecera != std::string::npos)
    {
        std::string cuerpo = req.substr(cabecera + 4);
        size_t p           = cuerpo.find("token=");
        std::string token  = p == std::string::npos ? "" : urlDecode(cuerpo.substr(p + 6, cuerpo.find('&', p) == std::string::npos ? std::string::npos : cuerpo.find('&', p) - p - 6));
        token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char ch) { return !std::isalnum(ch); }), token.end());
        if (token.size() < 16 || token.size() > 64)
        {
            responder(c, paginaResultado("Eso no parece un token. Cópialo completo y vuelve a intentarlo.", false));
            return;
        }
        nlohmann::json cfg = loadConfig();
        cfg["api_token"]   = token;
        saveConfig(cfg);
        nlohmann::json prueba = requestGames("zelda");
        const bool ok         = jbool(prueba, "success");
        srv->estado           = ok ? 1 : (prueba.contains("error") ? 3 : 2);
        responder(c, paginaResultado(ok ? "Listo: token guardado y funcionando" : "Token guardado, pero SteamGridDB no lo aceptó. Revisa que esté completo.", ok));
        return;
    }
    responder(c, PAGINA_INICIO);
}

void servidor(std::shared_ptr<VistaToken::Servidor> srv)
{
    g_silencio = true;
    /* Las llamadas BSD de libnx directas, sin la capa de descriptores de
       newlib: con socket()/setsockopt() la app se caía al abrir el servidor
       (Data Abort en _socketGetFd, 24-nov-2026). */
    int s = bsdSocket(AF_INET, SOCK_STREAM, 0);
    marca("servidor: socket " + std::to_string(s));
    if (s < 0)
    {
        srv->estado = 3;
        return;
    }
    int si = 1;
    bsdSetSockOpt(s, SOL_SOCKET, SO_REUSEADDR, &si, sizeof(si));
    sockaddr_in dir{};
    dir.sin_family      = AF_INET;
    dir.sin_port        = htons(PUERTO);
    dir.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bsdBind(s, (sockaddr*)&dir, sizeof(dir)) < 0 || bsdListen(s, 4) < 0)
    {
        bsdClose(s);
        srv->estado = 3;
        return;
    }
    bsdFcntl(s, F_SETFL, bsdFcntl(s, F_GETFL, 0) | O_NONBLOCK);
    marca("servidor: escuchando");
    while (!srv->parar && !g_saliendo)
    {
        int c = bsdAccept(s, nullptr, nullptr);
        if (c < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        bsdFcntl(c, F_SETFL, bsdFcntl(c, F_GETFL, 0) | O_NONBLOCK);
        atender(srv, c);
        bsdClose(c);
    }
    bsdClose(s);
}
}  // namespace

VistaToken::VistaToken()
    : srv(std::make_shared<Servidor>())
{
    this->teniaToken = !loadConfig().value("api_token", std::string()).empty();
    struct in_addr ip;
    ip.s_addr = (in_addr_t)gethostid();
    this->url = std::string("http://") + inet_ntoa(ip) + ":" + std::to_string(PUERTO) + "/";
    std::vector<uint8_t> temp(qrcodegen_BUFFER_LEN_MAX), qrbuf(qrcodegen_BUFFER_LEN_MAX);
    if (qrcodegen_encodeText(this->url.c_str(), temp.data(), qrbuf.data(), qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
    {
        this->lado = qrcodegen_getSize(qrbuf.data());
        this->qr.resize((size_t)this->lado * this->lado);
        for (int yy = 0; yy < this->lado; ++yy)
            for (int xx = 0; xx < this->lado; ++xx)
                this->qr[(size_t)yy * this->lado + xx] = qrcodegen_getModule(qrbuf.data(), xx, yy);
    }
    auto s = this->srv;
    lanzar([s] { servidor(s); });

    // Sin celular a mano: escribirlo aquí (o abrir la dirección en la PC).
    this->registerAction("Escribirlo aquí", brls::Key::Y, [this] {
        auto srv = this->srv;
        brls::Swkbd::openForText([srv](std::string t) {
            t.erase(std::remove_if(t.begin(), t.end(), [](unsigned char ch) { return !std::isalnum(ch); }), t.end());
            if (t.empty()) return;
            if (t.size() < 16 || t.size() > 64)
            {
                avisar("Eso no parece un token: cópialo completo.");
                return;
            }
            nlohmann::json cfg = loadConfig();
            cfg["api_token"]   = t;
            saveConfig(cfg);
            srv->estado = 4;  // guardado, probando
            lanzar([srv] {
                g_silencio            = true;
                nlohmann::json prueba = requestGames("zelda");
                srv->estado           = jbool(prueba, "success") ? 1 : (prueba.contains("error") ? 3 : 2);
            });
        }, "Token de SteamGridDB", "steamgriddb.com › Preferencias › API", 64, "");
        return true;
    });
}

VistaToken::~VistaToken()
{
    this->srv->parar = true;
}

void VistaToken::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx)
{
    const float lx = x + 60.0f;
    float ly       = y + 40.0f;
    titulo(this, vg, lx, ly, "Token de SteamGridDB", ctx);
    ly += 64.0f;

    const std::string pasos =
        "Con el token, la app puede buscar íconos en SteamGridDB. Es gratis y se pone una sola vez.\n\n"
        "Con el celular: escanea el código (mismo Wi-Fi que la Switch), pega tu token y toca Guardar.\n\n"
        "Con la PC: abre la dirección de abajo en el navegador y pega el token ahí.\n\n"
        "Sin nada a mano: pulsa \uE0E3 y escríbelo con el teclado de la Switch.\n\n"
        "El token se saca gratis en steamgriddb.com › Preferencias › API.";
    nvgFontSize(vg, 19.0f);
    nvgTextLineHeight(vg, 1.4f);
    nvgFillColor(vg, this->a(ctx->theme->textColor));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgBeginPath(vg);
    nvgTextBox(vg, lx, ly, 560.0f, pasos.c_str(), nullptr);

    texto(this, vg, lx, ly + 300, "Dirección: " + this->url, 20.0f, ctx->theme->textColor);

    std::string estado;
    NVGcolor color = ctx->theme->descriptionColor;
    switch (this->srv->estado.load())
    {
        case 1: estado = "Token guardado y funcionando. Ya puedes buscar íconos."; color = oqb::verdeTexto(); break;
        case 2: estado = "Se guardó, pero SteamGridDB no lo aceptó: revisa que esté completo."; color = oqb::doradoTexto(); break;
        case 3: estado = "No se pudo conectar: probablemente la Switch no está conectada al Wi-Fi."; color = oqb::doradoTexto(); break;
        case 4: estado = "Guardado. Probando con SteamGridDB…"; break;
        default:
            estado = this->teniaToken ? "Ya tienes un token guardado. Puedes reemplazarlo desde el celular." : "Esperando el token…";
    }
    texto(this, vg, lx, ly + 345, estado, 18.0f, color);

    // El QR, sobre blanco para que lo lea cualquier cámara.
    if (this->lado > 0)
    {
        const float q = 300.0f, qx = x + width - q - 90.0f, qy = y + 110.0f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, qx - 18, qy - 18, q + 36, q + 36, 16.0f);
        nvgFillColor(vg, this->a(nvgRGB(255, 255, 255)));
        nvgFill(vg);
        const float m = q / this->lado;
        nvgFillColor(vg, this->a(nvgRGB(0, 0, 0)));
        nvgBeginPath(vg);
        for (int yy = 0; yy < this->lado; ++yy)
            for (int xx = 0; xx < this->lado; ++xx)
                if (this->qr[(size_t)yy * this->lado + xx]) nvgRect(vg, qx + xx * m, qy + yy * m, m + 0.5f, m + 0.5f);
        nvgFill(vg);
    }
}

// ------------------------------------------------------ segundo plano -----

namespace
{
class VistaTrabajo : public brls::View
{
  public:
    struct Estado
    {
        std::atomic<int> hecho{ 0 }, total{ 0 };
        std::atomic<bool> termino{ false };
        std::mutex m;
        std::string resultado;
    };
    VistaTrabajo(const std::string& titulo, const std::string& detalle, Trabajo trabajo, bool reinicio)
        : tit(titulo)
        , det(detalle)
        , reinicio(reinicio)
        , st(std::make_shared<Estado>())
    {
        auto s = this->st;
        lanzar([s, trabajo] {
            g_silencio = true;
            std::string r = trabajo([s](int h, int t) {
                s->hecho = h;
                s->total = t;
            });
            g_genIconos++;
            std::lock_guard<std::mutex> l(s->m);
            s->resultado = r;
            s->termino   = true;
        });
        this->registerAction("Volver", brls::Key::B, [this] {
            if (this->st->termino) brls::Application::popView();
            return true;
        });
        this->registerAction("Volver", brls::Key::A, [this] {
            if (this->st->termino) brls::Application::popView();
            return true;
        }, true);
        if (reinicio)
            this->registerAction("Reiniciar ahora", brls::Key::Y, [this] {
                if (!this->st->termino) return true;
                if (R_SUCCEEDED(spsmInitialize()))
                {
                    spsmShutdown(true);
                    spsmExit();
                }
                return true;
            });
    }
    brls::View* getDefaultFocus() override { return this; }
    void layout(NVGcontext*, brls::Style*, brls::FontStash*) override {}
    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx) override
    {
        const float lx = x + 60.0f, lw = width - 120.0f;
        titulo(this, vg, lx, y + 40.0f, this->tit, ctx);
        texto(this, vg, lx, y + 104.0f, this->det, 18.0f, ctx->theme->descriptionColor);
        float avance;
        std::string t;
        if (this->st->termino)
        {
            std::lock_guard<std::mutex> l(this->st->m);
            t      = this->st->resultado + (this->reinicio ? "\n\nLos íconos del HOME se refrescan del todo al reiniciar ()." : "");
            avance = 1;
        }
        else if (this->st->total > 0)
        {
            t      = std::to_string(this->st->hecho.load()) + " de " + std::to_string(this->st->total.load()) + "… Puedes esperar aquí.";
            avance = (float)this->st->hecho / this->st->total;
        }
        else
        {
            t      = "Trabajando…";
            avance = std::fmod(std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count(), 1.2f) / 1.2f;
        }
        barra(this, vg, lx, y + 170.0f, lw, avance);
        nvgFontSize(vg, 19.0f);
        nvgTextLineHeight(vg, 1.4f);
        nvgFillColor(vg, this->a(ctx->theme->textColor));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgBeginPath(vg);
        nvgTextBox(vg, lx, y + 200.0f, lw, t.c_str(), nullptr);
    }

  private:
    std::string tit, det;
    bool reinicio;
    std::shared_ptr<Estado> st;
};
}  // namespace

void enSegundoPlano(const std::string& titulo, const std::string& detalle, Trabajo trabajo, bool ofrecerReinicio)
{
    brls::Application::pushView(new MarcoCompleto(new VistaTrabajo(titulo, detalle, trabajo, ofrecerReinicio)));
}

// ------------------------------------------------------------ nombres -----

brls::View* crearNombres()
{
    brls::List* list = new brls::List();
    if (!namesSysmoduleInstalled())
        list->addView(new brls::ListItem("Falta sys-ticon", "Para cambiar nombres hace falta sys-ticon. Lo instalas desde OQB Updater › Apps › Sysmodules. Los nombres que pongas aquí quedan guardados y se usan cuando lo tengas."));
    list->addView(new brls::Header("A cambia el nombre · X vuelve al original · se ve al reiniciar"));
    for (auto& g : installedGames())
    {
        const std::string tid = g["tid"].get<std::string>(), original = g["name"].get<std::string>(), autor = g.value("author", std::string());
        const std::string propio = customName(tid);
        brls::ListItem* item = new brls::ListItem(original, "", propio.empty() ? autor : "Se ve como: " + propio);
        if (!propio.empty()) item->setValue("nombre propio");
        item->getClickEvent()->subscribe([tid, original, autor, item](brls::View*) {
            const std::string actual = customName(tid);
            brls::Swkbd::openForText([tid, autor, item](std::string t) {
                if (t.empty()) return;
                if (setCustomName(tid, t, autor))
                {
                    item->setValue("nombre propio");
                    avisar("Nombre guardado: " + t + "\nSe ve al reiniciar");
                }
                else
                    avisar("No se pudo guardar (máximo 511 bytes)");
            }, "Nombre en el HOME", original, 64, actual.empty() ? original : actual);
        });
        item->registerAction("Nombre original", brls::Key::X, [tid, item] {
            if (removeCustomName(tid))
            {
                item->setValue("");
                avisar("Vuelve al nombre original al reiniciar");
            }
            return true;
        });
        list->addView(item);
    }
    return list;
}

// ------------------------------------------------------------ ajustes -----

brls::View* crearAjustes(VistaJuegos* juegos)
{
    brls::List* list = new brls::List();
    nlohmann::json config = loadConfig();

    list->addView(new brls::Header("SteamGridDB"));
    brls::ListItem* token = new brls::ListItem("Token desde el celular", "", "Un código QR y lo pegas desde el teléfono");
    token->setValue(config.value("api_token", std::string()).empty() ? "sin token" : "guardado");
    token->getClickEvent()->subscribe([token](brls::View*) {
        brls::Application::pushView(new MarcoCompleto(new VistaToken()));
        token->setValue("…");
    });
    list->addView(token);

    brls::InputListItem* aMano = new brls::InputListItem("Escribirlo a mano", "", "Pega tu token de steamgriddb.com", "Se saca gratis en steamgriddb.com", 64);
    aMano->getClickEvent()->subscribe([aMano](brls::View*) {
        if (aMano->getValue().empty()) return;
        nlohmann::json c = loadConfig();
        c["api_token"]   = aMano->getValue();
        saveConfig(c);
        aMano->setValue("");
        avisar("Token guardado");
    });
    list->addView(aMano);

    brls::ListItem* estilo = new brls::ListItem("Estilo al abrir un juego", "", "El primero que se muestra; L/R lo cambia ahí mismo");
    estilo->setValue(ESTILOS[std::max(0, std::min((int)ESTILOS.size() - 1, config.value("style_id", 0)))]);
    estilo->getClickEvent()->subscribe([estilo](brls::View*) {
        nlohmann::json c = loadConfig();
        int s            = (c.value("style_id", 0) + 1) % (int)ESTILOS.size();
        c["style_id"]    = s;
        saveConfig(c);
        estilo->setValue(ESTILOS[s]);
    });
    list->addView(estilo);

    list->addView(new brls::Header("Avanzado (lo elige la pestaña Tema)"));
    static const std::vector<std::string> fitNames = { "recortar", "ajustar", "estirar", "tema vertical 2:3" };
    brls::ListItem* fit = new brls::ListItem("Imágenes no cuadradas", "", "Cómo encajarlas en el cuadro del HOME");
    fit->setValue(fitNames[std::max(0, std::min(3, config.value("fit_mode", 0)))]);
    fit->getClickEvent()->subscribe([fit](brls::View*) {
        nlohmann::json c = loadConfig();
        int m            = (c.value("fit_mode", 0) + 1) % 4;
        c["fit_mode"]    = m;
        saveConfig(c);
        fit->setValue(fitNames[m]);
        const char* d[4] = { "Toma el centro, sin deformar", "Entra completa, con franjas", "La aplasta al cuadro", "Vertical en el tema y recortado donde va cuadrado" };
        avisar(d[m]);
    });
    list->addView(fit);

    static const std::vector<std::string> smallNames = { "se deja el original", "recortado", "igual que el grande" };
    brls::ListItem* chico = new brls::ListItem("Ícono chico", "", "El de la lista completa y la ventanita al cambiar de app");
    chico->setValue(smallNames[std::max(0, std::min(2, config.value("small_mode", 1)))]);
    chico->getClickEvent()->subscribe([chico](brls::View*) {
        nlohmann::json c  = loadConfig();
        int m             = (c.value("small_mode", 1) + 1) % 3;
        c["small_mode"]   = m;
        saveConfig(c);
        chico->setValue(smallNames[m]);
    });
    list->addView(chico);

    brls::ListItem* orden = new brls::ListItem("Orden de los juegos", "", "Alfabético o por lo último que jugaste");
    orden->setValue(config.value("sort_recent", false) ? "recientes" : "alfabético");
    orden->getClickEvent()->subscribe([orden, juegos](brls::View*) {
        nlohmann::json c = loadConfig();
        bool r           = !c.value("sort_recent", false);
        c["sort_recent"] = r;
        saveConfig(c);
        orden->setValue(r ? "recientes" : "alfabético");
        juegos->recargar();
    });
    list->addView(orden);

    list->addView(new brls::Header("Todos los juegos"));
    brls::ListItem* adaptar = new brls::ListItem("Adaptar todos al tema", "", "El ícono original de cada uno, preparado para el tema vertical");
    adaptar->getClickEvent()->subscribe([](brls::View*) {
        enSegundoPlano("Adaptar al tema", "El ícono original de cada juego, preparado para el tema.", [](std::function<void(int, int)> progreso) {
            nlohmann::json games = installedGames();
            std::vector<nlohmann::json> pendientes;
            for (auto& g : games)
                if (!hasCustomIcon(g["tid"].get<std::string>()) && !g.value("archivado", false)) pendientes.push_back(g);
            int hechos = 0, fallados = 0, n = 0;
            for (auto& g : pendientes)
            {
                progreso(n++, (int)pendientes.size());
                std::string original, tid = g["tid"].get<std::string>();
                if (extractOriginalIcon(tid, original) && overwriteIcon(tid, original)) hechos++;
                else fallados++;
            }
            progreso((int)pendientes.size(), (int)pendientes.size());
            if (pendientes.empty()) return std::string("Todos los juegos ya tienen ícono propio.");
            return "Adaptados " + std::to_string(hechos) + " juegos" + (fallados ? ", " + std::to_string(fallados) + " no se pudieron." : ".");
        }, true);
    });
    list->addView(adaptar);

    list->addView(new brls::Header("Consola"));
    brls::ListItem* sys = new brls::ListItem("Sysmodule", "");
    sys->setValue(sysmoduleInstalled() ? (sysmoduleName() == "sys-tweak" ? "sys-ticon" : sysmoduleName()) : "falta");
    list->addView(sys);

    brls::ListItem* reiniciar = new brls::ListItem("Reiniciar la consola", "", "Para refrescar del todo el HOME");
    reiniciar->getClickEvent()->subscribe([](brls::View*) { askReboot("Cierra cualquier juego antes de seguir."); });
    list->addView(reiniciar);

    brls::ListItem* cache = new brls::ListItem("Borrar caché de imágenes", "", "No toca los íconos ya puestos");
    cache->getClickEvent()->subscribe([](brls::View*) {
        std::error_code ec;
        std::filesystem::remove_all("sdmc:/gameIcons/full/", ec);
        std::filesystem::remove_all("sdmc:/gameIcons/thumbnails/", ec);
        avisar("Caché borrada");
    });
    list->addView(cache);
    return list;
}
