/*

Apple2_ProjectESP32 — Transfert de fichiers par WiFi

Second firmware, logé dans la partition ota_1 (environnement PlatformIO
« wifi »). L'émulateur n'a ni la mémoire ni la place en IRAM pour faire tourner
le WiFi en même temps que lui : son menu fait donc redémarrer la carte sur ce
firmware, qui ne fait que servir la carte SD à un navigateur web.

Dès son démarrage il redonne la main à l'émulateur pour le démarrage suivant :
quoi qu'il arrive (bouton de la page, touche Échap, coupure de courant), la
carte revient à l'émulateur.

Réseau : si la carte SD contient WIFI.TXT (nom du réseau sur la première ligne,
mot de passe sur la seconde), la carte s'y connecte ; sinon, ou si la connexion
échoue, elle crée son propre réseau « Apple2_ProjectESP32 » (mot de passe
« apple2esp32 »), où elle répond à l'adresse 192.168.4.1.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#ifdef A2_WIFI_APP

#include <stdio.h>
#include <string.h>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "Emu.h"
#include "ESPConfig.h"
#include "FileUtils.h"
#include "Video.h"

using namespace std;

#define AP_SSID "Apple2_ProjectESP32"
#define AP_PASSWORD "apple2esp32"
#define WIFI_FILE "/sd/WIFI.TXT"
#define IO_BUF 4096

static volatile bool exitRequested = false;
static EventGroupHandle_t wifiEvents;
#define GOT_IP_BIT BIT0
#define FAILED_BIT BIT1
static char ipText[20] = "192.168.4.1";

// ---------------------------------------------------------------------------
// Page web
// ---------------------------------------------------------------------------
static const char PAGE[] = R"HTML(<!doctype html><html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Apple2_ProjectESP32</title>
<style>body{font-family:sans-serif;max-width:760px;margin:1em auto;padding:0 1em;background:#000080;color:#ff0}
a{color:#0ff}table{width:100%;border-collapse:collapse}td{padding:.25em .4em;border-bottom:1px solid #448}
button,input{font-size:1em;margin:.2em}#msg{color:#fff;min-height:1.4em}.r{text-align:right}</style></head><body>
<h2>Apple2_ProjectESP32 &mdash; carte SD</h2><p id="path"></p><p id="msg"></p>
<p><input type="file" id="files" multiple><button onclick="up()">Envoyer ici</button>
<button onclick="mk()">Nouveau dossier</button><button onclick="quit()">Retour &agrave; l'&eacute;mulateur</button></p>
<table id="list"></table>
<script>
let cur='/sd';const q=p=>encodeURIComponent(p),msg=t=>document.getElementById('msg').textContent=t;
async function ls(p){cur=p;document.getElementById('path').textContent=p;
const r=await fetch('/api/list?path='+q(p));const l=await r.json();
l.sort((a,b)=>b.d-a.d||a.n.localeCompare(b.n));let h='';
if(p!='/sd')h+='<tr><td><a href="#" onclick="ls(\''+p.substring(0,p.lastIndexOf('/'))+'\');return false">..</a></td><td></td><td></td></tr>';
for(const e of l){const f=p+'/'+e.n,j=JSON.stringify(f).replace(/"/g,'&quot;');
h+='<tr><td>'+(e.d?'<a href="#" onclick="ls('+j+');return false">'+e.n+'/</a>':'<a href="/api/get?path='+q(f)+'">'+e.n+'</a>')
+'</td><td class="r">'+(e.d?'':e.s)+'</td><td class="r"><button onclick="del('+j+')">Supprimer</button></td></tr>';}
document.getElementById('list').innerHTML=h;}
async function up(){const fs=document.getElementById('files').files;
for(const f of fs){msg('Envoi de '+f.name+'...');
const r=await fetch('/api/upload?path='+q(cur+'/'+f.name),{method:'POST',body:f});
if(!r.ok){msg('Echec : '+f.name);return;}}
msg(fs.length+' fichier(s) envoye(s)');ls(cur);}
async function del(f){if(!confirm('Supprimer '+f+' ?'))return;
const r=await fetch('/api/delete?path='+q(f),{method:'POST'});msg(r.ok?'Supprime':'Echec (dossier non vide ?)');ls(cur);}
async function mk(){const n=prompt('Nom du dossier');if(!n)return;
const r=await fetch('/api/mkdir?path='+q(cur+'/'+n),{method:'POST'});msg(r.ok?'Dossier cree':'Echec');ls(cur);}
async function quit(){await fetch('/api/exit',{method:'POST'});msg("La carte redemarre sur l'emulateur.");}
ls('/sd');
</script></body></html>)HTML";

// ---------------------------------------------------------------------------
// Outils
// ---------------------------------------------------------------------------

// Paramètre « path » de l'URL, décodé. Seuls les chemins de la carte SD, sans
// retour en arrière, sont acceptés.
static bool getPath(httpd_req_t* req, string& path) {
    static char query[600], raw[520];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    if (httpd_query_key_value(query, "path", raw, sizeof(raw)) != ESP_OK) return false;
    path.clear();
    for (const char* p = raw; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            path += (char)strtol(hex, nullptr, 16);
            p += 2;
        } else if (*p == '+') {
            path += ' ';
        } else {
            path += *p;
        }
    }
    while (path.size() > 3 && path.back() == '/') path.pop_back();
    return path.compare(0, 3, "/sd") == 0 && (path.size() == 3 || path[3] == '/')
        && path.find("..") == string::npos;
}

static esp_err_t fail(httpd_req_t* req, const char* text) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, text);
    return ESP_FAIL;
}

// ---------------------------------------------------------------------------
// Requêtes
// ---------------------------------------------------------------------------

static esp_err_t handlePage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

// Contenu d'un dossier, en JSON : [{"n":"nom","d":0 ou 1,"s":taille}, ...]
static esp_err_t handleList(httpd_req_t* req) {
    string path;
    if (!getPath(req, path)) return fail(req, "bad path");
    DIR* dir = opendir(path.c_str());
    if (!dir) return fail(req, "cannot open");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    struct dirent* de;
    bool first = true;
    while ((de = readdir(dir)) != nullptr) {
        string name = de->d_name;
        if (name.empty() || name[0] == '.') continue;
        struct stat st;
        long size = 0;
        if (stat((path + "/" + name).c_str(), &st) == 0) size = st.st_size;
        string item = first ? "{\"n\":\"" : ",{\"n\":\"";
        for (char c : name) {
            if (c == '"' || c == '\\') item += '\\';
            item += c;
        }
        char tail[48];
        snprintf(tail, sizeof(tail), "\",\"d\":%d,\"s\":%ld}", de->d_type == DT_DIR ? 1 : 0, size);
        item += tail;
        httpd_resp_sendstr_chunk(req, item.c_str());
        first = false;
    }
    closedir(dir);
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, nullptr);
}

static esp_err_t handleGet(httpd_req_t* req) {
    string path;
    if (!getPath(req, path)) return fail(req, "bad path");
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return fail(req, "cannot open");

    string disposition = "attachment; filename=\"" + path.substr(path.find_last_of('/') + 1) + "\"";
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());
    static char buf[IO_BUF];
    size_t n;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && (n = fread(buf, 1, sizeof(buf), f)) > 0)
        err = httpd_resp_send_chunk(req, buf, n);
    fclose(f);
    if (err == ESP_OK) httpd_resp_send_chunk(req, nullptr, 0);
    return err;
}

// Corps de la requête = contenu du fichier
static esp_err_t handleUpload(httpd_req_t* req) {
    string path;
    if (!getPath(req, path) || path.size() <= 4) return fail(req, "bad path");
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return fail(req, "cannot create");

    static char buf[IO_BUF];
    int left = req->content_len;
    bool ok = true;
    while (ok && left > 0) {
        int n = httpd_req_recv(req, buf, left < (int)sizeof(buf) ? left : (int)sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0 || fwrite(buf, 1, n, f) != (size_t)n) ok = false;
        else left -= n;
    }
    fclose(f);
    if (!ok) {
        remove(path.c_str());
        return fail(req, "write failed");
    }
    printf("WiFi: received %s (%d bytes)\n", path.c_str(), (int)req->content_len);
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t handleDelete(httpd_req_t* req) {
    string path;
    if (!getPath(req, path) || path.size() <= 4) return fail(req, "bad path");
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return fail(req, "not found");
    int r = S_ISDIR(st.st_mode) ? rmdir(path.c_str()) : remove(path.c_str());
    return r == 0 ? httpd_resp_sendstr(req, "ok") : fail(req, "cannot delete");
}

static esp_err_t handleMkdir(httpd_req_t* req) {
    string path;
    if (!getPath(req, path) || path.size() <= 4) return fail(req, "bad path");
    return mkdir(path.c_str(), 0777) == 0 ? httpd_resp_sendstr(req, "ok") : fail(req, "cannot create");
}

static esp_err_t handleExit(httpd_req_t* req) {
    httpd_resp_sendstr(req, "ok");
    exitRequested = true;
    return ESP_OK;
}

static void startServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    config.recv_wait_timeout = 20;
    config.send_wait_timeout = 20;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        printf("WiFi: cannot start the web server\n");
        return;
    }
    static const httpd_uri_t uris[] = {
        { "/",           HTTP_GET,  handlePage,   nullptr },
        { "/api/list",   HTTP_GET,  handleList,   nullptr },
        { "/api/get",    HTTP_GET,  handleGet,    nullptr },
        { "/api/upload", HTTP_POST, handleUpload, nullptr },
        { "/api/delete", HTTP_POST, handleDelete, nullptr },
        { "/api/mkdir",  HTTP_POST, handleMkdir,  nullptr },
        { "/api/exit",   HTTP_POST, handleExit,   nullptr },
    };
    for (auto& uri : uris) httpd_register_uri_handler(server, &uri);
}

// ---------------------------------------------------------------------------
// Réseau
// ---------------------------------------------------------------------------

static void onWifiEvent(void* arg, esp_event_base_t base, int32_t id, void* data) {
    static int retries = 0;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (++retries <= 4) esp_wifi_connect();
        else xEventGroupSetBits(wifiEvents, FAILED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)data;
        snprintf(ipText, sizeof(ipText), IPSTR, IP2STR(&event->ip_info.ip));
        retries = 0;
        xEventGroupSetBits(wifiEvents, GOT_IP_BIT);
    }
}

// Connexion au réseau décrit par WIFI.TXT ; faux s'il n'y en a pas ou si elle échoue
static bool connectStation(char* ssidOut, size_t ssidSize) {
    char ssid[33] = "", password[65] = "";
    FILE* f = fopen(WIFI_FILE, "r");
    if (!f) return false;
    auto readLine = [&](char* out, size_t size) {
        if (!fgets(out, size, f)) out[0] = 0;
        for (char* p = out + strlen(out); p > out && (p[-1] == '\n' || p[-1] == '\r'); ) *--p = 0;
    };
    readLine(ssid, sizeof(ssid));
    readLine(password, sizeof(password));
    fclose(f);
    if (ssid[0] == 0) return false;

    esp_netif_create_default_wifi_sta();
    wifi_config_t cfg = {};
    strncpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strncpy((char*)cfg.sta.password, password, sizeof(cfg.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();
    printf("WiFi: connecting to %s\n", ssid);
    EventBits_t bits = xEventGroupWaitBits(wifiEvents, GOT_IP_BIT | FAILED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if (bits & GOT_IP_BIT) {
        snprintf(ssidOut, ssidSize, "%s", ssid);
        return true;
    }
    printf("WiFi: connection failed\n");
    esp_wifi_stop();
    return false;
}

static void startAccessPoint() {
    esp_netif_create_default_wifi_ap();
    wifi_config_t cfg = {};
    strcpy((char*)cfg.ap.ssid, AP_SSID);
    cfg.ap.ssid_len = strlen(AP_SSID);
    strcpy((char*)cfg.ap.password, AP_PASSWORD);
    cfg.ap.channel = 6;
    cfg.ap.max_connection = 2;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    esp_wifi_start();
    snprintf(ipText, sizeof(ipText), "192.168.4.1");
}

// ---------------------------------------------------------------------------
// Programme
// ---------------------------------------------------------------------------

// Une ligne de l'écran, dans la grille de 40 colonnes de l'Apple
static void screenLine(int row, const char* text) {
    Video::overlayText(1, 2 + row, text, false);
}

void WifiApp_run() {
    printf("\n| Apple2_ProjectESP32: WiFi file transfer\n");

    // Le prochain démarrage revient à l'émulateur, quoi qu'il arrive
    const esp_partition_t* emulator = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    if (emulator) esp_ota_set_boot_partition(emulator);

    Config::load();           // initialise aussi le NVS, dont le WiFi a besoin
    Video::init();
    FileUtils::initSD();
    Emu::PS2Controller.begin(fabgl::PS2Preset::KeyboardPort0, fabgl::KbdMode::CreateVirtualKeysQueue);

    wifiEvents = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onWifiEvent, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onWifiEvent, nullptr, nullptr);

    char network[40] = AP_SSID;
    bool station = FileUtils::SDReady && connectStation(network, sizeof(network));
    if (!station) startAccessPoint();
    startServer();
    printf("| Apple2_ProjectESP32: %s, network %s, http://%s\n", station ? "station" : "access point", network, ipText);

    char line[64];
    screenLine(0, "APPLE2_PROJECTESP32 - WIFI");
    if (!FileUtils::SDReady) screenLine(2, "Pas de carte SD / No SD card");
    snprintf(line, sizeof(line), "WiFi : %s", network);
    screenLine(4, line);
    if (!station) screenLine(5, "Mot de passe : " AP_PASSWORD);
    snprintf(line, sizeof(line), "http://%s", ipText);
    screenLine(7, line);
    screenLine(10, "Echap / Esc : retour a l'Apple");

    // Fin : bouton de la page web, ou touche Échap
    while (!exitRequested) {
        vTaskDelay(pdMS_TO_TICKS(50));
        auto kbd = Emu::PS2Controller.keyboard();
        fabgl::VirtualKeyItem item;
        while (kbd && kbd->virtualKeyAvailable())
            if (kbd->getNextVirtualKey(&item) && item.down && item.vk == fabgl::VK_ESCAPE) exitRequested = true;
    }
    vTaskDelay(pdMS_TO_TICKS(500));   // le temps que la réponse parte
    esp_restart();
}

#endif // A2_WIFI_APP
