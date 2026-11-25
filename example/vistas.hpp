#pragma once

#include <borealis.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

/* Rediseño 3.0: las pantallas propias, dibujadas a mano con el estilo de
   OQB Updater (grillas, chips, panel a la derecha). Lo que no necesita una
   grilla (Perfiles, Nombres, Ajustes) sigue siendo una lista de borealis, que
   toma los colores del tema. */

extern std::atomic<int> g_genIconos;
extern std::atomic<bool> g_saliendo;  // la app se cierra: los hilos cortan lo que hacen
void lanzar(std::function<void()> f);  // un hilo en segundo plano (sin detach)
void cosecharHilos();                  // junta los que terminaron; una vez por cuadro
void esperarHilos();
void luego(std::function<void()> f);  // en el cuadro siguiente (cerrar la pantalla del botón)                   // al salir: espera a todos (g_saliendo ya en true)  // sube cada vez que cambia un ícono: las grillas se refrescan

// Las pestañas, sin barra de título y con el nombre arriba del panel.
class MarcoPrincipal : public brls::TabFrame
{
  public:
    MarcoPrincipal();
    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx) override;
    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override;
};

// Una vista propia a pantalla completa (sin barra de título), con su pie.
class MarcoCompleto : public brls::AppletFrame
{
  public:
    explicit MarcoCompleto(brls::View* contenido);
    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx) override;
    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override;
};

// Juegos: grilla con el ícono que se ve hoy en el HOME.
class VistaJuegos : public brls::View
{
  public:
    VistaJuegos();
    ~VistaJuegos();
    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx) override;
    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override {}
    brls::View* getDefaultFocus() override { return this; }
    void recargar();

  private:
    struct Juego
    {
        std::string tid, nombre;
        bool archivado = false, usb = false;
        int estado     = 0;  // 0 original, 1 propio, 2 no entra
        int img        = -1;
    };
    std::vector<Juego> juegos;
    int filtro = 0, elegido = 0, primeraFila = 0, columnas = 6;
    std::string busqueda;
    int gen = -1;

    std::vector<int> visibles() const;
    void abrir();
    void liberar();
};

// Un juego: los íconos de SteamGridDB en grilla, con vista previa a la derecha.
class VistaIconos : public brls::View
{
  public:
    VistaIconos(const std::string& tid, const std::string& nombre);
    ~VistaIconos();
    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx) override;
    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override {}
    brls::View* getDefaultFocus() override { return this; }

    struct Resultado
    {
        nlohmann::json icono;
        std::string local;  // imagen de la SD, en vez de SteamGridDB
        std::string thumb;
        bool listo = false, fallo = false;
        int img    = -1;
        int w = 0, h = 0;  // medida de la imagen, para separar cuadradas y verticales
    };
    struct Busqueda
    {
        std::mutex m;
        std::atomic<bool> cancelar{ false };
        std::atomic<int> fase{ 0 };  // 0 buscando, 1 listo, 2 error
        std::string error, juegoNombre, juegoId;
        std::vector<std::pair<std::string, std::string>> coincidencias;  // nombre, id
        std::vector<Resultado> res;
    };
    struct Aplicando
    {
        std::atomic<int> fase{ 0 };  // 0 nada, 1 trabajando, 2 listo
        bool ok = false;
        std::string mensaje, imagen;
        bool recorte = false;  // al terminar, abrir el ajuste del recorte
    };

  private:
    std::string tid, nombre, termino, juegoId;
    int chip = 0;   // estilo (o «De la SD»), se cambia en Más opciones
    int forma = 0;  // 0 cuadrados, 1 verticales, 2 todos (L/R)
    int elegido = 0, primeraFila = 0, columnas = 5;
    std::vector<int> visibles();  // con el candado de la búsqueda tomado
    std::shared_ptr<Busqueda> busqueda;
    std::shared_ptr<Aplicando> aplicando;
    int previaGrande = -1, previaChica = -1, previaDe = -1, actual = -1, original = -1;
    bool temaVertical = false;  // la vista previa muestra solo lo de tu tema

    void buscar();
    void aplicar(bool recorte);
    void opciones();
    void liberarPrevias(NVGcontext* vg);
};

// Ajustar el ícono: la imagen entera con el área que se usa, y cómo queda.
class VistaRecorte : public brls::View
{
  public:
    VistaRecorte(const std::string& tid, const std::string& nombre, const std::string& ruta);
    ~VistaRecorte();
    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx) override;
    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override {}
    brls::View* getDefaultFocus() override { return this; }

    struct Aplicando
    {
        std::atomic<int> fase{ 0 };  // 0 nada, 1 trabajando, 2 listo
        bool ok = false;
        std::string mensaje;
    };

  private:
    std::string tid, nombre, ruta;
    std::vector<unsigned char> rgb;  // la imagen entera, decodificada una vez
    int W = 0, H = 0;
    int modo = 3;                    // 0 recortar, 1 con franjas, 2 estirar, 3 tema vertical
    float zoom = 1.0f, px = 0.5f, py = 0.5f;
    int img = -1, previaGrande = -1, previaChica = -1;
    bool sucio = true;
    std::shared_ptr<Aplicando> aplicando;

    void region(int& rx, int& ry, int& rw, int& rh) const;
    std::vector<unsigned char> recorte(int& w, int& h) const;
    void mover(float dx, float dy);
    void aplicar();
};

// Tema: normal (cuadrado) o con HOME vertical.
class VistaTema : public brls::View
{
  public:
    VistaTema();
    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx) override;
    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override {}
    brls::View* getDefaultFocus() override { return this; }

  private:
    int elegido = 0;
    int actual() const;  // 0 normal, 1 HOME vertical, -1 combinación propia
    void elegir(int cual);
};

// Íconos para todos: el más votado de cada juego que no tiene uno propio.
class VistaLote : public brls::View
{
  public:
    VistaLote();
    ~VistaLote();
    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx) override;
    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override {}
    brls::View* getDefaultFocus() override { return this; }

    struct Propuesta
    {
        std::string tid, nombre;
        nlohmann::json icono;
        std::string thumb;
        int estado    = 0;  // 0 buscando, 1 listo, 2 sin ícono
        bool incluido = true;
        int img       = -1;
    };
    struct Estado
    {
        std::mutex m;
        std::atomic<bool> cancelar{ false };
        std::atomic<int> fase{ 0 };  // 0 buscando, 1 propuesto, 2 aplicando, 3 terminado
        std::atomic<int> hechos{ 0 }, total{ 0 }, puestos{ 0 }, fallados{ 0 };
        std::vector<Propuesta> props;
    };

  private:
    int estilo = 0, forma = 0, elegido = 0, primeraFila = 0, columnas = 8;
    std::shared_ptr<Estado> st;
    bool avisado = false;
    void buscar();
    void aplicar();
};

// El token de SteamGridDB desde el celular: un QR a una página en la red local.
class VistaToken : public brls::View
{
  public:
    VistaToken();
    ~VistaToken();
    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height, brls::Style* style, brls::FrameContext* ctx) override;
    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override {}
    brls::View* getDefaultFocus() override { return this; }

    struct Servidor
    {
        std::atomic<bool> parar{ false };
        std::atomic<int> estado{ 0 };  // 0 esperando, 1 guardado y válido, 2 guardado pero rechazado, 3 sin red
        std::mutex m;
        std::string detalle;
    };

  private:
    std::shared_ptr<Servidor> srv;
    std::string url;
    bool teniaToken = false;
    std::vector<unsigned char> qr;  // módulos, lado * lado
    int lado = 0;
};

// Un trabajo largo con barra de progreso, sin congelar la pantalla.
using Trabajo = std::function<std::string(std::function<void(int, int)> progreso)>;
void enSegundoPlano(const std::string& titulo, const std::string& detalle, Trabajo trabajo, bool ofrecerReinicio);

brls::View* crearNombres();
brls::View* crearAjustes(VistaJuegos* juegos);
