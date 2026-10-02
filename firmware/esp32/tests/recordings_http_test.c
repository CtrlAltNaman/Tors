#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/recordings_http.c"
static int pins;
static bool read_fail;
static recording_info_t clip={.id=7,.stream_id=1,.pcm_bytes=640,.reason=REC_SILENCE};
void local_recordings_get_stats(local_recordings_stats_t *out) { *out=(local_recordings_stats_t){.enabled=true,.status="ready"}; }
unsigned local_recordings_list(recording_info_t out[REC_KEEP_COUNT]) { out[0]=clip; return 1; }
int local_recordings_open(uint32_t id,recording_info_t *out) { if(id!=7)return -1; ++pins; *out=clip; return 0; }
void local_recordings_close(int slot) { assert(slot==0 && pins==1); --pins; }
esp_err_t local_recordings_read(int slot,uint32_t offset,void *data,size_t n) {
    assert(slot==0 && pins==1 && offset+n<=640);
    if(read_fail) return ESP_FAIL;
    for(size_t i=0;i<n;++i) ((uint8_t*)data)[i]=(uint8_t)(offset+i);
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r,const char *s) { strcpy(r->type,s); return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *r,const char *s) { r->status=atoi(s); return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r,const char *name,const char *value) {
    if(!strcmp(name,"Content-Range")) strcpy(r->content_range,value);
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *r,const char *data,ssize_t n) {
    if(r->fail) return ESP_FAIL;
    assert(n>=0 && r->size+(size_t)n<sizeof(r->body));
    if(n) memcpy(r->body+r->size,data,(size_t)n);
    r->size+=(size_t)n; r->body[r->size]=0; return ESP_OK;
}
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r,const char *s) { return httpd_resp_send_chunk(r,s,s?strlen(s):0); }
esp_err_t httpd_resp_sendstr(httpd_req_t *r,const char *s) { return httpd_resp_sendstr_chunk(r,s); }
esp_err_t httpd_resp_send_err(httpd_req_t *r,int code,const char *s) { r->status=code; return httpd_resp_sendstr(r,s); }
size_t httpd_req_get_hdr_value_len(httpd_req_t *r,const char *name) { assert(!strcmp(name,"Range")); return r->range?strlen(r->range):0; }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r,const char *name,char *out,size_t size) {
    (void)name; assert(r->range && strlen(r->range)<size); strcpy(out,r->range); return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t server,const httpd_uri_t *route) { (void)server; assert(route->handler && route->uri); return ESP_OK; }
int main(void) {
    uint8_t expected[684]; recording_wav_header(expected,640);
    for(unsigned i=0;i<640;++i) expected[44+i]=(uint8_t)i;
    httpd_req_t r={.uri="/recordings/7.wav",.status=200};
    assert(wav_handler(&r)==ESP_OK && pins==0 && r.size==684 && !memcmp(r.body,expected,684));
    assert(!strcmp(r.type,"audio/wav"));
    r=(httpd_req_t){.uri="/recordings/7.wav",.range="bytes=40-53"};
    assert(wav_handler(&r)==ESP_OK && pins==0 && r.status==206 && r.size==14);
    assert(!memcmp(r.body,expected+40,14) && !strcmp(r.content_range,"bytes 40-53/684"));
    r=(httpd_req_t){.uri="/recordings/7.wav",.range="bytes=-2"};
    assert(wav_handler(&r)==ESP_OK && pins==0 && r.size==2 && !memcmp(r.body,expected+682,2));
    r=(httpd_req_t){.uri="/recordings/7.wav",.range="bytes=900-"};
    assert(wav_handler(&r)==ESP_OK && pins==0 && r.status==416);
    r=(httpd_req_t){.uri="/recordings/8.wav"};
    assert(wav_handler(&r)==ESP_OK && pins==0 && r.status==404);
    r=(httpd_req_t){.uri="/recordings/../7.wav"};
    assert(wav_handler(&r)==ESP_OK && r.status==404);
    r=(httpd_req_t){.uri="/recordings/7.wav",.fail=true};
    assert(wav_handler(&r)==ESP_FAIL && pins==0);
    r=(httpd_req_t){.uri="/recordings/7.wav"}; read_fail=true;
    assert(wav_handler(&r)==ESP_FAIL && pins==0); read_fail=false;
    r=(httpd_req_t){.uri="/recordings"};
    assert(index_handler(&r)==ESP_OK && strstr(r.body,"/recordings/7.wav") && strstr(r.body,"audio controls"));
    assert(recordings_http_register(NULL)==ESP_OK);
    puts("Recording HTTP: WAV body, cross-header ranges, 404/416, playback list and failure cleanup passed");
}
