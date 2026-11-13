# PSP版 スーパーマリオ64

🌐 [English](README.md) · [Español](README.es.md) · [Français](README.fr.md) · [Deutsch](README.de.md) · **日本語**

**スーパーマリオ64** を **Sony PSP** 向けにネイティブ移植したものです。**US版・日本版(JP)・ヨーロッパ版(EU)・Shindou版(SH)** に対応し、
[sm64-port](https://github.com/sm64-port/sm64-port) / [n64decomp/sm64](https://github.com/n64decomp/sm64) の逆コンパイル、
および [z2442](https://github.com/z2442/sm64-port) のPSP移植をベースにしています。

> この文書は[英語版README](README.md)の翻訳です。内容に差異がある場合は英語版が優先されます。非常に技術的な部分(実験的なTV出力、開発者向け情報)は英語版に詳しく書かれており、ここでは要約しています。
> 翻訳はネイティブスピーカーによる確認を受けていません。修正のご提案を歓迎します。

**この移植でできること**

- **30 / 60 FPS** の切り替え、フレームペーシングの設定。
- サウンドはPSPの **Media Engine**(2つ目のCPU)で生成され、メインCPUをゲームに回せます。
- ポーズ画面から開ける **ゲーム内オプションメニュー**(3ページ):**Graphics**、**Audio**(音楽・効果音の音量とサウンドモード)、**Controls**(全ボタンを自由に割り当て可能)。
- 設定はすべて、手で編集もできる **テキストファイル** `sm64config.txt` に保存されます。
- セーブデータは通常のPSPゲームと同様にメモリースティックに保存されます。
- **同じコードから4つのバージョン**:US、JP、EU(英語・フランス語・ドイツ語、PSPの16:9画面に合わせたPALレイアウト)、Shindou(実験的)。[対応バージョン](#対応バージョン)を参照してください。

> **このリポジトリにはゲームデータは含まれていません。** ビルド時にPSP用へ変換するため、
> 使用するバージョンの **スーパーマリオ64のROMをご自身でご用意ください**。ROMは含まれておらず、配布もしていません。
> 本プロジェクトはファンによるもので、任天堂とは一切関係ありません。

---

## 目次

1. [必要なもの](#1-必要なもの)
2. [Windowsでビルドする(手順)](#2-windowsでビルドする手順)
3. [PSPにインストールする(またはPPSSPPで開く)](#3-pspにインストールするまたはppssppで開く)
4. [遊ぶ:操作・オプションメニュー・セーブ](#4-遊ぶ操作オプションメニューセーブ)
5. [トラブルシューティング](#5-トラブルシューティング)
6. [Linux / WSLでビルドする](#6-linux--wslでビルドする)
7. [開発者向け](#7-開発者向け)
8. [クレジット](#8-クレジット)

---

## 1. 必要なもの

| 項目 | 詳細 |
|---|---|
| **PC** | Windows 10 または 11(64ビット)、空き容量 約 **10 GB**、インターネット接続(初回ビルド時のみ)。 |
| **スーパーマリオ64のROM** | ビルドしたいバージョンの **`.z64`** 形式(8 MB、ビッグエンディアン)。正確なファイル名とSHA-1は下の表を参照。 |
| **Docker Desktop** | 個人利用は無料。PSP用コンパイラ一式が入っているため、開発ツールのインストールは不要です。 |
| **カスタムファームウェア入りのPSP** | ARK-4、PRO CFWなど、およびメモリースティック。*または*エミュレータ [PPSSPP](https://www.ppsspp.org/)。 |

PCにインストールするのはDocker Desktopだけです。それ以外(コンパイラ、Python、ライブラリ)は、ビルドスクリプトが作るDockerコンテナ内にあります。

**PSP-1000(RAM 24 MB):** PSP-1000のメモリに収まるよう設計されており、24 MBモデルに設定したPPSSPPで約2 MBの空きヒープを残して起動・動作します。ただし **実機のPSP-1000ではまだテストしていません**。実験的な扱いとしてください。TV出力はPSP-1000では使えません。

### 対応バージョン

| バージョン | 用意するファイル | ROMのSHA-1 | 状態 |
|---|---|---|---|
| **US**(北米) | `baserom.us.z64` | `9bef1128717f958171a4afac3ed78ee2bb4e86ce` | メインのバージョン。最もテストされています。 |
| **JP**(日本) | `baserom.jp.z64` | `8a20a5c83d6ceb0f0506cfc9fa20d8f438cafe51` | ビルド可能。テストは少なめ。 |
| **EU**(ヨーロッパ) | `baserom.eu.z64` | `4ac5721683d0e0b6bbb561b58a71740845dceea9` | 英語・フランス語・ドイツ語。PSP実機とPPSSPPでテスト済み。 |
| **SH**(Shindou、日本 1997) | `baserom.sh.z64` **と** `baserom.jp.z64` | `3f319ae697533a255a1003d09202379d78d5a2e0`(SH) | **実験的。** 映像とゲームはSH ROM由来ですが、サウンドはまだJP ROM由来で、本物のShindouの音ではありません。 |

各ビルドは独立したゲームです。ビルドしたバージョンがそのまま得られます。ROMのファイル名は表のとおり(すべて小文字)にしてください。

**既知の制限**

- Shindou版は **両方の** ROM(SHとJP)が必要で、日本版のサウンドデータとオーディオエンジンを使用します(元のShindouのサウンドエンジンは未移植)。振動(Rumble)は使えません(PSPには振動モーターがありません)。
- 全バージョンが同じ保存フォルダとファイル名(`PSP\SAVEDATA\SM64PORT\sm64_save_file.bin`)を使います。バージョン間のセーブ互換は保証されません。**別バージョンに切り替える前にこのファイルをバックアップしてください。**

---

## 2. Windowsでビルドする(手順)

### ステップ1 — ROMを確認する

(例はUS版です。他のバージョンは[表](#対応バージョン)のSHA-1と比較してください。)

1. Windowsで **拡張子を表示** します:エクスプローラーの **表示 → 表示 → ファイル名拡張子**。
   (非表示のままだと、`baserom.us.z64` に改名したつもりが `baserom.us.z64.z64` になり、ビルドで見つかりません。)
2. スタートボタンを右クリック → **ターミナル**(または **Windows PowerShell**)で、ROMのパスを指定して実行します:

   ```powershell
   Get-FileHash -Algorithm SHA1 "C:\ROMのパス\rom.z64"
   ```

3. 結果が `9BEF1128717F958171A4AFAC3ED78EE2BB4E86CE`(US)と完全に一致する必要があります。
   異なる場合は、別バージョンのROMか別形式(`.v64` / `.n64`)です。ビルドには `.z64` 形式が必要です。

### ステップ2 — Docker Desktopをインストールする

1. <https://www.docker.com/products/docker-desktop/> からダウンロードしてインストーラーを実行します。
   **「Use WSL 2 instead of Hyper-V」** は有効のままにします(既定)。
2. インストーラーに求められたらPCを再起動します。
3. スタートメニューから **Docker Desktop** を開き、利用規約に同意します。左下に
   **「Engine running」**(緑)と表示されるまで待ちます。**ビルド中は常にDocker Desktopを開いておく必要があります。**
4. 確認のため、PowerShellで次を実行します:

   ```powershell
   docker --version
   ```

   `Docker version 27.x.x` のように表示されるはずです。

> Dockerが **仮想化** について警告する場合は、PCのBIOS/UEFIで有効にしてください
> (「Intel VT-x」「SVM Mode」「Virtualization Technology」などを探します)。[トラブルシューティング](#5-トラブルシューティング)も参照。

### ステップ3 — プロジェクトをダウンロードする

追加ソフト不要の最も簡単な方法:

1. このリポジトリのページで緑の **Code** ボタン → **Download ZIP** をクリックします。
2. ZIPを、**短く、スペースや日本語を含まないフォルダ**(例:`C:\sm64-psp`)に展開します。
   長すぎるパスや、OneDriveで同期されるフォルダは避けてください。

*(すでにGitを使っている場合は `git clone https://github.com/Edwintendo/sm64-port-psp.git` でも構いません。)*

### ステップ4 — ROMをプロジェクトフォルダに置く

1. ROMをプロジェクトフォルダの **`Makefile` と同じ場所** にコピーします。
2. 名前を正確に **`baserom.<version>.z64`**(すべて小文字)に変更します。`<version>` は `us`、`jp`、`eu`、`sh` のいずれかです(例:`baserom.us.z64`)。
   Shindou版は2つのファイル `baserom.sh.z64` と `baserom.jp.z64` が必要です。

フォルダには `Makefile`、`build_psp.ps1`、`Dockerfile.psp`、`src`、そしてROM(例:`baserom.us.z64`)などがあるはずです。

### ステップ5 — ビルドスクリプトを実行する

1. エクスプローラーでプロジェクトフォルダを開き、アドレスバーの空いた部分をクリックして `powershell` と入力し **Enter** を押します。
   (フォルダ内でPowerShellが開きます。Windows 11では、空白部分を右クリック → **ターミナルで開く** でも可。)
2. 次を実行します:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\build_psp.ps1
   ```

   これは **US** 版をビルドします。他のバージョンは `-Version` を付けます:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\build_psp.ps1 -Version eu
   ```

   (`us`、`jp`、`eu`、`sh`。)`-ExecutionPolicy Bypass` は設定を変えずに、今回だけスクリプトの実行を許可します。

3. 待ちます。**初回** は次の3つが行われ、時間がかかります:
   1. PSP用コンパイラのダウンロードと、`sm64psp` という小さなDockerイメージの作成(数分、ネット接続が必要)。
   2. ROMからの画像・音声の抽出。
   3. ゲームのコンパイル(最も長い部分。画面が文字で埋まりますが正常です)。
4. 最後に次のように表示されます:

   ```
   Listo: build\us_psp\mario64\  (copia EBOOT.PBP y SM64.PRX juntos a la misma carpeta de la PSP)
   ```

   (メッセージはスペイン語です。フォルダ名はバージョンにより `build\eu_psp\mario64\`、`build\jp_psp\mario64\`、`build\sh_psp\mario64\` になります。)

   2回目以降はずっと速くなります。変更した部分だけが再コンパイルされます。

**スクリプトのオプション**

| コマンド | 効果 |
|---|---|
| `.\build_psp.ps1 -Version eu` | バージョンを選択:`us`(既定)、`jp`、`eu`、`sh`。 |
| `.\build_psp.ps1 -Clean` | そのバージョンの前回のビルドを先に削除します。おかしくなった時に使います。 |
| `.\build_psp.ps1 -Opt O2` | `-O3` の代わりに `-O2` でビルド(別フォルダ `build_O2\`)。 |
| `.\build_psp.ps1 -Rebuild` | Dockerイメージを作り直します(`Dockerfile.psp` を変更した時のみ)。 |

(`powershell -ExecutionPolicy Bypass -File` を先頭に付けるのを忘れずに。例:
`powershell -ExecutionPolicy Bypass -File .\build_psp.ps1 -Version eu -Clean`)

### ステップ6 — 成果物を見つける

ゲームは **`build\<version>_psp\mario64\`**(例:`build\us_psp\mario64\`)にある **`EBOOT.PBP`** と **`SM64.PRX`** の2ファイルです。常に同じフォルダに一緒に置いてください。`EBOOT.PBP` は小さなローダーで、`SM64.PRX` がゲーム本体です(これにより実機での起動が大幅に速くなります)。

---

## 3. PSPにインストールする(またはPPSSPPで開く)

### 実機のPSP

1. PSPをUSBケーブルでPCにつなぎ、XMBで **USB接続** を有効にします。または、メモリースティックを取り出してカードリーダーに挿します。
2. メモリースティックの **`PSP`** フォルダ → **`GAME`** を開きます。
3. 新しいフォルダ **`SM64`**(複数バージョンなら別名、例:`SM64_EU`)を作り、`EBOOT.PBP` と `SM64.PRX` の **両方** をコピーします。パスは次のとおりです:

   ```
   PSP\GAME\SM64\EBOOT.PBP
   PSP\GAME\SM64\SM64.PRX
   ```

4. PSPを取り外します。XMBの **ゲーム → メモリースティック** に **Super Mario 64** が表示されます。

> ホームブリューを実行するには **カスタムファームウェア**(ARK-4、PRO、MEなど)が必要です。この移植は **ARK-4・333 MHz** のPSPで開発・テストしました。
> **PSP go** の場合は、本体メモリの `PSP\GAME` フォルダにコピーしてください。

### PCなしでリリースからインストールする

お使いのバージョンのリリースが公開されていれば(GitHub → *Releases*)、ビルドもDockerも不要です。リリースにはゲームデータは含まれず、初回起動時に **ご自身のROM** から自動的に準備されます。

1. バージョンに合ったzip(`SM64-PSP-us.zip`、`-jp`、`-eu`、`-sh`)をダウンロードして展開し、**`PSP`** フォルダをメモリースティックのルートにコピーします(確認が出たらマージ)。
2. ご自身のROMを `PSP\GAME\SM64_<VERSION>\data\` に置きます(ファイル名は自由、`.z64`・`.v64`・`.n64` 可)。Shindou版はSHとJPの両方のROMが必要です。
3. XMBから **Super Mario 64** を起動します。初回はROMを確認してゲームを組み立てます(進行バーが表示され、時間がかかることがあります)。完了後にゲームが始まります。次回からは直接起動し、ROM・`SM64.template.prx`・`manifest.bin` は不要になり削除できます。

インストーラーはPSPの言語(英語・フランス語・ドイツ語・スペイン語・イタリア語・日本語)でメッセージを表示します。ROMが見つからないと表示される場合は、そのバージョンの元の8 MBのROMか確認してください(SHA-1は[上の表](#対応バージョン))。インストーラーは実機のPSP-3000でテスト済みです。他の機種(特にPSP-1000)での結果報告を歓迎します。

### オプション:TV出力(実験的)

PSPのビデオケーブル経由でテレビに映像を出力できます。**PSP-2000 / 3000** のみ対応で、**`SM64.PRX` に組み込み済み** の小さな補助モジュールを使います(追加コピーは不要)。**Options → Graphics → TV Settings**(ビデオケーブル接続時のみ開けます)で **TV Output** を **240p**、**480i**、**480p** から選びます(**480p** は *コンポーネント* ケーブルが必要。**240p** と **480i** はコンポジット/AVケーブルでも可)。この機能は **実験的** です。実機のPSP-3000とコンポーネントケーブルでテスト済みですが、**480pは未テスト**(使用したテレビが非対応)で、コンポジット/AV出力も未確認です。画面が真っ黒のままなら、ゲームを再起動してください。TVが約5秒もたなかった場合は、次回起動時に自動的にTV Outputが *Off* に戻ります。詳細(表示領域、オーバースキャン、縦位置、制限、ケーブル接続時の音声、HOMEボタン)は[英語版README](README.md#optional-tv-output-experimental)を参照してください。

### PPSSPP(PC / Android)

PPSSPPを開き、**Load…** で `EBOOT.PBP` を選びます(`SM64.PRX` は同じフォルダに置いたまま)。またはファイルをPPSSPPのウィンドウにドラッグします。

---

## 4. 遊ぶ:操作・オプションメニュー・セーブ

### 既定の操作

| PSP | 機能 |
|---|---|
| **アナログスティック** | マリオの移動 |
| **✕ バツ** | A(ジャンプ) |
| **□ 四角** | B(パンチ / ダイブ) |
| **○ 丸** または **L** | Z(しゃがみ / ヒップドロップ) |
| **R** | R(カメラモード) |
| **△ 三角** | N64の「L」ボタン(ゲームでは未使用) |
| **方向キー** | Cボタン(カメラ。上 = 一人称視点) |
| **START** | ポーズ |

すべてオプションメニューで変更できます。

### オプションメニュー

1. **START** を押してゲームを一時停止します。
2. **R** を押します(ポーズ画面の右下に「**R Options**」と表示されます)。

| ボタン | 動作 |
|---|---|
| 方向キー / スティック **上下** | 項目の移動 |
| 方向キー **左右** または **✕** | 値の変更 / ページを開く |
| **□** または **○** | 戻る |
| **START** または **R** | メニューを閉じる(すべて保存) |

**ページ**

- **Graphics** — FPS Limit(30 / 60)、Frame Pacing(*Keep FPS* / *Keep Speed*)、Object Distance と Far Distance(100%〜300%)、Object Culling(*Original* / *Vertical*)、FPS Counter、Show Sound CPU、**TV Settings**(サブページ、ビデオケーブルが必要)、Reset to Defaults。
- **Audio** — Music Volume と Effects Volume(0〜100、100がゲーム本来の音量)、Sound Mode:Stereo、Mono、Headset。Sound Modeはファイル選択画面と同じ設定です(セーブデータに保存され、どちらで変更しても両方に反映されます)。
- **Controls** — ボタンを選んで **✕** を押すと「Press a button」と表示されるので、割り当てたいPSPのボタンを押します(約5秒待機)。
  そのボタンが別の動作に割り当て済みなら、2つが入れ替わります。ほかに Stick Deadzone と Reset to Defaults があります。

(メニューは英語です。)

### 保存場所

すべてメモリースティックの `PSP\SAVEDATA\SM64PORT\` にあります:

| ファイル | 内容 |
|---|---|
| `sm64_save_file.bin` | ゲームの進行状況(スター、コインなど) |
| `sm64config.txt` | すべての設定(グラフィック、オーディオ、操作)をテキストで保存 |
| `freeze.txt` | フリーズ後のみ:ゲームがその時何をしていたかの簡易レポート(トラブルシューティング参照) |

`sm64config.txt` はメモ帳で開けます。各行は `名前 = 値` の形式です。最初からやり直すには、ファイルを削除する(または *Reset to Defaults* を使う)だけです。

---

## 5. トラブルシューティング

**`docker` が認識されない / 「The term 'docker' is not recognized」**
Docker Desktopが未インストールか、インストール前からPowerShellを開いていました。インストールして、**PowerShellを閉じ、新しいウィンドウを開いて** ください。

**「error during connect…」/「Cannot connect to the Docker daemon」/「docker daemon is not running」**
**Docker Desktop** を開き、**「Engine running」** と表示されるのを待ってからやり直してください。

**Docker DesktopがWSL 2または仮想化が使えないと表示する**
- 管理者権限のPowerShellで `wsl --install` を実行し、再起動します。
- BIOS/UEFIで仮想化(Intel VT-x / AMD SVM)を有効にします。

**「running scripts is disabled on this system」**
スクリプトを次のとおりに実行してください:`powershell -ExecutionPolicy Bypass -File .\build_psp.ps1`

**`Failed to open baserom.us.z64!`**(`jp`、`eu`、`sh` でも同様)
ROMの場所または名前が違います。**`Makefile` の隣** にあり、名前が正確に `baserom.<version>.z64` である必要があります
(拡張子を表示して `baserom.us.z64.z64` になっていないか確認)。Shindouは `baserom.sh.z64` **と** `baserom.jp.z64` が必要です。

**`baserom.us.z64 has the wrong hash!`**(`jp`、`eu`、`sh` でも同様)
そのバージョンのROMではないか、形式が違います。[ステップ1](#ステップ1--romを確認する)をやり直してください。

**ビルドがエラーで止まる、または変更後に挙動がおかしい**
`-Clean` 付きで再実行してください:`powershell -ExecutionPolicy Bypass -File .\build_psp.ps1 -Clean`

**「No space left on device」**
ディスクの空きを作ってください(Dockerは数GB使います)。Docker Desktopの *Troubleshoot → Clean / Purge data* も使えます。

**ビルドがとても遅い**
初回は常に遅いです(ステップ5参照)。プロジェクトは通常のドライブ(ネットワークドライブ以外)に置き、重いプログラムは閉じてください。
Docker Desktop → Settings → Resources でCPUを増やせます。

**XMBにゲームが表示されない**
パスが正確に `PSP\GAME\SM64\EBOOT.PBP` と `PSP\GAME\SM64\SM64.PRX`(2ファイルとも同じフォルダ)で、PSPにカスタムファームウェアが入っているか確認してください。エラー付きのロード画面が出る場合は、`SM64.PRX` がないか別フォルダにあります。破損している場合は再コピーしてください。

**TV Outputを有効にしたら画面が真っ黒になった**
ゲームを再起動してください。TVを有効にして5秒もたなかった場合は、次回起動時にゲームが自動的にTV Outputを *Off* に戻し、PSPの画面を使います。

**ゲームがフリーズする**
約4秒間フレームが描画されないと、ウォッチドッグが `PSP\SAVEDATA\SM64PORT\` に `freeze.txt` を書き出します(フリーズが続く間は4秒ごとに最大12秒まで更新)。メインスレッドの位置、各スレッドの待機先、GPUとオーディオプロセッサの状態、レベル/マリオの動作、バッテリー残量と電圧が記録されます。Issueにこのファイルを添付してください。本体全体が固まってコードが動かなくなると書き出せません。その場合は `freeze.txt` が *ない* こと自体が手がかりになります。

**その他**
リポジトリのGitHubページで *Issue* を作成し、ビルド出力の最後の20〜30行を添付してください。

---

## 6. Linux / WSLでビルドする

LinuxのターミナルやDockerが動くWSLからも、同じDockerイメージを使えます。プロジェクトのルートでROM(例:`baserom.us.z64`)を置いた状態で実行します。他のバージョンは `VERSION=us` を `jp`、`eu`、`sh` に置き換えてください:

```bash
docker build -t sm64psp - < Dockerfile.psp
docker run --rm -v "$PWD:/sm64" -w /sm64 sm64psp make TARGET_PSP=1 pbp VERSION=us -j$(nproc)
```

成果物は同じく `build/<version>_psp/mario64/` の `EBOOT.PBP` + `SM64.PRX` です。(Linuxでは、Dockerが作ったファイルの所有者が `root` になります。必要なら `sudo chown -R $USER .` で直してください。)

**Dockerなし**:[PSPツールチェーン(pspdev)](https://github.com/pspdev/pspdev)と `python3`、`build-essential`/`audiofile` の開発パッケージをインストールし、次を実行します:

```bash
make TARGET_PSP=1 VERSION=us -j4        # ELF/PRXをビルド
make TARGET_PSP=1 VERSION=us pbp        # build/us_psp/mario64/{EBOOT.PBP,SM64.PRX} をビルド
```

サウンドは `libme-core` によりMedia Engineで動作します。EBOOTの隣に外部のサウンドPRXは不要です。

---

## 7. 開発者向け

詳細(gprofによるプロファイリング、元プロジェクトのその他のターゲット、プロジェクト構成)は[英語版README](README.md#7-for-developers)を参照してください。

リリースパッケージ(PCなしでインストールできるもの)の作成:`make TARGET_PSP=1 release VERSION=us -j8`(または `.\build_psp.ps1 -Version us -Release`)。インストーラーは `psp/installer/` にあります。`tools/psp/README.md` を参照。

プルリクエスト歓迎です。大きな変更はまずIssueで相談してください。

---

## 8. クレジット

**ゲームと逆コンパイル**

- **任天堂** — スーパーマリオ64(すべての権利は任天堂に帰属します)。
- **n64decomp/sm64** と全コントリビューター — すべての土台となる逆コンパイル。
- **sm64-port** — 逆コンパイルのポータブル(PC)版。60 fps用のフレーム補間システムは、その `enhancements/60fps.patch` に由来し、本リポジトリでPSP向けに調整しています。
- **Emill と MaikelChan** — `src/pc/gfx/` のC言語製Fast3Dレンダラー(ライセンスは `src/pc/gfx/LICENSE.txt`)。PSPのグラフィック層の基盤です。
- **queueRAM**(sm64tools)、**David Benepe**(n64graphics)、**Silicon Graphics**(IDOコンパイラ) — `tools/` に含まれるツール(それぞれ独自のライセンス)。

**PSP移植**

- **Hayden Kowalchuk(HaydenKow)** — 最初のPSP移植(2020年)。Gitの履歴上、最初のPSPグラフィックバックエンド(sceGu)、操作、テクスチャマネージャ、PSPビルドシステムの作者で、以降の作業はすべてこれを土台にしています。
- **z2442** — PSP移植を継続(VFPUによる頂点変換とクリッピング、Media Engine上のオーディオ)。本プロジェクトのベースとなるリポジトリの所有者です。
- **demmis98** — z2442のリポジトリでのPSP最適化と修正。
- **z2442のOcarina of Time移植(oot-PSP)とDaedalus** — TV出力の補助モジュール(`psp/dvemgr`)はOoT移植のものを取り入れており、それはN64エミュレータDaedalusのPSP向け映像出力方式に基づいています。
- **ghurone / Sonic-R-PSP** — 頂点アリーナのキャッシュ、横方向クリッピングをGEに任せる発想、コンパイラフラグに関するヒントは、PSP向けSonic R移植に由来します。
- **beckerd([psp_mk64_portable](https://github.com/beckerd/psp_mk64_portable))と JustKidding23([Ocarina-of-Time-PSP](https://github.com/JustKidding23/Ocarina-of-Time-PSP)、[z2442/oot-PSP](https://github.com/z2442/oot-PSP)ベース)** — ゲームデータを同梱せず、初回起動時にPSP上でプレイヤー自身のROMから生成するというアイデアは、これらの2つのPSP移植(マリオカート64とゼルダの伝説 時のオカリナ)に由来します。このリポジトリのインストーラー(`psp/installer/`)は別個の実装で、テンプレートとマニフェストから `SM64.PRX` を再構築します。
- **sm64coopdx / sm64ex-coop** — マルチプレイヤーの同期設計の参考(このリリースでは無効)、およびこの移植に合わせて調整したルイージ関連アセット(モデルとテクスチャ、HUDの顔アイコン、サウンドバンク、ボイスサンプル)の出典。ルイージのボイスは同プロジェクトでAndrat氏のクレジットとなっています。sm64coopdxはsm64ex-coop(djoslinと貢献者)とDynOS(PeachyPeach)を基にしています。これらのアセットはそれぞれの作者に帰属します。
- **Claude(Anthropic)** — この移植の後期のコード変更の大半を書いた、または共同で書いたAIアシスタント:60 fps対応とフレームペーシング、描画とGEの最適化、オプションメニュー、JP/EU/SH版への対応、ドキュメント。
- PSPプロジェクトに貢献した他のすべての方々、およびこのリポジトリのGit履歴に名前のある全員(`git shortlog -sn`)。

**使用ツール**

- ビルドに **pspdev**(PSPSDKとPSPツールチェーン)、テストに **PPSSPP**。

実際のゲームデータ(モデル、テクスチャ、音楽、効果音)はここでは配布して **いません**。ご自身が用意したROMから抽出されます。
ただし、リポジトリにはPSPのメニューでゲームを表示するために必要な、任天堂由来のファイルが一部含まれています(`psp/EBOOT/`:アイコン、背景画像、サウンド)。これらは本プロジェクトのライセンスの対象外で、権利は各権利者にあります。権利者の方で削除をご希望の場合はIssueを作成してください。
