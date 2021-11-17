//Meridian_211117_for_ESP32
//ESP32_DevKitC

/*
  ESP32 Pin Assign

  [3V3]               -> GND
  [EN]                -> ICS_3rd_TX
  [VP,36]             -> ICS_3rd_RX
  [VN,39]             -> LED（lights up when the processing time is not within the specified time.）
  [34]                -> (NeoPixel Data)
  [35]                -> (NeoPixel Clock)
  [32]                ->
  [33]                ->
  [25]                ->
  [26]                ->
  [27]                ->
  [14] SPI_CLK        -> SPI_SCK (Teensy[13])
  [12] SPI_MISO       -> SPI_MISO (Teensy[12])
  [GND]               ->
  [13] SPI_MOSI       -> SPI_MOSI (Teensy[11])
  [D2,9] RX1          ->
  [D3,10] TX1         ->
  [CMD,11]            ->
  [5V]                ->

  [GND]               ->
  [23]                ->
  [22] I2C_SCL        ->
  [TX] TX0            ->
  [RX] RX0            ->
  [21] I2C_SDA        ->
  [GND]               ->
  [19]                ->
  [18]                ->
  [05]                ->
  [17] TX2            ->
  [16] RX2            ->
  [04]                ->
  [00]                ->
  [02]                ->
  [15] SPI_SS         -> SPI_CS (Teensy[10])
  [D1,8]              ->
  [D0,7] I2C_SCL      ->
  [CLK,6] I2C_SDA     ->
*/

#include <ESP32DMASPISlave.h>

ESP32DMASPI::Slave slave;

static const int MSG_SIZE = 92;
static const int MSG_BUFF = MSG_SIZE * 2;

uint8_t* s_message_buf;
uint8_t* r_message_buf;
int checksum;


typedef union
{
  short sval[MSG_SIZE];
  uint8_t bval[MSG_BUFF];
} UnionData;

UnionData s_message_buf_2;
UnionData r_message_buf_2;


//UDPの設定
#include <WiFi.h>
#include <WiFiUdp.h>

const char* ssid = "xxxx";
const char* password = "xxxx";
const char* send_address = "192.168.xxx.xxx";  //送り先
const int send_port = 22222;  //送り先
const int receive_port = 22224;  //このESP32 のポート番号

WiFiUDP udp;


//共用体の設定。共用体はたとえばデータをショートで格納し、バイト型として取り出せる
typedef union {
  short sval[MSG_SIZE];//ショート型
  uint8_t bval[MSG_BUFF];//符号なしバイト型
} UDPData;
UDPData s_upd_message_buf; //送信用共用体のインスタンスを宣言
UDPData r_upd_message_buf; //受信用共用体のインスタンスを宣言

int count = 0;//送信変数用のカウント

//通信のエラーカウント
long spi_error = 0;
long spi_trial = 0;
long udp_ok = 0;
long udp_trial = 0;

//************************************************
//*********** 各 種 モ ー ド 設 定  ****************
//************** 0:OFF, 1:ON  ********************
//************************************************
bool monitor_src = 0; //ESP32でのシリアル表示:送信ソースデータ
bool monitor_udp_send = 0; //ESP32でのシリアル表示:UDP送信データ
bool monitor_udp_resv = 0; //ESP32でのシリアル表示:UDP受信データ
bool monitor_spi_send = 0; //ESP32でのシリアル表示:SPI送信データ
bool monitor_spi_resv = 0; //ESP32でのシリアル表示:SPI受信データ
bool monitor_udp_resv_check = 0; //ESP32でのシリアル表示:受信成功の可否
bool monitor_udp_resv_error = 0; //ESP32でのシリアル表示:受信エラー率
bool monitor_spi_resv_error = 0; //ESP32でのシリアル表示:受信エラー率
bool monitor_all_error = 0; //ESP32でのシリアル表示:全経路の受信エラー率(未導入)
bool monitor_resv_checksum = 0; //ESP32でのシリアル表示:受信成功の可否

void setup()
{
  Serial.begin(2000000);
  delay(500);//シリアル準備待ち用ディレイ
  Serial.println("Serial Start...");

  //WiFi 初期化
  WiFi.disconnect(true, true);//WiFi接続をリセット
  Serial.println("Connecting to WiFi to : " + String(ssid));//接続先を表示
  delay(100);
  WiFi.begin(ssid, password);//Wifiに接続
  while ( WiFi.status() != WL_CONNECTED) {//https://www.arduino.cc/en/Reference/WiFiStatus 返り値一覧
    delay(100);//接続が完了するまでループで待つ
  }
  Serial.println("WiFi connected.");//WiFi接続完了通知
  Serial.print("WiFi connected. ESP32's IP address is : ");
  Serial.println(WiFi.localIP());//デバイスのIPアドレスの表示

  //UDP 開始
  udp.begin(receive_port);
  delay(500);

  // use DMA
  s_message_buf = slave.allocDMABuffer(MSG_BUFF);
  r_message_buf = slave.allocDMABuffer(MSG_BUFF);

  // reset buffers
  memset(s_message_buf, 0, MSG_BUFF);
  memset(r_message_buf, 0, MSG_BUFF);

  // set send data

  checksum = 0;
  for (int i = 0; i < MSG_SIZE - 1; i++) // put datas in allay except last
  {
    s_message_buf[i] = uint8_t(MSG_SIZE - i);
    checksum += MSG_SIZE - i; // add checksum
  }
  s_message_buf[MSG_SIZE - 1] = uint8_t(checksum & 0xFF ^ 0xFF); //put checksum in last

  slave.setDataMode(SPI_MODE3);
  slave.setMaxTransferSize(MSG_BUFF);
  slave.setDMAChannel(2);
  slave.setQueueSize(1);
  slave.begin();
}



//受信用の関数
//パケットが来ていれば、RECVDATANUM 分だけr_ufdata配列に保存する
void receiveUDP() {
  int packetSize = udp.parsePacket();
  byte tmpbuf[MSG_BUFF];


  //データの受信
  if (packetSize == MSG_BUFF) {
    udp.read(tmpbuf, MSG_BUFF);
    for (int i = 0; i < MSG_BUFF; i++)
    {
      r_upd_message_buf.bval[i] = tmpbuf[i];
    }

    //データのシリアルモニタ表示
    if (monitor_udp_resv == 1) {
      Serial.print("[UDP RESV] ");
      for (int i = 0; i < MSG_SIZE; i++)
      {
        Serial.print(String(r_upd_message_buf.sval[i]));
        Serial.print(", ");
      }
      Serial.println();
    }
  } else
  {
    if (monitor_udp_resv == 1) {
      Serial.println("none.");
    }
  }


  udp_trial ++;
  long checksum = 0;
  for (int i = 0; i < MSG_SIZE - 1; i++) {
    checksum += r_upd_message_buf.sval[i];
  }
  checksum = checksum & 0xFF ^ 0xFF;
  if (r_upd_message_buf.sval[MSG_SIZE - 1] == (short)checksum) {
    if (monitor_udp_resv_check == 1) {
      Serial.println("UDP Rvok!");
    }
    udp_ok ++;
  } else
  {
    if (monitor_udp_resv_check == 1) {
      Serial.println("UDP RvNG*");
    }
  }

  if (udp_trial % 200 == 0) { //エラー率の表示
    if (monitor_udp_resv_error == 1) {
      Serial.print("UDP error rate ");
      Serial.print(float(udp_trial-udp_ok) / float(udp_trial) * 100);
      Serial.print(" %  ");
      Serial.print(udp_trial-udp_ok);
      Serial.print("/");
      Serial.println(udp_trial);
    }
  }
}


//送信用の関数
void sendUDP() {
  String test = "";//表示用の変数

  udp.beginPacket(send_address, send_port);//UDPパケットの開始

  for (int i = 0; i < MSG_BUFF; i++) {
    udp.write(s_upd_message_buf.bval[i]);//１バイトずつ送信
    if (i % 2 == 0) {//表示用に送信データを共用体から2バイトずつ取得
      test += String(s_upd_message_buf.sval[i / 2]) + ", ";
    }
  }

  //送信データの表示
  if (monitor_src == 1) {
    Serial.println("[UDP SEND] " + test );//送信データ（short型）を表示
  }

  udp.endPacket();//UDPパケットの終了
}


void loop()
{

  //もしデータパケットが来ていれば受信する
  receiveUDP();

  //UDP受信配列からSPI送信配列にデータを転写
  for (int i = 0; i < MSG_BUFF; i++) {
    s_message_buf[i] = r_upd_message_buf.bval[i];
  }

  // SPI send data
  if (slave.remained() == 0) {
    slave.queue(r_message_buf, s_message_buf, MSG_BUFF);
  }
  delayMicroseconds(50);

  memcpy(r_message_buf_2.bval, r_message_buf, MSG_BUFF);
  //delayMicroseconds(1);

  // show send data
  while (slave.available())
  {

    // show send data
    if (monitor_spi_send == 1) {
      Serial.print("[SPI SEND] : ");
      for (uint32_t i = 0; i < MSG_SIZE; i++)
      {
        Serial.print(s_message_buf[i]);
        Serial.print(",");
      }
      Serial.println();
    }

    // show received data
    if (monitor_spi_resv == 1) {
      Serial.print("[SPI RESV] : ");
      for (size_t i = 0; i < MSG_SIZE; ++i) {
        Serial.print(r_message_buf_2.sval[i]);
        Serial.print(",");
      }
      Serial.println();
    }

    slave.pop();//end transaction??

    // checksum culc
    checksum = 0;
    for (int i = 0; i < MSG_SIZE - 1 ; i++) {
      checksum += int(r_message_buf_2.sval[i]);
    }
    checksum = (checksum ^ 0xff) & 0xff;

    // show checksum result
    if (uint8_t (checksum) == uint8_t(r_message_buf_2.sval[MSG_SIZE - 1])) {
      if (monitor_resv_checksum == 1) {
        Serial.print("CKsm OK!: "); Serial.println(uint8_t (checksum));
      }
    } else {
      if (monitor_resv_checksum == 1) {
        Serial.print("CKsm**ERR**: "); Serial.println(uint8_t (checksum));
      }
    }
  }

  //SPI受信配列からUDP送信配列にデータを転写
  for (int i = 0; i < MSG_BUFF; i++) {
    s_upd_message_buf.bval[i] = r_message_buf_2.bval[i];
  }

  sendUDP();//UDPで送信

  delayMicroseconds(100);
}
