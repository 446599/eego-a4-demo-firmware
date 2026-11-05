#!/usr/bin/env bash
# 由需要 ESP-IDF 的脚本 source。调用方已加载 IDF_PATH 时保持现状，
# 否则用 IDF_EXPORT 指定导出脚本，未指定时按当前用户的标准安装位置查找。
# 找不到导出脚本时在进入构建或烧录前退出，避免误用其他工具链。

if [[ -n "${IDF_PATH:-}" ]]; then
    return 0
fi
if [[ -f "/c/Espressif/frameworks/esp-idf-v6.0.1/tools/idf.py" &&
      -x "/c/Espressif/tools/python_env/idf6.0_py3.14_env/Scripts/python.exe" ]]; then
    export IDF_PATH="/c/Espressif/frameworks/esp-idf-v6.0.1"
    export IDF_TOOLS_PATH="/c/Espressif/tools"
    export IDF_PYTHON_ENV_PATH="/c/Espressif/tools/python_env/idf6.0_py3.14_env"
    export ESP_IDF_VERSION="6.0.1"
    export PATH="${IDF_PYTHON_ENV_PATH}/Scripts:/c/Espressif/tools/ninja/1.12.1:/c/Espressif/tools/cmake/3.30.5/bin:${PATH}"
    return 0
fi
if [[ -z "${IDF_EXPORT:-}" && -f "/c/Espressif/frameworks/esp-idf-v6.0.1/export.sh" ]]; then
    export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-/c/Espressif/tools}"
    export IDF_EXPORT="/c/Espressif/frameworks/esp-idf-v6.0.1/export.sh"
fi
IDF_EXPORT="${IDF_EXPORT:-${HOME}/.espressif/v6.0.1/esp-idf/export.sh}"
if [[ ! -f "${IDF_EXPORT}" ]]; then
    echo "ESP-IDF export.sh not found at ${IDF_EXPORT}; set IDF_EXPORT to its path" >&2
    exit 2
fi

# shellcheck source=/dev/null
source "${IDF_EXPORT}" >/dev/null
