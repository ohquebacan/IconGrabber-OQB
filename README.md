# IconGrabber OQB

Fork de [IconGrabber](https://github.com/Slluxx/IconGrabber) de **Slluxx**, GPLv3.
Todo el merito del programa original es suyo; este fork solo le añade cosas y
lo mantiene funcionando en firmwares nuevos.

## Que cambia respecto al original

- Corregido el limite de tamaño de los iconos. El icono viaja en un buffer de
  131.072 bytes, pero hay DOS por titulo: `icon.jpg` (256x256, la fila principal
  del menu HOME) e `icon174.jpg` (174x174, la pantalla de todas las apps). Si el
  pequeño se pasaba, **no se aplicaba ninguno de los dos** y el juego se quedaba
  con su icono original. Asi fallaban Smash Ultimate y Mario Tennis Aces mientras
  otros si funcionaban. Ahora se capan a 120.000 y 60.000 bytes bajando la
  calidad del JPEG hasta que entren.
- **Ya no borra juegos archivados.** Invalidar la cache de control (`ns:am` 404)
  hace falta para que el menu deje de mostrar el icono viejo, pero si el juego no
  esta instalado su nombre e icono vivian solo ahi: quedaban con "?" y solo se
  recuperaban reinstalando. Ahora se comprueba que haya datos de control propios
  antes de invalidar.
- Modos de encaje para **temas verticales**: recortar, ajustar con franjas,
  estirar, o lienzo 2:3 pensado para que el tema deshaga el aplastado. Con vista
  previa para elegir la posicion del recorte.
- Arreglados varios cuelgues de la interfaz y lecturas defensivas de la API de
  SteamGridDB, que devuelve `null` en campos que el original daba por buenos.

Requiere una clave de API de steamgriddb.com y el sysmodule **sys-icon**.

---

REQUIRED: An API key from steamgriddb.com

This is IconGrabber. A homebrew that one can probably best describe as an unofficial steamgriddb.com client for the Nintendo Switch. You can search for games and then preview and download icons that you want. After that you can apply any icon to any installed title of your switch and replace the original. In order to do that, you need to use [sys-tweak](https://gbatemp.net/threads/custom-game-icons-tutorial-and-sharing-hub-no-forwarders.574675/) though.


The Switch default icons are 256x256 but the tool will automatically resize anything to fit. This means you can use 512x512 or 1024x1024 icons (as long as the ratio is 1:1). Once you applied a custom icon to a game, you need to restart the console for it to take effect.

This tool also works for vertical icon themes.

