/*

Apple2_ProjectESP32 — Disque SmartPort branché sur la prise du lecteur externe
de l'Apple //c (ROM 0, 3, 4 et //c Plus).

Le //c n'a pas de slots : son disque dur ou son lecteur UniDisk 3.5 se branche
à la place du lecteur externe et dialogue par paquets à travers l'IWM. Les
phases 1 et 3 ensemble ouvrent le bus ; la phase 0 (REQ) dit que l'Apple veut
émettre ou recevoir, et le périphérique répond sur la ligne d'état (ACK), que
l'IWM lit comme la protection en écriture.

  Apple : REQ haut, émet le paquet de commande.
  Périphérique : ACK bas à la fin du paquet. L'Apple baisse REQ, ACK remonte.
  Apple : REQ haut. Le périphérique émet sa réponse, puis ACK bas ;
          l'Apple baisse REQ, ACK remonte.
  Pour une écriture, l'Apple émet de même un paquet de données entre les deux.

Un paquet : $C3, destinataire, émetteur, type, auxiliaire, état, nombre
d'octets isolés, nombre de groupes de 7, les données, deux octets de somme de
contrôle, $C8. Tous les octets ont leur bit 7 à 1 : les bits 7 des données
voyagent à part, en tête de chaque groupe.

Le périphérique est émulé au niveau des octets échangés avec l'IWM : il sert
ses octets au rythme où le firmware les lit. L'image est celle montée comme
disque dur ; elle se présente comme un disque dur de blocs de 512 octets.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

*/

#include <string.h>
#include "A2Internal.h"

namespace A2 {
namespace SmartPort {

#define BYTE_CYCLES 12              // délai entre deux octets servis à l'IWM
#define PACKET_MAX 640              // un bloc de 512 octets codé tient en 606 octets

enum : uint8_t { TYPE_COMMAND = 0x80, TYPE_STATUS = 0x81, TYPE_DATA = 0x82 };
enum : uint8_t { CMD_STATUS = 0, CMD_READ_BLOCK, CMD_WRITE_BLOCK, CMD_FORMAT, CMD_CONTROL, CMD_INIT };
enum : uint8_t { ERR_NONE = 0x00, ERR_BAD_COMMAND = 0x01, ERR_BAD_CONTROL = 0x21, ERR_IO = 0x27,
                 ERR_WRITE_PROTECTED = 0x2B, ERR_BAD_BLOCK = 0x2D, ERR_OFFLINE = 0x2F };

struct State {
    uint8_t id;                     // numéro reçu à l'initialisation du bus, 0 avant
    uint8_t host;                   // numéro de l'Apple
    bool ack;                       // ligne ACK : haute au repos
    bool req;
    uint8_t pendingCommand;         // commande dont on attend le paquet de données
    uint32_t pendingBlock;
    uint32_t readCycles;            // instant du dernier octet servi
};

static State s;
static bool enabled = false;        // la machine a un port SmartPort
static bool mediaElsewhere = false; // l'image est une disquette du lecteur 3,5 pouces interne
static uint8_t packet[PACKET_MAX];  // paquet reçu, puis paquet à émettre
static int rxLen = 0, txLen = 0, txPos = 0;

void setEnabled(bool on) {
    enabled = on;
    memset(&s, 0, sizeof(s));
    s.ack = true;
    s.pendingCommand = 0xFF;
    rxLen = txLen = 0;
}

// //c Plus : une image de 800 Ko va dans le lecteur interne
void setInternal35(bool on) { mediaElsewhere = on; }

static inline uint32_t blocks() {
    uint32_t n = Hdd::blockCount();
    return (mediaElsewhere && Disk35::isMedia(n)) ? 0 : n;
}

static inline bool present() { return enabled && blocks() != 0; }

// Phases 0 et 2 ensemble : remise à zéro du bus, les numéros sont à redonner
void busReset() {
    s.id = 0;
    s.ack = true;
    s.pendingCommand = 0xFF;
    rxLen = txLen = 0;
}

bool ack() { return !present() || s.ack; }

// ---------------------------------------------------------------------------
// Paquets
// ---------------------------------------------------------------------------

// Prépare la réponse : en-tête, données codées par groupes de 7, somme de contrôle
static void reply(uint8_t type, uint8_t error, const uint8_t* data, int len) {
    uint8_t* p = packet;
    *p++ = 0xFF;
    *p++ = 0xC3;
    uint8_t* header = p;
    *p++ = s.host;
    *p++ = s.id;
    *p++ = type;
    *p++ = 0x80;
    *p++ = error | 0x80;
    int odd = len % 7, groups = len / 7;
    *p++ = (uint8_t)(odd | 0x80);
    *p++ = (uint8_t)(groups | 0x80);
    uint8_t sum = 0;
    for (int i = 0; i < 7; i++) sum ^= header[i];
    for (int i = 0; i < len; i++) sum ^= data[i];
    if (odd) {
        uint8_t top = 0x80;
        for (int i = 0; i < odd; i++) top |= (data[i] >> 7) << (6 - i);
        *p++ = top;
        for (int i = 0; i < odd; i++) *p++ = data[i] | 0x80;
    }
    const uint8_t* d = data + odd;
    for (int g = 0; g < groups; g++, d += 7) {
        uint8_t top = 0x80;
        for (int i = 0; i < 7; i++) top |= (d[i] >> 7) << (6 - i);
        *p++ = top;
        for (int i = 0; i < 7; i++) *p++ = d[i] | 0x80;
    }
    *p++ = sum | 0xAA;
    *p++ = (sum >> 1) | 0xAA;
    *p++ = 0xC8;
    txLen = (int)(p - packet);
    txPos = 0;
}

// Données du paquet reçu (packet[0] est $C3 ; write() a vérifié sa longueur :
// 6 octets isolés et 73 groupes au plus). Rend leur nombre, même au-delà de
// `maxLen` (seuls les premiers sont alors gardés), ou -1 si la somme de
// contrôle est fausse.
static int payload(uint8_t* out, int maxLen) {
    int odd = packet[6] & 0x7F, groups = packet[7] & 0x7F;
    int n = 0, pos = 8;
    uint8_t sum = 0;
    for (int i = 1; i <= 7; i++) sum ^= packet[i];
    if (odd) {
        uint8_t top = packet[pos++];
        for (int i = 0; i < odd; i++, n++) {
            uint8_t v = (uint8_t)((packet[pos++] & 0x7F) | (((top >> (6 - i)) & 1) << 7));
            sum ^= v;
            if (n < maxLen) out[n] = v;
        }
    }
    for (int g = 0; g < groups; g++) {
        uint8_t top = packet[pos++];
        for (int i = 0; i < 7; i++, n++) {
            uint8_t v = (uint8_t)((packet[pos++] & 0x7F) | (((top >> (6 - i)) & 1) << 7));
            sum ^= v;
            if (n < maxLen) out[n] = v;
        }
    }
    // La somme voyage sur deux octets : ses bits pairs, puis ses bits impairs
    uint8_t sent = (uint8_t)((packet[pos] & 0x55) | ((packet[pos + 1] & 0x55) << 1));
    return sent == sum ? n : -1;
}

// Réponse à la commande d'état : état général (code 0) ou bloc d'identification (code 3)
static void status(uint8_t code) {
    uint32_t n = blocks();
    uint8_t d[25];
    // Périphérique par blocs, lecture, écriture et formatage permis, en ligne
    d[0] = 0xF8 | (Hdd::writeProtected() ? 0x04 : 0);
    d[1] = (uint8_t)n;
    d[2] = (uint8_t)(n >> 8);
    d[3] = (uint8_t)(n >> 16);
    if (code == 0x00) { reply(TYPE_STATUS, ERR_NONE, d, 4); return; }
    if (code != 0x03) { reply(TYPE_STATUS, ERR_BAD_CONTROL, nullptr, 0); return; }
    static const char name[] = "ESP32 DISK      ";
    d[4] = 10;
    memcpy(d + 5, name, 16);
    d[21] = 0x02;       // disque dur
    d[22] = 0x20;       // non amovible
    d[23] = 0x01;       // version
    d[24] = 0x00;
    reply(TYPE_STATUS, ERR_NONE, d, 25);
}

// Paquet complet reçu de l'Apple
static void received() {
    uint8_t dest = packet[1], type = packet[3];
    uint8_t d[512];
    int n = payload(d, sizeof(d));
    // Paquet abîmé : pas d'accusé de réception, l'Apple le renverra
    if (n < 0) return;

    if (type == TYPE_DATA) {
        // Suite d'une écriture de bloc ou d'une commande de contrôle
        if (dest != s.id || s.pendingCommand == 0xFF) return;
        uint8_t error = ERR_NONE;
        if (s.pendingCommand == CMD_WRITE_BLOCK) {
            if (n != 512) error = ERR_IO;
            else if (Hdd::writeProtected()) error = ERR_WRITE_PROTECTED;
            else if (s.pendingBlock >= blocks()) error = ERR_BAD_BLOCK;
            else if (!A2_platformHddWrite(s.pendingBlock, d)) error = ERR_IO;
            else Hdd::statWrites++;
            Hdd::activity = 30;
        }
        s.pendingCommand = 0xFF;
        s.ack = false;
        reply(TYPE_STATUS, error, nullptr, 0);
        return;
    }
    if (type != TYPE_COMMAND || n < 2) return;
    uint8_t command = d[0];
    // À l'initialisation, le premier périphérique sans numéro prend celui du paquet
    if (command == CMD_INIT && s.id == 0) s.id = dest;
    if (dest != s.id) return;
    s.host = packet[2];
    s.ack = false;
    s.pendingCommand = 0xFF;
    // Après la commande et le nombre de paramètres : pointeur du tampon (2
    // octets), puis le numéro de bloc ou le code d'état
    uint32_t block = n >= 7 ? (d[4] | (d[5] << 8) | ((uint32_t)d[6] << 16)) : 0;
    switch (command) {
        case CMD_STATUS:
            status(n >= 5 ? d[4] : 0);
            break;
        case CMD_READ_BLOCK:
            Hdd::activity = 30;
            if (block >= blocks()) reply(TYPE_STATUS, ERR_BAD_BLOCK, nullptr, 0);
            else if (!A2_platformHddRead(block, d)) reply(TYPE_STATUS, ERR_IO, nullptr, 0);
            else {
                Hdd::statReads++;
                reply(TYPE_DATA, ERR_NONE, d, 512);
            }
            break;
        case CMD_WRITE_BLOCK:
        case CMD_CONTROL:
            // Le paquet de données suit
            s.pendingCommand = command;
            s.pendingBlock = command == CMD_WRITE_BLOCK ? block : 0;
            txLen = 0;
            break;
        case CMD_FORMAT:
            reply(TYPE_STATUS, Hdd::writeProtected() ? ERR_WRITE_PROTECTED : ERR_NONE, nullptr, 0);
            break;
        case CMD_INIT:
            // État $FF : dernier périphérique de la chaîne
            reply(TYPE_STATUS, 0x7F, nullptr, 0);
            break;
        default:
            reply(TYPE_STATUS, ERR_BAD_COMMAND, nullptr, 0);
            break;
    }
}

// ---------------------------------------------------------------------------
// Dialogue avec l'IWM
// ---------------------------------------------------------------------------

// Les phases viennent de changer, le bus étant ouvert (phases 1 et 3)
void phases(uint8_t p) {
    bool req = (p & 1) != 0;
    if (s.req && !req) {
        // L'Apple a vu notre accusé de réception : prêt pour la suite
        s.ack = true;
        rxLen = 0;
    }
    s.req = req;
}

// Octet émis par l'Apple
void write(uint8_t v) {
    if (!present() || !s.req) return;
    if (rxLen == 0 && v != 0xC3) return;        // octets de synchronisation
    if (rxLen < PACKET_MAX) packet[rxLen++] = v;
    // La longueur se déduit de l'en-tête : $C8 peut aussi être une donnée
    if (rxLen < 8) return;
    int odd = packet[6] & 0x7F, groups = packet[7] & 0x7F;
    // 512 octets font 1 octet isolé et 73 groupes ; un groupe incomplet a 6 octets au plus
    if (odd > 6 || groups > 73) { rxLen = 0; return; }
    int total = 8 + (odd ? odd + 1 : 0) + groups * 8 + 3;
    if (rxLen < total) return;
    txLen = 0;
    received();
    rxLen = 0;
}

// Octet lu par l'Apple : le suivant de la réponse, 0 tant qu'il n'est pas là
uint8_t read() {
    if (!present() || !s.req || !s.ack || txPos >= txLen) return 0;
    if ((uint32_t)(cycles - s.readCycles) < BYTE_CYCLES) return 0;
    s.readCycles = cycles;
    uint8_t v = packet[txPos++];
    if (txPos >= txLen) {
        // Fin de la réponse
        txLen = 0;
        s.ack = false;
    }
    return v;
}

void state(StateIO& io) {
    io.bytes(&s, sizeof(s));
    if (!io.saving) {
        // Un échange en cours est perdu : le firmware le reprendra
        s.ack = true;
        s.req = false;
        s.pendingCommand = 0xFF;
        rxLen = txLen = 0;
    }
}

}
}
