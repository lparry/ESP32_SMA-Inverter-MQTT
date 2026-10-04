/* MIT License

Copyright (c) 2022 Lupo135
Copyright (c) 2023 darrylb123

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "SMA_Inverter.h"
#include "BluetoothAuthObserver.h"
#include <memory>

int32_t  value32 = 0;
uint64_t value64 = 0;
uint64_t totalWh = 0;
uint64_t totalWh_prev = 0;
time_t   dateTime = 0;

const char btPin[] = {'0','0','0','0',0}; // BT pin Always 0000. (not login passcode!)

InverterData ESP32_SMA_Inverter::invData = InverterData();
DisplayData ESP32_SMA_Inverter::dispData = DisplayData();


bool ESP32_SMA_Inverter::begin(String localName, bool isMaster) {
  const bool started = serialBT.begin("ESP32toSMA", true);   // "true" creates this device as a BT Master.
  if (!started) {
    discardBtRx.store(true, std::memory_order_release);
    clearReceiveQueue();
    // BluetoothSerial::begin may fail after creating some of its static
    // queues, task, or controller state. end() resets those resources so a
    // later begin() attempt does not repeat initialization over partial state.
    serialBT.end();
    btRxCallbackActive.store(false, std::memory_order_release);
    btConnected = false;
    return false;
  }

  // In the pinned Arduino-ESP32 core, registering onData replaces the
  // BluetoothSerial 512-byte queue. Copy SPP data into a larger SPSC queue
  // so bounded web/MQTT work cannot block the Bluetooth receive callback.
  clearReceiveQueue();
  btRxCallbackActive.store(true, std::memory_order_release);
  serialBT.onData([this](const uint8_t *data, size_t length) {
    enqueueBluetoothData(data, length);
  });
  if (!serialBT.setPin(&btPin[0])) {
    discardBtRx.store(true, std::memory_order_release);
    clearReceiveQueue();
    serialBT.end();
    btRxCallbackActive.store(false, std::memory_order_release);
    btConnected = false;
    return false;
  }
  return true;
}


bool ESP32_SMA_Inverter::connect(uint8_t remoteAddress[]) {
  // Close any prior link and discard its queued data before accepting bytes
  // from the next connection attempt. The inverter sends its first handshake
  // packet as soon as SPP opens, while serialBT.connect() is still waiting.
  disconnect();
  BluetoothAuthObserver::beginAttempt(remoteAddress);
  discardBtRx.store(false, std::memory_order_release);
  bool bGotConnected = serialBT.connect(remoteAddress);
  const bool targetAuthenticationFailed = BluetoothAuthObserver::finishAttempt();
  if (!bGotConnected) {
    // Do not let a partial/late packet from a failed attempt become the first
    // bytes of a later session.
    disconnect();
  }
  btConnected = bGotConnected;
  if (!bGotConnected && targetAuthenticationFailed && !authRecoveryAttempted) {
    authRecoveryAttempted = true;
    logW("Bluetooth authentication failed; unpairing inverter once before retry");
    reconnectRequested = unpair(remoteAddress);
  }
  return bGotConnected; 
}

bool ESP32_SMA_Inverter::unpair(uint8_t remoteAddress[]) {
  disconnect();
  logW("Saved Bluetooth bonds before unpair: %d", esp_bt_gap_get_bond_device_num());
  if (!serialBT.unpairDevice(remoteAddress)) {
    logE("Inverter unpair request failed");
    return false;
  }
  // Bond removal is asynchronous. Confirm completion before reconnecting.
  const uint32_t started = millis();
  do {
    int count = esp_bt_gap_get_bond_device_num();
    if (count == 0) {
      logW("Inverter bond removed; PIN pairing will run on the next connection");
      return true;
    }
    if (count < 0) break;
    std::unique_ptr<esp_bd_addr_t[]> bonds(new esp_bd_addr_t[count]);
    if (esp_bt_gap_get_bond_device_list(&count, bonds.get()) != ESP_OK) break;
    bool present = false;
    for (int i = 0; i < count; ++i) {
      if (memcmp(bonds[i], remoteAddress, ESP_BD_ADDR_LEN) == 0) present = true;
    }
    if (!present) {
      logW("Inverter bond removed; PIN pairing will run on the next connection");
      return true;
    }
    delay(25);
  } while ((uint32_t)(millis() - started) < 1000);
  logE("Inverter bond removal was not confirmed");
  return false;
}

bool ESP32_SMA_Inverter::takeReconnectRequest() {
  bool requested = reconnectRequested;
  reconnectRequested = false;
  return requested;
}

void ESP32_SMA_Inverter::beginPollBudget(uint32_t budgetMs) {
  replyTimeouts = 0;
  pollBudgetExpired = false;
  pollBudgetDeadline = millis() + budgetMs;
  pollBudgetActive = budgetMs != 0;
}

void ESP32_SMA_Inverter::endPollBudget() {
  pollBudgetActive = false;
}

bool ESP32_SMA_Inverter::disconnect() {
  discardBtRx.store(true, std::memory_order_release);
  bool bGotDisconnected = serialBT.disconnect();
  btConnected = false;
  clearReceiveQueue();
  return bGotDisconnected;
}

void ESP32_SMA_Inverter::clearReceiveQueue() {
  discardBtRx.store(true, std::memory_order_release);
  // Let an SPP callback already copying a chunk finish before moving the
  // consumer cursor. New callbacks see discardBtRx and return immediately.
  while (btRxCallbackBusy.load(std::memory_order_acquire)) delay(1);
  if (btRxCallbackActive.load(std::memory_order_acquire)) {
    const uint32_t head = btRxHead.load(std::memory_order_acquire);
    btRxTail.store(head, std::memory_order_release);
    btRxOverflow.store(false, std::memory_order_release);
    return;
  }
  // flush() drains transmitted data only. Limit work if the peer keeps sending.
  for (size_t count = 0; count < COMMBUFSIZE && serialBT.available(); ++count) serialBT.read();
}

void ESP32_SMA_Inverter::enqueueBluetoothData(const uint8_t *data, size_t length) {
  if (!data || discardBtRx.load(std::memory_order_acquire)) return;
  btRxCallbackBusy.store(true, std::memory_order_release);
  if (discardBtRx.load(std::memory_order_acquire) ||
      btRxOverflow.load(std::memory_order_acquire)) {
    btRxCallbackBusy.store(false, std::memory_order_release);
    return;
  }

  // BluetoothSerial invokes this callback serially from its SPP event task;
  // BTgetByte is the sole consumer. Publish a completed chunk with one release.
  const uint32_t head = btRxHead.load(std::memory_order_relaxed);
  const uint32_t tail = btRxTail.load(std::memory_order_acquire);
  const uint32_t used = head - tail;
  const size_t available = used < BT_RX_QUEUE_CAPACITY ? BT_RX_QUEUE_CAPACITY - used : 0;
  const size_t copied = length < available ? length : available;
  for (size_t i = 0; i < copied; ++i) {
    btRxQueue[(head + i) & (BT_RX_QUEUE_CAPACITY - 1)] = data[i];
  }
  btRxHead.store(head + copied, std::memory_order_release);
  if (copied != length) btRxOverflow.store(true, std::memory_order_release);
  btRxCallbackBusy.store(false, std::memory_order_release);
}

bool ESP32_SMA_Inverter::readBluetoothByte(uint8_t *value) {
  const uint32_t tail = btRxTail.load(std::memory_order_relaxed);
  const uint32_t head = btRxHead.load(std::memory_order_acquire);
  if (tail == head) return false;
  *value = btRxQueue[tail & (BT_RX_QUEUE_CAPACITY - 1)];
  btRxTail.store(tail + 1, std::memory_order_release);
  return true;
}

E_RC ESP32_SMA_Inverter::failReceive(E_RC error) {
  // A corrupt length leaves the stream boundary unknown. Start a new session.
  disconnect();
  pcktBufPos = 0;
  return error;
}

void ESP32_SMA_Inverter::serviceBackground() {
  if (!serviceCallback || servicing || (uint32_t)(millis() - lastServiceMillis) < 100UL) return;
  lastServiceMillis = millis();
  servicing = true;
  serviceCallback();
  servicing = false;
}

//serialBT.disconnect();

bool ESP32_SMA_Inverter::isValidSender(uint8_t expAddr[6], uint8_t isAddr[6]) {
  bool broadcast = true;
  for (int i = 0; i < 6; i++) {
    if (expAddr[i] != 0xFF) {
      broadcast = false;
      break;
    }
  }
  if (broadcast) return true;

  for (int i = 0; i < 6; i++)
    if (isAddr[i] != expAddr[i]) {
      logV("Shoud-Addr: %02X %02X %02X %02X %02X %02X\n   Is-Addr: %02X %02X %02X %02X %02X %02X\n",
        expAddr[5], expAddr[4], expAddr[3], expAddr[2], expAddr[1], expAddr[0],
         isAddr[5],  isAddr[4],  isAddr[3],  isAddr[2],  isAddr[1],  isAddr[0]);
        return false;
    }
  return true;
}

// ----------------------------------------------------------------------------------------------
//unsigned int readBtPacket(int index, unsigned int cmdcodetowait) {
E_RC ESP32_SMA_Inverter::getPacket(uint8_t expAddr[6], int wait4Command,
                                   const uint32_t *callerDeadline) {
  //extern BluetoothSerial serialBT;
  logV("getPacket cmd=0x%04x\n", wait4Command);
  //extern bool readTimeout;
  receiveStarted = millis();
  receivingPacket = true;
  struct ReceiveGuard { bool& active; ~ReceiveGuard() { active = false; } } guard{receivingPacket};
  int index = 0;
  bool hasL2pckt = false;
  bool escNext = false;
  E_RC rc = E_OK; 
  L1Hdr *pL1Hdr = (L1Hdr *)&btrdBuf[0];
  int retries = 10;
  do {
    // read L1Hdr
    uint16_t rdCnt=0;
    for (rdCnt=0;rdCnt<18;rdCnt++) {
      btrdBuf[rdCnt]= BTgetByte(callerDeadline);
      if (readTimeout)  break;
    }
    logD("L1 Rec=%d bytes pkL=0x%04x=%d Cmd=0x%04x\n",
        rdCnt, pL1Hdr->pkLength, pL1Hdr->pkLength, pL1Hdr->command);

    if (rdCnt != sizeof(L1Hdr)) {
      logV("L1<18=%d bytes", rdCnt);
      #if (DEBUG_SMA > 2)
      HexDump(btrdBuf, rdCnt, 10, 'R');
      #endif
      return failReceive(E_NODATA);
    }
    // Validate L1 header
    if (btrdBuf[0] != 0x7E || (btrdBuf[0] ^ btrdBuf[1] ^ btrdBuf[2]) != btrdBuf[3]) {
      logW("Wrong L1 CRC!!" );
      return failReceive(E_CHKSUM);
    }

    if (pL1Hdr->pkLength < sizeof(L1Hdr) || pL1Hdr->pkLength > sizeof(btrdBuf)) {
      logE("Invalid L1 packet length: %u", pL1Hdr->pkLength);
      return failReceive(E_OVERFLOW);
    }

    if (pL1Hdr->pkLength > sizeof(L1Hdr)) { // more bytes to read
      for (rdCnt=18; rdCnt<pL1Hdr->pkLength; rdCnt++) {
        btrdBuf[rdCnt]= BTgetByte(callerDeadline);
        if (readTimeout) break;
      }
      if (rdCnt != pL1Hdr->pkLength) {
        logW("Incomplete Bluetooth packet: %u of %u bytes", rdCnt, pL1Hdr->pkLength);
        return failReceive(E_NODATA);
      }
      logV("L2 Rec=%d bytes", rdCnt-18);
      #if (DEBUG_SMA > 2)
      HexDump(btrdBuf, rdCnt, 10, 'R');
      #endif

      //Check if data is coming from the right inverter
      if (isValidSender(expAddr, pL1Hdr->SourceAddr)) {
        rc = E_OK;

        const size_t payloadLength = rdCnt - sizeof(L1Hdr);
        if (!hasL2pckt && btrdBuf[18] == 0x7E &&
            (payloadLength < 5 || get_u32(btrdBuf + 19) == BTH_L2SIGNATURE)) {
          hasL2pckt = true;
        }

        if (hasL2pckt) {
          //Copy BTrdBuf to pcktBuf

          for (int i=sizeof(L1Hdr); i<pL1Hdr->pkLength; i++) {
            if (index >= MAX_PCKT_BUF_SIZE) {
              logE("pcktBuf overflow! (%d)\n", index);
              return failReceive(E_OVERFLOW);
            }
            pcktBuf[index] = btrdBuf[i];
            //Keep 1st byte raw unescaped 0x7E
            if (escNext == true) {
              pcktBuf[index] ^= 0x20;
              escNext = false;
              index++;
            } else {
              if (pcktBuf[index] == 0x7D)
                escNext = true; //Throw away the 0x7d byte
              else
                index++;
            }
          }
          pcktBufPos = index;
        } else {  // no L2pckt
          memcpy(pcktBuf, btrdBuf, rdCnt);
          pcktBufPos = rdCnt;
        }
      } else { // isValidSender()
          rc = E_RETRY;
      }
    } else {  // L1 only
    #if (DEBUG_SMA > 2)
      HexDump(btrdBuf, rdCnt, 10, 'R');
    #endif
      //Check if data is coming from the right inverter
      if (isValidSender(expAddr, pL1Hdr->SourceAddr)) {
          rc = E_OK;

          memcpy(pcktBuf, btrdBuf, rdCnt);
          pcktBufPos = rdCnt;
      } else { // isValidSender()
          rc = E_RETRY;
      }
    }
    // Valid continuation frames are bounded by the packet size and deadline,
    // not by the allowance for packets sent by a different inverter.
    if (rc == E_RETRY && retries-- <= 0) {
      logE("Packet retries exceeded");
      return failReceive(E_INVRESP);
    }
  } while (((pL1Hdr->command != wait4Command) || (rc == E_RETRY)) && (0xFF != wait4Command));

  if (hasL2pckt && (escNext || pcktBufPos < 5 ||
      get_u32(pcktBuf + 1) != BTH_L2SIGNATURE)) return failReceive(E_INVRESP);
  if ((rc == E_OK) ) {
  #if (DEBUG_SMA > 1)
    logD("<<<====Rd Content of pcktBuf =======>>>");
    HexDump(pcktBuf, pcktBufPos, 10, 'P');
    logD("==>>>");
  #endif
  }

  if (pcktBufPos > pcktBufMax) {
    pcktBufMax = pcktBufPos;
    logD("pcktBufMax is now %d bytes\n", pcktBufMax);
  }

  return rc;
}

// *************************************************

void ESP32_SMA_Inverter::writePacketHeader(uint8_t *buf, const uint16_t control, const uint8_t *destaddress) {
  //extern uint16_t fcsChecksum;


    pcktBufPos = 0;

    fcsChecksum = 0xFFFF;
    buf[pcktBufPos++] = 0x7E;
    buf[pcktBufPos++] = 0;  //placeholder for len1
    buf[pcktBufPos++] = 0;  //placeholder for len2
    buf[pcktBufPos++] = 0;  //placeholder for checksum
    int i;
    for(i = 0; i < 6; i++) buf[pcktBufPos++] = espBTAddress[i];
    for(i = 0; i < 6; i++) buf[pcktBufPos++] = destaddress[i];

    buf[pcktBufPos++] = (uint8_t)(control & 0xFF);
    buf[pcktBufPos++] = (uint8_t)(control >> 8);
}


bool ESP32_SMA_Inverter::isCrcValid(uint8_t lb, uint8_t hb)
{
  bool bRet = false;
  
    if (((lb == 0x7E) || (hb == 0x7E) || (lb == 0x7D) || (hb == 0x7D)))
      bRet = false;
    else
      bRet = true;

  logD("isCrcValid at pcktBufPos: %d", bRet);
  return bRet;
}


uint32_t ESP32_SMA_Inverter::getattribute(uint8_t *pcktbuf, size_t recordsize)
{
    const uint32_t unavailable = 0xFFFFFD;
    if (recordsize < 40) return unavailable;
    uint32_t tag=0, attribute=0, prevTag=unavailable;
    bool found = false;
    for (size_t idx = 8; idx + 4 <= recordsize; idx += 4)
    {      
        attribute = ((uint32_t)get_u32(pcktbuf + idx));
        tag = attribute & 0x00FFFFFF;
        if (tag == 0xFFFFFE) // count on prevTag to contain a value to succeed
            break;
        if ((attribute >> 24) == 1) //only take into account meaningfull values here 
            if (!found) { prevTag = tag; found = true; }
    }

    return prevTag;
}


// ***********************************************
E_RC ESP32_SMA_Inverter::getInverterDataCfl(uint32_t command, uint32_t first, uint32_t last) {
  struct QueryTransaction {
    InverterData& data;
    DisplayData& display;
    InverterData& savedData;
    DisplayData& savedDisplay;
    bool committed = false;

    QueryTransaction(InverterData& data, DisplayData& display,
                     InverterData& savedData, DisplayData& savedDisplay)
        : data(data), display(display), savedData(savedData), savedDisplay(savedDisplay) {
      savedData = data;
      savedDisplay = display;
    }
    ~QueryTransaction() {
      if (!committed) {
        data = savedData;
        display = savedDisplay;
      }
    }
    void commit() { committed = true; }
  } transaction(invData, dispData, cflSnapshot, cflDisplaySnapshot);

  const uint32_t queryStarted = millis();
  const uint32_t queryDeadline = queryStarted + queryTimeoutMs;
  if ((first >> 8) <= DcMsVol && (last >> 8) >= DcMsAmp) {
    for (size_t k = 0; k < 2; ++k) {
      invData.Udc[k] = invData.Idc[k] = INT32_MIN;
      dispData.Udc[k] = dispData.Idc[k] = NAN;
    }
  }
  if ((first >> 8) <= DcMsWatt && (last >> 8) >= DcMsWatt) {
    for (size_t k = 0; k < 2; ++k) { invData.Wdc[k] = INT32_MIN; dispData.Wdc[k] = NAN; }
  }
  do {
    pcktID++;
    writePacketHeader(pcktBuf, 0x01, sixff); //addr_unknown);
    //if (invData.SUSyID == SID_SB240)
    //writePacket(pcktBuf, 0x09, 0xE0, 0, invData.SUSyID, invData.Serial);
    //else
    writePacket(pcktBuf, 0x09, 0xA0, 0, invData.SUSyID, invData.Serial);
    write32(pcktBuf, command);
    write32(pcktBuf, first);
    write32(pcktBuf, last);
    writePacketTrailer(pcktBuf);
    writePacketLength(pcktBuf);

  } while (!isCrcValid(pcktBuf[pcktBufPos - 3], pcktBuf[pcktBufPos - 2]));

    if (!BTsendPacket(pcktBuf)) return E_NODATA;


    uint16_t pcktcount = 0;
    bool  validPcktID = false;
    do {
    do {
      if ((int32_t)(millis() - queryDeadline) >= 0) return E_NODATA;
      invData.status = getPacket(invData.BTAddress, 0x0001, &queryDeadline);
   
      if (invData.status != E_OK) return invData.status;
      if (pcktBufPos < 32) return E_INVRESP;
      if (validateChecksum()) {
        const uint16_t responseId = get_u16(pcktBuf + 27) & 0x7FFF;
        if (responseId != (pcktID & 0x7FFF) || get_u16(pcktBuf + 15) != invData.SUSyID ||
            get_u32(pcktBuf + 17) != invData.Serial) continue;
        if ((invData.status = (E_RC)get_u16(pcktBuf + 23)) != E_OK) {
          logD("Packet status: 0x%02X\n", invData.status);
          return invData.status;
        }
        // *** analyze received data ***
        pcktcount = get_u16(pcktBuf + 25);
        uint16_t rcvpcktID = get_u16(pcktBuf + 27) & 0x7FFF;
        if ((pcktID & 0x7FFF) == rcvpcktID) {
          if ((get_u16(pcktBuf + 15) == invData.SUSyID) 
            && (get_u32(pcktBuf + 17) == invData.Serial)) {
            validPcktID = true;
            value32 = 0;
            value64 = 0;
            if (pcktBufPos < 44 || pcktBuf[5] < 9) {
              logE("Invalid SMA data packet length");
              return E_INVRESP;
            }
            uint32_t firstLri = get_u32(pcktBuf + 33);
            uint32_t lastLri = get_u32(pcktBuf + 37);
            if (lastLri < firstLri) {
              logE("Invalid SMA record range");
              return E_INVRESP;
            }
            const uint64_t recordCount = uint64_t(lastLri) - firstLri + 1;
            const uint32_t dataBytes = 4U * (uint32_t(pcktBuf[5]) - 9U);
            if (dataBytes != uint32_t(pcktBufPos - 44) || recordCount > dataBytes / 16U ||
                dataBytes % recordCount != 0) return E_INVRESP;
            const uint16_t recordsize = dataBytes / recordCount;
            if (recordsize < 16 || recordsize % 4 != 0) return E_INVRESP;
            logD("pcktID=0x%04x recsize=%d BufPos=%d pcktCnt=%04x", 
                            rcvpcktID,   recordsize, pcktBufPos, pcktcount);
            for (uint16_t ii = 41; ii + recordsize <= pcktBufPos - 3; ii += recordsize) {
              uint8_t *recptr = pcktBuf + ii;
              uint32_t code = get_u32(recptr);
              //LriDef lri = (LriDef)(code & 0x00FFFF00);
              uint16_t lri = (code & 0x00FFFF00) >> 8;
              uint32_t cls = code & 0xFF;
              uint8_t dataType = code >> 24;
              time_t datetime = (time_t)get_u32(recptr + 4);
              logV("lri=0x%04x cls=0x%08X dataType=0x%02x",lri, cls, dataType);
       
              if ((lri == OperationHealth || lri == OperationGriSwStt) &&
                  (dataType != 8 || recordsize < 40)) return E_INVRESP;
              bool numericValid = false;
              bool energyValid = false;
              value32 = INT32_MIN;
              value64 = UINT64_MAX;
              if (recordsize == 16) {
                value64 = get_u64(recptr + 8);
                energyValid = value64 != UINT64_MAX && value64 != 0x8000000000000000ULL;
                logV("value64=%llu=0x%016llx", (unsigned long long)value64, (unsigned long long)value64);
       
                  //if (is_NaN(value64) || is_NaN((uint64_t)value64)) value64 = 0;
              } else if ((dataType != 16) && (dataType != 8) && recordsize >= 20) { // ((dataType != DT_STRING) && (dataType != DT_STATUS)) {
                value32 = get_u32(recptr + 16);
                // Several SMA models use datatype 0x00 for signed values.
                numericValid = value32 != INT32_MIN &&
                    (dataType == 0x40 || uint32_t(value32) != UINT32_MAX);
                logV("value32=%d=0x%08x",value32, value32);
              }
              switch (lri) {
              case GridMsTotW: //SPOT_PACTOT
                  //This function gives us the time when the inverter was switched off
                  invData.LastTime = datetime;
                  invData.Pac = value32;
                  dispData.Pac = numericValid ? (float)value32 : NAN;
                  //debug_watt("SPOT_PACTOT", value32, datetime);
                  printUnixTime(timeBuf, datetime);
                  logI("Pac %15.3f kW %x  GMT:%s \n", tokW(value32),value32, timeBuf);
                  break;
       
              case GridMsWphsA: //SPOT_PAC1
                  invData.Pmax = value32;
                  dispData.Pmax = numericValid ? tokW(value32) : NAN;
                  //debug_watt("SPOT_PAC1", value32, datetime);
                  logI("Pmax %14.2f kW \n", tokW(value32));
                  //printUnixTime(timeBuf, datetime);
                  break;
       
              case GridMsPhVphsA: //SPOT_UAC1
                  invData.Uac[0] = value32;
                  dispData.Uac[0] = numericValid ? toVolt(value32) : NAN;
                  //debug_volt("SPOT_UAC1", value32, datetime);
                  logI("UacA %15.2f V  \n", toVolt(value32));
                  //printUnixTime(timeBuf, datetime);
                  break;
              case GridMsPhVphsB: //SPOT_UAC2
                  invData.Uac[1] = value32;
                  dispData.Uac[1] = numericValid ? toVolt(value32) : NAN;
                  //debug_volt("SPOT_UAC1", value32, datetime);
                  logI("UacB %15.2f V  \n", toVolt(value32));
                  //printUnixTime(timeBuf, datetime);
                  break;     
                case GridMsPhVphsC: //SPOT_UAC2
                  invData.Uac[2] = value32;
                  dispData.Uac[2] = numericValid ? toVolt(value32) : NAN;
                  //debug_volt("SPOT_UAC1", value32, datetime);
                  logI("UacC %15.2f V  \n", toVolt(value32));
                  //printUnixTime(timeBuf, datetime);
                  break;       
              case GridMsAphsA_1: //SPOT_IAC1
              case GridMsAphsA:
                  invData.Iac[0] = value32;
                  dispData.Iac[0] = numericValid ? toAmp(value32) : NAN;
                  //debug_amp("SPOT_IAC1", value32, datetime);
                  logI("IacA %15.2f A  \n", toAmp(value32));
                  //printUnixTime(timeBuf, datetime);
                  break;
              case GridMsAphsB_1: //SPOT_IAC1
              case GridMsAphsB:
                  invData.Iac[1] = value32;
                  dispData.Iac[1] = numericValid ? toAmp(value32) : NAN;
                  //debug_amp("SPOT_IAC1", value32, datetime);
                  logI("IacB %15.2f A  \n", toAmp(value32));
                  //printUnixTime(timeBuf, datetime);
                  break;
              case GridMsAphsC_1: //SPOT_IAC1
              case GridMsAphsC:
                  invData.Iac[2] = value32;
                  dispData.Iac[2] = numericValid ? toAmp(value32) : NAN;
                  //debug_amp("SPOT_IAC1", value32, datetime);
                  logI("IacB %15.2f A  \n", toAmp(value32));
                  //printUnixTime(timeBuf, datetime);
                  break;
       
              case GridMsHz: //SPOT_FREQ
                  invData.Freq = value32;
                  dispData.Freq = numericValid ? toHz(value32) : NAN;
                  logI("Freq %14.2f Hz \n", toHz(value32));
                  //printUnixTime(timeBuf, datetime);
                  break;
       
              case DcMsWatt: //SPOT_PDC1 / SPOT_PDC2
                  if (cls >= 1 && cls <= 2) {
                    invData.Wdc[cls - 1] = value32;
                    dispData.Wdc[cls - 1] = numericValid ? tokW(value32) : NAN;
                  }
                  logI("PDC %15.2f kW \n", tokW(value32));
                  //printUnixTime(timeBuf, datetime);
                  break;
       
              case DcMsVol: //SPOT_UDC1 / SPOT_UDC2
                  logI("Udc %15.2f V (%d) \n", toVolt(value32),cls);
                  if (cls >= 1 && cls <= 2) {
                    invData.Udc[cls - 1] = value32;
                    dispData.Udc[cls - 1] = numericValid ? toVolt(value32) : NAN;
                  }
                  
                  //printUnixTime(timeBuf, datetime);
                  break;
       
              case DcMsAmp: //SPOT_IDC1 / SPOT_IDC2
                  logI("Idc %15.2f A (%d)\n", toAmp(value32),cls);
                  if (cls >= 1 && cls <= 2) {
                    invData.Idc[cls - 1] = value32;
                    dispData.Idc[cls - 1] = numericValid ? toAmp(value32) : NAN;
                  }

                  //printUnixTime(timeBuf, datetime);
                  /* if ((invData.Udc[0]!=0) && (invData.Idc[0] != 0))
                    invData.Eta = ((uint64_t)invData.Uac * (uint64_t)invData.Iac * 10000) /
                                    ((uint64_t)invData.Udc[0] * (uint64_t)invData.Idc[0] );
                  else invData.Eta = 0;
                  logI("Efficiency %8.2f %%\n", toPercent(invData.Eta)); */
                  break;
       
              case MeteringDyWhOut: //SPOT_ETODAY
                  //This function gives us the current inverter time
                  //invData.InverterDatetime = datetime;
                  invData.EToday = value64;
                  invData.ETodayValid = energyValid;
                  dispData.EToday = energyValid ? tokWh(value64) : NAN;
                  //debug_kwh("SPOT_ETODAY", value64, datetime);
                  logI("E-Today %11.3f kWh\n", tokWh(value64));
                  //printUnixTime(timeBuf, datetime);
                  break;
       
              case MeteringTotWhOut: //SPOT_ETOTAL
                  //In case SPOT_ETODAY missing, this function gives us inverter time (eg: SUNNY TRIPOWER 6.0)
                  //invData.InverterDatetime = datetime;
                  invData.ETotal = value64;
                  invData.ETotalValid = energyValid;
                  dispData.ETotal = energyValid ? tokWh(value64) : NAN;
                  //debug_kwh("SPOT_ETOTAL", value64, datetime);
                  logI("E-Total %11.3f kWh\n", tokWh(value64));
                  //printUnixTime(timeBuf, datetime);
                  break;
       
              case MeteringTotOpTms: //SPOT_OPERTM
                  invData.OperationTime = value64;
                  //debug_hour("SPOT_OPERTM", value64, datetime);
                  logI("OperTime  %7.3f h \n", toHour(value64));
                  //printUnixTime(timeBuf, datetime);
                  break;
       
              case MeteringTotFeedTms: //SPOT_FEEDTM
                  invData.FeedInTime = value64;
                  //debug_hour("SPOT_FEEDTM", value64, datetime);
                  logI("FeedTime  %7.3f h  \n", toHour(value64));
                  //printUnixTime(timeBuf, datetime);
                  break;
       
              case CoolsysTmpNom:
                  invData.InvTemp = value32;
                  dispData.InvTemp = numericValid ? toTemp(value32) : NAN;
                  logI("Temp.     %7.3f C \n", toTemp(value32));
                  break;
              case OperationHealth:
                  value32 = getattribute(recptr, recordsize);
                  invData.DevStatus = value32;
                  logI("Device Status:    %d  \n", value32);
                  break;
              case OperationGriSwStt:
                  value32 = getattribute(recptr, recordsize);
                  invData.GridRelay = value32;
                  logI("Grid Relay:    %d  \n", value32);
                  break;
              case MeteringGridMsTotWOut:
                  //invData.MeteringGridMsTotWOut = value32;
                  break;
              case MeteringGridMsTotWIn:
                  //invData.MeteringGridMsTotWIn = value32;
                  break;
              default:

                logI("Caught: %x %d\n",lri,value32);
              }
            } //for
          } else {
            logW("*** Wrong SUSyID=%04x=%04x Serial=%08x=%08x", 
                 get_u16(pcktBuf + 15), invData.SUSyID, get_u32(pcktBuf + 17),invData.Serial);
          }
        } else {  // wrong PacketID
          logW("PacketID mismatch: exp=0x%04X is=0x%04X\n", pcktID, rcvpcktID);
          validPcktID = false;
          pcktcount = 0;
        }
      } else { // invalid Checksum
        invData.status = E_CHKSUM;
        return invData.status;
      }
   } while (pcktcount > 0);
   } while (!validPcktID);
   const E_RC result = invData.status;
   if (result == E_OK) transaction.commit();
   return result;
}
// ***********************************************
E_RC ESP32_SMA_Inverter::getInverterData(enum getInverterDataType type) {
  E_RC rc = E_OK;
  uint32_t command;
  uint32_t first;
  uint32_t last;

  switch (type) {
  case EnergyProduction:
      logD("*** EnergyProduction ***");
      // SPOT_ETODAY, SPOT_ETOTAL
      command = 0x54000200;
      first = 0x00260100;
      last = 0x002622FF;
      break;

  case SpotDCPower:
      logD("*** SpotDCPower ***");
      // SPOT_PDC1, SPOT_PDC2
      command = 0x53800200;
      first = 0x00251E00;
      last = 0x00251EFF;
      break;

  case SpotDCVoltage:
      logD("*** SpotDCVoltage ***");
      // SPOT_UDC1, SPOT_UDC2, SPOT_IDC1, SPOT_IDC2
      command = 0x53800200;
      first = 0x00451F00;
      last = 0x004521FF;
      break;

  case SpotACPower:
      logD("*** SpotACPower ***");
      // SPOT_PAC1, SPOT_PAC2, SPOT_PAC3
      command = 0x51000200;
      first = 0x00464000;
      last = 0x004642FF;
      break;

  case SpotACVoltage:
      logD("*** SpotACVoltage ***");
      // SPOT_UAC1, SPOT_UAC2, SPOT_UAC3, SPOT_IAC1, SPOT_IAC2, SPOT_IAC3
      command = 0x51000200;
      first = 0x00464800;
      last = 0x004655FF;
      break;

  case SpotGridFrequency:
      logD("*** SpotGridFrequency ***");
      // SPOT_FREQ
      command = 0x51000200;
      first = 0x00465700;
      last = 0x004657FF;
      break;

  case SpotACTotalPower:
      logD("*** SpotACTotalPower ***");
      // SPOT_PACTOT
      command = 0x51000200;
      first = 0x00263F00;
      last = 0x00263FFF;
      break;

  case TypeLabel:
      logD("*** TypeLabel ***");
      // INV_NAME, INV_TYPE, INV_CLASS
      command = 0x58000200;
      first = 0x00821E00;
      last = 0x008220FF;
      break;

  case SoftwareVersion:
      logD("*** SoftwareVersion ***");
      // INV_SWVERSION
      command = 0x58000200;
      first = 0x00823400;
      last = 0x008234FF;
      break;

  case DeviceStatus:
      logD("*** DeviceStatus ***");
      // INV_STATUS
      command = 0x51800200;
      first = 0x00214800;
      last = 0x002148FF;
      break;

  case GridRelayStatus:
      logD("*** GridRelayStatus ***");
      // INV_GRIDRELAY
      command = 0x51800200;
      first = 0x00416400;
      last = 0x004164FF;
      break;

  case OperationTime:
      logD("*** OperationTime ***");
      // SPOT_OPERTM, SPOT_FEEDTM
      command = 0x54000200;
      first = 0x00462E00;
      last = 0x00462FFF;
      break;

  case InverterTemp:
      logD("*** InverterTemp ***");
      command = 0x52000200;
      first = 0x00237700;
      last = 0x002377FF;
      break;

  case MeteringGridMsTotW:
      logD("*** MeteringGridMsTotW ***");
      command = 0x51000200;
      first = 0x00463600;
      last = 0x004637FF;
      break;

  default:
      logW("Invalid getInverterDataType!!");
      return E_BADARG;
  };

  // Request data from inverter
  for (uint8_t retries=1;; retries++) {
    rc = getInverterDataCfl(command, first, last);
    if (rc == E_LRINOTAVAIL) return rc;
    if (rc != E_OK) {
      if (retries>1) {
         return rc;
      }
      logI("Retrying.%d",retries);
    } else {
      break;
    } 
  }

  return rc;
}

//-------------------------------------------------------------------------
bool ESP32_SMA_Inverter::getBT_SignalStrength() {
  dispData.BTSigStrength = NAN;
  logI("*** SignalStrength ***");
  writePacketHeader(pcktBuf, 0x03, invData.BTAddress);
  writeByte(pcktBuf,0x05);
  writeByte(pcktBuf,0x00);
  writePacketLength(pcktBuf);
  if (!BTsendPacket(pcktBuf)) return false;

  if (getPacket(invData.BTAddress, 4) != E_OK || pcktBufPos <= 22) return false;
  dispData.BTSigStrength = ((float)btrdBuf[22] * 100.0f / 255.0f);
  logI("BT-Signal %9.1f %%", dispData.BTSigStrength );
  return true;
}

//-------------------------------------------------------------------------
E_RC ESP32_SMA_Inverter::initialiseSMAConnection() {
  //extern uint8_t sixff[6];
  logI(" -> Initialize");
  const uint32_t operationDeadline = millis() + replyTimeoutMs;
  E_RC rc = getPacket(invData.BTAddress, 2, &operationDeadline); // 1. Receive
  if (rc != E_OK || pcktBufPos <= 22) return (rc == E_OK) ? E_INVRESP : rc;
  invData.NetID = pcktBuf[22];
  logI("SMA netID=%02X\n", invData.NetID);
  writePacketHeader(pcktBuf, 0x02, invData.BTAddress);
  write32(pcktBuf, 0x00700400);
  writeByte(pcktBuf, invData.NetID);
  write32(pcktBuf, 0);
  write32(pcktBuf, 1);
  writePacketLength(pcktBuf);

  if (!BTsendPacket(pcktBuf)) return E_NODATA; // 1. Reply
  rc = getPacket(invData.BTAddress, 5, &operationDeadline); // 2. Receive
  if (rc != E_OK || pcktBufPos < 32) return (rc == E_OK) ? E_INVRESP : rc;

  // Extract ESP32 BT address
  memcpy(espBTAddress, pcktBuf+26,6); 
  logW("ESP32 BT address: %02X:%02X:%02X:%02X:%02X:%02X\n",
             espBTAddress[5], espBTAddress[4], espBTAddress[3],
             espBTAddress[2], espBTAddress[1], espBTAddress[0]);

  do {
  pcktID++;
  writePacketHeader(pcktBuf, 0x01, sixff); //addr_unknown);
  writePacket(pcktBuf, 0x09, 0xA0, 0, 0xFFFF, 0xFFFFFFFF); // anySUSyID, anySerial);
  write32(pcktBuf, 0x00000200);
  write32(pcktBuf, 0);
  write32(pcktBuf, 0);
  writePacketTrailer(pcktBuf);
  writePacketLength(pcktBuf);

  } while (!isCrcValid(pcktBuf[pcktBufPos - 3], pcktBuf[pcktBufPos - 2]));

  if (!BTsendPacket(pcktBuf)) return E_NODATA; // 2. Reply
  do {
    if ((int32_t)(millis() - operationDeadline) >= 0) return E_INVRESP;
    rc = getPacket(invData.BTAddress, 1, &operationDeadline); // 3. Receive
    if (rc != E_OK) return rc;
    if (!validateChecksum()) return E_CHKSUM;
    if (pcktBufPos < 36) return E_INVRESP;
    if ((get_u16(pcktBuf + 27) & 0x7FFF) != (pcktID & 0x7FFF) ||
        (get_u32(pcktBuf + 29) & 0xFFFFFF00UL) != 0x00000200UL) continue;
    if (get_u16(pcktBuf + 23) != 0 || pcktBufPos < 64) return E_INVRESP;
    // The serial occupies bytes 57-60; bytes 61-63 must be the trailer.
    const uint32_t serial = get_u32(pcktBuf + 57);
    if (serial == 0 || serial == UINT32_MAX) return E_INVRESP;
    if (serial != invData.Serial) invData.SUSyID = 0x007D;
    invData.Serial = serial;
    logW("Serial Nr: %lu\n", (unsigned long)invData.Serial);
    return E_OK;
  } while ((int32_t)(millis() - operationDeadline) < 0);
  return E_INVRESP;
}

// log off SMA Inverter - adapted from SBFspot Open Source Project (SBFspot.cpp) by mrtoy-me
void ESP32_SMA_Inverter::logoffSMAInverter()
{
  //extern uint8_t sixff[6];
  do {
    pcktID++;
    writePacketHeader(pcktBuf, 0x01, sixff);
    writePacket(pcktBuf, 0x08, 0xA0, 0x0300, 0xFFFF, 0xFFFFFFFF);
    write32(pcktBuf, 0xFFFD010E);
    write32(pcktBuf, 0xFFFFFFFF);
    writePacketTrailer(pcktBuf);
    writePacketLength(pcktBuf);
  } while (!isCrcValid(pcktBuf[pcktBufPos - 3], pcktBuf[pcktBufPos - 2]));

  if (!BTsendPacket(pcktBuf)) logW("Could not transmit inverter logoff");
  return;
}

// Read the SMA plant clock. This is the Bluetooth time query used by SBFspot.
E_RC ESP32_SMA_Inverter::readPlantTime(int32_t *currentTime, int32_t *lastTimeSet,
                                       int32_t *utcOffsetSeconds, uint32_t *setCount)
{
  do {
    pcktID++;
    writePacketHeader(pcktBuf, 0x01, invData.BTAddress);
    writePacket(pcktBuf, 0x10, 0xA0, 0, invData.SUSyID, invData.Serial);
    write32(pcktBuf, 0xF000020A);
    write32(pcktBuf, 0x00236D00);
    write32(pcktBuf, 0x00236D00);
    write32(pcktBuf, 0x00236D00);
    write32(pcktBuf, 0);
    write32(pcktBuf, 0);
    write32(pcktBuf, 0);
    write32(pcktBuf, 0);
    write32(pcktBuf, 1);
    write32(pcktBuf, 1);
    writePacketTrailer(pcktBuf);
    writePacketLength(pcktBuf);
  } while (!isCrcValid(pcktBuf[pcktBufPos - 3], pcktBuf[pcktBufPos - 2]));

  if (!BTsendPacket(pcktBuf)) return E_NODATA;
  const uint32_t started = millis();
  const uint32_t operationDeadline = started + replyTimeoutMs;
  bool matched = false;
  for (unsigned attempt = 0; attempt < 4 && (int32_t)(millis() - operationDeadline) < 0; ++attempt) {
    E_RC rc = getPacket(invData.BTAddress, 1, &operationDeadline);
    if (rc != E_OK) return rc;
    if (pcktBufPos < 68 || !validateChecksum()) continue;
    // Some SMA firmware does not echo clock packet IDs. Fail closed in that case:
    // a mismatched reply cannot safely supply the counter for a clock write.
    if ((get_u16(pcktBuf + 27) & 0x7FFF) != (pcktID & 0x7FFF) ||
        get_u16(pcktBuf + 15) != invData.SUSyID || get_u32(pcktBuf + 17) != invData.Serial ||
        get_u16(pcktBuf + 23) != 0 ||
        (get_u32(pcktBuf + 29) & 0xFFFFFF00UL) != 0xF0000200UL ||
        get_u32(pcktBuf + 41) != 0x00236D00UL) continue;
    matched = true;
    break;
  }
  if (!matched) return E_INVRESP;

  *currentTime = (int32_t)get_u32(pcktBuf + 45);
  *lastTimeSet = (int32_t)get_u32(pcktBuf + 49);
  *utcOffsetSeconds = (int32_t)(get_u32(pcktBuf + 57) & 0xFFFFFFFEUL);
  *setCount = get_u32(pcktBuf + 61);
  return E_OK;
}

E_RC ESP32_SMA_Inverter::syncPlantTime(int32_t utcOffsetSeconds,
                                       int32_t *beforeTime, int32_t *afterTime,
                                       const uint32_t *requestDeadline)
{
  if (!btConnected || beforeTime == nullptr || afterTime == nullptr) return E_BADARG;
  const uint8_t emptyAddress[6] = {};
  if (invData.SUSyID == 0 || invData.SUSyID == UINT16_MAX ||
      invData.Serial == 0 || invData.Serial == UINT32_MAX ||
      memcmp(invData.BTAddress, sixff, sizeof(sixff)) == 0 ||
      memcmp(invData.BTAddress, emptyAddress, sizeof(emptyAddress)) == 0) {
    logW("Refusing clock sync: inverter identity is not a single known device");
    return E_BADARG;
  }
  auto expired = [&]() {
    return requestDeadline && (int32_t)(millis() - *requestDeadline) >= 0;
  };
  if (expired()) return E_EXPIRED;

  time_t hostNow = time(nullptr);
  if (hostNow < 1700000000 || hostNow > INT32_MAX) {
    logW("Refusing clock sync: host time is not plausible");
    return E_BADARG;
  }

  int32_t lastTimeSet = 0;
  int32_t oldOffset = 0;
  uint32_t setCount = 0;
  E_RC rc = readPlantTime(beforeTime, &lastTimeSet, &oldOffset, &setCount);
  if (rc != E_OK) {
    logW("Unable to read inverter clock before update (%d)", rc);
    return rc;
  }

  hostNow = time(nullptr);
  if (hostNow < 1700000000 || hostNow > INT32_MAX) return E_BADARG;
  do {
    if (expired()) return E_EXPIRED;
    pcktID++;
    writePacketHeader(pcktBuf, 0x01, invData.BTAddress);
    writePacket(pcktBuf, 0x10, 0xA0, 0, invData.SUSyID, invData.Serial);
    write32(pcktBuf, 0xF000020A);
    write32(pcktBuf, 0x00236D00);
    write32(pcktBuf, 0x00236D00);
    write32(pcktBuf, 0x00236D00);
    write32(pcktBuf, (uint32_t)hostNow);
    write32(pcktBuf, (uint32_t)hostNow);
    write32(pcktBuf, (uint32_t)hostNow);
    // The low bit is SMA's daylight-saving flag. The configured offset is
    // already the current total UTC offset, so leave that flag clear.
    write32(pcktBuf, (uint32_t)(utcOffsetSeconds & ~1));
    write32(pcktBuf, setCount + 1);
    write32(pcktBuf, 1);
    writePacketTrailer(pcktBuf);
    writePacketLength(pcktBuf);
  } while (!isCrcValid(pcktBuf[pcktBufPos - 3], pcktBuf[pcktBufPos - 2]));

  // Connection, login and the initial clock read can outlast the authorization.
  if (expired()) return E_EXPIRED;
  if (!BTsendPacket(pcktBuf)) return E_NODATA;
  delay(500);

  int32_t verifiedLastSet = 0;
  int32_t verifiedOffset = 0;
  uint32_t verifiedSetCount = 0;
  rc = readPlantTime(afterTime, &verifiedLastSet, &verifiedOffset, &verifiedSetCount);
  if (rc != E_OK) return rc;

  int64_t difference = (int64_t)*afterTime - (int64_t)time(nullptr);
  if (difference < 0) difference = -difference;
  int64_t setDifference = (int64_t)*afterTime - (int64_t)verifiedLastSet;
  if (setDifference < 0) setDifference = -setDifference;
  const int32_t requestedOffset = utcOffsetSeconds & ~1;
  if (difference > 10 || setDifference > 10 ||
      verifiedOffset != requestedOffset || verifiedSetCount != setCount + 1) {
    logW("Inverter clock verification failed (host difference=%lld, last-set difference=%lld, offset=%ld expected=%ld, set count=%lu expected=%lu)",
         (long long)difference, (long long)setDifference, (long)verifiedOffset, (long)requestedOffset,
         (unsigned long)verifiedSetCount, (unsigned long)(setCount + 1));
    return E_INVRESP;
  }

  logI("Inverter clock verified; old=%ld new=%ld UTC offset=%ld",
       (long)*beforeTime, (long)*afterTime, (long)verifiedOffset);
  return E_OK;
}

// **** Logon SMA **********
E_RC ESP32_SMA_Inverter::logonSMAInverter(const char *password, const uint8_t user) {
  //extern uint8_t sixff[6];
  #define MAX_PWLENGTH 12
  uint8_t pw[MAX_PWLENGTH];
  E_RC rc = E_OK;

  // Encode password
  uint8_t encChar = (user == USERGROUP)? 0x88:0xBB;
  uint8_t idx;
  for (idx = 0; (password[idx] != 0) && (idx < sizeof(pw)); idx++)
    pw[idx] = password[idx] + encChar;
  for (; idx < MAX_PWLENGTH; idx++) pw[idx] = encChar;

    time_t now;
    now = time(NULL);
    do {
    pcktID++;
    writePacketHeader(pcktBuf, 0x01, sixff);
    writePacket(pcktBuf, 0x0E, 0xA0, 0x0100, 0xFFFF, 0xFFFFFFFF); // anySUSyID, anySerial);
    write32(pcktBuf, 0xFFFD040C);
    write32(pcktBuf, user); //userGroup);    // User / Installer
    write32(pcktBuf, 0x00000384); // Timeout = 900sec ?
    write32(pcktBuf, now);
    write32(pcktBuf, 0);
    writeArray(pcktBuf, pw, sizeof(pw));
    writePacketTrailer(pcktBuf);
    writePacketLength(pcktBuf);

    } while (!isCrcValid(pcktBuf[pcktBufPos - 3], pcktBuf[pcktBufPos - 2]));

    if (!BTsendPacket(pcktBuf)) return E_NODATA;

    const uint32_t receiveStarted = millis();
    const uint32_t operationDeadline = receiveStarted + replyTimeoutMs;
    constexpr unsigned maxLoginReplies = 10;
    for (unsigned attempt = 0; attempt < maxLoginReplies &&
         (int32_t)(millis() - operationDeadline) < 0; ++attempt) {
      if ((rc = getPacket(invData.BTAddress, 1, &operationDeadline)) != E_OK) return rc;
      if (pcktBufPos < 48) return E_INVRESP;
      if (!validateChecksum()) return E_CHKSUM;

      const uint16_t responseId = get_u16(pcktBuf + 27) & 0x7FFF;
      const uint16_t requestId = pcktID & 0x7FFF;
      const uint32_t responseCommand = get_u32(pcktBuf + 29);
      const uint32_t responseTime = get_u32(pcktBuf + 41);
      if (responseId != requestId || responseCommand != 0xFFFD040D ||
          responseTime != (uint32_t)now) {
        logD("Ignoring unrelated SMA login response (id=0x%04X expected=0x%04X command=0x%08lX time=%lu)",
             responseId, requestId, (unsigned long)responseCommand, (unsigned long)responseTime);
        continue;
      }

      const uint16_t responseSUSyID = get_u16(pcktBuf + 15);
      const uint32_t responseSerial = get_u32(pcktBuf + 17);
      const bool hasKnownSUSyID = invData.SUSyID != 0 && invData.SUSyID != UINT16_MAX &&
                                  invData.SUSyID != 0x007D;
      const bool hasKnownSerial = invData.Serial != 0 && invData.Serial != UINT32_MAX;
      if (responseSUSyID == 0 || responseSUSyID == UINT16_MAX ||
          responseSerial == 0 || responseSerial == UINT32_MAX ||
          (hasKnownSUSyID && responseSUSyID != invData.SUSyID) ||
          (hasKnownSerial && responseSerial != invData.Serial)) {
        logD("Ignoring SMA login response for another inverter (SUSyID=0x%04X serial=%lu)",
             responseSUSyID, (unsigned long)responseSerial);
        continue;
      }

      invData.SUSyID = responseSUSyID;
      invData.Serial = responseSerial;
      logV("Set:->SUSyID=0x%02X ->Serial=0x%02X ", invData.SUSyID, invData.Serial);
      const uint16_t retcode = get_u16(pcktBuf + 23);
      switch (retcode) {
        case 0: rc = E_OK; break;
        case 0x0100: rc = E_INVPASSW; break;
        default:
          logW("SMA login rejected with status 0x%04X", retcode);
          rc = E_INVRESP;
          break;
      }
      return rc;
    }

    logW("No matching SMA login response received within the bounded reply window");
    return E_INVRESP;
}

/* 
// ******* Archive Day Data **********
E_RC ArchiveDayData(time_t startTime) {
  DEBUG2_PRINT("*** ArchiveDayData ***");
  printUnixTime(timeBuf, startTime); DEBUG2_PRINTF("\nStartTime0 GMT:%s", timeBuf);
  // set time to begin of day
  uint8_t minutes = (startTime/60) % 60;
  uint8_t hours = (startTime/(60*60)) % 24;
  startTime -= minutes*60 + hours*60*60;
  printUnixTime(timeBuf, startTime); DEBUG2_PRINTF("\nStartTime2 GMT:%s", timeBuf);

  E_RC rc = E_OK;

  for (unsigned int i = 0; i<ARCH_DAY_SIZE; i++) {
     invData.dayWh[i] = 0;
  }
  invData.hasDayData = false;

  int packetcount = 0;
  bool validPcktID = false;

  E_RC hasData = E_ARCHNODATA;
  pcktID++;
  writePacketHeader(pcktBuf, 0x01, invData.BTAddress);
  writePacket(pcktBuf, 0x09, 0xE0, 0, invData.SUSyID, invData.Serial);
  write32(pcktBuf, 0x70000200);
  write32(pcktBuf, startTime - 300);
  write32(pcktBuf, startTime + 86100);
  writePacketTrailer(pcktBuf);
  writePacketLength(pcktBuf);

  BTsendPacket(pcktBuf);

  do {
    totalWh = 0;
    totalWh_prev = 0;
    dateTime = 0;

    do {
      rc = getPacket(invData.BTAddress, 1);

      if (rc != E_OK) {
         DEBUG3_PRINTF("\ngetPacket error=%d", rc);
         return rc;
      }
      // packetcount=nr of packets left on multi packet transfer n..0
      packetcount = pcktBuf[25];
      DEBUG2_PRINTF("packetcount=%d\n", packetcount);

      //TODO: Move checksum validation to getPacket
      if (!validateChecksum())
        return E_CHKSUM;
      else {
        unsigned short rcvpcktID = get_u16(pcktBuf + 27) & 0x7FFF;
        if (validPcktID || (pcktID == rcvpcktID)) {
          validPcktID = true;
          for (int x = 41; x < (pcktBufPos - 3); x += 12) {
            dateTime = (time_t)get_u32(pcktBuf + x);
            uint16_t idx =((dateTime/3600)%24 * 12)+((dateTime/60)%60/5); //h*12+min/5

            totalWh = get_u64(pcktBuf + x + 4);
            if ((totalWh > 0) && (!invData.hasDayData)) { 
              invData.DayStartTime = dateTime;
              invData.hasDayData = true;
              hasData = E_OK; 
              printUnixTime(timeBuf, dateTime); 
              DEBUG1_PRINTF("\nArchiveDayData %s", timeBuf);
            }
            if (idx < ARCH_DAY_SIZE) {
              invData.dayWh[idx] = totalWh;
              value64 = (totalWh - totalWh_prev) * 60 / 5; // assume 5 min. interval
              DEBUG3_PRINTF("[%03u] %6llu Wh %6llu W\n", idx, totalWh, value64);
            }
            totalWh_prev = totalWh;
          } //for
        } else {
            DEBUG1_PRINTF("Packet ID mismatch. Exp. %d, rec. %d\n", pcktID, rcvpcktID);
            validPcktID = true;
            packetcount = 0;
        }
      }
    } while (packetcount > 0);
  } while (!validPcktID);

  /* print values
  time_t startT = invData.DayStartTime;
  printUnixTime(timeBuf, startT);
  DEBUG2_PRINTF("Day History: %s\n", timeBuf);
  totalWh_prev = 0;

  for (uint16_t i = 0; i<ARCH_DAY_SIZE; i++) {
    totalWh = invData.dayWh[i];
    value32=0;
    if ((totalWh>0) && (totalWh_prev>0)) {
      value32 = (uint32_t)((totalWh - totalWh_prev)*60/5); 
    }
    if (totalWh>0) {
      printUnixTime(timeBuf, startT+3600+i*60*5); // GMT+1 + 5 min. interval
      DEBUG2_PRINTF("[%03d] %11.3f kWh  %7.3f kW %s\n", i, tokWh(totalWh), tokW(value32), timeBuf);
    }
    totalWh_prev = totalWh;
  }
  
  return hasData;
}
*/
// ******* read SMA current data **********
E_RC ESP32_SMA_Inverter::ReadCurrentData() {
  if (!btConnected) {
    logW("Bluetooth offline!");
    return E_NODATA;
  }
  // Each successful scan must describe this scan, including omitted readings.
  invData.Pac = invData.Freq = invData.InvTemp = INT32_MIN;
  dispData.Pac = dispData.Freq = dispData.InvTemp = NAN;
  invData.ETotal = invData.EToday = UINT64_MAX;
  invData.ETotalValid = invData.ETodayValid = false;
  dispData.ETotal = dispData.EToday = NAN;
  invData.DevStatus = invData.GridRelay = 0xFFFFFD;
  for (size_t k = 0; k < 3; ++k) {
    invData.Uac[k] = invData.Iac[k] = INT32_MIN;
    dispData.Uac[k] = dispData.Iac[k] = NAN;
  }
  if ((getInverterData(SpotACTotalPower)) != E_OK)  {
    logW("SpotACTotalPower error!"); // Pac
    return E_NODATA;
  }
  if ((getInverterData(SpotDCVoltage)) != E_OK)     {
    logW("getSpotDCVoltage error!"); // Udc + Idc
    return E_NODATA;
  }
  if ((getInverterData(SpotACVoltage)) != E_OK)     {
    logW("getSpotACVoltage error!"); // Uac + Iac
    return E_NODATA;
  }
  if ((getInverterData(EnergyProduction)) != E_OK)  {
    logW("EnergyProduction error!"); // E-Total + E-Today
    return E_NODATA;
  }
  if ((getInverterData(SpotGridFrequency)) != E_OK) {
    logW("SpotGridFrequency error!");
    return E_NODATA;
  }
  invData.InvTemp = INT32_MIN;
  dispData.InvTemp = NAN;
  const E_RC temperatureStatus = getInverterData(InverterTemp);
  if (temperatureStatus != E_OK && temperatureStatus != E_LRINOTAVAIL) {
    logW("InverterTemp error!");
    return E_NODATA;
  }
  if ((getInverterData(DeviceStatus)) != E_OK) {
    logW("Device Status error!");
    return E_NODATA;
  }
  if ((getInverterData(GridRelayStatus)) != E_OK) {
    logW("Grid Relay Status error!");
    return E_NODATA;
  }

//case 5: if ((getInverterData(SpotDCPower)) != E_OK)   DEBUG1_PRINTLN("getSpotDCPower error!"); //pcktBuf[23]=15 error!
//case 6: if ((getInverterData(SpotACPower)) != E_OK)   DEBUG1_PRINTLN("SpotACPower error!"   ); //pcktBuf[23]=15 error!
//case 7: if ((getInverterData(InverterTemp)) != E_OK)  DEBUG1_PRINTLN("InverterTemp error!"  ); //pcktBuf[23]=15 error!
//case 8: if ((getInverterData(OperationTime)) != E_OK) DEBUG1_PRINTLN("OperationTime error!" ); // OperTime + OperTime
  return E_OK;
} 



// **** receive BT byte *******
uint8_t ESP32_SMA_Inverter::BTgetByte(const uint32_t *callerDeadline) {
  readTimeout = false;
  //Returns a single byte from the bluetooth stream (with error timeout/reset)
  const uint32_t started = millis(); // elapsed subtraction survives millis() rollover
  uint8_t  rec = 0;  

  while (true) {
    const uint32_t now = millis();
    if ((uint32_t)(now - started) >= replyTimeoutMs ||
        (callerDeadline && (int32_t)(now - *callerDeadline) >= 0) ||
        (receivingPacket && (uint32_t)(now - receiveStarted) >= replyTimeoutMs)) {
      DEBUG2_PRINTLN("BTgetByte Timeout");
      readTimeout = true;
      ++replyTimeouts;
      break;
    }
    if (pollBudgetActive && (int32_t)(now - pollBudgetDeadline) >= 0) {
      if (!pollBudgetExpired) logW("Inverter poll budget exhausted; abandoning this poll");
      pollBudgetExpired = true;
      readTimeout = true;
      break;
    }
    if (btRxCallbackActive.load(std::memory_order_acquire)) {
      if (btRxOverflow.load(std::memory_order_acquire)) {
        logW("Bluetooth receive buffer overflow during network service");
        readTimeout = true;
        break;
      }
      if (readBluetoothByte(&rec)) break;
    } else if (serialBT.available()) break;
    serviceBackground();
    delay(1); // Let the Bluetooth stack and watchdog run while waiting.
  }

  if (!readTimeout && !btRxCallbackActive.load(std::memory_order_acquire)) rec = serialBT.read();
  return rec;
}
// **** transmit BT buffer ****
bool ESP32_SMA_Inverter::BTsendPacket(uint8_t *btbuffer) {
  // Queue one complete frame. Per-byte writes can drop bytes independently.
  const size_t length = pcktBufPos;
  if (serialBT.write(btbuffer, length) != length) {
    logW("Incomplete Bluetooth packet write");
    disconnect();
    return false;
  }
  //DEBUG2_PRINTLN();
  for(int i=0;i<pcktBufPos;i++) {
    #ifdef DebugBT
    if (i==0) DEBUG2_PRINT("*** sStart=");
    if (i==1) DEBUG2_PRINT(" len=");
    if (i==3) {
      if ((0x7e ^ *(btbuffer+1) ^ *(btbuffer+2)) == *(btbuffer+3)) 
        DEBUG2_PRINT(" checkOK=");
      else  DEBUG2_PRINT(" checkFalse!!=");
    }
    if (i==4)  DEBUG2_PRINTF("\nfrMac[%d]=",i);
    if (i==10) DEBUG2_PRINTF(" toMac[%d]=",i);
    if (i==16) DEBUG2_PRINTF(" Type[%d]=",i);
    if ((i==18)||(i==18+16)||(i==18+32)||(i==18+48)) DEBUG2_PRINTF("\nsDat[%d]=",i);
    DEBUG2_PRINTF("%02X,", *(btbuffer+i)); // Print out what we are sending, in hex, for inspection.
    #endif
  }
  HexDump(btbuffer, pcktBufPos, 10, 'T');
  return true;
}


//-------------------------------------------------------------------------
// ********************************************************
void ESP32_SMA_Inverter::writeByte(uint8_t *btbuffer, uint8_t v) {
  //Keep a rolling checksum over the payload
  fcsChecksum = (fcsChecksum >> 8) ^ fcstab[(fcsChecksum ^ v) & 0xff];
  if (v == 0x7d || v == 0x7e || v == 0x11 || v == 0x12 || v == 0x13) {
    btbuffer[pcktBufPos++] = 0x7d;
    btbuffer[pcktBufPos++] = v ^ 0x20;
  } else {
    btbuffer[pcktBufPos++] = v;
  }
}
// ********************************************************
void ESP32_SMA_Inverter::write32(uint8_t *btbuffer, uint32_t v) {
  writeByte(btbuffer,(uint8_t)((v >> 0) & 0xFF));
  writeByte(btbuffer,(uint8_t)((v >> 8) & 0xFF));
  writeByte(btbuffer,(uint8_t)((v >> 16) & 0xFF));
  writeByte(btbuffer,(uint8_t)((v >> 24) & 0xFF));
}
// ********************************************************
void ESP32_SMA_Inverter::write16(uint8_t *btbuffer, uint16_t v) {
  writeByte(btbuffer,(uint8_t)((v >> 0) & 0xFF));
  writeByte(btbuffer,(uint8_t)((v >> 8) & 0xFF));
}
// ********************************************************
void ESP32_SMA_Inverter::writeArray(uint8_t *btbuffer, const uint8_t bytes[], int loopcount) {
    for (int i = 0; i < loopcount; i++) writeByte(btbuffer, bytes[i]);
}
// ********************************************************
//  writePacket(pcktBuf, 0x0E, 0xA0, 0x0100, 0xFFFF, 0xFFFFFFFF); // anySUSyID, anySerial);
void ESP32_SMA_Inverter::writePacket(uint8_t *buf, uint8_t longwords, uint8_t ctrl, uint16_t ctrl2, uint16_t dstSUSyID, uint32_t dstSerial) {
  buf[pcktBufPos++] = 0x7E;   //Not included in checksum
  write32(buf, BTH_L2SIGNATURE);
  writeByte(buf, longwords);
  writeByte(buf, ctrl);
  write16(buf, dstSUSyID);
  write32(buf, dstSerial);
  write16(buf, ctrl2);
  write16(buf, appSUSyID);
  write32(buf, appSerial);
  write16(buf, ctrl2);
  write16(buf, 0);
  write16(buf, 0);
  write16(buf, pcktID | 0x8000);
}
//-------------------------------------------------------------------------
void ESP32_SMA_Inverter::writePacketTrailer(uint8_t *btbuffer) {
  fcsChecksum ^= 0xFFFF;
  btbuffer[pcktBufPos++] = fcsChecksum & 0x00FF;
  btbuffer[pcktBufPos++] = (fcsChecksum >> 8) & 0x00FF;
  btbuffer[pcktBufPos++] = 0x7E;  //Trailing byte
}
//-------------------------------------------------------------------------
void ESP32_SMA_Inverter::writePacketLength(uint8_t *buf) {
  buf[1] = pcktBufPos & 0xFF;         //Lo-Byte
  buf[2] = (pcktBufPos >> 8) & 0xFF;  //Hi-Byte
  buf[3] = buf[0] ^ buf[1] ^ buf[2];      //checksum
}
//-------------------------------------------------------------------------
bool ESP32_SMA_Inverter::validateChecksum() {
  if (pcktBufPos < 4 || pcktBuf[0] != 0x7E || pcktBuf[pcktBufPos - 1] != 0x7E) return false;
  fcsChecksum = 0xffff;
  //Skip over 0x7e at start and end of packet
  for(int i = 1; i <= pcktBufPos - 4; i++) {
    fcsChecksum = (fcsChecksum >> 8) ^ fcstab[(fcsChecksum ^ pcktBuf[i]) & 0xff];
  }
  fcsChecksum ^= 0xffff;

  if (get_u16(pcktBuf+pcktBufPos-3) == fcsChecksum) {
    return true;
  } else {
    DEBUG1_PRINTF("Invalid validateChecksum 0x%04X not 0x%04X\n", 
    fcsChecksum, get_u16(pcktBuf+pcktBufPos-3));
    return false;
  }
}
