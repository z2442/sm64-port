/* Texts of the installer. One row per message: id, then English, French, German, Spanish, Italian and Japanese.
 *
 * - The built-in debug font of the PSP is plain ASCII, so the Latin texts are written without accents.
 * - It has no kana or kanji either: the Japanese column is NOT compiled. tools/psp/gen_installer_ja.py renders it to small bitmaps
 *   (psp/installer/text_ja.inc, committed). Run that script again after changing a Japanese text or adding a message.
 * - Keep one row per line group; the generator reads this file. Add new messages at the end of the list. */
#ifndef INSTALLER_TEXT_H
#define INSTALLER_TEXT_H

#define SMI_MESSAGES(X) \
    X(M_FIRST, "First run: preparing the game from your ROM.", \
               "Premier lancement : preparation du jeu avec votre ROM.", \
               "Erster Start: Das Spiel wird aus deiner ROM vorbereitet.", \
               "Primera vez: preparando el juego con tu ROM.", \
               "Primo avvio: preparazione del gioco dalla tua ROM.", \
               "初回起動：ROMからゲームを準備しています。") \
    X(M_STAGE_HASH, "Checking the ROM...", "Verification de la ROM...", "ROM wird geprueft...", "Comprobando la ROM...", \
                    "Controllo della ROM...", "ROMを確認しています...") \
    X(M_STAGE_COPY, "Copying the template...", "Copie du modele...", "Vorlage wird kopiert...", "Copiando la plantilla...", \
                    "Copia del modello...", "テンプレートをコピーしています...") \
    X(M_STAGE_BUILD, "Building the game...", "Creation du jeu...", "Spiel wird erstellt...", "Construyendo el juego...", \
                     "Creazione del gioco...", "ゲームを構築しています...") \
    X(M_STAGE_VERIFY, "Verifying...", "Verification...", "Pruefung...", "Verificando...", "Verifica...", "検証しています...") \
    X(M_MISSING, "Missing ROM:", "ROM manquante :", "ROM fehlt:", "Falta la ROM:", "ROM mancante:", "ROMが見つかりません：") \
    X(M_NONE_MATCH, "There are 8 MB files, but none is the right ROM.", \
                    "Des fichiers de 8 Mo existent, mais aucun n'est la bonne ROM.", \
                    "Es gibt 8-MB-Dateien, aber keine ist die richtige ROM.", \
                    "Hay archivos de 8 MB, pero ninguno es la ROM correcta.", \
                    "Ci sono file da 8 MB, ma nessuno e la ROM giusta.", \
                    "8MBのファイルはありますが、正しいROMではありません。") \
    X(M_COPY_TO, "Copy your ROM (.z64, .v64 or .n64, any file name) to:", \
                 "Copiez votre ROM (.z64, .v64 ou .n64, nom libre) dans :", \
                 "Kopiere deine ROM (.z64, .v64 oder .n64, beliebiger Name) nach:", \
                 "Copia tu ROM (.z64, .v64 o .n64, con cualquier nombre) a:", \
                 "Copia la tua ROM (.z64, .v64 o .n64, qualsiasi nome) in:", \
                 "お持ちのROM（.z64 / .v64 / .n64、ファイル名は自由）を次のフォルダにコピーしてください：") \
    X(M_MUST_BE, "It must be the original 8 MB ROM of this version.", \
                 "Ce doit etre la ROM originale de 8 Mo de cette version.", \
                 "Es muss die originale 8-MB-ROM dieser Version sein.", \
                 "Debe ser la ROM original de 8 MB de esta version.", \
                 "Deve essere la ROM originale da 8 MB di questa versione.", \
                 "この版のオリジナルの8MB ROMが必要です。") \
    X(M_PRESS, "Press any button to exit.", "Appuyez sur un bouton pour quitter.", "Beliebige Taste zum Beenden druecken.", \
               "Pulsa cualquier boton para salir.", "Premi un tasto per uscire.", "いずれかのボタンで終了します。") \
    X(M_DONE, "Done. Starting the game...", "Termine. Lancement du jeu...", "Fertig. Das Spiel startet...", \
              "Listo. Iniciando el juego...", "Fatto. Avvio del gioco...", "完了。ゲームを起動します...") \
    X(M_CODE, "Error code:", "Code d'erreur :", "Fehlercode:", "Codigo de error:", "Codice errore:", "エラーコード：") \
    X(E_IO, "Read/write error (Memory Stick full?).", "Erreur de lecture/ecriture (Memory Stick plein ?).", \
            "Lese-/Schreibfehler (Memory Stick voll?).", "Error de lectura/escritura (Memory Stick lleno?).", \
            "Errore di lettura/scrittura (Memory Stick pieno?).", "読み書きエラー（メモリースティックの空き容量不足？）") \
    X(E_MANIFEST, "manifest.bin is damaged or from another version.", "manifest.bin est endommage ou d'une autre version.", \
                  "manifest.bin ist beschaedigt oder von einer anderen Version.", \
                  "manifest.bin esta danado o es de otra version.", "manifest.bin e danneggiato o di un'altra versione.", \
                  "manifest.bin が破損しているか、別のバージョンのものです。") \
    X(E_MEMORY, "Not enough memory.", "Memoire insuffisante.", "Nicht genug Speicher.", "Memoria insuficiente.", \
                "Memoria insufficiente.", "メモリが足りません。") \
    X(E_TEMPLATE, "SM64.template.prx is missing, damaged or does not match manifest.bin.", \
                  "SM64.template.prx est absent, endommage ou ne correspond pas a manifest.bin.", \
                  "SM64.template.prx fehlt, ist beschaedigt oder passt nicht zu manifest.bin.", \
                  "SM64.template.prx falta, esta danado o no corresponde a manifest.bin.", \
                  "SM64.template.prx manca, e danneggiato o non corrisponde a manifest.bin.", \
                  "SM64.template.prx が無い、破損している、または manifest.bin と一致しません。") \
    X(E_DECODE, "The ROM data is damaged.", "Les donnees de la ROM sont endommagees.", "Die ROM-Daten sind beschaedigt.", \
                "Los datos de la ROM estan danados.", "I dati della ROM sono danneggiati.", "ROMのデータが破損しています。") \
    X(E_VERIFY, "The result is not the expected one (damaged ROM or files).", \
                "Le resultat n'est pas celui attendu (ROM ou fichiers endommages).", \
                "Das Ergebnis ist nicht das erwartete (ROM oder Dateien beschaedigt).", \
                "El resultado no coincide con el esperado (ROM o archivos danados).", \
                "Il risultato non e quello atteso (ROM o file danneggiati).", \
                "結果が期待と一致しません（ROMまたはファイルの破損）。") \
    X(E_UNKNOWN, "Unknown error.", "Erreur inconnue.", "Unbekannter Fehler.", "Error desconocido.", "Errore sconosciuto.", \
                 "不明なエラー。") \
    X(E_NO_PATH, "The EBOOT path is unknown.", "Le chemin de l'EBOOT est inconnu.", "Der EBOOT-Pfad ist unbekannt.", \
                 "No se conoce la ruta del EBOOT.", "Il percorso dell'EBOOT e sconosciuto.", "EBOOTのパスが不明です。") \
    X(E_BAD_PATH, "Invalid EBOOT path.", "Chemin de l'EBOOT invalide.", "Ungueltiger EBOOT-Pfad.", "Ruta del EBOOT no valida.", \
                  "Percorso dell'EBOOT non valido.", "EBOOTのパスが正しくありません。") \
    X(E_LOAD, "Could not load SM64.PRX.", "Impossible de charger SM64.PRX.", "SM64.PRX konnte nicht geladen werden.", \
              "No se pudo cargar SM64.PRX.", "Impossibile caricare SM64.PRX.", "SM64.PRX を読み込めませんでした。") \
    X(E_START, "Could not start SM64.PRX.", "Impossible de demarrer SM64.PRX.", "SM64.PRX konnte nicht gestartet werden.", \
               "No se pudo iniciar SM64.PRX.", "Impossibile avviare SM64.PRX.", "SM64.PRX を起動できませんでした。") \
    X(R_US, "US (NTSC-U)", "Etats-Unis (NTSC-U)", "USA (NTSC-U)", "EE. UU. (NTSC-U)", "USA (NTSC-U)", "北米版 (NTSC-U)") \
    X(R_EU, "Europe (PAL)", "Europe (PAL)", "Europa (PAL)", "Europa (PAL)", "Europa (PAL)", "ヨーロッパ版 (PAL)") \
    X(R_JP, "Japan (NTSC-J)", "Japon (NTSC-J)", "Japan (NTSC-J)", "Japon (NTSC-J)", "Giappone (NTSC-J)", "日本版 (NTSC-J)") \
    X(R_SH, "Shindou (Rumble Pak Edition)", "Shindou (version Rumble Pak)", "Shindou (Rumble-Pak-Edition)", \
            "Shindou (edicion Rumble Pak)", "Shindou (edizione Rumble Pak)", "振動パック対応版 (Shindou)")

#endif
