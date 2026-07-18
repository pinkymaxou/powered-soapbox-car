#!/usr/bin/env bash
# Compile et exécute la SIMULATION PHYSIQUE (contrôleur réel + modèle du véhicule) :
# scénarios extrêmes/réalistes + balayage de paramètres. Utilisé par run_tests.sh et le CI.
set -euo pipefail
cd "$(dirname "$0")"
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I ../main -I sim \
    sim_main.cpp ../main/controller_core.cpp ../main/config_params.cpp -o /tmp/kart_sim_tests
/tmp/kart_sim_tests
