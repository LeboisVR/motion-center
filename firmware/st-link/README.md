# Images complètes ST-Link — NE PAS flasher via Motion Center

Les `.bin` de ce dossier sont des **images complètes de la flash (64 Ko)** :
bootloader IAP (0x08000000) + application (0x08005000) + page de métadonnées
CRC32 (0x0800FC00).

- **Usage** : récupération d'une carte dont le bootloader a été écrasé.
  Flasher avec **STM32CubeProgrammer / ST-Link à l'adresse `0x08000000`**
  (full chip erase recommandé avant).
- **Interdit via Motion Center** : l'outil de mise à jour intégré ne flashe
  que la zone application via le bootloader résident ; une image complète
  serait rejetée en cours d'écriture (hors zone autorisée).
- Ce dossier est volontairement hors de `firmware/` *racine* : Motion Center
  ne scanne que `firmware/*.bin` (non récursif), ces images ne doivent pas
  apparaître dans sa liste déroulante.

| Fichier | Contenu |
|---|---|
| `fw-v2.2.2-pro-box-v2-FULL-0x08000000.bin` | IAP.bin + fw-v2.2.2-pro-box-v2 (PRO Box V2, PCB V2.1, F103) |
