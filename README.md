<p align="center">
  <img src="docs/hero.png" alt="NPU Chat — a simple, private AI chat for the AMD Ryzen AI NPU" width="900">
</p>

<p align="center">
  <a href="https://github.com/vezzulab/npuchat/releases/latest"><img alt="Download" src="https://img.shields.io/github/v/release/vezzulab/npuchat?label=download&color=e01e5a"></a>
  <img alt="Platform" src="https://img.shields.io/badge/linux-x86__64-7b2ff7">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-ff7a18">
</p>

**NPU Chat** is a lightweight desktop app for chatting with local AI models on the NPU of AMD Ryzen AI laptops. It uses [FastFlowLM](https://github.com/FastFlowLM/FastFlowLM) under the hood. No cloud, no browser, no clutter.

## Why NPU Chat?

Most local AI apps on Linux (Ollama, llama.cpp, LM Studio) run models on the CPU or GPU. Ryzen AI laptops also have an **NPU**, a chip built for AI, which usually sits idle. NPU Chat puts it to work:

- **Your CPU and GPU stay free:** the model runs on the NPU, so your machine stays responsive while it writes.
- **Easy on the battery:** the NPU is designed for low-power AI. In our test the laptop drew about the same power while generating as it does at idle.
- **Native and simple:** a small GTK app. No browser tabs, Docker or web-server stack.

<sub>Measured on a Ryzen AI 5 430 on battery with `qwen3.5:9b` in power-saver mode, generating 400 tokens at 11.7 tok/s. CPU use was 6 % (3.5 % at idle) and GPU use 1 %. The whole laptop drew ~8.4 W, versus 8.6 W at idle.</sub>

## Features

- **100% local:** runs on the NPU, works offline.
- **Only models that fit:** checks your NPU, RAM and disk, and shows the expected speed of each model.
- **Assistants:** create your own (psychologist, strategist, chef…) or add one of the 48 in the gallery.
- **Chat with your documents:** add PDF, text or Markdown files (or drop them into a chat) and get answers with sources. Libraries stay on your computer, and you can attach them to a chat or to an assistant.
- **Skills:** the model can use a calculator, know the date and time, check your laptop's battery and memory, and look things up on Wikipedia (opt-in). It decides when, and you see what it did.
- **Calendar:** day, week, month and year views, repeats, reminders, alerts with snooze, drag to create and move, quick entry (“dinner with Ana thursday 7pm”), import and export, printing, and sync with your iPhone through iCloud (or any CalDAV server). The model can look at your agenda and add, move, rename and delete events, mark reminders done and find free time, all by name and with undo. Everything stays in one local file. It also exists as a standalone app.
- **Teams:** pick several assistants (e.g. personal trainer + nutritionist) and the right specialist answers each topic; one with nothing to add passes to the next.
- **Battery-aware:** power saver on battery, full speed when plugged in, and the model is unloaded when idle.
- **Updates itself:** new versions install in one click, verified with SHA-256.
- **The basics, done well:** chat history, Markdown and code blocks, light and dark themes, English and Español.

## Requirements

- An AMD Ryzen AI 300/400 series laptop (XDNA 2 NPU) with Linux kernel 7.0 or newer (or the amdxdna DKMS driver).
- Any current distro: Ubuntu 24.04+, Debian 13, Fedora 39+, Arch and others.
- [FastFlowLM](https://fastflowlm.com/docs/install_lin/) installed. See its guide for Ubuntu and Arch. On Fedora:

  ```bash
  sudo dnf copr enable alessandrolattao/fastflowlm
  sudo dnf install --exclude=xdna-driver-dkms fastflowlm
  sudo flm-fetch-kernels
  ```

## Install

Download `NPU-Chat-x86_64.AppImage` from [Releases](https://github.com/vezzulab/npuchat/releases/latest), then:

```bash
chmod +x NPU-Chat-x86_64.AppImage && ./NPU-Chat-x86_64.AppImage
```

## Contributing: Intel and OpenVINO

NPU Chat only supports AMD Ryzen AI today, because [FastFlowLM](https://github.com/FastFlowLM/FastFlowLM) only drives AMD NPUs. Intel Core Ultra NPUs could work through [OpenVINO Model Server](https://github.com/openvinotoolkit/model_server), which speaks the same OpenAI-style API the app already uses for chatting. What is missing is a second engine: detecting the chip, and listing, downloading and serving models with OpenVINO instead of `flm` (see `src/flm.c`, `src/sysinfo.c` and `src/catalog.c`). If you have an Intel laptop and want to build and test it, pull requests are welcome.

## Build from source

```bash
sudo dnf install gcc meson gtk4-devel libadwaita-devel libsoup3-devel json-glib-devel
./build.sh   # builds and installs to ~/.local
```

## Copilot key (optional)

On KDE, **Preferences → Keyboard → Copilot key opens NPU Chat → Enable** does it for you: it waits for you to press the key (so it only works on laptops that have one), asks for your password once, installs a udev rule that makes the key send F19 (KDE cannot bind it otherwise, because layouts name it "Assistant"), and assigns `Meta+Shift+F19` to NPU Chat. Pressing it again brings the window forward. Remove restores the key.

The same rule by hand: [packaging/90-copilot-key.hwdb](packaging/90-copilot-key.hwdb).

## Troubleshooting

**"The NPU needs a system setting to work"** means systemd caps locked memory at 8 MB, and the NPU needs more to load a model. Run this once, then reboot:

```bash
sudo mkdir -p /etc/systemd/system/user@.service.d /etc/systemd/user.conf.d
printf '[Service]\nLimitMEMLOCK=infinity\n' | sudo tee /etc/systemd/system/user@.service.d/memlock.conf
printf '[Manager]\nDefaultLimitMEMLOCK=infinity\n' | sudo tee /etc/systemd/user.conf.d/memlock.conf
```

## License

[MIT](LICENSE)
