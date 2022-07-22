// Created by Clemens Elflein on 3/07/22.
// Copyright (c) 2022 Clemens Elflein. All rights reserved.
//
// This work is licensed under a Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License.
//
// Feel free to use the design in your private/educational projects, but don't try to sell the design or products based on it without getting my consent first.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
//
#include <NeoPixelConnect.h>
#include <Arduino.h>
#include <FastCRC.h>
#include <PacketSerial.h>
#include "datatypes.h"
#include "pins.h"
#include "ui_datatypes.h"
#include "imu.h"
#ifdef ENABLE_SOUND_MODULE
#include <DFPlayerMini_Fast.h>
#include <soundsystem.h>
#endif

#define IMU_CYCLETIME 20          // cycletime for refresh IMU data
#define STATUS_CYCLETIME 100      // cycletime for refresh analog and digital Statusvalues
#define UI_SET_LED_CYCLETIME 1000 // cycletime for refresh UI status LEDs

#define LIFT_EMERGENCY_MILLIS 500  // Time for wheels to be lifted in order to count as emergency. This is to filter uneven ground.
#define BUTTON_EMERGENCY_MILLIS 20 // Time for button emergency to activate. This is to debounce the button if triggered on bumpy surfaces

#ifdef USB_SERIAL
#define PACKET_SERIAL Serial
#else
#define PACKET_SERIAL Serial1
#endif

SerialPIO uiSerial(PIN_UI_TX, PIN_UI_RX, 250);

#define UI1_SERIAL uiSerial

#define ANZ_SOUND_SD_FILES 3

// Millis after charging is retried
#define CHARGING_RETRY_MILLIS 10000

/**
 * @brief Some hardware parameters
 */
#define VIN_R1 10000.0f
#define VIN_R2 1000.0f
#define R_SHUNT 0.003f
#define CURRENT_SENSE_GAIN 100.0f

#define BATT_ABS_MAX 28.7f
#define BATT_ABS_Min 21.7f

#define BATT_FULL BATT_ABS_MAX - 0.3f
#define BATT_EMPTY BATT_ABS_Min + 0.3f

// Emergency will be engaged, if no heartbeat was received in this time frame.
#define HEARTBEAT_MILLIS 500

NeoPixelConnect p(PIN_NEOPIXEL, 1, pio1, 0); // use state machine 1, sm 0 is used by hardwareserial class
uint8_t led_blink_counter = 0;

PacketSerial packetSerial; // COBS communication PICO <> Raspi
PacketSerial UISerial;     // COBS communication PICO UI-Board
FastCRC16 CRC16;

#ifdef ENABLE_SOUND_MODULE
MP3Sound my_sound; // Soundsystem
#endif

unsigned long last_imu_millis = 0;
unsigned long last_status_update_millis = 0;
unsigned long last_heartbeat_millis = 0;
unsigned long last_UILED_millis = 0;

unsigned long lift_emergency_started = 0;
unsigned long button_emergency_started = 0;

// Predefined message buffers, so that we don't need to allocate new ones later.
struct ll_imu imu_message = {0};
struct ll_imu_quaternion imu_quat_message = {0};
struct ll_status status_message = {0};
struct ui_command uiCommandStruct = {0};
// A mutex which is used by core1 each time status_message is modified.
// We can lock it during message transmission to prevent core1 to modify data in this time.
auto_init_mutex(mtx_status_message);

bool emergency_latch = true;
bool sound_available = false;
bool charging_allowed = false;
bool ROS_running = false;
unsigned long charging_disabled_time = 0;

float imu_temp[9];

void sendMessage(void *message, size_t size);
void sendUIMessage(void *message, size_t size);
void onPacketReceived(const uint8_t *buffer, size_t size);
void onUIPacketReceived(const uint8_t *buffer, size_t size);

void setRaspiPower(bool power)
{
  // Update status bits in the status message
  status_message.status_bitmask = (status_message.status_bitmask & 0b11111101) | ((power & 0b1) << 1);
  digitalWrite(PIN_RASPI_POWER, power);
}

void updateEmergency()
{

  if (millis() - last_heartbeat_millis > HEARTBEAT_MILLIS)
  {
    emergency_latch = true;
    ROS_running = false;
  }
  uint8_t last_emergency = status_message.emergency_bitmask & 1;

  // Mask the emergency bits. 2x Lift sensor, 2x Emergency Button
  uint8_t pin_states = gpio_get_all() & (0b11001100);
  uint8_t emergency_state = 0;

  bool is_lifted = (~pin_states & 0b00001100) != 0;

#ifndef ELFLEIN_PROTOTYPE
  bool stop_pressed = (~pin_states & 0b11000000) != 0;
#else
  // mower without mangets at emergency button, only GPIO 7 is used
  bool stop_pressed = (pin_states & 0b01000000) != 0;
#endif

  if (is_lifted)
  {
    // We just lifted, store the timestamp
    if (lift_emergency_started == 0)
    {
      lift_emergency_started = millis();
    }
  }
  else
  {
    // Not lifted, reset the time
    lift_emergency_started = 0;
  }

  if (stop_pressed)
  {
    // We just pressed, store the timestamp
    if (button_emergency_started == 0)
    {
      button_emergency_started = millis();
    }
  }
  else
  {
    // Not pressed, reset the time
    button_emergency_started = 0;
  }

  if (lift_emergency_started > 0 && (millis() - lift_emergency_started) >= LIFT_EMERGENCY_MILLIS)
  {
    // Emergency bit 2 (lift wheel 1)set?
    if (~pin_states & 0b00000100)
      emergency_state |= 0b01000;
    // Emergency bit 1 (lift wheel 2)set?
    if (~pin_states & 0b00001000)
      emergency_state |= 0b10000;
  }

  if (button_emergency_started > 0 && (millis() - button_emergency_started) >= BUTTON_EMERGENCY_MILLIS)
  {
    // Emergency bit 2 (stop button) set?
#ifndef ELFLEIN_PROTOTYPE
    if (~pin_states & 0b01000000)
      emergency_state |= 0b00010;
    // Emergency bit 1 (stop button)set?
    if (~pin_states & 0b10000000)
      emergency_state |= 0b00100;
#else
    if (pin_states & 0b01000000)
      emergency_state |= 0b00100;
#endif
  }

  if (emergency_state || emergency_latch)
  {
    emergency_latch |= 1;
    emergency_state |= 1;
  }

  status_message.emergency_bitmask = emergency_state;

  // If it's a new emergency, instantly send the message. This is to not spam the channel during emergencies.
  if (last_emergency != (emergency_state & 1))
  {
    sendMessage(&status_message, sizeof(struct ll_status));

    // Show Info mower lifted or stop button pressed
    if (status_message.emergency_bitmask & 0b00110)
      uiCommandStruct.cmd2 = LED_blink_fast;
    else if (status_message.emergency_bitmask & 0b11000)
      uiCommandStruct.cmd2 = LED_blink_slow;
    else
      uiCommandStruct.cmd2 = LED_off;

    uiCommandStruct.type = Set_LED;
    uiCommandStruct.cmd1 = MOWER_LIFTED;
    sendUIMessage(&uiCommandStruct, sizeof(struct ui_command));
  }
}

// deals with the pyhsical information an control the UI-LEDs und buzzer in depency of voltage und current values
void manageUILEDS()
{
  struct ui_command uiCommandStruct = {0};

  // Show Info Docking LED
  if ((status_message.charging_current > 0.80f) && (status_message.v_charge > 20.0f))
    uiCommandStruct.cmd2 = LED_blink_fast;
  else if ((status_message.charging_current <= 0.80f) && (status_message.charging_current >= 0.15f) && (status_message.v_charge > 20.0f))
    uiCommandStruct.cmd2 = LED_blink_slow;
  else if ((status_message.charging_current < 0.15f) && (status_message.v_charge > 20.0f))
    uiCommandStruct.cmd2 = LED_on;
  else
    uiCommandStruct.cmd2 = LED_off;
  uiCommandStruct.type = Set_LED;
  uiCommandStruct.cmd1 = CHARGING;
  sendUIMessage(&uiCommandStruct, sizeof(struct ui_command));

  // Show Info Battery state
  if (status_message.v_battery >= (BATT_FULL - 1.5f))
    uiCommandStruct.cmd2 = LED_off;
  else if (status_message.v_battery <= (BATT_EMPTY + 1.8f))
    uiCommandStruct.cmd2 = LED_blink_fast;
  else
    uiCommandStruct.cmd2 = LED_blink_slow;
  uiCommandStruct.type = Set_LED;
  uiCommandStruct.cmd1 = BATTERY_LOW;
  sendUIMessage(&uiCommandStruct, sizeof(struct ui_command));

  // Show percent value accupower, but only, if mower is not docked
  uiCommandStruct.type = Set_LED;
  uiCommandStruct.cmd1 = LED_BAR1;
  if (status_message.v_charge < 10.0f) // activate only when undocked
  {
    // use the first LED row as bargraph

    uiCommandStruct.cmd2 = LED_on;
    uiCommandStruct.cmd3 = status_message.batt_percentage;
    sendUIMessage(&uiCommandStruct, sizeof(struct ui_command));
  }
  else
  {
    uiCommandStruct.cmd2 = LED_off;
    uiCommandStruct.cmd3 = 15;
    sendUIMessage(&uiCommandStruct, sizeof(struct ui_command));
    uiCommandStruct.cmd1 = LED18;
    sendUIMessage(&uiCommandStruct, sizeof(struct ui_command));
  }

  // Show Info mower lifted or stop button pressed
  uiCommandStruct.type = Set_LED;
  uiCommandStruct.cmd1 = MOWER_LIFTED;

  if (status_message.emergency_bitmask & 0b00110)
  {
    uiCommandStruct.cmd2 = LED_blink_fast;
    sendUIMessage(&uiCommandStruct, sizeof(struct ui_command));
  }
  else if (status_message.emergency_bitmask & 0b11000)
  {
    uiCommandStruct.cmd2 = LED_blink_slow;
    sendUIMessage(&uiCommandStruct, sizeof(struct ui_command));
  }
  else if (status_message.emergency_bitmask & 0b0000001 == 0)
  {
    uiCommandStruct.cmd2 = LED_off;
    sendUIMessage(&uiCommandStruct, sizeof(struct ui_command));
  }
}

void setup1()
{
  // Core
}

void loop1()
{
  // Loop through the mux and query actions. Store the result in the multicore fifo
  for (uint8_t mux_address = 0; mux_address < 7; mux_address++)
  {
    gpio_put_masked(0b111 << 13, mux_address << 13);
    delay(1);
    bool state = gpio_get(PIN_MUX_IN);

    switch (mux_address)
    {
    case 5:
      mutex_enter_blocking(&mtx_status_message);

      if (state)
      {
        status_message.status_bitmask |= 0b00010000;
      }
      else
      {
        status_message.status_bitmask &= 0b11101111;
      }
      mutex_exit(&mtx_status_message);

      break;
    case 6:
      mutex_enter_blocking(&mtx_status_message);
      if (state)
      {
        status_message.status_bitmask |= 0b00100000;
      }
      else
      {
        status_message.status_bitmask &= 0b11011111;
      }
      mutex_exit(&mtx_status_message);
      break;
    default:
      break;
    }
  }

  delay(100);
}

void setup()
{
  //  We do hardware init in this core, so that we don't get invalid states.
  //  Therefore, we pause the other core until setup() was a success
  rp2040.idleOtherCore();

  emergency_latch = true;
  ROS_running = false;

  lift_emergency_started = 0;
  button_emergency_started = 0;
  // Initialize messages
  imu_message = {0};
  imu_quat_message = {0};
  status_message = {0};
  imu_message.type = PACKET_ID_LL_IMU;
  status_message.type = PACKET_ID_LL_STATUS;
  imu_quat_message.type = PACKET_ID_LL_IMU_QUATERNION;

  // Setup pins
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  pinMode(PIN_ENABLE_CHARGE, OUTPUT);
  digitalWrite(PIN_ENABLE_CHARGE, LOW);

  gpio_init(PIN_RASPI_POWER);
  gpio_put(PIN_RASPI_POWER, true);
  gpio_set_dir(PIN_RASPI_POWER, true);
  gpio_put(PIN_RASPI_POWER, true);

  // Enable raspi power
  p.neoPixelSetValue(0, 32, 0, 0, true);
  delay(1000);
  setRaspiPower(true);
  p.neoPixelSetValue(0, 255, 0, 0, true);

  pinMode(PIN_MUX_OUT, OUTPUT);
  pinMode(PIN_MUX_ADDRESS_0, OUTPUT);
  pinMode(PIN_MUX_ADDRESS_1, OUTPUT);
  pinMode(PIN_MUX_ADDRESS_2, OUTPUT);

  pinMode(PIN_EMERGENCY_1, INPUT);
  pinMode(PIN_EMERGENCY_2, INPUT);
  pinMode(PIN_EMERGENCY_3, INPUT);
  pinMode(PIN_EMERGENCY_4, INPUT);

  analogReadResolution(12);

#ifdef USB_DEBUG
  DEBUG_SERIAL.begin(115200);
#endif

  // init serial com to RasPi
  PACKET_SERIAL.begin(115200);
  packetSerial.setStream(&PACKET_SERIAL);
  packetSerial.setPacketHandler(&onPacketReceived);

  UI1_SERIAL.begin(115200);
  UISerial.setStream(&UI1_SERIAL);
  UISerial.setPacketHandler(&onUIPacketReceived);

  /*
   * IMU INITIALIZATION
   */

  if (!init_imu())
  {
#ifdef USB_DEBUG
    DEBUG_SERIAL.println("IMU initialization unsuccessful");
    DEBUG_SERIAL.println("Check IMU wiring or try cycling power");
#endif
    status_message.status_bitmask = 0;
    while (1)
    {
      p.neoPixelSetValue(0, 255, 0, 0, true);
      delay(500);
      p.neoPixelSetValue(0, 0, 0, 0, true);
      delay(500);
    }

    {
#ifdef USB_DEBUG
      DEBUG_SERIAL.println("Error: Imu init failed");
#endif
      // We don't need to lock the mutex here, since core 1 is sleeping anyways
      sendMessage(&status_message, sizeof(struct ll_status));
      delay(1000);
    }
  }

#ifdef USB_DEBUG
  DEBUG_SERIAL.println("Imu initialized");
#endif

  /*
   * /IMU INITIALIZATION
   */

  status_message.status_bitmask |= 1;

#ifdef ENABLE_SOUND_MODULE
  p.neoPixelSetValue(0, 0, 255, 255, true);
  delay(1000);

  sound_available = my_sound.begin(NR_SOUNDFILES);
  if (sound_available)
  {
    p.neoPixelSetValue(0, 0, 0, 255, true);
    my_sound.setvolume(10);
    delay(100);
    my_sound.setvolume(10);
    delay(100);
    my_sound.playSoundAdHoc(1);
    delay(6000);
    p.neoPixelSetValue(0, 255, 255, 0, true);
  }
  else
  {
    for (uint8_t b = 0; b < 3; b++)
    {
      p.neoPixelSetValue(0, 0, 0, 0, true);
      delay(200);
      p.neoPixelSetValue(0, 0, 0, 255, true);
      delay(200);
    }
  }

  // Soundtest
  my_sound.playSound(2);
  my_sound.playSound(3);
  my_sound.playSound(13);
  my_sound.playSound(14);

  int i = my_sound.sounds2play();

#else
  sound_available = false;
#endif

  rp2040.resumeOtherCore();

  // UIboard clear all LEDs
  uiCommandStruct.type = Set_LED;
  uiCommandStruct.cmd1 = 0;
  uiCommandStruct.cmd2 = LED_All_OFF;
  sendUIMessage(&uiCommandStruct, sizeof(struct ui_command));
  digitalWrite(LED_BUILTIN, LOW);

}

void onUIPacketReceived(const uint8_t *buffer, size_t size)
{

  u_int16_t *crc_pointer = (uint16_t *)(buffer + (size - 2));
  u_int16_t readcrc = *crc_pointer;

  // check structure size
  if (size != sizeof(struct ui_command))
    return;
  if ((buffer[0] != Get_Version) && (buffer[0] != Get_Buttonnr))
    return;

  // check the CRC
  uint16_t crc = CRC16.ccitt(buffer, size - 2);

  if (buffer[size - 1] != ((crc >> 8) & 0xFF) ||
      buffer[size - 2] != (crc & 0xFF))
    return;

  struct ui_command *buttonboard = (struct ui_command *)buffer;

  // overwrite type for the ROS system
  buttonboard->type = PACKET_ID_LL_UI_EVENT;
  sendMessage(buttonboard, sizeof(struct ui_command));
}

void onPacketReceived(const uint8_t *buffer, size_t size)
{
  // we currently only support heartbeats
  if (size != sizeof(struct ll_heartbeat))
    return;
  if (buffer[0] != PACKET_ID_LL_HEARTBEAT)
    return;

  // check the CRC
  uint16_t crc = CRC16.ccitt(buffer, size - 2);

  if (buffer[size - 1] != ((crc >> 8) & 0xFF) ||
      buffer[size - 2] != (crc & 0xFF))
    return;

  // CRC and packet is OK, reset watchdog
  last_heartbeat_millis = millis();
  ROS_running = true;
  struct ll_heartbeat *heartbeat = (struct ll_heartbeat *)buffer;
  if (heartbeat->emergency_release_requested)
  {
    emergency_latch = false;
  }
  // Check in this order, so we can set it again in the same packet if required.
  if (heartbeat->emergency_requested)
  {
    emergency_latch = true;
  }
}

// returns true, if it's a good idea to charge the battery (current, voltages, ...)
bool checkShouldCharge()
{
  return status_message.v_charge < 30.0 && status_message.charging_current < 1.5 && status_message.v_battery < 29.0;
}

void updateChargingEnabled()
{
  if (charging_allowed)
  {
    if (!checkShouldCharge())
    {
      digitalWrite(PIN_ENABLE_CHARGE, LOW);
      charging_allowed = false;
      charging_disabled_time = millis();
    }
  }
  else
  {
    // enable charging after CHARGING_RETRY_MILLIS
    if (millis() - charging_disabled_time > CHARGING_RETRY_MILLIS)
    {
      if (!checkShouldCharge())
      {
        digitalWrite(PIN_ENABLE_CHARGE, LOW);
        charging_allowed = false;
        charging_disabled_time = millis();
      }
      else
      {
        digitalWrite(PIN_ENABLE_CHARGE, HIGH);
        charging_allowed = true;
      }
    }
  }
}

void updateNeopixel()
{
  led_blink_counter++;
  // flash red on emergencies
  if (emergency_latch && led_blink_counter&0b10)
  {
    p.neoPixelSetValue(0, 128, 0, 0, true);
  }
  else
  {
    if (ROS_running)
    {
      // Green, if ROS is running
      p.neoPixelSetValue(0, 0, 255, 0, true);
      digitalWrite(LED_BUILTIN, HIGH);
    }
    else
    {
      // Yellow, if it's not running
      p.neoPixelSetValue(0, 255, 50, 0, true);
      digitalWrite(LED_BUILTIN, LOW);
    }
  }
}

void loop()
{
  packetSerial.update();
  UISerial.update();
  imu_loop();
  updateChargingEnabled();
  updateEmergency();

  unsigned long now = millis();
  if (now - last_imu_millis > IMU_CYCLETIME)
  {
    // we have to copy to the temp data structure due to alignment issues
    imu_read(imu_temp, imu_temp + 3, imu_temp + 6);
    imu_message.acceleration_mss[0] = imu_temp[0];
    imu_message.acceleration_mss[1] = imu_temp[1];
    imu_message.acceleration_mss[2] = imu_temp[2];
    imu_message.gyro_rads[0] = imu_temp[3];
    imu_message.gyro_rads[1] = imu_temp[4];
    imu_message.gyro_rads[2] = imu_temp[5];
    imu_message.mag_uT[0] = imu_temp[6];
    imu_message.mag_uT[1] = imu_temp[7];
    imu_message.mag_uT[2] = imu_temp[8];

    imu_message.dt_millis = now - last_imu_millis;
    sendMessage(&imu_message, sizeof(struct ll_imu));

    imu_read_quat(imu_temp);
    imu_quat_message.q[0] = imu_temp[0];
    imu_quat_message.q[1] = imu_temp[1];
    imu_quat_message.q[2] = imu_temp[2];
    imu_quat_message.q[3] = imu_temp[3];
    sendMessage(&imu_quat_message, sizeof(struct ll_imu_quaternion));

    last_imu_millis = now;
  }

  if (now - last_status_update_millis > STATUS_CYCLETIME)
  {
    updateNeopixel();

    status_message.v_battery = (float)analogRead(PIN_ANALOG_BATTERY_VOLTAGE) * (3.3f / 4096.0f) * ((VIN_R1 + VIN_R2) / VIN_R2);
    status_message.v_charge = (float)analogRead(PIN_ANALOG_CHARGE_VOLTAGE) * (3.3f / 4096.0f) * ((VIN_R1 + VIN_R2) / VIN_R2);
    status_message.charging_current = (float)analogRead(PIN_ANALOG_CHARGE_CURRENT) * (3.3f / 4096.0f) / (CURRENT_SENSE_GAIN * R_SHUNT);
    status_message.status_bitmask = (status_message.status_bitmask & 0b11111011) | ((charging_allowed & 0b1) << 2);
    status_message.status_bitmask = (status_message.status_bitmask & 0b11011111) | ((sound_available & 0b1) << 5);

    // calculate percent value accu filling
    float delta = BATT_FULL - BATT_EMPTY;
    float vo = status_message.v_battery - BATT_EMPTY;
    status_message.batt_percentage = vo / delta * 100;
    if (status_message.batt_percentage > 100)
      status_message.batt_percentage = 100;

    mutex_enter_blocking(&mtx_status_message);
    sendMessage(&status_message, sizeof(struct ll_status));
    mutex_exit(&mtx_status_message);

    last_status_update_millis = now;
#ifdef USB_DEBUG
    DEBUG_SERIAL.print("status: 0b");
    DEBUG_SERIAL.print(status_message.status_bitmask, BIN);
    DEBUG_SERIAL.print("\t");

    DEBUG_SERIAL.print("vin: ");
    DEBUG_SERIAL.print(status_message.v_battery, 3);
    DEBUG_SERIAL.print(" V\t");
    DEBUG_SERIAL.print("vcharge: ");
    DEBUG_SERIAL.print(status_message.v_charge, 3);
    DEBUG_SERIAL.print(" V\t");
    DEBUG_SERIAL.print("charge_current: ");
    DEBUG_SERIAL.print(status_message.charging_current, 3);
    DEBUG_SERIAL.print(" A\t");
    DEBUG_SERIAL.print("emergency: 0b");
    DEBUG_SERIAL.print(status_message.emergency_bitmask, BIN);
    DEBUG_SERIAL.println();
#endif
  }

  if (now - last_UILED_millis > UI_SET_LED_CYCLETIME)
  {
    manageUILEDS();
    last_UILED_millis = now;
#ifdef ENABLE_SOUND_MODULE
    if (sound_available)
    {
      my_sound.processSounds();
    }
#endif
  }
}

void sendMessage(void *message, size_t size)
{
  // Only send messages, if ROS is running, else Raspi sometimes doesn't boot
  if (!ROS_running)
    return;

  // packages need to be at least 1 byte of type, 1 byte of data and 2 bytes of CRC
  if (size < 4)
  {
    return;
  }
  uint8_t *data_pointer = (uint8_t *)message;

  // calculate the CRC
  uint16_t crc = CRC16.ccitt((uint8_t *)message, size - 2);
  data_pointer[size - 1] = (crc >> 8) & 0xFF;
  data_pointer[size - 2] = crc & 0xFF;

  packetSerial.send((uint8_t *)message, size);
}

void sendUIMessage(void *message, size_t size)
{
  // packages need to be at least 1 byte of type, 1 byte of data and 2 bytes of CRC
  if (size < 4)
  {
    return;
  }
  uint8_t *data_pointer = (uint8_t *)message;

  // calculate the CRC
  uint16_t crc = CRC16.ccitt((uint8_t *)message, size - 2);
  data_pointer[size - 1] = (crc >> 8) & 0xFF;
  data_pointer[size - 2] = crc & 0xFF;

  UISerial.send((uint8_t *)message, size);
}
