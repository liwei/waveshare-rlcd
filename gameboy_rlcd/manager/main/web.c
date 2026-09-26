// WiFi ROM manager: the access point, the HTTP server and the page it serves.
//
// The device is its own network rather than a client of yours, so there are no
// credentials to store and it works with the SD card out of the device. In
// exchange, whoever wants the page steps off their own network to reach it, and
// the WPA2 passphrase on this access point is the only gate on a page that can
// delete files.
#include "web.h"

#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rom_files.h"

static const char *TAG = "web";

#define AP_SSID "gameboy-rlcd"
#define AP_PASSWORD "gameboy1234"
#define AP_CHANNEL 1
#define AP_MAX_CLIENTS 2

// The address esp_netif gives the access point, unless something changes it.
#define AP_ADDRESS "192.168.4.1"

static bool s_stack_ready; /* esp_netif and the event loop, done once */
static bool s_handler_ready;
static bool s_wifi_ready;  /* esp_wifi_init, undone by web_stop */
static bool s_running;
static int s_clients;
static esp_netif_t *s_ap;
static httpd_handle_t s_server;

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  (void)base;
  (void)data;

  if (id == WIFI_EVENT_AP_STACONNECTED) {
    s_clients++;
  } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
    if (s_clients > 0) {
      s_clients--;
    }
  }
}

// esp_netif and the default event loop are process-wide, and something else may
// already have made the loop.
static bool ensure_stack(void) {
  if (s_stack_ready) {
    return true;
  }

  esp_err_t err = esp_netif_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
    return false;
  }

  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
    return false;
  }

  s_stack_ready = true;
  return true;
}

static bool start_ap(void) {
  if (!ensure_stack()) {
    return false;
  }

  if (!s_wifi_ready) {
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
      return false;
    }
    s_wifi_ready = true;
  }

  if (s_ap == NULL) {
    s_ap = esp_netif_create_default_wifi_ap();
    if (s_ap == NULL) {
      ESP_LOGE(TAG, "no AP netif");
      return false;
    }
  }

  if (!s_handler_ready) {
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event,
                                                        NULL, NULL));
    s_handler_ready = true;
  }

  wifi_config_t config = {
      .ap =
          {
              .ssid = AP_SSID,
              .ssid_len = strlen(AP_SSID),
              .password = AP_PASSWORD,
              .channel = AP_CHANNEL,
              .max_connection = AP_MAX_CLIENTS,
              .authmode = WIFI_AUTH_WPA2_PSK,
          },
  };
  if (strlen(AP_PASSWORD) == 0) {
    config.ap.authmode = WIFI_AUTH_OPEN;
  }

  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
  if (err == ESP_OK) {
    err = esp_wifi_set_config(WIFI_IF_AP, &config);
  }
  if (err == ESP_OK) {
    err = esp_wifi_start();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "AP start: %s", esp_err_to_name(err));
    return false;
  }

  s_clients = 0;
  return true;
}

static void stop_ap(void) {
  esp_wifi_stop();
  if (s_ap != NULL) {
    esp_netif_destroy_default_wifi(s_ap);
    s_ap = NULL;
  }

  // Deinit rather than leave the driver resident: esp_wifi_init's own
  // allocations are about 60 KB of internal RAM, and leaving them behind would
  // make this mode cost that much for the rest of the session - a device that
  // ran the ROM manager once would play with visibly less headroom.
  if (s_wifi_ready) {
    esp_wifi_deinit();
    s_wifi_ready = false;
  }
}

/* ------------------------------------------------------------- the page --- */

extern const uint8_t rommanager_html_start[] asm("_binary_rommanager_html_start");
extern const uint8_t rommanager_html_end[] asm("_binary_rommanager_html_end");

static esp_err_t page_get(httpd_req_t *req) {
  const size_t len = (size_t)(rommanager_html_end - rommanager_html_start);

  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)rommanager_html_start, (ssize_t)len);
}

/* ------------------------------------------------------------------ api --- */

#define API_ROMS "/api/roms/"
#define UPLOAD_CHUNK (8 * 1024)

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Decodes a percent-encoded component in place. Names arrive UTF-8 encoded and
// go straight to FATFS, which is configured for UTF-8, so a Chinese title
// survives the round trip.
static void url_decode(char *text) {
  char *out = text;

  while (*text != '\0') {
    if (text[0] == '%' && text[1] != '\0' && text[2] != '\0') {
      const int high = hex_digit(text[1]);
      const int low = hex_digit(text[2]);
      if (high >= 0 && low >= 0) {
        *out++ = (char)((high << 4) | low);
        text += 3;
        continue;
      }
    }
    *out++ = *text++;
  }
  *out = '\0';
}

// The name from the request URI, decoded. False when it is absent or too long.
static bool request_name(httpd_req_t *req, char *out, size_t out_size) {
  const char *raw = req->uri + strlen(API_ROMS);
  if (*raw == '\0' || strlen(raw) >= out_size) {
    return false;
  }

  strlcpy(out, raw, out_size);
  url_decode(out);
  // A decoded name may not contain a query string or fragment.
  char *stop = strpbrk(out, "?#");
  if (stop != NULL) {
    *stop = '\0';
  }
  return out[0] != '\0';
}

static esp_err_t roms_get(httpd_req_t *req) {
  const int max = 96;
  rom_entry_t *entries = heap_caps_calloc((size_t)max, sizeof(rom_entry_t), MALLOC_CAP_SPIRAM);
  if (entries == NULL) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
  }

  const int count = rom_files_list(entries, max);

  uint64_t total = 0;
  uint64_t free_bytes = 0;
  rom_files_space(&total, &free_bytes);

  // Built by hand rather than with a JSON library: the shape is fixed.
  const size_t cap = 64u + (size_t)count * (sizeof(entries[0].name) + 128u);
  char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (body == NULL) {
    heap_caps_free(entries);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
  }

  size_t n = 0;
  n += (size_t)snprintf(body + n, cap - n, "{\"total\":%llu,\"free\":%llu,\"roms\":[",
                        (unsigned long long)total, (unsigned long long)free_bytes);
  for (int i = 0; i < count; i++) {
    n += (size_t)snprintf(body + n, cap - n, "%s{\"name\":\"%s\",\"size\":%llu,\"save\":%s}",
                          (i > 0) ? "," : "", entries[i].name,
                          (unsigned long long)entries[i].size, entries[i].save ? "true" : "false");
  }
  n += (size_t)snprintf(body + n, cap - n, "]}");

  httpd_resp_set_type(req, "application/json");
  const esp_err_t err = httpd_resp_send(req, body, (ssize_t)n);

  heap_caps_free(body);
  heap_caps_free(entries);
  return err;
}

// Writes the request body straight to a .part file, then renames it into place,
// so a connection that dies mid-upload leaves nothing that looks like a ROM.
static esp_err_t rom_put(httpd_req_t *req) {
  char name[ROM_FILES_NAME_MAX];
  if (!request_name(req, name, sizeof(name)) || !rom_files_valid_upload(name)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad file name");
  }
  if (req->content_len <= 0) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty upload");
  }

  uint64_t total = 0;
  uint64_t free_bytes = 0;
  rom_files_space(&total, &free_bytes);
  if ((uint64_t)req->content_len > free_bytes) {
    // 507 rather than a generic failure: the card simply has no room.
    httpd_resp_set_status(req, "507 Insufficient Storage");
    return httpd_resp_sendstr(req, "not enough room on the card");
  }

  char part[ROM_FILES_PATH_MAX];
  snprintf(part, sizeof(part), "%s/%s.part", rom_files_mount(), name);

  FILE *file = fopen(part, "wb");
  if (file == NULL) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot write");
  }

  char *chunk = heap_caps_malloc(UPLOAD_CHUNK, MALLOC_CAP_SPIRAM);
  if (chunk == NULL) {
    fclose(file);
    remove(part);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
  }

  int remaining = req->content_len;
  int stalled = 0;
  bool ok = true;

  while (remaining > 0) {
    const int want = (remaining < UPLOAD_CHUNK) ? remaining : UPLOAD_CHUNK;
    const int got = httpd_req_recv(req, chunk, want);

    if (got == HTTPD_SOCK_ERR_TIMEOUT) {
      // A phone that pauses is not a failure until it gives up entirely.
      if (++stalled > 5) {
        ok = false;
        break;
      }
      continue;
    }
    if (got <= 0) {
      ok = false;
      break;
    }

    stalled = 0;
    if (fwrite(chunk, 1, (size_t)got, file) != (size_t)got) {
      ok = false;
      break;
    }
    remaining -= got;
  }

  heap_caps_free(chunk);
  const bool closed = (fclose(file) == 0);
  if (!ok || !closed || remaining > 0) {
    remove(part);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
  }

  char path[ROM_FILES_PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s", rom_files_mount(), name);
  remove(path); // rename will not replace an existing file on FATFS
  if (rename(part, path) != 0) {
    remove(part);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot store");
  }

  ESP_LOGI(TAG, "stored %s (%d bytes)", name, (int)req->content_len);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_sendstr(req, "ok");
}

static esp_err_t rom_delete(httpd_req_t *req) {
  char name[ROM_FILES_NAME_MAX];
  if (!request_name(req, name, sizeof(name)) || !rom_files_delete(name)) {
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such ROM");
  }

  ESP_LOGI(TAG, "deleted %s", name);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_sendstr(req, "ok");
}

/* ----------------------------------------------------------------- setup --- */

bool web_start(void) {
  if (s_running) {
    return true;
  }

  const uint32_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

  if (!start_ap()) {
    stop_ap();
    ESP_LOGE(TAG, "ap failed with %u bytes of internal heap free", (unsigned)before);
    return false;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  // The default 4 KB is not enough for the directory walk behind /api/roms, and
  // this task's stack comes out of the internal RAM that is already tight.
  config.stack_size = 5120;
  config.max_uri_handlers = 8;
  config.lru_purge_enable = true;
  // A phone uploading a 4 MB ROM pauses; the default 5 s gives up on it.
  config.recv_wait_timeout = 30;

  esp_err_t err = httpd_start(&s_server, &config);
  if (err == ESP_OK) {
    const httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = page_get};
    const httpd_uri_t roms = {.uri = "/api/roms", .method = HTTP_GET, .handler = roms_get};
    // A trailing '*' is the httpd's own wildcard, which is how the file name
    // gets through in the URI.
    const httpd_uri_t rom_upload = {
        .uri = API_ROMS "*", .method = HTTP_PUT, .handler = rom_put};
    const httpd_uri_t rom_remove = {
        .uri = API_ROMS "*", .method = HTTP_DELETE, .handler = rom_delete};

    err = httpd_register_uri_handler(s_server, &page);
    if (err == ESP_OK) {
      err = httpd_register_uri_handler(s_server, &roms);
    }
    if (err == ESP_OK) {
      err = httpd_register_uri_handler(s_server, &rom_upload);
    }
    if (err == ESP_OK) {
      err = httpd_register_uri_handler(s_server, &rom_remove);
    }
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd: %s", esp_err_to_name(err));
    if (s_server != NULL) {
      httpd_stop(s_server);
      s_server = NULL;
    }
    stop_ap();
    return false;
  }

  s_running = true;
  ESP_LOGI(TAG, "ap up: internal heap %u -> %u bytes", (unsigned)before,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return true;
}

void web_stop(void) {
  if (s_server != NULL) {
    httpd_stop(s_server);
    s_server = NULL;
  }
  if (s_running || s_ap != NULL) {
    stop_ap();
  }
  if (s_running) {
    ESP_LOGI(TAG, "ap down: internal heap %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  }
  s_running = false;
  s_clients = 0;
}

bool web_running(void) { return s_running; }

int web_client_count(void) { return s_clients; }

const char *web_ip(void) { return AP_ADDRESS; }

const char *web_ssid(void) { return AP_SSID; }

const char *web_password(void) { return AP_PASSWORD; }
