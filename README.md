# ArduinoController

## 概要

Arduino Unoの設定済みGPIOをUSBシリアル経由で制御する独立したC++17ライブラリです。
GPIOのデジタル出力、PWM、サーボ角度出力を提供します。ポンプやベルトなど装置固有の動作は
ライブラリに含めず、出力ピンを通じて外部ドライバへ信号を出します。

## 現在の範囲

- Linux USBシリアルポートの接続・切断
- 名前付き出力の設定、値の範囲検証、全出力の安全停止
- 2秒（設定可能）の通信監視と、通信断時のArduino側LOW出力
- Arduino Uno用ファームウェアスケッチ
- 1行シリアル通信のタイムアウトと応答検証
- CMake/CTestによる疑似端末テスト（実機には接続しません）

このライブラリはベルトやポンプを直接駆動せず、XYZ軸も制御しません。
[MarlinController](../Marlin/MarlinController/README.md)など別の機器ライブラリと組み合わせる場合は、
上位アプリケーションが動作開始・完了待ち・キャンセルの順序を管理します。
同期XYZ動作の間Arduino出力を保持する薄い補助ライブラリは
[MachineCoordinator](../MachineCoordinator/README.md)にあります。

## ビルドとテスト

```sh
make build
make test
```

Arduino Uno用ファームウェアもビルドする場合は、Arduino CLIと`arduino:avr`コアを
インストールし、スケッチが使うServoライブラリを追加したうえで次を実行します。

```sh
arduino-cli core install arduino:avr
arduino-cli lib install Servo
make firmware
```

`ARDUINO_CLI`と`ARDUINO_FQBN`を指定すると、CLIの場所と対象ボードを変更できます。
通常の`make test`はPC側C++ライブラリの疑似端末テストで、ファームウェアのビルドや
Arduino実機への書き込みは含みません。

### Arduino Unoへ書き込む

Arduino Unoをデータ通信対応USBケーブルでPCへ接続し、シリアルポートを確認します。
Linuxでは`arduino-cli board list`で候補を確認できます（例: `/dev/ttyACM0`または
`/dev/ttyUSB0`）。ポート名は環境によって異なります。

Uno R4 Minimaでは、Uno R3用の`arduino:avr:uno`ではなく、Renesas用ボードコアを
インストールしてR4用のFQBNを指定します。

```sh
arduino-cli core update-index
arduino-cli core install arduino:renesas_uno
arduino-cli board listall | grep -i 'UNO R4 Minima'
arduino-cli board list
make upload ARDUINO_PORT=/dev/ttyACM0 ARDUINO_FQBN=arduino:renesas_uno:minima
```

Arduino CLIを`~/.local/bin`へインストールした環境では、Makeへ実行ファイルの場所を渡します。

```sh
ARDUINO_CLI="$HOME/.local/bin/arduino-cli" make upload \
  ARDUINO_PORT=/dev/ttyACM0 ARDUINO_FQBN=arduino:renesas_uno:minima
```

`make upload`は`arduino-cli compile --upload`を実行して、Arduino Uno向けスケッチを
コンパイル後、指定ポートのボードへ書き込みます。`Invalid FQBN: board ... not found`
と表示された場合、指定FQBNに対応するボードコアが未インストールの可能性があります。
対象コアをインストールし、`board listall`または`board list`で表示されたFQBNを指定して
ください。UNO R4 MinimaのFQBNは`arduino:renesas_uno:minima`です。別のボードを使う場合は
その機種に合うFQBNを指定します。書き込み前に対象ポートとボードを確認し、
シリアルモニターなど他のソフトが同じポートを使用していないことを確かめてください。
これはArduino本体へのファームウェア書き込みです。外部ドライバーやポンプの配線を
確認するまでは、負荷を接続せずに行ってください。

## 設定

[`arduino-controller.conf.example`](arduino-controller.conf.example)をコピーし、Arduinoのシリアル
ポートと許可する出力を設定します。
`OUTPUT`の各ピン/モードは、スケッチ内の`kFirmwareAllowlist`にも同じ組み合わせを登録してください。

```text
PORT /dev/ttyACM0
BAUD 115200
WATCHDOG_MS 2000

# OUTPUT <name> <DIGITAL|PWM|SERVO> <Arduino Uno pin>
OUTPUT pump_driver DIGITAL 8
```

出力名はアプリケーションから指定する論理名です。同じピンの重複設定やUnoで使えないモード/
ピンの組み合わせは拒否します。D0/D1はUSBシリアルと競合するため出力に使えません。
アナログピンは数字14～19（A0～A5）で指定できます。

## C++ API

```cpp
#include "GpioController.hpp"

#include <fstream>
#include <utility>

std::ifstream configFile("arduino-controller.conf");
auto config = arduino::GpioConfiguration::load(configFile, "arduino-controller.conf");
arduino::GpioController controller(std::move(config));

controller.connect();
controller.setOutput("indicator", 1);  // DIGITAL: 0 or 1
controller.setOutput("pump_driver", 1); // DIGITAL: HIGH (external driver only)
controller.stopAll(); // all configured outputs LOW
controller.disconnect();
```

`SERVO`出力の値は0～180度です。PWMはUnoのD3/D5/D6/D9/D10/D11のみ対応します。
UnoのServoライブラリはタイマーを共有するため、サーボ使用時はPWM D9/D10との併用を拒否します。

## Arduinoファームウェア

Arduino IDEまたはArduino CLIで[`firmware/ArduinoController/ArduinoController.ino`](firmware/ArduinoController/ArduinoController.ino)
をArduino Unoへ書き込みます。スケッチとホストライブラリのプロトコルを一致させてください。
USBシリアルを開くとUnoがリセットすることがあるため、接続処理はファームウェアの応答を確認します。

スケッチ内の`kFirmwareAllowlist`にも、使用するピンとモードを登録してください。
設定ファイルとスケッチ双方で許可された組み合わせだけを操作できます。ファームウェアは起動時に
その許可ピンをLOWへ初期化し、ホストが接続して許可したピンだけを操作します。
スケッチの初期許可リストはD8/DIGITAL、D3/PWM、D10/SERVOです。配線と設定に合わせて編集し、
対応する`OUTPUT`行と一致させてください。
PCからのハートビートが`WATCHDOG_MS`の間届かない場合、許可済みの全出力をLOWに戻し、
新しい接続の初期化が行われるまで出力命令を拒否します。`stopAll()`も即時LOWを要求し、
Arduinoの応答を確認します。通信が切れた際のLOW動作はファームウェアで行われます。

通信命令は`HELLO`/`READY`、`WD`、`ALLOW`、`SET`、`STOP`、`HB`とし、各操作は
`OK ...`または`ERR ...`の1行応答を確認します。Arduinoを開くとリセットする場合に備え、
ホストは最大8秒間`READY`を再試行します。

## 外部機器の安全

Arduino GPIOは信号出力用です。ポンプ、モーター、ソレノイド、リレーコイル等をGPIOから直接
駆動しないでください。電圧・電流・誘導負荷に適合したドライバ、保護部品、電源を使用し、
Arduinoと外部電源のGND要件も回路設計で確認してください。外付けLEDには適切な電流制限抵抗を
直列に接続します。LOWが負荷停止を意味するかはドライバ回路によるため、実機の安全状態を
確認してください。

## 複数機器との連携

ArduinoControllerはArduino GPIOだけを担当します。ベルト、XYZ軸、センサー等の処理を並行開始、
完了待ち、または割り込みする機能は含みません。これらは各機器ライブラリを組み合わせる
上位アプリケーションの責務です。MarlinControllerの現行移動APIは同期式で、移動中の安全な
ソフトウェア割り込みを保証しません。非常停止には機器側の独立した安全回路を用意してください。

## ライブラリとして利用する

```cmake
add_subdirectory(/path/to/ArduinoController arduino-controller-build)
target_link_libraries(my_app PRIVATE ArduinoController::arduino_controller)
```

## 接続条件

- Linux USBシリアル
- Arduino Uno GPIOファームウェアのボーレート: 115200
- 低レベル`SerialConnection`の対応ボーレート: 9600、19200、38400、57600、115200、230400
- 8N1、ハードウェアフロー制御なし
- 同じコントローラーへの操作は内部で直列化。複数の出力名を別スレッドから操作できます
- `connect()`/`disconnect()`と出力操作を同時に呼び出す場合は、呼び出し側で直列化してください
- 通信断、応答異常、未許可出力は例外として通知します

通信/テスト用の低レベル`SerialConnection`も提供しますが、GPIO出力には`GpioController`を
使用してください。実機のポート、配線、出力論理、安全状態は装置ごとに確認してください。
