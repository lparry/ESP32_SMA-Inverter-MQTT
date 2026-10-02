#include "ESP32_SMA_Inverter_App.h"
#include <cassert>
#include <iostream>
int main(){
 uint8_t bytes[]={0x78,0x56,0x34,0x12,0,0,0,0};
 assert(get_u16(bytes)==0x5678);assert(get_u32(bytes)==0x12345678);assert(get_u64(bytes)==0x12345678);
 std::cout << "Host foundation checks passed\n";
}
