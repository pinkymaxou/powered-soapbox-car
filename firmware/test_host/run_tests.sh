#!/usr/bin/env bash
# Compile et exécute les tests hôte (logique pure du firmware, sans ESP-IDF).
set -euo pipefail
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -Werror -I ../main test_main.cpp -o /tmp/kart_host_tests
/tmp/kart_host_tests

# Simulation physique : la VRAIE logique (controller_core) pilote le modèle du véhicule
# à travers des scénarios extrêmes et réalistes (anti-renversement, pannes, LVC…).
./run_sim.sh
