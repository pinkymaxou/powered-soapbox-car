# Composants vendorés (Bluepad32 / BTstack) — patchés pour ESP-IDF 6.1

Ces composants sont **volontairement commités** (vendorés) : ils ne viennent pas du registre
de composants ESP-IDF et ont été **patchés à la main** pour compiler sur IDF 6.1. Un clone
frais du dépôt doit compiler sans étape manuelle — ne pas les re-gitignorer.

## Provenance

- `bluepad32/` + `btstack/` : dépôt Bluepad32 (+ BTstack via son submodule), intégrés par
  le script `integrate_btstack.py` de Bluepad32, puis copiés ici.
- `cmd_nvs/`, `cmd_system/` : composants console fournis avec Bluepad32.

## Patches appliqués pour IDF 6.1 (composant `driver` scindé en `esp_driver_*`)

| Fichier | Modification |
|---|---|
| `btstack/CMakeLists.txt` | `priv_requires` += `esp_driver_gpio/uart/i2c/i2s` |
| `cmd_system/CMakeLists.txt` | `REQUIRES` += `esp_driver_uart` |
| `bluepad32/CMakeLists.txt` | retrait de `uni_mouse_quadrature.c` (dépend de `driver/timer.h`, API legacy supprimée) et des plateformes intégrées (nina/unijoysticle/mightymiggy — inutiles avec la plateforme CUSTOM) ; `requires` += `esp_driver_gpio esp_driver_ledc` |
| `bluepad32/uni_mouse_quadrature_stub.c` | **ajouté** : stub du facteur d'échelle souris (référencé par la console BT) |

En cas de montée de version de Bluepad32 : réappliquer ces patches (ou vérifier qu'ils ne
sont plus nécessaires), puis recompiler `firmware/`.
