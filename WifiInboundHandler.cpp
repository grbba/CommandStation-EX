/*
 *  © 2021 Fred Decker
 *  © 2021 Fred Decker
 *  © 2020-2025 Chris Harlow
 *  © 2020, Chris Harlow. All rights reserved.
 *  © 2020, Harald Barth.
 *  
 *  This file is part of Asbelos DCC API
 *
 *  This is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  It is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with CommandStation.  If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef ARDUINO_AVR_UNO_WIFI_REV2
#include <Arduino.h>
#include "WifiInboundHandler.h"
#include "RingStream.h"
#include "CommandDistributor.h"
#include "DIAG.h"
#include "Websockets.h"
#include "TelnetConsole.h"

WifiInboundHandler * WifiInboundHandler::singleton;

void WifiInboundHandler::setup(Stream * ESStream) {
  singleton=new WifiInboundHandler(ESStream);
}

void WifiInboundHandler::loop() {
  singleton->loop1();
}


WifiInboundHandler::WifiInboundHandler(Stream * ESStream) {
  wifiStream=ESStream;
  clientPendingCIPSEND=-1;
  inboundRing=new RingStream(INBOUND_RING);
  outboundRing=new RingStream(OUTBOUND_RING);
  pendingCipsend=false;
} 


// Handle any inbound transmission
// +IPD,x,lll:data is stored in streamer[x]
// Other input returns  
void WifiInboundHandler::loop1() {
   // First handle all inbound traffic events because they will block the sending 
   if (loop2()!=INBOUND_IDLE) return;

   WiThrottle::loop(outboundRing);
#ifdef WIFI_TELNET
   if (clientPendingCIPSEND < 0 && outboundRing->peek() < 0)
     TelnetConsole::pump(outboundRing);
#endif
   
    // if nothing is already CIPSEND pending, we can CIPSEND one reply
    if (clientPendingCIPSEND<0) {
       clientPendingCIPSEND=outboundRing->read();
       if (clientPendingCIPSEND>=0) {
         currentReplySize=outboundRing->count();
         pendingCipsend=true;
       }
     }
    

    if (pendingCipsend && millis()-lastCIPSEND > CIPSENDgap) {
         // add allowances for websockets
         bool websocket=clientPendingCIPSEND & Websockets::WEBSOCK_CLIENT_MARKER;
         byte realClient=clientPendingCIPSEND & ~Websockets::WEBSOCK_CLIENT_MARKER;
#ifdef WIFI_TELNET
         realClient &= ~TelnetConsole::CLIENT_MARKER;
#endif
         int16_t realSize=currentReplySize;
         if (websocket) realSize+=Websockets::getOutboundHeaderSize(currentReplySize);
#ifdef WIFI_TELNET
         TelnetConsole::MuteDiagnostics mute;
#endif
         if (Diag::WIFI) DIAG( F("WiFi: [[CIPSEND=%d,%d]]"), realClient, realSize);
         StringFormatter::send(wifiStream, F("AT+CIPSEND=%d,%d\r\n"),  realClient,realSize);
         pendingCipsend=false;
         return;
      }
    
    
#ifdef WIFI_TELNET
    byte consoleClient;
    byte consoleCommand[128];
    if ((preferConsole || inboundRing->peek() < 0) &&
        TelnetConsole::nextCommand(consoleClient, consoleCommand)) {
      preferConsole = false;
      CommandDistributor::parse(consoleClient, consoleCommand, outboundRing);
      return;
    }
#endif
    // if something waiting to execute, we can call it 
      int clientId=inboundRing->read();
      if (clientId>=0) {
         int count=inboundRing->count();
         if (Diag::WIFI) DIAG(F("Wifi EXEC: %d %d:"),clientId,count); 
         byte cmd[count+1];
         // Copy raw bytes to avoid websocket masked data being
         // confused with a ram-saving flash insert marker.
         for (int i=0;i<count;i++) cmd[i]=inboundRing->readRawByte();   
         cmd[count]=0;
         if (Diag::WIFI) DIAG(F("%e"),cmd); 
         
         CommandDistributor::parse(clientId,cmd,outboundRing);
         preferConsole = true;
         return;
      }
   }



// This is a Finite State Automation (FSA) handling the inbound bytes from an ES AT command processor    

WifiInboundHandler::INBOUND_STATE WifiInboundHandler::loop2() {
#ifdef WIFI_TELNET
  TelnetConsole::MuteDiagnostics mute;
#endif

  while (wifiStream->available()) {
    int ch = wifiStream->read();

    // echo the char to the diagnostic stream in escaped format
    if (Diag::WIFI) {
      // DIAG(F(" %d/"), loopState);
      StringFormatter::printEscape(ch); // DIAG in disguise
    }

    switch (loopState) {
      case ANYTHING:  // looking for +IPD, > , busy ,  n,CONNECTED, n,CLOSED, ERROR, SEND OK 
        
        if (ch == '+') {
          loopState = IPD;
          break; 
        }
        
        if (ch=='>') {
           if (clientPendingCIPSEND < 0) { loopState = SKIPTOEND; break; }
           bool websocket=clientPendingCIPSEND & Websockets::WEBSOCK_CLIENT_MARKER; 
           if (Diag::WIFI) DIAG(F("[XMIT %d ws=%b]"),currentReplySize,websocket);
           if (websocket) Websockets::writeOutboundHeader(wifiStream,currentReplySize); 
           for (int i=0;i<currentReplySize;i++) {
#ifdef WIFI_TELNET
             // RingStream::read interprets 0xff as a flash pointer marker.
             // Telnet negotiation contains literal 0xff and must be read raw.
             int cout = (clientPendingCIPSEND & TelnetConsole::CLIENT_MARKER)
               ? outboundRing->readRawByte() : outboundRing->read();
#else
             int cout=outboundRing->read();
#endif
             if (websocket && (cout=='\n')) cout='\r';
             wifiStream->write(cout);
             if (Diag::WIFI) StringFormatter::printEscape(cout); // DIAG in disguise
           }
           clientPendingCIPSEND=-1;
           pendingCipsend=false;
           loopState=SKIPTOEND;
           break;
        }
        
        if (ch=='R') { // Received ... bytes 
          loopState=SKIPTOEND;
          break;
        }
       
        if (ch=='S') { // SEND OK probably 
          loopState=SKIPTOEND;
          lastCIPSEND=0; // no need to wait next time 
          break;
        }
        
        if (ch=='b') {   // This is a busy indicator... probabaly must restart a CIPSEND  
           pendingCipsend=(clientPendingCIPSEND>=0);
           if (pendingCipsend) lastCIPSEND=millis(); // forces a gap to next CIPSEND
           loopState=SKIPTOEND; 
           break; 
        }
        
        if (ch>='0' && ch<='9') { 
              runningClientId=ch-'0';
              loopState=GOT_CLIENT_ID;
              break;
        }

        if (ch=='E' || ch=='l') { // ERROR or "link is not valid"
          if (clientPendingCIPSEND>=0) {
            // A CIPSEND was errored... just toss it away
            purgeCurrentCIPSEND(); 
          }
          loopState=SKIPTOEND; 
          break; 
        }
        
        break;
        
      case IPD:  // Looking for I   in +IPD
        loopState = (ch == 'I') ? IPD1 : SKIPTOEND;
        break;
        
      case IPD1:  // Looking for P   in +IPD
        loopState = (ch == 'P') ? IPD2 : SKIPTOEND;
        break;
        
      case IPD2:  // Looking for D   in +IPD
        loopState = (ch == 'D') ?  IPD3 : SKIPTOEND;
        break;
        
      case IPD3:  // Looking for ,   After +IPD
        loopState = (ch == ',') ? IPD4_CLIENT : SKIPTOEND;
        break;
        
      case IPD4_CLIENT:  // reading connection id
        if (ch >= '0' && ch <='9'){
           runningClientId=ch-'0';
           loopState=IPD5;
        }
        else loopState=SKIPTOEND;
        break;
        
      case IPD5:  // Looking for ,   After +IPD,client
        loopState = (ch == ',') ? IPD6_LENGTH : SKIPTOEND;
        dataLength=0;  // ready to start collecting the length
        break;
        
      case IPD6_LENGTH: // reading for length
        if (ch == ':') {
          if (dataLength==0) {
            loopState=ANYTHING;
            break;
          }
          if (Diag::WIFI) DIAG(F("Wifi inbound data(%d:%d):"),runningClientId,dataLength); 
          
#ifdef WIFI_TELNET
          loopState = TelnetConsole::handles(runningClientId) ? IPD_TELNET : IPD_ROUTE;
#else
          beginPacketData();
#endif
          break; 
        }
        dataLength = dataLength * 10 + (ch - '0');
        break;
        
      case IPD_ROUTE:
#ifdef WIFI_TELNET
        if (TelnetConsole::claim(runningClientId, ch)) {
          loopState = IPD_TELNET;
          TelnetConsole::receive(runningClientId, ch);
          if (--dataLength == 0) loopState = ANYTHING;
          break;
        }
#endif
        beginPacketData();
        consumePacketData(ch);
        break;

      case IPD_TELNET:
#ifdef WIFI_TELNET
        TelnetConsole::receive(runningClientId, ch);
#endif
        if (--dataLength == 0) loopState = ANYTHING;
        break;

      case IPD_DATA:
      case IPD_PRESCAN:
      case IPD_POSTSCAN:
      case IPD_IGNORE_DATA:
        consumePacketData(ch);
        break;

      case GOT_CLIENT_ID:  // got x before CLOSE or CONNECTED
        loopState=(ch==',') ? GOT_CLIENT_ID2: SKIPTOEND;
        break;
        
      case GOT_CLIENT_ID2:  // got "x,"  
        if (ch=='C') {
         // got "x C" before CLOSE or CONNECTED, or CONNECT FAILED
         int pendingClient = clientPendingCIPSEND;
#ifdef WIFI_TELNET
         if (pendingClient >= 0) pendingClient &= ~(TelnetConsole::CLIENT_MARKER | Websockets::WEBSOCK_CLIENT_MARKER);
#endif
         if (runningClientId==pendingClient) purgeCurrentCIPSEND();
         else CommandDistributor::forget(runningClientId);
#ifdef WIFI_TELNET
         outboundRing->discardClient(runningClientId,
           clientPendingCIPSEND < 0 ? 0 : currentReplySize);
#endif
        }
        loopState=SKIPTOEND;   
        break;
         
      case SKIPTOEND: // skipping for /n
        if (ch=='\n') loopState=ANYTHING;
        break;
    }  // switch
  } // available
  return (loopState==ANYTHING) ? INBOUND_IDLE: INBOUND_BUSY;
}

void WifiInboundHandler::beginPacketData() {
  if (runningClientId < 0 || runningClientId >= MAX_NUM_TCP_CLIENTS ||
      (dataLength < 100 && inboundRing->freeSpace() <= dataLength + 1)) {
    loopState = IPD_IGNORE_DATA;
    return;
  }
  inboundRing->mark(runningClientId);
  prescanPoint = 0;
  loopState = dataLength > 100 ? IPD_PRESCAN : IPD_DATA;
}

void WifiInboundHandler::consumePacketData(int ch) {
  const char key[] = "Sec-WebSocket-Key: ";
  dataLength--;
  switch (loopState) {
    case IPD_DATA:
      inboundRing->write(ch);
      if (!dataLength) { inboundRing->commit(); loopState = ANYTHING; }
      break;
    case IPD_PRESCAN:
      if (ch == key[prescanPoint]) {
        if (!key[++prescanPoint]) {
          inboundRing->print(key);
          loopState = IPD_POSTSCAN;
        }
      } else prescanPoint = 0;
      if (!dataLength) { inboundRing->commit(); loopState = ANYTHING; }
      break;
    case IPD_POSTSCAN:
      inboundRing->write(ch);
      if (ch == '\n' || !dataLength) {
        inboundRing->commit();
        loopState = dataLength ? IPD_IGNORE_DATA : ANYTHING;
      }
      break;
    default:
      if (!dataLength) loopState = ANYTHING;
      break;
  }
}

void WifiInboundHandler::purgeCurrentCIPSEND() {
         // A CIPSEND was sent but errored... or the client closed just toss it away
         byte client = clientPendingCIPSEND & ~Websockets::WEBSOCK_CLIENT_MARKER;
#ifdef WIFI_TELNET
         client &= ~TelnetConsole::CLIENT_MARKER;
#endif
         CommandDistributor::forget(client);
         DIAG(F("Wifi: DROPPING CIPSEND=%d,%d"),clientPendingCIPSEND,currentReplySize);
         for (int i=0;i<currentReplySize;i++) {
#ifdef WIFI_TELNET
           if (clientPendingCIPSEND & TelnetConsole::CLIENT_MARKER) outboundRing->readRawByte();
           else
#endif
             outboundRing->read();
         }
         pendingCipsend=false;  
         clientPendingCIPSEND=-1;
#ifdef WIFI_TELNET
         outboundRing->discardClient(client, 0);
#endif
}

#endif
