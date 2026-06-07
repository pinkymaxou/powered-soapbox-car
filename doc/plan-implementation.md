# Plan d'implémentation — Kart électrique 2 places

Ordre de montage et de mise en service, des plus simples aux plus risqués.
Référence : plan détaillé [`kart-pedales-enfant.md`](kart-pedales-enfant.md).
**Règle d'or : tout tester roues en l'air et à basse vitesse avant le sol.**

```mermaid
flowchart LR
    P1["1. Châssis bois"] --> P2["2. Roues + essieux"] --> P3["3. Direction"]
    P3 --> P4["4. Transmission<br/>(gearbox + courroies)"] --> P5["5. Puissance<br/>(batteries, driver)"]
    P5 --> P6["6. Commande<br/>(ESP32, capteurs)"] --> P7["7. Firmware<br/>+ calibration"]
    P7 --> P8["8. Essais progressifs"] --> P9["9. Sécurité finale"]
```

## Phase 0 — Préparation
- Rassembler le matériel (voir §4 du plan détaillé) et les outils (perceuse, scie, clés, fer à souder, multimètre, imprimante 3D).
- Imprimer les 2 réducteurs (1:16) et un gabarit de perçage.
- **Lunettes + gants** ; travailler batteries débranchées.

## Phase 1 — Châssis bois
1. Découper la **grille de madriers 2×3** (longerons + traverses rapprochées tous les ~25–30 cm).
2. Visser/coller le **plancher CP 6 mm** sur la grille ; **12 mm sous la banquette**.
3. Monter la **banquette unique** (~80 cm) + dossier ; renforts d'angle.
4. ✅ *Vérif : s'asseoir à deux dessus sans flexion excessive.*

## Phase 2 — Roues + essieux
1. Monter les **4 roues Ø30 cm** sur leurs **boulons à épaulement** (perçage 3/8").
2. Bagues si moyeu lisse ; les 4 roues tournent **libres**.
3. ✅ *Vérif : voie 84 cm, roues d'aplomb, rien ne frotte.*

## Phase 3 — Direction (charnière pivot central)
1. Fixer la **charnière** (pivot central) entre traverse mobile et platine fixe + **boulon traversant + platine anti-arrachement**.
2. Monter la **colonne de volant déportée à gauche**, bras Pitman + **bielle** (rotules boulon+nylstop).
3. Régler : **roues droites = volant centré** ; poser les **butées de braquage** (~25°).
4. ✅ *Vérif : direction franche, sans jeu, butées OK.*

## Phase 4 — Transmission
1. **Visser une poulie sur chaque roue AR** (plusieurs rayons, grandes rondelles / contre-platine).
2. Monter les **réducteurs 3D 1:16** + moteurs sur supports bois renforcés.
3. **Courroies** + réglage de **tension** (trous oblongs / galet) ; **carter**.
4. ✅ *Vérif (sans courant) : tout tourne à la main, courroie ni lâche ni trop tendue.*

## Phase 5 — Électronique de puissance ⚠️
1. Monter **2 packs 20 V/5 Ah** + leurs **adaptateurs à glissière**.
2. Câbler chaque pack : **fusible** → **diode idéale 40/60 A** → **rail commun** (diode-OR).
3. Insérer l'**interrupteur à clé** + l'**arrêt d'urgence** (coupe-courant général) en série.
4. Câbler le **driver** (VB+/VB- — ⚠️ **polarité, pas de protection inversion**), sorties moteurs M1/M2.
5. **Câblage puissance ~10 AWG**, cosses serties.
6. ✅ *Vérif au multimètre AVANT de brancher : polarité, ~20 V au driver, e-stop coupe tout.*

## Phase 6 — Électronique de commande
1. Carte **ESP32 + breakout** (rails 5 V / 3,3 V, GND commun).
2. **Abaisseur 20 V → 5 V** pour l'ESP32 (pris avant l'e-stop côté logique selon schéma).
3. **Perfboard soudée** : 2 ponts diviseurs (**throttle ÷1,5** 10 k/20 k, **Vbat** 100 k/15 k) + 0,1 µF.
4. **Pédale Hall** (5 V / GND / signal → diviseur → GPIO34) ; **repérer les fils au multimètre**.
5. **Encodeurs** A/B (3,3 V) → PCNT ; **boutons** START/REVERSE/CAL (pull-up) ; **LED reverse** ; **ruban WS2812B**.
6. ✅ *Vérif : masses communes, 3,3 V/5 V présents, aucun court-circuit.*

## Phase 7 — Firmware + réglages
1. `idf.py build flash monitor` (voir [`../firmware/README.md`](../firmware/README.md)).
2. Connexion Wi-Fi **Kart-Config** → `http://192.168.4.1`.
3. **Calibrer l'accélérateur** (désarmé, à l'arrêt) ; ajuster **`vbat_div_ratio`** au multimètre.
4. **Mesurer et régler `ENC_CPR` et `BELT_RATIO`** (sinon vitesse/limiteur/PID faux).
5. Régler **limite de vitesse basse** + vérifier seuils LVC.
6. ✅ *Vérif sur la page : état, tensions, vitesses, sens moteurs corrects.*

## Phase 8 — Essais progressifs (roues en l'air)
1. **Roues soulevées** : armer (appui START), throttle léger → sens correct des 2 roues (inverser M1A/M1B si besoin).
2. Tester **frein électrique au relâché**, **marche arrière**, **désarmement** (appui maintenu), **e-stop**.
3. Provoquer les défauts : **LVC** (alim basse simulée), **panne encodeur** (débrancher) → doit refuser/couper.
4. Poser au sol : **terrain plat, vitesse mini**, 1 enfant léger d'abord, puis 2.
5. Monter la limite **progressivement**.

## Phase 9 — Sécurité finale
- **Ceinture** ancrée au châssis, **casques**, **carters** (poulies/courroies/câbles), angles arrondis.
- **Cale-pieds**, axes sécurisés (nylstop + arrêt).
- **Inspection avant chaque usage** : tension courroies, serrages, e-stop, batterie, frein électrique.
- Usage **sous surveillance adulte**, loin de la circulation et des pentes.

> Voir aussi la section **« Limitations et risques connus »** du [README](../README.md).
