/*
 * mogo's FTP server for esp8266
 * Shamelessly forked from https://github.com/nailbuster/esp8266FTPServer and formatted/tweaked.
 *
 * === Original comment below ===
 * FTP Serveur for ESP8266
 * based on FTP Serveur for Arduino Due and Ethernet shield (W5100) or WIZ820io (W5200)
 * based on Jean-Michel Gallego's work
 * modified to work with esp8266 SPIFFS by David Paiva david@nailbuster.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ESP8266FtpServer.h"

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <FS.h>

// Uncomment the line below to enable debug to serial port.
#define FTP_DEBUG Serial

#ifndef FTP_DEBUG
struct {
  static void println(const char*) {
  }
  static void println(const String&) {
  }
  static void print(char) {
  }
} NullDebug;
#define FTP_DEBUG NullDebug
#endif  // FTP_DEBUG

FtpServer::FtpServer() :
    ftpServer(FTP_CTRL_PORT), dataServer(FTP_DATA_PORT_PASV) {
}

void FtpServer::setup(String uname, String pword) {
  // Tells the ftp server to begin listening for incoming connection
  _FTP_USER = uname;
  _FTP_PASS = pword;

  ftpServer.begin();
  delay(10);
  dataServer.begin();
  delay(10);
  millisTimeOut = (uint32_t) FTP_TIME_OUT * 60 * 1000;
  millisDelay = 0;
  cmdStatus = 0;
  iniVariables();
}

void FtpServer::iniVariables() {
  // Default for data port
  dataPort = FTP_DATA_PORT_PASV;

  // Default Data connection is Active
  dataPassiveConn = true;

  // Set the root directory
  strcpy(cwdName, "/");

  rnfrCmd = false;
  transferStatus = 0;
}

void FtpServer::loop() {
  if ((int32_t) (millisDelay - millis()) > 0) {
    return;
  }

  if (ftpServer.hasClient()) {
    client.stop();
    client = ftpServer.available();
  }

  if (cmdStatus == 0) {
    if (client.connected()) {
      disconnectClient();
    }
    cmdStatus = 1;
  } else if (cmdStatus == 1) {
    // Ftp server waiting for connection
    abortTransfer();
    iniVariables();
    FTP_DEBUG.println(
        "Ftp server waiting for connection on port " + String(FTP_CTRL_PORT));
    cmdStatus = 2;
  } else if (cmdStatus == 2) {
    // Ftp server idle
    if (client.connected()) {
      // A client connected
      clientConnected();
      millisEndConnection = millis() + 10 * 1000; // wait client id during 10 s.
      cmdStatus = 3;
    }
  } else if (readChar() > 0) {
    // got response
    if (cmdStatus == 3) {
      // Ftp server waiting for user identity
      if (userIdentity()) {
        cmdStatus = 4;
      } else {
        cmdStatus = 0;
      }
    } else if (cmdStatus == 4) {
      // Ftp server waiting for user registration
      if (userPassword()) {
        cmdStatus = 5;
        millisEndConnection = millis() + millisTimeOut;
      } else {
        cmdStatus = 0;
      }
    } else if (cmdStatus == 5) {
      // Ftp server waiting for user command
      if (!processCommand()) {
        cmdStatus = 0;
      } else {
        millisEndConnection = millis() + millisTimeOut;
      }
    }
  } else if (!client.connected() || !client) {
    cmdStatus = 1;
    FTP_DEBUG.println("client disconnected");
  }

  if (transferStatus == 1) {
    // Retrieve data
    if (!doRetrieve()) {
      transferStatus = 0;
    }
  } else if (transferStatus == 2) {
    // Store data
    if (!doStore()) {
      transferStatus = 0;
    }
  } else if (cmdStatus > 2
      && !((int32_t) (millisEndConnection - millis()) > 0)) {
    client.println("530 Timeout");
    millisDelay = millis() + 200;    // delay of 200 ms
    cmdStatus = 0;
  }
}

void FtpServer::clientConnected() {
  FTP_DEBUG.println("Client connected!");
  client.println("220--- Welcome to FTP for ESP8266 ---");
  client.println("220---   By David Paiva   ---");
  client.println("220 --   Version " + String(FTP_SERVER_VERSION) + "   --");
  iCL = 0;
}

void FtpServer::disconnectClient() {
  FTP_DEBUG.println(" Disconnecting client");
  abortTransfer();
  client.println("221 Goodbye");
  client.stop();
}

bool FtpServer::userIdentity() {
  if (strcmp(command, "USER")) {
    client.println("500 Syntax error");
  } else if (strcmp(parameters, _FTP_USER.c_str())) {
    client.println("530 user not found");
  } else {
    client.println("331 OK. Password required");
    strcpy(cwdName, "/");
    return true;
  }
  millisDelay = millis() + 100;  // delay of 100 ms
  return false;
}

bool FtpServer::userPassword() {
  if (strcmp(command, "PASS")) {
    client.println("500 Syntax error");
  } else if (strcmp(parameters, _FTP_PASS.c_str())) {
    client.println("530 ");
  } else {
    FTP_DEBUG.println("OK. Waiting for commands.");
    client.println("230 OK.");
    return true;
  }
  millisDelay = millis() + 100;  // delay of 100 ms
  return false;
}

bool FtpServer::processCommand() {
  // Access control commands
  if (!strcmp(command, "CDUP")) {
    //  CDUP - Change to Parent Directory
    client.println("250 Ok. Current directory is " + String(cwdName));
    return true;
  }
  if (!strcmp(command, "CWD")) {
    //  CWD - Change Working Directory
    if (strcmp(parameters, ".") == 0) {  // 'CWD .' is the same as PWD command
      client.println(
          "257 \"" + String(cwdName) + "\" is your current directory");
    } else {
      client.println("250 Ok. Current directory is " + String(cwdName));
    }
    return true;
  }
  if (!strcmp(command, "PWD")) {
    //  PWD - Print Directory
    client.println("257 \"" + String(cwdName) + "\" is your current directory");
    return true;
  }
  if (!strcmp(command, "QUIT")) {
    disconnectClient();
    return false;
  }

  // Transfer parameters commands.
  if (!strcmp(command, "MODE")) {
    //  MODE - Transfer Mode
    if (!strcmp(parameters, "S")) {
      client.println("200 S Ok");
    } else {
      client.println("504 Only S(tream) is supported");
    }
    return true;
  }
  if (!strcmp(command, "PASV")) {
    //  PASV - Passive Connection management
    if (data.connected()) {
      data.stop();
    }
    dataIp = WiFi.localIP();
    dataPort = FTP_DATA_PORT_PASV;
    FTP_DEBUG.println("Connection management set to passive");
    FTP_DEBUG.println("Data port set to " + String(dataPort));
    client.println(
        "227 Entering Passive Mode (" + String(dataIp[0]) + ","
            + String(dataIp[1]) + "," + String(dataIp[2]) + ","
            + String(dataIp[3]) + "," + String(dataPort >> 8) + ","
            + String(dataPort & 255) + ").");
    dataPassiveConn = true;
    return true;
  }
  if (!strcmp(command, "PORT")) {
    //  PORT - Data Port
    if (data) {
      data.stop();
    }
    // get IP of data client
    dataIp[0] = atoi(parameters);
    char * p = strchr(parameters, ',');
    for (uint8_t i = 1; i < 4; i++) {
      dataIp[i] = atoi(++p);
      p = strchr(p, ',');
    }
    // get port of data client
    dataPort = 256 * atoi(++p);
    p = strchr(p, ',');
    dataPort += atoi(++p);
    if (p == NULL) {
      client.println("501 Can't interpret parameters");
    } else {
      client.println("200 PORT command successful");
      dataPassiveConn = false;
    }
    return true;
  }
  if (!strcmp(command, "STRU")) {
    //  STRU - File Structure
    if (!strcmp(parameters, "F")) {
      client.println("200 F Ok");
    } else {
      client.println("504 Only F(ile) is supported");
    }
    return true;
  }
  if (!strcmp(command, "TYPE")) {
    //  TYPE - Data Type
    if (!strcmp(parameters, "A")) {
      client.println("200 TYPE is now ASII");
    } else if (!strcmp(parameters, "I")) {
      client.println("200 TYPE is now 8-bit binary");
    } else {
      client.println("504 Unknow TYPE");
    }
    return true;
  }

  // FTP service commands.
  if (!strcmp(command, "ABOR")) {
    //  ABOR - Abort
    abortTransfer();
    client.println("226 Data connection closed");
    return true;
  }
  if (!strcmp(command, "DELE")) {
    //  DELE - Delete a File
    char path[ FTP_CWD_SIZE];
    if (strlen(parameters) == 0) {
      client.println("501 No file name");
    } else if (makePath(path)) {
      if (!SPIFFS.exists(path)) {
        client.println("550 File " + String(parameters) + " not found");
      } else {
        if (SPIFFS.remove(path)) {
          client.println("250 Deleted " + String(parameters));
        } else {
          client.println("450 Can't delete " + String(parameters));
        }
      }
    }
    return true;
  }
  if (!strcmp(command, "LIST")) {
    // LIST - List
    if (!dataConnect()) {
      client.println("425 No data connection");
    } else {
      client.println("150 Accepted data connection");
      uint16_t nm = 0;
      Dir dir = SPIFFS.openDir(cwdName);
      if (!SPIFFS.exists(cwdName)) {
        client.println("550 Can't open directory " + String(cwdName));
      } else {
        while (dir.next()) {
          String fn, fs;
          fn = dir.fileName();
          fn.remove(0, 1);
          fs = String(dir.fileSize());
          data.println("+r,s" + fs);
          data.println(",\t" + fn);
          nm++;
        }
        client.println("226 " + String(nm) + " matches total");
      }
      data.stop();
    }
    return true;
  }
  if (!strcmp(command, "MLSD")) {
    //  MLSD - Listing for Machine Processing (see RFC 3659)
    if (!dataConnect()) {
      client.println("425 No data connection MLSD");
    } else {
      client.println("150 Accepted data connection");
      uint16_t nm = 0;
      Dir dir = SPIFFS.openDir(cwdName);
      while (dir.next()) {
        String fn, fs;
        fn = dir.fileName();
        fn.remove(0, 1);
        fs = String(dir.fileSize());
        data.println(
            "Type=file;Size=" + fs + ";" + "modify=20000101160656;" + " " + fn);
        nm++;
      }
      client.println("226-options: -a -l");
      client.println("226 " + String(nm) + " matches total");
      data.stop();
    }
    return true;
  }
  if (!strcmp(command, "NLST")) {
    if (!dataConnect()) {
      client.println("425 No data connection");
    } else {
      client.println("150 Accepted data connection");
      uint16_t nm = 0;
      Dir dir = SPIFFS.openDir(cwdName);
      if (!SPIFFS.exists(cwdName)) {
        client.println("550 Can't open directory " + String(parameters));
      } else {
        while (dir.next()) {
          data.println(dir.fileName());
          nm++;
        }
        client.println("226 " + String(nm) + " matches total");
      }
      data.stop();
    }
    return true;
  }
  if (!strcmp(command, "NOOP")) {
    client.println("200 Zzz...");
    return true;
  }
  if (!strcmp(command, "RETR")) {
    //  RETR - Retrieve
    char path[ FTP_CWD_SIZE];
    if (strlen(parameters) == 0) {
      client.println("501 No file name");
    } else if (makePath(path)) {
      file = SPIFFS.open(path, "r");
      if (!file) {
        client.println("550 File " + String(parameters) + " not found");
      } else if (!file) {
        client.println("450 Can't open " + String(parameters));
      } else if (!dataConnect()) {
        client.println("425 No data connection");
      } else {
        FTP_DEBUG.println("Sending " + String(parameters));
        client.println("150-Connected to port " + String(dataPort));
        client.println("150 " + String(file.size()) + " bytes to download");
        millisBeginTrans = millis();
        bytesTransfered = 0;
        transferStatus = 1;
      }
    }
    return true;
  }
  if (!strcmp(command, "STOR")) {
    //  STOR - Store
    char path[ FTP_CWD_SIZE];
    if (strlen(parameters) == 0) {
      client.println("501 No file name");
    } else if (makePath(path)) {
      file = SPIFFS.open(path, "w");
      if (!file) {
        client.println("451 Can't open/create " + String(parameters));
      } else if (!dataConnect()) {
        client.println("425 No data connection");
        file.close();
      } else {
        FTP_DEBUG.println("Receiving " + String(parameters));
        client.println("150 Connected to port " + String(dataPort));
        millisBeginTrans = millis();
        bytesTransfered = 0;
        transferStatus = 2;
      }
    }
    return true;
  }
  if (!strcmp(command, "MKD")) {
    //  MKD - Make Directory
    client.println("550 Can't create \"" + String(parameters)); //not support on espyet
    return true;
  }
  if (!strcmp(command, "RMD")) {
    //  RMD - Remove a Directory
    client.println("501 Can't delete \"" + String(parameters));
    return true;
  }
  if (!strcmp(command, "RNFR")) {
    //  RNFR - Rename From
    buf[0] = 0;
    if (strlen(parameters) == 0) {
      client.println("501 No file name");
    } else if (makePath(buf)) {
      if (!SPIFFS.exists(buf)) {
        client.println("550 File " + String(parameters) + " not found");
      } else {
        FTP_DEBUG.println("Renaming " + String(buf));
        client.println(
            "350 RNFR accepted - file exists, ready for destination");
        rnfrCmd = true;
      }
    }
    return true;
  }
  if (!strcmp(command, "RNTO")) {
    // RNTO - Rename To
    char path[ FTP_CWD_SIZE];
    if (strlen(buf) == 0 || !rnfrCmd) {
      client.println("503 Need RNFR before RNTO");
    } else if (strlen(parameters) == 0) {
      client.println("501 No file name");
    } else if (makePath(path)) {
      if (SPIFFS.exists(path)) {
        client.println("553 " + String(parameters) + " already exists");
      } else {
        FTP_DEBUG.println("Renaming " + String(buf) + " to " + String(path));
        if (SPIFFS.rename(buf, path))
          client.println("250 File successfully renamed or moved");
        else
          client.println("451 Rename/move failure");

      }
    }
    rnfrCmd = false;
    return true;
  }

  // Extension commands (RFC 3659)
  if (!strcmp(command, "FEAT")) {
    //  FEAT - New Features
    client.println("211-Extensions supported:");
    client.println(" MLSD");
    client.println("211 End.");
    return true;
  }
  if (!strcmp(command, "MDTM")) {
    //  MDTM - File Modification Time (see RFC 3659)
    client.println("550 Unable to retrieve time");
    return true;
  }
  if (!strcmp(command, "SIZE")) {
    //  SIZE - Size of the file
    char path[ FTP_CWD_SIZE];
    if (strlen(parameters) == 0) {
      client.println("501 No file name");
    } else if (makePath(path)) {
      file = SPIFFS.open(path, "r");
      if (!file)
        client.println("450 Can't open " + String(parameters));
      else {
        client.println("213 " + String(file.size()));
        file.close();
      }
    }
    return true;
  }
  if (!strcmp(command, "SITE")) {
    //  SITE - System command
    client.println("500 Unknow SITE command " + String(parameters));
    return true;
  }

  //  Unrecognized commands ...
  client.println("500 Unknow command");
  return true;
}

bool FtpServer::dataConnect() {
  unsigned long startTime = millis();
  //wait 5 seconds for a data connection
  if (!data.connected()) {
    // TODO(mogo): Bad, should wait ever.
    while (!dataServer.hasClient() && millis() - startTime < 10000) {
      //delay(100);
      yield();
    }
    if (dataServer.hasClient()) {
      data.stop();
      data = dataServer.available();
      FTP_DEBUG.println("ftpdataserver client....");
    }
  }
  return data.connected();
}

bool FtpServer::doRetrieve() {
  //int16_t nb = file.readBytes((uint8_t*) buf, FTP_BUF_SIZE );
  int16_t nb = file.readBytes(buf, FTP_BUF_SIZE);
  if (nb > 0) {
    data.write((uint8_t*) buf, nb);
    bytesTransfered += nb;
    return true;
  }
  closeTransfer();
  return false;
}

bool FtpServer::doStore() {
  if (data.connected()) {
    int16_t nb = data.readBytes((uint8_t*) buf, FTP_BUF_SIZE);
    if (nb > 0) {
      // Serial.println( millis() << " " << nb << endl;
      file.write((uint8_t*) buf, nb);
      bytesTransfered += nb;
    }
    return true;
  }
  closeTransfer();
  return false;
}

void FtpServer::closeTransfer() {
  uint32_t deltaT = (int32_t) (millis() - millisBeginTrans);
  if (deltaT > 0 && bytesTransfered > 0) {
    client.println("226-File successfully transferred");
    client.println(
        "226 " + String(deltaT) + " ms, " + String(bytesTransfered / deltaT)
            + " kbytes/s");
  } else {
    client.println("226 File successfully transferred");
  }

  file.close();
  data.stop();
}

void FtpServer::abortTransfer() {
  if (transferStatus > 0) {
    file.close();
    data.stop();
    client.println("426 Transfer aborted");
    FTP_DEBUG.println("Transfer aborted!");
  }
  transferStatus = 0;
}

// Read a char from client connected to ftp server
//
//  update cmdLine and command buffers, iCL and parameters pointers
//
//  return:
//    -2 if buffer cmdLine is full
//    -1 if line not completed
//     0 if empty line received
//    length of cmdLine (positive) if no empty line received

int8_t FtpServer::readChar() {
  int8_t rc = -1;

  if (client.available()) {
    char c = client.read();
    // char c;
    // client.readBytes((uint8_t*) c, 1);
    FTP_DEBUG.print(c);
    if (c == '\\') {
      c = '/';
    }
    if (c != '\r') {
      if (c != '\n') {
        if (iCL < FTP_CMD_SIZE) {
          cmdLine[iCL++] = c;
        } else {
          rc = -2; //  Line too long
        }
      } else {
        cmdLine[iCL] = 0;
        command[0] = 0;
        parameters = NULL;
        // empty line?
        if (iCL == 0) {
          rc = 0;
        } else {
          rc = iCL;
          // search for space between command and parameters
          parameters = strchr(cmdLine, ' ');
          if (parameters != NULL) {
            if (parameters - cmdLine > 4) {
              rc = -2; // Syntax error
            } else {
              strncpy(command, cmdLine, parameters - cmdLine);
              command[parameters - cmdLine] = 0;

              while (*(++parameters) == ' ') {
              }
            }
          } else if (strlen(cmdLine) > 4) {
            rc = -2; // Syntax error.
          } else {
            strcpy(command, cmdLine);
          }
          iCL = 0;
        }
      }
    }
    if (rc > 0) {
      for (uint8_t i = 0; i < strlen(command); i++) {
        command[i] = toupper(command[i]);
      }
    }
    if (rc == -2) {
      iCL = 0;
      client.println("500 Syntax error");
    }
  }
  return rc;
}

// Make complete path/name from cwdName and parameters
//
// 3 possible cases: parameters can be absolute path, relative path or only the name
//
// parameters:
//   fullName : where to store the path/name
//
// return:
//    true, if done

bool FtpServer::makePath(char * fullName) {
  return makePath(fullName, parameters);
}

bool FtpServer::makePath(char * fullName, char * param) {
  if (param == NULL) {
    param = parameters;
  }

  // Root or empty?
  if (strcmp(param, "/") == 0 || strlen(param) == 0) {
    strcpy(fullName, "/");
    return true;
  }
  // If relative path, concatenate with current dir
  if (param[0] != '/') {
    strcpy(fullName, cwdName);
    if (fullName[strlen(fullName) - 1] != '/') {
      strncat(fullName, "/", FTP_CWD_SIZE);
    }
    strncat(fullName, param, FTP_CWD_SIZE);
  } else {
    strcpy(fullName, param);
  }
  // If ends with '/', remove it
  uint16_t strl = strlen(fullName) - 1;
  if (fullName[strl] == '/' && strl > 1) {
    fullName[strl] = 0;
  }
  if (strlen(fullName) < FTP_CWD_SIZE) {
    return true;
  }

  client.println("500 Command line too long");
  return false;
}

// Calculate year, month, day, hour, minute and second
//   from first parameter sent by MDTM command (YYYYMMDDHHMMSS)
//
// parameters:
//   pyear, pmonth, pday, phour, pminute and psecond: pointer of
//     variables where to store data
//
// return:
//    0 if parameter is not YYYYMMDDHHMMSS
//    length of parameter + space

uint8_t FtpServer::getDateTime(uint16_t * pyear, uint8_t * pmonth,
    uint8_t * pday, uint8_t * phour, uint8_t * pminute, uint8_t * psecond) {
  char dt[15];

  // Date/time are expressed as a 14 digits long string
  //   terminated by a space and followed by name of file
  if (strlen(parameters) < 15 || parameters[14] != ' ') {
    return 0;
  }
  for (uint8_t i = 0; i < 14; i++) {
    if (!isdigit(parameters[i])) {
      return 0;
    }
  }

  strncpy(dt, parameters, 14);
  dt[14] = 0;
  *psecond = atoi(dt + 12);
  dt[12] = 0;
  *pminute = atoi(dt + 10);
  dt[10] = 0;
  *phour = atoi(dt + 8);
  dt[8] = 0;
  *pday = atoi(dt + 6);
  dt[6] = 0;
  *pmonth = atoi(dt + 4);
  dt[4] = 0;
  *pyear = atoi(dt);
  return 15;
}

// Create string YYYYMMDDHHMMSS from date and time
//
// parameters:
//    date, time
//    tstr: where to store the string. Must be at least 15 characters long
//
// return:
//    pointer to tstr

char * FtpServer::makeDateTimeStr(char * tstr, uint16_t date, uint16_t time) {
  sprintf(tstr, "%04u%02u%02u%02u%02u%02u", ((date & 0xFE00) >> 9) + 1980,
      (date & 0x01E0) >> 5, date & 0x001F, (time & 0xF800) >> 11,
      (time & 0x07E0) >> 5, (time & 0x001F) << 1);
  return tstr;
}

