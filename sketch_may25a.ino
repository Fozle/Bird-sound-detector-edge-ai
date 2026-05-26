#include "driver/i2s_pdm.h"

#define I2S_CLK_PIN   GPIO_NUM_42
#define I2S_DATA_PIN  GPIO_NUM_41
#define SAMPLE_RATE   16000
#define SAMPLE_BUFFER 512

i2s_chan_handle_t rx_chan;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Mic Test v3 ===");

  // Create channel
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
                                I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num  = 8;
  chan_cfg.dma_frame_num = 256;

  esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_chan);
  Serial.printf("new_channel:  %s\n", esp_err_to_name(err));

  // PDM RX config — minimal, explicit
  i2s_pdm_rx_config_t pdm_cfg = {
    .clk_cfg = {
      .sample_rate_hz = SAMPLE_RATE,
      .clk_src        = I2S_CLK_SRC_DEFAULT,
      .mclk_multiple  = I2S_MCLK_MULTIPLE_256,
      .dn_sample_mode = I2S_PDM_DSR_8S,
    },
    .slot_cfg = {
      .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
      .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
      .slot_mode      = I2S_SLOT_MODE_MONO,
      .slot_mask      = I2S_PDM_SLOT_LEFT,
    },
    .gpio_cfg = {
      .clk = I2S_CLK_PIN,
      .din = I2S_DATA_PIN,
      .invert_flags = {
        .clk_inv = false,
      },
    },
  };

  err = i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_cfg);
  Serial.printf("init_pdm_rx:  %s\n", esp_err_to_name(err));

  err = i2s_channel_enable(rx_chan);
  Serial.printf("enable:       %s\n", esp_err_to_name(err));

  Serial.println("Ready. Speak into the mic...");
}

void loop() {
  int16_t samples[SAMPLE_BUFFER];
  size_t bytes_read = 0;

  i2s_channel_read(rx_chan, samples, sizeof(samples),
                   &bytes_read, pdMS_TO_TICKS(1000));

  int n = bytes_read / sizeof(int16_t);
  if (n == 0) {
    Serial.println("No data");
    return;
  }

  // RMS calculation
  long sum = 0;
  int16_t peak = 0;
  for (int i = 0; i < n; i++) {
    sum += (long)samples[i] * samples[i];
    if (abs(samples[i]) > peak) peak = abs(samples[i]);
  }
  int rms = (int)sqrt((double)sum / n);

  // Print first 8 raw sample values so we can see actual data
  Serial.print("RAW[ ");
  for (int i = 0; i < 8; i++) {
    Serial.print(samples[i]);
    Serial.print(" ");
  }
  Serial.print("] RMS:");
  Serial.print(rms);
  Serial.print(" PEAK:");
  Serial.print(peak);
  Serial.print(" |");
  int bars = constrain(map(rms, 0, 5000, 0, 30), 0, 30);
  for (int i = 0; i < bars; i++) Serial.print("=");
  Serial.println("|");
}