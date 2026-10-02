#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include "esp_err.h"
typedef void *httpd_handle_t;
typedef struct {
    const char *uri,*range;
    char body[8192],content_range[80],type[80];
    size_t size;
    int status;
    bool fail;
} httpd_req_t;
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
#define HTTP_GET 0
#define HTTPD_404_NOT_FOUND 404
esp_err_t httpd_resp_set_type(httpd_req_t *r,const char *value);
esp_err_t httpd_resp_set_status(httpd_req_t *r,const char *value);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r,const char *name,const char *value);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r,const char *data,ssize_t n);
esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r,const char *data);
esp_err_t httpd_resp_sendstr(httpd_req_t *r,const char *data);
esp_err_t httpd_resp_send_err(httpd_req_t *r,int code,const char *text);
size_t httpd_req_get_hdr_value_len(httpd_req_t *r,const char *name);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r,const char *name,char *out,size_t size);
esp_err_t httpd_register_uri_handler(httpd_handle_t server,const httpd_uri_t *route);
