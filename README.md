# ESP32Midi — ESP32-S3 / RP2350 SoundFont GM音源

ESP32-S3 / RP2350 を USB-MIDI 音源にするファームウェアです。既存の SoundFont 2 (.sf2) を
フラッシュに書き込み、波形データを RAM へコピーせずフラッシュから直接再生します。

対応ボード(PlatformIO の環境で切り替え):

| 環境 (`-e`) | ボード | 出力 | フラッシュ / SF2 上限 |
|---|---|---|---|
| `esp32s3box-lite`(既定) | ESP32-S3-BOX-Lite | ES8156 → 内蔵スピーカー(モノラル) | 16MB / 約12.9MB |
| `xiao-pcm5102` | Seeed XIAO ESP32S3 + PCM5102A | ステレオ ライン出力(実機未確認) | 8MB / 約6.4MB |
| `weact-rp2350` | WeAct RP2350A_V20 16MB + WeAct I2S Earphone V1 | ステレオ ヘッドホン出力(64 ボイス) | 16MB / 15MB |
| `weact-rp2350-128` | 同上 | 同上(128 ボイス、起動後に 240MHz へオーバークロック) | 16MB / 15MB |

- 最大同時発音数 128 (`SYNTH_MAX_VOICES`)、32kHz ステレオ
- 2コアでボイスを半分ずつ分担してレンダリング
- SF2: キー/ベロシティ分割、ループ(モード1/3)、音量EG (DAHDSR)、モジュレーションEG、
  ビブラートLFO/モジュレーションLFO(ピッチ・フィルタ・音量)、レゾナンス付きLPF、
  exclusive class、パン
- エフェクト: リバーブ(Freeverb)、ステレオコーラス。センド/リターン型で、送り量は
  CC91 / CC93(GM 既定値 40 / 0)+ SF2 の reverbEffectsSend / chorusEffectsSend
- MIDI: Note On/Off、Program Change、Bank Select、Pitch Bend(RPN 0 でレンジ変更可)、
  CC1/7/10/11/64/91/93/120/121/123、GM/GS/XG リセット SysEx、GM マスターボリューム SysEx。
  10ch はドラム(bank 128)
- 入力: USB-MIDI(本体の USB-C)、UART MIDI(`MIDI_UART_RX_PIN` で有効化)、
  BLE-MIDI(ESP32-S3 の `-ble` 環境。下記)
- 画面(BOX-Lite): 16ch のプリセット名と LED 風レベルメーター、発音数・CPU 負荷バー、音量スライダー
- 出力遅延: I2S DMA バッファ 3 × 128 フレーム(32kHz で 12ms。`AUDIO_DMA_DESC` / `AUDIO_DMA_FRAMES`)
- ボタン(BOX-Lite): PREV = 音量−、NEXT = 音量＋、ENTER 短押し = パニック(音を止める、音色は保持)、ENTER 1秒長押し = GMリセット
- ボタン(XIAO): BOOT 短押し = パニック、1秒長押し = GMリセット。発音中はユーザー LED が点灯

## 使い方

```sh
# 1. SoundFont を置く (SF2 のみ。SF3 は不可、最大 約12.9MB)
cp ~/Downloads/TimGM6mb.sf2 soundfont/default.sf2

# 2. ファームウェアを書き込む (XIAO は -e xiao-pcm5102 を付ける)
pio run -t upload

# 3. SoundFont を sf2 パーティションへ書き込む (SF2 を替えたときだけでよい)
pio run -t uploadsf2
```

書き込み時は 1200bps touch で自動的にダウンロードモードへ入り、終了後はウォッチドッグ
リセットでアプリが起動します。うまくいかない場合は BOOT を押しながら RESET を押して
ダウンロードモードにし、書き込み後に RESET を押してください。

macOS では BOX-Lite は「Espressif ESP32-S3-Box」、XIAO はボード定義の名前の MIDI デバイスとして
認識されます。
USB シリアルに生の MIDI バイト列を送っても演奏できます(テスト用)。

### 大きな SoundFont を縮める

`tools/sf2shrink.py` は SoundFont をフラッシュに収まる SF2 に変換します(要 `brew install sox`)。
SF3 は展開し、サンプルを指定サイズに収まるレートまで下げます(ループ長は整数のまま保つので
音程はずれない)。出力は 32kHz なので、16kHz を超える成分はもともと鳴りません。

```sh
# GeneralUser GS (30.8MB) を RP2350 の 15MB 領域へ。GM とドラム(バンク 0 / 128)だけ残し、
# アコースティックピアノ(プログラム 0〜3)は 32kHz、ほかは収まるレート(約 13.9kHz)にする
python3 tools/sf2shrink.py GeneralUser-GS.sf2 soundfont/default.sf2 \
    --max-size 14.9M --banks 0,128 --keep 0-3 --keep-rate 32000
```

| オプション | 内容 |
|---|---|
| `--max-size` | 出力サイズの上限。収まる最大のレートを 100Hz 刻みで選ぶ |
| `--rate` | サンプルレートの上限を直接指定 |
| `--banks` | 残すバンク |
| `--keep` / `--keep-rate` | 指定したバンク 0 のプログラムのサンプルだけ別の上限にする |
| `--mono` | ステレオのサンプル対をモノラルにまとめる |

FluidR3Mono_GM(SF3 22.6MB)は展開すると約 218MB あり、15MB には収まらなかった。

## XIAO ESP32S3 + PCM5102A の配線

| PCM5102A | XIAO ESP32S3 |
|---|---|
| BCK | D8 (GPIO7) |
| LCK | D9 (GPIO8) |
| DIN | D10 (GPIO9) |
| SCK | GND(DAC 内部の PLL を使う) |
| XSMT | 3.3V(ミュート解除) |
| FMT / FLT / DEMP | GND |
| VIN / GND | 5V / GND |

WeAct などのモジュールは SCK / XSMT / FMT 等が基板上のジャンパーやパッドで設定済みの場合が
あるので、モジュールの回路図を確認してください。出力はライン出力なので、ヘッドホンか
アンプ付きスピーカーにつないでください。UART MIDI を使う場合は `MIDI_UART_RX_PIN=44`(D7)。

PCM5102A には音量調整がないので、音量は DAW などから GM マスターボリュームの SysEx
(`F0 7F 7F 04 01 ll mm F7`)か CC7 で調整します。

## WeAct RP2350A_V20 + I2S Earphone V1

PlatformIO Core 6.2.0 以上が必要です(PlatformIO IDE 同梱の `~/.platformio/penv/bin/pio` など)。

| Earphone V1 (P1) | RP2350A_V20 |
|---|---|
| 1 VCC | VSYS(39 番ピン)。モジュール上に 3.3V LDO があるので 3V3 には繋がない |
| 2 GND | GND |
| 3 /SD | GP21(27 番ピン)。起動時はミュート、I2S 開始後に解除 |
| 4 MCLK | 未接続(モジュール上で 100kΩ プルダウン → DAC は BCK から PLL) |
| 5 BCK | GP18(24 番ピン) |
| 6 DIN | GP20(26 番ピン) |
| 7 WS | GP19(25 番ピン)。PIO I2S の制約で BCK + 1 に固定 |

- MIDI 入力は USB-MIDI と UART MIDI(GP1 = UART0 RX、31250bps。DIN 端子からはフォトカプラ経由)
- USB スタックは Adafruit TinyUSB(`-DUSE_TINYUSB`)。コア標準のスタックに MIDIUSB を足すと、
  CDC も MIDI もデータが流れなくなった
- 書き込みは `pio run -e weact-rp2350 -t upload`(1200bps touch で自動的に BOOTSEL)。
  起動しないファームウェアを書いた場合は BOOT を押しながら USB を挿す
- I2S は 32bit スロット(BCK = 64fs)で出力
- SoundFont はフラッシュ末尾の filesystem 領域(0x100FF000〜、15MB)に生のまま置き、XIP で直接読む。
  `pio run -e weact-rp2350 -t uploadsf2` は 1200bps touch で BOOTSEL に入れて picotool で書き込む
  (うまくいかない場合は BOOT を押しながら RESET)
- PSRAM がないので、リージョン表は SRAM に置く。全プリセットを起動時に展開すると GeneralUser GS で
  約 420KB になって入らないため、チャンネルが選んだプリセットだけをプログラムチェンジ時に展開し、
  使われていないものはヒープの空きが `SF2_HEAP_RESERVE`(32KB)を切る前に古い順に解放する
- コア 1 がオーディオループ、コア 0 が `loop()` の合間にボイスの後半とリバーブを処理する。
  コア 0 が間に合わないときはコア 1 が肩代わりするので、音は止まらず処理できるボイスが減るだけ
- 描画ループ・エフェクトは SRAM に配置(`AUDIO_HOT`)。フラッシュのキャッシュは波形の読み出しに使う
- ボタンなし。音量は GM マスターボリュームの SysEx か CC7。LED(GP25)は発音中に点灯
- 初期音量は BOX-Lite の -18dB(`DEFAULT_MASTER_GAIN` 0.045)。TimGM6mb では -12dB(0.09)まで
  大丈夫だったが、音の大きい GeneralUser GS ではイヤホンモジュールのアンプ(HT97226)で歪んだ
- 低いレートのサンプル(13.9kHz に縮めた GeneralUser GS など)は、線形補間で帯域の上に折り返しの像が
  残ってザラつくので、ボイスのローパスフィルタをサンプルの帯域端(サンプルレート × ピッチ比 × 0.45)
  以下に抑える。GeneralUser GS(ピアノ以外 13.9kHz)のオーケストラデモでは CPU 1 秒平均が最大 93%、
  1 ブロックの最大 117%(処理落ち 66 ブロック、アンダーランなし)
- 出力バッファは 8 × 64 フレーム(16ms)。3 × 128 では数秒ごとにアンダーランした
- `weact-rp2350-128` は 150MHz で起動してから、コア電圧 1.20V・フラッシュ分周 /4 にして 240MHz に上げる
  (`SYNTH_CPU_MHZ`)。F_CPU=240MHz で起動すると立ち上がらなかった
- 実測(TimGM6mb、`tools/demo/orchestra.mid`、リバーブ+コーラスあり、処理落ち・アンダーランなし):

  | 環境 | 同時発音 | CPU 負荷(1 秒平均) | 1 ブロックの最大 |
  |---|---|---|---|
  | `weact-rp2350`(150MHz) | 64 | 平均 63%、最大 72% | — |
  | `weact-rp2350-128`(240MHz) | 128 | 平均 69%、最大 77% | 87% |

  `weact-rp2350-128` ではほかに、ベートーヴェン交響曲第5番第1楽章(5分16秒、最大 70 音、CPU 最大 49%)と
  ショパン バラード第1番(11分34秒、最大 38 音、CPU 最大 29%)も処理落ちなしで再生できた

- 同時発音数の上限で音を奪うときは、予備の 16 ボイスで 5ms フェードアウトさせる(即座に切ると
  全ボイスが埋まる場面でプツプツ鳴った)。シリアルログの `block max` は 1 秒間で最も重かったブロックの負荷
- macOS での MIDI 出力先名は "Pico 2"。`midiplay` は既定で "ESP32" を含む出力先を選ぶので
  `midiplay -d "Pico 2" song.mid` のように指定する
- シリアルモニタを開くと起動ログと状態(`status: blocks …`)を表示する。
  診断用に `-DDIAG_LED` を付けると、青 LED でメインループの停止位置を表示する

## BLE-MIDI (ESP32-S3)

`esp32s3box-lite-ble` / `xiao-pcm5102-ble` 環境は USB-MIDI / UART に加えて BLE-MIDI
ペリフェラル「ESP32 MIDI Box」として受信します(ファームウェアは約 300KB 増)。

```sh
pio run -e esp32s3box-lite-ble -t upload
```

macOS では「Audio MIDI 設定」→ ウインドウ → MIDI スタジオを表示 → Bluetooth を構成 で
「ESP32 MIDI Box」に接続します。出力先名は「ESP32 MIDI Box Bluetooth」になるので
`midiplay -d Bluetooth song.mid` で再生できます。BOX-Lite の画面右下の「BLE」は接続中に青、
待ち受け中に灰色になります。BLE の接続間隔(7.5〜15ms)ぶん USB より遅延と揺れが増えます。
BOX-Lite でオーケストラのデモ(最大 133 音)を BLE で流して、音切れや遅れは感じられませんでした。

## MIDI ファイルの再生 (macOS)

GarageBand は外部 MIDI 出力を持たないので、曲は「ファイル → 書き出す → MIDI ファイル」で
.mid に書き出し、`midiplay` で音源へ送ります。

```sh
tools/midiplay/build.sh                 # 初回のみビルド
tools/midiplay/midiplay song.mid        # 名前に "ESP32" を含む MIDI 出力先へ再生
tools/midiplay/midiplay -l              # 出力先一覧
tools/midiplay/midiplay -d "IAC" x.mid  # 出力先を指定
```

再生前に GM System On を送り、Ctrl-C で止めると全チャンネルの発音を止めます。

### デモ曲

`tools/demo/make_orchestra.py` は 16ch 全部を使うオーケストラ風のデモ曲(約87秒)を生成します。
TimGM6mb ではほぼ全編で 120〜128 音を使い、CPU 負荷は最大約80%でした。

```sh
python3 tools/demo/make_orchestra.py /tmp/orchestra.mid
tools/midiplay/midiplay /tmp/orchestra.mid
```

## デバッグ

`pio run -e debug -t upload` は USB を内蔵 USB-Serial-JTAG にしたビルドです(USB-MIDI なし)。
クラッシュ時のバックトレースがシリアルに出ます。発音中は 1 秒ごとに
`voices / cpu / xrun` がログ出力されます。

## フラッシュ構成

BOX-Lite(16MB, `partitions.csv`):

| パーティション | オフセット | サイズ |
|---|---|---|
| nvs | 0x9000 | 24KB |
| factory (アプリ) | 0x10000 | 3MB |
| sf2 | 0x310000 | 約12.9MB |

XIAO(8MB, `partitions_8mb.csv`):

| パーティション | オフセット | サイズ |
|---|---|---|
| nvs | 0x9000 | 24KB |
| factory (アプリ) | 0x10000 | 1.5MB |
| sf2 | 0x190000 | 約6.4MB(TimGM6mb は入る) |

## 同時発音数 128 について

ボイスあたりの処理はブロック単位(64サンプル)の制御計算と、線形補間+ゲインランプの
軽い内部ループにしています。リバーブは 1 ブロック遅れでコア 0、コーラスはコア 1 で処理します。
実測(TimGM6mb、15ch × 弦/パッドの和音、約120 音同時、リバーブ+コーラスあり)で
CPU 負荷 約66〜71%、処理落ちなしでした(エフェクトなしの 128 音では約62%)。
BOX-Lite で `tools/demo/orchestra.mid`(常に 128 音)を再生したときは平均 73%、最大 80%
(1 ブロックの最大 89%)、処理落ち・アンダーランなしでした。
同時発音数の上限に達して音を奪うときは、予備の 16 ボイスで 5ms フェードアウトさせます。
異なるサンプルが大量に同時に鳴るほどフラッシュ読み出しのキャッシュミスで負荷が上がります。

画面の `CPU` が 100% に近づくか、シリアルログの `xrun`(DMA アンダーラン = 音切れ)が増える場合は、
`platformio.ini` で調整してください。

- `-DSYNTH_SAMPLE_RATE=22050` (負荷が約30%減)
- `-DSYNTH_MAX_VOICES=96`

## 構成

| ファイル | 内容 |
|---|---|
| `src/sf2.*` | SF2 パーサ(プリセット×インストゥルメントのゾーンを、使うプリセットの分だけリージョンに展開して RAM / PSRAM に置く) |
| `src/synth.*` | ボイス管理、エンベロープ/LFO/フィルタ、MIDI処理、2コアのレンダリング |
| `src/effects.*` | リバーブ / コーラス |
| `src/board.h` | ボードごとのピン定義と機能の有無 |
| `src/platform.h` | プラットフォーム層(メモリ確保、SF2 のマップ、2コアのスケジューリング) |
| `src/audio_out.h` | I2S 出力のインターフェイス |
| `src/platform/esp32/` | ESP32-S3 実装(FreeRTOS タスク、パーティション mmap、I2S + ES8156 / PCM5102A) |
| `src/platform/rp2350/` | RP2350 実装(コア 1 = オーディオ、XIP 上の SF2、PIO I2S + PCM5100A) |
| `src/midi_in.*` | USB-MIDI / UART MIDI / BLE-MIDI の受信とパース |
| `src/display.*` | LovyanGFX による状態表示(画面のないボードでは空の実装) |
| `scripts/upload_sf2.py` | `uploadsf2` ターゲット |
| `tools/sf2shrink.py` | SoundFont をフラッシュに収まるよう縮める変換スクリプト |

## ライセンス

このリポジトリのソースコードは [MIT License](LICENSE) です。

外部のコンポーネントはそれぞれのライセンスに従います。

| コンポーネント | ライセンス | 備考 |
|---|---|---|
| Arduino-ESP32 | LGPL-2.1 | ビルド時に取得。ファームウェアのバイナリを配布する場合は LGPL の条件に注意 |
| arduino-pico | LGPL-2.1 | ビルド時に取得(RP2350) |
| LovyanGFX | FreeBSD | ビルド時に取得 |
| TinyUSB | MIT | Arduino-ESP32 に同梱 |
| Freeverb(`src/effects.cpp` のリバーブ) | パブリックドメイン | Jezar at Dreampoint |
| ES8156 初期化シーケンス | — | Espressif esp_codec_dev(Apache-2.0)のレジスタ設定を参照 |
| SoundFont | 各 SoundFont のライセンス | リポジトリには含まない。例: TimGM6mb は GPLv2、GeneralUser GS は独自ライセンス(改変・同梱可)、FluidR3 は MIT。書き込み済みの機器を配布する場合は SoundFont のライセンスに従うこと |

USB の VID/PID は Espressif のもの(303A:1001)を使っています。製品として配布する場合は独自の PID を取得してください。
