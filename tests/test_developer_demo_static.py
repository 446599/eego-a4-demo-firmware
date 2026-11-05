from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_developer_demo_build_selection_is_isolated():
    cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
    project = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

    assert 'SRCS "developer_demo.c"' in cmake
    assert '"app_main.c"' not in cmake
    assert '"devdemo/devdemo_app_loop.c"' in cmake
    assert '"devdemo/apps/app_overview.c"' in cmake
    assert 'set(PROJECT_VER "V1.1.0-DEMO"' in project
    assert not (ROOT / "main/app_main.c").exists()
    assert not (ROOT / "main/Kconfig.projbuild").exists()


def test_developer_demo_stays_scan_only_and_does_not_start_production_apps():
    sources = [ROOT / "main/developer_demo.c"]
    sources.extend(sorted((ROOT / "main/devdemo").rglob("*.c")))
    source = "\n".join(path.read_text(encoding="utf-8") for path in sources)

    required = [
        "a4_board_read_touch",
        "touch_calibration_map",
        "a4_board_button_pressed",
        "a4_board_set_frontlight",
        "a4_board_read_battery_percent",
        "a4_board_read_datetime",
        "a4_sd_mount",
        "esp_wifi_scan_start",
        "ble_gap_disc",
        "epd_uc8279c_show_frame_fast_partial",
        "DEVDEMO_REDRAW_AREA",
        "devdemo_ui_slider",
        "owns_keys = true",
    ]
    forbidden = [
        "ble_transfer_start",
        "ble_hid_enable",
        "ble_gap_connect",
        "ble_gap_adv_start",
        "ble_store_config_init",
        "wifi_manager_set_enabled",
        "wifi_manager_connect",
        "esp_wifi_set_config",
        "ota_manager_init",
        "reader_session_create",
        "cloud_service_init",
        "usb_storage_mode_run_if_requested",
        "reader_engine",
        "reader_session",
        "ota_manager",
        "cloud_service",
        "wifi_transfer",
        "usb_storage_mode",
    ]

    for symbol in required:
        assert symbol in source
    for symbol in forbidden:
        assert symbol not in source


def test_developer_demo_uses_page_registry_and_separate_apps():
    registry = (ROOT / "main/devdemo/devdemo_app_registry.c").read_text(
        encoding="utf-8"
    )
    apps = sorted((ROOT / "main/devdemo/apps").glob("app_*.c"))

    assert len(apps) == 10
    assert "&devdemo_app_overview" in registry
    assert "&devdemo_app_display" in registry
    assert "&devdemo_app_touch" in registry
    assert "&devdemo_app_keys" in registry
    assert "&devdemo_app_frontlight" in registry
    assert "&devdemo_app_power" in registry
    assert "&devdemo_app_rtc" in registry
    assert "&devdemo_app_sd" in registry
    assert "&devdemo_app_wifi" in registry
    assert "&devdemo_app_ble" in registry


def test_product_names_distinguish_domestic_and_overseas_models():
    names = (ROOT / "main/devdemo/devdemo_hardware_names.h").read_text(
        encoding="utf-8"
    )
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    document = (ROOT / "docs/DEVELOPER_DEMO.md").read_text(encoding="utf-8")

    assert '"A4 Pro / A4"' in names
    assert '"A4 Pro / A4 Lite"' in names
    assert '"A4 Frontlight"' in names
    assert "海外 `A4 Lite` 对应国内 **无前光的 A4 Pro**" in readme
    assert "海外 `A4` 对应国内 **A4 Pro**" in readme
    assert "海外 `A4 Lite` = 国内无前光的 `A4 Pro`" in document


def test_repository_contains_only_demo_components():
    components = {
        path.name
        for path in (ROOT / "components").iterdir()
        if path.is_dir()
    }
    assert components == {
        "a4_board",
        "a4_sd",
        "display_manager",
        "epd_uc8279c",
        "fatfs",
    }
    assert not (ROOT / "server").exists()
    assert not (ROOT / "components/reader_engine").exists()
    assert not (ROOT / "components/ota_manager").exists()


def test_developer_demo_document_covers_hardware_and_evidence_boundaries():
    document = (ROOT / "docs/DEVELOPER_DEMO.md").read_text(encoding="utf-8")
    readme = (ROOT / "README.md").read_text(encoding="utf-8")

    for fact in [
        "A4 Lite",
        "CW2015",
        "PCF8563",
        "GPIO42",
        "scan-only",
        "不会建立连接",
        "主机验证",
        "光学验证",
        "页面与交互",
        "app_overview.c",
        "app_ble.c",
    ]:
        assert fact in document
    assert "(docs/DEVELOPER_DEMO.md)" in readme


def test_readme_documents_quick_start_flash_and_safety_limits():
    readme = (ROOT / "README.md").read_text(encoding="utf-8")

    for heading in [
        "## 快速开始",
        "## 页面",
        "## 操作",
        "## 硬件版本",
        "## 软件结构",
        "## 无线演示",
        "## 测试",
        "## 常见问题",
        "## 证据边界",
        "## 限制",
    ]:
        assert heading in readme
    for target in [
        ROOT / "docs/DEVELOPER_DEMO.md",
        ROOT / "AGENTS.md",
        ROOT / "LICENSES/fonts",
    ]:
        assert target.exists()
    assert "erase-region 0xe000 0x2000" in readme
    assert "V1.1.0-DEMO" in readme
