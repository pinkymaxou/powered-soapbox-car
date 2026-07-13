# Réducteur 1:16 — identification du pignon moteur et conception du train

## Pignon moteur (mesuré)

| Paramètre | Valeur | Source |
|---|---|---|
| Nombre de dents `z` | **16** | compté |
| Ø extérieur `dₐ` | **19 mm** | pied à coulisse (z pair → mesure directe fiable) |
| Épaisseur de denture | **≈ 1/4″ (6,35 mm)** | mesuré — limite la largeur UTILE de l'étage 1 |
| Module apparent | `m = dₐ/(z+2) = 19/18 ≈ 1,056` | calcul |
| **Standard identifié** | **24 DP** (diametral pitch impérial) → `m = 25,4/24 = 1,058` ; `dₐ = 18/24″ = 0,750″ = 19,05 mm` ✅ | déduction |
| Angle de pression | 20° (à confirmer à l'engrènement ; 14,5° possible sur vieilles dentures impériales) | hypothèse |

**Variante à écarter/confirmer par test** : module 1 métrique avec déport `x = +0,5`
(`dₐ = m·(z+2+2x) = 19 mm` aussi). Départage : pas apparent `p = π·m` →
**24 DP : 3,33 mm** vs module 1 : 3,14 mm ; ou imprimer un pignon d'essai 16T/24 DP
et vérifier l'engrènement dent-dans-dent avec le pignon moteur.

## ⭐ Variante retenue : boîte 1:8 imprimée + courroie 1:2 vers la roue (= 1:16)

Même réduction totale (≈13,8 km/h), mais le **dernier étage — le plus chargé — devient la
courroie** (encaisse les chocs, silencieuse, tolérante à l'alignement) : la boîte ne voit
plus que ~2,9 N·m en sortie au lieu de ~5,8 N·m.

![Schéma du réducteur 1:8](schematics/gearbox.png)

> Régénérable : `. .venv-schem/bin/activate && python doc/schematics/gearbox.py`
> (vue en plan aux entraxes réels + coupe axiale de l'empilement).

**Boîte 1:8 retenue — tout en 24 DP** (réutilise le dessin de la 64T) :

| Engrènement | Rapport | Entraxe | Pièces |
|---|---|---|---|
| **16T moteur → 64T** | 4:1 | `80/48` = 1,6667″ = **42,33 mm** | 64T imprimée (Ø ext. 69,9 mm), ép. 10 mm — largeur UTILE limitée au pignon moteur (6,35 mm) → ≈15 MPa, OK ; les 10 mm donnent ±1,8 mm de tolérance d'alignement axial |
| **32T → 64T (sortie)** | 2:1 | `96/48` = 2,0000″ = **50,80 mm** | 32T **solidaire de la 1ʳᵉ 64T** (pignon composé) ; 64T de sortie identique à la 1ʳᵉ |

- La **32T** (Ø ext. 36,0 mm) est collée/fusionnée à la 64T de l'étage 1 → un seul pignon
  composé sur l'arbre intermédiaire ; la sortie de boîte porte la 2ᵉ 64T + la poulie.
- **Compatibilité tourillon Ø25** (roulements 25×37) : Ø de pied 32T = **31,2 mm** →
  OK **uniquement en tourillon imprimé d'une pièce** (monolithique). ⚠️ Ne PAS aléser la 32T
  à 25 pour un axe traversant (3 mm de paroi sous les dents, pas de place pour une clavette).
  Côté sortie, aucun souci (pied 64T = 65,1 mm).
- **Assemblage des flasques/tourillons : 3 vis sur cercle de perçage + pilote central**
  (retenu) : les 3 vis précompriment les couches (anti-délaminage FDM) et transmettent le
  couple (~30 N/vis au rayon ~15 mm — très large) ; le **perçage central sert de pilote
  d'alignement** : ajustement serré (+0,05/+0,1) et **≥ 5 mm d'engagement** pour garantir
  concentricité ET perpendicularité (un voilage = engrènement irrégulier 1×/tour). Têtes et
  écrous en lamages/poches hex + rondelles, nylstop, resserrer après rodage (fluage) ; vérifier
  que rien ne dépasse dans le plan de denture voisin (zone de chevauchement des deux 64T).
- **Épaisseurs (calcul Lewis, τ moteur 0,36 N·m, driver limité 20 A, ASA admissible 15–20 MPa,
  facteurs vitesse + à-coups du frein par plugging inclus)** :
  | Engrenage | Épaisseur | Contrainte au pic |
  |---|---|---|
  | 16T moteur (métal) | 6,35 mm (imposé) | — (fixe la largeur utile étage 1) |
  | 64T étage 1 | **10 mm** (12 si marge d'alignement souhaitée) | ~18–20 MPa sur 6,35 utiles — l'épaissir ne change rien |
  | 32T | **20 mm** | ~16 MPa (Ft ≈ 80 N) |
  | 64T sortie | **20 mm minimum** (22 = marge d'alignement) | ~13 MPa ; ⚠️ largeur utile étage 2 = min(32T, 64T) — à 15 mm la 32T remonte à ~21 MPa |
- Encombrement moteur→sortie : 42,33 + 50,80 ≈ **93 mm** (axes repliables en angle).
- **Rodage & lubrification** : (1) rodage **à sec** 10–15 min à vide, basse vitesse — les
  crêtes de couches se polissent, la poussière tombe (graisser trop tôt = pâte abrasive) ;
  (2) **nettoyer** la poussière ; (3) **fine couche** de graisse **PTFE ou silicone** sur les
  flancs de dents (pas de bain ; lithium toléré, **jamais** de dégrippant/solvant sur l'ASA) ;
  (4) après quelques heures : resserrer les vis des flasques (fluage), contrôler l'usure.
  Roulements 6805 graissés à vie ; **aucune graisse côté courroie**.
- **Carter en 2 parties** : coque principale + **plaque dévissable** (insertion des engrenages
  puis fermeture). Impératifs plaque : **2 pions de centrage** (les vis seules ont du jeu →
  l'entraxe doit répéter à ~0,1 mm), logements de roulements **épaulés** (captifs plaque
  vissée), épaisseur ≥ 6–8 mm ou nervurée, inserts filetés à chaud côté coque.
- **Matériau : ASA** (retenu si l'impression le permet) — buse 240–260 °C, plateau 90–110 °C,
  **caisson quasi indispensable**, ventiler (styrène), **retrait ~0,4–0,7 %** → gabarit d'essai
  pour caler les cotes (logements Ø37, tourillons Ø24,9, dentures).
- **Comparatif matériaux** (critère décisif : température — frottement + carter fermé + été) :
  | | Tg | Admissible fatigue à chaud | Verdict engrenages |
  |---|---|---|---|
  | **ASA** | ~100 °C | 15–20 MPa | ✅ **choix final** (UV + chaleur + fluage) ; inter-couches faible compensé par les 3 vis |
  | **PETG** | ~80 °C | 12–15 MPa | 🟡 repli sans caisson : épaissir l'étage 2 à **22–25 mm**, resserrer les vis (fluage), usure un peu plus rapide |
  | **PLA** | ~58 °C | 5–8 MPa à 50 °C | ❌ **prototypes seulement** (précis et rapide pour valider entraxes/engrènement) — ramollit l'été, flue à l'arrêt, dents qui s'ébrèchent |

*Alternatives étudiées : 1 étage 16T→128T (entraxe 76,20 mm, roue Ø137,6) ou 2ᵉ étage en
module 2 (15T→30T, entraxe 45 mm) — écartées au profit de la réutilisation de la 64T.*

**Courroie 1:2** : rapport de dents **exactement 2:1** (ex. synchrone HTD 5M : poulie boîte
30T Ø47,7 → poulie roue 60T Ø95,5, vissée sur la jante 12″). Tension utile ≈ 100 N au couple
max — très confortable pour une courroie de 15 mm.

> ⚠️ **Impact firmware** : `hw::GEAR_RATIO` (config.hpp) suppose capteur AS5600 ≡ vitesse
> roue (courroie 1:1 → `GEAR_RATIO = 1`). Avec la courroie 1:2, si l'aimant du capteur reste
> sur la **sortie de boîte**, le capteur tourne 2× plus vite que la roue → mettre
> **`GEAR_RATIO = 2`**. S'il est monté **sur la roue**, rien à changer.

---

## Train initial (référence) : 2 étages de 4:1 (= 1:16), courroie 1:1

```
Moteur ──[16T métal, 24 DP]──╮
                              ├─ Étage 1 (4:1) ─→ arbre intermédiaire ──[15T]──╮
                   [64T imprimé]                                                ├─ Étage 2 (4:1) ─→ roue
                                                                     [60T imprimé]
```

Le 64T (étage 1) et le 15T (étage 2) sont **solidaires** (pignon composé, imprimé d'une pièce).
Seul l'étage 1 doit matcher le pignon moteur ; l'étage 2 est libre → **module 2** (couple 4× plus
élevé = dents plus grosses).

### Étage 1 — 16T (métal) → 64T (imprimé)

| Paramètre | Valeur (24 DP) | Variante module 1 + déport |
|---|---|---|
| Module / angle | 1,058 mm (24 DP), 20° | m = 1, roue générée avec x = −0,5 |
| **Entraxe** | **42,33 mm** | **40,00 mm** |
| Ø ext. roue 64T | ≈ 69,9 mm | ≈ 67 mm |
| Largeur de denture | 10–12 mm (≥ pignon moteur) | idem |
| Forme | droite (imposée par le pignon métal) | idem |

### Paramètres CAO saisis — roue 64T de l'étage 1 (générateur d'engrenage droit)

| Champ | Valeur saisie | Équivalent métrique |
|---|---|---|
| Standard | **English** | — |
| Pressure Angle | **20 deg** | 20° |
| Diametral Pitch | **24** | module 1,058 mm |
| Number of Teeth | **64** | — |
| Backlash | **0.1 mm** | jeu d'impression |
| Root Fillet Radius | 0.000 in | ⚠️ voir note |
| Gear Thickness | 0.394 in | **10,0 mm** |
| Hole Diameter | 0.394 in | **10,0 mm** (alésage) |
| Pitch Diameter (calculé) | 2.7 in (= 64/24 = 2,667″) | **67,7 mm** primitif → Ø ext. 69,85 mm |

> ⚠️ **Root Fillet Radius = 0** : congé de pied de dent nul = concentration de contrainte au
> pied (là où une dent imprimée casse). Mettre une petite valeur (ex. **0.012 in ≈ 0,3 mm**)
> si le générateur l'accepte — gratuit en impression 3D et nettement plus résistant.

### Étage 2 — 15T → 60T (imprimés, module 2)

| Paramètre | Valeur |
|---|---|
| Module / angle | 2 mm, 20° |
| **Entraxe** | **75,00 mm** |
| Ø ext. 15T / 60T | 34 mm / 124 mm |
| Largeur de denture | 15 mm |
| Forme | **chevrons (herringbone)** conseillés — imprimable, silencieux, auto-centrant |

### Dimensionnement (ordres de grandeur)

- Couple moteur ≈ 0,36 N·m (172 W @ 4615 tr/min) → intermédiaire ≈ 1,4 N·m → **~95 N**
  tangentiels sur le 15T → ≈ 13 MPa en pied de dent (Lewis, m2 × 15 mm) → marge ≈ ×3 en
  **PETG** (nylon encore mieux pour le 15T). Le driver limite à 20 A → pas de couple de
  blocage démesuré.
- Sortie : 4615/16 ≈ 288 tr/min à 12 V ; **~240 tr/min à PWM 50 % → ~13,8 km/h** (roue 12″)
  — cohérent avec le README.

### Impression / montage

- **Backlash +0,10–0,15 mm** dans le générateur pour toutes les pièces imprimées.
- 100 % de remplissage ou ≥ 6 périmètres.
- **Roulements retenus : 6805 — 25×37×7 mm (épaisseur confirmée)** → logements carter
  profondeur **7 mm**, épaulés.
  Montage : roulements **logés dans les parois du carter** ; le pignon composé 64T+15T est
  imprimé avec des **tourillons Ø25 intégrés** de chaque côté qui tournent dedans.
  - ⚠️ Impossible de loger le roulement **dans** le 15T (Ø pied ≈ 25 mm < Ø ext. 37 mm du
    roulement) → c'est bien l'axe qui tourne, pas le roulement dans l'engrenage.
  - Logement carter : Ø **37,1–37,2 mm** (ajustement imprimé), épaulé, profondeur = épaisseur
    du roulement ; tourillon Ø **24,9 mm** (léger serrage dans la bague intérieure).
  - L'alésage 10 mm saisi dans la CAO du 64T devient inutile avec les tourillons intégrés
    (ou sert de passage central si on préfère un axe traversant).
- Encombrement moteur→sortie : 42,33 + 75 ≈ **117 mm** d'entraxe total.
- Ordre de validation : pignon d'essai 16T/24 DP → étage 1 seul → train complet.
