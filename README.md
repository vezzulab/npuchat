<p align="center">
  <img src="docs/hero.png" alt="NPU Chat — a simple, private AI chat for the AMD Ryzen AI NPU" width="900">
</p>

<p align="center">
  <a href="https://github.com/vezzulab/NPU-Chat/releases/latest"><img alt="Download" src="https://img.shields.io/github/v/release/vezzulab/NPU-Chat?label=download&color=e01e5a"></a>
  <img alt="Platform" src="https://img.shields.io/badge/linux-x86__64-7b2ff7">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-ff7a18">
</p>

**NPU Chat** is a lightweight desktop app for chatting with local AI models on the NPU of AMD Ryzen AI laptops. It uses [FastFlowLM](https://github.com/FastFlowLM/FastFlowLM) under the hood. No cloud, no browser, no clutter.

## Features

- **100% local:** runs on the NPU, works offline.
- **Only models that fit:** checks your NPU, RAM and disk, and shows the expected speed of each model.
- **Assistants:** create your own (psychologist, strategist, chef…) or add one of the 48 in the gallery.
- **Battery-aware:** power saver on battery, full speed when plugged in, and the model is unloaded when idle.
- **The basics, done well:** chat history, Markdown and code blocks, light and dark themes, English and Español.

## Requirements

- An AMD Ryzen AI 300/400 series laptop (XDNA 2 NPU) running Linux kernel 7.0 or newer.
- [FastFlowLM](https://fastflowlm.com/docs/install_lin/) installed. On Fedora:

  ```bash
  sudo dnf copr enable alessandrolattao/fastflowlm
  sudo dnf install --exclude=xdna-driver-dkms fastflowlm
  sudo flm-fetch-kernels
  ```

## Install

Download `NPU-Chat-x86_64.AppImage` from [Releases](https://github.com/vezzulab/NPU-Chat/releases/latest), then:

```bash
chmod +x NPU-Chat-x86_64.AppImage && ./NPU-Chat-x86_64.AppImage
```

## Build from source

```bash
sudo dnf install gcc meson gtk4-devel libadwaita-devel libsoup3-devel json-glib-devel
./build.sh   # builds and installs to ~/.local
```

## Troubleshooting

**"The NPU needs a system setting to work"** means systemd caps locked memory at 8 MB, and the NPU needs more to load a model. Run this once, then reboot:

```bash
sudo mkdir -p /etc/systemd/system/user@.service.d /etc/systemd/user.conf.d
printf '[Service]\nLimitMEMLOCK=infinity\n' | sudo tee /etc/systemd/system/user@.service.d/memlock.conf
printf '[Manager]\nDefaultLimitMEMLOCK=infinity\n' | sudo tee /etc/systemd/user.conf.d/memlock.conf
```

## License

[MIT](LICENSE)
