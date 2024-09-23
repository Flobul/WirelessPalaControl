#include "WirelessPalaControl.h"

#ifdef ESP8266
#define PALA_SERIAL Serial
#else
#define PALA_SERIAL Serial2
#endif

// Serial management functions -------------
int WebPalaControl::myOpenSerial(uint32_t baudrate)
{
#ifdef ESP8266
  PALA_SERIAL.begin(baudrate);
  PALA_SERIAL.pins(15, 13); // swap ESP8266 pins to alternative positions (D7(GPIO13)(RX)/D8(GPIO15)(TX))
#else
  PALA_SERIAL.begin(baudrate, SERIAL_8N1, 23, 5); // set ESP32 pins to match hat position (IO23(RX)/IO5(TX))
#endif
  return 0;
}
void WebPalaControl::myCloseSerial()
{
  PALA_SERIAL.end();
  // set TX PIN to OUTPUT HIGH to avoid stove bus blocking
#ifdef ESP8266
  pinMode(15, OUTPUT);
  digitalWrite(15, HIGH);
#else
  pinMode(5, OUTPUT);
  digitalWrite(5, HIGH);
#endif
}
int WebPalaControl::mySelectSerial(unsigned long timeout)
{
  size_t avail;
  unsigned long startmillis = millis();
  while ((avail = PALA_SERIAL.available()) == 0 && (startmillis + timeout) > millis())
    ;

  return avail;
}
size_t WebPalaControl::myReadSerial(void *buf, size_t count) { return PALA_SERIAL.read((char *)buf, count); }
size_t WebPalaControl::myWriteSerial(const void *buf, size_t count) { return PALA_SERIAL.write((const uint8_t *)buf, count); }
int WebPalaControl::myDrainSerial()
{
  PALA_SERIAL.flush(); // On ESP, Serial.flush() is drain
  return 0;
}
int WebPalaControl::myFlushSerial()
{
  PALA_SERIAL.flush();
  while (PALA_SERIAL.read() != -1)
    ; // flush RX buffer
  return 0;
}
void WebPalaControl::myUSleep(unsigned long usecond) { delayMicroseconds(usecond); }

void WebPalaControl::mqttConnectedCallback(MQTTMan *mqttMan, bool firstConnection)
{

  // Subscribe to needed topic
  // prepare topic subscription
  String subscribeTopic = _ha.mqtt.generic.baseTopic;

  // Replace placeholders
  MQTTMan::prepareTopic(subscribeTopic);

  switch (_ha.mqtt.type) // switch on MQTT type
  {
  case HA_MQTT_GENERIC:
  case HA_MQTT_GENERIC_JSON:
  case HA_MQTT_GENERIC_CATEGORIZED:
    subscribeTopic += F("cmd");
    break;
  }

  if (firstConnection)
    mqttMan->publish(subscribeTopic.c_str(), ""); // make empty publish only for firstConnection
  mqttMan->subscribe(subscribeTopic.c_str());

  // raise flag to publish Home Assistant discovery data
  _needPublishHassDiscovery = true;
}

void WebPalaControl::mqttDisconnectedCallback()
{
  // if MQTT is disconnected, MQTT Reconnection will publish "1" to connectedTopic
  _publishedStoveConnected = false;
}

void WebPalaControl::mqttCallback(char *topic, uint8_t *payload, unsigned int length)
{
  // if topic is basetopic/cmd
  // commented because only this topic is subscribed

  String cmd;
  String strJson;

  // convert payload to String cmd
  cmd.concat((char *)payload, length);

  // replace '+' by ' '
  cmd.replace('+', ' ');

  // execute Palazzetti command
  executePalaCmd(cmd, strJson, true);

  // publish json result to MQTT
  String baseTopic = _ha.mqtt.generic.baseTopic;
  MQTTMan::prepareTopic(baseTopic);
  String resTopic(baseTopic);
  resTopic += F("result");
  _mqttMan.publish(resTopic.c_str(), strJson.c_str());
}

void WebPalaControl::publishStoveConnectedToMqtt(bool stoveConnected)
{
  if (_mqttMan.connected() && _publishedStoveConnected != stoveConnected)
  {
    // if Stove is connected, publish 2 to connected topic otherwise fallback to 1
    _mqttMan.publishToConnectedTopic((stoveConnected ? "2" : "1"));
    _publishedStoveConnected = stoveConnected;
  }
}

bool WebPalaControl::publishDataToMqtt(const String &baseTopic, const String &palaCategory, const JsonDocument &jsonDoc)
{
  bool res = false;
  if (_mqttMan.connected())
  {
    if (_ha.mqtt.type == HA_MQTT_GENERIC)
    {
      // for each key/value pair in DATA
      for (JsonPairConst kv : jsonDoc["DATA"].as<JsonObjectConst>())
      {
        // prepare topic
        String topic(baseTopic);
        topic += kv.key().c_str();
        // publish
        res = _mqttMan.publish(topic.c_str(), kv.value().as<String>().c_str());
      }
    }

    if (_ha.mqtt.type == HA_MQTT_GENERIC_JSON)
    {
      // prepare topic
      String topic(baseTopic);
      topic += palaCategory;
      // serialize DATA to JSON
      String serializedData;
      serializeJson(jsonDoc["DATA"], serializedData);
      // publish
      res = _mqttMan.publish(topic.c_str(), serializedData.c_str());
    }

    if (_ha.mqtt.type == HA_MQTT_GENERIC_CATEGORIZED)
    {
      // prepare category topic
      String categoryTopic(baseTopic);
      categoryTopic += palaCategory;
      categoryTopic += '/';

      // for each key/value pair in DATA
      for (JsonPairConst kv : jsonDoc["DATA"].as<JsonObjectConst>())
      {
        // prepare topic
        String topic(categoryTopic);
        topic += kv.key().c_str();
        // publish
        res = _mqttMan.publish(topic.c_str(), kv.value().as<String>().c_str());
      }
    }
  }
  return res;
}

bool WebPalaControl::publishHassDiscoveryToMqtt()
{
  if (!_Pala.isInitialized() || !_mqttMan.connected())
    return false;

  LOG_SERIAL.println(F("Publish Home Assistant Discovery data"));

  // read static data from stove
  char SN[28];
  byte SNCHK;
  uint16_t MOD, VER;
  char FWDATE[11];
  uint16_t FLUID;
  byte MAINTPROBE;
  byte STOVETYPE;
  byte FAN2TYPE;
  byte FAN2MODE;
  if (Palazzetti::CommandResult::OK != _Pala.getStaticData(&SN, &SNCHK, nullptr, &MOD, &VER, nullptr, &FWDATE, &FLUID, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &MAINTPROBE, &STOVETYPE, &FAN2TYPE, &FAN2MODE, nullptr, nullptr, nullptr, nullptr, nullptr))
    return false;

  // read all status from stove
  bool refreshStatus = false;
  unsigned long currentMillis = millis();
  if ((currentMillis - _lastAllStatusRefreshMillis) > 15000UL) // refresh AllStatus data if it's 15sec old
    refreshStatus = true;
  float SETP;
  uint16_t FANLMINMAX[6];
  if (Palazzetti::CommandResult::OK != _Pala.getAllStatus(false, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &SETP, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &FANLMINMAX, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
    return false;
  else if (refreshStatus)
    _lastAllStatusRefreshMillis = currentMillis;

  // calculate flags (https://github.com/palazzetti/palazzetti-sdk-asset-parser-python/blob/main/palazzetti_sdk_asset_parser/data/asset_parser.json)
  bool hasSetPoint = (SETP != 0);
  bool hasPower = (STOVETYPE != 8);
  bool hasOnOff = (STOVETYPE != 7 && STOVETYPE != 8);
  bool hasRoomFan = (FAN2TYPE > 1);
  bool hasFan3 = (FAN2TYPE > 2);
  bool ifFan3SwitchEntity = (FANLMINMAX[2] == 0 && FANLMINMAX[3] == 1);
  bool hasFan4 = (FAN2TYPE > 3);
  bool ifFan4SwitchEntity = (FANLMINMAX[4] == 0 && FANLMINMAX[5] == 1);
  bool isAirType = (STOVETYPE == 1 || STOVETYPE == 3 || STOVETYPE == 5 || STOVETYPE == 7 || STOVETYPE == 8);
  bool hasFanAuto = (FAN2MODE == 2 || FAN2MODE == 3);

  // variables
  JsonDocument jsonDoc;
  String device, availability, payload;
  String baseTopic;
  String uniqueIdPrefixWPalaControl, uniqueIdPrefixStove;
  String uniqueId;
  String topic;

  // prepare base topic
  baseTopic = _ha.mqtt.generic.baseTopic;
  MQTTMan::prepareTopic(baseTopic);

  // prepare unique id prefix for WPalaControl
  uniqueIdPrefixWPalaControl = F("WPalaControl_");
  uniqueIdPrefixWPalaControl += WiFi.macAddress();
  uniqueIdPrefixWPalaControl.replace(":", "");

  // prepare unique id prefix for Stove
  uniqueIdPrefixStove = F("WPalaControl_");
  uniqueIdPrefixStove += SN;

  // ---------- WPalaControl Device ----------

  // prepare WPalaControl device JSON
  jsonDoc["configuration_url"] = F("http://wpalacontrol.local");
  jsonDoc["identifiers"][0] = uniqueIdPrefixWPalaControl;
  jsonDoc["manufacturer"] = F("Domochip");
  jsonDoc["model"] = F("WPalaControl");
  #ifdef ESP8266
  jsonDoc["name"] = WiFi.hostname();
  #else
  jsonDoc["name"] = WiFi.getHostname();
  #endif
  jsonDoc["sw_version"] = String(F("v")) + String(BASE_VERSION) + '-' + String(VERSION);
  serializeJson(jsonDoc, device); // serialize to device String
  jsonDoc.clear();                // clean jsonDoc

  // ----- WPalaControl Entities -----

  //
  // Connectivity entity
  //

  // prepare uniqueId, topic and payload for WPalaControl connectivity sensor
  uniqueId = uniqueIdPrefixWPalaControl;
  uniqueId += F("_Connectivity");

  topic = _ha.mqtt.hassDiscoveryPrefix;
  topic += F("/binary_sensor/");
  topic += uniqueId;
  topic += F("/config");

  // prepare payload for WPalaControl connectivity sensor
  jsonDoc["~"] = baseTopic.substring(0, baseTopic.length() - 1); // remove ending '/'
  jsonDoc["device_class"] = F("connectivity");
  jsonDoc["device"] = serialized(device);
  jsonDoc["entity_category"] = F("diagnostic");
  jsonDoc["object_id"] = F("wpalacontrol_connectivity");
  jsonDoc["state_topic"] = F("~/connected");
  jsonDoc["unique_id"] = uniqueId;
  jsonDoc["value_template"] = F("{{ iif(int(value) > 0, 'ON', 'OFF') }}");
  serializeJson(jsonDoc, payload);

  // publish
  _mqttMan.publish(topic.c_str(), payload.c_str(), true);

  // clean
  jsonDoc.clear();
  payload = "";
  device = "";

  // ---------- Stove Device ----------

  // prepare availability JSON for Stove entities
  jsonDoc["topic"] = F("~/connected");
  jsonDoc["value_template"] = F("{{ iif(int(value) > 0, 'online', 'offline') }}");
  serializeJson(jsonDoc, availability); // serialize to availability String
  jsonDoc.clear();                      // clean jsonDoc

  // prepare Stove device JSON
  jsonDoc["configuration_url"] = F("http://wpalacontrol.local");
  jsonDoc["identifiers"][0] = uniqueIdPrefixStove;
  jsonDoc["model"] = String(MOD);
  jsonDoc["name"] = F("Stove");
  jsonDoc["sw_version"] = String(VER) + F(" (") + FWDATE + ')';
  jsonDoc["via_device"] = uniqueIdPrefixWPalaControl;
  serializeJson(jsonDoc, device); // serialize to device String
  jsonDoc.clear();                // clean jsonDoc

  // ----- Stove Entities -----

  //
  // Connectivity entity
  //

  uniqueId = uniqueIdPrefixStove;
  uniqueId += F("_Connectivity");

  topic = _ha.mqtt.hassDiscoveryPrefix;
  topic += F("/binary_sensor/");
  topic += uniqueId;
  topic += F("/config");

  // prepare payload for Stove connectivity sensor
  jsonDoc["~"] = baseTopic.substring(0, baseTopic.length() - 1); // remove ending '/'
  jsonDoc["device_class"] = F("connectivity");
  jsonDoc["device"] = serialized(device);
  jsonDoc["entity_category"] = F("diagnostic");
  jsonDoc["object_id"] = F("stove_connectivity");
  jsonDoc["state_topic"] = F("~/connected");
  jsonDoc["unique_id"] = uniqueId;
  jsonDoc["value_template"] = F("{{ iif(int(value) > 1, 'ON', 'OFF') }}");

  serializeJson(jsonDoc, payload);

  // publish
  _mqttMan.publish(topic.c_str(), payload.c_str(), true);

  // clean
  jsonDoc.clear();
  payload = "";

  //
  // Status entity
  //

  uniqueId = uniqueIdPrefixStove;
  uniqueId += F("_STATUS");

  topic = _ha.mqtt.hassDiscoveryPrefix;
  topic += F("/sensor/");
  topic += uniqueId;
  topic += F("/config");

  // prepare payload for Stove status sensor
  jsonDoc["~"] = baseTopic.substring(0, baseTopic.length() - 1); // remove ending '/'
  jsonDoc["availability"] = serialized(availability);
  jsonDoc["device"] = serialized(device);
  jsonDoc["entity_category"] = F("diagnostic");
  jsonDoc["name"] = F("Status");
  jsonDoc["object_id"] = F("stove_status");
  const __FlashStringHelper *statusTopicList[] = {F("~/STATUS"), F("~/STAT"), F("~/STAT/STATUS")};
  jsonDoc["state_topic"] = statusTopicList[_ha.mqtt.type];
  jsonDoc["unique_id"] = uniqueId;
  if (_ha.mqtt.type == HA_MQTT_GENERIC_JSON)
    jsonDoc["value_template"] = F("{{ value_json.STATUS }}");

  serializeJson(jsonDoc, payload);

  // publish
  _mqttMan.publish(topic.c_str(), payload.c_str(), true);

  // clean
  jsonDoc.clear();
  payload = "";

  //
  // Status Text entity
  //

  uniqueId = uniqueIdPrefixStove;
  uniqueId += F("_STATUS_Text");

  topic = _ha.mqtt.hassDiscoveryPrefix;
  topic += F("/sensor/");
  topic += uniqueId;
  topic += F("/config");

  // prepare payload for Stove status text sensor
  jsonDoc["~"] = baseTopic.substring(0, baseTopic.length() - 1); // remove ending '/'
  jsonDoc["availability"] = serialized(availability);
  jsonDoc["device"] = serialized(device);
  jsonDoc["device_class"] = F("enum");
  jsonDoc["name"] = F("Status");
  jsonDoc["object_id"] = F("stove_status_text");
  // const __FlashStringHelper *statusTopicList[] = {F("~/STATUS"), F("~/STAT"), F("~/STAT/STATUS")}; // reuse statusTopicList
  jsonDoc["state_topic"] = statusTopicList[_ha.mqtt.type];
  jsonDoc["unique_id"] = uniqueId;
  if (_ha.mqtt.type == HA_MQTT_GENERIC || _ha.mqtt.type == HA_MQTT_GENERIC_CATEGORIZED)
    jsonDoc["value_template"] = F("{% set ns = namespace(found=false) %}{% set statusList=[([0],'Off'),([1],'Off Timer'),([2],'Test Fire'),([3,4,5],'Ignition'),([6],'Burning'),([9],'Cool'),([10],'Fire Stop'),([11],'Clean Fire'),([12],'Cool'),([239],'MFDoor Alarm'),([240],'Fire Error'),([241],'Chimney Alarm'),([243],'Grate Error'),([244],'NTC2 Alarm'),([245],'NTC3 Alarm'),([247],'Door Alarm'),([248],'Pressure Alarm'),([249],'NTC1 Alarm'),([250],'TC1 Alarm'),([252],'Gas Alarm'),([253],'No Pellet Alarm')] %}{% for num,text in statusList %}{% if int(value) in num %}{{ text }}{% set ns.found = true %}{% break %}{% endif %}{% endfor %}{% if not ns.found %}Unkown STATUS code {{ value }}{% endif %}");
  else if (_ha.mqtt.type == HA_MQTT_GENERIC_JSON)
    jsonDoc["value_template"] = F("{% set ns = namespace(found=false) %}{% set statusList=[([0],'Off'),([1],'Off Timer'),([2],'Test Fire'),([3,4,5],'Ignition'),([6],'Burning'),([9],'Cool'),([10],'Fire Stop'),([11],'Clean Fire'),([12],'Cool'),([239],'MFDoor Alarm'),([240],'Fire Error'),([241],'Chimney Alarm'),([243],'Grate Error'),([244],'NTC2 Alarm'),([245],'NTC3 Alarm'),([247],'Door Alarm'),([248],'Pressure Alarm'),([249],'NTC1 Alarm'),([250],'TC1 Alarm'),([252],'Gas Alarm'),([253],'No Pellet Alarm')] %}{% for num,text in statusList %}{% if int(value_json.STATUS) in num %}{{ text }}{% set ns.found = true %}{% break %}{% endif %}{% endfor %}{% if not ns.found %}Unkown STATUS code {{ value_json.STATUS }}{% endif %}");

  serializeJson(jsonDoc, payload);

  // publish
  _mqttMan.publish(topic.c_str(), payload.c_str(), true);

  // clean
  jsonDoc.clear();
  payload = "";

  //
  // Room temperature entity
  //

  uniqueId = uniqueIdPrefixStove;
  uniqueId += F("_RoomTemp");

  topic = _ha.mqtt.hassDiscoveryPrefix;
  topic += F("/sensor/");
  topic += uniqueId;
  topic += F("/config");

  // prepare payload for Stove room temperature sensor
  jsonDoc["~"] = baseTopic.substring(0, baseTopic.length() - 1); // remove ending '/'
  jsonDoc["availability"] = serialized(availability);
  jsonDoc["device"] = serialized(device);
  jsonDoc["device_class"] = F("temperature");
  jsonDoc["name"] = F("Room Temperature");
  jsonDoc["object_id"] = F("stove_roomtemp");
  jsonDoc["suggested_display_precision"] = 1;
  jsonDoc["state_class"] = F("measurement");
  jsonDoc["unique_id"] = uniqueId;
  jsonDoc["unit_of_measurement"] = F("°C");
  if (_ha.mqtt.type == HA_MQTT_GENERIC)
    jsonDoc["state_topic"] = String(F("~/T")) + (char)('1' + MAINTPROBE);
  else if (_ha.mqtt.type == HA_MQTT_GENERIC_JSON)
  {
    jsonDoc["state_topic"] = F("~/TMPS");
    jsonDoc["value_template"] = String(F("{{ value_json.T")) + (char)('1' + MAINTPROBE) + F(" }}");
  }
  else if (_ha.mqtt.type == HA_MQTT_GENERIC_CATEGORIZED)
    jsonDoc["state_topic"] = String(F("~/TMPS/T")) + (char)('1' + MAINTPROBE);

  serializeJson(jsonDoc, payload);

  // publish
  _mqttMan.publish(topic.c_str(), payload.c_str(), true);

  // clean
  jsonDoc.clear();
  payload = "";

  //
  // Pellet consumption entity
  //

  uniqueId = uniqueIdPrefixStove;
  uniqueId += F("_PQT");

  topic = _ha.mqtt.hassDiscoveryPrefix;
  topic += F("/sensor/");
  topic += uniqueId;
  topic += F("/config");

  // prepare payload for Stove pellet consumption sensor
  jsonDoc["~"] = baseTopic.substring(0, baseTopic.length() - 1); // remove ending '/'
  jsonDoc["availability"] = serialized(availability);
  jsonDoc["device"] = serialized(device);
  jsonDoc["device_class"] = F("weight");
  jsonDoc["icon"] = F("mdi:chart-bell-curve-cumulative");
  jsonDoc["name"] = F("Pellet Consumed");
  jsonDoc["object_id"] = F("stove_pqt");
  jsonDoc["state_class"] = F("total_increasing");
  const __FlashStringHelper *pqtTopicList[] = {F("~/PQT"), F("~/CNTR"), F("~/CNTR/PQT")};
  jsonDoc["state_topic"] = pqtTopicList[_ha.mqtt.type];
  jsonDoc["unique_id"] = uniqueId;
  jsonDoc["unit_of_measurement"] = F("kg");
  if (_ha.mqtt.type == HA_MQTT_GENERIC_JSON)
    jsonDoc["value_template"] = F("{{ value_json.PQT }}");

  serializeJson(jsonDoc, payload);

  // publish
  _mqttMan.publish(topic.c_str(), payload.c_str(), true);

  // clean
  jsonDoc.clear();
  payload = "";

  //
  // OnOff entity
  //

  if (hasOnOff)
  {
    uniqueId = uniqueIdPrefixStove;
    uniqueId += F("_ON_OFF");

    topic = _ha.mqtt.hassDiscoveryPrefix;
    topic += F("/switch/");
    topic += uniqueId;
    topic += F("/config");

    // prepare payload for Stove onoff switch
    jsonDoc["~"] = baseTopic.substring(0, baseTopic.length() - 1); // remove ending '/'
    jsonDoc["availability"] = serialized(availability);
    jsonDoc["command_topic"] = F("~/cmd");
    jsonDoc["device"] = serialized(device);
    jsonDoc["icon"] = F("mdi:power");
    jsonDoc["name"] = F("On/Off");
    jsonDoc["object_id"] = F("stove_on_off");
    jsonDoc["payload_off"] = F("CMD+OFF");
    jsonDoc["payload_on"] = F("CMD+ON");
    jsonDoc["state_off"] = F("OFF");
    jsonDoc["state_on"] = F("ON");
    // const __FlashStringHelper *statusTopicList[] = {F("~/STATUS"), F("~/STAT"), F("~/STAT/STATUS")}; // reuse statusTopicList
    jsonDoc["state_topic"] = statusTopicList[_ha.mqtt.type];
    jsonDoc["unique_id"] = uniqueId;
    if (_ha.mqtt.type == HA_MQTT_GENERIC || _ha.mqtt.type == HA_MQTT_GENERIC_CATEGORIZED)
      jsonDoc["value_template"] = F("{{ iif(int(value) > 1, 'ON', 'OFF') }}");
    else if (_ha.mqtt.type == HA_MQTT_GENERIC_JSON)
      jsonDoc["value_template"] = F("{{ iif(int(value_json.STATUS) > 1, 'ON', 'OFF') }}");

    serializeJson(jsonDoc, payload);

    // publish
    _mqttMan.publish(topic.c_str(), payload.c_str(), true);

    // clean
    jsonDoc.clear();
    payload = "";
  }

  //
  // SetPoint entity
  //

  if (hasSetPoint)
  {
    uniqueId = uniqueIdPrefixStove;
    uniqueId += F("_SETP");

    topic = _ha.mqtt.hassDiscoveryPrefix;
    topic += F("/number/");
    topic += uniqueId;
    topic += F("/config");

    // prepare payload for Stove setpoint number
    jsonDoc["~"] = baseTopic.substring(0, baseTopic.length() - 1); // remove ending '/'
    jsonDoc["availability"] = serialized(availability);
    jsonDoc["command_template"] = F("SET+SETP+{{ value }}");
    jsonDoc["command_topic"] = F("~/cmd");
    jsonDoc["device"] = serialized(device);
    jsonDoc["device_class"] = F("temperature");
    jsonDoc["min"] = 17;
    jsonDoc["max"] = 23;
    jsonDoc["mode"] = F("slider");
    jsonDoc["name"] = F("SetPoint");
    jsonDoc["object_id"] = F("stove_setp");
    const __FlashStringHelper *setpTopicList[] = {F("~/SETP"), F("~/SET"), F("~/SET/SETP")};
    jsonDoc["state_topic"] = setpTopicList[_ha.mqtt.type];
    jsonDoc["unique_id"] = uniqueId;
    jsonDoc["unit_of_measurement"] = F("°C");
    if (_ha.mqtt.type == HA_MQTT_GENERIC_JSON)
      jsonDoc["value_template"] = F("{{ value_json.SETP }}");

    serializeJson(jsonDoc, payload);

    // publish
    _mqttMan.publish(topic.c_str(), payload.c_str(), true);

    // clean
    jsonDoc.clear();
    payload = "";
  }

  //
  // Power entity
  //

  if (hasPower)
  {
    uniqueId = uniqueIdPrefixStove;
    uniqueId += F("_PWR");

    topic = _ha.mqtt.hassDiscoveryPrefix;
    topic += F("/number/");
    topic += uniqueId;
    topic += F("/config");

    // prepare payload for Stove power number
    jsonDoc["~"] = baseTopic.substring(0, baseTopic.length() - 1); // remove ending '/'
    jsonDoc["availability"] = serialized(availability);
    jsonDoc["command_template"] = F("SET+POWR+{{ value }}");
    jsonDoc["command_topic"] = F("~/cmd");
    jsonDoc["device"] = serialized(device);
    jsonDoc["icon"] = F("mdi:signal");
    jsonDoc["min"] = 1;
    jsonDoc["max"] = 5;
    jsonDoc["mode"] = F("slider");
    jsonDoc["name"] = F("Power");
    jsonDoc["object_id"] = F("stove_pwr");
    const __FlashStringHelper *pwrTopicList[] = {F("~/PWR"), F("~/POWR"), F("~/POWR/PWR")};
    jsonDoc["state_topic"] = pwrTopicList[_ha.mqtt.type];
    jsonDoc["unique_id"] = uniqueId;
    if (_ha.mqtt.type == HA_MQTT_GENERIC_JSON)
      jsonDoc["value_template"] = F("{{ value_json.PWR }}");

    serializeJson(jsonDoc, payload);

    // publish
    _mqttMan.publish(topic.c_str(), payload.c_str(), true);

    // clean
    jsonDoc.clear();
    payload = "";
  }

  //
  // RoomFan entity
  //

  if (hasRoomFan)
  {
    uniqueId = uniqueIdPrefixStove;
    uniqueId += F("_RFAN");

    topic = _ha.mqtt.hassDiscoveryPrefix;
    topic += F("/number/");
    topic += uniqueId;
    topic += F("/config");

    // prepare payload for Stove room fan
    jsonDoc["~"] = baseTopic.substring(0, baseTopic.length() - 1); // remove ending '/'

    // specific availibility for room fan
    JsonArray availability = jsonDoc["availability"].to<JsonArray>();

    JsonObject availability_0 = availability.add<JsonObject>();
    availability_0["topic"] = F("~/connected");
    availability_0["value_template"] = F("{{ iif(int(value) > 0, 'online', 'offline') }}");

    JsonObject availability_1 = availability.add<JsonObject>();
    const __FlashStringHelper *f2lTopicList[] = {F("~/F2L"), F("~/FAND"), F("~/FAND/F2L")};
    availability_1["topic"] = f2lTopicList[_ha.mqtt.type];
    if (_ha.mqtt.type == HA_MQTT_GENERIC || _ha.mqtt.type == HA_MQTT_GENERIC_CATEGORIZED)
      availability_1["value_template"] = F("{{ iif(int(value) < 7, 'online', 'offline') }}");
    else if (_ha.mqtt.type == HA_MQTT_GENERIC_JSON)
      availability_1["value_template"] = F("{{ iif(int(value_json.F2L) < 7, 'online', 'offline') }}");

    jsonDoc["availability_mode"] = F("all");

    jsonDoc["command_template"] = F("SET+RFAN+{{ value }}");
    jsonDoc["command_topic"] = F("~/cmd");
    jsonDoc["device"] = serialized(device);
    jsonDoc["icon"] = F("mdi:fan");
    jsonDoc["min"] = 0;
    jsonDoc["max"] = 6;
    jsonDoc["name"] = F("Room Fan");
    jsonDoc["object_id"] = F("stove_rfan");
    jsonDoc["payload_reset"] = F("7");
    // const __FlashStringHelper *f2lTopicList[] = {F("~/F2L"), F("~/FAND"), F("~/FAND/F2L")}; //reuse f2lTopicList
    jsonDoc["state_topic"] = f2lTopicList[_ha.mqtt.type];
    jsonDoc["unique_id"] = uniqueId;
    if (_ha.mqtt.type == HA_MQTT_GENERIC_JSON)
      jsonDoc["value_template"] = F("{{ value_json.F2L }}");

    serializeJson(jsonDoc, payload);

    // publish
    _mqttMan.publish(topic.c_str(), payload.c_str(), true);

    // clean
    jsonDoc.clear();
    payload = "";
  }

  //
  // RoomFan Auto entity
  //

  if (isAirType && hasFanAuto)
  {
    uniqueId = uniqueIdPrefixStove;
    uniqueId += F("_RFAN_Auto");

    topic = _ha.mqtt.hassDiscoveryPrefix;
    topic += F("/switch/");
    topic += uniqueId;
    topic += F("/config");

    // prepare payload for Stove room fan auto mode
    jsonDoc["~"] = baseTopic.substring(0, baseTopic.length() - 1); // remove ending '/'
    jsonDoc["availability"] = serialized(availability);
    jsonDoc["command_topic"] = F("~/cmd");
    jsonDoc["device"] = serialized(device);
    jsonDoc["icon"] = F("mdi:fan-auto");
    jsonDoc["name"] = F("Room Fan Auto");
    jsonDoc["object_id"] = F("stove_rfan_auto");
    jsonDoc["payload_off"] = F("SET+RFAN+3");
    jsonDoc["payload_on"] = F("SET+RFAN+7");
    jsonDoc["state_off"] = F("OFF");
    jsonDoc["state_on"] = F("ON");
    const __FlashStringHelper *f2lTopicList[] = {F("~/F2L"), F("~/FAND"), F("~/FAND/F2L")};
    jsonDoc["state_topic"] = f2lTopicList[_ha.mqtt.type];
    jsonDoc["unique_id"] = uniqueId;
    if (_ha.mqtt.type == HA_MQTT_GENERIC || _ha.mqtt.type == HA_MQTT_GENERIC_CATEGORIZED)
      jsonDoc["value_template"] = F("{{ iif(int(value) == 7, 'ON', 'OFF') }}");
    else if (_ha.mqtt.type == HA_MQTT_GENERIC_JSON)
      jsonDoc["value_template"] = F("{{ iif(int(value_json.F2L) == 7, 'ON', 'OFF') }}");

    serializeJson(jsonDoc, payload);

    // publish
    _mqttMan.publish(topic.c_str(), payload.c_str(), true);

    // clean
    jsonDoc.clear();
    payload = "";
  }

  //
  // Fan3 entity
  //

  if (hasFan3)
  {
    uniqueId = uniqueIdPrefixStove;
    uniqueId += F("_FAN3");

    // entity type depends on Min and Max value of FAN3
    topic = _ha.mqtt.hassDiscoveryPrefix;
    topic += ifFan3SwitchEntity ? F("/switch/") : F("/number/");
    topic += uniqueId;
    topic += F("/config");

    // prepare payload for Stove fan3 number
    jsonDoc["~"] = baseTopic.substring(0, baseTopic.length() - 1); // remove ending '/'
    jsonDoc["availability"] = serialized(availability);
    jsonDoc["command_topic"] = F("~/cmd");
    jsonDoc["device"] = serialized(device);
    jsonDoc["icon"] = F("mdi:fan-speed-2");
    jsonDoc["name"] = F("Left Fan");
    jsonDoc["object_id"] = F("stove_fan3");
    const __FlashStringHelper *f3lTopicList[] = {F("~/F3L"), F("~/FAND"), F("~/FAND/F3L")};
    jsonDoc["state_topic"] = f3lTopicList[_ha.mqtt.type];
    jsonDoc["unique_id"] = uniqueId;
    if (_ha.mqtt.type == HA_MQTT_GENERIC_JSON)
      jsonDoc["value_template"] = F("{{ value_json.F3L }}");

    // add entity type specific configuration
    if (ifFan3SwitchEntity)
    {
      jsonDoc["payload_off"] = F("SET+FN3L+0");
      jsonDoc["payload_on"] = F("SET+FN3L+1");
      jsonDoc["state_off"] = F("0");
      jsonDoc["state_on"] = F("1");
    }
    else
    {
      jsonDoc["command_template"] = F("SET+FN3L+{{ value }}");
      jsonDoc["min"] = FANLMINMAX[2];
      jsonDoc["max"] = FANLMINMAX[3];
      jsonDoc["mode"] = F("slider");
    }

    serializeJson(jsonDoc, payload);

    // publish
    _mqttMan.publish(topic.c_str(), payload.c_str(), true);

    // clean
    jsonDoc.clear();
    payload = "";
  }

  //
  // Fan4 entity
  //

  if (hasFan4)
  {
    uniqueId = uniqueIdPrefixStove;
    uniqueId += F("_FAN4");

    // entity type depends on Min and Max value of FAN4
    topic = _ha.mqtt.hassDiscoveryPrefix;
    topic += ifFan4SwitchEntity ? F("/switch/") : F("/number/");
    topic += uniqueId;
    topic += F("/config");

    // prepare payload for Stove fan4 number
    jsonDoc["~"] = baseTopic.substring(0, baseTopic.length() - 1); // remove ending '/'
    jsonDoc["availability"] = serialized(availability);
    jsonDoc["command_topic"] = F("~/cmd");
    jsonDoc["device"] = serialized(device);
    jsonDoc["icon"] = F("mdi:fan-speed-3");
    jsonDoc["name"] = F("Right Fan");
    jsonDoc["object_id"] = F("stove_fan4");
    const __FlashStringHelper *f4lTopicList[] = {F("~/F4L"), F("~/FAND"), F("~/FAND/F4L")};
    jsonDoc["state_topic"] = f4lTopicList[_ha.mqtt.type];
    jsonDoc["unique_id"] = uniqueId;
    if (_ha.mqtt.type == HA_MQTT_GENERIC_JSON)
      jsonDoc["value_template"] = F("{{ value_json.F4L }}");

    // add entity type specific configuration
    if (ifFan4SwitchEntity)
    {
      jsonDoc["payload_off"] = F("SET+FN4L+0");
      jsonDoc["payload_on"] = F("SET+FN4L+1");
      jsonDoc["state_off"] = F("0");
      jsonDoc["state_on"] = F("1");
    }
    else
    {
      jsonDoc["command_template"] = F("SET+FN4L+{{ value }}");
      jsonDoc["min"] = FANLMINMAX[4];
      jsonDoc["max"] = FANLMINMAX[5];
      jsonDoc["mode"] = F("slider");
    }

    serializeJson(jsonDoc, payload);

    // publish
    _mqttMan.publish(topic.c_str(), payload.c_str(), true);

    // clean
    jsonDoc.clear();
    payload = "";
  }

  // TODO
  return true;
}

bool WebPalaControl::executePalaCmd(const String &cmd, String &strJson, bool publish /* = false*/)
{
  bool cmdProcessed = false;                                                             // cmd has been processed
  Palazzetti::CommandResult cmdSuccess = Palazzetti::CommandResult::COMMUNICATION_ERROR; // Palazzetti function calls successful

  // Prepare answer structure
  JsonDocument jsonDoc;
  JsonObject info = jsonDoc["INFO"].to<JsonObject>();
  JsonObject data = jsonDoc["DATA"].to<JsonObject>();

  // Parse parameters
  byte cmdParamNumber = 0;
  String strCmdParams[6];
  uint16_t cmdParams[6];

  if (cmd.length() > 9 && cmd[8] == ' ')
  {
    String cmdWorkingCopy = cmd.substring(9);
    cmdWorkingCopy.trim();

    // SET TIME special case
    if (cmd.startsWith(F("SET TIME ")))
    {
      cmdWorkingCopy.replace('-', ' ');
      cmdWorkingCopy.replace(':', ' ');
    }

    // SET STPF special case
    if (cmd.startsWith(F("SET STPF ")))
      cmdWorkingCopy.replace('.', ' ');

    while (cmdWorkingCopy.length() && cmdParamNumber < 6)
    {
      int pos = cmdWorkingCopy.indexOf(' ');
      if (pos == -1)
      {
        strCmdParams[cmdParamNumber] = cmdWorkingCopy;
        cmdWorkingCopy = "";
      }
      else
      {
        strCmdParams[cmdParamNumber] = cmdWorkingCopy.substring(0, pos);
        cmdWorkingCopy = cmdWorkingCopy.substring(pos + 1);
      }
      cmdParams[cmdParamNumber] = strCmdParams[cmdParamNumber].toInt();

      // verify convertion is successfull
      // ( copy strCmdParams and remove all 0 from the string, if convertion result is 0, then resulting string should be empty)
      String validation = strCmdParams[cmdParamNumber];
      validation.replace("0", "");
      if (cmdParams[cmdParamNumber] == 0 && validation.length())
      {
        cmdProcessed = true;
        info["MSG"] = String(F("Incorrect Parameter Value : ")) + strCmdParams[cmdParamNumber];
      }

      cmdParamNumber++;
    }

    // too much parameters has been sent
    if (cmdParamNumber == 6 && cmdWorkingCopy.length())
    {
      cmdProcessed = true;
      info["MSG"] = F("Incorrect Parameter Number");
    }
  }

  if (!cmdProcessed && cmd == F("GET STDT"))
  {
    cmdProcessed = true;

    char SN[28];
    byte SNCHK;
    int MBTYPE;
    uint16_t MOD, VER, CORE;
    char FWDATE[11];
    uint16_t FLUID;
    uint16_t SPLMIN, SPLMAX;
    byte UICONFIG;
    byte HWTYPE;
    byte DSPTYPE;
    byte DSPFWVER;
    byte CONFIG;
    byte PELLETTYPE;
    uint16_t PSENSTYPE;
    byte PSENSLMAX, PSENSLTSH, PSENSLMIN;
    byte MAINTPROBE;
    byte STOVETYPE;
    byte FAN2TYPE;
    byte FAN2MODE;
    byte BLEMBMODE;
    byte BLEDSPMODE;
    byte CHRONOTYPE;
    byte AUTONOMYTYPE;
    byte NOMINALPWR;
    cmdSuccess = _Pala.getStaticData(&SN, &SNCHK, &MBTYPE, &MOD, &VER, &CORE, &FWDATE, &FLUID, &SPLMIN, &SPLMAX, &UICONFIG, &HWTYPE, &DSPTYPE, &DSPFWVER, &CONFIG, &PELLETTYPE, &PSENSTYPE, &PSENSLMAX, &PSENSLTSH, &PSENSLMIN, &MAINTPROBE, &STOVETYPE, &FAN2TYPE, &FAN2MODE, &BLEMBMODE, &BLEDSPMODE, &CHRONOTYPE, &AUTONOMYTYPE, &NOMINALPWR);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      // ----- WPalaControl generated values -----
      #ifdef ESP8266
      data["LABEL"] = WiFi.hostname();
      #else
      data["LABEL"] = WiFi.getHostname();
      #endif
      // Network infos
      data["GWDEVICE"] = F("wlan0"); // always wifi
      data["MAC"] = WiFi.macAddress();
      data["GATEWAY"] = WiFi.gatewayIP().toString();
      data["DNS"][0] = WiFi.dnsIP().toString();

      // Wifi infos
      data["WMAC"] = WiFi.macAddress();
      data["WMODE"] = (WiFi.getMode() & WIFI_STA) ? F("sta") : F("ap");
      data["WADR"] = (WiFi.getMode() & WIFI_STA) ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
      data["WGW"] = WiFi.gatewayIP().toString();
      data["WENC"] = F("psk2");
      data["WPWR"] = String(WiFi.RSSI()) + F(" dBm"); // need conversion to dBm?
      data["WSSID"] = WiFi.SSID();
      data["WPR"] = (true) ? F("dhcp") : F("static");
      data["WMSK"] = WiFi.subnetMask().toString();
      #ifdef ESP8266
      #else
      data["WBCST"] = WiFi.broadcastIP().toString();
      #endif
      data["WCH"] = String(WiFi.channel());

      // Ethernet infos
      data["EPR"] = F("dhcp");
      data["EGW"] = F("0.0.0.0");
      data["EMSK"] = F("0.0.0.0");
      data["EADR"] = F("0.0.0.0");
      data["EMAC"] = WiFi.macAddress();
      data["ECBL"] = F("down");
      data["EBCST"] = "";

      data["APLCONN"] = 1; // appliance connected
      data["ICONN"] = 0;   // internet connected

      data["CBTYPE"] = F("miniembplug"); // CBox model
      data["sendmsg"] = F("2.1.2 2018-03-28 10:19:09");
      data["plzbridge"] = F("2.2.1 2022-10-24 11:13:21");
      data["SYSTEM"] = F("2.5.3 2021-10-08 10:30:20 (657c8cf)");

      data["CLOUD_ENABLED"] = true;

      // ----- Values from stove -----
      data["SN"] = SN;
      data["SNCHK"] = SNCHK;
      data["MBTYPE"] = MBTYPE;
      data["MOD"] = MOD;
      data["VER"] = VER;
      data["CORE"] = CORE;
      data["FWDATE"] = FWDATE;
      data["FLUID"] = FLUID;
      data["SPLMIN"] = SPLMIN;
      data["SPLMAX"] = SPLMAX;
      data["UICONFIG"] = UICONFIG;
      data["HWTYPE"] = HWTYPE;
      data["DSPTYPE"] = DSPTYPE;
      data["DSPFWVER"] = DSPFWVER;
      data["CONFIG"] = CONFIG;
      data["PELLETTYPE"] = PELLETTYPE;
      data["PSENSTYPE"] = PSENSTYPE;
      data["PSENSLMAX"] = PSENSLMAX;
      data["PSENSLTSH"] = PSENSLTSH;
      data["PSENSLMIN"] = PSENSLMIN;
      data["MAINTPROBE"] = MAINTPROBE;
      data["STOVETYPE"] = STOVETYPE;
      data["FAN2TYPE"] = FAN2TYPE;
      data["FAN2MODE"] = FAN2MODE;
      data["BLEMBMODE"] = BLEMBMODE;
      data["BLEDSPMODE"] = BLEDSPMODE;
      data["CHRONOTYPE"] = 0; // disable chronothermostat (no planning) (enabled if > 1)
      data["AUTONOMYTYPE"] = AUTONOMYTYPE;
      data["NOMINALPWR"] = NOMINALPWR;
    }
  }

  if (!cmdProcessed && cmd == F("GET LABL"))
  {
    cmdProcessed = true;
    cmdSuccess = Palazzetti::CommandResult::OK;

    #ifdef ESP8266
    data["LABEL"] = WiFi.hostname();
    #else
    data["LABEL"] = WiFi.getHostname();
    #endif
  }

  if (!cmdProcessed && cmd == F("GET ALLS"))
  {
    cmdProcessed = true;

    bool refreshStatus = false;
    unsigned long currentMillis = millis();
    if ((currentMillis - _lastAllStatusRefreshMillis) > 15000UL) // refresh AllStatus data if it's 15sec old
      refreshStatus = true;

    int MBTYPE;
    uint16_t MOD, VER, CORE;
    char FWDATE[11];
    char APLTS[20];
    uint16_t APLWDAY;
    byte CHRSTATUS;
    uint16_t STATUS, LSTATUS;
    bool isMFSTATUSValid;
    uint16_t MFSTATUS;
    float SETP;
    byte PUMP;
    uint16_t PQT;
    uint16_t F1V;
    uint16_t F1RPM;
    uint16_t F2L;
    uint16_t F2LF;
    uint16_t FANLMINMAX[6];
    uint16_t F2V;
    bool isF3LF4LValid;
    uint16_t F3L;
    uint16_t F4L;
    byte PWR;
    float FDR;
    uint16_t DPT;
    uint16_t DP;
    byte IN;
    byte OUT;
    float T1, T2, T3, T4, T5;
    bool isSNValid;
    char SN[28];
    cmdSuccess = _Pala.getAllStatus(refreshStatus, &MBTYPE, &MOD, &VER, &CORE, &FWDATE, &APLTS, &APLWDAY, &CHRSTATUS, &STATUS, &LSTATUS, &isMFSTATUSValid, &MFSTATUS, &SETP, &PUMP, &PQT, &F1V, &F1RPM, &F2L, &F2LF, &FANLMINMAX, &F2V, &isF3LF4LValid, &F3L, &F4L, &PWR, &FDR, &DPT, &DP, &IN, &OUT, &T1, &T2, &T3, &T4, &T5, &isSNValid, &SN);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      if (refreshStatus)
        _lastAllStatusRefreshMillis = currentMillis;

      data["MBTYPE"] = MBTYPE;
      data["MAC"] = WiFi.macAddress();
      data["MOD"] = MOD;
      data["VER"] = VER;
      data["CORE"] = CORE;
      data["FWDATE"] = FWDATE;
      data["APLTS"] = APLTS;
      data["APLWDAY"] = APLWDAY;
      data["CHRSTATUS"] = CHRSTATUS;
      data["STATUS"] = STATUS;
      data["LSTATUS"] = LSTATUS;
      if (isMFSTATUSValid)
        data["MFSTATUS"] = MFSTATUS;
      data["SETP"] = serialized(String(SETP, 2));
      data["PUMP"] = PUMP;
      data["PQT"] = PQT;
      data["F1V"] = F1V;
      data["F1RPM"] = F1RPM;
      data["F2L"] = F2L;
      data["F2LF"] = F2LF;
      JsonArray fanlminmax = data["FANLMINMAX"].to<JsonArray>();
      fanlminmax.add(FANLMINMAX[0]);
      fanlminmax.add(FANLMINMAX[1]);
      fanlminmax.add(FANLMINMAX[2]);
      fanlminmax.add(FANLMINMAX[3]);
      fanlminmax.add(FANLMINMAX[4]);
      fanlminmax.add(FANLMINMAX[5]);
      data["F2V"] = F2V;
      if (isF3LF4LValid)
      {
        data["F3L"] = F3L;
        data["F4L"] = F4L;
      }
      data["PWR"] = PWR;
      data["FDR"] = serialized(String(FDR, 2));
      data["DPT"] = DPT;
      data["DP"] = DP;
      data["IN"] = IN;
      data["OUT"] = OUT;
      data["T1"] = serialized(String(T1, 2));
      data["T2"] = serialized(String(T2, 2));
      data["T3"] = serialized(String(T3, 2));
      data["T4"] = serialized(String(T4, 2));
      data["T5"] = serialized(String(T5, 2));

      data["EFLAGS"] = 0; // new ErrorFlags not implemented
      if (isSNValid)
        data["SN"] = SN;
    }
  }

  if (!cmdProcessed && cmd == F("GET STAT"))
  {
    cmdProcessed = true;

    uint16_t STATUS, LSTATUS, FSTATUS;
    cmdSuccess = _Pala.getStatus(&STATUS, &LSTATUS, &FSTATUS);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["STATUS"] = STATUS;
      data["LSTATUS"] = LSTATUS;
      data["FSTATUS"] = FSTATUS;
    }
  }

  if (!cmdProcessed && cmd == F("GET TMPS"))
  {
    cmdProcessed = true;

    float T1, T2, T3, T4, T5;
    cmdSuccess = _Pala.getAllTemps(&T1, &T2, &T3, &T4, &T5);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["T1"] = serialized(String(T1, 2));
      data["T2"] = serialized(String(T2, 2));
      data["T3"] = serialized(String(T3, 2));
      data["T4"] = serialized(String(T4, 2));
      data["T5"] = serialized(String(T5, 2));
    }
  }

  if (!cmdProcessed && cmd == F("GET FAND"))
  {
    cmdProcessed = true;

    uint16_t F1V, F2V, F1RPM, F2L, F2LF;
    bool isF3SF4SValid;
    float F3S, F4S;
    bool isF3LF4LValid;
    uint16_t F3L, F4L;
    cmdSuccess = _Pala.getFanData(&F1V, &F2V, &F1RPM, &F2L, &F2LF, &isF3SF4SValid, &F3S, &F4S, &isF3LF4LValid, &F3L, &F4L);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["F1V"] = F1V;
      data["F2V"] = F2V;
      data["F1RPM"] = F1RPM;
      data["F2L"] = F2L;
      data["F2LF"] = F2LF;
      if (isF3SF4SValid)
      {
        data["F3S"] = serialized(String(F3S, 2));
        data["F4S"] = serialized(String(F4S, 2));
      }
      if (isF3LF4LValid)
      {
        data["F3L"] = F3L;
        data["F4L"] = F4L;
      }
    }
  }

  if (!cmdProcessed && cmd == F("GET SETP"))
  {
    cmdProcessed = true;

    float SETP;
    cmdSuccess = _Pala.getSetPoint(&SETP);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["SETP"] = serialized(String(SETP, 2));
    }
  }

  if (!cmdProcessed && cmd == F("GET POWR"))
  {
    cmdProcessed = true;

    byte PWR;
    float FDR;
    cmdSuccess = _Pala.getPower(&PWR, &FDR);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["PWR"] = PWR;
      data["FDR"] = serialized(String(FDR, 2));
    }
  }

  if (!cmdProcessed && (cmd == F("GET CUNT") || cmd == F("GET CNTR")))
  {
    cmdProcessed = true;

    uint16_t IGN, POWERTIMEh, POWERTIMEm, HEATTIMEh, HEATTIMEm, SERVICETIMEh, SERVICETIMEm, ONTIMEh, ONTIMEm, OVERTMPERRORS, IGNERRORS, PQT;
    cmdSuccess = _Pala.getCounters(&IGN, &POWERTIMEh, &POWERTIMEm, &HEATTIMEh, &HEATTIMEm, &SERVICETIMEh, &SERVICETIMEm, &ONTIMEh, &ONTIMEm, &OVERTMPERRORS, &IGNERRORS, &PQT);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["IGN"] = IGN;
      data["POWERTIME"] = String(POWERTIMEh) + ':' + (POWERTIMEm / 10) + (POWERTIMEm % 10);
      data["HEATTIME"] = String(HEATTIMEh) + ':' + (HEATTIMEm / 10) + (HEATTIMEm % 10);
      data["SERVICETIME"] = String(SERVICETIMEh) + ':' + (SERVICETIMEm / 10) + (SERVICETIMEm % 10);
      data["ONTIME"] = String(ONTIMEh) + ':' + (ONTIMEm / 10) + (ONTIMEm % 10);
      data["OVERTMPERRORS"] = OVERTMPERRORS;
      data["IGNERRORS"] = IGNERRORS;
      data["PQT"] = PQT;
    }
  }

  if (!cmdProcessed && cmd == F("GET DPRS"))
  {
    cmdProcessed = true;

    uint16_t DP_TARGET, DP_PRESS;
    cmdSuccess = _Pala.getDPressData(&DP_TARGET, &DP_PRESS);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["DP_TARGET"] = DP_TARGET;
      data["DP_PRESS"] = DP_PRESS;
    }
  }

  if (!cmdProcessed && cmd == F("GET TIME"))
  {
    cmdProcessed = true;

    char STOVE_DATETIME[20];
    byte STOVE_WDAY;
    cmdSuccess = _Pala.getDateTime(&STOVE_DATETIME, &STOVE_WDAY);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["STOVE_DATETIME"] = STOVE_DATETIME;
      data["STOVE_WDAY"] = STOVE_WDAY;
    }
  }

  if (!cmdProcessed && cmd == F("GET IOPT"))
  {
    cmdProcessed = true;

    byte IN_I01, IN_I02, IN_I03, IN_I04;
    byte OUT_O01, OUT_O02, OUT_O03, OUT_O04, OUT_O05, OUT_O06, OUT_O07;
    cmdSuccess = _Pala.getIO(&IN_I01, &IN_I02, &IN_I03, &IN_I04, &OUT_O01, &OUT_O02, &OUT_O03, &OUT_O04, &OUT_O05, &OUT_O06, &OUT_O07);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["IN_I01"] = IN_I01;
      data["IN_I02"] = IN_I02;
      data["IN_I03"] = IN_I03;
      data["IN_I04"] = IN_I04;
      data["OUT_O01"] = OUT_O01;
      data["OUT_O02"] = OUT_O02;
      data["OUT_O03"] = OUT_O03;
      data["OUT_O04"] = OUT_O04;
      data["OUT_O05"] = OUT_O05;
      data["OUT_O06"] = OUT_O06;
      data["OUT_O07"] = OUT_O07;
    }
  }

  if (!cmdProcessed && cmd == F("GET SERN"))
  {
    cmdProcessed = true;

    char SN[28];
    cmdSuccess = _Pala.getSN(&SN);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["SN"] = SN;
    }
  }

  if (!cmdProcessed && cmd == F("GET MDVE"))
  {
    cmdProcessed = true;

    uint16_t MOD, VER, CORE;
    char FWDATE[11];
    cmdSuccess = _Pala.getModelVersion(&MOD, &VER, &CORE, &FWDATE);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["MOD"] = MOD;
      data["VER"] = VER;
      data["CORE"] = CORE;
      data["FWDATE"] = FWDATE;
    }
  }

  if (!cmdProcessed && cmd == F("GET CHRD"))
  {
    cmdProcessed = true;

    byte CHRSTATUS;
    float PCHRSETP[6];
    byte PSTART[6][2];
    byte PSTOP[6][2];
    byte DM[7][3];
    cmdSuccess = _Pala.getChronoData(&CHRSTATUS, &PCHRSETP, &PSTART, &PSTOP, &DM);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["CHRSTATUS"] = CHRSTATUS;

      // Add Programs (P1->P6)
      char programName[3] = {'P', 'X', 0};
      char time[6] = {'0', '0', ':', '0', '0', 0};
      for (byte i = 0; i < 6; i++)
      {
        programName[1] = i + '1';
        JsonObject px = data[programName].to<JsonObject>();
        px["CHRSETP"] = serialized(String(PCHRSETP[i], 2));
        time[0] = PSTART[i][0] / 10 + '0';
        time[1] = PSTART[i][0] % 10 + '0';
        time[3] = PSTART[i][1] / 10 + '0';
        time[4] = PSTART[i][1] % 10 + '0';
        px["START"] = time;
        time[0] = PSTOP[i][0] / 10 + '0';
        time[1] = PSTOP[i][0] % 10 + '0';
        time[3] = PSTOP[i][1] / 10 + '0';
        time[4] = PSTOP[i][1] % 10 + '0';
        px["STOP"] = time;
      }

      // Add Days (D1->D7)
      char dayName[3] = {'D', 'X', 0};
      char memoryName[3] = {'M', 'X', 0};
      for (byte dayNumber = 0; dayNumber < 7; dayNumber++)
      {
        dayName[1] = dayNumber + '1';
        JsonObject dx = data[dayName].to<JsonObject>();
        for (byte memoryNumber = 0; memoryNumber < 3; memoryNumber++)
        {
          memoryName[1] = memoryNumber + '1';
          if (DM[dayNumber][memoryNumber])
          {
            programName[1] = DM[dayNumber][memoryNumber] + '0';
            dx[memoryName] = programName;
          }
          else
            dx[memoryName] = F("OFF");
        }
      }
    }
  }

  if (!cmdProcessed && cmd.startsWith(F("GET PARM ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 1)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
    {
      byte paramValue;
      cmdSuccess = _Pala.getParameter(cmdParams[0], &paramValue);

      if (cmdSuccess == Palazzetti::CommandResult::OK)
      {
        String paramName("PAR");
        paramName += cmdParams[0];
        data[paramName] = paramValue;
      }
    }
  }

  if (!cmdProcessed && cmd.startsWith(F("GET HPAR ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 1)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
    {
      uint16_t hiddenParamValue;
      cmdSuccess = _Pala.getHiddenParameter(cmdParams[0], &hiddenParamValue);

      if (cmdSuccess == Palazzetti::CommandResult::OK)
      {
        String hiddenParamName("HPAR");
        hiddenParamName += cmdParams[0];
        data[hiddenParamName] = hiddenParamValue;
      }
    }
  }

  if (!cmdProcessed && cmd == F("CMD ON"))
  {
    cmdProcessed = true;

    uint16_t STATUS, LSTATUS, FSTATUS;
    cmdSuccess = _Pala.switchOn(&STATUS, &LSTATUS, &FSTATUS);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["STATUS"] = STATUS;
      data["LSTATUS"] = LSTATUS;
      data["FSTATUS"] = FSTATUS;
    }
  }

  if (!cmdProcessed && cmd == F("CMD OFF"))
  {
    cmdProcessed = true;

    uint16_t STATUS, LSTATUS, FSTATUS;
    cmdSuccess = _Pala.switchOff(&STATUS, &LSTATUS, &FSTATUS);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["STATUS"] = STATUS;
      data["LSTATUS"] = LSTATUS;
      data["FSTATUS"] = FSTATUS;
    }
  }

  if (!cmdProcessed && cmd.startsWith(F("SET POWR ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 1)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
    {
      byte PWRReturn;
      bool isF2LReturnValid;
      uint16_t _F2LReturn;
      uint16_t FANLMINMAXReturn[6];
      cmdSuccess = _Pala.setPower(cmdParams[0], &PWRReturn, &isF2LReturnValid, &_F2LReturn, &FANLMINMAXReturn);

      if (cmdSuccess == Palazzetti::CommandResult::OK)
      {
        data["PWR"] = PWRReturn;
        if (isF2LReturnValid)
          data["F2L"] = _F2LReturn;
        JsonArray fanlminmax = data["FANLMINMAX"].to<JsonArray>();
        fanlminmax.add(FANLMINMAXReturn[0]);
        fanlminmax.add(FANLMINMAXReturn[1]);
        fanlminmax.add(FANLMINMAXReturn[2]);
        fanlminmax.add(FANLMINMAXReturn[3]);
        fanlminmax.add(FANLMINMAXReturn[4]);
        fanlminmax.add(FANLMINMAXReturn[5]);
      }
    }
  }

  if (!cmdProcessed && cmd == F("SET PWRU"))
  {
    cmdProcessed = true;

    byte PWRReturn;
    bool isF2LReturnValid;
    uint16_t _F2LReturn;
    uint16_t FANLMINMAXReturn[6];
    cmdSuccess = _Pala.setPowerUp(&PWRReturn, &isF2LReturnValid, &_F2LReturn, &FANLMINMAXReturn);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["PWR"] = PWRReturn;
      if (isF2LReturnValid)
        data["F2L"] = _F2LReturn;
      JsonArray fanlminmax = data["FANLMINMAX"].to<JsonArray>();
      fanlminmax.add(FANLMINMAXReturn[0]);
      fanlminmax.add(FANLMINMAXReturn[1]);
      fanlminmax.add(FANLMINMAXReturn[2]);
      fanlminmax.add(FANLMINMAXReturn[3]);
      fanlminmax.add(FANLMINMAXReturn[4]);
      fanlminmax.add(FANLMINMAXReturn[5]);
    }
  }

  if (!cmdProcessed && cmd == F("SET PWRD"))
  {
    cmdProcessed = true;

    byte PWRReturn;
    bool isF2LReturnValid;
    uint16_t _F2LReturn;
    uint16_t FANLMINMAXReturn[6];
    cmdSuccess = _Pala.setPowerDown(&PWRReturn, &isF2LReturnValid, &_F2LReturn, &FANLMINMAXReturn);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["PWR"] = PWRReturn;
      if (isF2LReturnValid)
        data["F2L"] = _F2LReturn;
      JsonArray fanlminmax = data["FANLMINMAX"].to<JsonArray>();
      fanlminmax.add(FANLMINMAXReturn[0]);
      fanlminmax.add(FANLMINMAXReturn[1]);
      fanlminmax.add(FANLMINMAXReturn[2]);
      fanlminmax.add(FANLMINMAXReturn[3]);
      fanlminmax.add(FANLMINMAXReturn[4]);
      fanlminmax.add(FANLMINMAXReturn[5]);
    }
  }

  if (!cmdProcessed && cmd.startsWith(F("SET TIME ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 6)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    // Check if date is valid
    // basic control
    if (cmdParams[0] < 2000 || cmdParams[0] > 2099)
      info["MSG"] = F("Incorrect Year");
    else if (cmdParams[1] < 1 || cmdParams[1] > 12)
      info["MSG"] = F("Incorrect Month");
    else if ((cmdParams[2] < 1 || cmdParams[2] > 31) ||
             ((cmdParams[2] == 4 || cmdParams[2] == 6 || cmdParams[2] == 9 || cmdParams[2] == 11) && cmdParams[3] > 30) ||                        // 30 days month control
             (cmdParams[2] == 2 && cmdParams[3] > 29) ||                                                                                          // February leap year control
             (cmdParams[2] == 2 && cmdParams[3] == 29 && !(((cmdParams[0] % 4 == 0) && (cmdParams[0] % 100 != 0)) || (cmdParams[0] % 400 == 0)))) // February not leap year control
      info["MSG"] = F("Incorrect Day");
    else if (cmdParams[3] > 23)
      info["MSG"] = F("Incorrect Hour");
    else if (cmdParams[4] > 59)
      info["MSG"] = F("Incorrect Minute");
    else if (cmdParams[5] > 59)
      info["MSG"] = F("Incorrect Second");

    if (info["MSG"].isNull())
    {
      char STOVE_DATETIMEReturn[20];
      byte STOVE_WDAYReturn;
      cmdSuccess = _Pala.setDateTime(cmdParams[0], cmdParams[1], cmdParams[2], cmdParams[3], cmdParams[4], cmdParams[5], &STOVE_DATETIMEReturn, &STOVE_WDAYReturn);

      if (cmdSuccess == Palazzetti::CommandResult::OK)
      {
        data["STOVE_DATETIME"] = STOVE_DATETIMEReturn;
        data["STOVE_WDAY"] = STOVE_WDAYReturn;
      }
    }
  }

  if (!cmdProcessed && cmd.startsWith(F("SET RFAN ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 1)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
    {
      bool isPWRReturnValid;
      byte PWRReturn;
      uint16_t F2LReturn;
      uint16_t F2LFReturn;
      cmdSuccess = _Pala.setRoomFan(cmdParams[0], &isPWRReturnValid, &PWRReturn, &F2LReturn, &F2LFReturn);

      if (cmdSuccess == Palazzetti::CommandResult::OK)
      {
        if (isPWRReturnValid)
          data["PWR"] = PWRReturn;
        data["F2L"] = F2LReturn;
        data["F2LF"] = F2LFReturn;
      }
    }
  }

  if (!cmdProcessed && cmd == F("SET FN2U"))
  {
    cmdProcessed = true;

    bool isPWRReturnValid;
    byte PWRReturn;
    uint16_t F2LReturn;
    uint16_t F2LFReturn;
    cmdSuccess = _Pala.setRoomFanUp(&isPWRReturnValid, &PWRReturn, &F2LReturn, &F2LFReturn);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      if (isPWRReturnValid)
        data["PWR"] = PWRReturn;
      data["F2L"] = F2LReturn;
      data["F2LF"] = F2LFReturn;
    }
  }

  if (!cmdProcessed && cmd == F("SET FN2D"))
  {
    cmdProcessed = true;

    bool isPWRReturnValid;
    byte PWRReturn;
    uint16_t F2LReturn;
    uint16_t F2LFReturn;
    cmdSuccess = _Pala.setRoomFanDown(&isPWRReturnValid, &PWRReturn, &F2LReturn, &F2LFReturn);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      if (isPWRReturnValid)
        data["PWR"] = PWRReturn;
      data["F2L"] = F2LReturn;
      data["F2LF"] = F2LFReturn;
    }
  }

  if (!cmdProcessed && cmd.startsWith(F("SET FN3L ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 1)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
    {
      uint16_t F3LReturn;
      cmdSuccess = _Pala.setRoomFan3(cmdParams[0], &F3LReturn);

      if (cmdSuccess == Palazzetti::CommandResult::OK)
      {
        data["F3L"] = F3LReturn;
      }
    }
  }

  if (!cmdProcessed && cmd.startsWith(F("SET FN4L ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 1)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
    {
      uint16_t F4LReturn;
      cmdSuccess = _Pala.setRoomFan4(cmdParams[0], &F4LReturn);

      if (cmdSuccess == Palazzetti::CommandResult::OK)
      {
        data["F4L"] = F4LReturn;
      }
    }
  }

  if (!cmdProcessed && cmd.startsWith(F("SET SLNT ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 1)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
    {
      byte SLNTReturn;
      byte PWRReturn;
      uint16_t F2LReturn;
      uint16_t F2LFReturn;
      bool isF3LF4LReturnValid;
      uint16_t F3LReturn;
      uint16_t F4LReturn;
      cmdSuccess = _Pala.setSilentMode(cmdParams[0], &SLNTReturn, &PWRReturn, &F2LReturn, &F2LFReturn, &isF3LF4LReturnValid, &F3LReturn, &F4LReturn);

      if (cmdSuccess == Palazzetti::CommandResult::OK)
      {
        data["SLNT"] = SLNTReturn;
        data["PWR"] = PWRReturn;
        data["F2L"] = F2LReturn;
        data["F2LF"] = F2LFReturn;
        if (isF3LF4LReturnValid)
        {
          data["F3L"] = F3LReturn;
          data["F4L"] = F4LReturn;
        }
      }
    }
  }

  if (!cmdProcessed && cmd.startsWith(F("SET CSST ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 1)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
    {
      byte CHRSTATUSReturn;
      cmdSuccess = _Pala.setChronoStatus(cmdParams[0], &CHRSTATUSReturn);

      if (cmdSuccess == Palazzetti::CommandResult::OK)
      {
        data["CHRSTATUS"] = CHRSTATUSReturn;
      }
    }
  }

  if (!cmdProcessed && cmd.startsWith(F("SET CSTH ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 2)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
      cmdSuccess = _Pala.setChronoStartHH(cmdParams[0], cmdParams[1]);
  }

  if (!cmdProcessed && cmd.startsWith(F("SET CSTM ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 2)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
      cmdSuccess = _Pala.setChronoStartMM(cmdParams[0], cmdParams[1]);
  }

  if (!cmdProcessed && cmd.startsWith(F("SET CSPH ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 2)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
      cmdSuccess = _Pala.setChronoStopHH(cmdParams[0], cmdParams[1]);
  }

  if (!cmdProcessed && cmd.startsWith(F("SET CSPM ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 2)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
      cmdSuccess = _Pala.setChronoStopMM(cmdParams[0], cmdParams[1]);
  }

  if (!cmdProcessed && cmd.startsWith(F("SET CSET ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 2)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
      cmdSuccess = _Pala.setChronoSetpoint(cmdParams[0], cmdParams[1]);
  }

  if (!cmdProcessed && cmd.startsWith(F("SET CDAY ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 3)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
    {
      cmdSuccess = _Pala.setChronoDay(cmdParams[0], cmdParams[1], cmdParams[2]);

      if (cmdSuccess == Palazzetti::CommandResult::OK)
      {
        char dayName[3] = {'D', 'X', 0};
        char memoryName[3] = {'M', 'X', 0};
        char programName[3] = {'P', 'X', 0};

        dayName[1] = cmdParams[0] + '0';
        memoryName[1] = cmdParams[1] + '0';
        programName[1] = cmdParams[2] + '0';

        JsonObject dx = data[dayName].to<JsonObject>();
        if (cmdParams[2])
          dx[memoryName] = programName;
        else
          dx[memoryName] = F("OFF");
      }
    }
  }

  if (!cmdProcessed && cmd.startsWith(F("SET CPRD ")))
  {
    cmdProcessed = true;

    if (info["MSG"].isNull())
    {
      cmdSuccess = _Pala.setChronoPrg(cmdParams[0], cmdParams[1], cmdParams[2], cmdParams[3], cmdParams[4], cmdParams[5]);

      if (cmdSuccess == Palazzetti::CommandResult::OK)
      {
        char programName[3] = {'P', 'X', 0};
        char time[6] = {'0', '0', ':', '0', '0', 0};

        programName[1] = cmdParams[0] + '0';
        JsonObject px = data[programName].to<JsonObject>();
        px["CHRSETP"] = (float)cmdParams[1];
        time[0] = cmdParams[2] / 10 + '0';
        time[1] = cmdParams[2] % 10 + '0';
        time[3] = cmdParams[3] / 10 + '0';
        time[4] = cmdParams[3] % 10 + '0';
        px["START"] = time;
        time[0] = cmdParams[4] / 10 + '0';
        time[1] = cmdParams[4] % 10 + '0';
        time[3] = cmdParams[5] / 10 + '0';
        time[4] = cmdParams[5] % 10 + '0';
        px["STOP"] = time;
      }
    }
  }

  if (!cmdProcessed && cmd.startsWith(F("SET SETP ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 1)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
    {
      float SETPReturn;
      cmdSuccess = _Pala.setSetpoint((byte)cmdParams[0], &SETPReturn);

      if (cmdSuccess == Palazzetti::CommandResult::OK)
      {
        data["SETP"] = serialized(String(SETPReturn, 2));
      }
    }
  }

  if (!cmdProcessed && cmd == F("SET STPU"))
  {
    cmdProcessed = true;

    float SETPReturn;
    cmdSuccess = _Pala.setSetPointUp(&SETPReturn);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["SETP"] = serialized(String(SETPReturn, 2));
    }
  }

  if (!cmdProcessed && cmd == F("SET STPD"))
  {
    cmdProcessed = true;

    float SETPReturn;
    cmdSuccess = _Pala.setSetPointDown(&SETPReturn);

    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      data["SETP"] = serialized(String(SETPReturn, 2));
    }
  }

  if (!cmdProcessed && cmd.startsWith(F("SET STPF ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 2)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;
    else if (cmdParams[1] > 80 || cmdParams[1] % 20 != 0)
      info["MSG"] = String(F("Incorrect Parameter Value : ")) + strCmdParams[0] + '.' + strCmdParams[1];

    // convert splitted float string back to float
    float setPointFloat = cmdParams[1]; // load decimal part
    setPointFloat /= 100.0f;
    setPointFloat += cmdParams[0]; // load integer part

    if (info["MSG"].isNull())
    {
      float SETPReturn;
      cmdSuccess = _Pala.setSetpoint(setPointFloat, &SETPReturn);

      if (cmdSuccess == Palazzetti::CommandResult::OK)
      {
        data["SETP"] = serialized(String(SETPReturn, 2));
      }
    }
  }

  if (!cmdProcessed && cmd.startsWith(F("SET PARM ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 2)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
    {
      cmdSuccess = _Pala.setParameter(cmdParams[0], cmdParams[1]);

      if (cmdSuccess == Palazzetti::CommandResult::OK)
      {
        data[String(F("PAR")) + cmdParams[0]] = cmdParams[1];
      }
    }
  }

  if (!cmdProcessed && cmd.startsWith(F("SET HPAR ")))
  {
    cmdProcessed = true;

    if (cmdParamNumber != 2)
      info["MSG"] = String(F("Incorrect Parameter Number : ")) + cmdParamNumber;

    if (info["MSG"].isNull())
    {
      cmdSuccess = _Pala.setHiddenParameter(cmdParams[0], cmdParams[1]);

      if (cmdSuccess == Palazzetti::CommandResult::OK)
      {
        data[String(F("HPAR")) + cmdParams[0]] = cmdParams[1];
      }
    }
  }

  // if command has been processed
  if (cmdProcessed)
  {

    // if MQTT protocol is enabled then update connected topic to reflect stove connectivity
    if (_ha.protocol == HA_PROTO_MQTT)
      publishStoveConnectedToMqtt(cmdSuccess == Palazzetti::CommandResult::OK);

    // if communication with stove was successful
    if (cmdSuccess == Palazzetti::CommandResult::OK)
    {
      info["CMD"] = cmd.substring(0, 8);

      info["RSP"] = F("OK");
      jsonDoc["SUCCESS"] = true;

      if (publish)
      {
        String strData;
        serializeJson(data, strData);
        statusEventSourceBroadcast(strData);

        String baseTopic = _ha.mqtt.generic.baseTopic;
        MQTTMan::prepareTopic(baseTopic);

        if (_ha.protocol == HA_PROTO_MQTT && _haSendResult)
        {
          _haSendResult &= publishDataToMqtt(baseTopic, cmd.substring(4), jsonDoc);
        }
      }
    }
    else
    {
      info["CMD"] = cmd;

      // if there is no MSG in info then stove communication failed
      if (info["MSG"].isNull())
      {
        info["RSP"] = F("TIMEOUT");
        info["MSG"] = F("Stove communication failed");
      }
      else
        info["RSP"] = F("ERROR");

      jsonDoc["SUCCESS"] = false;
      data["NODATA"] = true;
    }
  }
  else
  {
    // command is unknown and not processed
    info["RSP"] = F("ERROR");
    info["CMD"] = F("UNKNOWN");
    info["MSG"] = F("No valid request received");
    jsonDoc["SUCCESS"] = false;
    data["NODATA"] = true;
  }

  // serialize result to the provided strJson
  serializeJson(jsonDoc, strJson);

  return jsonDoc["SUCCESS"].as<bool>();
}

void WebPalaControl::publishTick()
{
  LOG_SERIAL.println(F("PublishTick"));

  // array of commands to execute
  const char *cmdList[] = {
      "GET STAT",
      "GET TMPS",
      "GET FAND",
      "GET CNTR",
      "GET TIME",
      "GET SETP",
      "GET POWR",
      "GET DPRS"};

  // initialize _haSendResult for publish session
  _haSendResult = true;

  // execute commands
  for (const char *cmd : cmdList)
  {
    String strJson;
    // execute command with publish flag to true
    if (!executePalaCmd(cmd, strJson, true))
      break;
  }
}

void WebPalaControl::udpRequestHandler(WiFiUDP &udpServer)
{

  int packetSize = udpServer.parsePacket();
  if (packetSize <= 0)
    return;

  String strData;
  String strAnswer;

  strData.reserve(packetSize + 1);

  // while udpServer.read() do not return -1, get returned value and add it to strData
  int bufferByte;
  while ((bufferByte = udpServer.read()) >= 0)
    strData += (char)bufferByte;

  // process request
  if (strData.endsWith(F("bridge?")))
    executePalaCmd(F("GET STDT"), strAnswer);
  else if (strData.endsWith(F("bridge?GET ALLS")))
    executePalaCmd(F("GET ALLS"), strAnswer);
  else
    executePalaCmd("", strAnswer);

  // answer to the requester
  udpServer.beginPacket(udpServer.remoteIP(), udpServer.remotePort());
  udpServer.write((const uint8_t *)strAnswer.c_str(), strAnswer.length());
  udpServer.endPacket();
}

//------------------------------------------
// Used to initialize configuration properties to default values
void WebPalaControl::setConfigDefaultValues()
{
  _ha.protocol = HA_PROTO_DISABLED;
  _ha.hostname[0] = 0;
  _ha.uploadPeriod = 60;

  _ha.mqtt.type = HA_MQTT_GENERIC_JSON;
  _ha.mqtt.port = 1883;
  _ha.mqtt.username[0] = 0;
  _ha.mqtt.password[0] = 0;
  strcpy_P(_ha.mqtt.generic.baseTopic, PSTR("$model$"));
  _ha.mqtt.hassDiscoveryEnabled = true;
  strcpy_P(_ha.mqtt.hassDiscoveryPrefix, PSTR("homeassistant"));
}

//------------------------------------------
// Parse JSON object into configuration properties
void WebPalaControl::parseConfigJSON(JsonDocument &doc)
{
  if (!doc[F("haproto")].isNull())
    _ha.protocol = doc[F("haproto")];
  if (!doc[F("hahost")].isNull())
    strlcpy(_ha.hostname, doc[F("hahost")], sizeof(_ha.hostname));
  if (!doc[F("haupperiod")].isNull())
    _ha.uploadPeriod = doc[F("haupperiod")];

  if (!doc[F("hamtype")].isNull())
    _ha.mqtt.type = doc[F("hamtype")];
  if (!doc[F("hamport")].isNull())
    _ha.mqtt.port = doc[F("hamport")];
  if (!doc[F("hamu")].isNull())
    strlcpy(_ha.mqtt.username, doc[F("hamu")], sizeof(_ha.mqtt.username));
  if (!doc[F("hamp")].isNull())
    strlcpy(_ha.mqtt.password, doc[F("hamp")], sizeof(_ha.mqtt.password));

  if (!doc[F("hamgbt")].isNull())
    strlcpy(_ha.mqtt.generic.baseTopic, doc[F("hamgbt")], sizeof(_ha.mqtt.generic.baseTopic));

  if (!doc[F("hamhassde")].isNull())
    _ha.mqtt.hassDiscoveryEnabled = doc[F("hamhassde")];

  if (!doc[F("hamhassdp")].isNull())
    strlcpy(_ha.mqtt.hassDiscoveryPrefix, doc[F("hamhassdp")], sizeof(_ha.mqtt.hassDiscoveryPrefix));
}

//------------------------------------------
// Parse HTTP POST parameters in request into configuration properties
bool WebPalaControl::parseConfigWebRequest(WebServer &server)
{

  // Parse HA protocol
  if (server.hasArg(F("haproto")))
    _ha.protocol = server.arg(F("haproto")).toInt();

  // if an home Automation protocol has been selected then get common param
  if (_ha.protocol != HA_PROTO_DISABLED)
  {
    if (server.hasArg(F("hahost")) && server.arg(F("hahost")).length() < sizeof(_ha.hostname))
      strcpy(_ha.hostname, server.arg(F("hahost")).c_str());
    if (server.hasArg(F("haupperiod")))
      _ha.uploadPeriod = server.arg(F("haupperiod")).toInt();
  }

  // Now get specific param
  switch (_ha.protocol)
  {

  case HA_PROTO_MQTT:

    if (server.hasArg(F("hamtype")))
      _ha.mqtt.type = server.arg(F("hamtype")).toInt();
    if (server.hasArg(F("hamport")))
      _ha.mqtt.port = server.arg(F("hamport")).toInt();
    if (server.hasArg(F("hamu")) && server.arg(F("hamu")).length() < sizeof(_ha.mqtt.username))
      strcpy(_ha.mqtt.username, server.arg(F("hamu")).c_str());
    char tempPassword[64 + 1] = {0};
    // put MQTT password into temporary one for predefpassword
    if (server.hasArg(F("hamp")) && server.arg(F("hamp")).length() < sizeof(tempPassword))
      strcpy(tempPassword, server.arg(F("hamp")).c_str());
    // check for previous password (there is a predefined special password that mean to keep already saved one)
    if (strcmp_P(tempPassword, appDataPredefPassword))
      strcpy(_ha.mqtt.password, tempPassword);

    switch (_ha.mqtt.type)
    {
    case HA_MQTT_GENERIC:
    case HA_MQTT_GENERIC_JSON:
    case HA_MQTT_GENERIC_CATEGORIZED:
      if (server.hasArg(F("hamgbt")) && server.arg(F("hamgbt")).length() < sizeof(_ha.mqtt.generic.baseTopic))
        strcpy(_ha.mqtt.generic.baseTopic, server.arg(F("hamgbt")).c_str());

      if (!_ha.hostname[0] || !_ha.mqtt.generic.baseTopic[0])
        _ha.protocol = HA_PROTO_DISABLED;
      break;
    }

    if (server.hasArg(F("hamhassde")))
      _ha.mqtt.hassDiscoveryEnabled = (server.arg(F("hamhassde")) == F("on"));
    else
      _ha.mqtt.hassDiscoveryEnabled = false;

    if (server.hasArg(F("hamhassdp")) && server.arg(F("hamhassdp")).length() < sizeof(_ha.mqtt.hassDiscoveryPrefix))
      strcpy(_ha.mqtt.hassDiscoveryPrefix, server.arg(F("hamhassdp")).c_str());

    break;
  }
  return true;
}

//------------------------------------------
// Generate JSON from configuration properties
String WebPalaControl::generateConfigJSON(bool forSaveFile = false)
{
  String gc('{');

  gc = gc + F("\"haproto\":") + _ha.protocol;
  gc = gc + F(",\"hahost\":\"") + _ha.hostname + '"';
  gc = gc + F(",\"haupperiod\":") + _ha.uploadPeriod;

  // if for WebPage or protocol selected is MQTT
  if (!forSaveFile || _ha.protocol == HA_PROTO_MQTT)
  {
    gc = gc + F(",\"hamtype\":") + _ha.mqtt.type;
    gc = gc + F(",\"hamport\":") + _ha.mqtt.port;
    gc = gc + F(",\"hamu\":\"") + _ha.mqtt.username + '"';
    if (forSaveFile)
      gc = gc + F(",\"hamp\":\"") + _ha.mqtt.password + '"';
    else
      gc = gc + F(",\"hamp\":\"") + (__FlashStringHelper *)appDataPredefPassword + '"'; // predefined special password (mean to keep already saved one)

    gc = gc + F(",\"hamgbt\":\"") + _ha.mqtt.generic.baseTopic + '"';

    gc = gc + F(",\"hamhassde\":") + _ha.mqtt.hassDiscoveryEnabled;

    gc = gc + F(",\"hamhassdp\":\"") + _ha.mqtt.hassDiscoveryPrefix + '"';
  }

  gc += '}';

  return gc;
}

//------------------------------------------
// Generate JSON of application status
String WebPalaControl::generateStatusJSON()
{
  String gs('{');

  char SN[28];
  if (_Pala.getSN(&SN) == Palazzetti::CommandResult::OK)
    gs = gs + F("\"liveData\":{\"SN\":\"") + SN + F("\"}");
  else
    gs = gs + F("\"liveData\":{\"MSG\":\"Stove communication failed! please check cabling to your stove.\"}");

  gs = gs + F(",\"has1\":\"");
  switch (_ha.protocol)
  {
  case HA_PROTO_DISABLED:
    gs = gs + F("Disabled");
    break;
  case HA_PROTO_MQTT:
    gs = gs + F("MQTT Connection State : ");
    switch (_mqttMan.state())
    {
    case MQTT_CONNECTION_TIMEOUT:
      gs = gs + F("Timed Out");
      break;
    case MQTT_CONNECTION_LOST:
      gs = gs + F("Lost");
      break;
    case MQTT_CONNECT_FAILED:
      gs = gs + F("Failed");
      break;
    case MQTT_CONNECTED:
      gs = gs + F("Connected");
      break;
    case MQTT_CONNECT_BAD_PROTOCOL:
      gs = gs + F("Bad Protocol Version");
      break;
    case MQTT_CONNECT_BAD_CLIENT_ID:
      gs = gs + F("Incorrect ClientID ");
      break;
    case MQTT_CONNECT_UNAVAILABLE:
      gs = gs + F("Server Unavailable");
      break;
    case MQTT_CONNECT_BAD_CREDENTIALS:
      gs = gs + F("Bad Credentials");
      break;
    case MQTT_CONNECT_UNAUTHORIZED:
      gs = gs + F("Connection Unauthorized");
      break;
    }

    if (_mqttMan.state() == MQTT_CONNECTED)
      gs = gs + F("\",\"has2\":\"Last Publish Result : ") + (_haSendResult ? F("OK") : F("Failed"));

    break;
  }
  gs += '"';

  gs += '}';

  return gs;
}

//------------------------------------------
// code to execute during initialization and reinitialization of the app
bool WebPalaControl::appInit(bool reInit)
{
  // Stop UdpServer
  _udpServer.stop();

  // Stop Publish
  _publishTicker.detach();

  // Stop MQTT
  _mqttMan.disconnect();

  // if MQTT used so configure it
  if (_ha.protocol == HA_PROTO_MQTT)
  {
    // prepare will topic
    String willTopic = _ha.mqtt.generic.baseTopic;
    MQTTMan::prepareTopic(willTopic);
    willTopic += F("connected");

    // setup MQTT
    _mqttMan.setBufferSize(1600); // max JSON size (STDT ~1100 but STATUS_text HAss discovery ~1600)
    _mqttMan.setClient(_wifiClient).setServer(_ha.hostname, _ha.mqtt.port);
    _mqttMan.setConnectedAndWillTopic(willTopic.c_str());
    _mqttMan.setConnectedCallback(std::bind(&WebPalaControl::mqttConnectedCallback, this, std::placeholders::_1, std::placeholders::_2));
    _mqttMan.setDisconnectedCallback(std::bind(&WebPalaControl::mqttDisconnectedCallback, this));
    _mqttMan.setCallback(std::bind(&WebPalaControl::mqttCallback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    // Connect
    _mqttMan.connect(_ha.mqtt.username, _ha.mqtt.password);
  }

  LOG_SERIAL.println(F("Connecting to Stove..."));

  Palazzetti::CommandResult cmdRes;
  cmdRes = _Pala.initialize(
      std::bind(&WebPalaControl::myOpenSerial, this, std::placeholders::_1),
      std::bind(&WebPalaControl::myCloseSerial, this),
      std::bind(&WebPalaControl::mySelectSerial, this, std::placeholders::_1),
      std::bind(&WebPalaControl::myReadSerial, this, std::placeholders::_1, std::placeholders::_2),
      std::bind(&WebPalaControl::myWriteSerial, this, std::placeholders::_1, std::placeholders::_2),
      std::bind(&WebPalaControl::myDrainSerial, this),
      std::bind(&WebPalaControl::myFlushSerial, this),
      std::bind(&WebPalaControl::myUSleep, this, std::placeholders::_1));

  if (cmdRes == Palazzetti::CommandResult::OK)
  {
    LOG_SERIAL.println(F("Stove connected"));
    char SN[28];
    _Pala.getSN(&SN);
    LOG_SERIAL.print(F("Stove Serial Number: "));
    LOG_SERIAL.println(SN);
  }
  else
  {
    LOG_SERIAL.println(F("Stove connection failed"));
  }

  if (cmdRes == Palazzetti::CommandResult::OK)
    publishTick(); // if configuration changed, publish immediately

#ifdef ESP8266
  _publishTicker.attach(_ha.uploadPeriod, [this]()
                        { this->_needPublish = true; });
#else
  _publishTicker.attach<typeof this>(_ha.uploadPeriod, [](typeof this palaControl)
                                     { palaControl->_needPublish = true; }, this);
#endif

  // Start UDP Server
  _udpServer.begin(54549);

  return cmdRes == Palazzetti::CommandResult::OK;
}

//------------------------------------------
// Return HTML Code to insert into Status Web page
const PROGMEM char *WebPalaControl::getHTMLContent(WebPageForPlaceHolder wp)
{
  switch (wp)
  {
  case status:
    return status1htmlgz;
    break;
  case config:
    return config1htmlgz;
    break;
  default:
    return nullptr;
    break;
  };
  return nullptr;
}

// and his Size
size_t WebPalaControl::getHTMLContentSize(WebPageForPlaceHolder wp)
{
  switch (wp)
  {
  case status:
    return sizeof(status1htmlgz);
    break;
  case config:
    return sizeof(config1htmlgz);
    break;
  default:
    return 0;
    break;
  };
  return 0;
}

//------------------------------------------
// code to register web request answer to the web server
void WebPalaControl::appInitWebServer(WebServer &server, bool &shouldReboot, bool &pauseApplication)
{
  // Handle HTTP GET requests
  server.on(F("/cgi-bin/sendmsg.lua"), HTTP_GET, [this, &server]()
            {
    String cmd;
    String strJson;

    if (server.hasArg(F("cmd"))) cmd = server.arg(F("cmd"));

    // WPalaControl specific command
    if (cmd.startsWith(F("BKP PARM ")))
    {
      byte fileType;

      String strFileType(cmd.substring(9));

      if (strFileType == F("CSV"))
        fileType = 0;
      else if (strFileType == F("JSON"))
        fileType = 1;
      else
      {
        String ret(F("{\"INFO\":{\"CMD\":\"BKP PARM\",\"MSG\":\"Incorrect File Type : "));
        ret += strFileType;
        ret += F("\"},\"SUCCESS\":false,\"DATA\":{\"NODATA\":true}}");
        SERVER_KEEPALIVE_FALSE()
        server.send(200, F("text/json"), ret);
        return;
      }

      byte params[0x6A];
      Palazzetti::CommandResult cmdRes = _Pala.getAllParameters(&params);

      if (cmdRes == Palazzetti::CommandResult::OK)
      {
        String toReturn;

        switch (fileType)
        {
        case 0: //CSV
          toReturn += F("PARM;VALUE\r\n");
          for (byte i = 0; i < 0x6A; i++)
            toReturn += String(i) + ';' + params[i] + '\r' + '\n';

          server.sendHeader(F("Content-Disposition"), F("attachment; filename=\"PARM.csv\""));
          SERVER_KEEPALIVE_FALSE()
          server.send(200, F("text/csv"), toReturn);
          break;

        case 1: //JSON
          toReturn += F("{\"PARM\":[");
          for (byte i = 0; i < 0x6A; i++)
          {
            if (i)
              toReturn += F(",\r\n");
            toReturn += params[i];
          }
          toReturn += F("]}");

          server.sendHeader(F("Content-Disposition"), F("attachment; filename=\"PARM.json\""));
          SERVER_KEEPALIVE_FALSE()
          server.send(200, F("text/json"), toReturn);
          break;
        }

        return;
      }
      else
      {
        SERVER_KEEPALIVE_FALSE()
        server.send(200, F("text/json"), F("{\"INFO\":{\"CMD\":\"BKP PARM\",\"MSG\":\"Stove communication failed\",\"RSP\":\"TIMEOUT\"},\"SUCCESS\":false,\"DATA\":{\"NODATA\":true}}"));
        return;
      }
    }

    // WPalaControl specific command
    if (cmd.startsWith(F("BKP HPAR ")))
    {
      byte fileType;

      String strFileType(cmd.substring(9));

      if (strFileType == F("CSV"))
        fileType = 0;
      else if (strFileType == F("JSON"))
        fileType = 1;
      else
      {
        String ret(F("{\"INFO\":{\"CMD\":\"BKP HPAR\",\"MSG\":\"Incorrect File Type : "));
        ret += strFileType;
        ret += F("\"},\"SUCCESS\":false,\"DATA\":{\"NODATA\":true}}");
        SERVER_KEEPALIVE_FALSE()
        server.send(200, F("text/json"), ret);
        return;
      }

      uint16_t hiddenParams[0x6F];
      Palazzetti::CommandResult cmdRes = _Pala.getAllHiddenParameters(&hiddenParams);

      if (cmdRes == Palazzetti::CommandResult::OK)
      {
        String toReturn;

        switch (fileType)
        {
        case 0: //CSV
          toReturn += F("HPAR;VALUE\r\n");
          for (byte i = 0; i < 0x6F; i++)
            toReturn += String(i) + ';' + hiddenParams[i] + '\r' + '\n';

          SERVER_KEEPALIVE_FALSE()
          server.sendHeader(F("Content-Disposition"), F("attachment; filename=\"HPAR.csv\""));
          server.send(200, F("text/csv"), toReturn);
          break;

        case 1: //JSON
          toReturn += F("{\"HPAR\":[");
          for (byte i = 0; i < 0x6F; i++)
          {
            if (i)
              toReturn += F(",\r\n");
            toReturn += hiddenParams[i];
          }
          toReturn += F("]}");

          SERVER_KEEPALIVE_FALSE()
          server.sendHeader(F("Content-Disposition"), F("attachment; filename=\"HPAR.json\""));
          server.send(200, F("text/json"), toReturn);
          break;
        }

        return;
      }
      else
      {
        SERVER_KEEPALIVE_FALSE()
        server.send(200, F("text/json"), F("{\"INFO\":{\"CMD\":\"BKP HPAR\",\"MSG\":\"Stove communication failed\",\"RSP\":\"TIMEOUT\"},\"SUCCESS\":false,\"DATA\":{\"NODATA\":true}}"));
        return;
      }
    }

    // Other commands processed using normal Palazzetti logic
    executePalaCmd(cmd, strJson);

    // send response
    SERVER_KEEPALIVE_FALSE()
    server.send(200, F("text/json"), strJson); });

  // Handle HTTP POST requests (Body contains a JSON)
  server.on(
      F("/cgi-bin/sendmsg.lua"), HTTP_POST, [this, &server]()
      {
        String cmd;
        JsonDocument jsonDoc;
        String strJson;

        DeserializationError error = deserializeJson(jsonDoc, server.arg(F("plain")));

        if (!error && !jsonDoc["command"].isNull())
          cmd = jsonDoc["command"].as<String>();

        // process cmd
        executePalaCmd(cmd, strJson);

        // send response
        SERVER_KEEPALIVE_FALSE()
        server.send(200, F("text/json"), strJson); });
}

//------------------------------------------
// Run for timer
void WebPalaControl::appRun()
{
  if (_ha.protocol == HA_PROTO_MQTT)
    _mqttMan.loop();

  if (_needPublish)
  {
    _needPublish = false;
    publishTick();
  }

  // if MQTT and Home Assistant discovery enabled and publish is needed
  if (_ha.protocol == HA_PROTO_MQTT && _ha.mqtt.hassDiscoveryEnabled && _needPublishHassDiscovery)
  {
    if (publishHassDiscoveryToMqtt()) // publish discovery
    {
      _needPublishHassDiscovery = false;
      publishTick(); // publish immediately after HAss discovery
    }
  }

  // Handle UDP requests
  udpRequestHandler(_udpServer);
}

//------------------------------------------
// Constructor
WebPalaControl::WebPalaControl(char appId, String appName) : Application(appId, appName)
{
  // TX/GPIO15 is pulled down and so block the stove bus by default...
  pinMode(15, OUTPUT); // set TX PIN to OUTPUT HIGH to unlock bus during WiFi connection
  digitalWrite(15, HIGH);
}
