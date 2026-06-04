#include <driver/i2s.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>

// ============ YOUR CONFIGURATION ============
#define SSID "arafat"
#define PASSWORD "12121212"
#define STATION_TOKEN "BCeVpoWb6qwrV7CabmxzVA4L"

// BirdWeather API endpoint
#define BIRDWEATHER_API "https://api.birdweather.com/api/v1/audio"

// ============ I2S MICROPHONE PINS ============
#define I2S_BCLK_PIN  8
#define I2S_WS_PIN    46
#define I2S_DOUT_PIN  3

// ============ AUDIO SETTINGS ============
#define SAMPLE_RATE   16000
#define SAMPLES_PER_BUFFER 256
#define RECORDING_DURATION_SEC 5  // Record 5 seconds (reduced to fit in memory)
#define VAD_THRESHOLD 1500  // Adjust: lower = more sensitive, higher = less sensitive

// ============ AUDIO BUFFER ============
const int TOTAL_SAMPLES = SAMPLE_RATE * RECORDING_DURATION_SEC;  // 80,000 samples
int16_t *audio_buffer = NULL;  // Allocate dynamically in setup()
int buffer_index = 0;
bool is_recording = false;
unsigned long record_start_time = 0;

// ============ I2S SETUP ============
void setup_i2s() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
  };

  i2s_pin_config_t pin_config = {
    .mck_io_num   = I2S_PIN_NO_CHANGE,
    .bck_io_num   = I2S_BCLK_PIN,
    .ws_io_num    = I2S_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_DOUT_PIN
  };

  esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[I2S] Install failed: %s\n", esp_err_to_name(err));
  }

  err = i2s_set_pin(I2S_NUM_0, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("[I2S] Set pin failed: %s\n", esp_err_to_name(err));
  }

  Serial.println("[I2S] Microphone initialized");
}

// ============ WiFi SETUP ============
void setup_wifi() {
  Serial.print("[WiFi] Connecting to ");
  Serial.println(SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected!");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi] Connection failed");
  }
}

// ============ VOICE ACTIVITY DETECTION ============
int calculate_rms(int16_t *samples, int count) {
  if (count == 0) return 0;
  long sum = 0;
  for (int i = 0; i < count; i++) {
    sum += (long)samples[i] * samples[i];
  }
  return (int)sqrt(sum / count);
}

// ============ MAIN SETUP ============
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n========== BIRD SOUND DETECTOR ==========");
  Serial.println("ESP32-S3 → BirdWeather API");
  Serial.println("=========================================\n");

  // Allocate audio buffer from heap (PSRAM if available)
  audio_buffer = (int16_t *)malloc(TOTAL_SAMPLES * sizeof(int16_t));
  if (!audio_buffer) {
    Serial.println("[ERROR] Failed to allocate audio buffer!");
    while(1) delay(100);  // Hang
  }
  Serial.printf("[Memory] Audio buffer allocated: %d bytes\n", TOTAL_SAMPLES * 2);
  Serial.printf("[Memory] Free heap: %d bytes\n", ESP.getFreeHeap());

  setup_i2s();
  setup_wifi();

  Serial.println("[System] Ready. Listening for bird sounds...");
  Serial.print("[System] VAD Threshold: ");
  Serial.println(VAD_THRESHOLD);
}

// ============ MAIN LOOP ============
void loop() {
  int16_t samples[SAMPLES_PER_BUFFER];
  size_t bytes_read = 0;

  // Read audio chunk from I2S
  esp_err_t err = i2s_read(I2S_NUM_0, samples, sizeof(samples), &bytes_read, 
                           pdMS_TO_TICKS(100));

  if (err != ESP_OK || bytes_read == 0) {
    return;
  }

  int num_samples = bytes_read / sizeof(int16_t);

  // Calculate RMS (volume level)
  int rms = calculate_rms(samples, num_samples);

  // -------- VAD LOGIC --------
  if (!is_recording && rms > VAD_THRESHOLD) {
    // TRIGGER: Sound detected!
    Serial.print("[VAD] Sound triggered! RMS: ");
    Serial.print(rms);
    Serial.println(" (recording...)");
    is_recording = true;
    buffer_index = 0;
    record_start_time = millis();
  }

  // If recording, fill buffer
  if (is_recording) {
    for (int i = 0; i < num_samples; i++) {
      if (buffer_index < TOTAL_SAMPLES) {
        audio_buffer[buffer_index++] = samples[i];
      }
    }

    // Check if recording time expired
    unsigned long elapsed = millis() - record_start_time;
    if (elapsed >= (RECORDING_DURATION_SEC * 1000) || buffer_index >= TOTAL_SAMPLES) {
      // RECORDING COMPLETE
      Serial.print("\n[Recording] Complete! ");
      Serial.print(buffer_index);
      Serial.println(" samples captured");
      
      is_recording = false;

      // Send to BirdWeather
      send_to_birdweather();

      // Reset for next detection
      buffer_index = 0;
    }
  }

  // Print status every 50 frames
  static int frame_count = 0;
  frame_count++;
  if (frame_count % 50 == 0) {
    Serial.print("[Live] RMS: ");
    Serial.print(rms);
    Serial.print(" | Recording: ");
    Serial.print(is_recording ? "YES" : "NO");
    Serial.print(" | Buffer: ");
    Serial.print(buffer_index);
    Serial.print("/");
    Serial.println(TOTAL_SAMPLES);
  }
}

// ============ SEND TO BIRDWEATHER ============
void send_to_birdweather() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Upload] WiFi disconnected, retrying...");
    WiFi.reconnect();
    delay(2000);
    if (WiFi.status() != WL_CONNECTED) {
      return;
    }
  }

  Serial.println("[Upload] Sending to BirdWeather API...");

  HTTPClient http;
  http.begin(BIRDWEATHER_API);
  
  // Set headers
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("Authorization", "Bearer " + String(STATION_TOKEN));
  http.addHeader("X-Sample-Rate", String(SAMPLE_RATE));

  // Create WAV header
  uint8_t *wav_buffer = create_wav_header((uint8_t *)audio_buffer, buffer_index * 2);
  int wav_size = calculate_wav_size(buffer_index * 2);

  Serial.print("[Upload] WAV size: ");
  Serial.println(wav_size);

  // Send POST with audio data
  int httpCode = http.POST(wav_buffer, wav_size);

  Serial.print("[Upload] HTTP Code: ");
  Serial.println(httpCode);

  if (httpCode > 0) {
    String response = http.getString();
    Serial.print("[Upload] Response: ");
    Serial.println(response);
    log_detection(true, "", 0.0);
  } else {
    Serial.print("[Upload] Error: ");
    Serial.println(http.errorToString(httpCode));
    log_detection(false, "Upload failed", 0.0);
  }

  http.end();
  
  // Free WAV buffer
  free(wav_buffer);
}

// ============ WAV FILE CREATION ============
uint8_t* create_wav_header(uint8_t *pcm_data, int pcm_size) {
  int wav_size = 44 + pcm_size;  // 44 = WAV header size
  uint8_t *wav_buffer = (uint8_t *)malloc(wav_size);

  // WAV Header
  memcpy(wav_buffer + 0, "RIFF", 4);
  *(uint32_t *)(wav_buffer + 4) = wav_size - 8;
  memcpy(wav_buffer + 8, "WAVE", 4);

  // fmt subchunk
  memcpy(wav_buffer + 12, "fmt ", 4);
  *(uint32_t *)(wav_buffer + 16) = 16;
  *(uint16_t *)(wav_buffer + 20) = 1;
  *(uint16_t *)(wav_buffer + 22) = 1;
  *(uint32_t *)(wav_buffer + 24) = SAMPLE_RATE;
  *(uint32_t *)(wav_buffer + 28) = SAMPLE_RATE * 2;
  *(uint16_t *)(wav_buffer + 32) = 2;
  *(uint16_t *)(wav_buffer + 34) = 16;

  // data subchunk
  memcpy(wav_buffer + 36, "data", 4);
  *(uint32_t *)(wav_buffer + 40) = pcm_size;

  // Copy PCM data
  memcpy(wav_buffer + 44, pcm_data, pcm_size);

  return wav_buffer;
}

int calculate_wav_size(int pcm_size) {
  return 44 + pcm_size;
}

// ============ LOGGING ============
void log_detection(bool success, String species, double confidence) {
  Serial.print("[Log] Detection - Success: ");
  Serial.print(success ? "YES" : "NO");
  Serial.print(" | Species: ");
  Serial.print(species.length() > 0 ? species : "Unknown");
  Serial.print(" | Confidence: ");
  Serial.println(confidence);
}
