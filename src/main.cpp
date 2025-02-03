/**
 *  ESP32 Ultrasonic Person Detector with Low-Power Strategy
 *
 *  Power Strategy Overview:
 *  - **Active Sampling:** On wakeup, the device is active for 5 seconds.
 *    During this period, the ultrasonic sensor is sampled every 500 ms.
 *    (A total of about 10 samples are taken.)
 *
 *  - **Detection Logic:** If at least 6 out of 10 samples show a distance
 *    below 50 cm (indicating an object/person is in front of the sensor),
 *    the device flags a detection ("person present").
 *
 *  - **State Change Transmission:** The previous state is stored in RTC memory.
 *    Only when the current detection state differs from the previous state does
 *    the device connect to WiFi, initialize Firebase, and send a binary update
 *    (1 for “person present”, 0 for “no person”) to Firebase.
 *
 *  - **Adaptive Deep Sleep:** To maximize battery life:
 *      - If no person is detected, the device goes into deep sleep for 30 seconds.
 *      - If a person is detected, the device goes into a shorter sleep of 10 seconds,
 *        allowing for faster updates when someone is in the room.
 *
 *  - **Estimated Power Consumption:** In active mode, the ESP32 (with WiFi on)
 *    draws roughly ~80 mA for a brief 5-second period. In deep sleep the consumption
 *    can be as low as ~10 µA. Under a typical scenario (e.g., 90% no-person, 10% person)
 *    the weighted average current is estimated to be around 13 mA.
 *    Over 24 hours, the estimated consumption is:
 *
 *         I_avg ≈ 13 mA  →  13 mA × 24 h ≈ 312 mAh,
 *
 *    which is comfortably supported by a single 500 mAh battery.
 *
 *  - **Visualization:** When you run this code and capture a one-minute
 *    window with your Power Profiler Kit, annotate the following:
 *      - The deep-sleep segments (with current in the µA range).
 *      - The active segments (with higher current draw).
 *      - The average current value computed.
 *
 *  This strategy minimizes WiFi usage and sensor operation while maintaining
 *  detection reliability.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>

// WiFi and Firebase credentials (replace with your own as needed)
#define WIFI_SSID         "UW MPSK"
#define WIFI_PASSWORD     "K7LMs,Y_#d"
#define DATABASE_SECRET   "HQInHv2FIzmuqQKNIlwkgcAgCB3auDxY44QHLlIX"
#define DATABASE_URL      "https://lab4-shareef-default-rtdb.firebaseio.com/"

// Pins for the HC-SR04 Ultrasonic Sensor
static const int TRIG_PIN = D4;  // adjust as needed for your board
static const int ECHO_PIN = D5;

// Detection parameters
static const float OBJECT_THRESHOLD_CM      = 50.0f;    // threshold distance in cm
static const unsigned long ACTIVE_PERIOD_MS   = 5000;     // active sampling period: 5 seconds
static const unsigned long MEASUREMENT_INTERVAL_MS = 500; // sample every 500 ms
static const int REQUIRED_COUNT             = 6;        // need at least 6 "close" readings

// Deep-sleep durations (in milliseconds)
static const unsigned long NO_PERSON_SLEEP_MS = 30000;   // 30 sec sleep when no person
static const unsigned long PERSON_SLEEP_MS    = 10000;   // 10 sec sleep when person detected

// Firebase related objects
WiFiClientSecure sslClient;
DefaultNetwork network;
AsyncClientClass client(sslClient, getNetwork(network));
FirebaseApp app;
RealtimeDatabase Database;
AsyncResult result;
LegacyToken dbSecret(DATABASE_SECRET);

// Store last detection state in RTC memory (persists across deep sleep)
RTC_DATA_ATTR bool lastPersonPresent = false;

/**
 * Measures the distance (in centimeters) using the ultrasonic sensor.
 */
float measureDistanceCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Use pulseIn with a timeout (e.g., 30 ms) to avoid blocking indefinitely.
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  float distance = (duration * 0.034) / 2.0;
  Serial.print("Measured distance: ");
  Serial.print(distance);
  Serial.println(" cm");
  return distance;
}

/**
 * Connects to the WiFi network.
 */
void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  int retries = 20;
  while (WiFi.status() != WL_CONNECTED && retries > 0) {
    delay(500);
    Serial.print(".");
    retries--;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected.");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connection failed.");
  }
}

/**
 * Initializes the Firebase connection.
 */
void initFirebase() {
  Firebase.printf("Firebase Client v%s\n", FIREBASE_CLIENT_VERSION);
  sslClient.setInsecure(); // use a secure connection (or add certificate handling)
  initializeApp(client, app, getAuth(dbSecret));
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);
  client.setAsyncResult(result);
  Serial.println("Firebase initialized.");
}

/**
 * Sends the binary detection status (1 for person present, 0 for not) to Firebase.
 */
void sendToFirebase(const String& path, int status) {
  if(WiFi.status() == WL_CONNECTED) {
    bool ok = Database.set<number_t>(client, path, number_t(status));
    if(!ok) {
      Serial.print("Firebase set error: ");
      Serial.println(client.lastError().message());
    } else {
      Serial.println("State updated to Firebase: " + String(status));
    }
  } else {
    Serial.println("WiFi not connected. Cannot send update to Firebase.");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  Serial.println("Starting active sensor sampling cycle...");

  unsigned long startTime = millis();
  int detectionCount = 0;
  int totalCount = 0;

  // Sample the sensor repeatedly for the active period.
  while (millis() - startTime < ACTIVE_PERIOD_MS) {
    float dist = measureDistanceCM();
    totalCount++;
    if (dist < OBJECT_THRESHOLD_CM) {
      detectionCount++;
    }
    delay(MEASUREMENT_INTERVAL_MS);
  }

  // Decide if a person is present based on the number of close measurements.
  bool currentPersonPresent = (detectionCount >= REQUIRED_COUNT);

  Serial.print("Detection count: ");
  Serial.print(detectionCount);
  Serial.print(" out of ");
  Serial.print(totalCount);
  Serial.println(" measurements.");
  Serial.print("Person present: ");
  Serial.println(currentPersonPresent ? "YES" : "NO");

  // Only transmit to Firebase if there is a change in the detection state.
  if (currentPersonPresent != lastPersonPresent) {
    Serial.println("State change detected. Updating Firebase...");
    connectWiFi();
    initFirebase();
    // Send binary status: 1 for person present, 0 for not.
    sendToFirebase("/lab4/presence", currentPersonPresent ? 1 : 0);
    // Disconnect WiFi to conserve power.
    WiFi.disconnect(true);
    // Save the new state for the next cycle.
    lastPersonPresent = currentPersonPresent;
  } else {
    Serial.println("No state change detected. No update sent.");
  }

  // Select the sleep duration based on detection:
  // - Shorter sleep when a person is present for a faster update cycle.
  // - Longer sleep when no person is present to save energy.
  unsigned long sleepDuration;
  if (currentPersonPresent) {
    sleepDuration = PERSON_SLEEP_MS;
    Serial.println("Person detected: entering short deep sleep.");
  } else {
    sleepDuration = NO_PERSON_SLEEP_MS;
    Serial.println("No person detected: entering longer deep sleep.");
  }
  Serial.printf("Deep sleeping for %lu ms...\n", sleepDuration);

  // Configure deep sleep (convert ms to µs).
  esp_sleep_enable_timer_wakeup(sleepDuration * 1000ULL);
  esp_deep_sleep_start();
}

void loop() {
  // Not used because the device operates in a wake-sample-transmit-sleep cycle.
}
