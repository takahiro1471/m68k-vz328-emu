# MC68VZ328 UART/TIMER エミュレータ

MC68VZ328 (DragonBall) の **UART1/UART2**、**タイマ1/タイマ2**、割り込みコントローラを
エミュレートする、実験・学習向けの小さなボードエミュレータです。
CPU コアには Musashi 4.60 の 68000 コアを同梱しています。

情報実験で使う `m68k-as` / `m68k-term` のワークフロー
(`.s` → `m68k-as -t 400` → `.abs` → 転送・実行) を、実機なしで再現できることを
目標にしています。

## 特徴

- MC68000 命令セット (Musashi、68000 のみ)
- 24 ビット (16 MiB) のアドレス空間
- UART1 / UART2
  - 8 バイトの送受信 FIFO
  - UBAUD レジスタからのボーレート計算 (16 × 2^DIVIDE × (65 - 下位6ビット))
  - RXRE / TXEE などのレベル割り込み
  - URX 読み出しで受信 FIFO をポップ
- タイマ1 / タイマ2
  - SYSCLK、SYSCLK/16、CLK32 のクロックソース
  - TPRER プリスケーラ、リスタート/フリーランニング、コンペア割り込み
  - TSTAT の書き込み 0 によるクリア
- 割り込みコントローラ (IVR/IMR/ISR/IPR/ILCR)
- S レコード (`.abs`) の直接ロードと、`m68k-term` 互換の IPL ローダ
- PTY / 既存シリアルデバイス / stdin・stdout の 3 種類のホスト接続
- LED、トグルスイッチ、押しボタンの簡易エミュレーション
- PTY を使った自動テスト (`make test`)

## ビルド

必要なもの:

- C11 コンパイラ、make
- Linux (PTY と `termios` を使用)
- テストを動かす場合は `m68k-as` と Python 3
- `tools/m68k-term-emu` を使う場合は `kermit`

```sh
make
```

`m68k-vz328` がカレントディレクトリに生成されます。
Musashi の命令デコーダ (`build/m68kops.c`) はビルド時に自動生成されます。

```sh
make test     # 自動テスト
make clean
```

## すぐに試す

### 直接ロードして実行

```sh
cd tests
m68k-as -t 400 uart_poll.s
cd ..

./m68k-vz328 --load tests/uart_poll.abs
```

UART1 に次のように出力して終了します (終了コード 0)。

```
Hello from UART polling!
```

`--load` を指定した場合、UART1 の既定の接続先は stdin/stdout です。
`tests/uart_irq.abs` を実行すると、入力した文字がエコーされます。`q` で終了します。

```sh
./m68k-vz328 --load tests/uart_irq.abs
Type characters (q to quit): abcq
abcq
UART 1 interrupt OK!
```

### m68k-term / kermit で転送する

同梱のラッパーを使うと、実機の `m68k-termUSB0` と同じ操作で IPL から
プログラムを転送できます。

```sh
tools/m68k-term-emu
```

kermit の画面で次の操作を行います。

1. `C-\ c` (Ctrl を押しながら `\`、続けて `c`) でコマンドモードへ
2. `transmit tests/uart_poll.abs` で S レコードを送信
3. `c` で端末モードへ戻る
4. `if you want to start your program, hit any key` と表示されたら何かキーを押す

終了するときは `C-\ c` でコマンドモードへ戻り、`q` を入力します。
ラッパーは UART1 用の PTY を作り、インストール済みの kermit 設定
(`/usr/local/bin/m68k-termUSB0`) の `set line` と `set speed` を書き換えて起動します。

直接ロードしたプログラムを kermit から操作したい場合は次のようにします。

```sh
tools/m68k-term-emu --load tests/uart_poll.abs
```

この場合は `--wait-key` が自動的に付加され、kermit 接続後にキーを押すと実行が始まります。

ラッパーを使わずに PTY を自分で接続する場合:

```sh
./m68k-vz328 --pty-link /tmp/vz328-tty
# 別の端末で kermit を起動し、set line /tmp/vz328-tty などで接続
```

## IPL ローダ

`--ipl` を指定するか、`--load` を指定せずに起動すると、実機の IPL に似た
S レコードローダが動きます。

- 起動メッセージを UART1 に出力し、S レコードを待つ
- S0/S1/S2/S3/S5/S6/S7/S8/S9 を解釈
- 1 レコードごとに `.` を出力
- S7/S8/S9 の開始アドレスを使用 (なければ `0x00000400`)
- `*** Load Success ***` とロード範囲・開始アドレスを表示
- 1 文字のキー入力を待ってから、SSP `0x0003FFF0`、開始アドレスから実行

S レコードは行単位で受け取ります。Kermitの`transmit`は設定によって
行末をCRまたはLFに変換しますが、どちらも読み込めます。

## コマンドラインオプション

```
-l, --load FILE        .abs (S レコード) を読み込んで実行
-i, --ipl              IPL モードで起動 (UART1 から S レコードを待つ)
    --wait-key         --load 実行前にキー入力を待つ

-p, --pty              UART1 に PTY を作る
    --pty-link PATH    UART1 の PTY を作り、PATH に symlink する
-s, --serial DEVICE    UART1 に既存のシリアルデバイスを使う
    --stdio            UART1 に stdin/stdout を使う

    --uart2 DEVICE     UART2 に既存のシリアルデバイスを使う
    --uart2-pty        UART2 に PTY を作る
    --uart2-link PATH  UART2 の PTY を作り、PATH に symlink する

    --buttons MASK     押しボタン (active low、既定 0x0F)
    --switches MASK    トグルスイッチ (既定 0xFF)
    --led              LED の内容を stderr に表示

    --fast             実時間スロットルなしで最速実行
    --max-cycles N     N サイクル後に停止 (K/M/G 接尾辞可)
    --run-time SEC     SEC 秒 (エミュレート時間) 後に停止
    --dump             終了時に CPU レジスタを表示
    --trace-io         内部レジスタとボード I/O のアクセスを表示
    --trace-irq        割り込みコントローラの動作を表示
-h, --help
-V, --version
```

既定の UART1 接続は、`--load` ありなら stdio、IPL モードなら PTY です。
UART2 は既定で未接続です。

## メモリマップ

| 範囲 | 内容 |
| --- | --- |
| `0x000000` - `0x03FFFF` | RAM (256 KiB) |
| `0xC00000` - `0xC7FFFF` | ROM (例外スタブのみ) |
| `0xD00000` - `0xD1FFFF` | ボード I/O (LED、スイッチ) |
| `0xFFF000` - `0xFFFFFF` | MC68VZ328 内蔵レジスタ |

実験プログラムは `m68k-as -t 400` で `0x00000400` から配置されます。
IPL の既定スタックは `0x0003FFF0` です。

### ボード I/O

| アドレス | 内容 |
| --- | --- |
| `0xD0002F` / `0xD0002D` / `0xD0002B` / `0xD00029` | LED7 / LED6 / LED5 / LED4 |
| `0xD0003F` / `0xD0003D` / `0xD0003B` / `0xD00039` | LED3 / LED2 / LED1 / LED0 |
| `0xD00041` | トグルスイッチ (読み出し) |
| `0xFFF419` | 押しボタン (下位 4 ビット、active low) |

## 実装しているレジスタ

### UART1 / UART2

| レジスタ | UART1 | UART2 |
| --- | --- | --- |
| USTCNT | `0xFFF900` | `0xFFF910` |
| UBAUD | `0xFFF902` | `0xFFF912` |
| URX | `0xFFF904` | `0xFFF914` |
| UTX | `0xFFF906` | `0xFFF916` |
| UMISC | `0xFFF908` | `0xFFF918` |
| NIPR | `0xFFF90A` | `0xFFF91A` |

- 送受信 FIFO は各 8 バイトです。
- URX をワードで読むと受信データ 1 バイトをポップします。
- UTX の下位バイト書き込みで送信 FIFO に積みます。bit12 (SEND BREAK) と
  bit11 (NOCTS1) は制御ビットとして保持します。
- USTCNT の RXRE/TXEE/TXHE/TXAE による割り込みはレベルトリガです。
- CTS は常にアサートされているものとして扱い、フロー制御は行いません。

### タイマ1 / タイマ2

| レジスタ | タイマ1 | タイマ2 |
| --- | --- | --- |
| TCTL | `0xFFF600` | `0xFFF610` |
| TPRER | `0xFFF602` | `0xFFF612` |
| TCMP | `0xFFF604` | `0xFFF614` |
| TCR | `0xFFF606` | `0xFFF616` |
| TCN | `0xFFF608` | `0xFFF618` |
| TSTAT | `0xFFF60A` | `0xFFF61A` |

- カウンタ周波数はクロックソース / (TPRER + 1) です。
- `TCTL.FRR=0` でコンペア時にカウンタを 0 に戻します。`FRR=1` では
  フリーランニングします。
- コンペアで `TSTAT.COMP` をセットし、`TCTL.IRQEN=1` なら割り込みを要求します。
- TSTAT は書き込み 0 でクリアします (`tstat &= value`)。
- TIN 外部クロックとキャプチャ機能は未実装です。

### 割り込みコントローラ

| レジスタ | アドレス |
| --- | --- |
| IVR | `0xFFF300` |
| IMR | `0xFFF304` |
| ISR | `0xFFF30C` |
| IPR | `0xFFF310` |
| ILCR | `0xFFF314` |

- ベクタ番号は `(IVR & 0xF8) | 割り込みレベル` です。
  IVR が 0 の場合は未初期化ベクタ `0x0F` を返します。
- IMR は active low です (0 のビットが許可)。
- 割り込み要因ビット: Timer1 = bit1、UART1 = bit2、Timer2 = bit5、UART2 = bit12。
- 固定レベル: UART1 = 4、Timer1 = 6。
- UART2 と Timer2 のレベルは ILCR で設定します。リセット値 `0x6533` では
  UART2 = 5、Timer2 = 3 です。
- 実験で使う設定 (`IVR=0x40`) では、UART1 はベクタ `0x44`
  (アドレス `0x110`)、Timer1 はベクタ `0x46` (アドレス `0x118`) になります。

## 終了用マジックアドレス

テストプログラムや実験プログラムからエミュレータを終了させるために、
次のアドレスへの書き込みを監視しています。

| アドレス | 値 | 動作 |
| --- | --- | --- |
| `0xFFF7FE` | `0x600D` | 正常終了 (終了コード 0) |
| `0xFFF7FE` | `0xDEAD` | 異常終了 (終了コード 2) |

既定の例外ベクタは `0xC00000` の小さな ROM スタブを指しており、
未処理の例外が発生すると `0xDEAD` を書き込んで `stop #0x2700` します。
そのため、ベクタを設定し忘れたプログラムでも「どの PC で例外が起きたか」を
stderr で確認できます。

## テスト

```sh
make test
```

実行されるテスト:

- `uart_poll`: UART1 ポーリング送信
- `uart_irq`: UART1 受信割り込みとエコー
- `uart_irq_min`: URX 読み出しだけで RX 割り込みが解除されること
- `uart2_poll`: UART2 送信 (PTY)
- `timer_irq`: Timer1 コンペア割り込み
- `timer2_irq`: Timer2 コンペア割り込み
- `exception`: 未処理例外が `0xDEAD` で終了すること
- `ipl_stdio`: stdin からの S レコードロードと実行
- `pty_uart1` / `pty_uart2` / `pty_ipl`: PTY 経由の対話テスト

PTY テストは Python 3 の `pty` 相当の機能 (標準ライブラリのみ) を使います。

## ソース構成

```
src/main.c       コマンドライン、実行ループ、入出力のポーリング
src/vz328.c      メモリマップ、内蔵レジスタ、割り込み、IPL
src/uart.c       UART1/UART2
src/timer.c      タイマ1/タイマ2
src/host_port.c  PTY / シリアル / stdio
src/srec.c       S レコードローダ
src/cpu.c        Musashi の接続
third_party/musashi/  Musashi 4.60
tests/           アセンブリテストと自動テスト
tools/m68k-term-emu  kermit ラッパー
```

## 制限事項

- サイクル精度のエミュレータではありません。UART とタイマはイベント駆動で、
  CPU 時間 (33.161216 MHz) を基準に動作します。
- 実装していない内蔵周辺機能 (DMA、SPI、LCD、PWM、RTC、DRAM コントローラなど)
  は、読み出し 0・書き込み無視です。
- 外部 TIN クロック、タイマのキャプチャ、UART の CTS/RTS フロー制御、
  IrDA、9 ビットモードは未実装です。
- 実機のブート ROM は載せていません。IPL はエミュレータ内で S レコードを
  直接処理します。
- LED とスイッチは簡易実装です。

## ライセンス

同梱の Musashi 4.60 は Karl Stenerud 氏による MIT ライセンスのソフトウェアです。
各ソースファイルの先頭にライセンス条項があります。
