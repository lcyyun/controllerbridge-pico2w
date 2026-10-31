#include "bridge_runtime.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
static uint8_t persisted[84];
static bool have_store=false, fail_store=false;
bool bridge_store_load(uint8_t *data,size_t n) { if(!have_store) return false; memcpy(data,persisted,n); return true; }
bool bridge_store_save(const uint8_t *data,size_t n) { if(fail_store) return false; memcpy(persisted,data,n); have_store=true; return true; }
static void timestamp(uint8_t *body,uint32_t value) { for(int i=0;i<4;++i) body[27+i]=value>>(8*i); }
static std::string status(uint32_t now) { char data[2048]; bridge_runtime_status(data,sizeof(data),now); return data; }
static bool contains(uint32_t now,const char *text) { return status(now).find(text)!=std::string::npos; }
int main() {
    bridge_runtime_init();
    assert(contains(0,"\"input_valid\":false"));
    assert(contains(0,"\"input_battery_percent\":-1"));
    uint8_t ds5[63]{}; ds5[0]=ds5[1]=ds5[2]=ds5[3]=128; ds5[7]=8; ds5[52]=5;
    timestamp(ds5,0xfffffffe); bridge_runtime_ds5(ds5,63,0xffffff00);
    assert(contains(0xffffff00,"\"unique_input_reports\":1"));
    bridge_runtime_ds5(ds5,63,0xffffff01);
    assert(contains(0xffffff01,"\"unique_input_reports\":1"));
    timestamp(ds5,0xffffffff); bridge_runtime_ds5(ds5,63,0xffffff02);
    assert(contains(0xffffff02,"\"unique_input_reports\":2")); // stationary source frame counts
    timestamp(ds5,0); bridge_runtime_ds5(ds5,63,0xfffffff0);
    assert(contains(0x20,"\"input_valid\":true")); // monotonic clock wrap
    assert(contains(0x100,"\"input_stale\":true"));
    assert(contains(0x100,"\"input_battery_percent\":-1"));
    char reply[2048];
    assert(bridge_runtime_mapping_command("mapping set ds5 south left_paddle",reply,sizeof(reply)));
    assert(strstr(reply,"\"ok\":true"));
    ds5[9]=0x40; timestamp(ds5,1); bridge_runtime_ds5(ds5,63,300);
    uint8_t report[64];
    assert(bridge_runtime_report(BL616_BRIDGE_ROLE_DUALSENSE_EDGE,report,sizeof(report),300)==64);
    assert(report[8]&0x20); assert(report[10]&0x40); // mapped South plus original Edge paddle
    assert(memcmp(report+16,ds5+15,12)==0); // native IMU unchanged
    assert(bridge_runtime_report(BL616_BRIDGE_ROLE_XBOX_360,report,sizeof(report),300)==20);
    assert(report[3]&0x10);
    bridge_runtime_set_role(4); bridge_runtime_set_preference(1); assert(bridge_runtime_save());
    bridge_runtime_init(); assert(bridge_runtime_role()==4 && bridge_runtime_preference()==1);
    assert(bridge_runtime_mapping_command("mapping get ds5",reply,sizeof(reply)) && strstr(reply,"left_paddle"));
    Ns2InputSnapshot ns2{};ns2.valid=true;ns2.source_sequence_valid=true;ns2.source_sequence=10;
    ns2.battery_percent=-1;ns2.lx=ns2.ly=ns2.rx=ns2.ry=2048;ns2.buttons=1;
    bridge_runtime_ns2(&ns2,400);assert(contains(400,"\"active_input\":\"none\"")); // preference enforced
    bridge_runtime_set_preference(0);bridge_runtime_ns2(&ns2,401);
    assert(contains(401,"\"active_input\":\"ns2pro\"")); assert(contains(401,"\"input_battery_percent\":-1"));
    assert(bridge_runtime_report(BL616_BRIDGE_ROLE_DUALSENSE,report,sizeof(report),401)==64 && (report[8]&0x20));
    assert(bridge_runtime_report(BL616_BRIDGE_ROLE_NS2PRO,report,sizeof(report),401)==64 && (report[5]&0x04));
    auto before=status(401); bridge_runtime_ns2(&ns2,402);assert(contains(402,"\"input_valid\":true"));
    ns2.source_sequence=11;bridge_runtime_ns2(&ns2,403);assert(contains(403,"\"input_rate_supported\":true"));
    assert(!bridge_runtime_live(654)); // duplicate reports never refresh input age
    assert(bridge_runtime_report(BL616_BRIDGE_ROLE_XBOX_360,report,sizeof(report),654)==20 && !report[3]);
    bridge_runtime_reset_input();assert(contains(655,"\"input_valid\":false"));
    bridge_runtime_ns2(&ns2,656);assert(bridge_runtime_live(656)); // identical first counter after reconnect accepted
    fail_store=true;assert(!bridge_runtime_save());fail_store=false;
    assert(bridge_runtime_mapping_command("mapping set ds5 c none",reply,sizeof(reply)));
    assert(bridge_runtime_mapping_command("mapping set ds5 invalid south",reply,sizeof(reply)) && strstr(reply,"\"ok\":false"));
    assert(!bridge_runtime_report(BL616_BRIDGE_ROLE_DUALSENSE,nullptr,64,656));
    assert(!bridge_runtime_report(BL616_BRIDGE_ROLE_DUALSENSE,report,63,656));
    assert(!bridge_runtime_report(BL616_BRIDGE_ROLE_UNKNOWN,report,64,656));
    puts("runtime telemetry, wrap, stale input, preference, cross-role encoding, native mapping and persistence: PASS");
}
