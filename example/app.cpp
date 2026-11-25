/*
    IconGrabber OQB 3.0 — arranque.
    Rediseño con el estilo de OQB Updater: el mismo tema (y el color que
    elegiste allá), Instrument Sans y los títulos en Upheaval.
*/
#include <curl/curl.h>
#include <switch.h>

#include <borealis.hpp>
#include <exception>
#include <filesystem>

#include "core.hpp"
#include "dialogo_centrado.hpp"
#include "oqb_theme.hpp"
#include "vistas.hpp"

int main(int argc, char* argv[])
{
    std::remove("sdmc:/config/icongrabber/registro.log");
    marca("inicio");
    std::set_terminate([] {
        std::string t = "sin excepción activa";
        if (auto e = std::current_exception())
        {
            try { std::rethrow_exception(e); }
            catch (const std::exception& x) { t = x.what(); }
            catch (...) { t = "desconocida"; }
        }
        marca("terminate: " + t);
        abort();
    });
    nsInitialize();
    recordarIconosDelArranque();
    socketInitializeDefault();
    curl_global_init(CURL_GLOBAL_ALL);

    marca("servicios listos");
    brls::Logger::setLogLevel(brls::LogLevel::INFO);
    brls::i18n::loadTranslations();
    if (!brls::Application::init("IconGrabber OQB", oqb::style(), oqb::themeVariants()))
    {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }

    // Instrument Sans en toda la app; lo que no tiene (los símbolos de los
    // botones, chino, coreano) sale de las fuentes de la consola.
    {
        brls::FontStash* fuentes = brls::Application::getFontStash();
        NVGcontext* vg           = brls::Application::getNVGContext();
        const int instrument     = nvgCreateFont(vg, "instrument", "romfs:/fonts/InstrumentSans-Medium.ttf");
        if (instrument >= 0)
        {
            for (int f : { fuentes->regular, fuentes->sharedSymbols, fuentes->korean })
                if (f > 0) nvgAddFallbackFontId(vg, instrument, f);
            fuentes->regular = instrument;
        }
    }

    marca("borealis y fuentes listos");
    std::error_code ec;
    std::filesystem::create_directories("sdmc:/iconos/", ec);

    MarcoPrincipal* raiz = new MarcoPrincipal();
    marca("marco");
    VistaJuegos* juegos  = new VistaJuegos();
    marca("juegos: " + std::to_string(installedGames().size()));
    brls::List* perfiles = new brls::List();
    fillProfilesTab(perfiles);

    raiz->addTab("Juegos", juegos);
    raiz->addTab("Tema", new VistaTema());
    raiz->addTab("Perfiles", perfiles);
    marca("perfiles");
    raiz->addTab("Nombres", crearNombres());
    marca("nombres");
    raiz->addSeparator();
    raiz->addTab("Ajustes", crearAjustes(juegos));

    marca("ajustes");
    brls::Application::pushView(raiz);
    marca("ventana puesta");

    // Sin token, lo primero es ofrecer ponerlo desde el celular.
    // sys-icon y Nexus Bacán juntos: al encender, la consola se queda en el logo.
    if (sysIconActivo() && nexusInstalado())
    {
        brls::Dialog* d = new DialogoCentrado("Tienes sys-icon y Nexus Bacán a la vez. Con los dos, la consola se queda en el logo de Nintendo Switch al reiniciar.\n\nApaga sys-icon antes de reiniciar. Tus íconos quedan guardados para cuando lo vuelvas a usar.");
        d->addButton("Apagar sys-icon", [d](brls::View*) {
            const bool ok = apagarSysIcon();
            d->close([ok] { avisar(ok ? "sys-icon apagado. Ya puedes reiniciar." : "No se pudo apagar: borra atmosphere/contents/00FF69636F6EFF00/flags/boot2.flag"); });
        });
        d->addButton("Dejarlo", [d](brls::View*) { d->close(); });
        d->setCancelable(false);
        d->open();
    }

    try
    {
        marca("token?");
        const bool sinToken = loadConfig().value("api_token", std::string()).empty();
        marca(sinToken ? "sin token" : "con token");
        if (sinToken)
        {
            VistaToken* vt = new VistaToken();
            marca("vista token creada");
            MarcoCompleto* mc = new MarcoCompleto(vt);
            marca("marco token creado");
            brls::Application::pushView(mc);
            marca("token puesto");
        }
    }
    catch (const std::exception& e)
    {
        marca(std::string("excepción al abrir el token: ") + e.what());
    }
    catch (...)
    {
        marca("excepción desconocida al abrir el token");
    }

    /* Un error en un cuadro (por ejemplo, un archivo ocupado) no debe cerrar
       la app: se anota y se sigue. */
    int cuadros = 0, errores = 0;
    while (true)
    {
        try
        {
            if (!brls::Application::mainLoop()) break;
            cosecharHilos();
            if (++cuadros == 1 || cuadros == 60) marca("cuadro " + std::to_string(cuadros));
        }
        catch (const std::exception& e)
        {
            marca(std::string("excepción en un cuadro: ") + e.what());
            if (++errores > 50) break;
        }
    }
    marca("saliendo");
    g_saliendo = true;
    esperarHilos();
    marca("hilos terminados");

    curl_global_cleanup();
    socketExit();
    nsExit();
    return EXIT_SUCCESS;
}
