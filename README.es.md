<p align="center">
  <img src="docs/hero.png" alt="NPU Chat — un chat de IA simple y privado para la NPU de AMD Ryzen AI" width="900">
</p>

<p align="center">
  <a href="https://github.com/vezzulab/npuchat/releases/latest"><img alt="Descargar" src="https://img.shields.io/github/v/release/vezzulab/npuchat?label=descargar&color=e01e5a"></a>
  <img alt="Plataforma" src="https://img.shields.io/badge/linux-x86__64-7b2ff7">
  <img alt="Licencia" src="https://img.shields.io/badge/licencia-MIT-ff7a18">
</p>

<p align="center"><a href="README.md">English</a> · <b>Español</b></p>

**NPU Chat** es una aplicación de escritorio ligera para chatear con modelos de IA locales en la NPU de las laptops AMD Ryzen AI. Usa [FastFlowLM](https://github.com/FastFlowLM/FastFlowLM) por debajo. Sin nube, sin navegador, sin complicaciones.

## ¿Por qué NPU Chat?

La mayoría de las apps de IA local en Linux (Ollama, llama.cpp, LM Studio) ejecutan los modelos en la CPU o en la GPU. Las laptops Ryzen AI también tienen una **NPU**, un chip hecho para IA que casi siempre está sin usar. NPU Chat la pone a trabajar:

- **Tu CPU y tu GPU quedan libres:** el modelo corre en la NPU, así que tu equipo sigue fluido mientras escribe.
- **Cuida la batería:** la NPU está diseñada para IA de bajo consumo. En nuestra prueba, la laptop consumió casi lo mismo generando texto que en reposo.
- **Nativa y simple:** una pequeña app GTK. Sin pestañas de navegador, Docker ni servidores web.

<sub>Medido en un Ryzen AI 5 430 con batería y `qwen3.5:9b` en modo ahorro, generando 400 tokens a 11.7 tok/s. La CPU usó 6 % (3.5 % en reposo) y la GPU 1 %. La laptop completa consumió ~8.4 W, frente a 8.6 W en reposo.</sub>

## Características

- **100 % local:** corre en la NPU y funciona sin conexión.
- **Solo modelos que caben:** revisa tu NPU, tu RAM y tu disco, y muestra la velocidad esperada de cada modelo. Un modelo demasiado grande para tu memoria no se ofrece, no se descarga y no se carga, para que el equipo nunca quede al límite.
- **Asistentes:** crea los tuyos (psicólogo, estratega, chef…) o añade uno de los 48 de la galería.
- **Chatea con tus documentos:** añade archivos PDF, de texto o Markdown (o arrástralos a un chat) y recibe respuestas con fuentes. Las bibliotecas se quedan en tu computador y puedes asociarlas a un chat o a un asistente.
- **Skills:** el modelo puede usar una calculadora, conocer la fecha y la hora, consultar la batería y la memoria de tu laptop y buscar en Wikipedia (opcional). Él decide cuándo, y tú ves lo que hizo.
- **Calendario:** vistas de día, semana, mes y año, repeticiones, recordatorios, alertas con posponer, arrastrar para crear y mover, entrada rápida («cena con Ana jueves 7pm»), importar y exportar, imprimir, y sincronización con tu iPhone mediante iCloud (o cualquier servidor CalDAV). El modelo puede ver tu agenda y añadir, mover, renombrar y borrar eventos, marcar recordatorios como hechos y buscar tiempo libre, todo por nombre y con deshacer. Todo queda en un solo archivo local. También existe como app independiente.
- **Equipos:** elige varios asistentes (por ejemplo entrenador personal + nutricionista) y el especialista adecuado responde cada tema; el que no tiene nada que añadir pasa al siguiente.
- **Consciente de la batería:** modo ahorro con batería, máxima velocidad conectada a corriente, y el modelo se descarga de la memoria cuando no se usa (tras 10 minutos con batería y 30 conectada) y se carga al enviar el primer mensaje.
- **Se actualiza sola:** las versiones nuevas se instalan con un clic, verificadas con SHA-256.
- **Lo básico, bien hecho:** historial de chats, Markdown y bloques de código, temas claro y oscuro, English y Español.

## Requisitos

- Una laptop AMD Ryzen AI serie 300/400 (NPU XDNA 2) con kernel de Linux 7.0 o superior (o el driver amdxdna por DKMS).
- Cualquier distribución actual: Ubuntu 24.04+, Debian 13, Fedora 39+, Arch y otras.
- [FastFlowLM](https://fastflowlm.com/docs/install_lin/) instalado. Consulta su guía para Ubuntu y Arch. En Fedora:

  ```bash
  sudo dnf copr enable alessandrolattao/fastflowlm
  sudo dnf install --exclude=xdna-driver-dkms fastflowlm
  sudo flm-fetch-kernels
  ```

## Instalación

Descarga `NPU-Chat-x86_64.AppImage` desde [Releases](https://github.com/vezzulab/npuchat/releases/latest) y ejecuta:

```bash
chmod +x NPU-Chat-x86_64.AppImage && ./NPU-Chat-x86_64.AppImage
```

## Contribuir: Intel y OpenVINO

NPU Chat solo funciona hoy con AMD Ryzen AI, porque [FastFlowLM](https://github.com/FastFlowLM/FastFlowLM) solo maneja NPUs de AMD. Las NPUs de Intel Core Ultra podrían funcionar con [OpenVINO Model Server](https://github.com/openvinotoolkit/model_server), que habla la misma API estilo OpenAI que la app ya usa para chatear. Lo que falta es un segundo motor: detectar el chip, y listar, descargar y servir modelos con OpenVINO en lugar de `flm` (mira `src/flm.c`, `src/sysinfo.c` y `src/catalog.c`). Si tienes una laptop Intel y quieres construirlo y probarlo, los pull requests son bienvenidos.

## Compilar desde el código

```bash
sudo dnf install gcc meson gtk4-devel libadwaita-devel libsoup3-devel json-glib-devel
./build.sh   # compila e instala en ~/.local
```

## Tecla Copilot (opcional)

En KDE, **Preferencias → Teclado → La tecla Copilot abre NPU Chat → Activar** lo hace por ti: espera a que pulses la tecla (así que solo funciona en laptops que la tienen), pide tu contraseña una vez, instala una regla udev que hace que la tecla envíe F19 (KDE no puede asignarla de otro modo, porque los esquemas de teclado la llaman «Assistant») y asigna `Meta+Shift+F19` a NPU Chat. Al pulsarla de nuevo, la ventana pasa al frente. Quitar restaura la tecla.

La misma regla a mano: [packaging/90-copilot-key.hwdb](packaging/90-copilot-key.hwdb).

## Solución de problemas

**«La NPU necesita un ajuste del sistema para funcionar»** significa que systemd limita la memoria bloqueada a 8 MB y la NPU necesita más para cargar un modelo. Ejecuta esto una vez y reinicia:

```bash
sudo mkdir -p /etc/systemd/system/user@.service.d /etc/systemd/user.conf.d
printf '[Service]\nLimitMEMLOCK=infinity\n' | sudo tee /etc/systemd/system/user@.service.d/memlock.conf
printf '[Manager]\nDefaultLimitMEMLOCK=infinity\n' | sudo tee /etc/systemd/user.conf.d/memlock.conf
```

## Licencia

[MIT](LICENSE)
