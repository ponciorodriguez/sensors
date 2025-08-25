# sensors

Proyecto de sensores para ESP32 desarrollado con PlatformIO y Visual Studio Code.

## Estructura del proyecto

```
.
├── include/      # Archivos de cabecera (.h)
├── lib/          # Librerías externas/personalizadas
├── src/          # Código fuente principal (main.cpp)
├── test/         # Pruebas unitarias PlatformIO
├── platformio.ini # Configuración del entorno PlatformIO
```

## Compilar y subir

1. Instala [Visual Studio Code](https://code.visualstudio.com/) y la extensión [PlatformIO IDE](https://platformio.org/install/ide?install=vscode).
2. Abre la carpeta del proyecto en VSCode.
3. Compila con el botón "Build" de PlatformIO.
4. Sube el firmware a la ESP32 con el botón "Upload".
5. Monitorea el puerto serie con "Monitor".

## Control de versiones

Este proyecto está preparado para ser subido y gestionado en [GitHub](https://github.com/) y [GitLab](https://gitlab.com/).

## Consideraciones

- No uses entorno virtual Python (venv) salvo que añadas scripts Python específicos.
- Usa el `.gitignore` recomendado para PlatformIO.
- Puedes colaborar, crear issues y compartir el proyecto.

## Licencia

[MIT](LICENSE)