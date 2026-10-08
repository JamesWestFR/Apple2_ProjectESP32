# Apple2_ProjectESP32 — émulateur Apple II pour ESP32

Émulateur Apple ][, ][+, //e, //e Enhanced, //c et //c Plus pour la carte **LILYGO TTGO VGA32**
(ESP32-PICO, sortie VGA, clavier PS/2, carte SD). Il reprend l'infrastructure de
[CPC_ProjectESP32](https://github.com/JamesWestFR/CPC_ProjectESP32) et un cœur
d'émulation Apple II écrit pour ce projet, avec
[AppleWin](https://github.com/AppleWin/AppleWin),
[LinApple](https://github.com/linappleii/linapple),
[izapple2](https://github.com/ivanizag/izapple2) et
[apple2ts](https://github.com/ct6502/apple2ts) comme références de comportement.

C'est un projet personnel et indépendant, sans lien avec les auteurs de ces
projets ni avec Apple.

## État au 8 octobre 2026

Le firmware tourne sur la carte. Un premier essai avec écran, clavier et
haut-parleur a été concluant (image et son, 6 et 7 octobre) ; tout le reste de
ce qui suit a été vérifié **par le port série** (console de diagnostic, voir
plus bas), sans regarder l'écran.

Vérifié sur la carte, par le port série :

- démarrage : PSRAM de 4 Mo, carte SD montée, 66 Ko de mémoire interne libres ;
- 59,9 images/s ; une image coûte 4,4 ms de calcul à l'invite du BASIC et
  3,7 à 4,8 ms dans Karateka, pour un budget de 16,7 ms ; 200 images/s en
  vitesse maximale. Pire cas, toute l'image redessinée à chaque fois : 8,3 ms
  en haute résolution, 6,7 ms en texte ;
- `PRINT 6*7` tapé par la console affiche `42` ;
- `Karateka` démarre depuis la carte SD (image copiée en PSRAM) et lit les
  mêmes pistes que sur PC ;
- DOS 3.3 : `SAVE` écrit 3 pistes sur la carte SD ; après éjection, réinsertion
  et redémarrage, `RUN` relit le programme ;
- le contenu du framebuffer VGA, rapatrié en PNG : texte, basse résolution en
  16 couleurs, haute résolution de Karateka, menu et choix de fichiers ;
- le menu : navigation, liste des fichiers de la carte, insertion et démarrage
  d'une disquette, disquette mémorisée et remontée au redémarrage ;
- disque dur : une image `.2mg` de 800 Ko (A2DeskTop) démarre depuis la carte
  SD, en lecture et en écriture (290 blocs lus, 31 écrits) ;
- Mockingboard : un lecteur de musique (pt3_player) la détecte et joue, son
  compteur de temps avance ; 10,7 ms par image ;
- disque dur de 32 Mo : sur un fichier de cette taille créé sur la carte SD,
  un bloc se lit en 1,2 ms à la suite et en 19 ms en accès dispersé (51 ms au
  pire), sans erreur ;
- le nouveau menu, en français et en anglais : chaque page relue en image ;
- sauvegarde d'état : Choplifter sauvé puis repris depuis la carte SD ;
- écriture disque immédiate : `SAVE` sous DOS 3.3 est sur la carte SD dès la
  fin de l'écriture du secteur ;
- réglages par jeu : DOS 3.3 mémorisé en ][+, Karateka sur les réglages
  généraux, retour à DOS 3.3 en ][+ ;
- images `.woz` d'origine, lues depuis la carte SD : Commando, RoboCop, et
  Flight Simulator II sur le //c en ROM 4 ;
- disque dur `Silvern Castle` (1,5 Mo) : écran titre ;
- Apple //c : démarrage sur DOS 3.3 depuis son lecteur intégré, `PRINT 6*7`,
  chargement de Karateka ; jeu de caractères français affiché ;
- Apple //c, souris intégrée : MousePaint s'ouvre au clic et son pointeur suit
  les déplacements envoyés par la console ; imprimante : `PR#1` puis `PRINT`
  crée `PRINTER.TXT` sur la carte SD ;
- carte souris : des déplacements envoyés par la console mènent le pointeur
  d'A2DeskTop à l'endroit attendu (aucune vraie souris PS/2 n'a été essayée) ;
- transfert WiFi : le choix du menu redémarre la carte sur le second firmware,
  qui crée son réseau et annonce `http://192.168.4.1` ; un reset revient à
  l'émulateur. La page web elle-même n'a pas été ouverte ;
- `8-bit Games.hdv` (32 Mo), envoyé par le port série : lanceur FastBoot puis
  Choplifter ;
- Apple //c Plus : ProDOS 2.4.3 démarre de l'image de 800 Ko du lecteur
  3,5 pouces interne ; `SAVE`, `DELETE`, `SAVE` puis `CAT` depuis le BASIC
  (17 blocs écrits sur la carte SD) ; Apple II DeskTop 1.5 s'ouvre sur son
  bureau, avec le volume de l'extension mémoire ;
- disque SmartPort : `8-bit Games.hdv` démarre jusqu'à son lanceur sur le
  //c Plus, et sur les //c ROM 0 et ROM 4 par le `PR#5` automatique.

**À essayer à fond, avec écran, clavier et haut-parleur** : les deux
dispositions du clavier, la Mockingboard à l'oreille, la manette au clavier,
les lignes de balayage, la capture d'écran, la pause, les derniers fichiers,
l'affichage de la vitesse.

Vérifié sur PC seulement, avec le banc de test du dossier `host/`, qui compile
les mêmes fichiers du cœur d'émulation (`src/core/`) :

- 6502 et 65C02 : les tests de Klaus Dormann passent (test fonctionnel 6502 en
  96 241 367 cycles, opcodes étendus du 65C02) ;
- les quatre modèles démarrent : `*` du moniteur pour l'Apple ][, « APPLE ][ »,
  « Apple ][ », « Apple //e » ;
- `Karateka` et `Pandemonium` (dossier `FilesDSK/Apple2`) se chargent ; Karateka
  va jusqu'au jeu, sur //e Enhanced comme sur ][+ ;
- DOS 3.3 : démarrage, `CATALOG`, `SAVE` puis relecture du programme depuis
  l'image modifiée ;
- ProDOS 2.4.3 (image `.po`, et la même renommée en `.dsk`) : démarrage jusqu'au
  lanceur Bitsy Bye ;
- 80 colonnes (`PR#3`), //e sans mémoire auxiliaire ;
- basse résolution (16 couleurs), haute résolution (6 couleurs), mode mixte ;
- le son produit un signal (fichier WAV relu), jamais écouté ;
- images `.nib` et `.2mg` de 140 Ko : démarrage et `CATALOG` ; `SAVE` puis `RUN` sur l'image `.nib` ;
- second lecteur et formatage : `INIT HELLO,D2` sur une image vierge, puis
  `CATALOG,D2` ;
- double haute résolution : aplats de couleur dessinés en BASIC, et bureau
  d'A2DeskTop ;
- sauvegarde d'état : Karateka repris en plein jeu donne, 400 images plus
  tard, une image identique au pixel près, y compris sur une machine démarrée
  dans un autre modèle ; le lecteur de musique Mockingboard reprend au même
  instant de son morceau ;
- images `.woz`, avec le lecteur au bit près : sur 17 disquettes d'origine de
  la collection d'apple2ts, une seule reste bloquée à son démarrage
  (Wasteland) ; Frogger et « Paul Whitehead Teaches Chess » relus en image.
  Les disquettes du dossier `FilesDSK/Apple2/FormatWOZ` démarrent : Commando,
  Flight Simulator II, Frogger II, Karateka (faces A et B, et sa copie
  déplombée), Pitfall II, Platoon, Rambo, Rampage, Renegade, RoboCop (faces A ;
  les faces B ne sont pas des disquettes de démarrage) ;
- Apple //c Plus, lecteur 3,5 pouces interne : ProDOS 2.4.3 et Apple II
  DeskTop 1.5 démarrent d'une image de 800 Ko ; `SAVE`, `DELETE` et `CREATE`
  depuis le BASIC, relus après coup (sur PC, les blocs écrits ont été
  retrouvés dans l'image) ; ProDOS démarre aussi de la disquette 5,25 pouces,
  et le lecteur 3,5 pouces passe avant elle quand les deux sont là ;
- disque SmartPort des //c ROM 0, 3, 4 et Plus : Silvern Castle (1,5 Mo) et
  `8-bit Games.hdv` (32 Mo) démarrent jusqu'à leur écran d'accueil ; écriture
  d'un fichier depuis le BASIC en ROM 4 (sur PC) ;
- sauvegarde d'état pendant un chargement sur le lecteur 3,5 pouces et sur le
  disque SmartPort : la reprise donne la même image au point près (sur PC) ;
- Apple //c en ROM 0, 3 et 4 : DOS 3.3, ProDOS ; l'extension mémoire apparaît
  sous ProDOS (`/RAM4`) en ROM 4 ; Karateka et MousePaint en ROM 4 ;
- Apple //c : bandeau « Apple //c », DOS 3.3 (`SAVE` puis `RUN`), ProDOS,
  80 colonnes, Karateka jusqu'au jeu, Pandemonium, Aztec en `.woz` ;
- jeu de caractères français : à, °, ç, §, é, ù, è, £ aux codes de @ [ \ ] { | } #,
  en 40 et en 80 colonnes ;
- carte souris : A2DeskTop et MousePaint la reconnaissent ; le pointeur suit
  les déplacements, le clic ouvre MousePaint ;
- disque dur : ProDOS démarre du slot 7 sur //e et sur ][+ ; l'image
  `8-bit Games.hdv` (32 Mo) démarre sur son lanceur FastBoot, d'où Choplifter,
  Mario Bros, Alien Typhoon et Pitfall II se lancent.

Écrit mais **jamais essayé**, ni sur PC ni sur la carte : la double basse
résolution ; une disquette 3,5 pouces simple face (400 Ko) ; le formatage
d'une disquette 3,5 pouces ou d'un disque SmartPort.

## Ce qui est émulé

| | |
|---|---|
| Modèles | Apple ][ (Integer BASIC), ][+ (Applesoft, Autostart), //e (6502), //e Enhanced (65C02, MouseText), //c en ROM 255, 0, 3 ou 4 (extension mémoire de 1 Mo en ROM 3 et 4), //c Plus (à 1 MHz : son accélérateur n'est pas émulé) |
| Caractères | américains, ou français sur //e, //e Enhanced et //c |
| Mémoire | 64 Ko dont la carte langage ; 128 Ko sur //e (mémoire auxiliaire en PSRAM) |
| Vidéo | texte 40 et 80 colonnes, basse et haute résolution, doubles résolutions, mode mixte, clignotement ; couleur ou moniteur blanc, vert, ambre |
| Son | haut-parleur (bascule de `$C030`) ; carte Mockingboard en slot 4 (deux 6522, deux AY-3-8910, six voies) |
| Disquettes | carte Disk II en slot 6, deux lecteurs 5,25 pouces, en lecture et en écriture : `.dsk`, `.do`, `.po`, `.nib`, `.2mg` de 140 Ko ; `.woz` en lecture seule |
| Disque dur | images par blocs `.hdv`, `.po`, `.2mg`, de 800 Ko à 32 Mo, lues sur la carte SD : carte ProDOS en slot 7 sur ][, ][+ et //e ; disque SmartPort sur la prise du lecteur externe des //c ROM 0, 3, 4 et Plus |
| Disquette 3,5 pouces | lecteur interne du //c Plus, en lecture et en écriture : l'image de 800 Ko montée comme disque dur |
| Entrées | clavier, deux boutons (touches Pomme), manette au clavier ; carte souris AppleMouse II en slot 2, avec une souris PS/2 sur la seconde prise |

Ce qui n'est pas émulé : l'**Apple IIGS** (processeur 65C816 à 2,8 MHz, 1 Mo de
RAM, vidéo et son propres : hors de portée de cette carte). Les images de
800 Ko du dossier `FilesDSK/Apple2GS` se montent comme disque dur, mais leurs
programmes sont faits pour le IIGS et ne tournent pas sur un //e. Ne sont pas
émulés non plus : sur le //c, la réception sur les ports série et le port 2
(modem), et, comme sur un vrai, aucune carte (ni Mockingboard ni carte
disque dur : son disque dur est un périphérique SmartPort) ; l'accélérateur
du //c Plus (un 65C02 à 4 MHz est hors de portée de l'ESP32 : la machine
tourne à 1 MHz, comme un //c Plus dont on a coupé l'accélérateur) et ses
lecteurs 3,5 pouces externes ; la cassette ; la carte série, la synthèse
vocale de la Mockingboard ; l'écriture sur une image `.woz`.

## Utilisation

1. Copier des images de disquette sur une carte SD formatée en FAT32 (les
   sous-dossiers sont acceptés).
2. Brancher l'écran VGA, le clavier PS/2 et la carte SD, puis alimenter la carte.

L'Apple démarre sur le disque dur s'il y en a un, sinon sur la disquette du
lecteur 1. Sans disquette, il la cherche
sans fin, comme un vrai : **Ctrl+F11** (Ctrl-Reset) rend la main au BASIC.

| Touche | Action |
|--------|--------|
| F12 | Menu, en français ou en anglais : disquettes et disque dur, derniers fichiers, réglages de la machine, de la manette, de la vidéo et du son, aide, reset. L'émulation et le son sont en pause tant qu'il est ouvert |
| F11 | Ouvre directement le choix d'une disquette pour le lecteur 1 |
| Ctrl+F11 | Ctrl-Reset |
| Ctrl+F12 | Redémarrage à froid de l'Apple |
| Alt gauche | Pomme ouverte (bouton 0) |
| Touche Windows ou Menu | Pomme pleine (bouton 1) ; aussi Alt droite avec le clavier QWERTY |
| Flèches | Flèches de l'Apple (gauche : retour arrière) |
| Retour arrière, Suppr | Flèche gauche, touche DELETE du //e |
| Verr Maj | Éteint : majuscules (par défaut, comme le veulent la plupart des programmes). Allumé : minuscules et majuscules |
| F9 | Manette sur les flèches, oui ou non : Ctrl droite et Maj droite sont alors les boutons 0 et 1 |
| Ctrl+F9 | Sauvegarde de l'état du jeu, dans un fichier `.A2S` à côté de l'image (`JEU.DSK` donne `JEU.A2S`) |
| Ctrl+F10 | Reprise de l'état sauvé pour l'image en place |
| Pause | Fige l'émulation ; un second appui la relance |
| Arrêt défil | Vitesse maximale ; un second appui revient à la vitesse normale |
| Impr écran | Capture de l'écran dans `A2SHOTnn.BMP`, à la racine de la carte SD |

Le clavier de l'Apple envoie des caractères, pas des positions de touches : la
touche du PC marquée A donne un A. Le menu choisit la disposition du clavier PC,
AZERTY (par défaut) ou QWERTY. Les lettres accentuées n'existent pas sur l'Apple
américain et ne donnent rien.

Dans le choix d'une disquette : **Entrée** insère la disquette dans le lecteur 1
et redémarre l'Apple dessus ; **Espace** l'insère sans redémarrer (pour changer
de disquette en cours de jeu) ; Retour arrière remonte d'un dossier ; une lettre
va au fichier suivant qui commence ainsi. Une image trop grande pour une
disquette 5,25 pouces (800 Ko et plus) est montée comme disque dur, et l'Apple
redémarre dessus ; « Retirer le disque dur » rend le démarrage à la disquette. Les disquettes en place sont
mémorisées et remontées au démarrage suivant.

Le menu principal rappelle ce qui est inséré et ouvre des sous-menus ; sous
chaque choix, une ou deux lignes l'expliquent. Échap ou Retour arrière revient
au menu précédent, F12 ferme. « Langues/Languages » passe du français à
l'anglais.

- **Disquettes et disque dur** : insérer dans le lecteur 1 ou 2, monter un
  disque dur, éjecter, échanger les deux disquettes.
- **Derniers fichiers** : les huit dernières images utilisées, la plus récente
  d'abord.
- **Modèle** (réglages de la machine) : choisi puis appliqué par « Appliquer
  le modèle », avec un redémarrage de l'Apple.
- **Sauver / reprendre l'état du jeu** : toute la machine (6502, mémoire,
  lecteurs, carte son) est écrite sur la carte SD ; le contenu des disquettes
  n'en fait pas partie. Un état reprend son modèle d'Apple.
- **Réglages par jeu** (réglages de la machine, non par défaut) : le modèle,
  la manette, le moniteur et la double haute résolution sont mémorisés pour
  chaque image, dans `A2GAMES.CFG` sur la carte SD, et entrent en vigueur au
  démarrage de l'Apple. Un jeu sans réglages propres suit les réglages
  généraux.
- **Double haute résolution** (réglages vidéo) : couleur ou monochrome, sans
  changer le reste de l'affichage.
- **Caractères** (réglages vidéo) : américains ou français, sur //e et //c.
  En français, les lettres accentuées remplacent @ [ \ ] { | } # ~ comme sur
  un Apple français (l'invite du BASIC devient §), et les touches é, è, ç, à,
  ù, °, §, £ du clavier AZERTY donnent ces caractères.
- **Infos à l'écran** : rien, les voyants des lecteurs (par défaut), ou les
  voyants et la vitesse (images par seconde et part du temps d'une image prise
  par l'émulation).
- **Moniteur** : couleur, ou monochrome blanc, vert, ambre (sans les couleurs
  d'artefact de la haute résolution, donc plus net pour le texte).
- **Lignes de balayage** : une ligne VGA sur deux en noir.
- **Disquette rapide** : tant que le lecteur tourne, l'émulation va aussi vite
  que l'ESP32 le permet. Le son est alors haché.
- **Mockingboard** : présence de la carte son dans le slot 4 (oui par défaut).
- **Disque dur sur le //c** : en ROM 0, 3 et 4, l'image est un disque
  SmartPort branché à la place du lecteur externe. Un vrai //c ne démarre de
  lui-même que sur sa disquette : sans disquette dans le lecteur 1,
  l'émulateur fait donc Ctrl-Reset puis tape `PR#5` à votre place. Avec une
  disquette, c'est elle qui démarre, et le disque reste accessible (slot 5).
  La ROM 255 ne connaît pas le SmartPort.
- **Sur le //c Plus** : une image de 800 Ko montée comme disque dur est la
  disquette du lecteur 3,5 pouces interne, et la machine démarre dessus ; une
  image plus grande est un disque SmartPort, qu'elle trouve aussi d'elle-même.
  La disquette 5,25 pouces du lecteur 1 ne démarre que s'il n'y a ni l'une ni
  l'autre. Une disquette que le programme éjecte revient d'elle-même au bout
  de quelques secondes.
- **Sur le //c** : la souris est intégrée (le réglage « Carte souris » sert
  seulement à mettre en service la souris PS/2 de la seconde prise) ; ce que le
  port 1 imprime (`PR#1`) est ajouté à `PRINTER.TXT`, à la racine de la carte SD.
- **Carte souris** (réglages de la machine, non par défaut) : carte AppleMouse II
  dans le slot 2. La souris PS/2 se branche sur la seconde prise de la carte ;
  elle est prise en compte au redémarrage de l'ESP32. Bouton gauche : bouton de
  la souris de l'Apple.

Transfert de fichiers par WiFi : le choix « Transfert de fichiers WiFi... » du
menu principal fait redémarrer la carte sur un second firmware, qui sert la
carte SD à un navigateur web : on peut y envoyer des fichiers, en télécharger,
en supprimer et créer des dossiers, sans sortir la carte SD.

1. Sans rien préparer, la carte crée son propre réseau WiFi
   `Apple2_ProjectESP32` (mot de passe `apple2esp32`). Y connecter le PC ou le
   téléphone, puis ouvrir `http://192.168.4.1`.
2. Ou bien créer à la racine de la carte SD un fichier `WIFI.TXT` avec le nom
   du réseau sur la première ligne et son mot de passe sur la seconde : la
   carte le rejoint, et affiche à l'écran l'adresse à ouvrir.

Pour revenir à l'émulateur : le bouton de la page, la touche Échap sur la
carte, ou simplement éteindre et rallumer. Le second firmware s'installe une
fois avec `.\build.ps1 -Env wifi -Flash` ; tant qu'il ne l'est pas, le menu le
dit.

En double haute résolution, le texte fin d'un programme comme A2DeskTop est
illisible en couleur, comme sur un moniteur couleur d'époque : passer la
double haute résolution en monochrome.

Un chiffre en bas à droite de l'écran signale le lecteur qui travaille.

## Compiler et flasher

Prérequis : Python avec PlatformIO (`pip install platformio`) et `setuptools<81`.

```powershell
.\build.ps1                     # compile
.\build.ps1 -Flash              # compile et flashe
.\build.ps1 -Flash -Monitor 20  # ... puis affiche 20 s de journal série
.\build.ps1 -Monitor 70         # journal série seul (la carte est redémarrée)
.\build.ps1 -Env wifi -Flash    # second firmware : transfert de fichiers par WiFi
```

ESP-IDF refuse les chemins contenant des espaces : le script recopie donc
`Apple2_ProjectESP32\` dans `C:\Apple2_ProjectESP32_build\` et compile là-bas.
Les sources de référence sont celles de ce dossier.

Flasher remplace le firmware présent sur la carte (par exemple
CPC_ProjectESP32), qu'il faut reflasher pour le retrouver. Les réglages de
l'Apple sont dans leur propre espace de la flash (`apple2`).

Toutes les minutes, le firmware écrit sa vitesse sur le port série.

Les ROM sont embarquées dans le firmware : `python scripts/rom2header.py`
régénère `include/roms_apple2.h` à partir de `RomsApple/`.

## Console série

Le firmware écoute le port série (115 200 bauds) : on peut l'observer et le
piloter sans écran ni clavier. `scripts/a2console.py` envoie les commandes :

```powershell
python scripts\a2console.py s                         # état : 6502, vitesse, lecteurs, mémoire
python scripts\a2console.py r "wait 1" "k PRINT 6*7\r" "wait 1" t    # tape une ligne, lit l'écran texte
python scripts\a2console.py "put FilesDSK\Apple2\JEU.dsk=/sd/JEU.DSK" "d1 /sd/JEU.DSK" b
python scripts\a2console.py "shot ecran.png"          # l'image affichée, en PNG (20 s)
python scripts\a2console.py menu "vk down" "vk enter" # le menu, touche par touche
```

`put` copie un fichier du PC sur la carte SD sans la sortir (30 s pour une
disquette). La liste des commandes est en tête de
`Apple2_ProjectESP32/src/SerialConsole.cpp`.

## Banc de test sur PC

`host\build.bat` (MSVC, Build Tools de Visual Studio) compile le cœur
d'émulation en `host\build\a2host.exe`, piloté par une suite d'actions :

```powershell
host\build\a2host.exe model=3 "disk1=FilesDSK\Apple2\Karateka (1984)(Broderbund).dsk" boot run=1800 shot=karateka.png state
host\build\a2host.exe model=1 boot run=100 reset run=30 "type=PRINT 6*7\r" run=30 text
```

La liste des actions est en tête de `host/main.cpp` : insertion de disquette,
frappe de texte, capture PNG, écran texte, son en WAV, état du 6502, mesure de
vitesse, tests de Klaus Dormann.

## Architecture

```
app_main (main.cpp)
 └─ Emu::setup()   RAM, VGA, SD, disquettes mémorisées, PS/2, tâche audio
 └─ Emu::loop()    une image Apple à chaque retour vertical VGA (59,94 Hz), cœur 0
      ├─ Keyb::process()    clavier PS/2 -> caractère, boutons, manette, touches F11/F12
      └─ A2::runFrame()     262 lignes de 65 cycles de 6502
           ├─ cpuRun(65)         les accès à $C000-$CFFF passent par ioRead / ioWrite
           └─ renderLine(y)      la ligne qui vient d'être balayée -> A2_platformLine -> framebuffer VGA
```

| Fichier | Rôle |
|---------|------|
| `src/core/A2.h` | Interface du cœur d'émulation, sans dépendance à l'ESP32 |
| `src/core/Cpu6502.cpp` | 6502 (avec ses instructions non documentées stables) et 65C02, durée comptée par instruction |
| `src/core/A2Machine.cpp` | Tables de pages, soft switches du ][ et du //e, carte langage, clavier, manettes, haut-parleur, boucle d'une image |
| `src/core/A2Video.cpp` | Rendu d'une ligne de 560 points dans chaque mode, bus flottant |
| `src/core/A2Disk.cpp` | Carte Disk II et IWM du //c : moteur pas à pas, conversion d'une piste en nibbles et retour, lecteur WOZ au bit près, reconnaissance des formats d'image, aiguillage vers le lecteur 3,5 pouces et le bus SmartPort |
| `src/core/A2Hdd.cpp` | Disque dur ProDOS : firmware de la carte (écrit pour ce projet), commandes par blocs |
| `src/core/A2Mockingboard.cpp` | Mockingboard : 6522 (compteurs, interruptions) et AY-3-8910 |
| `src/core/A2IIc.cpp` | Ce que le //c a en propre : souris en quadrature, interruption de retour vertical, ports série, ROM commutée, extension mémoire ; circuit MIG et registres de l'accélérateur du //c Plus |
| `src/core/A2Disk35.cpp` | Lecteur 3,5 pouces du //c Plus : registres et commandes du lecteur, secteurs codés à la volée, décodage de ce qui est écrit |
| `src/core/A2SmartPort.cpp` | Disque SmartPort du //c : paquets échangés avec l'IWM, commandes par blocs |
| `src/core/A2Mouse.cpp` | Carte souris : PIA 6821 et dialogue avec son microcontrôleur, porté d'AppleWin |
| `src/WifiApp.cpp` | Second firmware (environnement `wifi`, partition ota_1) : serveur web qui donne accès à la carte SD, repris de CPC_ProjectESP32 |
| `src/Emu.cpp` | Initialisation, boucle principale, tâche audio |
| `src/Video.cpp` | Sortie VGA : palette, lignes de balayage, texte par-dessus l'image, capture |
| `src/A2Keyboard.cpp` | Clavier PS/2 : dispositions AZERTY et QWERTY, boutons, manette au clavier |
| `src/DiskImage.cpp` | Images de disquette sur la SD, copiées en PSRAM, écritures reportées dans le fichier |
| `src/OSD.cpp` | Menu et choix de fichiers, affichés avec les caractères de l'Apple |
| `src/SerialConsole.cpp` | Console de diagnostic sur le port série |
| `src/ESPConfig.cpp`, `src/FileUtils.cpp` | Réglages en NVS, montage de la carte SD |
| `src/ESP32Lib/`, `components/` | Pilote VGA de bitluni, FabGL (PS/2), pwm_audio |

Choix d'émulation à connaître :

- **Vidéo** : le framebuffer fait 560 x 192 points, la largeur du 80 colonnes
  et de la double haute résolution. Il part dans un signal VGA 640x480 à 60 Hz,
  chaque ligne deux fois, entouré de noir. Les modes à 280 points occupent deux
  points chacun, ce qui laisse la place du décalage d'un demi-point des octets
  haute résolution à bit 7 levé. Une ligne est rendue juste après ses 65 cycles :
  les changements de mode en cours d'image sont rendus à la ligne près. Une
  ligne dont le mode et les octets n'ont pas changé n'est pas redessinée : c'est
  ce qui ramène le calcul d'une image de 16 ms à 4 ms.
- **Couleurs haute résolution** : règle des voisins (point isolé coloré selon
  sa colonne et le bit 7, deux points voisins blancs, trou entre deux points
  rempli), pas une simulation du signal NTSC.
- **Disquette** : un octet se présente tous les 32 cycles, comme sur un vrai
  lecteur, mais le disque attend le programme : un octet non lu n'est jamais
  perdu. Un secteur écrit est décodé et reporté dans l'image dès la fin de son
  écriture ; seuls les secteurs qui ont changé sont écrits.
- **Images `.woz`** : un autre lecteur, au bit près. Le disque tourne avec le
  temps du 6502 (un bit toutes les 4 cycles, ou la durée propre à l'image),
  qu'on le lise ou non, et le registre de la carte se remplit bit par bit ;
  quarts de piste, bits faibles et position angulaire au changement de piste
  sont respectés. Un léger glissement à chaque tour évite qu'une boucle de
  lecture calée sur la durée d'un tour retombe indéfiniment au même endroit.
- **Mémoire** : la RAM principale est en mémoire interne, la RAM auxiliaire du
  //e et les images de disquette en PSRAM. L'image du disque dur reste sur la
  carte SD, lue bloc par bloc.
- **Rendu** : il écrit directement les octets du framebuffer VGA
  (`A2::setPixelMap`), sans tampon de numéros de couleur à convertir.

## Contenu du dépôt

| Dossier | Contenu |
|---------|---------|
| `Apple2_ProjectESP32/` | Le firmware (projet PlatformIO / ESP-IDF 4.4.5, carte `pico32`) |
| `host/` | Banc de test sur PC du cœur d'émulation |
| `scripts/` | `rom2header.py` (ROM vers `roms_apple2.h`), `read_serial.py` (journal série), `a2console.py` (console série) |

## Licence et crédits

GPL v3, comme les projets dont le code est issu :

- [ESPectrum](https://github.com/EremusOne/ESPectrum) (Víctor Iborra, David Crespo) — architecture, pilote audio ;
- [ESP32Lib](https://github.com/bitluni/ESP32Lib) (bitluni) — sortie VGA ;
- [FabGL](https://github.com/fdivitto/FabGL) (Fabrizio Di Vittorio) — clavier PS/2 ;
- [apple2ts](https://github.com/ct6502/apple2ts) (Chris Torrence) — tables du moteur pas à pas du Disk II ;
- [AppleWin](https://github.com/AppleWin/AppleWin), [LinApple](https://github.com/linappleii/linapple), [izapple2](https://github.com/ivanizag/izapple2) — références de comportement ;
- [tests 6502](https://github.com/Klaus2m5/6502_65C02_functional_tests) de Klaus Dormann ;
- [pt3_player](http://www.deater.net/weave/vmwprod/pt3_player/) de Vince Weaver, qui a servi à essayer la Mockingboard.

Les ROM de l'Apple II restent la propriété d'Apple.
