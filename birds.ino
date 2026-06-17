#include <driver/i2s_std.h>

#define I2S_WS  17
#define I2S_SD  18
#define I2S_SCK 8
#define SAMPLE_RATE 48000
#define CLIP_SECONDS 3
#define GAIN 4

const uint8_t MAGIC[8] = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A};
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

void sendByte(uint8_t b) { Serial.write(&b, 1); }

void sendHeader(uint32_t sr, uint32_t samples) {
  for (int i = 0; i < 8; i++) sendByte(MAGIC[i]);
  for (int i = 0; i < 4; i++) sendByte((sr >> (8*i)) & 0xFF);
  for (int i = 0; i < 4; i++) sendByte((samples >> (8*i)) & 0xFF);
  Serial.flush();
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  i2s_init();
}

void loop() {
  uint32_t totalSamples = (uint32_t)SAMPLE_RATE * CLIP_SECONDS;
  sendHeader(SAMPLE_RATE, totalSamples);
  int32_t raw[256];
  int16_t out[256];
  uint32_t sent = 0;
  while (sent < totalSamples) {
    size_t br = 0;
    i2s_channel_read(rx_handle, raw, sizeof(raw), &br, portMAX_DELAY);
    int n = br / sizeof(int32_t), o = 0;
    for (int i = 0; i < n && sent < totalSamples; i++) {
      int32_t v = (raw[i] >> 16) * GAIN;
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;
      out[o++] = (int16_t)v;
      sent++;
    }
    Serial.write((uint8_t*)out, o * 2);
  }
  delay(20);
}