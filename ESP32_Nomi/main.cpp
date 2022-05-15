#include <Arduino.h>

//20220514am
//サーボ/wifi/bno055センサに加えてPS4のBTコントローラ接続を合成
//通信品質は特にリモコン操作時にエラーをたくさん吐き出してしまう問題あり

//20220514 
//サーボ/wifi/bno055センサを合成し、かろうじて動いている状態のもの
//センサデータの配列への格納は未実装
//モニタリング用のセンサデータシリアル書き出しがあるので、そこの処理は重そう
//PlatformIOでの書き込みが前提。ボードバージョンは3.3.2で動作確認

//20220509
// 100Hzでなかなか安定。分単位でエラーがでないこともあれば、連続してギクシャクすることもある。
// スキップエラーは1%以下。
// ESP32 DevkitC + PlatformIOで使うことを想定
// PlatformIO上でのESP32のボードバージョンは3.3.2が良い。（4系はうまく動かないらしい）
// Serial1を使うには設定を変更する必要あり. 参考 → https://twitter.com/Ninagawa123/status/1522963599353806849

//[E-2] ライブラリ導入 -----------------------------------
#include <WiFi.h>    //UDPの設定
#include <WiFiUdp.h> //UDPの設定
#include <IcsHardSerialClass.h>
#include <Adafruit_BNO055.h>
#include <SPI.h> //SPIは使用しないが<Adafruit_BNO055.h>のインクルードに必要
#include <PS4Controller.h> //PS4コントローラー

Adafruit_BNO055 bno = Adafruit_BNO055(55, 0x28);

TaskHandle_t thp[4]; //マルチスレッドのタスクハンドル格納用

//頻繁に変更する変数 #DEFINE ---------------------------------
#define FRAME_DURATION 10      // 1フレームあたりの単位時間（単位ms）
#define AP_SSID "bishamon2"    //アクセスポイントのAP_SSID
#define AP_PASS "c6edKxHU"     //アクセスポイントのパスワード
#define SEND_IP "192.168.1.26" //送り先のPCのIPアドレス（PCのIPアドレスを調べておく）

//[E-3] 各種設定 #DEFINE ---------------------------------
#define MSG_SIZE 90                                                // Meridim配列の長さ設定（デフォルトは90）
#define SEND_PORT 22222                                            //送り先のポート番号
#define RESV_PORT 22224                                            //このESP32のポート番号
#define ERR_LED 22                                                 // LED用 処理が時間内に収まっていない場合に点灯
#define BAUDRATE1 1250000                                          //サーボの通信速度
#define TIMEOUT1 3                                                 //サーボ返信エラーをスルーするのに程よい設定
#define EN_PIN_L 5                                                 //サーボL系統のENピン
#define EN_PIN_R 4                                                 //サーボR系統のENピン
IcsHardSerialClass krs_L(&Serial1, EN_PIN_L, BAUDRATE1, TIMEOUT1); //サーボL系統UARTの設定（TX16,RX17,EN4）
IcsHardSerialClass krs_R(&Serial2, EN_PIN_R, BAUDRATE1, TIMEOUT1); //サーボR系統UARTの設定（TX14,RX27,EN5）
#define SERVO_NUM_L 11                                             // L系統につないだサーボの数
#define SERVO_NUM_R 11                                             // R系統につないだサーボの数

#define JOYPAD_MOUNT 4    ////ジョイパッドの搭載 0:なし、1:Wii_yoko, 2:Wii+Nun, 3:PS3, 4:PS4, 5:WiiPRO, 6:Xbox
#define JOYPAD_POLLING 10 ////ジョイパッドの問い合わせフレーム間隔

//コントローラー用変数
unsigned short pad_btn = 0;
short pad_stick_R_x = 0;
short pad_stick_R_y = 0;
short pad_stick_L_x = 0;
short pad_stick_L_y = 0;
short pad_R2_val = 0;
short pad_L2_val = 0;
long pad_btn_disp = 65536;
int joypad_count = 0; ////ジョイパッドの問い合わせフレームカウント用

//変数一般
long frame_count = 0;
static const int MSG_BUFF = MSG_SIZE * 2;
long frame_ms = FRAME_DURATION;    // 1フレームあたりの単位時間(ms)
long merc = (long)millis();        // フレーム管理時計の時刻 Meridian Clock.
long curr_millis = (long)millis(); // 現在時刻を取得
long curr_micro = (long)micros();  // 現在時刻を取得
char frame_sync_s = 0;             //フレーム毎に0-199をカウントし、送信用Meridm[88]の下位8ビットに格納
char frame_sync_r_expect = 0;      //フレーム毎に0-199をカウントし、受信値と比較
char frame_sync_r_resv = 0;        //今フレームに受信したframe_sync_rを格納
long error_count_udp = 0;
int frame_count_diff = 5;
int kr; //角度データ一時保存用
int kl;
float n; //計算用
//uint8_t *s_message_buf;
//uint8_t *r_message_buf;





// wifi設定
WiFiUDP udp;

//共用体の設定。共用体はたとえばデータをショートで格納し、バイト型として取り出せる
typedef union
{
  short sval[MSG_SIZE];
  uint8_t bval[MSG_BUFF];
} UnionData;
UnionData s_udp_meridim; // UDP送信用共用体のインスタンスを宣言
UnionData r_udp_meridim; // UDP受信用共用体のインスタンスを宣言
UnionData pad_udp_meridim; // UDP送信用共用体のインスタンスを宣言


// (TS-6-9-2) KRSサーボのポジション用配列.degreeではなくサーボ値が入る
int s_servo_pos_L[15];
int s_servo_pos_R[15];
int r_servo_pos_L[15];
int r_servo_pos_R[15];

// (TS-6-9-3) 各サーボのサーボマウントありなし判定用配列（mtはmountの略）
bool idl_mt[15]; // L系統
bool idr_mt[15]; // R系統

// (TS-6-9-4) 各サーボの回転方向順逆補正用配列（pnはposi/negaの略）
int idl_pn[15]; // L系統
int idr_pn[15]; // R系統

// (TS-6-9-5-KRS) 各サーボの直立ポーズ時のデフォルト値（nはneutralの略）
int idl_n[15]; // L系統
int idr_n[15]; // R系統

// (TS-6-9-6) 各サーボのポジション値.中央値を0とした時の増減合計値
int idl_d[15]; // L系統
int idr_d[15]; // R系統

// +----------------------------------------------------------------------
// | 関数名　　:  float2HfShort(float val)
// +----------------------------------------------------------------------
// | 機能　　　:  floatを100倍し小数点以下を四捨五入した整数をshortに収める
// | 引数　　　:  float型. 有効数字　-327.67 ~ 327.67
// | 戻り値　　:  short型. 限界値を超えたら限界値を返す(-32767,32767)
// +----------------------------------------------------------------------
short float2HfShort(float val)
{
  int x = round(val * 100); //floatの小数点以下を四捨五入して整数化
  if (x > 32766) {
    x = 32767;
  } else if (x < -32766) {
    x = -32767;
  }
  return (short)x;
}

// +----------------------------------------------------------------------
// | 関数名　　:  float2HfInt(float val)
// +----------------------------------------------------------------------
// | 機能　　　:  floatを100倍し小数点以下を四捨五入した整数をintに収める
// | 引数　　　:  float型. 有効数字8桁(例:123.45678, -12345.678)
// | 戻り値　　:  int型.
// +----------------------------------------------------------------------
int float2HfInt(float val)
{
  int x = round(val * 100); //100倍し小数点以下を四捨五入
  return x;
}

// +----------------------------------------------------------------------
// | 関数名　　:  Krs2HfDeg(int krs, int n, int pn)
// +----------------------------------------------------------------------
// | 機能     :  KRS単位値をdegree値*100に変換. HfはHundredfoldの略.
// | 引数１　　:  int型. サーボ値（KRS単位値）
// | 引数２　　:  int型. トリム値（KRS単位値）       具体例：idl_n[i], idr_n[i]
// | 引数３　　:  int型. 回転方向の順逆補正（+1,-1）　具体例：idl_pn[i],idr_pn[i]
// | 戻り値　　:  int型. degree値*100
// +----------------------------------------------------------------------
int Krs2HfDeg(int krs, int n, int pn)
{
  float x = (krs - 7500 - n) * 3.375 * pn;
  return int(x);
}

// +----------------------------------------------------------------------
// | 関数名　　:  HfDeg2Krs(int hfdegree, int n, int pn)
// +----------------------------------------------------------------------
// | 機能     :  degree値*100 をKRS単位値に変換. HfはHundredfoldの略.
// | 引数１　　:  float型. degree値
// | 引数２　　:  int型. トリム値（KRS単位値）       具体例：idl_n[i], idr_n[i]
// | 引数３　　:  int型. 回転方向の順逆補正（+1,-1）　具体例：idl_pn[i],idr_pn[i]
// | 戻り値　　:  int型. KRS単位値（3500-11500）
// | 備考　　　:  0.02度ぐらいからサーボ値には反映される(=0.59で1に繰り上がる)
// +----------------------------------------------------------------------
int HfDeg2Krs(int hfdegree, int n, int pn)
{
  float x = 7500 + n + (hfdegree / 3.375 * pn); //
  if (x > 11500)                                //上限を設定
  {
    x = 11500;
  }
  else if (x < 3500) //下限を設定
  {
    x = 3500;
  }
  return int(x);
}

// +----------------------------------------------------------------------
// | 関数名　　:  checksum_val(short arr[], int len)
// +----------------------------------------------------------------------
// | 機能     :  配列のチェックサムを算出
// | 　　     :  チェックサムは配列の末尾を除く合計数をビット反転しShort型にしたもの
// | 引数１　　:  Meridim配列(Short型の配列)
// | 引数２　　:  配列の長さ
// | 戻り値　　:  short型. チェックサム値
// +----------------------------------------------------------------------
short checksum_val(short arr[], int len)
{
  int cksm = 0;
  for (int i = 0; i < len - 1; i++)
  {
    cksm += int(arr[i]);
  }
  return short(~cksm);
}

// +-------------------------------------------------------------------
// | 関数名　　:  checksum_rslt(short arr[], int len)
// +-------------------------------------------------------------------
// | 機能     :  チェックサムの合否判定
// | 　　     :  配列の末尾を除く合計数をビット反転しShort型にしたものと、末尾の値を比較
// | 引数１　　:  Meridim配列(Short型の配列)
// | 引数２　　:  配列の長さ
// | 戻り値　　:  bool型. チェックサム値がOKならtrue, NGならfalse
// +------------------------------------------------------------------
bool checksum_rslt(short arr[], int len)
{
  int cksm = 0;
  for (int i = 0; i < len - 1; i++)
  {
    cksm += int(arr[i]);
  }
  if (short(~cksm) == arr[len - 1])
  {
    return true;
  }
  return false;
}

// +----------------------------------------------------------------------
// | 関数名　　:  receiveUDP()
// +----------------------------------------------------------------------
// | 機能     :  UDP通信の受信パケットを確認し、
// | 　　        受信していたら共用体r_udp_meridimに値を格納する
// | 引数　　　:  なし
// +----------------------------------------------------------------------
void receiveUDP()
{
  if (udp.parsePacket() >= MSG_BUFF) //データの受信バッファ確認
  {
    udp.read(r_udp_meridim.bval, MSG_BUFF); //データの受信
  }
}

// +----------------------------------------------------------------------
// | 関数名　　:  receiveUDP()
// +----------------------------------------------------------------------
// | 機能     :  共用体s_udp_meridimをUDP通信で送信する
// | 引数　　　:  なし
// +----------------------------------------------------------------------
void sendUDP()
{
  udp.beginPacket(SEND_IP, SEND_PORT); // UDPパケットの開始
  udp.write(s_udp_meridim.bval, MSG_BUFF);
  udp.endPacket(); // UDPパケットの終了
}

float bno055_read[16];

void get_bno055_data(void *args)
{
  while (1)
  {

    // Possible vector values can be:
    // - VECTOR_ACCELEROMETER - m/s^2
    // - VECTOR_MAGNETOMETER  - uT
    // - VECTOR_GYROSCOPE     - rad/s
    // - VECTOR_EULER         - degrees
    // - VECTOR_LINEARACCEL   - m/s^2
    // - VECTOR_GRAVITY       - m/s^2

    /*
    // キャリブレーションのステータスの取得と表示
    uint8_t system, gyro, accel, mag = 0;
    bno.getCalibration(&system, &gyro, &accel, &mag);
    Serial.print("CALIB Sys:");
    Serial.print(system, DEC);
    Serial.print(", Gy");
    Serial.print(gyro, DEC);
    Serial.print(", Ac");
    Serial.print(accel, DEC);
    Serial.print(", Mg");
    Serial.print(mag, DEC);
    */

    /*
    // ジャイロセンサ値の取得と表示
    imu::Vector<3> gyroscope = bno.getVector(Adafruit_BNO055::VECTOR_GYROSCOPE);
    Serial.print(" 　Gy_xyz:");
    Serial.print(gyroscope.x());
    Serial.print(", ");
    Serial.print(gyroscope.y());
    Serial.print(", ");
    Serial.print(gyroscope.z());
    */

    /*
    // 加速度センサ値の取得と表示
    imu::Vector<3> accelermetor = bno.getVector(Adafruit_BNO055::VECTOR_ACCELEROMETER);
    Serial.print(" 　Ac_xyz:");
    Serial.print(accelermetor.x());
    Serial.print(", ");
    Serial.print(accelermetor.y());
    Serial.print(", ");
    Serial.print(accelermetor.z());
    */

    /*
    // 磁力センサ値の取得と表示
    imu::Vector<3> magnetmetor = bno.getVector(Adafruit_BNO055::VECTOR_MAGNETOMETER);
    Serial.print(" 　Mg_xyz:");
    Serial.print(magnetmetor .x());
    Serial.print(", ");
    Serial.print(magnetmetor .y());
    Serial.print(", ");
    Serial.print(magnetmetor .z());
    */

    // センサフュージョンによる方向推定値の取得と表示
    imu::Vector<3> euler = bno.getVector(Adafruit_BNO055::VECTOR_EULER);
    //Serial.print("  DIR_xyz:");
    //Serial.print(euler.x());
    //Serial.print(", ");
    //Serial.print(euler.y());
    //Serial.print(", ");
    //Serial.print(euler.z());
    bno055_read[12] = euler.y();//DMP_ROLL推定値
    bno055_read[13] = euler.z();//DMP_PITCH推定値
    bno055_read[14] = euler.x()-180;//DMP_YAW推定値

    /*
      // センサフュージョンの方向推定値のクオータニオン
      imu::Quaternion quat = bno.getQuat();
      Serial.print("qW: ");
      Serial.print(quat.w(), 4);
      Serial.print(" qX: ");
      Serial.print(quat.x(), 4);
      Serial.print(" qY: ");
      Serial.print(quat.y(), 4);
      Serial.print(" qZ: ");
      Serial.print(quat.z(), 4);
      Serial.print("\t\t");
    */

    //Serial.println();
    delay(10);
  }
}

//-------------------------------------------------------------------------
//---- 関 数 各 種  --------------------------------------------------------
//-------------------------------------------------------------------------

// ■ PS4コントローラ受信用関数 ----------------------------------------------------------

void PS4pad_receive()
{
  // Below has all accessible outputs from the controller
  if (PS4.isConnected())
  {
    pad_btn = 0;
    if (PS4.Right())
      pad_btn |= (B00000000 * 256) + B00100000;
    if (PS4.Down())
      pad_btn |= (B00000000 * 256) + B01000000;
    if (PS4.Up())
      pad_btn |= (B00000000 * 256) + B00010000;
    if (PS4.Left())
      pad_btn |= (B00000000 * 256) + B10000000;

    if (PS4.Square())
      pad_btn |= (B10000000 * 256) + B00000000;
    if (PS4.Cross())
      pad_btn |= (B01000000 * 256) + B00000000;
    if (PS4.Circle())
      pad_btn |= (B00100000 * 256) + B00000000;
    if (PS4.Triangle())
      pad_btn |= (B00010000 * 256) + B00000000;

    if (PS4.UpRight())
      pad_btn |= (B00000000 * 256) + B00110000;
    if (PS4.DownRight())
      pad_btn |= (B00000000 * 256) + B01100000;
    if (PS4.UpLeft())
      pad_btn |= (B00000000 * 256) + B10010000;
    if (PS4.DownLeft())
      pad_btn |= (B00000000 * 256) + B11000000;

    if (PS4.L1())
      pad_btn |= (B00000100 * 256) + B00000000;
    if (PS4.R1())
      pad_btn |= (B00001000 * 256) + B00000000;

    if (PS4.Share())
      pad_btn |= (B00000000 * 256) + B00000001;
    if (PS4.Options())
      pad_btn |= (B00000000 * 256) + B00001000;
    if (PS4.L3())
      pad_btn |= (B00000000 * 256) + B00000100;
    if (PS4.R3())
      pad_btn |= (B00000000 * 256) + B00000010;

    if (PS4.PSButton())
      pad_btn |= (B00000000 * 256) + B01010000; // same as up & down
    if (PS4.Touchpad())
      pad_btn |= (B00000000 * 256) + B00101000; // same as left & right

    if (PS4.L2())
    {
      pad_btn |= (0x00000001 * 256) + B00000000;
      pad_L2_val = constrain(PS4.L2Value(), 0, 255);
    }
    if (PS4.R2())
    {
      pad_btn |= 512; //(0x00000010 * 256) + B00000000;
      pad_R2_val = constrain(PS4.R2Value(), 0, 255);
    }

    if (PS4.LStickX())
    {
      pad_stick_L_x = constrain(PS4.LStickX(), -127, 127);
    }
    if (PS4.LStickY())
    {
      pad_stick_L_y = constrain(PS4.LStickY(), -127, 127);
    }
    if (PS4.RStickX())
    {
      pad_stick_R_x = constrain(PS4.RStickX(), -127, 127);
    }
    if (PS4.RStickY())
    {
      pad_stick_R_y = constrain(PS4.RStickY(), -127, 127);
    }

    pad_udp_meridim.sval[80] = pad_btn;
    pad_udp_meridim.sval[81] = pad_stick_L_x * 256 + pad_stick_L_y;
    pad_udp_meridim.sval[82] = pad_stick_R_x * 256 + pad_stick_R_y;
    pad_udp_meridim.sval[83] = pad_L2_val * 256 + pad_R2_val;
  }
}

void Core0_BT_r(void *args)
{ //サブCPU(Core0)で実行するプログラム
  while (1)
  { //ここで無限ループを作っておく
    //コントローラの受信ループ
    if (JOYPAD_MOUNT == 4)
    {
      PS4pad_receive();
    }
    Serial.println(pad_udp_meridim.sval[80], BIN);

    delay(1); // JOYPAD_POLLING ms秒待つ
  }
}




// +----------------------------------------------------------------------
// | SET UP
// +----------------------------------------------------------------------
void setup()
{
  Serial.begin(2000000);
  delay(100);
  Serial.println("Serial Start...");

  pinMode(ERR_LED, OUTPUT);
  pinMode(21, INPUT_PULLUP); // SDA 21番ピンのプルアップ(念のため)
  pinMode(22, INPUT_PULLUP); // SDA 22番ピンのプルアップ(念のため)

  if (!bno.begin()) // センサの初期化
  {
    Serial.print("Ooops, no BNO055 detected ... Check your wiring or I2C ADDR!");
    while (1)
      ;
  }
  delay(100);
  bno.setExtCrystalUse(false);
  xTaskCreatePinnedToCore(get_bno055_data, "get_bno055_data", 4096, NULL, 10, &thp[0], 1);
  xTaskCreatePinnedToCore(Core0_BT_r, "Core0_BT_r", 4096, NULL, 5, &thp[1], 0);

  krs_L.begin(); //サーボモータの通信初期設定
  krs_R.begin(); //サーボモータの通信初期設定

  // WiFi 初期化
  WiFi.disconnect(true, true);                                  // WiFi接続をリセット
  Serial.println("Connecting to WiFi to : " + String(AP_SSID)); //接続先を表示
  delay(100);
  WiFi.begin(AP_SSID, AP_PASS); // Wifiに接続
  while (WiFi.status() != WL_CONNECTED)
  {            // https://www.arduino.cc/en/Reference/WiFiStatus 返り値一覧
    delay(50); //接続が完了するまでループで待つ
  }
  Serial.println("WiFi connected."); // WiFi接続完了通知
  Serial.print("ESP32's IP address is  => ");
  Serial.println(WiFi.localIP()); // ESP32自身のIPアドレスの表示

  // UDP通信の開始
  udp.begin(RESV_PORT);
  // delay(100);

  // ESP32自身のBluetoothMacアドレスを表示
  uint8_t bt_mac[6];
  String self_mac_address = "";
  esp_read_mac(bt_mac, ESP_MAC_BT);
  self_mac_address = String(bt_mac[0], HEX) + ":" + String(bt_mac[1], HEX) + ":" + String(bt_mac[2], HEX) + ":" + String(bt_mac[3], HEX) + ":" + String(bt_mac[4], HEX) + ":" + String(bt_mac[5], HEX);
  Serial.print("ESP32's Bluetooth Mac Address is => " + self_mac_address);
  Serial.println();
  delay(1000);
  delay(1000);




  // コントローラの接続開始
  // PS4コントローラの接続開始
  if (JOYPAD_MOUNT == 4)
  {
    PS4.begin("e8:68:e7:30:b4:52"); // ESP32のMACが入ります.PS4にも設定します。
  }

  // +----------------------------------------------------------------------
  // |  (TS-10) サーボ設定
  // +----------------------------------------------------------------------
  // (TS-10-1) 各サーボのマウントありなし（1:サーボあり、0:サーボなし）
  idl_mt[0] = true;   //頭ヨー
  idl_mt[1] = true;   //左肩ピッチ
  idl_mt[2] = true;   //左肩ロール
  idl_mt[3] = true;   //左肘ヨー
  idl_mt[4] = true;   //左肘ピッチ
  idl_mt[5] = true;   //左股ヨー
  idl_mt[6] = true;   //左股ロール
  idl_mt[7] = true;   //左股ピッチ
  idl_mt[8] = true;   //左膝ピッチ
  idl_mt[9] = true;   //左足首ピッチ
  idl_mt[10] = true;  //左足首ロール
  idl_mt[11] = false; //予備
  idl_mt[12] = false; //予備
  idl_mt[13] = false; //予備
  idl_mt[14] = false; //予備
  idr_mt[0] = true;   //腰ヨー
  idr_mt[1] = true;   //右肩ピッチ
  idr_mt[2] = true;   //右肩ロール
  idr_mt[3] = true;   //右肘ヨー
  idr_mt[4] = true;   //右肘ピッチ
  idr_mt[5] = true;   //右股ヨー
  idr_mt[6] = true;   //右股ロール
  idr_mt[7] = true;   //右股ピッチ
  idr_mt[8] = true;   //右膝ピッチ
  idr_mt[9] = true;   //右足首ピッチ
  idr_mt[10] = true;  //右足首ロール
  idr_mt[11] = false; //予備
  idr_mt[12] = false; //予備
  idr_mt[13] = false; //予備
  idr_mt[14] = false; //予備

  // (TS-10-2) 各サーボの内外回転プラマイ方向補正(1 or -1)
  idl_pn[0] = 1;  //頭ヨー
  idl_pn[1] = 1;  //左肩ピッチ
  idl_pn[2] = 1;  //左肩ロール
  idl_pn[3] = 1;  //左肘ヨー
  idl_pn[4] = 1;  //左肘ピッチ
  idl_pn[5] = 1;  //左股ヨー
  idl_pn[6] = 1;  //左股ロール
  idl_pn[7] = 1;  //左股ピッチ
  idl_pn[8] = 1;  //左膝ピッチ
  idl_pn[9] = 1;  //左足首ピッチ
  idl_pn[10] = 1; //左足首ロール
  idl_pn[11] = 1; //予備
  idl_pn[12] = 1; //予備
  idl_pn[13] = 1; //予備
  idl_pn[14] = 1; //予備
  idr_pn[0] = 1;  //腰ヨー
  idr_pn[1] = 1;  //右肩ピッチ
  idr_pn[2] = 1;  //右肩ロール
  idr_pn[3] = 1;  //右肘ヨー
  idr_pn[4] = 1;  //右肘ピッチ
  idr_pn[5] = 1;  //右股ヨー
  idr_pn[6] = 1;  //右股ロール
  idr_pn[7] = 1;  //右股ピッチ
  idr_pn[8] = 1;  //右膝ピッチ
  idr_pn[9] = 1;  //右足首ピッチ
  idr_pn[10] = 1; //右足首ロール
  idr_pn[11] = 1; //予備
  idr_pn[12] = 1; //予備
  idr_pn[13] = 1; //予備
  idr_pn[14] = 1; //予備

  // (TS-10-3) 各サーボの直立デフォルト値　(KRS値  0deg=7500, +-90deg=7500+-2667  KRS値=deg/0.03375)
  //           直立状態になるよう、具体的な数値を入れて現物調整する
  idl_n[0] = 0;     //頭ヨー
  idl_n[1] = -70;   //左肩ピッチ
  idl_n[2] = -2700; //左肩ロール
  idl_n[3] = 0;     //左肘ヨー
  idl_n[4] = 2666;  //左肘ピッチ
  idl_n[5] = 0;     //左股ヨー
  idl_n[6] = 0;     //左股ロール
  idl_n[7] = -40;   //左股ピッチ
  idl_n[8] = -1720; //左膝ピッチ
  idl_n[9] = -600;  //左足首ピッチ
  idl_n[10] = -20;  //左足首ロール
  idl_n[11] = 0;    //予備
  idl_n[12] = 0;    //予備
  idl_n[13] = 0;    //予備
  idl_n[14] = 0;    //予備
  idr_n[0] = 0;     //腰ヨー
  idr_n[1] = 0;     //右肩ピッチ
  idr_n[2] = -2650; //右肩ロール
  idr_n[3] = 0;     //右肘ヨー
  idr_n[4] = 2666;  //右肘ピッチ
  idr_n[5] = 0;     //右股ヨー
  idr_n[6] = 50;    //右股ロール
  idr_n[7] = -100;  //右股ピッチ
  idr_n[8] = -1700; //右膝ピッチ
  idr_n[9] = -600;  //右足首ピッチ
  idr_n[10] = -70;  //右足首ロール
  idr_n[11] = 0;    //予備
  idr_n[12] = 0;    //予備
  idr_n[13] = 0;    //予備
  idr_n[14] = 0;    //予備

  merc += 8000; //設定時間分をmercに追加

  // curr_millis = (long)millis();
  // Serial.println(curr_millis - merc);
}



// +-------------------------------------------------------------------
// | MAIN LOOP
// +-------------------------------------------------------------------
void loop()
{
  // bno055ticker.update（）;

  //---- < 1 > U D P 受 信 --------------------------------------------------
  //[1-1] UDP受信の実行 もしデータパケットが来ていれば受信する
  receiveUDP();
  delay(1);

  //　→ ここでr_udp_meridim.sval に受信したMeridim配列が入っている状態。

  //[1-2] UDP受信配列からSPI送信配列にデータを転写
  memcpy(s_udp_meridim.bval, r_udp_meridim.bval, MSG_SIZE * 2);

  //----  [ xx ] ESP 内 部 で 位 置 制 御 す る 場 合 の 処 理 -----------------------------

  // [xxx] コントロールパッド受信値の転記
  for (int i = 80; i < 84; i++)
  {
    s_udp_meridim.sval[i]=pad_udp_meridim.sval[i];
  }

  // [7-2] 前回のラストに読み込んだサーボ位置をサーボ配列に書き込む
  for (int i = 0; i < 15; i++)
  {
    s_servo_pos_L[i] = HfDeg2Krs(int(r_udp_meridim.sval[i * 2 + 21]), idl_n[i], idl_pn[i]);
    s_servo_pos_R[i] = HfDeg2Krs(int(r_udp_meridim.sval[i * 2 + 51]), idr_n[i], idr_pn[i]);
  }

  //チェックサムを確認
  if (checksum_rslt(r_udp_meridim.sval, MSG_SIZE)) // Check sum OK!
  {
    s_udp_meridim.bval[177] &= B10111111; //エラーフラグ14番(ESP32のPCからのUDP受信エラー検出)をオフ
    // サーボ受信値の処理
    for (int i = 0; i < SERVO_NUM_R; i++) //接続したサーボの数だけ繰り返す。最大は15
    {
      if (idr_mt[i]) //サーボのマウントありなしを確認
      {
        if (r_udp_meridim.sval[(i * 2) + 50] == 1) //サーボコマンドが1ならサーボをオンして位置を指定
        {
          kr = krs_R.setPos(i, HfDeg2Krs(r_udp_meridim.sval[i * 2 + 51], idr_n[i], idr_pn[i]));
        }
        else //サーボコマンドが0ならサーボをオフして位置を取得
        {
          kr = krs_R.setFree(i);
        } //サーボの返り値を送信Meridim配列に格納
        s_udp_meridim.sval[(i * 2) + 51] = short(Krs2HfDeg(kr, idr_n[i], idr_pn[i]));
      }
      if (idl_mt[i]) //サーボのマウントありなしを確認
      {
        if (r_udp_meridim.sval[(i * 2) + 20] == 1) //サーボコマンドが1ならサーボをオンして位置を指定
        {
          kl = krs_L.setPos(i, HfDeg2Krs(r_udp_meridim.sval[i * 2 + 21], idl_n[i], idl_pn[i]));
        }
        else //サーボコマンドが0ならサーボをオフして位置を取得
        {
          kl = krs_L.setFree(i); // 1以外ならとりあえずサーボを脱力し位置を取得。手持ちの最大は15
        }                        //サーボの返り値を送信Meridim配列に格納
        s_udp_meridim.sval[(i * 2) + 21] = short(Krs2HfDeg(kl, idr_n[i], idr_pn[i]));
      }
      delayMicroseconds(3); //コツ. このディレイがあるとサーボの送受信がうまく動く.
    }
  }
  else // Check sum NG*
  {
    error_count_udp++;
    s_udp_meridim.bval[177] |= B01000000; //エラーフラグ14番(ESP32のPCからのUDP受信エラー検出)をオン
  }

  // [1-3] 通信エラー処理(スキップ検出)
  frame_sync_r_expect++; //フレームカウント予想値を加算
  if (frame_sync_r_expect > 199)
  { //予想値が200以上ならカウントを0に戻す
    frame_sync_r_expect = 0;
  }
  // frame_sync_rの確認(受信フレームにスキップが生じていないかをMeridim[88]の下位8ビットのカウンターで判断)
  frame_sync_r_resv = s_udp_meridim.bval[176]; //数値を受け取る

  if (frame_sync_r_resv == frame_sync_r_expect) // frame_sync_rの受信値が期待通りなら順番どおり受信
  {
    s_udp_meridim.bval[177] &= B11111011; //エラーフラグ9番(Teensy受信のスキップ検出)をオフ
  }
  else
  {
    s_udp_meridim.bval[177] |= B00000100; //エラーフラグ9番(Teensy受信のスキップ検出)をオン
    if (frame_sync_r_resv == frame_sync_r_expect - 1)
    {
      frame_sync_r_expect = frame_sync_r_resv + 1; //同じ値を２回取得した場合には実際はシーケンスが進んだものとして補正（アルゴリズム要検討）
    }
    else
    {
      frame_sync_r_expect = frame_sync_r_resv; //取りこぼしについては現在の受信値を正解の予測値としてキープ
    }
  }

  n = cos(PI / 2 / 300 * frame_count) * 500;

  //----  [ 11 ] フ レ ー ム 終 端 処 理 ----------------------------------------------

  // [11-1] この時点で１フレーム内に処理が収まっていない時の処理
  curr_millis = (long)millis(); // 現在時刻を更新
  if (curr_millis > merc)
  {                                           // 現在時刻がフレーム管理時計を超えていたらアラートを出す
    Serial.println("*** processing delay :"); //シリアルに遅延msを表示
    Serial.println(curr_millis - merc);
    digitalWrite(ERR_LED, HIGH); //処理落ちが発生していたらLEDを点灯
  }
  else
  {
    digitalWrite(ERR_LED, LOW); //処理が収まっていればLEDを消灯
  }

  // [11-2] この時点で時間が余っていたら時間消化。時間がオーバーしていたらこの処理を自然と飛ばす。
  curr_millis = (long)millis();
  curr_micro = (long)micros(); // 現在時刻を取得

  while ((merc - curr_millis) >= 1)
  {
    delay(1);
    curr_millis = (long)millis();
  }

  while (curr_micro < merc * 1000)
  {
    curr_micro = (long)micros(); // 現在時刻を取得
  }

  // [11-3]フレーム管理時計mercのカウントアップ
  merc = merc + frame_ms;                       //フレーム管理時計を1フレーム分進める
  frame_count = frame_count + frame_count_diff; //サインカーブ動作用のフレームカウントをいくつずつ進めるかをここで設定。

  //---- < 4 > U D P 送 信 信 号 作 成 --------------------------------------------------

  // [xxx] センサーからの値を送信用に格納

  s_udp_meridim.sval[12]=float2HfShort(bno055_read[12]);//DMP_ROLL推定値
  s_udp_meridim.sval[13]=float2HfShort(bno055_read[13]);//DMP_PITCH推定値
  s_udp_meridim.sval[14]=float2HfShort(bno055_read[14]);//DMP_YAW推定値

  // [9-5] フレームスキップ検出用のカウントをカウントアップして送信用に格納
  frame_sync_s++;
  if (frame_sync_s > 199)
  {
    frame_sync_s = 0;
  }
  s_udp_meridim.bval[176] = frame_sync_s;

  // チェックサムを計算して格納
  s_udp_meridim.sval[MSG_SIZE - 1] = checksum_val(s_udp_meridim.sval, MSG_SIZE);

  //---- < 5 > U D P 送 信 ----------------------------------------------
  //[4-1] UDP送信を実行
  sendUDP();
  delay(1);
}

