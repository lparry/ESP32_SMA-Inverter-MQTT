#include "ESP32_SMA_Inverter_App.h"
#include "BluetoothAuthObserver.h"
#include "gap_sdk_fake.h"
#include <cassert>
#include <cmath>
#include <iostream>
// A fixed host clock makes CRC collision and clock-write tests deterministic.
#if defined(__APPLE__)
#define SMA_TEST_TIME_NOEXCEPT
#else
#define SMA_TEST_TIME_NOEXCEPT noexcept
#endif
extern "C" time_t time(time_t *out) SMA_TEST_TIME_NOEXCEPT {
 const time_t now=1800000000;if(out)*out=now;return now;
}
struct AfterProtocolHeader { char c; uint32_t value; };
static_assert(sizeof(L1Hdr)==18, "SMA wire header must remain packed");
static_assert(alignof(AfterProtocolHeader)==alignof(uint32_t), "packing must be restored");
std::vector<uint8_t> l1Frame(const std::vector<uint8_t>&payload,uint16_t command=1,const uint8_t*source=nullptr){
 uint16_t n=18+payload.size();std::vector<uint8_t> header(18,0);
 header[0]=0x7e;header[1]=n;header[2]=n>>8;header[3]=header[0]^header[1]^header[2];
 header[16]=command;header[17]=command>>8;
 if(source)std::copy(source,source+6,header.begin()+4);
 header.insert(header.end(),payload.begin(),payload.end());return header;
}
void queueL1(BluetoothSerial&b,const std::vector<uint8_t>&payload,uint16_t command=1,const uint8_t*source=nullptr){
 auto frame=l1Frame(payload,command,source);b.inject(frame.data(),frame.size());
}
void put(std::vector<uint8_t>&v,size_t offset,uint64_t n,size_t width=4){
 for(size_t k=0;k<width;k++)v.at(offset+k)=n>>(8*k);
}
std::vector<uint8_t> response(ESP32_SMA_Inverter&i,const std::vector<uint8_t>&data,uint32_t first=1,uint32_t last=1,uint16_t status=0){
 std::vector<uint8_t> v(41+data.size());v[0]=0x7e;put(v,1,BTH_L2SIGNATURE);v[5]=9+data.size()/4;
 put(v,15,i.invData.SUSyID,2);put(v,17,i.invData.Serial);put(v,23,status,2);put(v,27,i.pcktID,2);
 put(v,33,first);put(v,37,last);std::copy(data.begin(),data.end(),v.begin()+41);return v;
}
void queueResponse(BluetoothSerial&b,std::vector<uint8_t> v,bool corrupt=false,const uint8_t*source=nullptr){
 uint16_t crc=0xffff;
 for(size_t k=1;k<v.size();k++){crc^=v[k];for(int bit=0;bit<8;bit++)crc=(crc>>1)^((crc&1)?0x8408:0);}
 crc^=0xffff;if(corrupt)crc^=1;v.push_back(crc);v.push_back(crc>>8);
 std::vector<uint8_t> wire{0x7e};
 for(size_t k=1;k<v.size();k++){auto c=v[k];if(c==0x7d||c==0x7e||c==0x11||c==0x12||c==0x13){wire.push_back(0x7d);wire.push_back(c^0x20);}else wire.push_back(c);}
 wire.push_back(0x7e);queueL1(b,wire,1,source?source:ESP32_SMA_Inverter::invData.BTAddress);
}
std::vector<uint8_t> loginResponse(ESP32_SMA_Inverter&i,uint16_t susyId,uint32_t serial){
 std::vector<uint8_t> data(8);put(data,0,uint32_t(time(nullptr)));
 auto v=response(i,data);put(v,15,susyId,2);put(v,17,serial);put(v,29,0xFFFD040D);return v;
}
E_RC query(const std::vector<uint8_t>&data,uint32_t first=1,uint32_t last=1,uint16_t status=0){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;b.input.clear();b.output.clear();
 b.sent=[&]{queueResponse(b,response(i,data,first,last,status));b.output.clear();};
 auto rc=i.getInverterDataCfl(0x51000200,first,last);b.sent=nullptr;return rc;
}







struct AppHttpResponse {
 int code=0;
 std::string body;
 std::map<std::string,std::string> headers;
 bool restarted=false;
};






















void testBluetoothTimerRollover(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 b.input.clear();fake::ticks=UINT32_MAX-25ULL;auto start=fake::ticks;
 b.onAvailable=[&]{if(fake::ticks-start>=50 && b.input.empty())b.input.push_back(0x42);};
 assert(i.BTgetByte()==0x42);assert(!i.readTimeout);assert(fake::ticks-start>=50);
 b.onAvailable=nullptr;fake::ticks=UINT32_MAX-25ULL;start=fake::ticks;
 i.BTgetByte();assert(i.readTimeout);assert(fake::ticks-start>=20000);
}
void testSlowPacketDeadline(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 b.input.clear();fake::ticks=100;auto start=fake::ticks;auto delays=fake::delayCalls;
 std::vector<uint8_t> packet(200,0);packet[0]=0x7e;packet[1]=200;packet[3]=0x7e^200;packet[16]=1;
 size_t offset=0;uint64_t next=start+1000;
 b.onAvailable=[&]{if(fake::ticks>=next&&offset<packet.size()){b.input.push_back(packet[offset++]);next+=1000;}};
 assert(i.getPacket(i.sixff,1)==E_NODATA);assert(fake::ticks-start<20100);
 assert(fake::delayCalls>delays);assert(!i.receivingPacket);b.onAvailable=nullptr;
}
void testCallerOperationDeadlinesAndRollover(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 const auto savedIdentity=i.invData;const auto savedId=i.pcktID;
 const bool savedConnected=i.btConnected;const auto savedTicks=fake::ticks;
 const auto savedSent=b.sent;const auto savedAvailable=b.onAvailable;
 const uint8_t target[]={0x20,0x30,0x40,0x50,0x60,0x70};
 std::copy(target,target+6,i.invData.BTAddress);i.invData.SUSyID=0x1234;i.invData.Serial=55;

 // Initialization shares one 20-second budget across its handshake stages.
 b.input.clear();b.output.clear();fake::ticks=9000000;queueL1(b,{0,4,0x70,0,1},2,target);
 const uint64_t initStarted=fake::ticks;uint64_t initReplyWindow=0;bool initReplyQueued=false;int initSends=0;
 b.sent=[&]{if(++initSends==1)initReplyWindow=fake::ticks;b.output.clear();};
 b.onAvailable=[&]{
  if(initSends==1&&!initReplyQueued&&fake::ticks-initReplyWindow>=19800){
   queueL1(b,std::vector<uint8_t>(14),5,target);initReplyQueued=true;
  }
 };
 assert(i.initialiseSMAConnection()==E_NODATA);
 assert(initReplyQueued&&initSends==2&&fake::ticks-initStarted<20500);

 // A late unrelated login reply must not start a fresh 20-second packet wait.
 b.input.clear();b.output.clear();fake::ticks=uint64_t(UINT32_MAX)-5000;
 const uint64_t loginStarted=fake::ticks;uint64_t loginReceiveStarted=0;bool loginReplyQueued=false;
 b.sent=[&]{loginReceiveStarted=fake::ticks;b.output.clear();};
 b.onAvailable=[&]{
  if(!loginReplyQueued&&fake::ticks-loginReceiveStarted>=19800){
   auto unrelated=loginResponse(i,0x1234,i.invData.Serial);
   put(unrelated,27,(i.pcktID+1)&0x7fff,2);queueResponse(b,unrelated);loginReplyQueued=true;
  }
 };
 assert(i.logonSMAInverter("0000",USERGROUP)==E_NODATA);
 assert(loginReplyQueued&&fake::ticks-loginStarted<20500);
 assert(!i.receivingPacket&&i.pcktBufPos==0);

 // The plant clock read has the same overall bound when its first reply is stale.
 b.input.clear();b.output.clear();fake::ticks=11000000;
 const uint64_t clockStarted=fake::ticks;uint64_t clockReceiveStarted=0;bool clockReplyQueued=false;
 auto clockReply=[&]{
  std::vector<uint8_t> data(24);put(data,0,0x00236d00);put(data,4,1800000000);
  put(data,8,1790000000);put(data,16,36000);put(data,20,7);
  auto reply=response(i,data);put(reply,29,0xf000020b);return reply;
 };
 b.sent=[&]{clockReceiveStarted=fake::ticks;b.output.clear();};
 b.onAvailable=[&]{
  if(!clockReplyQueued&&fake::ticks-clockReceiveStarted>=19800){
   auto unrelated=clockReply();put(unrelated,27,(i.pcktID+1)&0x7fff,2);
   queueResponse(b,unrelated);clockReplyQueued=true;
  }
 };
 int32_t plantNow=0,lastSet=0,offset=0;uint32_t setCount=0;
 assert(i.readPlantTime(&plantNow,&lastSet,&offset,&setCount)==E_NODATA);
 assert(clockReplyQueued&&fake::ticks-clockStarted<20500);

 // CFL has a 30-second transaction budget even after an unrelated late reply.
 b.input.clear();b.output.clear();fake::ticks=7000000;
 const uint64_t queryStarted=fake::ticks;uint64_t queryReceiveStarted=0;bool queryReplyQueued=false;
 b.sent=[&]{queryReceiveStarted=fake::ticks;b.output.clear();};
 b.onAvailable=[&]{
  if(!queryReplyQueued&&fake::ticks-queryReceiveStarted>=19800){
   auto unrelated=response(i,{});put(unrelated,27,(i.pcktID+1)&0x7fff,2);
   queueResponse(b,unrelated);queryReplyQueued=true;
  }
 };
 assert(i.getInverterDataCfl(0x51000200,1,1)==E_NODATA);
 assert(queryReplyQueued&&fake::ticks-queryStarted<31000);
 assert(!i.receivingPacket&&i.pcktBufPos==0);

 // A deadline also bounds a packet assembled from multiple L1 fragments;
 // its absolute comparison remains correct when millis() wraps.
 b.input.clear();b.output.clear();fake::ticks=uint64_t(UINT32_MAX)-100;
 const uint32_t packetStarted=millis();const uint64_t packetStartTicks=fake::ticks;
 const uint32_t packetDeadline=packetStarted+160UL;
 assert(packetDeadline<packetStarted);
 const std::vector<uint8_t> continuation(80,0x44);
 const auto secondFrame=l1Frame(continuation,1);
 size_t secondOffset=0;uint64_t nextByteAt=0;bool firstFragmentQueued=false;
 b.onAvailable=[&]{
  const uint64_t elapsed=fake::ticks-packetStartTicks;
  if(!firstFragmentQueued&&elapsed>=70){
   queueL1(b,{0x7e,0x01,0x02,0x03},8);firstFragmentQueued=true;
  }
  if(firstFragmentQueued&&secondOffset==0&&elapsed>=120){
   secondOffset=20;b.inject(secondFrame.data(),secondOffset);nextByteAt=elapsed+10;
  } else if(secondOffset>0&&secondOffset<secondFrame.size()&&elapsed>=nextByteAt){
   b.inject(secondFrame.data()+secondOffset,1);++secondOffset;nextByteAt+=10;
  }
 };
 assert(i.getPacket(i.sixff,1,&packetDeadline)==E_NODATA);
 assert(firstFragmentQueued&&secondOffset>20&&secondOffset<secondFrame.size());
 assert(fake::ticks-packetStartTicks<500&&!i.receivingPacket&&i.pcktBufPos==0);

 // The next receive has no inherited deadline and can complete normally.
 b.onAvailable=nullptr;b.input.clear();queueL1(b,{0x42},1);
 assert(i.getPacket(i.sixff,1)==E_OK&&!i.receivingPacket);
 b.sent=savedSent;b.onAvailable=savedAvailable;b.input.clear();b.output.clear();
 i.invData=savedIdentity;i.pcktID=savedId;i.btConnected=savedConnected;fake::ticks=savedTicks;
}
void testFragmentEscapes(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 const std::vector<uint8_t> wire={0x7e,0xff,0x03,0x60,0x65,0x7d,0x5e,0x7d,0x5d,0x7e};
 const std::vector<uint8_t> decoded={0x7e,0xff,0x03,0x60,0x65,0x7e,0x7d,0x7e};
 for(size_t split=1;split<wire.size();split++){
  b.input.clear();queueL1(b,{wire.begin(),wire.begin()+split},8);queueL1(b,{wire.begin()+split,wire.end()});
  assert(i.getPacket(i.sixff,1)==E_OK);assert(i.pcktBufPos==decoded.size());
  assert(std::equal(decoded.begin(),decoded.end(),i.pcktBuf));
 }
 b.input.clear();queueL1(b,{0x7e,0xff,0x03,0x60,0x65,0x7d});
 assert(i.getPacket(i.sixff,1)==E_INVRESP);
}
void testMalformedRecordRanges(){
 std::vector<uint8_t> record(16);put(record,0,uint32_t(MeteringTotWhOut)<<8);put(record,8,12345,8);
 assert(query(record,0,UINT32_MAX)==E_INVRESP);
 assert(query(record,2,1)==E_INVRESP);
 assert(query(record,1,2)==E_INVRESP);
 assert(query(record)==E_OK);assert(ESP32_SMA_Inverter::invData.ETotal==12345);
 auto&i=ESP32_SMA_Inverter::getInstance();i.pcktBufPos=0;assert(!i.validateChecksum());
}
void testStatusRecordBounds(){
 for(size_t n:{size_t(16),size_t(20),size_t(36)}){
  std::vector<uint8_t> record(n);put(record,0,(8U<<24)|(uint32_t(OperationHealth)<<8));
  assert(query(record)==E_INVRESP);
  assert(ESP32_SMA_Inverter::getInstance().getattribute(record.data(),n)==0xFFFFFD);
 }
 std::vector<uint8_t> record(40);put(record,0,(8U<<24)|(uint32_t(OperationHealth)<<8));
 put(record,8,(1U<<24)|51);put(record,12,0xfffffe);
 assert(query(record)==E_OK);assert(ESP32_SMA_Inverter::invData.DevStatus==51);
 put(record,0,uint32_t(OperationHealth)<<8);assert(query(record)==E_INVRESP);
}

std::vector<uint8_t> numericRecord(uint16_t lri,uint8_t channel,int32_t value){
 std::vector<uint8_t> record(40);put(record,0,(0x40U<<24)|(uint32_t(lri)<<8)|channel);put(record,16,uint32_t(value));return record;
}
void testDcChannels(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 std::vector<uint8_t> data;
 for(auto r:{numericRecord(DcMsVol,2,20000),numericRecord(DcMsVol,1,10000),numericRecord(DcMsAmp,1,1000),numericRecord(DcMsAmp,2,2000)})data.insert(data.end(),r.begin(),r.end());
 auto poll=[&]{b.input.clear();b.output.clear();b.sent=[&]{queueResponse(b,response(i,data,1,data.size()/40));b.output.clear();};auto rc=i.getInverterData(SpotDCVoltage);b.sent=nullptr;return rc;};
 assert(poll()==E_OK);assert(i.dispData.Udc[0]==100 && i.dispData.Udc[1]==200);
 assert(i.dispData.Idc[0]==1 && i.dispData.Idc[1]==2);
 data=numericRecord(DcMsVol,2,30000);auto current=numericRecord(DcMsAmp,1,4000);data.insert(data.end(),current.begin(),current.end());
 assert(poll()==E_OK);assert(std::isnan(i.dispData.Udc[0]));assert(std::isnan(i.dispData.Idc[1]));
 assert(i.dispData.Udc[1]==300 && i.dispData.Idc[0]==4);
}
std::vector<uint8_t> decodeOutput(const std::vector<uint8_t>&wire){
 std::vector<uint8_t> out;bool escape=false;
 for(size_t k=18;k<wire.size();k++){auto c=wire[k];if(escape){out.push_back(c^0x20);escape=false;}else if(c==0x7d)escape=true;else out.push_back(c);}
 return out;
}
void testUnsupportedTemperature(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;i.btConnected=true;
 auto poll=[&](bool mandatoryError){
  b.input.clear();b.output.clear();
  b.sent=[&]{auto tx=decodeOutput(b.output);uint16_t lri=get_u32(tx.data()+33)>>8;std::vector<uint8_t> data;
   uint16_t status=0;
   if(lri==CoolsysTmpNom||(mandatoryError&&lri==GridMsHz))status=E_LRINOTAVAIL;
   else if(lri==MeteringTotWhOut){data.resize(16);put(data,0,uint32_t(lri)<<8);put(data,8,50000,8);}
   else if(lri==OperationHealth||lri==OperationGriSwStt){data.resize(40);put(data,0,(8U<<24)|(uint32_t(lri)<<8));put(data,8,(1U<<24)|51);put(data,12,0xfffffe);}
   else data=numericRecord(lri,1,1234);
   queueResponse(b,response(i,data,1,1,status));b.output.clear();};
  auto rc=i.ReadCurrentData();b.sent=nullptr;return rc;
 };
 i.dispData.Uac[1]=222; i.invData.ETodayValid=true;
 assert(poll(false)==E_OK);assert(std::isnan(i.dispData.Uac[1]));assert(!i.invData.ETodayValid);assert(i.dispData.Pac==1234);assert(i.invData.ETotalValid);assert(std::isnan(i.dispData.InvTemp));
 assert(poll(true)==E_NODATA);i.btConnected=false;
}






bool unsafeCrc(ESP32_SMA_Inverter&i){return !i.isCrcValid(i.pcktBuf[i.pcktBufPos-3],i.pcktBuf[i.pcktBufPos-2]);}
void testLoginAndInitCrc(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;uint16_t seed=0;
 const auto savedIdentity=i.invData;
 const uint8_t target[]={0x20,0x30,0x40,0x50,0x60,0x70};
 std::copy(target,target+6,i.invData.BTAddress);i.invData.SUSyID=0x007d;i.invData.Serial=0;
 for(;seed<1000;seed++){
  i.pcktID=seed+1;i.writePacketHeader(i.pcktBuf,1,i.sixff);i.writePacket(i.pcktBuf,14,0xa0,0x0100,0xffff,0xffffffff);
  i.write32(i.pcktBuf,0xfffd040c);i.write32(i.pcktBuf,USERGROUP);i.write32(i.pcktBuf,900);i.write32(i.pcktBuf,time(nullptr));i.write32(i.pcktBuf,0);
  uint8_t pw[12];std::fill(pw,pw+12,0x88);std::fill(pw,pw+4,uint8_t('0'+0x88));i.writeArray(i.pcktBuf,pw,12);i.writePacketTrailer(i.pcktBuf);
  if(unsafeCrc(i))break;
 }
 assert(seed<1000);i.pcktID=seed;b.output.clear();b.input.clear();
 b.sent=[&]{auto tx=decodeOutput(b.output);assert(b.output[b.output.size()-3]!=0x7d&&b.output[b.output.size()-3]!=0x7e);
  std::vector<uint8_t> data(8);put(data,0,get_u32(tx.data()+41));auto reply=response(i,data);
  put(reply,15,0x1234,2);put(reply,17,456);put(reply,29,0xFFFD040D);queueResponse(b,reply);b.output.clear();};
 assert(i.logonSMAInverter("0000",USERGROUP)==E_OK);assert(i.pcktID>seed+1);
 assert(i.invData.SUSyID==0x1234&&i.invData.Serial==456);b.sent=nullptr;
 i.invData=savedIdentity;
 for(seed=0;seed<1000;seed++){
  i.pcktID=seed+1;i.writePacketHeader(i.pcktBuf,1,i.sixff);i.writePacket(i.pcktBuf,9,0xa0,0,0xffff,0xffffffff);
  i.write32(i.pcktBuf,0x00000200);i.write32(i.pcktBuf,0);i.write32(i.pcktBuf,0);i.writePacketTrailer(i.pcktBuf);
  if(unsafeCrc(i))break;
 }
 assert(seed<1000);i.pcktID=seed;b.output.clear();b.input.clear();queueL1(b,{0,4,0x70,0,1},2);int step=0;
 b.sent=[&]{if(step++==0)queueL1(b,std::vector<uint8_t>(14),5);else{
   assert(!unsafeCrc(i));std::vector<uint8_t> data(20);put(data,16,123);auto reply=response(i,data);put(reply,29,0x00000201);queueResponse(b,reply);}b.output.clear();};
 assert(i.initialiseSMAConnection()==E_OK);assert(i.pcktID>seed+1);b.sent=nullptr;
 i.invData=savedIdentity;
}
void testLoginFiltersSenderAndKnownSerial(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;const auto savedIdentity=i.invData;const auto savedId=i.pcktID;
 const uint8_t target[]={0x10,0x21,0x32,0x43,0x54,0x65};
 const uint8_t other[]={0x99,0x88,0x77,0x66,0x55,0x44};
 std::copy(target,target+6,i.invData.BTAddress);i.invData.SUSyID=0x007d;i.invData.Serial=123456;
 i.pcktID=0x7fff;b.input.clear();b.output.clear();
 b.sent=[&]{
  queueResponse(b,loginResponse(i,0x4321,123456),false,other);
  auto wrongCommand=loginResponse(i,0x5678,123456);put(wrongCommand,29,0xFFFD010F);queueResponse(b,wrongCommand);
  queueResponse(b,loginResponse(i,0x5678,654321));
  queueResponse(b,loginResponse(i,0x1234,123456));
  b.output.clear();
 };
 assert(i.logonSMAInverter("0000",USERGROUP)==E_OK);
 assert(i.pcktID==0x8000);assert(i.invData.SUSyID==0x1234&&i.invData.Serial==123456);
 assert(b.input.empty());b.sent=nullptr;i.invData=savedIdentity;i.pcktID=savedId;
}
void testLoginBoundsUnrelatedReplies(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;const auto savedIdentity=i.invData;const auto savedId=i.pcktID;
 const uint8_t target[]={0x10,0x21,0x32,0x43,0x54,0x65};
 std::copy(target,target+6,i.invData.BTAddress);i.invData.SUSyID=0x007d;i.invData.Serial=123456;
 i.pcktID=40;b.input.clear();b.output.clear();
 size_t oneReplyBytes=0;
 b.sent=[&]{for(unsigned k=0;k<11;++k){queueResponse(b,loginResponse(i,0x1234,654321));if(k==0)oneReplyBytes=b.input.size();}b.output.clear();};
 assert(i.logonSMAInverter("0000",USERGROUP)==E_INVRESP);
 assert(i.invData.SUSyID==0x007d&&i.invData.Serial==123456);assert(b.input.size()==oneReplyBytes);
 b.input.clear();
 b.sent=nullptr;i.invData=savedIdentity;i.pcktID=savedId;
}
void testSenderAddressMatching(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 uint8_t targetWithFF[]={0x10,0xff,0x32,0x43,0x54,0x65};
 uint8_t nearMatch[6];std::copy(targetWithFF,targetWithFF+6,nearMatch);nearMatch[1]=0x21;
 b.input.clear();queueL1(b,{0xa1},1,nearMatch);queueL1(b,{0xb2},1,targetWithFF);
 assert(i.getPacket(targetWithFF,1)==E_OK);
 assert(i.pcktBufPos==19&&i.pcktBuf[18]==0xb2);

 uint8_t exactAddress[]={0x10,0x21,0x32,0x43,0x54,0x65};
 b.input.clear();queueL1(b,{0xc3},1,exactAddress);
 assert(i.getPacket(exactAddress,1)==E_OK);
 assert(i.pcktBufPos==19&&i.pcktBuf[18]==0xc3);

 uint8_t broadcast[]={0xff,0xff,0xff,0xff,0xff,0xff};
 const uint8_t anySender[]={0xde,0xad,0xbe,0xef,0x12,0x34};
 b.input.clear();queueL1(b,{0xd4},1,anySender);
 assert(i.getPacket(broadcast,1)==E_OK);
 assert(i.pcktBufPos==19&&i.pcktBuf[18]==0xd4);
}
void testLogoffRetriesCrcCollision(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;const uint16_t savedId=i.pcktID;const bool savedWriteFails=b.writeFails;
 uint16_t seed=0;
 for(;seed<1000;++seed){
  i.pcktID=seed+1;i.writePacketHeader(i.pcktBuf,0x01,i.sixff);
  i.writePacket(i.pcktBuf,0x08,0xA0,0x0300,0xFFFF,0xFFFFFFFF);
  i.write32(i.pcktBuf,0xFFFD010E);i.write32(i.pcktBuf,0xFFFFFFFF);
  i.writePacketTrailer(i.pcktBuf);i.writePacketLength(i.pcktBuf);
  if(unsafeCrc(i))break;
 }
 assert(seed<1000);i.pcktID=seed;b.output.clear();b.sent=nullptr;
 i.logoffSMAInverter();assert(i.pcktID>seed+1);
 auto tx=decodeOutput(b.output);assert(tx.size()>=36&&tx.front()==0x7e&&tx.back()==0x7e);
 assert(tx[5]==0x08&&tx[6]==0xA0);assert(get_u16(tx.data()+7)==0xFFFF);
 assert(get_u32(tx.data()+9)==0xFFFFFFFF);assert(get_u16(tx.data()+13)==0x0300);
 assert(get_u16(tx.data()+27)==uint16_t(i.pcktID|0x8000));
 assert(get_u32(tx.data()+29)==0xFFFD010E&&get_u32(tx.data()+33)==0xFFFFFFFF);
 const uint8_t crcLow=tx[tx.size()-3],crcHigh=tx[tx.size()-2];
 assert(crcLow!=0x7d&&crcLow!=0x7e&&crcHigh!=0x7d&&crcHigh!=0x7e);
 uint16_t crc=0xffff;
 for(size_t k=1;k<tx.size()-3;++k){crc^=tx[k];for(int bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0x8408:0);}
 crc^=0xffff;assert(get_u16(tx.data()+tx.size()-3)==crc);

 b.output.clear();b.writeFails=true;i.logoffSMAInverter();assert(b.output.empty());
 b.writeFails=savedWriteFails;i.pcktID=savedId;
}






void testClockReplyCorrelation(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;i.invData.SUSyID=0x1234;i.invData.Serial=55;
 i.invData.BTAddress[0]=0x20;
 auto clock=[&]{std::vector<uint8_t> data(24);put(data,0,0x00236d00);put(data,4,1800000000);put(data,8,1790000000);put(data,16,36000);put(data,20,7);auto v=response(i,data);put(v,29,0xf000020b);return v;};
 for(int mode=0;mode<8;mode++){
  b.input.clear();b.output.clear();
  b.sent=[&]{auto bad=clock();
   if(mode==0)put(bad,27,i.pcktID-1,2);
   if(mode==1)put(bad,15,999,2);
   if(mode==2)put(bad,17,999);
   if(mode==3)put(bad,23,1,2);
   if(mode==4)put(bad,29,0x51000201);
   if(mode==5)put(bad,41,0x00263f00);
   queueResponse(b,bad,mode==6);if(mode==7)b.input[4]^=1;queueResponse(b,clock());b.output.clear();};
  int32_t now=0,last=0,offset=0;uint32_t count=0;assert(i.readPlantTime(&now,&last,&offset,&count)==E_OK);
  assert(now==1800000000 && offset==36000 && count==7);b.sent=nullptr;
 }
 unsigned writes=0;b.output.clear();b.input.clear();i.btConnected=true;
 b.sent=[&]{auto tx=decodeOutput(b.output);if(get_u32(tx.data()+45)!=0)++writes;
  auto bad=clock();put(bad,27,i.pcktID-1,2);for(int k=0;k<4;k++)queueResponse(b,bad);b.output.clear();};
 int32_t before=0,after=0;assert(i.syncPlantTime(36000,&before,&after)==E_INVRESP);assert(writes==0);
 b.sent=nullptr;i.btConnected=false;i.invData.Serial=0;i.invData.BTAddress[0]=0;
}
void testClockTargetsOneInverter(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;const auto saved=i.invData;
 const uint8_t address[]={0x20,0x30,0x40,0x50,0x60,0x70};
 std::copy(address,address+6,i.invData.BTAddress);i.invData.SUSyID=0x1234;i.invData.Serial=55;i.btConnected=true;
 b.input.clear();b.output.clear();unsigned reads=0,writes=0;
 b.sent=[&]{
  assert(std::equal(address,address+6,b.output.begin()+10));
  auto tx=decodeOutput(b.output);assert(get_u16(tx.data()+7)==0x1234);assert(get_u32(tx.data()+9)==55);
  if(get_u32(tx.data()+45)!=0){++writes;b.output.clear();return;}
  ++reads;std::vector<uint8_t> data(24);put(data,0,0x00236d00);
  put(data,4,writes?1800000000:1790000000);put(data,8,writes?1800000000:1790000000);
  put(data,16,36000);put(data,20,writes?8:7);auto v=response(i,data);put(v,29,0xf000020b);
  queueResponse(b,v);b.output.clear();
 };
 int32_t before=0,after=0;assert(i.syncPlantTime(36000,&before,&after)==E_OK);
 assert(reads==2&&writes==1&&before==1790000000&&after==1800000000);
 const auto target=i.invData;
 for(int invalid=0;invalid<6;++invalid){
  i.invData=target;
  if(invalid==0)i.invData.Serial=0;
  if(invalid==1)i.invData.Serial=UINT32_MAX;
  if(invalid==2)i.invData.SUSyID=0;
  if(invalid==3)i.invData.SUSyID=UINT16_MAX;
  if(invalid==4)std::fill(i.invData.BTAddress,i.invData.BTAddress+6,0);
  if(invalid==5)std::fill(i.invData.BTAddress,i.invData.BTAddress+6,0xff);
  assert(i.syncPlantTime(36000,&before,&after)==E_BADARG);assert(reads==2&&writes==1);
 }
 b.sent=nullptr;i.btConnected=false;i.invData=saved;
}




void testStaleErrorIsIgnored(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;auto record=numericRecord(GridMsTotW,1,1234);
 b.input.clear();b.output.clear();b.sent=[&]{auto stale=response(i,record,1,1,E_LRINOTAVAIL);
  put(stale,27,(i.pcktID-1)&0x7fff,2);queueResponse(b,stale);queueResponse(b,response(i,record));b.output.clear();};
 assert(i.getInverterDataCfl(0x51000200,1,1)==E_OK);assert(i.dispData.Pac==1234);b.sent=nullptr;
}
void testMalformedPacketClearsSession(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;b.input.clear();
 queueL1(b,{1,2,3,4});b.input[3]^=1;queueL1(b,{5,6,7,8});i.btConnected=true;
 assert(i.getPacket(i.sixff,1)==E_CHKSUM);assert(!i.btConnected);assert(b.input.empty());
}
void testManyValidFragments(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;b.input.clear();b.output.clear();
 b.sent=[&]{queueResponse(b,response(i,numericRecord(GridMsTotW,1,4567)));
  std::vector<uint8_t> wire(b.input.begin()+18,b.input.end());b.input.clear();
  for(size_t k=0;k<wire.size();k+=5){size_t end=std::min(k+5,wire.size());
   queueL1(b,{wire.begin()+k,wire.begin()+end},end==wire.size()?1:8);}
  b.output.clear();};
 assert(i.getInverterDataCfl(0x51000200,1,1)==E_OK);assert(i.dispData.Pac==4567);b.sent=nullptr;
}
void testCflAttemptsAreTransactional(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 const int32_t savedPac=i.invData.Pac,savedFreq=i.invData.Freq;
 const float savedDisplayPac=i.dispData.Pac,savedDisplayFreq=i.dispData.Freq;
 const time_t savedLastTime=i.invData.LastTime;const E_RC savedStatus=i.invData.status;
 i.invData.Pac=111;i.dispData.Pac=111;i.invData.Freq=900;i.dispData.Freq=9;
 i.invData.LastTime=123;i.invData.status=E_OK;

 b.input.clear();b.output.clear();
 b.sent=[&]{auto first=response(i,numericRecord(GridMsTotW,1,2222));put(first,25,1,2);
  queueResponse(b,first);queueResponse(b,response(i,numericRecord(GridMsHz,1,5000)),true);b.output.clear();};
 assert(i.getInverterDataCfl(0x51000200,1,1)==E_CHKSUM);b.sent=nullptr;
 assert(i.invData.Pac==111&&i.dispData.Pac==111);assert(i.invData.LastTime==123&&i.invData.status==E_OK);

 unsigned attempt=0;b.input.clear();b.output.clear();
 b.sent=[&]{++attempt;
  if(attempt==1){auto first=response(i,numericRecord(GridMsTotW,1,3333));put(first,25,1,2);
   queueResponse(b,first);queueResponse(b,response(i,numericRecord(GridMsHz,1,6000)),true);}
  else queueResponse(b,response(i,numericRecord(GridMsHz,1,5000)));
  b.output.clear();};
 assert(i.getInverterData(SpotACTotalPower)==E_OK);b.sent=nullptr;assert(attempt==2);
 // The successful retry omits Pac, so the rejected first attempt's Pac is not retained.
 assert(i.invData.Pac==111&&i.dispData.Pac==111);assert(i.invData.LastTime==123);
 assert(i.invData.Freq==5000&&i.dispData.Freq==50);

 b.input.clear();b.output.clear();
 b.sent=[&]{auto first=response(i,numericRecord(GridMsTotW,1,4444));put(first,25,1,2);
  queueResponse(b,first);queueResponse(b,response(i,numericRecord(GridMsHz,1,5100)));b.output.clear();};
 assert(i.getInverterDataCfl(0x51000200,1,1)==E_OK);b.sent=nullptr;
 assert(i.invData.Pac==4444&&i.dispData.Pac==4444);
 assert(i.invData.Freq==5100&&i.dispData.Freq==51);
 i.invData.Pac=savedPac;i.dispData.Pac=savedDisplayPac;i.invData.Freq=savedFreq;i.dispData.Freq=savedDisplayFreq;
 i.invData.LastTime=savedLastTime;i.invData.status=savedStatus;
}
void testBluetoothWriteFailureAndSignalValidity(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;b.input.clear();b.output.clear();b.writeFails=true;
 assert(i.getInverterDataCfl(0x51000200,1,1)==E_NODATA);assert(b.output.empty());
 i.dispData.BTSigStrength=70;assert(!i.getBT_SignalStrength());assert(std::isnan(i.dispData.BTSigStrength));
 b.writeFails=false;
}

void testInitReplyAndTrailerBounds(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 for(size_t length:{size_t(61),size_t(62),size_t(63),size_t(64)}){
  b.input.clear();b.output.clear();queueL1(b,{0,4,0x70,0,1},2);int step=0;
  b.sent=[&]{if(step++==0)queueL1(b,std::vector<uint8_t>(14),5);else{
   std::vector<uint8_t> data(20);put(data,16,123);auto stale=response(i,data);
   put(stale,29,0x51000201);queueResponse(b,stale);data.resize(length-44);
   auto actual=response(i,data);put(actual,29,0x00000201);queueResponse(b,actual);}
   b.output.clear();};
  auto serial=i.invData.Serial;
  assert(i.initialiseSMAConnection()==(length==64?E_OK:E_INVRESP));
  assert(i.invData.Serial==(length==64?123:serial));b.sent=nullptr;
 }
 i.invData.Serial=0;
}
void testInitRefreshesModelWhenSerialChanges(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 const auto savedIdentity=i.invData;const auto savedId=i.pcktID;
 const uint8_t target[]={0x21,0x32,0x43,0x54,0x65,0x76};
 std::copy(target,target+6,i.invData.BTAddress);
 i.invData.SUSyID=0x1111;i.invData.Serial=111;

 auto initialize=[&](uint32_t serial){
  b.input.clear();b.output.clear();queueL1(b,{0,4,0x70,0,1},2,target);int step=0;
  b.sent=[&]{if(step++==0)queueL1(b,std::vector<uint8_t>(14),5,target);else{
   std::vector<uint8_t> data(20);put(data,16,serial);
   auto reply=response(i,data);put(reply,29,0x00000201);queueResponse(b,reply);
  }b.output.clear();};
  const auto rc=i.initialiseSMAConnection();b.sent=nullptr;return rc;
 };

 assert(initialize(222)==E_OK);
 assert(i.invData.Serial==222&&i.invData.SUSyID==0x007D);
 b.input.clear();b.output.clear();
 b.sent=[&]{queueResponse(b,loginResponse(i,0x2222,222));b.output.clear();};
 assert(i.logonSMAInverter("0000",USERGROUP)==E_OK);
 assert(i.invData.Serial==222&&i.invData.SUSyID==0x2222);

 // A repeated initialization for the same serial must retain the learned model
 // and reject an otherwise-correlated login reply from another model.
 assert(initialize(222)==E_OK);
 assert(i.invData.Serial==222&&i.invData.SUSyID==0x2222);
 b.input.clear();b.output.clear();
 b.sent=[&]{queueResponse(b,loginResponse(i,0x3333,222));
  queueResponse(b,loginResponse(i,0x2222,222));b.output.clear();};
 assert(i.logonSMAInverter("0000",USERGROUP)==E_OK);
 assert(i.invData.Serial==222&&i.invData.SUSyID==0x2222);assert(b.input.empty());
 b.sent=nullptr;b.input.clear();b.output.clear();i.invData=savedIdentity;i.pcktID=savedId;
}
void testClockExpiresDuringRead(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;i.btConnected=true;i.invData.Serial=55;
 i.invData.SUSyID=0x1234;i.invData.BTAddress[0]=0x20;
 b.input.clear();b.output.clear();unsigned sends=0;uint32_t deadline=millis()+1000;
 b.sent=[&]{++sends;std::vector<uint8_t> data(24);put(data,0,0x00236d00);put(data,4,1800000000);
  put(data,8,1790000000);put(data,16,36000);put(data,20,7);auto v=response(i,data);put(v,29,0xf000020b);
  fake::ticks+=1500;queueResponse(b,v);b.output.clear();};
 int32_t before=0,after=0;assert(i.syncPlantTime(36000,&before,&after,&deadline)==E_EXPIRED);assert(sends==1);
 b.sent=nullptr;i.btConnected=false;i.invData.Serial=0;i.invData.BTAddress[0]=0;
}
void testConnectRetainsEarlyHandshakeAndCleansFailedSession(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 const auto savedIdentity=i.invData;const auto savedId=i.pcktID;
 const bool savedConnectResult=b.connectResult;auto savedSent=b.sent;auto savedDuringConnect=b.duringConnect;
 uint8_t target[]={0x20,0x30,0x40,0x50,0x60,0x70};
 std::copy(target,target+6,i.invData.BTAddress);i.invData.SUSyID=0x007d;i.invData.Serial=0;i.pcktID=1;
 b.input.clear();b.output.clear();b.connectResult=true;assert(i.begin("early-handshake-regression",true));

 bool injectedBeforeConnectReturned=false;int step=0;
 b.duringConnect=[&]{
  const std::vector<uint8_t> initPayload={0,4,0x70,0,1};
  queueL1(b,initPayload,2,i.invData.BTAddress);
  injectedBeforeConnectReturned=true;
 };
 b.sent=[&]{
  if(step++==0) queueL1(b,std::vector<uint8_t>(14),5,i.invData.BTAddress);
  else {
   std::vector<uint8_t> data(20);put(data,16,123);auto reply=response(i,data);
   put(reply,29,0x00000201);queueResponse(b,reply);
  }
  b.output.clear();
 };
 assert(i.connect(target));assert(injectedBeforeConnectReturned);
 assert(i.initialiseSMAConnection()==E_OK);assert(i.invData.Serial==123);b.sent=nullptr;

 // Bytes left from an old session must be removed before the next attempt.
 const uint8_t oldSessionByte=0x31,newSessionByte=0x52;
 b.inject(&oldSessionByte,1);
 b.duringConnect=[&]{b.inject(&newSessionByte,1);};
 assert(i.connect(target));assert(i.BTgetByte()==newSessionByte);

 // Data received during a failed connection attempt must not seed a retry.
 const uint8_t failedAttemptByte=0x61,retryByte=0x72;
 b.connectResult=false;b.duringConnect=[&]{b.inject(&failedAttemptByte,1);};
 assert(!i.connect(target));
 b.connectResult=true;b.duringConnect=[&]{b.inject(&retryByte,1);};
 assert(i.connect(target));assert(i.BTgetByte()==retryByte);

 i.disconnect();b.duringConnect=savedDuringConnect;b.sent=savedSent;b.connectResult=savedConnectResult;
 i.invData=savedIdentity;i.pcktID=savedId;
}
static unsigned primaryGapCallbackCalls=0, replacementGapCallbackCalls=0;
static void primaryGapCallback(esp_bt_gap_cb_event_t,esp_bt_gap_cb_param_t*){++primaryGapCallbackCalls;}
static void replacementGapCallback(esp_bt_gap_cb_event_t,esp_bt_gap_cb_param_t*){++replacementGapCallbackCalls;}
void testBluetoothAuthRecoveryMatchesTargetPeer(){
 auto&i=ESP32_SMA_Inverter::getInstance();auto&b=i.serialBT;
 const auto savedIdentity=i.invData;const auto savedId=i.pcktID;
 const bool savedConnectResult=b.connectResult;auto savedDuringConnect=b.duringConnect;
 const unsigned savedUnpairs=b.unpairCalls;
 uint8_t target[]={0x20,0x30,0x40,0x50,0x60,0x70};
 const uint8_t other[]={0x90,0x80,0x70,0x60,0x50,0x40};
 fake_gap_sdk::reset();primaryGapCallbackCalls=replacementGapCallbackCalls=0;

 // The production wrapper registers itself with the fake SDK in another
 // translation unit and forwards all GAP events to the original core handler.
 fake_gap_sdk::dispatchAuthDuringNextRegistration(target,ESP_BT_STATUS_SUCCESS);
 assert(__wrap_esp_bt_gap_register_callback(primaryGapCallback)==ESP_OK);
 fake_gap_sdk::waitForRegistrationDispatch();
 assert(fake_gap_sdk::registrationCalls()==1);
 assert(fake_gap_sdk::registeredCallback()!=nullptr);
 assert(fake_gap_sdk::registeredCallback()!=primaryGapCallback);
 const auto installedGapProxy=fake_gap_sdk::registeredCallback();
 assert(primaryGapCallbackCalls==1); // event delivered at the install boundary
 assert(__wrap_esp_bt_gap_register_callback(nullptr)==FAKE_NULL_GAP_CALLBACK_ERROR);
 assert(fake_gap_sdk::registrationCalls()==2);
 assert(fake_gap_sdk::registeredCallback()==installedGapProxy);
 fake_gap_sdk::dispatchEvent(ESP_BT_GAP_DISC_STATE_CHANGED_EVT);
 assert(primaryGapCallbackCalls==2); // SDK rejection preserves its prior callback
 fake_gap_sdk::setNextRegistrationResult(-7);
 assert(__wrap_esp_bt_gap_register_callback(replacementGapCallback)==-7);
 assert(fake_gap_sdk::registrationCalls()==3);
 assert(fake_gap_sdk::registeredCallback()==installedGapProxy);
 fake_gap_sdk::dispatchEvent(ESP_BT_GAP_DISC_STATE_CHANGED_EVT);
 assert(primaryGapCallbackCalls==3&&replacementGapCallbackCalls==0);

 assert(i.begin("auth-peer-regression",true));
 b.connectResult=false;b.duringConnect=nullptr;
 const unsigned initialUnpairs=b.unpairCalls;

 // A failed auth event outside a connect attempt is ignored.
 fake_gap_sdk::dispatchAuth(target,1);
 assert(!i.connect(target));
 assert(b.unpairCalls==initialUnpairs&&!i.takeReconnectRequest());

 // Another peer's failed pairing during the outgoing attempt must not be
 // mistaken for an inverter failure, even though the connect also fails.
 b.duringConnect=[&]{fake_gap_sdk::dispatchAuth(other,1);};
 assert(!i.connect(target));
 assert(b.unpairCalls==initialUnpairs&&!i.takeReconnectRequest());

 // Successful auth events are not recovery triggers. A success from another
 // peer after a matching success remains unrelated to the target.
 b.duringConnect=[&]{
  fake_gap_sdk::dispatchAuth(target,ESP_BT_STATUS_SUCCESS);
  fake_gap_sdk::dispatchAuth(other,ESP_BT_STATUS_SUCCESS);
 };
 assert(!i.connect(target));
 assert(b.unpairCalls==initialUnpairs&&!i.takeReconnectRequest());

 // A matching target failure is latched even if an unrelated success follows;
 // the existing one-time recovery removes the target bond and requests retry.
 b.duringConnect=[&]{
  fake_gap_sdk::dispatchAuth(target,1);
  fake_gap_sdk::dispatchAuth(other,ESP_BT_STATUS_SUCCESS);
 };
 assert(!i.connect(target));
 assert(b.unpairCalls==initialUnpairs+1&&i.takeReconnectRequest());

 // Later matching failures do not repeat the existing one-time unpair.
 b.duringConnect=[&]{fake_gap_sdk::dispatchAuth(target,1);};
 assert(!i.connect(target));
 assert(b.unpairCalls==initialUnpairs+1&&!i.takeReconnectRequest());
 assert(primaryGapCallbackCalls==10&&replacementGapCallbackCalls==0);

 i.disconnect();b.duringConnect=savedDuringConnect;b.connectResult=savedConnectResult;
 i.invData=savedIdentity;i.pcktID=savedId;
}








int main(){
 testClockReplyCorrelation();
 testClockTargetsOneInverter();
 testLoginAndInitCrc();
 testLoginFiltersSenderAndKnownSerial();
 testLoginBoundsUnrelatedReplies();
 testSenderAddressMatching();
 testLogoffRetriesCrcCollision();
 testUnsupportedTemperature();
 testDcChannels();
 testStatusRecordBounds();
 testMalformedRecordRanges();
 testFragmentEscapes();
 testSlowPacketDeadline();
 testCallerOperationDeadlinesAndRollover();
 testBluetoothTimerRollover();
 testStaleErrorIsIgnored();
 testMalformedPacketClearsSession();
 testManyValidFragments();
 testCflAttemptsAreTransactional();
 testBluetoothWriteFailureAndSignalValidity();
 testInitReplyAndTrailerBounds();
 testInitRefreshesModelWhenSerialChanges();
 testClockExpiresDuringRead();
 testConnectRetainsEarlyHandshakeAndCleansFailedSession();
 testBluetoothAuthRecoveryMatchesTargetPeer();
 InverterData identity{}; identity.SUSyID=0x1234; assert(identity.SUSyID==0x1234);
 uint8_t bytes[]={0x78,0x56,0x34,0x12,0,0,0,0};
 assert(get_u16(bytes)==0x5678);
 assert(get_u32(bytes)==0x12345678);
 assert(get_u64(bytes)==0x12345678);
 std::cout << "Host regression checks passed\n";
}
