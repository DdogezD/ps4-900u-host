#include <FS.h>
#include "WiFi.h"
#include <WiFiUdp.h>
#include "lwip/lwip_napt.h"
#include "ESPAsyncWebServer.h"
#include "esp_task_wdt.h"
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>

// Brownout detector threshold control (ESP32-S2 regi2c analog BOD).
// Lowering it from the core default (level 7 = most sensitive) reduces spurious
// brownout resets on marginal power. Must be declared at file scope (extern "C"
// is not allowed inside a function by the Arduino compiler).
extern "C" void regi2c_ctrl_write_reg_mask(uint8_t block, uint8_t host_id, uint8_t reg_add, uint8_t msb, uint8_t lsb, uint8_t data);
#define I2C_BOD            0x61
#define I2C_BOD_HOSTID     1
#define I2C_BOD_THRESHOLD  0x5


#if defined(CONFIG_IDF_TARGET_ESP32S2) | defined(CONFIG_IDF_TARGET_ESP32S3)  // ESP32-S2/S3 BOARDS(usb emulation)
#include "USB.h"
#include "USBMSC.h"
#include "exfathax.h"
#elif defined(CONFIG_IDF_TARGET_ESP32)  // ESP32 BOARDS
#define USBCONTROL false                // set to true if you are using usb control(wired up usb drive)
#define usbPin 4                        // set the pin you want to use for usb control
#else
#error "Selected board not supported"
#endif


                     // use PsFree [ true / false ]
#define PSFREE true  // use the newer psfree webkit exploit.
                     // this is fairly stable but may fail which will require you to try and load the payload again.
                     
                              // set the cpu frequency [ 80 / 160 / 240 ]
#define CPU_FREQ_DEFAULT 80   // lowers the cpu frequency to reduce heat and power draw.
                              // these values can be changed in the admin config page.

                                  // reduce wifi tx power [ true / false ]
#define LOW_TXPOWER_DEFAULT false // reduces the wifi tx power to 11dBm to lower heat.
                                  // the ps4 is right next to the dongle so high power is not needed.

                                                  // upstream NTP servers the ESP32 proxies the PS4's NTP
                                                  // queries to. On every PS4 request it queries all of them
                                                  // concurrently and returns the fastest valid reply (no local
                                                  // clock is cached). Defaults = fastest measured on the user's
                                                  // China network (tencent ~50ms, aliyun ~75ms).
#define NTP_SERVER1_DEFAULT "ntp.tencent.com"     // 1st, hostname or IP; override via config.ini NTP_SERVER1
#define NTP_SERVER2_DEFAULT "ntp2.tencent.com"    // 2nd; override via config.ini NTP_SERVER2
#define NTP_SERVER3_DEFAULT "ntp1.aliyun.com"     // 3rd; override via config.ini NTP_SERVER3
#define NTP_SERVER4_DEFAULT "time.windows.com"    // 4th, global anycast for provider diversity; override via config.ini NTP_SERVER4

                              // NTP time-sync forwarding feature [ true / false ]
#define USE_NTP_DEFAULT true  // enables sync + local NTP server + GoldHEN DNAT interception;
                              // override via config.ini USE_NTP

                              // diagnostic logger to /log.txt [ true / false ]
#define USE_LOG_DEFAULT true  // writes BOOT/DNS/NTP events; override via config.ini USE_LOG



//-------------------DEFAULT SETTINGS------------------//

                        // use config.ini [ true / false ]
#define USECONFIG true  // this will allow you to change these settings below via the admin webpage. 
                        // if you want to permanently use the values below then set this to false.

//create access point
boolean startAP = true;
String AP_SSID = "PS4";
String AP_PASS = "900cracker";
IPAddress Server_IP(10, 1, 1, 1);
IPAddress Subnet_Mask(255, 255, 255, 0);
// Upstream NTP servers the ESP32 relays the PS4's NTP queries to (up to 3,
// configurable via config.ini NTP_SERVER1 / NTP_SERVER2 / NTP_SERVER3 /
// NTP_SERVER4). No local clock is cached — each PS4 NTP request is answered
// directly from the fastest live upstream reply. The GoldHEN-side DNAT target
// (time1.google.com = 216.239.35.0) is fixed and NOT configurable.
String ntpServer1 = NTP_SERVER1_DEFAULT;
String ntpServer2 = NTP_SERVER2_DEFAULT;
String ntpServer3 = NTP_SERVER3_DEFAULT;
String ntpServer4 = NTP_SERVER4_DEFAULT;
boolean useNtp = USE_NTP_DEFAULT;
boolean useLog = USE_LOG_DEFAULT;

//connect to wifi
boolean connectWifi = true;
String WIFI_SSID = "WIFI_HOME";
String WIFI_PASS = "password123";
String WIFI_HOSTNAME = "ps4.local";

//server port
int WEB_PORT = 80;

//Auto Usb Wait(milliseconds)
int USB_WAIT = 10000;

// Displayed firmware version
String firmwareVer = "v1.0.0, 26/08/07 by DogEZ";

//ESP sleep after x minutes
boolean espSleep = false;
int TIME2SLEEP = 30;  // minutes

//Power save settings (applied on boot; override via config.ini)
int cpuFreq = CPU_FREQ_DEFAULT;
boolean lowTxPower = LOW_TXPOWER_DEFAULT;


//-----------------------------------------------------//


#include "Loader.h"
#include "Pages.h"
#include "jzip.h"

#include "SPIFFS.h"
#define FILESYS SPIFFS

DNSServer dnsServer;
WiFiUDP ntpServerUdp;
AsyncWebServer server(WEB_PORT);
WiFiUDP ntpProbeUdp;        // concurrent NTP probe (query all servers, use first reply)
IPAddress ntpProbeIps[4];   // resolved upstream NTP server addresses
int ntpProbeCount = 0;
uint32_t tSetup = 0;        // setup() start, for boot timing probes
boolean hasEnabled = false;
boolean isFormating = false;
long enTime = 0;
long bootTime = 0;
File upFile;
#if defined(CONFIG_IDF_TARGET_ESP32S2) | defined(CONFIG_IDF_TARGET_ESP32S3)
USBMSC dev;
#endif


/*
#if ARDUINO_USB_CDC_ON_BOOT
#define HWSerial Serial0
#define USBSerial Serial
#else
#define HWSerial Serial
#if defined(CONFIG_IDF_TARGET_ESP32S2) | defined(CONFIG_IDF_TARGET_ESP32S3)
USBCDC USBSerial;
#endif
#endif
*/


String split(String str, String from, String to) {
  String tmpstr = str;
  tmpstr.toLowerCase();
  from.toLowerCase();
  to.toLowerCase();
  int pos1 = tmpstr.indexOf(from);
  int pos2 = tmpstr.indexOf(to, pos1 + from.length());
  String retval = str.substring(pos1 + from.length(), pos2);
  return retval;
}


bool instr(String str, String search) {
  int result = str.indexOf(search);
  if (result == -1) {
    return false;
  }
  return true;
}


String formatBytes(size_t bytes) {
  if (bytes < 1024) {
    return String(bytes) + " B";
  } else if (bytes < (1024 * 1024)) {
    return String(bytes / 1024.0) + " KB";
  } else if (bytes < (1024 * 1024 * 1024)) {
    return String(bytes / 1024.0 / 1024.0) + " MB";
  } else {
    return String(bytes / 1024.0 / 1024.0 / 1024.0) + " GB";
  }
}


String urlencode(String str) {
  String encodedString = "";
  char c;
  char code0;
  char code1;
  char code2;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encodedString += '+';
    } else if (isalnum(c)) {
      encodedString += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) {
        code1 = (c & 0xf) - 10 + 'A';
      }
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) {
        code0 = c - 10 + 'A';
      }
      code2 = '\0';
      encodedString += '%';
      encodedString += code0;
      encodedString += code1;
    }
    yield();
  }
  encodedString.replace("%2E", ".");
  return encodedString;
}


void sendwebmsg(AsyncWebServerRequest *request, String htmMsg) {
  String tmphtm = "<!DOCTYPE html><html><head><link rel=\"stylesheet\" href=\"style.css\"></head><center><br><br><br><br><br><br>" + htmMsg + "</center></html>";
  request->send(200, "text/html", tmphtm);
}


void handleFwUpdate(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (!index) {
    String path = request->url();
    if (path != "/update.html") {
      request->send(500, "text/plain", "Internal Server Error");
      return;
    }
    if (!filename.equals("fwupdate.bin")) {
      sendwebmsg(request, "Invalid update file: " + filename);
      return;
    }
    if (!filename.startsWith("/")) {
      filename = "/" + filename;
    }
    //HWSerial.printf("Update Start: %s\n", filename.c_str());
    if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
      Update.printError(Serial);
      sendwebmsg(request, "Update Failed: " + String(Update.errorString()));
    }
  }
  if (!Update.hasError()) {
    if (Update.write(data, len) != len) {
      Update.printError(Serial);
      sendwebmsg(request, "Update Failed: " + String(Update.errorString()));
    }
  }
  if (final) {
    if (Update.end(true)) {
      //HWSerial.printf("Update Success: %uB\n", index+len);
      String tmphtm = "<!DOCTYPE html><html><head><meta http-equiv=\"refresh\" content=\"8; url=/info.html\"><style type=\"text/css\">body {background-color: #1451AE; color: #ffffff; font-size: 14px; font-weight: bold; margin: 0 0 0 0.0; padding: 0.4em 0.4em 0.4em 0.6em;}</style></head><center><br><br><br><br><br><br>Update Success, Rebooting.</center></html>";
      request->send(200, "text/html", tmphtm);
      delay(1000);
      ESP.restart();
    } else {
      Update.printError(Serial);
    }
  }
}


void handleDelete(AsyncWebServerRequest *request) {
  if (!request->hasParam("file", true)) {
    request->redirect("/fileman.html");
    return;
  }
  String path = request->getParam("file", true)->value();
  if (path.length() == 0) {
    request->redirect("/fileman.html");
    return;
  }
  if (FILESYS.exists("/" + path) && path != "/" && !path.equals("config.ini")) {
    FILESYS.remove("/" + path);
  }
  request->redirect("/fileman.html");
}



void handleFileMan(AsyncWebServerRequest *request) {
  File dir = FILESYS.open("/");
  String output = "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>File Manager</title><link rel=\"stylesheet\" href=\"style.css\"><style>body{overflow-y:auto;} th{border: 1px solid #dddddd; background-color:gray;padding: 8px;}</style><script>function statusDel(fname) {var answer = confirm(\"Are you sure you want to delete \" + fname + \" ?\");if (answer) {return true;} else { return false; }} </script></head><body><br><table id=filetable></table><script>var filelist = [";
  int fileCount = 0;
  while (dir) {
    File file = dir.openNextFile();
    if (!file) {
      dir.close();
      break;
    }
    String fname = String(file.name());
    if (fname.length() > 0 && !fname.equals("config.ini") && !file.isDirectory()) {
      fileCount++;
      fname.replace("|", "%7C");
      fname.replace("\"", "%22");
      output += "\"" + fname + "|" + formatBytes(file.size()) + "\",";
    }
    file.close();
    esp_task_wdt_reset();
  }
  if (fileCount == 0) {
    output += "];</script><center>No files found<br>You can upload files using the <a href=\"/upload.html\" target=\"mframe\"><u>File Uploader</u></a> page.</center></p></body></html>";
  } else {
    output += "];var output = \"\";filelist.forEach(function(entry) {var splF = entry.split(\"|\"); output += \"<tr>\";output += \"<td><a href=\\\"\" +  splF[0] + \"\\\">\" + splF[0] + \"</a></td>\"; output += \"<td>\" + splF[1] + \"</td>\";output += \"<td><a href=\\\"/\" + splF[0] + \"\\\" download><button type=\\\"submit\\\">Download</button></a></td>\";output += \"<td><form action=\\\"/delete\\\" method=\\\"post\\\"><button type=\\\"submit\\\" name=\\\"file\\\" value=\\\"\" + splF[0] + \"\\\" onClick=\\\"return statusDel('\" + splF[0] + \"');\\\">Delete</button></form></td>\";output += \"</tr>\";}); document.getElementById(\"filetable\").innerHTML = \"<tr><th colspan='1'><center>File Name</center></th><th colspan='1'><center>File Size</center></th><th colspan='1'><center><a href='/dlall' target='mframe'><button type='submit'>Download All</button></a></center></th><th colspan='1'><center><a href='/format.html' target='mframe'><button type='submit'>Delete All</button></a></center></th></tr>\" + output;</script></body></html>";
  }
  request->send(200, "text/html", output);
}


void handleDlFiles(AsyncWebServerRequest *request) {
  File dir = FILESYS.open("/");
  String output = "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>File Downloader</title><link rel=\"stylesheet\" href=\"style.css\"><style>body{overflow-y:auto;}</style><script type=\"text/javascript\" src=\"jzip.js\"></script><script>var filelist = [";
  int fileCount = 0;
  while (dir) {
    File file = dir.openNextFile();
    if (!file) {
      dir.close();
      break;
    }
    String fname = String(file.name());
    if (fname.length() > 0 && !fname.equals("config.ini") && !file.isDirectory()) {
      fileCount++;
      fname.replace("\"", "%22");
      output += "\"" + fname + "\",";
    }
    file.close();
    esp_task_wdt_reset();
  }
  if (fileCount == 0) {
    output += "];</script></head><center>No files found to download<br>You can upload files using the <a href=\"/upload.html\" target=\"mframe\"><u>File Uploader</u></a> page.</center></p></body></html>";
  } else {
    output += "]; async function dlAll(){var zip = new JSZip();for (var i = 0; i < filelist.length; i++) {if (filelist[i] != ''){var xhr = new XMLHttpRequest();xhr.open('GET',filelist[i],false);xhr.overrideMimeType('text/plain; charset=x-user-defined'); xhr.onload = function(e) {if (this.status == 200) {zip.file(filelist[i], this.response,{binary: true});}};xhr.send();document.getElementById('fp').innerHTML = 'Adding: ' + filelist[i];await new Promise(r => setTimeout(r, 50));}}document.getElementById('gen').style.display = 'none';document.getElementById('comp').style.display = 'block';zip.generateAsync({type:'blob'}).then(function(content) {saveAs(content,'esp_files.zip');});}</script></head><body onload='setTimeout(dlAll,100);'><center><br><br><br><br><div id='gen' style='display:block;'><div id='loader'></div><br><br>Generating ZIP<br><p id='fp'></p></div><div id='comp' style='display:none;'><br><br><br><br>Complete<br><br>Downloading: esp_files.zip</div></center></body></html>";
  }
  request->send(200, "text/html", output);
}


void handlePayloads(AsyncWebServerRequest *request) {
  File dir = FILESYS.open("/");
  String output = "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>ESP Server</title><link rel=\"stylesheet\" href=\"style.css\"><style>body { background-color: #1451AE; color: #ffffff; font-size: 14px; font-weight: bold; margin: 0 0 0 0.0; overflow-y:hidden; text-shadow: 3px 2px DodgerBlue;}</style><script>function setpayload(payload,title,waittime){ sessionStorage.setItem('payload', payload); sessionStorage.setItem('title', title); sessionStorage.setItem('waittime', waittime);  window.open('loader.html', '_self');}</script></head><body><center><h1>Payloads</h1>";
  int cntr = 0;
  int payloadCount = 0;
  if (USB_WAIT < 5000) { USB_WAIT = 5000; }  // correct unrealistic timing values
  if (USB_WAIT > 25000) { USB_WAIT = 25000; }

  while (dir) {
    File file = dir.openNextFile();
    if (!file) {
      dir.close();
      break;
    }
    String fname = String(file.name());
    if (fname.endsWith(".gz")) {
      fname = fname.substring(0, fname.length() - 3);
    }
    if (fname.length() > 0 && fname.endsWith(".bin") && !file.isDirectory()) {
      payloadCount++;
      String fnamev = fname;
      fnamev.replace(".bin", "");
      output += "<a onclick=\"setpayload('" + urlencode(fname) + "','" + fnamev + "','" + String(USB_WAIT) + "')\"><button class=\"btn\">" + fnamev + "</button></a>&nbsp;";
      cntr++;
      if (cntr == 4) {
        cntr = 0;
        output += "<p></p>";
      }
    }
    file.close();
    esp_task_wdt_reset();
  }

  if (payloadCount == 0) {
    output += "<msg>No .bin payloads found<br>You need to upload the payloads to the ESP32 board.<br>in the arduino ide select <b>Tools</b> &gt; <b>ESP32 Sketch Data Upload</b><br>or<br>Using a pc/laptop connect to <b>" + AP_SSID + "</b> and navigate to <a href=\"/admin.html\"><u>http://" + WIFI_HOSTNAME + "/admin.html</u></a> and upload the .bin payloads using the <b>File Uploader</b></msg></center></body></html>";
  }
  output += "</center></body></html>";
  request->send(200, "text/html", output);
}


#if USECONFIG
void handleConfig(AsyncWebServerRequest *request) {
  if (request->hasParam("ap_ssid", true) && request->hasParam("ap_pass", true) && request->hasParam("web_ip", true) && request->hasParam("web_port", true) && request->hasParam("subnet", true) && request->hasParam("wifi_ssid", true) && request->hasParam("wifi_pass", true) && request->hasParam("wifi_host", true) && request->hasParam("usbwait", true)) {
    AP_SSID = request->getParam("ap_ssid", true)->value();
    if (!request->getParam("ap_pass", true)->value().equals("********")) {
      AP_PASS = request->getParam("ap_pass", true)->value();
    }
    WIFI_SSID = request->getParam("wifi_ssid", true)->value();
    if (!request->getParam("wifi_pass", true)->value().equals("********")) {
      WIFI_PASS = request->getParam("wifi_pass", true)->value();
    }
    String tmpip = request->getParam("web_ip", true)->value();
    String tmpwport = request->getParam("web_port", true)->value();
    String tmpsubn = request->getParam("subnet", true)->value();
    String WIFI_HOSTNAME = request->getParam("wifi_host", true)->value();
    String tmpua = "false";
    String tmpcw = "false";
    String tmpslp = "false";
    if (request->hasParam("useap", true)) { tmpua = "true"; }
    if (request->hasParam("usewifi", true)) { tmpcw = "true"; }
    if (request->hasParam("espsleep", true)) { tmpslp = "true"; }
    if (tmpua.equals("false") && tmpcw.equals("false")) { tmpua = "true"; }
    int USB_WAIT = request->getParam("usbwait", true)->value().toInt();
    int TIME2SLEEP = request->getParam("sleeptime", true)->value().toInt();
    int cpuFreq = 160;
    if (request->hasParam("cpu_freq", true)) { cpuFreq = request->getParam("cpu_freq", true)->value().toInt(); }
    String tmpltx = "false";
    if (request->hasParam("low_txpower", true)) { tmpltx = "true"; }
    if (request->hasParam("ntp_server1", true)) {
      String tmpns1 = request->getParam("ntp_server1", true)->value();
      tmpns1.trim();
      if (tmpns1.length() > 0) { ntpServer1 = tmpns1; }
    }
    if (request->hasParam("ntp_server2", true)) {
      String tmpns2 = request->getParam("ntp_server2", true)->value();
      tmpns2.trim();
      if (tmpns2.length() > 0) { ntpServer2 = tmpns2; }
    }
    if (request->hasParam("ntp_server3", true)) {
      String tmpns3 = request->getParam("ntp_server3", true)->value();
      tmpns3.trim();
      if (tmpns3.length() > 0) { ntpServer3 = tmpns3; }
    }
    if (request->hasParam("ntp_server4", true)) {
      String tmpns4 = request->getParam("ntp_server4", true)->value();
      tmpns4.trim();
      if (tmpns4.length() > 0) { ntpServer4 = tmpns4; }
    }
    useNtp = request->hasParam("use_ntp", true);
    useLog = request->hasParam("use_log", true);
    File iniFile = FILESYS.open("/config.ini", "w");
    if (iniFile) {
      iniFile.print("\r\nAP_SSID=" + AP_SSID + "\r\nAP_PASS=" + AP_PASS + "\r\nWEBSERVER_IP=" + tmpip + "\r\nWEBSERVER_PORT=" + tmpwport + "\r\nSUBNET_MASK=" + tmpsubn + "\r\nWIFI_SSID=" + WIFI_SSID + "\r\nWIFI_PASS=" + WIFI_PASS + "\r\nWIFI_HOST=" + WIFI_HOSTNAME + "\r\nUSEAP=" + tmpua + "\r\nCONWIFI=" + tmpcw + "\r\nUSBWAIT=" + USB_WAIT + "\r\nESPSLEEP=" + tmpslp + "\r\nSLEEPTIME=" + TIME2SLEEP + "\r\nCPU_FREQ=" + String(cpuFreq) + "\r\nLOW_TXPOWER=" + tmpltx + "\r\nNTP_SERVER1=" + ntpServer1 + "\r\nNTP_SERVER2=" + ntpServer2 + "\r\nNTP_SERVER3=" + ntpServer3 + "\r\nNTP_SERVER4=" + ntpServer4 + "\r\nUSE_NTP=" + String(useNtp ? "true" : "false") + "\r\nUSE_LOG=" + String(useLog ? "true" : "false") + "\r\n");
      iniFile.close();
    }
    String htmStr = "<!DOCTYPE html><html><head><meta http-equiv=\"refresh\" content=\"8; url=/info.html\"><style type=\"text/css\">#loader {z-index: 1;width: 50px;height: 50px;margin: 0 0 0 0;border: 6px solid #f3f3f3;border-radius: 50%;border-top: 6px solid #3498db;width: 50px;height: 50px;-webkit-animation: spin 2s linear infinite;animation: spin 2s linear infinite; } @-webkit-keyframes spin {0%{-webkit-transform: rotate(0deg);}100%{-webkit-transform: rotate(360deg);}}@keyframes spin{0%{ transform: rotate(0deg);}100%{transform: rotate(360deg);}}body {background-color: #1451AE; color: #ffffff; font-size: 14px; font-weight: bold; margin: 0 0 0 0.0; padding: 0.4em 0.4em 0.4em 0.6em;} #msgfmt {font-size: 14px; font-weight: bold;}#status {font-size: 14px; font-weight: bold;}</style></head><center><br><br><br><br><br><p id=\"status\"><div id='loader'></div><br>Config saved<br>Rebooting</p></center></html>";
    request->send(200, "text/html", htmStr);
    delay(1000);
    ESP.restart();
  } else {
    request->redirect("/config.html");
  }
}
#endif


void handleReboot(AsyncWebServerRequest *request) {
  //HWSerial.print("Rebooting ESP");
  AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", rebooting_gz, sizeof(rebooting_gz));
  response->addHeader("Content-Encoding", "gzip");
  request->send(response);
  delay(1000);
  ESP.restart();
}


#if USECONFIG
void handleConfigHtml(AsyncWebServerRequest *request) {
  String tmpUa = "";
  String tmpCw = "";
  String tmpSlp = "";
  String tmpLtx = "checked";
  String tmpUntp = "checked";
  String tmpUlog = "checked";
  String tmpCpu80 = "", tmpCpu160 = "selected", tmpCpu240 = "";
  if (startAP) { tmpUa = "checked"; }
  if (connectWifi) { tmpCw = "checked"; }
  if (espSleep) { tmpSlp = "checked"; }
  if (!lowTxPower) { tmpLtx = ""; }
  if (!useNtp) { tmpUntp = ""; }
  if (!useLog) { tmpUlog = ""; }
  if (cpuFreq == 80) { tmpCpu80 = "selected"; tmpCpu160 = ""; }
  if (cpuFreq == 240) { tmpCpu240 = "selected"; tmpCpu160 = ""; }

  String htmStr = "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>Config Editor</title><style type=\"text/css\">body {background-color: #1451AE; color: #ffffff; font-size: 14px;font-weight: bold;margin: 0 0 0 0.0;padding: 0.4em 0.4em 0.4em 0.6em;}input[type=\"submit\"]:hover {background: #ffffff;color: green;}input[type=\"submit\"]:active{outline-color: green;color: green;background: #ffffff; }table {font-family: arial, sans-serif;border-collapse: collapse;}td {border: 1px solid #dddddd;text-align: left;padding: 8px;}th {border: 1px solid #dddddd; background-color:gray;text-align: center;padding: 8px;}</style></head><body><form action=\"/config.html\" method=\"post\"><center><table><tr><th colspan=\"2\"><center>Access Point</center></th></tr><tr><td>AP SSID:</td><td><input name=\"ap_ssid\" value=\"" + AP_SSID + "\"></td></tr><tr><td>AP PASSWORD:</td><td><input name=\"ap_pass\" value=\"********\"></td></tr><tr><td>AP IP:</td><td><input name=\"web_ip\" value=\"" + Server_IP.toString() + "\"></td></tr><tr><td>SUBNET MASK:</td><td><input name=\"subnet\" value=\"" + Subnet_Mask.toString() + "\"></td></tr><tr><td>START AP:</td><td><input type=\"checkbox\" name=\"useap\" " + tmpUa + "></td></tr><tr><th colspan=\"2\"><center>Web Server</center></th></tr><tr><td>WEBSERVER PORT:</td><td><input name=\"web_port\" value=\"" + String(WEB_PORT) + "\"></td></tr><tr><th colspan=\"2\"><center>Wifi Connection</center></th></tr><tr><td>WIFI SSID:</td><td><input name=\"wifi_ssid\" value=\"" + WIFI_SSID + "\"></td></tr><tr><td>WIFI PASSWORD:</td><td><input name=\"wifi_pass\" value=\"********\"></td></tr><tr><td>WIFI HOSTNAME:</td><td><input name=\"wifi_host\" value=\"" + WIFI_HOSTNAME + "\"></td></tr><tr><td>CONNECT WIFI:</td><td><input type=\"checkbox\" name=\"usewifi\" " + tmpCw + "></td></tr><tr><th colspan=\"2\"><center>Auto USB Wait</center></th></tr><tr><td>WAIT TIME(ms):</td><td><input name=\"usbwait\" value=\"" + USB_WAIT + "\"></td></tr><tr><th colspan=\"2\"><center>ESP Sleep Mode</center></th></tr><tr><td>ENABLE SLEEP:</td><td><input type=\"checkbox\" name=\"espsleep\" " + tmpSlp + "></td></tr><tr><td>TIME TO SLEEP(minutes):</td><td><input name=\"sleeptime\" value=\"" + TIME2SLEEP + "\"></td></tr><tr><th colspan=\"2\"><center>Power Save</center></th></tr><tr><td>CPU FREQUENCY:</td><td><select name=\"cpu_freq\"><option value=\"80\" " + tmpCpu80 + ">80 MHz</option><option value=\"160\" " + tmpCpu160 + ">160 MHz</option><option value=\"240\" " + tmpCpu240 + ">240 MHz</option></select></td></tr><tr><td>LOW WIFI TX POWER:</td><td><input type=\"checkbox\" name=\"low_txpower\" " + tmpLtx + "></td></tr><tr><th colspan=\"2\"><center>System</center></th></tr><tr><td>ENABLE LOG:</td><td><input type=\"checkbox\" name=\"use_log\" " + tmpUlog + "></td></tr><tr><th colspan=\"2\"><center>NTP Time Sync</center></th></tr><tr><td>ENABLE NTP SYNC:</td><td><input type=\"checkbox\" name=\"use_ntp\" " + tmpUntp + "></td></tr><tr><td>NTP SERVER 1:</td><td><input name=\"ntp_server1\" value=\"" + ntpServer1 + "\"></td></tr><tr><td>NTP SERVER 2:</td><td><input name=\"ntp_server2\" value=\"" + ntpServer2 + "\"></td></tr><tr><td>NTP SERVER 3:</td><td><input name=\"ntp_server3\" value=\"" + ntpServer3 + "\"></td></tr><tr><td>NTP SERVER 4:</td><td><input name=\"ntp_server4\" value=\"" + ntpServer4 + "\"></td></tr></table><br><input id=\"savecfg\" type=\"submit\" value=\"Save Config\"></center></form></body></html>";
  request->send(200, "text/html", htmStr);
}
#endif


void handleFileUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (!index) {
    String path = request->url();
    if (path != "/upload.html") {
      request->send(500, "text/plain", "Internal Server Error");
      return;
    }
    if (!filename.startsWith("/")) {
      filename = "/" + filename;
    }
    if (filename.equals("/config.ini")) { return; }
    //HWSerial.printf("Upload Start: %s\n", filename.c_str());
    upFile = FILESYS.open(filename, "w");
  }
  if (upFile) {
    upFile.write(data, len);
  }
  if (final) {
    upFile.close();
    //HWSerial.printf("upload Success: %uB\n", index+len);
  }
}


void handleConsoleUpdate(String rgn, AsyncWebServerRequest *request) {
  String Version = "05.050.000";
  String sVersion = "05.050.000";
  String lblVersion = "5.05";
  String imgSize = "0";
  String imgPath = "";
  String xmlStr = "<?xml version=\"1.0\" ?><update_data_list><region id=\"" + rgn + "\"><force_update><system level0_system_ex_version=\"0\" level0_system_version=\"" + Version + "\" level1_system_ex_version=\"0\" level1_system_version=\"" + Version + "\"/></force_update><system_pup ex_version=\"0\" label=\"" + lblVersion + "\" sdk_version=\"" + sVersion + "\" version=\"" + Version + "\"><update_data update_type=\"full\"><image size=\"" + imgSize + "\">" + imgPath + "</image></update_data></system_pup><recovery_pup type=\"default\"><system_pup ex_version=\"0\" label=\"" + lblVersion + "\" sdk_version=\"" + sVersion + "\" version=\"" + Version + "\"/><image size=\"" + imgSize + "\">" + imgPath + "</image></recovery_pup></region></update_data_list>";
  request->send(200, "text/xml", xmlStr);
}

#if !USBCONTROL && defined(CONFIG_IDF_TARGET_ESP32)
void handleCacheManifest(AsyncWebServerRequest *request) {
#if !USBCONTROL
  String output = "CACHE MANIFEST\r\n";
  File dir = FILESYS.open("/");
  while (dir) {
    File file = dir.openNextFile();
    if (!file) {
      dir.close();
      break;
    }
    String fname = String(file.name());
    if (fname.length() > 0 && !fname.equals("config.ini") && !file.isDirectory()) {
      if (fname.endsWith(".gz")) {
        fname = fname.substring(0, fname.length() - 3);
      }
      output += urlencode(fname) + "\r\n";
    }
    file.close();
  }
  if (!instr(output, "index.html\r\n")) {
    output += "index.html\r\n";
  }
  if (!instr(output, "menu.html\r\n")) {
    output += "menu.html\r\n";
  }
  if (!instr(output, "loader.html\r\n")) {
    output += "loader.html\r\n";
  }
  if (!instr(output, "payloads.html\r\n")) {
    output += "payloads.html\r\n";
  }
  if (!instr(output, "exploit.js\r\n")) {
    output += "exploit.js\r\n";
  }
  if (!instr(output, "style.css\r\n")) {
    output += "style.css\r\n";
  }
  request->send(200, "text/cache-manifest", output);
#else
  request->send(404);
#endif
}
#endif

void handleInfo(AsyncWebServerRequest *request) {
  float flashFreq = (float)ESP.getFlashChipSpeed() / 1000.0 / 1000.0;
  FlashMode_t ideMode = ESP.getFlashChipMode();
  String mcuType = CONFIG_IDF_TARGET;
  mcuType.toUpperCase();
  String output = "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>System Information</title><link rel=\"stylesheet\" href=\"style.css\"></head>";
  output += "<hr>###### Software ######<br><br>";
  output += "Firmware version: " + firmwareVer + "<br>";
  output += "SDK version: " + String(ESP.getSdkVersion()) + "<br><hr>";
  output += "###### Board ######<br><br>";
  output += "MCU: " + mcuType + "<br>";
#if defined(USB_PRODUCT)
  output += "Board: " + String(USB_PRODUCT) + "<br>";
#endif
  output += "Chip Id: " + String(ESP.getChipModel()) + "<br>";
  output += "CPU frequency: " + String(ESP.getCpuFreqMHz()) + "MHz<br>";
  output += "Cores: " + String(ESP.getChipCores()) + "<br><hr>";
  output += "###### Flash chip information ######<br><br>";
  output += "Flash chip Id: " + String(ESP.getFlashChipMode()) + "<br>";
  output += "Estimated Flash size: " + formatBytes(ESP.getFlashChipSize()) + "<br>";
  output += "Flash frequency: " + String(flashFreq) + " MHz<br>";
  output += "Flash write mode: " + String((ideMode == FM_QIO ? "QIO" : ideMode == FM_QOUT ? "QOUT"
                                                                     : ideMode == FM_DIO  ? "DIO"
                                                                     : ideMode == FM_DOUT ? "DOUT"
                                                                                          : "UNKNOWN"))
            + "<br><hr>";
  output += "###### Storage information ######<br><br>";
  output += "Filesystem: SPIFFS<br>";
  output += "Total Size: " + formatBytes(FILESYS.totalBytes()) + "<br>";
  output += "Used Space: " + formatBytes(FILESYS.usedBytes()) + "<br>";
  output += "Free Space: " + formatBytes(FILESYS.totalBytes() - FILESYS.usedBytes()) + "<br><hr>";
#if defined(CONFIG_IDF_TARGET_ESP32S2) | defined(CONFIG_IDF_TARGET_ESP32S3)
  if (ESP.getPsramSize() > 0) {
    output += "###### PSRam information ######<br><br>";
    output += "Psram Size: " + formatBytes(ESP.getPsramSize()) + "<br>";
    output += "Free psram: " + formatBytes(ESP.getFreePsram()) + "<br>";
    output += "Max alloc psram: " + formatBytes(ESP.getMaxAllocPsram()) + "<br><hr>";
  }
#endif
  output += "###### Ram information ######<br><br>";
  output += "Ram size: " + formatBytes(ESP.getHeapSize()) + "<br>";
  output += "Free ram: " + formatBytes(ESP.getFreeHeap()) + "<br>";
  output += "Max alloc ram: " + formatBytes(ESP.getMaxAllocHeap()) + "<br><hr>";
  output += "###### Sketch information ######<br><br>";
  output += "Sketch hash: " + ESP.getSketchMD5() + "<br>";
  output += "Sketch size: " + formatBytes(ESP.getSketchSize()) + "<br>";
  output += "Free space available: " + formatBytes(ESP.getFreeSketchSpace() - ESP.getSketchSize()) + "<br><hr>";
  output += "</html>";
  request->send(200, "text/html", output);
}


#if USECONFIG
void writeConfig() {
  File iniFile = FILESYS.open("/config.ini", "w");
  if (iniFile) {
    String tmpua = "false";
    String tmpcw = "false";
    String tmpslp = "false";
    String tmpltx = "false";
    if (startAP) { tmpua = "true"; }
    if (connectWifi) { tmpcw = "true"; }
    if (espSleep) { tmpslp = "true"; }
    if (lowTxPower) { tmpltx = "true"; }
    iniFile.print("\r\nAP_SSID=" + AP_SSID + "\r\nAP_PASS=" + AP_PASS + "\r\nWEBSERVER_IP=" + Server_IP.toString() + "\r\nWEBSERVER_PORT=" + String(WEB_PORT) + "\r\nSUBNET_MASK=" + Subnet_Mask.toString() + "\r\nWIFI_SSID=" + WIFI_SSID + "\r\nWIFI_PASS=" + WIFI_PASS + "\r\nWIFI_HOST=" + WIFI_HOSTNAME + "\r\nUSEAP=" + tmpua + "\r\nCONWIFI=" + tmpcw + "\r\nUSBWAIT=" + USB_WAIT + "\r\nESPSLEEP=" + tmpslp + "\r\nSLEEPTIME=" + TIME2SLEEP + "\r\nCPU_FREQ=" + String(cpuFreq) + "\r\nLOW_TXPOWER=" + tmpltx + "\r\nNTP_SERVER1=" + ntpServer1 + "\r\nNTP_SERVER2=" + ntpServer2 + "\r\nNTP_SERVER3=" + ntpServer3 + "\r\nNTP_SERVER4=" + ntpServer4 + "\r\nUSE_NTP=" + String(useNtp ? "true" : "false") + "\r\nUSE_LOG=" + String(useLog ? "true" : "false") + "\r\n");
    iniFile.close();
  }
}
#endif






//Lower the power draw of the dongle: reduce CPU frequency (configurable via
//CPU_FREQ) and cut the WiFi TX power (the PS4 is right next to the dongle).
//Called after the WiFi stack has been started.
void applyPowerSettings() {
  // WiFi modem sleep / power save is ON by default and makes the softAP hang
  // when a client disconnects and reconnects. Always keep it disabled.
  WiFi.setSleep(false);
  if (cpuFreq == 80 || cpuFreq == 160 || cpuFreq == 240) {
    setCpuFrequencyMhz(cpuFreq);
  }
  if (lowTxPower) {
    WiFi.setTxPower(WIFI_POWER_11dBm);
  }
}


// Diagnostic logger: appends a timestamped line to /log.txt on the filesystem.
// Read it via http://ps4.local/log.txt (or the file manager). Disabled when
// USE_LOG / config.ini USE_LOG is false.
void appendLog(String line) {
  if (!useLog) { return; }
  // Cap the log so it can't fill the filesystem over many boots. Only BOOT and
  // NTP events are logged now (DNS per-query logging was removed because it
  // wrote the filesystem constantly and risked corruption on a power cut).
  if (FILESYS.exists("/log.txt")) {
    File chk = FILESYS.open("/log.txt", "r");
    if (chk) {
      if (chk.size() > 32768) { chk.close(); FILESYS.remove("/log.txt"); }
      else { chk.close(); }
    }
  }
  File f = FILESYS.open("/log.txt", "a");
  if (f) {
    f.print("[");
    f.print(millis());
    f.print("ms] ");
    f.println(line);
    f.close();
  }
}

// Human-readable ESP32 reset reason.
String resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return String((int)esp_reset_reason());
  }
}

// GoldHEN syncs its clock via NTP to a hardcoded server (time1.google.com =
// 216.239.35.0:123), bypassing DNS. The lwIP DNAT portmap in setup() rewrites
// those packets to this device's IP:123, so the PS4's UDP NTP requests arrive
// here. On each request we query the configured upstream NTP servers
// (NTP_SERVER1 / NTP_SERVER2 / NTP_SERVER3) CONCURRENTLY, take the fastest
// valid reply's time, and answer the PS4 with it directly — no local clock is
// cached (the ESP32-S2 has no external RTC crystal, so a cached time would
// drift). The reply source is rewritten back to the forwarded server's IP:123
// by the patched lwIP so the PS4 accepts it.
void handleNTPRequest() {
  int len = ntpServerUdp.parsePacket();
  if (len > 0) {
    appendLog("NTP from " + ntpServerUdp.remoteIP().toString() + " len=" + String(len));
  }
  if (len >= 48) {
    uint8_t buf[48];
    ntpServerUdp.read(buf, 48);
    // Query all upstream servers at once and use the first valid reply. Bounded
    // wait (~350ms) so the loop can't stall; with the fast China servers the
    // reply normally arrives in <100ms.
    uint32_t ntpSecs = ntpFastQuery(350);
    if (ntpSecs == 0) {
      // No valid upstream time (offline or all servers unreachable). Send NO
      // reply: the client retries. (A Kiss-of-Death with transmit=0 was tried
      // but the PS4's client mis-read it as the 2036 NTP wrap-around and set
      // the clock to 2036-02-07, so no reply is safer.)
      appendLog("NTP from " + ntpServerUdp.remoteIP().toString() + " SKIP-no-upstream");
      return;
    }
    appendLog("NTP from " + ntpServerUdp.remoteIP().toString() + " VN=" + String((buf[0] >> 3) & 7) + " answered");
    // Echo the client's transmit timestamp into the response's originate field.
    memcpy(buf + 24, buf + 40, 8);
    // LI=0, echo the client's NTP version, Mode=4 (server reply).
    buf[0] = (buf[0] & 0x38) | 0x04;
    buf[1] = 2;       // stratum 2 (secondary: we got time from upstream)
    buf[2] = 6;       // poll interval 2^6 = 64s
    buf[3] = 0xEC;    // precision 2^-20
    memset(buf + 4, 0, 8);  // root delay + root dispersion = 0
    // Reference ID = our AP IP (a valid IPv4 for a stratum-2 server).
    IPAddress ref = Server_IP;
    buf[12] = ref[0]; buf[13] = ref[1]; buf[14] = ref[2]; buf[15] = ref[3];
    // Write a 64-bit NTP timestamp (seconds + zero fraction) at the given offset.
    for (int off = 16; off <= 40; off += 8) {
      if (off == 24) { continue; }  // 24..31 is the originate field (echoed above)
      buf[off]     = (ntpSecs >> 24) & 0xFF;
      buf[off + 1] = (ntpSecs >> 16) & 0xFF;
      buf[off + 2] = (ntpSecs >> 8) & 0xFF;
      buf[off + 3] = ntpSecs & 0xFF;
      buf[off + 4] = buf[off + 5] = buf[off + 6] = buf[off + 7] = 0;
    }
    ntpServerUdp.beginPacket(ntpServerUdp.remoteIP(), ntpServerUdp.remotePort());
    ntpServerUdp.write(buf, 48);
    ntpServerUdp.endPacket();
  }
}

// Resolve the configured upstream NTP server hostnames once (blocking DNS).
void ntpProbeStart() {
  ntpProbeCount = 0;
  IPAddress ip;
  if (WiFi.hostByName(ntpServer1.c_str(), ip)) { ntpProbeIps[ntpProbeCount++] = ip; }
  if (WiFi.hostByName(ntpServer2.c_str(), ip)) { ntpProbeIps[ntpProbeCount++] = ip; }
  if (WiFi.hostByName(ntpServer3.c_str(), ip)) { ntpProbeIps[ntpProbeCount++] = ip; }
  if (WiFi.hostByName(ntpServer4.c_str(), ip)) { ntpProbeIps[ntpProbeCount++] = ip; }
}

// Send an NTP query to EVERY configured server concurrently, so whichever
// server is reachable first wins (vs. the sequential SNTP client which waits a
// full timeout per server).
void ntpProbeSend() {
  uint8_t q[48] = { 0 };
  q[0] = 0x1b;  // LI=0, VN=3, Mode=3 (client)
  for (int i = 0; i < ntpProbeCount; i++) {
    ntpProbeUdp.beginPacket(ntpProbeIps[i], 123);
    ntpProbeUdp.write(q, 48);
    ntpProbeUdp.endPacket();
  }
}

// Query every configured upstream NTP server concurrently and return the NTP
// epoch seconds (transmit timestamp) of the FIRST valid reply, or 0 if none
// arrives within timeoutMs. This is the direct, uncached path used to answer
// the PS4.
uint32_t ntpFastQuery(unsigned long timeoutMs) {
  if (ntpProbeCount == 0 || WiFi.status() != WL_CONNECTED) return 0;
  ntpProbeSend();
  unsigned long deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    int len = ntpProbeUdp.parsePacket();
    if (len >= 48) {
      uint8_t buf[48];
      ntpProbeUdp.read(buf, 48);
      if ((buf[0] & 0x07) == 4) {  // Mode=4 (server reply)
        uint32_t secs = ((uint32_t)buf[40] << 24) | ((uint32_t)buf[41] << 16) | ((uint32_t)buf[42] << 8) | buf[43];
        if (secs > 2208988800UL) {
          appendLog("NTP upstream " + ntpProbeUdp.remoteIP().toString());
          return secs;  // NTP epoch seconds, used directly for the PS4 reply
        }
      }
    }
    delay(5);  // yield so lwIP processes inbound UDP
  }
  return 0;
}


void setup() {
  // Lower the ESP32-S2 brownout detector threshold from the core default
  // (level 7 ~2.9V = MOST sensitive) to level 4 (~2.7V), so marginal power
  // dips don't trigger a brownout reset that occasionally hangs on recovery.
  // Real low-voltage conditions still reset the chip.
  regi2c_ctrl_write_reg_mask(I2C_BOD, I2C_BOD_HOSTID, I2C_BOD_THRESHOLD, 2, 0, 4);  // level 4
  delay(50);  // let the power rail settle

  //HWSerial.begin(115200);
  //HWSerial.println("Version: " + firmwareVer);
  //USBSerial.begin();
  tSetup = millis();  // boot timing probe start

  // Run the whole boot at max CPU speed (240MHz) so the heavy early work
  // (filesystem mount, config parse, WiFi init) finishes ASAP; applyPowerSettings()
  // at the end of setup drops the clock back to the configured power-save
  // cpuFreq (80/160MHz).
  setCpuFrequencyMhz(240);

  pinMode(38, OUTPUT);
  digitalWrite(38, HIGH);

#if USBCONTROL && defined(CONFIG_IDF_TARGET_ESP32)
  pinMode(usbPin, OUTPUT);
  digitalWrite(usbPin, LOW);
#endif

  if (FILESYS.begin(true)) {
    appendLog("TIMING fs-mount ms=" + String(millis() - tSetup));

#if USECONFIG
    if (FILESYS.exists("/config.ini")) {
      File iniFile = FILESYS.open("/config.ini", "r");
      if (iniFile) {
        String iniData;
        while (iniFile.available()) {
          char chnk = iniFile.read();
          iniData += chnk;
        }
        iniFile.close();

        if (instr(iniData, "AP_SSID=")) {
          AP_SSID = split(iniData, "AP_SSID=", "\r\n");
          AP_SSID.trim();
        }

        if (instr(iniData, "AP_PASS=")) {
          AP_PASS = split(iniData, "AP_PASS=", "\r\n");
          AP_PASS.trim();
        }

        if (instr(iniData, "WEBSERVER_IP=")) {
          String strwIp = split(iniData, "WEBSERVER_IP=", "\r\n");
          strwIp.trim();
          Server_IP.fromString(strwIp);
        }

        if (instr(iniData, "SUBNET_MASK=")) {
          String strsIp = split(iniData, "SUBNET_MASK=", "\r\n");
          strsIp.trim();
          Subnet_Mask.fromString(strsIp);
        }

        if (instr(iniData, "WIFI_SSID=")) {
          WIFI_SSID = split(iniData, "WIFI_SSID=", "\r\n");
          WIFI_SSID.trim();
        }

        if (instr(iniData, "WIFI_PASS=")) {
          WIFI_PASS = split(iniData, "WIFI_PASS=", "\r\n");
          WIFI_PASS.trim();
        }

        if (instr(iniData, "WIFI_HOST=")) {
          WIFI_HOSTNAME = split(iniData, "WIFI_HOST=", "\r\n");
          WIFI_HOSTNAME.trim();
        }

        if (instr(iniData, "USEAP=")) {
          String strua = split(iniData, "USEAP=", "\r\n");
          strua.trim();
          if (strua.equals("true")) {
            startAP = true;
          } else {
            startAP = false;
          }
        }

        if (instr(iniData, "CONWIFI=")) {
          String strcw = split(iniData, "CONWIFI=", "\r\n");
          strcw.trim();
          if (strcw.equals("true")) {
            connectWifi = true;
          } else {
            connectWifi = false;
          }
        }

        if (instr(iniData, "USBWAIT=")) {
          String strusw = split(iniData, "USBWAIT=", "\r\n");
          strusw.trim();
          USB_WAIT = strusw.toInt();
        }

        if (instr(iniData, "ESPSLEEP=")) {
          String strsl = split(iniData, "ESPSLEEP=", "\r\n");
          strsl.trim();
          if (strsl.equals("true")) {
            espSleep = true;
          } else {
            espSleep = false;
          }
        }

        if (instr(iniData, "SLEEPTIME=")) {
          String strslt = split(iniData, "SLEEPTIME=", "\r\n");
          strslt.trim();
          TIME2SLEEP = strslt.toInt();
        }

        if (instr(iniData, "CPU_FREQ=")) {
          String strcf = split(iniData, "CPU_FREQ=", "\r\n");
          strcf.trim();
          cpuFreq = strcf.toInt();
        }

        if (instr(iniData, "LOW_TXPOWER=")) {
          String strltx = split(iniData, "LOW_TXPOWER=", "\r\n");
          strltx.trim();
          if (strltx.equals("true")) { lowTxPower = true; } else { lowTxPower = false; }
        }

        if (instr(iniData, "NTP_SERVER1=")) {
          String strns1 = split(iniData, "NTP_SERVER1=", "\r\n");
          strns1.trim();
          if (strns1.length() > 0) { ntpServer1 = strns1; }
        }

        if (instr(iniData, "NTP_SERVER2=")) {
          String strns2 = split(iniData, "NTP_SERVER2=", "\r\n");
          strns2.trim();
          if (strns2.length() > 0) { ntpServer2 = strns2; }
        }

        if (instr(iniData, "NTP_SERVER3=")) {
          String strns3 = split(iniData, "NTP_SERVER3=", "\r\n");
          strns3.trim();
          if (strns3.length() > 0) { ntpServer3 = strns3; }
        }

        if (instr(iniData, "NTP_SERVER4=")) {
          String strns4 = split(iniData, "NTP_SERVER4=", "\r\n");
          strns4.trim();
          if (strns4.length() > 0) { ntpServer4 = strns4; }
        }

        if (instr(iniData, "USE_NTP=")) {
          String struntp = split(iniData, "USE_NTP=", "\r\n");
          struntp.trim();
          useNtp = struntp.equals("true");
        }

        if (instr(iniData, "USE_LOG=")) {
          String strulog = split(iniData, "USE_LOG=", "\r\n");
          strulog.trim();
          useLog = strulog.equals("true");
        }
      }
    } else {
      writeConfig();
    }
#endif

    // Boot/shutdown diagnostic marker (after config parse so USE_LOG applies).
    appendLog("BOOT reset=" + resetReasonStr() + " freeheap=" + String(ESP.getFreeHeap()) + " uptime_ms=" + String(millis()));

  } else {
    //HWSerial.println("Filesystem failed to mount");
  }

  if (startAP) {
    //HWSerial.println("SSID: " + AP_SSID);
    //HWSerial.println("Password: " + AP_PASS);
    //HWSerial.println("");
    //HWSerial.println("WEB Server IP: " + Server_IP.toString());
    //HWSerial.println("Subnet: " + Subnet_Mask.toString());
    //HWSerial.println("WEB Server Port: " + String(WEB_PORT));
    //HWSerial.println("");
    WiFi.softAPConfig(Server_IP, Server_IP, Subnet_Mask);
    WiFi.softAP(AP_SSID.c_str(), AP_PASS.c_str());
    //HWSerial.println("WIFI AP started");
    dnsServer.setTTL(30);
    dnsServer.setErrorReplyCode(DNSReplyCode::ServerFailure);
    dnsServer.start(53, "*", Server_IP);
    appendLog("TIMING ap-start ms=" + String(millis() - tSetup));
    //HWSerial.println("DNS server started");
    //HWSerial.println("DNS Server IP: " + Server_IP.toString());
  }

  if (connectWifi && WIFI_SSID.length() > 0 && WIFI_PASS.length() > 0) {
    WiFi.setAutoConnect(true);
    WiFi.setAutoReconnect(true);
    WiFi.hostname(WIFI_HOSTNAME);
    WiFi.begin(WIFI_SSID.c_str(), WIFI_PASS.c_str());
    appendLog("TIMING sta-begin ms=" + String(millis() - tSetup));
    // Async: do NOT block setup() waiting for the STA link. The AP+DNS server
    // above is already up (119ms), so the PS4 can be served immediately. The
    // STA-dependent init (mDNS, DNS-for-LAN when AP off, NTP probe resolution)
    // is done once in loop() once WiFi connects.
  }


  //Reduce power draw now that the WiFi stack is running
  applyPowerSettings();

  // Answer the PS4's NTP requests on UDP port 123 by querying the configured
  // upstream NTP servers (NTP_SERVER1/2/3) concurrently on every request. No
  // local clock is cached (the S2 has no external RTC crystal, so a cached time
  // would drift), so configTime is intentionally NOT used.
  if (useNtp) {
    ntpServerUdp.begin(123);
    // NTP probe hostname resolution is deferred to loop()'s STA-ready handler
    // (WiFi connects asynchronously; ntpProbeStart() needs a working link for
    // the blocking hostByName calls).
  }

  // GoldHEN syncs time via NTP to a hardcoded server (time1.google.com =
  // 216.239.35.0:123), bypassing DNS. Enable lwIP NAT on the AP and add a DNAT
  // portmap so those packets are redirected to the local NTP server. (Requires
  // the rebuilt core liblwip.a with CONFIG_LWIP_IPV4_NAPT; IP_FORWARD is off.)
#if IP_NAPT
  if (useNtp) {
    ip_napt_enable((u32_t)Server_IP, 1);
    ip_portmap_add(17, (u32_t)IPAddress(216, 239, 35, 0), 123, (u32_t)Server_IP, 123);  // 17 = IPPROTO_UDP
  }
#endif


  server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Microsoft Connect Test");
  });
#if !USBCONTROL && defined(CONFIG_IDF_TARGET_ESP32)
  server.on("/cache.manifest", HTTP_GET, [](AsyncWebServerRequest *request) {
    handleCacheManifest(request);
  });
#endif

#if USECONFIG
  server.on("/config.ini", HTTP_ANY, [](AsyncWebServerRequest *request) {
    request->send(404);
  });
#endif

  server.on("/upload.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", upload_gz, sizeof(upload_gz));
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

  server.on(
    "/upload.html", HTTP_POST, [](AsyncWebServerRequest *request) {
      request->redirect("/fileman.html");
    },
    handleFileUpload);

  server.on("/fileman.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    handleFileMan(request);
  });

  server.on("/delete", HTTP_POST, [](AsyncWebServerRequest *request) {
    handleDelete(request);
  });

#if USECONFIG
  server.on("/config.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    handleConfigHtml(request);
  });

  server.on("/config.html", HTTP_POST, [](AsyncWebServerRequest *request) {
    handleConfig(request);
  });
#endif

  server.on("/admin.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", admin_gz, sizeof(admin_gz));
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

  server.on("/reboot.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", reboot_gz, sizeof(reboot_gz));
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

  server.on("/reboot.html", HTTP_POST, [](AsyncWebServerRequest *request) {
    handleReboot(request);
  });

  server.on("/update.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", update_gz, sizeof(update_gz));
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

  server.on(
    "/update.html", HTTP_POST, [](AsyncWebServerRequest *request) {
    },
    handleFwUpdate);

  server.on("/info.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    handleInfo(request);
  });

  server.on("/usbon", HTTP_POST, [](AsyncWebServerRequest *request) {
    enableUSB();
    request->send(200, "text/plain", "ok");
  });

  server.on("/usboff", HTTP_POST, [](AsyncWebServerRequest *request) {
    disableUSB();
    request->send(200, "text/plain", "ok");
  });

  server.on("/format.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", format_gz, sizeof(format_gz));
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });

  server.on("/format.html", HTTP_POST, [](AsyncWebServerRequest *request) {
    isFormating = true;
    request->send(304);
  });

  server.on("/dlall", HTTP_GET, [](AsyncWebServerRequest *request) {
    handleDlFiles(request);
  });

  server.on("/jzip.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/javascript", jzip_gz, sizeof(jzip_gz));
    response->addHeader("Content-Encoding", "gzip");
    request->send(response);
  });


  server.serveStatic("/", FILESYS, "/").setDefaultFile("index.html");

  server.onNotFound([](AsyncWebServerRequest *request) {
    //HWSerial.println(request->url());
    String path = request->url();
    if (instr(path, "/update/ps4/")) {
      String Region = split(path, "/update/ps4/list/", "/");
      handleConsoleUpdate(Region, request);
      return;
    }
    if (instr(path, "/document/") && instr(path, "/ps4/")) {
      request->redirect("http://" + WIFI_HOSTNAME + "/index.html");
      return;
    }
    if (path.endsWith("index.html") || path.endsWith("index.htm") || path.endsWith("/")) {
      AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_gz, sizeof(index_gz));
      response->addHeader("Content-Encoding", "gzip");
      request->send(response);
      return;
    }
    if (path.endsWith("style.css")) {
      AsyncWebServerResponse *response = request->beginResponse_P(200, "text/css", style_gz, sizeof(style_gz));
      response->addHeader("Content-Encoding", "gzip");
      request->send(response);
      return;
    }
#if !USBCONTROL && defined(CONFIG_IDF_TARGET_ESP32)
    if (path.endsWith("menu.html")) {
      AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", menu_gz, sizeof(menu_gz));
      response->addHeader("Content-Encoding", "gzip");
      request->send(response);
      return;
    }
#endif
#if PSFREE
    if (path.endsWith("exploit.js")) {
      AsyncWebServerResponse *response = request->beginResponse_P(200, "text/javascript", psf_gz, sizeof(psf_gz));
      response->addHeader("Content-Encoding", "gzip");
      request->send(response);
      return;
    }
#endif
    if (path.endsWith("payloads.html")) {
      handlePayloads(request);
      return;
    }
    if (path.endsWith("loader.html")) {
      AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", loader_gz, sizeof(loader_gz));
      response->addHeader("Content-Encoding", "gzip");
      request->send(response);
      return;
    }

    request->send(404);
  });


  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  server.begin();
  //HWSerial.println("HTTP server started");

  if (TIME2SLEEP < 5) { TIME2SLEEP = 5; }  //min sleep time


  appendLog("TIMING setup-done ms=" + String(millis() - tSetup) + " abs=" + String(millis()));


  bootTime = millis();
}


#if defined(CONFIG_IDF_TARGET_ESP32S2) | defined(CONFIG_IDF_TARGET_ESP32S3)
static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (lba > 4) { lba = 4; }
  memcpy(buffer, exfathax[lba] + offset, bufsize);
  return bufsize;
}

void enableUSB() {
  dev.vendorID("PS4");
  dev.productID("ESP32 Server");
  dev.productRevision("1.0");
  dev.onRead(onRead);
  dev.mediaPresent(true);
  dev.begin(8192, 512);
  USB.begin();
  enTime = millis();
  hasEnabled = true;
}

void disableUSB() {
  enTime = 0;
  hasEnabled = false;
  dev.end();
  ESP.restart();
}
#else
void enableUSB() {
  digitalWrite(usbPin, HIGH);
  enTime = millis();
  hasEnabled = true;
}

void disableUSB() {
  enTime = 0;
  hasEnabled = false;
  digitalWrite(usbPin, LOW);
}
#endif


void loop() {
  if (espSleep && !isFormating) {
    if (millis() >= (bootTime + (TIME2SLEEP * 60000))) {
      //HWSerial.print("Esp sleep");
      digitalWrite(38, HIGH);
      gpio_hold_en((gpio_num_t)38);
      gpio_deep_sleep_hold_en();
      esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
      esp_deep_sleep_start();
      return;
    }
  }
  if (hasEnabled && millis() >= (enTime + 15000)) {
    disableUSB();
  }

  // Async STA init: once the home-WiFi link comes up, run the STA-dependent
  // setup that used to block in setup() (mDNS, LAN DNS when AP is off, and NTP
  // probe hostname resolution). Runs once; the AP+DNS server is already live
  // so the PS4 is never blocked waiting for this.
  if (connectWifi && WiFi.status() == WL_CONNECTED) {
    static bool staReady = false;
    if (!staReady) {
      staReady = true;
      IPAddress LAN_IP = WiFi.localIP();
      if (LAN_IP) {
        String mdnsHost = WIFI_HOSTNAME;
        mdnsHost.replace(".local", "");
        MDNS.begin(mdnsHost.c_str());
        if (!startAP) {
          dnsServer.setTTL(30);
          dnsServer.setErrorReplyCode(DNSReplyCode::ServerFailure);
          dnsServer.start(53, "*", LAN_IP);
        }
      }
      if (useNtp) { ntpProbeStart(); }
      appendLog("TIMING sta-ready ms=" + String(millis() - tSetup));
    }
  }
  if (isFormating) {
    //HWSerial.print("Formatting Storage");
    isFormating = false;
    FILESYS.end();
    FILESYS.format();
    FILESYS.begin(true);
    delay(1000);
#if USECONFIG
    writeConfig();
#endif
  }

  dnsServer.processNextRequest();

  // DIAGNOSTIC: log each DNS query the ESP32 answers (source IP + domain), via
  // the DNSServer lib's query counter so only genuinely new queries are logged.
  // Remove when diagnosis done.
  if (useLog) {
    static uint32_t lastDnsCount = 0;
    uint32_t qc = dnsServer.getQueryCount();
    if (qc != lastDnsCount) {
      lastDnsCount = qc;
      appendLog("DNS " + dnsServer.getLastQueryIP().toString() + " " + dnsServer.getLastQueryDomain());
    }
  }

  if (useNtp) {
    // No local clock is kept — each PS4 NTP request is answered directly from
    // the fastest live upstream query (see handleNTPRequest/ntpFastQuery).
    handleNTPRequest();
  }
}
