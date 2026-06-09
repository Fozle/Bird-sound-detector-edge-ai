#include <WiFi.h>
#include <driver/i2s_std.h>

// ---- EDIT THESE ----
const char* WIFI_SSID = "HammadHome";        // must be 2.4GHz (the S3 can't do 5GHz)
const char* WIFI_PASS = "06870711";
const char* PI_HOST   = "192.168.0.107";    // the IP your receiver printed
const int   PI_PORT   = 5000;
// --------------------

#define TEST_MODE   false
#define I2S_WS  17
#define I2S_SD  18
#define I2S_SCK 8
#define SAMPLE_RATE 48000
#define GAIN        4

const int CLIP_SECONDS = TEST_MODE ? 5 : 3;
i2s_chan_handle_t rx_handle;

void i2s_init() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_new_channel(&chan_cfg, NULL, &rx_handle);
  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = (gpio_num_t)I2S_SCK,
                  .ws = (gpio_num_t)I2S_WS, .dout = I2S_GPIO_UNUSED, .din = (gpio_num_t)I2S_SD,
                  .invert_flags = { false, false, false } },
  };
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  i2s_channel_init_std_mode(rx_handle, &std_cfg);
  i2s_channel_enable(rx_handle);
  int32_t junk[256]; size_t br;
  for (int i = 0; i < 10; i++) i2s_channel_read(rx_handle, junk, sizeof(junk), &br, 100);
}

void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.printf(" connected: %s\n", WiFi.localIP().toString().c_str());
}

void w32(WiFiClient& c, uint32_t v){ uint8_t b[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)}; c.write(b,4);}
void w16(WiFiClient& c, uint16_t v){ uint8_t b[2]={(uint8_t)v,(uint8_t)(v>>8)}; c.write(b,2);}

void wavHeader(WiFiClient& c, uint32_t sr, uint32_t dataBytes){
  c.write((const uint8_t*)"RIFF",4); w32(c,36+dataBytes); c.write((const uint8_t*)"WAVE",4);
  c.write((const uint8_t*)"fmt ",4); w32(c,16); w16(c,1); w16(c,1);
  w32(c,sr); w32(c,sr*2); w16(c,2); w16(c,16);
  c.write((const uint8_t*)"data",4); w32(c,dataBytes);
}

void sendClip(){
  if (WiFi.status() != WL_CONNECTED) wifiConnect();
  WiFiClient client;
  if (!client.connect(PI_HOST, PI_PORT)) { Serial.println("connect FAILED"); delay(2000); return; }

  uint32_t totalSamples = (uint32_t)SAMPLE_RATE * CLIP_SECONDS;
  uint32_t dataBytes    = totalSamples * 2;
  uint32_t wavBytes     = 44 + dataBytes;

  client.printf("POST /upload HTTP/1.1\r\nHost: %s\r\n", PI_HOST);
  client.printf("Content-Type: audio/wav\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", wavBytes);
  wavHeader(client, SAMPLE_RATE, dataBytes);

  int32_t raw[256]; int16_t out[256];
  uint32_t sent = 0;
  Serial.printf("Streaming %ds clip...\n", CLIP_SECONDS);
  while (sent < totalSamples) {
    size_t br = 0;
    i2s_channel_read(rx_handle, raw, sizeof(raw), &br, portMAX_DELAY);
    int n = br / sizeof(int32_t), o = 0;
    for (int i = 0; i < n && sent < totalSamples; i++) {
      int32_t v = (raw[i] >> 16) * GAIN;
      if (v >  32767) v =  32767;
      if (v < -32768) v = -32768;
      out[o++] = (int16_t)v; sent++;
    }
    client.write((uint8_t*)out, o * 2);
  }
  unsigned long t = millis();
  while (client.connected() && millis()-t < 1500) { while(client.available()) client.read(); }
  client.stop();
  Serial.println("Clip sent.");
}

void setup(){
  Serial.begin(115200); delay(1000);
  i2s_init(); wifiConnect();
}

void loop(){
  sendClip();
  if (TEST_MODE) { Serial.println("TEST_MODE done."); while(true) delay(1000); }
  delay(200);
}
