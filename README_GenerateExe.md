# GenerateExe.bat — Build de l'exécutable Motion Center V1.2

## En bref

`GenerateExe.bat` construit **uniquement** l'application Windows
`dist\Motion Center V1.2.exe` (PyInstaller, mode *onefile*).

> ⚠️ **Il ne compile AUCUN firmware.** Les firmwares des box sont des
> binaires **pré-compilés** pris tels quels dans le dossier [`firmware/`](./firmware)
> et simplement **embarqués** dans l'exe. Si un firmware a été modifié, il faut
> le recompiler séparément (voir [Compilation des firmwares](#compilation-des-firmwares))
> et copier le `.hex`/`.bin` dans `firmware/` **avant** de lancer `GenerateExe.bat`.

## Ce que fait le script, étape par étape

1. Se place dans le dossier du script (`cd /d %~dp0`).
2. Détecte l'interpréteur Python (`python`, sinon `py`) dans le PATH.
3. Installe/vérifie les dépendances de build via pip : `pyinstaller`, `pillow`, `pyserial`.
4. Garde-fou : vérifie que `PIL.ImageTk` est importable (sans Pillow, les photos
   des box ne seraient pas affichables dans l'app).
5. Ferme une éventuelle instance de `Motion Center V1.2.exe` en cours
   (`taskkill`) et supprime l'ancien `dist\Motion Center V1.2.exe`
   (échoue proprement si le fichier est verrouillé, p. ex. par la synchro Google Drive).
6. Lance `python -m PyInstaller --noconfirm --clean "Motion Center V1.2 onefile.spec"`.
7. Vérifie que `dist\Motion Center V1.2.exe` existe, puis `pause`.

Codes retour : `0` = succès, `1` = échec (message `[ERREUR] ...` affiché).

## Données embarquées dans l'exe

Définies dans [`Motion Center V1.2 onefile.spec`](<./Motion Center V1.2 onefile.spec>) (section `datas`) :

| Dossier source     | Contenu embarqué                                                        |
|--------------------|--------------------------------------------------------------------------|
| `icons/`           | Icône de l'app + icônes UI (web, Discord, etc.)                          |
| `images/`          | Photos des box, visuels vérins, animations                              |
| `firmware/`        | **Firmwares pré-compilés** (`.hex` AVR, `.bin` STM32) proposés au flash |
| `Simhub profile/`  | Profil SimHub « Lebois Racing Motion Center v1.2 »                      |
| `avrdude/`         | `avrdude` + `arduino-cli.exe` pour flasher les box AVR                  |

L'exe est généré sans console (`console=False`), sans UPX, avec l'icône
`icons\Motion-Center-icon.ico`.

## Prérequis

- Windows, Python 3 dans le PATH (le script installe lui-même PyInstaller/Pillow/pyserial).
- Les firmwares à jour déjà présents dans `firmware/` (voir ci-dessous).

## Sorties

- `dist\Motion Center V1.2.exe` — livrable final (autonome, un seul fichier).
- `build\Motion Center V1.2 onefile\` — fichiers intermédiaires PyInstaller (ignorables).

## Compilation des firmwares

Chaque firmware se compile avec sa propre chaîne d'outils, **hors** de ce script :

| Box (BOX=)                        | Sources                                                   | Outil de compilation                         | Sortie à copier dans `firmware/`        |
|-----------------------------------|-----------------------------------------------------------|----------------------------------------------|------------------------------------------|
| Competition Box V1 (1)            | `firmware hardware/competition-control-box-v2.1/`         | PlatformIO `pio run -e leonardo`, en ajustant `build_flags` : `-D BOX_VERSION=1 -D MAX_ACTUATORS=5` | `firmware_vX.Y.Z_BOX_V1.hex` |
| Competition Box V2 (2)            | `firmware hardware/competition-control-box-v2.1/`         | PlatformIO `pio run -e leonardo` (défaut `MAX_ACTUATORS=6`, cf. [`platformio.ini`](./platformio.ini)) | `firmware_vX.Y.Z_BOX_V2.hex` |
| PRO Box V1 (4, PCB V2.0, F103)    | `firmware hardware/SRT80 PRO Control BOX PCB V2.0 STM32F103C8T6/` (`PCB_V21=0`) | STM32CubeIDE (make + arm-none-eabi-gcc) | `fw-vX.Y.Z-pro-box-v1.bin` |
| PRO Box V2 (4, PCB V2.1, F103)    | même projet F103, avec `PCB_V21=1`                        | STM32CubeIDE                                 | `fw-vX.Y.Z-pro-box-v2.bin`              |
| PRO Box V2.3 (4, G431)            | `firmware hardware/SRT80 PRO Control BOX PCB V2.3 STM32G431CBT6/` | STM32CubeIDE (flash via USB DFU, pas par Motion Center) | `.bin` (DFU) |
| Competition Box V3 (3/5, G474)    | `firmware hardware/Competition Control Box V5.1/`         | STM32CubeIDE                                 | `fw-vX.Y.Z-competition-box-g4.bin`      |
| Bootloaders IAP (F103 / G474)     | `firmware hardware/IAP/`, `firmware hardware/Competition Control Box V5.1 Bootloader/` | STM32CubeIDE | flashés en usine (non embarqués)  |

Détails matériels et protocole : voir [`box_specs.md`](./box_specs.md).

### Workflow complet après modification d'un firmware

1. Recompiler le firmware concerné (PlatformIO ou STM32CubeIDE).
2. Copier le `.hex`/`.bin` produit dans `firmware/` en respectant le nommage ci-dessus
   (l'app détecte version et box cible à partir du nom de fichier).
3. Lancer `GenerateExe.bat` pour régénérer l'exe avec les nouveaux firmwares embarqués.

## Dépannage

| Message | Cause / solution |
|---------|------------------|
| `[ERREUR] Python introuvable dans le PATH.` | Installer Python ou ajouter `python`/`py` au PATH. |
| `[ERREUR] Pillow/PIL indisponible` | `python -m pip install pillow`, puis relancer. |
| `[ERREUR] Impossible de supprimer dist\...exe` | Fermer Motion Center s'il tourne, ou mettre en pause la synchro Google Drive, puis réessayer. |
| `[ERREUR] La compilation PyInstaller a echoue.` | Lire la sortie PyInstaller au-dessus ; souvent un antivirus qui bloque `dist/` ou une dépendance manquante. |
