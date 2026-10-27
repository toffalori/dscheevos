/* Wi-Fi (the console's saved access points) and a small HTTP/1.1 client. */
#ifndef DSC_NET_H
#define DSC_NET_H

#include <stddef.h>
#include <stdint.h>

/* Connects with the access points saved in the console's settings. Uses the
 * DSi's own Wi-Fi (WPA2) when available. Returns 0 when online. Calls
 * each_frame (if any) once per frame while it waits. */
int net_connect(int timeout_frames, void (*each_frame)(void));
const char* net_error(void);

/* The access point we joined, as seen in its beacons (no keys). */
typedef struct net_ap_t {
  char ssid[32];
  unsigned char ssid_len;
  unsigned char bssid[6];
  unsigned char channel;
  unsigned char security;               /* dswifi Wifi_ApSecurityType */
  unsigned char pair_cipher, group_cipher;  /* dswifi Wifi_ApCryptType */
} net_ap_t;
int net_current_ap(net_ap_t* out);      /* 0 if found */

typedef struct http_response_t {
  int status;           /* HTTP status, or < 0 on a network error */
  char* body;           /* NUL-terminated, malloc'd; free with http_free */
  size_t length;
} http_response_t;

void http_set_user_agent(const char* user_agent);
/* The console clock minus UTC in quarter hours, from the last server reply's
 * Date header. Returns 0 if no reply had one (or the clock is off by more
 * than any time zone). */
int net_clock_offset_q(int8_t* offset_q);
/* POST (or GET when post is NULL). http:// and https:// URLs. */
void http_request(const char* url, const char* post, const char* content_type, http_response_t* out);
void http_free(http_response_t* r);

#endif
