// Execute the real recorder with an in-memory NOR-flash adapter. Scheduling is
// stepped explicitly; this tests persistence/retention/failures, not RTOS timing.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../main/local_recordings.c"
static uint8_t flash[REC_SLOT_COUNT*REC_SLOT_BYTES];
static esp_partition_t fake_partition={sizeof(flash),false};
static unsigned writes,erases,frame_count=7;
static bool gap,write_fail;
const esp_partition_t *esp_partition_find_first(int t,int s,const char *label) {
    assert(t==1 && s==255 && !strcmp(label,"storage")); return &fake_partition;
}
esp_err_t esp_partition_read(const esp_partition_t *p,size_t off,void *out,size_t n) {
    assert(p==&fake_partition && off+n<=sizeof(flash)); memcpy(out,flash+off,n); return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *p,size_t off,const void *in,size_t n) {
    assert(p==&fake_partition && off+n<=sizeof(flash));
    if(write_fail) return ESP_FAIL;
    const uint8_t *data=in;
    for(size_t i=0;i<n;++i) { assert((flash[off+i]&data[i])==data[i]); flash[off+i]&=data[i]; }
    ++writes; return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p,size_t off,size_t n) {
    assert(p==&fake_partition && off+n<=sizeof(flash) && off%4096==0 && n==4096);
    memset(flash+off,255,n); ++erases; return ESP_OK;
}
int xTaskCreate(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *handle) {
    (void)fn;(void)name;(void)arg; assert(stack==4096 && priority==1); *handle=(void *)1; return pdPASS;
}
void vTaskDelay(unsigned ticks) { (void)ticks; }
void xTaskNotifyGive(TaskHandle_t handle) { assert(handle==(void *)1); }
unsigned ulTaskNotifyTake(int clear,unsigned ticks) { (void)clear;(void)ticks; return 0; }
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t handle) { assert(handle==(void *)1); return 1024; }
void device_diagnostics_event(esp_log_level_t level,const char *format,...) { (void)level;(void)format; }
static recording_frame_result_t capture(uint32_t stream,uint64_t index,int16_t samples[320]) {
    (void)stream;
    if(index>=frame_count) return gap?REC_FRAME_LOST:REC_FRAME_END;
    for(int i=0;i<320;++i) samples[i]=(int16_t)(index*320+i);
    return REC_FRAME_DATA;
}
static void reboot(void) {
    memset(slots,0,sizeof(slots)); stats=(local_recordings_stats_t){.status="not_initialized"};
    ready_slot=active_slot=-1; next_id=1; worker=NULL; maintenance_allowed=true;
}
static void record(unsigned stream) {
    prepare_slot(); assert(local_recordings_begin(stream,0));
    record_slot(active_slot,stream,0);
}
int main(void) {
    memset(flash,255,sizeof(flash));
    assert(local_recordings_init(capture)==ESP_OK); prepare_slot();
    assert(!local_recordings_busy() && stats.ready);
    record(1); record(2); record(3);
    recording_info_t clips[3],info;
    assert(local_recordings_list(clips)==3 && clips[0].id==3 && clips[2].id==1);
    int pinned=local_recordings_open(1,&info); assert(pinned>=0);
    record(4);
    assert(local_recordings_list(clips)==3 && clips[0].id==4 && clips[2].id==2);
    assert(local_recordings_open(1,&info)<0); // Expired for new readers, old reader still valid.
    unsigned before=erases; prepare_slot(); assert(erases==before && !stats.ready);
    int16_t samples[320]; assert(local_recordings_read(pinned,0,samples,sizeof(samples))==ESP_OK);
    assert(samples[1]==1 && samples[319]==319);
    assert(local_recordings_read(pinned,info.pcm_bytes,samples,1)==ESP_ERR_INVALID_ARG);
    local_recordings_close(pinned); prepare_slot(); assert(erases==before+256 && stats.ready);
    reboot(); assert(local_recordings_init(capture)==ESP_OK);
    assert(local_recordings_list(clips)==3 && clips[0].id==4 && next_id==5);
    frame_count=1600; record(5);
    assert(local_recordings_list(clips)==3 && clips[0].id==5);
    assert(clips[0].pcm_bytes==REC_MAX_PCM_BYTES && clips[0].reason==REC_LIMIT);
    assert(!strcmp(stats.status,"saved") && !stats.active);
    frame_count=3; gap=true; record(6);
    assert(local_recordings_list(clips)==3 && clips[0].reason==REC_GAP && clips[0].pcm_bytes==1920);
    local_recordings_allow_maintenance(false); before=erases; prepare_slot(); assert(erases==before);
    local_recordings_allow_maintenance(true); prepare_slot();
    write_fail=true; assert(local_recordings_begin(7,0)); record_slot(active_slot,7,0);
    assert(!stats.enabled && !stats.active); write_fail=false;
    reboot(); assert(local_recordings_init(capture)==ESP_OK);
    assert(local_recordings_list(clips)==3 && clips[0].id==6); // Previous committed clips survive failure.
    // Recognized interrupted recording: no committed header, safely reclaimable.
    prepare_slot(); int spare=ready_slot; memcpy(flash+base(spare),owner,sizeof(owner));
    flash[base(spare)+REC_DATA_OFFSET]=0;
    reboot(); assert(local_recordings_init(capture)==ESP_OK); prepare_slot(); assert(stats.ready);
    // An unrecognized partition must never be formatted or erased.
    memset(flash,255,sizeof(flash)); flash[256]=0; reboot(); before=erases; unsigned old_writes=writes;
    assert(local_recordings_init(capture)!=ESP_OK);
    assert(erases==before && writes==old_writes && !stats.enabled && flash[256]==0);
    puts("Recorder: retention, pinned reads, cap, gaps, reboot, interrupted writes and unknown-data preservation passed");
}
