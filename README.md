# ESP32Midi — ESP32-S3-BOX-Lite SoundFont GM音源

ESP32-S3-BOX-Lite を USB-MIDI 音源にするファームウェアです。既存の SoundFont 2 (.sf2) を
フラッシュに書き込み、波形データを RAM へコピーせずフラッシュから直接再生します。

- 最大同時発音数 128 (`SYNTH_MAX_VOICES`)、32kHz ステレオ
- 2コアでボイスを半分ずつ分担してレンダリング
- SF2: キー/ベロシティ分割、ループ(モード1/3)、音量EG (DAHDSR)、モジュレーションEG、
  ビブラートLFO/モジュレーションLFO(ピッチ・フィルタ・音量)、レゾナンス付きLPF、
  exclusive class、パン
- エフェクト: リバーブ(Freeverb)、ステレオコーラス。センド/リターン型で、送り量は
  CC91 / CC93(GM 既定値 40 / 0)+ SF2 の reverbEffectsSend / chorusEffectsSend
- MIDI: Note On/Off、Program Change、Bank Select、Pitch Bend(RPN 0 でレンジ変更可)、
  CC1/7/10/11/64/91/93/120/121/123、GM/GS/XG リセット SysEx。10ch はドラム(bank 128)
- 入力: USB-MIDI(本体の USB-C)、UART MIDI(`MIDI_UART_RX_PIN` で有効化)
- 画面: 16ch のプリセット名と LED 風レベルメーター、発音数・CPU 負荷バー、音量スライダー
- 出力遅延: I2S DMA バッファ 3 × 128 フレーム(32kHz で 12ms。`AUDIO_DMA_DESC` / `AUDIO_DMA_FRAMES`)
- ボタン: PREV = 音量−、NEXT = 音量＋、ENTER = GMリセット

## 使い方

```sh
# 1. SoundFont を置く (SF2 のみ。SF3 は不可、最大 約12.9MB)
cp ~/Downloads/TimGM6mb.sf2 soundfont/default.sf2

# 2. ファームウェアを書き込む
pio run -t upload

# 3. SoundFont を sf2 パーティションへ書き込む (SF2 を替えたときだけでよい)
pio run -t uploadsf2
```

書き込み時は 1200bps touch で自動的にダウンロードモードへ入り、終了後はウォッチドッグ
リセットでアプリが起動します。うまくいかない場合は BOOT を押しながら RESET を押して
ダウンロードモードにし、書き込み後に RESET を押してください。

macOS では「Espressif ESP32-S3-Box」という MIDI デバイスとして認識されます。
USB シリアルに生の MIDI バイト列を送っても演奏できます(テスト用)。

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

## フラッシュ構成 (16MB)

| パーティション | オフセット | サイズ |
|---|---|---|
| nvs | 0x9000 | 24KB |
| factory (アプリ) | 0x10000 | 3MB |
| sf2 | 0x310000 | 約12.9MB |

## 同時発音数 128 について

ボイスあたりの処理はブロック単位(64サンプル)の制御計算と、線形補間+ゲインランプの
軽い内部ループにしています。リバーブは 1 ブロック遅れでコア 0、コーラスはコア 1 で処理します。
実測(TimGM6mb、15ch × 弦/パッドの和音、約120 音同時、リバーブ+コーラスあり)で
CPU 負荷 約66〜71%、処理落ちなしでした(エフェクトなしの 128 音では約62%)。
異なるサンプルが大量に同時に鳴るほどフラッシュ読み出しのキャッシュミスで負荷が上がります。

画面の `CPU` が 100% に近づくか、シリアルログの `xrun`(DMA アンダーラン = 音切れ)が増える場合は、
`platformio.ini` で調整してください。

- `-DSYNTH_SAMPLE_RATE=22050` (負荷が約30%減)
- `-DSYNTH_MAX_VOICES=96`

## 構成

| ファイル | 内容 |
|---|---|
| `src/sf2.*` | SF2 パーサ(プリセット×インストゥルメントのゾーンをリージョンに展開して PSRAM に置く) |
| `src/synth.*` | ボイス管理、エンベロープ/LFO/フィルタ、MIDI処理、2コアのレンダリングタスク |
| `src/effects.*` | リバーブ / コーラス |
| `src/audio_out.*` | I2S と ES8156 DAC の初期化 |
| `src/midi_in.*` | USB-MIDI / UART MIDI の受信とパース |
| `src/display.*` | LovyanGFX による状態表示 |
| `scripts/upload_sf2.py` | `uploadsf2` ターゲット |

## ライセンス

このリポジトリのソースコードは [MIT License](LICENSE) です。

外部のコンポーネントはそれぞれのライセンスに従います。

| コンポーネント | ライセンス | 備考 |
|---|---|---|
| Arduino-ESP32 | LGPL-2.1 | ビルド時に取得。ファームウェアのバイナリを配布する場合は LGPL の条件に注意 |
| LovyanGFX | FreeBSD | ビルド時に取得 |
| TinyUSB | MIT | Arduino-ESP32 に同梱 |
| Freeverb(`src/effects.cpp` のリバーブ) | パブリックドメイン | Jezar at Dreampoint |
| ES8156 初期化シーケンス | — | Espressif esp_codec_dev(Apache-2.0)のレジスタ設定を参照 |
| SoundFont | 各 SoundFont のライセンス | リポジトリには含まない。例: TimGM6mb は GPLv2。書き込み済みの機器を配布する場合は SoundFont のライセンスに従うこと |

USB の VID/PID は Espressif のもの(303A:1001)を使っています。製品として配布する場合は独自の PID を取得してください。
