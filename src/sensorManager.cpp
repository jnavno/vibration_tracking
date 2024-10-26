#include "sensorManager.h"
#include <Wire.h>
#include "MPU6050.h"
#include <variant.h>
#include <powerManager.h>
#include <spiffsManager.h>
#include "SPIFFS.h"
#include "FFT.h"  // FFT library
#include "FFT_signal.h"  // FFT input/output buffer

MPU6050 mpu;
volatile bool wakeup_flag = false;
float inputBuffer[MAX_SAMPLES];
int remainingCycles = CYCLES_FOR_5_MIN;

// Declare variables for FFT thresholds and frequencies
float max_axe_magnitude = 0;
float max_saw_magnitude = 0;
float max_chainsaw_magnitude = 0;

#define SAMPLING_FREQUENCY 5  // Sampling frequency (adjust as needed)
#define AXE_MIN_FREQ 20.0
#define AXE_MAX_FREQ 60.0
#define SAW_MIN_FREQ 5.0
#define SAW_MAX_FREQ 30.0
#define CHAINSAW_MIN_FREQ 50.0
#define CHAINSAW_MAX_FREQ 250.0
#define SOME_THRESHOLD 0.3

// Declare the performFFT function
void performFFT();

bool setupSensors() {
    powerCycleMPU(true);  // Power on the MPU via Vext
    delay(2000);          // Increased delay to ensure full power-up

    Wire.begin(38, 1);   // Initialize I2C for MPU6050
    Wire.setClock(100000); // Set I2C clock speed for stability

    if (!initializeMPU()) {
        Serial.println("MPU6050 initialization failed in setupSensors.");
        return false;  // Return false if initialization fails
    }

    Serial.println("MPU6050 successfully initialized.");
    return true;  // Return true if initialization succeeds
}


bool initializeMPU() {
    mpu.initialize();
    delay(100);  // Small delay for MPU stability check
    if (!mpu.testConnection()) {
        Serial.println("MPU6050 connection failed.");
        return false;
    }
    Serial.println("MPU6050 connected");

    // Configure the FIFO for accelerometer data
    mpu.setFIFOEnabled(false);
    mpu.resetFIFO();
    mpu.setAccelFIFOEnabled(true);
    mpu.setFIFOEnabled(true);
    mpu.setRate(99);                                // ~5Hz sampling rate
    mpu.setFullScaleAccelRange(MPU6050_ACCEL_FS_2); // ±2g range
    delay(1000);                                    // Wait for MPU to stabilize
    return true;
}

void monitorSensors() {
    if (remainingCycles <= 0) {
        quickBlinkAndHalt();  // Stop the system if out of cycles
        return;
    }

    for (int phase = 1; phase <= CYCLES_FOR_5_MIN; phase++) {
        Serial.printf("Recording phase %d...\n", phase);
        Serial.printf("%d remaining reading cycles\n", remainingCycles);

        bool phaseCompleted = false;
        int retryCount = 0;

        while (!phaseCompleted && retryCount < 3) {
            toggleSensorPower(true);  // Ensure MPU is powered on
            delay(1000);              // Allow time to stabilize power
            if (!initializeMPU()) {
                Serial.println("MPU6050 initialization failed.");
                retryCount++;
                continue;
            }

            readAccelerometerDataForPhase(phase);

            bool loggingSuccess = logDataToSPIFFS(inputBuffer, MAX_SAMPLES, phase);
            if (loggingSuccess) {
                phaseCompleted = true;
                toggleSensorPower(false); // Power off MPU
                performFFT();  // Run FFT analysis on logged data
            } else {
                Serial.println("Failed to log data to SPIFFS. Retrying...");
                SPIFFS.end();
                if (!SPIFFS.begin(true)) {
                    Serial.println("SPIFFS remount failed. Skipping phase...");
                    break;
                }
            }
        }
        remainingCycles--;
        delay(5000);  // Wait before next phase
    }
}

void readAccelerometerDataForPhase(int phase) {
    uint8_t fifoBuffer[BLOCK_SIZE];
    int samplesRead = 0;
    unsigned long startMillis = millis();

    while (millis() - startMillis < PHASE_DURATION) {
        uint16_t fifoCount = mpu.getFIFOCount();

        if (fifoCount >= BLOCK_SIZE) {
            mpu.getFIFOBytes(fifoBuffer, BLOCK_SIZE);
            mpu.resetFIFO();

            for (int i = 0; i <= BLOCK_SIZE - 6 && samplesRead < MAX_SAMPLES; i += 6) {
                int16_t accelX = (fifoBuffer[i] << 8) | fifoBuffer[i + 1];
                inputBuffer[samplesRead++] = accelX / 16384.0; // Convert to g-force
            }
        } else if (fifoCount == 0) {
            delay(100);  // Wait for FIFO to fill
        }

        delay(1000 / SAMPLE_RATE);  // Control reading rate
    }
}

void performFFT() {
    fft_config_t *real_fft_plan = fft_init(FFT_N, FFT_REAL, FFT_FORWARD, fft_input, fft_output);

    for (int i = 0; i < SAMPLES; i++) {
        real_fft_plan->input[i] = (i < MAX_SAMPLES) ? inputBuffer[i] : 0;
    }

    fft_execute(real_fft_plan);

    float max_axe_magnitude = 0;
    float max_saw_magnitude = 0;
    float max_chainsaw_magnitude = 0;

    Serial.println("FFT Results:");
    for (int i = 1; i < (SAMPLES / 2); i++) {
        float frequency = (i * SAMPLING_FREQUENCY) / SAMPLES;
        float magnitude = sqrt(pow(real_fft_plan->output[2 * i], 2) + pow(real_fft_plan->output[2 * i + 1], 2));

        Serial.printf("Frequency: %f Hz, Magnitude: %f\n", frequency, magnitude);

        if (frequency >= AXE_MIN_FREQ && frequency <= AXE_MAX_FREQ) {
            if (magnitude > max_axe_magnitude) max_axe_magnitude = magnitude;
        } else if (frequency >= SAW_MIN_FREQ && frequency <= SAW_MAX_FREQ) {
            if (magnitude > max_saw_magnitude) max_saw_magnitude = magnitude;
        } else if (frequency >= CHAINSAW_MIN_FREQ && frequency <= CHAINSAW_MAX_FREQ) {
            if (magnitude > max_chainsaw_magnitude) max_chainsaw_magnitude = magnitude;
        }
    }

    fft_destroy(real_fft_plan);

    if (max_chainsaw_magnitude > SOME_THRESHOLD) {
        Serial.println("Chainsaw cutting detected!");
    } else if (max_axe_magnitude > SOME_THRESHOLD) {
        Serial.println("Hand axe/hatchet cutting detected!");
    } else if (max_saw_magnitude > SOME_THRESHOLD) {
        Serial.println("Handsaw cutting detected!");
    } else {
        Serial.println("No significant cutting activity detected.");
    }
}

bool checkFIFOOverflow() {
    if (mpu.getIntFIFOBufferOverflowStatus()) {
        Serial.println("FIFO overflow detected!");
        mpu.resetFIFO();
        return true;
    }
    return false;
}

void powerCycleMPU(bool on) {
    toggleSensorPower(on);  // Control MPU power
    delay(on ? PRE_TOGGLE_DELAY : 3000);
}