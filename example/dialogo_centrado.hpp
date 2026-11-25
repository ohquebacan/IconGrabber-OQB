#pragma once

#include <algorithm>
#include <borealis.hpp>

/* Dialog de borealis centrado (de OQB Updater). Cuando el texto no entra en su
   alto fijo, borealis agranda la ventana solo hacia abajo y queda pegada al
   borde. Aqui se le da el alto que haga falta antes de acomodarla. */
class DialogoCentrado : public brls::Dialog
{
  public:
    // alto 0: se calcula por el largo del texto (unas 50 letras por linea).
    explicit DialogoCentrado(const std::string& texto, unsigned alto = 0)
        : brls::Dialog(texto)
        , alto(alto)
    {
        if (this->alto == 0)
        {
            unsigned lineas = 1, enLinea = 0;
            for (unsigned char c : texto)
            {
                if ((c & 0xC0) == 0x80) continue;  // medio caracter UTF-8
                if (c == '\n' || ++enLinea > 50)
                {
                    ++lineas;
                    enLinea = 0;
                }
            }
            this->alto = 150 + lineas * 38;
        }
    }

    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override
    {
        const unsigned antes = style->Dialog.height;
        style->Dialog.height = std::max(antes, std::min(this->alto, 640u));
        brls::Dialog::layout(vg, style, stash);
        style->Dialog.height = antes;
    }

  private:
    unsigned alto;
};
