#include "recordings_http.h"
#include "local_recordings.h"
#include <stdio.h>
#include <string.h>
static void headers(httpd_req_t *r,const char *type) {
    httpd_resp_set_type(r,type);
    httpd_resp_set_hdr(r,"Cache-Control","no-store");
    httpd_resp_set_hdr(r,"X-Content-Type-Options","nosniff");
}
static esp_err_t index_handler(httpd_req_t *r) {
    headers(r,"text/html; charset=utf-8");
    local_recordings_stats_t stats; local_recordings_get_stats(&stats);
    recording_info_t clips[REC_KEEP_COUNT]; unsigned count=local_recordings_list(clips);
    if(httpd_resp_sendstr_chunk(r,"<!doctype html><html lang=en><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'><title>ESP32 recordings</title>"
        "<style>body{font:16px system-ui;margin:24px;background:#101820;color:#eee}a{color:#8df}"
        "article{padding:16px 0;border-top:1px solid #567}audio{display:block;margin:10px 0}</style>"
        "<h1>Microphone recordings</h1><p><a href='/'>Device status</a> | <a href='/recordings'>Refresh list</a></p>"
        "<p>Latest three KWS recordings; 16 kHz mono PCM16 WAV; max 30 seconds including prebuffer. "
        "Completed clips survive reboot. New recordings replace the oldest. Trusted LAN only: no authentication.</p>")!=ESP_OK) return ESP_FAIL;
    char line[640];
    snprintf(line,sizeof(line),"<p>Storage: %s | current/last PCM bytes: %lu | errors: %lu</p>",
             stats.status,(unsigned long)stats.pcm_bytes,(unsigned long)stats.errors);
    if(httpd_resp_sendstr_chunk(r,line)!=ESP_OK) return ESP_FAIL;
    for(unsigned i=0;i<count;++i) {
        recording_info_t *v=&clips[i];
        snprintf(line,sizeof(line),"<article><h2>Recording %lu</h2><p>%.2f seconds | stream %lu | ended: %s | PCM CRC32: %08lx</p>"
            "<audio controls preload='none' src='/recordings/%lu.wav'></audio>"
            "<a download='recording-%lu.wav' href='/recordings/%lu.wav'>Download WAV</a></article>",
            (unsigned long)v->id,v->pcm_bytes/32000.0,(unsigned long)v->stream_id,recording_reason_name(v->reason),
            (unsigned long)v->pcm_crc,(unsigned long)v->id,(unsigned long)v->id,(unsigned long)v->id);
        if(httpd_resp_sendstr_chunk(r,line)!=ESP_OK) return ESP_FAIL;
    }
    if(!count && httpd_resp_sendstr_chunk(r,"<p>No completed recordings yet. Say Hello Tors, speak, then return to ambient noise.</p>")!=ESP_OK) return ESP_FAIL;
    if(httpd_resp_sendstr_chunk(r,"<p>capture_gap means the saved clip ended early: inspect HEALTH counters. "
        "30s_limit is the local cap, not a backend stream limit. Refresh after recording finishes.</p></html>")!=ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(r,NULL,0);
}
static esp_err_t wav_handler(httpd_req_t *r) {
    uint32_t id;
    if(!recording_parse_path(r->uri,&id)) return httpd_resp_send_err(r,HTTPD_404_NOT_FOUND,"Unknown recording");
    recording_info_t info;
    int slot=local_recordings_open(id,&info);
    if(slot<0) return httpd_resp_send_err(r,HTTPD_404_NOT_FOUND,"Recording expired or not completed");
    char range[80]; const char *range_value=NULL;
    size_t range_length=httpd_req_get_hdr_value_len(r,"Range");
    bool range_ok=range_length<sizeof(range);
    if(range_length && range_ok) {
        range_ok=httpd_req_get_hdr_value_str(r,"Range",range,sizeof(range))==ESP_OK;
        range_value=range;
    }
    uint32_t first=0,last=0,total=44+info.pcm_bytes;
    if(!range_ok || !recording_parse_range(range_value,total,&first,&last)) {
        local_recordings_close(slot);
        char value[48]; snprintf(value,sizeof(value),"bytes */%lu",(unsigned long)total);
        httpd_resp_set_status(r,"416 Range Not Satisfiable"); httpd_resp_set_hdr(r,"Content-Range",value);
        return httpd_resp_sendstr(r,"Invalid byte range");
    }
    headers(r,"audio/wav"); httpd_resp_set_hdr(r,"Accept-Ranges","bytes");
    char content_range[80], disposition[80];
    if(range_value) {
        httpd_resp_set_status(r,"206 Partial Content");
        snprintf(content_range,sizeof(content_range),"bytes %lu-%lu/%lu",(unsigned long)first,(unsigned long)last,(unsigned long)total);
        httpd_resp_set_hdr(r,"Content-Range",content_range);
    }
    snprintf(disposition,sizeof(disposition),"inline; filename=recording-%lu.wav",(unsigned long)id);
    httpd_resp_set_hdr(r,"Content-Disposition",disposition);
    uint8_t wav[44],buffer[1024]; recording_wav_header(wav,info.pcm_bytes);
    esp_err_t err=ESP_OK;
    for(uint32_t pos=first;pos<=last && err==ESP_OK;) {
        size_t bytes=last-pos+1;
        if(pos<44) {
            if(bytes>44-pos) bytes=44-pos;
            err=httpd_resp_send_chunk(r,(const char *)wav+pos,bytes);
        } else {
            if(bytes>sizeof(buffer)) bytes=sizeof(buffer);
            err=local_recordings_read(slot,pos-44,buffer,bytes);
            if(err==ESP_OK) err=httpd_resp_send_chunk(r,(const char *)buffer,bytes);
        }
        pos+=(uint32_t)bytes;
    }
    local_recordings_close(slot);
    return err==ESP_OK?httpd_resp_send_chunk(r,NULL,0):err;
}
esp_err_t recordings_http_register(httpd_handle_t server) {
    const httpd_uri_t list={.uri="/recordings",.method=HTTP_GET,.handler=index_handler};
    const httpd_uri_t audio={.uri="/recordings/*",.method=HTTP_GET,.handler=wav_handler};
    esp_err_t err=httpd_register_uri_handler(server,&list);
    return err==ESP_OK?httpd_register_uri_handler(server,&audio):err;
}
