#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"

#include <Wire.h>
#include <SPI.h>
#include <Adafruit_Sensor.h>
#include "Adafruit_BME680.h"


// Setze PINs für den BME680
#define BME_SCK 14
#define BME_MISO 15
#define BME_MOSI 21
#define BME_CS 20

#define SEALEVELPRESSURE_HPA (1013.25)

// Initialisierung des BME 680
Adafruit_BME680 bme(BME_CS, BME_MOSI, BME_MISO, BME_SCK);

// Standard Endgeräte config
#define ESP_ZB_ZED_CONFIG()                                                                 \
  {                                                                                         \
    .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED, .install_code_policy = INSTALLCODE_POLICY_ENABLE, \
    .nwk_cfg = {                                                                            \
      .zed_cfg =                                                                            \
        {                                                                                   \
          .ed_timeout = ED_AGING_TIMEOUT,                                                   \
          .keep_alive = ED_KEEP_ALIVE,                                                      \
        },                                                                                  \
    },                                                                                      \
  }

// Zigbee Radio Konfiguration
#define ESP_ZB_DEFAULT_RADIO_CONFIG() \
  { .radio_mode = ZB_RADIO_MODE_NATIVE, }

// Zigbee Host Konfiguration
#define ESP_ZB_DEFAULT_HOST_CONFIG() \
  { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE, }

// Zigbee Endgeräte Konfiguration
#define INSTALLCODE_POLICY_ENABLE   false
#define ED_AGING_TIMEOUT            ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ED_KEEP_ALIVE               3000                                 /* 3000 millisecond */
#define HA_ESP_SENSOR_ENDPOINT      10                                   /* temperature sensor endpoint */
#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK /* Zigbee primary channel mask */

/* Temperature sensor configuration */
#define TEMP_SENSOR_UPDATE_INTERVAL (1)  /* Local sensor update interval (second) */
#define TEMP_SENSOR_MIN_VALUE       (0) /* Local sensor min measured value (degree Celsius) */
#define TEMP_SENSOR_MAX_VALUE       (70) /* Local sensor max measured value (degree Celsius) */

/********************* Zigbee functions **************************/
// Rechnet die Kommawerte vom Typ float, die der ESP zurückgibt in ein int16_t um
static int16_t zb_temperature_to_s16(float temp) {
  return (int16_t)(temp * 100);
}

static void esp_app_temp_sensor_handler(int16_t temperature) {
  /* Update temperature sensor measured value */
  esp_zb_lock_acquire(portMAX_DELAY);           // Sperrt den Zigbee Stack für Änderungen
  esp_zb_zcl_set_attribute_val(
    HA_ESP_SENSOR_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, &temperature,
    false
  );                                            // Aktualisieren der Werte
  esp_zb_lock_release();                        // Freigeben des Zigbee Stacks
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
  uint32_t *p_sg_p = signal_struct->p_app_signal;
  esp_err_t err_status = signal_struct->esp_err_status;
  esp_zb_app_signal_type_t sig_type = (esp_zb_app_signal_type_t)*p_sg_p;
  switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
      log_i("Zigbee stack initialized");
      esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
      break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
      if (err_status == ESP_OK) {
        log_i("Start network steering");
        xTaskCreate(temp_sensor_value_update, "temp_sensor_update", 2048, NULL, 10, NULL);
        if (esp_zb_bdb_is_factory_new()) {
          esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        }
      } else {
        log_w("Failed to initialize Zigbee stack (status: %s)", esp_err_to_name(err_status));
      }
      break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
      if (err_status == ESP_OK) {
        log_i("Joined network successfully");
      } else {
        log_i("Network steering was not successful (status: %s)", esp_err_to_name(err_status));
        esp_zb_scheduler_alarm((esp_zb_callback_t)esp_zb_bdb_start_top_level_commissioning, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
      }
      break;
    default: log_i("ZDO signal: %s, status: %s", esp_zb_zdo_signal_to_string(sig_type), esp_err_to_name(err_status)); break;
  }
}

static esp_zb_cluster_list_t *custom_temperature_sensor_clusters_create(esp_zb_temperature_sensor_cfg_t *temperature_sensor) {
  esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
  ESP_ERROR_CHECK(esp_zb_cluster_list_add_temperature_meas_cluster(
    cluster_list, esp_zb_temperature_meas_cluster_create(&(temperature_sensor->temp_meas_cfg)), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE
  ));
  return cluster_list;
}

static esp_zb_ep_list_t *custom_temperature_sensor_ep_create(uint8_t endpoint_id, esp_zb_temperature_sensor_cfg_t *temperature_sensor) {
  esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
  esp_zb_endpoint_config_t endpoint_config = {
    .endpoint = endpoint_id, .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID, .app_device_id = ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID, .app_device_version = 0
  };
  esp_zb_ep_list_add_ep(ep_list, custom_temperature_sensor_clusters_create(temperature_sensor), endpoint_config);
  return ep_list;
}


static void esp_zb_task(void *pvParameters) {
  esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
  esp_zb_init(&zb_nwk_cfg);
  esp_zb_temperature_sensor_cfg_t sensor_cfg = ESP_ZB_DEFAULT_TEMPERATURE_SENSOR_CONFIG();
  sensor_cfg.temp_meas_cfg.min_value = zb_temperature_to_s16(TEMP_SENSOR_MIN_VALUE);
  sensor_cfg.temp_meas_cfg.max_value = zb_temperature_to_s16(TEMP_SENSOR_MAX_VALUE);
  esp_zb_ep_list_t *esp_zb_sensor_ep = custom_temperature_sensor_ep_create(HA_ESP_SENSOR_ENDPOINT, &sensor_cfg);
  esp_zb_device_register(esp_zb_sensor_ep);

  // Konfiguration, wann der Wert geupdatet werden soll
  esp_zb_zcl_reporting_info_t reporting_info = {
    .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
    .ep = HA_ESP_SENSOR_ENDPOINT,
    .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
    .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
    .attr_id = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
    .u =
      {
        .send_info =
          {
            .min_interval = 1,
            .max_interval = 0,
            .delta =
              {
                .u16 = 0,
              },
            .def_min_interval = 1,
            .def_max_interval = 0,
          },
      },
    .dst =
      {
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
      },
    .manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
  };
  esp_zb_zcl_update_reporting_info(&reporting_info);        
  esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);    // Setze den primäeren Funkkanal
  ESP_ERROR_CHECK(esp_zb_start(false));
  esp_zb_main_loop_iteration();
}

uint16_t getPressure() {

  return (uint16_t)(bme.pressure / 100);

}




/************************ Temp sensor *****************************/
// Updatet den Temperaturwert jede Sekunde
static void temp_sensor_value_update(void *arg) {
  for (;;) {
    esp_app_temp_sensor_handler(getData());
    delay(1000);  // Send the temperature every second
  }
}

uint16_t zaehlen = 0;
uint16_t zaehlen2 = 0;

// Gibt den Wert vom Sensor als int16_t zurück
static int16_t getData(){

  if((zaehlen % 2 == 0) && (zaehlen2 % 2 == 0)){

    zaehlen++;
    Serial.print("Temperatur: ");
    Serial.println(bme.temperature + 4);
    delay(1000);
    return (zb_temperature_to_s16(bme.temperature) - 400);

  }

  else if ((zaehlen % 2 != 0) && (zaehlen2 % 2 == 0)){

    zaehlen2++;
    Serial.print("Luftfeuchte: ");
    Serial.println(bme.humidity + 3);
    delay(1000);
    return (zb_temperature_to_s16(bme.humidity) + 300);

  }

  else if ((zaehlen % 2 != 0) && (zaehlen2 % 2 != 0)){
    zaehlen++;
    Serial.print("Hoehe: ");
    Serial.println(bme.readAltitude(SEALEVELPRESSURE_HPA) + 100);
    delay(1000);
    return (bme.readAltitude(SEALEVELPRESSURE_HPA) + 100);

  }

  else if ((zaehlen % 2 == 0) && (zaehlen2 % 2 != 0)){
    zaehlen2++;
    Serial.print("Luftdruck: ");
    Serial.println(getPressure() + 51);
    delay(1000);
    return(getPressure() + 51);


  }

return 0;

}



/********************* Arduino functions **************************/
void setup() {
  Serial.begin(115200);   // Initialisierung der Seriellen Verbindung

  // Testen, ob Sensor da
  if (!bme.begin()) {
    Serial.println("Could not find a valid BME680 sensor, check wiring!");
    while (1);
  }

  // Oversampling für den BME setzen
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150); // 320*C for 150 ms

  // Zigbee initialisieren
  esp_zb_platform_config_t config = {
    .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
    .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
  };
  ESP_ERROR_CHECK(esp_zb_platform_config(&config));


  // Zigbee task starten
  xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}


void loop() {

    // Testen und lesen
    if (!bme.performReading()) {
        Serial.println("Failed to perform reading :(");
        return;
    }

    // Temperaturwert an den ZigBee-Koordinator übermitteln
    esp_app_temp_sensor_handler(getData());

    // 1 Sekunde warten
    delay(1000);
}
