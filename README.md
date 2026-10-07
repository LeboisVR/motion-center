# Motion Center V1.2

Application desktop pour configurer et piloter les Control Box Motion Center,
avec support des firmwares V1, V2, PRO STM32 et Competition Box STM32.

## Contenu

- `Motion_Center_V1_2.py` : application Python/Tkinter.
- `firmware/` : firmwares `.hex` et `.bin` distribuables avec l'application.
- `firmware hardware/` : sources firmware et projets STM32/Arduino.
- `Simhub profile/` : profils SimHub.

## Lancer depuis les sources

Python 3.10 ou plus récent est recommandé.

```powershell
py -m pip install pyserial
py Motion_Center_V1_2.py
```

## Générer l'exécutable Windows

```powershell
py -m pip install pyinstaller
pyinstaller --clean --noconfirm "Motion Center V1.2 onefile.spec"
```

L'exécutable est créé dans `dist/` et embarque le dossier `firmware/`.

## Firmware AVR

Le projet PlatformIO principal cible une Arduino Leonardo/ATmega32U4 :

```powershell
pio run
```

La variante V1 utilise `BOX_VERSION=1` et `MAX_ACTUATORS=5` ; la variante V2
utilise `BOX_VERSION=2` et `MAX_ACTUATORS=6`.

## Notes

Le flashage dépend du matériel : avrdude/DFU pour les Control Box AVR et
l'outil de bootloader adapté pour les versions STM32. Vérifier le modèle de
box avant toute mise à jour firmware.