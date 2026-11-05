# EegoRead A4 Demo Maintenance

- Keep this repository independent from the production reader project.
- Do not add reader, OTA, cloud, account, file-transfer, USB MSC or production
  startup code.
- Keep network pages scan-only and stop radios in each page's `on_exit`.
- Preserve raw hardware distinctions: A4 Pro is the current platform;
  frontlight presence is a configuration, and CW2015/legacy is the power-board
  profile. Legacy boards use the domestic A4/A4 Frontlight names.
- Build only in `build-final` with ESP-IDF 6.0.1.
- Separate build success, startup logs, screen observation and electrical
  measurements as distinct evidence layers.
- Do not add secrets, tokens, private keys, account data or device credentials.
